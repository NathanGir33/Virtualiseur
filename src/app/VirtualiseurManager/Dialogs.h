#pragma once

#include "Ui.h"
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
// One color per kind of setting, the same in the settings list and the
// machine's cards (pages past kPageCount: the snapshots card).
inline COLORREF PageColor(int page)
{
    switch (page) {
    case kPageGeneral: return ui::color::kSlate;
    case kPageSystem: return ui::color::kAccent;
    case kPageDisplay: return ui::color::kPurple;
    case kPageStorage: return ui::color::kTeal;
    case kPageAudio: return ui::color::kPink;
    case kPageNetwork: return ui::color::kGreen;
    case kPageShares: return ui::color::kAmber;
    case kPageInstall: return ui::color::kSlate;
    default: return ui::color::kIndigo;
    }
}

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

// Export to a .vrtx file (Machine → Exporter…, Instantanés → Exporter…):
// asks for the file, then exports with a progress window (Annuler). Snapshot 0:
// the current state, machine off. Returns a status message, empty if the
// user gave up before exporting (failures are also shown in a message box).
std::wstring ShowExportDialog(HWND owner, const VmEntry& vm, bool stopped, std::uint32_t snapshot);

// Import of a .vrtx file (Fichier → Importer une machine…): file, name, then
// the copy with a progress window. Returns the new machine's name, empty if
// nothing was imported; `message` is set for the status bar.
std::wstring ShowImportDialog(HWND owner, const fs::path& library, const std::vector<VmEntry>& vms, std::wstring& message);

} // namespace manager
