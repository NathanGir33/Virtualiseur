#include "VirtioSerial.h"

#include "../core/Log.h"

#include <algorithm>
#include <cstring>

namespace vmm {
namespace {
// struct virtio_console_control events.
constexpr std::uint16_t kDeviceReady = 0;
constexpr std::uint16_t kDeviceAdd = 1;
constexpr std::uint16_t kPortReady = 3;
constexpr std::uint16_t kPortOpen = 6;
constexpr std::uint16_t kPortName = 7;

constexpr std::uint32_t kPortId = 1;
constexpr std::uint16_t kControlReceive = 2;  // Host -> guest control.
constexpr std::uint16_t kControlTransmit = 3; // Guest -> host control.
constexpr std::uint16_t kPortReceive = 4;     // Host -> guest data.
constexpr std::uint16_t kPortTransmit = 5;    // Guest -> host data.
constexpr std::size_t kMaxOutgoing = 64u * 1024 * 1024;
} // namespace

VirtioSerial::VirtioSerial(const GuestMemory& memory, InterruptSink& sink, IoBus& ioBus, std::uint16_t ioBase,
                           std::uint32_t gsi, std::string portName, Receive receive, OpenChanged openChanged)
    : VirtioModernDevice(3, 0x07, 0x80, memory, sink, gsi, ioBus, ioBase, 6),
      portName_(std::move(portName)), receive_(std::move(receive)), openChanged_(std::move(openChanged))
{
}

std::uint8_t VirtioSerial::ReadDeviceConfig(std::uint32_t offset)
{
    // struct virtio_console_config: cols, rows (u16), max_nr_ports, emerg_wr (u32).
    if (offset >= 4 && offset < 8) return static_cast<std::uint8_t>(2u >> ((offset - 4) * 8));
    return 0;
}

void VirtioSerial::DeviceReset()
{
    control_.clear();
    outgoing_.clear();
    SetGuestOpen(false);
}

void VirtioSerial::SetGuestOpen(bool open)
{
    if (open == guestOpen_) return;
    guestOpen_ = open;
    if (!open) outgoing_.clear();
    if (openChanged_) openChanged_(open);
}

void VirtioSerial::QueueNotify(std::uint16_t queue)
{
    switch (queue) {
    case kControlReceive: FlushControl(); break;
    case kPortReceive: FlushData(); break;
    case kControlTransmit: {
        auto& ring = queues_[queue];
        VirtQueue::Element element;
        bool any = false;
        while (ring.Pop(memory_, element)) {
            std::uint8_t message[8] = {};
            std::size_t copied = 0;
            for (const auto& buffer : element.readable) {
                const auto count = (std::min<std::size_t>)(buffer.length, sizeof(message) - copied);
                if (count == 0 || !memory_.Read(buffer.address, message + copied, count)) break;
                copied += count;
            }
            ring.Push(memory_, element.head, 0);
            any = true;
            if (copied == sizeof(message)) {
                std::uint32_t id = 0;
                std::uint16_t event = 0;
                std::uint16_t value = 0;
                std::memcpy(&id, message, 4);
                std::memcpy(&event, message + 4, 2);
                std::memcpy(&value, message + 6, 2);
                HandleControl(id, event, value);
            }
        }
        if (any) SignalUsedBuffers(queue);
        break;
    }
    case kPortTransmit: Drain(queue, true); break;
    default: Drain(queue, false); break; // Port 0 is not used.
    }
}

void VirtioSerial::Drain(std::uint16_t queue, bool deliver)
{
    auto& ring = queues_[queue];
    VirtQueue::Element element;
    std::vector<std::uint8_t> data;
    bool any = false;
    while (ring.Pop(memory_, element)) {
        for (const auto& buffer : element.readable) {
            if (!deliver || buffer.length > 16u * 1024 * 1024) continue;
            data.resize(buffer.length);
            if (memory_.Read(buffer.address, data.data(), buffer.length) && receive_) receive_(data);
        }
        ring.Push(memory_, element.head, 0);
        any = true;
    }
    if (any) SignalUsedBuffers(queue);
}

void VirtioSerial::HandleControl(std::uint32_t id, std::uint16_t event, std::uint16_t value)
{
    switch (event) {
    case kDeviceReady:
        if (value == 1) SendControl(kPortId, kDeviceAdd, 1);
        break;
    case kPortReady:
        if (id == kPortId && value == 1) {
            SendControl(kPortId, kPortName, 1,
                        std::span(reinterpret_cast<const std::uint8_t*>(portName_.data()), portName_.size()));
            SendControl(kPortId, kPortOpen, 1); // The host end is always connected.
        }
        break;
    case kPortOpen:
        if (id == kPortId) {
            VMM_INFO("virtio-serial: port %s %s par le système invité.", portName_.c_str(),
                     value != 0 ? "ouvert" : "fermé");
            SetGuestOpen(value != 0);
            FlushData();
        }
        break;
    default:
        break;
    }
}

void VirtioSerial::SendControl(std::uint32_t id, std::uint16_t event, std::uint16_t value,
                               std::span<const std::uint8_t> extra)
{
    std::vector<std::uint8_t> message(8 + extra.size());
    std::memcpy(message.data(), &id, 4);
    std::memcpy(message.data() + 4, &event, 2);
    std::memcpy(message.data() + 6, &value, 2);
    if (!extra.empty()) std::memcpy(message.data() + 8, extra.data(), extra.size());
    control_.push_back(std::move(message));
    FlushControl();
}

void VirtioSerial::FlushControl()
{
    auto& ring = queues_[kControlReceive];
    if (!ring.Ready()) return;
    VirtQueue::Element element;
    bool any = false;
    while (!control_.empty() && ring.Pop(memory_, element)) {
        const auto& message = control_.front();
        std::size_t written = 0;
        for (const auto& buffer : element.writable) {
            const auto count = (std::min<std::size_t>)(buffer.length, message.size() - written);
            if (count == 0 || !memory_.Write(buffer.address, message.data() + written, count)) break;
            written += count;
        }
        ring.Push(memory_, element.head, static_cast<std::uint32_t>(written));
        control_.pop_front();
        any = true;
    }
    if (any) SignalUsedBuffers(kControlReceive);
}

void VirtioSerial::Send(std::span<const std::uint8_t> data)
{
    if (!guestOpen_ || outgoing_.size() + data.size() > kMaxOutgoing) return;
    outgoing_.insert(outgoing_.end(), data.begin(), data.end());
    FlushData();
}

void VirtioSerial::FlushData()
{
    auto& ring = queues_[kPortReceive];
    if (!guestOpen_ || !ring.Ready()) return;
    VirtQueue::Element element;
    std::vector<std::uint8_t> chunk;
    bool any = false;
    while (!outgoing_.empty() && ring.Pop(memory_, element)) {
        std::size_t written = 0;
        for (const auto& buffer : element.writable) {
            const auto count = (std::min<std::size_t>)(buffer.length, outgoing_.size());
            if (count == 0) break;
            chunk.assign(outgoing_.begin(), outgoing_.begin() + static_cast<std::ptrdiff_t>(count));
            if (!memory_.Write(buffer.address, chunk.data(), count)) break;
            outgoing_.erase(outgoing_.begin(), outgoing_.begin() + static_cast<std::ptrdiff_t>(count));
            written += count;
        }
        ring.Push(memory_, element.head, static_cast<std::uint32_t>(written));
        any = true;
    }
    if (any) SignalUsedBuffers(kPortReceive);
}

} // namespace vmm
