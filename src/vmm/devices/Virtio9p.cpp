#include "Virtio9p.h"

#include "../core/Log.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace vmm {
namespace {
constexpr std::uint64_t kFeatureMountTag = 1ull << 0; // VIRTIO_9P_MOUNT_TAG.
constexpr std::uint16_t kVirtioType = 9;
} // namespace

Virtio9p::Virtio9p(const GuestMemory& memory, const VirtioPlacement& placement, std::string tag,
                   const std::filesystem::path& folder, bool readOnly)
    : VirtioModernDevice(kVirtioType, 0x01, 0x80, memory, placement, 1), tag_(std::move(tag)), server_(folder, readOnly)
{
    if (tag_.empty() || tag_.size() > kMaxTagLength) throw std::invalid_argument("nom de partage vide ou trop long");
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    worker_ = std::jthread([this](std::stop_token stop) { Worker(stop); });
}

Virtio9p::~Virtio9p()
{
    worker_.request_stop();
    if (wake_ != nullptr) SetEvent(wake_);
    if (worker_.joinable()) worker_.join();
    if (wake_ != nullptr) CloseHandle(wake_);
}

std::uint64_t Virtio9p::DeviceFeatures() const
{
    return kFeatureMountTag | VirtQueue::kFeatureIndirectDescriptors;
}

std::uint8_t Virtio9p::ReadDeviceConfig(std::uint32_t offset)
{
    // struct virtio_9p_config: le16 tag_len, then the tag (not NUL-terminated).
    const auto length = static_cast<std::uint16_t>(tag_.size());
    if (offset < 2) return static_cast<std::uint8_t>(length >> (offset * 8));
    const auto index = offset - 2;
    return index < tag_.size() ? static_cast<std::uint8_t>(tag_[index]) : 0;
}

void Virtio9p::QueueNotify(std::uint16_t)
{
    SetEvent(wake_);
}

void Virtio9p::DeviceReset()
{
    // Wait for an in-flight batch before the queue memory is released.
    const std::lock_guard guard(queueMutex_);
}

void Virtio9p::Worker(std::stop_token stop)
{
    SetThreadDescription(GetCurrentThread(), L"virtio-9p");
    while (!stop.stop_requested()) {
        WaitForSingleObject(wake_, INFINITE);
        if (stop.stop_requested()) break;
        bool completedAny = false;
        {
            const std::lock_guard guard(queueMutex_);
            auto& queue = queues_[0];
            VirtQueue::Element element;
            while (queue.Pop(memory_, element)) {
                // The T-message is spread over the readable buffers (a large
                // Twrite carries its data in separate pages); the R-message
                // goes over the writable ones in order (a large Rread lands
                // its header in the first and its data in the guest's pages).
                request_.clear();
                bool valid = true;
                for (const auto& buffer : element.readable) {
                    if (request_.size() + buffer.length > NinePServer::kMaxMessageSize + 4096) {
                        valid = false;
                        break;
                    }
                    const auto offset = request_.size();
                    request_.resize(offset + buffer.length);
                    if (!memory_.Read(buffer.address, request_.data() + offset, buffer.length)) {
                        valid = false;
                        break;
                    }
                }
                std::uint32_t written = 0;
                if (valid) {
                    const auto response = server_.Handle(request_);
                    for (const auto& buffer : element.writable) {
                        if (written == response.size()) break;
                        const auto count = (std::min<std::size_t>)(buffer.length, response.size() - written);
                        if (!memory_.Write(buffer.address, response.data() + written, count)) break;
                        written += static_cast<std::uint32_t>(count);
                    }
                    if (written < response.size()) {
                        VMM_WARN("virtio-9p « %s » : réponse de %zu octets, tampons de %u octets.", tag_.c_str(),
                                 response.size(), written);
                    }
                }
                queue.Push(memory_, element.head, written);
                ++requests_;
                completedAny = true;
            }
        }
        if (completedAny) {
            const DeviceGuard guard(lock_);
            SignalUsedBuffers(0);
        }
    }
}

} // namespace vmm
