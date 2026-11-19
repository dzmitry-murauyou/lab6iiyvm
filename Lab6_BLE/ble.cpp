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
#include <winrt/Windows.Devices.Enumeration.h>
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
#include <map>
#include <mutex>
#include <set>
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
using namespace Windows::Devices::Enumeration;
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
// Дополнительные каналы данных: тот же формат кадра, тот же сервис.
guid const& Data2Uuid() {
    static const guid value = MakeGuid(L"A1B2C3D4-5E6F-4A7B-8C9D-0E1F2A3B4C64");
    return value;
}
guid const& Data3Uuid() {
    static const guid value = MakeGuid(L"A1B2C3D4-5E6F-4A7B-8C9D-0E1F2A3B4C65");
    return value;
}
guid const& Data4Uuid() {
    static const guid value = MakeGuid(L"A1B2C3D4-5E6F-4A7B-8C9D-0E1F2A3B4C66");
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
// Окно неподтверждённых данных. Write Without Response не повторяет записи:
// если залить больше, чем буфер приёмника, блоки теряются и передача встаёт
// на бесконечных пересылках. Поэтому окно маленькое и адаптивное.
inline unsigned long long NowUs() {
    return (unsigned long long)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr unsigned long long kWindowBytesTarget = 4096;   // сколько байт держим «в полёте»
constexpr int kWindowBlocksMin = 4;
constexpr unsigned long long kStallMs = 1500;   // нет ACK — пробуем откатиться
constexpr unsigned long long kWindowBytesMax = 8192;  // больше в полёте не держим
constexpr int kWindowBlocksMax = 32;
// Как часто приёмник подтверждает принятое.
// Подтверждение — тоже PDU, и оно занимает слот в интервале связи, как и
// блок данных. Поэтому подтверждаем редко: раз в ~3 КБ (6 блоков при MTU 517)
// или раз в 800 мс, смотря что наступит раньше.
constexpr unsigned long long kAckBytes = 3060;
constexpr unsigned long long kAckIntervalMs = 800;
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

// MAC из строки идентификатора устройства Windows:
// «BluetoothLE#BluetoothLE<адаптер>-AA:BB:CC:DD:EE:FF». 0 — не разобрали.
std::uint64_t MacFromDeviceId(std::wstring const& id) {
    const std::size_t dash = id.find_last_of(L'-');
    if (dash == std::wstring::npos || id.size() < dash + 18) return 0;
    const std::wstring tail = id.substr(dash + 1, 17);
    std::uint64_t mac = 0;
    int digits = 0;
    for (wchar_t ch : tail) {
        if (ch == L':') continue;
        int value = -1;
        if (ch >= L'0' && ch <= L'9') value = ch - L'0';
        else if (ch >= L'A' && ch <= L'F') value = ch - L'A' + 10;
        else if (ch >= L'a' && ch <= L'f') value = ch - L'a' + 10;
        if (value < 0) return 0;
        mac = (mac << 4) | static_cast<std::uint64_t>(value);
        ++digits;
    }
    return digits == 12 ? mac : 0;
}

// «Имя (AA:BB:CC:DD:EE:FF)» для журнала; если имени нет — только MAC.
std::wstring MacWithName(std::uint64_t address, std::wstring const& name) {
    return name.empty() ? MacText(address)
                        : Format(L"%s (%s)", name.c_str(), MacText(address).c_str());
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

std::wstring AdvStatusText(GattServiceProviderAdvertisementStatus status) {
    switch (status) {
        case GattServiceProviderAdvertisementStatus::Created: return L"создана";
        case GattServiceProviderAdvertisementStatus::Stopped: return L"остановлена";
        case GattServiceProviderAdvertisementStatus::Started: return L"идёт";
        case GattServiceProviderAdvertisementStatus::StartedWithoutAllAdvertisementData:
            return L"идёт без части данных";
        case GattServiceProviderAdvertisementStatus::Aborted: return L"прервана";
        default: return Format(L"код %d", (int)status);
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
    std::thread mac_thread;   // разовый запрос MAC своего адаптера

    // ---------------- отправитель: соединение ----------------
    std::mutex snd_mu;
    std::condition_variable snd_cv;

    BluetoothLEDevice device{nullptr};
    GattDeviceService service{nullptr};
    GattCharacteristic ctrl_ch{nullptr};
    GattCharacteristic data_ch{nullptr};
    GattCharacteristic data2_ch{nullptr};
    GattCharacteristic data3_ch{nullptr};
    GattCharacteristic data4_ch{nullptr};
    GattCharacteristic status_ch{nullptr};
    GattSession session{nullptr};
    event_token device_token{};
    event_token value_token{};
    bool value_subscribed = false;
    bool connected = false;
    std::uint64_t link_address = 0;   // MAC текущего соединения — для журнала
    bool write_failing = false;       // ошибка записи уже в журнале
    bool status_seen = false;         // первое уведомление Status уже отмечено

    // Определение имён: у большинства BLE-устройств имени нет в рекламном
    // пакете, оно лежит в GAP-характеристике 0x2A00 (её читают «Параметры
    // Windows»). Читаем в отдельном потоке, чтобы не тормозить скан.
    std::atomic<bool> name_stop{false};
    std::mutex name_mu;
    std::deque<std::pair<std::uint64_t, int>> name_queue;
    // Имена, которые Windows уже знает (кэш «Параметров»): их видно без
    // подключения к устройству.
    std::map<std::uint64_t, std::wstring> sys_names;
    unsigned long long adv_count = 0;   // сколько рекламных пакетов приняли
    std::set<std::uint64_t> name_tried;

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

    // Подтверждения отправляет отдельный поток: путь приёма блоков не должен
    // ждать notify (иначе каждый блок обрабатывается сотни миллисекунд).
    std::mutex ack_mu;
    std::condition_variable ack_cv;
    unsigned long long ack_pending = 0;
    bool ack_has = false;
    std::atomic<bool> ack_stop{true};
    std::thread ack_thread;
    std::atomic<unsigned long long> ack_sent{0};
    std::atomic<unsigned long long> ack_sent_count{0};   // сколько ACK реально ушло
    std::atomic<bool> rx_write_kind_logged{false};

    HANDLE file = INVALID_HANDLE_VALUE;
    std::wstring r_name;
    std::wstring r_path;
    unsigned long long r_size = 0;
    unsigned long long r_crc_expect = 0;
    unsigned long long r_received = 0;
    // Блоки, пришедшие раньше времени: каналы идут параллельно, порядок между
    // ними не гарантирован, поэтому такие блоки ждут, пока закроется «дырка».
    std::map<std::uint32_t, std::vector<std::uint8_t>> r_out;
    unsigned long long r_chunks = 0;
    unsigned long long r_ack_mark = 0;
    unsigned long long r_last_ack = 0;
    // Замеры приёмной стороны: пауза между блоками и время обработки.
    unsigned long long rx_prev_us = 0;
    unsigned long long rx_gap_us = 0;
    unsigned long long rx_gap_n = 0;
    unsigned long long rx_work_us = 0;
    unsigned long long rx_work_n = 0;
    unsigned long long rx_log_ms = 0;
    // Очередь принятых блоков: GATT-сервер отдаёт нам блок, мы копируем байты
    // и сразу освобождаем deferral, а запись в файл идёт в отдельном потоке.
    // Иначе стек ждёт нас и принимает один блок за интервал соединения.
    std::deque<std::vector<std::uint8_t>> rxq;
    std::mutex rxq_mu;
    std::condition_variable rxq_cv;
    std::thread rxq_thread;
    std::atomic<bool> rxq_stop{false};
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
    // Имя устройства: сначала системный кэш, затем GAP-характеристика 0x2A00.
    // Второй шаг требует подключения, поэтому выполняется только в фоновом
    // потоке и не для всех устройств сразу.
    // Имя из системного кэша (то, что показывает «Параметры Windows»).
    std::wstring SysNameFor(std::uint64_t address) {
        std::lock_guard<std::mutex> lock(mu);
        auto it = sys_names.find(address);
        return it == sys_names.end() ? std::wstring() : it->second;
    }

    // Разовая загрузка имён известных системе BLE-устройств.
    void CacheSystemNames() {
        try {
            auto infos = DeviceInformation::FindAllAsync(BluetoothLEDevice::GetDeviceSelector()).get();
            int found = 0;
            for (auto const& info : infos) {
                const std::uint64_t mac = MacFromDeviceId(std::wstring(info.Id().c_str()));
                const std::wstring name = info.Name().c_str();
                if (mac == 0 || name.empty()) continue;
                {
                    std::lock_guard<std::mutex> lock(mu);
                    sys_names[mac] = name;
                    for (auto& device : devices) {
                        if (device.address == mac && device.name != name) device.name = name;
                    }
                }
                ++found;
            }
            if (found > 0) {
                Log(0, Format(L"Системный кэш: имена известны для %d устройств", found));
                NotifyUi();
            }
        } catch (...) {
            // Кэш — необязательный источник имён: молча продолжаем.
        }
    }

    std::wstring QueryName(std::uint64_t address, int address_type) {
        BluetoothLEDevice dev = nullptr;
        std::wstring name;
        try {
            const BluetoothAddressType type =
                address_type == 1 ? BluetoothAddressType::Random : BluetoothAddressType::Public;
            dev = BluetoothLEDevice::FromBluetoothAddressAsync(address, type).get();
            if (!dev) return std::wstring();

            name = dev.Name().c_str();
            if (!name.empty()) {
                dev.Close();
                return name;
            }

            auto gap = dev.GetGattServicesForUuidAsync(GattServiceUuids::GenericAccess(),
                                                       BluetoothCacheMode::Uncached)
                           .get();
            if (gap.Status() == GattCommunicationStatus::Success && gap.Services().Size() > 0) {
                auto chars = gap.Services()
                                 .GetAt(0)
                                 .GetCharacteristicsForUuidAsync(GattCharacteristicUuids::GapDeviceName(),
                                                                 BluetoothCacheMode::Uncached)
                                 .get();
                if (chars.Status() == GattCommunicationStatus::Success &&
                    chars.Characteristics().Size() > 0) {
                    auto res = chars.Characteristics()
                                   .GetAt(0)
                                   .ReadValueAsync(BluetoothCacheMode::Uncached)
                                   .get();
                    if (res.Status() == GattCommunicationStatus::Success && res.Value()) {
                        DataReader reader = DataReader::FromBuffer(res.Value());
                        std::vector<std::uint8_t> bytes(reader.UnconsumedBufferLength());
                        if (!bytes.empty()) reader.ReadBytes(bytes);
                        std::string raw(bytes.size(), '\0');
                        for (std::size_t i = 0; i < bytes.size(); ++i) raw[i] = (char)bytes[i];
                        name = Wide(raw);   // имя в GAP-характеристике — UTF-8
                    }
                }
            }
            dev.Close();
        } catch (...) {
            try {
                if (dev) dev.Close();
            } catch (...) {
            }
            return std::wstring();
        }
        return name;
    }

    // Разовая попытка определить имя (по выбору устройства в списке).
    void ResolveName(std::uint64_t address, int address_type) {
        std::thread([this, address, address_type] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            const std::wstring name = QueryName(address, address_type);
            if (!name.empty()) SetDeviceName(address, name);
            winrt::uninit_apartment();
        }).detach();
    }

    // Поставить устройство в очередь на определение имени (без повторов).
    void EnqueueName(std::uint64_t address, int address_type) {
        {
            std::lock_guard<std::mutex> lock(name_mu);
            if (name_queue.size() >= 12) return;
            if (!name_tried.insert(address).second) return;
            name_queue.emplace_back(address, address_type);
        }
    }

    // Фоновый поток: по одному устройству, с паузой между подключениями.
    void NameWorker() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        // Сначала — имена, которые Windows уже знает: они появляются в списке
        // сразу, без подключения к устройствам.
        CacheSystemNames();
        while (!name_stop.load()) {
            std::pair<std::uint64_t, int> item{0, 0};
            bool have = false;
            {
                std::lock_guard<std::mutex> lock(name_mu);
                if (!name_queue.empty()) {
                    item = name_queue.front();
                    name_queue.pop_front();
                    have = true;
                }
            }
            if (!have) {
                Sleep(120);
                continue;
            }
            // Пока идёт соединение с нашим устройством или передача —
            // не подключаемся к посторонним: это мешает основному обмену.
            bool busy = false;
            {
                std::lock_guard<std::mutex> lock(mu);
                const LinkState state = static_cast<LinkState>(stats.link);
                busy = (state == LinkReady || state == LinkTransfer || state == LinkPaused ||
                        state == LinkConnecting);
            }
            if (busy || send_busy.load()) {
                {
                    std::lock_guard<std::mutex> lock(name_mu);
                    name_queue.push_front(item);   // вернём в начало очереди
                }
                Sleep(300);
                continue;
            }

            const std::wstring name = QueryName(item.first, item.second);
            if (!name.empty()) SetDeviceName(item.first, name);
            Sleep(400);
        }
        winrt::uninit_apartment();
    }

    void SetDeviceName(std::uint64_t address, std::wstring const& name) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(mu);
            for (auto& device : devices) {
                if (device.address == address && device.name != name) {
                    device.name = name;
                    changed = true;
                    break;
                }
            }
        }
        if (changed) {
            Log(0, Format(L"Имя устройства %s: %s", MacText(address).c_str(), name.c_str()));
            NotifyUi();
        }
    }

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

        try {
            const std::wstring mac = MacText(adapter.BluetoothAddress());
            Edit([&](Stats& s) { s.local_mac = mac; });
            Log(0, Format(L"Адаптер MAC: %s%s", mac.c_str(),
                          need_peripheral ? L" — на этот адрес отправляет Central" : L""));
        } catch (...) {
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

        // Поток определения имён живёт ровно столько же, сколько сканирование.
        name_stop.store(false);
        {
            std::lock_guard<std::mutex> lock(name_mu);
            name_queue.clear();
            name_tried.clear();
        }
        std::thread name_thread([this] { NameWorker(); });

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

            {
                unsigned long long packets = 0;
                std::size_t known = 0;
                {
                    std::lock_guard<std::mutex> lock(mu);
                    packets = adv_count;
                    known = devices.size();
                }
                Log(0, Format(L"Сканирование завершено: пакетов %d, устройств в списке %d",
                              (int)packets, (int)known));
            }

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

        name_stop.store(true);
        if (name_thread.joinable()) name_thread.join();

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

        if (name.empty()) name = SysNameFor(address);

        bool added = false;
        bool marked = false;
        {
            std::lock_guard<std::mutex> lock(mu);
            ++adv_count;
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

        if (name.empty() && rssi >= -70) EnqueueName(address, address_type);

        if (added) {
            Log(marked ? 3 : 0,
                Format(L"Найдено: MAC %s, имя «%s», адрес %s, RSSI %d дБм%s",
                       MacText(address).c_str(), name.empty() ? L"нет" : name.c_str(),
                       address_type == 1 ? L"random" : L"public", rssi,
                       marked ? L" — наш сервис" : L""));
            NotifyUi();
        } else if (marked) {
            Log(3, Format(L"Устройство MAC %s рекламирует наш сервис", MacText(address).c_str()));
            NotifyUi();
        }
    }

    // =======================================================================
    //  ПОДКЛЮЧЕНИЕ (Central)
    // =======================================================================
    void OnDeviceConnectionChanged(BluetoothConnectionStatus status, std::uint64_t address) {
        if (status != BluetoothConnectionStatus::Disconnected) return;
        bool was = false;
        {
            std::lock_guard<std::mutex> lock(snd_mu);
            was = connected;
            connected = false;
        }
        snd_cv.notify_all();
        if (!was) return;
        Log(1, L"Соединение разорвано: MAC " + MacText(address));
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
        // Первое уведомление отмечаем один раз: если ACK не идут, эта строка
        // в журнале покажет, что подписка работает, а проблема не в ней.
        bool first_status = false;
        std::uint64_t address = 0;
        {
            std::lock_guard<std::mutex> lock(snd_mu);
            if (!status_seen) {
                status_seen = true;
                first_status = true;
                address = link_address;
            }
        }
        if (first_status) {
            Log(0, L"Первое уведомление Status получено (MAC " + MacText(address) + L")");
        }

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

    // Тип адреса в рекламе иногда приходит как Unspecified, а некоторые
    // адаптеры отдают random/public наоборот. Поэтому пробуем оба варианта:
    // сначала заявленный, затем второй.
    bool TryConnect(std::uint64_t address, int address_type) {
        const int first = address_type == 1 ? 1 : 0;
        if (TryConnectWith(address, first)) return true;
        if (cancel.load()) return false;
        Log(1, Format(L"Повтор подключения к MAC %s с типом адреса %s", MacText(address).c_str(),
                      first == 1 ? L"public" : L"random"));
        return TryConnectWith(address, 1 - first);
    }

    bool TryConnectWith(std::uint64_t address, int address_type) {
        ResetLink();

        SetLink(LinkConnecting, L"Подключение…");
        Log(0, Format(L"Подключение к MAC %s (%s)…", MacText(address).c_str(),
                      address_type == 1 ? L"random" : L"public"));
        {
            std::lock_guard<std::mutex> lock(snd_mu);
            link_address = address;
            write_failing = false;
            status_seen = false;
        }

        try {
            // Тип адреса важен: смартфоны и трекеры рекламируются со случайным
            // (random) адресом, и подключение по одному числу не сработает.
            const BluetoothAddressType type =
                address_type == 1 ? BluetoothAddressType::Random : BluetoothAddressType::Public;
            // Сразу после сканирования устройство бывает ещё не готово к
            // подключению — несколько попыток с паузой заметно надёжнее.
            BluetoothLEDevice dev = nullptr;
            for (int attempt = 0; attempt < 3 && !dev; ++attempt) {
                dev = BluetoothLEDevice::FromBluetoothAddressAsync(address, type).get();
                if (!dev && attempt < 2) SleepCancelable(400);
            }
            if (!dev) {
                Log(2, L"Устройство MAC " + MacText(address) +
                           L" не найдено (возможно, выключено или вне зоны)");
                return false;
            }

            // Просим у стека «скоростной» профиль соединения: интервал
            // 11.25–15 мс вместо обычных 30–150 мс. Это только просьба, но
            // именно интервал определяет скорость при MTU 517.
            try {
                dev.RequestPreferredConnectionParameters(
                    BluetoothLEPreferredConnectionParameters::ThroughputOptimized());
                Log(0, L"Запрошен скоростной профиль соединения (интервал ~15 мс) · MAC " +
                           MacText(address));
            } catch (...) {
                Log(1, L"Стек не поддерживает запрос параметров соединения — интервал "
                        L"назначает Central сам · MAC " + MacText(address));
            }

            GattSession sess = nullptr;
            try {
                sess = GattSession::FromDeviceIdAsync(dev.BluetoothDeviceId()).get();
                if (sess) sess.MaintainConnection(true);
            } catch (...) {
                Log(1, L"Не удалось удержать соединение (GattSession), MAC " +
                           MacText(address));
            }

            // Кэш GATT на Windows бывает «пустым» сразу после подключения:
            // пробуем Uncached, затем Cached, затем перебор всех сервисов.
            GattDeviceService svc = nullptr;
            for (int attempt = 0; attempt < 3 && !svc; ++attempt) {
                const BluetoothCacheMode mode =
                    attempt == 0 ? BluetoothCacheMode::Uncached : BluetoothCacheMode::Cached;
                GattCommunicationStatus status = GattCommunicationStatus::Unreachable;
                try {
                    auto result = dev.GetGattServicesForUuidAsync(ServiceUuid(), mode).get();
                    status = result.Status();
                    if (status == GattCommunicationStatus::Success &&
                        result.Services().Size() > 0) {
                        svc = result.Services().GetAt(0);
                    }
                    if (!svc) {
                        auto all = dev.GetGattServicesAsync(mode).get();
                        status = all.Status();
                        if (status == GattCommunicationStatus::Success) {
                            for (auto const& candidate : all.Services()) {
                                if (candidate.Uuid() == ServiceUuid()) {
                                    svc = candidate;
                                    break;
                                }
                            }
                        }
                    }
                } catch (hresult_error const& e) {
                    Log(1, Format(L"Поиск сервиса на MAC %s: %s (попытка %d)",
                                  MacText(address).c_str(), e.message().c_str(), attempt + 1));
                }
                if (!svc) {
                    Log(1, Format(L"Сервис пока не найден (MAC %s, попытка %d): %s",
                                  MacText(address).c_str(), attempt + 1,
                                  CommStatusText(status).c_str()));
                    if (attempt < 2) SleepCancelable(400);
                }
            }
            if (!svc) {
                Log(2, L"Сервис передачи не найден на MAC " + MacText(address) +
                           L". Устройство должно быть в роли «Приёмник».");
                ResetLink();
                return false;
            }

            auto chars = svc.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
            if (chars.Status() != GattCommunicationStatus::Success) {
                chars = svc.GetCharacteristicsAsync(BluetoothCacheMode::Cached).get();
            }
            if (chars.Status() != GattCommunicationStatus::Success) {
                Log(2, Format(L"Не удалось получить характеристики на MAC %s: %s",
                              MacText(address).c_str(), CommStatusText(chars.Status()).c_str()));
                ResetLink();
                return false;
            }

            GattCharacteristic ctrl = nullptr, data = nullptr, stat = nullptr;
            GattCharacteristic data2 = nullptr, data3 = nullptr, data4 = nullptr;
            for (auto const& ch : chars.Characteristics()) {
                if (ch.Uuid() == ControlUuid()) {
                    ctrl = ch;
                } else if (ch.Uuid() == DataUuid()) {
                    data = ch;
                } else if (ch.Uuid() == Data2Uuid()) {
                    data2 = ch;
                } else if (ch.Uuid() == Data3Uuid()) {
                    data3 = ch;
                } else if (ch.Uuid() == Data4Uuid()) {
                    data4 = ch;
                } else if (ch.Uuid() == StatusUuid()) {
                    stat = ch;
                }
            }
            if (!ctrl || !data || !stat) {
                Log(2, L"На MAC " + MacText(address) +
                           L" в сервисе нет нужных характеристик Control/Data/Status");
                ResetLink();
                return false;
            }

            auto sub = stat.WriteClientCharacteristicConfigurationDescriptorAsync(
                           GattClientCharacteristicConfigurationDescriptorValue::Notify)
                           .get();
            if (sub != GattCommunicationStatus::Success) {
                Log(2, Format(L"Не удалось подписаться на уведомления (MAC %s): %s",
                              MacText(address).c_str(), CommStatusText(sub).c_str()));
                ResetLink();
                return false;
            }

            Log(0, Format(L"Уведомления Status включены (MAC %s)", MacText(address).c_str()));

            const event_token token = stat.ValueChanged(
                [this](GattCharacteristic const&, GattValueChangedEventArgs const& args) {
                    OnValueChanged(args);
                });

            std::wstring name = dev.Name().c_str();
            // Размер MTU мог измениться после обмена ATT MTU (он идёт сразу
            // после подключения): перечитываем несколько раз и берём максимум.
            int pdu = 23;
            for (int attempt = 0; attempt < 4; ++attempt) {
                int fresh = 0;
                try {
                    if (sess) fresh = (int)sess.MaxPduSize();
                } catch (...) {
                    fresh = 0;
                }
                if (fresh > pdu) pdu = fresh;
                if (pdu >= kMaxPdu) break;
                if (attempt < 3) SleepCancelable(120);
            }
            pdu = std::clamp(pdu, 23, kMaxPdu);
            const int chunk = std::max(8, pdu - 3 - 4);
            if (pdu <= 23) {
                Log(1, Format(L"MTU не увеличен (%d): блок %d байт — приёмник не согласовал "
                              L"больший MTU, скорость будет низкой",
                              pdu, chunk));
            }
            Log(0, Format(L"MTU %d · блок %d Б · окно %d Б · Central", pdu, chunk,
                          (int)kWindowBytesTarget));

            {
                std::lock_guard<std::mutex> lock(snd_mu);
                device = dev;
                service = svc;
                ctrl_ch = ctrl;
                data_ch = data;
                data2_ch = data2;
                data3_ch = data3;
                data4_ch = data4;
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
                if (!name.empty()) SetDeviceName(address, name);
                mtu = pdu;
                payload = chunk;
            }

            device_token = dev.ConnectionStatusChanged(
                [this, address](BluetoothLEDevice const& d,
                                winrt::Windows::Foundation::IInspectable const&) {
                    OnDeviceConnectionChanged(d.ConnectionStatus(), address);
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
            Log(2, Format(L"Ошибка подключения к MAC %s: %s", MacText(address).c_str(),
                          e.message().c_str()));
            ResetLink();
            return false;
        } catch (...) {
            Log(2, L"Ошибка подключения к MAC " + MacText(address));
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
            data2_ch = nullptr;
            data3_ch = nullptr;
            data4_ch = nullptr;
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
    // Один и тот же сбой записи пишем в журнал один раз (иначе поток блоков
    // засыпает журнал сотнями строк и MAC в них не найти).
    void LogWriteError(std::wstring const& what, GattCommunicationStatus status) {
        std::uint64_t address = 0;
        bool first = false;
        {
            std::lock_guard<std::mutex> lock(snd_mu);
            first = !write_failing;
            write_failing = true;
            address = link_address;
        }
        if (!first) return;
        Log(2, Format(L"Запись не выполнена: %s · MAC %s · %s", what.c_str(),
                      MacText(address).c_str(), CommStatusText(status).c_str()));
    }

    void WriteSucceeded() {
        std::lock_guard<std::mutex> lock(snd_mu);
        write_failing = false;
    }

    std::uint64_t LinkAddress() {
        std::lock_guard<std::mutex> lock(snd_mu);
        return link_address;
    }

    // MAC своего адаптера узнаём сразу после выбора роли: в роли приёмника
    // это адрес, на который должен целиться отправитель, и ждать нажатия
    // «Старт» для его показа незачем.
    void QueryLocalMac() {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!stats.local_mac.empty()) return;
        }
        JoinThread(mac_thread);
        mac_thread = std::thread([this] {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                BluetoothAdapter adapter = BluetoothAdapter::GetDefaultAsync().get();
                if (adapter) {
                    const std::wstring mac = MacText(adapter.BluetoothAddress());
                    if (!mac.empty()) {
                        Edit([&](Stats& s) { s.local_mac = mac; });
                        Log(0, L"Мой MAC: " + mac);
                    }
                }
                winrt::uninit_apartment();
            } catch (...) {
            }
        });
    }

    std::wstring LocalMac() {
        std::lock_guard<std::mutex> lock(mu);
        return stats.local_mac;
    }

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
            const auto result = ch.WriteValueWithResultAsync(ToBuffer(text),
                                                             GattWriteOption::WriteWithResponse)
                                    .get();
            if (result.Status() != GattCommunicationStatus::Success) {
                LogWriteError(Format(L"команда «%s»", line.c_str()), result.Status());
                return false;
            }
            WriteSucceeded();
            return true;
        } catch (hresult_error const& e) {
            LogWriteError(Format(L"команда «%s» · исключение: %s", line.c_str(),
                                 e.message().c_str()),
                          GattCommunicationStatus::Unreachable);
            return false;
        } catch (...) {
            return false;
        }
    }

    // Одна незавершённая запись блока: сама операция и её смещение (для журнала).
    struct PendingBlock {
        winrt::Windows::Foundation::IAsyncOperation<GattWriteResult> op{};
        unsigned long long offset = 0;
    };

    // Ставит блок в очередь записи, НЕ дожидаясь её завершения. Так в стеке
    // может быть несколько записей сразу, и за интервал соединения уходит
    // несколько блоков, а не один.
    bool StartWriteBlock(GattCharacteristic const& channel, unsigned long long offset,
                         const std::uint8_t* data, std::size_t size,
                         std::deque<PendingBlock>& pending) {
        {
            std::lock_guard<std::mutex> lock(snd_mu);
            if (!connected) return false;
        }
        if (!channel) return false;
        const GattCharacteristic& ch = channel;

        std::vector<std::uint8_t> frame;
        EncodeFrame(offset, data, size, frame);

        // Стек может не принять вторую запись, пока жива первая (у некоторых
        // версий Windows на характеристику разрешена одна операция). Тогда
        // дожидаемся старой и пробуем ещё раз, а не рвём сеанс.
        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                PendingBlock block;
                block.op = ch.WriteValueWithResultAsync(ToBuffer(frame.data(), frame.size()),
                                                        GattWriteOption::WriteWithoutResponse);
                block.offset = offset;
                pending.push_back(std::move(block));
                return true;
            } catch (hresult_error const& e) {
                const bool drained = !pending.empty() && FinishOldestWrite(pending);
                if (attempt > 0 || !drained) {
                    LogWriteError(Format(L"блок со смещением %llu · исключение: %s", offset,
                                         e.message().c_str()),
                                  GattCommunicationStatus::Unreachable);
                    return false;
                }
            } catch (...) {
                return false;
            }
        }
        return false;
    }

    // Дожидается самой старой записи и проверяет её результат: блоки не
    // теряются молча, а ошибка сразу видна в журнале.
    bool FinishOldestWrite(std::deque<PendingBlock>& pending) {
        if (pending.empty()) return true;
        PendingBlock block = std::move(pending.front());
        pending.pop_front();
        try {
            const auto result = block.op.get();
            if (result.Status() != GattCommunicationStatus::Success) {
                LogWriteError(Format(L"блок со смещением %llu", block.offset), result.Status());
                return false;
            }
            WriteSucceeded();
            return true;
        } catch (hresult_error const& e) {
            LogWriteError(Format(L"блок со смещением %llu · исключение: %s", block.offset,
                                 e.message().c_str()),
                          GattCommunicationStatus::Unreachable);
            return false;
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
        if (offset > 0) {
            Log(3, L"Приёмник (MAC " + MacText(LinkAddress()) + L") принял ранее " +
                       SizeText(offset) + L" — продолжаем");
        }

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
        unsigned long long last_progress_log = GetTickCount64();
        int window_blocks = 8;
        int stall_count = 0;
        std::deque<PendingBlock> pending;   // записи «в полёте»
        // Каналы данных: Windows пропускает примерно один PDU за интервал
        // соединения на характеристику, поэтому пишем в несколько сразу.
        std::vector<GattCharacteristic> channels;
        {
            std::lock_guard<std::mutex> lock(snd_mu);
            if (data_ch) channels.push_back(data_ch);
            if (data2_ch) channels.push_back(data2_ch);
            if (data3_ch) channels.push_back(data3_ch);
            if (data4_ch) channels.push_back(data4_ch);
        }
        if (channels.empty()) throw LinkLost("нет характеристики Data");
        std::size_t ch_index = 0;
        // Замеры: сколько занимает одна запись в стеке и сколько мы ждём ACK.
        unsigned long long write_us = 0, write_n = 0;
        unsigned long long ack_wait_us = 0, ack_wait_n = 0;
        const unsigned long long loop_start = GetTickCount64();
        bool slow_hint_done = false;
        {
            int chunk_now = 0;
            {
                std::lock_guard<std::mutex> lock(snd_mu);
                chunk_now = payload;
            }
            const int safe_chunk = std::max(8, chunk_now);
            // Держим «в полёте» примерно kWindowBytesTarget байт: при малом
            // MTU это много мелких блоков, при большом — несколько крупных.
            window_blocks = (int)std::clamp<unsigned long long>(
                kWindowBytesTarget / (unsigned long long)safe_chunk,
                (unsigned long long)kWindowBlocksMin, (unsigned long long)kWindowBlocksMax);
            // Повторная просьба уже на живом соединении: при подключении стек
            // мог её проигнорировать, а интервал — единственное, что осталось
            // между нами и десятками КБ/с.
            {
                BluetoothLEDevice dev = nullptr;
                {
                    std::lock_guard<std::mutex> lock(snd_mu);
                    dev = device;
                }
                if (dev) {
                    try {
                        dev.RequestPreferredConnectionParameters(
                            BluetoothLEPreferredConnectionParameters::ThroughputOptimized());
                        Log(0, L"Повторно запрошен скоростной профиль соединения");
                    } catch (...) {
                    }
                }
            }

            Log(0, Format(L"Параметры потока: блок %d байт, окно %d блоков (%d байт), "
                          L"каналов данных %d",
                          chunk_now, window_blocks, chunk_now * window_blocks,
                          (int)channels.size()));
        }

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

                // Окно считаем в блоках, а не в байтах: при MTU 23 держать
                // «в полёте» сотни записей нельзя — стек их отбросит.
                const unsigned long long window =
                    (unsigned long long)std::max(chunk, 8) * (unsigned long long)window_blocks;

                // ВАЖНО: курсор почти всегда впереди подтверждённой позиции —
                // это и есть окно «в полёте», а не потеря блоков. Раньше здесь
                // стоял безусловный откат `cursor = ack_now`, и передача
                // навсегда замирала на первом блоке (~510 Б), бесконечно
                // переписывая его заново.
                if (ack_now > cursor) cursor = ack_now;

                if (ack_now > last_ack) {
                    // Подтверждения идут ровно — можно осторожно расширяться.
                    if (ack_now - last_ack >= window && window_blocks < kWindowBlocksMax &&
                        (unsigned long long)(window_blocks + 1) * (unsigned long long)chunk <=
                            kWindowBytesMax) {
                        ++window_blocks;
                    }
                    last_ack = ack_now;
                    last_ack_tick = GetTickCount64();
                    stall_count = 0;
                } else if (GetTickCount64() - last_ack_tick > kStallMs) {
                    last_ack_tick = GetTickCount64();
                    if (cursor > ack_now) {
                        // В полёте были неподтверждённые блоки, а ACK нет —
                        // значит они потерялись. Возвращаемся к подтверждённой
                        // позиции и ужимаем окно.
                        cursor = ack_now;
                        window_blocks = std::max(kWindowBlocksMin, window_blocks / 2);
                        Log(2, Format(L"Нет подтверждений — повтор с %s, окно %d блоков",
                                      SizeText(ack_now).c_str(), window_blocks));
                    } else if (++stall_count > 3) {
                        throw Stall("нет подтверждений");
                    }
                }

                if (cursor >= ack_now && cursor - ack_now >= window) {
                    const unsigned long long wait_t0 = NowUs();
                    {
                        std::unique_lock<std::mutex> lock(snd_mu);
                        snd_cv.wait_for(lock, 120ms);
                    }
                    ack_wait_us += NowUs() - wait_t0;
                    ++ack_wait_n;
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

                // Держим в полёте до window_blocks записей и ждём только
                // когда очередь полна: иначе стек отправляет один блок за
                // интервал соединения (это и давало единицы КБ/с).
                const std::size_t in_flight = (std::size_t)std::max(2, std::min(window_blocks, 16));
                while (pending.size() >= in_flight) {
                    const unsigned long long w_t0 = NowUs();
                    const bool written = FinishOldestWrite(pending);
                    write_us += NowUs() - w_t0;
                    ++write_n;
                    if (!written) throw LinkLost("блок не записан");
                }
                const GattCharacteristic& channel = channels[ch_index++ % channels.size()];
                if (!StartWriteBlock(channel, cursor, buffer.data(), got, pending)) {
                    throw LinkLost("блок не записан");
                }
                cursor += got;
                ++chunks;

                const unsigned long long now = GetTickCount64();
                if (now - last_ui >= 100) {
                    last_ui = now;
                    const double speed = SampleSpeed(ack_now, false);
                    PublishProgress(ack_now, size, speed, chunks);
                    // Раз в 2 с — строка прогресса в журнал: по ней видно,
                    // идёт ли передача и с какой реальной скоростью.
                    if (now - last_progress_log >= 2000) {
                        last_progress_log = now;
                        const unsigned long long w_ms = write_n ? write_us / write_n / 1000 : 0;
                        const unsigned long long a_ms = ack_wait_n ? ack_wait_us / ack_wait_n / 1000 : 0;
                        Log(0, Format(L"%s/с · запись %llu мс · блок %d Б · окно %d · "
                                      L"%s из %s",
                                      SizeText((unsigned long long)(speed > 0 ? speed : 0)).c_str(),
                                      w_ms, chunk, window_blocks,
                                      SizeText(ack_now).c_str(), SizeText(size).c_str()));
                        write_us = write_n = ack_wait_us = ack_wait_n = 0;
                        // Если запись стоит десятки миллисекунд — упёрлись не в
                        // программу, а в интервал соединения. Скажем об этом
                        // прямо, чтобы не искать виноватого вслепую.
                        if (!slow_hint_done && now - loop_start >= 4000 && w_ms > 60) {
                            slow_hint_done = true;
                            const unsigned long long per_s = 1000 / std::max<unsigned long long>(w_ms, 1);
                            Log(2, Format(L"Скорость ограничена интервалом соединения: одна запись "
                                          L"~%llu мс, блок %d Б → ~%llu Б/с. Интервал задаёт стек "
                                          L"(запрошен скоростной профиль); от программы зависит "
                                          L"только размер блока и окно.",
                                          w_ms, chunk, (unsigned long long)chunk * per_s));
                        }
                    }
                }
            }
        } catch (...) {
            CloseHandle(file);
            throw;
        }
        CloseHandle(file);

        // Дожидаемся записей, ещё находящихся в стеке: иначе последние блоки
        // могли не уйти, а мы бы уже просили SYNC.
        while (!pending.empty()) {
            if (!FinishOldestWrite(pending)) throw LinkLost("последние блоки не записаны");
        }

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
            Log(3, Format(L"Готово: %s, CRC32 %08X совпал%s · MAC %s", SizeText(size).c_str(),
                          remote_crc, extra.c_str(), MacText(LinkAddress()).c_str()));
        } else {
            Log(2, Format(L"CRC32 не совпал (MAC %s): отправитель %08X, приёмник %08X",
                          MacText(LinkAddress()).c_str(), crc, remote_crc));
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
        Log(0, Format(L"Файл «%s»: %s, CRC32 %08X → MAC %s", name.c_str(),
                      SizeText(size).c_str(), crc, MacText(address).c_str()));

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
                // Подтверждения перестали приходить: не сдаёмся, а
                // переподключаемся и продолжаем с подтверждённой позиции —
                // иначе большой файл обрывался бы на первом же сбое.
                Log(2, Format(L"Приём остановился (MAC %s): %hs — повтор с подтверждённой позиции",
                              MacText(address).c_str(), e.what()));
                {
                    std::lock_guard<std::mutex> lock(snd_mu);
                    connected = false;
                }
                Edit([](Stats& s) {
                    s.connected = false;
                    s.transfer = TrPaused;
                    s.link = LinkPaused;
                    s.status = L"Приём не отвечает — восстановление…";
                });
                if (attempt < kMaxAttempts) {
                    ResetLink();
                    SleepCancelable(1200);
                }
            } catch (LinkLost const& e) {
                Log(1, Format(L"Связь прервана (MAC %s): %hs", MacText(address).c_str(),
                              e.what()));
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
                    Log(0, Format(L"Попытка восстановить соединение с MAC %s (%d из %d)…",
                                  MacText(address).c_str(), attempt + 1, kMaxAttempts));
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
            if (link_failed) {
                Log(2, L"Не удалось установить соединение с приёмником MAC " + MacText(address));
            }
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
        std::vector<std::uint8_t> bytes;
        bool with_response = false;
        try {
            auto request = args.GetRequestAsync().get();
            bytes = FromBuffer(request.Value());
            with_response = request.Option() == GattWriteOption::WriteWithResponse;
            if (with_response) request.Respond();
        } catch (...) {
        }
        // Освобождаем deferral ДО записи в файл: стек сразу может принять
        // следующий блок, а не ждать, пока мы сходим на диск.
        deferral.Complete();
        if (!bytes.empty()) EnqueueData(std::move(bytes));
        // Один раз в журнал: Write Command (без подтверждения ATT) быстрый,
        // Write Request ждёт ответа на каждом блоке — это в разы медленнее.
        if (!rx_write_kind_logged.exchange(true)) {
            Log(0, with_response
                       ? L"Блоки приходят как Write Request (с ответом ATT) — медленный режим"
                       : L"Блоки приходят как Write Command (без ответа ATT) — как требуется");
        }
    }

    // Поток разбора принятых блоков: только файл и подтверждения.
    void RxQueueMain() {
        for (;;) {
            std::vector<std::uint8_t> bytes;
            {
                std::unique_lock<std::mutex> lock(rxq_mu);
                rxq_cv.wait_for(lock, std::chrono::milliseconds(200),
                                [this] { return rxq_stop.load() || !rxq.empty(); });
                if (rxq.empty()) {
                    if (rxq_stop.load()) return;
                    continue;
                }
                bytes = std::move(rxq.front());
                rxq.pop_front();
            }
            try {
                HandleData(bytes);
            } catch (...) {
            }
        }
    }

    void EnqueueData(std::vector<std::uint8_t> bytes) {
        {
            std::lock_guard<std::mutex> lock(rxq_mu);
            // Если отправитель обогнал приём, старые блоки всё равно не нужны:
            // он повторит их с подтверждённой позиции.
            if (rxq.size() > 512) rxq.pop_front();
            rxq.push_back(std::move(bytes));
        }
        rxq_cv.notify_one();
    }

    // Строка вида «Bluetooth#Bluetooth…-AA:BB:CC:DD:EE:FF» — в ней есть MAC
    // и адаптера, и Central: по ней видно, кто именно подключился.
    std::wstring CentralText() {
        try {
            GattLocalCharacteristic status = nullptr;
            {
                std::lock_guard<std::mutex> lock(recv_mu);
                status = local_status;
            }
            if (status) {
                auto clients = status.SubscribedClients();
                if (clients.Size() > 0) {
                    const std::wstring id = clients.GetAt(0).Session().DeviceId().Id().c_str();
                    if (!id.empty()) return id;
                }
            }
        } catch (...) {
        }
        return L"идентификатор Central недоступен";
    }

    // Согласованный с Central размер MTU (0 — неизвестен).
    int CentralMtu() {
        try {
            GattLocalCharacteristic status = nullptr;
            {
                std::lock_guard<std::mutex> lock(recv_mu);
                status = local_status;
            }
            if (status) {
                auto clients = status.SubscribedClients();
                if (clients.Size() > 0) return (int)clients.GetAt(0).Session().MaxPduSize();
            }
        } catch (...) {
        }
        return 0;
    }

    // Короткий MAC Central (из строки идентификатора) — для строки в окне.
    std::wstring CentralMac() {
        const std::uint64_t mac = MacFromDeviceId(CentralText());
        return mac == 0 ? std::wstring() : MacText(mac);
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
            {
                const int mtu_now = CentralMtu();
                Log(mtu_now > 0 ? 0 : 3,
                    (mtu_now > 0 ? Format(L"MTU %d · блок %d Б · ", mtu_now,
                                          std::max(8, mtu_now - 7))
                                 : std::wstring()) +
                        L"Central подключился · " + CentralText());
            }
            const std::wstring central_mac = CentralMac();
            Edit([&](Stats& s) {
                s.peer_mac = central_mac;
                s.connected = true;
                s.link = s.transfer == TrReceiving ? LinkTransfer : LinkReady;
                s.status = L"Central подключён";
            });
        } else {
            Log(1, L"Central отключился — сеанс сохранён для возобновления · " + CentralText());
            bool session_active = false;
            {
                std::lock_guard<std::mutex> lock(recv_mu);
                session_active = r_has;
            }
            Edit([&](Stats& s) {
                s.connected = false;
                s.peer_mac.clear();
                if (s.transfer == TrReceiving) {
                    s.transfer = TrPaused;
                    s.status = L"Связь потеряна — ждём повторного подключения";
                    s.link = LinkPaused;
                } else if (!session_active) {
                    // Данные уже идут (сеанс открыт) — «Ожидание подключения…»
                    // здесь было бы неправдой и путало.
                    s.status = L"Ожидание подключения…";
                    s.link = LinkAdvertising;
                }
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
        // Список подписчиков бывает пуст в первые моменты после подключения,
        // хотя notify уже доставляется: не отбрасываем отправку из-за него.
        // Само ожидание (.get()) тут не нужно: каждый notify занимает PDU и
        // десятки миллисекунд, а подтверждения идемпотентны — следующее ACK
        // всё равно несёт более свежее смещение.
        try {
            status.NotifyValueAsync(ToBuffer(Utf8(line)));
        } catch (...) {
        }
    }

    // Просим фоновой поток отправить подтверждение (не блокируя приём блока).
    void SignalAck(unsigned long long value) {
        {
            std::lock_guard<std::mutex> lock(ack_mu);
            if (value > ack_pending) ack_pending = value;
            ack_has = true;
        }
        ack_cv.notify_one();
    }

    // Поток подтверждений: отправляет ACK по сигналу и дополнительно раз в
    // 100 мс, если принято больше, чем подтверждено (отправитель мог замолчать,
    // упёршись в окно).
    void AckWorker() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        unsigned long long last_send = 0;
        while (!ack_stop.load()) {
            unsigned long long value = 0;
            {
                std::unique_lock<std::mutex> lock(ack_mu);
                ack_cv.wait_for(lock, std::chrono::milliseconds(100),
                                [this] { return ack_has || ack_stop.load(); });
                if (ack_has) {
                    value = ack_pending;
                    ack_has = false;
                }
            }
            if (ack_stop.load()) break;
            const unsigned long long now = GetTickCount64();
            if (value == 0) {
                // Страховка раз в секунду. Раньше здесь стояло «если принято
                // больше, чем подтверждено» без задержки — ACK уходил почти на
                // каждый блок и отнимал у данных слот в интервале связи.
                if (now - last_send < 1000) continue;
                std::lock_guard<std::mutex> lock(recv_mu);
                if (r_has && r_received > ack_sent.load()) value = r_received;
            }
            if (value > ack_sent.load()) {
                ack_sent.store(value);
                last_send = now;
                ack_sent_count.fetch_add(1);
                NotifyStatus(Format(L"ACK|%llu", value));
            }
        }
        winrt::uninit_apartment();
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
                    r_out.clear();
                    r_ack_mark = 0;
                    ack_sent.store(0);
                    {
                        std::lock_guard<std::mutex> ack_lock(ack_mu);
                        ack_pending = 0;
                        ack_has = false;
                    }
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
            Log(3, Format(L"Приём «%s» от Central %s: продолжаем с %s из %s", name.c_str(),
                          CentralText().c_str(), SizeText(offset).c_str(),
                          SizeText(size).c_str()));
        } else {
            Log(0, Format(L"Приём «%s» от Central %s: %s, ожидаемый CRC32 %08X", name.c_str(),
                          CentralText().c_str(), SizeText(size).c_str(), crc));
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
            r_out.clear();
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
            Log(3, Format(L"Принято %s, CRC32 %08X совпал, файл: %s · Central %s",
                          SizeText(file_size).c_str(), actual, path.c_str(),
                          CentralText().c_str()));
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
        Log(1, L"Передача отменена отправителем (частичный файл сохранён) · Central " +
                   CentralText());
        Edit([](Stats& s) {
            s.transfer = TrPaused;
            s.status = L"Передача отменена отправителем";
            s.speed = 0.0;
            s.eta = 0.0;
        });
    }

    void HandleData(std::vector<std::uint8_t> const& bytes) {
        const unsigned long long rx_t0 = NowUs();
        std::uint32_t offset = 0;
        const std::uint8_t* payload = nullptr;
        std::size_t count = 0;
        if (!DecodeFrame(bytes.data(), bytes.size(), offset, payload, count)) return;
        if (count == 0) return;

        bool need_ack = false;
        bool progress = false;
        unsigned long long acknowledged = 0;

        {
            std::lock_guard<std::mutex> lock(recv_mu);
            if (!r_has || file == INVALID_HANDLE_VALUE || r_done) return;

            auto write_chunk = [&](const std::uint8_t* p, std::size_t n) -> bool {
                DWORD written = 0;
                const BOOL res = WriteFile(file, p, (DWORD)n, &written, nullptr);
                return res && written == n;
            };

            const std::uint32_t off = offset;
            bool accepted = false;
            bool failed = false;
            if (off == (std::uint32_t)r_received) {
                accepted = true;
                if (!write_chunk(payload, count)) {
                    failed = true;
                } else {
                    r_received += count;
                    ++r_chunks;
                    // Добираем блоки, пришедшие раньше со смежных каналов:
                    // пишем строго по порядку смещений.
                    for (;;) {
                        auto it = r_out.find((std::uint32_t)r_received);
                        if (it == r_out.end()) break;
                        std::vector<std::uint8_t> buffered = std::move(it->second);
                        r_out.erase(it);
                        if (!write_chunk(buffered.data(), buffered.size())) {
                            failed = true;
                            break;
                        }
                        r_received += buffered.size();
                        ++r_chunks;
                    }
                }
            } else if (off > (std::uint32_t)r_received) {
                // Блок пришёл вперёд (параллельный канал) — держим до «дырки».
                if (r_out.size() < 1024 && r_out.find(off) == r_out.end()) {
                    r_out.emplace(off, std::vector<std::uint8_t>(payload, payload + count));
                }
            } else {
                // Дубликат: подтверждаем уже принятую позицию.
                need_ack = true;
                acknowledged = r_received;
            }

            if (failed) {
                need_ack = true;
                acknowledged = r_received;
            } else if (accepted) {
                const unsigned long long now = GetTickCount64();
                if (r_received - r_ack_mark >= kAckBytes ||
                    now - r_last_ack >= kAckIntervalMs) {
                    r_ack_mark = r_received;
                    r_last_ack = now;
                    need_ack = true;
                    progress = now - r_last_ui >= 100;
                    if (progress) r_last_ui = now;
                }
                acknowledged = r_received;
            }
        }

        if (need_ack) SignalAck(acknowledged);

        // Замеры: если пауза между блоками ~интервал соединения, значит
        // тормозит звено, а не обработка; если пауза мала, а блоков мало —
        // виноват наш приём.
        unsigned long long dbg_gap_ms = 0, dbg_work_us = 0, dbg_bps = 0, dbg_acks = 0;
        bool dbg_log = false;
        {
            const unsigned long long rx_t1 = NowUs();
            std::lock_guard<std::mutex> lock(recv_mu);
            if (rx_prev_us && rx_t1 > rx_prev_us) {
                rx_gap_us += rx_t1 - rx_prev_us;
                ++rx_gap_n;
            }
            rx_prev_us = rx_t1;
            rx_work_us += rx_t1 - rx_t0;
            ++rx_work_n;
            const unsigned long long now_ms = GetTickCount64();
            if (rx_log_ms == 0) rx_log_ms = now_ms;
            if (now_ms - rx_log_ms >= 2000) {
                dbg_gap_ms = rx_gap_n ? rx_gap_us / rx_gap_n / 1000 : 0;
                dbg_work_us = rx_work_n ? rx_work_us / rx_work_n : 0;
                dbg_bps = rx_work_n * 1000 / (now_ms - rx_log_ms);
                dbg_acks = ack_sent_count.exchange(0) * 1000 / (now_ms - rx_log_ms);
                rx_gap_us = rx_gap_n = rx_work_us = rx_work_n = 0;
                rx_log_ms = now_ms;
                dbg_log = true;
            }
        }
        if (dbg_log) {
            Log(0, Format(L"%llu бл/с · пауза %llu мс · обраб. %llu мкс · ACK %llu/с",
                          dbg_bps, dbg_gap_ms, dbg_work_us, dbg_acks));
        }

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
                s.connected = true;   // блоки идут — значит Central на связи
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
        GattLocalCharacteristic data_local2 = nullptr;
        GattLocalCharacteristic data_local3 = nullptr;
        GattLocalCharacteristic data_local4 = nullptr;
        GattLocalCharacteristic status_local = nullptr;
        event_token t_ctrl{}, t_data{}, t_data2{}, t_data3{}, t_data4{}, t_read{}, t_subs{};

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
            // Дополнительные каналы данных: Windows отправляет примерно один
            // PDU за интервал соединения на характеристику, поэтому несколько
            // характеристик = несколько PDU за интервал. Формат кадра тот же.
            {
                const guid extra[3] = {Data2Uuid(), Data3Uuid(), Data4Uuid()};
                GattLocalCharacteristic* slots[3] = {&data_local2, &data_local3, &data_local4};
                for (int i = 0; i < 3; ++i) {
                    GattLocalCharacteristicParameters params;
                    params.CharacteristicProperties(Props(PropWriteWithoutResponse | PropWrite));
                    params.WriteProtectionLevel(GattProtectionLevel::Plain);
                    params.UserDescription(L"Блоки файла (параллельный канал)");
                    auto created = service.CreateCharacteristicAsync(extra[i], params).get();
                    if (created.Error() == BluetoothError::Success) {
                        *slots[i] = created.Characteristic();
                    }
                }
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
            if (data_local2) {
                t_data2 = data_local2.WriteRequested(
                    [this](GattLocalCharacteristic const&, GattWriteRequestedEventArgs const& args) {
                        OnDataWrite(args);
                    });
            }
            if (data_local3) {
                t_data3 = data_local3.WriteRequested(
                    [this](GattLocalCharacteristic const&, GattWriteRequestedEventArgs const& args) {
                        OnDataWrite(args);
                    });
            }
            if (data_local4) {
                t_data4 = data_local4.WriteRequested(
                    [this](GattLocalCharacteristic const&, GattWriteRequestedEventArgs const& args) {
                        OnDataWrite(args);
                    });
            }
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

            // Реклама включается не мгновенно: коротко ждём статус «идёт» и
            // пишем его в журнал — если рекламы нет, отсюда это сразу видно.
            GattServiceProviderAdvertisementStatus adv_status =
                provider_local.AdvertisementStatus();
            for (int i = 0; i < 20; ++i) {
                if (adv_status == GattServiceProviderAdvertisementStatus::Started) break;
                Sleep(100);
                adv_status = provider_local.AdvertisementStatus();
            }
            Log(adv_status == GattServiceProviderAdvertisementStatus::Started ? 3 : 2,
                Format(L"Реклама: %s · мой MAC %s", AdvStatusText(adv_status).c_str(),
                      LocalMac().c_str()));

            {
                const int n = 1 + (data_local2 ? 1 : 0) + (data_local3 ? 1 : 0) +
                              (data_local4 ? 1 : 0);
                Log(0, Format(L"Каналов данных в сервисе: %d (Data…Data4) — до %d блоков "
                              L"за интервал соединения",
                              n, n));
            }

            Log(3, L"GATT-сервис опубликован. Идёт BLE-реклама — ожидание Central…");
            Log(0, L"Адрес в BLE-рекламе Windows случайный — он может отличаться от MAC "
                   L"адаптера. На отправителе выбирайте пункт «Приёмник Lab6».");

            ack_stop.store(false);
            ack_thread = std::thread([this] { AckWorker(); });
            rxq_stop.store(false);
            rxq_thread = std::thread([this] { RxQueueMain(); });
            Log(0, L"На отправителе выберите в списке «Приёмник Lab6 (" + LocalMac() +
                       L")» и нажмите «Подключить».");
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
            if (data_local2) data_local2.WriteRequested(t_data2);
            if (data_local3) data_local3.WriteRequested(t_data3);
            if (data_local4) data_local4.WriteRequested(t_data4);
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

        ack_stop.store(true);
        ack_cv.notify_all();
        JoinThread(ack_thread);

        rxq_stop.store(true);
        rxq_cv.notify_all();
        JoinThread(rxq_thread);
        {
            std::lock_guard<std::mutex> lock(rxq_mu);
            rxq.clear();
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
        name_stop.store(true);
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
        data2_ch = nullptr;
        data3_ch = nullptr;
        data4_ch = nullptr;
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

void Engine::RefreshLocalMac() { impl_->QueryLocalMac(); }

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
        const std::wstring keep_mac = d.stats.local_mac;
        d.stats = Stats{};
        d.stats.local_mac = keep_mac;   // свой MAC не зависит от роли
        d.stats.role = (int)role;
        d.stats.status =
            role == RoleSender ? L"Сканирование не запущено" : L"Приём не запущен";
        d.stats.speed_hist.clear();
    }
    d.role = role;
    d.cancel.store(false);
    d.scan_stop.store(true);
    d.recv_stop.store(true);

    d.Log(0, role == RoleSender ? L"Роль: отправитель (GATT Central) — передача файла на приёмник"
                                : L"Роль: приёмник (GATT Peripheral) — приём файла от Central");
    d.QueryLocalMac();
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

void Engine::RequestName(std::size_t index) {
    Impl& d = *impl_;
    std::uint64_t address = 0;
    int address_type = 0;
    bool need = false;
    {
        std::lock_guard<std::mutex> lock(d.mu);
        if (index < d.devices.size() && d.devices[index].name.empty()) {
            address = d.devices[index].address;
            address_type = d.devices[index].address_type;
            need = true;
        }
    }
    if (need) d.ResolveName(address, address_type);
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
    d.Log(0, L"Выбрано устройство: " + MacWithName(address, list[index].name));
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
    const std::uint64_t was_address = d.LinkAddress();
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
    d.Log(0, was_address ? (L"Отключено от MAC " + MacText(was_address))
                         : std::wstring(L"Отключено"));
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
    JoinThread(d.mac_thread);
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
