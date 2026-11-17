#include "Vm.h"

#include "boot/BootSources.h"
#include "core/Log.h"
#include "devices/AcpiPm.h"
#include "devices/IdeController.h"
#include "devices/IoApic.h"
#include "devices/LegacyDevices.h"
#include "devices/Pci.h"
#include "devices/Ps2Controller.h"
#include "devices/VirtioBlk.h"
#include "devices/VirtioNet.h"
#include "devices/VirtioSound.h"
#include "devices/Virtio9p.h"
#include "DiskSnapshots.h"
#include "firmware/AcpiTables.h"
#include "net/BridgeNetworkBackend.h"
#include "net/InternalNetworkBackend.h"
#include "net/SlirpNetworkBackend.h"

#include <WinHvEmulation.h>
#include <powrprof.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace vmm {
namespace {

std::vector<std::uint8_t> ReadHostFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Impossible d'ouvrir " + path.string());
    const auto size = static_cast<std::size_t>(input.tellg());
    std::vector<std::uint8_t> data(size);
    input.seekg(0);
    input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!input) throw std::runtime_error("Lecture incomplète de " + path.string());
    return data;
}

bool HasArgument(const std::string& commandLine, const std::string& name)
{
    std::istringstream words(commandLine);
    std::string word;
    while (words >> word) {
        if (word == name || word.rfind(name + "=", 0) == 0) return true;
    }
    return false;
}

void AppendArgument(std::string& commandLine, const std::string& argument)
{
    if (!commandLine.empty()) commandLine.push_back(' ');
    commandLine += argument;
}

// virtio PCI functions: slot, INTx line (GSI) used only without MSI-X.
// Each slot gets I/O BAR0 at 0xC000 + slot * 0x100 and MMIO BAR1 at
// kVirtioMmioBase + slot * 64 KiB, inside the PCI root window (_CRS).
constexpr std::uint8_t kVirtioNetSlot = 3;
constexpr std::uint8_t kVirtioBlkSlot = 4;
constexpr std::uint8_t kVirtioGpuSlot = 5;
constexpr std::uint8_t kVirtioTabletSlot = 6;
constexpr std::uint8_t kVirtioSerialSlot = 7;
constexpr std::uint8_t kVirtioSoundSlot = 8;
constexpr std::uint8_t kQemuAgentSlot = 9;
constexpr std::uint8_t kShareSlot = 10;     // 10, 11, 12: one virtio-9p card per shared folder.
constexpr std::uint32_t kVirtioNetGsi = 11;
constexpr std::uint32_t kVirtioBlkGsi = 10;
constexpr std::uint32_t kVirtioGpuGsi = 16;
constexpr std::uint32_t kVirtioTabletGsi = 17;
constexpr std::uint32_t kVirtioSerialGsi = 18;
constexpr std::uint32_t kVirtioSoundGsi = 19;
constexpr std::uint32_t kQemuAgentGsi = 20;
constexpr std::uint32_t kShareGsi = 21;      // 21, 22, 23.
constexpr std::uint32_t kVirtioMmioBase = 0xFE000000;

std::string PathUtf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::wstring WidenUtf8(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

// PCI host bridge and ISA bridge identities (i440FX / PIIX3).
class SimplePciFunction final : public PciFunction {
public:
    using PciFunction::PciFunction;
};

} // namespace

Vm::Vm(const VmConfig& config, SerialOutput serialOutput)
    : config_(config), serialOutput_(std::move(serialOutput))
{
    if (config_.cpus == 0 || config_.cpus > 64) throw std::invalid_argument("Nombre de vCPU invalide (1-64).");

    const auto capabilities = virtualiseur::whp::QueryHostCapabilities();
    if (FAILED(capabilities.status) || !capabilities.hypervisorPresent) {
        throw std::runtime_error("Windows Hypervisor Platform n'est pas disponible. Active la fonctionnalité "
                                 "« Plateforme de l'hyperviseur Windows » puis redémarre Windows.");
    }

    partition_ = std::make_unique<virtualiseur::whp::Partition>(config_.cpus, true, config_.hyperv);
    if (config_.hyperv) {
        const auto features = partition_->SyntheticFeatures();
        if (features.AsUINT64 == 0) {
            VMM_WARN("Enlightenments Hyper-V indisponibles sur cet hôte : l'invité étalonnera ses horloges.");
        } else {
            VMM_INFO("Enlightenments Hyper-V : page TSC %s, fréquences %s, timers synthétiques %s.",
                     features.AccessPartitionReferenceTsc ? "oui" : "non",
                     features.AccessFrequencyRegs ? "oui" : "non",
                     features.DirectSyntheticTimers && features.AccessSyntheticTimerRegs ? "directs" : "non");
        }
    }

    // Doorbells: try one on an unused address of the PCI hole.
    if (HANDLE probe = CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        if (void* doorbell = partition_->CreateDoorbell(kVirtioMmioBase - 0x1000, probe)) {
            partition_->DeleteDoorbell(doorbell);
            doorbells_ = true;
        }
        CloseHandle(probe);
    }
    if (doorbells_) {
        VMM_INFO("Notifications virtio : doorbells WHP (sans sortie VM).");
    } else {
        VMM_INFO("Notifications virtio : sorties d'E/S (doorbells WHP indisponibles, 0x%08lx).",
                 static_cast<unsigned long>(partition_->LastDoorbellError()));
    }

    std::uint64_t framebufferBytes = 0;
    if (config_.display == DisplayMode::Graphics) {
        config_.width = std::clamp<std::uint32_t>(config_.width, 640, 3840) & ~7u;
        config_.height = std::clamp<std::uint32_t>(config_.height, 480, 2160);
        FramebufferInfo info;
        info.base = layout::kFramebufferBase;
        info.width = config_.width;
        info.height = config_.height;
        info.stride = config_.width * 4;
        framebufferBytes = static_cast<std::uint64_t>(info.stride) * info.height;
        framebuffer_ = info;
    }
    memory_ = std::make_unique<GuestMemory>(*partition_, config_.memoryMiB * 1024 * 1024, framebufferBytes);

    if (!config_.disk.empty()) {
        if (std::filesystem::exists(config_.disk)) {
            // With snapshots, the VM writes to the current layer over the base image.
            const auto current = virtualiseur::whp::SnapshotStore::CurrentDisk(config_.disk);
            disk_.emplace(virtualiseur::whp::VirtualDisk::Open(current));
            if (disk_->LayerCount() != 0) {
                VMM_INFO("Disque : couche active %s, %zu couche(s) au-dessus de %s.", PathUtf8(current.filename()).c_str(),
                         disk_->LayerCount(), PathUtf8(config_.disk.filename()).c_str());
            }
        } else {
            if (config_.diskSizeMiB == 0) throw std::runtime_error("Le disque n'existe pas et aucune taille n'est indiquée.");
            disk_.emplace(virtualiseur::whp::VirtualDisk::CreateSparse(config_.disk, config_.diskSizeMiB * 1024 * 1024));
            VMM_INFO("Disque sparse créé: %s (%llu MiB).", config_.disk.string().c_str(),
                     static_cast<unsigned long long>(config_.diskSizeMiB));
        }
    }
    if (!config_.iso.empty()) iso_ = std::make_unique<virtualiseur::whp::Iso9660Image>(config_.iso);

    BuildDevices();
    LoadBootImage();
}

Vm::~Vm()
{
    if (powerNotify_ != nullptr) PowerUnregisterSuspendResumeNotification(powerNotify_);
    RequestStop(exitReason_.load());
    {
        const std::lock_guard lock(pauseMutex_);
        paused_ = false;
    }
    pauseCondition_.notify_all();
    for (auto& thread : vcpus_) {
        if (thread.joinable()) thread.join();
    }
    // Stop device threads before the partition and guest memory go away;
    // notification threads first, they reach the network backend.
    for (auto& function : pciFunctions_) function->Shutdown();
    agent_.reset();
    qemuAgent_.reset();
    network_.reset();
    pciFunctions_.clear();
    pit_.reset();
    // Delete the partition now, while the guest memory it maps still exists.
    if (partition_) {
        const HRESULT deleted = partition_->Destroy();
        if (FAILED(deleted)) VMM_WARN("Suppression de la partition WHP : HRESULT 0x%08lx.", static_cast<unsigned long>(deleted));
    }
}

void Vm::BuildDevices()
{
    InterruptSink* sink = nullptr;
    ioApic_ = std::make_unique<IoApic>(*partition_, (std::min<std::uint32_t>)(config_.cpus, 15));
    sink = ioApic_.get();
    mmioBus_.Register(layout::kIoApicBase, 0x1000, ioApic_.get());

    pic_ = std::make_unique<Pic8259Pair>();
    for (const std::uint16_t port : std::array<std::uint16_t, 6>{0x20, 0x21, 0xA0, 0xA1, 0x4D0, 0x4D1}) {
        ioBus_.Register(port, 1, pic_.get());
    }

    pit_ = std::make_unique<Pit8254>(deviceLock_, *sink);
    ioBus_.Register(0x40, 4, pit_.get());
    ioBus_.Register(0x61, 1, pit_.get());

    rtc_ = std::make_unique<CmosRtc>(config_.memoryMiB * 1024 * 1024, config_.rtcLocalTime);
    ioBus_.Register(0x70, 2, rtc_.get());

    uart_ = std::make_unique<Uart16550>(*sink, 4, [this](std::uint8_t value) {
        if (serialOutput_) serialOutput_(value);
    });
    ioBus_.Register(0x3F8, 8, uart_.get());

    ps2_ = std::make_unique<Ps2Controller>(*sink, [this] { Reset(); });
    ioBus_.Register(0x60, 1, ps2_.get());
    ioBus_.Register(0x64, 1, ps2_.get());

    acpi_ = std::make_unique<AcpiPm>(*sink, [this] {
        VMM_INFO("Le système invité s'est éteint (ACPI S5).");
        RequestStop(VmExit::PowerOff);
    });
    ioBus_.Register(AcpiPm::kBase, 12, acpi_.get());

    vga_ = std::make_unique<VgaRegisters>();
    ioBus_.Register(0x3B0, 0x30, vga_.get());

    post_ = std::make_unique<PostPort>();
    ioBus_.Register(0x80, 1, post_.get());
    ioBus_.Register(0xE9, 1, post_.get());

    pciBus_ = std::make_unique<PciBus>([this] { Reset(); });
    ioBus_.Register(0xCF8, 8, pciBus_.get());

    auto host = std::make_unique<SimplePciFunction>(std::uint16_t{0x8086}, std::uint16_t{0x1237}, std::uint8_t{0x06},
                                                    std::uint8_t{0x00}, std::uint8_t{0x00}, std::uint8_t{0x02});
    pciBus_->Add(0, 0, host.get());
    pciFunctions_.push_back(std::move(host));

    auto isa = std::make_unique<SimplePciFunction>(std::uint16_t{0x8086}, std::uint16_t{0x7000}, std::uint8_t{0x06},
                                                   std::uint8_t{0x01}, std::uint8_t{0x00}, std::uint8_t{0x00});
    isa->SetMultiFunction();
    pciBus_->Add(1, 0, isa.get());
    pciFunctions_.push_back(std::move(isa));

    auto ide = std::make_unique<IdeController>(*memory_, *sink, ioBus_);
    ide_ = ide.get();
    pciBus_->Add(1, 1, ide.get());
    pciFunctions_.push_back(std::move(ide));
    if (disk_ && config_.diskBus == DiskBus::Ide) ide_->AttachDisk(0, &*disk_);
    // The CD-ROM drive is always there, empty or not, so a disc can be
    // inserted while the VM runs.
    ide_->AttachCdrom(1, iso_.get());

    const auto placement = [this, sink](std::uint8_t slot, std::uint32_t gsi) {
        return VirtioPlacement{ioBus_, mmioBus_, *sink, static_cast<std::uint16_t>(0xC000 + slot * 0x100),
                               kVirtioMmioBase + slot * 0x10000u, gsi, doorbells_ ? partition_.get() : nullptr};
    };

    if (config_.network != NetworkMode::None) {
        auto net = std::make_unique<VirtioNet>(*memory_, placement(kVirtioNetSlot, kVirtioNetGsi),
            [this](std::span<const std::uint8_t> frame) { return network_ && network_->Send(frame); }, config_.mac);
        virtioNet_ = net.get();
        pciBus_->Add(kVirtioNetSlot, 0, net.get());
        pciFunctions_.push_back(std::move(net));
        GuestPort receive{
            [this](std::span<const std::uint8_t> frame) {
                const DeviceGuard guard(virtioNet_->Lock());
                virtioNet_->Receive(frame);
            },
            [this] {
                const DeviceGuard guard(virtioNet_->Lock());
                virtioNet_->FlushReceived();
            }};
        // A backend that cannot start leaves the card in place, cable unplugged.
        try {
            switch (config_.network) {
            case NetworkMode::Nat:
            case NetworkMode::HostOnly:
#ifdef VIRTUALISEUR_NO_SLIRP
                VMM_WARN("Réseau NAT indisponible : ce binaire a été compilé sans libslirp.");
#else
                network_ = std::make_unique<SlirpNetworkBackend>(receive, config_.network == NetworkMode::HostOnly,
                                                                 config_.portForwards);
#endif
                break;
            case NetworkMode::Bridged:
                network_ = std::make_unique<BridgeNetworkBackend>(receive, config_.bridgeAdapter, config_.mac);
                break;
            case NetworkMode::Internal:
                network_ = std::make_unique<InternalNetworkBackend>(receive, config_.internalNetwork, config_.mac);
                break;
            default:
                VMM_INFO("Carte réseau %s : câble débranché.", FormatMac(config_.mac).c_str());
                break;
            }
        } catch (const std::exception& error) {
            VMM_WARN("Réseau indisponible (%s) : la carte réseau reste débranchée.", error.what());
            network_.reset();
        }
        virtioNet_->SetLinkUp(network_ != nullptr && config_.cableConnected);
        if (network_ && !config_.cableConnected) VMM_INFO("Carte réseau : câble débranché.");
    }
    if (disk_ && config_.diskBus == DiskBus::Virtio) {
        auto blk = std::make_unique<VirtioBlk>(*memory_, placement(kVirtioBlkSlot, kVirtioBlkGsi), *disk_);
        virtioBlk_ = blk.get();
        pciBus_->Add(kVirtioBlkSlot, 0, blk.get());
        pciFunctions_.push_back(std::move(blk));
    }

    if (config_.display == DisplayMode::Graphics && config_.virtioGpu && framebuffer_) {
        auto gpu = std::make_unique<VirtioGpu>(*memory_, placement(kVirtioGpuSlot, kVirtioGpuGsi),
                                               framebuffer_->width, framebuffer_->height,
                                               framebuffer_->base, memory_->FramebufferBytes(), config_.gpu3d);
        virtioGpu_ = gpu.get();
        virtioGpu_->SetShareAllowed(config_.gpuShare);
        pciBus_->Add(kVirtioGpuSlot, 0, gpu.get());
        pciFunctions_.push_back(std::move(gpu));
    }
    if (config_.display == DisplayMode::Graphics && config_.tablet) {
        auto tablet = std::make_unique<VirtioTablet>(*memory_, placement(kVirtioTabletSlot, kVirtioTabletGsi));
        tablet_ = tablet.get();
        pciBus_->Add(kVirtioTabletSlot, 0, tablet.get());
        pciFunctions_.push_back(std::move(tablet));
    }

    if (config_.display != DisplayMode::None) {
        // spice-vdagent port: clipboard sharing and file transfer.
        agent_ = std::make_unique<GuestAgent>();
        auto* agent = agent_.get();
        auto serial = std::make_unique<VirtioSerial>(
            *memory_, placement(kVirtioSerialSlot, kVirtioSerialGsi), GuestAgent::kPortName,
            [agent](std::span<const std::uint8_t> data) { agent->OnBytes(data); },
            [agent](bool open) { agent->OnOpen(open); });
        agent->Attach(serial.get());
        pciBus_->Add(kVirtioSerialSlot, 0, serial.get());
        pciFunctions_.push_back(std::move(serial));
    }

    {
        // qemu-guest-agent port: keeps the guest clock on the host's.
        qemuAgent_ = std::make_unique<QemuGuestAgent>();
        auto* agent = qemuAgent_.get();
        auto serial = std::make_unique<VirtioSerial>(
            *memory_, placement(kQemuAgentSlot, kQemuAgentGsi), QemuGuestAgent::kPortName,
            [agent](std::span<const std::uint8_t> data) { agent->OnBytes(data); },
            [agent](bool open) { agent->OnOpen(open); });
        agent->Attach(serial.get());
        pciBus_->Add(kQemuAgentSlot, 0, serial.get());
        pciFunctions_.push_back(std::move(serial));
    }

    if (config_.audio) {
        try {
            auto sound = std::make_unique<VirtioSound>(*memory_, placement(kVirtioSoundSlot, kVirtioSoundGsi),
                                                       L"Virtualiseur — " + WidenUtf8(config_.name), config_.microphone);
            sound_ = sound.get();
            pciBus_->Add(kVirtioSoundSlot, 0, sound.get());
            pciFunctions_.push_back(std::move(sound));
        } catch (const std::exception& error) {
            VMM_WARN("Carte son désactivée : %s", error.what());
        }
    }

    std::vector<PciInterruptRoute> shareRoutes;
    std::vector<std::string> shareTags;
    for (std::size_t index = 0; index < config_.shares.size(); ++index) {
        const auto& share = config_.shares[index];
        if (index >= kMaxShares) {
            VMM_WARN("Dossier partagé « %s » ignoré : %zu au plus.", share.tag.c_str(), kMaxShares);
            continue;
        }
        const auto slot = static_cast<std::uint8_t>(kShareSlot + index);
        const auto gsi = static_cast<std::uint32_t>(kShareGsi + index);
        try {
            auto device = std::make_unique<Virtio9p>(*memory_, placement(slot, gsi), share.tag, share.path, share.readOnly);
            VMM_INFO("Dossier partagé « %s »%s : %s. Monté par qemu-guest-agent sur /media/%s, ou à la main : mount -t 9p -o trans=virtio,version=9p2000.L,msize=524288 %s <dossier>",
                     share.tag.c_str(), share.readOnly ? " (lecture seule)" : "", PathUtf8(share.path).c_str(), share.tag.c_str(), share.tag.c_str());
            shares_.push_back(device.get());
            pciBus_->Add(slot, 0, device.get());
            pciFunctions_.push_back(std::move(device));
            shareRoutes.push_back({slot, static_cast<std::uint8_t>(gsi)});
            shareTags.push_back(share.tag);
        } catch (const std::exception& error) {
            VMM_WARN("Dossier partagé « %s » (%s) indisponible : %s.", share.tag.c_str(), PathUtf8(share.path).c_str(), error.what());
        }
    }

    AcpiConfig acpi;
    acpi.cpuCount = config_.cpus;
    acpi.pciMmioStart = static_cast<std::uint32_t>(memory_->LowRamEnd());
    acpi.pciRoutes = {{kVirtioNetSlot, static_cast<std::uint8_t>(kVirtioNetGsi)},
                      {kVirtioBlkSlot, static_cast<std::uint8_t>(kVirtioBlkGsi)},
                      {kVirtioGpuSlot, static_cast<std::uint8_t>(kVirtioGpuGsi)},
                      {kVirtioTabletSlot, static_cast<std::uint8_t>(kVirtioTabletGsi)},
                      {kVirtioSerialSlot, static_cast<std::uint8_t>(kVirtioSerialGsi)},
                      {kVirtioSoundSlot, static_cast<std::uint8_t>(kVirtioSoundGsi)},
                      {kQemuAgentSlot, static_cast<std::uint8_t>(kQemuAgentGsi)}};
    acpi.pciRoutes.insert(acpi.pciRoutes.end(), shareRoutes.begin(), shareRoutes.end());
    // qemu-guest-agent mounts them in the guest on /media/<tag> when it connects.
    if (qemuAgent_) qemuAgent_->SetShares(std::move(shareTags));
    const auto rsdp = BuildAcpiTables(*memory_, acpi);
    (void)rsdp;
}

void Vm::LoadBootImage()
{
    std::optional<BootSelection> selection;
    const bool kernelGiven = !config_.kernel.empty();
    auto boot = config_.boot;
    if (boot == BootDevice::Auto) boot = kernelGiven ? BootDevice::Kernel : BootDevice::Auto;

    if (boot == BootDevice::Kernel) {
        BootSelection direct;
        direct.image.kernel = ReadHostFile(config_.kernel);
        if (!config_.initrd.empty()) direct.image.initrd = ReadHostFile(config_.initrd);
        direct.description = "noyau " + config_.kernel.filename().string();
        selection = std::move(direct);
    }
    if (!selection && (boot == BootDevice::Disk || boot == BootDevice::Auto) && disk_) {
        selection = FindDiskBoot(*disk_);
        if (!selection && boot == BootDevice::Disk) {
            throw std::runtime_error("Aucun système Linux amorçable (ext2/3/4 avec /boot) n'a été trouvé sur le disque.");
        }
    }
    bool fromIso = false;
    if (!selection && (boot == BootDevice::Cdrom || boot == BootDevice::Auto) && iso_) {
        fromIso = true;
        selection = FindIsoBoot(*iso_, config_.graphicalInstaller);
        if (!selection) {
            throw std::runtime_error("L'ISO ne contient pas de noyau/initrd Linux reconnu (Debian, Ubuntu, Fedora, Arch...).");
        }
        std::vector<std::uint8_t> preseed;
        if (!config_.preseed.empty()) {
            preseed = ReadHostFile(config_.preseed);
            AppendArgument(selection->image.commandLine, "auto=true priority=critical");
            VMM_INFO("Installation automatique: %s injecté comme /preseed.cfg.", config_.preseed.string().c_str());
        }
        const bool debianInstaller = selection->description.rfind("installateur Debian", 0) == 0;
        if (config_.guestTools && debianInstaller) {
            // Guest tools: once the installation is done, install
            // qemu-guest-agent (clock kept on the host's) and, if a desktop
            // was chosen, spice-vdagent (clipboard, file drops). The installer
            // loads /preseed.cfg from its initrd on its own; without
            // "auto=true" it asks the usual questions. Without network, apt
            // fails quietly and the installation goes on.
            const std::string existing(preseed.begin(), preseed.end());
            if (existing.find("preseed/late_command") != std::string::npos) {
                VMM_WARN("Le preseed définit déjà preseed/late_command : les outils invité n'y sont pas ajoutés.");
            } else {
                const std::string line =
                    "\n# Virtualiseur : agents invité (heure synchronisée ; avec un bureau, presse-papiers "
                    "partagé et dépôt de fichiers).\n"
                    "d-i preseed/late_command string in-target apt-get install -y qemu-guest-agent || true; "
                    "if [ -d /target/usr/share/xsessions ] || [ -d /target/usr/share/wayland-sessions ]; then "
                    "in-target apt-get install -y spice-vdagent || true; fi\n";
                preseed.insert(preseed.end(), line.begin(), line.end());
                VMM_INFO("Outils invité : qemu-guest-agent sera installé, et spice-vdagent avec le bureau.");
            }
        }
        if (!preseed.empty()) AppendFileToInitrd(selection->image.initrd, "/preseed.cfg", preseed);
    }
    if (!selection) {
        throw std::runtime_error("Rien à démarrer: indique une ISO, un disque contenant Linux ou un noyau.");
    }

    std::string commandLine = config_.replaceCommandLine ? config_.commandLine : selection->image.commandLine;
    if (!config_.replaceCommandLine && !config_.commandLine.empty()) AppendArgument(commandLine, config_.commandLine);
    if (!HasArgument(commandLine, "console")) {
        // The last console= becomes /dev/console: keep the screen primary.
        AppendArgument(commandLine, "console=ttyS0,115200n8");
        if (config_.display != DisplayMode::None) AppendArgument(commandLine, "console=tty0");
    }
    // With the Hyper-V reference TSC page the guest has a stable clock
    // source; otherwise trust the (invariant) TSC rather than let the
    // watchdog demote it to the slow ACPI PM timer.
    if (!partition_->SyntheticFeatures().AccessPartitionReferenceTsc && !HasArgument(commandLine, "tsc")) {
        AppendArgument(commandLine, "tsc=reliable");
    }
    if (fromIso && config_.display == DisplayMode::None && !HasArgument(commandLine, "DEBIAN_FRONTEND")) {
        AppendArgument(commandLine, "DEBIAN_FRONTEND=text");
    }
    selection->image.commandLine = commandLine;
    bootDescription_ = selection->description;
    VMM_INFO("Démarrage: %s", bootDescription_.c_str());
    VMM_INFO("Ligne de commande: %s", commandLine.c_str());

    const auto info = LoadLinux(*memory_, selection->image, 0xE0000, framebuffer_);
    SetupLinuxBootCpu(*partition_, 0, info);
    for (std::uint32_t index = 1; index < config_.cpus; ++index) ParkApplicationProcessor(*partition_, index);
    // Let WHP's local APIC accept interrupts before the guest programs it.
    partition_->SetLocalApicRegister(0, 0x0F0, 0x000001FF);
}

namespace {

// Host sleep: the guest clock is checked when the host wakes up.
ULONG CALLBACK HostPowerCallback(PVOID context, ULONG type, PVOID)
{
    if (type == PBT_APMRESUMEAUTOMATIC) static_cast<Vm*>(context)->SyncGuestTime("réveil de l'hôte");
    return 0;
}

} // namespace

void Vm::Start()
{
    state_ = VmState::Running;
    powerSubscription_ = {&HostPowerCallback, this};
    if (PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK, &powerSubscription_, &powerNotify_) != ERROR_SUCCESS) {
        powerNotify_ = nullptr;
    }
    for (std::uint32_t index = 0; index < config_.cpus; ++index) {
        vcpus_.emplace_back([this, index] { VcpuThread(index); });
    }
}

VmExit Vm::Wait()
{
    for (auto& thread : vcpus_) {
        if (thread.joinable()) thread.join();
    }
    state_ = VmState::Stopped;
    return exitReason_.load();
}

void Vm::CancelAll()
{
    for (std::uint32_t index = 0; index < config_.cpus; ++index) (void)partition_->CancelRunVirtualProcessor(index);
}

void Vm::RequestStop(VmExit reason)
{
    if (!stopRequested_.exchange(true)) exitReason_ = reason;
    {
        const std::lock_guard lock(pauseMutex_);
        paused_ = false;
    }
    pauseCondition_.notify_all();
    if (partition_) CancelAll();
}

void Vm::Pause()
{
    if (state_ != VmState::Running) return;
    if (qemuAgent_) qemuAgent_->SetSuspended(true);
    {
        const std::lock_guard lock(pauseMutex_);
        paused_ = true;
    }
    state_ = VmState::Paused;
    CancelAll();
    VMM_INFO("VM en pause.");
}

void Vm::Resume()
{
    if (state_ != VmState::Paused) return;
    {
        const std::lock_guard lock(pauseMutex_);
        paused_ = false;
    }
    state_ = VmState::Running;
    pauseCondition_.notify_all();
    VMM_INFO("VM reprise.");
    if (qemuAgent_) {
        qemuAgent_->SetSuspended(false);
        qemuAgent_->RequestTimeSync("reprise après une pause");
    }
}

void Vm::PowerOff()
{
    VMM_INFO("Arrêt forcé de la VM demandé.");
    RequestStop(VmExit::PowerOff);
}

void Vm::Reset()
{
    VMM_INFO("Redémarrage de la VM demandé.");
    RequestStop(VmExit::Reset);
}

bool Vm::AcpiShutdown()
{
    if (state_ == VmState::Paused) Resume();
    const DeviceGuard guard(deviceLock_);
    return acpi_->PressPowerButton();
}

bool Vm::CdromEjected() const
{
    const DeviceGuard guard(deviceLock_);
    return ide_ == nullptr || ide_->CdromEjected();
}

bool Vm::HasCdrom() const
{
    const DeviceGuard guard(deviceLock_);
    return ide_ != nullptr && ide_->HasCdrom();
}

void Vm::ChangeCdrom(const std::filesystem::path& iso)
{
    // Open the new image before touching the drive: a bad file leaves the
    // current disc in place.
    auto image = iso.empty() ? nullptr : std::make_unique<virtualiseur::whp::Iso9660Image>(iso);
    std::unique_ptr<virtualiseur::whp::Iso9660Image> previous;
    {
        const DeviceGuard guard(deviceLock_);
        if (ide_ == nullptr || !ide_->HasCdrom()) throw std::runtime_error("Cette VM n'a pas de lecteur CD-ROM.");
        ide_->ChangeCdromMedia(image.get());
        previous = std::move(iso_);
        iso_ = std::move(image);
        config_.iso = iso;
    }
    if (iso.empty()) VMM_INFO("CD-ROM : disque éjecté depuis l'hôte.");
    else VMM_INFO("CD-ROM : image %s insérée.", PathUtf8(iso).c_str());
}

std::filesystem::path Vm::CdromImage() const
{
    const DeviceGuard guard(deviceLock_);
    return iso_ ? config_.iso : std::filesystem::path{};
}

bool Vm::CanConnectCable() const
{
    return virtioNet_ != nullptr && network_ != nullptr;
}

void Vm::SetCableConnected(bool connected)
{
    if (virtioNet_ == nullptr) return;
    {
        const DeviceGuard guard(deviceLock_);
        if (config_.cableConnected == connected) return;
        config_.cableConnected = connected;
    }
    {
        const DeviceGuard guard(virtioNet_->Lock());
        virtioNet_->ChangeLink(connected && network_ != nullptr);
    }
    VMM_INFO("Carte réseau : câble %s.", connected ? "branché" : "débranché");
}

bool Vm::CableConnected() const
{
    const DeviceGuard guard(deviceLock_);
    return config_.cableConnected;
}

VmConfig Vm::CurrentConfig() const
{
    const DeviceGuard guard(deviceLock_);
    return config_;
}

std::uint32_t Vm::TakeLiveSnapshot(const std::wstring& name, std::string& state, std::string& detail)
{
    if (!disk_) throw std::runtime_error("Cette machine n'a pas de disque.");

    // 1. Freeze, with no disk lock held: the guest syncs its dirty data to
    //    the disk while freezing, so its I/O must keep flowing meanwhile.
    QemuGuestAgent::FreezeResult freeze;
    if (qemuAgent_) freeze = qemuAgent_->Freeze(std::chrono::seconds(4));
    else freeze.reason = "pas de canal qemu-guest-agent";

    // 2. Thaw owed as soon as the freeze was sent, whatever happens below.
    struct ThawGuard {
        QemuGuestAgent* agent;
        bool owed;
        ~ThawGuard()
        {
            if (owed && agent != nullptr) agent->Thaw();
        }
    } thaw{qemuAgent_.get(), freeze.sent};

    const bool frozen = freeze.frozen > 0;
    state = frozen ? "frozen" : "crash";
    std::wstring description;
    if (frozen) {
        detail = std::to_string(freeze.frozen) + " système(s) de fichiers gelé(s)";
        description = L"Machine allumée, " + WidenUtf8(detail) + L".";
    } else {
        detail = "sans gel (" + freeze.reason + ") : cohérent comme après une coupure de courant";
        description = L"Machine allumée, " + WidenUtf8(detail) + L".";
    }

    // 3. New current layer (recorded first), then the switch under the
    //    disk's exclusive lock: flush, reopen read-only, new top.
    virtualiseur::whp::SnapshotStore store(config_.disk);
    const auto take = store.BeginLive(name, description, frozen ? L"frozen" : L"crash");
    try {
        disk_->PushLayer(take.layer);
    } catch (...) {
        store.AbortLive(take);
        throw;
    }
    VMM_INFO("Instantané %u pris, machine allumée : %s. Couche active : %s.", take.id, detail.c_str(),
             PathUtf8(take.layer.filename()).c_str());
    return take.id;
}

void Vm::SyncGuestTime(const char* reason)
{
    if (qemuAgent_) qemuAgent_->RequestTimeSync(reason);
}

DisplaySnapshot Vm::Display() const
{
    DisplaySnapshot snapshot;
    snapshot.mode = config_.display;
    if (config_.display == DisplayMode::Graphics && framebuffer_) {
        snapshot.framebuffer = memory_->Framebuffer();
        snapshot.width = framebuffer_->width;
        snapshot.height = framebuffer_->height;
        snapshot.stride = framebuffer_->stride;
    } else if (config_.display == DisplayMode::Text) {
        const DeviceGuard guard(deviceLock_);
        const auto start = vga_->StartOffset();
        snapshot.text = reinterpret_cast<const volatile std::uint16_t*>(
            memory_->Pointer(layout::kVgaTextBase, 0x8000)) + (start < 0x3000 ? start : 0);
        snapshot.width = 80;
        snapshot.height = 25;
        snapshot.cursor = static_cast<std::uint16_t>(vga_->CursorOffset() - start);
        snapshot.cursorVisible = vga_->CursorVisible();
    }
    return snapshot;
}

void Vm::KeyboardScanCode(std::uint8_t code)
{
    const DeviceGuard guard(deviceLock_);
    ps2_->KeyboardScanCode(code);
}

int Vm::KeyboardLeds() const
{
    const DeviceGuard guard(deviceLock_);
    return ps2_->KeyboardLeds();
}

void Vm::MouseMotion(int dx, int dy, int wheel, std::uint8_t buttons)
{
    const DeviceGuard guard(deviceLock_);
    ps2_->MouseMotion(dx, dy, wheel, buttons);
}

bool Vm::CopyGpuScanout(ScanoutCopy& copy, DirtyRows& rows)
{
    return virtioGpu_ != nullptr && virtioGpu_->CopyScanout(copy, rows);
}

void Vm::SetGpuTextureDisplay(bool enabled)
{
    if (virtioGpu_ != nullptr) virtioGpu_->SetTextureDisplay(enabled);
}

void Vm::SetGpuShareAllowed(bool allowed)
{
    if (virtioGpu_ != nullptr) virtioGpu_->SetShareAllowed(allowed);
}

bool Vm::GpuCursorSnapshot(GpuCursor& cursor)
{
    return virtioGpu_ != nullptr && virtioGpu_->CursorSnapshot(cursor);
}

void Vm::RequestDisplaySize(std::uint32_t width, std::uint32_t height)
{
    if (virtioGpu_ == nullptr) return;
    const DeviceGuard guard(virtioGpu_->Lock());
    virtioGpu_->RequestDisplaySize(width, height);
}

bool Vm::TabletReady() const
{
    if (tablet_ == nullptr) return false;
    const DeviceGuard guard(tablet_->Lock());
    return tablet_->Ready();
}

void Vm::TabletMove(std::int32_t x, std::int32_t y)
{
    if (tablet_ == nullptr) return;
    const DeviceGuard guard(tablet_->Lock());
    tablet_->Move(x, y);
}

void Vm::TabletButton(VirtioTablet::Button button, bool pressed)
{
    if (tablet_ == nullptr) return;
    const DeviceGuard guard(tablet_->Lock());
    tablet_->SetButton(button, pressed);
}

void Vm::TabletWheel(std::int32_t steps, bool horizontal)
{
    if (tablet_ == nullptr) return;
    const DeviceGuard guard(tablet_->Lock());
    tablet_->Wheel(steps, horizontal);
}

void Vm::SetAudioMuted(bool muted)
{
    if (sound_ != nullptr) sound_->SetMuted(muted);
}

void Vm::SetMicrophoneEnabled(bool enabled)
{
    {
        const DeviceGuard guard(deviceLock_);
        config_.microphone = enabled;
    }
    if (sound_ != nullptr) sound_->SetMicrophoneEnabled(enabled);
}

bool Vm::MicrophoneEnabled() const
{
    return sound_ != nullptr && sound_->MicrophoneEnabled();
}

bool Vm::SerialInput(std::uint8_t value)
{
    const DeviceGuard guard(deviceLock_);
    if (uart_->ReceiveFifoFull()) return false;
    uart_->ReceiveByte(value);
    return true;
}

std::string Vm::Statistics() const
{
    std::ostringstream text;
    text << "sorties=" << exits_.load() << " io=" << ioExits_.load() << " mmio=" << mmioExits_.load()
         << " irq_ioapic=" << ioApic_->DeliveredCount() << " irq_msi=" << ioApic_->MsiCount()
         << " ticks_pit=" << pit_->TimerInterrupts();
    if (virtioBlk_ != nullptr) text << " virtio_blk=" << virtioBlk_->CompletedRequests();
    if (virtioGpu_ != nullptr) {
        text << " gpu_flush=" << virtioGpu_->Flushes();
        if (virtioGpu_->Accelerated()) text << " gpu_submit_3d=" << virtioGpu_->Submits();
    }
    if (sound_ != nullptr) text << " audio_octets=" << sound_->PlayedBytes() << " micro_octets=" << sound_->CapturedBytes();
    if (qemuAgent_) {
        if (const auto offset = qemuAgent_->LastOffsetMs()) text << " ecart_horloge_ms=" << *offset;
        else text << " agent_qemu=" << (qemuAgent_->Connected() ? "connecte" : "absent");
    }
    for (const auto* share : shares_) text << " 9p_" << share->Tag() << "=" << share->Requests();
    return text.str();
}

// --- Port and memory dispatch ----------------------------------------------------------

// Each access takes only the lock of the device it reaches: a vCPU kicking
// the disk does not wait for another one talking to the network card.
std::uint32_t Vm::PortRead(std::uint16_t port, std::uint8_t size)
{
    if (const auto* route = ioBus_.Find(port)) {
        const DeviceGuard guard(route->lock != nullptr ? *route->lock : deviceLock_);
        return route->device->IoRead(port, size);
    }
    if (unhandledPorts_.fetch_add(1) < 32) VMM_DEBUG("Lecture du port non émulé 0x%04x (%u octets).", port, size);
    return 0xFFFFFFFFu;
}

void Vm::PortWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value)
{
    if (const auto* route = ioBus_.Find(port)) {
        const DeviceGuard guard(route->lock != nullptr ? *route->lock : deviceLock_);
        route->device->IoWrite(port, size, value);
        return;
    }
    if (unhandledPorts_.fetch_add(1) < 32) {
        VMM_DEBUG("Écriture sur le port non émulé 0x%04x = 0x%x (%u octets).", port, value, size);
    }
}

void Vm::MemoryAccess(std::uint64_t gpa, std::uint8_t* data, std::uint8_t size, bool write)
{
    MmioRoute route{};
    if (mmioBus_.Find(gpa, route)) {
        const DeviceGuard guard(route.lock != nullptr ? *route.lock : deviceLock_);
        if (write) {
            std::uint64_t value = 0;
            std::memcpy(&value, data, (std::min<std::size_t>)(size, 8));
            route.device->MmioWrite(gpa, size, value);
        } else {
            const auto value = route.device->MmioRead(gpa, size);
            std::memcpy(data, &value, (std::min<std::size_t>)(size, 8));
        }
        return;
    }
    if (auto* host = memory_->Pointer(gpa, size)) {
        if (write) std::memcpy(host, data, size);
        else std::memcpy(data, host, size);
        return;
    }
    // Unbacked physical address: reads float high, writes are discarded.
    if (!write) std::memset(data, 0xFF, size);
}

HRESULT CALLBACK Vm::EmulatorIo(void* context, WHV_EMULATOR_IO_ACCESS_INFO* access)
{
    auto& vcpu = *static_cast<VcpuContext*>(context);
    const auto size = static_cast<std::uint8_t>(access->AccessSize);
    if (access->Direction == 0) {
        access->Data = vcpu.vm->PortRead(access->Port, size);
    } else {
        vcpu.vm->PortWrite(access->Port, size, access->Data);
    }
    return S_OK;
}

HRESULT CALLBACK Vm::EmulatorMemory(void* context, WHV_EMULATOR_MEMORY_ACCESS_INFO* access)
{
    auto& vcpu = *static_cast<VcpuContext*>(context);
    vcpu.vm->MemoryAccess(access->GpaAddress, access->Data, access->AccessSize, access->Direction != 0);
    return S_OK;
}

HRESULT CALLBACK Vm::EmulatorGetRegisters(void* context, const WHV_REGISTER_NAME* names,
                                          UINT32 count, WHV_REGISTER_VALUE* values)
{
    auto& vcpu = *static_cast<VcpuContext*>(context);
    return WHvGetVirtualProcessorRegisters(vcpu.vm->partition_->Handle(), vcpu.index, names, count, values);
}

HRESULT CALLBACK Vm::EmulatorSetRegisters(void* context, const WHV_REGISTER_NAME* names,
                                          UINT32 count, const WHV_REGISTER_VALUE* values)
{
    auto& vcpu = *static_cast<VcpuContext*>(context);
    return WHvSetVirtualProcessorRegisters(vcpu.vm->partition_->Handle(), vcpu.index, names, count, values);
}

HRESULT CALLBACK Vm::EmulatorTranslate(void* context, WHV_GUEST_VIRTUAL_ADDRESS gva,
                                       WHV_TRANSLATE_GVA_FLAGS flags,
                                       WHV_TRANSLATE_GVA_RESULT_CODE* result,
                                       WHV_GUEST_PHYSICAL_ADDRESS* gpa)
{
    auto& vcpu = *static_cast<VcpuContext*>(context);
    WHV_TRANSLATE_GVA_RESULT translation{};
    const auto status = WHvTranslateGva(vcpu.vm->partition_->Handle(), vcpu.index, gva, flags, &translation, gpa);
    *result = translation.ResultCode;
    return status;
}

// --- vCPU loop -----------------------------------------------------------------------------------

void Vm::VcpuThread(std::uint32_t index)
{
    SetThreadDescription(GetCurrentThread(), (L"vCPU " + std::to_wstring(index)).c_str());
    VcpuContext context{this, index, nullptr};
    WHV_EMULATOR_CALLBACKS callbacks{};
    callbacks.Size = sizeof(callbacks);
    callbacks.WHvEmulatorIoPortCallback = &Vm::EmulatorIo;
    callbacks.WHvEmulatorMemoryCallback = &Vm::EmulatorMemory;
    callbacks.WHvEmulatorGetVirtualProcessorRegisters = &Vm::EmulatorGetRegisters;
    callbacks.WHvEmulatorSetVirtualProcessorRegisters = &Vm::EmulatorSetRegisters;
    callbacks.WHvEmulatorTranslateGvaPage = &Vm::EmulatorTranslate;
    if (FAILED(WHvEmulatorCreateEmulator(&callbacks, &context.emulator))) {
        VMM_ERROR("vCPU %u: création de l'émulateur d'instructions impossible.", index);
        RequestStop(VmExit::Error);
        return;
    }

    try {
        while (!stopRequested_.load()) {
            {
                std::unique_lock lock(pauseMutex_);
                pauseCondition_.wait(lock, [this] { return !paused_ || stopRequested_.load(); });
            }
            if (stopRequested_.load()) break;

            const auto exit = partition_->RunVirtualProcessor(index);
            ++exits_;
            switch (exit.ExitReason) {
            case WHvRunVpExitReasonCanceled:
                break;
            case WHvRunVpExitReasonX64IoPortAccess:
                ++ioExits_;
                HandleIoExit(context, exit);
                break;
            case WHvRunVpExitReasonMemoryAccess:
                ++mmioExits_;
                HandleMmioExit(context, exit);
                break;
            case WHvRunVpExitReasonX64ApicEoi:
                ioApic_->EndOfInterrupt(exit.ApicEoi.InterruptVector); // Internally locked.
                break;
            case WHvRunVpExitReasonX64Halt:
                HandleHaltExit(index);
                break;
            case WHvRunVpExitReasonUnrecoverableException:
                VMM_WARN("vCPU %u: triple faute à RIP=0x%llx, redémarrage de la VM.", index,
                         static_cast<unsigned long long>(exit.VpContext.Rip));
                RequestStop(VmExit::Reset);
                break;
            default:
                VMM_ERROR("vCPU %u: sortie WHP non gérée 0x%x à RIP=0x%llx.", index,
                          static_cast<unsigned>(exit.ExitReason), static_cast<unsigned long long>(exit.VpContext.Rip));
                RequestStop(VmExit::Error);
                break;
            }
        }
    } catch (const std::exception& error) {
        VMM_ERROR("vCPU %u: %s", index, error.what());
        RequestStop(VmExit::Error);
    }
    WHvEmulatorDestroyEmulator(context.emulator);
}

void Vm::HandleIoExit(VcpuContext& context, const WHV_RUN_VP_EXIT_CONTEXT& exit)
{
    const auto& io = exit.IoPortAccess;
    if (io.AccessInfo.StringOp || io.AccessInfo.RepPrefix) {
        WHV_EMULATOR_STATUS status{};
        const auto result = WHvEmulatorTryIoEmulation(context.emulator, &context, &exit.VpContext, &io, &status);
        if (FAILED(result) || !status.EmulationSuccessful) {
            VMM_ERROR("Émulation d'E/S chaîne impossible sur le port 0x%x (statut 0x%x).",
                      io.PortNumber, status.AsUINT32);
            RequestStop(VmExit::Error);
        }
        return;
    }

    const auto size = static_cast<std::uint8_t>(io.AccessInfo.AccessSize);
    std::array<WHV_REGISTER_NAME, 2> names{WHvX64RegisterRip, WHvX64RegisterRax};
    std::array<WHV_REGISTER_VALUE, 2> values{};
    values[0].Reg64 = exit.VpContext.Rip + exit.VpContext.InstructionLength;
    UINT32 count = 1;
    if (io.AccessInfo.IsWrite) {
        PortWrite(io.PortNumber, size, static_cast<std::uint32_t>(io.Rax));
    } else {
        const auto data = PortRead(io.PortNumber, size);
        if (size == 4) {
            values[1].Reg64 = data; // 32-bit results zero-extend into RAX.
        } else {
            const std::uint64_t mask = size == 1 ? 0xFF : 0xFFFF;
            values[1].Reg64 = (io.Rax & ~mask) | (data & mask);
        }
        count = 2;
    }
    const auto result = WHvSetVirtualProcessorRegisters(partition_->Handle(), context.index, names.data(), count, values.data());
    if (FAILED(result)) throw std::runtime_error("WHvSetVirtualProcessorRegisters a échoué après une E/S.");
}

void Vm::HandleMmioExit(VcpuContext& context, const WHV_RUN_VP_EXIT_CONTEXT& exit)
{
    WHV_EMULATOR_STATUS status{};
    const auto result = WHvEmulatorTryMmioEmulation(context.emulator, &context, &exit.VpContext,
                                                    &exit.MemoryAccess, &status);
    if (FAILED(result) || !status.EmulationSuccessful) {
        VMM_ERROR("Émulation MMIO impossible: GPA=0x%llx GVA=0x%llx RIP=0x%llx (statut 0x%x).",
                  static_cast<unsigned long long>(exit.MemoryAccess.Gpa),
                  static_cast<unsigned long long>(exit.MemoryAccess.Gva),
                  static_cast<unsigned long long>(exit.VpContext.Rip), status.AsUINT32);
        RequestStop(VmExit::Error);
    }
}

void Vm::HandleHaltExit(std::uint32_t index)
{
    // With WHP's local APIC, HLT normally waits inside the hypervisor. A HLT
    // exit with interrupts disabled means the guest stopped for good.
    const WHV_REGISTER_NAME name = WHvX64RegisterRflags;
    WHV_REGISTER_VALUE value{};
    if (FAILED(WHvGetVirtualProcessorRegisters(partition_->Handle(), index, &name, 1, &value))) return;
    if ((value.Reg64 & 0x200) != 0) return;
    if (index == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (haltSince_ == std::chrono::steady_clock::time_point{}) haltSince_ = now;
        if (now - haltSince_ > std::chrono::seconds(2)) {
            VMM_INFO("Le système invité est arrêté (HLT avec interruptions masquées).");
            RequestStop(VmExit::PowerOff);
            return;
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

} // namespace vmm
