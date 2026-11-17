#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace vmm {

// Device locks. Legacy devices (PIT, UART, PS/2, IDE, PCI configuration...)
// share the VM-wide lock; hot devices (virtio functions, I/O APIC) have
// their own, so vCPUs and device threads working on different devices run
// in parallel. Lock order: VM-wide lock, then a device lock, then the I/O
// APIC's internal lock; a device thread never takes the VM-wide lock.
using DeviceLock = std::recursive_mutex;
using DeviceGuard = std::lock_guard<std::recursive_mutex>;

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

// Receives interrupts from device models. "gsi" is the global system
// interrupt number (I/O APIC input); ISA IRQs are translated by the caller
// with IsaIrqToGsi(). SignalMsi sends a message-signalled interrupt (MSI or
// MSI-X table entry): edge-triggered, no line state, no EOI round trip.
// Both are thread-safe.
class InterruptSink {
public:
    virtual ~InterruptSink() = default;
    virtual void SetGsi(std::uint32_t gsi, bool asserted) = 0;
    virtual void SignalMsi(std::uint64_t address, std::uint32_t data) = 0;
};

// ISA IRQ 0 (PIT) is wired to I/O APIC input 2, as on every PC chipset and as
// declared by the MADT interrupt source override. Other ISA IRQs map 1:1.
constexpr std::uint32_t IsaIrqToGsi(std::uint32_t irq) noexcept { return irq == 0 ? 2 : irq; }

// Where an access goes, and which lock serialises it (nullptr: the VM-wide
// lock).
struct IoRoute {
    IoDevice* device;
    DeviceLock* lock;
};

// Port I/O dispatcher. A flat 64K table keeps lookups O(1) on the exit path;
// vCPUs read it without a lock while PCI BAR moves update it.
class IoBus final {
public:
    IoBus() : table_(65536) {}

    void Register(std::uint16_t first, std::uint32_t count, IoDevice* device, DeviceLock* lock = nullptr)
    {
        const IoRoute* route = Intern(device, lock);
        for (std::uint32_t index = 0; index < count && first + index < 65536u; ++index) {
            table_[first + index].store(route, std::memory_order_release);
        }
    }

    void Unregister(std::uint16_t first, std::uint32_t count, IoDevice* device)
    {
        for (std::uint32_t index = 0; index < count && first + index < 65536u; ++index) {
            const auto* route = table_[first + index].load(std::memory_order_relaxed);
            if (route != nullptr && route->device == device) table_[first + index].store(nullptr, std::memory_order_release);
        }
    }

    const IoRoute* Find(std::uint16_t port) const noexcept { return table_[port].load(std::memory_order_acquire); }

private:
    // Routes live as long as the bus: a vCPU may still hold one after a remap.
    const IoRoute* Intern(IoDevice* device, DeviceLock* lock)
    {
        const std::lock_guard guard(routesMutex_);
        for (const auto& route : routes_) {
            if (route.device == device && route.lock == lock) return &route;
        }
        return &routes_.emplace_back(IoRoute{device, lock});
    }

    std::vector<std::atomic<const IoRoute*>> table_;
    std::mutex routesMutex_;
    std::deque<IoRoute> routes_;
};

struct MmioRoute {
    std::uint64_t base;
    std::uint64_t size;
    MmioDevice* device;
    DeviceLock* lock; // nullptr: the VM-wide lock.
};

class MmioBus final {
public:
    void Register(std::uint64_t base, std::uint64_t size, MmioDevice* device, DeviceLock* lock = nullptr)
    {
        const std::unique_lock guard(mutex_);
        ranges_.push_back({base, size, device, lock});
    }

    void Unregister(std::uint64_t base, MmioDevice* device)
    {
        const std::unique_lock guard(mutex_);
        for (auto it = ranges_.begin(); it != ranges_.end(); ++it) {
            if (it->base == base && it->device == device) {
                ranges_.erase(it);
                return;
            }
        }
    }

    bool Find(std::uint64_t address, MmioRoute& route) const
    {
        const std::shared_lock guard(mutex_);
        for (const auto& range : ranges_) {
            if (address >= range.base && address - range.base < range.size) {
                route = range;
                return true;
            }
        }
        return false;
    }

private:
    mutable std::shared_mutex mutex_;
    std::vector<MmioRoute> ranges_;
};

} // namespace vmm
