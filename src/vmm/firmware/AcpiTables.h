#pragma once

#include "../core/GuestMemory.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace vmm {

struct PciInterruptRoute {
    std::uint8_t slot;
    std::uint8_t gsi;
};

struct AcpiConfig {
    std::uint32_t cpuCount = 1;
    std::uint32_t pciMmioStart = 0xC0000000; // Start of the 32-bit PCI memory window.
    std::vector<PciInterruptRoute> pciRoutes;
};

// fw_cfg file names of the ACPI tables (as QEMU names them).
inline constexpr std::string_view kAcpiRsdpFile = "etc/acpi/rsdp";
inline constexpr std::string_view kAcpiTablesFile = "etc/acpi/tables";
inline constexpr std::string_view kTableLoaderFile = "etc/table-loader";

// The ACPI tables as two relocatable blobs, the way QEMU hands them to the
// firmware:
//   rsdp    the 20-byte RSDP (ACPI 1.0: RSDT only)
//   tables  FACS (first: the blob is 64-byte aligned), DSDT, FADT, MADT, RSDT
// Each pointer field starts out holding its target's offset inside
// `tables` and has its exact width (4 or 8 bytes); checksum fields are 0.
// The DSDT is generated AML: the S5 sleep package (soft off), the PCI root
// bridge, its resources and its _PRT (INTA-INTD of every populated slot).
// The MADT: local APICs, the I/O APIC, overrides for IRQ 0 (PIT), 9 (SCI),
// 14 and 15 (IDE), LINT1 NMI.
struct AcpiTableSet {
    struct Pointer {
        bool inRsdp;          // Field in `rsdp` (else in `tables`), pointing into `tables`.
        std::uint32_t offset; // Field offset in its blob.
        std::uint8_t size;    // 4 or 8.
    };
    struct Checksum {
        bool inRsdp;
        std::uint32_t start;
        std::uint32_t length;
        std::uint32_t resultOffset;
    };
    std::vector<std::uint8_t> rsdp;
    std::vector<std::uint8_t> tables;
    std::vector<Pointer> pointers;
    std::vector<Checksum> checksums;
};

// Builds and validates the blobs (throws std::logic_error on a broken rule).
AcpiTableSet BuildAcpiTableSet(const AcpiConfig& config);

// fw_cfg "etc/table-loader" for OVMF: ALLOCATE rsdp (16-byte aligned, F
// segment) and tables (64-byte aligned, high memory), then every
// ADD_POINTER, then every ADD_CHECKSUM (always after the pointers).
std::vector<std::uint8_t> BuildTableLoader(const AcpiTableSet& set);

// Linux direct boot: runs that same script in guest memory, RSDP at 0xE0000
// (where Linux scans for it), tables at 0xE0040. Returns the RSDP address.
std::uint64_t BuildAcpiTables(const GuestMemory& memory, const AcpiConfig& config);

} // namespace vmm
