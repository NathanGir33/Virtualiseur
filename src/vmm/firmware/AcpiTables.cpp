#include "AcpiTables.h"

#include "../devices/AcpiPm.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace vmm {
namespace {

constexpr std::uint64_t kRsdpAddress = 0xE0000;
constexpr std::uint64_t kRsdtAddress = 0xE0040;
constexpr std::uint64_t kFadtAddress = 0xE0100;
constexpr std::uint64_t kFacsAddress = 0xE0200;
constexpr std::uint64_t kMadtAddress = 0xE0300;
constexpr std::uint64_t kDsdtAddress = 0xE0800;

using Bytes = std::vector<std::uint8_t>;

void Put8(Bytes& data, std::size_t offset, std::uint8_t value) { data[offset] = value; }
void Put16(Bytes& data, std::size_t offset, std::uint16_t value) { std::memcpy(&data[offset], &value, 2); }
void Put32(Bytes& data, std::size_t offset, std::uint32_t value) { std::memcpy(&data[offset], &value, 4); }
void Put64(Bytes& data, std::size_t offset, std::uint64_t value) { std::memcpy(&data[offset], &value, 8); }

void Checksum(Bytes& data, std::size_t checksumOffset, std::size_t length)
{
    data[checksumOffset] = 0;
    std::uint8_t sum = 0;
    for (std::size_t index = 0; index < length; ++index) sum = static_cast<std::uint8_t>(sum + data[index]);
    data[checksumOffset] = static_cast<std::uint8_t>(0u - sum);
}

Bytes TableHeader(std::string_view signature, std::size_t length, std::uint8_t revision)
{
    Bytes table(length, 0);
    std::memcpy(table.data(), signature.data(), 4);
    Put32(table, 4, static_cast<std::uint32_t>(length));
    Put8(table, 8, revision);
    std::memcpy(&table[10], "VIRTUA", 6);
    std::memcpy(&table[16], "VIRTWHP ", 8);
    Put32(table, 24, 1);
    std::memcpy(&table[28], "VRTL", 4);
    Put32(table, 32, 1);
    return table;
}

// --- Minimal AML encoder -----------------------------------------------------

Bytes PkgLength(std::size_t contentLength)
{
    if (contentLength + 1 <= 0x3F) return {static_cast<std::uint8_t>(contentLength + 1)};
    if (contentLength + 2 <= 0xFFF) {
        const auto length = contentLength + 2;
        return {static_cast<std::uint8_t>(0x40 | (length & 0x0F)), static_cast<std::uint8_t>(length >> 4)};
    }
    const auto length = contentLength + 3;
    if (length > 0xFFFFF) throw std::length_error("AML package trop grand.");
    return {static_cast<std::uint8_t>(0x80 | (length & 0x0F)),
            static_cast<std::uint8_t>((length >> 4) & 0xFF),
            static_cast<std::uint8_t>(length >> 12)};
}

void Append(Bytes& out, const Bytes& data) { out.insert(out.end(), data.begin(), data.end()); }

Bytes NameSeg(std::string_view name)
{
    Bytes result(4, '_');
    for (std::size_t index = 0; index < name.size() && index < 4; ++index) {
        result[index] = static_cast<std::uint8_t>(name[index]);
    }
    return result;
}

Bytes Integer(std::uint64_t value)
{
    if (value == 0) return {0x00};
    if (value == 1) return {0x01};
    if (value <= 0xFF) return {0x0A, static_cast<std::uint8_t>(value)};
    if (value <= 0xFFFF) return {0x0B, static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8)};
    if (value <= 0xFFFFFFFF) {
        return {0x0C, static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
                static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
    }
    Bytes result{0x0E};
    for (int index = 0; index < 8; ++index) result.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
    return result;
}

Bytes Package(const std::vector<Bytes>& elements)
{
    Bytes content{static_cast<std::uint8_t>(elements.size())};
    for (const auto& element : elements) Append(content, element);
    Bytes result{0x12};
    Append(result, PkgLength(content.size()));
    Append(result, content);
    return result;
}

Bytes Name(std::string_view name, const Bytes& value)
{
    Bytes result{0x08};
    Append(result, NameSeg(name));
    Append(result, value);
    return result;
}

Bytes Scope(const Bytes& nameString, const Bytes& body)
{
    Bytes content = nameString;
    Append(content, body);
    Bytes result{0x10};
    Append(result, PkgLength(content.size()));
    Append(result, content);
    return result;
}

Bytes Device(std::string_view name, const Bytes& body)
{
    Bytes content = NameSeg(name);
    Append(content, body);
    Bytes result{0x5B, 0x82};
    Append(result, PkgLength(content.size()));
    Append(result, content);
    return result;
}

Bytes EisaId(const char* id)
{
    const auto letter = [](char c) { return static_cast<std::uint32_t>(c - 'A' + 1) & 0x1F; };
    const auto hex = [](char c) -> std::uint32_t {
        return c >= '0' && c <= '9' ? static_cast<std::uint32_t>(c - '0') : static_cast<std::uint32_t>(c - 'A' + 10);
    };
    const std::uint32_t vendor = (letter(id[0]) << 10) | (letter(id[1]) << 5) | letter(id[2]);
    const std::uint32_t product = (hex(id[3]) << 12) | (hex(id[4]) << 8) | (hex(id[5]) << 4) | hex(id[6]);
    // The EISA ID is stored big-endian in a little-endian DWORD.
    const std::uint32_t value = ((vendor >> 8) & 0xFF) | ((vendor & 0xFF) << 8) |
                                (((product >> 8) & 0xFF) << 16) | ((product & 0xFF) << 24);
    return {0x0C, static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8),
            static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 24)};
}

Bytes Buffer(const Bytes& data)
{
    Bytes content = Integer(data.size());
    Append(content, data);
    Bytes result{0x11};
    Append(result, PkgLength(content.size()));
    Append(result, content);
    return result;
}

void Le16(Bytes& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void Le32(Bytes& out, std::uint32_t value)
{
    Le16(out, value & 0xFFFF);
    Le16(out, value >> 16);
}

// Word address space descriptor (bus number or I/O range).
void WordRange(Bytes& out, std::uint8_t resourceType, std::uint8_t typeFlags,
               std::uint32_t minimum, std::uint32_t maximum)
{
    out.insert(out.end(), {0x88, 0x0D, 0x00, resourceType, 0x0C, typeFlags});
    Le16(out, 0);                         // Granularity
    Le16(out, minimum);
    Le16(out, maximum);
    Le16(out, 0);                         // Translation
    Le16(out, maximum - minimum + 1);     // Length
}

// DWord memory range descriptor.
void DWordMemory(Bytes& out, bool cacheable, std::uint32_t minimum, std::uint32_t maximum)
{
    out.insert(out.end(), {0x87, 0x17, 0x00, 0x00, 0x0C, static_cast<std::uint8_t>(cacheable ? 0x03 : 0x01)});
    Le32(out, 0);
    Le32(out, minimum);
    Le32(out, maximum);
    Le32(out, 0);
    Le32(out, maximum - minimum + 1);
}

Bytes PciRootResources(const AcpiConfig& config)
{
    Bytes resources;
    WordRange(resources, 2, 0x00, 0x00, 0xFF);           // Bus numbers 0-255.
    resources.insert(resources.end(), {0x47, 0x01, 0xF8, 0x0C, 0xF8, 0x0C, 0x01, 0x08}); // CF8-CFF
    WordRange(resources, 1, 0x03, 0x0000, 0x0CF7);       // I/O below the config ports.
    WordRange(resources, 1, 0x03, 0x0D00, 0xFFFF);       // I/O above them.
    DWordMemory(resources, true, 0x000A0000, 0x000BFFFF); // Legacy VGA window.
    DWordMemory(resources, false, config.pciMmioStart, 0xFEBFFFFF);
    resources.insert(resources.end(), {0x79, 0x00});     // End tag.
    return resources;
}

Bytes BuildDsdt(const AcpiConfig& config)
{
    Bytes body;
    // \_S5: soft-off with SLP_TYPa = SLP_TYPb = 5.
    Append(body, Name("_S5_", Package({Integer(AcpiPm::kS5SleepType), Integer(AcpiPm::kS5SleepType),
                                       Integer(0), Integer(0)})));

    std::vector<Bytes> routes;
    for (const auto& route : config.pciRoutes) {
        routes.push_back(Package({Integer((static_cast<std::uint64_t>(route.slot) << 16) | 0xFFFF),
                                  Integer(0), Integer(0), Integer(route.gsi)}));
    }
    Bytes pci;
    Append(pci, Name("_HID", EisaId("PNP0A03")));
    Append(pci, Name("_UID", Integer(0)));
    Append(pci, Name("_BBN", Integer(0)));
    Append(pci, Name("_CRS", Buffer(PciRootResources(config))));
    Append(pci, Name("_PRT", Package(routes)));

    Bytes rootSb{0x5C};
    Append(rootSb, NameSeg("_SB_"));
    Append(body, Scope(rootSb, Device("PCI0", pci)));

    auto table = TableHeader("DSDT", 36 + body.size(), 2);
    std::memcpy(&table[36], body.data(), body.size());
    Checksum(table, 9, table.size());
    return table;
}

Bytes BuildFadt()
{
    auto table = TableHeader("FACP", 244, 3);
    Put32(table, 36, static_cast<std::uint32_t>(kFacsAddress));
    Put32(table, 40, static_cast<std::uint32_t>(kDsdtAddress));
    Put8(table, 45, 0); // Unspecified PM profile.
    Put16(table, 46, static_cast<std::uint16_t>(AcpiPm::kSciGsi));
    Put32(table, 48, 0); // No SMI command port: ACPI is permanently enabled.
    Put32(table, 56, AcpiPm::kPm1EventBlock);
    Put32(table, 64, AcpiPm::kPm1ControlBlock);
    Put32(table, 76, AcpiPm::kPmTimerBlock);
    Put8(table, 88, 4); // PM1_EVT_LEN
    Put8(table, 89, 2); // PM1_CNT_LEN
    Put8(table, 91, 4); // PM_TMR_LEN
    Put16(table, 96, 101);  // C2 unsupported.
    Put16(table, 98, 1001); // C3 unsupported.
    Put8(table, 108, 0x32); // CMOS century register.
    Put16(table, 109, 0x0003); // IAPC_BOOT_ARCH: legacy devices + 8042.
    // Flags: WBINVD, PROC_C1, SLP_BUTTON (none), TMR_VAL_EXT, RESET_REG_SUP.
    Put32(table, 112, (1u << 0) | (1u << 2) | (1u << 5) | (1u << 8) | (1u << 10));
    // RESET_REG: system I/O port 0xCF9, write 0x06.
    Put8(table, 116, 1);
    Put8(table, 117, 8);
    Put8(table, 118, 0);
    Put8(table, 119, 1);
    Put64(table, 120, 0xCF9);
    Put8(table, 128, 0x06);
    Put64(table, 140, kDsdtAddress); // X_DSDT
    Checksum(table, 9, table.size());
    return table;
}

Bytes BuildFacs()
{
    Bytes table(64, 0);
    std::memcpy(table.data(), "FACS", 4);
    Put32(table, 4, 64);
    Put8(table, 32, 1); // Version.
    return table;
}

Bytes BuildMadt(const AcpiConfig& config)
{
    const std::size_t length = 44 + 8 * config.cpuCount + 12 + 10 * 2 + 6;
    auto table = TableHeader("APIC", length, 3);
    Put32(table, 36, static_cast<std::uint32_t>(layout::kLocalApicBase));
    Put32(table, 40, 1); // PCAT_COMPAT: dual 8259 present (masked by the OS).
    std::size_t offset = 44;
    for (std::uint32_t cpu = 0; cpu < config.cpuCount; ++cpu) {
        Put8(table, offset, 0);
        Put8(table, offset + 1, 8);
        Put8(table, offset + 2, static_cast<std::uint8_t>(cpu)); // ACPI processor UID
        Put8(table, offset + 3, static_cast<std::uint8_t>(cpu)); // APIC ID (= WHP VP index)
        Put32(table, offset + 4, 1); // Enabled.
        offset += 8;
    }
    Put8(table, offset, 1);
    Put8(table, offset + 1, 12);
    // I/O APIC ID right after the CPUs; the register only holds 4 bits.
    Put8(table, offset + 2, static_cast<std::uint8_t>((std::min<std::uint32_t>)(config.cpuCount, 15)));
    Put32(table, offset + 4, static_cast<std::uint32_t>(layout::kIoApicBase));
    Put32(table, offset + 8, 0); // GSI base.
    offset += 12;
    // IRQ0 -> GSI2 (PIT), conforming polarity/trigger.
    Put8(table, offset, 2);
    Put8(table, offset + 1, 10);
    Put8(table, offset + 2, 0);
    Put8(table, offset + 3, 0);
    Put32(table, offset + 4, 2);
    Put16(table, offset + 8, 0);
    offset += 10;
    // IRQ9 -> GSI9 (ACPI SCI), active high, level triggered.
    Put8(table, offset, 2);
    Put8(table, offset + 1, 10);
    Put8(table, offset + 2, 0);
    Put8(table, offset + 3, 9);
    Put32(table, offset + 4, 9);
    Put16(table, offset + 8, 0x000D);
    offset += 10;
    // Local APIC NMI on LINT1 for all processors.
    Put8(table, offset, 4);
    Put8(table, offset + 1, 6);
    Put8(table, offset + 2, 0xFF);
    Put16(table, offset + 3, 0);
    Put8(table, offset + 5, 1);
    Checksum(table, 9, table.size());
    return table;
}

} // namespace

std::uint64_t BuildAcpiTables(const GuestMemory& memory, const AcpiConfig& config)
{
    if (config.cpuCount == 0 || config.cpuCount > 64) {
        throw std::invalid_argument("Nombre de processeurs ACPI invalide.");
    }
    const auto dsdt = BuildDsdt(config);
    const auto fadt = BuildFadt();
    const auto facs = BuildFacs();
    const auto madt = BuildMadt(config);

    auto rsdt = TableHeader("RSDT", 36 + 8, 1);
    Put32(rsdt, 36, static_cast<std::uint32_t>(kFadtAddress));
    Put32(rsdt, 40, static_cast<std::uint32_t>(kMadtAddress));
    Checksum(rsdt, 9, rsdt.size());

    Bytes rsdp(20, 0);
    std::memcpy(rsdp.data(), "RSD PTR ", 8);
    std::memcpy(&rsdp[9], "VIRTUA", 6);
    Put8(rsdp, 15, 0);
    Put32(rsdp, 16, static_cast<std::uint32_t>(kRsdtAddress));
    Checksum(rsdp, 8, 20);

    if (kMadtAddress + madt.size() > kDsdtAddress || kDsdtAddress + dsdt.size() > 0xF0000) {
        throw std::runtime_error("Les tables ACPI dépassent la zone réservée.");
    }
    const bool written =
        memory.Write(kRsdpAddress, rsdp.data(), rsdp.size()) &&
        memory.Write(kRsdtAddress, rsdt.data(), rsdt.size()) &&
        memory.Write(kFadtAddress, fadt.data(), fadt.size()) &&
        memory.Write(kFacsAddress, facs.data(), facs.size()) &&
        memory.Write(kMadtAddress, madt.data(), madt.size()) &&
        memory.Write(kDsdtAddress, dsdt.data(), dsdt.size());
    if (!written) throw std::runtime_error("Impossible d'écrire les tables ACPI en mémoire invitée.");
    return kRsdpAddress;
}

} // namespace vmm
