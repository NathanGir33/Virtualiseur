#include "PciConfigMechanism1.h"

#include <stdexcept>

namespace virtualiseur::whp {
namespace {

constexpr std::uint16_t kIntelVendor = 0x8086;
constexpr std::uint16_t kI440FxHostBridge = 0x1237;
constexpr std::uint16_t kPiix3IsaBridge = 0x7000;
constexpr std::uint16_t kPiix3IdeController = 0x7010;
constexpr std::uint16_t kVirtioVendor = 0x1AF4;
constexpr std::uint16_t kVirtioNetLegacy = 0x1000;

std::uint8_t FunctionKey(std::uint8_t device, std::uint8_t function)
{
    return static_cast<std::uint8_t>((device << 3) | function);
}

} // namespace

PciConfigMechanism1::PciConfigMechanism1()
{
    auto& host = functions_[FunctionKey(0, 0)];
    Set16(host, 0x00, kIntelVendor);
    Set16(host, 0x02, kI440FxHostBridge);
    Set16(host, 0x04, 0x0007); // I/O, memory, and bus mastering enabled.
    host[0x08] = 0x02;
    host[0x0A] = 0x00;
    host[0x0B] = 0x06; // Host bridge.
    host[0x0E] = 0x00;
    host[0x3D] = 0x00;

    auto& isa = functions_[FunctionKey(1, 0)];
    Set16(isa, 0x00, kIntelVendor);
    Set16(isa, 0x02, kPiix3IsaBridge);
    Set16(isa, 0x04, 0x0007);
    isa[0x08] = 0x00;
    isa[0x0A] = 0x01;
    isa[0x0B] = 0x06; // ISA bridge.
    // PIIX3 exposes the IDE controller as function 1 of this PCI device.
    // Without the multifunction bit, PCI enumeration stops at function 0,
    // leaving both IDE channels invisible to Linux.
    isa[0x0E] = 0x80;

    auto& ide = functions_[FunctionKey(1, 1)];
    Set16(ide, 0x00, kIntelVendor);
    Set16(ide, 0x02, kPiix3IdeController);
    Set16(ide, 0x04, 0x0005); // I/O space and bus mastering enabled.
    ide[0x08] = 0x00;
    ide[0x09] = 0x8A; // Compatibility mode, programmable timing, bus-master IDE capable.
    ide[0x0A] = 0x01;
    ide[0x0B] = 0x01; // IDE controller.
    ide[0x0E] = 0x00;
    // PIIX requires both channel-enable bits set in config bytes 41h/43h.
    // ata_piix checks these before resetting or probing each channel.
    ide[0x41] = 0x80; // Primary IDE channel enabled.
    ide[0x43] = 0x80; // Secondary IDE channel enabled.
    Set32(ide, 0x10, 0x000001F1);
    Set32(ide, 0x14, 0x000003F5);
    Set32(ide, 0x18, 0x00000171);
    Set32(ide, 0x1C, 0x00000375);
    Set32(ide, 0x20, 0x0000C001); // 16-byte I/O BAR4 for the bus-master IDE engine.
    ide[0x3C] = 0xFF; // Legacy IRQ routing is handled by the PIC model.
    ide[0x3D] = 0x01;

    // Transitional VirtIO network device (legacy PCI interface). Debian's
    // virtio_net driver supports this ID without an additional driver disk.
    auto& net = functions_[FunctionKey(3, 0)];
    Set16(net, 0x00, kVirtioVendor);
    Set16(net, 0x02, kVirtioNetLegacy);
    Set16(net, 0x04, 0x0005); // I/O space and bus mastering enabled.
    net[0x08] = 0x01;
    net[0x09] = 0x00;
    net[0x0A] = 0x00;
    net[0x0B] = 0x02; // Ethernet controller.
    net[0x0E] = 0x00;
    Set32(net, 0x10, 0x0000C101); // 256-byte legacy VirtIO I/O BAR.
    Set16(net, 0x2C, kVirtioVendor);
    Set16(net, 0x2E, kVirtioNetLegacy);
    net[0x3C] = 11;
    net[0x3D] = 1;
}

bool PciConfigMechanism1::HandlesPort(std::uint16_t port) const noexcept
{
    return (port >= 0x0CF8 && port <= 0x0CFB) ||
           (port >= 0x0CFC && port <= 0x0CFF);
}

std::uint32_t PciConfigMechanism1::ReadPort(std::uint16_t port, std::uint8_t accessSize) const
{
    if (!HandlesPort(port) || (accessSize != 1 && accessSize != 2 && accessSize != 4) ||
        static_cast<std::uint32_t>(port & 3) + accessSize > 4) {
        throw std::invalid_argument("Accès PCI configuration mechanism 1 invalide.");
    }
    std::uint32_t result = 0;
    for (std::uint8_t index = 0; index < accessSize; ++index) {
        const auto currentPort = static_cast<std::uint16_t>(port + index);
        std::uint8_t value = 0xFF;
        if (currentPort >= 0x0CF8 && currentPort <= 0x0CFB) {
            value = static_cast<std::uint8_t>(configAddress_ >> ((currentPort - 0x0CF8) * 8));
        } else if ((configAddress_ & 0x80000000u) != 0) {
            value = ReadConfigByte((configAddress_ & 0xFCu) + (currentPort - 0x0CFC));
        }
        result |= static_cast<std::uint32_t>(value) << (index * 8);
    }
    return result;
}

void PciConfigMechanism1::WritePort(
    std::uint16_t port,
    std::uint8_t accessSize,
    std::uint32_t value)
{
    if (!HandlesPort(port) || (accessSize != 1 && accessSize != 2 && accessSize != 4) ||
        static_cast<std::uint32_t>(port & 3) + accessSize > 4) {
        throw std::invalid_argument("Accès PCI configuration mechanism 1 invalide.");
    }
    for (std::uint8_t index = 0; index < accessSize; ++index) {
        const auto currentPort = static_cast<std::uint16_t>(port + index);
        const auto byteValue = static_cast<std::uint8_t>(value >> (index * 8));
        if (currentPort >= 0x0CF8 && currentPort <= 0x0CFB) {
            const auto shift = (currentPort - 0x0CF8) * 8;
            configAddress_ = (configAddress_ & ~(0xFFu << shift)) |
                             (static_cast<std::uint32_t>(byteValue) << shift);
        } else if ((configAddress_ & 0x80000000u) != 0) {
            WriteConfigByte((configAddress_ & 0xFCu) + (currentPort - 0x0CFC), byteValue);
        }
    }
}

void PciConfigMechanism1::Set16(ConfigSpace& config, std::size_t offset, std::uint16_t value)
{
    config[offset] = static_cast<std::uint8_t>(value);
    config[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void PciConfigMechanism1::Set32(ConfigSpace& config, std::size_t offset, std::uint32_t value)
{
    config[offset] = static_cast<std::uint8_t>(value);
    config[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    config[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    config[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

PciConfigMechanism1::ConfigSpace* PciConfigMechanism1::FindFunction(std::uint32_t address)
{
    if (((address >> 16) & 0xFF) != 0) return nullptr; // Bus 0 only.
    const auto device = static_cast<std::uint8_t>((address >> 11) & 0x1F);
    const auto function = static_cast<std::uint8_t>((address >> 8) & 0x07);
    const auto found = functions_.find(FunctionKey(device, function));
    return found == functions_.end() ? nullptr : &found->second;
}

const PciConfigMechanism1::ConfigSpace* PciConfigMechanism1::FindFunction(std::uint32_t address) const
{
    if (((address >> 16) & 0xFF) != 0) return nullptr;
    const auto device = static_cast<std::uint8_t>((address >> 11) & 0x1F);
    const auto function = static_cast<std::uint8_t>((address >> 8) & 0x07);
    const auto found = functions_.find(FunctionKey(device, function));
    return found == functions_.end() ? nullptr : &found->second;
}

std::uint8_t PciConfigMechanism1::ReadConfigByte(std::uint32_t offset) const
{
    const auto* function = FindFunction(configAddress_);
    const auto functionKey = FunctionKey(
        static_cast<std::uint8_t>((configAddress_ >> 11) & 0x1F),
        static_cast<std::uint8_t>((configAddress_ >> 8) & 0x07));
    if (functionKey == FunctionKey(1, 1) && ideBar4Sizing_ &&
        offset >= 0x20 && offset <= 0x23) {
        constexpr std::array<std::uint8_t, 4> bar4Mask{0xF1, 0xFF, 0xFF, 0xFF};
        return bar4Mask[offset - 0x20];
    }
    if (functionKey == FunctionKey(3, 0) && virtioNetBarSizing_ &&
        offset >= 0x10 && offset <= 0x13) {
        constexpr std::array<std::uint8_t, 4> bar0Mask{0x01, 0xFF, 0xFF, 0xFF};
        return bar0Mask[offset - 0x10];
    }
    return function != nullptr && offset < function->size() ? (*function)[offset] : 0xFF;
}

void PciConfigMechanism1::WriteConfigByte(std::uint32_t offset, std::uint8_t value)
{
    auto* function = FindFunction(configAddress_);
    if (function == nullptr || offset >= function->size()) return;

    const auto functionKey = FunctionKey(
        static_cast<std::uint8_t>((configAddress_ >> 11) & 0x1F),
        static_cast<std::uint8_t>((configAddress_ >> 8) & 0x07));
    if (functionKey == FunctionKey(1, 1) && offset >= 0x20 && offset <= 0x23) {
        if (value == 0xFF) {
            ideBar4Sizing_ = true;
            return;
        }
        ideBar4Sizing_ = false;
        if (offset == 0x20) value = static_cast<std::uint8_t>((value & 0xF0) | 0x01);
        (*function)[offset] = value;
        return;
    }
    if (functionKey == FunctionKey(3, 0) && offset >= 0x10 && offset <= 0x13) {
        if (value == 0xFF) {
            virtioNetBarSizing_ = true;
            return;
        }
        virtioNetBarSizing_ = false;
        if (offset == 0x10) value = static_cast<std::uint8_t>((value & 0xFC) | 0x01);
        (*function)[offset] = value;
        return;
    }

    // Keep vendor/device IDs and the class/header fields read-only. BAR and
    // command writes remain visible to PCI enumeration and driver probing.
    if ((offset <= 0x03) || (offset >= 0x08 && offset <= 0x0B) || offset == 0x0E) return;
    (*function)[offset] = value;
}

} // namespace virtualiseur::whp
