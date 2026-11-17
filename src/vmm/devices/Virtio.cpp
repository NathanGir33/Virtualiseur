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


} // namespace vmm
