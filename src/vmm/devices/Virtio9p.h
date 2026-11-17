#pragma once

#include "VirtioModern.h"
#include "../share/NinePServer.h"

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace vmm {

// virtio-9p (modern, 1AF4:1049): one shared host folder, served in 9P2000.L
// by NinePServer. The device configuration carries the mount tag
// (VIRTIO_9P_MOUNT_TAG): one device per share, each mounted by its tag in
// the guest (mount -t 9p -o trans=virtio,version=9p2000.L <tag> <dir>).
//
// As with virtio-blk, requests run on a dedicated I/O thread: the vCPU goes
// back to the guest right after the queue notification.
class Virtio9p final : public VirtioModernDevice {
public:
    static constexpr std::size_t kMaxTagLength = 31;

    // Throws if the folder cannot be shared.
    Virtio9p(const GuestMemory& memory, const VirtioPlacement& placement, std::string tag,
             const std::filesystem::path& folder, bool readOnly);
    ~Virtio9p() override;

    const std::string& Tag() const noexcept { return tag_; }
    const std::wstring& Folder() const noexcept { return server_.RootPath(); }
    std::uint64_t Requests() const noexcept { return requests_.load(); }

protected:
    std::uint64_t DeviceFeatures() const override;
    std::uint8_t ReadDeviceConfig(std::uint32_t offset) override;
    void QueueNotify(std::uint16_t queue) override;
    HANDLE NotifyEvent(std::uint16_t) override { return wake_; }
    void DeviceReset() override;

private:
    void Worker(std::stop_token stop);

    std::string tag_;
    NinePServer server_;
    std::mutex queueMutex_;
    std::vector<std::uint8_t> request_; // Worker-thread scratch buffer.
    HANDLE wake_ = nullptr;
    std::atomic_uint64_t requests_ = 0;
    std::jthread worker_;
};

} // namespace vmm
