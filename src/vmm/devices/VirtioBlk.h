#pragma once

#include "VirtioModern.h"

#include "VirtualDisk.h"

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace vmm {

// virtio-blk (modern, 1AF4:1042). Requests are executed on a dedicated I/O
// thread so the vCPU returns to the guest immediately after the queue
// notification.
class VirtioBlk final : public VirtioModernDevice {
public:
    VirtioBlk(const GuestMemory& memory, const VirtioPlacement& placement, DeviceLock& lock,
              virtualiseur::whp::VirtualDisk& disk);
    ~VirtioBlk() override;

    std::uint64_t CompletedRequests() const noexcept { return completed_; }

protected:
    std::uint64_t DeviceFeatures() const override;
    std::uint8_t ReadDeviceConfig(std::uint32_t offset) override;
    void QueueNotify(std::uint16_t queue) override;
    void DeviceReset() override;

private:
    void Worker(std::stop_token stop);
    std::uint8_t Execute(const VirtQueue::Element& element, std::uint32_t& written);

    DeviceLock& lock_;
    virtualiseur::whp::VirtualDisk& disk_;
    std::mutex queueMutex_;
    std::vector<std::uint8_t> bounce_; // Worker-thread scratch buffer for one request.
    HANDLE wake_ = nullptr;
    std::atomic_uint64_t completed_ = 0;
    std::jthread worker_;
};

} // namespace vmm
