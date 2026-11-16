#pragma once

#include "Virtio.h"

#include "VirtualDisk.h"

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace vmm {

// virtio-blk (1AF4:1001). Requests are executed on a dedicated I/O thread so
// the vCPU returns to the guest immediately after the queue notification.
class VirtioBlk final : public VirtioPciDevice {
public:
    VirtioBlk(const GuestMemory& memory, InterruptSink& sink, DeviceLock& lock, IoBus& ioBus,
              std::uint16_t ioBase, std::uint32_t gsi, virtualiseur::whp::VirtualDisk& disk);
    ~VirtioBlk() override;

    std::uint64_t CompletedRequests() const noexcept { return completed_; }

protected:
    std::uint32_t DeviceFeatures() const override;
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
