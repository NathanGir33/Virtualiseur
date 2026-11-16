#pragma once

#include "Virtio.h"

#include <array>
#include <cstdint>
#include <vector>

namespace vmm {

// Modern (virtio 1.0, non-transitional) virtio-pci transport. Required by
// drivers such as virtio-gpu and virtio-input, which refuse legacy devices.
//
// The configuration structures are exposed through PCI vendor capabilities
// and all live in one 256-byte I/O BAR (Linux accepts I/O BARs for them),
// so every access is a cheap port I/O exit:
//   0x00  common configuration (0x38 bytes)
//   0x38  ISR status (1 byte, read clears)
//   0x3C  queue notification (16-bit queue index, one address for all queues)
//   0x40  device-specific configuration (up to 0xC0 bytes)
// Interrupts are INTx (no MSI-X capability): the driver reads the ISR.
class VirtioModernDevice : public PciFunction, public IoDevice {
public:
    static constexpr std::uint64_t kFeatureVersion1 = 1ull << 32;

    VirtioModernDevice(std::uint16_t virtioType, std::uint8_t classCode, std::uint8_t subclass,
                       const GuestMemory& memory, InterruptSink& sink, std::uint32_t gsi,
                       IoBus& ioBus, std::uint16_t ioBase, std::size_t queueCount);

    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;

protected:
    // Device features, without VIRTIO_F_VERSION_1 (always offered).
    virtual std::uint64_t DeviceFeatures() const = 0;
    virtual std::uint8_t ReadDeviceConfig(std::uint32_t offset) = 0;
    virtual void WriteDeviceConfig(std::uint32_t, std::uint8_t) {}
    virtual void QueueNotify(std::uint16_t queue) = 0;
    virtual void DeviceReset() {}
    virtual void DriverOk() {}

    // Must be called with the device lock held.
    void SignalUsedBuffers(std::uint16_t queue);
    void SignalConfigChange();
    bool DriverReady() const noexcept { return (status_ & 0x04) != 0; }

    const GuestMemory& memory_;
    std::vector<VirtQueue> queues_;
    std::uint64_t driverFeatures_ = 0;

private:
    static constexpr std::uint32_t kCommonOffset = 0x00;
    static constexpr std::uint32_t kCommonLength = 0x38;
    static constexpr std::uint32_t kIsrOffset = 0x38;
    static constexpr std::uint32_t kNotifyOffset = 0x3C;
    static constexpr std::uint32_t kDeviceOffset = 0x40;
    static constexpr std::uint32_t kDeviceLength = 0xC0;

    struct QueueConfig {
        std::uint16_t size = VirtQueue::kSize;
        std::uint16_t vector = 0xFFFF;
        std::uint64_t descriptors = 0;
        std::uint64_t driver = 0;
        std::uint64_t device = 0;
        bool enabled = false;
    };

    void AddCapability(std::uint8_t position, std::uint8_t next, std::uint8_t type,
                       std::uint32_t offset, std::uint32_t length, std::uint8_t extraLength = 0);
    std::uint32_t ReadCommon(std::uint32_t offset, std::uint8_t size);
    void WriteCommon(std::uint32_t offset, std::uint8_t size, std::uint32_t value);
    void Reset();
    void UpdateLine();
    std::uint64_t OfferedFeatures() const { return DeviceFeatures() | kFeatureVersion1; }

    InterruptSink& sink_;
    std::uint32_t gsi_;
    std::vector<QueueConfig> queueConfig_;
    std::uint32_t deviceFeatureSelect_ = 0;
    std::uint32_t driverFeatureSelect_ = 0;
    std::uint16_t configVector_ = 0xFFFF;
    std::uint16_t selected_ = 0;
    std::uint8_t status_ = 0;
    std::uint8_t isr_ = 0;
    bool lineAsserted_ = false;
};

} // namespace vmm
