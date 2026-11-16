#include "VirtioNetDevice.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace {
constexpr std::uint16_t kIoBase = 0xC100;
constexpr std::uint16_t kQueueMax = 256;
constexpr std::uint16_t kDescNext = 1;
constexpr std::uint16_t kDescWrite = 2;
constexpr std::uint32_t kFeatureMac = 1u << 5;
constexpr std::uint32_t kFeatureStatus = 1u << 16;
constexpr std::uint16_t kAvailNoInterrupt = 1;
constexpr std::uint16_t kUsedNoNotify = 1;
constexpr std::size_t kNetHeaderSize = 10; // VirtIO-net header, without mergeable buffers.

template <typename T>
T Load(const std::uint8_t* data)
{
    T value{};
    std::memcpy(&value, data, sizeof(value));
    return value;
}

template <typename T>
void Store(std::uint8_t* data, T value)
{
    std::memcpy(data, &value, sizeof(value));
}
} // namespace

VirtioNetDevice::VirtioNetDevice(MapGuestMemory mapMemory, TransmitPacket transmit)
    : mapMemory_(std::move(mapMemory)), transmit_(std::move(transmit))
{
    queues_[0].size = kQueueMax;
    queues_[1].size = kQueueMax;
}

bool VirtioNetDevice::HandlesPort(std::uint16_t port) const noexcept
{
    return port >= kIoBase && port < kIoBase + 0x100;
}

std::uint32_t VirtioNetDevice::Read(std::uint16_t port, std::uint8_t size)
{
    const std::lock_guard lock(mutex_);
    if (!HandlesPort(port) || size == 0 || size > 4 || port + size > kIoBase + 0x100) return 0xFFFFFFFFu;
    std::uint32_t result = 0;
    for (std::uint8_t i = 0; i < size; ++i) {
        result |= ReadRegister(static_cast<std::uint8_t>(port - kIoBase + i), 1) << (i * 8);
    }
    return result;
}

void VirtioNetDevice::Write(std::uint16_t port, std::uint8_t size, std::uint32_t value)
{
    const std::lock_guard lock(mutex_);
    if (!HandlesPort(port) || size == 0 || size > 4 || port + size > kIoBase + 0x100) return;
    for (std::uint8_t i = 0; i < size; ++i) {
        WriteRegister(static_cast<std::uint8_t>(port - kIoBase + i + 0), 1,
                      static_cast<std::uint8_t>(value >> (i * 8)));
    }
}

std::uint8_t* VirtioNetDevice::Map(std::uint64_t address, std::size_t size) const
{
    return mapMemory_ ? mapMemory_(address, size) : nullptr;
}

std::uint32_t VirtioNetDevice::ReadRegister(std::uint8_t offset, std::uint8_t size)
{
    std::uint32_t value = 0;
    for (std::uint8_t i = 0; i < size; ++i) {
        const auto reg = static_cast<std::uint8_t>(offset + i);
        std::uint8_t byte = 0;
        if (reg < 4) byte = static_cast<std::uint8_t>((kFeatureMac | kFeatureStatus) >> (reg * 8));
        else if (reg >= 0x04 && reg < 0x08) byte = static_cast<std::uint8_t>(guestFeatures_ >> ((reg - 4) * 8));
        else if (reg == 0x0C || reg == 0x0D) byte = static_cast<std::uint8_t>(queues_[selectedQueue_].size >> ((reg - 0x0C) * 8));
        else if (reg == 0x0E || reg == 0x0F) byte = static_cast<std::uint8_t>(selectedQueue_ >> ((reg - 0x0E) * 8));
        else if (reg == 0x12) byte = deviceStatus_;
        else if (reg == 0x13) {
            byte = interruptStatus_;
            interruptStatus_ = 0;
        } else if (reg >= 0x14 && reg < 0x1A) byte = mac_[reg - 0x14];
        else if (reg == 0x1A) byte = static_cast<std::uint8_t>(linkUp_.load(std::memory_order_acquire) ? 1 : 0);
        value |= static_cast<std::uint32_t>(byte) << (i * 8);
    }
    return value;
}

void VirtioNetDevice::WriteRegister(std::uint8_t offset, std::uint8_t size, std::uint32_t value)
{
    (void)size;
    if (offset >= 0x04 && offset < 0x08) {
        const auto shift = (offset - 0x04) * 8;
        guestFeatures_ = (guestFeatures_ & ~(0xFFu << shift)) | ((value & 0xFFu) << shift);
    } else if (offset >= 0x08 && offset < 0x0C) {
        auto& queue = queues_[selectedQueue_];
        const auto shift = (offset - 0x08) * 8;
        queue.pfn = (queue.pfn & ~(0xFFu << shift)) | ((value & 0xFFu) << shift);
        queue.ready = queue.pfn != 0 && queue.size != 0;
        queue.lastAvailable = 0;
    } else if (offset == 0x0E) {
        selectedQueue_ = static_cast<std::uint16_t>(value & 1);
    } else if (offset == 0x10) {
        NotifyQueue(static_cast<std::uint16_t>(value & 1));
    } else if (offset == 0x12) {
        deviceStatus_ = static_cast<std::uint8_t>(value);
        if (deviceStatus_ == 0) {
            queues_ = {};
            guestFeatures_ = 0;
            selectedQueue_ = 0;
            interruptStatus_ = 0;
            interruptDeliveryPending_ = false;
        }
    }
}

std::uint64_t VirtioNetDevice::QueueBase(const Queue& queue) const noexcept
{
    return static_cast<std::uint64_t>(queue.pfn) << 12;
}

std::uint64_t VirtioNetDevice::AvailableBase(const Queue& queue) const noexcept
{
    return QueueBase(queue) + static_cast<std::uint64_t>(queue.size) * 16;
}

std::uint64_t VirtioNetDevice::UsedBase(const Queue& queue) const noexcept
{
    const auto availEnd = AvailableBase(queue) + 4 + static_cast<std::uint64_t>(queue.size) * 2;
    return (availEnd + 0xFFFu) & ~std::uint64_t{0xFFFu};
}

bool VirtioNetDevice::ReadDescriptor(const Queue& queue, std::uint16_t index, Descriptor& descriptor) const
{
    if (!queue.ready || index >= queue.size) return false;
    const auto* data = Map(QueueBase(queue) + static_cast<std::uint64_t>(index) * 16, 16);
    if (!data) return false;
    descriptor.address = Load<std::uint64_t>(data);
    descriptor.length = Load<std::uint32_t>(data + 8);
    descriptor.flags = Load<std::uint16_t>(data + 12);
    descriptor.next = Load<std::uint16_t>(data + 14);
    return true;
}

bool VirtioNetDevice::TakeInterrupt() noexcept
{
    const std::lock_guard lock(mutex_);
    const bool pending = interruptDeliveryPending_;
    interruptDeliveryPending_ = false;
    return pending;
}

void VirtioNetDevice::SetInterrupt()
{
    interruptStatus_ |= 1;
    interruptDeliveryPending_ = true;
}

void VirtioNetDevice::NotifyQueue(std::uint16_t index)
{
    if (index >= queues_.size()) return;
    auto& queue = queues_[index];
    if (!queue.ready || queue.size == 0 || queue.size > kQueueMax) return;
    auto* available = Map(AvailableBase(queue), 4 + static_cast<std::size_t>(queue.size) * 2);
    auto* used = Map(UsedBase(queue), 4 + static_cast<std::size_t>(queue.size) * 8);
    if (!available || !used) return;
    const auto flags = Load<std::uint16_t>(available);
    auto usedIndex = Load<std::uint16_t>(used + 2);
    const auto availableIndex = Load<std::uint16_t>(available + 2);
    while (queue.lastAvailable != availableIndex) {
        const auto head = Load<std::uint16_t>(available + 4 + (queue.lastAvailable % queue.size) * 2);
        std::vector<Descriptor> chain;
        std::uint16_t current = head;
        for (std::uint16_t count = 0; count < queue.size; ++count) {
            Descriptor descriptor{};
            if (!ReadDescriptor(queue, current, descriptor)) return;
            chain.push_back(descriptor);
            if ((descriptor.flags & kDescNext) == 0) break;
            current = descriptor.next;
            if (current >= queue.size) return;
        }
        std::uint32_t bytesWritten = 0;
        if (index == 1) {
            std::vector<std::uint8_t> packet;
            bool valid = true;
            for (const auto& descriptor : chain) {
                if ((descriptor.flags & kDescWrite) != 0 ||
                    descriptor.length > 65536 || packet.size() + descriptor.length > 65536) {
                    valid = false;
                    break;
                }
                auto* data = Map(descriptor.address, descriptor.length);
                if (!data) { valid = false; break; }
                packet.insert(packet.end(), data, data + descriptor.length);
            }
            if (valid && packet.size() >= kNetHeaderSize) {
                const bool accepted = transmit_ ? transmit_(std::span(packet).subspan(kNetHeaderSize)) : false;
                (void)accepted;
            }
        } else {
            // RX buffers remain posted until a frame arrives through Receive().
            // VirtIO requires their used entry to be withheld in that case.
            break;
        }
        const auto slot = used + 4 + (usedIndex % queue.size) * 8;
        Store<std::uint32_t>(slot, head);
        Store<std::uint32_t>(slot + 4, bytesWritten);
        ++usedIndex;
        Store<std::uint16_t>(used + 2, usedIndex);
        ++queue.lastAvailable;
    }
    if ((flags & kAvailNoInterrupt) == 0) SetInterrupt();
}

bool VirtioNetDevice::CompleteRx(std::span<const std::uint8_t> frame)
{
    auto& queue = queues_[0];
    if (!queue.ready || queue.size == 0 || frame.size() + kNetHeaderSize > 65536) return false;
    auto* available = Map(AvailableBase(queue), 4 + static_cast<std::size_t>(queue.size) * 2);
    auto* used = Map(UsedBase(queue), 4 + static_cast<std::size_t>(queue.size) * 8);
    if (!available || !used) return false;
    const auto availableIndex = Load<std::uint16_t>(available + 2);
    if (queue.lastAvailable == availableIndex) return false;
    const auto head = Load<std::uint16_t>(available + 4 + (queue.lastAvailable % queue.size) * 2);
    std::vector<Descriptor> chain;
    auto current = head;
    for (std::uint16_t count = 0; count < queue.size; ++count) {
        Descriptor descriptor{};
        if (!ReadDescriptor(queue, current, descriptor) || (descriptor.flags & kDescWrite) == 0) return false;
        chain.push_back(descriptor);
        if ((descriptor.flags & kDescNext) == 0) break;
        current = descriptor.next;
        if (current >= queue.size) return false;
    }
    std::vector<std::uint8_t> data(kNetHeaderSize, 0);
    data.insert(data.end(), frame.begin(), frame.end());
    std::size_t copied = 0;
    for (const auto& descriptor : chain) {
        auto* target = Map(descriptor.address, descriptor.length);
        if (!target) return false;
        const auto count = std::min<std::size_t>(descriptor.length, data.size() - copied);
        std::memcpy(target, data.data() + copied, count);
        copied += count;
        if (copied == data.size()) break;
    }
    if (copied != data.size()) return false;
    auto usedIndex = Load<std::uint16_t>(used + 2);
    auto* slot = used + 4 + (usedIndex % queue.size) * 8;
    Store<std::uint32_t>(slot, head);
    Store<std::uint32_t>(slot + 4, static_cast<std::uint32_t>(data.size()));
    Store<std::uint16_t>(used + 2, static_cast<std::uint16_t>(usedIndex + 1));
    ++queue.lastAvailable;
    if ((Load<std::uint16_t>(available) & kAvailNoInterrupt) == 0) SetInterrupt();
    return true;
}

bool VirtioNetDevice::Receive(std::span<const std::uint8_t> frame)
{
    const std::lock_guard lock(mutex_);
    return CompleteRx(frame);
}
