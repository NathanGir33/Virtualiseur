#pragma once

#include "VirtioModern.h"

#include <cstdint>
#include <deque>
#include <string>

namespace vmm {

// virtio-input absolute pointer ("tablet"): the guest pointer follows the
// host cursor exactly, so the mouse needs no capture and adds no latency.
// Coordinates span 0..kAbsMax over the whole guest screen.
class VirtioTablet final : public VirtioModernDevice {
public:
    static constexpr std::int32_t kAbsMax = 32767;
    enum class Button : std::uint16_t { Left = 0x110, Right = 0x111, Middle = 0x112, Side = 0x113, Extra = 0x114 };

    VirtioTablet(const GuestMemory& memory, const VirtioPlacement& placement);

    // Host input; called with the device lock held.
    bool Ready() const noexcept { return DriverReady() && queues_[0].Ready(); }
    void Move(std::int32_t x, std::int32_t y);
    void SetButton(Button button, bool pressed);
    void Wheel(std::int32_t steps, bool horizontal);

protected:
    std::uint64_t DeviceFeatures() const override { return 0; }
    std::uint8_t ReadDeviceConfig(std::uint32_t offset) override;
    void WriteDeviceConfig(std::uint32_t offset, std::uint8_t value) override;
    void QueueNotify(std::uint16_t queue) override;
    void DeviceReset() override;

private:
    struct Event {
        std::uint16_t type;
        std::uint16_t code;
        std::uint32_t value;
    };
    void Queue(std::uint16_t type, std::uint16_t code, std::int32_t value);
    void Sync();
    void Flush();
    // Fills `data` for the selected configuration item; returns its size.
    std::uint8_t ConfigItem(std::uint8_t* data) const;

    std::uint8_t select_ = 0;
    std::uint8_t subsel_ = 0;
    std::deque<Event> pending_;
    std::int32_t lastX_ = -1;
    std::int32_t lastY_ = -1;
};

} // namespace vmm
