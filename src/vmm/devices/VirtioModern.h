#pragma once

#include "Virtio.h"

#include <array>
#include <cstdint>
#include <vector>

namespace vmm {

// Where the firmware places a virtio PCI function and how it interrupts.
struct VirtioPlacement {
    IoBus& ioBus;
    MmioBus& mmioBus;
    InterruptSink& sink;
    std::uint16_t ioBase;   // BAR0: 256 bytes of port I/O.
    std::uint32_t mmioBase; // BAR1: VirtioModernDevice::kMmioBarSize bytes of MMIO.
    std::uint32_t gsi;      // INTx line, used only while MSI-X is off.
};

// Modern (virtio 1.0, non-transitional) virtio-pci transport.
//
// The configuration structures are exposed through PCI vendor capabilities
// and all live in one 256-byte I/O BAR (Linux accepts I/O BARs for them),
// so register accesses are cheap port I/O exits:
//   0x00  common configuration (0x38 bytes)
//   0x38  ISR status (1 byte, read clears)
//   0x3C  queue notification (16-bit queue index, one address for all queues)
//   0x40  device-specific configuration (up to 0xC0 bytes)
//
// Interrupts are MSI-X when the driver enables it (one vector per queue plus
// one for configuration changes): each message goes straight to a local
// APIC, edge-triggered, so there is no shared line, no ISR read and no EOI
// exit. The MSI-X table and pending bits live in memory BAR1 (MSI-X tables
// cannot sit in an I/O BAR):
//   0x0000  MSI-X table (16 bytes per vector)
//   0x0800  MSI-X pending bit array
// Without MSI-X (pci=nomsi, old drivers) the device falls back to INTx.
class VirtioModernDevice : public PciFunction, public IoDevice, public MmioDevice {
public:
    static constexpr std::uint64_t kFeatureVersion1 = 1ull << 32;
    static constexpr std::uint32_t kMmioBarSize = 0x4000;

    VirtioModernDevice(std::uint16_t virtioType, std::uint8_t classCode, std::uint8_t subclass,
                       const GuestMemory& memory, const VirtioPlacement& placement, std::size_t queueCount);

    // BAR accesses arrive with Lock() held (the buses take it).
    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;
    std::uint64_t MmioRead(std::uint64_t address, std::uint8_t size) override;
    void MmioWrite(std::uint64_t address, std::uint8_t size, std::uint64_t value) override;
    // PCI configuration space: called with the VM-wide lock, takes Lock().
    std::uint32_t ConfigRead(std::uint32_t offset, std::uint8_t size) override;
    void ConfigWrite(std::uint32_t offset, std::uint8_t size, std::uint32_t value) override;

    // This function's own lock: device state, rings and interrupts. Host
    // threads (I/O workers, UI input, audio) take it before touching the device.
    DeviceLock& Lock() const noexcept { return lock_; }

protected:
    // Device features, without VIRTIO_F_VERSION_1 (always offered).
    virtual std::uint64_t DeviceFeatures() const = 0;
    virtual std::uint8_t ReadDeviceConfig(std::uint32_t offset) = 0;
    virtual void WriteDeviceConfig(std::uint32_t, std::uint8_t) {}
    virtual void QueueNotify(std::uint16_t queue) = 0;
    virtual void DeviceReset() {}
    virtual void DriverOk() {}

    // Must be called with Lock() held.
    void SignalUsedBuffers(std::uint16_t queue);
    void SignalConfigChange();
    bool DriverReady() const noexcept { return (status_ & 0x04) != 0; }

    mutable DeviceLock lock_;
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

    static constexpr std::uint8_t kMsixCapability = 0x84;
    static constexpr std::uint32_t kMsixTableOffset = 0x0000;
    static constexpr std::uint32_t kMsixPbaOffset = 0x0800;
    static constexpr std::uint16_t kNoVector = 0xFFFF;

    struct QueueConfig {
        std::uint16_t size = VirtQueue::kSize;
        std::uint16_t vector = kNoVector;
        std::uint64_t descriptors = 0;
        std::uint64_t driver = 0;
        std::uint64_t device = 0;
        bool enabled = false;
    };
    struct MsixEntry {
        std::uint64_t address = 0;
        std::uint32_t data = 0;
        bool masked = true; // Vector control bit 0, set at reset.
        bool pending = false;
    };

    void AddCapability(std::uint8_t position, std::uint8_t next, std::uint8_t type,
                       std::uint32_t offset, std::uint32_t length, std::uint8_t extraLength = 0);
    std::uint32_t ReadCommon(std::uint32_t offset, std::uint8_t size);
    void WriteCommon(std::uint32_t offset, std::uint8_t size, std::uint32_t value);
    void Reset();
    void UpdateLine();
    std::uint64_t OfferedFeatures() const { return DeviceFeatures() | kFeatureVersion1; }

    bool MsixEnabled() const noexcept { return (config_[kMsixCapability + 3] & 0x80) != 0; }
    bool MsixFunctionMasked() const noexcept { return (config_[kMsixCapability + 3] & 0x40) != 0; }
    std::uint16_t AcceptVector(std::uint32_t vector) const noexcept
    {
        return vector < msix_.size() ? static_cast<std::uint16_t>(vector) : kNoVector;
    }
    // Sends MSI-X `vector`, or records it as pending while masked.
    void SendVector(std::uint16_t vector);
    void SendPendingVectors();
    std::uint32_t ReadMsixDword(std::uint32_t offset) const;
    void WriteMsixDword(std::uint32_t offset, std::uint32_t value);

    InterruptSink& sink_;
    std::uint32_t gsi_;
    std::vector<QueueConfig> queueConfig_;
    std::vector<MsixEntry> msix_;
    std::uint32_t deviceFeatureSelect_ = 0;
    std::uint32_t driverFeatureSelect_ = 0;
    std::uint16_t configVector_ = kNoVector;
    std::uint16_t selected_ = 0;
    std::uint8_t status_ = 0;
    std::uint8_t isr_ = 0;
    bool lineAsserted_ = false;
};

} // namespace vmm
