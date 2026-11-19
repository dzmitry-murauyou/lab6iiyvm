// ===========================================================================
//  visual.cpp — вся визуальная часть: тема, шрифты, раскладка, отрисовка.
//
//  Оформление повторяет ЛР №5 (тёмная тема, карточки, шапка с индикатором,
//  owner-draw контролы), но наполнение другое: вместо предпросмотра камеры —
//  карточка передачи с прогрессом, скоростью, ETA и графиком скорости.
//
//  Раскладка «потоком сверху вниз» внутри карточки с обрезкой по её границам
//  (SetClip): при любом размере окна и любом DPI элементы не наезжают друг на
//  друга, а лишние (путь файла, CRC, график) просто убираются.
// ===========================================================================
#include "visual.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace visual {

using namespace Gdiplus;

// ---------------------------------------------------------------------------
//  Тема
// ---------------------------------------------------------------------------
namespace theme {

const Color Bg(255, 10, 14, 24);
const Color Card(215, 18, 26, 44);
const Color Txt(255, 235, 240, 248);
const Color Sub(255, 165, 178, 198);
const Color Dim(255, 120, 130, 150);
const Color Line(255, 70, 95, 135);
const Color Good(255, 46, 204, 113);
const Color Warn(255, 241, 196, 15);
const Color Bad(255, 235, 87, 87);
const Color Field(235, 12, 18, 30);
const Color Btn(255, 30, 42, 64);
const Color BtnHot(255, 42, 58, 86);
const Color Accent(255, 94, 156, 255);

const int Pad = 22;
const int Gap = 12;
const int HeaderH = 92;
const int LogH = 110;
const int FileH = 156;
const int PanelW = 352;
const int RowH = 22;
const int InnerPad = 16;
const int Radius = 6;   // в DIP; масштабируется через S(), поэтому целое

}  // namespace theme

// ---------------------------------------------------------------------------
//  DPI
// ---------------------------------------------------------------------------
namespace {
int g_dpi = 96;
}

void SetDpi(int dpi) { g_dpi = (dpi >= 96 && dpi <= 480) ? dpi : 96; }

int S(int dip) { return MulDiv(dip, g_dpi, 96); }

// ---------------------------------------------------------------------------
//  Помощники рисования
// ---------------------------------------------------------------------------
namespace {

int Px(REAL value) { return (int)std::lround(value); }

void FillRectG(Graphics& g, const RECT& rc, const Color& color) {
    SolidBrush brush(color);
    g.FillRectangle(&brush, (INT)rc.left, (INT)rc.top, (INT)(rc.right - rc.left),
                    (INT)(rc.bottom - rc.top));
}

void RoundedRect(Graphics& g, const RECT& rc, const Color& fill, const Color& border,
                 REAL radius) {
    GraphicsPath path;
    const REAL x = (REAL)rc.left, y = (REAL)rc.top;
    const REAL w = (REAL)(rc.right - rc.left), h = (REAL)(rc.bottom - rc.top);
    if (w <= 0 || h <= 0) return;
    const REAL d = std::min(radius * 2, std::min(w, h));
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
    SolidBrush brush(fill);
    g.FillPath(&brush, &path);
    Pen pen(border, 1.0f);
    g.DrawPath(&pen, &path);
}

void Card(Graphics& g, const RECT& rc) {
    RoundedRect(g, rc, theme::Card, theme::Line, S(theme::Radius * 4) / 4.0f);
}

// Текст всегда обрезается по прямоугольнику с многоточием: переноса строк нет,
// поэтому слова не налезают друг на друга при любой ширине окна.
void TextG(Graphics& g, const std::wstring& text, const RECT& rc, Font* font, const Color& color,
           StringAlignment align = StringAlignmentNear) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    StringFormat fmt;
    fmt.SetAlignment(align);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush brush(color);
    RectF layout((REAL)rc.left, (REAL)rc.top, (REAL)(rc.right - rc.left),
                 (REAL)(rc.bottom - rc.top));
    g.DrawString(text.c_str(), -1, font, layout, &fmt, &brush);
}

int TextWidth(Graphics& g, const std::wstring& text, Font* font) {
    if (text.empty()) return 0;
    StringFormat fmt;
    fmt.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsMeasureTrailingSpaces);
    RectF bounds;
    g.MeasureString(text.c_str(), -1, font, RectF(0, 0, 5000.0f, 120.0f), &fmt, &bounds);
    return Px(bounds.Width);
}

void Dot(Graphics& g, int cx, int cy, int r, const Color& color) {
    SolidBrush brush(color);
    g.FillEllipse(&brush, cx - r, cy - r, r * 2, r * 2);
}

void Separator(Graphics& g, const RECT& card, int y) {
    Pen pen(theme::Line, 1.0f);
    g.DrawLine(&pen, (REAL)(card.left + S(theme::InnerPad)), (REAL)y,
               (REAL)(card.right - S(theme::InnerPad)), (REAL)y);
}

// Строка «подпись — значение».
void Row(Graphics& g, const RECT& card, int y, const std::wstring& label,
         const std::wstring& value, Font* label_font, Font* value_font) {
    const int p = S(theme::InnerPad);
    const int label_w = S(96);
    RECT l{card.left + p, y, std::min(card.left + p + label_w, card.right - p),
           y + S(theme::RowH)};
    RECT v{l.right, y, card.right - p, y + S(theme::RowH)};
    TextG(g, label, l, label_font, theme::Sub);
    TextG(g, value.empty() ? L"—" : value, v, value_font, theme::Txt);
}

Color StateColor(int state) {
    switch (state) {
        case StateOk:   return theme::Good;
        case StateBusy: return theme::Warn;
        case StateFail: return theme::Bad;
        default:        return theme::Dim;
    }
}

Color PhaseColor(int phase) {
    switch (phase) {
        case PhaseRun:   return theme::Accent;
        case PhasePause: return theme::Warn;
        case PhaseDone:  return theme::Good;
        case PhaseFail:  return theme::Bad;
        default:         return theme::Dim;
    }
}

const wchar_t* PhaseText(int phase) {
    switch (phase) {
        case PhaseRun:   return L"ПЕРЕДАЧА";
        case PhasePause: return L"ПАУЗА";
        case PhaseDone:  return L"ГОТОВО";
        case PhaseFail:  return L"ОШИБКА";
        default:         return L"ОЖИДАНИЕ";
    }
}

Color LevelColor(int level) {
    switch (level) {
        case 2: return theme::Bad;
        case 1: return theme::Warn;
        case 3: return theme::Good;
        default: return theme::Txt;
    }
}

// Полоса прогресса: закруглённый фон и закруглённая заливка, обрезанная по
// текущему проценту (углы остаются скруглёнными с обеих сторон).
void ProgressBar(Graphics& g, const RECT& rc, double progress, const Color& accent) {
    const REAL radius = S(theme::Radius * 4) / 8.0f;
    RoundedRect(g, rc, theme::Field, theme::Line, radius);

    const double value = std::clamp(progress, 0.0, 1.0);
    if (value <= 0.0) return;

    const int width = (int)std::lround((rc.right - rc.left - 2) * value);
    if (width <= 1) return;

    const GraphicsState saved = g.Save();
    g.SetClip(RectF((REAL)(rc.left + 1), (REAL)(rc.top + 1), (REAL)width,
                    (REAL)(rc.bottom - rc.top - 2)));
    RoundedRect(g, rc, accent, accent, radius);
    g.Restore(saved);

    // Деления каждые 10 % — видно, какая часть файла уже прошла.
    Pen tick(Color(70, 255, 255, 255), 1.0f);
    for (int step = 1; step < 10; ++step) {
        const int x = rc.left + (int)std::lround((double)(rc.right - rc.left) * step / 10.0);
        if (x > rc.left + width) break;
        g.DrawLine(&tick, (REAL)x, (REAL)(rc.top + 2), (REAL)x, (REAL)(rc.bottom - 2));
    }
}

// График скорости: заливка + линия по истории значений.
void SpeedGraph(Graphics& g, const RECT& rc, const std::vector<float>& history, Font* font) {
    if (rc.right - rc.left < 40 || rc.bottom - rc.top < 30) return;

    RoundedRect(g, rc, Color(200, 10, 15, 26), theme::Line, S(theme::Radius * 4) / 8.0f);

    const int left = rc.left + S(8);
    const int right = rc.right - S(8);
    const int top = rc.top + S(8);
    const int bottom = rc.bottom - S(8);
    if (right - left < 20 || bottom - top < 12) return;

    Pen grid(Color(60, 120, 150, 200), 1.0f);
    for (int i = 1; i <= 2; ++i) {
        const int y = top + (bottom - top) * i / 3;
        g.DrawLine(&grid, (REAL)left, (REAL)y, (REAL)right, (REAL)y);
    }

    if (history.size() < 2) {
        Pen base(Color(90, 120, 150, 200), 1.0f);
        g.DrawLine(&base, (REAL)left, (REAL)(bottom - 1), (REAL)right, (REAL)(bottom - 1));
        RECT text_rc{left, top, right, bottom};
        TextG(g, L"накопление данных…", text_rc, font, theme::Dim, StringAlignmentCenter);
        return;
    }

    float maximum = 0.0f;
    for (float value : history) maximum = std::max(maximum, value);
    if (maximum < 1.0f) maximum = 1.0f;

    const int count = (int)history.size();
    std::vector<PointF> points;
    points.reserve((size_t)count);
    for (int i = 0; i < count; ++i) {
        const REAL x = (REAL)left + (REAL)(right - left) * (count == 1 ? 0.5f : (REAL)i / (count - 1));
        const float ratio = std::clamp(history[(size_t)i] / maximum, 0.0f, 1.0f);
        const REAL y = (REAL)(bottom - 1) - (REAL)(bottom - top - 2) * ratio;
        points.push_back(PointF(x, y));
    }

    GraphicsPath area;
    area.AddLine(points.front().X, (REAL)(bottom - 1), points.front().X, points.front().Y);
    for (size_t i = 1; i < points.size(); ++i) area.AddLine(points[i - 1], points[i]);
    area.AddLine(points.back().X, points.back().Y, points.back().X, (REAL)(bottom - 1));
    area.CloseFigure();

    LinearGradientBrush fill(RectF((REAL)left, (REAL)top, (REAL)(right - left),
                                   (REAL)(bottom - top)),
                             Color(120, 94, 156, 255), Color(10, 94, 156, 255),
                             LinearGradientModeVertical);
    g.FillPath(&fill, &area);

    Pen line(theme::Accent, 1.6f);
    g.DrawLines(&line, points.data(), (INT)points.size());

    // Максимум шкалы — справа сверху, чтобы график читался без осей.
    std::wstring label = L"макс ";
    {
        wchar_t buffer[64];
        if (maximum >= 1024.0f * 1024.0f) {
            _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"%.1f МБ/с",
                         maximum / (1024.0f * 1024.0f));
        } else if (maximum >= 1024.0f) {
            _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"%.1f КБ/с", maximum / 1024.0f);
        } else {
            _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"%.0f Б/с", maximum);
        }
        label += buffer;
    }
    RECT label_rc{left, top, right, top + S(16)};
    TextG(g, label, label_rc, font, theme::Dim, StringAlignmentFar);
}

}  // namespace

// ---------------------------------------------------------------------------
//  Жизненный цикл
// ---------------------------------------------------------------------------
View::~View() { Destroy(); }

bool View::Create(HWND parent, HINSTANCE inst) {
    parent_ = parent;
    inst_ = inst;
    field_brush_ = CreateSolidBrush(RGB(12, 18, 30));
    RecreateFonts();
    CreateControls();
    ApplyFontsToControls();
    created_ = true;
    Layout();
    return true;
}

void View::Destroy() {
    for (Font** font : {&f_title_, &f_section_, &f_label_, &f_value_, &f_button_, &f_log_,
                        &f_big_, &f_small_}) {
        delete *font;
        *font = nullptr;
    }
    if (field_brush_) {
        DeleteObject(field_brush_);
        field_brush_ = nullptr;
    }
    created_ = false;
}

void View::RecreateFonts() {
    for (Font** font : {&f_title_, &f_section_, &f_label_, &f_value_, &f_button_, &f_log_,
                        &f_big_, &f_small_}) {
        delete *font;
        *font = nullptr;
    }

    FontFamily family(L"Segoe UI");
    f_title_ = new Font(&family, (REAL)S(24), FontStyleBold, UnitPixel);
    f_section_ = new Font(&family, (REAL)S(16), FontStyleBold, UnitPixel);
    f_label_ = new Font(&family, (REAL)S(14), FontStyleRegular, UnitPixel);
    f_value_ = new Font(&family, (REAL)S(14), FontStyleBold, UnitPixel);
    f_button_ = new Font(&family, (REAL)S(14), FontStyleBold, UnitPixel);
    f_log_ = new Font(&family, (REAL)S(14), FontStyleRegular, UnitPixel);
    f_big_ = new Font(&family, (REAL)S(40), FontStyleBold, UnitPixel);
    f_small_ = new Font(&family, (REAL)S(12), FontStyleRegular, UnitPixel);

    ApplyFontsToControls();
}

HWND View::Make(const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent_,
                           (HMENU)(INT_PTR)id, inst_, nullptr);
}

void View::CreateControls() {
    h_role_ = Make(L"COMBOBOX", L"",
                   CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL |
                       WS_TABSTOP,
                   ID_ROLE);
    h_device_ = Make(L"COMBOBOX", L"",
                     CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL |
                         WS_TABSTOP,
                     ID_DEVICE);
    h_scan_ = Make(L"BUTTON", L"Поиск", BS_OWNERDRAW | WS_TABSTOP, ID_SCAN);
    h_connect_ = Make(L"BUTTON", L"Подключить", BS_OWNERDRAW | WS_TABSTOP, ID_CONNECT);
    h_disconnect_ = Make(L"BUTTON", L"Отключить", BS_OWNERDRAW | WS_TABSTOP, ID_DISCONNECT);
    h_browse_ = Make(L"BUTTON", L"Обзор…", BS_OWNERDRAW | WS_TABSTOP, ID_BROWSE);
    h_send_ = Make(L"BUTTON", L"Отправить", BS_OWNERDRAW | WS_TABSTOP, ID_SEND);
    h_cancel_ = Make(L"BUTTON", L"Отменить", BS_OWNERDRAW | WS_TABSTOP, ID_CANCEL);
    h_open_dir_ = Make(L"BUTTON", L"Открыть папку", BS_OWNERDRAW | WS_TABSTOP, ID_OPEN_DIR);
    h_report_ = Make(L"BUTTON", L"Сохранить отчёт", BS_OWNERDRAW | WS_TABSTOP, ID_REPORT);

    h_file_ = Make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_FILE);
    h_log_ = Make(L"LISTBOX", L"",
                  WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                      WS_TABSTOP,
                  ID_LOG);
    SendMessageW(h_log_, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

    // Роли фиксированы: отправитель работает как Central, приёмник — как
    // Peripheral (GATT-сервер). Порядок совпадает с ble::Role.
    // Высота элемента задаётся явно: у owner-draw списка закрытая часть имеет
    // высоту элемента, и без этого она «уезжает» относительно макета.
    SendMessageW(h_role_, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)S(22));
    SendMessageW(h_role_, CB_SETITEMHEIGHT, 0, (LPARAM)S(22));
    SendMessageW(h_device_, CB_SETITEMHEIGHT, (WPARAM)-1, (LPARAM)S(22));
    SendMessageW(h_device_, CB_SETITEMHEIGHT, 0, (LPARAM)S(22));

    SendMessageW(h_role_, CB_ADDSTRING, 0, (LPARAM)L"Отправитель (Central)");
    SendMessageW(h_role_, CB_ADDSTRING, 0, (LPARAM)L"Приёмник (Peripheral)");
    SendMessageW(h_role_, CB_SETCURSEL, 0, 0);
}

void View::ApplyFontsToControls() {
    if (!h_log_) return;
    for (HWND combo : {h_role_, h_device_}) {
        SendMessageW(combo, CB_SETITEMHEIGHT, (WPARAM)-1, S(22));
        SendMessageW(combo, CB_SETITEMHEIGHT, (WPARAM)0, S(22));
    }
    SendMessageW(h_log_, LB_SETITEMHEIGHT, 0, S(22));
}

void View::AddLogLine(const std::wstring& line, int level) {
    if (!h_log_) return;
    const int index = (int)SendMessageW(h_log_, LB_ADDSTRING, 0, (LPARAM)line.c_str());
    if (index < 0) return;
    SendMessageW(h_log_, LB_SETITEMDATA, index, (LPARAM)level);
    SendMessageW(h_log_, LB_SETTOPINDEX, index, 0);
    while (SendMessageW(h_log_, LB_GETCOUNT, 0, 0) > 500) {
        SendMessageW(h_log_, LB_DELETESTRING, 0, 0);
    }
}

int View::MinWidth() const {
    return S(theme::Pad * 2 + 430 + theme::Gap + theme::PanelW) + S(40);
}

int View::MinHeight() const {
    // + высота карточки передачи и запас на заголовок окна и рамку.
    return S(theme::Pad * 2 + theme::HeaderH + theme::Gap + theme::FileH + theme::Gap +
             theme::LogH) +
           S(280);
}

// ---------------------------------------------------------------------------
//  Раскладка
// ---------------------------------------------------------------------------
void View::Layout() {
    if (!created_ || !h_log_) return;

    RECT rc;
    GetClientRect(parent_, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    const int pad = S(theme::Pad), gap = S(theme::Gap);
    const int p = S(theme::InnerPad);

    log_rc_ = {pad, h - pad - S(theme::LogH), w - pad, h - pad};
    file_rc_ = {pad, log_rc_.top - gap - S(theme::FileH), w - pad, log_rc_.top - gap};
    session_rc_ = {w - pad - S(theme::PanelW), S(theme::HeaderH), w - pad, file_rc_.top - gap};
    transfer_rc_ = {pad, S(theme::HeaderH), session_rc_.left - gap, file_rc_.top - gap};

    header_right_ = w - pad;
    title_rc_ = {pad, S(10), w - pad, S(46)};
    subtitle_rc_ = {pad, S(48), w - pad, S(70)};

    // --- карточка «Передача» ----------------------------------------------
    {
        const int left = transfer_rc_.left + p;
        const int right = transfer_rc_.right - p;
        const int top_limit = transfer_rc_.top + S(10);
        const int bottom_limit = transfer_rc_.bottom - S(10);

        transfer_head_ = {left, top_limit, right, top_limit + S(24)};
        int y = top_limit + S(30);

        name_rc_ = {left, y, right, y + S(26)};
        y += S(28);

        show_path_ = (bottom_limit - y) >= S(215);
        if (show_path_) {
            path_rc_ = {left, y, right, y + S(18)};
            y += S(22);
        } else {
            path_rc_ = RECT{0, 0, 0, 0};
        }

        percent_rc_ = {left, y, right, y + S(52)};
        const int chip_w = S(120);
        chip_rc_ = {right - chip_w, y + S(16), right, y + S(42)};
        y += S(56);

        bar_rc_ = {left, y, right, y + S(12)};
        y += S(24);

        const int cell_w = (right - left - 2 * gap) / 3;
        for (int i = 0; i < 3; ++i) {
            const int x = left + i * (cell_w + gap);
            cell_rc_[i] = {x, y, x + cell_w, y + S(36)};
        }
        y += S(38);

        show_crc_ = (bottom_limit - y) >= S(100);
        if (show_crc_) {
            crc_rc_ = {left, y, right, y + S(20)};
            y += S(26);
        } else {
            crc_rc_ = RECT{0, 0, 0, 0};
        }

        graph_head_ = {left, y, right, y + S(18)};
        graph_rc_ = {left, y + S(20), right, bottom_limit};
        show_graph_ = (graph_rc_.bottom - graph_rc_.top) >= S(44);
        if (!show_graph_) {
            graph_head_ = graph_rc_ = RECT{0, 0, 0, 0};
        }

    }

    // --- карточка «Сеанс» --------------------------------------------------
    {
        const int left = session_rc_.left + p;
        const int right = session_rc_.right - p;
        const int field_h = S(26);

        session_head_ = {left, session_rc_.top + S(10), right, session_rc_.top + S(34)};

        int y = session_rc_.top + S(42);
        role_label_ = {left, y, right, y + S(18)};
        y += S(20);
        SetWindowPos(h_role_, nullptr, left, y, right - left, field_h, SWP_NOZORDER);
        y += field_h + S(10);

        device_label_ = {left, y, right, y + S(18)};
        y += S(20);
        const int scan_w = S(82);
        const int combo_w = std::max(S(120), (int)(right - left) - scan_w - S(8));
        SetWindowPos(h_device_, nullptr, left, y, combo_w, S(240), SWP_NOZORDER);
        SetWindowPos(h_scan_, nullptr, right - scan_w, y, scan_w, field_h, SWP_NOZORDER);
        y += field_h + S(12);

        const int btn_w = (right - left - S(10)) / 2;
        SetWindowPos(h_connect_, nullptr, left, y, btn_w, S(32), SWP_NOZORDER);
        SetWindowPos(h_disconnect_, nullptr, left + btn_w + S(10), y,
                     right - (left + btn_w + S(10)), S(32), SWP_NOZORDER);
        y += S(32) + S(10);

        separator_y_ = y;
        info_top_ = y + S(10);

        const int row_h = S(theme::RowH);
        const int bottom_limit = session_rc_.bottom - S(10);

        rows_ = 3;   // Связь / RSSI / MTU — остальное ушло в журнал
        while (rows_ > 0 && bottom_limit - info_top_ < rows_ * row_h) --rows_;
        for (int i = 0; i < 5; ++i) {
            const int y_row = info_top_ + i * row_h;
            info_rc_[i] = {left, y_row, right, y_row + row_h};
        }
    }

    // --- карточка «Файл» ---------------------------------------------------
    {
        const int left = file_rc_.left + p;
        const int right = file_rc_.right - p;

        file_head_ = {left, file_rc_.top + S(8), right, file_rc_.top + S(30)};

        const int y1 = file_rc_.top + S(34);
        const int label_w = S(44);
        const int browse_w = S(84);
        file_label_strip_ = {left, y1, left + label_w, y1 + S(24)};

        const int edit_x = file_label_strip_.right + S(6);
        const int edit_w = std::max(S(160), (int)right - browse_w - S(8) - edit_x);
        file_edit_rc_ = {edit_x, y1, edit_x + edit_w, y1 + S(24)};
        SetWindowPos(h_file_, nullptr, edit_x, y1, edit_w, S(24), SWP_NOZORDER);
        SetWindowPos(h_browse_, nullptr, edit_x + edit_w + S(8), y1,
                     std::max(S(60), (int)right - (edit_x + edit_w + S(8))), S(24), SWP_NOZORDER);

        const int gap2 = S(10);
        const int avail_w = (int)(right - left) - gap2;
        const int w1 = std::max(S(120), avail_w * 34 / 100);

        const int y2 = file_rc_.top + S(66);
        SetWindowPos(h_send_, nullptr, left, y2, w1, S(30), SWP_NOZORDER);
        SetWindowPos(h_cancel_, nullptr, left + w1 + gap2, y2,
                     std::max(S(100), (int)right - (left + w1 + gap2)), S(30), SWP_NOZORDER);

        const int y3 = file_rc_.top + S(104);
        const int half_w = avail_w / 2;
        SetWindowPos(h_open_dir_, nullptr, left, y3, half_w, S(28), SWP_NOZORDER);
        SetWindowPos(h_report_, nullptr, left + half_w + gap2, y3,
                     (int)right - (left + half_w + gap2), S(28), SWP_NOZORDER);
    }

    // --- карточка «Журнал» -------------------------------------------------
    {
        log_head_ = {log_rc_.left + p, log_rc_.top + S(8), log_rc_.right - p,
                     log_rc_.top + S(30)};
        const int top = log_rc_.top + S(38);
        SetWindowPos(h_log_, nullptr, log_rc_.left + p, top, log_rc_.right - log_rc_.left - 2 * p,
                     std::max(S(40), (int)log_rc_.bottom - S(12) - top), SWP_NOZORDER);
    }

    InvalidateRect(parent_, nullptr, FALSE);
}

void View::SetEnabled(const Model& model) {
    const bool sender = model.sender;
    const bool busy = model.phase == PhaseRun;

    // Состояние «включено/выключено» меняется нечасто: считаем его битовой
    // маской и выходим, если ничего не поменялось (не дёргаем перерисовку
    // owner-draw кнопок 7 раз в секунду).
    unsigned int mask = 0;
    auto put = [&mask](bool value) { mask = (mask << 1) | (value ? 1u : 0u); };
    put(!busy);                                              // роль
    put(sender);                                             // поиск/приём
    put(sender && model.has_devices);                        // устройство
    put(sender && model.has_devices && !model.connected);    // подключить
    put(sender && model.connected);                          // отключить
    put(sender);                                             // обзор
    put(sender);                                             // файл
    put(sender && !busy);                                    // отправить
    put(sender && busy);                                     // отменить

    if (mask == enabled_mask_) return;
    enabled_mask_ = mask;

    EnableWindow(h_role_, busy ? FALSE : TRUE);
    EnableWindow(h_scan_, sender ? TRUE : FALSE);
    EnableWindow(h_device_, (sender && model.has_devices) ? TRUE : FALSE);
    EnableWindow(h_connect_, (sender && model.has_devices && !model.connected) ? TRUE : FALSE);
    EnableWindow(h_disconnect_, (sender && model.connected) ? TRUE : FALSE);
    EnableWindow(h_browse_, sender ? TRUE : FALSE);
    EnableWindow(h_file_, sender ? TRUE : FALSE);
    EnableWindow(h_send_, (sender && !busy) ? TRUE : FALSE);
    EnableWindow(h_cancel_, (sender && busy) ? TRUE : FALSE);
    EnableWindow(h_open_dir_, TRUE);
    EnableWindow(h_report_, TRUE);

    // Owner-draw кнопки не перерисовываются сами при смене состояния.
    for (HWND button : {h_scan_, h_connect_, h_disconnect_, h_browse_, h_send_, h_cancel_,
                        h_open_dir_, h_report_}) {
        InvalidateRect(button, nullptr, FALSE);
    }
}

// ---------------------------------------------------------------------------
//  Отрисовка
// ---------------------------------------------------------------------------
void View::PaintBackground(Graphics& g, int w, int h) {
    SolidBrush base(theme::Bg);
    g.FillRectangle(&base, 0, 0, w, h);

    LinearGradientBrush sheen(Point(0, 0), Point(0, h), Color(255, 26, 34, 54),
                              Color(255, 8, 11, 20));
    g.FillRectangle(&sheen, 0, 0, w, h);
}

void View::Paint(HDC dc, const RECT& client, const Model& model) {
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    PaintBackground(g, client.right, client.bottom);
    DrawHeader(g, model);
    DrawTransferCard(g, model);
    DrawSessionCard(g, model);
    DrawFileCard(g, model);
    DrawLogCard(g, model);
    DrawToasts(g);
}

void View::DrawHeader(Graphics& g, const Model& model) {
    g.SetClip(RectF(0.0f, 0.0f, (REAL)std::max<LONG>(1, header_right_ + S(theme::Pad)),
                    (REAL)S(theme::HeaderH)));

    const std::wstring status = model.status.empty() ? L"Не подключено" : model.status;
    const int measured = TextWidth(g, status, f_value_);
    const int max_w = S(390);
    const int status_w = std::clamp(measured + S(30), S(140), max_w);

    // Правый край берём из раскладки: раньше он читался из title_rc_, который
    // эта же функция перезаписывала, и шапка с каждым кадром «уползала» влево.
    const int status_right = header_right_ > 0 ? header_right_
                                               : (int)std::max<LONG>(1, title_rc_.right);
    RECT status_rc{status_right - status_w, S(16), status_right, S(48)};

    const int text_right = std::max(S(theme::Pad) + S(120), (int)status_rc.left - S(20));
    title_rc_ = {S(theme::Pad), S(11), text_right, S(47)};
    subtitle_rc_ = {S(theme::Pad), S(49), text_right, S(71)};

    TextG(g, L"GATT · Передача файлов", title_rc_, f_title_, theme::Txt);
    TextG(g, L"Bluetooth LE · GATT · вариант 8", subtitle_rc_, f_label_, theme::Sub);

    const Color state = StateColor(model.state);
    const int cy = (status_rc.top + status_rc.bottom) / 2;
    Dot(g, status_rc.left + S(6), cy, S(5), state);

    RECT status_text{status_rc.left + S(20), status_rc.top, status_rc.right, status_rc.bottom};
    TextG(g, status, status_text, f_value_, state);

    g.ResetClip();
}

void View::DrawTransferCard(Graphics& g, const Model& model) {
    g.SetClip(RectF((REAL)transfer_rc_.left, (REAL)transfer_rc_.top,
                    (REAL)(transfer_rc_.right - transfer_rc_.left),
                    (REAL)(transfer_rc_.bottom - transfer_rc_.top)));

    Card(g, transfer_rc_);
    TextG(g, model.sender ? L"Передача" : L"Приём", transfer_head_, f_section_, theme::Txt);

    // Имя файла и путь.
    if (model.file_name.empty()) {
        TextG(g, L"Файл не выбран", name_rc_, f_section_, theme::Dim);
    } else {
        TextG(g, model.file_name, name_rc_, f_section_, theme::Txt);
    }
    if (show_path_ && !model.file_path.empty()) {
        TextG(g, model.file_path, path_rc_, f_small_, theme::Dim);
    }

    // Процент и «чип» фазы.
    {
        wchar_t buffer[16];
        _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"%d %%",
                     (int)std::lround(std::clamp(model.progress, 0.0, 1.0) * 100.0));
        TextG(g, buffer, percent_rc_, f_big_, theme::Txt);
    }

    const Color phase_color = PhaseColor(model.phase);
    RoundedRect(g, chip_rc_, Color(48, phase_color.GetR(), phase_color.GetG(), phase_color.GetB()),
                Color(200, phase_color.GetR(), phase_color.GetG(), phase_color.GetB()),
                S(theme::Radius * 4) / 8.0f);
    TextG(g, PhaseText(model.phase), chip_rc_, f_button_, phase_color, StringAlignmentCenter);

    ProgressBar(g, bar_rc_, model.progress, phase_color);

    // Три показателя: передано / скорость / осталось.
    {
        const wchar_t* labels[3] = {L"Передано", L"Скорость", L"Осталось"};
        const std::wstring values[3] = {model.transferred, model.speed, model.eta};
        for (int i = 0; i < 3; ++i) {
            RECT rc = cell_rc_[i];
            if (rc.right <= rc.left) continue;
            RECT label_rc{rc.left, rc.top, rc.right, rc.top + S(16)};
            RECT value_rc{rc.left, rc.top + S(16), rc.right, rc.bottom};
            TextG(g, labels[i], label_rc, f_small_, theme::Sub);
            TextG(g, values[i].empty() ? L"—" : values[i], value_rc, f_value_,
                  i == 1 ? model.state == StateBusy ? theme::Accent : theme::Txt : theme::Txt);
        }
    }

    // CRC32: ожидаемая сумма и сумма приёмника.
    if (show_crc_) {
        Color color = theme::Dim;
        if (model.crc_state == 1) color = theme::Good;
        else if (model.crc_state == 2) color = theme::Bad;
        const std::wstring text = model.crc.empty() ? L"CRC32 — ещё не проверялся"
                                                    : (L"CRC32  " + model.crc);
        TextG(g, text, crc_rc_, f_label_, color);
    }

    // График скорости.
    if (show_graph_) {
        TextG(g, L"Скорость во времени", graph_head_, f_small_, theme::Sub);
        SpeedGraph(g, graph_rc_, model.speed_hist, f_small_);
    }

    g.ResetClip();
}

void View::DrawSessionCard(Graphics& g, const Model& model) {
    g.SetClip(RectF((REAL)session_rc_.left, (REAL)session_rc_.top,
                    (REAL)(session_rc_.right - session_rc_.left),
                    (REAL)(session_rc_.bottom - session_rc_.top)));

    Card(g, session_rc_);
    TextG(g, L"Сеанс", session_head_, f_section_, theme::Txt);

    TextG(g, L"Роль", role_label_, f_label_, theme::Sub);
    TextG(g, L"Устройство", device_label_, f_label_, theme::Sub);

    Separator(g, session_rc_, separator_y_);

    const std::wstring values[3] = {model.device, model.rssi, model.mtu};
    const wchar_t* labels[3] = {L"Связь:", L"RSSI:", L"MTU:"};
    for (int i = 0; i < rows_ && i < 3; ++i) {
        Row(g, session_rc_, info_rc_[i].top, labels[i], values[i], f_label_, f_value_);
    }

    g.ResetClip();
}

void View::DrawFileCard(Graphics& g, const Model& model) {
    g.SetClip(RectF((REAL)file_rc_.left, (REAL)file_rc_.top,
                    (REAL)(file_rc_.right - file_rc_.left),
                    (REAL)(file_rc_.bottom - file_rc_.top)));

    Card(g, file_rc_);
    TextG(g, model.sender ? L"Файл для отправки" : L"Каталог приёма", file_head_, f_section_,
          theme::Txt);
    TextG(g, L"Файл", file_label_strip_, f_label_, theme::Sub);

    // Рамка вокруг EDIT: у стандартного поля нет своего оформления.
    RECT field = file_edit_rc_;
    field.left -= 1;
    field.top -= 1;
    field.right += 1;
    field.bottom += 1;
    RoundedRect(g, field, Color(255, 12, 18, 30), theme::Line, S(theme::Radius * 4) / 4.0f);

    g.ResetClip();
}

void View::DrawLogCard(Graphics& g, const Model& model) {
    g.SetClip(RectF((REAL)log_rc_.left, (REAL)log_rc_.top, (REAL)(log_rc_.right - log_rc_.left),
                    (REAL)(log_rc_.bottom - log_rc_.top)));

    Card(g, log_rc_);
    TextG(g, L"Журнал событий", log_head_, f_section_, theme::Txt);
    (void)model;

    g.ResetClip();
}

// ---------------------------------------------------------------------------
//  Уведомления
// ---------------------------------------------------------------------------
void View::PushToast(const std::wstring& text, int level) {
    if (text.empty()) return;
    if (toasts_.size() >= 3) toasts_.erase(toasts_.begin());
    toasts_.push_back({text, level, GetTickCount64() + 2600});
}

bool View::ExpireToasts() {
    const ULONGLONG now = GetTickCount64();
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                                 [now](const Toast& t) { return t.until <= now; }),
                  toasts_.end());
    return !toasts_.empty();
}

void View::DrawToasts(Graphics& g) {
    if (toasts_.empty()) return;

    const ULONGLONG now = GetTickCount64();
    g.SetClip(RectF((REAL)transfer_rc_.left, (REAL)transfer_rc_.top,
                    (REAL)(transfer_rc_.right - transfer_rc_.left),
                    (REAL)(transfer_rc_.bottom - transfer_rc_.top)));

    const int right = transfer_rc_.right - S(12);
    int y = transfer_rc_.top + S(12);

    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        const ULONGLONG left = it->until > now ? it->until - now : 0;
        const int alpha = left < 400 ? (int)(255 * left / 400) : 255;
        if (alpha <= 4) break;

        Color accent = theme::Line;
        if (it->level == 1) accent = theme::Warn;
        else if (it->level == 2) accent = theme::Bad;
        else if (it->level == 3) accent = theme::Good;

        const int w = std::min(S(430), TextWidth(g, it->text, f_value_) + S(40));
        const int h = S(34);
        RECT rc{right - w, y, right, y + h};

        RoundedRect(g, rc, Color((BYTE)(235 * alpha / 255), 18, 26, 44),
                    Color((BYTE)alpha, accent.GetR(), accent.GetG(), accent.GetB()),
                    S(theme::Radius * 4) / 4.0f);

        RECT bar{rc.left + S(8), rc.top + S(8), rc.left + S(12), rc.bottom - S(8)};
        FillRectG(g, bar, Color((BYTE)alpha, accent.GetR(), accent.GetG(), accent.GetB()));

        RECT text_rc{rc.left + S(20), rc.top, rc.right - S(10), rc.bottom};
        TextG(g, it->text, text_rc, f_value_, Color((BYTE)alpha, 235, 240, 248));

        y += h + S(6);
        if (y > transfer_rc_.bottom - h) break;
    }

    g.ResetClip();
}

// ---------------------------------------------------------------------------
//  Owner-draw
// ---------------------------------------------------------------------------
bool View::DrawItem(const DRAWITEMSTRUCT* item) {
    if (!item) return false;
    switch (item->CtlType) {
        case ODT_BUTTON:   DrawButton(item); return true;
        case ODT_COMBOBOX: DrawCombo(item); return true;
        case ODT_LISTBOX:  DrawLogItem(item); return true;
        default:           return false;
    }
}

void View::DrawButton(const DRAWITEMSTRUCT* item) {
    Graphics g(item->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;

    Color fill = theme::Btn, border = theme::Line, text = theme::Txt;
    if (disabled) {
        fill = Color(255, 26, 33, 48);
        border = Color(255, 62, 76, 102);
        text = Color(255, 158, 170, 190);
    } else if (item->CtlID == ID_CONNECT || item->CtlID == ID_SEND) {
        fill = pressed ? Color(255, 30, 130, 90) : Color(255, 24, 110, 74);
        border = Color(255, 60, 200, 140);
        text = Color(255, 235, 255, 245);
    } else if (item->CtlID == ID_CANCEL) {
        fill = pressed ? Color(255, 130, 42, 42) : Color(255, 96, 32, 32);
        border = Color(255, 200, 84, 84);
        text = Color(255, 255, 235, 235);
    } else if (pressed) {
        fill = theme::BtnHot;
    }

    RECT rc = item->rcItem;
    RoundedRect(g, rc, fill, border, S(theme::Radius * 4) / 4.0f);

    wchar_t buffer[128] = {};
    GetWindowTextW(item->hwndItem, buffer, 128);
    TextG(g, buffer, rc, f_button_, text, StringAlignmentCenter);
}

void View::DrawCombo(const DRAWITEMSTRUCT* item) {
    Graphics g(item->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    const bool disabled = (item->itemState & ODS_DISABLED) != 0;

    if (item->itemID == (UINT)-1) {
        RoundedRect(g, item->rcItem, theme::Field, theme::Line, S(theme::Radius * 4) / 4.0f);
        const int sel = (int)SendMessageW(item->hwndItem, CB_GETCURSEL, 0, 0);
        std::wstring text;
        if (sel >= 0) {
            wchar_t buffer[256] = {};
            if (SendMessageW(item->hwndItem, CB_GETLBTEXT, sel, (LPARAM)buffer) != CB_ERR) {
                text = buffer;
            }
        }
        RECT text_rc = item->rcItem;
        text_rc.left += S(8);
        text_rc.right -= S(22);
        TextG(g, text, text_rc, f_label_,
              disabled ? theme::Dim : (text.empty() ? theme::Dim : theme::Txt));

        const int cx = item->rcItem.right - S(13);
        const int cy = (item->rcItem.top + item->rcItem.bottom) / 2;
        Pen pen(disabled ? theme::Dim : theme::Sub, 1.4f);
        g.DrawLine(&pen, cx - S(4), cy - S(2), cx, cy + S(2));
        g.DrawLine(&pen, cx, cy + S(2), cx + S(4), cy - S(2));
        return;
    }

    wchar_t buffer[256] = {};
    SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, (LPARAM)buffer);
    const bool selected = (item->itemState & ODS_SELECTED) != 0;
    FillRectG(g, item->rcItem, selected ? theme::BtnHot : theme::Field);
    RECT text_rc = item->rcItem;
    text_rc.left += S(8);
    TextG(g, buffer, text_rc, f_label_, selected ? theme::Txt : theme::Sub);
}

void View::DrawLogItem(const DRAWITEMSTRUCT* item) {
    Graphics g(item->hDC);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    wchar_t buffer[1024] = {};
    SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, (LPARAM)buffer);
    const std::wstring line(buffer);

    FillRectG(g, item->rcItem, Color(255, 14, 20, 34));

    const size_t time_len = 23;   // «29.10.2026 22:15:03.120»
    const std::wstring stamp = line.substr(0, std::min(time_len, line.size()));

    RECT rc = item->rcItem;
    rc.left += S(8);
    RECT time_rc = rc;
    time_rc.right = std::min(rc.right, rc.left + TextWidth(g, stamp + L"  ", f_log_));
    TextG(g, stamp, time_rc, f_log_, theme::Dim);

    RECT text_rc = rc;
    text_rc.left = time_rc.right;
    const int level = (int)SendMessageW(item->hwndItem, LB_GETITEMDATA, item->itemID, 0);
    TextG(g, line.size() > time_len ? line.substr(time_len) : L"", text_rc, f_log_,
          LevelColor(level));
}

}  // namespace visual
