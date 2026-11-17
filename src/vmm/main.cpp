#include <winsock2.h>

#include "Vm.h"
#include "core/Log.h"
#include "ui/DisplayWindow.h"

#include <Windows.h>
#include <share.h>

#include <atomic>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
// Common Controls 6: Windows visual styles and task dialogs in the VM window.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace vmm;

namespace {

void PrintUsage()
{
    std::fputs(
        "Virtualiseur VM - hyperviseur WHP\n\n"
        "Usage: VirtualiseurVM.exe [options]\n"
        "  --name <nom>              Nom de la machine (titre, canal de contrôle)\n"
        "  --memory <MiB>            Mémoire vive (défaut 2048)\n"
        "  --cpus <n>                Nombre de vCPU (défaut 1)\n"
        "  --iso <fichier.iso>       ISO attachée au lecteur CD-ROM\n"
        "  --disk <fichier.img>      Disque brut (créé sparse s'il manque, voir --disk-size)\n"
        "  --disk-size <MiB>         Taille du disque à créer\n"
        "  --disk-bus virtio|ide     Bus du disque (défaut virtio, le plus rapide)\n"
        "  --boot auto|cdrom|disk    Ordre de démarrage (auto: disque s'il contient Linux, sinon CD)\n"
        "  --kernel <bzImage> [--initrd <fichier>]  Démarrage direct d'un noyau\n"
        "  --append \"<args>\"         Arguments ajoutés à la ligne de commande du noyau\n"
        "  --cmdline \"<args>\"        Remplace la ligne de commande du noyau\n"
        "  --display gfx|text|none   Écran graphique, VGA texte ou aucun (défaut gfx)\n"
        "  --resolution <L>x<H>      Résolution de l'écran graphique (défaut 1280x800)\n"
        "  --gpu virgl|virtio|none   virtio-gpu avec 3D OpenGL (virgl, défaut si virglrenderer est\n"
        "                            présent), virtio-gpu 2D seule, ou framebuffer simple\n"
        "  --pointer tablet|ps2      Souris absolue sans capture, ou souris PS/2 seule (défaut tablet)\n"
        "  --installer text|gtk      Variante de l'installateur Debian (défaut text)\n"
        "  --net <mode>              Carte réseau (défaut none) : nat (NAT libslirp), bridged (accès\n"
        "                            par pont, Npcap), internal (réseau interne entre VM), hostonly\n"
        "                            (isolée, joignable par redirections), disconnected (câble débranché)\n"
        "  --bridge <carte>          Carte de l'hôte pour le pont (ex. \"Wi-Fi\", \"Ethernet\")\n"
        "  --intnet <nom>            Nom du réseau interne (défaut intnet)\n"
        "  --mac <adresse>           Adresse MAC de la carte (défaut 52:54:00:12:34:56)\n"
        "  --nat-forward tcp|udp:<hôte>:<invité>  Redirection de port 127.0.0.1 -> invité (répétable)\n"
        "  --fullscreen              Ouvre la fenêtre en plein écran (Ctrl droit + F pour en sortir)\n"
        "  --audio on|off            Carte son virtio-sound vers la sortie audio de Windows (défaut on)\n"
        "  --hyperv on|off           Enlightenments Hyper-V : horloge TSC de référence, timers synthétiques (défaut on)\n"
        "  --gpu-share on|off        3D virgl : écran affiché depuis une texture Direct3D partagée, sans relecture (défaut on)\n"
        "  --preseed <fichier>       Installation Debian automatique (fichier preseed)\n"
        "  --guest-tools on|off      Installer Debian : ajoute spice-vdagent (presse-papiers, fichiers)\n"
        "                            quand un bureau est installé (défaut on)\n"
        "  --rtc utc|local           Horloge matérielle en UTC (Linux, défaut) ou heure locale (Windows)\n"
        "  --serial-log <fichier>    Copie la console série (ttyS0) dans un fichier\n"
        "  --serial-input <fichier>  Envoie le contenu d'un fichier (suivi en continu) sur ttyS0\n"
        "  --log <fichier>           Journal de diagnostic du VMM\n"
        "  --verbose                 Journal détaillé\n",
        stdout);
}

struct Options {
    VmConfig vm;
    std::filesystem::path serialLog;
    std::filesystem::path serialInput;
    std::filesystem::path log;
    bool verbose = false;
};

std::uint64_t ParseNumber(const std::string& text, const char* what)
{
    try {
        std::size_t used = 0;
        const auto value = std::stoull(text, &used);
        if (used != text.size()) throw std::invalid_argument(what);
        return value;
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("Valeur invalide pour ") + what + ": " + text);
    }
}

std::string PathUtf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path Utf8Path(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return std::filesystem::path(wide);
}

Options ParseOptions(const std::vector<std::string>& args)
{
    Options options;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& option = args[index];
        const auto next = [&]() -> const std::string& {
            if (index + 1 >= args.size()) throw std::invalid_argument("Valeur manquante après " + option);
            return args[++index];
        };
        if (option == "--name") options.vm.name = next();
        else if (option == "--memory") options.vm.memoryMiB = ParseNumber(next(), "--memory");
        else if (option == "--cpus") options.vm.cpus = static_cast<std::uint32_t>(ParseNumber(next(), "--cpus"));
        else if (option == "--iso") options.vm.iso = Utf8Path(next());
        else if (option == "--disk") options.vm.disk = Utf8Path(next());
        else if (option == "--disk-size") options.vm.diskSizeMiB = ParseNumber(next(), "--disk-size");
        else if (option == "--disk-bus") {
            const auto& value = next();
            if (value == "virtio") options.vm.diskBus = DiskBus::Virtio;
            else if (value == "ide") options.vm.diskBus = DiskBus::Ide;
            else throw std::invalid_argument("--disk-bus attend virtio ou ide.");
        } else if (option == "--boot") {
            const auto& value = next();
            if (value == "auto") options.vm.boot = BootDevice::Auto;
            else if (value == "cdrom") options.vm.boot = BootDevice::Cdrom;
            else if (value == "disk") options.vm.boot = BootDevice::Disk;
            else throw std::invalid_argument("--boot attend auto, cdrom ou disk.");
        } else if (option == "--kernel") options.vm.kernel = Utf8Path(next());
        else if (option == "--initrd") options.vm.initrd = Utf8Path(next());
        else if (option == "--append") options.vm.commandLine = next();
        else if (option == "--cmdline") {
            options.vm.commandLine = next();
            options.vm.replaceCommandLine = true;
        } else if (option == "--display") {
            const auto& value = next();
            if (value == "gfx") options.vm.display = DisplayMode::Graphics;
            else if (value == "text") options.vm.display = DisplayMode::Text;
            else if (value == "none") options.vm.display = DisplayMode::None;
            else throw std::invalid_argument("--display attend gfx, text ou none.");
        } else if (option == "--resolution") {
            const auto& value = next();
            const auto x = value.find('x');
            if (x == std::string::npos) throw std::invalid_argument("--resolution attend LxH, ex. 1280x800.");
            options.vm.width = static_cast<std::uint32_t>(ParseNumber(value.substr(0, x), "--resolution"));
            options.vm.height = static_cast<std::uint32_t>(ParseNumber(value.substr(x + 1), "--resolution"));
        } else if (option == "--gpu") {
            const auto& value = next();
            options.vm.virtioGpu = value != "none";
            options.vm.gpu3d = value == "virgl";
            if (value != "virgl" && value != "virtio" && value != "none") {
                throw std::invalid_argument("--gpu attend virgl, virtio ou none.");
            }
        } else if (option == "--pointer") {
            const auto& value = next();
            if (value == "tablet") options.vm.tablet = true;
            else if (value == "ps2") options.vm.tablet = false;
            else throw std::invalid_argument("--pointer attend tablet ou ps2.");
        } else if (option == "--guest-tools") {
            const auto& value = next();
            if (value == "on") options.vm.guestTools = true;
            else if (value == "off") options.vm.guestTools = false;
            else throw std::invalid_argument("--guest-tools attend on ou off.");
        } else if (option == "--installer") {
            options.vm.graphicalInstaller = next() == "gtk";
        } else if (option == "--net") {
            const auto& value = next();
            if (value == "none") options.vm.network = NetworkMode::None;
            else if (value == "nat" || value == "slirp") options.vm.network = NetworkMode::Nat;
            else if (value == "bridged" || value == "bridge") options.vm.network = NetworkMode::Bridged;
            else if (value == "internal") options.vm.network = NetworkMode::Internal;
            else if (value == "hostonly") options.vm.network = NetworkMode::HostOnly;
            else if (value == "disconnected") options.vm.network = NetworkMode::Disconnected;
            else throw std::invalid_argument("--net attend none, nat, bridged, internal, hostonly ou disconnected.");
        } else if (option == "--bridge") {
            options.vm.bridgeAdapter = next();
        } else if (option == "--intnet") {
            options.vm.internalNetwork = next();
            if (options.vm.internalNetwork.empty()) throw std::invalid_argument("--intnet attend un nom de réseau.");
        } else if (option == "--mac") {
            if (!ParseMac(next(), options.vm.mac)) throw std::invalid_argument("--mac attend une adresse comme 52:54:00:12:34:56.");
        } else if (option == "--nat-forward") {
            // tcp:2222:22 (port hôte, puis port invité).
            const auto& value = next();
            const auto first = value.find(':');
            const auto second = first == std::string::npos ? std::string::npos : value.find(':', first + 1);
            const auto protocol = value.substr(0, first);
            if (second == std::string::npos || (protocol != "tcp" && protocol != "udp")) {
                throw std::invalid_argument("--nat-forward attend tcp:<port hôte>:<port invité>, ex. tcp:2222:22.");
            }
            PortForward forward;
            forward.udp = protocol == "udp";
            const auto host = ParseNumber(value.substr(first + 1, second - first - 1), "--nat-forward");
            const auto guest = ParseNumber(value.substr(second + 1), "--nat-forward");
            if (host == 0 || host > 65535 || guest == 0 || guest > 65535) {
                throw std::invalid_argument("--nat-forward : ports entre 1 et 65535.");
            }
            forward.hostPort = static_cast<std::uint16_t>(host);
            forward.guestPort = static_cast<std::uint16_t>(guest);
            options.vm.portForwards.push_back(forward);
        } else if (option == "--fullscreen") {
            options.vm.fullscreen = true;
        } else if (option == "--audio") {
            const auto& value = next();
            if (value == "on") options.vm.audio = true;
            else if (value == "off") options.vm.audio = false;
            else throw std::invalid_argument("--audio attend on ou off.");
        } else if (option == "--mic") {
            const auto& value = next();
            if (value == "on") options.vm.microphone = true;
            else if (value == "off") options.vm.microphone = false;
            else throw std::invalid_argument("--mic attend on ou off.");
        } else if (option == "--gpu-share") {
            const auto& value = next();
            if (value == "on") options.vm.gpuShare = true;
            else if (value == "off") options.vm.gpuShare = false;
            else throw std::invalid_argument("--gpu-share attend on ou off.");
        } else if (option == "--hyperv") {
            const auto& value = next();
            if (value == "on") options.vm.hyperv = true;
            else if (value == "off") options.vm.hyperv = false;
            else throw std::invalid_argument("--hyperv attend on ou off.");
        } else if (option == "--preseed") options.vm.preseed = Utf8Path(next());
        else if (option == "--rtc") options.vm.rtcLocalTime = next() == "local";
        else if (option == "--serial-log") options.serialLog = Utf8Path(next());
        else if (option == "--serial-input") options.serialInput = Utf8Path(next());
        else if (option == "--log") options.log = Utf8Path(next());
        else if (option == "--verbose") options.verbose = true;
        else if (option == "--help" || option == "-h" || option == "/?") {
            PrintUsage();
            std::exit(0);
        } else {
            throw std::invalid_argument("Option inconnue: " + option);
        }
    }
    return options;
}

// Serial console sink: buffered writes to the console and/or a log file.
class SerialSink final {
public:
    SerialSink(bool console, const std::filesystem::path& logPath) : console_(console)
    {
        if (!logPath.empty()) file_ = _wfsopen(logPath.c_str(), L"ab", _SH_DENYNO);
        flusher_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                Flush();
            }
            Flush();
        });
    }
    ~SerialSink()
    {
        flusher_.request_stop();
        if (flusher_.joinable()) flusher_.join();
        if (file_ != nullptr) std::fclose(file_);
    }
    void Put(std::uint8_t value)
    {
        const std::lock_guard lock(mutex_);
        buffer_.push_back(static_cast<char>(value));
        if (buffer_.size() > 8192) FlushLocked();
    }

private:
    void Flush()
    {
        const std::lock_guard lock(mutex_);
        FlushLocked();
    }
    void FlushLocked()
    {
        if (buffer_.empty()) return;
        if (console_) {
            DWORD written = 0;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buffer_.data(), static_cast<DWORD>(buffer_.size()), &written, nullptr);
        }
        if (file_ != nullptr) {
            std::fwrite(buffer_.data(), 1, buffer_.size(), file_);
            std::fflush(file_);
        }
        buffer_.clear();
    }

    bool console_;
    std::FILE* file_ = nullptr;
    std::mutex mutex_;
    std::string buffer_;
    std::jthread flusher_;
};

bool HasInteractiveConsole()
{
    DWORD mode = 0;
    return GetConsoleWindow() != nullptr && GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
}

void ConfigureConsole()
{
    SetConsoleOutputCP(CP_UTF8);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(output, &mode)) {
        SetConsoleMode(output, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (GetConsoleMode(input, &mode)) {
        // Raw VT input: arrow keys become escape sequences, Ctrl+C reaches the guest.
        SetConsoleMode(input, ENABLE_VIRTUAL_TERMINAL_INPUT);
    }
}

void SendToSerial(VmSlot& slot, std::uint8_t value, const std::stop_token& stop)
{
    while (!stop.stop_requested()) {
        const auto vm = slot.Get();
        if (vm && vm->State() == VmState::Running && vm->SerialInput(value)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void ConsoleInputThread(std::stop_token stop, VmSlot& slot)
{
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    char buffer[64];
    while (!stop.stop_requested()) {
        if (WaitForSingleObject(input, 100) != WAIT_OBJECT_0) continue;
        DWORD count = 0;
        if (!ReadFile(input, buffer, sizeof(buffer), &count, nullptr) || count == 0) continue;
        for (DWORD index = 0; index < count; ++index) SendToSerial(slot, static_cast<std::uint8_t>(buffer[index]), stop);
    }
}

// Follows a file like "tail -f" and types its bytes on the serial console.
void FileInputThread(std::stop_token stop, VmSlot& slot, std::filesystem::path path)
{
    std::uint64_t offset = 0;
    while (!stop.stop_requested()) {
        std::FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") == 0 && file != nullptr) {
            _fseeki64(file, static_cast<long long>(offset), SEEK_SET);
            int character;
            std::vector<std::uint8_t> data;
            while ((character = std::fgetc(file)) != EOF) data.push_back(static_cast<std::uint8_t>(character));
            std::fclose(file);
            for (const auto byte : data) {
                SendToSerial(slot, byte, stop);
                ++offset;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// Control channel used by the manager: one text command per connection.
void ControlThread(std::stop_token stop, VmSlot& slot, std::wstring pipeName)
{
    const auto createPipe = [&pipeName] {
        return CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
    };
    HANDLE pipe = createPipe();
    while (pipe != INVALID_HANDLE_VALUE && !stop.stop_requested()) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool connected = ConnectNamedPipe(pipe, &overlapped) != 0;
        if (!connected && GetLastError() == ERROR_PIPE_CONNECTED) connected = true;
        while (!connected && !stop.stop_requested()) {
            if (WaitForSingleObject(overlapped.hEvent, 200) == WAIT_OBJECT_0) {
                DWORD ignored = 0;
                connected = GetOverlappedResult(pipe, &overlapped, &ignored, FALSE) != 0;
                break;
            }
        }
        // Keep a listening instance available while this client is served,
        // so the manager never sees the VM disappear between two commands.
        const HANDLE next = connected ? createPipe() : INVALID_HANDLE_VALUE;
        if (connected && !stop.stop_requested()) {
            char buffer[2048] = {};
            DWORD read = 0;
            OVERLAPPED io{};
            io.hEvent = overlapped.hEvent;
            ResetEvent(io.hEvent);
            if (ReadFile(pipe, buffer, sizeof(buffer) - 1, &read, &io) ||
                (GetLastError() == ERROR_IO_PENDING && WaitForSingleObject(io.hEvent, 2000) == WAIT_OBJECT_0 &&
                 GetOverlappedResult(pipe, &io, &read, FALSE))) {
                std::string command(buffer, read);
                while (!command.empty() && (command.back() == '\n' || command.back() == '\r')) command.pop_back();
                std::string reply = "ok";
                const auto vm = slot.Get();
                if (!vm) reply = "starting";
                else if (command == "pause") vm->Pause();
                else if (command == "resume") vm->Resume();
                else if (command == "poweroff") vm->PowerOff();
                else if (command == "reset") vm->Reset();
                else if (command == "acpi-shutdown") reply = vm->AcpiShutdown() ? "ok" : "ignored";
                else if (command == "status") {
                    const auto state = vm->State();
                    reply = state == VmState::Paused ? "paused" : state == VmState::Running ? "running" : "stopped";
                } else if (command == "stats") reply = vm->Statistics();
                else if (command == "gpu-share on") vm->SetGpuShareAllowed(true);
                else if (command == "gpu-share off") vm->SetGpuShareAllowed(false);
                else if (command == "cable on" || command == "cable off") {
                    if (!vm->CanConnectCable()) reply = "no-network";
                    else vm->SetCableConnected(command == "cable on");
                } else if (command == "mic on" || command == "mic off") {
                    if (!vm->HasAudio()) reply = "no-audio";
                    else vm->SetMicrophoneEnabled(command == "mic on");
                } else if (command == "mic") reply = vm->MicrophoneEnabled() ? "on" : "off";
                else if (command == "timesync") vm->SyncGuestTime("demande par le tube");
                else if (command == "cable") reply = vm->CableConnected() ? "on" : "off";
                else if (command == "cdrom") reply = vm->CdromImage().empty() ? "empty" : PathUtf8(vm->CdromImage());
                else if (command == "cdrom-eject" || command.rfind("cdrom-insert ", 0) == 0) {
                    try {
                        vm->ChangeCdrom(command == "cdrom-eject" ? std::filesystem::path{} : Utf8Path(command.substr(13)));
                    } catch (const std::exception& error) {
                        reply = std::string("error: ") + error.what();
                    }
                }
                else reply = "unknown-command";
                DWORD written = 0;
                OVERLAPPED wio{};
                wio.hEvent = overlapped.hEvent;
                ResetEvent(wio.hEvent);
                if (!WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &written, &wio) &&
                    GetLastError() == ERROR_IO_PENDING) {
                    WaitForSingleObject(wio.hEvent, 2000);
                }
                FlushFileBuffers(pipe);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(overlapped.hEvent);
        if (next != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            pipe = next;
        } else if (!connected) {
            CancelIo(pipe);
        }
    }
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
}

std::wstring Widen(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

// Same rule as the manager (VmLibrary.cpp): a pipe name may hold any character
// but a backslash, so the full VM name is kept and two VMs never share a pipe.
std::wstring PipeName(const std::string& vmName)
{
    std::wstring name = L"\\\\.\\pipe\\Virtualiseur-";
    for (const wchar_t character : Widen(vmName)) {
        name.push_back(character == L'\\' || character < 0x20 ? L'_' : character);
    }
    return name;
}

std::atomic<VmSlot*> g_consoleSlot = nullptr;

BOOL WINAPI ConsoleControlHandler(DWORD event)
{
    if (event == CTRL_CLOSE_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        if (auto* slot = g_consoleSlot.load()) {
            if (const auto vm = slot->Get()) vm->PowerOff();
        }
        return TRUE;
    }
    return FALSE;
}

// Logs where a crash happened (module + offset, and the caller for a jump to
// a null pointer) before Windows terminates the process.
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* exception)
{
    const auto describe = [](std::uint64_t address) {
        HMODULE module = nullptr;
        wchar_t path[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &module)) {
            GetModuleFileNameW(module, path, MAX_PATH);
        }
        const auto offset = module != nullptr ? address - reinterpret_cast<std::uint64_t>(module) : address;
        char text[MAX_PATH + 64];
        std::snprintf(text, sizeof(text), "%ls+0x%llx", path, static_cast<unsigned long long>(offset));
        return std::string(text);
    };
    const auto* record = exception->ExceptionRecord;
    const auto address = reinterpret_cast<std::uint64_t>(record->ExceptionAddress);
    std::uint64_t caller = 0;
    if (address == 0 && exception->ContextRecord != nullptr) {
        caller = *reinterpret_cast<const std::uint64_t*>(exception->ContextRecord->Rsp);
    }
    VMM_ERROR("Plantage du VMM: exception 0x%08lx à %s%s%s", record->ExceptionCode, describe(address).c_str(),
              caller != 0 ? ", appelé depuis " : "", caller != 0 ? describe(caller).c_str() : "");
    return EXCEPTION_CONTINUE_SEARCH;
}

void ShowFatalError(const std::string& message, bool hasConsole)
{
    VMM_ERROR("%s", message.c_str());
    if (!hasConsole) {
        MessageBoxW(nullptr, Widen(message).c_str(), L"Virtualiseur", MB_ICONERROR);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    // Render the guest screen 1:1 in physical pixels instead of letting
    // Windows bitmap-stretch (and blur) the window on high-DPI displays.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const bool hasConsole = HasInteractiveConsole();
    if (hasConsole) ConfigureConsole();

    Options options;
    try {
        std::vector<std::string> args;
        for (int index = 1; index < argc; ++index) args.push_back(Narrow(argv[index]));
        if (args.empty()) {
            PrintUsage();
            return 2;
        }
        options = ParseOptions(args);
    } catch (const std::exception& error) {
        ShowFatalError(error.what(), hasConsole);
        return 2;
    }
    if (!options.log.empty()) Log::Open(options.log);
    Log::SetVerbose(options.verbose);
    SetUnhandledExceptionFilter(&CrashHandler);

    VmSlot slot;
    g_consoleSlot = &slot;
    SetConsoleCtrlHandler(&ConsoleControlHandler, TRUE);

    SerialSink serial(hasConsole && options.vm.display == DisplayMode::None, options.serialLog);
    std::jthread consoleInput;
    if (hasConsole && options.vm.display == DisplayMode::None) {
        consoleInput = std::jthread([&slot](std::stop_token stop) { ConsoleInputThread(stop, slot); });
    }
    std::jthread fileInput;
    if (!options.serialInput.empty()) {
        fileInput = std::jthread([&slot, path = options.serialInput](std::stop_token stop) {
            FileInputThread(stop, slot, path);
        });
    }
    std::jthread control([&slot, name = PipeName(options.vm.name)](std::stop_token stop) {
        ControlThread(stop, slot, name);
    });

    std::unique_ptr<DisplayWindow> window;
    std::atomic<HWND> windowHandle = nullptr;
    std::atomic_int exitCode = 0;
    std::atomic_bool shuttingDown = false;
    std::string fatalError;

    // The supervisor owns the VM lifecycle; guest resets recreate the VM.
    std::thread supervisor([&] {
        auto config = options.vm;
        for (;;) {
            if (shuttingDown.load()) break;
            std::shared_ptr<Vm> vm;
            try {
                vm = std::make_shared<Vm>(config, [&serial](std::uint8_t value) { serial.Put(value); });
            } catch (const std::exception& error) {
                fatalError = error.what();
                exitCode = 1;
                break;
            }
            slot.Set(vm);
            vm->Start();
            if (shuttingDown.load()) vm->PowerOff();
            const auto reason = vm->Wait();
            VMM_INFO("Statistiques: %s", vm->Statistics().c_str());
            slot.Set(nullptr);
            config = vm->CurrentConfig(); // Keeps the ISO and cable changed while running.
            // Other threads (window, control pipe) may still hold the VM for
            // a moment. WHP cannot map the memory of a new partition while
            // the old one exists, so destroy it here, last, before any restart.
            for (int waited = 0; vm.use_count() > 1; ++waited) {
                if (waited == 2000) VMM_WARN("La VM arrêtée est encore utilisée par un autre thread ; attente.");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            vm.reset();
            if (reason == VmExit::Reset && !shuttingDown.load()) {
                VMM_INFO("Redémarrage de la machine virtuelle.");
                continue;
            }
            exitCode = reason == VmExit::Error ? 1 : 0;
            break;
        }
        if (const auto handle = windowHandle.load()) PostMessageW(handle, DisplayWindow::kVmStoppedMessage, 0, 0);
    });

    if (options.vm.display != DisplayMode::None) {
        try {
            const std::uint32_t width = options.vm.display == DisplayMode::Graphics ? options.vm.width : 720;
            const std::uint32_t height = options.vm.display == DisplayMode::Graphics ? options.vm.height : 400;
            window = std::make_unique<DisplayWindow>(Widen(options.vm.name), slot, width, height,
                                                     options.vm.fullscreen && options.vm.display == DisplayMode::Graphics);
            windowHandle = window->Handle();
            if (supervisor.joinable() && WaitForSingleObject(supervisor.native_handle(), 0) == WAIT_OBJECT_0) {
                PostMessageW(window->Handle(), DisplayWindow::kVmStoppedMessage, 0, 0);
            }
            window->Run();
        } catch (const std::exception& error) {
            VMM_ERROR("Interface: %s", error.what());
        }
        windowHandle = nullptr;
        shuttingDown = true;
        if (const auto vm = slot.Get()) vm->PowerOff();
    }
    supervisor.join();
    window.reset();
    control.request_stop();
    fileInput = {};
    consoleInput = {};
    if (!fatalError.empty()) ShowFatalError(fatalError, hasConsole);
    return exitCode.load();
}
