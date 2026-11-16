#pragma once

#include "../core/Bus.h"

#include <cstdint>
#include <deque>
#include <functional>

namespace vmm {

// i8042 keyboard controller with an MF2 keyboard (port 1) and an IntelliMouse
// compatible PS/2 mouse (port 2). Host input arrives as scan code set 1
// (8042 translation enabled) and relative mouse packets.
class Ps2Controller final : public IoDevice {
public:
    using ResetRequest = std::function<void()>;

    Ps2Controller(InterruptSink& sink, ResetRequest reset);

    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;

    // Host input; call with the device lock held.
    void KeyboardScanCode(std::uint8_t code);
    void MouseMotion(int dx, int dy, int wheel, std::uint8_t buttons);
    bool MouseStreaming() const noexcept { return mouseStreaming_; }
    // Keyboard LEDs last set by the guest (bit 0 Scroll, 1 Num, 2 Caps
    // Lock), or -1 before the guest driver has set them.
    int KeyboardLeds() const noexcept { return keyboardLeds_; }

private:
    struct Byte {
        std::uint8_t value;
        bool auxiliary;
    };

    void Queue(std::uint8_t value, bool auxiliary);
    void UpdateInterrupts();
    void KeyboardCommand(std::uint8_t value);
    void MouseCommand(std::uint8_t value);

    InterruptSink& sink_;
    ResetRequest reset_;
    std::deque<Byte> output_;
    std::uint8_t commandByte_ = 0x47; // Translation, system flag, both port IRQs.
    std::uint8_t pendingCommand_ = 0;
    std::uint8_t outputPort_ = 0x03;
    bool keyboardDisabled_ = false;
    bool auxDisabled_ = false;
    bool keyboardScanning_ = true;
    std::uint8_t keyboardPending_ = 0;
    int keyboardLeds_ = -1;

    std::uint8_t mousePending_ = 0;
    bool mouseStreaming_ = false;
    std::uint8_t mouseSampleRate_ = 100;
    std::uint8_t mouseResolution_ = 2;
    std::uint8_t mouseScaling_ = 0;
    std::uint8_t mouseId_ = 0;
    std::uint8_t sampleHistory_[3] = {};
    std::uint8_t mouseButtons_ = 0;
    bool irq1_ = false;
    bool irq12_ = false;
};

} // namespace vmm
