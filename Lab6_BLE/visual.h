// ===========================================================================
//  visual.h — интерфейс между логикой (main.cpp) и визуалом (visual.cpp).
//
//  Дизайн перенесён из ЛР №5: тёмная тема, карточки со скруглением, шапка с
//  индикатором состояния, owner-draw кнопки/списки, всплывающие уведомления.
//  Модуль ничего не знает о Bluetooth: он получает готовую структуру Model
//  и рисует по ней окно. Вся логика — в main.cpp и ble.cpp.
// ===========================================================================
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <string>
#include <vector>

namespace visual {

// Цвет и текст индикатора в шапке.
enum AppState { StateIdle = 0, StateBusy = 1, StateOk = 2, StateFail = 3 };

// Фаза передачи — от неё зависит цвет «чипа» в карточке «Передача».
enum Phase { PhaseIdle = 0, PhaseRun = 1, PhasePause = 2, PhaseDone = 3, PhaseFail = 4 };

// Идентификаторы дочерних контролов.
enum : int {
    ID_ROLE = 1201,
    ID_DEVICE,
    ID_SCAN,
    ID_CONNECT,
    ID_DISCONNECT,
    ID_FILE,
    ID_BROWSE,
    ID_SEND,
    ID_CANCEL,
    ID_OPEN_DIR,
    ID_REPORT,
    ID_LOG,
};

// Всё, что визуалу нужно знать о состоянии приложения.
struct Model {
    int role = 0;          // 0 — отправитель (Central), 1 — приёмник (Peripheral)
    int state = StateIdle;
    int phase = PhaseIdle;

    std::wstring status;   // индикатор в шапке
    std::wstring file_name;
    std::wstring file_path;
    std::wstring transferred;
    std::wstring speed;
    std::wstring eta;
    std::wstring chunks;
    std::wstring crc;      // «1A2B3C4D → 1A2B3C4D»
    int crc_state = 0;     // 0 — нет данных, 1 — совпал, 2 — ошибка
    std::wstring hint;     // подсказка при простое
    double progress = 0.0;

    std::wstring device;
    std::wstring rssi;
    std::wstring mtu;
    std::wstring channel;
    std::wstring service;
    std::wstring role_name;

    std::vector<float> speed_hist;

    bool has_devices = false;
    bool scanning = false;
    bool connected = false;
    bool sender = true;
};

// --- Масштабирование по DPI -------------------------------------------------
void SetDpi(int dpi);
int S(int dip);

// --- Окно ------------------------------------------------------------------
class View {
public:
    View() = default;
    ~View();

    View(const View&) = delete;
    View& operator=(const View&) = delete;

    bool Create(HWND parent, HINSTANCE inst);
    void Destroy();

    void RecreateFonts();                       // после смены DPI
    void Layout();                              // раскладка и позиции контролов
    void Paint(HDC dc, const RECT& client, const Model& model);
    bool DrawItem(const DRAWITEMSTRUCT* item);  // owner-draw: кнопки, комбо, журнал
    void SetEnabled(const Model& model);

    // Доступ к контролам для логики.
    HWND RoleCombo() const { return h_role_; }
    HWND DeviceCombo() const { return h_device_; }
    HWND FileEdit() const { return h_file_; }
    HWND LogList() const { return h_log_; }

    void AddLogLine(const std::wstring& line, int level);
    void PushToast(const std::wstring& text, int level);
    bool ExpireToasts();

    HBRUSH FieldBrush() const { return field_brush_; }

    int MinWidth() const;
    int MinHeight() const;

private:
    HWND Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id);
    void CreateControls();
    void ApplyFontsToControls();

    void PaintBackground(Gdiplus::Graphics& g, int w, int h);
    void DrawHeader(Gdiplus::Graphics& g, const Model& model);
    void DrawTransferCard(Gdiplus::Graphics& g, const Model& model);
    void DrawSessionCard(Gdiplus::Graphics& g, const Model& model);
    void DrawFileCard(Gdiplus::Graphics& g, const Model& model);
    void DrawLogCard(Gdiplus::Graphics& g, const Model& model);
    void DrawToasts(Gdiplus::Graphics& g);

    void DrawButton(const DRAWITEMSTRUCT* item);
    void DrawCombo(const DRAWITEMSTRUCT* item);
    void DrawLogItem(const DRAWITEMSTRUCT* item);

    HWND parent_ = nullptr;
    HINSTANCE inst_ = nullptr;
    bool created_ = false;

    HWND h_role_ = nullptr, h_device_ = nullptr, h_scan_ = nullptr;
    HWND h_connect_ = nullptr, h_disconnect_ = nullptr;
    HWND h_file_ = nullptr, h_browse_ = nullptr, h_send_ = nullptr, h_cancel_ = nullptr;
    HWND h_open_dir_ = nullptr, h_report_ = nullptr, h_log_ = nullptr;
    HBRUSH field_brush_ = nullptr;

    Gdiplus::Font* f_title_ = nullptr;
    Gdiplus::Font* f_section_ = nullptr;
    Gdiplus::Font* f_label_ = nullptr;
    Gdiplus::Font* f_value_ = nullptr;
    Gdiplus::Font* f_button_ = nullptr;
    Gdiplus::Font* f_log_ = nullptr;
    Gdiplus::Font* f_big_ = nullptr;
    Gdiplus::Font* f_small_ = nullptr;

    // Геометрия карточек и элементов (физические пиксели).
    RECT transfer_rc_{}, session_rc_{}, file_rc_{}, log_rc_{};
    RECT title_rc_{}, subtitle_rc_{};
    RECT transfer_head_{}, session_head_{}, file_head_{}, log_head_{};

    RECT name_rc_{}, path_rc_{}, percent_rc_{}, chip_rc_{}, bar_rc_{};
    RECT cell_rc_[3]{};          // «Передано», «Скорость», «Осталось»
    RECT crc_rc_{}, graph_rc_{}, graph_head_{};
    RECT hint_rc_{};          // подсказка в карточке «Передача»
    RECT session_hint_rc_{};  // подсказка в карточке «Сеанс»

    RECT role_label_{}, device_label_{}, dir_label_{}, file_label_strip_{};
    RECT file_edit_rc_{}, separator_rc_{};
    RECT info_rc_[5]{};          // строки «подпись — значение» в карточке «Сеанс»

    struct Toast {
        std::wstring text;
        int level = 0;
        ULONGLONG until = 0;
    };
    std::vector<Toast> toasts_;

    int separator_y_ = 0;
    int info_top_ = 0;
    int rows_ = 5;
    bool show_path_ = true;
    bool show_crc_ = true;
    bool show_graph_ = true;
    bool show_hint_ = true;
    bool show_dir_label_ = true;
    unsigned int enabled_mask_ = 0xFFFFFFFFu;
};

}  // namespace visual
