#include "Pci.h"

namespace vmm {

PciFunction::PciFunction(std::uint16_t vendor, std::uint16_t device, std::uint8_t classCode,
                         std::uint8_t subclass, std::uint8_t progIf, std::uint8_t revision)
{
    config_[0x00] = static_cast<std::uint8_t>(vendor);
    config_[0x01] = static_cast<std::uint8_t>(vendor >> 8);
    config_[0x02] = static_cast<std::uint8_t>(device);
    config_[0x03] = static_cast<std::uint8_t>(device >> 8);
    config_[0x08] = revision;
    config_[0x09] = progIf;
    config_[0x0A] = subclass;
    config_[0x0B] = classCode;
    // Command: I/O space, memory space, bus master, INTx disable.
    writeMask_[0x04] = 0x07;
    writeMask_[0x05] = 0x04;
    writeMask_[0x0C] = 0xFF; // Cache line size.
    writeMask_[0x0D] = 0xFF; // Latency timer.
    writeMask_[0x3C] = 0xFF; // Interrupt line.
}

void PciFunction::SetIoBar(std::uint32_t index, std::uint16_t base, std::uint16_t size, IoDevice* handler)
{
    auto& bar = bars_[index];
    bar.size = size;
    bar.io = handler;
    const std::uint32_t value = (static_cast<std::uint32_t>(base) & ~static_cast<std::uint32_t>(size - 1)) | 0x01;
    const std::uint32_t mask = 0xFFFFFFFCu & ~static_cast<std::uint32_t>(size - 1);
    const auto offset = 0x10 + index * 4;
    for (std::uint32_t byte = 0; byte < 4; ++byte) {
        config_[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
        writeMask_[offset + byte] = static_cast<std::uint8_t>(mask >> (byte * 8));
    }
    config_[0x04] |= 0x01; // Firmware leaves I/O decoding enabled.
    RemapBars();
}

void PciFunction::SetMmioBar(std::uint32_t index, std::uint32_t base, std::uint32_t size, MmioDevice* handler)
{
    auto& bar = bars_[index];
    bar.size = size;
    bar.mmio = handler;
    const std::uint32_t value = base & ~(size - 1); // 32-bit, non-prefetchable.
    const std::uint32_t mask = 0xFFFFFFF0u & ~(size - 1);
    const auto offset = 0x10 + index * 4;
    for (std::uint32_t byte = 0; byte < 4; ++byte) {
        config_[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
        writeMask_[offset + byte] = static_cast<std::uint8_t>(mask >> (byte * 8));
    }
    config_[0x04] |= 0x02; // Firmware leaves memory decoding enabled.
    RemapBars();
}

void PciFunction::SetMemoryBar(std::uint32_t index, std::uint32_t base, std::uint32_t size, bool prefetchable)
{
    const std::uint32_t value = (base & ~(size - 1)) | (prefetchable ? 0x08u : 0x00u);
    const std::uint32_t mask = 0xFFFFFFF0u & ~(size - 1);
    const auto offset = 0x10 + index * 4;
    for (std::uint32_t byte = 0; byte < 4; ++byte) {
        config_[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
        writeMask_[offset + byte] = static_cast<std::uint8_t>(mask >> (byte * 8));
    }
    config_[0x04] |= 0x02; // Firmware leaves memory decoding enabled.
}

void PciFunction::SetSubsystem(std::uint16_t vendor, std::uint16_t device)
{
    config_[0x2C] = static_cast<std::uint8_t>(vendor);
    config_[0x2D] = static_cast<std::uint8_t>(vendor >> 8);
    config_[0x2E] = static_cast<std::uint8_t>(device);
    config_[0x2F] = static_cast<std::uint8_t>(device >> 8);
}

void PciFunction::SetInterrupt(std::uint8_t line, std::uint8_t pin)
{
    config_[0x3C] = line;
    config_[0x3D] = pin;
}

std::uint32_t PciFunction::ConfigRead(std::uint32_t offset, std::uint8_t size)
{
    std::uint32_t value = 0;
    for (std::uint8_t index = 0; index < size && offset + index < config_.size(); ++index) {
        value |= static_cast<std::uint32_t>(config_[offset + index]) << (index * 8);
    }
    return value;
}

void PciFunction::ConfigWrite(std::uint32_t offset, std::uint8_t size, std::uint32_t value)
{
    bool barsTouched = false;
    for (std::uint8_t index = 0; index < size && offset + index < config_.size(); ++index) {
        const auto position = offset + index;
        const auto byte = static_cast<std::uint8_t>(value >> (index * 8));
        const auto mask = writeMask_[position];
        config_[position] = static_cast<std::uint8_t>((config_[position] & ~mask) | (byte & mask));
        if ((position >= 0x10 && position < 0x28) || position == 0x04) barsTouched = true;
    }
    if (barsTouched) RemapBars();
}

std::uint32_t PciFunction::BarAddress(std::uint32_t index) const noexcept
{
    const auto offset = 0x10 + index * 4;
    return static_cast<std::uint32_t>(config_[offset]) | (static_cast<std::uint32_t>(config_[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(config_[offset + 2]) << 16) | (static_cast<std::uint32_t>(config_[offset + 3]) << 24);
}

void PciFunction::RemapBars()
{
    const bool ioEnabled = (config_[0x04] & 0x01) != 0;
    const bool memoryEnabled = (config_[0x04] & 0x02) != 0;
    for (std::uint32_t index = 0; index < bars_.size(); ++index) {
        auto& bar = bars_[index];
        if (bar.io != nullptr && ioBus_ != nullptr) {
            const auto address = BarAddress(index) & 0xFFFFFFFCu;
            const bool shouldMap = ioEnabled && address != 0 && address <= 0x10000u - bar.size;
            if (bar.mapped && (!shouldMap || bar.mappedBase != address)) {
                ioBus_->Unregister(static_cast<std::uint16_t>(bar.mappedBase), bar.size, bar.io);
                bar.mapped = false;
            }
            if (shouldMap && !bar.mapped) {
                ioBus_->Register(static_cast<std::uint16_t>(address), bar.size, bar.io);
                bar.mappedBase = address;
                bar.mapped = true;
            }
        } else if (bar.mmio != nullptr && mmioBus_ != nullptr) {
            const auto address = BarAddress(index) & 0xFFFFFFF0u;
            // Sizing writes all ones: such a BAR wraps past 4 GiB and stays unmapped.
            const bool shouldMap = memoryEnabled && address != 0 &&
                                   static_cast<std::uint64_t>(address) + bar.size <= 0xFEC00000ull;
            if (bar.mapped && (!shouldMap || bar.mappedBase != address)) {
                mmioBus_->Unregister(bar.mappedBase, bar.mmio);
                bar.mapped = false;
                MmioBarChanged(index, 0, false);
            }
            if (shouldMap && !bar.mapped) {
                mmioBus_->Register(address, bar.size, bar.mmio);
                bar.mappedBase = address;
                bar.mapped = true;
                MmioBarChanged(index, address, true);
            }
        }
    }
}

void PciBus::Add(std::uint8_t slot, std::uint8_t function, PciFunction* device)
{
    functions_[static_cast<std::uint16_t>((slot << 3) | function)] = device;
}

PciFunction* PciBus::Selected() const
{
    if ((address_ & 0x80000000u) == 0 || ((address_ >> 16) & 0xFF) != 0) return nullptr;
    const auto key = static_cast<std::uint16_t>((address_ >> 8) & 0xFF);
    const auto found = functions_.find(key);
    return found == functions_.end() ? nullptr : found->second;
}

std::uint32_t PciBus::IoRead(std::uint16_t port, std::uint8_t size)
{
    if (port == 0xCF8 && size == 4) return address_;
    if (port == 0xCF9 && size == 1) return resetControl_;
    if (port >= 0xCF8 && port < 0xCFC) {
        return (address_ >> ((port - 0xCF8) * 8)) & (size == 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1));
    }
    auto* function = Selected();
    if (function == nullptr) return 0xFFFFFFFF;
    const auto offset = (address_ & 0xFC) + (port - 0xCFC);
    return function->ConfigRead(offset, size);
}

void PciBus::IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value)
{
    if (port == 0xCF8 && size == 4) {
        address_ = value;
        return;
    }
    if (port == 0xCF9 && size == 1) {
        resetControl_ = static_cast<std::uint8_t>(value & 0x0E);
        if ((value & 0x04) != 0 && reset_) reset_();
        return;
    }
    if (port < 0xCFC) return;
    auto* function = Selected();
    if (function == nullptr) return;
    const auto offset = (address_ & 0xFC) + (port - 0xCFC);
    function->ConfigWrite(offset, size, value);
}

} // namespace vmm
