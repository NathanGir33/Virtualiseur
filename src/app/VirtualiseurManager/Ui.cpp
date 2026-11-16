#include "Ui.h"

#include <commctrl.h>

#include <algorithm>
#include <map>

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
