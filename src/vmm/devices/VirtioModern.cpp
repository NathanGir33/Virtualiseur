#include "VirtioModern.h"

namespace vmm {
namespace {
constexpr std::uint8_t kStatusDriverOk = 0x04;
constexpr std::uint8_t kStatusFeaturesOk = 0x08;

constexpr std::uint8_t kCapCommon = 1;
constexpr std::uint8_t kCapNotify = 2;
constexpr std::uint8_t kCapIsr = 3;
constexpr std::uint8_t kCapDevice = 4;

constexpr std::uint16_t kCommandIntxDisable = 0x0400;
} // namespace

VirtioModernDevice::VirtioModernDevice(std::uint16_t virtioType, std::uint8_t classCode, std::uint8_t subclass,
                                       const GuestMemory& memory, const VirtioPlacement& placement,
                                       std::size_t queueCount)
    : PciFunction(0x1AF4, static_cast<std::uint16_t>(0x1040 + virtioType), classCode, subclass, 0x00, 0x01),
      memory_(memory), queues_(queueCount), sink_(placement.sink), gsi_(placement.gsi),
      queueConfig_(queueCount), msix_(queueCount + 1)
{
    AttachIoBus(&placement.ioBus);
    AttachMmioBus(&placement.mmioBus);
    SetSubsystem(0x1AF4, 0x1100);
    SetInterrupt(static_cast<std::uint8_t>(placement.gsi), 1);
    config_[0x04] |= 0x04; // Bus master, as left by the firmware.
    SetIoBar(0, placement.ioBase, 256, this);
    SetMmioBar(1, placement.mmioBase, kMmioBarSize, this);

    // Capability list: common, notify, ISR, device configuration, MSI-X.
    config_[0x06] |= 0x10;
    config_[0x34] = 0x40;
    AddCapability(0x40, 0x50, kCapCommon, kCommonOffset, kCommonLength);
    AddCapability(0x50, 0x64, kCapNotify, kNotifyOffset, 2, 4); // notify_off_multiplier = 0.
    AddCapability(0x64, 0x74, kCapIsr, kIsrOffset, 1);
    AddCapability(0x74, kMsixCapability, kCapDevice, kDeviceOffset, kDeviceLength);

    auto* msix = &config_[kMsixCapability];
    msix[0] = 0x11;
    msix[1] = 0x00;
    const auto tableSize = static_cast<std::uint16_t>(msix_.size() - 1);
    msix[2] = static_cast<std::uint8_t>(tableSize);
    msix[3] = static_cast<std::uint8_t>(tableSize >> 8);
    writeMask_[kMsixCapability + 3] = 0xC0; // MSI-X Enable, Function Mask.
    const std::uint32_t table = kMsixTableOffset | 1; // BIR 1.
    const std::uint32_t pba = kMsixPbaOffset | 1;
    for (int byte = 0; byte < 4; ++byte) {
        msix[4 + byte] = static_cast<std::uint8_t>(table >> (byte * 8));
        msix[8 + byte] = static_cast<std::uint8_t>(pba >> (byte * 8));
    }
}

void VirtioModernDevice::AddCapability(std::uint8_t position, std::uint8_t next, std::uint8_t type,
                                       std::uint32_t offset, std::uint32_t length, std::uint8_t extraLength)
{
    auto* cap = &config_[position];
    cap[0] = 0x09; // Vendor-specific.
    cap[1] = next;
    cap[2] = static_cast<std::uint8_t>(16 + extraLength);
    cap[3] = type;
    cap[4] = 0; // BAR0.
    for (int byte = 0; byte < 4; ++byte) {
        cap[8 + byte] = static_cast<std::uint8_t>(offset >> (byte * 8));
        cap[12 + byte] = static_cast<std::uint8_t>(length >> (byte * 8));
    }
}

// --- BAR0: virtio registers ----------------------------------------------------------------------

std::uint32_t VirtioModernDevice::IoRead(std::uint16_t port, std::uint8_t size)
{
    const auto barBase = static_cast<std::uint16_t>((config_[0x10] | (config_[0x11] << 8)) & 0xFFFC);
    const auto offset = static_cast<std::uint32_t>(port - barBase);
    if (offset < kCommonOffset + kCommonLength) return ReadCommon(offset, size);
    if (offset == kIsrOffset) {
        const auto value = isr_;
        isr_ = 0;
        UpdateLine();
        return value;
    }
    if (offset >= kDeviceOffset) {
        std::uint32_t value = 0;
        for (std::uint8_t index = 0; index < size; ++index) {
            value |= static_cast<std::uint32_t>(ReadDeviceConfig(offset - kDeviceOffset + index)) << (index * 8);
        }
        return value;
    }
    return 0;
}

void VirtioModernDevice::IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value)
{
    const auto barBase = static_cast<std::uint16_t>((config_[0x10] | (config_[0x11] << 8)) & 0xFFFC);
    const auto offset = static_cast<std::uint32_t>(port - barBase);
    if (offset < kCommonOffset + kCommonLength) {
        WriteCommon(offset, size, value);
    } else if (offset == kNotifyOffset) {
        const auto queue = static_cast<std::uint16_t>(value);
        if (queue < queues_.size() && queues_[queue].Ready()) QueueNotify(queue);
    } else if (offset >= kDeviceOffset) {
        for (std::uint8_t index = 0; index < size; ++index) {
            WriteDeviceConfig(offset - kDeviceOffset + index, static_cast<std::uint8_t>(value >> (index * 8)));
        }
    }
}

std::uint32_t VirtioModernDevice::ReadCommon(std::uint32_t offset, std::uint8_t size)
{
    // Build the structure, then extract the requested bytes: drivers use
    // natural widths, but byte accesses stay correct this way.
    std::array<std::uint8_t, kCommonLength> image{};
    const auto put = [&image](std::uint32_t at, std::uint64_t value, std::uint32_t bytes) {
        for (std::uint32_t index = 0; index < bytes; ++index) image[at + index] = static_cast<std::uint8_t>(value >> (index * 8));
    };
    const auto features = OfferedFeatures();
    put(0x00, deviceFeatureSelect_, 4);
    put(0x04, deviceFeatureSelect_ < 2 ? features >> (deviceFeatureSelect_ * 32) : 0, 4);
    put(0x08, driverFeatureSelect_, 4);
    put(0x0C, driverFeatureSelect_ < 2 ? driverFeatures_ >> (driverFeatureSelect_ * 32) : 0, 4);
    put(0x10, configVector_, 2);
    put(0x12, queues_.size(), 2);
    put(0x14, status_, 1);
    put(0x15, 0, 1); // config_generation: the device configuration never changes mid-read.
    put(0x16, selected_, 2);
    if (selected_ < queueConfig_.size()) {
        const auto& queue = queueConfig_[selected_];
        put(0x18, queue.size, 2);
        put(0x1A, queue.vector, 2);
        put(0x1C, queue.enabled ? 1 : 0, 2);
        put(0x1E, 0, 2); // queue_notify_off: every queue shares the notify register.
        put(0x20, queue.descriptors, 8);
        put(0x28, queue.driver, 8);
        put(0x30, queue.device, 8);
    }
    std::uint32_t value = 0;
    for (std::uint8_t index = 0; index < size && offset + index < kCommonLength; ++index) {
        value |= static_cast<std::uint32_t>(image[offset + index]) << (index * 8);
    }
    return value;
}

void VirtioModernDevice::WriteCommon(std::uint32_t offset, std::uint8_t size, std::uint32_t value)
{
    const auto setHalf = [value, offset](std::uint64_t& target, std::uint32_t base) {
        const auto shift = (offset - base) * 8;
        target = (target & ~(0xFFFFFFFFull << shift)) | (static_cast<std::uint64_t>(value) << shift);
    };
    QueueConfig* queue = selected_ < queueConfig_.size() ? &queueConfig_[selected_] : nullptr;
    switch (offset) {
    case 0x00: deviceFeatureSelect_ = value; break;
    case 0x08: driverFeatureSelect_ = value; break;
    case 0x0C:
        if (driverFeatureSelect_ < 2 && size == 4) {
            const auto shift = driverFeatureSelect_ * 32;
            driverFeatures_ = (driverFeatures_ & ~(0xFFFFFFFFull << shift)) | (static_cast<std::uint64_t>(value) << shift);
            driverFeatures_ &= OfferedFeatures();
        }
        break;
    // The driver reads vectors back: NO_VECTOR tells it the vector was refused.
    case 0x10: configVector_ = AcceptVector(value & 0xFFFF); break;
    case 0x14: {
        auto status = static_cast<std::uint8_t>(value);
        if (status == 0) {
            Reset();
            break;
        }
        // FEATURES_OK only sticks if the driver accepted VIRTIO_F_VERSION_1.
        if ((status & kStatusFeaturesOk) != 0 && (driverFeatures_ & kFeatureVersion1) == 0) {
            status = static_cast<std::uint8_t>(status & ~kStatusFeaturesOk);
        }
        const bool becameReady = (status & kStatusDriverOk) != 0 && (status_ & kStatusDriverOk) == 0;
        status_ = status;
        if (becameReady) DriverOk();
        break;
    }
    case 0x16: selected_ = static_cast<std::uint16_t>(value); break;
    case 0x18:
        if (queue != nullptr && !queue->enabled) {
            const auto requested = static_cast<std::uint16_t>(value);
            if (requested != 0 && requested <= VirtQueue::kSize && (requested & (requested - 1)) == 0) {
                queue->size = requested;
            }
        }
        break;
    case 0x1A: if (queue != nullptr) queue->vector = AcceptVector(value & 0xFFFF); break;
    case 0x1C:
        if (queue != nullptr && (value & 1) != 0 && !queue->enabled) {
            queue->enabled = true;
            queues_[selected_].Enable(queue->descriptors, queue->driver, queue->device, queue->size);
        }
        break;
    case 0x20: case 0x24: if (queue != nullptr) setHalf(queue->descriptors, 0x20); break;
    case 0x28: case 0x2C: if (queue != nullptr) setHalf(queue->driver, 0x28); break;
    case 0x30: case 0x34: if (queue != nullptr) setHalf(queue->device, 0x30); break;
    default: break;
    }
}

void VirtioModernDevice::Reset()
{
    DeviceReset();
    for (auto& queue : queues_) queue.Reset();
    for (auto& queue : queueConfig_) queue = QueueConfig{};
    driverFeatures_ = 0;
    deviceFeatureSelect_ = 0;
    driverFeatureSelect_ = 0;
    configVector_ = kNoVector;
    selected_ = 0;
    status_ = 0;
    isr_ = 0;
    UpdateLine();
}

// --- Interrupts ----------------------------------------------------------------------------------

void VirtioModernDevice::SignalUsedBuffers(std::uint16_t queue)
{
    if (queue >= queues_.size() || queues_[queue].InterruptSuppressed(memory_)) return;
    if (MsixEnabled()) {
        SendVector(queueConfig_[queue].vector);
        return;
    }
    isr_ |= 0x01;
    UpdateLine();
}

void VirtioModernDevice::SignalConfigChange()
{
    if (!DriverReady()) return;
    if (MsixEnabled()) {
        SendVector(configVector_);
        return;
    }
    isr_ |= 0x02;
    UpdateLine();
}

void VirtioModernDevice::UpdateLine()
{
    const bool asserted = isr_ != 0 && !MsixEnabled() && (Command() & kCommandIntxDisable) == 0;
    if (asserted == lineAsserted_) return;
    lineAsserted_ = asserted;
    sink_.SetGsi(gsi_, asserted);
}

void VirtioModernDevice::SendVector(std::uint16_t vector)
{
    if (vector >= msix_.size()) return; // NO_VECTOR: the driver wants no interrupt.
    auto& entry = msix_[vector];
    if (entry.masked || MsixFunctionMasked()) {
        entry.pending = true;
        return;
    }
    entry.pending = false;
    sink_.SignalMsi(entry.address, entry.data);
}

void VirtioModernDevice::SendPendingVectors()
{
    if (!MsixEnabled() || MsixFunctionMasked()) return;
    for (std::uint16_t vector = 0; vector < msix_.size(); ++vector) {
        if (msix_[vector].pending && !msix_[vector].masked) SendVector(vector);
    }
}

void VirtioModernDevice::ConfigWrite(std::uint32_t offset, std::uint8_t size, std::uint32_t value)
{
    PciFunction::ConfigWrite(offset, size, value);
    // MSI-X enable / function mask, or INTx disable, may have changed.
    UpdateLine();
    SendPendingVectors();
}

// --- BAR1: MSI-X table and pending bits ----------------------------------------------------------

std::uint32_t VirtioModernDevice::ReadMsixDword(std::uint32_t offset) const
{
    if (offset >= kMsixTableOffset && offset < kMsixTableOffset + msix_.size() * 16) {
        const auto& entry = msix_[(offset - kMsixTableOffset) / 16];
        switch (offset % 16) {
        case 0: return static_cast<std::uint32_t>(entry.address);
        case 4: return static_cast<std::uint32_t>(entry.address >> 32);
        case 8: return entry.data;
        default: return entry.masked ? 1u : 0u;
        }
    }
    if (offset >= kMsixPbaOffset && offset < kMsixPbaOffset + ((msix_.size() + 63) / 64) * 8) {
        const auto first = (offset - kMsixPbaOffset) * 8;
        std::uint32_t bits = 0;
        for (std::uint32_t bit = 0; bit < 32 && first + bit < msix_.size(); ++bit) {
            if (msix_[first + bit].pending) bits |= 1u << bit;
        }
        return bits;
    }
    return 0;
}

void VirtioModernDevice::WriteMsixDword(std::uint32_t offset, std::uint32_t value)
{
    if (offset < kMsixTableOffset || offset >= kMsixTableOffset + msix_.size() * 16) return; // PBA is read-only.
    const auto vector = static_cast<std::uint16_t>((offset - kMsixTableOffset) / 16);
    auto& entry = msix_[vector];
    switch (offset % 16) {
    case 0: entry.address = (entry.address & 0xFFFFFFFF00000000ull) | value; break;
    case 4: entry.address = (entry.address & 0xFFFFFFFFull) | (static_cast<std::uint64_t>(value) << 32); break;
    case 8: entry.data = value; break;
    default: {
        const bool wasMasked = entry.masked;
        entry.masked = (value & 1) != 0;
        if (wasMasked && !entry.masked && entry.pending && MsixEnabled()) SendVector(vector);
        break;
    }
    }
}

std::uint64_t VirtioModernDevice::MmioRead(std::uint64_t address, std::uint8_t size)
{
    const auto offset = static_cast<std::uint32_t>(address - MmioBarBase(1));
    const auto aligned = offset & ~3u;
    std::uint64_t value = ReadMsixDword(aligned);
    if (size == 8) value |= static_cast<std::uint64_t>(ReadMsixDword(aligned + 4)) << 32;
    value >>= (offset & 3) * 8;
    return size >= 8 ? value : value & ((1ull << (size * 8)) - 1);
}

void VirtioModernDevice::MmioWrite(std::uint64_t address, std::uint8_t size, std::uint64_t value)
{
    const auto offset = static_cast<std::uint32_t>(address - MmioBarBase(1));
    // The table is written with 32- or 64-bit accesses (PCI spec 6.8.2).
    if ((offset & 3) != 0 || size < 4) return;
    WriteMsixDword(offset, static_cast<std::uint32_t>(value));
    if (size == 8) WriteMsixDword(offset + 4, static_cast<std::uint32_t>(value >> 32));
}

} // namespace vmm
