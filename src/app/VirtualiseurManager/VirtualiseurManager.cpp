// Virtualiseur Manager: VM library and lifecycle front-end, laid out like
// VirtualBox: toolbar, machine list, details of the selected machine, and a
// settings window organised by category.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include "Dialogs.h"
#include "Ui.h"
#include "VmLibrary.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace manager;
using ui::Scale;
namespace color = ui::color;

namespace {

enum CommandId : int {
    IdList = 100,
    IdDetails,
    IdStatusBar,
    IdNew,
    IdSettings,
    IdDelete,
    IdStart,
    IdPause,
    IdShutdown,
    IdPowerOff,
    IdReset,
    IdOpenFolder,
    IdShowLog,
    IdOpenLibrary,
    IdRefresh,
    IdExit,
    IdAbout,
};

constexpr UINT_PTR kStatusTimer = 1;
constexpr int kToolbarHeight = 74;
constexpr int kListWidth = 290;
constexpr UINT kOpenSettingsMessage = WM_APP + 1; // wParam: settings page.

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HWND g_list = nullptr;
HWND g_details = nullptr;
HWND g_status = nullptr;
HMENU g_menu = nullptr;
HACCEL g_accelerators = nullptr;
std::map<int, HWND> g_buttons;
std::vector<VmEntry> g_vms;
int g_selected = -1;
fs::path g_library;
fs::path g_settingsFile;
bool g_whp = false;
bool g_virgl = false;
int g_detailsScroll = 0;
int g_detailsHeight = 0;
std::vector<std::pair<RECT, int>> g_cardHeaders; // Details: clickable card titles -> settings page.

VmEntry* Selected()
{
    return g_selected >= 0 && g_selected < static_cast<int>(g_vms.size()) ? &g_vms[static_cast<std::size_t>(g_selected)]
                                                                         : nullptr;
}

bool WhpAvailable()
{
    HMODULE library = LoadLibraryExW(L"WinHvPlatform.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (library == nullptr) return false;
    using GetCapability = HRESULT(WINAPI*)(int, void*, UINT32, UINT32*);
    const auto get = reinterpret_cast<GetCapability>(GetProcAddress(library, "WHvGetCapability"));
    std::array<std::uint8_t, 64> buffer{};
    UINT32 written = 0;
    const bool present = get != nullptr && SUCCEEDED(get(0, buffer.data(), static_cast<UINT32>(buffer.size()), &written)) &&
                         buffer[0] != 0; // WHvCapabilityCodeHypervisorPresent.
    FreeLibrary(library);
    return present;
}

COLORREF StateColor(RunState state)
{
    switch (state) {
    case RunState::Running: return color::kGreen;
    case RunState::Paused: return color::kOrange;
    case RunState::Starting: return color::kAccent;
    default: return color::kSecondary;
    }
}

void SetStatus(const std::wstring& text)
{
    SendMessageW(g_status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

void UpdateHostStatus()
{
    const auto& host = Host();
    std::wstring text = L"Hôte : " + std::to_wstring(host.logicalCpus) + L" processeurs logiques · " +
                        FormatBytes(static_cast<std::uint64_t>(host.memoryMiB) * 1024 * 1024) + L" · WHP " +
                        (g_whp ? L"actif" : L"indisponible") + L" · 3D " + (g_virgl ? L"disponible" : L"absente");
    SendMessageW(g_status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(text.c_str()));
}

// --- Library and selection ---------------------------------------------------------------------

void RefreshList()
{
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    const auto top = SendMessageW(g_list, LB_GETTOPINDEX, 0, 0);
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (std::size_t index = 0; index < g_vms.size(); ++index) SendMessageW(g_list, LB_ADDSTRING, 0, 0);
    SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(g_selected), 0);
    SendMessageW(g_list, LB_SETTOPINDEX, static_cast<WPARAM>(top), 0);
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
}

void UpdateDetailsScroll();

void UpdateCommands()
{
    const auto* vm = Selected();
    const auto state = vm != nullptr ? vm->state : RunState::Stopped;
    const bool stopped = state == RunState::Stopped;
    const bool live = vm != nullptr && !stopped;
    const auto enable = [](int id, bool enabled) {
        if (const auto found = g_buttons.find(id); found != g_buttons.end()) EnableWindow(found->second, enabled);
        EnableMenuItem(g_menu, static_cast<UINT>(id), MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    };
    enable(IdSettings, vm != nullptr && stopped);
    enable(IdDelete, vm != nullptr && stopped);
    enable(IdStart, vm != nullptr && stopped);
    enable(IdPause, live && (state == RunState::Running || state == RunState::Paused));
    enable(IdShutdown, live);
    enable(IdPowerOff, live);
    enable(IdReset, live);
    enable(IdOpenFolder, vm != nullptr);
    enable(IdShowLog, vm != nullptr && PathExists(vm->directory / L"vmm.log"));
    const bool paused = state == RunState::Paused;
    ui::SetFlatButton(g_buttons[IdPause], paused ? ui::glyph::kPlay : ui::glyph::kPause, paused ? L"Reprendre" : L"Pause");
    ModifyMenuW(g_menu, IdPause, MF_BYCOMMAND | MF_STRING | (live ? MF_ENABLED : MF_GRAYED), IdPause,
                paused ? L"&Reprendre" : L"&Pause");
}

void Select(int index)
{
    g_selected = g_vms.empty() ? -1 : std::clamp(index, 0, static_cast<int>(g_vms.size()) - 1);
    SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(g_selected), 0);
    if (auto* vm = Selected()) RefreshDiskInfo(*vm);
    g_detailsScroll = 0;
    UpdateCommands();
    UpdateDetailsScroll();
    InvalidateRect(g_details, nullptr, FALSE);
}

void SelectByName(const std::wstring& name)
{
    for (std::size_t index = 0; index < g_vms.size(); ++index) {
        if (_wcsicmp(g_vms[index].config.name.c_str(), name.c_str()) == 0) {
            Select(static_cast<int>(index));
            return;
        }
    }
    Select(g_selected);
}

void Reload(const std::wstring& selectName = {})
{
    const auto* current = Selected();
    const std::wstring name = !selectName.empty() ? selectName : current != nullptr ? current->config.name : std::wstring();
    g_vms = LoadVmLibrary(g_library, g_vms);
    RefreshList();
    if (!name.empty()) SelectByName(name);
    else Select(0);
}

void PollStates()
{
    bool changed = false;
    for (auto& vm : g_vms) {
        RunState state = RunState::Stopped;
        if (vm.process != nullptr && WaitForSingleObject(vm.process, 0) == WAIT_OBJECT_0) {
            CloseHandle(vm.process);
            vm.process = nullptr;
        }
        if (const auto reply = SendCommand(vm.config.name, "status")) {
            state = *reply == "paused" ? RunState::Paused : RunState::Running;
        } else if (vm.process != nullptr) {
            state = RunState::Starting;
        }
        if (state != vm.state) {
            if (state == RunState::Stopped) RefreshDiskInfo(vm); // The disk grew while running.
            vm.state = state;
            changed = true;
        }
    }
    if (changed) {
        InvalidateRect(g_list, nullptr, FALSE);
        InvalidateRect(g_details, nullptr, FALSE);
        UpdateCommands();
    }
}

// --- Commands -----------------------------------------------------------------------------------

bool Confirm(const std::wstring& title, const std::wstring& instruction, const std::wstring& content,
             const wchar_t* confirmLabel, bool warning)
{
    TASKDIALOG_BUTTON buttons[] = {{IDOK, confirmLabel}, {IDCANCEL, L"Annuler"}};
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = g_window;
    config.hInstance = g_instance;
    config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = title.c_str();
    config.pszMainIcon = warning ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = content.c_str();
    config.cButtons = 2;
    config.pButtons = buttons;
    config.nDefaultButton = IDCANCEL;
    int pressed = IDCANCEL;
    return SUCCEEDED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr)) && pressed == IDOK;
}

void StartVm()
{
    auto* vm = Selected();
    if (vm == nullptr || vm->state != RunState::Stopped) return;
    std::wstring message;
    const bool started = LaunchVm(*vm, message);
    SetStatus(message);
    if (!started) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    InvalidateRect(g_list, nullptr, FALSE);
    InvalidateRect(g_details, nullptr, FALSE);
    UpdateCommands();
}

void NewVm()
{
    NewVmRequest request;
    if (!ShowNewVmDialog(g_window, g_library, g_vms, request)) return;
    VmEntry entry;
    entry.directory = g_library / request.config.name;
    entry.config = request.config;
    SaveConfig(entry);
    Reload(entry.config.name);
    SetStatus(L"Machine « " + entry.config.name + L" » créée.");
    if (request.startNow) StartVm();
}

void ApplySettings(VmEntry& vm, VmConfig config)
{
    std::wstring notes;
    // Grow the raw disk first, while its path is still the current one.
    RefreshDiskInfo(vm);
    const auto wanted = static_cast<std::uint64_t>(config.diskGiB) * 1024 * 1024 * 1024;
    if (config.disk == vm.config.disk && vm.diskExists && wanted > vm.diskBytes) {
        if (ExtendDisk(config.disk, wanted)) {
            notes += L" Disque agrandi à " + FormatBytes(wanted) + L" : agrandis la partition dans l'invité "
                     L"(par exemple avec GParted ou growpart).";
        } else {
            notes += L" Le disque n'a pas pu être agrandi.";
        }
    }
    if (config.name != vm.config.name && vm.directory.filename() == vm.config.name) {
        // The folder follows the name when it still matches the old one.
        const auto target = vm.directory.parent_path() / config.name;
        std::error_code error;
        if (!PathExists(target)) fs::rename(vm.directory, target, error);
        if (!error && PathExists(target)) {
            if (config.disk.parent_path() == vm.directory) config.disk = target / config.disk.filename();
            vm.directory = target;
        } else {
            notes += L" Le dossier n'a pas pu être renommé.";
        }
    }
    vm.config = config;
    SaveConfig(vm);
    Reload(config.name);
    SetStatus(L"Configuration de « " + config.name + L" » enregistrée." + notes);
}

void OpenSettings(int page)
{
    auto* vm = Selected();
    if (vm == nullptr) return;
    if (vm->state != RunState::Stopped) {
        SetStatus(L"Éteins la machine pour modifier sa configuration.");
        MessageBeep(MB_ICONINFORMATION);
        return;
    }
    VmConfig config;
    if (ShowSettingsDialog(g_window, *vm, g_vms, config, page)) ApplySettings(*vm, config);
}

void DeleteVm()
{
    auto* vm = Selected();
    if (vm == nullptr) return;
    if (vm->state != RunState::Stopped) {
        SetStatus(L"Éteins la machine avant de la supprimer.");
        return;
    }
    RefreshDiskInfo(*vm);
    const auto name = vm->config.name;
    const std::wstring content =
        L"Le dossier de la machine sera supprimé définitivement, avec son disque virtuel" +
        (vm->diskExists ? L" (" + FormatBytes(vm->diskAllocated) + L" sur l'hôte)" : std::wstring()) +
        L" :\n" + vm->directory.wstring() + L"\n\nL'image ISO d'installation est conservée.";
    if (!Confirm(L"Supprimer la machine", L"Supprimer « " + name + L" » ?", content, L"Supprimer", true)) return;

    std::error_code error;
    const bool diskOutside = !vm->config.disk.empty() && vm->config.disk.parent_path() != vm->directory;
    if (diskOutside && PathExists(vm->config.disk) &&
        Confirm(L"Supprimer la machine", L"Supprimer aussi le disque ?",
                L"Le disque est hors du dossier de la machine :\n" + vm->config.disk.wstring(), L"Supprimer le disque", true)) {
        fs::remove(vm->config.disk, error);
    }
    fs::remove_all(vm->directory, error);
    if (vm->process != nullptr) CloseHandle(vm->process);
    vm->process = nullptr;
    g_vms.erase(g_vms.begin() + g_selected);
    RefreshList();
    Select((std::min)(g_selected, static_cast<int>(g_vms.size()) - 1));
    if (error) {
        const auto text = error.message();
        SetStatus(L"Suppression incomplète : " + std::wstring(text.begin(), text.end()));
    } else {
        SetStatus(L"Machine « " + name + L" » supprimée.");
    }
}

void TogglePause()
{
    auto* vm = Selected();
    if (vm == nullptr) return;
    const char* command = vm->state == RunState::Paused ? "resume" : "pause";
    if (!SendCommand(vm->config.name, command)) SetStatus(L"La machine ne répond pas.");
    PollStates();
}

void AcpiShutdown()
{
    auto* vm = Selected();
    if (vm == nullptr) return;
    const auto reply = SendCommand(vm->config.name, "acpi-shutdown");
    if (!reply) SetStatus(L"La machine ne répond pas.");
    else if (*reply == "ignored") SetStatus(L"Le système invité n'a pas pris en compte le bouton d'alimentation ACPI.");
    else SetStatus(L"Signal d'arrêt envoyé : le système invité s'éteint proprement.");
}

void PowerOff()
{
    auto* vm = Selected();
    if (vm == nullptr) return;
    if (!Confirm(L"Éteindre", L"Éteindre immédiatement « " + vm->config.name + L" » ?",
                 L"C'est comme couper le courant : les données non enregistrées du système invité seront perdues.",
                 L"Éteindre", true)) {
        return;
    }
    if (!SendCommand(vm->config.name, "poweroff") && vm->process != nullptr) TerminateProcess(vm->process, 1);
    SetStatus(L"Machine « " + vm->config.name + L" » éteinte.");
}

void ResetVm()
{
    auto* vm = Selected();
    if (vm == nullptr) return;
    if (!Confirm(L"Redémarrer", L"Redémarrer « " + vm->config.name + L" » ?",
                 L"Le redémarrage est immédiat : les données non enregistrées du système invité seront perdues.",
                 L"Redémarrer", true)) {
        return;
    }
    if (!SendCommand(vm->config.name, "reset")) SetStatus(L"La machine ne répond pas.");
}

void ShowAbout()
{
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = g_window;
    config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS;
    config.pszWindowTitle = L"À propos de Virtualiseur";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"Virtualiseur 0.2";
    config.pszContent =
        L"Hyperviseur de type 2 pour Windows, construit sur Windows Hypervisor Platform.\n\n"
        L"Périphériques : virtio-gpu (3D virgl), virtio-sound, virtio-net (NAT libslirp), virtio-blk, "
        L"virtio-input, virtio-serial (presse-papiers partagé), IDE/ATAPI, ACPI.\n\n"
        L"Raccourcis : Ctrl+N nouvelle machine, Ctrl+S configuration, Entrée démarrer, Suppr supprimer, F5 actualiser.";
    config.dwCommonButtons = TDCBF_OK_BUTTON;
    TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

void ShowContextMenu(POINT screen)
{
    auto* vm = Selected();
    HMENU menu = CreatePopupMenu();
    if (vm == nullptr) {
        AppendMenuW(menu, MF_STRING, IdNew, L"&Nouvelle machine…");
    } else {
        const bool stopped = vm->state == RunState::Stopped;
        const bool live = !stopped;
        AppendMenuW(menu, MF_STRING | (stopped ? 0 : MF_GRAYED), IdStart, L"&Démarrer");
        AppendMenuW(menu, MF_STRING | (live ? 0 : MF_GRAYED), IdPause, vm->state == RunState::Paused ? L"&Reprendre" : L"&Pause");
        AppendMenuW(menu, MF_STRING | (live ? 0 : MF_GRAYED), IdShutdown, L"&Arrêt ACPI");
        AppendMenuW(menu, MF_STRING | (live ? 0 : MF_GRAYED), IdPowerOff, L"É&teindre");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (stopped ? 0 : MF_GRAYED), IdSettings, L"&Configuration…");
        AppendMenuW(menu, MF_STRING, IdOpenFolder, L"&Ouvrir le dossier");
        AppendMenuW(menu, MF_STRING | (PathExists(vm->directory / L"vmm.log") ? 0 : MF_GRAYED), IdShowLog, L"Afficher le &journal");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (stopped ? 0 : MF_GRAYED), IdDelete, L"&Supprimer…");
    }
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, screen.x, screen.y, 0, g_window, nullptr);
    DestroyMenu(menu);
}

// --- Details panel ----------------------------------------------------------------------------------

struct DetailRow {
    std::wstring label;
    std::wstring value;
    COLORREF valueColor = CLR_INVALID;
    bool path = false;
};
struct DetailCard {
    wchar_t glyph;
    std::wstring title;
    int page;
    std::vector<DetailRow> rows;
};

std::vector<DetailCard> BuildCards(const VmEntry& vm)
{
    const auto& c = vm.config;
    std::vector<DetailCard> cards;
    cards.push_back({ui::glyph::kInfo, L"Général", kPageGeneral,
                     {{L"Nom", c.name},
                      {L"Système invité", c.os.empty() ? L"Non précisé" : c.os},
                      {L"Dossier", vm.directory.wstring(), CLR_INVALID, true}}});
    const wchar_t* boot = c.boot == BootOrder::Cdrom ? L"Lecteur CD" : c.boot == BootOrder::Disk ? L"Disque dur"
                                                                                               : L"Automatique";
    cards.push_back({ui::glyph::kSystem, L"Système", kPageSystem,
                     {{L"Mémoire vive", FormatNumber(c.memoryMiB) + L" Mio"},
                      {L"Processeurs", std::to_wstring(c.cpus)},
                      {L"Démarrage", boot},
                      {L"Horloge", c.rtcLocal ? L"Heure locale" : L"UTC"}}});
    std::wstring resolution = c.resolution;
    std::replace(resolution.begin(), resolution.end(), L'x', L'×');
    DetailRow acceleration{L"Accélération 3D", L"Désactivée"};
    if (c.virtioGpu && c.gpu3d) {
        acceleration = g_virgl ? DetailRow{L"Accélération 3D", L"Activée (OpenGL, virgl)", color::kGreen}
                               : DetailRow{L"Accélération 3D", L"Indisponible : virglrenderer absent", color::kOrange};
    }
    cards.push_back({ui::glyph::kDisplay, L"Affichage", kPageDisplay,
                     {{L"Carte graphique", c.virtioGpu ? L"virtio-gpu" : L"Framebuffer simple"},
                      acceleration,
                      {L"Résolution initiale", resolution}}});
    std::wstring disk = c.disk.filename().wstring();
    if (vm.diskExists) {
        disk += L" — " + FormatBytes(vm.diskBytes) + L" (" + FormatBytes(vm.diskAllocated) + L" utilisés)";
    } else {
        disk += L" — " + FormatNumber(c.diskGiB) + L" Gio, créé au démarrage";
    }
    DetailRow iso{L"Lecteur CD", L"Vide", color::kSecondary};
    if (!c.iso.empty()) {
        iso = PathExists(c.iso) ? DetailRow{L"Lecteur CD", c.iso.filename().wstring()}
                                : DetailRow{L"Lecteur CD", c.iso.filename().wstring() + L" (introuvable)", color::kRed};
    }
    cards.push_back({ui::glyph::kDisk, L"Stockage", kPageStorage,
                     {{L"Contrôleur", c.diskBus == DiskBus::Ide ? L"IDE" : L"virtio-blk"},
                      {L"Disque dur", disk},
                      iso}});
    cards.push_back({ui::glyph::kAudio, L"Audio", kPageAudio,
                     c.audio ? std::vector<DetailRow>{{L"Contrôleur", L"virtio-sound"},
                                                      {L"Sortie hôte", L"Périphérique par défaut de Windows"}}
                             : std::vector<DetailRow>{{L"Son", L"Désactivé", color::kSecondary}}});
    std::vector<DetailRow> network;
    switch (c.network) {
    case NetworkMode::None:
        network.push_back({L"Carte 1", L"Désactivée", color::kSecondary});
        break;
    case NetworkMode::Bridged:
        network.push_back({L"Carte 1", L"Accès par pont : " + (c.bridgeAdapter.empty() ? L"automatique" : c.bridgeAdapter)});
        if (!NpcapAvailable()) network.push_back({L"Npcap", L"Non installé (npcap.com)", color::kOrange});
        break;
    case NetworkMode::Internal:
        network.push_back({L"Carte 1", L"Réseau interne « " + c.internalNetwork + L" »"});
        break;
    case NetworkMode::Disconnected:
        network.push_back({L"Carte 1", L"Câble débranché", color::kSecondary});
        break;
    default:
        network.push_back({L"Carte 1", NetworkModeText(c.network)});
        break;
    }
    if (c.network != NetworkMode::None) network.push_back({L"Adresse MAC", c.mac});
    if ((c.network == NetworkMode::Nat || c.network == NetworkMode::HostOnly) && !c.forwards.empty()) {
        std::wstring rules;
        for (const auto& forward : c.forwards) {
            if (!rules.empty()) rules += L", ";
            rules += std::wstring(forward.udp ? L"UDP " : L"TCP ") + std::to_wstring(forward.host) + L"→" +
                     std::to_wstring(forward.guest);
        }
        network.push_back({L"Redirections", rules});
    }
    cards.push_back({ui::glyph::kNetwork, L"Réseau", kPageNetwork, network});
    if (c.fullscreen) cards[2].rows.push_back({L"Démarrage", L"Plein écran"});
    if (!c.iso.empty()) {
        cards.push_back({ui::glyph::kInstall, L"Installation", kPageInstall,
                         {{L"Installateur Debian", c.graphicalInstaller ? L"Graphique" : L"Texte"},
                          {L"Preseed", c.preseed.empty() ? L"Aucun (interactif)" : c.preseed.filename().wstring()},
                          {L"Outils invité", c.guestTools ? L"Installés avec le bureau" : L"Non"}}});
    }
    return cards;
}

int CardHeight(const DetailCard& card) { return Scale(50) + static_cast<int>(card.rows.size()) * Scale(26) + Scale(12); }

// Paints the details (or the welcome page) and returns the content height.
int PaintDetails(HDC dc, const RECT& client, int scroll, bool draw)
{
    if (draw) {
        ui::Fill(dc, client, color::kWindow);
        g_cardHeaders.clear();
    }
    const int margin = Scale(24);
    const auto* vm = Selected();
    if (vm == nullptr) {
        RECT card{margin, margin - scroll, client.right - margin, margin - scroll + Scale(250)};
        if (draw) {
            ui::RoundedRect(dc, card, Scale(8), color::kPanel, color::kBorder);
            RECT glyph{card.left + Scale(24), card.top + Scale(24), card.left + Scale(72), card.top + Scale(72)};
            ui::RoundedRect(dc, glyph, Scale(10), color::kAccentSoft);
            ui::Text(dc, std::wstring(1, ui::glyph::kAdd), glyph, ui::Font().glyphLarge, color::kAccent,
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            RECT title{glyph.right + Scale(18), card.top + Scale(24), card.right - Scale(24), card.top + Scale(60)};
            ui::Text(dc, L"Bienvenue dans Virtualiseur", title, ui::Font().title, color::kText);
            RECT body{glyph.right + Scale(18), card.top + Scale(66), card.right - Scale(24), card.bottom - Scale(16)};
            ui::Text(dc,
                     L"Aucune machine virtuelle pour l'instant. Clique sur « Nouvelle » (Ctrl+N) : choisis une image "
                     L"ISO Linux, règle la mémoire, les processeurs et le disque avec les curseurs, puis démarre.\n\n"
                     L"Les machines sont rangées dans " + g_library.wstring() + L".\n\n" +
                         (g_whp ? L"Windows Hypervisor Platform est actif : tout est prêt."
                                : L"Windows Hypervisor Platform est indisponible : active la fonctionnalité "
                                  L"« Plateforme de l'hyperviseur Windows » puis redémarre Windows."),
                     body, ui::Font().body, color::kSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        }
        return Scale(250) + margin * 2;
    }

    // Header: badge, name, system and state.
    const auto badge = Badge(vm->config);
    int y = margin - scroll;
    RECT badgeRect{margin, y, margin + Scale(60), y + Scale(60)};
    if (draw) {
        ui::RoundedRect(dc, badgeRect, Scale(12), badge.color);
        ui::Text(dc, badge.initials, badgeRect, ui::Font().title, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT name{badgeRect.right + Scale(16), y + Scale(2), client.right - margin, y + Scale(36)};
        ui::Text(dc, vm->config.name, name, ui::Font().title, color::kText);
        RECT dot{badgeRect.right + Scale(17), y + Scale(43), badgeRect.right + Scale(25), y + Scale(51)};
        ui::Circle(dc, dot, StateColor(vm->state));
        RECT state{dot.right + Scale(8), y + Scale(36), client.right - margin, y + Scale(58)};
        std::wstring subtitle = StateText(vm->state);
        subtitle += L"  ·  " + (vm->config.os.empty() ? std::wstring(L"Linux") : vm->config.os);
        subtitle += L"  ·  " + FormatNumber(vm->config.memoryMiB) + L" Mio, " + std::to_wstring(vm->config.cpus) +
                    (vm->config.cpus > 1 ? L" processeurs" : L" processeur");
        ui::Text(dc, subtitle, state, ui::Font().body, color::kSecondary);
    }
    y += Scale(60) + Scale(20);

    // Cards, in one or two columns (shortest column first).
    const auto cards = BuildCards(*vm);
    const int gap = Scale(14);
    const int width = client.right - margin * 2;
    const int columns = width >= Scale(720) ? 2 : 1;
    const int cardWidth = (width - gap * (columns - 1)) / columns;
    std::array<int, 2> bottoms{y, y};
    for (const auto& card : cards) {
        const int column = columns == 2 && bottoms[1] < bottoms[0] ? 1 : 0;
        const int left = margin + column * (cardWidth + gap);
        const int top = bottoms[static_cast<std::size_t>(column)];
        const int height = CardHeight(card);
        RECT box{left, top, left + cardWidth, top + height};
        RECT header{box.left + Scale(1), box.top + Scale(1), box.right - Scale(1), box.top + Scale(44)};
        if (draw) g_cardHeaders.emplace_back(header, card.page);
        if (draw && box.bottom >= 0 && box.top <= client.bottom) {
            ui::RoundedRect(dc, box, Scale(8), color::kPanel, color::kBorder);
            RECT glyph{box.left + Scale(14), box.top + Scale(12), box.left + Scale(40), box.top + Scale(38)};
            ui::Text(dc, std::wstring(1, card.glyph), glyph, ui::Font().glyph, color::kAccent, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            RECT title{glyph.right + Scale(8), box.top + Scale(12), box.right - Scale(14), box.top + Scale(38)};
            ui::Text(dc, card.title, title, ui::Font().heading, color::kText);
            ui::Fill(dc, RECT{box.left + Scale(14), box.top + Scale(44), box.right - Scale(14), box.top + Scale(45)},
                     color::kBorder);
            int rowTop = box.top + Scale(52);
            const int labelWidth = (std::min)(Scale(150), cardWidth / 3);
            for (const auto& row : card.rows) {
                RECT label{box.left + Scale(16), rowTop, box.left + Scale(16) + labelWidth, rowTop + Scale(24)};
                ui::Text(dc, row.label, label, ui::Font().body, color::kSecondary);
                RECT value{label.right + Scale(8), rowTop, box.right - Scale(14), rowTop + Scale(24)};
                ui::Text(dc, row.value, value, ui::Font().body,
                         row.valueColor != CLR_INVALID ? row.valueColor : color::kText,
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                             (row.path ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS));
                rowTop += Scale(26);
            }
        }
        bottoms[static_cast<std::size_t>(column)] = top + height + gap;
    }
    const int bottom = (std::max)(bottoms[0], bottoms[1]);
    return bottom + scroll + margin - gap;
}

// Overlay scrollbar (Windows 11 style): a slim thumb drawn over the content.
bool g_scrollDragging = false;
bool g_scrollHover = false;
int g_scrollGrab = 0;

bool DetailsThumb(const RECT& client, RECT& thumb)
{
    const int view = client.bottom;
    if (g_detailsHeight <= view || view <= 0) return false;
    const int height = (std::max)(Scale(40), static_cast<int>(static_cast<long long>(view) * view / g_detailsHeight));
    const int travel = view - height - Scale(8);
    const int top = Scale(4) + static_cast<int>(static_cast<long long>(g_detailsScroll) * travel /
                                                 (std::max)(1, g_detailsHeight - view));
    const int width = g_scrollDragging || g_scrollHover ? Scale(8) : Scale(5);
    thumb = RECT{client.right - Scale(4) - width, top, client.right - Scale(4), top + height};
    return true;
}

void UpdateDetailsScroll()
{
    RECT client{};
    GetClientRect(g_details, &client);
    HDC dc = GetDC(g_details);
    g_detailsHeight = PaintDetails(dc, client, 0, false);
    ReleaseDC(g_details, dc);
    const int maximum = (std::max)(0, g_detailsHeight - static_cast<int>(client.bottom));
    g_detailsScroll = std::clamp(g_detailsScroll, 0, maximum);
}

void ScrollDetailsTo(int position)
{
    RECT client{};
    GetClientRect(g_details, &client);
    const int maximum = (std::max)(0, g_detailsHeight - static_cast<int>(client.bottom));
    position = std::clamp(position, 0, maximum);
    if (position == g_detailsScroll) return;
    g_detailsScroll = position;
    InvalidateRect(g_details, nullptr, FALSE);
}

void ScrollDetailsToThumb(int mouseY)
{
    RECT client{};
    GetClientRect(g_details, &client);
    RECT thumb{};
    if (!DetailsThumb(client, thumb)) return;
    const int height = thumb.bottom - thumb.top;
    const int travel = (std::max)(1, static_cast<int>(client.bottom) - height - Scale(8));
    const int top = mouseY - g_scrollGrab - Scale(4);
    ScrollDetailsTo(static_cast<int>(static_cast<long long>(top) * (g_detailsHeight - client.bottom) / travel));
}

LRESULT CALLBACK DetailsProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        {
            ui::BufferedPaint buffer(dc, client);
            PaintDetails(buffer.Dc(), client, g_detailsScroll, true);
            RECT thumb{};
            if (DetailsThumb(client, thumb)) {
                ui::RoundedRect(buffer.Dc(), thumb, Scale(4),
                                g_scrollDragging ? RGB(120, 120, 120) : g_scrollHover ? RGB(150, 150, 150) : RGB(185, 185, 185));
            }
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        UpdateDetailsScroll();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        ScrollDetailsTo(g_detailsScroll - GET_WHEEL_DELTA_WPARAM(wParam) * Scale(60) / WHEEL_DELTA);
        return 0;
    case WM_LBUTTONDOWN: {
        RECT client{};
        GetClientRect(window, &client);
        RECT thumb{};
        const POINT point{static_cast<SHORT>(LOWORD(lParam)), static_cast<SHORT>(HIWORD(lParam))};
        if (DetailsThumb(client, thumb) && point.x >= client.right - Scale(16)) {
            g_scrollDragging = true;
            g_scrollGrab = PtInRect(&thumb, point) ? point.y - thumb.top : (thumb.bottom - thumb.top) / 2;
            SetCapture(window);
            ScrollDetailsToThumb(point.y);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT point{static_cast<SHORT>(LOWORD(lParam)), static_cast<SHORT>(HIWORD(lParam))};
        if (g_scrollDragging) {
            ScrollDetailsToThumb(point.y);
            return 0;
        }
        RECT client{};
        GetClientRect(window, &client);
        const bool hover = g_detailsHeight > client.bottom && point.x >= client.right - Scale(16);
        if (hover != g_scrollHover) {
            g_scrollHover = hover;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_scrollHover) {
            g_scrollHover = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (g_scrollDragging) {
            g_scrollDragging = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && Selected() != nullptr && Selected()->state == RunState::Stopped &&
            !g_scrollDragging && !g_scrollHover) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            for (const auto& [rect, page] : g_cardHeaders) {
                if (PtInRect(&rect, point)) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
        }
        break;
    case WM_LBUTTONUP: {
        if (g_scrollDragging) {
            ReleaseCapture();
            return 0;
        }
        // A click on a card title opens the matching settings page, as in VirtualBox.
        const POINT point{static_cast<SHORT>(LOWORD(lParam)), static_cast<SHORT>(HIWORD(lParam))};
        if (g_scrollHover) return 0;
        for (const auto& [rect, page] : g_cardHeaders) {
            if (PtInRect(&rect, point)) {
                PostMessageW(g_window, kOpenSettingsMessage, static_cast<WPARAM>(page), 0);
                return 0;
            }
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (Selected() == nullptr) PostMessageW(g_window, WM_COMMAND, IdNew, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// --- Machine list -----------------------------------------------------------------------------------

void DrawListItem(const DRAWITEMSTRUCT& item)
{
    ui::BufferedPaint buffer(item.hDC, item.rcItem);
    const HDC dc = buffer.Dc();
    ui::Fill(dc, item.rcItem, color::kWindow);
    if (item.itemID >= g_vms.size()) return;
    const auto& vm = g_vms[item.itemID];
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    RECT card = item.rcItem;
    InflateRect(&card, -Scale(8), -Scale(3));
    if (selected) {
        ui::RoundedRect(dc, card, Scale(6), color::kPanel, color::kBorder);
        RECT bar{card.left + Scale(1), card.top + Scale(16), card.left + Scale(4), card.bottom - Scale(16)};
        ui::RoundedRect(dc, bar, Scale(1), color::kAccent);
    }
    const auto badge = Badge(vm.config);
    const int middle = (card.top + card.bottom) / 2;
    RECT badgeRect{card.left + Scale(14), middle - Scale(20), card.left + Scale(54), middle + Scale(20)};
    ui::RoundedRect(dc, badgeRect, Scale(8), badge.color);
    ui::Text(dc, badge.initials, badgeRect, ui::Font().badge, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT name{badgeRect.right + Scale(12), card.top + Scale(9), card.right - Scale(10), middle + Scale(1)};
    ui::Text(dc, vm.config.name, name, ui::Font().bold, color::kText,
             DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT dot{name.left + Scale(1), middle + Scale(8), name.left + Scale(9), middle + Scale(16)};
    ui::Circle(dc, dot, StateColor(vm.state));
    RECT state{dot.right + Scale(7), middle + Scale(3), card.right - Scale(10), middle + Scale(21)};
    std::wstring text = StateText(vm.state);
    if (!vm.config.os.empty()) text += L"  ·  " + vm.config.os;
    ui::Text(dc, text, state, ui::Font().caption, vm.state == RunState::Stopped ? color::kSecondary : StateColor(vm.state));
}

// --- Layout and window ----------------------------------------------------------------------------------

void Layout()
{
    RECT client{};
    GetClientRect(g_window, &client);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT status{};
    GetWindowRect(g_status, &status);
    const int statusHeight = status.bottom - status.top;
    const int parts[2] = {(std::max)(Scale(200), static_cast<int>(client.right) - Scale(470)), -1};
    SendMessageW(g_status, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));

    const int toolbar = Scale(kToolbarHeight);
    int x = Scale(10);
    const int buttonWidth = Scale(84);
    const int buttonHeight = Scale(64);
    const int buttonTop = (toolbar - buttonHeight) / 2;
    for (const int id : std::array<int, 8>{IdNew, IdSettings, IdDelete, 0, IdStart, IdPause, IdShutdown, IdPowerOff}) {
        if (id == 0) {
            x += Scale(17);
            continue;
        }
        SetWindowPos(g_buttons[id], nullptr, x, buttonTop, buttonWidth, buttonHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        x += buttonWidth + Scale(2);
    }
    const int bodyHeight = (std::max)(0, static_cast<int>(client.bottom) - toolbar - statusHeight);
    const int listWidth = Scale(kListWidth);
    SetWindowPos(g_list, nullptr, 0, toolbar + Scale(6), listWidth, (std::max)(0, bodyHeight - Scale(6)),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_details, nullptr, listWidth + 1, toolbar, (std::max)(0, static_cast<int>(client.right) - listWidth - 1),
                 bodyHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_window, nullptr, FALSE);
}

void ApplyFonts()
{
    SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(ui::Font().caption), TRUE);
    SendMessageW(g_list, LB_SETITEMHEIGHT, 0, Scale(64));
}

void PaintMain(HWND window)
{
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    {
        ui::BufferedPaint buffer(dc, client);
        const HDC target = buffer.Dc();
        const int toolbar = Scale(kToolbarHeight);
        ui::Fill(target, client, color::kWindow);
        ui::Fill(target, RECT{0, 0, client.right, toolbar}, color::kPanel);
        ui::Fill(target, RECT{0, toolbar - 1, client.right, toolbar}, color::kBorder);
        // Separator between the library and the machine commands.
        RECT settings{};
        GetWindowRect(g_buttons[IdDelete], &settings);
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&settings), 2);
        const int separator = settings.right + Scale(9);
        ui::Fill(target, RECT{separator, Scale(16), separator + 1, toolbar - Scale(16)}, color::kBorder);
        ui::Fill(target, RECT{Scale(kListWidth), toolbar, Scale(kListWidth) + 1, client.bottom}, color::kBorder);
    }
    EndPaint(window, &paint);
}

void SaveWindowPlacement()
{
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(g_window, &placement)) return;
    WritePrivateProfileStructW(L"manager", L"placement", &placement, sizeof(placement), g_settingsFile.c_str());
    if (const auto* vm = Selected()) {
        WritePrivateProfileStringW(L"manager", L"selected", vm->config.name.c_str(), g_settingsFile.c_str());
    }
}

void CreateControls(HWND window)
{
    g_menu = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IdNew, L"&Nouvelle machine…\tCtrl+N");
    AppendMenuW(file, MF_STRING, IdOpenLibrary, L"Ouvrir le dossier des &machines");
    AppendMenuW(file, MF_STRING, IdRefresh, L"&Actualiser\tF5");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IdExit, L"&Quitter");
    AppendMenuW(g_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&Fichier");
    HMENU machine = CreatePopupMenu();
    AppendMenuW(machine, MF_STRING, IdStart, L"&Démarrer\tEntrée");
    AppendMenuW(machine, MF_STRING, IdPause, L"&Pause");
    AppendMenuW(machine, MF_STRING, IdShutdown, L"&Arrêt ACPI");
    AppendMenuW(machine, MF_STRING, IdReset, L"&Redémarrer (reset)");
    AppendMenuW(machine, MF_STRING, IdPowerOff, L"É&teindre");
    AppendMenuW(machine, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(machine, MF_STRING, IdSettings, L"&Configuration…\tCtrl+S");
    AppendMenuW(machine, MF_STRING, IdOpenFolder, L"&Ouvrir le dossier");
    AppendMenuW(machine, MF_STRING, IdShowLog, L"Afficher le &journal");
    AppendMenuW(machine, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(machine, MF_STRING, IdDelete, L"&Supprimer…\tSuppr");
    AppendMenuW(g_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(machine), L"&Machine");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IdAbout, L"À &propos de Virtualiseur");
    AppendMenuW(g_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Aide");
    SetMenu(window, g_menu);

    const auto addButton = [window](int id, wchar_t glyph, const wchar_t* label, COLORREF glyphColor = CLR_INVALID) {
        HWND button = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 10, 10, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
        ui::MakeFlatButton(button, glyph, label, glyphColor);
        g_buttons[id] = button;
    };
    addButton(IdNew, ui::glyph::kAdd, L"Nouvelle", color::kAccent);
    addButton(IdSettings, ui::glyph::kSettings, L"Configuration");
    addButton(IdDelete, ui::glyph::kDelete, L"Supprimer");
    addButton(IdStart, ui::glyph::kPlay, L"Démarrer", color::kGreen);
    addButton(IdPause, ui::glyph::kPause, L"Pause");
    addButton(IdShutdown, ui::glyph::kPower, L"Arrêt ACPI");
    addButton(IdPowerOff, ui::glyph::kStop, L"Éteindre", color::kRed);

    g_list = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                             LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT, 0, 0, 10, 10, window,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdList)), g_instance, nullptr);
    g_details = CreateWindowExW(0, L"VirtualiseurDetails", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdDetails)), g_instance, nullptr);
    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, nullptr, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdStatusBar)), g_instance, nullptr);
    ApplyFonts();
}

void OnCommand(int id)
{
    switch (id) {
    case IdNew: NewVm(); break;
    case IdSettings: OpenSettings(kPageGeneral); break;
    case IdDelete: DeleteVm(); break;
    case IDOK:
    case IdStart: StartVm(); break;
    case IdPause: TogglePause(); break;
    case IdShutdown: AcpiShutdown(); break;
    case IdPowerOff: PowerOff(); break;
    case IdReset: ResetVm(); break;
    case IdOpenFolder:
        if (const auto* vm = Selected()) ShellExecuteW(g_window, L"open", vm->directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case IdShowLog:
        if (const auto* vm = Selected()) {
            const auto log = vm->directory / L"vmm.log";
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(g_window, L"open", log.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32) {
                ShellExecuteW(g_window, L"open", L"notepad.exe", QuoteArgument(log.wstring()).c_str(), nullptr, SW_SHOWNORMAL);
            }
        }
        break;
    case IdOpenLibrary: ShellExecuteW(g_window, L"open", g_library.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case IdRefresh:
        Reload();
        PollStates();
        SetStatus(L"Bibliothèque actualisée : " + std::to_wstring(g_vms.size()) + L" machine(s).");
        break;
    case IdExit: PostMessageW(g_window, WM_CLOSE, 0, 0); break;
    case IdAbout: ShowAbout(); break;
    default: break;
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        g_window = window;
        ui::InitTheme(GetDpiForWindow(window));
        CreateControls(window);
        Layout(); // Status bar parts before their texts.
        g_whp = WhpAvailable();
        g_virgl = VirglAvailable();
        std::array<wchar_t, 512> selected{};
        GetPrivateProfileStringW(L"manager", L"selected", L"", selected.data(), static_cast<DWORD>(selected.size()),
                                 g_settingsFile.c_str());
        g_vms = LoadVmLibrary(g_library, g_vms);
        RefreshList();
        if (selected[0] != 0) SelectByName(selected.data());
        else Select(0);
        PollStates();
        SetTimer(window, kStatusTimer, 1000, nullptr);
        UpdateHostStatus();
        if (FindVmExecutable().empty()) {
            SetStatus(L"Attention : VirtualiseurVM.exe est introuvable, compile src\\vmm en Release.");
        } else if (!g_whp) {
            SetStatus(L"Windows Hypervisor Platform est indisponible : active-la dans les fonctionnalités de Windows.");
        } else {
            SetStatus(g_vms.empty() ? L"Aucune machine : clique sur « Nouvelle » pour commencer."
                                    : L"Prêt. " + std::to_wstring(g_vms.size()) + L" machine(s) dans " + g_library.wstring());
        }
        Layout();
        return 0;
    }
    case WM_SIZE:
        Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = {Scale(900), Scale(560)};
        return 0;
    }
    case WM_DPICHANGED: {
        ui::InitTheme(HIWORD(wParam));
        ApplyFonts();
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        UpdateDetailsScroll();
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    }
    case WM_PAINT:
        PaintMain(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wParam == kStatusTimer) PollStates();
        return 0;
    case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(ui::Brush(color::kWindow));
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure->CtlType == ODT_LISTBOX) {
            measure->itemHeight = static_cast<UINT>(Scale(64));
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item.CtlType == ODT_LISTBOX && item.CtlID == IdList) {
            DrawListItem(item);
            return TRUE;
        }
        if (ui::DrawFlatButton(item, color::kPanel)) return TRUE;
        break;
    }
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wParam) == g_list) {
            POINT screen{static_cast<SHORT>(LOWORD(lParam)), static_cast<SHORT>(HIWORD(lParam))};
            if (screen.x == -1 && screen.y == -1) {
                RECT item{};
                SendMessageW(g_list, LB_GETITEMRECT, static_cast<WPARAM>((std::max)(g_selected, 0)), reinterpret_cast<LPARAM>(&item));
                screen = {item.left + Scale(40), item.bottom};
                ClientToScreen(g_list, &screen);
            } else {
                POINT client = screen;
                ScreenToClient(g_list, &client);
                const auto hit = SendMessageW(g_list, LB_ITEMFROMPOINT, 0, MAKELPARAM(client.x, client.y));
                if (HIWORD(hit) == 0 && LOWORD(hit) < g_vms.size()) Select(LOWORD(hit));
            }
            ShowContextMenu(screen);
            return 0;
        }
        break;
    case kOpenSettingsMessage:
        OpenSettings(static_cast<int>(wParam));
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IdList) {
            if (HIWORD(wParam) == LBN_SELCHANGE) {
                Select(static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0)));
            } else if (HIWORD(wParam) == LBN_DBLCLK) {
                StartVm();
            }
            return 0;
        }
        OnCommand(id);
        return 0;
    }
    case WM_CLOSE:
        SaveWindowPlacement();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kStatusTimer);
        for (auto& vm : g_vms) {
            if (vm.process != nullptr) CloseHandle(vm.process);
            vm.process = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    g_instance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS |
                                                        ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    ui::StartGraphics();

    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents))) {
        g_library = fs::path(documents) / L"Virtualiseur" / L"VMs";
        CoTaskMemFree(documents);
    } else {
        g_library = ExecutableDirectory() / L"VMs";
    }
    g_settingsFile = g_library.parent_path() / L"manager.ini";
    std::error_code error;
    fs::create_directories(g_library, error);

    WNDCLASSEXW details{sizeof(details)};
    details.style = CS_DBLCLKS;
    details.lpfnWndProc = DetailsProc;
    details.hInstance = instance;
    details.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    details.lpszClassName = L"VirtualiseurDetails";
    RegisterClassExW(&details);

    constexpr wchar_t className[] = L"VirtualiseurManagerWindow";
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (windowClass.hIcon == nullptr) windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.lpszClassName = className;
    if (!RegisterClassExW(&windowClass)) return 1;

    const UINT dpi = GetDpiForSystem();
    ui::InitTheme(dpi);
    RECT bounds{0, 0, MulDiv(1120, static_cast<int>(dpi), 96), MulDiv(700, static_cast<int>(dpi), 96)};
    AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPEDWINDOW, TRUE, 0, dpi);
    HWND window = CreateWindowExW(0, className, L"Virtualiseur — Gestionnaire de machines virtuelles",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                  bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) return 2;

    WINDOWPLACEMENT placement{};
    if (GetPrivateProfileStructW(L"manager", L"placement", &placement, sizeof(placement), g_settingsFile.c_str()) &&
        placement.length == sizeof(placement)) {
        if (placement.showCmd == SW_SHOWMINIMIZED) placement.showCmd = SW_SHOWNORMAL;
        if (showCommand != SW_SHOWDEFAULT && showCommand != SW_SHOWNORMAL) placement.showCmd = static_cast<UINT>(showCommand);
        SetWindowPlacement(window, &placement);
    } else {
        ShowWindow(window, showCommand);
    }
    UpdateWindow(window);

    ACCEL accelerators[] = {
        {FCONTROL | FVIRTKEY, 'N', IdNew},
        {FCONTROL | FVIRTKEY, 'S', IdSettings},
        {FVIRTKEY, VK_DELETE, IdDelete},
        {FVIRTKEY, VK_F5, IdRefresh},
    };
    g_accelerators = CreateAcceleratorTableW(accelerators, static_cast<int>(std::size(accelerators)));

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (g_accelerators != nullptr && TranslateAcceleratorW(window, g_accelerators, &message)) continue;
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    DestroyAcceleratorTable(g_accelerators);
    ui::StopGraphics();
    return static_cast<int>(message.wParam);
}
