#pragma once

// VM library: each VM lives in <library>\<name>\ with a vm.ini file, its raw
// disk image and the logs of its last run. Running VMs are separate
// VirtualiseurVM.exe processes controlled through a named pipe
// (\\.\pipe\Virtualiseur-<name>): status, pause, resume, acpi-shutdown, poweroff.

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <array>
#include <vector>

namespace manager {

namespace fs = std::filesystem;

enum class RunState { Stopped, Starting, Running, Paused };
enum class DiskBus { Virtio, Ide };
enum class BootOrder { Auto, Cdrom, Disk };
// UEFI: OVMF starts the boot loader of the ISO or the disk (any system).
// Linux direct: the VMM loads a Linux kernel itself (machines made before UEFI).
enum class Firmware { Uefi, LinuxDirect };
// Network card attachment, as in VirtualBox.
enum class NetworkMode { None, Nat, Bridged, Internal, HostOnly, Disconnected };

// NAT port forwarding: 127.0.0.1:host -> guest port.
struct PortForward {
    bool udp = false;
    unsigned short host = 0;
    unsigned short guest = 0;
};

struct HostAdapterInfo {
    std::wstring name;        // Friendly name ("Wi-Fi"), stored in vm.ini.
    std::wstring description; // "Intel(R) Wi-Fi 6 AX201 160MHz".
    bool wireless = false;
    bool connected = false;
    bool gateway = false;  // Has a default gateway: the host's actual connection.
    bool virtualAdapter = false;
};

// Host folder shared with the guest (virtio-9p), mounted there by its tag.
struct ShareConfig {
    std::wstring tag;
    fs::path path;
    bool readOnly = false;
};
inline constexpr std::size_t kMaxShares = 3;

struct VmConfig {
    std::wstring name;
    unsigned memoryMiB = 2048;
    unsigned cpus = 2;
    fs::path disk;
    unsigned diskGiB = 20;
    DiskBus diskBus = DiskBus::Virtio;
    fs::path iso;
    NetworkMode network = NetworkMode::Nat;
    std::wstring mac;                // "52:54:00:xx:xx:xx", unique per machine.
    std::wstring bridgeAdapter;      // Host adapter friendly name (bridged mode).
    std::wstring internalNetwork = L"intnet";
    std::vector<PortForward> forwards;
    bool fullscreen = false;
    std::wstring resolution = L"1280x800";
    bool virtioGpu = true;
    bool gpu3d = true;
    bool audio = true;
    bool microphone = false; // The guest may record the Windows microphone.
    std::array<ShareConfig, kMaxShares> shares; // Empty path: unused slot.
    std::wstring uuid;     // Unique id (given to clones; empty for older machines).
    fs::path cloneOf;      // Linked clone: the source machine's disk image (its snapshot store).
    bool graphicalInstaller = false;
    bool guestTools = true;
    fs::path preseed;
    BootOrder boot = BootOrder::Auto;
    // New machines: UEFI. A vm.ini without the key: Linux direct (made before UEFI existed).
    Firmware firmware = Firmware::Uefi;
    bool rtcLocal = false;
    std::wstring os; // Guest system detected from the ISO name ("Debian 13"), may be empty.
};

// A guest agent, as the VM reports it in its "status" reply (state of its
// virtio-serial port, opened by the guest daemon).
enum class AgentState { Unknown, Absent, Active, NoPort };

struct VmEntry {
    VmConfig config;
    fs::path directory;
    RunState state = RunState::Stopped;
    HANDLE process = nullptr;
    // While it runs: qemu-guest-agent and spice-vdagent (NoPort without a graphical display).
    AgentState qemuAgent = AgentState::Unknown;
    AgentState spiceAgent = AgentState::Unknown;
    // Disk image facts (RefreshDiskInfo).
    bool diskExists = false;
    std::uint64_t diskBytes = 0;
    std::uint64_t diskAllocated = 0;
    std::size_t snapshotCount = 0;
};

struct HostInfo {
    unsigned logicalCpus = 1;
    unsigned physicalCores = 1;
    unsigned memoryMiB = 4096;
};

constexpr const wchar_t* kResolutions[] = {L"1024x768", L"1280x720", L"1280x800", L"1366x768", L"1440x900",
                                           L"1600x900", L"1680x1050", L"1920x1080", L"1920x1200", L"2560x1440"};

const HostInfo& Host();
fs::path ExecutableDirectory();
fs::path FindVmExecutable();
// virglrenderer next to VirtualiseurVM.exe: 3D acceleration possible.
bool VirglAvailable();

std::vector<VmEntry> LoadVmLibrary(const fs::path& root, std::vector<VmEntry>& previous);
void SaveConfig(const VmEntry& entry);
void RefreshDiskInfo(VmEntry& entry);

// UEFI variable store of a machine: <machine folder>\vars.fd, created from
// the template at its first UEFI start. Each machine has its own, linked
// clones included: the VM opens it exclusively and the firmware rewrites it.
fs::path VarsStorePath(const VmEntry& vm);
// The blank store shipped with OVMF (firmware\OVMF_VARS_4M.fd next to
// VirtualiseurVM.exe); empty if missing.
fs::path VarsTemplate();
// Copies a variable store to `to` (which must not exist) through a .tmp file
// renamed at the end. The source is read shared: its VM may be running and
// replacing it. Throws on error.
void CopyVarsStore(const fs::path& from, const fs::path& to);
// Deletes a machine's vars.fd and its leftovers (vars.fd.tmp, vars.fd.lock).
// False, with `message`, if one of them remains.
bool RemoveVarsStore(const VmEntry& vm, std::wstring& message);
const wchar_t* FirmwareText(Firmware firmware);

// "Debian 13" from "debian-13.7.0-amd64-netinst.iso"; empty if unknown.
std::wstring DetectOsFromIso(const fs::path& iso);

// Network helpers.
std::vector<HostAdapterInfo> HostNetworkAdapters();
bool NpcapAvailable();
std::wstring RandomMac();
// Fresh unique id (lowercase UUID) and a MAC address no machine of `vms` uses.
std::wstring NewUuid();
std::wstring UniqueMac(const std::vector<VmEntry>& vms);
bool ValidMac(const std::wstring& mac);
std::wstring FormatForwards(const std::vector<PortForward>& forwards); // "tcp:2222:22;udp:5353:53"
std::vector<PortForward> ParseForwards(const std::wstring& text);
const wchar_t* NetworkModeText(NetworkMode mode);

std::wstring QuoteArgument(const std::wstring& argument);
bool PathExists(const fs::path& path);
bool CreateSparseDisk(const fs::path& path, std::uint64_t bytes);
// Grows a raw disk image (the file stays sparse). Never shrinks.
bool ExtendDisk(const fs::path& path, std::uint64_t bytes);
std::uint64_t FreeSpace(const fs::path& path);

// Sends one control command to a running VM; nullopt if it does not answer.
std::optional<std::string> SendCommand(const std::wstring& vmName, const char* command);

// Clone of a machine, from its current state (snapshot 0) or a snapshot.
// Full: the state is flattened into a new sparse disk.img (no history).
// Linked: disk.layer over a frozen layer of the source (a snapshot is taken
// first from the current state, frozen through the VM if it runs), recorded
// in the source's snapshot store. New name, MAC address and id.
struct CloneRequest {
    std::wstring name;
    bool linked = false;
    std::uint32_t snapshot = 0;
};
bool CloneVm(const VmEntry& source, bool sourceStopped, const CloneRequest& request, const fs::path& library,
             const std::vector<VmEntry>& vms, const std::function<bool(std::uint64_t, std::uint64_t)>& progress,
             std::wstring& message);
// Export to a single .vrtx file (MachineArchive): the settings, one
// flattened disk state, without snapshot history, and for a UEFI machine its
// variable store (vars.fd, current: snapshots do not keep it; none for a
// Linux direct machine or a UEFI one never started). `snapshot` 0 exports the
// current state, which needs the machine off. Identity (uuid, linked-clone
// source) is not exported; ISO, preseed and shared folders stay host paths.
// `progress` returns false to cancel: nothing is left behind (`cancelled` set).
using Progress = std::function<bool(std::uint64_t, std::uint64_t)>;
bool ExportVm(const VmEntry& vm, bool stopped, std::uint32_t snapshot, const fs::path& target, const Progress& progress,
              bool& cancelled, std::wstring& message);

// What a .vrtx file holds, read before importing it (header and settings only).
struct ArchivePreview {
    std::wstring name;
    std::wstring os;
    unsigned memoryMiB = 0;
    unsigned cpus = 0;
    std::uint64_t diskBytes = 0;
    std::uint32_t version = 0;
    Firmware firmware = Firmware::LinuxDirect; // Version 1 files: Linux direct.
    bool hasVars = false;
};
bool PreviewArchive(const fs::path& archive, ArchivePreview& preview, std::wstring& message);
// Checks a new machine name (valid characters, no machine or folder with it).
bool CheckNewVmName(const std::wstring& name, const fs::path& library, const std::vector<VmEntry>& vms, std::wstring& message);
// Imports a .vrtx file as machine `name`: new sparse disk.img, new MAC address
// and id; host paths that do not exist here (ISO, preseed, shares) are cleared.
// A UEFI machine gets the archive's vars.fd, or a blank one from the
// template when the archive has none (version 1, never started).
// On error or cancellation the machine folder is removed entirely.
bool ImportVm(const fs::path& archive, const std::wstring& name, const fs::path& library, const std::vector<VmEntry>& vms,
              const Progress& progress, bool& cancelled, std::wstring& message);

// The guest tools CD shipped next to the manager (empty if missing).
fs::path GuestToolsImage();

// Starts the VM process; on failure returns false with a message.
bool LaunchVm(VmEntry& vm, std::wstring& message);

// "2 048 Mio", "6,1 Gio".
std::wstring FormatNumber(std::uint64_t value);
std::wstring FormatBytes(std::uint64_t bytes);
const wchar_t* StateText(RunState state);

} // namespace manager
