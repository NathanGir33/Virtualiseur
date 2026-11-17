#pragma once

#include "../core/Bus.h"
#include "../core/GuestMemory.h"
#include "Pci.h"

#include <cstdint>
#include <vector>

namespace vmm {

// Split virtqueue (virtio 1.0): the driver gives the addresses of the
// descriptor table, the available (driver) ring and the used (device) ring.
class VirtQueue final {
public:
    static constexpr std::uint16_t kSize = 256; // Maximum queue size.

    struct Buffer {
        std::uint64_t address;
        std::uint32_t length;
    };
    struct Element {
        std::uint16_t head = 0;
        std::vector<Buffer> readable; // Driver -> device.
        std::vector<Buffer> writable; // Device -> driver.
    };

    // `size` must be a power of two no larger than kSize.
    void Enable(std::uint64_t descriptors, std::uint64_t available, std::uint64_t used, std::uint16_t size) noexcept
    {
        descriptors_ = descriptors;
        available_ = available;
        used_ = used;
        size_ = size;
        ready_ = size != 0 && size <= kSize && (size & (size - 1)) == 0;
        lastAvailable_ = 0;
    }
    bool Ready() const noexcept { return ready_; }
    void Reset() noexcept { ready_ = false; lastAvailable_ = 0; }

    bool HasAvailable(const GuestMemory& memory) const;
    // Pops the next descriptor chain. Returns false when the ring is empty or
    // the chain is malformed (the chain is then skipped).
    bool Pop(const GuestMemory& memory, Element& element);
    void Push(const GuestMemory& memory, std::uint16_t head, std::uint32_t written);
    bool InterruptSuppressed(const GuestMemory& memory) const;

    static constexpr std::uint32_t kFeatureIndirectDescriptors = 1u << 28;

private:
    static bool ReadChain(const GuestMemory& memory, std::uint64_t table, std::uint32_t tableSize,
                          std::uint16_t first, bool allowIndirect, Element& element);
    std::uint64_t Descriptors() const noexcept { return descriptors_; }
    std::uint64_t Available() const noexcept { return available_; }
    std::uint64_t Used() const noexcept { return used_; }

    std::uint64_t descriptors_ = 0;
    std::uint64_t available_ = 0;
    std::uint64_t used_ = 0;
    std::uint16_t size_ = kSize;
    bool ready_ = false;
    std::uint16_t lastAvailable_ = 0;
};

} // namespace vmm
