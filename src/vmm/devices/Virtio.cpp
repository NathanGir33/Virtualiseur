#include "Virtio.h"

#include "../core/Log.h"

namespace vmm {
namespace {
constexpr std::uint16_t kDescNext = 1;
constexpr std::uint16_t kDescWrite = 2;
constexpr std::uint16_t kDescIndirect = 4;
} // namespace

bool VirtQueue::HasAvailable(const GuestMemory& memory) const
{
    if (!Ready()) return false;
    std::uint16_t index = 0;
    if (!memory.ReadValue(Available() + 2, index)) return false;
    return index != lastAvailable_;
}

bool VirtQueue::Pop(const GuestMemory& memory, Element& element)
{
    if (!Ready()) return false;
    std::uint16_t availableIndex = 0;
    if (!memory.ReadValue(Available() + 2, availableIndex) || availableIndex == lastAvailable_) return false;
    std::uint16_t head = 0;
    if (!memory.ReadValue(Available() + 4 + (lastAvailable_ % size_) * 2ull, head)) return false;
    ++lastAvailable_;

    element.head = head;
    element.readable.clear();
    element.writable.clear();

    return ReadChain(memory, Descriptors(), size_, head, true, element);
}

// Walks a descriptor chain in `table` (the ring's table or an indirect table).
bool VirtQueue::ReadChain(const GuestMemory& memory, std::uint64_t table, std::uint32_t tableSize,
                          std::uint16_t first, bool allowIndirect, Element& element)
{
    std::uint16_t index = first;
    for (std::uint32_t count = 0; count < tableSize; ++count) {
        if (index >= tableSize) return false;
        const auto descriptor = table + index * 16ull;
        std::uint64_t address = 0;
        std::uint32_t length = 0;
        std::uint16_t flags = 0;
        std::uint16_t next = 0;
        if (!memory.ReadValue(descriptor, address) || !memory.ReadValue(descriptor + 8, length) ||
            !memory.ReadValue(descriptor + 12, flags) || !memory.ReadValue(descriptor + 14, next)) {
            return false;
        }
        if ((flags & kDescIndirect) != 0) {
            // An indirect table replaces the rest of the chain and cannot nest.
            if (!allowIndirect || length < 16 || (length % 16) != 0 || length > 16 * 4096) return false;
            return ReadChain(memory, address, length / 16, 0, false, element);
        }
        ((flags & kDescWrite) != 0 ? element.writable : element.readable).push_back({address, length});
        if ((flags & kDescNext) == 0) return true;
        index = next;
    }
    return false;
}

void VirtQueue::Push(const GuestMemory& memory, std::uint16_t head, std::uint32_t written)
{
    std::uint16_t usedIndex = 0;
    if (!memory.ReadValue(Used() + 2, usedIndex)) return;
    const auto slot = Used() + 4 + (usedIndex % size_) * 8ull;
    (void)memory.WriteValue(slot, static_cast<std::uint32_t>(head));
    (void)memory.WriteValue(slot + 4, written);
    (void)memory.WriteValue(Used() + 2, static_cast<std::uint16_t>(usedIndex + 1));
}

bool VirtQueue::InterruptSuppressed(const GuestMemory& memory) const
{
    std::uint16_t flags = 0;
    return memory.ReadValue(Available(), flags) && (flags & 1) != 0;
}

VirtioPciDevice::VirtioPciDevice(std::uint16_t pciDeviceId, std::uint16_t virtioType, std::uint8_t classCode,
                                 std::uint8_t subclass, const GuestMemory& memory, InterruptSink& sink,
                                 std::uint32_t gsi, IoBus& ioBus, std::uint16_t ioBase, std::size_t queueCount)
    : PciFunction(0x1AF4, pciDeviceId, classCode, subclass, 0x00, 0x00),
      memory_(memory), queues_(queueCount), sink_(sink), gsi_(gsi), ioBase_(ioBase)
{
    AttachIoBus(&ioBus);
    SetSubsystem(0x1AF4, virtioType);
    SetInterrupt(static_cast<std::uint8_t>(gsi), 1);
    config_[0x04] = 0x05;
    SetIoBar(0, ioBase, 256, this);
}

std::uint32_t VirtioPciDevice::IoRead(std::uint16_t port, std::uint8_t size)
{
    const auto barBase = static_cast<std::uint16_t>((config_[0x10] | (config_[0x11] << 8)) & 0xFFFC);
    const auto offset = static_cast<std::uint32_t>(port - barBase);
    std::uint32_t value = 0;
    for (std::uint8_t index = 0; index < size; ++index) {
        value |= static_cast<std::uint32_t>(ReadRegister(offset + index)) << (index * 8);
    }
    return value;
}

void VirtioPciDevice::IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value)
{
    const auto barBase = static_cast<std::uint16_t>((config_[0x10] | (config_[0x11] << 8)) & 0xFFFC);
    const auto offset = static_cast<std::uint32_t>(port - barBase);
    for (std::uint8_t index = 0; index < size; ++index) {
        WriteRegister(offset + index, static_cast<std::uint8_t>(value >> (index * 8)));
    }
    // Multi-byte registers take effect once their last byte is written.
    if (offset == 0x08 && size == 4 && selected_ < queues_.size()) {
        queues_[selected_].SetPfn(pendingPfn_);
    } else if (offset == 0x10 && size >= 2) {
        if (pendingNotify_ < queues_.size()) QueueNotify(pendingNotify_);
    }
}

std::uint8_t VirtioPciDevice::ReadRegister(std::uint32_t offset)
{
    switch (offset) {
    case 0x00: case 0x01: case 0x02: case 0x03:
        return static_cast<std::uint8_t>(DeviceFeatures() >> ((offset & 3) * 8));
    case 0x04: case 0x05: case 0x06: case 0x07:
        return static_cast<std::uint8_t>(guestFeatures_ >> ((offset & 3) * 8));
    case 0x08: case 0x09: case 0x0A: case 0x0B:
        return selected_ < queues_.size()
            ? static_cast<std::uint8_t>(queues_[selected_].Pfn() >> ((offset & 3) * 8)) : 0;
    case 0x0C: return selected_ < queues_.size() ? static_cast<std::uint8_t>(VirtQueue::kSize & 0xFF) : 0;
    case 0x0D: return selected_ < queues_.size() ? static_cast<std::uint8_t>(VirtQueue::kSize >> 8) : 0;
    case 0x0E: return static_cast<std::uint8_t>(selected_);
    case 0x0F: return static_cast<std::uint8_t>(selected_ >> 8);
    case 0x12: return status_;
    case 0x13: {
        const auto value = isr_;
        isr_ = 0;
        UpdateLine();
        return value;
    }
    default:
        if (offset >= 0x14) return ReadDeviceConfig(offset - 0x14);
        return 0;
    }
}

void VirtioPciDevice::WriteRegister(std::uint32_t offset, std::uint8_t value)
{
    switch (offset) {
    case 0x04: case 0x05: case 0x06: case 0x07: {
        const auto shift = (offset & 3) * 8;
        guestFeatures_ = (guestFeatures_ & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(value) << shift);
        guestFeatures_ &= DeviceFeatures();
        break;
    }
    case 0x08: case 0x09: case 0x0A: case 0x0B: {
        const auto shift = (offset & 3) * 8;
        if (offset == 0x08) pendingPfn_ = 0;
        pendingPfn_ = (pendingPfn_ & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(value) << shift);
        break;
    }
    case 0x0E: selected_ = static_cast<std::uint16_t>((selected_ & 0xFF00) | value); break;
    case 0x0F: selected_ = static_cast<std::uint16_t>((selected_ & 0x00FF) | (value << 8)); break;
    case 0x10: pendingNotify_ = static_cast<std::uint16_t>((pendingNotify_ & 0xFF00) | value); break;
    case 0x11: pendingNotify_ = static_cast<std::uint16_t>((pendingNotify_ & 0x00FF) | (value << 8)); break;
    case 0x12:
        status_ = value;
        if (value == 0) {
            DeviceReset();
            for (auto& queue : queues_) queue.Reset();
            guestFeatures_ = 0;
            selected_ = 0;
            isr_ = 0;
            UpdateLine();
        }
        break;
    default:
        if (offset >= 0x14) WriteDeviceConfig(offset - 0x14, value);
        break;
    }
}

void VirtioPciDevice::SignalUsedBuffers(std::uint16_t queue)
{
    if (queue < queues_.size() && queues_[queue].InterruptSuppressed(memory_)) return;
    isr_ |= 0x01;
    UpdateLine();
}

void VirtioPciDevice::UpdateLine()
{
    const bool asserted = isr_ != 0;
    if (asserted == lineAsserted_) return;
    lineAsserted_ = asserted;
    sink_.SetGsi(gsi_, asserted);
}

} // namespace vmm
