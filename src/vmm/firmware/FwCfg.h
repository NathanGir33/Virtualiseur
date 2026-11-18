#pragma once

#include "../core/Bus.h"
#include "../core/GuestMemory.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace vmm {

// QEMU's firmware configuration interface (fw_cfg), the way OVMF finds the
// machine: memory map, CPU count, ACPI tables, boot order.
//
//   0x510 (16-bit write)  selector; the item's read cursor goes back to 0
//   0x511 (8-bit read)    item data, the cursor advancing; past the end: 0
//   0x514 - 0x51B         DMA: the guest writes the address of a
//                         FWCfgDmaAccess {be32 control, be32 length,
//                         be64 address} as a big-endian 64-bit value, high
//                         half at 0x514 then low half at 0x518 (which starts
//                         the transfer). Reads give the signature "QEMU CFG".
//
// DMA: control SELECT (0x08, selector in bits 16-31), then READ (0x02: item
// bytes copied to guest memory), SKIP (0x04) or WRITE (0x10: refused, items
// are read-only). Lengths are checked against the guest RAM, item payloads
// stay little-endian (as QEMU builds them); control is set to 0 (or to the
// ERROR bit 0x01) only once the copy is complete.
//
// Items: 0x00 signature "QEMU", 0x01 features (traditional + DMA), 0x02 UUID,
// 0x03 RAM size, 0x05 / 0x0F CPU count, 0x19 file directory (big-endian
// {count; {size, select, reserved, name[56]}...}), files from 0x20.
class FwCfg final : public IoDevice {
public:
    static constexpr std::uint16_t kSelectorPort = 0x510;
    static constexpr std::uint16_t kDataPort = 0x511;
    static constexpr std::uint16_t kDmaPort = 0x514; // 8 bytes.

    explicit FwCfg(const GuestMemory& memory) : memory_(memory) {}

    void AddItem(std::uint16_t selector, std::vector<std::uint8_t> data);
    void AddFile(const std::string& name, std::vector<std::uint8_t> data);
    // Builds the file directory: call once every file is added.
    void Finalize();

    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;

private:
    void Select(std::uint16_t selector);
    std::uint8_t NextByte();
    void RunDma(std::uint64_t address);

    const GuestMemory& memory_;
    std::map<std::uint16_t, std::vector<std::uint8_t>> items_;
    std::map<std::uint16_t, std::string> fileNames_;
    std::uint16_t nextFile_ = 0x20;
    std::uint16_t selector_ = 0;
    std::uint32_t offset_ = 0;
    std::uint8_t dmaAddress_[8]{};
    std::set<std::uint16_t> reported_; // Selectors already logged.
};

} // namespace vmm
