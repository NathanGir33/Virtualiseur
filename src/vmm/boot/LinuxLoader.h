#pragma once

#include "../core/GuestMemory.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vmm {

struct FramebufferInfo {
    std::uint64_t base = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0; // Bytes per line; 32 bits per pixel, XRGB8888.
};

struct LinuxImage {
    std::vector<std::uint8_t> kernel; // bzImage
    std::vector<std::uint8_t> initrd; // optional
    std::string commandLine;
};

// Appends an uncompressed "newc" cpio archive holding one file to an initrd.
// Linux unpacks concatenated archives, so the file appears in the initramfs
// (used for /preseed.cfg unattended Debian installs).
void AppendFileToInitrd(std::vector<std::uint8_t>& initrd, const std::string& guestPath,
                        const std::vector<std::uint8_t>& content);

struct LinuxBootInfo {
    std::uint64_t entryPoint = 0;
    std::uint64_t bootParams = 0;
    std::uint64_t kernelAddress = 0;
    std::uint64_t initrdAddress = 0;
    std::uint16_t protocol = 0;
};

// Loads a bzImage through the 32-bit Linux/x86 boot protocol: protected-mode
// kernel at its preferred address, initrd at the top of low RAM, boot_params
// with the E820 map, ACPI RSDP and either a linear framebuffer (simplefb /
// simpledrm) or an 80x25 VGA text console.
LinuxBootInfo LoadLinux(const GuestMemory& memory, const LinuxImage& image,
                        std::uint64_t rsdpAddress, const std::optional<FramebufferInfo>& framebuffer);

// Puts virtual processor `index` at the 32-bit entry point in flat protected
// mode with ESI pointing at boot_params.
void SetupLinuxBootCpu(virtualiseur::whp::Partition& partition, std::uint32_t index, const LinuxBootInfo& info);

// Parks an application processor in wait-for-SIPI state; the guest wakes it
// with INIT/SIPI through its local APIC.
void ParkApplicationProcessor(virtualiseur::whp::Partition& partition, std::uint32_t index);

} // namespace vmm
