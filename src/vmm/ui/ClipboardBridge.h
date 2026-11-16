#pragma once

#include "../agent/GuestAgent.h"

#include <Windows.h>
#include <shellapi.h>

#include <cstdint>
#include <span>
#include <vector>

namespace vmm {

// Connects the Windows clipboard and file drops of the display window to
// the guest agent. Runs on the UI thread.
//
// Host -> guest: a clipboard change is announced (grab); the guest asks for
// the data when one of its applications pastes. Guest -> host: a guest
// grab is answered by fetching the data at once and placing it on the
// Windows clipboard (Windows needs it synchronously at paste time).
class ClipboardBridge final {
public:
    explicit ClipboardBridge(HWND window);
    ~ClipboardBridge();

    // WM_CLIPBOARDUPDATE.
    void OnClipboardUpdate(GuestAgent* agent);
    // WM_DROPFILES; returns false if the agent cannot take files.
    void OnDropFiles(HDROP drop, GuestAgent* agent);
    // Periodic: handles the agent's messages (call from the UI timer).
    void Process(GuestAgent* agent);

private:
    void Announce(GuestAgent& agent);
    void Answer(GuestAgent& agent, std::uint32_t type);
    void Receive(GuestAgent& agent, std::uint32_t type, std::span<const std::uint8_t> data);
    std::vector<std::uint8_t> HostText(bool unixLineEnds);
    std::vector<std::uint8_t> HostBmp();
    bool OpenClipboardRetry();

    HWND window_;
    bool comInitialized_ = false;
    bool agentReady_ = false;
    bool hostGrabbed_ = false; // The guest holds a grab announced by the host.
    UINT pngFormat_ = 0;
};

} // namespace vmm
