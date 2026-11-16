#pragma once

#include <array>
#include <cstdint>
#include <map>

namespace virtualiseur::whp {

// Minimal PCI configuration-mechanism-1 bus with an i440FX host bridge and
// PIIX3-compatible ISA/IDE functions for a legacy PC guest.
class PciConfigMechanism1 final {
public:
    PciConfigMechanism1();
    bool HandlesPort(std::uint16_t port) const noexcept;
    std::uint32_t ReadPort(std::uint16_t port, std::uint8_t accessSize) const;
    void WritePort(std::uint16_t port, std::uint8_t accessSize, std::uint32_t value);

private:
    using ConfigSpace = std::array<std::uint8_t, 256>;

    static void Set16(ConfigSpace& config, std::size_t offset, std::uint16_t value);
    static void Set32(ConfigSpace& config, std::size_t offset, std::uint32_t value);
    ConfigSpace* FindFunction(std::uint32_t address);
    const ConfigSpace* FindFunction(std::uint32_t address) const;
    std::uint8_t ReadConfigByte(std::uint32_t offset) const;
    void WriteConfigByte(std::uint32_t offset, std::uint8_t value);

    std::map<std::uint8_t, ConfigSpace> functions_;
    std::uint32_t configAddress_ = 0;
    bool ideBar4Sizing_ = false;
    bool virtioNetBarSizing_ = false;
};

} // namespace virtualiseur::whp
