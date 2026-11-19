// ===========================================================================
//  ble.h — интерфейс Bluetooth LE движка (вариант 8).
//
//  Движок умеет работать в двух ролях:
//    • Отправитель (GATT Central)  — сканирует LE-рекламу, подключается,
//      находит кастомный сервис и передаёт файл блоками через
//      характеристику Data (Write Without Response), подтверждения получает
//      уведомлениями характеристики Status;
//    • Приёмник (GATT Peripheral) — публикует сервис, принимает блоки,
//      пишет их на диск, ведёт контроль CRC32 и подтверждает смещение.
//
//  Заголовок намеренно не знает о WinRT: все типы Windows.Devices.Bluetooth
//  спрятаны в ble.cpp за PIMPL. Благодаря этому main.cpp остаётся обычным
//  Win32-приложением и собирается без C++/WinRT-типов в интерфейсе.
// ===========================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ble {

// Роль приложения в BLE-обмене.
enum Role { RoleSender = 0, RoleReceiver = 1 };

// Состояние радиоканала (для индикатора в шапке).
enum LinkState {
    LinkIdle = 0,
    LinkScanning,
    LinkAdvertising,
    LinkConnecting,
    LinkReady,
    LinkTransfer,
    LinkPaused,
    LinkDone,
    LinkError,
};

// Что происходит с файлом.
enum Transfer { TrNone = 0, TrSending, TrReceiving, TrPaused, TrDone, TrFailed };

// Движок держит состояние у себя; окно забирает его по этому сообщению.
constexpr UINT kUpdateMessage = WM_APP + 42;

struct DeviceInfo {
    std::uint64_t address = 0;
    int address_type = 0;       // 0 — public, 1 — random (BluetoothAddressType)
    std::wstring name;
    int rssi = -127;
    bool has_service = false;   // в рекламе есть UUID нашего сервиса
    unsigned long long seen = 0;
};

struct Stats {
    int role = RoleSender;
    int link = LinkIdle;
    int transfer = TrNone;

    std::wstring status;   // короткая строка состояния
    std::wstring device;   // имя и адрес подключённого устройства
    std::wstring file;     // имя файла
    std::wstring file_path;

    int rssi = -127;
    int mtu = 0;
    int payload = 0;       // байт данных в одном блоке (MTU - 3 - 4)

    unsigned long long total = 0;
    unsigned long long done = 0;      // подтверждено приёмником
    unsigned long long chunks = 0;
    unsigned long long acks = 0;
    unsigned long long resumed = 0;   // с какого байта возобновили
    double speed = 0;                 // байт/с (сглаженная)
    double eta = 0;                   // секунд до конца

    bool has_crc = false;
    unsigned int crc_expected = 0;
    unsigned int crc_actual = 0;
    bool crc_ok = false;

    std::wstring local_mac;   // MAC своего адаптера (в приёмнике — адрес для отправки)
    std::wstring peer_mac;    // MAC второй стороны (Central в режиме приёмника)

    bool scanning = false;
    bool connected = false;
    std::vector<float> speed_hist;   // история скорости, байт/с (до 160 точек)
};

struct LogEntry {
    std::wstring text;
    int level = 0;   // 0 — инфо, 1 — предупреждение, 2 — ошибка, 3 — успех
};

// ---------------------------------------------------------------------------
//  Движок
// ---------------------------------------------------------------------------
class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void SetNotifyWindow(HWND hwnd);          // куда посылать kUpdateMessage
    void RefreshLocalMac();                   // узнать MAC своего адаптера
    void SetIncomingDir(const std::wstring& dir);
    std::wstring IncomingDir() const;

    Role CurrentRole() const;
    void SetRole(Role role);                  // переключает стек, всё останавливает

    // --- отправитель (Central) ---
    void StartScan();
    void StopScan();
    std::vector<DeviceInfo> Devices() const;
    void ClearDevices();
    bool Connect(std::size_t index);          // индекс в списке Devices()
    void RequestName(std::size_t index);      // догрузить имя из системы (если есть)
    void Disconnect();
    bool StartSend(const std::wstring& path);
    void Cancel();

    // --- приёмник (Peripheral) ---
    bool StartReceiving();
    void StopReceiving();

    Stats GetStats() const;
    std::vector<LogEntry> TakeLog();
    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ble
