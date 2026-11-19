// ===========================================================================
//  main.cpp — приложение «GATT · Передача файлов» (ЛР №6, вариант 8).
//
//  Логика окна: команды пользователя → ble::Engine (отдельные MTA-потоки),
//  обратно — снимки состояния и журнал. Визуал (visual.cpp) получает готовую
//  структуру visual::Model и ничего не знает о Bluetooth.
//
//  Роли:
//    «Отправитель (Central)» — ищет LE-устройства, подключается, передаёт файл
//    блоками Write Without Response, показывает прогресс/скорость/ETA,
//    проверяет CRC32 и возобновляет передачу после разрыва связи;
//    «Приёмник (Peripheral)» — публикует GATT-сервис, принимает блоки,
//    пишет файл на диск, подтверждает смещение и считает CRC32.
// ===========================================================================
#include "ble.h"
#include "visual.h"

#include <commctrl.h>
#include <commdlg.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")

#pragma comment(linker, "\"/manifestdependency:type='win32' \
 name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
 processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using namespace visual;

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buffer, std::size(buffer), _TRUNCATE, fmt, args);
    va_end(args);
    return buffer;
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                         nullptr, nullptr);
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), out.data(), size, nullptr,
                        nullptr);
    return out;
}

std::wstring NowStamp(bool milliseconds) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return milliseconds ? Format(L"%02u.%02u.%04u %02u:%02u:%02u.%03u", st.wDay, st.wMonth,
                                 st.wYear, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds)
                        : Format(L"%02u.%02u.%04u %02u:%02u:%02u", st.wDay, st.wMonth, st.wYear,
                                 st.wHour, st.wMinute, st.wSecond);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    const wchar_t last = dir.back();
    return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
}

bool DirExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDir(const std::wstring& path) {
    if (path.empty()) return false;
    if (DirExists(path)) return true;

    std::wstring partial;
    for (size_t i = 0; i < path.size(); ++i) {
        partial += path[i];
        const bool last = (i + 1 == path.size());
        if (path[i] == L'\\' || path[i] == L'/' || last) {
            if (partial.size() > 3) CreateDirectoryW(partial.c_str(), nullptr);
        }
    }
    return DirExists(path);
}

std::wstring DocumentsDir() {
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &raw))) {
        result = raw;
    }
    if (raw) CoTaskMemFree(raw);
    if (result.empty()) {
        wchar_t buffer[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"USERPROFILE", buffer, MAX_PATH) > 0) {
            result = JoinPath(buffer, L"Documents");
        } else {
            result = L".";
        }
    }
    return result;
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
    if (seconds <= 0.0 || seconds > 172800.0) return L"—";
    const int total = (int)(seconds + 0.5);
    if (total < 60) return Format(L"~%d с", total);
    if (total < 3600) return Format(L"~%d мин %02d с", total / 60, total % 60);
    return Format(L"~%d ч %02d мин", total / 3600, (total % 3600) / 60);
}

}  // namespace

// ===========================================================================
//  Приложение
// ===========================================================================
namespace {

class App {
public:
    enum : UINT {
        TIMER_UI = 1,
        TIMER_TOAST = 2,
    };

    App() = default;
    ~App() { Cleanup(); }

    bool Init(HINSTANCE inst, int show) {
        inst_ = inst;

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &App::WndProc;
        wc.hInstance = inst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0,
                                     LR_DEFAULTSIZE | LR_SHARED);
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return false;

        hwnd_ = CreateWindowExW(0, kClassName, L"Передача файлов по Bluetooth",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                S(1020), S(700), nullptr, nullptr, inst_, this);
        if (!hwnd_) return false;

        ShowWindow(hwnd_, show);
        UpdateWindow(hwnd_);
        return true;
    }

    int Run() {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return (int)msg.wParam;
    }

private:
    static constexpr wchar_t kClassName[] = L"Lab6GattTransferWindow";

    // -----------------------------------------------------------------------
    //  Окно
    // -----------------------------------------------------------------------
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        App* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<App*>(cs->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->Handle(msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE:
                OnCreate();
                return 0;

            case WM_SIZE:
                view_.Layout();
                return 0;

            case WM_GETMINMAXINFO: {
                auto* info = reinterpret_cast<MINMAXINFO*>(lp);
                info->ptMinTrackSize.x = view_.MinWidth();
                info->ptMinTrackSize.y = view_.MinHeight();
                return 0;
            }

            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(hwnd_, &ps);
                RECT rc;
                GetClientRect(hwnd_, &rc);
                if (rc.right > 0 && rc.bottom > 0) {
                    HDC mem = CreateCompatibleDC(dc);
                    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                    HGDIOBJ old = SelectObject(mem, bmp);
                    view_.Paint(mem, rc, model_);
                    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
                    SelectObject(mem, old);
                    DeleteObject(bmp);
                    DeleteDC(mem);
                }
                EndPaint(hwnd_, &ps);
                return 0;
            }

            case WM_TIMER:
                if (wp == TIMER_UI) {
                    Refresh();
                    return 0;
                }
                if (wp == TIMER_TOAST) {
                    if (!view_.ExpireToasts()) KillTimer(hwnd_, TIMER_TOAST);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                return 0;

            case ble::kUpdateMessage:
                Refresh();
                return 0;

            case WM_COMMAND:
                OnCommand(LOWORD(wp), HIWORD(wp));
                return 0;

            case WM_DROPFILES: {
                HDROP drop = (HDROP)wp;
                wchar_t path[MAX_PATH * 4] = {};
                if (DragQueryFileW(drop, 0, path, _countof(path))) {
                    if (ble::RoleSender == engine_.CurrentRole()) {
                        SetWindowTextW(view_.FileEdit(), path);
                        AddLog(0, std::wstring(L"Выбран файл: ") + path);
                    }
                }
                DragFinish(drop);
                return 0;
            }

            case WM_DPICHANGED: {
                SetDpi((int)HIWORD(wp));
                view_.RecreateFonts();
                RECT* rc = reinterpret_cast<RECT*>(lp);
                SetWindowPos(hwnd_, nullptr, rc->left, rc->top, rc->right - rc->left,
                             rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
                view_.Layout();
                InvalidateRect(hwnd_, nullptr, TRUE);
                return 0;
            }

            case WM_DRAWITEM: {
                auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
                if (view_.DrawItem(item)) return TRUE;
                break;
            }

            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX: {
                HDC dc = (HDC)wp;
                SetTextColor(dc, RGB(235, 240, 248));
                SetBkColor(dc, RGB(12, 18, 30));
                return (LRESULT)view_.FieldBrush();
            }

            case WM_CLOSE:
                DestroyWindow(hwnd_);
                return 0;

            case WM_DESTROY:
                Cleanup();
                PostQuitMessage(0);
                return 0;

            default:
                break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    void OnCreate() {
        view_.Create(hwnd_, inst_);

        incoming_dir_ = JoinPath(DocumentsDir(), L"Lab6_BLE");
        EnsureDir(incoming_dir_);

        engine_.SetNotifyWindow(hwnd_);
        engine_.SetIncomingDir(incoming_dir_);
        engine_.SetRole(ble::RoleSender);
        engine_.RefreshLocalMac();   // «Мой MAC» виден сразу после запуска

        SendMessageW(view_.RoleCombo(), CB_SETCURSEL, 0, 0);
        DragAcceptFiles(hwnd_, TRUE);

        SetTimer(hwnd_, TIMER_UI, 150, nullptr);
        AddLog(0, L"Приложение запущено · приём и передача файлов по Bluetooth LE");
        AddLog(0, L"Каталог приёма: " + incoming_dir_);

        Refresh();
    }

    // -----------------------------------------------------------------------
    //  Команды
    // -----------------------------------------------------------------------
    void OnCommand(int id, int code) {
        switch (id) {
            case ID_ROLE:
                if (code == CBN_SELCHANGE) SwitchRole();
                break;
            case ID_SCAN:
                if (code == BN_CLICKED) ToggleScan();
                break;
            case ID_CONNECT:
                if (code == BN_CLICKED) Connect();
                break;
            case ID_DISCONNECT:
                if (code == BN_CLICKED) engine_.Disconnect();
                break;
            case ID_DEVICE:
                if (code == CBN_SELCHANGE) {
                    const int sel = (int)SendMessageW(view_.DeviceCombo(), CB_GETCURSEL, 0, 0);
                    if (sel >= 0) engine_.RequestName(DeviceIndex(sel));
                }
                break;
            case ID_BROWSE:
                if (code == BN_CLICKED) BrowseFile();
                break;
            case ID_SEND:
                if (code == BN_CLICKED) SendFile();
                break;
            case ID_CANCEL:
                if (code == BN_CLICKED) engine_.Cancel();
                break;
            default:
                break;
        }
    }

    void SwitchRole() {
        const int sel = (int)SendMessageW(view_.RoleCombo(), CB_GETCURSEL, 0, 0);
        const ble::Role role = sel == 1 ? ble::RoleReceiver : ble::RoleSender;

        engine_.SetRole(role);   // останавливает всё лишнее и пишет роль в журнал

        SendMessageW(view_.DeviceCombo(), CB_RESETCONTENT, 0, 0);
        device_signature_.clear();
        devices_count_ = 0;
        device_order_.clear();

        SetWindowTextW(view_.FileEdit(),
                       role == ble::RoleSender ? send_path_.c_str() : incoming_dir_.c_str());

        // Не заставляем искать кнопку: приём стартует сразу (иначе «Мой MAC»
        // и реклама появлялись только после ручного «Старт»), скан — тоже.
        if (role == ble::RoleReceiver) {
            engine_.StartReceiving();
            AddLog(0, L"Приём запущен: идёт BLE-реклама, ждём подключения отправителя");
        } else {
            engine_.ClearDevices();
            engine_.StartScan();
            AddLog(0, L"Поиск устройств запущен");
        }
        Refresh();
    }

    void ToggleScan() {
        const ble::Stats stats = engine_.GetStats();
        if (stats.role == ble::RoleReceiver) {
            if (stats.scanning) {
                engine_.StopReceiving();
                AddLog(0, L"Приём остановлен");
            } else {
                engine_.StartReceiving();
                AddLog(0, L"Запуск GATT-сервера и BLE-рекламы…");
            }
            return;
        }

        if (stats.scanning) {
            engine_.StopScan();
        } else {
            engine_.ClearDevices();
            SendMessageW(view_.DeviceCombo(), CB_RESETCONTENT, 0, 0);
            device_signature_.clear();
            devices_count_ = 0;
            engine_.StartScan();
        }
    }

    void Connect() {
        const int sel = (int)SendMessageW(view_.DeviceCombo(), CB_GETCURSEL, 0, 0);
        if (sel < 0) {
            AddLog(1, L"Сначала выберите устройство в списке");
            return;
        }
        if (!engine_.Connect(DeviceIndex(sel))) {
            AddLog(1, L"Подключение не запущено");
        }
    }

    void BrowseFile() {
        wchar_t path[MAX_PATH * 4] = {};
        if (!send_path_.empty()) wcsncpy_s(path, send_path_.c_str(), _TRUNCATE);

        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = hwnd_;
        dialog.lpstrFilter = L"Все файлы\0*.*\0Документы\0*.txt;*.pdf;*.doc*;*.xls*\0"
                             L"Изображения\0*.jpg;*.jpeg;*.png;*.bmp\0";
        dialog.lpstrFile = path;
        dialog.nMaxFile = _countof(path);
        dialog.lpstrTitle = L"Файл для передачи по BLE";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;

        if (!GetOpenFileNameW(&dialog)) return;

        send_path_ = path;
        SetWindowTextW(view_.FileEdit(), send_path_.c_str());
        AddLog(0, L"Выбран файл: " + send_path_);
        Refresh();
    }

    void SendFile() {
        wchar_t buffer[MAX_PATH * 4] = {};
        GetWindowTextW(view_.FileEdit(), buffer, _countof(buffer));
        std::wstring path = buffer;

        if (path.empty()) {
            BrowseFile();
            GetWindowTextW(view_.FileEdit(), buffer, _countof(buffer));
            path = buffer;
            if (path.empty()) return;
        }

        if (path == incoming_dir_) {
            AddLog(1, L"Выберите файл, а не каталог");
            return;
        }

        const ble::Stats current = engine_.GetStats();
        if (!current.connected) {
            const int sel = (int)SendMessageW(view_.DeviceCombo(), CB_GETCURSEL, 0, 0);
            if (sel < 0) {
                AddLog(1, L"Выберите устройство в списке (сначала «Поиск»)");
                return;
            }
            engine_.Connect(DeviceIndex(sel));   // задаём адрес; передача ждёт связи
        }

        send_path_ = path;
        if (!engine_.StartSend(path)) {
            AddLog(1, L"Передача не запущена");
        }
    }


    // -----------------------------------------------------------------------
    //  Обновление модели
    // -----------------------------------------------------------------------
    void Refresh() {
        for (const auto& entry : engine_.TakeLog()) {
            AddLog(entry.level, entry.text);
        }

        const ble::Stats stats = engine_.GetStats();
        stats_ = stats;

        RefreshDeviceCombo(stats);
        UpdateModel(stats);

        // Кнопка поиска/приёма меняет подпись в зависимости от состояния.
        const bool active = stats.scanning;
        const std::wstring label = stats.role == ble::RoleReceiver
                                       ? (active ? L"Стоп" : L"Старт")
                                       : (active ? L"Стоп" : L"Поиск");
        if (label != scan_label_) {
            scan_label_ = label;
            SetWindowTextW(GetDlgItem(hwnd_, ID_SCAN), scan_label_.c_str());
        }

        view_.SetEnabled(model_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Индекс, выбранный в комбобоксе, → индекс в списке движка.
    size_t DeviceIndex(int combo_index) const {
        if (combo_index < 0 || (size_t)combo_index >= device_order_.size()) return 0;
        return device_order_[(size_t)combo_index];
    }

    void RefreshDeviceCombo(const ble::Stats& stats) {
        if (stats.role != ble::RoleSender) return;

        const std::vector<ble::DeviceInfo> devices = engine_.Devices();

        // Отбрасываем «шум»: устройства без имени и с очень слабым сигналом
        // только засоряют список. Свой сервис показываем всегда и первым.
        std::vector<size_t> order;
        order.reserve(devices.size());
        for (size_t i = 0; i < devices.size(); ++i) {
            const ble::DeviceInfo& device = devices[i];
            if (device.has_service || !device.name.empty() || device.rssi >= -88) {
                order.push_back(i);
            }
        }
        std::stable_sort(order.begin(), order.end(), [&devices](size_t a, size_t b) {
            return devices[a].has_service > devices[b].has_service;
        });
        const size_t kMaxDevices = 24;
        if (order.size() > kMaxDevices) order.resize(kMaxDevices);

        std::wstring signature;
        for (size_t index : order) {
            const ble::DeviceInfo& device = devices[index];
            signature += Format(L"%016llX|%s|%d;", (unsigned long long)device.address,
                                device.name.c_str(), device.has_service ? 1 : 0);
        }

        if (signature == device_signature_) {
            devices_count_ = devices.size();
            return;
        }
        device_signature_ = signature;

        const int previous = (int)SendMessageW(view_.DeviceCombo(), CB_GETCURSEL, 0, 0);
        SendMessageW(view_.DeviceCombo(), CB_RESETCONTENT, 0, 0);

        device_order_ = order;

        for (size_t index : order) {
            const ble::DeviceInfo& device = devices[index];
            std::wstring text;
            if (!device.name.empty()) {
                text = device.name;
            } else if (device.has_service) {
                // Наш приёмник рекламируется без имени — подписываем понятно.
                text = L"Приёмник Lab6";
            } else {
                text = L"Без имени";
            }
            text += Format(L"  (%02X:%02X:%02X:%02X:%02X:%02X)",
                           (unsigned)((device.address >> 40) & 0xFF),
                           (unsigned)((device.address >> 32) & 0xFF),
                           (unsigned)((device.address >> 24) & 0xFF),
                           (unsigned)((device.address >> 16) & 0xFF),
                           (unsigned)((device.address >> 8) & 0xFF),
                           (unsigned)(device.address & 0xFF));
            // Тип BLE-адреса: public выдаётся производителем, random генерирует
            // хост (смартфоны, трекеры) — полезно при идентификации устройства.
            if (device.address_type == 1) text += L"  (random)";
            if (device.has_service && !device.name.empty()) text += L"   •  Lab6";
            SendMessageW(view_.DeviceCombo(), CB_ADDSTRING, 0, (LPARAM)text.c_str());
        }

        devices_count_ = order.size();
        if (!order.empty()) {
            int select = previous;
            if (select < 0 || select >= (int)order.size()) {
                select = 0;
                for (size_t i = 0; i < order.size(); ++i) {
                    if (devices[order[i]].has_service) {
                        select = (int)i;
                        break;
                    }
                }
            }
            SendMessageW(view_.DeviceCombo(), CB_SETCURSEL, select, 0);
        }
    }

    void UpdateModel(const ble::Stats& stats) {
        Model& model = model_;

        model.role = stats.role;
        model.sender = stats.role == ble::RoleSender;
        model.has_devices = devices_count_ > 0;
        model.scanning = stats.scanning;
        model.connected = stats.connected;
        model.status = stats.status;

        // Индикатор в шапке.
        switch (stats.link) {
            case ble::LinkError:
                model.state = StateFail;
                break;
            case ble::LinkDone:
                model.state = StateOk;
                break;
            case ble::LinkScanning:
            case ble::LinkAdvertising:
            case ble::LinkConnecting:
            case ble::LinkTransfer:
            case ble::LinkPaused:
                model.state = StateBusy;
                break;
            case ble::LinkReady:
                model.state = stats.connected ? StateOk : StateIdle;
                break;
            default:
                model.state = StateIdle;
                break;
        }

        switch (stats.transfer) {
            case ble::TrSending:
            case ble::TrReceiving: model.phase = PhaseRun; break;
            case ble::TrPaused:    model.phase = PhasePause; break;
            case ble::TrDone:      model.phase = PhaseDone; break;
            case ble::TrFailed:    model.phase = PhaseFail; break;
            default:               model.phase = PhaseIdle; break;
        }

        model.file_name = stats.file;
        model.file_path = stats.file_path;

        model.progress = stats.total > 0 ? std::min(1.0, (double)stats.done / (double)stats.total)
                                         : 0.0;

        if (stats.total > 0) {
            model.transferred =
                Format(L"%s из %s", SizeText(stats.done).c_str(), SizeText(stats.total).c_str());
        } else {
            model.transferred = L"—";
        }

        model.speed = SpeedText(stats.speed);
        model.eta = TimeText(stats.eta);

        if (stats.has_crc && (stats.crc_actual != 0 || stats.crc_ok || stats.transfer == ble::TrDone ||
                              stats.transfer == ble::TrFailed)) {
            model.crc = Format(L"%08X  →  %08X", stats.crc_expected, stats.crc_actual);
            model.crc_state = stats.crc_ok ? 1 : 2;
        } else if (stats.has_crc) {
            model.crc = Format(L"%08X  →  …", stats.crc_expected);
            model.crc_state = 0;
        } else {
            model.crc.clear();
            model.crc_state = 0;
        }

        model.device = stats.device;
        model.local_mac = stats.local_mac;
        model.peer_mac = stats.peer_mac;
        model.role_name = model.sender ? L"Central" : L"Peripheral";
    }

    // -----------------------------------------------------------------------
    //  Журнал, отчёт, завершение
    // -----------------------------------------------------------------------
    void AddLog(int level, const std::wstring& text) {
        if (text.empty()) return;
        const std::wstring line = NowStamp(true) + L"  " + text;

        view_.AddLogLine(line, level);

        // Дублируем в файл: журнал сохраняется даже при аварийном закрытии.
        if (!incoming_dir_.empty()) {
            HANDLE file = CreateFileW(JoinPath(incoming_dir_, L"ble_log.txt").c_str(),
                                      FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                const std::string utf8 = ToUtf8(line + L"\r\n");
                DWORD written = 0;
                LARGE_INTEGER size{};
                if (GetFileSizeEx(file, &size) && size.QuadPart == 0) {
                    static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
                    WriteFile(file, bom, 3, &written, nullptr);
                }
                WriteFile(file, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
                CloseHandle(file);
            }
        }

        if (level >= 1) view_.PushToast(text, level);
        if (!cleaned_) SetTimer(hwnd_, TIMER_TOAST, 80, nullptr);
    }


    void Cleanup() {
        if (cleaned_) return;
        cleaned_ = true;

        KillTimer(hwnd_, TIMER_UI);
        KillTimer(hwnd_, TIMER_TOAST);
        DragAcceptFiles(hwnd_, FALSE);

        AddLog(0, L"Приложение закрыто");
        engine_.Shutdown();
        view_.Destroy();
    }

    // --- поля --------------------------------------------------------------
    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    bool cleaned_ = false;

    View view_;
    Model model_;
    ble::Engine engine_;
    ble::Stats stats_;

    std::wstring incoming_dir_;
    std::wstring send_path_;
    std::wstring device_signature_;
    std::vector<size_t> device_order_;   // индекс в комбобоксе → индекс в Devices()
    std::wstring scan_label_;
    size_t devices_count_ = 0;
};

}  // namespace

// ===========================================================================
//  Точка входа
// ===========================================================================
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    // DPI: иначе шрифты выглядят мелкими и размытыми на масштабе 125–150 %.
    if (HMODULE user32 = GetModuleHandleW(L"user32")) {
        using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto set_ctx = reinterpret_cast<SetCtxFn>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        } else {
            SetProcessDPIAware();
        }
    }
    {
        HDC screen = GetDC(nullptr);
        int dpi = 96;
        if (screen) {
            dpi = GetDeviceCaps(screen, LOGPIXELSY);
            ReleaseDC(nullptr, screen);
        }
        SetDpi(dpi);
    }

    // GDI+ — основа оформления (как в ЛР №5).
    Gdiplus::GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr) != Gdiplus::Ok) return 1;

    // COM в STA нужен диалогу выбора файла/каталога; BLE-движок работает в MTA.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&controls);

    int result = 0;
    {
        App app;
        if (!app.Init(inst, show)) {
            MessageBoxW(nullptr, L"Не удалось создать окно приложения.", L"Ошибка",
                        MB_OK | MB_ICONERROR);
            result = 1;
        } else {
            result = app.Run();
        }
    }

    if (SUCCEEDED(com)) CoUninitialize();
    Gdiplus::GdiplusShutdown(gdiplus_token);
    return result;
}
