#pragma once

#include "VirtioModern.h"

#include <array>
#include <deque>
#include <functional>
#include <span>
#include <vector>

namespace vmm {

// virtio-net (modern, 1AF4:1041) with the MAC and link status features.
// Frames from the host backend are queued briefly when the guest has no RX
// buffer posted.
class VirtioNet final : public VirtioModernDevice {
public:
    using Transmit = std::function<bool(std::span<const std::uint8_t>)>;

    VirtioNet(const GuestMemory& memory, const VirtioPlacement& placement, Transmit transmit,
              const std::array<std::uint8_t, 6>& mac);

    void SetLinkUp(bool up) noexcept { linkUp_ = up; }
    // Device lock held. Receive puts a frame in the RX ring (or the backlog)
    // without interrupting; FlushReceived interrupts the guest once for all
    // the frames received since the previous flush.
    void Receive(std::span<const std::uint8_t> frame);
    void FlushReceived();

protected:
    std::uint64_t DeviceFeatures() const override;
    std::uint8_t ReadDeviceConfig(std::uint32_t offset) override;
    void QueueNotify(std::uint16_t queue) override;
    void DeviceReset() override;

private:
    bool DeliverFrame(std::span<const std::uint8_t> frame);
    void FlushBacklog();
    void ProcessTransmit();

    Transmit transmit_;
    std::array<std::uint8_t, 6> mac_;
    std::deque<std::vector<std::uint8_t>> backlog_;
    bool linkUp_ = false;
    bool receivedUnsignalled_ = false;
};

} // namespace vmm
