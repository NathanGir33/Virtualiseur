#pragma once

#include "Virtio.h"

#include <array>
#include <deque>
#include <functional>
#include <span>
#include <vector>

namespace vmm {

// virtio-net (1AF4:1000) with the MAC and link status features. Frames from
// the host backend are queued briefly when the guest has no RX buffer posted.
class VirtioNet final : public VirtioPciDevice {
public:
    using Transmit = std::function<bool(std::span<const std::uint8_t>)>;

    VirtioNet(const GuestMemory& memory, InterruptSink& sink, IoBus& ioBus,
              std::uint16_t ioBase, std::uint32_t gsi, Transmit transmit,
              const std::array<std::uint8_t, 6>& mac);

    void SetLinkUp(bool up) noexcept { linkUp_ = up; }
    // Called with the device lock held.
    void Receive(std::span<const std::uint8_t> frame);

protected:
    std::uint32_t DeviceFeatures() const override;
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
};

} // namespace vmm
