#pragma once

#include "DirtyRows.h"
#include "WindowsHypervisorBackend.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vmm {

// Guest-physical address map of the virtual PC:
//   0x00000000 - 0x0009FFFF  conventional RAM (top 4 KiB reserved as EBDA)
//   0x000A0000 - 0x000FFFFF  legacy VGA/BIOS window (RAM-backed, reserved; VGA
//                            text buffer and ACPI tables live here)
//   0x00100000 - lowRamEnd   RAM, capped at 3 GiB
//   0xC0000000 - 0xFFFFFFFF  PCI/MMIO hole: framebuffer, I/O APIC, local APIC
//   0x100000000 - ...        RAM displaced by the hole
namespace layout {
constexpr std::uint64_t kLegacyWindowStart = 0xA0000;
constexpr std::uint64_t kLegacyWindowSize = 0x60000;
constexpr std::uint64_t kHighMemoryStart = 0x100000;
constexpr std::uint64_t kPciHoleStart = 0xC0000000;
constexpr std::uint64_t kFramebufferBase = 0xE0000000;
constexpr std::uint64_t kIoApicBase = 0xFEC00000;
constexpr std::uint64_t kLocalApicBase = 0xFEE00000;
constexpr std::uint64_t kFourGiB = 0x100000000ull;
constexpr std::uint64_t kVgaTextBase = 0xB8000;
} // namespace layout

struct E820Entry {
    std::uint64_t address;
    std::uint64_t size;
    std::uint32_t type; // 1 = RAM, 2 = reserved
};

class GuestMemory final {
public:
    GuestMemory(virtualiseur::whp::Partition& partition,
                std::uint64_t ramBytes,
                std::uint64_t framebufferBytes);

    // Returns a host pointer for [gpa, gpa + size) or nullptr if that range is
    // not entirely backed by one host allocation.
    std::uint8_t* Pointer(std::uint64_t gpa, std::size_t size) const noexcept
    {
        return static_cast<std::uint8_t*>(partition_.GetHostAddressForGuestPhysicalAddress(gpa, size));
    }

    bool Read(std::uint64_t gpa, void* destination, std::size_t size) const noexcept;
    bool Write(std::uint64_t gpa, const void* source, std::size_t size) const noexcept;

    template <typename T>
    bool ReadValue(std::uint64_t gpa, T& value) const noexcept
    {
        const auto* pointer = Pointer(gpa, sizeof(T));
        if (pointer == nullptr) return false;
        std::atomic_thread_fence(std::memory_order_acquire);
        std::memcpy(&value, pointer, sizeof(T));
        return true;
    }

    template <typename T>
    bool WriteValue(std::uint64_t gpa, const T& value) const noexcept
    {
        auto* pointer = Pointer(gpa, sizeof(T));
        if (pointer == nullptr) return false;
        std::memcpy(pointer, &value, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        return true;
    }

    std::uint64_t RamBytes() const noexcept { return ramBytes_; }
    std::uint64_t LowRamEnd() const noexcept { return lowRamEnd_; }
    std::uint64_t HighRamBytes() const noexcept { return highRamBytes_; }
    const std::vector<E820Entry>& E820() const noexcept { return e820_; }

    std::uint8_t* Framebuffer() const noexcept { return framebuffer_; }

    // Adds to `rows` the framebuffer rows written since the previous query,
    // one band per run of dirty pages. Without dirty tracking the whole
    // framebuffer is reported.
    void FramebufferDirtyRows(std::uint32_t stride, std::uint32_t height, DirtyRows& rows) const;
    std::uint64_t FramebufferBytes() const noexcept { return framebufferBytes_; }

private:
    virtualiseur::whp::Partition& partition_;
    std::uint64_t ramBytes_;
    std::uint64_t lowRamEnd_ = 0;
    std::uint64_t highRamBytes_ = 0;
    std::uint8_t* framebuffer_ = nullptr;
    std::uint64_t framebufferBytes_ = 0;
    std::vector<E820Entry> e820_;
};

} // namespace vmm
