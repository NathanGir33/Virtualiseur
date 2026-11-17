#include "VirtioInput.h"

#include <algorithm>
#include <cstring>

namespace vmm {
namespace {
// virtio_input_config.select values.
constexpr std::uint8_t kCfgIdName = 0x01;
constexpr std::uint8_t kCfgIdSerial = 0x02;
constexpr std::uint8_t kCfgIdDevids = 0x03;
constexpr std::uint8_t kCfgPropBits = 0x10;
constexpr std::uint8_t kCfgEvBits = 0x11;
constexpr std::uint8_t kCfgAbsInfo = 0x12;

// Linux input event types and codes.
constexpr std::uint16_t kEvSyn = 0x00;
constexpr std::uint16_t kEvKey = 0x01;
constexpr std::uint16_t kEvRel = 0x02;
constexpr std::uint16_t kEvAbs = 0x03;
constexpr std::uint16_t kRelHWheel = 0x06;
constexpr std::uint16_t kRelWheel = 0x08;
constexpr std::uint16_t kAbsX = 0x00;
constexpr std::uint16_t kAbsY = 0x01;

constexpr std::size_t kMaxPending = 1024;
constexpr char kName[] = "Virtualiseur Tablet";
constexpr char kSerial[] = "virtualiseur-tablet";

void SetBit(std::uint8_t* bitmap, std::uint32_t bit, std::uint8_t& size)
{
    bitmap[bit / 8] |= static_cast<std::uint8_t>(1u << (bit % 8));
    size = (std::max)(size, static_cast<std::uint8_t>(bit / 8 + 1));
}
} // namespace

VirtioTablet::VirtioTablet(const GuestMemory& memory, const VirtioPlacement& placement)
    : VirtioModernDevice(18, 0x09, 0x80, memory, placement, 2)
{
}

std::uint8_t VirtioTablet::ConfigItem(std::uint8_t* data) const
{
    std::uint8_t size = 0;
    switch (select_) {
    case kCfgIdName:
        if (subsel_ != 0) return 0;
        std::memcpy(data, kName, sizeof(kName) - 1);
        return sizeof(kName) - 1;
    case kCfgIdSerial:
        if (subsel_ != 0) return 0;
        std::memcpy(data, kSerial, sizeof(kSerial) - 1);
        return sizeof(kSerial) - 1;
    case kCfgIdDevids: {
        if (subsel_ != 0) return 0;
        const std::uint16_t ids[4] = {0x06 /* BUS_VIRTUAL */, 0x0627, 0x0003, 0x0001};
        std::memcpy(data, ids, sizeof(ids));
        return sizeof(ids);
    }
    case kCfgPropBits:
        return 0;
    case kCfgEvBits:
        if (subsel_ == kEvKey) {
            for (std::uint32_t code = 0x110; code <= 0x114; ++code) SetBit(data, code, size);
        } else if (subsel_ == kEvRel) {
            SetBit(data, kRelHWheel, size);
            SetBit(data, kRelWheel, size);
        } else if (subsel_ == kEvAbs) {
            SetBit(data, kAbsX, size);
            SetBit(data, kAbsY, size);
        }
        return size;
    case kCfgAbsInfo: {
        if (subsel_ != kAbsX && subsel_ != kAbsY) return 0;
        const std::uint32_t info[5] = {0, static_cast<std::uint32_t>(kAbsMax), 0, 0, 0}; // min max fuzz flat res
        std::memcpy(data, info, sizeof(info));
        return sizeof(info);
    }
    default:
        return 0;
    }
}

std::uint8_t VirtioTablet::ReadDeviceConfig(std::uint32_t offset)
{
    // struct virtio_input_config: select, subsel, size, reserved[5], u[128].
    if (offset == 0) return select_;
    if (offset == 1) return subsel_;
    std::uint8_t data[128] = {};
    const auto size = ConfigItem(data);
    if (offset == 2) return size;
    if (offset >= 8 && offset < 8 + sizeof(data)) return data[offset - 8];
    return 0;
}

void VirtioTablet::WriteDeviceConfig(std::uint32_t offset, std::uint8_t value)
{
    if (offset == 0) select_ = value;
    else if (offset == 1) subsel_ = value;
}

void VirtioTablet::DeviceReset()
{
    pending_.clear();
    select_ = 0;
    subsel_ = 0;
    lastX_ = -1;
    lastY_ = -1;
}

void VirtioTablet::QueueNotify(std::uint16_t queue)
{
    if (queue == 0) {
        Flush(); // New event buffers.
        return;
    }
    // Status queue (LEDs...): consume and complete.
    VirtQueue::Element element;
    bool any = false;
    while (queues_[1].Pop(memory_, element)) {
        queues_[1].Push(memory_, element.head, 0);
        any = true;
    }
    if (any) SignalUsedBuffers(1);
}

void VirtioTablet::Queue(std::uint16_t type, std::uint16_t code, std::int32_t value)
{
    if (pending_.size() >= kMaxPending) return;
    pending_.push_back({type, code, static_cast<std::uint32_t>(value)});
}

void VirtioTablet::Sync()
{
    Queue(kEvSyn, 0, 0);
    Flush();
}

void VirtioTablet::Flush()
{
    auto& queue = queues_[0];
    bool any = false;
    VirtQueue::Element element;
    while (!pending_.empty() && queue.Pop(memory_, element)) {
        const auto& event = pending_.front();
        std::uint32_t written = 0;
        if (!element.writable.empty() && element.writable[0].length >= sizeof(Event) &&
            memory_.Write(element.writable[0].address, &event, sizeof(Event))) {
            written = sizeof(Event);
        }
        queue.Push(memory_, element.head, written);
        pending_.pop_front();
        any = true;
    }
    if (any) SignalUsedBuffers(0);
}

void VirtioTablet::Move(std::int32_t x, std::int32_t y)
{
    if (!Ready()) return;
    if (x == lastX_ && y == lastY_) return;
    if (x != lastX_) Queue(kEvAbs, kAbsX, x);
    if (y != lastY_) Queue(kEvAbs, kAbsY, y);
    lastX_ = x;
    lastY_ = y;
    Sync();
}

void VirtioTablet::SetButton(Button button, bool pressed)
{
    if (!Ready()) return;
    Queue(kEvKey, static_cast<std::uint16_t>(button), pressed ? 1 : 0);
    Sync();
}

void VirtioTablet::Wheel(std::int32_t steps, bool horizontal)
{
    if (!Ready() || steps == 0) return;
    Queue(kEvRel, horizontal ? kRelHWheel : kRelWheel, steps);
    Sync();
}

} // namespace vmm
