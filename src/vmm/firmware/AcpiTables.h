#pragma once

#include "../core/GuestMemory.h"

#include <cstdint>
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

// Builds RSDP/RSDT/FADT/FACS/DSDT/MADT in the reserved 0xE0000 BIOS area and
// returns the RSDP guest-physical address. The DSDT is generated AML: it
// declares the S5 sleep package (soft off), the PCI root bridge and its _PRT.
std::uint64_t BuildAcpiTables(const GuestMemory& memory, const AcpiConfig& config);

} // namespace vmm
