#pragma once

#include "../core/Bus.h"
#include "../core/GuestMemory.h"
#include "AcpiTables.h"
#include "FwCfg.h"
#include "WindowsHypervisorBackend.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vmm {

// Architectural reset state of the bootstrap processor, cold boot and VM
// reset alike (a reset recreates the VM): real mode, CS selector 0xF000 with
// base 0xFFFF0000 and RIP 0xFFF0, so the first instruction is fetched at
// 0xFFFFFFF0 (top of the firmware flash); data segments base 0 / limit
// 0xFFFF; CR0 = 0x60000010 (CD, NW, ET); paging, long mode and interrupts off.
// The application processors wait for INIT/SIPI (ParkApplicationProcessor).
void SetupFirmwareResetCpu(virtualiseur::whp::Partition& partition, std::uint32_t index);

// Fills fw_cfg for OVMF: features, UUID, RAM size, CPU count, "etc/e820"
// (RAM below the PCI hole and above 4 GiB), the ACPI tables
// ("etc/acpi/rsdp", "etc/acpi/tables", "etc/table-loader"), "bootorder",
// then the file directory.
void ConfigureFwCfg(FwCfg& fwCfg, const GuestMemory& memory, std::uint32_t cpus, const std::array<std::uint8_t, 16>& uuid,
                    const AcpiTableSet& acpi, const std::vector<std::string>& bootOrder);

// Open Firmware device paths for fw_cfg "bootorder", in the form OVMF's
// QemuBootOrderLib translates: PCI slot and function in lowercase hex
// without "0x", the function only when not 0.
//   IDE device (PIIX IDE at 01.1): /pci@i0cf8/ide@1,1/drive@<channel>/disk@<unit>
//   virtio-blk disk:               /pci@i0cf8/scsi@<slot>[,<function>]/disk@0,0
std::string OfwIdePath(std::uint32_t channel, std::uint32_t unit);
std::string OfwVirtioBlkPath(std::uint8_t slot, std::uint8_t function = 0);

// QEMU's debug console (port 0x402). OVMF ("verbose-dynamic" builds) prints
// its debug log there only when a read of the port returns 0xE9; each line
// goes to the VMM log. Registered only when asked for (--uefi-debug): the
// verbose log costs one VM exit per character.
class FirmwareDebugPort final : public IoDevice {
public:
    static constexpr std::uint16_t kPort = 0x402;
    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;

private:
    std::string line_;
};

} // namespace vmm
