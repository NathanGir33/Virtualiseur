#include "AcpiTables.h"

#include "../devices/AcpiPm.h"

#include <algorithm>
#include <array>
#include <string>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace vmm {
namespace {


using Bytes = std::vector<std::uint8_t>;

void Put8(Bytes& data, std::size_t offset, std::uint8_t value) { data[offset] = value; }
void Put16(Bytes& data, std::size_t offset, std::uint16_t value) { std::memcpy(&data[offset], &value, 2); }
void Put32(Bytes& data, std::size_t offset, std::uint32_t value) { std::memcpy(&data[offset], &value, 4); }
void Put64(Bytes& data, std::size_t offset, std::uint64_t value) { std::memcpy(&data[offset], &value, 8); }

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

    // _PRT: every interrupt pin (INTA-INTD) of every populated slot, wired
    // straight to the slot's I/O APIC input (the functions use INTA; the
    // other pins are declared so that no kernel finds a hole in the table).
    std::vector<Bytes> routes;
    for (const auto& route : config.pciRoutes) {
        for (std::uint64_t pin = 0; pin < 4; ++pin) {
            routes.push_back(Package({Integer((static_cast<std::uint64_t>(route.slot) << 16) | 0xFFFF),
                                      Integer(pin), Integer(0), Integer(route.gsi)}));
        }
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
    return table;
}

Bytes BuildFadt()
{
    auto table = TableHeader("FACP", 244, 3);
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
    // Interrupt source overrides: ISA IRQ -> GSI, flags (polarity bits 0-1,
    // trigger bits 2-3; 0 = conforms to the bus, ISA: edge, active high).
    struct Override {
        std::uint8_t irq;
        std::uint32_t gsi;
        std::uint16_t flags;
    };
    static constexpr std::array<Override, 4> kOverrides{{
        {0, 2, 0x0000},   // PIT on I/O APIC input 2.
        {9, 9, 0x000D},   // ACPI SCI: active high, level triggered.
        {14, 14, 0x0005}, // IDE primary channel: active high, edge (legacy mode).
        {15, 15, 0x0005}, // IDE secondary channel (the ATAPI CD-ROM).
    }};
    const std::size_t length = 44 + 8 * config.cpuCount + 12 + 10 * kOverrides.size() + 6;
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
    for (const auto& entry : kOverrides) {
        Put8(table, offset, 2);
        Put8(table, offset + 1, 10);
        Put8(table, offset + 2, 0); // ISA bus.
        Put8(table, offset + 3, entry.irq);
        Put32(table, offset + 4, entry.gsi);
        Put16(table, offset + 8, entry.flags);
        offset += 10;
    }
    // Local APIC NMI on LINT1 for all processors.
    Put8(table, offset, 4);
    Put8(table, offset + 1, 6);
    Put8(table, offset + 2, 0xFF);
    Put16(table, offset + 3, 0);
    Put8(table, offset + 5, 1);
    return table;
}

std::uint64_t ReadLittleEndian(const Bytes& data, std::size_t offset, std::size_t size)
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index) value |= static_cast<std::uint64_t>(data[offset + index]) << (index * 8);
    return value;
}

void WriteLittleEndian(Bytes& data, std::size_t offset, std::size_t size, std::uint64_t value)
{
    for (std::size_t index = 0; index < size; ++index) data[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
}

std::uint8_t Sum8(const Bytes& data, std::size_t start, std::size_t length)
{
    std::uint8_t sum = 0;
    for (std::size_t index = 0; index < length; ++index) sum = static_cast<std::uint8_t>(sum + data[start + index]);
    return sum;
}

// The loader script's rules, checked before anything is handed out: a
// pointer field holds its target's offset inside the tables blob (the
// loader adds the blob's base), is 4 or 8 bytes wide and lies inside its
// own blob; the target is a table (4-letter signature); checksum fields
// start at 0 and lie inside their blob. Checksums come after every pointer
// by construction (BuildTableLoader emits them last).
void Validate(const AcpiTableSet& set)
{
    const auto fail = [](const char* why) { throw std::logic_error(std::string("Script ACPI invalide : ") + why + "."); };
    for (const auto& pointer : set.pointers) {
        const auto& blob = pointer.inRsdp ? set.rsdp : set.tables;
        if (pointer.size != 4 && pointer.size != 8) fail("taille de pointeur");
        if (pointer.offset + pointer.size > blob.size()) fail("pointeur hors de son fichier");
        const auto target = ReadLittleEndian(blob, pointer.offset, pointer.size);
        if (target + 4 > set.tables.size()) fail("cible hors des tables");
        for (std::size_t index = 0; index < 4; ++index) {
            const auto c = set.tables[static_cast<std::size_t>(target) + index];
            if (c < 'A' || c > 'Z') fail("cible qui n'est pas une table");
        }
    }
    for (const auto& checksum : set.checksums) {
        const auto& blob = checksum.inRsdp ? set.rsdp : set.tables;
        if (checksum.start + checksum.length > blob.size() || checksum.resultOffset < checksum.start ||
            checksum.resultOffset >= checksum.start + checksum.length) {
            fail("somme de contrôle hors de son fichier");
        }
        if (blob[checksum.resultOffset] != 0) fail("champ de somme non nul");
    }
}

} // namespace

AcpiTableSet BuildAcpiTableSet(const AcpiConfig& config)
{
    if (config.cpuCount == 0 || config.cpuCount > 64) {
        throw std::invalid_argument("Nombre de processeurs ACPI invalide.");
    }
    AcpiTableSet set;
    // FACS first: the blob is 64-byte aligned and so must the FACS be.
    const auto add = [&set](const Bytes& table) {
        while (set.tables.size() % 8 != 0) set.tables.push_back(0);
        const auto offset = static_cast<std::uint32_t>(set.tables.size());
        Append(set.tables, table);
        return offset;
    };
    const auto facs = add(BuildFacs());
    const auto dsdt = add(BuildDsdt(config));
    const auto fadt = add(BuildFadt());
    const auto madt = add(BuildMadt(config));
    const auto rsdt = add(TableHeader("RSDT", 36 + 8, 1));

    const auto point = [&set](bool inRsdp, std::uint32_t offset, std::uint8_t size, std::uint32_t target) {
        // The field starts out holding the target's offset in the tables blob.
        WriteLittleEndian(inRsdp ? set.rsdp : set.tables, offset, size, target);
        set.pointers.push_back({inRsdp, offset, size});
    };
    point(false, fadt + 36, 4, facs);  // FIRMWARE_CTRL
    point(false, fadt + 40, 4, dsdt);  // DSDT
    point(false, fadt + 140, 8, dsdt); // X_DSDT
    point(false, rsdt + 36, 4, fadt);
    point(false, rsdt + 40, 4, madt);

    set.rsdp.assign(20, 0);
    std::memcpy(set.rsdp.data(), "RSD PTR ", 8);
    std::memcpy(&set.rsdp[9], "VIRTUA", 6);
    set.rsdp[15] = 0; // ACPI 1.0 RSDP: RSDT only.
    point(true, 16, 4, rsdt);

    // Checksums after every pointer: the loader patches pointers first.
    const auto sum = [&set](std::uint32_t table) {
        const auto length = static_cast<std::uint32_t>(ReadLittleEndian(set.tables, table + 4, 4));
        set.checksums.push_back({false, table, length, table + 9});
    };
    sum(dsdt);
    sum(fadt);
    sum(madt);
    sum(rsdt);
    set.checksums.push_back({true, 0, 20, 8});
    Validate(set);
    return set;
}

std::vector<std::uint8_t> BuildTableLoader(const AcpiTableSet& set)
{
    // QEMU's BIOSLinkerLoader commands, 128 bytes each, little-endian.
    constexpr std::uint32_t kAllocate = 1;
    constexpr std::uint32_t kAddPointer = 2;
    constexpr std::uint32_t kAddChecksum = 3;
    constexpr std::uint8_t kZoneHigh = 1;
    constexpr std::uint8_t kZoneFseg = 2;
    Bytes script;
    const auto command = [&script](std::uint32_t type) {
        const auto start = script.size();
        script.resize(start + 128, 0);
        WriteLittleEndian(script, start, 4, type);
        return start + 4;
    };
    const auto name = [&script](std::size_t at, std::string_view file) {
        std::memcpy(&script[at], file.data(), file.size()); // 56-byte field, NUL padded.
    };
    auto at = command(kAllocate);
    name(at, kAcpiRsdpFile);
    WriteLittleEndian(script, at + 56, 4, 16);
    script[at + 60] = kZoneFseg; // The RSDP lives in the F segment, as on a BIOS machine.
    at = command(kAllocate);
    name(at, kAcpiTablesFile);
    WriteLittleEndian(script, at + 56, 4, 64);
    script[at + 60] = kZoneHigh;
    for (const auto& pointer : set.pointers) {
        at = command(kAddPointer);
        name(at, pointer.inRsdp ? kAcpiRsdpFile : kAcpiTablesFile); // Patched file.
        name(at + 56, kAcpiTablesFile);                             // Pointed-to file.
        WriteLittleEndian(script, at + 112, 4, pointer.offset);
        script[at + 116] = pointer.size;
    }
    for (const auto& checksum : set.checksums) {
        at = command(kAddChecksum);
        name(at, checksum.inRsdp ? kAcpiRsdpFile : kAcpiTablesFile);
        WriteLittleEndian(script, at + 56, 4, checksum.resultOffset);
        WriteLittleEndian(script, at + 60, 4, checksum.start);
        WriteLittleEndian(script, at + 64, 4, checksum.length);
    }
    return script;
}

std::uint64_t BuildAcpiTables(const GuestMemory& memory, const AcpiConfig& config)
{
    // Linux direct boot: the VMM runs the very loader script OVMF gets, with
    // the RSDP at 0xE0000 (where Linux scans for it) and the tables after it.
    constexpr std::uint64_t kRsdpAddress = 0xE0000;
    constexpr std::uint64_t kTablesAddress = 0xE0040; // 64-byte aligned (FACS).
    auto set = BuildAcpiTableSet(config);
    if (kTablesAddress + set.tables.size() > 0xF0000) throw std::runtime_error("Les tables ACPI dépassent la zone réservée.");
    for (const auto& pointer : set.pointers) {
        auto& blob = pointer.inRsdp ? set.rsdp : set.tables;
        const auto value = ReadLittleEndian(blob, pointer.offset, pointer.size) + kTablesAddress;
        WriteLittleEndian(blob, pointer.offset, pointer.size, value);
    }
    for (const auto& checksum : set.checksums) {
        auto& blob = checksum.inRsdp ? set.rsdp : set.tables;
        blob[checksum.resultOffset] = static_cast<std::uint8_t>(0u - Sum8(blob, checksum.start, checksum.length));
    }
    if (!memory.Write(kRsdpAddress, set.rsdp.data(), set.rsdp.size()) ||
        !memory.Write(kTablesAddress, set.tables.data(), set.tables.size())) {
        throw std::runtime_error("Impossible d'écrire les tables ACPI en mémoire invitée.");
    }
    return kRsdpAddress;
}

} // namespace vmm
