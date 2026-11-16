#include "VirtioNet.h"

#include <algorithm>

namespace vmm {
namespace {
constexpr std::uint32_t kFeatureMac = 1u << 5;
constexpr std::uint32_t kFeatureStatus = 1u << 16;
constexpr std::size_t kHeaderSize = 10; // struct virtio_net_hdr without mergeable buffers.
constexpr std::size_t kMaxBacklog = 256;
} // namespace

VirtioNet::VirtioNet(const GuestMemory& memory, InterruptSink& sink, IoBus& ioBus,
                     std::uint16_t ioBase, std::uint32_t gsi, Transmit transmit,
                     const std::array<std::uint8_t, 6>& mac)
    : VirtioPciDevice(0x1000, 1, 0x02, 0x00, memory, sink, gsi, ioBus, ioBase, 2),
      transmit_(std::move(transmit)), mac_(mac)
{
}

std::uint32_t VirtioNet::DeviceFeatures() const
{
    return kFeatureMac | kFeatureStatus | VirtQueue::kFeatureIndirectDescriptors;
}

std::uint8_t VirtioNet::ReadDeviceConfig(std::uint32_t offset)
{
    if (offset < 6) return mac_[offset];
    if (offset == 6) return linkUp_ ? 1 : 0;
    return 0;
}

void VirtioNet::DeviceReset()
{
    backlog_.clear();
}

void VirtioNet::QueueNotify(std::uint16_t queue)
{
    if (queue == 1) ProcessTransmit();
    else if (queue == 0) FlushBacklog();
}

void VirtioNet::ProcessTransmit()
{
    auto& queue = queues_[1];
    VirtQueue::Element element;
    bool any = false;
    std::vector<std::uint8_t> packet;
    while (queue.Pop(memory_, element)) {
        packet.clear();
        bool valid = true;
        for (const auto& buffer : element.readable) {
            if (buffer.length > 65536 || packet.size() + buffer.length > 65536 + kHeaderSize) {
                valid = false;
                break;
            }
            const auto offset = packet.size();
            packet.resize(offset + buffer.length);
            if (!memory_.Read(buffer.address, packet.data() + offset, buffer.length)) {
                valid = false;
                break;
            }
        }
        if (valid && packet.size() > kHeaderSize && linkUp_ && transmit_) {
            (void)transmit_(std::span(packet).subspan(kHeaderSize));
        }
        queue.Push(memory_, element.head, 0);
        any = true;
    }
    if (any) SignalUsedBuffers(1);
}

bool VirtioNet::DeliverFrame(std::span<const std::uint8_t> frame)
{
    auto& queue = queues_[0];
    if (!DriverReady() || !queue.HasAvailable(memory_)) return false;
    VirtQueue::Element element;
    if (!queue.Pop(memory_, element)) return false;
    std::vector<std::uint8_t> data(kHeaderSize, 0);
    data.insert(data.end(), frame.begin(), frame.end());
    std::size_t copied = 0;
    for (const auto& buffer : element.writable) {
        if (copied == data.size()) break;
        const auto count = (std::min<std::size_t>)(buffer.length, data.size() - copied);
        if (!memory_.Write(buffer.address, data.data() + copied, count)) break;
        copied += count;
    }
    queue.Push(memory_, element.head, static_cast<std::uint32_t>(copied));
    return true;
}

void VirtioNet::Receive(std::span<const std::uint8_t> frame)
{
    if (!linkUp_) return;
    if (backlog_.empty() && DeliverFrame(frame)) {
        SignalUsedBuffers(0);
        return;
    }
    if (backlog_.size() < kMaxBacklog) backlog_.emplace_back(frame.begin(), frame.end());
}

void VirtioNet::FlushBacklog()
{
    bool any = false;
    while (!backlog_.empty() && DeliverFrame(backlog_.front())) {
        backlog_.pop_front();
        any = true;
    }
    if (any) SignalUsedBuffers(0);
}

} // namespace vmm
