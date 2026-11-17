#pragma once

#include "../core/Bus.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>

namespace vmm {

// Type-0 PCI function with a 256-byte configuration space. Decoded BARs (I/O
// BARs and MMIO BARs) are re-registered on their bus whenever the guest moves
// them or toggles decoding in the command register.
class PciFunction {
public:
    PciFunction(std::uint16_t vendor, std::uint16_t device, std::uint8_t classCode,
                std::uint8_t subclass, std::uint8_t progIf, std::uint8_t revision);
    virtual ~PciFunction() = default;

    void AttachIoBus(IoBus* bus) noexcept { ioBus_ = bus; }
    void AttachMmioBus(MmioBus* bus) noexcept { mmioBus_ = bus; }
    // Lock taken around BAR accesses (default: the VM-wide lock). Set before
    // declaring the BARs.
    void SetBarLock(DeviceLock* lock) noexcept { barLock_ = lock; }

    // Declares an I/O BAR decoded by `handler`. `size` is a power of two >= 4.
    void SetIoBar(std::uint32_t index, std::uint16_t base, std::uint16_t size, IoDevice* handler);
    // Declares a 32-bit memory BAR decoded by `handler` (MMIO exits). `size`
    // is a power of two >= 4 KiB.
    void SetMmioBar(std::uint32_t index, std::uint32_t base, std::uint32_t size, MmioDevice* handler);
    // Declares a 32-bit memory BAR backed by guest RAM mapped at a fixed
    // address (no decoding by the VMM). `size` is a power of two >= 16.
    void SetMemoryBar(std::uint32_t index, std::uint32_t base, std::uint32_t size, bool prefetchable);
    void SetSubsystem(std::uint16_t vendor, std::uint16_t device);
    void SetInterrupt(std::uint8_t line, std::uint8_t pin);
    void SetMultiFunction() noexcept { config_[0x0E] |= 0x80; }

    virtual std::uint32_t ConfigRead(std::uint32_t offset, std::uint8_t size);
    virtual void ConfigWrite(std::uint32_t offset, std::uint8_t size, std::uint32_t value);

    std::uint16_t Command() const noexcept { return static_cast<std::uint16_t>(config_[4] | (config_[5] << 8)); }
    bool BusMasterEnabled() const noexcept { return (Command() & 0x04) != 0; }

protected:
    // Called after an MMIO BAR is mapped at `base` or unmapped.
    virtual void MmioBarChanged(std::uint32_t /*index*/, std::uint32_t /*base*/, bool /*mapped*/) {}
    // Current guest-physical base of an MMIO BAR, 0 when not decoded.
    std::uint32_t MmioBarBase(std::uint32_t index) const noexcept
    {
        return bars_[index].mapped ? bars_[index].mappedBase : 0;
    }

    std::array<std::uint8_t, 256> config_{};
    std::array<std::uint8_t, 256> writeMask_{};

private:
    struct Bar {
        std::uint32_t size = 0;
        std::uint32_t mappedBase = 0;
        bool mapped = false;
        IoDevice* io = nullptr;
        MmioDevice* mmio = nullptr;
    };
    void RemapBars();
    std::uint32_t BarAddress(std::uint32_t index) const noexcept;

    std::array<Bar, 6> bars_{};
    IoBus* ioBus_ = nullptr;
    MmioBus* mmioBus_ = nullptr;
    DeviceLock* barLock_ = nullptr;
};

// PCI configuration mechanism #1 (ports 0xCF8-0xCFF) on bus 0, plus the
// PIIX reset control register at 0xCF9.
class PciBus final : public IoDevice {
public:
    explicit PciBus(std::function<void()> resetRequest) : reset_(std::move(resetRequest)) {}

    void Add(std::uint8_t slot, std::uint8_t function, PciFunction* device);

    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;

private:
    PciFunction* Selected() const;

    std::map<std::uint16_t, PciFunction*> functions_;
    std::uint32_t address_ = 0;
    std::uint8_t resetControl_ = 0;
    std::function<void()> reset_;
};

} // namespace vmm
