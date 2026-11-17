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

// Disk snapshots of a machine: take (live through the VM when it runs),
// restore, delete, clone. Returns kSnapshotsChanged | kLibraryChanged flags.
inline constexpr int kSnapshotsChanged = 1;
inline constexpr int kLibraryChanged = 2; // A clone was created.
int ShowSnapshotsDialog(HWND owner, const VmEntry& vm, bool stopped, const fs::path& library, const std::vector<VmEntry>& vms);

} // namespace manager
