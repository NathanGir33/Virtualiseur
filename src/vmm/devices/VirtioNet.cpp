#include "VirtioNet.h"

#include <algorithm>

namespace vmm {
namespace {
constexpr std::uint64_t kFeatureMac = 1ull << 5;
constexpr std::uint64_t kFeatureStatus = 1ull << 16;
// struct virtio_net_hdr: with VIRTIO_F_VERSION_1 it always includes the
// 16-bit num_buffers field, mergeable RX buffers or not.
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kMaxBacklog = 256;
} // namespace

VirtioNet::VirtioNet(const GuestMemory& memory, const VirtioPlacement& placement, Transmit transmit,
                     const std::array<std::uint8_t, 6>& mac)
    : VirtioModernDevice(1, 0x02, 0x00, memory, placement, 2), transmit_(std::move(transmit)), mac_(mac)
{
}

std::uint64_t VirtioNet::DeviceFeatures() const
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
    receivedUnsignalled_ = false;
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
    data[10] = 1; // num_buffers: the frame fits in this one chain.
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
        receivedUnsignalled_ = true;
        return;
    }
    if (backlog_.size() < kMaxBacklog) backlog_.emplace_back(frame.begin(), frame.end());
}

void VirtioNet::FlushReceived()
{
    if (!receivedUnsignalled_) return;
    receivedUnsignalled_ = false;
    SignalUsedBuffers(0);
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
