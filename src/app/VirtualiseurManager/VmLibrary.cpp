#include <winsock2.h>
#include <iphlpapi.h>

#include "VmLibrary.h"

#include "DiskLayer.h"
#include "DiskSnapshots.h"
#include "MachineArchive.h"
#include "VirtualDisk.h"

#include <winioctl.h>

#include <random>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <map>

namespace manager {
namespace {

std::wstring IniRead(const fs::path& file, const wchar_t* key, const wchar_t* fallback = L"")
{
    std::array<wchar_t, 4096> buffer{};
    GetPrivateProfileStringW(L"vm", key, fallback, buffer.data(), static_cast<DWORD>(buffer.size()), file.c_str());
    return buffer.data();
}

unsigned IniNumber(const fs::path& file, const wchar_t* key, unsigned fallback)
{
    return static_cast<unsigned>(GetPrivateProfileIntW(L"vm", key, static_cast<INT>(fallback), file.c_str()));
}

bool IniFlag(const fs::path& file, const wchar_t* key, bool fallback)
{
    const auto value = IniRead(file, key, fallback ? L"on" : L"off");
    return value != L"off" && value != L"0" && value != L"none" && value != L"false";
}

std::wstring Lower(std::wstring text)
{
    for (auto& character : text) character = static_cast<wchar_t>(std::towlower(character));
    return text;
}

NetworkMode ParseNetworkMode(const std::wstring& text)
{
    if (text == L"none") return NetworkMode::None;
    if (text == L"bridged") return NetworkMode::Bridged;
    if (text == L"internal") return NetworkMode::Internal;
    if (text == L"hostonly") return NetworkMode::HostOnly;
    if (text == L"disconnected") return NetworkMode::Disconnected;
    return NetworkMode::Nat;
}

const wchar_t* NetworkModeKey(NetworkMode mode)
{
    switch (mode) {
    case NetworkMode::None: return L"none";
    case NetworkMode::Bridged: return L"bridged";
    case NetworkMode::Internal: return L"internal";
    case NetworkMode::HostOnly: return L"hostonly";
    case NetworkMode::Disconnected: return L"disconnected";
    default: return L"nat";
    }
}

// Stable MAC for machines created before MACs were stored: derived from the
// folder name, so it does not change between runs.
std::wstring DerivedMac(const std::wstring& seed)
{
    std::uint32_t hash = 2166136261u;
    for (const wchar_t ch : seed) {
        hash ^= static_cast<std::uint32_t>(std::towlower(ch));
        hash *= 16777619u;
    }
    wchar_t text[18];
    swprintf_s(text, L"52:54:00:%02x:%02x:%02x", (hash >> 16) & 0xFF, (hash >> 8) & 0xFF, hash & 0xFF);
    return text;
}

// Same rule as the VMM (main.cpp): a pipe name may hold any character but a
// backslash, so the full VM name is kept and two VMs never share a pipe.
std::wstring PipeName(const std::wstring& vmName)
{
    std::wstring name = L"\\\\.\\pipe\\Virtualiseur-";
    for (const wchar_t character : vmName) {
        name.push_back(character == L'\\' || character < 0x20 ? L'_' : character);
    }
    return name;
}

HostInfo QueryHost()
{
    HostInfo info;
    info.logicalCpus = (std::max)(1u, static_cast<unsigned>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    MEMORYSTATUSEX status{sizeof(status)};
    if (GlobalMemoryStatusEx(&status)) info.memoryMiB = static_cast<unsigned>(status.ullTotalPhys / (1024 * 1024));
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    std::vector<std::uint8_t> buffer(length);
    unsigned cores = 0;
    if (length != 0 && GetLogicalProcessorInformationEx(
                           RelationProcessorCore,
                           reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &length)) {
        for (DWORD offset = 0; offset < length;) {
            const auto* item = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
            if (item->Relationship == RelationProcessorCore) ++cores;
            offset += item->Size;
        }
    }
    info.physicalCores = std::clamp(cores, 1u, info.logicalCpus);
    return info;
}

std::optional<VmEntry> LoadEntry(const fs::path& directory)
{
    VmEntry entry;
    entry.directory = directory;
    const auto file = directory / L"vm.ini";
    auto& c = entry.config;
    c.name = directory.filename().wstring();
    if (PathExists(file)) {
        c.name = IniRead(file, L"name", c.name.c_str());
        c.memoryMiB = IniNumber(file, L"memory", 2048);
        c.cpus = (std::max)(1u, IniNumber(file, L"cpus", 2));
        c.disk = IniRead(file, L"disk");
        c.diskGiB = (std::max)(1u, IniNumber(file, L"disk_gib", 20));
        c.diskBus = IniRead(file, L"disk_bus", L"virtio") == L"ide" ? DiskBus::Ide : DiskBus::Virtio;
        c.iso = IniRead(file, L"iso");
        c.network = ParseNetworkMode(IniRead(file, L"network", L"nat"));
        c.mac = IniRead(file, L"mac");
        if (!ValidMac(c.mac)) c.mac = DerivedMac(directory.filename().wstring());
        c.bridgeAdapter = IniRead(file, L"bridge_adapter");
        c.internalNetwork = IniRead(file, L"intnet", L"intnet");
        if (c.internalNetwork.empty()) c.internalNetwork = L"intnet";
        c.forwards = ParseForwards(IniRead(file, L"nat_forward"));
        c.fullscreen = IniFlag(file, L"fullscreen", false);
        c.resolution = IniRead(file, L"resolution", L"1280x800");
        const auto gpu = IniRead(file, L"gpu", L"virgl");
        c.virtioGpu = gpu != L"none";
        c.gpu3d = gpu == L"virgl";
        c.audio = IniFlag(file, L"audio", true);
        c.microphone = IniFlag(file, L"microphone", false);
        for (std::size_t index = 0; index < kMaxShares; ++index) {
            const auto prefix = L"share" + std::to_wstring(index + 1) + L"_";
            auto& share = c.shares[index];
            share.tag = IniRead(file, (prefix + L"tag").c_str());
            share.path = IniRead(file, (prefix + L"path").c_str());
            share.readOnly = IniFlag(file, (prefix + L"ro").c_str(), false);
        }
        c.graphicalInstaller = IniRead(file, L"installer", L"text") == L"gtk";
        c.guestTools = IniFlag(file, L"guest_tools", true);
        c.preseed = IniRead(file, L"preseed");
        const auto boot = IniRead(file, L"boot", L"auto");
        c.boot = boot == L"cdrom" ? BootOrder::Cdrom : boot == L"disk" ? BootOrder::Disk : BootOrder::Auto;
        c.rtcLocal = IniRead(file, L"rtc", L"utc") == L"local";
        c.os = IniRead(file, L"os");
        if (c.os.empty()) c.os = DetectOsFromIso(c.iso);
        c.uuid = IniRead(file, L"uuid");
        c.cloneOf = IniRead(file, L"clone_of");
        if (c.disk.empty()) c.disk = directory / L"disk.img";
        RefreshDiskInfo(entry);
        return entry;
    }
    // Import a VM folder created by older versions (disk image without vm.ini).
    if (PathExists(directory / L"disk.img")) {
        c.disk = directory / L"disk.img";
        std::error_code error;
        c.diskGiB = static_cast<unsigned>(fs::file_size(c.disk, error) / (1024ull * 1024 * 1024));
        if (c.diskGiB == 0) c.diskGiB = 20;
        c.mac = DerivedMac(directory.filename().wstring());
        SaveConfig(entry);
        RefreshDiskInfo(entry);
        return entry;
    }
    return std::nullopt;
}

} // namespace

const HostInfo& Host()
{
    static const HostInfo info = QueryHost();
    return info;
}

bool PathExists(const fs::path& path)
{
    std::error_code error;
    return !path.empty() && fs::exists(path, error) && !error;
}

std::wstring QuoteArgument(const std::wstring& argument)
{
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(ch);
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(ch);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

fs::path ExecutableDirectory()
{
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    return fs::path(path.data()).parent_path();
}

fs::path FindVmExecutable()
{
    const auto directory = ExecutableDirectory();
    for (const auto& candidate : {
             directory / L"VirtualiseurVM.exe",
             (directory / L".." / L".." / L".." / L".." / L".." / L"vmm" / L"out" / L"x64" / L"Release" /
              L"VirtualiseurVM.exe").lexically_normal(),
             (directory / L".." / L".." / L".." / L".." / L".." / L"vmm" / L"out" / L"x64" / L"Debug" /
              L"VirtualiseurVM.exe").lexically_normal()}) {
        if (PathExists(candidate)) return candidate;
    }
    return {};
}

bool VirglAvailable()
{
    const auto vm = FindVmExecutable();
    return !vm.empty() && PathExists(vm.parent_path() / L"libvirglrenderer-1.dll");
}

std::vector<VmEntry> LoadVmLibrary(const fs::path& root, std::vector<VmEntry>& previous)
{
    // Keep the process handles of VMs launched by this manager across reloads.
    std::map<std::wstring, HANDLE> processes;
    for (auto& vm : previous) {
        if (vm.process != nullptr) processes[vm.config.name] = vm.process;
        vm.process = nullptr;
    }
    std::vector<VmEntry> result;
    std::error_code error;
    fs::create_directories(root, error);
    for (const auto& item : fs::directory_iterator(root, error)) {
        if (!item.is_directory()) continue;
        if (auto entry = LoadEntry(item.path())) {
            if (const auto found = processes.find(entry->config.name); found != processes.end()) {
                entry->process = found->second;
                processes.erase(found);
            }
            for (const auto& old : previous) {
                if (old.config.name == entry->config.name) entry->state = old.state;
            }
            result.push_back(std::move(*entry));
        }
    }
    for (auto& [name, handle] : processes) CloseHandle(handle);
    std::sort(result.begin(), result.end(), [](const VmEntry& a, const VmEntry& b) {
        return _wcsicmp(a.config.name.c_str(), b.config.name.c_str()) < 0;
    });
    return result;
}

void SaveConfig(const VmEntry& entry)
{
    std::error_code error;
    fs::create_directories(entry.directory, error);
    const auto file = entry.directory / L"vm.ini";
    if (!PathExists(file)) {
        // A UTF-16 BOM makes the profile API store Unicode paths and names.
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            const wchar_t bom = 0xFEFF;
            DWORD written = 0;
            WriteFile(handle, &bom, sizeof(bom), &written, nullptr);
            CloseHandle(handle);
        }
    }
    const auto& c = entry.config;
    const auto put = [&](const wchar_t* key, const std::wstring& value) {
        WritePrivateProfileStringW(L"vm", key, value.c_str(), file.c_str());
    };
    put(L"name", c.name);
    WritePrivateProfileStringW(L"vm", L"engine", nullptr, file.c_str()); // Old key (single engine now).
    put(L"memory", std::to_wstring(c.memoryMiB));
    put(L"cpus", std::to_wstring(c.cpus));
    put(L"disk", c.disk.wstring());
    put(L"disk_gib", std::to_wstring(c.diskGiB));
    put(L"disk_bus", c.diskBus == DiskBus::Ide ? L"ide" : L"virtio");
    put(L"iso", c.iso.wstring());
    put(L"network", NetworkModeKey(c.network));
    put(L"mac", c.mac);
    put(L"bridge_adapter", c.bridgeAdapter);
    put(L"intnet", c.internalNetwork);
    put(L"nat_forward", FormatForwards(c.forwards));
    put(L"fullscreen", c.fullscreen ? L"on" : L"off");
    put(L"resolution", c.resolution);
    put(L"gpu", !c.virtioGpu ? L"none" : c.gpu3d ? L"virgl" : L"virtio");
    put(L"audio", c.audio ? L"on" : L"off");
    put(L"microphone", c.microphone ? L"on" : L"off");
    for (std::size_t index = 0; index < kMaxShares; ++index) {
        const auto prefix = L"share" + std::to_wstring(index + 1) + L"_";
        const auto& share = c.shares[index];
        put((prefix + L"tag").c_str(), share.path.empty() ? L"" : share.tag);
        put((prefix + L"path").c_str(), share.path.wstring());
        put((prefix + L"ro").c_str(), share.readOnly ? L"on" : L"off");
    }
    put(L"installer", c.graphicalInstaller ? L"gtk" : L"text");
    put(L"guest_tools", c.guestTools ? L"on" : L"off");
    put(L"preseed", c.preseed.wstring());
    put(L"boot", c.boot == BootOrder::Cdrom ? L"cdrom" : c.boot == BootOrder::Disk ? L"disk" : L"auto");
    put(L"rtc", c.rtcLocal ? L"local" : L"utc");
    put(L"uuid", c.uuid);
    put(L"clone_of", c.cloneOf.wstring());
    put(L"os", c.os);
}

void RefreshDiskInfo(VmEntry& entry)
{
    entry.diskExists = false;
    entry.diskBytes = 0;
    entry.diskAllocated = 0;
    entry.snapshotCount = 0;
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (entry.config.disk.empty() ||
        !GetFileAttributesExW(entry.config.disk.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return;
    }
    entry.diskExists = true;
    entry.diskBytes = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (const auto size = virtualiseur::whp::LayerFile::SizeOf(entry.config.disk); size != 0) entry.diskBytes = size; // A layer: its virtual size.
    DWORD high = 0;
    const DWORD low = GetCompressedFileSizeW(entry.config.disk.c_str(), &high);
    if (low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR) {
        entry.diskAllocated = (static_cast<std::uint64_t>(high) << 32) | low;
    }
    // Snapshots: the layer files count too.
    entry.snapshotCount = 0;
    try {
        const virtualiseur::whp::SnapshotStore store(entry.config.disk);
        entry.snapshotCount = store.Snapshots().size();
        entry.diskAllocated += store.LayerBytes();
    } catch (const std::exception&) {
    }
}

std::wstring DetectOsFromIso(const fs::path& iso)
{
    if (iso.empty()) return {};
    const auto name = Lower(iso.filename().wstring());
    struct Known {
        const wchar_t* key;
        const wchar_t* display;
        bool minorVersion;
    };
    static constexpr Known kKnown[] = {
        {L"debian", L"Debian", false},     {L"kubuntu", L"Kubuntu", true},    {L"xubuntu", L"Xubuntu", true},
        {L"ubuntu", L"Ubuntu", true},      {L"linuxmint", L"Linux Mint", false}, {L"mint", L"Linux Mint", false},
        {L"fedora", L"Fedora", false},     {L"archlinux", L"Arch Linux", false}, {L"arch", L"Arch Linux", false},
        {L"opensuse", L"openSUSE", false}, {L"kali", L"Kali Linux", true},    {L"alpine", L"Alpine Linux", true},
        {L"rocky", L"Rocky Linux", false}, {L"alma", L"AlmaLinux", false},   {L"centos", L"CentOS", false},
        {L"manjaro", L"Manjaro", false},   {L"pop-os", L"Pop!_OS", true},     {L"elementary", L"elementary OS", false},
    };
    for (const auto& known : kKnown) {
        const auto at = name.find(known.key);
        if (at == std::wstring::npos) continue;
        std::wstring result = known.display;
        // First number after the name: "13" (debian-13.7.0) or "24.04" (ubuntu-24.04.1).
        auto position = at + std::wcslen(known.key);
        while (position < name.size() && !std::iswdigit(name[position])) {
            if (std::iswalpha(name[position]) && position > at + std::wcslen(known.key) + 1) break;
            ++position;
        }
        std::wstring version;
        bool dot = false;
        for (; position < name.size(); ++position) {
            const auto ch = name[position];
            if (std::iswdigit(ch)) {
                version.push_back(ch);
            } else if (ch == L'.' && known.minorVersion && !dot && !version.empty()) {
                version.push_back(ch);
                dot = true;
            } else {
                break;
            }
        }
        while (!version.empty() && version.back() == L'.') version.pop_back();
        if (!version.empty() && version.size() <= 8) result += L" " + version;
        return result;
    }
    return {};
}

bool CreateSparseDisk(const fs::path& path, std::uint64_t bytes)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD ignored = 0;
    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(bytes);
    const bool ok = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ignored, nullptr) &&
                    SetFilePointerEx(file, size, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    CloseHandle(file);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

bool ExtendDisk(const fs::path& path, std::uint64_t bytes)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER current{};
    bool ok = GetFileSizeEx(file, &current) != 0;
    if (ok && static_cast<std::uint64_t>(current.QuadPart) < bytes) {
        DWORD ignored = 0;
        DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ignored, nullptr); // Keep it sparse.
        LARGE_INTEGER size{};
        size.QuadPart = static_cast<LONGLONG>(bytes);
        ok = SetFilePointerEx(file, size, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    }
    CloseHandle(file);
    return ok;
}

std::uint64_t FreeSpace(const fs::path& path)
{
    // GetDiskFreeSpaceEx wants an existing directory: walk up from the path.
    fs::path probe = path;
    std::error_code error;
    while (!probe.empty() && !fs::is_directory(probe, error)) {
        const auto parent = probe.parent_path();
        if (parent == probe) break;
        probe = parent;
    }
    ULARGE_INTEGER available{};
    if (probe.empty() || !GetDiskFreeSpaceExW(probe.c_str(), &available, nullptr, nullptr)) return 0;
    return available.QuadPart;
}

std::optional<std::string> SendCommand(const std::wstring& vmName, const char* command)
{
    const auto name = PipeName(vmName);
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(name.c_str(), 300)) return std::nullopt;
        pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return std::nullopt;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
    DWORD written = 0;
    std::string reply;
    if (WriteFile(pipe, command, static_cast<DWORD>(std::strlen(command)), &written, nullptr)) {
        char buffer[512];
        DWORD read = 0;
        if (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr)) reply.assign(buffer, read);
    }
    CloseHandle(pipe);
    return reply;
}

bool LaunchVm(VmEntry& vm, std::wstring& message)
{
    const auto& c = vm.config;
    for (const auto& share : c.shares) {
        if (!share.path.empty() && !PathExists(share.path)) {
            message = L"Le dossier partagé « " + share.tag + L" » est introuvable : " + share.path.wstring();
            return false;
        }
    }
    if (!c.iso.empty() && !PathExists(c.iso)) {
        message = L"L'image ISO est introuvable : " + c.iso.wstring();
        return false;
    }
    if (c.iso.empty() && !PathExists(c.disk)) {
        message = L"Le disque est vide : choisis une image ISO d'installation pour le premier démarrage.";
        return false;
    }

    if (c.disk.extension() == L".qcow2") {
        message = L"Disque qcow2 (ancien moteur QEMU) non pris en charge : Virtualiseur utilise des disques bruts (.img).";
        return false;
    }

    const fs::path executable = FindVmExecutable();
    if (executable.empty()) {
        message = L"VirtualiseurVM.exe est introuvable (compile src\\vmm\\VirtualiseurVM.vcxproj en Release).";
        return false;
    }
    std::wstring arguments;
    {
        const wchar_t* gpu = !c.virtioGpu ? L"none" : (c.gpu3d && VirglAvailable()) ? L"virgl" : L"virtio";
        arguments = L"--name " + QuoteArgument(c.name) +
                    L" --memory " + std::to_wstring(c.memoryMiB) +
                    L" --cpus " + std::to_wstring(c.cpus) +
                    L" --disk " + QuoteArgument(c.disk.wstring()) +
                    L" --disk-size " + std::to_wstring(static_cast<std::uint64_t>(c.diskGiB) * 1024) +
                    L" --disk-bus " + (c.diskBus == DiskBus::Ide ? L"ide" : L"virtio") +
                    L" --display gfx --resolution " + c.resolution +
                    L" --gpu " + gpu +
                    L" --audio " + (c.audio ? L"on" : L"off") +
                    L" --mic " + (c.microphone ? L"on" : L"off") +
                    L" --net " + NetworkModeKey(c.network) +
                    L" --installer " + (c.graphicalInstaller ? L"gtk" : L"text") +
                    L" --guest-tools " + (c.guestTools ? L"on" : L"off") +
                    L" --rtc " + (c.rtcLocal ? L"local" : L"utc") +
                    L" --serial-log " + QuoteArgument((vm.directory / L"serial.log").wstring()) +
                    L" --log " + QuoteArgument((vm.directory / L"vmm.log").wstring());
        if (c.network != NetworkMode::None && ValidMac(c.mac)) arguments += L" --mac " + c.mac;
        if (c.network == NetworkMode::Bridged && !c.bridgeAdapter.empty()) {
            arguments += L" --bridge " + QuoteArgument(c.bridgeAdapter);
        }
        if (c.network == NetworkMode::Internal) arguments += L" --intnet " + QuoteArgument(c.internalNetwork);
        if (c.network == NetworkMode::Nat || c.network == NetworkMode::HostOnly) {
            for (const auto& forward : c.forwards) {
                arguments += L" --nat-forward " + std::wstring(forward.udp ? L"udp:" : L"tcp:") +
                             std::to_wstring(forward.host) + L":" + std::to_wstring(forward.guest);
            }
        }
        if (c.fullscreen) arguments += L" --fullscreen";
        for (const auto& share : c.shares) {
            if (share.path.empty()) continue;
            arguments += L" --share " + QuoteArgument(share.tag + L"=" + share.path.wstring() + (share.readOnly ? L",ro" : L""));
        }
        if (!c.iso.empty()) arguments += L" --iso " + QuoteArgument(c.iso.wstring());
        if (c.boot == BootOrder::Cdrom) arguments += L" --boot cdrom";
        else if (c.boot == BootOrder::Disk) arguments += L" --boot disk";
        if (!c.preseed.empty() && PathExists(c.preseed)) arguments += L" --preseed " + QuoteArgument(c.preseed.wstring());
        std::error_code error;
        fs::remove(vm.directory / L"serial.log", error);
    }

    std::wstring commandLine = QuoteArgument(executable.wstring()) + L" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process)) {
        message = L"Impossible de lancer la VM (erreur Windows " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    CloseHandle(process.hThread);
    if (vm.process != nullptr) CloseHandle(vm.process);
    vm.process = process.hProcess;
    vm.state = RunState::Starting;
    message = L"« " + c.name + L" » démarre dans sa propre fenêtre.";
    return true;
}

std::wstring FormatNumber(std::uint64_t value)
{
    auto digits = std::to_wstring(value);
    std::wstring result;
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index != 0 && (digits.size() - index) % 3 == 0) result.push_back(L'\u00A0');
        result.push_back(digits[index]);
    }
    return result;
}

std::wstring FormatBytes(std::uint64_t bytes)
{
    constexpr const wchar_t* kUnits[] = {L"octets", L"Kio", L"Mio", L"Gio", L"Tio"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t text[64];
    if (unit == 0 || value >= 100.0) swprintf_s(text, L"%.0f\u00A0%s", value, kUnits[unit]);
    else swprintf_s(text, L"%.1f\u00A0%s", value, kUnits[unit]);
    for (auto* ch = text; *ch != 0; ++ch) {
        if (*ch == L'.') *ch = L','; // Décimale à la française.
    }
    return text;
}

std::vector<HostAdapterInfo> HostNetworkAdapters()
{
    std::vector<HostAdapterInfo> result;
    ULONG size = 32 * 1024;
    std::vector<std::uint8_t> buffer;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
                                      GAA_FLAG_INCLUDE_GATEWAYS,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (status != NO_ERROR) return result;
    for (auto* item = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); item != nullptr; item = item->Next) {
        // Physical-style adapters only (Ethernet, Wi-Fi), with a MAC address.
        if ((item->IfType != IF_TYPE_ETHERNET_CSMACD && item->IfType != IF_TYPE_IEEE80211) ||
            item->PhysicalAddressLength != 6 || item->FriendlyName == nullptr) {
            continue;
        }
        HostAdapterInfo adapter;
        adapter.name = item->FriendlyName;
        adapter.description = item->Description != nullptr ? item->Description : L"";
        adapter.wireless = item->IfType == IF_TYPE_IEEE80211;
        adapter.connected = item->OperStatus == IfOperStatusUp;
        adapter.gateway = item->FirstGatewayAddress != nullptr;
        const auto description = Lower(adapter.description);
        for (const auto* marker : {L"virtual", L"host-only", L"tap-windows", L"hyper-v", L"vmware", L"vpn", L"loopback",
                                   L"bluetooth", L"wan miniport", L"tunnel"}) {
            if (description.find(marker) != std::wstring::npos) adapter.virtualAdapter = true;
        }
        result.push_back(std::move(adapter));
    }
    // The host's actual connection first (gateway), then connected physical adapters.
    const auto score = [](const HostAdapterInfo& adapter) {
        return (adapter.gateway ? 4 : 0) + (adapter.connected ? 2 : 0) + (adapter.virtualAdapter ? 0 : 1);
    };
    std::stable_sort(result.begin(), result.end(), [&](const HostAdapterInfo& a, const HostAdapterInfo& b) {
        return score(a) > score(b);
    });
    return result;
}

bool NpcapAvailable()
{
    std::array<wchar_t, MAX_PATH> system{};
    GetSystemDirectoryW(system.data(), MAX_PATH);
    return PathExists(fs::path(system.data()) / L"Npcap" / L"wpcap.dll");
}

std::wstring NewUuid()
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) return {};
    wchar_t text[40] = {};
    StringFromGUID2(guid, text, 40); // {xxxxxxxx-...}
    std::wstring uuid(text + 1, wcslen(text) - 2);
    for (auto& c : uuid) c = static_cast<wchar_t>(std::towlower(c));
    return uuid;
}

std::wstring UniqueMac(const std::vector<VmEntry>& vms)
{
    for (;;) {
        auto mac = RandomMac();
        if (std::none_of(vms.begin(), vms.end(), [&](const VmEntry& vm) { return _wcsicmp(vm.config.mac.c_str(), mac.c_str()) == 0; })) {
            return mac;
        }
    }
}

namespace {

std::string Utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

} // namespace

bool CloneVm(const VmEntry& source, bool sourceStopped, const CloneRequest& request, const fs::path& library,
             const std::vector<VmEntry>& vms, const std::function<bool(std::uint64_t, std::uint64_t)>& progress,
             std::wstring& message)
{
    using virtualiseur::whp::LayerFile;
    using virtualiseur::whp::SnapshotStore;
    const auto& name = request.name;
    if (!CheckNewVmName(name, library, vms, message)) return false;
    const auto directory = library / name;
    try {
        const auto sourceFolder = source.config.disk.parent_path();
        SnapshotStore store(source.config.disk);
        fs::path state;          // What the clone starts from.
        std::wstring pinned;     // Linked clone: the source's frozen layer.
        if (request.snapshot != 0) {
            const auto* snapshot = store.Find(request.snapshot);
            if (snapshot == nullptr) throw std::runtime_error("Instantané introuvable.");
            pinned = snapshot->layer;
        } else if (request.linked) {
            // A linked clone needs a frozen layer: snapshot the current state
            // first (frozen through the VM when it runs).
            const auto snapshotName = L"Clone " + name;
            std::uint32_t id = 0;
            if (sourceStopped) {
                id = store.Take(snapshotName, L"");
            } else {
                const auto reply = SendCommand(source.config.name, ("snapshot-take " + Utf8(snapshotName)).c_str());
                if (!reply || reply->rfind("ok ", 0) != 0) {
                    message = L"Instantané de la machine source impossible : " +
                              (reply ? FromUtf8(reply->rfind("error: ", 0) == 0 ? reply->substr(7) : *reply) : L"elle ne répond pas.");
                    return false;
                }
                id = static_cast<std::uint32_t>(std::strtoul(reply->c_str() + 3, nullptr, 10));
            }
            store = SnapshotStore(source.config.disk);
            const auto* snapshot = store.Find(id);
            if (snapshot == nullptr) throw std::runtime_error("Instantané « Clone » introuvable.");
            pinned = snapshot->layer;
        } else {
            if (!sourceStopped) {
                message = L"Éteins la machine source, ou clone un de ses instantanés : son état actuel change pendant qu'elle tourne.";
                return false;
            }
            state = store.CurrentLayer();
        }
        if (!pinned.empty()) state = sourceFolder / pinned;

        VmEntry clone;
        clone.directory = directory;
        clone.config = source.config;
        clone.config.name = name;
        clone.config.mac = UniqueMac(vms); // Locally administered unicast, unique in the library.
        clone.config.uuid = NewUuid();
        clone.config.cloneOf.clear();
        fs::create_directories(directory);
        try {
            if (request.linked) {
                // A layer whose parent is the source's frozen layer, by relative path.
                clone.config.disk = directory / L"disk.layer";
                const auto parent = state.lexically_normal().lexically_relative(directory.lexically_normal());
                if (parent.empty()) throw std::runtime_error("Chemin relatif vers la machine source impossible.");
                LayerFile::Create(clone.config.disk, parent.wstring(), LayerFile::SizeOf(state));
                clone.config.cloneOf = source.config.disk;
                SaveConfig(clone);
                store.RegisterClone({clone.config.uuid, name,
                                     clone.config.disk.lexically_normal().lexically_relative(sourceFolder.lexically_normal()).wstring(),
                                     pinned});
                message = L"Clone lié « " + name + L" » créé : il dépend des instantanés de « " + source.config.name + L" ».";
            } else {
                clone.config.disk = directory / L"disk.img";
                virtualiseur::whp::VirtualDisk::Flatten(state, clone.config.disk, progress);
                SaveConfig(clone);
                message = L"Clone complet « " + name + L" » créé.";
            }
        } catch (...) {
            std::error_code ignored;
            fs::remove_all(directory, ignored);
            throw;
        }
        return true;
    } catch (const std::exception& error) {
        message = L"Clonage impossible : " + FromUtf8(error.what());
        return false;
    }
}

bool CheckNewVmName(const std::wstring& name, const fs::path& library, const std::vector<VmEntry>& vms, std::wstring& message)
{
    if (name.empty() || name.size() > 80 || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos || name.back() == L'.') {
        message = L"Nom invalide : évite les caractères \\ / : * ? \" < > | et le point final.";
        return false;
    }
    if (PathExists(library / name) || std::any_of(vms.begin(), vms.end(), [&](const VmEntry& vm) {
            return _wcsicmp(vm.config.name.c_str(), name.c_str()) == 0;
        })) {
        message = L"Une machine porte déjà ce nom.";
        return false;
    }
    return true;
}

namespace {

// The settings of an archive: "key=value" lines of the [vm] section, UTF-8.
std::vector<std::pair<std::wstring, std::wstring>> ParseArchiveConfig(const std::string& text)
{
    std::vector<std::pair<std::wstring, std::wstring>> result;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        auto line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto equal = line.find('=');
        if (equal == std::string::npos || equal == 0) continue;
        const auto key = line.substr(0, equal);
        // Only plain keys: nothing that could address another section or file.
        if (key.size() > 32 || !std::all_of(key.begin(), key.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            })) {
            continue;
        }
        result.emplace_back(FromUtf8(key), FromUtf8(line.substr(equal + 1)));
    }
    return result;
}

std::wstring ArchiveValue(const std::vector<std::pair<std::wstring, std::wstring>>& config, const wchar_t* key)
{
    for (const auto& [name, value] : config) {
        if (name == key) return value;
    }
    return {};
}

} // namespace

bool ExportVm(const VmEntry& vm, bool stopped, std::uint32_t snapshot, const fs::path& target, const Progress& progress,
              bool& cancelled, std::wstring& message)
{
    using virtualiseur::whp::SnapshotStore;
    cancelled = false;
    std::error_code ignored;
    const auto scratch = fs::temp_directory_path(ignored) / (L"Virtualiseur-export-" + NewUuid());
    try {
        // The disk state: a snapshot's frozen layer, or the current state (machine off).
        const SnapshotStore store(vm.config.disk);
        fs::path state;
        if (snapshot != 0) {
            const auto* found = store.Find(snapshot);
            if (found == nullptr) throw std::runtime_error("Instantané introuvable.");
            state = vm.config.disk.parent_path() / found->layer;
        } else {
            if (!stopped) {
                message = L"Éteins la machine, ou exporte un de ses instantanés : son état actuel change pendant qu'elle tourne.";
                return false;
            }
            state = store.CurrentLayer();
        }

        // The settings, serialized by SaveConfig into a scratch vm.ini then
        // read back as one [vm] section: no second list of keys to maintain.
        VmEntry copy;
        copy.directory = scratch;
        copy.config = vm.config;
        copy.config.disk = L"disk.img";
        copy.config.uuid.clear();
        copy.config.cloneOf.clear();
        SaveConfig(copy);
        std::vector<wchar_t> section(64 * 1024);
        const DWORD length = GetPrivateProfileSectionW(L"vm", section.data(), static_cast<DWORD>(section.size()),
                                                       (scratch / L"vm.ini").c_str());
        fs::remove_all(scratch, ignored);
        if (length == 0 || length >= section.size() - 2) throw std::runtime_error("Lecture de la configuration impossible.");
        std::string config = "[vm]\r\n";
        for (const wchar_t* entry = section.data(); *entry != L'\0'; entry += wcslen(entry) + 1) {
            config += Utf8(entry) + "\r\n";
        }

        virtualiseur::whp::ExportMachine(state, config, target, progress);
        std::error_code error;
        const auto size = fs::file_size(target, error);
        message = L"« " + vm.config.name + L" » exportée vers " + target.filename().wstring() +
                  (error ? L"." : L" (" + FormatBytes(size) + L").");
        return true;
    } catch (const virtualiseur::whp::ArchiveCancelled&) {
        fs::remove_all(scratch, ignored);
        cancelled = true;
        message = L"Export annulé : aucun fichier n'a été créé.";
        return false;
    } catch (const std::exception& error) {
        fs::remove_all(scratch, ignored);
        message = L"Export impossible : " + FromUtf8(error.what());
        return false;
    }
}

bool PreviewArchive(const fs::path& archive, ArchivePreview& preview, std::wstring& message)
{
    try {
        const auto info = virtualiseur::whp::ReadArchiveInfo(archive);
        const auto config = ParseArchiveConfig(info.config);
        preview.name = ArchiveValue(config, L"name");
        if (preview.name.empty()) preview.name = archive.stem().wstring();
        preview.os = ArchiveValue(config, L"os");
        preview.memoryMiB = static_cast<unsigned>(std::wcstoul(ArchiveValue(config, L"memory").c_str(), nullptr, 10));
        preview.cpus = static_cast<unsigned>(std::wcstoul(ArchiveValue(config, L"cpus").c_str(), nullptr, 10));
        preview.diskBytes = info.virtualSize;
        return true;
    } catch (const std::exception& error) {
        message = FromUtf8(error.what());
        return false;
    }
}

bool ImportVm(const fs::path& archive, const std::wstring& name, const fs::path& library, const std::vector<VmEntry>& vms,
              const Progress& progress, bool& cancelled, std::wstring& message)
{
    cancelled = false;
    if (!CheckNewVmName(name, library, vms, message)) return false;
    const auto directory = library / name;
    bool created = false;
    try {
        const auto info = virtualiseur::whp::ReadArchiveInfo(archive);
        const auto config = ParseArchiveConfig(info.config);
        if (!fs::create_directories(directory)) throw std::runtime_error("Dossier de la machine impossible à créer.");
        created = true;
        // The disk first: every block is checked (CRC) before the machine exists.
        const auto disk = directory / L"disk.img";
        virtualiseur::whp::ImportDisk(archive, disk, progress);

        // vm.ini: UTF-16 BOM (Unicode names and paths), then the archive's keys.
        const auto file = directory / L"vm.ini";
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Création de vm.ini impossible.");
        const wchar_t bom = 0xFEFF;
        DWORD written = 0;
        WriteFile(handle, &bom, sizeof(bom), &written, nullptr);
        CloseHandle(handle);
        for (const auto& [key, value] : config) WritePrivateProfileStringW(L"vm", key.c_str(), value.c_str(), file.c_str());

        auto entry = LoadEntry(directory);
        if (!entry) throw std::runtime_error("Configuration de la machine illisible.");
        auto& c = entry->config;
        c.name = name;
        c.disk = disk;
        c.diskGiB = static_cast<unsigned>((std::max)(1ull, (info.virtualSize + (1ull << 30) - 1) >> 30));
        c.mac = UniqueMac(vms); // A new identity: the source machine may run on the same network.
        c.uuid = NewUuid();
        c.cloneOf.clear();
        // Host paths from the exporting computer that do not exist here.
        if (!c.iso.empty() && !PathExists(c.iso)) c.iso.clear();
        if (!c.preseed.empty() && !PathExists(c.preseed)) c.preseed.clear();
        for (auto& share : c.shares) {
            if (!share.path.empty() && !PathExists(share.path)) share = {};
        }
        SaveConfig(*entry);
        message = L"Machine « " + name + L" » importée.";
        return true;
    } catch (const virtualiseur::whp::ArchiveCancelled&) {
        cancelled = true;
        message = L"Import annulé : rien n'a été créé.";
    } catch (const std::exception& error) {
        message = L"Import impossible : " + FromUtf8(error.what());
    }
    if (created) {
        std::error_code ignored;
        fs::remove_all(directory, ignored); // Transactional: no half-imported machine.
    }
    return false;
}

std::wstring RandomMac()
{
    // 52:54:00 (locally administered, as QEMU uses) + 3 random bytes.
    std::random_device device;
    const auto value = device();
    wchar_t text[18];
    swprintf_s(text, L"52:54:00:%02x:%02x:%02x", (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    return text;
}

bool ValidMac(const std::wstring& mac)
{
    if (mac.size() != 17) return false;
    for (std::size_t index = 0; index < mac.size(); ++index) {
        const wchar_t ch = mac[index];
        if (index % 3 == 2) {
            if (ch != L':' && ch != L'-') return false;
        } else if (!std::iswxdigit(ch)) {
            return false;
        }
    }
    const auto first = std::wcstoul(mac.substr(0, 2).c_str(), nullptr, 16);
    return (first & 1) == 0; // Not a multicast address.
}

std::wstring FormatForwards(const std::vector<PortForward>& forwards)
{
    std::wstring text;
    for (const auto& forward : forwards) {
        if (!text.empty()) text += L";";
        text += std::wstring(forward.udp ? L"udp:" : L"tcp:") + std::to_wstring(forward.host) + L":" +
                std::to_wstring(forward.guest);
    }
    return text;
}

std::vector<PortForward> ParseForwards(const std::wstring& text)
{
    std::vector<PortForward> result;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find(L';', start);
        if (end == std::wstring::npos) end = text.size();
        const auto rule = text.substr(start, end - start);
        start = end + 1;
        const auto first = rule.find(L':');
        const auto second = first == std::wstring::npos ? std::wstring::npos : rule.find(L':', first + 1);
        if (second == std::wstring::npos) continue;
        const auto protocol = rule.substr(0, first);
        const auto host = std::wcstoul(rule.substr(first + 1, second - first - 1).c_str(), nullptr, 10);
        const auto guest = std::wcstoul(rule.substr(second + 1).c_str(), nullptr, 10);
        if ((protocol != L"tcp" && protocol != L"udp") || host == 0 || host > 65535 || guest == 0 || guest > 65535) continue;
        result.push_back({protocol == L"udp", static_cast<unsigned short>(host), static_cast<unsigned short>(guest)});
    }
    return result;
}

const wchar_t* NetworkModeText(NetworkMode mode)
{
    switch (mode) {
    case NetworkMode::Nat: return L"NAT";
    case NetworkMode::Bridged: return L"Accès par pont";
    case NetworkMode::Internal: return L"Réseau interne";
    case NetworkMode::HostOnly: return L"Réseau privé hôte";
    case NetworkMode::Disconnected: return L"Câble débranché";
    default: return L"Désactivée";
    }
}

const wchar_t* StateText(RunState state)
{
    switch (state) {
    case RunState::Running: return L"En marche";
    case RunState::Paused: return L"En pause";
    case RunState::Starting: return L"Démarrage…";
    default: return L"Éteinte";
    }
}

} // namespace manager
