#pragma once

// Look and feel of the manager: Windows 11 style light theme, DPI-aware
// fonts, anti-aliased shapes (GDI+) and flat owner-drawn toolbar buttons.

#include <Windows.h>

#include <string>

namespace ui {

namespace color {
inline constexpr COLORREF kWindow = RGB(243, 243, 243);
inline constexpr COLORREF kPanel = RGB(255, 255, 255);
inline constexpr COLORREF kBorder = RGB(224, 224, 224);
inline constexpr COLORREF kText = RGB(26, 26, 26);
inline constexpr COLORREF kSecondary = RGB(96, 96, 96);
inline constexpr COLORREF kAccent = RGB(0, 103, 192);
inline constexpr COLORREF kAccentSoft = RGB(230, 241, 251);
inline constexpr COLORREF kHover = RGB(235, 235, 235);
inline constexpr COLORREF kPressed = RGB(222, 222, 222);
inline constexpr COLORREF kGreen = RGB(16, 124, 16);
inline constexpr COLORREF kOrange = RGB(202, 120, 0);
inline constexpr COLORREF kRed = RGB(196, 43, 28);
inline constexpr COLORREF kDisabled = RGB(165, 165, 165);
} // namespace color

// Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10) glyphs.
namespace glyph {
inline constexpr wchar_t kAdd = 0xE710;
inline constexpr wchar_t kSettings = 0xE713;
inline constexpr wchar_t kDelete = 0xE74D;
inline constexpr wchar_t kPlay = 0xE768;
inline constexpr wchar_t kPause = 0xE769;
inline constexpr wchar_t kPower = 0xE7E8;
inline constexpr wchar_t kStop = 0xE71A;
inline constexpr wchar_t kFolder = 0xE838;
inline constexpr wchar_t kDocument = 0xE8A5;
inline constexpr wchar_t kInfo = 0xE946;
inline constexpr wchar_t kSystem = 0xE770;
inline constexpr wchar_t kDisplay = 0xE7F4;
inline constexpr wchar_t kDisk = 0xEDA2;
inline constexpr wchar_t kAudio = 0xE767;
inline constexpr wchar_t kNetwork = 0xE968;
inline constexpr wchar_t kInstall = 0xE896;
inline constexpr wchar_t kWarning = 0xE7BA;
} // namespace glyph

struct Fonts {
    HFONT body = nullptr;
    HFONT bold = nullptr;
    HFONT caption = nullptr;
    HFONT heading = nullptr;
    HFONT title = nullptr;
    HFONT glyph = nullptr;
    HFONT glyphLarge = nullptr;
    HFONT badge = nullptr;
};

void StartGraphics();
void StopGraphics();
// (Re)creates the fonts for a DPI.
void InitTheme(UINT dpi);
UINT Dpi();
int Scale(int value);
const Fonts& Font();
HBRUSH Brush(COLORREF color); // Cached; never delete.

void Fill(HDC dc, const RECT& rect, COLORREF color);
void RoundedRect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border = CLR_INVALID);
void Circle(HDC dc, const RECT& rect, COLORREF fill);
void Line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color);
void Text(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color,
          UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
int TextWidth(HDC dc, const std::wstring& text, HFONT font);

// Flat toolbar button (BS_OWNERDRAW): glyph above a label, hover feedback.
void MakeFlatButton(HWND button, wchar_t glyph, const std::wstring& label, COLORREF glyphColor = CLR_INVALID);
void SetFlatButton(HWND button, wchar_t glyph, const std::wstring& label, COLORREF glyphColor = CLR_INVALID);
// WM_DRAWITEM handler for flat buttons; returns false for other controls.
bool DrawFlatButton(const DRAWITEMSTRUCT& item, COLORREF background);

// Off-screen buffer for flicker-free painting.
class BufferedPaint final {
public:
    BufferedPaint(HDC target, const RECT& rect);
    ~BufferedPaint();
    BufferedPaint(const BufferedPaint&) = delete;
    BufferedPaint& operator=(const BufferedPaint&) = delete;
    HDC Dc() const noexcept { return memory_; }

private:
    HDC target_;
    RECT rect_;
    HDC memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
};

} // namespace ui
