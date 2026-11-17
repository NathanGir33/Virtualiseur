#include "Ui.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shlwapi.h>

#include <algorithm>
#include <map>
#include <vector>

#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "UxTheme.lib")
#pragma comment(lib, "Shlwapi.lib")

namespace Gdiplus {
using std::max;
using std::min;
} // namespace Gdiplus
#include <gdiplus.h>

#pragma comment(lib, "Gdiplus.lib")

namespace ui {
namespace {

ULONG_PTR g_gdiplusToken = 0;
UINT g_dpi = 96;
Fonts g_fonts;
std::map<COLORREF, HBRUSH> g_brushes;

struct FlatButtonState {
    wchar_t glyph = 0;
    std::wstring label;
    COLORREF glyphColor = CLR_INVALID;
    bool hover = false;
};
std::map<HWND, FlatButtonState> g_flatButtons;

Gdiplus::Color ToColor(COLORREF color)
{
    return Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color));
}

void AddRoundedPath(Gdiplus::GraphicsPath& path, float x, float y, float width, float height, float radius)
{
    const float diameter = (std::min)({radius * 2.0f, width, height});
    if (diameter <= 0.5f) {
        path.AddRectangle(Gdiplus::RectF(x, y, width, height));
        return;
    }
    path.AddArc(x, y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(x + width - diameter, y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(x + width - diameter, y + height - diameter, diameter, diameter, 0.0f, 90.0f);
    path.AddArc(x, y + height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

const wchar_t* GlyphFace()
{
    // Windows 11 ships Segoe Fluent Icons; Windows 10 has Segoe MDL2 Assets
    // (same code points).
    static const wchar_t* face = [] {
        LOGFONTW query{};
        query.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(query.lfFaceName, L"Segoe Fluent Icons");
        bool found = false;
        HDC dc = GetDC(nullptr);
        EnumFontFamiliesExW(dc, &query, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) -> int {
            *reinterpret_cast<bool*>(found) = true;
            return 0;
        }, reinterpret_cast<LPARAM>(&found), 0);
        ReleaseDC(nullptr, dc);
        return found ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }();
    return face;
}

HFONT MakeFont(const LOGFONTW& base, int heightScale100, int weight, const wchar_t* face = nullptr)
{
    LOGFONTW font = base;
    font.lfHeight = MulDiv(base.lfHeight, heightScale100, 100);
    font.lfWeight = weight;
    font.lfQuality = CLEARTYPE_QUALITY;
    if (face != nullptr) {
        wcscpy_s(font.lfFaceName, face);
        font.lfCharSet = DEFAULT_CHARSET;
    }
    return CreateFontIndirectW(&font);
}

void DeleteFonts()
{
    for (HFONT* font : {&g_fonts.body, &g_fonts.bold, &g_fonts.caption, &g_fonts.heading, &g_fonts.title,
                        &g_fonts.glyph, &g_fonts.glyphLarge, &g_fonts.badge}) {
        if (*font != nullptr) DeleteObject(*font);
        *font = nullptr;
    }
}

LRESULT CALLBACK FlatButtonProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    switch (message) {
    case WM_MOUSEMOVE: {
        auto& state = g_flatButtons[window];
        if (!state.hover) {
            state.hover = true;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            InvalidateRect(window, nullptr, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE:
        g_flatButtons[window].hover = false;
        InvalidateRect(window, nullptr, FALSE);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        g_flatButtons.erase(window);
        RemoveWindowSubclass(window, &FlatButtonProc, 1);
        break;
    default:
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace

void StartGraphics()
{
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr);
}

void StopGraphics()
{
    DeleteFonts();
    for (auto& [color, brush] : g_brushes) DeleteObject(brush);
    g_brushes.clear();
    if (g_gdiplusToken != 0) Gdiplus::GdiplusShutdown(g_gdiplusToken);
    g_gdiplusToken = 0;
}

void InitTheme(UINT dpi)
{
    g_dpi = dpi;
    DeleteFonts();
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
    LOGFONTW base = metrics.lfMessageFont;
    // Segoe UI Variable (Windows 11) is the system UI face; Segoe UI otherwise.
    LOGFONTW query{};
    query.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(query.lfFaceName, L"Segoe UI Variable Text");
    bool variable = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &query, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) -> int {
        *reinterpret_cast<bool*>(found) = true;
        return 0;
    }, reinterpret_cast<LPARAM>(&variable), 0);
    ReleaseDC(nullptr, dc);
    if (variable) wcscpy_s(base.lfFaceName, L"Segoe UI Variable Text");

    g_fonts.body = MakeFont(base, 100, FW_NORMAL);
    g_fonts.bold = MakeFont(base, 100, FW_SEMIBOLD);
    g_fonts.caption = MakeFont(base, 88, FW_NORMAL);
    g_fonts.heading = MakeFont(base, 115, FW_SEMIBOLD, variable ? L"Segoe UI Variable Display" : nullptr);
    g_fonts.title = MakeFont(base, 165, FW_SEMIBOLD, variable ? L"Segoe UI Variable Display" : nullptr);
    g_fonts.glyph = MakeFont(base, 125, FW_NORMAL, GlyphFace());
    g_fonts.glyphLarge = MakeFont(base, 175, FW_NORMAL, GlyphFace());
    g_fonts.badge = MakeFont(base, 120, FW_BOLD, variable ? L"Segoe UI Variable Display" : nullptr);
}

UINT Dpi() { return g_dpi; }
int Scale(int value) { return MulDiv(value, static_cast<int>(g_dpi), 96); }
const Fonts& Font() { return g_fonts; }

HBRUSH Brush(COLORREF color)
{
    auto& brush = g_brushes[color];
    if (brush == nullptr) brush = CreateSolidBrush(color);
    return brush;
}

void Fill(HDC dc, const RECT& rect, COLORREF color) { FillRect(dc, &rect, Brush(color)); }

void RoundedRect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border)
{
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const auto x = static_cast<float>(rect.left);
    const auto y = static_cast<float>(rect.top);
    const auto width = static_cast<float>(rect.right - rect.left);
    const auto height = static_cast<float>(rect.bottom - rect.top);
    if (fill != CLR_INVALID) {
        Gdiplus::GraphicsPath path;
        AddRoundedPath(path, x, y, width, height, static_cast<float>(radius));
        Gdiplus::SolidBrush brush(ToColor(fill));
        graphics.FillPath(&brush, &path);
    }
    if (border != CLR_INVALID) {
        Gdiplus::GraphicsPath path;
        AddRoundedPath(path, x + 0.5f, y + 0.5f, width - 1.0f, height - 1.0f, static_cast<float>(radius));
        Gdiplus::Pen pen(ToColor(border), 1.0f);
        graphics.DrawPath(&pen, &path);
    }
}

void Circle(HDC dc, const RECT& rect, COLORREF fill)
{
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::SolidBrush brush(ToColor(fill));
    graphics.FillEllipse(&brush, static_cast<float>(rect.left), static_cast<float>(rect.top),
                         static_cast<float>(rect.right - rect.left), static_cast<float>(rect.bottom - rect.top));
}

void Line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color)
{
    if (y1 == y2) {
        Fill(dc, RECT{x1, y1, x2, y1 + 1}, color);
    } else {
        Fill(dc, RECT{x1, y1, x1 + 1, y2}, color);
    }
    (void)x2;
}

void Text(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT format)
{
    const auto previousFont = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
    SelectObject(dc, previousFont);
}

int TextWidth(HDC dc, const std::wstring& text, HFONT font)
{
    const auto previousFont = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, previousFont);
    return size.cx;
}

void MakeFlatButton(HWND button, wchar_t glyph, const std::wstring& label, COLORREF glyphColor)
{
    g_flatButtons[button] = FlatButtonState{glyph, label, glyphColor, false};
    SetWindowTextW(button, label.c_str()); // Accessibility and mnemonics.
    SetWindowSubclass(button, &FlatButtonProc, 1, 0);
}

void SetFlatButton(HWND button, wchar_t glyph, const std::wstring& label, COLORREF glyphColor)
{
    auto& state = g_flatButtons[button];
    if (state.glyph == glyph && state.label == label && state.glyphColor == glyphColor) return;
    state.glyph = glyph;
    state.label = label;
    state.glyphColor = glyphColor;
    SetWindowTextW(button, label.c_str());
    InvalidateRect(button, nullptr, FALSE);
}

void SetFlatButtonColor(HWND button, COLORREF glyphColor)
{
    g_flatButtons[button].glyphColor = glyphColor;
    InvalidateRect(button, nullptr, FALSE);
}

bool DrawFlatButton(const DRAWITEMSTRUCT& item, COLORREF background)
{
    const auto found = g_flatButtons.find(item.hwndItem);
    if (found == g_flatButtons.end()) return false;
    const auto& state = found->second;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    BufferedPaint buffer(item.hDC, item.rcItem);
    const HDC dc = buffer.Dc();
    Fill(dc, item.rcItem, background);
    if (!disabled && (pressed || state.hover)) {
        RECT box = item.rcItem;
        InflateRect(&box, -Scale(2), -Scale(2));
        RoundedRect(dc, box, Scale(6), pressed ? color::kPressed : color::kHover);
    }
    const COLORREF glyphColor = disabled ? color::kDisabled
                              : state.glyphColor != CLR_INVALID ? state.glyphColor : color::kText;
    RECT glyphRect = item.rcItem;
    glyphRect.top += Scale(7);
    glyphRect.bottom = glyphRect.top + Scale(26);
    Text(dc, std::wstring(1, state.glyph), glyphRect, Font().glyph, glyphColor, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT labelRect = item.rcItem;
    labelRect.top = glyphRect.bottom + Scale(1);
    labelRect.left += Scale(2);
    labelRect.right -= Scale(2);
    Text(dc, state.label, labelRect, Font().caption, disabled ? color::kDisabled : color::kText,
         DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if ((item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -Scale(2), -Scale(2));
        RoundedRect(dc, focus, Scale(6), CLR_INVALID, color::kAccent);
    }
    return true;
}

// --- Theme ---------------------------------------------------------------------------------------

namespace {

Theme g_theme = Theme::System;
bool g_dark = false;

bool WindowsUsesDarkApps()
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && value == 0;
}

// uxtheme exports, by ordinal only (Windows 10 1903+): dark popup menus.
void SetMenuMode(bool dark)
{
    using SetPreferredAppMode = int(WINAPI*)(int);
    using FlushMenuThemes = void(WINAPI*)();
    static const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (uxtheme == nullptr) return;
    const auto setMode = reinterpret_cast<SetPreferredAppMode>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    const auto flush = reinterpret_cast<FlushMenuThemes>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
    if (setMode != nullptr) setMode(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (flush != nullptr) flush();
}

} // namespace

void SetTheme(Theme theme)
{
    g_theme = theme;
    g_dark = theme == Theme::Dark || (theme == Theme::System && WindowsUsesDarkApps());
    using namespace color;
    if (g_dark) {
        // Windows 11 dark: layered greys, lighter accents for contrast.
        kWindow = RGB(32, 32, 32);
        kPanel = RGB(43, 43, 43);
        kBorder = RGB(62, 62, 62);
        kText = RGB(242, 242, 242);
        kSecondary = RGB(170, 170, 170);
        kAccent = RGB(96, 205, 255);
        kAccentSoft = RGB(30, 58, 80);
        kHover = RGB(56, 56, 56);
        kPressed = RGB(66, 66, 66);
        kGreen = RGB(108, 203, 95);
        kOrange = RGB(252, 180, 70);
        kRed = RGB(255, 110, 100);
        kDisabled = RGB(105, 105, 105);
        kPurple = RGB(214, 153, 245);
        kTeal = RGB(72, 210, 200);
        kPink = RGB(255, 128, 200);
        kAmber = RGB(255, 200, 90);
        kIndigo = RGB(160, 160, 255);
        kSlate = RGB(165, 175, 190);
    } else {
        kWindow = RGB(243, 243, 243);
        kPanel = RGB(255, 255, 255);
        kBorder = RGB(224, 224, 224);
        kText = RGB(26, 26, 26);
        kSecondary = RGB(96, 96, 96);
        kAccent = RGB(0, 103, 192);
        kAccentSoft = RGB(230, 241, 251);
        kHover = RGB(235, 235, 235);
        kPressed = RGB(222, 222, 222);
        kGreen = RGB(16, 124, 16);
        kOrange = RGB(202, 120, 0);
        kRed = RGB(196, 43, 28);
        kDisabled = RGB(165, 165, 165);
        kPurple = RGB(136, 23, 152);
        kTeal = RGB(0, 120, 120);
        kPink = RGB(191, 0, 119);
        kAmber = RGB(157, 93, 0);
        kIndigo = RGB(79, 70, 229);
        kSlate = RGB(90, 98, 112);
    }
    SetMenuMode(g_dark);
}

Theme CurrentTheme() { return g_theme; }
bool IsDark() { return g_dark; }

void ThemeWindow(HWND window)
{
    const BOOL dark = g_dark ? TRUE : FALSE;
    constexpr DWORD kImmersiveDarkMode = 20; // DWMWA_USE_IMMERSIVE_DARK_MODE.
    DwmSetWindowAttribute(window, kImmersiveDarkMode, &dark, sizeof(dark));
    SetWindowTheme(window, g_dark ? L"DarkMode_Explorer" : nullptr, nullptr);
}

namespace {

// The 3D client edge of edits and lists is drawn with system (light) colors:
// in the dark theme it is repainted with the theme's border color.
// The status bar's size grip keeps the system (light) colors whatever the
// theme: once the bar is painted, the grip square is painted again with the
// theme's background and a triangle of dots (the grip still works: the
// status bar answers WM_NCHITTEST there).
LRESULT CALLBACK SizeGripProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_PAINT) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        RECT client{};
        GetClientRect(window, &client);
        const int size = client.bottom - client.top;
        if ((GetWindowLongW(window, GWL_STYLE) & SBARS_SIZEGRIP) != 0 && size > 0) {
            const HDC dc = GetDC(window);
            const RECT grip{client.right - size, client.top, client.right, client.bottom};
            Fill(dc, grip, color::kWindow);
            const int dot = (std::max)(2, Scale(2));
            const int step = dot + Scale(2);
            const int right = client.right - Scale(3);
            const int bottom = client.bottom - Scale(3);
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column <= row; ++column) {
                    const int x = right - (column + 1) * step + Scale(2);
                    const int y = bottom - (2 - row + 1) * step + Scale(2);
                    Fill(dc, RECT{x, y, x + dot, y + dot}, color::kDisabled);
                }
            }
            ReleaseDC(window, dc);
        }
        return result;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, &SizeGripProc, 3);
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK ClientEdgeProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR)
{
    if (message == WM_NCPAINT) {
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (g_dark && (GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) != 0) {
            const HDC dc = GetWindowDC(window);
            RECT rect{};
            GetWindowRect(window, &rect);
            OffsetRect(&rect, -rect.left, -rect.top);
            FrameRect(dc, &rect, Brush(color::kBorder));
            InflateRect(&rect, -1, -1);
            FrameRect(dc, &rect, Brush(color::kPanel));
            ReleaseDC(window, dc);
        }
        return result;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, &ClientEdgeProc, 2);
    return DefSubclassProc(window, message, wParam, lParam);
}

} // namespace

void ThemeControl(HWND control)
{
    wchar_t name[32] = {};
    GetClassNameW(control, name, 32);
    const std::wstring kind = name;
    if (kind == L"Edit" || kind == WC_COMBOBOXW) {
        SetWindowTheme(control, g_dark ? L"DarkMode_CFD" : nullptr, nullptr);
    } else if (kind == L"ListBox" || kind == L"Button" || kind == L"ScrollBar") {
        SetWindowTheme(control, g_dark ? L"DarkMode_Explorer" : nullptr, nullptr);
    } else if (kind == STATUSCLASSNAMEW) {
        // Unthemed so that SB_SETBKCOLOR applies; its text is owner-drawn.
        SetWindowTheme(control, L"", L"");
        SendMessageW(control, SB_SETBKCOLOR, 0, static_cast<LPARAM>(color::kWindow));
        SetWindowSubclass(control, &SizeGripProc, 3, 0);
    }
    if (kind == L"Edit" || kind == L"ListBox") SetWindowSubclass(control, &ClientEdgeProc, 2, 0); // Dark client edge.
    SetWindowPos(control, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(control, nullptr, TRUE);
}

COLORREF Mix(COLORREF a, COLORREF b, int percent)
{
    const auto blend = [percent](int x, int y) { return (x * (100 - percent) + y * percent) / 100; };
    return RGB(blend(GetRValue(a), GetRValue(b)), blend(GetGValue(a), GetGValue(b)), blend(GetBValue(a), GetBValue(b)));
}

LRESULT DrawCheckBox(const NMCUSTOMDRAW& draw, COLORREF background)
{
    if (draw.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    const HWND button = draw.hdr.hwndFrom;
    const bool checked = SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool enabled = IsWindowEnabled(button) != FALSE;
    const bool hot = (draw.uItemState & CDIS_HOT) != 0;
    const bool focused = (draw.uItemState & CDIS_FOCUS) != 0 && (SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0;
    const HDC dc = draw.hdc;
    const RECT client = draw.rc;
    Fill(dc, client, background);
    const int size = Scale(18);
    const int top = (client.top + client.bottom - size) / 2;
    RECT box{client.left + Scale(1), top, client.left + Scale(1) + size, top + size};
    if (checked) {
        RoundedRect(dc, box, Scale(4), enabled ? (hot ? Mix(color::kAccent, color::kText, 15) : color::kAccent) : color::kDisabled);
        Text(dc, std::wstring(1, glyph::kCheck), box, Font().caption, g_dark ? RGB(0, 0, 0) : RGB(255, 255, 255),
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        RoundedRect(dc, box, Scale(4), hot && enabled ? color::kHover : background,
                    enabled ? color::kSecondary : color::kDisabled);
    }
    wchar_t label[256] = {};
    GetWindowTextW(button, label, 256);
    RECT text{box.right + Scale(8), client.top, client.right, client.bottom};
    const auto font = reinterpret_cast<HFONT>(SendMessageW(button, WM_GETFONT, 0, 0));
    Text(dc, label, text, font != nullptr ? font : Font().body, enabled ? color::kText : color::kDisabled,
         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_HIDEPREFIX);
    if (focused) RoundedRect(dc, RECT{box.left - Scale(1), box.top - Scale(1), box.right + Scale(1), box.bottom + Scale(1)},
                             Scale(5), CLR_INVALID, color::kText);
    return CDRF_SKIPDEFAULT;
}

namespace {

// Undocumented menu-bar drawing messages (as used by Windows itself).
constexpr UINT kUahDrawMenu = 0x0091;
constexpr UINT kUahDrawMenuItem = 0x0092;
struct UahMenu {
    HMENU menu;
    HDC dc;
    DWORD flags;
};
struct UahMenuItem {
    int position;
    DWORD metrics[8];
    DWORD popupMetrics[5];
};
struct UahDrawMenuItem {
    DRAWITEMSTRUCT draw;
    UahMenu menu;
    UahMenuItem item;
};

RECT MenuBarRect(HWND window)
{
    MENUBARINFO info{sizeof(info)};
    GetMenuBarInfo(window, OBJID_MENU, 0, &info);
    RECT bounds{};
    GetWindowRect(window, &bounds);
    RECT bar = info.rcBar;
    OffsetRect(&bar, -bounds.left, -bounds.top);
    return bar;
}

} // namespace

bool HandleMenuBar(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result)
{
    if (!g_dark) return false;
    switch (message) {
    case kUahDrawMenu: {
        const auto* menu = reinterpret_cast<const UahMenu*>(lParam);
        RECT bar = MenuBarRect(window);
        bar.top -= 1; // Covers the light line Windows leaves above the bar.
        Fill(menu->dc, bar, color::kPanel);
        result = 0;
        return true;
    }
    case kUahDrawMenuItem: {
        const auto* item = reinterpret_cast<const UahDrawMenuItem*>(lParam);
        wchar_t text[128] = {};
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_STRING;
        info.dwTypeData = text;
        info.cch = 127;
        GetMenuItemInfoW(item->menu.menu, static_cast<UINT>(item->item.position), TRUE, &info);
        const UINT state = item->draw.itemState;
        const bool active = (state & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
        Fill(item->draw.hDC, item->draw.rcItem, active ? color::kHover : color::kPanel);
        RECT rect = item->draw.rcItem;
        Text(item->draw.hDC, text, rect, Font().body,
             (state & (ODS_INACTIVE | ODS_DISABLED | ODS_GRAYED)) != 0 ? color::kDisabled : color::kText,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | ((state & ODS_NOACCEL) != 0 ? DT_HIDEPREFIX : 0));
        result = 0;
        return true;
    }
    case WM_NCPAINT:
    case WM_NCACTIVATE: {
        // Windows paints a light line under the menu bar: paint over it.
        result = DefWindowProcW(window, message, wParam, lParam);
        const HDC dc = GetWindowDC(window);
        const RECT bar = MenuBarRect(window);
        Fill(dc, RECT{bar.left, bar.bottom, bar.right, bar.bottom + 1}, color::kPanel);
        ReleaseDC(window, dc);
        return true;
    }
    default:
        return false;
    }
}

std::unique_ptr<Picture> Picture::Load(const std::wstring& path)
{
    // Share delete: the writer can rename a new file over this one meanwhile.
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    LARGE_INTEGER size{};
    std::vector<BYTE> bytes;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20)) {
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size()) bytes.clear();
    }
    CloseHandle(file);
    if (bytes.empty()) return nullptr;
    IStream* stream = SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size()));
    if (stream == nullptr) return nullptr;
    auto* bitmap = Gdiplus::Bitmap::FromStream(stream);
    if (bitmap == nullptr || bitmap->GetLastStatus() != Gdiplus::Ok) {
        delete bitmap;
        stream->Release();
        return nullptr;
    }
    std::unique_ptr<Picture> picture(new Picture());
    picture->bitmap_ = bitmap;
    picture->stream_ = stream;
    return picture;
}

Picture::~Picture()
{
    delete static_cast<Gdiplus::Bitmap*>(bitmap_);
    if (stream_ != nullptr) static_cast<IStream*>(stream_)->Release();
}

int Picture::Width() const { return static_cast<int>(static_cast<Gdiplus::Bitmap*>(bitmap_)->GetWidth()); }
int Picture::Height() const { return static_cast<int>(static_cast<Gdiplus::Bitmap*>(bitmap_)->GetHeight()); }

void Picture::Draw(HDC dc, const RECT& rect) const
{
    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.DrawImage(static_cast<Gdiplus::Bitmap*>(bitmap_), static_cast<INT>(rect.left), static_cast<INT>(rect.top),
                       static_cast<INT>(rect.right - rect.left), static_cast<INT>(rect.bottom - rect.top));
}

BufferedPaint::BufferedPaint(HDC target, const RECT& rect) : target_(target), rect_(rect)
{
    // The bitmap starts at the origin so that GDI and GDI+ share the same
    // coordinates (GDI+ does not follow DC viewport offsets reliably).
    memory_ = CreateCompatibleDC(target);
    bitmap_ = CreateCompatibleBitmap(target, (std::max)(1L, rect.right), (std::max)(1L, rect.bottom));
    previous_ = SelectObject(memory_, bitmap_);
}

BufferedPaint::~BufferedPaint()
{
    BitBlt(target_, rect_.left, rect_.top, rect_.right - rect_.left, rect_.bottom - rect_.top, memory_, rect_.left,
           rect_.top, SRCCOPY);
    SelectObject(memory_, previous_);
    DeleteObject(bitmap_);
    DeleteDC(memory_);
}

} // namespace ui
