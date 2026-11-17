#include "IoApic.h"

#include "../core/Log.h"

namespace vmm {

IoApic::IoApic(virtualiseur::whp::Partition& partition, std::uint32_t id) : partition_(partition), id_(id & 0x0F)
{
    for (auto& entry : redirection_) entry = 1ull << 16; // Masked at reset.
}

std::uint64_t IoApic::MmioRead(std::uint64_t address, std::uint8_t size)
{
    const std::lock_guard guard(mutex_);
    const auto offset = address & 0xFF;
    std::uint32_t value = 0xFFFFFFFF;
    if (offset == 0x00) value = select_;
    else if (offset == 0x10) value = ReadRegister(select_);
    else if (offset >= 0x10 && offset < 0x14) {
        value = ReadRegister(select_) >> ((offset - 0x10) * 8);
    }
    if (size >= 4) return value;
    return value & ((1u << (size * 8)) - 1u);
}

void IoApic::MmioWrite(std::uint64_t address, std::uint8_t size, std::uint64_t value)
{
    const auto offset = address & 0xFF;
    if (offset == 0x40) {
        EndOfInterrupt(static_cast<std::uint32_t>(value & 0xFF)); // IOAPIC v0x20 EOI register.
        return;
    }
    const std::lock_guard guard(mutex_);
    if (offset == 0x00) {
        select_ = static_cast<std::uint32_t>(value & 0xFF);
    } else if (offset == 0x10 && size >= 4) {
        WriteRegister(select_, static_cast<std::uint32_t>(value));
    }
}

std::uint32_t IoApic::ReadRegister(std::uint32_t index) const
{
    if (index == 0x00) return id_ << 24;
    if (index == 0x01) return ((kPins - 1) << 16) | 0x11; // Version 0x11, 24 entries.
    if (index == 0x02) return id_ << 24;
    if (index >= 0x10 && index < 0x10 + kPins * 2) {
        const auto pin = (index - 0x10) / 2;
        const auto entry = redirection_[pin];
        return (index & 1) != 0 ? static_cast<std::uint32_t>(entry >> 32)
                                : static_cast<std::uint32_t>(entry);
    }
    return 0;
}

void IoApic::WriteRegister(std::uint32_t index, std::uint32_t value)
{
    if (index == 0x00) {
        id_ = (value >> 24) & 0x0F;
        return;
    }
    if (index < 0x10 || index >= 0x10 + kPins * 2) return;
    const auto pin = (index - 0x10) / 2;
    auto& entry = redirection_[pin];
    if ((index & 1) != 0) {
        entry = (entry & 0x00000000FFFFFFFFull) | (static_cast<std::uint64_t>(value) << 32);
    } else {
        // Delivery status (bit 12) and Remote IRR (bit 14) are read-only.
        constexpr std::uint64_t readOnly = (1ull << 12) | (1ull << 14);
        entry = (entry & (0xFFFFFFFF00000000ull | readOnly)) | (value & ~static_cast<std::uint32_t>(readOnly));
        if (!LevelTriggered(entry)) entry &= ~(1ull << 14);
    }
    programmed_ = true;
    if (!Masked(entry)) Service(pin);
}

void IoApic::SetGsi(std::uint32_t gsi, bool asserted)
{
    if (gsi >= kPins) return;
    const std::lock_guard guard(mutex_);
    const bool previous = lines_[gsi];
    lines_[gsi] = asserted;
    const auto entry = redirection_[gsi];
    if (LevelTriggered(entry)) {
        if (asserted) Service(gsi);
        return;
    }
    if (asserted && !previous) {
        pendingEdge_[gsi] = true;
        Service(gsi);
    }
}

void IoApic::SignalMsi(std::uint64_t address, std::uint32_t data)
{
    // Intel SDM 10.11: address 0xFEExxxxx with the destination in bits 19:12
    // and the destination mode in bit 2; data holds the vector and the
    // delivery mode. MSIs are edge-triggered: no Remote IRR, no EOI exit.
    if ((address >> 20) != 0xFEE) {
        VMM_WARN("MSI: adresse 0x%llx hors de la fenêtre APIC, ignorée.", static_cast<unsigned long long>(address));
        return;
    }
    const auto vector = data & 0xFF;
    WHV_INTERRUPT_CONTROL control{};
    switch ((data >> 8) & 0x07) {
    case 0: control.Type = WHvX64InterruptTypeFixed; break;
    case 1: control.Type = WHvX64InterruptTypeLowestPriority; break;
    case 4: control.Type = WHvX64InterruptTypeNmi; break;
    default:
        VMM_WARN("MSI: mode de remise %u non pris en charge.", (data >> 8) & 0x07);
        return;
    }
    if (control.Type != WHvX64InterruptTypeNmi && vector < 0x10) return;
    control.DestinationMode = (address & 0x4) != 0
        ? WHvX64InterruptDestinationModeLogical : WHvX64InterruptDestinationModePhysical;
    control.TriggerMode = WHvX64InterruptTriggerModeEdge;
    control.Destination = static_cast<std::uint32_t>((address >> 12) & 0xFF);
    control.Vector = vector;
    try {
        partition_.RequestInterrupt(control);
        ++msiDelivered_;
    } catch (const std::exception& error) {
        VMM_ERROR("MSI: injection du vecteur 0x%x impossible: %s", vector, error.what());
    }
}

void IoApic::Service(std::uint32_t pin)
{
    auto& entry = redirection_[pin];
    if (Masked(entry)) return;
    if (LevelTriggered(entry)) {
        if (!lines_[pin] || RemoteIrr(entry)) return;
        entry |= 1ull << 14;
        Deliver(pin);
    } else if (pendingEdge_[pin]) {
        pendingEdge_[pin] = false;
        Deliver(pin);
    }
}

void IoApic::EndOfInterrupt(std::uint32_t vector)
{
    const std::lock_guard guard(mutex_);
    for (std::uint32_t pin = 0; pin < kPins; ++pin) {
        auto& entry = redirection_[pin];
        if (!LevelTriggered(entry) || !RemoteIrr(entry) || (entry & 0xFF) != vector) continue;
        entry &= ~(1ull << 14);
        Service(pin); // Re-deliver if the device still asserts its line.
    }
}

void IoApic::Deliver(std::uint32_t pin)
{
    const auto entry = redirection_[pin];
    const auto vector = static_cast<std::uint32_t>(entry & 0xFF);
    const auto deliveryMode = static_cast<std::uint32_t>((entry >> 8) & 0x07);
    WHV_INTERRUPT_CONTROL control{};
    switch (deliveryMode) {
    case 1: control.Type = WHvX64InterruptTypeLowestPriority; break;
    case 4: control.Type = WHvX64InterruptTypeNmi; break;
    default: control.Type = WHvX64InterruptTypeFixed; break;
    }
    if (control.Type != WHvX64InterruptTypeNmi && vector < 0x10) {
        VMM_WARN("IOAPIC: vecteur invalide 0x%x sur la broche %u ignoré.", vector, pin);
        return;
    }
    control.DestinationMode = (entry & (1ull << 11)) != 0
        ? WHvX64InterruptDestinationModeLogical : WHvX64InterruptDestinationModePhysical;
    control.TriggerMode = LevelTriggered(entry)
        ? WHvX64InterruptTriggerModeLevel : WHvX64InterruptTriggerModeEdge;
    control.Destination = static_cast<std::uint32_t>(entry >> 56);
    control.Vector = vector;
    try {
        partition_.RequestInterrupt(control);
        ++delivered_;
    } catch (const std::exception& error) {
        VMM_ERROR("IOAPIC: injection de la broche %u impossible: %s", pin, error.what());
    }
}

} // namespace vmm
