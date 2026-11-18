// ===========================================================================
//  ble.cpp — реализация Bluetooth LE движка (C++/WinRT + Windows 10/11).
//
//  РОЛИ
//    Отправитель (Central)  — BluetoothLEAdvertisementWatcher для поиска,
//                             BluetoothLEDevice + GATT-клиент для передачи;
//    Приёмник (Peripheral)  — GattServiceProvider: публикует сервис,
//                             принимает write-запросы, пишет файл на диск.
//
//  ПРОТОКОЛ (вариант 8)
//    Сервис     A1B2C3D4-5E6F-4A7B-8C9D-0E1F2A3B4C5D
//      Control  ...4C61  write / write-without-response  команды (текст UTF-8)
//      Data     ...4C62  write-without-response         блоки файла
//      Status   ...4C63  notify + read                  подтверждения (текст)
//
//    Control:  START|размер|crc32|имя      -> Status: READY|смещение
//              END|размер|crc32            -> Status: DONE|размер|crc|OK|путь
//              ABORT                       -> Status: ABORTED
//    Data:     [uint32 offset][данные]  — блок файла; приёмник принимает блок,
//              только если его смещение совпадает с текущей длиной файла.
//    Status:   ACK|получено  — раз в ~8 КБ или 100 мс.
//
//    Благодаря смещению в кадре потери/дубликаты Write Without Response не
//    портят файл: приёмник отбрасывает неподходящие блоки и сообщает своё
//    смещение, а отправитель повторяет передачу с подтверждённой позиции.
//    После разрыва связи сеанс сохраняется: повторный START возвращает уже
//    принятую длину файла — передача продолжается с этого места.
//
//  ПОТОКИ
//    UI-поток никогда не вызывает WinRT: движок работает в отдельных
//    MTA-потоках (сканирование, соединение/передача, GATT-сервер) и общается
//    с окном через PostMessage(kUpdateMessage) и снимки состояния под mutex.
// ===========================================================================
#include "ble.h"

#include "crc32.h"
#include "protocol.h"

#include <objbase.h>

#include <winrt/base.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "ole32.lib")

// Using-директивы вынесены в глобальную область: они должны быть видны и в
// анонимном пространстве имён (помощники), и в namespace ble (сам движок).
using namespace winrt;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------
//  Идентификаторы GATT
// ---------------------------------------------------------------------------
guid MakeGuid(wchar_t const* text) {
    std::wstring braces = L"{";
    braces += text;
    braces += L"}";

    GUID parsed{};
    if (FAILED(CLSIDFromString(braces.c_str(), &parsed))) {
        return {};
    }
    // winrt::guid конструируется из GUID (преобразующий конструктор C++/WinRT).
    return guid{parsed};
}

guid const& ServiceUuid() {
    static const guid value = MakeGuid(L"A1B2C3D4-5E6F-4A7B-8C9D-0E1F2A3B4C5D");
    return value;
}
guid const& ControlUuid() {
    static const guid value = MakeGuid(L"A1B2C3D4-5E6F-4A7B-8C9D-0E1F2A3B4C61");
    return value;
}
guid const& DataUuid() {
    static const guid value = MakeGuid(L"A1B2C3D4-5E6F-4A7B-8C9D-0E1F2A3B4C62");
    return value;
}
guid const& StatusUuid() {
    static const guid value = MakeGuid(L"A1B2C3D4-5E6F-4A7B-8C9D-0E1F2A3B4C63");
    return value;
}

// Значения GattCharacteristicProperties (флаги). Считаем числами, чтобы не
// зависеть от генерации оператора | для перечислимого типа.
enum GattProps : std::uint32_t {
    PropRead = 0x02,
    PropWriteWithoutResponse = 0x04,
    PropWrite = 0x08,
    PropNotify = 0x10,
};

GattCharacteristicProperties Props(std::uint32_t value) {
    return static_cast<GattCharacteristicProperties>(value);
}

constexpr int kMaxPayloadDefault = 20 - 3 - 4;   // MTU 23 → 13 байт данных
constexpr int kMaxPdu = 517;                     // практический максимум BLE
constexpr unsigned long long kWindowMin = 8 * 1024;
constexpr int kMaxAttempts = 6;                  // попытки восстановить связь

// ---------------------------------------------------------------------------
//  Строки и числа
// ---------------------------------------------------------------------------
std::wstring Format(wchar_t const* fmt, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    return buffer;
}

std::string Utf8(std::wstring const& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                         nullptr, nullptr);
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr,
                        nullptr);
    return out;
}

std::wstring Wide(std::string const& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring out((size_t)size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), size);
    return out;
}

std::wstring MacText(std::uint64_t address) {
    wchar_t buffer[24];
    _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"%02X:%02X:%02X:%02X:%02X:%02X",
                 (unsigned)((address >> 40) & 0xFF), (unsigned)((address >> 32) & 0xFF),
                 (unsigned)((address >> 24) & 0xFF), (unsigned)((address >> 16) & 0xFF),
                 (unsigned)((address >> 8) & 0xFF), (unsigned)(address & 0xFF));
    return buffer;
}

std::wstring SizeText(unsigned long long bytes) {
    const wchar_t* units[] = {L"Б", L"КБ", L"МБ", L"ГБ"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? Format(L"%.0f %s", value, units[unit])
                     : Format(L"%.1f %s", value, units[unit]);
}

std::wstring SpeedText(double bytes_per_sec) {
    if (bytes_per_sec <= 0.0) return L"—";
    return SizeText((unsigned long long)bytes_per_sec) + L"/с";
}

std::wstring TimeText(double seconds) {
    if (seconds <= 0.0 || seconds > 86400.0 * 2) return L"—";
    const int total = (int)(seconds + 0.5);
    if (total < 60) return Format(L"~%d с", total);
    if (total < 3600) return Format(L"~%d мин %02d с", total / 60, total % 60);
    return Format(L"~%d ч %02d мин", total / 3600, (total % 3600) / 60);
}

// Разбор строк протокола и кадры Data — общие с автотестами (protocol.h).
using ble::proto::DecodeFrame;
using ble::proto::EncodeFrame;
using ble::proto::kMaxFileSize;
using ble::proto::ParseUll;
using ble::proto::SanitizeName;
using ble::proto::Split;

std::wstring FileNamePart(std::wstring const& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring JoinPath(std::wstring const& dir, std::wstring const& name) {
    if (dir.empty()) return name;
    const wchar_t last = dir.back();
    return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
}

bool DirExists(std::wstring const& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDir(std::wstring const& path) {
    if (path.empty()) return false;
    if (DirExists(path)) return true;
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE;
}

std::wstring BluetoothErrorText(BluetoothError error) {
    switch (error) {
        case BluetoothError::Success: return L"успешно";
        case BluetoothError::RadioNotAvailable: return L"радиомодуль недоступен";
        case BluetoothError::ResourceInUse: return L"ресурс занят";
        case BluetoothError::DeviceNotConnected: return L"устройство не подключено";
        case BluetoothError::OtherError: return L"прочая ошибка";
        case BluetoothError::DisabledByPolicy: return L"запрещено политикой";
        case BluetoothError::DisabledByUser: return L"отключено пользователем";
        case BluetoothError::ConsentRequired: return L"требуется согласие";
        default: return Format(L"код %d", (int)error);
    }
}

std::wstring CommStatusText(GattCommunicationStatus status) {
    switch (status) {
        case GattCommunicationStatus::Success: return L"успешно";
        case GattCommunicationStatus::Unreachable: return L"устройство недоступно";
        case GattCommunicationStatus::ProtocolError: return L"ошибка протокола";
        case GattCommunicationStatus::AccessDenied: return L"доступ запрещён";
        default: return Format(L"код %d", (int)status);
    }
}

// ---------------------------------------------------------------------------
//  Буферы WinRT
// ---------------------------------------------------------------------------
IBuffer ToBuffer(const void* data, size_t size) {
    DataWriter writer;
    if (size > 0) {
        writer.WriteBytes(array_view<std::uint8_t const>(static_cast<std::uint8_t const*>(data),
                                                         static_cast<std::uint8_t const*>(data) +
                                                             size));
    }
    return writer.DetachBuffer();
}

IBuffer ToBuffer(std::string const& text) { return ToBuffer(text.data(), text.size()); }

std::vector<std::uint8_t> FromBuffer(IBuffer const& buffer) {
    if (!buffer) return {};
    DataReader reader = DataReader::FromBuffer(buffer);
    std::vector<std::uint8_t> bytes(reader.UnconsumedBufferLength());
    if (!bytes.empty()) {
        reader.ReadBytes(array_view<std::uint8_t>(bytes.data(), bytes.data() + bytes.size()));
    }
    return bytes;
}

// CRC32 файла, читая его потоково блоками по 64 КБ.
std::uint32_t Crc32File(std::wstring const& path, unsigned long long* size_out,
                        std::atomic<bool>* cancel) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER size{};
    if (size_out) *size_out = GetFileSizeEx(file, &size) ? (unsigned long long)size.QuadPart : 0;

    lab6::Crc32 crc;
    std::vector<std::uint8_t> buffer(64 * 1024);
    DWORD read = 0;
    while (ReadFile(file, buffer.data(), (DWORD)buffer.size(), &read, nullptr) && read > 0) {
        crc.Update(buffer.data(), read);
        if (cancel && cancel->load()) break;
    }
    CloseHandle(file);
    return crc.Value();
}

// ---------------------------------------------------------------------------
//  Ошибки передачи
// ---------------------------------------------------------------------------
struct LinkLost : std::runtime_error {
    explicit LinkLost(char const* what) : std::runtime_error(what) {}
};
struct Stall : std::runtime_error {
    explicit Stall(char const* what) : std::runtime_error(what) {}
};
struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("cancelled") {}
};

void JoinThread(std::thread& thread) {
    if (!thread.joinable()) return;
    if (thread.get_id() == std::this_thread::get_id()) {
        thread.detach();
    } else {
        thread.join();
    }
}

}  // namespace

// ===========================================================================
//  Реализация движка
// ===========================================================================
namespace ble {

struct Engine::Impl {
    // ---------------- общее состояние ----------------
    mutable std::mutex mu;
    Stats stats;
    std::vector<LogEntry> log;
    std::vector<DeviceInfo> devices;
    std::wstring incoming_dir;
    HWND notify = nullptr;
    Role role = RoleSender;

    std::atomic<bool> cancel{false};
    std::atomic<bool> scan_stop{false};
    std::atomic<bool> recv_stop{false};
    std::atomic<bool> send_busy{false};
    std::atomic<bool> link_busy{false};
    std::atomic<bool> scan_active{false};   // поток сканирования уже запущен
    std::atomic<bool> recv_active{false};   // поток GATT-сервера уже запущен

    std::thread scan_thread;
    std::thread link_thread;
    std::thread send_thread;
    std::thread recv_thread;

    // ---------------- отправитель: соединение ----------------
    std::mutex snd_mu;
    std::condition_variable snd_cv;

    BluetoothLEDevice device{nullptr};
    GattDeviceService service{nullptr};
    GattCharacteristic ctrl_ch{nullptr};
    GattCharacteristic data_ch{nullptr};
    GattCharacteristic status_ch{nullptr};
    GattSession session{nullptr};
    event_token device_token{};
    event_token value_token{};
    bool value_subscribed = false;
    bool connected = false;

    std::uint64_t target = 0;
    int target_type = 0;
    std::wstring target_name;
    unsigned long long acked = 0;
    unsigned long long ack_count = 0;
    std::deque<std::wstring> inbox;   // READY / DONE / ERR
    int mtu = 23;
    int payload = kMaxPayloadDefault;

    // ---------------- приёмник: GATT-сервер ----------------
    std::mutex recv_mu;
    GattServiceProvider provider{nullptr};
    GattLocalCharacteristic local_ctrl{nullptr};
    GattLocalCharacteristic local_data{nullptr};
    GattLocalCharacteristic local_status{nullptr};
    event_token tok_ctrl{};
    event_token tok_data{};
    event_token tok_read{};
    event_token tok_subs{};
    bool receiving = false;
    bool central_here = false;

    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring r_name;
    std::wstring r_path;
    unsigned long long r_size = 0;
    unsigned long long r_crc_expect = 0;
    unsigned long long r_received = 0;
    unsigned long long r_chunks = 0;
    unsigned long long r_ack_mark = 0;
    unsigned long long r_last_ack = 0;
    unsigned long long r_last_ui = 0;
    bool r_has = false;
    bool r_done = false;
    std::wstring r_line = L"IDLE";

    // история скорости текущей передачи
    std::mutex sample_mu;
    std::deque<std::pair<unsigned long long, unsigned long long>> samples;
    unsigned long long last_sample_ui = 0;

    // -----------------------------------------------------------------------
    //  Вспомогательное
    // -----------------------------------------------------------------------
    void NotifyUi() {
        if (notify && IsWindow(notify)) PostMessageW(notify, kUpdateMessage, 0, 0);
    }

    // Ожидание, прерываемое отменой: чтобы закрытие окна не висело 2 секунды
    // на паузе между попытками восстановления связи.
    void SleepCancelable(int ms) {
        for (int elapsed = 0; elapsed < ms && !cancel.load(); elapsed += 50) Sleep(50);
    }

    void Log(int level, std::wstring const& text) {
        {
            std::lock_guard<std::mutex> lock(mu);
            log.push_back(LogEntry{text, level});
            if (log.size() > 800) log.erase(log.begin(), log.begin() + 200);
        }
        NotifyUi();
    }

    template <typename Fn>
    void Edit(Fn&& fn) {
        {
            std::lock_guard<std::mutex> lock(mu);
            fn(stats);
        }
        NotifyUi();
    }

    void SetLink(int link, std::wstring const& status) {
        Edit([&](Stats& s) {
            s.link = link;
            if (!status.empty()) s.status = status;
        });
    }

    void PushSpeed(double speed) {
        std::lock_guard<std::mutex> lock(mu);
        stats.speed = speed;
        stats.speed_hist.push_back((float)speed);
        if (stats.speed_hist.size() > 160) {
            stats.speed_hist.erase(stats.speed_hist.begin(),
                                   stats.speed_hist.begin() + (stats.speed_hist.size() - 160));
        }
    }

    std::vector<DeviceInfo> DeviceList() const {
        std::lock_guard<std::mutex> lock(mu);
        return devices;
    }

    // =======================================================================
    //  СКАНИРОВАНИЕ (Central)
    // =======================================================================
    // Возможности Bluetooth-адаптера. Нужны, чтобы отличить «нет адаптера»,
    // «адаптер без BLE» и «адаптер без роли Peripheral» (частый случай у
    // дешёвых USB-донглов со сторонним стеком: реклама и GATT-сервер в этом
    // случае недоступны, и без этой проверки пользователь увидит лишь
    // невнятную ошибку HRESULT).
    BluetoothAdapter GetAdapter() {
        try {
            return BluetoothAdapter::GetDefaultAsync().get();
        } catch (...) {
            return nullptr;
        }
    }

    // Проверяет адаптер и пишет его возможности в журнал.
    // need_peripheral — требуем роль Peripheral (приёмник).
    bool CheckAdapter(bool need_peripheral) {
        BluetoothAdapter adapter = GetAdapter();
        if (!adapter) {
            Log(3, L"Bluetooth-адаптер не найден. Подключите адаптер и включите Bluetooth.");
            SetLink(LinkError, L"Нет Bluetooth-адаптера");
            return false;
        }

        const bool le = adapter.IsLowEnergySupported();
        const bool central = adapter.IsCentralRoleSupported();
        const bool peripheral = adapter.IsPeripheralRoleSupported();

        Log(0, Format(L"Адаптер: BLE — %s, Central — %s, Peripheral — %s",
                      le ? L"да" : L"нет", central ? L"да" : L"нет", peripheral ? L"да" : L"нет"));

        if (!le) {
            Log(3, L"Bluetooth Low Energy не поддерживается этим адаптером.");
            SetLink(LinkError, L"Нет поддержки BLE");
            return false;
        }
        if (need_peripheral && !peripheral) {
            Log(3, L"Адаптер не поддерживает роль Peripheral: реклама и GATT-сервер недоступны. "
                   L"Нужен встроенный адаптер с драйвером Microsoft (USB-донглы со сторонним "
                   L"стеком ролью Peripheral не обладают).");
            SetLink(LinkError, L"Роль Peripheral не поддерживается");
            return false;
        }
        if (!need_peripheral && !central) {
            Log(3, L"Адаптер не поддерживает роль Central (GATT-клиент).");
            SetLink(LinkError, L"Роль Central не поддерживается");
            return false;
        }
        return true;
    }

    void ScanMain() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if (!CheckAdapter(false)) {
            winrt::uninit_apartment();
            return;
        }
        try {
            BluetoothLEAdvertisementWatcher watcher;
            watcher.ScanningMode(BluetoothLEScanningMode::Active);

            auto received = watcher.Received(
                [this](BluetoothLEAdvertisementWatcher const&,
                       BluetoothLEAdvertisementReceivedEventArgs const& args) { OnAdvertisement(args); });
            auto stopped = watcher.Stopped(
                [this](BluetoothLEAdvertisementWatcher const&,
                       BluetoothLEAdvertisementWatcherStoppedEventArgs const& args) {
                    if (args.Error() != BluetoothError::Success) {
                        Log(1, L"Сканирование прервано: " + BluetoothErrorText(args.Error()));
                    }
                });

            watcher.Start();
            SetLink(LinkScanning, L"Сканирование BLE…");
            Edit([](Stats& s) { s.scanning = true; });
            Log(0, L"Сканирование BLE запущено (active scan)");

            while (!scan_stop.load() && !cancel.load()) Sleep(80);

            watcher.Stop();
            try {
                watcher.Received(received);
                watcher.Stopped(stopped);
            } catch (...) {
            }
        } catch (hresult_error const& e) {
            Log(2, L"Сканирование недоступно: " + std::wstring(e.message().c_str()));
            SetLink(LinkError, L"Bluetooth недоступен");
        } catch (...) {
            Log(2, L"Сканирование недоступно");
        }

        Edit([](Stats& s) {
            s.scanning = false;
            if (s.link == LinkScanning) s.link = LinkIdle;
            s.status = L"Сканирование остановлено";
        });
        winrt::uninit_apartment();
    }

    void OnAdvertisement(BluetoothLEAdvertisementReceivedEventArgs const& args) {
        const std::uint64_t address = args.BluetoothAddress();
        const int address_type = (int)args.BluetoothAddressType();
        const int rssi = (int)args.RawSignalStrengthInDBm();
        std::wstring name = args.Advertisement().LocalName().c_str();
        bool has_service = false;
        try {
            for (auto const& uuid : args.Advertisement().ServiceUuids()) {
                if (uuid == ServiceUuid()) {
                    has_service = true;
                    break;
                }
            }
        } catch (...) {
        }

        bool added = false;
        bool marked = false;
        {
            std::lock_guard<std::mutex> lock(mu);
            DeviceInfo* found = nullptr;
            for (auto& device : devices) {
                if (device.address == address) {
                    found = &device;
                    break;
                }
            }
            if (!found) {
                DeviceInfo info;
                info.address = address;
                info.address_type = address_type;
                info.name = name;
                info.rssi = rssi;
                info.has_service = has_service;
                info.seen = GetTickCount64();
                devices.push_back(info);
                added = true;
                marked = has_service;
            } else {
                if (!name.empty()) found->name = name;
                found->rssi = rssi;
                if (has_service && !found->has_service) {
                    found->has_service = true;
                    marked = true;
                }
                found->seen = GetTickCount64();
            }
        }

        if (added) {
            Log(marked ? 3 : 0,
                Format(L"Найдено: %s (%s), RSSI %d дБм%s",
                       name.empty() ? L"без имени" : name.c_str(), MacText(address).c_str(), rssi,
                       marked ? L" — наш сервис" : L""));
            NotifyUi();
        } else if (marked) {
            Log(3, Format(L"Устройство %s рекламирует наш сервис", MacText(address).c_str()));
            NotifyUi();
        }
    }

    // =======================================================================
    //  ПОДКЛЮЧЕНИЕ (Central)
    // =======================================================================
    void OnDeviceConnectionChanged(BluetoothConnectionStatus status) {
        if (status != BluetoothConnectionStatus::Disconnected) return;
        bool was = false;
        {
            std::lock_guard<std::mutex> lock(snd_mu);
            was = connected;
            connected = false;
        }
        snd_cv.notify_all();
        if (!was) return;
        Log(1, L"Соединение разорвано");
        Edit([](Stats& s) {
            s.connected = false;
            if (s.transfer == TrSending) {
                s.transfer = TrPaused;
                s.link = LinkPaused;
                s.status = L"Связь потеряна — ждём восстановления";
            } else {
                s.link = LinkIdle;
                s.status = L"Не подключено";
            }
        });
    }

    void OnValueChanged(GattValueChangedEventArgs const& args) {
        std::vector<std::uint8_t> bytes;
        try {
            bytes = FromBuffer(args.CharacteristicValue());
        } catch (...) {
            return;
        }
        const std::string text(bytes.begin(), bytes.end());
        size_t start = 0;
        while (start <= text.size()) {
            const size_t nl = text.find('\n', start);
            const std::string part =
                text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            start = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

            std::wstring line = Wide(part);
            while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) line.pop_back();
            if (!line.empty()) HandleStatusLine(line);
        }
    }

    void HandleStatusLine(std::wstring const& line) {
        if (line.rfind(L"ACK|", 0) == 0) {
            const unsigned long long value = ParseUll(line.substr(4));
            unsigned long long total_acks = 0;
            {
                std::lock_guard<std::mutex> lock(snd_mu);
                if (value > acked) acked = value;
                total_acks = ++ack_count;
            }
            snd_cv.notify_all();
            Edit([&](Stats& s) { s.acks = total_acks; });
            return;
        }

        {
            std::lock_guard<std::mutex> lock(snd_mu);
            inbox.push_back(line);
            if (inbox.size() > 32) inbox.pop_front();
        }
        snd_cv.notify_all();
        if (line.rfind(L"ERR|", 0) == 0) Log(2, L"Приёмник сообщил: " + line.substr(4));
    }

    bool WaitStatus(std::wstring const& prefix, std::wstring& out, int timeout_ms) {
        std::unique_lock<std::mutex> lock(snd_mu);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            for (auto it = inbox.begin(); it != inbox.end(); ++it) {
                if (it->rfind(prefix, 0) == 0) {
                    out = *it;
                    inbox.erase(it);
                    return true;
                }
            }
            if (!connected || cancel.load()) return false;
            if (snd_cv.wait_until(lock, deadline) == std::cv_status::timeout) return false;
        }
    }

    void ResetInbox() {
        std::lock_guard<std::mutex> lock(snd_mu);
        inbox.clear();
    }

    bool TryConnect(std::uint64_t address, int address_type) {
        ResetLink();

        SetLink(LinkConnecting, L"Подключение…");
        Log(0, L"Подключение к " + MacText(address) + L"…");

        try {
            // Тип адреса важен: смартфоны и трекеры рекламируются со случайным
            // (random) адресом, и подключение по одному числу не сработает.
            const BluetoothAddressType type =
                address_type == 1 ? BluetoothAddressType::Random : BluetoothAddressType::Public;
            BluetoothLEDevice dev = BluetoothLEDevice::FromBluetoothAddressAsync(address, type).get();
            if (!dev) {
                Log(2, L"Устройство не найдено (возможно, выключено)");
                return false;
            }

            GattSession sess = nullptr;
            try {
                sess = GattSession::FromDeviceIdAsync(dev.BluetoothDeviceId()).get();
                if (sess) sess.MaintainConnection(true);
            } catch (...) {
                Log(1, L"Не удалось удержать соединение (GattSession)");
            }

            GattDeviceService svc = nullptr;
            for (int attempt = 0; attempt < 3 && !svc; ++attempt) {
                auto result = dev.GetGattServicesForUuidAsync(ServiceUuid(),
                                                             BluetoothCacheMode::Uncached).get();
                if (result.Status() == GattCommunicationStatus::Success &&
                    result.Services().Size() > 0) {
                    svc = result.Services().GetAt(0);
                } else if (attempt < 2) {
                    SleepCancelable(400);
                }
            }
            if (!svc) {
                Log(2, L"Сервис передачи не найден. Устройство должно быть в роли «Приёмник».");
                ResetLink();
                return false;
            }

            auto chars = svc.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
            if (chars.Status() != GattCommunicationStatus::Success) {
                Log(2, L"Не удалось получить характеристики: " + CommStatusText(chars.Status()));
                ResetLink();
                return false;
            }

            GattCharacteristic ctrl = nullptr, data = nullptr, stat = nullptr;
            for (auto const& ch : chars.Characteristics()) {
                if (ch.Uuid() == ControlUuid()) {
                    ctrl = ch;
                } else if (ch.Uuid() == DataUuid()) {
                    data = ch;
                } else if (ch.Uuid() == StatusUuid()) {
                    stat = ch;
                }
            }
            if (!ctrl || !data || !stat) {
                Log(2, L"В сервисе нет нужных характеристик Control/Data/Status");
                ResetLink();
                return false;
            }

            auto sub = stat.WriteClientCharacteristicConfigurationDescriptorAsync(
                           GattClientCharacteristicConfigurationDescriptorValue::Notify)
                           .get();
            if (sub != GattCommunicationStatus::Success) {
                Log(2, L"Не удалось подписаться на уведомления: " + CommStatusText(sub));
                ResetLink();
                return false;
            }

            const event_token token = stat.ValueChanged(
                [this](GattCharacteristic const&, GattValueChangedEventArgs const& args) {
                    OnValueChanged(args);
                });

            std::wstring name = dev.Name().c_str();
            int pdu = 23;
            try {
                if (sess) pdu = (int)sess.MaxPduSize();
            } catch (...) {
                pdu = 23;
            }
            pdu = std::clamp(pdu, 23, kMaxPdu);
            const int chunk = std::max(8, pdu - 3 - 4);

            {
                std::lock_guard<std::mutex> lock(snd_mu);
                device = dev;
                service = svc;
                ctrl_ch = ctrl;
                data_ch = data;
                status_ch = stat;
                session = sess;
                value_token = token;
                value_subscribed = true;
                connected = true;
                acked = 0;
                ack_count = 0;
                inbox.clear();
                target = address;
                target_type = address_type;
                target_name = name;
                mtu = pdu;
                payload = chunk;
            }

            device_token = dev.ConnectionStatusChanged(
                [this](BluetoothLEDevice const& d, winrt::Windows::Foundation::IInspectable const&) {
                    OnDeviceConnectionChanged(d.ConnectionStatus());
                });

            int known_rssi = -127;
            {
                std::lock_guard<std::mutex> lock(mu);
                for (auto const& info : devices) {
                    if (info.address == address) known_rssi = info.rssi;
                }
            }

            const std::wstring label =
                name.empty() ? MacText(address)
                             : Format(L"%s (%s)", name.c_str(), MacText(address).c_str());
            Edit([&](Stats& s) {
                s.connected = true;
                s.link = LinkReady;
                s.device = label;
                s.rssi = known_rssi;
                s.mtu = pdu;
                s.payload = chunk;
                s.status = L"Готов к передаче";
            });
            Log(3, Format(L"Подключено: %s, MTU %d, блок данных %d байт", label.c_str(), pdu,
                          chunk));
            return true;
        } catch (hresult_error const& e) {
            Log(2, L"Ошибка подключения: " + std::wstring(e.message().c_str()));
            ResetLink();
            return false;
        } catch (...) {
            Log(2, L"Ошибка подключения");
            ResetLink();
            return false;
        }
    }

    // Освобождает GATT-объекты. Вызывается только из рабочего потока движка.
    void ResetLink() {
        BluetoothLEDevice dev = nullptr;
        GattDeviceService svc = nullptr;
        GattSession sess = nullptr;
        GattCharacteristic stat = nullptr;
        event_token dev_token{}, val_token{};
        bool had_subscription = false;

        {
            std::lock_guard<std::mutex> lock(snd_mu);
            dev = device;
            svc = service;
            sess = session;
            stat = status_ch;
            dev_token = device_token;
            val_token = value_token;
            had_subscription = value_subscribed;

            device = nullptr;
            service = nullptr;
            ctrl_ch = nullptr;
            data_ch = nullptr;
            status_ch = nullptr;
            session = nullptr;
            value_subscribed = false;
            connected = false;
            inbox.clear();
        }
        snd_cv.notify_all();

        try {
            if (stat && had_subscription) stat.ValueChanged(val_token);
        } catch (...) {
        }
        try {
            if (dev) dev.ConnectionStatusChanged(dev_token);
        } catch (...) {
        }
        try {
            if (sess) sess.MaintainConnection(false);
        } catch (...) {
        }
        try {
            if (svc) svc.Close();
        } catch (...) {
        }
        try {
            if (dev) dev.Close();
        } catch (...) {
        }
    }

    void ConnectJob(std::uint64_t address, int address_type) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const bool ok = TryConnect(address, address_type);
        if (!ok) {
            Edit([](Stats& s) {
                s.connected = false;
                s.link = LinkError;
                s.status = L"Не удалось подключиться";
                s.device.clear();
            });
        }
        link_busy.store(false);
        winrt::uninit_apartment();
    }

    // =======================================================================
    //  ПЕРЕДАЧА (Central → Peripheral)
    // =======================================================================
    bool WriteControl(std::wstring const& line) {
        GattCharacteristic ch = nullptr;
        {
            std::lock_guard<std::mutex> lock(snd_mu);
            if (!connected) return false;
            ch = ctrl_ch;
        }
        if (!ch) return false;
        try {
            const std::string text = Utf8(line);
            auto result = ch.WriteValueWithResultAsync(ToBuffer(text),
                                                       GattWriteOption::WriteWithResponse)
                              .get();
            return result.Status() == GattCommunicationStatus::Success;
        } catch (...) {
            return false;
        }
    }

    bool WriteBlock(unsigned long long offset, const std::uint8_t* data, std::size_t size) {
        GattCharacteristic ch = nullptr;
        {
            std::lock_guard<std::mutex> lock(snd_mu);
            if (!connected) return false;
            ch = data_ch;
        }
        if (!ch) return false;

        std::vector<std::uint8_t> frame;
        EncodeFrame(offset, data, size, frame);

        try {
            auto result = ch.WriteValueWithResultAsync(ToBuffer(frame.data(), frame.size()),
                                                       GattWriteOption::WriteWithoutResponse)
                              .get();
            return result.Status() == GattCommunicationStatus::Success;
        } catch (...) {
            return false;
        }
    }

    void AbortRemote() {
        if (connected) WriteControl(L"ABORT");
    }

    double SampleSpeed(unsigned long long done, bool reset) {
        std::lock_guard<std::mutex> lock(sample_mu);
        const unsigned long long now = GetTickCount64();
        if (reset) {
            samples.clear();
            samples.push_back({now, done});
            return 0.0;
        }
        samples.push_back({now, done});
        while (samples.size() > 2 && now - samples.front().first > 1500) samples.pop_front();
        if (samples.size() < 2) return 0.0;
        const auto& first = samples.front();
        const auto& last = samples.back();
        const double dt = (double)(last.first - first.first) / 1000.0;
        if (dt < 0.05) return 0.0;
        return (double)(last.second - first.second) / dt;
    }

    void PublishProgress(unsigned long long done, unsigned long long total, double speed,
                         unsigned long long chunks) {
        Edit([&](Stats& s) {
            s.done = done;
            s.chunks = chunks;
            s.speed = speed;
            s.eta = speed > 1.0 ? (double)(total > done ? total - done : 0) / speed : 0.0;
        });
        if (GetTickCount64() - last_sample_ui >= 250) {
            last_sample_ui = GetTickCount64();
            PushSpeed(speed);
        }
    }

    bool SendFileOnce(std::wstring const& path, std::wstring const& name, unsigned long long size,
                      std::uint32_t crc) {
        ResetInbox();

        // START идемпотентен (приёмник отвечает уже принятым смещением),
        // поэтому его безопасно повторить, если уведомление потерялось.
        std::wstring line;
        bool ready = false;
        for (int attempt = 0; attempt < 3 && !ready; ++attempt) {
            if (!WriteControl(Format(L"START|%llu|%08X|%s", size, crc, name.c_str()))) {
                throw LinkLost("START не отправлен");
            }
            ready = WaitStatus(L"READY|", line, 4000);
            if (!ready) {
                if (cancel.load()) throw Cancelled();
                if (attempt < 2) Log(1, L"Нет ответа на START, повтор…");
            }
        }
        if (!ready) throw LinkLost("приёмник не ответил на START");
        unsigned long long offset = ParseUll(line.substr(6));
        if (offset > size) offset = 0;

        {
            std::lock_guard<std::mutex> lock(snd_mu);
            acked = offset;
        }
        SampleSpeed(offset, true);
        Edit([&](Stats& s) {
            s.resumed = offset;
            s.done = offset;
            s.transfer = TrSending;
            s.status = offset ? L"Возобновление передачи…" : L"Передача файла…";
        });
        if (offset > 0) Log(3, L"Приёмник принял ранее " + SizeText(offset) + L" — продолжаем");

        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("не удалось открыть файл");

        std::vector<std::uint8_t> buffer;
        unsigned long long cursor = offset;
        unsigned long long last_ack = offset;
        unsigned long long last_ui = 0;
        unsigned long long chunks = 0;
        unsigned long long last_ack_tick = GetTickCount64();
        bool ok = true;

        try {
            while (cursor < size) {
                if (cancel.load()) throw Cancelled();

                unsigned long long ack_now = 0;
                int chunk = 0;
                bool linked = false;
                {
                    std::lock_guard<std::mutex> lock(snd_mu);
                    ack_now = acked;
                    chunk = payload;
                    linked = connected;
                }
                if (!linked) throw LinkLost("соединение закрыто");

                if (cursor > ack_now) cursor = ack_now;   // откат к подтверждённому
                if (ack_now > last_ack) {
                    last_ack = ack_now;
                    last_ack_tick = GetTickCount64();
                }

                const unsigned long long window =
                    std::max<unsigned long long>(kWindowMin, (unsigned long long)chunk * 24);
                if (cursor - ack_now >= window) {
                    std::unique_lock<std::mutex> lock(snd_mu);
                    snd_cv.wait_for(lock, 120ms);
                    if (GetTickCount64() - last_ack_tick > 6000) throw Stall("нет подтверждений");
                    continue;
                }

                if (chunk <= 0) chunk = 8;
                if (buffer.size() != (size_t)chunk) buffer.resize((size_t)chunk);

                LARGE_INTEGER position{};
                position.QuadPart = (LONGLONG)cursor;
                if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
                    throw std::runtime_error("позиционирование в файле");
                }
                DWORD got = 0;
                if (!ReadFile(file, buffer.data(), (DWORD)buffer.size(), &got, nullptr) || got == 0) {
                    throw std::runtime_error("чтение файла");
                }

                if (!WriteBlock(cursor, buffer.data(), got)) {
                    throw LinkLost("блок не записан");
                }
                cursor += got;
                ++chunks;

                const unsigned long long now = GetTickCount64();
                if (now - last_ui >= 100) {
                    last_ui = now;
                    const double speed = SampleSpeed(ack_now, false);
                    PublishProgress(ack_now, size, speed, chunks);
                }
            }
        } catch (...) {
            CloseHandle(file);
            throw;
        }
        CloseHandle(file);

        // Ждём подтверждения последних блоков. Приёмник подтверждает раз в
        // 8 КБ/100 мс, поэтому для маленького файла последний ACK мог ещё не
        // прийти — просим SYNC, чтобы не ждать таймаут зря.
        const unsigned long long wait_start = GetTickCount64();
        unsigned long long last_sync = 0;
        for (;;) {
            if (cancel.load()) throw Cancelled();
            unsigned long long ack_now = 0;
            {
                std::lock_guard<std::mutex> lock(snd_mu);
                ack_now = acked;
            }
            if (ack_now >= size) break;
            if (GetTickCount64() - wait_start > 8000) throw Stall("последние блоки не подтверждены");
            if (ack_now < size && GetTickCount64() - last_sync >= 400) {
                last_sync = GetTickCount64();
                WriteControl(L"SYNC");
            }
            {
                std::unique_lock<std::mutex> lock(snd_mu);
                snd_cv.wait_for(lock, 100ms);
            }
            PublishProgress(ack_now, size, SampleSpeed(ack_now, false), chunks);
        }

        Log(0, L"Все блоки доставлены, приёмник проверяет CRC32…");
        if (!WriteControl(Format(L"END|%llu|%08X", size, crc))) throw LinkLost("END не отправлен");
        if (!WaitStatus(L"DONE|", line, 30000)) throw LinkLost("нет ответа DONE");

        const auto parts = Split(line, L'|', 6);
        const unsigned int remote_crc =
            parts.size() > 2 ? (unsigned int)wcstoul(parts[2].c_str(), nullptr, 16) : 0;
        const bool match = parts.size() > 3 && parts[3] == L"OK";
        ok = match && remote_crc == crc;

        const std::wstring saved = parts.size() > 4 ? parts[4] : L"";

        Edit([&](Stats& s) {
            s.done = size;
            s.crc_actual = remote_crc;
            s.crc_ok = ok;
            s.transfer = ok ? TrDone : TrFailed;
            s.link = ok ? LinkDone : LinkError;
            s.speed = 0.0;
            s.eta = 0.0;
            s.status = ok ? L"Передача завершена" : L"CRC32 не совпал";
        });

        if (ok) {
            const std::wstring extra = saved.empty() ? std::wstring() : (L", файл: " + saved);
            Log(3, Format(L"Готово: %s, CRC32 %08X совпал%s", SizeText(size).c_str(), remote_crc,
                          extra.c_str()));
        } else {
            Log(2, Format(L"CRC32 не совпал: отправитель %08X, приёмник %08X", crc, remote_crc));
        }
        return ok;
    }

    void SendJob(std::wstring path, std::uint64_t address, int address_type) {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        send_busy.store(true);
        cancel.store(false);

        unsigned long long size = 0;
        const std::uint32_t crc = Crc32File(path, &size, &cancel);
        const std::wstring name = SanitizeName(FileNamePart(path));

        if (cancel.load() || size == 0) {
            Log(2, L"Файл пуст или не читается: " + path);
            Edit([](Stats& s) {
                s.transfer = TrFailed;
                s.link = LinkError;
                s.status = L"Файл не прочитан";
            });
            send_busy.store(false);
            winrt::uninit_apartment();
            return;
        }
        if (size > kMaxFileSize) {
            Log(2, L"Файл больше 4 ГБ — формат кадра это не поддерживает");
            Edit([](Stats& s) {
                s.transfer = TrFailed;
                s.link = LinkError;
                s.status = L"Файл слишком велик";
            });
            send_busy.store(false);
            winrt::uninit_apartment();
            return;
        }

        Edit([&](Stats& s) {
            s.role = RoleSender;
            s.file = name;
            s.file_path = path;
            s.total = size;
            s.done = 0;
            s.chunks = 0;
            s.acks = 0;
            s.resumed = 0;
            s.has_crc = true;
            s.crc_expected = crc;
            s.crc_actual = 0;
            s.crc_ok = false;
            s.speed = 0.0;
            s.eta = 0.0;
            s.speed_hist.clear();
            s.transfer = TrSending;
            s.status = L"Подготовка…";
        });
        Log(0, Format(L"Файл «%s»: %s, CRC32 %08X", name.c_str(), SizeText(size).c_str(), crc));

        bool finished = false;
        bool link_failed = false;

        for (int attempt = 0; attempt <= kMaxAttempts && !cancel.load(); ++attempt) {
            JoinThread(link_thread);
            if (!connected) {
                if (!TryConnect(address, address_type)) {
                    if (cancel.load()) break;
                    link_failed = true;
                    SleepCancelable(1800);
                    continue;
                }
            }
            link_failed = false;

            try {
                const bool ok = SendFileOnce(path, name, size, crc);
                finished = true;
                (void)ok;
                break;
            } catch (Cancelled const&) {
                Log(1, L"Передача отменена пользователем");
                AbortRemote();
                ResetInbox();
                Edit([](Stats& s) {
                    s.transfer = TrPaused;
                    s.link = LinkIdle;
                    s.status = L"Передача отменена";
                    s.speed = 0.0;
                    s.eta = 0.0;
                });
                break;
            } catch (Stall const& e) {
                Log(2, Format(L"Приём остановился: %hs", e.what()));
                Edit([](Stats& s) {
                    s.transfer = TrFailed;
                    s.link = LinkPaused;
                    s.status = L"Приём не отвечает";
                });
                break;
            } catch (LinkLost const& e) {
                Log(1, Format(L"Связь прервана: %hs", e.what()));
                {
                    std::lock_guard<std::mutex> lock(snd_mu);
                    connected = false;
                }
                Edit([](Stats& s) {
                    s.connected = false;
                    s.transfer = TrPaused;
                    s.link = LinkPaused;
                    s.status = L"Связь потеряна — восстановление…";
                });
                if (attempt < kMaxAttempts) {
                    Log(0, Format(L"Попытка восстановить соединение (%d из %d)…", attempt + 1,
                                  kMaxAttempts));
                    ResetLink();
                    SleepCancelable(2000);
                }
            } catch (std::exception const& e) {
                Log(2, Format(L"Передача прервана: %hs", e.what()));
                Edit([](Stats& s) {
                    s.transfer = TrFailed;
                    s.link = LinkError;
                    s.status = L"Ошибка передачи";
                });
                break;
            }
        }

        if (!finished && !cancel.load()) {
            Edit([&](Stats& s) {
                if (s.transfer == TrSending) {
                    s.transfer = TrFailed;
                    s.status = link_failed ? L"Устройство недоступно" : L"Передача не завершена";
                    if (link_failed) s.link = LinkError;
                }
                s.speed = 0.0;
                s.eta = 0.0;
            });
            if (link_failed) Log(2, L"Не удалось установить соединение с приёмником");
        }

        send_busy.store(false);

        // Успешную связь не рвём: следующую передачу можно начать сразу.
        bool keep_link = false;
        {
            std::lock_guard<std::mutex> lock(snd_mu);
            keep_link = connected;
        }
        if (!keep_link) {
            try {
                ResetLink();
            } catch (...) {
            }
        }
    }

    // =======================================================================
    //  ПРИЁМНИК (Peripheral / GATT-сервер)
    // =======================================================================
    void OnControlWrite(GattWriteRequestedEventArgs const& args) {
        auto deferral = args.GetDeferral();
        try {
            auto request = args.GetRequestAsync().get();
            const auto bytes = FromBuffer(request.Value());
            if (request.Option() == GattWriteOption::WriteWithResponse) request.Respond();
            const std::string text(bytes.begin(), bytes.end());
            HandleControl(Wide(text));
        } catch (...) {
        }
        deferral.Complete();
    }

    void OnDataWrite(GattWriteRequestedEventArgs const& args) {
        auto deferral = args.GetDeferral();
        try {
            auto request = args.GetRequestAsync().get();
            const auto bytes = FromBuffer(request.Value());
            if (request.Option() == GattWriteOption::WriteWithResponse) request.Respond();
            HandleData(bytes);
        } catch (...) {
        }
        deferral.Complete();
    }

    void OnStatusRead(GattReadRequestedEventArgs const& args) {
        auto deferral = args.GetDeferral();
        try {
            auto request = args.GetRequestAsync().get();
            std::wstring line;
            {
                std::lock_guard<std::mutex> lock(recv_mu);
                line = r_line;
            }
            request.RespondWithValue(ToBuffer(Utf8(line)));
        } catch (...) {
        }
        deferral.Complete();
    }

    void OnSubscribersChanged() {
        bool has_client = false;
        try {
            GattLocalCharacteristic status = nullptr;
            {
                std::lock_guard<std::mutex> lock(recv_mu);
                status = local_status;
            }
            if (status) has_client = status.SubscribedClients().Size() > 0;
        } catch (...) {
        }

        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(recv_mu);
            changed = (central_here != has_client);
            central_here = has_client;
        }
        if (!changed) return;

        if (has_client) {
            Log(3, L"Central подключился к GATT-серверу");
            Edit([](Stats& s) {
                s.connected = true;
                s.link = s.transfer == TrReceiving ? LinkTransfer : LinkReady;
                s.status = L"Central подключён";
            });
        } else {
            Log(1, L"Central отключился — сеанс сохранён для возобновления");
            Edit([](Stats& s) {
                s.connected = false;
                if (s.transfer == TrReceiving) {
                    s.transfer = TrPaused;
                    s.status = L"Связь потеряна — ждём повторного подключения";
                } else {
                    s.status = L"Ожидание подключения…";
                }
                s.link = s.transfer == TrPaused ? LinkPaused : LinkAdvertising;
            });
        }
    }

    void NotifyStatus(std::wstring const& line) {
        GattLocalCharacteristic status = nullptr;
        {
            std::lock_guard<std::mutex> lock(recv_mu);
            r_line = line;
            status = local_status;
        }
        if (!status) return;
        try {
            if (status.SubscribedClients().Size() == 0) return;
            status.NotifyValueAsync(ToBuffer(Utf8(line))).get();
        } catch (...) {
        }
    }

    void HandleControl(std::wstring const& line) {
        const auto parts = Split(line, L'|', 4);
        if (parts.empty()) return;

        if (parts[0] == L"START") {
            StartSession(parts);
        } else if (parts[0] == L"END") {
            FinishSession(parts);
        } else if (parts[0] == L"ABORT") {
            AbortSession();
        } else if (parts[0] == L"SYNC") {
            SyncStatus();
        } else {
            NotifyStatus(L"ERR|неизвестная команда");
        }
    }

    void StartSession(std::vector<std::wstring> const& parts) {
        const unsigned long long size = parts.size() > 1 ? ParseUll(parts[1]) : 0;
        const unsigned int crc =
            parts.size() > 2 ? (unsigned int)wcstoul(parts[2].c_str(), nullptr, 16) : 0;
        const std::wstring name = SanitizeName(parts.size() > 3 ? parts[3] : L"received.bin");

        if (size == 0) {
            NotifyStatus(L"ERR|нулевой размер");
            return;
        }

        std::wstring dir;
        {
            std::lock_guard<std::mutex> lock(mu);
            dir = incoming_dir;
        }
        EnsureDir(dir);
        const std::wstring path = JoinPath(dir, name);

        unsigned long long offset = 0;
        bool resumed = false;
        bool failed = false;

        {
            std::lock_guard<std::mutex> lock(recv_mu);
            if (r_has && r_size == size && r_crc_expect == crc && r_name == name &&
                file != INVALID_HANDLE_VALUE) {
                offset = r_received;
                resumed = offset > 0;
            } else {
                if (file != INVALID_HANDLE_VALUE) {
                    FlushFileBuffers(file);
                    CloseHandle(file);
                    file = INVALID_HANDLE_VALUE;
                }
                r_has = false;
                r_done = false;

                HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle == INVALID_HANDLE_VALUE) {
                    failed = true;
                } else {
                    file = handle;
                    r_name = name;
                    r_path = path;
                    r_size = size;
                    r_crc_expect = crc;
                    r_received = 0;
                    r_chunks = 0;
                    r_ack_mark = 0;
                    r_last_ack = 0;
                    r_last_ui = 0;
                    r_has = true;
                    r_done = false;
                    offset = 0;
                }
            }
        }

        if (failed) {
            Log(2, L"Не удалось создать файл: " + path);
            NotifyStatus(L"ERR|файл не создан");
            return;
        }

        Edit([&](Stats& s) {
            s.connected = true;
            s.transfer = TrReceiving;
            s.file = name;
            s.file_path = path;
            s.total = size;
            s.done = offset;
            s.chunks = 0;
            s.acks = 0;
            s.resumed = offset;
            s.has_crc = true;
            s.crc_expected = crc;
            s.crc_actual = 0;
            s.crc_ok = false;
            s.speed = 0.0;
            s.eta = 0.0;
            s.speed_hist.clear();
            s.link = LinkTransfer;
            s.status = offset ? L"Приём (продолжение)" : L"Приём файла…";
        });
        sample_receiver(offset, true);

        if (resumed) {
            Log(3, Format(L"Приём «%s»: продолжаем с %s из %s", name.c_str(),
                          SizeText(offset).c_str(), SizeText(size).c_str()));
        } else {
            Log(0, Format(L"Приём «%s»: %s, ожидаемый CRC32 %08X", name.c_str(),
                          SizeText(size).c_str(), crc));
        }
        NotifyStatus(Format(L"READY|%llu", offset));
    }

    void FinishSession(std::vector<std::wstring> const& parts) {
        std::wstring path;
        unsigned long long size = 0;
        unsigned int expected = 0;
        unsigned long long received = 0;
        bool valid = false;

        {
            std::lock_guard<std::mutex> lock(recv_mu);
            valid = r_has && file != INVALID_HANDLE_VALUE;
            if (valid) {
                FlushFileBuffers(file);
                CloseHandle(file);
                file = INVALID_HANDLE_VALUE;
                path = r_path;
                size = r_size;
                expected = (unsigned int)r_crc_expect;
                received = r_received;
            }
        }

        if (!valid) {
            NotifyStatus(L"ERR|сеанс не начат");
            return;
        }

        unsigned long long file_size = 0;
        const unsigned int actual = Crc32File(path, &file_size, nullptr);
        const bool ok = (file_size == size) && (actual == expected);

        {
            std::lock_guard<std::mutex> lock(recv_mu);
            r_done = ok;
            // Сеанс закрыт в любом случае: повторный START создаст файл заново,
            // иначе неудачная передача дописалась бы поверх уже принятых данных.
            r_has = false;
            r_received = 0;
            r_chunks = 0;
        }

        NotifyStatus(Format(L"DONE|%llu|%08X|%s|%s", file_size, actual, ok ? L"OK" : L"MISMATCH",
                            path.c_str()));

        Edit([&](Stats& s) {
            s.done = file_size;
            s.crc_actual = actual;
            s.crc_ok = ok;
            s.transfer = ok ? TrDone : TrFailed;
            s.link = ok ? LinkDone : LinkError;
            s.speed = 0.0;
            s.eta = 0.0;
            s.status = ok ? L"Файл принят, CRC32 совпал" : L"CRC32 не совпал";
        });

        if (ok) {
            Log(3, Format(L"Принято %s, CRC32 %08X совпал, файл: %s", SizeText(file_size).c_str(),
                          actual, path.c_str()));
        } else {
            Log(2, Format(L"CRC32 не совпал: ожидали %08X, получили %08X (получено %s из %s)",
                          expected, actual, SizeText(file_size).c_str(),
                          SizeText(size).c_str()));
        }
        (void)parts;
        (void)received;
    }

    // Отправитель просит прислать текущее смещение (например, перед END).
    void SyncStatus() {
        unsigned long long received = 0;
        {
            std::lock_guard<std::mutex> lock(recv_mu);
            received = r_has ? r_received : 0;
        }
        NotifyStatus(Format(L"ACK|%llu", received));
    }

    void AbortSession() {
        {
            std::lock_guard<std::mutex> lock(recv_mu);
            if (file != INVALID_HANDLE_VALUE) {
                FlushFileBuffers(file);
                CloseHandle(file);
                file = INVALID_HANDLE_VALUE;
            }
            r_has = false;
            r_done = false;
        }
        NotifyStatus(L"ABORTED");
        Log(1, L"Передача отменена отправителем (частичный файл сохранён)");
        Edit([](Stats& s) {
            s.transfer = TrPaused;
            s.status = L"Передача отменена отправителем";
            s.speed = 0.0;
            s.eta = 0.0;
        });
    }

    void HandleData(std::vector<std::uint8_t> const& bytes) {
        std::uint32_t offset = 0;
        const std::uint8_t* payload = nullptr;
        std::size_t count = 0;
        if (!DecodeFrame(bytes.data(), bytes.size(), offset, payload, count)) return;

        bool need_ack = false;
        bool progress = false;
        unsigned long long acknowledged = 0;

        {
            std::lock_guard<std::mutex> lock(recv_mu);
            if (!r_has || file == INVALID_HANDLE_VALUE || r_done) return;

            if (offset != (std::uint32_t)r_received) {
                // Дубликат или пропуск: просим повторить с подтверждённой позиции.
                need_ack = true;
                acknowledged = r_received;
            } else {
                DWORD written = 0;
                const BOOL ok = WriteFile(file, payload, (DWORD)count, &written, nullptr);
                if (ok && written == count) {
                    r_received += written;
                    ++r_chunks;
                    const unsigned long long now = GetTickCount64();
                    if (r_received - r_ack_mark >= 8192 || now - r_last_ack >= 100) {
                        r_ack_mark = r_received;
                        r_last_ack = now;
                        need_ack = true;
                        progress = now - r_last_ui >= 100;
                        if (progress) r_last_ui = now;
                    }
                    acknowledged = r_received;
                } else {
                    need_ack = true;
                    acknowledged = r_received;
                }
            }
        }

        if (need_ack) NotifyStatus(Format(L"ACK|%llu", acknowledged));

        if (progress) {
            unsigned long long received = 0;
            unsigned long long total = 0;
            unsigned long long chunks = 0;
            {
                std::lock_guard<std::mutex> lock(recv_mu);
                received = r_received;
                total = r_size;
                chunks = r_chunks;
            }
            const double speed = sample_receiver(received, false);
            Edit([&](Stats& s) {
                s.done = received;
                s.total = total;
                s.chunks = chunks;
                s.speed = speed;
                s.eta = speed > 1.0 ? (double)(total > received ? total - received : 0) / speed
                                    : 0.0;
            });
            if (GetTickCount64() - last_sample_ui >= 250) {
                last_sample_ui = GetTickCount64();
                PushSpeed(speed);
            }
        }
    }

    double sample_receiver(unsigned long long received, bool reset) {
        std::lock_guard<std::mutex> lock(sample_mu);
        const unsigned long long now = GetTickCount64();
        if (reset) {
            samples.clear();
            samples.push_back({now, received});
            return 0.0;
        }
        samples.push_back({now, received});
        while (samples.size() > 2 && now - samples.front().first > 1500) samples.pop_front();
        if (samples.size() < 2) return 0.0;
        const auto& first = samples.front();
        const auto& last = samples.back();
        const double dt = (double)(last.first - first.first) / 1000.0;
        if (dt < 0.05) return 0.0;
        return (double)(last.second - first.second) / dt;
    }

    void ReceiverMain() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if (!CheckAdapter(true)) {
            winrt::uninit_apartment();
            return;
        }

        GattServiceProvider provider_local = nullptr;
        GattLocalCharacteristic ctrl_local = nullptr;
        GattLocalCharacteristic data_local = nullptr;
        GattLocalCharacteristic status_local = nullptr;
        event_token t_ctrl{}, t_data{}, t_read{}, t_subs{};

        try {
            auto result = GattServiceProvider::CreateAsync(ServiceUuid()).get();
            if (result.Error() != BluetoothError::Success) {
                Log(2, Format(L"Не удалось создать GATT-сервис: %s. Нужен адаптер с поддержкой роли "
                              L"Peripheral (Windows 10 1809+).",
                              BluetoothErrorText(result.Error()).c_str()));
                Edit([](Stats& s) {
                    s.link = LinkError;
                    s.status = L"GATT-сервер недоступен";
                    s.scanning = false;
                });
                winrt::uninit_apartment();
                return;
            }

            provider_local = result.ServiceProvider();
            auto service = provider_local.Service();

            {
                GattLocalCharacteristicParameters params;
                params.CharacteristicProperties(
                    Props(PropWrite | PropWriteWithoutResponse));
                params.WriteProtectionLevel(GattProtectionLevel::Plain);
                params.UserDescription(L"Команды протокола передачи");
                auto created = service.CreateCharacteristicAsync(ControlUuid(), params).get();
                if (created.Error() != BluetoothError::Success) {
                    throw std::runtime_error("не создана характеристика Control");
                }
                ctrl_local = created.Characteristic();
            }
            {
                GattLocalCharacteristicParameters params;
                params.CharacteristicProperties(
                    Props(PropWriteWithoutResponse | PropWrite));
                params.WriteProtectionLevel(GattProtectionLevel::Plain);
                params.UserDescription(L"Блоки файла");
                auto created = service.CreateCharacteristicAsync(DataUuid(), params).get();
                if (created.Error() != BluetoothError::Success) {
                    throw std::runtime_error("не создана характеристика Data");
                }
                data_local = created.Characteristic();
            }
            {
                GattLocalCharacteristicParameters params;
                params.CharacteristicProperties(Props(PropNotify | PropRead));
                params.ReadProtectionLevel(GattProtectionLevel::Plain);
                params.UserDescription(L"Подтверждения и состояние");
                auto created = service.CreateCharacteristicAsync(StatusUuid(), params).get();
                if (created.Error() != BluetoothError::Success) {
                    throw std::runtime_error("не создана характеристика Status");
                }
                status_local = created.Characteristic();
            }

            t_ctrl = ctrl_local.WriteRequested(
                [this](GattLocalCharacteristic const&, GattWriteRequestedEventArgs const& args) {
                    OnControlWrite(args);
                });
            t_data = data_local.WriteRequested(
                [this](GattLocalCharacteristic const&, GattWriteRequestedEventArgs const& args) {
                    OnDataWrite(args);
                });
            t_read = status_local.ReadRequested(
                [this](GattLocalCharacteristic const&, GattReadRequestedEventArgs const& args) {
                    OnStatusRead(args);
                });
            t_subs = status_local.SubscribedClientsChanged(
                [this](GattLocalCharacteristic const&, winrt::Windows::Foundation::IInspectable const&) {
                    OnSubscribersChanged();
                });

            {
                std::lock_guard<std::mutex> lock(recv_mu);
                provider = provider_local;
                local_ctrl = ctrl_local;
                local_data = data_local;
                local_status = status_local;
                tok_ctrl = t_ctrl;
                tok_data = t_data;
                tok_read = t_read;
                tok_subs = t_subs;
                receiving = true;
                r_line = L"IDLE";
            }

            GattServiceProviderAdvertisingParameters advertising;
            advertising.IsConnectable(true);
            advertising.IsDiscoverable(true);
            provider_local.StartAdvertising(advertising);

            Log(3, L"GATT-сервис опубликован. Идёт BLE-реклама — ожидание Central…");
            Edit([](Stats& s) {
                s.role = RoleReceiver;
                s.link = LinkAdvertising;
                s.scanning = true;
                s.status = L"Ожидание подключения…";
                s.device = L"Приёмник (GATT-сервер)";
            });

            while (!recv_stop.load()) Sleep(100);

            try {
                provider_local.StopAdvertising();
            } catch (...) {
            }
        } catch (hresult_error const& e) {
            Log(2, L"Ошибка GATT-сервера: " + std::wstring(e.message().c_str()));
            Edit([](Stats& s) {
                s.link = LinkError;
                s.status = L"GATT-сервер не запущен";
                s.scanning = false;
            });
        } catch (std::exception const& e) {
            Log(2, Format(L"Ошибка GATT-сервера: %hs", e.what()));
            Edit([](Stats& s) {
                s.link = LinkError;
                s.status = L"GATT-сервер не запущен";
                s.scanning = false;
            });
        }

        // --- остановка: сначала снимаем обработчики, потом удаляем объекты ---
        {
            std::lock_guard<std::mutex> lock(recv_mu);
            if (file != INVALID_HANDLE_VALUE) {
                FlushFileBuffers(file);
                CloseHandle(file);
                file = INVALID_HANDLE_VALUE;
            }
            r_has = false;
        }

        try {
            if (ctrl_local) ctrl_local.WriteRequested(t_ctrl);
        } catch (...) {
        }
        try {
            if (data_local) data_local.WriteRequested(t_data);
        } catch (...) {
        }
        try {
            if (status_local) {
                status_local.ReadRequested(t_read);
                status_local.SubscribedClientsChanged(t_subs);
            }
        } catch (...) {
        }
        try {
            if (provider_local) provider_local.StopAdvertising();
        } catch (...) {
        }

        {
            std::lock_guard<std::mutex> lock(recv_mu);
            provider = nullptr;
            local_ctrl = nullptr;
            local_data = nullptr;
            local_status = nullptr;
            receiving = false;
            central_here = false;
        }

        Edit([](Stats& s) {
            s.scanning = false;
            s.connected = false;
            if (s.link == LinkAdvertising || s.link == LinkReady) s.link = LinkIdle;
            s.status = L"Приём остановлен";
        });
        Log(0, L"GATT-сервер остановлен");

        // Локальные ссылки отпускаем до uninit_apartment.
        provider_local = nullptr;
        ctrl_local = nullptr;
        data_local = nullptr;
        status_local = nullptr;

        winrt::uninit_apartment();
    }

    // =======================================================================
    //  Остановка
    // =======================================================================
    void StopScan() {
        scan_stop.store(true);
        JoinThread(scan_thread);
        scan_active.store(false);
    }

    void StopReceiving() {
        recv_stop.store(true);
        JoinThread(recv_thread);
        recv_active.store(false);
    }

    void StopSending() {
        cancel.store(true);
        JoinThread(send_thread);
        JoinThread(link_thread);
    }

    void ClearWinrt() {
        provider = nullptr;
        local_ctrl = nullptr;
        local_data = nullptr;
        local_status = nullptr;
        device = nullptr;
        service = nullptr;
        ctrl_ch = nullptr;
        data_ch = nullptr;
        status_ch = nullptr;
        session = nullptr;
    }

    ~Impl() {
        scan_stop.store(true);
        recv_stop.store(true);
        cancel.store(true);
        JoinThread(scan_thread);
        JoinThread(recv_thread);
        JoinThread(send_thread);
        JoinThread(link_thread);
    }
};

// ===========================================================================
//  Engine — тонкая обёртка над Impl
// ===========================================================================
Engine::Engine() : impl_(new Impl()) {}

Engine::~Engine() { Shutdown(); }

void Engine::SetNotifyWindow(HWND hwnd) { impl_->notify = hwnd; }

void Engine::SetIncomingDir(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->incoming_dir = dir;
}

std::wstring Engine::IncomingDir() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->incoming_dir;
}

Role Engine::CurrentRole() const { return impl_->role; }

void Engine::SetRole(Role role) {
    Impl& d = *impl_;
    if (d.role == role) return;

    d.StopSending();
    d.StopScan();
    d.StopReceiving();

    {
        std::lock_guard<std::mutex> lock(d.mu);
        d.devices.clear();
        d.stats = Stats{};
        d.stats.role = (int)role;
        d.stats.status =
            role == RoleSender ? L"Сканирование не запущено" : L"Приём не запущен";
        d.stats.speed_hist.clear();
    }
    d.role = role;
    d.cancel.store(false);
    d.scan_stop.store(true);
    d.recv_stop.store(true);

    d.Log(0, role == RoleSender ? L"Роль: отправитель (GATT Central)"
                                : L"Роль: приёмник (GATT Peripheral)");
    d.NotifyUi();
}

void Engine::StartScan() {
    Impl& d = *impl_;
    if (d.role != RoleSender) return;

    // Флаг выставляется синхронно: второй клик «Поиск» не станет джойнить
    // живой поток сканирования (иначе UI завис бы навсегда).
    bool expected = false;
    if (!d.scan_active.compare_exchange_strong(expected, true)) return;

    bool running = false;
    {
        std::lock_guard<std::mutex> lock(d.mu);
        running = d.stats.scanning;
    }
    if (running) {
        d.scan_active.store(false);
        return;
    }

    JoinThread(d.scan_thread);   // предыдущий поток уже завершился
    d.scan_stop.store(false);
    d.scan_thread = std::thread([&d] {
        try {
            d.ScanMain();
        } catch (...) {
        }
        d.scan_active.store(false);
    });
}

void Engine::StopScan() {
    Impl& d = *impl_;
    if (!d.scan_thread.joinable()) return;
    d.StopScan();
    d.Log(0, L"Сканирование остановлено");
}

std::vector<DeviceInfo> Engine::Devices() const { return impl_->DeviceList(); }

void Engine::ClearDevices() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->devices.clear();
    impl_->NotifyUi();
}

bool Engine::Connect(std::size_t index) {
    Impl& d = *impl_;
    if (d.role != RoleSender) return false;
    if (d.send_busy.load()) {
        d.Log(1, L"Идёт передача — дождитесь окончания");
        return false;
    }
    bool expected = false;
    if (!d.link_busy.compare_exchange_strong(expected, true)) {
        d.Log(1, L"Подключение уже выполняется");
        return false;
    }

    std::vector<DeviceInfo> list = d.DeviceList();
    if (index >= list.size()) {
        d.link_busy.store(false);
        return false;
    }

    d.StopScan();

    JoinThread(d.link_thread);
    {
        std::lock_guard<std::mutex> lock(d.snd_mu);
        d.target = list[index].address;
        d.target_type = list[index].address_type;
        d.target_name = list[index].name;
    }

    const std::uint64_t address = list[index].address;
    const int address_type = list[index].address_type;
    d.link_thread = std::thread([&d, address, address_type] {
        try {
            d.ConnectJob(address, address_type);
        } catch (...) {
            d.link_busy.store(false);
        }
    });
    return true;
}

void Engine::Disconnect() {
    Impl& d = *impl_;
    if (d.send_busy.load()) {
        d.Log(1, L"Сначала остановите передачу");
        return;
    }
    JoinThread(d.link_thread);
    {
        std::thread worker([&d] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            d.ResetLink();
            winrt::uninit_apartment();
        });
        worker.join();
    }
    d.Edit([](Stats& s) {
        s.connected = false;
        s.link = LinkIdle;
        s.status = L"Не подключено";
        s.device.clear();
        s.mtu = 0;
        s.payload = 0;
    });
    d.Log(0, L"Отключено");
}

bool Engine::StartSend(const std::wstring& path) {
    Impl& d = *impl_;
    if (d.role != RoleSender) return false;
    // Атомарно занимаем «слот передачи»: второй клик не запустит вторую копию.
    bool expected = false;
    if (!d.send_busy.compare_exchange_strong(expected, true)) {
        d.Log(1, L"Передача уже выполняется");
        return false;
    }
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        d.Log(2, L"Файл не найден: " + path);
        d.send_busy.store(false);
        return false;
    }

    std::uint64_t address = 0;
    int address_type = 0;
    {
        std::lock_guard<std::mutex> lock(d.snd_mu);
        address = d.target;
        address_type = d.target_type;
    }
    if (address == 0) {
        d.Log(2, L"Сначала выберите устройство в списке и подключитесь");
        d.send_busy.store(false);
        return false;
    }

    d.cancel.store(false);
    JoinThread(d.send_thread);
    d.send_thread = std::thread([&d, path, address, address_type] {
        try {
            d.SendJob(path, address, address_type);
        } catch (...) {
            d.send_busy.store(false);
        }
    });
    return true;
}

void Engine::Cancel() {
    Impl& d = *impl_;
    if (!d.send_busy.load()) return;
    d.cancel.store(true);
    d.snd_cv.notify_all();
    d.Log(1, L"Запрошена отмена передачи…");
}

bool Engine::StartReceiving() {
    Impl& d = *impl_;
    if (d.role != RoleReceiver) return false;

    bool expected = false;
    if (!d.recv_active.compare_exchange_strong(expected, true)) return false;

    bool running = false;
    {
        std::lock_guard<std::mutex> lock(d.recv_mu);
        running = d.receiving;
    }
    if (running) {
        d.recv_active.store(false);
        return false;
    }

    JoinThread(d.recv_thread);
    d.recv_stop.store(false);
    d.recv_thread = std::thread([&d] {
        try {
            d.ReceiverMain();
        } catch (...) {
        }
        d.recv_active.store(false);
    });
    return true;
}

void Engine::StopReceiving() {
    Impl& d = *impl_;
    if (!d.recv_thread.joinable()) return;
    d.StopReceiving();
}

Stats Engine::GetStats() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->stats;
}

std::vector<LogEntry> Engine::TakeLog() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::vector<LogEntry> out;
    out.swap(impl_->log);
    return out;
}

void Engine::Shutdown() {
    Impl& d = *impl_;
    d.cancel.store(true);
    d.StopSending();
    d.StopScan();
    d.StopReceiving();

    // WinRT-объекты освобождаем в MTA-потоке: UI-поток живёт в STA.
    std::thread worker([&d] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        d.ResetLink();
        d.ClearWinrt();
        winrt::uninit_apartment();
    });
    worker.join();
}

}  // namespace ble
