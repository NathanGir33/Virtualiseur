#pragma once

// Look and feel of the manager: Windows 11 style light or dark theme (or the
// Windows setting), DPI-aware fonts, anti-aliased shapes (GDI+) and flat
// owner-drawn toolbar buttons.

#include <Windows.h>
#include <commctrl.h>

#include <string>

namespace ui {

// The current palette: SetTheme rewrites it (light values below by default).
// Colors are read at paint time, so a theme change only needs a repaint.
namespace color {
inline COLORREF kWindow = RGB(243, 243, 243);
inline COLORREF kPanel = RGB(255, 255, 255);
inline COLORREF kBorder = RGB(224, 224, 224);
inline COLORREF kText = RGB(26, 26, 26);
inline COLORREF kSecondary = RGB(96, 96, 96);
inline COLORREF kAccent = RGB(0, 103, 192);
inline COLORREF kAccentSoft = RGB(230, 241, 251);
inline COLORREF kHover = RGB(235, 235, 235);
inline COLORREF kPressed = RGB(222, 222, 222);
inline COLORREF kGreen = RGB(16, 124, 16);
inline COLORREF kOrange = RGB(202, 120, 0);
inline COLORREF kRed = RGB(196, 43, 28);
inline COLORREF kDisabled = RGB(165, 165, 165);
// One color per kind of setting (cards, icons): same meaning everywhere.
inline COLORREF kPurple = RGB(136, 23, 152);   // Display.
inline COLORREF kTeal = RGB(0, 120, 120);      // Storage.
inline COLORREF kPink = RGB(191, 0, 119);      // Audio.
inline COLORREF kAmber = RGB(157, 93, 0);      // Shared folders.
inline COLORREF kIndigo = RGB(79, 70, 229);    // Snapshots.
inline COLORREF kSlate = RGB(90, 98, 112);     // General, installation.
} // namespace color

enum class Theme { System, Light, Dark };
// Fills the palette and the Windows dark-mode hints (menus). System follows
// the Windows "app mode" setting.
void SetTheme(Theme theme);
Theme CurrentTheme();
bool IsDark();
// Title bar (DWM) and the window's own scroll bars follow the theme.
void ThemeWindow(HWND window);
// Native controls (edits, lists, combo boxes, buttons) follow the theme.
void ThemeControl(HWND control);
// `b` over `a` at `percent` % (tints, light versions of a color).
COLORREF Mix(COLORREF a, COLORREF b, int percent);
// Windows 11 style check box (NM_CUSTOMDRAW of a BS_AUTOCHECKBOX button);
// returns the custom-draw result.
LRESULT DrawCheckBox(const NMCUSTOMDRAW& draw, COLORREF background);
// Main menu bar in the dark theme (undocumented WM_UAHDRAWMENU* messages, as
// Windows itself has no dark menu bar): true when the message was handled.
bool HandleMenuBar(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

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
inline constexpr wchar_t kCamera = 0xE722;
inline constexpr wchar_t kDocument = 0xE8A5;
inline constexpr wchar_t kInfo = 0xE946;
inline constexpr wchar_t kSystem = 0xE770;
inline constexpr wchar_t kDisplay = 0xE7F4;
inline constexpr wchar_t kDisk = 0xEDA2;
inline constexpr wchar_t kAudio = 0xE767;
inline constexpr wchar_t kNetwork = 0xE968;
inline constexpr wchar_t kInstall = 0xE896;
inline constexpr wchar_t kWarning = 0xE7BA;
inline constexpr wchar_t kCheck = 0xE73E;
inline constexpr wchar_t kComputer = 0xE977;
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
void SetFlatButtonColor(HWND button, COLORREF glyphColor); // After a theme change.
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
