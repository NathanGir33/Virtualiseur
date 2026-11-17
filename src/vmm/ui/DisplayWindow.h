#pragma once

#include "../Vm.h"
#include "ClipboardBridge.h"

#include <Windows.h>
#include <d2d1_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace vmm {

// Shared slot holding the VM currently attached to the UI. The VM object is
// recreated on guest reset while the window stays open.
class VmSlot final {
public:
    void Set(std::shared_ptr<Vm> vm)
    {
        const std::lock_guard lock(mutex_);
        vm_ = std::move(vm);
    }
    std::shared_ptr<Vm> Get() const
    {
        const std::lock_guard lock(mutex_);
        return vm_;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<Vm> vm_;
};

// Host window showing the guest screen. Rendering never erases the client
// area (no flicker), scales to any window size with the aspect ratio kept,
// and captures keyboard and mouse input for the guest.
class DisplayWindow final {
public:
    static constexpr UINT kVmStoppedMessage = WM_APP + 1;

    DisplayWindow(std::wstring vmName, VmSlot& slot, std::uint32_t width, std::uint32_t height, bool fullscreen = false);
    ~DisplayWindow();

    HWND Handle() const noexcept { return window_; }
    // Runs the message loop until the window is destroyed.
    int Run();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam);

    void Paint();
    // One display refresh: uploads the framebuffer rows the guest changed and
    // presents them. Driven by the frame timer (monitor refresh rate); the
    // WM_TIMER fallback keeps the screen alive during modal loops.
    void OnFrame(bool fromTimerMessage);
    void UpdateFramePeriod();
    void ArmFrameTimer();
    // Direct2D renderer for graphics mode: GPU scaling, whole window drawn per
    // frame. Returns false when Direct2D is unavailable (GDI fallback).
    bool RenderGraphics(const DisplaySnapshot& snapshot, const DirtyRows& rows);
    // Graphics source: the virtio-gpu scanout once the guest driver runs,
    // the boot framebuffer before. Fills the changed rows when `queryDirty`.
    bool GraphicsSnapshot(Vm& vm, DisplaySnapshot& snapshot, DirtyRows& rows, bool queryDirty);
    // Guest cursor plane: a Windows cursor in tablet mode, drawn otherwise.
    bool DrawsGuestCursor() const noexcept { return gpuMode_ && cursor_.visible && !tabletActive_; }
    void UpdateGuestCursor(bool shapeChanged);
    bool PointerOverGuest(POINT client, std::int32_t& x, std::int32_t& y) const;
    void OnTabletMouse(UINT message, WPARAM wParam, LPARAM lParam);
    void ReleaseTabletButtons();
    void RequestGuestResize();
    // Debounced guest resolution change after the window size changed.
    void ScheduleGuestResize(UINT delayMs);
    // WM_SIZING: keeps the client width a multiple of 8 (guest mode grid).
    void SnapSizingWidth(WPARAM edge, RECT& rect);
    // Full screen on the window's monitor; a slim bar at the top edge gives
    // the menu and the way out (also Right Ctrl + F).
    void ToggleFullscreen();
    void ShowFullscreenBar(DWORD durationMs);
    void HideFullscreenBar();
    static LRESULT CALLBACK BarProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void PaintBar();
    void ShowMenuPopup(POINT screen);
    // Keyboard capture (as in VirtualBox): while the VM window is in front,
    // a low-level hook sends the keys Windows would keep for itself (Windows
    // key, Alt+Tab, media and volume keys...) to the guest instead.
    static LRESULT CALLBACK KeyboardHook(int code, WPARAM wParam, LPARAM lParam);
    bool CapturesKeyboard() const;
    bool OnHookKey(const KBDLLHOOKSTRUCT& key);
    void SetKeyboardSuspended(bool suspended);
    bool EnsureD2D();
    void ReleaseD2D();
    bool CreateTargetBitmap();
    void ResizeSwapChain(UINT width, UINT height);
    // Tells the VM whether the screen is drawn from the shared GPU texture.
    void SyncTextureDisplay(Vm& vm);
    void PaintGraphics(HDC dc, const RECT& client, const DisplaySnapshot& snapshot);
    void PaintText(HDC dc, const RECT& client, const DisplaySnapshot& snapshot);
    RECT FitRect(const RECT& client, std::uint32_t width, std::uint32_t height) const;
    void UpdateTitle();
    // CD-ROM and network cable items (read from the VM when a menu opens).
    void UpdateDeviceMenu();
    void ChooseCdromImage(Vm& vm);
    void OnKey(WPARAM wParam, LPARAM lParam, bool released);
    void SendScanCode(std::uint8_t code);
    void ReleaseAllKeys();
    void SyncLockKeys();
    void CaptureMouse();
    void ReleaseMouse();
    void OnRawInput(LPARAM lParam);
    void OnCommand(UINT id);
    void ResizeToGuest();

    std::wstring name_;
    VmSlot& slot_;
    HWND window_ = nullptr;
    HMENU menu_ = nullptr;
    std::uint32_t guestWidth_;
    std::uint32_t guestHeight_;
    bool mouseCaptured_ = false;
    bool audioMuted_ = false;
    bool autoResize_ = true;       // Guest resolution follows the window size.
    bool hostKeyDown_ = false;     // Right Ctrl held (host key combinations).
    bool swallowF_ = false;        // The F of Right Ctrl + F is not sent to the guest.
    bool hostKeyAlone_ = false;    // Right Ctrl pressed without another key (a tap).
    HHOOK keyboardHook_ = nullptr;
    bool captureKeyboard_ = true;  // Setting (menu Périphériques).
    bool keyboardSuspended_ = false; // Host key tapped: keys go to Windows until a click.
    bool inMenu_ = false;          // Menus navigate with the keyboard: no capture.
    static DisplayWindow* hookOwner_;

    // Full screen.
    bool fullscreen_ = false;
    WINDOWPLACEMENT windowedPlacement_{};
    LONG_PTR windowedStyle_ = 0;
    HWND bar_ = nullptr;
    HFONT barFont_ = nullptr;
    HFONT barBoldFont_ = nullptr;
    RECT barMenuButton_{};
    RECT barExitButton_{};
    int barHot_ = 0;               // 1 menu, 2 exit.
    ULONGLONG barHideAt_ = 0;
    std::uint8_t mouseButtons_ = 0;
    std::set<std::uint16_t> pressedKeys_;
    ULONGLONG lockSyncHoldUntil_ = 0;
    std::wstring lastTitle_;
    const Vm* lastVm_ = nullptr;
    RECT paintRect_{};
    std::unique_ptr<ClipboardBridge> clipboard_; // Shared clipboard and file drops (guest agent).

    // Frame pacing.
    HANDLE frameTimer_ = nullptr;
    LONGLONG framePeriod_ = 166'667; // 100 ns units (60 Hz).
    ULONGLONG lastFrameTick_ = 0;
    bool closing_ = false;

    // Direct2D graphics renderer: a device context on the process-wide
    // Direct3D 11 device (SharedD3DDevice) drawing into a flip-model swap
    // chain, so it can show the virgl screen texture without a copy.
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2dTarget_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBitmap_; // Swap chain back buffer.
    Microsoft::WRL::ComPtr<ID2D1Bitmap> d2dBitmap_;      // Upload of CPU framebuffers.
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> sharedBitmap_;  // The shared virgl texture.
    std::uint64_t sharedGeneration_ = ~0ull;
    const Vm* textureDisplayVm_ = nullptr; // VM last told about texture display...
    bool textureDisplay_ = false;          // ...and what it was told.
    std::uint32_t bitmapWidth_ = 0;
    std::uint32_t bitmapHeight_ = 0;
    bool uploadAll_ = true;       // The bitmap no longer mirrors the framebuffer.
    bool d2dUnavailable_ = false; // Direct2D failed: GDI fallback.
    Microsoft::WRL::ComPtr<ID2D1Bitmap> cursorBitmap_;
    std::uint64_t cursorBitmapSerial_ = ~0ull;

    // virtio-gpu screen and cursor.
    ScanoutCopy gpuCopy_;
    bool gpuMode_ = false;
    GpuCursor cursor_;
    HCURSOR guestCursor_ = nullptr;
    std::uint64_t guestCursorSerial_ = ~0ull;
    double guestCursorScale_ = 0.0;
    std::uint32_t screenWidth_ = 0;  // Guest screen currently shown.
    std::uint32_t screenHeight_ = 0;

    // Absolute pointer (virtio tablet): no capture needed.
    bool tabletActive_ = false;
    std::uint8_t tabletButtons_ = 0;
    int wheelRemainder_ = 0;
    int hwheelRemainder_ = 0;
    bool inSizeMove_ = false;
    bool wasMaximized_ = false;
    SIZE lastClientSize_{};

    // Text mode back buffer.
    HDC textDc_ = nullptr;
    HBITMAP textBitmap_ = nullptr;
    HFONT textFont_ = nullptr;
    int cellWidth_ = 9;
    int cellHeight_ = 16;
    std::vector<std::uint16_t> textShadow_;
    std::uint16_t lastCursor_ = 0xFFFF;
    bool cursorBlink_ = false;
    bool textDirty_ = true;
    DWORD lastBlink_ = 0;
};

} // namespace vmm
