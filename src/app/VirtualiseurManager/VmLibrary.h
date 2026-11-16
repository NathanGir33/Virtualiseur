#pragma once

// VM library: each VM lives in <library>\<name>\ with a vm.ini file, its raw
// disk image and the logs of its last run. Running VMs are separate
// VirtualiseurVM.exe processes controlled through a named pipe
// (\\.\pipe\Virtualiseur-<name>): status, pause, resume, acpi-shutdown, poweroff.

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace manager {

namespace fs = std::filesystem;

enum class RunState { Stopped, Starting, Running, Paused };
enum class DiskBus { Virtio, Ide };
enum class BootOrder { Auto, Cdrom, Disk };
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
    bool graphicalInstaller = false;
    bool guestTools = true;
    fs::path preseed;
    BootOrder boot = BootOrder::Auto;
    bool rtcLocal = false;
    std::wstring os; // Guest system detected from the ISO name ("Debian 13"), may be empty.
};

struct VmEntry {
    VmConfig config;
    fs::path directory;
    RunState state = RunState::Stopped;
    HANDLE process = nullptr;
    // Disk image facts (RefreshDiskInfo).
    bool diskExists = false;
    std::uint64_t diskBytes = 0;
    std::uint64_t diskAllocated = 0;
};

struct HostInfo {
    unsigned logicalCpus = 1;
    unsigned physicalCores = 1;
    unsigned memoryMiB = 4096;
};

struct OsBadge {
    std::wstring family;   // "Debian", "Ubuntu"... or "Linux".
    std::wstring initials; // Two letters shown in the badge.
    COLORREF color;
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

// "Debian 13" from "debian-13.7.0-amd64-netinst.iso"; empty if unknown.
std::wstring DetectOsFromIso(const fs::path& iso);
OsBadge Badge(const VmConfig& config);

// Network helpers.
std::vector<HostAdapterInfo> HostNetworkAdapters();
bool NpcapAvailable();
std::wstring RandomMac();
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
// Starts the VM process; on failure returns false with a message.
bool LaunchVm(VmEntry& vm, std::wstring& message);

// "2 048 Mio", "6,1 Gio".
std::wstring FormatNumber(std::uint64_t value);
std::wstring FormatBytes(std::uint64_t bytes);
const wchar_t* StateText(RunState state);

} // namespace manager
