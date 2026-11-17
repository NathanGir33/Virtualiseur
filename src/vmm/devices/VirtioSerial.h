#pragma once

#include "VirtioModern.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace vmm {

// virtio-console in multiport mode ("virtio-serial") with a single named
// port (id 1). The guest sees it as /dev/virtio-ports/<name>; host software
// exchanges a byte stream with whatever guest program opens it.
//
// Queues: 0/1 port 0 (unused), 2/3 control, 4/5 port 1 receive/transmit.
class VirtioSerial final : public VirtioModernDevice {
public:
    using Receive = std::function<void(std::span<const std::uint8_t>)>; // Device lock held.
    using OpenChanged = std::function<void(bool open)>;                  // Device lock held.

    VirtioSerial(const GuestMemory& memory, const VirtioPlacement& placement, std::string portName,
                 Receive receive, OpenChanged openChanged);

    // Device lock held.
    bool GuestOpen() const noexcept { return guestOpen_; }
    void Send(std::span<const std::uint8_t> data);
    std::size_t Backlog() const noexcept { return outgoing_.size(); }

protected:
    std::uint64_t DeviceFeatures() const override { return 1ull << 1; } // VIRTIO_CONSOLE_F_MULTIPORT.
    std::uint8_t ReadDeviceConfig(std::uint32_t offset) override;
    void QueueNotify(std::uint16_t queue) override;
    void DeviceReset() override;

private:
    void SendControl(std::uint32_t id, std::uint16_t event, std::uint16_t value,
                     std::span<const std::uint8_t> extra = {});
    void HandleControl(std::uint32_t id, std::uint16_t event, std::uint16_t value);
    void FlushControl();
    void FlushData();
    void Drain(std::uint16_t queue, bool deliver);
    void SetGuestOpen(bool open);

    std::string portName_;
    Receive receive_;
    OpenChanged openChanged_;
    bool guestOpen_ = false;
    std::deque<std::vector<std::uint8_t>> control_; // Host -> guest control messages.
    std::deque<std::uint8_t> outgoing_;             // Host -> guest port data.
};

} // namespace vmm
