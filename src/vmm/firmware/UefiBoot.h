#pragma once

#include "../core/Bus.h"
#include "../core/GuestMemory.h"
#include "FwCfg.h"
#include "WindowsHypervisorBackend.h"

#include <array>
#include <cstdint>
#include <string>

namespace vmm {

// Architectural reset state of the bootstrap processor, cold boot and VM
// reset alike (a reset recreates the VM): real mode, CS selector 0xF000 with
// base 0xFFFF0000 and RIP 0xFFF0, so the first instruction is fetched at
// 0xFFFFFFF0 (top of the firmware flash); data segments base 0 / limit
// 0xFFFF; CR0 = 0x60000010 (CD, NW, ET); paging, long mode and interrupts off.
// The application processors wait for INIT/SIPI (ParkApplicationProcessor).
void SetupFirmwareResetCpu(virtualiseur::whp::Partition& partition, std::uint32_t index);

// Fills fw_cfg for OVMF: features, UUID, RAM size, CPU count, "etc/e820"
// (RAM below the PCI hole and above 4 GiB), then the file directory.
void ConfigureFwCfg(FwCfg& fwCfg, const GuestMemory& memory, std::uint32_t cpus, const std::array<std::uint8_t, 16>& uuid);

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
