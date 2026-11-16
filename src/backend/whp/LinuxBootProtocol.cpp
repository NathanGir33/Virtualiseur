#include "LinuxBootProtocol.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace virtualiseur::whp {
namespace {

constexpr std::uint64_t kPageSize = 4096;
constexpr std::uint64_t kLowMemorySize = 0xA0000;
constexpr std::uint64_t kLowRamEnd = 0x9F000;
constexpr std::uint64_t kHighMemoryStart = 0x100000;
constexpr std::uint64_t kPciHoleStart = 0xC0000000;
constexpr std::uint64_t kFourGiB = 0x100000000ull;
constexpr std::uint64_t kBootParamsAddress = 0x7000;
constexpr std::uint64_t kCommandLineAddress = 0x20000;
constexpr std::uint64_t kGdtAddress = 0x5000;
constexpr std::uint64_t kRsdpAddress = 0xE0000;
constexpr std::uint64_t kRsdtAddress = 0xE0100;
constexpr std::uint64_t kMadtAddress = 0xE0200;
constexpr std::size_t kBootParamsSize = 4096;
constexpr std::size_t kSetupHeaderOffset = 0x1F1;
constexpr std::size_t kScreenInfoOrigVideoModeOffset = 0x06;
constexpr std::size_t kScreenInfoVideoColumnsOffset = 0x07;
constexpr std::size_t kScreenInfoEgaBxOffset = 0x0A;
constexpr std::size_t kScreenInfoVideoLinesOffset = 0x0E;
constexpr std::size_t kScreenInfoVideoTypeOffset = 0x0F;
constexpr std::size_t kScreenInfoVideoPointsOffset = 0x10;
constexpr std::size_t kE820EntryCountOffset = 0x1E8;
constexpr std::size_t kE820TableOffset = 0x2D0;
constexpr std::size_t kE820EntrySize = 20;
constexpr std::size_t kAcpiRsdpAddressOffset = 0x070;
constexpr std::size_t kMaxE820Entries = 128;
constexpr std::size_t kCommandLinePointerOffset = 0x228;
constexpr std::size_t kRamdiskImageOffset = 0x218;
constexpr std::size_t kRamdiskSizeOffset = 0x21C;
constexpr std::size_t kExtRamdiskImageOffset = 0x0C0;
constexpr std::size_t kExtRamdiskSizeOffset = 0x0C4;
constexpr std::size_t kTypeOfLoaderOffset = 0x210;
constexpr std::size_t kLoadFlagsOffset = 0x211;
constexpr std::size_t kSetupSectorsOffset = 0x1F1;
constexpr std::size_t kBootFlagOffset = 0x1FE;
constexpr std::size_t kHeaderOffset = 0x202;
constexpr std::size_t kVersionOffset = 0x206;
constexpr std::size_t kCode32StartOffset = 0x214;
constexpr std::size_t kInitrdAddressMaxOffset = 0x22C;
constexpr std::size_t kKernelAlignmentOffset = 0x230;
constexpr std::size_t kRelocatableKernelOffset = 0x234;
constexpr std::size_t kInitSizeOffset = 0x260;
constexpr std::uint16_t kLinuxBootFlag = 0xAA55;
constexpr std::uint16_t kProtocolVersionBzImage = 0x0200;
constexpr std::uint8_t kLoadHighFlag = 0x01;
constexpr std::uint32_t kE820Ram = 1;
constexpr std::uint32_t kE820Reserved = 2;

std::uint16_t ReadU16(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    if (offset > data.size() || data.size() - offset < sizeof(std::uint16_t)) {
        throw std::runtime_error("L'en-tete bzImage est tronque.");
    }
    std::uint16_t value{};
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    if (offset > data.size() || data.size() - offset < sizeof(std::uint32_t)) {
        throw std::runtime_error("L'en-tete bzImage est tronque.");
    }
    std::uint32_t value{};
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

void WriteU32(std::uint8_t* destination, std::uint32_t value)
{
    std::memcpy(destination, &value, sizeof(value));
}

void WriteU16(std::uint8_t* destination, std::uint16_t value)
{
    std::memcpy(destination, &value, sizeof(value));
}

void FinalizeAcpiChecksum(std::uint8_t* table, std::size_t size, std::size_t checksumOffset)
{
    table[checksumOffset] = 0;
    std::uint8_t sum = 0;
    for (std::size_t index = 0; index < size; ++index) {
        sum = static_cast<std::uint8_t>(sum + table[index]);
    }
    table[checksumOffset] = static_cast<std::uint8_t>(0u - sum);
}

void WriteMinimalAcpiTables(std::uint8_t* firmwareMemory)
{
    constexpr std::size_t rsdpOffset = kRsdpAddress - 0xA0000;
    constexpr std::size_t rsdtOffset = kRsdtAddress - 0xA0000;
    constexpr std::size_t madtOffset = kMadtAddress - 0xA0000;
    constexpr std::array<std::uint8_t, 6> oemId{'V', 'I', 'R', 'T', 'U', 'A'};
    constexpr std::array<std::uint8_t, 8> tableId{'V', 'I', 'R', 'T', 'U', 'A', 'L', ' '};

    std::array<std::uint8_t, 20> rsdp{};
    std::memcpy(rsdp.data(), "RSD PTR ", 8);
    std::memcpy(rsdp.data() + 9, oemId.data(), oemId.size());
    rsdp[15] = 0; // ACPI 1.0 RSDP.
    WriteU32(rsdp.data() + 16, static_cast<std::uint32_t>(kRsdtAddress));
    FinalizeAcpiChecksum(rsdp.data(), rsdp.size(), 8);

    std::array<std::uint8_t, 40> rsdt{};
    std::memcpy(rsdt.data(), "RSDT", 4);
    WriteU32(rsdt.data() + 4, static_cast<std::uint32_t>(rsdt.size()));
    rsdt[8] = 1;
    std::memcpy(rsdt.data() + 10, oemId.data(), oemId.size());
    std::memcpy(rsdt.data() + 16, tableId.data(), tableId.size());
    WriteU32(rsdt.data() + 24, 1);
    std::memcpy(rsdt.data() + 28, "VIRT", 4);
    WriteU32(rsdt.data() + 32, 1);
    WriteU32(rsdt.data() + 36, static_cast<std::uint32_t>(kMadtAddress));
    FinalizeAcpiChecksum(rsdt.data(), rsdt.size(), 9);

    std::array<std::uint8_t, 52> madt{};
    std::memcpy(madt.data(), "APIC", 4);
    WriteU32(madt.data() + 4, static_cast<std::uint32_t>(madt.size()));
    madt[8] = 1;
    std::memcpy(madt.data() + 10, oemId.data(), oemId.size());
    std::memcpy(madt.data() + 16, tableId.data(), tableId.size());
    WriteU32(madt.data() + 24, 1);
    std::memcpy(madt.data() + 28, "VIRT", 4);
    WriteU32(madt.data() + 32, 1);
    WriteU32(madt.data() + 36, 0xFEE00000); // Local APIC MMIO base.
    WriteU32(madt.data() + 40, 1); // Legacy 8259 PIC is present.
    madt[44] = 0; // Processor Local APIC structure.
    madt[45] = 8;
    madt[46] = 0;
    madt[47] = 0;
    WriteU32(madt.data() + 48, 1); // One enabled processor.
    FinalizeAcpiChecksum(madt.data(), madt.size(), 9);

    std::memcpy(firmwareMemory + rsdpOffset, rsdp.data(), rsdp.size());
    std::memcpy(firmwareMemory + rsdtOffset, rsdt.data(), rsdt.size());
    std::memcpy(firmwareMemory + madtOffset, madt.data(), madt.size());
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("Impossible d'ouvrir le fichier: " + path.string());
    }
    const auto end = input.tellg();
    if (end <= 0 || static_cast<std::uintmax_t>(end) >
                         static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)())) {
        throw std::runtime_error("Fichier vide ou trop volumineux: " + path.string());
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        throw std::runtime_error("Lecture incomplete du fichier: " + path.string());
    }
    return data;
}

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment)
{
    if (alignment == 0 || value > (std::numeric_limits<std::uint64_t>::max)() - (alignment - 1)) {
        throw std::runtime_error("Alignement memoire Linux invalide.");
    }
    return ((value + alignment - 1) / alignment) * alignment;
}

void AddE820Entry(std::uint8_t* bootParams, std::size_t& count,
                  std::uint64_t address, std::uint64_t size, std::uint32_t type)
{
    if (size == 0 || count >= kMaxE820Entries) {
        return;
    }
    auto* entry = bootParams + kE820TableOffset + count * kE820EntrySize;
    std::memcpy(entry, &address, sizeof(address));
    std::memcpy(entry + 8, &size, sizeof(size));
    std::memcpy(entry + 16, &type, sizeof(type));
    ++count;
}

WHV_X64_SEGMENT_REGISTER FlatSegment(std::uint16_t selector, std::uint16_t attributes)
{
    WHV_X64_SEGMENT_REGISTER segment{};
    segment.Limit = 0xFFFFFFFF;
    segment.Selector = selector;
    segment.Attributes = attributes;
    return segment;
}

} // namespace

LinuxBootImage::LinuxBootImage(
    Partition& partition,
    const std::filesystem::path& kernelPath,
    const std::filesystem::path& initrdPath,
    std::string commandLine,
    std::uint64_t guestRamSizeBytes)
    : partition_(partition), guestRamSizeBytes_(guestRamSizeBytes)
{
    if (guestRamSizeBytes_ < 64ull * 1024 * 1024 ||
        guestRamSizeBytes_ > 0x100000000ull ||
        (guestRamSizeBytes_ % kPageSize) != 0) {
        throw std::invalid_argument("La RAM doit etre alignee sur 4 KiB et comprise entre 64 MiB et 4 GiB.");
    }
    if (guestRamSizeBytes_ <= kHighMemoryStart) {
        throw std::invalid_argument("La VM doit avoir de la RAM au-dessus de 1 MiB.");
    }
    if (commandLine.find('\0') != std::string::npos) {
        throw std::invalid_argument("La ligne de commande Linux contient un octet NUL.");
    }

    const auto kernel = ReadFile(kernelPath);
    if (kernel.size() < kHeaderOffset + 0x40 || ReadU16(kernel, kBootFlagOffset) != kLinuxBootFlag ||
        ReadU32(kernel, kHeaderOffset) != 0x53726448 /* 'HdrS' */) {
        throw std::runtime_error("Le fichier ne ressemble pas a une image de demarrage Linux x86.");
    }
    const auto protocolVersion = ReadU16(kernel, kVersionOffset);
    const auto loadFlags = kernel[kLoadFlagsOffset];
    if (protocolVersion < kProtocolVersionBzImage || (loadFlags & kLoadHighFlag) == 0) {
        throw std::runtime_error("Le noyau doit etre un bzImage Linux x86 (protocole >= 2.00).");
    }

    auto setupSectors = kernel[kSetupSectorsOffset];
    if (setupSectors == 0) {
        setupSectors = 4;
    }
    const std::size_t payloadOffset = (static_cast<std::size_t>(setupSectors) + 1) * 512;
    if (payloadOffset >= kernel.size()) {
        throw std::runtime_error("Le bzImage ne contient pas de noyau protege apres sa zone setup.");
    }
    const std::size_t headerLength = kernel.size() > 0x201 ? kernel[0x201] : 0;
    const std::size_t headerEnd = (std::min)(
        kernel.size(), (std::max)(kHeaderOffset + 0x40, 0x202 + headerLength));

    std::uint64_t alignment = protocolVersion >= 0x0205 ? ReadU32(kernel, kKernelAlignmentOffset) : kPageSize;
    if (alignment < kPageSize || (alignment & (alignment - 1)) != 0) {
        alignment = kPageSize;
    }
    const bool relocatable = protocolVersion >= 0x0205 && kernel[kRelocatableKernelOffset] != 0;
    kernelLoadAddress_ = relocatable ? AlignUp(kHighMemoryStart, alignment) : kHighMemoryStart;
    kernelImageSize_ = kernel.size() - payloadOffset;
    entryPoint_ = ReadU32(kernel, kCode32StartOffset);
    if (entryPoint_ == 0 || (!relocatable && entryPoint_ != kHighMemoryStart)) {
        throw std::runtime_error("Point d'entree 32 bits bzImage non pris en charge par ce chargeur.");
    }
    if (relocatable) {
        entryPoint_ = kernelLoadAddress_;
    }
    const auto initSize = protocolVersion >= 0x020A ? ReadU32(kernel, kInitSizeOffset) : 0;
    const auto requiredKernelMemory = (std::max)(
        kernelImageSize_, static_cast<std::uint64_t>(initSize));
    if (kernelLoadAddress_ >= guestRamSizeBytes_ ||
        requiredKernelMemory > guestRamSizeBytes_ - kernelLoadAddress_) {
        throw std::runtime_error("La RAM invite est trop petite pour le noyau et sa zone de decompression.");
    }

    // Keep RAM below the VGA hole and above 1 MiB as separate guest mappings.
    lowMemory_ = static_cast<std::uint8_t*>(partition_.AllocateAndMapGuestMemory(0, kLowMemorySize));
    // Model the legacy VGA/BIOS address window as zero-filled reserved memory.
    // Linux boot code probes this range even though it is not reported as E820 RAM.
    (void)partition_.AllocateAndMapGuestMemory(0xA0000, 0x60000);
    if (guestRamSizeBytes_ <= kPciHoleStart) {
        const auto highSize = guestRamSizeBytes_ - kHighMemoryStart;
        highMemory_ = static_cast<std::uint8_t*>(
            partition_.AllocateAndMapGuestMemory(kHighMemoryStart, static_cast<std::size_t>(highSize)));
    } else {
        // A PC reserves 3-4 GiB for PCI/MMIO. RAM displaced by that window
        // is remapped above 4 GiB and reported separately in E820.
        const auto lowHighSize = kPciHoleStart - kHighMemoryStart;
        highMemory_ = static_cast<std::uint8_t*>(
            partition_.AllocateAndMapGuestMemory(kHighMemoryStart, static_cast<std::size_t>(lowHighSize)));
        (void)partition_.AllocateAndMapGuestMemory(
            kPciHoleStart, static_cast<std::size_t>(kFourGiB - kPciHoleStart));
        const auto remappedSize = guestRamSizeBytes_ - kPciHoleStart;
        (void)partition_.AllocateAndMapGuestMemory(
            kFourGiB, static_cast<std::size_t>(remappedSize));
    }

    std::memcpy(highMemory_ + (kernelLoadAddress_ - kHighMemoryStart),
                kernel.data() + payloadOffset, static_cast<std::size_t>(kernelImageSize_));

    auto* bootParams = lowMemory_ + kBootParamsAddress;
    std::memset(bootParams, 0, kBootParamsSize);
    if (headerEnd > kSetupHeaderOffset) {
        const auto setupHeaderSize = headerEnd - kSetupHeaderOffset;
        std::memcpy(bootParams + kSetupHeaderOffset, kernel.data() + kSetupHeaderOffset, setupHeaderSize);
    }
    // Provide the Linux boot protocol's screen_info for 80x25 color VGA text
    // mode. The probe maps guest VGA text memory at 0xB8000 for the host window.
    bootParams[kScreenInfoOrigVideoModeOffset] = 0x03;
    bootParams[kScreenInfoVideoColumnsOffset] = 80;
    WriteU16(bootParams + kScreenInfoEgaBxOffset, 0x0010);
    bootParams[kScreenInfoVideoLinesOffset] = 25;
    bootParams[kScreenInfoVideoTypeOffset] = 0x22; // VIDEO_TYPE_VGAC
    WriteU16(bootParams + kScreenInfoVideoPointsOffset, 16);
    bootParams[kTypeOfLoaderOffset] = 0xFF;
    bootParams[kLoadFlagsOffset] = static_cast<std::uint8_t>(bootParams[kLoadFlagsOffset] & ~0x80);
    WriteU32(bootParams + kCommandLinePointerOffset, static_cast<std::uint32_t>(kCommandLineAddress));
    const auto firmwareMemory = static_cast<std::uint8_t*>(
        partition_.GetHostAddressForGuestPhysicalAddress(0xA0000, 0x60000));
    if (firmwareMemory == nullptr) {
        throw std::runtime_error("La zone memoire reservee au firmware ACPI est inaccessible.");
    }
    WriteMinimalAcpiTables(firmwareMemory);
    const std::uint64_t rsdpAddress = kRsdpAddress;
    std::memcpy(bootParams + kAcpiRsdpAddressOffset, &rsdpAddress, sizeof(rsdpAddress));
    if (relocatable) {
        WriteU32(bootParams + kCode32StartOffset, static_cast<std::uint32_t>(entryPoint_));
    }

    const auto maxCommandLine = protocolVersion >= 0x0206 ? ReadU32(kernel, 0x238) : 255u;
    if (commandLine.size() > maxCommandLine ||
        commandLine.size() + 1 > kLowMemorySize - kCommandLineAddress) {
        throw std::invalid_argument("La ligne de commande depasse la limite annoncee par le noyau.");
    }
    std::memcpy(lowMemory_ + kCommandLineAddress, commandLine.c_str(), commandLine.size() + 1);

    std::size_t e820Count = 0;
    AddE820Entry(bootParams, e820Count, 0, kLowRamEnd, kE820Ram);
    AddE820Entry(bootParams, e820Count, kLowRamEnd, 0x100000 - kLowRamEnd, kE820Reserved);
    if (guestRamSizeBytes_ <= kPciHoleStart) {
        AddE820Entry(bootParams, e820Count, kHighMemoryStart,
                     guestRamSizeBytes_ - kHighMemoryStart, kE820Ram);
    } else {
        AddE820Entry(bootParams, e820Count, kHighMemoryStart,
                     kPciHoleStart - kHighMemoryStart, kE820Ram);
        // Leave the PCI aperture as an E820 gap. Linux uses gaps below 4 GiB
        // to place 32-bit PCI resources; marking the whole hole as an explicit
        // reserved entry prevents its PCI gap allocator from finding space.
        AddE820Entry(bootParams, e820Count, kFourGiB,
                     guestRamSizeBytes_ - kPciHoleStart, kE820Ram);
    }
    bootParams[kE820EntryCountOffset] = static_cast<std::uint8_t>(e820Count);

    if (!initrdPath.empty()) {
        const auto initrd = ReadFile(initrdPath);
        const auto initrdMaximum = protocolVersion >= 0x0203 ? ReadU32(kernel, kInitrdAddressMaxOffset) : 0;
        const auto maximumAddress = initrdMaximum == 0 ? 0x37FFFFFFull : initrdMaximum;
        if (initrd.size() > guestRamSizeBytes_ - kernelLoadAddress_ - requiredKernelMemory) {
            throw std::runtime_error("L'initrd et la zone noyau ne tiennent pas dans la RAM invite.");
        }
        const auto availableTop = (std::min)(
            guestRamSizeBytes_, static_cast<std::uint64_t>(maximumAddress + 1));
        if (availableTop < initrd.size()) {
            throw std::runtime_error("L'initrd depasse la limite d'adresse annoncee par le noyau.");
        }
        initrdAddress_ = (availableTop - initrd.size()) & ~(kPageSize - 1);
        if (initrdAddress_ < kernelLoadAddress_ + requiredKernelMemory || initrdAddress_ < kHighMemoryStart) {
            throw std::runtime_error("Pas assez de RAM libre pour placer l'initrd sans recouvrir le noyau.");
        }
        initrdSize_ = initrd.size();
        std::memcpy(highMemory_ + (initrdAddress_ - kHighMemoryStart), initrd.data(), initrd.size());

        WriteU32(bootParams + kRamdiskImageOffset, static_cast<std::uint32_t>(initrdAddress_));
        WriteU32(bootParams + kRamdiskSizeOffset, static_cast<std::uint32_t>(initrdSize_));
        WriteU32(bootParams + kExtRamdiskImageOffset, static_cast<std::uint32_t>(initrdAddress_ >> 32));
        WriteU32(bootParams + kExtRamdiskSizeOffset, static_cast<std::uint32_t>(initrdSize_ >> 32));
    }

    constexpr std::array<std::uint64_t, 3> gdt{
        0,
        0x00CF9A000000FFFF,
        0x00CF92000000FFFF,
    };
    std::memcpy(lowMemory_ + kGdtAddress, gdt.data(), sizeof(gdt));
}

std::uint64_t LinuxBootImage::EntryPoint() const noexcept { return entryPoint_; }
std::uint64_t LinuxBootImage::BootParamsAddress() const noexcept { return kBootParamsAddress; }
std::uint64_t LinuxBootImage::KernelLoadAddress() const noexcept { return kernelLoadAddress_; }
std::uint64_t LinuxBootImage::KernelImageSize() const noexcept { return kernelImageSize_; }
std::uint64_t LinuxBootImage::InitrdAddress() const noexcept { return initrdAddress_; }
std::uint64_t LinuxBootImage::InitrdSize() const noexcept { return initrdSize_; }
std::uint64_t LinuxBootImage::GuestRamSize() const noexcept { return guestRamSizeBytes_; }

void LinuxBootImage::ConfigureBootVcpu(std::uint32_t virtualProcessorIndex)
{
    const std::array<WHV_REGISTER_NAME, 17> names{
        WHvX64RegisterRax, WHvX64RegisterRbx, WHvX64RegisterRbp, WHvX64RegisterRdi,
        WHvX64RegisterRsi, WHvX64RegisterRsp, WHvX64RegisterRip, WHvX64RegisterRflags,
        WHvX64RegisterGdtr, WHvX64RegisterCs, WHvX64RegisterDs, WHvX64RegisterEs,
        WHvX64RegisterSs, WHvX64RegisterCr0, WHvX64RegisterCr3, WHvX64RegisterCr4,
        WHvX64RegisterEfer,
    };
    std::array<WHV_REGISTER_VALUE, names.size()> values{};
    values[1].Reg64 = 0; // EBX = 0 at Linux 32-bit entry.
    values[2].Reg64 = 0; // EBP = 0.
    values[3].Reg64 = 0; // EDI = 0.
    values[4].Reg64 = kBootParamsAddress; // ESI points to boot_params.
    values[5].Reg64 = 0x8FF0;
    values[6].Reg64 = entryPoint_;
    values[7].Reg64 = 0x2; // Interrupts disabled.
    values[8].Table.Base = kGdtAddress;
    values[8].Table.Limit = 23;
    values[9].Segment = FlatSegment(0x10, 0xC09B);
    values[10].Segment = FlatSegment(0x18, 0xC093);
    values[11].Segment = FlatSegment(0x18, 0xC093);
    values[12].Segment = FlatSegment(0x18, 0xC093);
    values[13].Reg64 = 0x11; // CR0.ET | CR0.PE; paging disabled.
    values[14].Reg64 = 0;
    values[15].Reg64 = 0;
    values[16].Reg64 = 0;
    partition_.SetVirtualProcessorRegisters(virtualProcessorIndex, names, values);
}

} // namespace virtualiseur::whp
