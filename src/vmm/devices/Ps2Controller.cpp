#include "Ps2Controller.h"

#include <algorithm>

namespace vmm {

Ps2Controller::Ps2Controller(InterruptSink& sink, ResetRequest reset)
    : sink_(sink), reset_(std::move(reset))
{
}

void Ps2Controller::Queue(std::uint8_t value, bool auxiliary)
{
    if (output_.size() >= 256) return;
    output_.push_back({value, auxiliary});
    UpdateInterrupts();
}

void Ps2Controller::UpdateInterrupts()
{
    // The 8042 raises IRQ1/IRQ12 while its output buffer holds a byte for the
    // corresponding port. Each newly loaded byte produces a fresh edge.
    const bool full = !output_.empty();
    const bool aux = full && output_.front().auxiliary;
    const bool irq1 = full && !aux && (commandByte_ & 0x01) != 0;
    const bool irq12 = full && aux && (commandByte_ & 0x02) != 0;
    if (irq1 != irq1_) {
        irq1_ = irq1;
        sink_.SetGsi(IsaIrqToGsi(1), irq1);
    }
    if (irq12 != irq12_) {
        irq12_ = irq12;
        sink_.SetGsi(IsaIrqToGsi(12), irq12);
    }
}

std::uint32_t Ps2Controller::IoRead(std::uint16_t port, std::uint8_t)
{
    if (port == 0x64) {
        std::uint8_t status = 0x14; // System flag, last write was a command.
        if (!output_.empty()) {
            status |= 0x01;
            if (output_.front().auxiliary) status |= 0x20;
        }
        return status;
    }
    if (output_.empty()) return 0x00;
    const auto value = output_.front().value;
    output_.pop_front();
    // Drop the line before the next byte is presented so the I/O APIC sees a
    // new rising edge for it.
    if (irq1_) { irq1_ = false; sink_.SetGsi(IsaIrqToGsi(1), false); }
    if (irq12_) { irq12_ = false; sink_.SetGsi(IsaIrqToGsi(12), false); }
    UpdateInterrupts();
    return value;
}

void Ps2Controller::IoWrite(std::uint16_t port, std::uint8_t, std::uint32_t value)
{
    const auto byte = static_cast<std::uint8_t>(value);
    if (port == 0x64) {
        pendingCommand_ = 0;
        switch (byte) {
        case 0x20: Queue(commandByte_, false); break;
        case 0x60: case 0xD1: case 0xD2: case 0xD3: case 0xD4: pendingCommand_ = byte; break;
        case 0xA7: auxDisabled_ = true; commandByte_ |= 0x20; break;
        case 0xA8: auxDisabled_ = false; commandByte_ &= static_cast<std::uint8_t>(~0x20); break;
        case 0xA9: Queue(0x00, false); break; // AUX interface test OK.
        case 0xAA: Queue(0x55, false); break; // Controller self-test OK.
        case 0xAB: Queue(0x00, false); break; // Keyboard interface test OK.
        case 0xAD: keyboardDisabled_ = true; commandByte_ |= 0x10; break;
        case 0xAE: keyboardDisabled_ = false; commandByte_ &= static_cast<std::uint8_t>(~0x10); break;
        case 0xC0: Queue(0x80, false); break; // Input port: keyboard not inhibited.
        case 0xD0: Queue(outputPort_, false); break;
        case 0xFE: case 0xF0: case 0xF2: case 0xF4: case 0xF6: case 0xF8: case 0xFA: case 0xFC:
            // Pulse output lines; bit 0 low resets the CPU.
            if ((byte & 0x01) == 0 && reset_) reset_();
            break;
        default: break;
        }
        UpdateInterrupts();
        return;
    }

    switch (pendingCommand_) {
    case 0x60:
        pendingCommand_ = 0;
        commandByte_ = byte;
        keyboardDisabled_ = (byte & 0x10) != 0;
        auxDisabled_ = (byte & 0x20) != 0;
        UpdateInterrupts();
        return;
    case 0xD1:
        pendingCommand_ = 0;
        outputPort_ = byte;
        if ((byte & 0x01) == 0 && reset_) reset_();
        return;
    case 0xD2:
        pendingCommand_ = 0;
        Queue(byte, false);
        return;
    case 0xD3:
        pendingCommand_ = 0;
        Queue(byte, true);
        return;
    case 0xD4:
        pendingCommand_ = 0;
        MouseCommand(byte);
        return;
    default:
        break;
    }
    KeyboardCommand(byte);
}

void Ps2Controller::KeyboardCommand(std::uint8_t value)
{
    if (keyboardPending_ != 0) {
        const auto command = keyboardPending_;
        keyboardPending_ = 0;
        // Scan code set and typematic rate are accepted; LEDs are remembered
        // so the host can keep Num Lock and Caps Lock in sync.
        if (command == 0xED) keyboardLeds_ = value & 0x07;
        if (command == 0xF0 && value == 0) {
            Queue(0xFA, false);
            Queue(0x41, false); // Reports translated scan code set 2.
            return;
        }
        Queue(0xFA, false);
        return;
    }
    switch (value) {
    case 0xED: case 0xF0: case 0xF3:
        keyboardPending_ = value;
        Queue(0xFA, false);
        break;
    case 0xEE: Queue(0xEE, false); break; // Echo.
    case 0xF2: Queue(0xFA, false); Queue(0xAB, false); Queue(0x83, false); break;
    case 0xF4: keyboardScanning_ = true; Queue(0xFA, false); break;
    case 0xF5: keyboardScanning_ = false; Queue(0xFA, false); break;
    case 0xF6: keyboardScanning_ = true; Queue(0xFA, false); break;
    case 0xFF:
        keyboardScanning_ = true;
        Queue(0xFA, false);
        Queue(0xAA, false);
        break;
    default:
        Queue(0xFA, false);
        break;
    }
}

void Ps2Controller::MouseCommand(std::uint8_t value)
{
    if (mousePending_ != 0) {
        const auto command = mousePending_;
        mousePending_ = 0;
        if (command == 0xF3) {
            mouseSampleRate_ = value;
            sampleHistory_[0] = sampleHistory_[1];
            sampleHistory_[1] = sampleHistory_[2];
            sampleHistory_[2] = value;
            // IntelliMouse unlock sequence: sample rates 200, 100, 80.
            if (sampleHistory_[0] == 200 && sampleHistory_[1] == 100 && sampleHistory_[2] == 80) {
                mouseId_ = 3;
            }
        } else if (command == 0xE8) {
            mouseResolution_ = static_cast<std::uint8_t>(value & 3);
        }
        Queue(0xFA, true);
        return;
    }
    switch (value) {
    case 0xE6: mouseScaling_ = 0; Queue(0xFA, true); break;
    case 0xE7: mouseScaling_ = 1; Queue(0xFA, true); break;
    case 0xE8: case 0xF3: mousePending_ = value; Queue(0xFA, true); break;
    case 0xE9:
        Queue(0xFA, true);
        Queue(static_cast<std::uint8_t>((mouseStreaming_ ? 0x20 : 0) | (mouseScaling_ ? 0x10 : 0) |
                                        (mouseButtons_ & 0x07)), true);
        Queue(mouseResolution_, true);
        Queue(mouseSampleRate_, true);
        break;
    case 0xEA: case 0xF0: Queue(0xFA, true); break;
    case 0xEB: // Read data.
        Queue(0xFA, true);
        Queue(static_cast<std::uint8_t>(0x08 | (mouseButtons_ & 0x07)), true);
        Queue(0, true);
        Queue(0, true);
        if (mouseId_ == 3) Queue(0, true);
        break;
    case 0xF2: Queue(0xFA, true); Queue(mouseId_, true); break;
    case 0xF4: mouseStreaming_ = true; Queue(0xFA, true); break;
    case 0xF5: mouseStreaming_ = false; Queue(0xFA, true); break;
    case 0xF6:
        mouseStreaming_ = false;
        mouseSampleRate_ = 100;
        mouseResolution_ = 2;
        mouseScaling_ = 0;
        Queue(0xFA, true);
        break;
    case 0xFF:
        mouseStreaming_ = false;
        mouseSampleRate_ = 100;
        mouseResolution_ = 2;
        mouseScaling_ = 0;
        mouseId_ = 0;
        Queue(0xFA, true);
        Queue(0xAA, true);
        Queue(0x00, true);
        break;
    default:
        Queue(0xFA, true);
        break;
    }
}

void Ps2Controller::KeyboardScanCode(std::uint8_t code)
{
    if (keyboardDisabled_ || !keyboardScanning_) return;
    Queue(code, false);
}

void Ps2Controller::MouseMotion(int dx, int dy, int wheel, std::uint8_t buttons)
{
    mouseButtons_ = static_cast<std::uint8_t>(buttons & 0x07);
    if (!mouseStreaming_ || auxDisabled_) return;
    // Split large motions into several packets so each delta fits in 9 bits.
    do {
        const int stepX = std::clamp(dx, -255, 255);
        const int stepY = std::clamp(dy, -255, 255);
        const int stepWheel = std::clamp(wheel, -8, 7);
        dx -= stepX;
        dy -= stepY;
        wheel -= stepWheel;
        // PS/2 Y axis points up; host Y axis points down.
        const int packetY = -stepY;
        std::uint8_t header = static_cast<std::uint8_t>(0x08 | (buttons & 0x07));
        if (stepX < 0) header |= 0x10;
        if (packetY < 0) header |= 0x20;
        if (output_.size() > 200) return; // Guest is not draining; drop motion.
        Queue(header, true);
        Queue(static_cast<std::uint8_t>(stepX & 0xFF), true);
        Queue(static_cast<std::uint8_t>(packetY & 0xFF), true);
        if (mouseId_ == 3) Queue(static_cast<std::uint8_t>(stepWheel & 0x0F), true);
    } while (dx != 0 || dy != 0 || wheel != 0);
}

} // namespace vmm
