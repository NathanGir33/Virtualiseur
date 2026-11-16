#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace vmm {

// A device reachable through x86 port I/O. Accesses are 1, 2 or 4 bytes wide.
class IoDevice {
public:
    virtual ~IoDevice() = default;
    virtual std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) = 0;
    virtual void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) = 0;
};

// A device reachable through guest-physical memory that is not backed by RAM.
class MmioDevice {
public:
    virtual ~MmioDevice() = default;
    virtual std::uint64_t MmioRead(std::uint64_t address, std::uint8_t size) = 0;
    virtual void MmioWrite(std::uint64_t address, std::uint8_t size, std::uint64_t value) = 0;
};

// Receives interrupt line changes from device models. "gsi" is the global
// system interrupt number (I/O APIC input); ISA IRQs are translated by the
// caller with IsaIrqToGsi().
class InterruptSink {
public:
    virtual ~InterruptSink() = default;
    virtual void SetGsi(std::uint32_t gsi, bool asserted) = 0;
};

// ISA IRQ 0 (PIT) is wired to I/O APIC input 2, as on every PC chipset and as
// declared by the MADT interrupt source override. Other ISA IRQs map 1:1.
constexpr std::uint32_t IsaIrqToGsi(std::uint32_t irq) noexcept { return irq == 0 ? 2 : irq; }

// Port I/O dispatcher. A flat 64K table keeps lookups O(1) on the exit path.
class IoBus final {
public:
    IoBus() : table_(65536, nullptr) {}

    void Register(std::uint16_t first, std::uint32_t count, IoDevice* device)
    {
        for (std::uint32_t index = 0; index < count && first + index < 65536u; ++index) {
            table_[first + index] = device;
        }
    }

    void Unregister(std::uint16_t first, std::uint32_t count, IoDevice* device)
    {
        for (std::uint32_t index = 0; index < count && first + index < 65536u; ++index) {
            if (table_[first + index] == device) table_[first + index] = nullptr;
        }
    }

    IoDevice* Find(std::uint16_t port) const noexcept { return table_[port]; }

private:
    std::vector<IoDevice*> table_;
};

class MmioBus final {
public:
    void Register(std::uint64_t base, std::uint64_t size, MmioDevice* device)
    {
        ranges_.push_back({base, size, device});
    }

    MmioDevice* Find(std::uint64_t address) const noexcept
    {
        for (const auto& range : ranges_) {
            if (address >= range.base && address - range.base < range.size) return range.device;
        }
        return nullptr;
    }

private:
    struct Range {
        std::uint64_t base;
        std::uint64_t size;
        MmioDevice* device;
    };
    std::vector<Range> ranges_;
};

// Coarse lock protecting all device models. vCPU exit handlers and device
// threads (timers, network, storage completion, UI input) take it before
// touching device state, which keeps every model single-threaded internally.
using DeviceLock = std::recursive_mutex;
using DeviceGuard = std::lock_guard<std::recursive_mutex>;

} // namespace vmm
