#include "DisplayWindow.h"

#include <commctrl.h>

#include <algorithm>
#include <array>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace vmm {
namespace {

// Fallback refresh (title, text mode, and graphics while a modal loop such as
// a window drag or a message box blocks the frame loop).
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshTimerMs = 30;
constexpr UINT_PTR kResizeTimer = 2; // Debounces guest resolution changes.
constexpr UINT_PTR kBarTimer = 3;    // Hides the full-screen bar.
enum MenuId : UINT {
    kMenuPause = 101,
    kMenuResume,
    kMenuCtrlAltDel,
    kMenuAcpiShutdown,
    kMenuReset,
    kMenuPowerOff,
    kMenuFullscreen = 201,
    kMenuAutoResize,      // Radio: the guest resolution follows the window...
    kMenuFixedResolution, // ...or stays put and the image is scaled.
    kMenuActualSize,
    kMenuHelp = 301,
    kMenuMute = 401,
    kMenuCaptureKeyboard,
};

// Set-1 scan codes (all E0-prefixed) of the keys Windows reports by virtual
// key only: media, volume and browser keys of laptop and multimedia keyboards.
struct MediaKey {
    DWORD vk;
    std::uint8_t scan;
};
constexpr std::array<MediaKey, 18> kMediaKeys{{
    {VK_VOLUME_MUTE, 0x20}, {VK_VOLUME_DOWN, 0x2E}, {VK_VOLUME_UP, 0x30},
    {VK_MEDIA_NEXT_TRACK, 0x19}, {VK_MEDIA_PREV_TRACK, 0x10}, {VK_MEDIA_STOP, 0x24}, {VK_MEDIA_PLAY_PAUSE, 0x22},
    {VK_BROWSER_BACK, 0x6A}, {VK_BROWSER_FORWARD, 0x69}, {VK_BROWSER_REFRESH, 0x67}, {VK_BROWSER_STOP, 0x68},
    {VK_BROWSER_SEARCH, 0x65}, {VK_BROWSER_FAVORITES, 0x66}, {VK_BROWSER_HOME, 0x32},
    {VK_LAUNCH_MAIL, 0x6C}, {VK_LAUNCH_MEDIA_SELECT, 0x6D}, {VK_LAUNCH_APP1, 0x6B}, {VK_LAUNCH_APP2, 0x21},
}};

constexpr wchar_t kBarClass[] = L"VirtualiseurFullscreenBar";

constexpr std::array<COLORREF, 16> kVgaColors{
    RGB(0, 0, 0), RGB(0, 0, 170), RGB(0, 170, 0), RGB(0, 170, 170),
    RGB(170, 0, 0), RGB(170, 0, 170), RGB(170, 85, 0), RGB(170, 170, 170),
    RGB(85, 85, 85), RGB(85, 85, 255), RGB(85, 255, 85), RGB(85, 255, 255),
    RGB(255, 85, 85), RGB(255, 85, 255), RGB(255, 255, 85), RGB(255, 255, 255)};

// CP437 control-range glyphs (smileys, arrows...) used by text UIs.
constexpr std::array<wchar_t, 32> kCp437Low{
    L' ', 0x263A, 0x263B, 0x2665, 0x2666, 0x2663, 0x2660, 0x2022, 0x25D8, 0x25CB, 0x25D9, 0x2642, 0x2640,
    0x266A, 0x266B, 0x263C, 0x25BA, 0x25C4, 0x2195, 0x203C, 0x00B6, 0x00A7, 0x25AC, 0x21A8, 0x2191,
    0x2193, 0x2192, 0x2190, 0x221F, 0x2194, 0x25B2, 0x25BC};

wchar_t Cp437ToUnicode(std::uint8_t value)
{
    if (value < 0x20) return kCp437Low[value];
    if (value < 0x7F) return static_cast<wchar_t>(value);
    char source = static_cast<char>(value);
    wchar_t result = L'?';
    (void)MultiByteToWideChar(437, 0, &source, 1, &result, 1);
    return result;
}

} // namespace

DisplayWindow* DisplayWindow::hookOwner_ = nullptr;

DisplayWindow::DisplayWindow(std::wstring vmName, VmSlot& slot, std::uint32_t width, std::uint32_t height,
                             bool fullscreen)
    : name_(std::move(vmName)), slot_(slot), guestWidth_(width), guestHeight_(height)
{
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = &DisplayWindow::WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (windowClass.hIcon == nullptr) windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = nullptr; // Never erased: avoids flicker.
    windowClass.lpszClassName = L"VirtualiseurDisplay";
    RegisterClassExW(&windowClass);

    WNDCLASSEXW barClass{};
    barClass.cbSize = sizeof(barClass);
    barClass.lpfnWndProc = &DisplayWindow::BarProc;
    barClass.hInstance = instance;
    barClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    barClass.lpszClassName = kBarClass;
    RegisterClassExW(&barClass);

    menu_ = CreateMenu();
    HMENU machine = CreatePopupMenu();
    AppendMenuW(machine, MF_STRING, kMenuPause, L"&Pause");
    AppendMenuW(machine, MF_STRING, kMenuResume, L"&Reprendre");
    AppendMenuW(machine, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(machine, MF_STRING, kMenuCtrlAltDel, L"Envoyer Ctrl+Alt+&Suppr");
    AppendMenuW(machine, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(machine, MF_STRING, kMenuAcpiShutdown, L"Arrêt &ACPI (bouton d'alimentation)");
    AppendMenuW(machine, MF_STRING, kMenuReset, L"Re&démarrer (reset)");
    AppendMenuW(machine, MF_STRING, kMenuPowerOff, L"É&teindre (forcé)");
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(machine), L"&Machine");
    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, kMenuFullscreen, L"&Plein écran\tCtrl droit+F");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, kMenuAutoResize, L"&Résolution de l'invité adaptée à la fenêtre");
    AppendMenuW(view, MF_STRING, kMenuFixedResolution, L"Résolution fi&xe, image mise à l'échelle");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, kMenuActualSize, L"Fenêtre à la &taille de l'écran invité");
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&Affichage");
    HMENU devices = CreatePopupMenu();
    AppendMenuW(devices, MF_STRING, kMenuMute, L"&Couper le son");
    AppendMenuW(devices, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(devices, MF_STRING, kMenuCaptureKeyboard,
                L"Capturer le &clavier (touche Windows, Alt+Tab, touches multimédia)");
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(devices), L"&Périphériques");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, kMenuHelp, L"&Raccourcis clavier et souris");
    AppendMenuW(menu_, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"A&ide");

    // Fit the initial window to 90% of the work area.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const double maxWidth = (work.right - work.left) * 0.9;
    const double maxHeight = (work.bottom - work.top) * 0.9 - 60;
    const double scale = (std::min)({1.0, maxWidth / width, maxHeight / height});
    RECT bounds{0, 0, static_cast<LONG>(width * scale), static_cast<LONG>(height * scale)};
    AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, TRUE, 0);

    window_ = CreateWindowExW(0, windowClass.lpszClassName, name_.c_str(), WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
                              nullptr, menu_, instance, this);
    if (window_ == nullptr) throw std::runtime_error("Impossible de créer la fenêtre d'affichage.");
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
    RECT client{};
    GetClientRect(window_, &client);
    lastClientSize_ = {client.right, client.bottom};
    clipboard_ = std::make_unique<ClipboardBridge>(window_);
    SetTimer(window_, kRefreshTimer, kRefreshTimerMs, nullptr);
    // The hook runs on this (UI) thread, which pumps messages continuously.
    hookOwner_ = this;
    keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &DisplayWindow::KeyboardHook, instance, 0);
    UpdateTitle();
    if (fullscreen) ToggleFullscreen();
}

DisplayWindow::~DisplayWindow()
{
    if (barFont_ != nullptr) DeleteObject(barFont_);
    if (barBoldFont_ != nullptr) DeleteObject(barBoldFont_);
    ReleaseD2D();
    if (guestCursor_ != nullptr) DestroyCursor(guestCursor_);
    if (frameTimer_ != nullptr) CloseHandle(frameTimer_);
    if (textBitmap_ != nullptr) DeleteObject(textBitmap_);
    if (textDc_ != nullptr) DeleteDC(textDc_);
    if (textFont_ != nullptr) DeleteObject(textFont_);
}

int DisplayWindow::Run()
{
    // WM_TIMER is coalesced on the 15.6 ms system tick (a 16 ms timer fires
    // every ~31 ms) and is the lowest-priority message. A high-resolution
    // waitable timer paces frames at the monitor refresh rate instead.
    frameTimer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (frameTimer_ == nullptr) frameTimer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    UpdateFramePeriod();
    ArmFrameTimer();

    MSG message{};
    for (;;) {
        const DWORD handles = frameTimer_ != nullptr ? 1 : 0;
        const DWORD wait = MsgWaitForMultipleObjectsEx(handles, handles != 0 ? &frameTimer_ : nullptr, INFINITE,
                                                       QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (handles != 0 && wait == WAIT_OBJECT_0) {
            ArmFrameTimer();
            OnFrame(false);
        }
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return static_cast<int>(message.wParam);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

void DisplayWindow::UpdateFramePeriod()
{
    int hertz = 0;
    if (HDC dc = GetDC(window_)) {
        hertz = GetDeviceCaps(dc, VREFRESH);
        ReleaseDC(window_, dc);
    }
    if (hertz <= 1) hertz = 60; // 0 or 1 means "hardware default".
    hertz = std::clamp(hertz, 30, 240);
    framePeriod_ = 10'000'000 / hertz;
}

void DisplayWindow::ArmFrameTimer()
{
    if (frameTimer_ == nullptr || closing_) return;
    LARGE_INTEGER due{};
    due.QuadPart = -framePeriod_; // Relative, 100 ns units.
    if (!SetWaitableTimer(frameTimer_, &due, 0, nullptr, nullptr, FALSE)) {
        CloseHandle(frameTimer_);
        frameTimer_ = nullptr; // WM_TIMER alone keeps refreshing.
    }
}

void DisplayWindow::OnFrame(bool fromTimerMessage)
{
    if (closing_) return;
    const auto vm = slot_.Get();
    if (vm.get() != lastVm_) {
        // New VM (guest reset): new framebuffer, redraw everything.
        lastVm_ = vm.get();
        if (vm) vm->SetAudioMuted(audioMuted_); // Keep the mute setting across guest resets.
        uploadAll_ = true;
        cursor_ = GpuCursor{};
        tabletButtons_ = 0;
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    if (!vm) {
        if (fromTimerMessage) InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    SyncTextureDisplay(*vm);
    if (vm->Display().mode != DisplayMode::Graphics) {
        // Text mode diffs its cell buffer itself; the fallback rate is enough.
        if (fromTimerMessage) InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    const auto now = GetTickCount64();
    if (fromTimerMessage) {
        // The frame loop is running: let it pace the graphics.
        if (frameTimer_ != nullptr && now - lastFrameTick_ < 100) return;
    } else {
        lastFrameTick_ = now;
    }

    DisplaySnapshot snapshot;
    DirtyRows rows;
    const bool wasGpu = gpuMode_;
    if (!GraphicsSnapshot(*vm, snapshot, rows, true)) return;
    const bool dirty = !rows.Empty();
    if (wasGpu != gpuMode_) {
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    bool cursorMoved = false;
    if (gpuMode_) {
        const auto shapeSerial = cursor_.shapeSerial;
        const bool wasDrawn = DrawsGuestCursor();
        if (vm->GpuCursorSnapshot(cursor_)) {
            UpdateGuestCursor(cursor_.shapeSerial != shapeSerial);
            cursorMoved = wasDrawn || DrawsGuestCursor();
        }
    }
    if (!dirty && !uploadAll_ && !cursorMoved) return;
    if (RenderGraphics(snapshot, rows)) return;

    // GDI fallback: invalidate only the window bands showing the changed rows.
    if (!dirty) {
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    const auto target = FitRect(client, snapshot.width, snapshot.height);
    const double scale = static_cast<double>(target.bottom - target.top) / snapshot.height;
    for (const auto& changed : rows.Bands()) {
        RECT band{target.left, target.top + static_cast<LONG>(changed.top * scale) - 2,
                  target.right, target.top + static_cast<LONG>(changed.bottom * scale) + 3};
        InvalidateRect(window_, &band, FALSE);
    }
}

bool DisplayWindow::GraphicsSnapshot(Vm& vm, DisplaySnapshot& snapshot, DirtyRows& rows, bool queryDirty)
{
    rows.Clear();
    // The scanout copy always consumes the guest's changes: they must reach
    // the bitmap even when called from WM_PAINT.
    if (vm.CopyGpuScanout(gpuCopy_, rows)) {
        if (!gpuMode_) {
            gpuMode_ = true;
            uploadAll_ = true;
            // The guest driver is up: give it the window's size at once, so
            // the desktop starts 1:1 instead of scaled.
            ScheduleGuestResize(100);
        }
        snapshot.mode = DisplayMode::Graphics;
        snapshot.texture = gpuCopy_.texture;
        snapshot.framebuffer = gpuCopy_.texture ? nullptr : gpuCopy_.pixels.data();
        snapshot.width = gpuCopy_.width;
        snapshot.height = gpuCopy_.height;
        snapshot.stride = gpuCopy_.width * 4;
    } else {
        if (gpuMode_) {
            gpuMode_ = false;
            uploadAll_ = true;
        }
        snapshot = vm.Display();
        if (snapshot.mode != DisplayMode::Graphics || snapshot.framebuffer == nullptr) return false;
        if (queryDirty) vm.FramebufferDirtyRows(rows);
    }
    if (snapshot.width != screenWidth_ || snapshot.height != screenHeight_) {
        screenWidth_ = snapshot.width;
        screenHeight_ = snapshot.height;
        uploadAll_ = true;
        UpdateGuestCursor(false); // The cursor scale follows the screen scale.
    }
    return true;
}

bool DisplayWindow::PointerOverGuest(POINT client, std::int32_t& x, std::int32_t& y) const
{
    if (screenWidth_ == 0 || screenHeight_ == 0) return false;
    RECT clientRect{};
    GetClientRect(window_, &clientRect);
    const auto target = FitRect(clientRect, screenWidth_, screenHeight_);
    const auto width = target.right - target.left;
    const auto height = target.bottom - target.top;
    if (width <= 1 || height <= 1) return false;
    const bool inside = PtInRect(&target, client) != FALSE;
    const auto px = std::clamp<LONG>(client.x - target.left, 0, width - 1);
    const auto py = std::clamp<LONG>(client.y - target.top, 0, height - 1);
    x = static_cast<std::int32_t>((static_cast<std::int64_t>(px) * VirtioTablet::kAbsMax) / (width - 1));
    y = static_cast<std::int32_t>((static_cast<std::int64_t>(py) * VirtioTablet::kAbsMax) / (height - 1));
    return inside;
}

void DisplayWindow::UpdateGuestCursor(bool shapeChanged)
{
    if (!gpuMode_ || cursor_.width == 0 || cursor_.height == 0 || screenHeight_ == 0) return;
    RECT client{};
    GetClientRect(window_, &client);
    const auto target = FitRect(client, screenWidth_, screenHeight_);
    const double scale = static_cast<double>(target.bottom - target.top) / screenHeight_;
    if (scale <= 0.0) return;
    if (!shapeChanged && guestCursor_ != nullptr && guestCursorSerial_ == cursor_.shapeSerial &&
        std::abs(scale - guestCursorScale_) < 0.01) {
        return;
    }

    // Windows cursor from the guest image, at the on-screen scale. DRM
    // cursors are premultiplied; Windows alpha cursors are not.
    const int width = (std::max)(1, static_cast<int>(cursor_.width * scale + 0.5));
    const int height = (std::max)(1, static_cast<int>(cursor_.height * scale + 0.5));
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = width;
    header.bV5Height = -height;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (color == nullptr || bits == nullptr) {
        if (color != nullptr) DeleteObject(color);
        return;
    }
    auto* out = static_cast<std::uint8_t*>(bits);
    for (int row = 0; row < height; ++row) {
        const auto sourceRow = (std::min)(cursor_.height - 1, static_cast<std::uint32_t>(row / scale));
        for (int column = 0; column < width; ++column) {
            const auto sourceColumn = (std::min)(cursor_.width - 1, static_cast<std::uint32_t>(column / scale));
            const auto* pixel = &cursor_.pixels[(static_cast<std::size_t>(sourceRow) * cursor_.width + sourceColumn) * 4];
            auto* destination = &out[(static_cast<std::size_t>(row) * width + column) * 4];
            const auto alpha = pixel[3];
            for (int channel = 0; channel < 3; ++channel) {
                destination[channel] = alpha == 0 ? 0
                    : static_cast<std::uint8_t>((std::min)(255, pixel[channel] * 255 / alpha));
            }
            destination[3] = alpha;
        }
    }
    std::vector<std::uint8_t> maskBits(static_cast<std::size_t>((width + 15) / 16 * 2) * height, 0);
    HBITMAP mask = CreateBitmap(width, height, 1, 1, maskBits.data());
    ICONINFO icon{};
    icon.fIcon = FALSE;
    icon.xHotspot = static_cast<DWORD>(cursor_.hotX * scale);
    icon.yHotspot = static_cast<DWORD>(cursor_.hotY * scale);
    icon.hbmMask = mask;
    icon.hbmColor = color;
    HCURSOR created = CreateIconIndirect(&icon);
    DeleteObject(mask);
    DeleteObject(color);
    if (created == nullptr) return;
    if (guestCursor_ != nullptr) DestroyCursor(guestCursor_);
    guestCursor_ = created;
    guestCursorSerial_ = cursor_.shapeSerial;
    guestCursorScale_ = scale;

    // Apply at once if the pointer is over the guest screen.
    POINT point{};
    if (tabletActive_ && GetCursorPos(&point) && WindowFromPoint(point) == window_) {
        ScreenToClient(window_, &point);
        std::int32_t x = 0;
        std::int32_t y = 0;
        if (PointerOverGuest(point, x, y)) SetCursor(cursor_.visible ? guestCursor_ : nullptr);
    }
}

void DisplayWindow::OnTabletMouse(UINT message, WPARAM wParam, LPARAM lParam)
{
    const auto vm = slot_.Get();
    if (!vm) return;
    POINT point{static_cast<SHORT>(LOWORD(lParam)), static_cast<SHORT>(HIWORD(lParam))};
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        const bool horizontal = message == WM_MOUSEHWHEEL;
        auto& remainder = horizontal ? hwheelRemainder_ : wheelRemainder_;
        remainder += GET_WHEEL_DELTA_WPARAM(wParam);
        const int steps = remainder / WHEEL_DELTA;
        remainder -= steps * WHEEL_DELTA;
        if (steps != 0) vm->TabletWheel(steps, horizontal);
        return;
    }
    std::int32_t x = 0;
    std::int32_t y = 0;
    (void)PointerOverGuest(point, x, y);
    vm->TabletMove(x, y);

    using Button = VirtioTablet::Button;
    const auto press = [&](std::uint8_t bit, Button button, bool down) {
        if (down == ((tabletButtons_ & bit) != 0)) return;
        tabletButtons_ = static_cast<std::uint8_t>(down ? tabletButtons_ | bit : tabletButtons_ & ~bit);
        vm->TabletButton(button, down);
    };
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: press(0x01, Button::Left, true); break;
    case WM_LBUTTONUP: press(0x01, Button::Left, false); break;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: press(0x02, Button::Right, true); break;
    case WM_RBUTTONUP: press(0x02, Button::Right, false); break;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: press(0x04, Button::Middle, true); break;
    case WM_MBUTTONUP: press(0x04, Button::Middle, false); break;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP: {
        const bool down = message != WM_XBUTTONUP;
        if (GET_XBUTTON_WPARAM(wParam) == XBUTTON1) press(0x08, Button::Side, down);
        else press(0x10, Button::Extra, down);
        break;
    }
    default: break;
    }
    // Keep receiving the drag outside the window while a button is held.
    if (tabletButtons_ != 0 && GetCapture() != window_) SetCapture(window_);
    else if (tabletButtons_ == 0 && GetCapture() == window_) ReleaseCapture();
}

void DisplayWindow::ReleaseTabletButtons()
{
    if (tabletButtons_ == 0) return;
    if (const auto vm = slot_.Get()) {
        using Button = VirtioTablet::Button;
        constexpr std::array<std::pair<std::uint8_t, Button>, 5> buttons{{
            {std::uint8_t{0x01}, Button::Left}, {std::uint8_t{0x02}, Button::Right},
            {std::uint8_t{0x04}, Button::Middle}, {std::uint8_t{0x08}, Button::Side},
            {std::uint8_t{0x10}, Button::Extra}}};
        for (const auto& [bit, button] : buttons) {
            if ((tabletButtons_ & bit) != 0) vm->TabletButton(button, false);
        }
    }
    tabletButtons_ = 0;
    if (GetCapture() == window_) ReleaseCapture();
}

void DisplayWindow::RequestGuestResize()
{
    if (!gpuMode_) return;
    const auto vm = slot_.Get();
    if (!vm) return;
    RECT client{};
    GetClientRect(window_, &client);
    const auto width = client.right - client.left;
    const auto height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;
    lastClientSize_ = {width, height};
    // Ask for what the guest will set (its modes have a width multiple of 8),
    // so the request and the guest's answer always agree.
    vm->RequestDisplaySize(static_cast<std::uint32_t>(width) & ~7u, static_cast<std::uint32_t>(height));
}

bool DisplayWindow::EnsureD2D()
{
    if (d2dTarget_) return true;
    if (d2dUnavailable_) return false;
    const SharedD3DGuard d3d(SharedD3DLock());
    const auto fail = [this] {
        ReleaseD2D();
        d2dUnavailable_ = true;
        return false;
    };
    ID3D11Device* device = SharedD3DDevice();
    if (device == nullptr) return fail();
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)))) {
        return fail();
    }
    if (!d2dFactory_) {
        D2D1_FACTORY_OPTIONS options{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                     reinterpret_cast<void**>(d2dFactory_.GetAddressOf())))) {
            return fail();
        }
    }
    if (FAILED(d2dFactory_->CreateDevice(dxgiDevice.Get(), d2dDevice_.GetAddressOf())) ||
        FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dTarget_.GetAddressOf()))) {
        return fail();
    }
    // 96 DPI: Direct2D coordinates are physical pixels (the process is
    // per-monitor DPI aware).
    d2dTarget_->SetDpi(96.0f, 96.0f);

    RECT client{};
    GetClientRect(window_, &client);
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = static_cast<UINT>((std::max)(client.right - client.left, 1L));
    description.Height = static_cast<UINT>((std::max)(client.bottom - client.top, 1L));
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(dxgiFactory->CreateSwapChainForHwnd(device, window_, &description, nullptr, nullptr,
                                                   swapChain_.GetAddressOf()))) {
        return fail();
    }
    (void)dxgiFactory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER); // Full screen is ours.
    if (!CreateTargetBitmap()) return fail();
    d2dBitmap_.Reset();
    sharedBitmap_.Reset();
    sharedGeneration_ = ~0ull;
    uploadAll_ = true;
    return true;
}

bool DisplayWindow::CreateTargetBitmap()
{
    Microsoft::WRL::ComPtr<IDXGISurface> backBuffer;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) return false;
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    if (FAILED(d2dTarget_->CreateBitmapFromDxgiSurface(backBuffer.Get(), &properties, targetBitmap_.GetAddressOf()))) {
        return false;
    }
    d2dTarget_->SetTarget(targetBitmap_.Get());
    return true;
}

void DisplayWindow::ResizeSwapChain(UINT width, UINT height)
{
    if (!swapChain_ || width == 0 || height == 0) return;
    const SharedD3DGuard d3d(SharedD3DLock());
    d2dTarget_->SetTarget(nullptr);
    targetBitmap_.Reset(); // The buffers cannot be resized while referenced.
    if (FAILED(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0)) || !CreateTargetBitmap()) {
        ReleaseD2D();
    }
}

void DisplayWindow::ReleaseD2D()
{
    const SharedD3DGuard d3d(SharedD3DLock());
    cursorBitmap_.Reset();
    cursorBitmapSerial_ = ~0ull;
    d2dBitmap_.Reset();
    sharedBitmap_.Reset();
    sharedGeneration_ = ~0ull;
    if (d2dTarget_) d2dTarget_->SetTarget(nullptr);
    targetBitmap_.Reset();
    swapChain_.Reset();
    d2dTarget_.Reset();
    d2dDevice_.Reset();
    bitmapWidth_ = 0;
    bitmapHeight_ = 0;
    uploadAll_ = true;
}

void DisplayWindow::SyncTextureDisplay(Vm& vm)
{
    // The shared texture is only usable while Direct2D draws the window.
    const bool wanted = !d2dUnavailable_;
    if (textureDisplayVm_ == &vm && textureDisplay_ == wanted) return;
    textureDisplayVm_ = &vm;
    textureDisplay_ = wanted;
    vm.SetGpuTextureDisplay(wanted);
}

bool DisplayWindow::RenderGraphics(const DisplaySnapshot& snapshot, const DirtyRows& rows)
{
    const SharedD3DGuard d3d(SharedD3DLock());
    if (!EnsureD2D()) return false;
    RECT client{};
    GetClientRect(window_, &client);
    if (client.right <= client.left || client.bottom <= client.top) {
        uploadAll_ = uploadAll_ || !rows.Empty(); // Minimized: catch up when shown again.
        return true;
    }

    ID2D1Bitmap* image = nullptr;
    if (snapshot.texture) {
        // virgl screen: drawn straight from the texture the GPU thread fills.
        const auto& shared = *snapshot.texture;
        if (!shared.texture) return true;
        if (!sharedBitmap_ || sharedGeneration_ != shared.generation) {
            sharedBitmap_.Reset();
            Microsoft::WRL::ComPtr<IDXGISurface> surface;
            const auto properties = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96.0f, 96.0f);
            if (FAILED(shared.texture.As(&surface)) ||
                FAILED(d2dTarget_->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
                                                               sharedBitmap_.GetAddressOf()))) {
                ReleaseD2D();
                d2dUnavailable_ = true; // The VM goes back to readbacks (SyncTextureDisplay).
                InvalidateRect(window_, nullptr, FALSE);
                return false;
            }
            sharedGeneration_ = shared.generation;
        }
        image = sharedBitmap_.Get();
    } else {
        if (snapshot.framebuffer == nullptr) return true;
        if (!d2dBitmap_ || bitmapWidth_ != snapshot.width || bitmapHeight_ != snapshot.height) {
            d2dBitmap_.Reset();
            const auto properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
            if (FAILED(d2dTarget_->CreateBitmap(D2D1::SizeU(snapshot.width, snapshot.height), properties,
                                                d2dBitmap_.GetAddressOf()))) {
                ReleaseD2D();
                d2dUnavailable_ = true;
                InvalidateRect(window_, nullptr, FALSE);
                return false;
            }
            bitmapWidth_ = snapshot.width;
            bitmapHeight_ = snapshot.height;
            uploadAll_ = true;
        }

        // XRGB8888 guest pixels are BGRA bytes: uploaded as is, only the changed rows.
        if (uploadAll_) {
            if (FAILED(d2dBitmap_->CopyFromMemory(nullptr, snapshot.framebuffer, snapshot.stride))) return true;
            uploadAll_ = false;
        } else {
            for (const auto& band : rows.Bands()) {
                const auto area = D2D1::RectU(0, band.top, snapshot.width, band.bottom);
                (void)d2dBitmap_->CopyFromMemory(&area,
                    snapshot.framebuffer + static_cast<std::size_t>(band.top) * snapshot.stride, snapshot.stride);
            }
        }
        image = d2dBitmap_.Get();
    }

    const auto target = FitRect(client, snapshot.width, snapshot.height);
    const auto drawWidth = target.right - target.left;
    const auto drawHeight = target.bottom - target.top;
    const D2D1_RECT_F destination{static_cast<float>(target.left), static_cast<float>(target.top),
                                  static_cast<float>(target.right), static_cast<float>(target.bottom)};
    const bool exact = drawWidth == static_cast<LONG>(snapshot.width) && drawHeight == static_cast<LONG>(snapshot.height);
    const bool downscale = drawWidth < static_cast<LONG>(snapshot.width);
    // The shared texture may be larger than the guest screen (kept across
    // small resolution changes): draw only the screen's part of it.
    const D2D1_RECT_F source{0.0f, 0.0f, static_cast<float>(snapshot.width), static_cast<float>(snapshot.height)};

    d2dTarget_->BeginDraw();
    d2dTarget_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f));
    const auto mode = exact ? D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                    : downscale ? D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC
                                : D2D1_INTERPOLATION_MODE_LINEAR;
    d2dTarget_->DrawBitmap(image, &destination, 1.0f, mode, &source, nullptr);
    if (DrawsGuestCursor() && cursor_.pixels.size() >= static_cast<std::size_t>(cursor_.width) * cursor_.height * 4) {
        // Captured PS/2 mouse: draw the guest cursor plane over the screen.
        if (!cursorBitmap_ || cursorBitmapSerial_ != cursor_.shapeSerial) {
            cursorBitmap_.Reset();
            const auto properties = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
            (void)d2dTarget_->CreateBitmap(D2D1::SizeU(cursor_.width, cursor_.height), cursor_.pixels.data(),
                                           cursor_.width * 4, properties, cursorBitmap_.GetAddressOf());
            cursorBitmapSerial_ = cursor_.shapeSerial;
        }
        if (cursorBitmap_) {
            const float scale = static_cast<float>(drawHeight) / static_cast<float>(snapshot.height);
            const float left = static_cast<float>(target.left) + static_cast<float>(cursor_.x) * scale;
            const float upper = static_cast<float>(target.top) + static_cast<float>(cursor_.y) * scale;
            const D2D1_RECT_F where{left, upper, left + static_cast<float>(cursor_.width) * scale,
                                    upper + static_cast<float>(cursor_.height) * scale};
            d2dTarget_->PushAxisAlignedClip(destination, D2D1_ANTIALIAS_MODE_ALIASED);
            d2dTarget_->DrawBitmap(cursorBitmap_.Get(), where, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            d2dTarget_->PopAxisAlignedClip();
        }
    }
    HRESULT result = d2dTarget_->EndDraw();
    // Pacing is ours; DWM composition never tears a windowed flip-model swap chain.
    if (SUCCEEDED(result)) result = swapChain_->Present(0, 0);
    if (result == D2DERR_RECREATE_TARGET || result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET) {
        // GPU reset or driver update: the shared device is gone, use GDI.
        ReleaseD2D();
        d2dUnavailable_ = true;
        InvalidateRect(window_, nullptr, FALSE);
        return false;
    }
    if (FAILED(result)) {
        ReleaseD2D();
        d2dUnavailable_ = true;
        InvalidateRect(window_, nullptr, FALSE);
        return false;
    }
    return true;
}

LRESULT CALLBACK DisplayWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto* self = static_cast<DisplayWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    }
    auto* self = reinterpret_cast<DisplayWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self == nullptr) return DefWindowProcW(window, message, wParam, lParam);
    return self->Handle(message, wParam, lParam);
}

LRESULT DisplayWindow::Handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_TIMER:
        if (wParam == kResizeTimer) {
            KillTimer(window_, kResizeTimer);
            RequestGuestResize();
            return 0;
        }
        if (wParam == kBarTimer) {
            POINT cursor{};
            RECT bar{};
            const bool hovered = bar_ != nullptr && IsWindowVisible(bar_) && GetCursorPos(&cursor) &&
                                 GetWindowRect(bar_, &bar) && PtInRect(&bar, cursor);
            if (hovered) barHideAt_ = (std::max)(barHideAt_, GetTickCount64() + 800);
            if (!fullscreen_ || GetTickCount64() >= barHideAt_) HideFullscreenBar();
            return 0;
        }
        if (wParam == kRefreshTimer) {
            OnFrame(true);
            // The guest loads its virtio-input driver some time after boot:
            // from then on the mouse is absolute and needs no capture.
            const auto vm = slot_.Get();
            if (clipboard_) clipboard_->Process(vm ? vm->Agent() : nullptr);
            const bool tablet = vm && vm->TabletReady();
            if (tablet != tabletActive_) {
                tabletActive_ = tablet;
                if (tablet) ReleaseMouse();
                else ReleaseTabletButtons();
                UpdateGuestCursor(true);
                InvalidateRect(window_, nullptr, FALSE);
            }
            UpdateTitle();
        }
        return 0;
    case WM_SIZE:
        if (mouseCaptured_) CaptureMouse(); // Refresh the clip rectangle.
        if (d2dTarget_) ResizeSwapChain(LOWORD(lParam), HIWORD(lParam));
        InvalidateRect(window_, nullptr, FALSE);
        UpdateGuestCursor(false);
        // The guest follows the window live: while dragging, once the edge
        // pauses; otherwise (maximize, snap, full screen) right away.
        if (wParam != SIZE_MINIMIZED) ScheduleGuestResize(inSizeMove_ ? 250 : 60);
        return 0;
    case WM_SIZING:
        if (autoResize_ && gpuMode_ && !fullscreen_) SnapSizingWidth(wParam, *reinterpret_cast<RECT*>(lParam));
        return TRUE;
    case WM_ENTERSIZEMOVE:
        inSizeMove_ = true;
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_EXITSIZEMOVE:
        inSizeMove_ = false;
        UpdateFramePeriod(); // The window may now be on a monitor with another refresh rate.
        ScheduleGuestResize(30);
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_DISPLAYCHANGE:
        UpdateFramePeriod();
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && tabletActive_ && gpuMode_) {
            // The guest cursor becomes the Windows cursor: it moves with no latency.
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window_, &point);
            std::int32_t x = 0;
            std::int32_t y = 0;
            if (PointerOverGuest(point, x, y)) {
                SetCursor(cursor_.visible ? guestCursor_ : nullptr);
                return TRUE;
            }
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_MOUSEMOVE:
        // Full screen: the bar slides in when the pointer touches the top edge.
        if (fullscreen_ && !mouseCaptured_ && static_cast<SHORT>(HIWORD(lParam)) <= 1) ShowFullscreenBar(1500);
        [[fallthrough]];
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_MBUTTONDBLCLK:
        if (tabletActive_) OnTabletMouse(message, wParam, lParam);
        return message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK ? TRUE : 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        if (tabletActive_) {
            POINT point{static_cast<SHORT>(LOWORD(lParam)), static_cast<SHORT>(HIWORD(lParam))};
            ScreenToClient(window_, &point); // Wheel messages carry screen coordinates.
            OnTabletMouse(message, wParam, MAKELPARAM(point.x, point.y));
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != window_) ReleaseTabletButtons();
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        OnKey(wParam, lParam, false);
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        OnKey(wParam, lParam, true);
        return 0;
    case WM_SYSCHAR: case WM_CHAR:
        return 0;
    case WM_KILLFOCUS:
        hostKeyDown_ = false;
        swallowF_ = false;
        ReleaseAllKeys();
        ReleaseMouse();
        ReleaseTabletButtons();
        return 0;
    case WM_SETFOCUS:
        SyncLockKeys();
        return 0;
    case WM_CLIPBOARDUPDATE:
        if (clipboard_) {
            const auto vm = slot_.Get();
            clipboard_->OnClipboardUpdate(vm ? vm->Agent() : nullptr);
        }
        return 0;
    case WM_DROPFILES:
        if (clipboard_) {
            const auto vm = slot_.Get();
            clipboard_->OnDropFiles(reinterpret_cast<HDROP>(wParam), vm ? vm->Agent() : nullptr);
        } else {
            DragFinish(reinterpret_cast<HDROP>(wParam));
        }
        return 0;
    case WM_ACTIVATE:
        // Coming back to the VM window (click, Alt+Tab, taskbar) recaptures the keyboard.
        if (LOWORD(wParam) != WA_INACTIVE) SetKeyboardSuspended(false);
        return DefWindowProcW(window_, message, wParam, lParam);
    case WM_ENTERMENULOOP:
        inMenu_ = true;
        return 0;
    case WM_EXITMENULOOP:
        inMenu_ = false;
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        SetKeyboardSuspended(false);
        if (tabletActive_) {
            OnTabletMouse(message, wParam, lParam);
        } else if (!mouseCaptured_) {
            const auto vm = slot_.Get();
            const auto snapshot = vm ? vm->Display() : DisplaySnapshot{};
            if (snapshot.mode == DisplayMode::Graphics) CaptureMouse();
        }
        return 0;
    case WM_INPUT:
        OnRawInput(lParam);
        return 0;
    case WM_COMMAND:
        OnCommand(LOWORD(wParam));
        return 0;
    case WM_CLOSE: {
        const auto vm = slot_.Get();
        if (!vm || vm->State() == VmState::Stopped) {
            DestroyWindow(window_);
            return 0;
        }
        ReleaseMouse();
        constexpr int kShutdown = 1001;
        constexpr int kPowerOff = 1002;
        const TASKDIALOG_BUTTON buttons[] = {
            {kShutdown, L"Arrêt propre\nEnvoie le bouton d'alimentation ACPI : le système invité s'éteint lui-même."},
            {kPowerOff, L"Éteindre immédiatement\nComme couper le courant : les données non enregistrées sont perdues."},
        };
        TASKDIALOGCONFIG dialog{sizeof(dialog)};
        dialog.hwndParent = window_;
        dialog.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        dialog.pszWindowTitle = name_.c_str();
        dialog.pszMainIcon = TD_INFORMATION_ICON;
        dialog.pszMainInstruction = L"Fermer la machine virtuelle ?";
        dialog.cButtons = static_cast<UINT>(std::size(buttons));
        dialog.pButtons = buttons;
        dialog.nDefaultButton = kShutdown;
        dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        int choice = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&dialog, &choice, nullptr, nullptr))) {
            choice = MessageBoxW(window_, L"Oui : arrêt propre (ACPI)\nNon : éteindre immédiatement", name_.c_str(),
                                 MB_YESNOCANCEL | MB_ICONQUESTION);
            choice = choice == IDYES ? kShutdown : choice == IDNO ? kPowerOff : IDCANCEL;
        }
        if (choice == kShutdown) {
            if (!vm->AcpiShutdown()) {
                if (MessageBoxW(window_, L"Le système invité n'a pas activé le bouton d'alimentation ACPI.\n"
                                         L"Éteindre la machine immédiatement ?",
                                name_.c_str(), MB_YESNO | MB_ICONWARNING) == IDYES) {
                    vm->PowerOff();
                }
            }
        } else if (choice == kPowerOff) {
            vm->PowerOff();
        }
        return 0;
    }
    case kVmStoppedMessage:
        ReleaseMouse();
        DestroyWindow(window_);
        return 0;
    case WM_DESTROY:
        closing_ = true;
        clipboard_.reset();
        ReleaseMouse();
        if (bar_ != nullptr) DestroyWindow(bar_);
        bar_ = nullptr;
        if (keyboardHook_ != nullptr) UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
        hookOwner_ = nullptr;
        KillTimer(window_, kRefreshTimer);
        KillTimer(window_, kResizeTimer);
        KillTimer(window_, kBarTimer);
        if (frameTimer_ != nullptr) CancelWaitableTimer(frameTimer_);
        ReleaseD2D();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window_, message, wParam, lParam);
    }
}

RECT DisplayWindow::FitRect(const RECT& client, std::uint32_t width, std::uint32_t height) const
{
    const auto clientWidth = client.right - client.left;
    const auto clientHeight = client.bottom - client.top;
    if (width == 0 || height == 0 || clientWidth <= 0 || clientHeight <= 0) return client;
    const double scale = (std::min)(static_cast<double>(clientWidth) / width,
                                    static_cast<double>(clientHeight) / height);
    const auto drawWidth = static_cast<LONG>(width * scale);
    const auto drawHeight = static_cast<LONG>(height * scale);
    RECT rect{};
    rect.left = client.left + (clientWidth - drawWidth) / 2;
    rect.top = client.top + (clientHeight - drawHeight) / 2;
    rect.right = rect.left + drawWidth;
    rect.bottom = rect.top + drawHeight;
    return rect;
}

void DisplayWindow::Paint()
{
    const auto vm = slot_.Get();
    if (vm && vm.get() != lastVm_) {
        lastVm_ = vm.get();
        vm->SetAudioMuted(audioMuted_);
        uploadAll_ = true;
        cursor_ = GpuCursor{};
        tabletButtons_ = 0;
    }
    DisplaySnapshot snapshot;
    DirtyRows rows;
    if (vm) {
        snapshot = vm->Display();
        if (snapshot.mode == DisplayMode::Graphics) (void)GraphicsSnapshot(*vm, snapshot, rows, false);
    }
    const bool graphics = snapshot.mode == DisplayMode::Graphics &&
                          (snapshot.framebuffer != nullptr || snapshot.texture != nullptr);
    if (vm) SyncTextureDisplay(*vm);
    if (graphics) {
        // Direct2D redraws the whole window each frame: nothing to clip.
        if (RenderGraphics(snapshot, rows)) {
            ValidateRect(window_, nullptr);
            return;
        }
    } else {
        ReleaseD2D(); // GDI owns the window from here.
    }

    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);
    paintRect_ = paint.rcPaint;
    RECT client{};
    GetClientRect(window_, &client);
    if (graphics && snapshot.framebuffer != nullptr) {
        PaintGraphics(dc, client, snapshot);
    } else if (graphics) {
        // GDI fallback while the VM still sends a GPU texture: the next frames
        // come back in system memory (SyncTextureDisplay).
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        InvalidateRect(window_, nullptr, FALSE);
    } else if (snapshot.mode == DisplayMode::Text && snapshot.text != nullptr) {
        PaintText(dc, client, snapshot);
    } else {
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetTextColor(dc, RGB(200, 200, 200));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, L"Démarrage de la machine virtuelle…", -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    EndPaint(window_, &paint);
}

void DisplayWindow::PaintGraphics(HDC dc, const RECT& client, const DisplaySnapshot& snapshot)
{
    const auto target = FitRect(client, snapshot.width, snapshot.height);
    // Paint only the letterbox bars, never the image area.
    const auto black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RECT bar{client.left, client.top, client.right, target.top};
    FillRect(dc, &bar, black);
    bar = {client.left, target.bottom, client.right, client.bottom};
    FillRect(dc, &bar, black);
    bar = {client.left, target.top, target.left, target.bottom};
    FillRect(dc, &bar, black);
    bar = {target.right, target.top, client.right, target.bottom};
    FillRect(dc, &bar, black);

    // Only convert the framebuffer rows that intersect the invalidated band:
    // a blinking cursor then costs a few lines instead of a full-screen scale.
    const auto drawWidth = target.right - target.left;
    const auto drawHeight = target.bottom - target.top;
    if (drawWidth <= 0 || drawHeight <= 0) return;
    const double scale = static_cast<double>(drawHeight) / snapshot.height;
    const RECT clip = paintRect_;
    auto firstRow = static_cast<LONG>((clip.top - target.top) / scale) - 1;
    auto lastRow = static_cast<LONG>((clip.bottom - target.top) / scale) + 2;
    firstRow = std::clamp<LONG>(firstRow, 0, static_cast<LONG>(snapshot.height));
    lastRow = std::clamp<LONG>(lastRow, firstRow, static_cast<LONG>(snapshot.height));
    if (lastRow <= firstRow) return;
    const auto rows = lastRow - firstRow;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = static_cast<LONG>(snapshot.stride / 4);
    info.bmiHeader.biHeight = -rows;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const bool downscale = drawWidth < static_cast<LONG>(snapshot.width);
    SetStretchBltMode(dc, downscale ? HALFTONE : COLORONCOLOR);
    if (downscale) SetBrushOrgEx(dc, 0, 0, nullptr);
    const auto destinationTop = target.top + static_cast<LONG>(firstRow * scale);
    const auto destinationBottom = target.top + static_cast<LONG>(lastRow * scale);
    StretchDIBits(dc, target.left, destinationTop, drawWidth, destinationBottom - destinationTop,
                  0, 0, static_cast<int>(snapshot.width), rows,
                  snapshot.framebuffer + static_cast<std::size_t>(firstRow) * snapshot.stride,
                  &info, DIB_RGB_COLORS, SRCCOPY);
}

void DisplayWindow::PaintText(HDC dc, const RECT& client, const DisplaySnapshot& snapshot)
{
    const int columns = static_cast<int>(snapshot.width);
    const int rows = static_cast<int>(snapshot.height);
    if (textDc_ == nullptr) {
        textFont_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");
        textDc_ = CreateCompatibleDC(dc);
        SelectObject(textDc_, textFont_);
        TEXTMETRICW metrics{};
        GetTextMetricsW(textDc_, &metrics);
        cellWidth_ = metrics.tmAveCharWidth;
        cellHeight_ = metrics.tmHeight;
        textBitmap_ = CreateCompatibleBitmap(dc, columns * cellWidth_, rows * cellHeight_);
        SelectObject(textDc_, textBitmap_);
        textShadow_.assign(static_cast<std::size_t>(columns * rows), 0xFFFF);
    }

    const DWORD now = GetTickCount();
    if (now - lastBlink_ > 500) {
        cursorBlink_ = !cursorBlink_;
        lastBlink_ = now;
        textDirty_ = true;
    }
    bool changed = textDirty_ || snapshot.cursor != lastCursor_;
    std::vector<std::uint16_t> cells(static_cast<std::size_t>(columns * rows));
    for (std::size_t index = 0; index < cells.size(); ++index) cells[index] = snapshot.text[index];
    if (cells != textShadow_) changed = true;

    if (changed) {
        textShadow_ = cells;
        lastCursor_ = snapshot.cursor;
        textDirty_ = false;
        SetBkMode(textDc_, OPAQUE);
        std::vector<wchar_t> line(static_cast<std::size_t>(columns));
        for (int y = 0; y < rows; ++y) {
            int x = 0;
            while (x < columns) {
                const auto attribute = static_cast<std::uint8_t>(cells[static_cast<std::size_t>(y * columns + x)] >> 8);
                int end = x;
                while (end < columns &&
                       static_cast<std::uint8_t>(cells[static_cast<std::size_t>(y * columns + end)] >> 8) == attribute) {
                    line[static_cast<std::size_t>(end - x)] =
                        Cp437ToUnicode(static_cast<std::uint8_t>(cells[static_cast<std::size_t>(y * columns + end)]));
                    ++end;
                }
                SetTextColor(textDc_, kVgaColors[attribute & 0x0F]);
                SetBkColor(textDc_, kVgaColors[(attribute >> 4) & 0x07]);
                RECT run{x * cellWidth_, y * cellHeight_, end * cellWidth_, (y + 1) * cellHeight_};
                ExtTextOutW(textDc_, run.left, run.top, ETO_OPAQUE | ETO_CLIPPED, &run, line.data(),
                            static_cast<UINT>(end - x), nullptr);
                x = end;
            }
        }
        if (snapshot.cursorVisible && cursorBlink_ && snapshot.cursor < columns * rows) {
            const int cx = snapshot.cursor % columns;
            const int cy = snapshot.cursor / columns;
            const auto attribute = static_cast<std::uint8_t>(cells[snapshot.cursor] >> 8);
            HBRUSH brush = CreateSolidBrush(kVgaColors[attribute & 0x0F]);
            RECT cursor{cx * cellWidth_, (cy + 1) * cellHeight_ - 3, (cx + 1) * cellWidth_, (cy + 1) * cellHeight_ - 1};
            FillRect(textDc_, &cursor, brush);
            DeleteObject(brush);
        }
    }

    const int sourceWidth = columns * cellWidth_;
    const int sourceHeight = rows * cellHeight_;
    const auto target = FitRect(client, static_cast<std::uint32_t>(sourceWidth), static_cast<std::uint32_t>(sourceHeight));
    const auto black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RECT bar{client.left, client.top, client.right, target.top};
    FillRect(dc, &bar, black);
    bar = {client.left, target.bottom, client.right, client.bottom};
    FillRect(dc, &bar, black);
    bar = {client.left, target.top, target.left, target.bottom};
    FillRect(dc, &bar, black);
    bar = {target.right, target.top, client.right, target.bottom};
    FillRect(dc, &bar, black);
    SetStretchBltMode(dc, HALFTONE);
    SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchBlt(dc, target.left, target.top, target.right - target.left, target.bottom - target.top,
               textDc_, 0, 0, sourceWidth, sourceHeight, SRCCOPY);
}

void DisplayWindow::UpdateTitle()
{
    std::wstring title = name_ + L" — Virtualiseur";
    const auto vm = slot_.Get();
    if (!vm) title += L" [redémarrage]";
    else if (vm->State() == VmState::Paused) title += L" [en pause]";
    if (mouseCaptured_) title += L"  —  Ctrl droit libère la souris";
    else if (keyboardSuspended_ && captureKeyboard_) title += L"  —  clavier rendu à Windows (clic dans la fenêtre pour le reprendre)";
    if (title != lastTitle_) {
        SetWindowTextW(window_, title.c_str());
        lastTitle_ = title;
    }
    const bool paused = vm && vm->State() == VmState::Paused;
    EnableMenuItem(menu_, kMenuPause, MF_BYCOMMAND | (vm && !paused ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu_, kMenuResume, MF_BYCOMMAND | (paused ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu_, kMenuMute, MF_BYCOMMAND | (vm && vm->HasAudio() ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(menu_, kMenuMute, MF_BYCOMMAND | (audioMuted_ ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(menu_, kMenuFullscreen, MF_BYCOMMAND | (fullscreen_ ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(menu_, kMenuAutoResize, kMenuFixedResolution,
                       autoResize_ ? kMenuAutoResize : kMenuFixedResolution, MF_BYCOMMAND);
    // 1:1 window only makes sense when the guest keeps its resolution.
    EnableMenuItem(menu_, kMenuActualSize, MF_BYCOMMAND | (autoResize_ ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu_, kMenuCaptureKeyboard, MF_BYCOMMAND | (keyboardHook_ != nullptr ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(menu_, kMenuCaptureKeyboard,
                  MF_BYCOMMAND | (captureKeyboard_ && keyboardHook_ != nullptr ? MF_CHECKED : MF_UNCHECKED));
}

bool DisplayWindow::CapturesKeyboard() const
{
    return captureKeyboard_ && !keyboardSuspended_ && !inMenu_ && !closing_ && GetForegroundWindow() == window_ &&
           !IsIconic(window_);
}

void DisplayWindow::SetKeyboardSuspended(bool suspended)
{
    if (suspended == keyboardSuspended_) return;
    keyboardSuspended_ = suspended;
    UpdateTitle();
}

LRESULT CALLBACK DisplayWindow::KeyboardHook(int code, WPARAM wParam, LPARAM lParam)
{
    auto* self = hookOwner_;
    if (code == HC_ACTION && self != nullptr && self->CapturesKeyboard() &&
        self->OnHookKey(*reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam))) {
        return 1; // Handled for the guest: Windows never sees it.
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

bool DisplayWindow::OnHookKey(const KBDLLHOOKSTRUCT& key)
{
    const DWORD vk = key.vkCode;
    // Lock keys still go through Windows: the host keeps its own Caps/Num
    // Lock state, which the guest is aligned with (WM_KEYDOWN path).
    if (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL) return false;
    auto scan = static_cast<std::uint8_t>(key.scanCode);
    bool extended = (key.flags & LLKHF_EXTENDED) != 0;
    for (const auto& media : kMediaKeys) {
        if (media.vk == vk) {
            scan = media.scan;
            extended = true;
            break;
        }
    }
    if (scan == 0) return false; // Synthetic key without a scan code: let Windows handle it.
    if (scan == 0x2A || scan == 0x36) extended = false; // Shifts are never E0-prefixed on the wire.
    const bool released = (key.flags & LLKHF_UP) != 0;
    // Same path as WM_KEYDOWN/WM_KEYUP (host key, lock sync, key tracking).
    const auto lParam = static_cast<LPARAM>((static_cast<std::uint32_t>(scan) << 16) | (extended ? 1u << 24 : 0u) |
                                            (released ? 0xC0000000u : 0u) | 1u);
    OnKey(vk, lParam, released);
    return true;
}

void DisplayWindow::SnapSizingWidth(WPARAM edge, RECT& rect)
{
    // The guest kernel builds the requested mode with drm_cvt_mode, which
    // rounds the width down to a multiple of 8: a 1389-pixel window got a
    // 1384-pixel desktop, centred between two thin black bars. Keeping the
    // client width on that grid while dragging gives the guest exactly the
    // window size, shown 1:1.
    RECT frame{};
    const UINT dpi = GetDpiForWindow(window_);
    AdjustWindowRectExForDpi(&frame, static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)), TRUE,
                             static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_EXSTYLE)), dpi != 0 ? dpi : 96);
    const LONG frameWidth = frame.right - frame.left;
    const LONG client = rect.right - rect.left - frameWidth;
    if (client <= 8) return;
    const LONG width = (client + 4) / 8 * 8 + frameWidth; // Nearest multiple of 8.
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) rect.left = rect.right - width;
    else rect.right = rect.left + width;
}

void DisplayWindow::ScheduleGuestResize(UINT delayMs)
{
    if (!autoResize_ || closing_) return;
    SetTimer(window_, kResizeTimer, delayMs, nullptr); // Restarts the countdown.
}

void DisplayWindow::ToggleFullscreen()
{
    if (!fullscreen_) {
        windowedPlacement_.length = sizeof(windowedPlacement_);
        GetWindowPlacement(window_, &windowedPlacement_);
        windowedStyle_ = GetWindowLongPtrW(window_, GWL_STYLE);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
        fullscreen_ = true;
        SetMenu(window_, nullptr);
        SetWindowLongPtrW(window_, GWL_STYLE, (windowedStyle_ & ~(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE)) | WS_POPUP);
        SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        ShowFullscreenBar(3000);
    } else {
        fullscreen_ = false;
        HideFullscreenBar();
        SetWindowLongPtrW(window_, GWL_STYLE, windowedStyle_);
        SetMenu(window_, menu_);
        SetWindowPlacement(window_, &windowedPlacement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    if (mouseCaptured_) CaptureMouse(); // New clip rectangle.
    InvalidateRect(window_, nullptr, FALSE);
    UpdateTitle();
}

void DisplayWindow::ShowFullscreenBar(DWORD durationMs)
{
    if (!fullscreen_) return;
    const UINT dpi = GetDpiForWindow(window_);
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    if (bar_ == nullptr) {
        bar_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kBarClass, L"", WS_POPUP, 0, 0, 1, 1,
                               window_, nullptr, GetModuleHandleW(nullptr), this);
        if (bar_ == nullptr) return;
    }
    if (barFont_ == nullptr) {
        NONCLIENTMETRICSW metrics{sizeof(metrics)};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
        barFont_ = CreateFontIndirectW(&metrics.lfMessageFont);
        metrics.lfMessageFont.lfWeight = FW_SEMIBOLD;
        barBoldFont_ = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
    const int width = scale(560);
    const int height = scale(40);
    const int left = monitor.rcMonitor.left + (monitor.rcMonitor.right - monitor.rcMonitor.left - width) / 2;
    SetWindowPos(bar_, HWND_TOPMOST, left, monitor.rcMonitor.top, width, height, SWP_NOACTIVATE);
    // Rounded lower corners; the upper ones sit against the screen edge.
    const int radius = scale(10);
    SetWindowRgn(bar_, CreateRoundRectRgn(0, -radius, width + 1, height + 1, radius * 2, radius * 2), TRUE);
    barExitButton_ = RECT{width - scale(196), scale(6), width - scale(8), height - scale(6)};
    barMenuButton_ = RECT{barExitButton_.left - scale(88), scale(6), barExitButton_.left - scale(6), height - scale(6)};
    ShowWindow(bar_, SW_SHOWNOACTIVATE);
    InvalidateRect(bar_, nullptr, FALSE);
    barHideAt_ = (std::max)(barHideAt_, GetTickCount64() + durationMs);
    SetTimer(window_, kBarTimer, 250, nullptr);
}

void DisplayWindow::HideFullscreenBar()
{
    KillTimer(window_, kBarTimer);
    barHideAt_ = 0;
    barHot_ = 0;
    if (bar_ != nullptr) ShowWindow(bar_, SW_HIDE);
}

void DisplayWindow::ShowMenuPopup(POINT screen)
{
    UpdateTitle(); // Enable states and check marks.
    // The menu bar's submenus, borrowed (and detached again before destroying the popup).
    HMENU popup = CreatePopupMenu();
    const int count = GetMenuItemCount(menu_);
    for (int index = 0; index < count; ++index) {
        wchar_t text[64] = {};
        GetMenuStringW(menu_, static_cast<UINT>(index), text, 64, MF_BYPOSITION);
        AppendMenuW(popup, MF_POPUP, reinterpret_cast<UINT_PTR>(GetSubMenu(menu_, index)), text);
    }
    TrackPopupMenu(popup, TPM_TOPALIGN | TPM_LEFTALIGN, screen.x, screen.y, 0, window_, nullptr);
    while (GetMenuItemCount(popup) > 0) RemoveMenu(popup, 0, MF_BYPOSITION);
    DestroyMenu(popup);
}

LRESULT CALLBACK DisplayWindow::BarProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
    }
    auto* self = reinterpret_cast<DisplayWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self == nullptr) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; // The keyboard stays with the guest.
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        self->PaintBar();
        return 0;
    case WM_MOUSEMOVE: {
        const POINT point{static_cast<SHORT>(LOWORD(lParam)), static_cast<SHORT>(HIWORD(lParam))};
        const int hot = PtInRect(&self->barMenuButton_, point) ? 1 : PtInRect(&self->barExitButton_, point) ? 2 : 0;
        if (hot != self->barHot_) {
            self->barHot_ = hot;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        self->barHot_ = 0;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const POINT point{static_cast<SHORT>(LOWORD(lParam)), static_cast<SHORT>(HIWORD(lParam))};
        if (PtInRect(&self->barExitButton_, point)) {
            self->ToggleFullscreen();
        } else if (PtInRect(&self->barMenuButton_, point)) {
            POINT screen{self->barMenuButton_.left, self->barMenuButton_.bottom};
            ClientToScreen(window, &screen);
            self->barHideAt_ = GetTickCount64() + 60'000; // Stays while the menu is open.
            self->ShowMenuPopup(screen);
            self->barHideAt_ = GetTickCount64() + 1500;
        }
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void DisplayWindow::PaintBar()
{
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(bar_, &paint);
    RECT client{};
    GetClientRect(bar_, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    const auto previousBitmap = SelectObject(dc, bitmap);
    const UINT dpi = GetDpiForWindow(bar_);
    const auto scale = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };

    const auto fill = [dc](const RECT& rect, COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    };
    const auto text = [dc](const std::wstring& value, RECT rect, HFONT font, COLORREF color, UINT format) {
        const auto previous = SelectObject(dc, font);
        SetTextColor(dc, color);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect, format | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, previous);
    };
    fill(client, RGB(32, 32, 32));
    RECT title{scale(14), 0, barMenuButton_.left - scale(8), client.bottom};
    text(name_, title, barBoldFont_, RGB(255, 255, 255), DT_LEFT | DT_END_ELLIPSIS);
    const auto button = [&](const RECT& rect, bool hot, const wchar_t* label) {
        if (hot) {
            HBRUSH brush = CreateSolidBrush(RGB(70, 70, 70));
            HGDIOBJ previousBrush = SelectObject(dc, brush);
            HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, rect.left, rect.top, rect.right + 1, rect.bottom + 1, scale(8), scale(8));
            SelectObject(dc, previousPen);
            SelectObject(dc, previousBrush);
            DeleteObject(brush);
        }
        text(label, rect, barFont_, RGB(235, 235, 235), DT_CENTER);
    };
    button(barMenuButton_, barHot_ == 1, L"Menu ▾");
    button(barExitButton_, barHot_ == 2, L"Quitter le plein écran");
    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previousBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(bar_, &paint);
}

void DisplayWindow::SendScanCode(std::uint8_t code)
{
    if (const auto vm = slot_.Get()) vm->KeyboardScanCode(code);
}

void DisplayWindow::OnKey(WPARAM wParam, LPARAM lParam, bool released)
{
    const auto scan = static_cast<std::uint8_t>((lParam >> 16) & 0xFF);
    bool extended = (lParam & (1 << 24)) != 0;
    // Right Ctrl is the host key: it releases the mouse and is not forwarded.
    // A tap (no other key meanwhile) also hands the keyboard back to Windows
    // until the next click in the window. Right Ctrl + F toggles full screen.
    if (scan == 0x1D && extended) {
        if (!released) {
            if (!hostKeyDown_) hostKeyAlone_ = true; // Not on auto-repeat.
            ReleaseMouse();
        } else if (hostKeyDown_ && hostKeyAlone_ && CapturesKeyboard()) {
            SetKeyboardSuspended(true);
        }
        hostKeyDown_ = !released;
        return;
    }
    if (!released) hostKeyAlone_ = false;
    if (wParam == 'F' && (swallowF_ || hostKeyDown_)) {
        // Posted: this may run inside the low-level keyboard hook, which must return quickly.
        if (!released && !swallowF_) PostMessageW(window_, WM_COMMAND, kMenuFullscreen, 0);
        swallowF_ = !released;
        return;
    }
    if (wParam == VK_PAUSE) {
        if (!released) {
            for (const std::uint8_t code : std::array<std::uint8_t, 6>{0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5}) SendScanCode(code);
        }
        return;
    }
    if (scan == 0) return;
    // Windows flags Num Lock as extended, but on the wire it is the plain
    // set-1 code 0x45 (E0 45 means nothing to the guest).
    if (wParam == VK_NUMLOCK) extended = false;
    if (wParam == VK_NUMLOCK || wParam == VK_CAPITAL) {
        // Both sides toggle: wait for the guest LEDs before syncing again.
        lockSyncHoldUntil_ = GetTickCount64() + 500;
    } else if (!released) {
        SyncLockKeys();
    }
    const auto key = static_cast<std::uint16_t>((extended ? 0xE000 : 0) | scan);
    if (released) pressedKeys_.erase(key);
    else pressedKeys_.insert(key);
    if (extended) SendScanCode(0xE0);
    SendScanCode(static_cast<std::uint8_t>(scan | (released ? 0x80 : 0)));
}

void DisplayWindow::SyncLockKeys()
{
    // Align the guest Num Lock and Caps Lock with the host (the guest may
    // have booted with Num Lock off, or the state changed while the window
    // was in the background), by tapping the key in the guest.
    const auto now = GetTickCount64();
    if (now < lockSyncHoldUntil_) return;
    const auto vm = slot_.Get();
    if (!vm) return;
    const int leds = vm->KeyboardLeds();
    if (leds < 0) return; // The guest driver has not set the LEDs yet.
    bool tapped = false;
    if (((GetKeyState(VK_NUMLOCK) & 1) != 0) != ((leds & 0x02) != 0)) {
        SendScanCode(0x45);
        SendScanCode(0xC5);
        tapped = true;
    }
    if (((GetKeyState(VK_CAPITAL) & 1) != 0) != ((leds & 0x04) != 0)) {
        SendScanCode(0x3A);
        SendScanCode(0xBA);
        tapped = true;
    }
    if (tapped) lockSyncHoldUntil_ = now + 500;
}

void DisplayWindow::ReleaseAllKeys()
{
    for (const auto key : pressedKeys_) {
        if ((key & 0xE000) != 0) SendScanCode(0xE0);
        SendScanCode(static_cast<std::uint8_t>((key & 0xFF) | 0x80));
    }
    pressedKeys_.clear();
}

void DisplayWindow::CaptureMouse()
{
    RECT client{};
    GetClientRect(window_, &client);
    POINT corners[2] = {{client.left, client.top}, {client.right, client.bottom}};
    MapWindowPoints(window_, nullptr, corners, 2);
    RECT clip{corners[0].x, corners[0].y, corners[1].x, corners[1].y};
    ClipCursor(&clip);
    if (!mouseCaptured_) {
        RAWINPUTDEVICE device{};
        device.usUsagePage = 0x01;
        device.usUsage = 0x02;
        device.dwFlags = 0;
        device.hwndTarget = window_;
        RegisterRawInputDevices(&device, 1, sizeof(device));
        while (ShowCursor(FALSE) >= 0) {}
        mouseCaptured_ = true;
        mouseButtons_ = 0;
        UpdateTitle();
    }
}

void DisplayWindow::ReleaseMouse()
{
    if (!mouseCaptured_) return;
    ClipCursor(nullptr);
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = RIDEV_REMOVE;
    device.hwndTarget = nullptr;
    RegisterRawInputDevices(&device, 1, sizeof(device));
    while (ShowCursor(TRUE) < 0) {}
    mouseCaptured_ = false;
    if (mouseButtons_ != 0) {
        mouseButtons_ = 0;
        if (const auto vm = slot_.Get()) vm->MouseMotion(0, 0, 0, 0);
    }
    UpdateTitle();
}

void DisplayWindow::OnRawInput(LPARAM lParam)
{
    RAWINPUT input{};
    UINT size = sizeof(input);
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) return;
    if (!mouseCaptured_ || input.header.dwType != RIM_TYPEMOUSE) return;
    const auto& mouse = input.data.mouse;
    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) return;
    const auto flags = mouse.usButtonFlags;
    if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) mouseButtons_ |= 0x01;
    if (flags & RI_MOUSE_LEFT_BUTTON_UP) mouseButtons_ &= ~0x01;
    if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) mouseButtons_ |= 0x02;
    if (flags & RI_MOUSE_RIGHT_BUTTON_UP) mouseButtons_ &= ~0x02;
    if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) mouseButtons_ |= 0x04;
    if (flags & RI_MOUSE_MIDDLE_BUTTON_UP) mouseButtons_ &= ~0x04;
    int wheel = 0;
    if (flags & RI_MOUSE_WHEEL) wheel = -static_cast<SHORT>(mouse.usButtonData) / WHEEL_DELTA;
    if (const auto vm = slot_.Get()) vm->MouseMotion(mouse.lLastX, mouse.lLastY, wheel, mouseButtons_);
}

void DisplayWindow::ResizeToGuest()
{
    if (fullscreen_) ToggleFullscreen();
    const auto width = screenWidth_ != 0 ? screenWidth_ : guestWidth_;
    const auto height = screenHeight_ != 0 ? screenHeight_ : guestHeight_;
    RECT bounds{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRectEx(&bounds, WS_OVERLAPPEDWINDOW, TRUE, 0);
    wasMaximized_ = false; // Restoring here must not resize the guest.
    ShowWindow(window_, SW_RESTORE);
    SetWindowPos(window_, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    RECT client{};
    GetClientRect(window_, &client);
    lastClientSize_ = {client.right, client.bottom};
}

void DisplayWindow::OnCommand(UINT id)
{
    const auto vm = slot_.Get();
    switch (id) {
    case kMenuPause: if (vm) vm->Pause(); break;
    case kMenuResume: if (vm) vm->Resume(); break;
    case kMenuCtrlAltDel:
        for (const std::uint8_t code : std::array<std::uint8_t, 8>{0x1D, 0x38, 0xE0, 0x53, 0xE0, 0xD3, 0xB8, 0x9D}) SendScanCode(code);
        break;
    case kMenuAcpiShutdown:
        if (vm && !vm->AcpiShutdown()) {
            MessageBoxW(window_, L"Le système invité n'a pas (encore) activé le bouton d'alimentation ACPI.",
                        name_.c_str(), MB_ICONINFORMATION);
        }
        break;
    case kMenuReset:
        if (vm && MessageBoxW(window_, L"Redémarrer la machine virtuelle ? Les données non enregistrées du système invité seront perdues.",
                              name_.c_str(), MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
            vm->Reset();
        }
        break;
    case kMenuPowerOff:
        if (vm && MessageBoxW(window_, L"Éteindre immédiatement la machine virtuelle ?",
                              name_.c_str(), MB_OKCANCEL | MB_ICONWARNING) == IDOK) {
            vm->PowerOff();
        }
        break;
    case kMenuActualSize:
        ResizeToGuest();
        break;
    case kMenuFullscreen:
        ToggleFullscreen();
        break;
    case kMenuAutoResize:
        if (!autoResize_) {
            autoResize_ = true;
            ScheduleGuestResize(30); // Catch up with the current window size.
        }
        UpdateTitle();
        break;
    case kMenuFixedResolution:
        autoResize_ = false;
        KillTimer(window_, kResizeTimer);
        UpdateTitle();
        break;
    case kMenuMute:
        audioMuted_ = !audioMuted_;
        if (vm) vm->SetAudioMuted(audioMuted_);
        break;
    case kMenuCaptureKeyboard:
        captureKeyboard_ = !captureKeyboard_;
        keyboardSuspended_ = false;
        break;
    case kMenuHelp:
        MessageBoxW(window_,
            L"• Une fois le système invité démarré (pilote virtio-input), la souris suit directement le "
            L"curseur Windows, sans capture.\n"
            L"• Avant cela, cliquer dans l'écran capture la souris ; Ctrl droit la libère.\n"
            L"• Quand la fenêtre est au premier plan, le clavier va entièrement à la machine : touche Windows, "
            L"Alt+Tab, touches de volume et multimédia comprises (Ctrl+Alt+Suppr reste à Windows : menu Machine). "
            L"Un appui seul sur Ctrl droit rend le clavier à Windows ; un clic dans la fenêtre le reprend.\n"
            L"• Le clavier transmet les touches physiques : choisis la disposition (ex. français AZERTY) "
            L"dans le système invité.\n"
            L"• Avec la carte virtio-gpu, étirer ou agrandir la fenêtre change la résolution de l'invité en "
            L"direct (menu Affichage pour le désactiver) ; sinon l'image garde ses proportions.\n"
            L"• Ctrl droit + F : plein écran. En plein écran, amener la souris en haut de l'écran affiche "
            L"une barre avec le menu et le bouton de sortie.\n"
            L"• Le son sort sur le périphérique audio par défaut de Windows ; le menu Périphériques le coupe.\n"
            L"• Fermer la fenêtre propose un arrêt propre (ACPI) ou immédiat. Si GNOME met la machine en veille "
            L"au lieu de l'éteindre, une touche la réveille ; règle « Bouton d'alimentation » sur « Éteindre » "
            L"dans les paramètres d'énergie de l'invité.",
            L"Virtualiseur — aide", MB_ICONINFORMATION);
        break;
    default:
        break;
    }
    UpdateTitle();
}

} // namespace vmm
