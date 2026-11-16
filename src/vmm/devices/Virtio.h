#pragma once

#include "../core/Bus.h"
#include "../core/GuestMemory.h"
#include "Pci.h"

#include <cstdint>
#include <vector>

namespace vmm {

// Split virtqueue. Legacy devices (virtio 0.9.5) give one page frame holding
// the descriptor table, the available ring, then the used ring aligned to
// 4 KiB; modern devices (virtio 1.0) give the three addresses and the size.
class VirtQueue final {
public:
    static constexpr std::uint16_t kSize = 256; // Maximum (and legacy) queue size.

    struct Buffer {
        std::uint64_t address;
        std::uint32_t length;
    };
    struct Element {
        std::uint16_t head = 0;
        std::vector<Buffer> readable; // Driver -> device.
        std::vector<Buffer> writable; // Device -> driver.
    };

    void SetPfn(std::uint32_t pfn) noexcept
    {
        pfn_ = pfn;
        size_ = kSize;
        descriptors_ = static_cast<std::uint64_t>(pfn) << 12;
        available_ = descriptors_ + kSize * 16ull;
        used_ = (available_ + 4 + kSize * 2ull + 2 + 4095) & ~4095ull;
        ready_ = pfn != 0;
        lastAvailable_ = 0;
    }
    std::uint32_t Pfn() const noexcept { return pfn_; }
    // Modern layout. `size` must be a power of two no larger than kSize.
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
    void Reset() noexcept { pfn_ = 0; ready_ = false; lastAvailable_ = 0; }

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

    std::uint32_t pfn_ = 0;
    std::uint64_t descriptors_ = 0;
    std::uint64_t available_ = 0;
    std::uint64_t used_ = 0;
    std::uint16_t size_ = kSize;
    bool ready_ = false;
    std::uint16_t lastAvailable_ = 0;
};

// Legacy virtio-pci transport (I/O BAR0, INTx). Subclasses implement the
// device-specific features, configuration space and queue processing.
class VirtioPciDevice : public PciFunction, public IoDevice {
public:
    VirtioPciDevice(std::uint16_t pciDeviceId, std::uint16_t virtioType, std::uint8_t classCode,
                    std::uint8_t subclass, const GuestMemory& memory, InterruptSink& sink,
                    std::uint32_t gsi, IoBus& ioBus, std::uint16_t ioBase, std::size_t queueCount);

    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;

protected:
    virtual std::uint32_t DeviceFeatures() const = 0;
    virtual std::uint8_t ReadDeviceConfig(std::uint32_t offset) = 0;
    virtual void WriteDeviceConfig(std::uint32_t, std::uint8_t) {}
    virtual void QueueNotify(std::uint16_t queue) = 0;
    virtual void DeviceReset() {}

    // Must be called with the device lock held.
    void SignalUsedBuffers(std::uint16_t queue);
    bool DriverReady() const noexcept { return (status_ & 0x04) != 0; }

    const GuestMemory& memory_;
    std::vector<VirtQueue> queues_;
    std::uint32_t guestFeatures_ = 0;

private:
    std::uint8_t ReadRegister(std::uint32_t offset);
    void WriteRegister(std::uint32_t offset, std::uint8_t value);
    void UpdateLine();

    InterruptSink& sink_;
    std::uint32_t gsi_;
    std::uint16_t ioBase_;
    std::uint16_t selected_ = 0;
    std::uint8_t status_ = 0;
    std::uint8_t isr_ = 0;
    std::uint32_t pendingPfn_ = 0;
    std::uint16_t pendingNotify_ = 0;
    bool lineAsserted_ = false;
};

} // namespace vmm
