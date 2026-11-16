#pragma once

#include "../core/Bus.h"

#include <chrono>
#include <cstdint>
#include <functional>

namespace vmm {

// ACPI fixed hardware: PM1a event/control blocks and the 3.579545 MHz PM
// timer. Implements soft-off (S5) and the fixed power button, which lets the
// host request a clean guest shutdown.
class AcpiPm final : public IoDevice {
public:
    static constexpr std::uint16_t kBase = 0x600;
    static constexpr std::uint16_t kPm1EventBlock = kBase;      // 4 bytes
    static constexpr std::uint16_t kPm1ControlBlock = kBase + 4; // 2 bytes
    static constexpr std::uint16_t kPmTimerBlock = kBase + 8;    // 4 bytes
    static constexpr std::uint32_t kSciGsi = 9;
    static constexpr std::uint8_t kS5SleepType = 5;

    AcpiPm(InterruptSink& sink, std::function<void()> powerOff);

    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;

    // Host "power button" press. Returns false if the guest OS has not
    // enabled power button events (no ACPI-aware OS running yet).
    bool PressPowerButton();

private:
    std::uint8_t ReadByte(std::uint16_t offset);
    void WriteByte(std::uint16_t offset, std::uint8_t value);
    void UpdateSci();

    InterruptSink& sink_;
    std::function<void()> powerOff_;
    std::uint16_t status_ = 0;
    std::uint16_t enable_ = 0;
    std::uint16_t control_ = 0x0001; // SCI_EN: ACPI mode is always on (no SMI_CMD).
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    bool sciAsserted_ = false;
};

} // namespace vmm
