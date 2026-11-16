#include "AcpiPm.h"

#include "../core/Log.h"

namespace vmm {

AcpiPm::AcpiPm(InterruptSink& sink, std::function<void()> powerOff)
    : sink_(sink), powerOff_(std::move(powerOff))
{
}

std::uint32_t AcpiPm::IoRead(std::uint16_t port, std::uint8_t size)
{
    const auto offset = static_cast<std::uint16_t>(port - kBase);
    if (offset >= 8 && offset < 12) {
        // 32-bit PM timer (FADT TMR_VAL_EXT).
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start_).count();
        const auto ns = static_cast<std::uint64_t>(elapsed);
        const auto ticks = (ns / 1000000000ull) * 3579545ull + ((ns % 1000000000ull) * 3579545ull) / 1000000000ull;
        const auto value = static_cast<std::uint32_t>(ticks);
        return value >> ((offset - 8) * 8);
    }
    std::uint32_t value = 0;
    for (std::uint8_t index = 0; index < size; ++index) {
        value |= static_cast<std::uint32_t>(ReadByte(static_cast<std::uint16_t>(offset + index))) << (index * 8);
    }
    return value;
}

void AcpiPm::IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value)
{
    const auto offset = static_cast<std::uint16_t>(port - kBase);
    if (offset == 0 && size >= 2) {
        status_ = static_cast<std::uint16_t>(status_ & ~static_cast<std::uint16_t>(value)); // Write-1-to-clear.
        if (size == 4) enable_ = static_cast<std::uint16_t>(value >> 16);
        UpdateSci();
        return;
    }
    if (offset == 4 && size >= 2) {
        const auto control = static_cast<std::uint16_t>(value);
        control_ = static_cast<std::uint16_t>((control & ~0x2000u) | 0x0001u);
        if ((control & 0x2000) != 0) {
            const auto sleepType = static_cast<std::uint8_t>((control >> 10) & 0x07);
            VMM_INFO("ACPI: entrée en veille demandée (SLP_TYP=%u).", sleepType);
            if (sleepType == kS5SleepType && powerOff_) powerOff_();
        }
        return;
    }
    for (std::uint8_t index = 0; index < size; ++index) {
        WriteByte(static_cast<std::uint16_t>(offset + index), static_cast<std::uint8_t>(value >> (index * 8)));
    }
}

std::uint8_t AcpiPm::ReadByte(std::uint16_t offset)
{
    switch (offset) {
    case 0: return static_cast<std::uint8_t>(status_);
    case 1: return static_cast<std::uint8_t>(status_ >> 8);
    case 2: return static_cast<std::uint8_t>(enable_);
    case 3: return static_cast<std::uint8_t>(enable_ >> 8);
    case 4: return static_cast<std::uint8_t>(control_);
    case 5: return static_cast<std::uint8_t>(control_ >> 8);
    default: return 0xFF;
    }
}

void AcpiPm::WriteByte(std::uint16_t offset, std::uint8_t value)
{
    switch (offset) {
    case 0: status_ = static_cast<std::uint16_t>(status_ & ~static_cast<std::uint16_t>(value)); break;
    case 1: status_ = static_cast<std::uint16_t>(status_ & ~(static_cast<std::uint16_t>(value) << 8)); break;
    case 2: enable_ = static_cast<std::uint16_t>((enable_ & 0xFF00) | value); break;
    case 3: enable_ = static_cast<std::uint16_t>((enable_ & 0x00FF) | (value << 8)); break;
    case 4: control_ = static_cast<std::uint16_t>((control_ & 0xFF00) | value | 1); break;
    case 5: {
        const auto high = static_cast<std::uint16_t>(value << 8);
        if ((high & 0x2000) != 0) {
            const auto sleepType = static_cast<std::uint8_t>((high >> 10) & 0x07);
            if (sleepType == kS5SleepType && powerOff_) powerOff_();
        }
        control_ = static_cast<std::uint16_t>((control_ & 0x00FF) | (high & ~0x2000u));
        break;
    }
    default: break;
    }
    UpdateSci();
}

bool AcpiPm::PressPowerButton()
{
    status_ |= 0x0100; // PWRBTN_STS
    UpdateSci();
    return (enable_ & 0x0100) != 0;
}

void AcpiPm::UpdateSci()
{
    const bool asserted = (status_ & enable_ & 0x0521) != 0; // TMR, GBL, PWRBTN, RTC.
    if (asserted != sciAsserted_) {
        sciAsserted_ = asserted;
        sink_.SetGsi(kSciGsi, asserted);
    }
}

} // namespace vmm
