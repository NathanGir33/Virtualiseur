#include "LinuxLoader.h"

#include "../core/Log.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace vmm {
namespace {

constexpr std::uint64_t kGdtAddress = 0x5000;
constexpr std::uint64_t kBootParamsAddress = 0x7000;
constexpr std::uint64_t kCommandLineAddress = 0x20000;
constexpr std::uint64_t kCommandLineMax = 0x10000;
constexpr std::uint64_t kPage = 4096;

template <typename T>
T Get(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    if (offset + sizeof(T) > data.size()) throw std::runtime_error("En-tête bzImage tronqué.");
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

template <typename T>
void Put(std::uint8_t* data, std::size_t offset, T value)
{
    std::memcpy(data + offset, &value, sizeof(T));
}

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void AppendCpioEntry(std::vector<std::uint8_t>& out, const std::string& name, std::uint32_t mode,
                     const std::vector<std::uint8_t>& content, std::uint32_t inode)
{
    char header[111];
    std::snprintf(header, sizeof(header), "070701%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X",
                  inode, mode, 0u, 0u, 1u, 0u, static_cast<std::uint32_t>(content.size()), 0u, 0u, 0u, 0u,
                  static_cast<std::uint32_t>(name.size() + 1), 0u);
    out.insert(out.end(), header, header + 110);
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(0);
    while (out.size() % 4 != 0) out.push_back(0);
    out.insert(out.end(), content.begin(), content.end());
    while (out.size() % 4 != 0) out.push_back(0);
}

} // namespace

void AppendFileToInitrd(std::vector<std::uint8_t>& initrd, const std::string& guestPath,
                        const std::vector<std::uint8_t>& content)
{
    while (initrd.size() % 4 != 0) initrd.push_back(0);
    std::string name = guestPath;
    while (!name.empty() && name.front() == '/') name.erase(name.begin());
    std::vector<std::uint8_t> archive;
    AppendCpioEntry(archive, name, 0100644, content, 0x5600);
    AppendCpioEntry(archive, "TRAILER!!!", 0, {}, 0);
    while (archive.size() % 512 != 0) archive.push_back(0);
    initrd.insert(initrd.end(), archive.begin(), archive.end());
}

LinuxBootInfo LoadLinux(const GuestMemory& memory, const LinuxImage& image,
                        std::uint64_t rsdpAddress, const std::optional<FramebufferInfo>& framebuffer)
{
    const auto& kernel = image.kernel;
    if (kernel.size() < 0x270 || Get<std::uint16_t>(kernel, 0x1FE) != 0xAA55 ||
        Get<std::uint32_t>(kernel, 0x202) != 0x53726448) {
        throw std::runtime_error("Le noyau n'est pas une image bzImage Linux x86.");
    }
    LinuxBootInfo info{};
    info.protocol = Get<std::uint16_t>(kernel, 0x206);
    if (info.protocol < 0x020A || (kernel[0x211] & 0x01) == 0) {
        throw std::runtime_error("Protocole de démarrage Linux >= 2.10 (noyau 2.6.31+) requis.");
    }
    std::uint32_t setupSectors = kernel[0x1F1];
    if (setupSectors == 0) setupSectors = 4;
    const std::size_t payloadOffset = (setupSectors + 1) * 512ull;
    if (payloadOffset >= kernel.size()) throw std::runtime_error("bzImage sans noyau protégé.");
    const std::size_t payloadSize = kernel.size() - payloadOffset;

    const bool relocatable = kernel[0x234] != 0;
    std::uint64_t alignment = Get<std::uint32_t>(kernel, 0x230);
    if (alignment < kPage || (alignment & (alignment - 1)) != 0) alignment = 0x200000;
    const auto preferred = Get<std::uint64_t>(kernel, 0x258);
    const auto initSize = static_cast<std::uint64_t>(Get<std::uint32_t>(kernel, 0x260));
    const auto required = (std::max<std::uint64_t>)(payloadSize, initSize);

    // Load at the preferred address (16 MiB on x86_64) so the decompressor
    // does not have to move itself; fall back to the lowest aligned address.
    std::uint64_t kernelAddress = preferred != 0 ? preferred : 0x100000;
    if (!relocatable) kernelAddress = 0x100000;
    if (kernelAddress + required > memory.LowRamEnd()) {
        if (!relocatable) throw std::runtime_error("RAM invitée insuffisante pour ce noyau.");
        kernelAddress = AlignUp(0x100000, alignment);
    }
    if (kernelAddress + required > memory.LowRamEnd()) {
        throw std::runtime_error("RAM invitée insuffisante pour décompresser le noyau.");
    }
    if (!memory.Write(kernelAddress, kernel.data() + payloadOffset, payloadSize)) {
        throw std::runtime_error("Impossible de copier le noyau en mémoire invitée.");
    }
    info.kernelAddress = kernelAddress;
    info.entryPoint = kernelAddress;

    // boot_params ("zero page") with a copy of the setup header.
    std::array<std::uint8_t, 4096> params{};
    const std::size_t headerEnd = (std::min<std::size_t>)(kernel.size(), 0x202 + kernel[0x201]);
    std::memcpy(params.data() + 0x1F1, kernel.data() + 0x1F1, headerEnd - 0x1F1);
    auto* bp = params.data();
    Put<std::uint8_t>(bp, 0x210, 0xFF);               // type_of_loader: undefined.
    bp[0x211] = static_cast<std::uint8_t>(bp[0x211] & ~0x20); // Not quiet: keep early messages.
    Put<std::uint32_t>(bp, 0x214, static_cast<std::uint32_t>(kernelAddress)); // code32_start

    // Screen information.
    if (framebuffer) {
        const auto size = static_cast<std::uint64_t>(framebuffer->stride) * framebuffer->height;
        Put<std::uint8_t>(bp, 0x0F, 0x23); // VIDEO_TYPE_VLFB
        Put<std::uint16_t>(bp, 0x12, static_cast<std::uint16_t>(framebuffer->width));
        Put<std::uint16_t>(bp, 0x14, static_cast<std::uint16_t>(framebuffer->height));
        Put<std::uint16_t>(bp, 0x16, 32);
        Put<std::uint32_t>(bp, 0x18, static_cast<std::uint32_t>(framebuffer->base));
        Put<std::uint32_t>(bp, 0x1C, static_cast<std::uint32_t>((size + 0xFFFF) >> 16)); // 64 KiB units.
        Put<std::uint16_t>(bp, 0x24, static_cast<std::uint16_t>(framebuffer->stride));
        Put<std::uint8_t>(bp, 0x26, 8);  // red size
        Put<std::uint8_t>(bp, 0x27, 16); // red position
        Put<std::uint8_t>(bp, 0x28, 8);
        Put<std::uint8_t>(bp, 0x29, 8);
        Put<std::uint8_t>(bp, 0x2A, 8);
        Put<std::uint8_t>(bp, 0x2B, 0);
        Put<std::uint8_t>(bp, 0x2C, 8);
        Put<std::uint8_t>(bp, 0x2D, 24);
        Put<std::uint16_t>(bp, 0x32, 1); // pages
    } else {
        Put<std::uint8_t>(bp, 0x06, 0x03); // 80x25 colour text.
        Put<std::uint8_t>(bp, 0x07, 80);
        Put<std::uint16_t>(bp, 0x0A, 0x0010);
        Put<std::uint8_t>(bp, 0x0E, 25);
        Put<std::uint8_t>(bp, 0x0F, 0x22); // VIDEO_TYPE_VGAC
        Put<std::uint16_t>(bp, 0x10, 16);
    }

    // ACPI root pointer and memory map.
    Put<std::uint64_t>(bp, 0x070, rsdpAddress);
    const auto& e820 = memory.E820();
    const auto count = (std::min<std::size_t>)(e820.size(), 128);
    for (std::size_t index = 0; index < count; ++index) {
        Put<std::uint64_t>(bp, 0x2D0 + index * 20, e820[index].address);
        Put<std::uint64_t>(bp, 0x2D0 + index * 20 + 8, e820[index].size);
        Put<std::uint32_t>(bp, 0x2D0 + index * 20 + 16, e820[index].type);
    }
    bp[0x1E8] = static_cast<std::uint8_t>(count);

    // Command line.
    const auto maxCommandLine = Get<std::uint32_t>(kernel, 0x238);
    if (image.commandLine.size() >= kCommandLineMax ||
        (maxCommandLine != 0 && image.commandLine.size() > maxCommandLine)) {
        throw std::runtime_error("Ligne de commande du noyau trop longue.");
    }
    if (!memory.Write(kCommandLineAddress, image.commandLine.c_str(), image.commandLine.size() + 1)) {
        throw std::runtime_error("Impossible d'écrire la ligne de commande du noyau.");
    }
    Put<std::uint32_t>(bp, 0x228, static_cast<std::uint32_t>(kCommandLineAddress));

    // Initrd at the top of low RAM, below the kernel's address limit.
    if (!image.initrd.empty()) {
        std::uint64_t limit = Get<std::uint32_t>(kernel, 0x22C);
        if (limit == 0) limit = 0x37FFFFFF;
        const auto top = (std::min<std::uint64_t>)(memory.LowRamEnd(), limit + 1);
        if (image.initrd.size() > top) throw std::runtime_error("Initrd trop volumineux pour la RAM invitée.");
        const auto address = (top - image.initrd.size()) & ~(kPage - 1);
        if (address < kernelAddress + required) {
            throw std::runtime_error("RAM invitée insuffisante pour le noyau et l'initrd (augmente la mémoire).");
        }
        if (!memory.Write(address, image.initrd.data(), image.initrd.size())) {
            throw std::runtime_error("Impossible de copier l'initrd en mémoire invitée.");
        }
        Put<std::uint32_t>(bp, 0x218, static_cast<std::uint32_t>(address));
        Put<std::uint32_t>(bp, 0x21C, static_cast<std::uint32_t>(image.initrd.size()));
        info.initrdAddress = address;
    }

    if (!memory.Write(kBootParamsAddress, params.data(), params.size())) {
        throw std::runtime_error("Impossible d'écrire boot_params.");
    }

    // GDT with the boot protocol's __BOOT_CS (0x10) and __BOOT_DS (0x18).
    constexpr std::array<std::uint64_t, 4> gdt{0, 0, 0x00CF9A000000FFFFull, 0x00CF92000000FFFFull};
    if (!memory.Write(kGdtAddress, gdt.data(), sizeof(gdt))) throw std::runtime_error("GDT invalide.");

    info.bootParams = kBootParamsAddress;
    VMM_INFO("Noyau Linux (protocole %u.%02u) chargé à 0x%llx, initrd %zu octets à 0x%llx.",
             info.protocol >> 8, info.protocol & 0xFF, static_cast<unsigned long long>(kernelAddress),
             image.initrd.size(), static_cast<unsigned long long>(info.initrdAddress));
    return info;
}

void SetupLinuxBootCpu(virtualiseur::whp::Partition& partition, std::uint32_t index, const LinuxBootInfo& info)
{
    const auto segment = [](std::uint16_t selector, std::uint16_t attributes) {
        WHV_X64_SEGMENT_REGISTER value{};
        value.Base = 0;
        value.Limit = 0xFFFFFFFF;
        value.Selector = selector;
        value.Attributes = attributes;
        return value;
    };
    const std::array<WHV_REGISTER_NAME, 18> names{
        WHvX64RegisterRax, WHvX64RegisterRbx, WHvX64RegisterRbp, WHvX64RegisterRdi,
        WHvX64RegisterRsi, WHvX64RegisterRsp, WHvX64RegisterRip, WHvX64RegisterRflags,
        WHvX64RegisterGdtr, WHvX64RegisterCs, WHvX64RegisterDs, WHvX64RegisterEs,
        WHvX64RegisterSs, WHvX64RegisterFs, WHvX64RegisterGs, WHvX64RegisterCr0,
        WHvX64RegisterCr4, WHvX64RegisterEfer,
    };
    std::array<WHV_REGISTER_VALUE, names.size()> values{};
    values[4].Reg64 = info.bootParams;
    values[5].Reg64 = 0x8FF0;
    values[6].Reg64 = info.entryPoint;
    values[7].Reg64 = 0x2;
    values[8].Table.Base = kGdtAddress;
    values[8].Table.Limit = 4 * 8 - 1;
    values[9].Segment = segment(0x10, 0xC09B);
    values[10].Segment = segment(0x18, 0xC093);
    values[11].Segment = segment(0x18, 0xC093);
    values[12].Segment = segment(0x18, 0xC093);
    values[13].Segment = segment(0x18, 0xC093);
    values[14].Segment = segment(0x18, 0xC093);
    values[15].Reg64 = 0x11; // PE | ET
    values[16].Reg64 = 0;
    values[17].Reg64 = 0;
    partition.SetVirtualProcessorRegisters(index, names, values);
}

void ParkApplicationProcessor(virtualiseur::whp::Partition& partition, std::uint32_t index)
{
    const WHV_REGISTER_NAME name = WHvRegisterInternalActivityState;
    WHV_REGISTER_VALUE value{};
    value.InternalActivity.StartupSuspend = 1;
    partition.SetVirtualProcessorRegisters(index, std::span(&name, 1), std::span(&value, 1));
}

} // namespace vmm
