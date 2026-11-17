#pragma once

#include "VmLibrary.h"

#include <vector>

namespace manager {

// Settings pages, in the order of the category list.
enum SettingsPage : int {
    kPageGeneral,
    kPageSystem,
    kPageDisplay,
    kPageStorage,
    kPageAudio,
    kPageNetwork,
    kPageShares,
    kPageInstall,
    kPageCount,
};

// VirtualBox-style settings window. Returns true when the user pressed OK;
// `config` then holds the validated settings (disk path never empty).
bool ShowSettingsDialog(HWND owner, const VmEntry& vm, const std::vector<VmEntry>& library, VmConfig& config,
                        int startPage = kPageGeneral);

struct NewVmRequest {
    VmConfig config;
    bool startNow = true;
};

// "New virtual machine" window: name, ISO, hardware sliders and options.
bool ShowNewVmDialog(HWND owner, const fs::path& library, const std::vector<VmEntry>& existing, NewVmRequest& request);

// Disk snapshots of a machine: take (stopped), restore, delete. Returns true if
// something changed.
bool ShowSnapshotsDialog(HWND owner, const VmEntry& vm, bool stopped);

} // namespace manager
