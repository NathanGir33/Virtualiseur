#pragma once

#include "WindowsHypervisorBackend.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace virtualiseur::whp {

// Stages a 32-bit Linux/x86 boot-protocol entry from a bzImage. This prepares
// the CPU and boot_params; it does not provide PC devices or firmware.
class LinuxBootImage final {
public:
    LinuxBootImage(
        Partition& partition,
        const std::filesystem::path& kernelPath,
        const std::filesystem::path& initrdPath,
        std::string commandLine,
        std::uint64_t guestRamSizeBytes);

    std::uint64_t EntryPoint() const noexcept;
    std::uint64_t BootParamsAddress() const noexcept;
    std::uint64_t KernelLoadAddress() const noexcept;
    std::uint64_t KernelImageSize() const noexcept;
    std::uint64_t InitrdAddress() const noexcept;
    std::uint64_t InitrdSize() const noexcept;
    std::uint64_t GuestRamSize() const noexcept;

    void ConfigureBootVcpu(std::uint32_t virtualProcessorIndex);

private:
    Partition& partition_;
    std::uint64_t guestRamSizeBytes_;
    std::uint64_t kernelLoadAddress_ = 0;
    std::uint64_t kernelImageSize_ = 0;
    std::uint64_t entryPoint_ = 0;
    std::uint64_t initrdAddress_ = 0;
    std::uint64_t initrdSize_ = 0;
    std::uint8_t* lowMemory_ = nullptr;
    std::uint8_t* highMemory_ = nullptr;
};

} // namespace virtualiseur::whp
