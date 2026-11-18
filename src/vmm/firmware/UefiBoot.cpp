#include "UefiBoot.h"

#include "../core/Log.h"

#include <cstring>

namespace vmm {
namespace {

void PutLittleEndian(std::vector<std::uint8_t>& out, std::uint64_t value, int bytes)
{
    for (int index = 0; index < bytes; ++index) out.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

} // namespace

void SetupFirmwareResetCpu(virtualiseur::whp::Partition& partition, std::uint32_t index)
{
    const auto segment = [](std::uint16_t selector, std::uint64_t base, std::uint16_t attributes) {
        WHV_X64_SEGMENT_REGISTER value{};
        value.Base = base;
        value.Limit = 0xFFFF;
        value.Selector = selector;
        value.Attributes = attributes;
        return value;
    };
    const std::array<WHV_REGISTER_NAME, 19> names{
        WHvX64RegisterCs,   WHvX64RegisterDs,   WHvX64RegisterEs,   WHvX64RegisterSs,  WHvX64RegisterFs,
        WHvX64RegisterGs,   WHvX64RegisterLdtr, WHvX64RegisterTr,   WHvX64RegisterGdtr, WHvX64RegisterIdtr,
        WHvX64RegisterRip,  WHvX64RegisterRflags, WHvX64RegisterCr0, WHvX64RegisterCr3, WHvX64RegisterCr4,
        WHvX64RegisterEfer, WHvX64RegisterRdx,  WHvX64RegisterRsp,  WHvX64RegisterRax,
    };
    std::array<WHV_REGISTER_VALUE, names.size()> values{};
    values[0].Segment = segment(0xF000, 0xFFFF0000, 0x009B); // Code: present, readable, accessed.
    for (int data = 1; data <= 5; ++data) values[data].Segment = segment(0, 0, 0x0093);
    values[6].Segment = segment(0, 0, 0x0082); // LDT.
    values[7].Segment = segment(0, 0, 0x008B); // Busy TSS.
    values[8].Table.Base = 0;
    values[8].Table.Limit = 0xFFFF;
    values[9].Table.Base = 0;
    values[9].Table.Limit = 0xFFFF;
    values[10].Reg64 = 0xFFF0;
    values[11].Reg64 = 0x2;
    values[12].Reg64 = 0x60000010; // CD | NW | ET.
    values[13].Reg64 = 0;
    values[14].Reg64 = 0;
    values[15].Reg64 = 0;
    values[16].Reg64 = 0x600;      // EDX: processor signature (family 6), as after a reset.
    values[17].Reg64 = 0;
    values[18].Reg64 = 0;
    partition.SetVirtualProcessorRegisters(index, names, values);
}

void ConfigureFwCfg(FwCfg& fwCfg, const GuestMemory& memory, std::uint32_t cpus, const std::array<std::uint8_t, 16>& uuid)
{
    fwCfg.AddItem(0x00, {'Q', 'E', 'M', 'U'});
    fwCfg.AddItem(0x01, {0x03, 0x00, 0x00, 0x00}); // Traditional interface + DMA.
    fwCfg.AddItem(0x02, std::vector<std::uint8_t>(uuid.begin(), uuid.end()));
    std::vector<std::uint8_t> ram;
    PutLittleEndian(ram, memory.RamBytes(), 8);
    fwCfg.AddItem(0x03, std::move(ram));
    std::vector<std::uint8_t> cpuCount;
    PutLittleEndian(cpuCount, cpus, 2);
    fwCfg.AddItem(0x05, cpuCount); // Boot CPUs.
    fwCfg.AddItem(0x0F, cpuCount); // Maximum CPUs (no hot-plug).

    // e820: {u64 address, u64 length, u32 type}, packed, little-endian.
    std::vector<std::uint8_t> e820;
    const auto add = [&](std::uint64_t address, std::uint64_t length, std::uint32_t type) {
        PutLittleEndian(e820, address, 8);
        PutLittleEndian(e820, length, 8);
        PutLittleEndian(e820, type, 4);
    };
    add(0, memory.LowRamEnd(), 1);
    if (memory.HighRamBytes() != 0) add(layout::kFourGiB, memory.HighRamBytes(), 1);
    fwCfg.AddFile("etc/e820", std::move(e820));
    fwCfg.Finalize();
}

std::uint32_t FirmwareDebugPort::IoRead(std::uint16_t, std::uint8_t)
{
    return 0xE9; // "Debug console present": OVMF then prints its log.
}

void FirmwareDebugPort::IoWrite(std::uint16_t, std::uint8_t size, std::uint32_t value)
{
    for (std::uint8_t index = 0; index < size; ++index) {
        const auto character = static_cast<char>(value >> (index * 8));
        if (character == '\n') {
            if (!line_.empty() && line_.back() == '\r') line_.pop_back();
            VMM_INFO("OVMF : %s", line_.c_str());
            line_.clear();
        } else if (line_.size() < 1024) {
            line_.push_back(character);
        }
    }
}

} // namespace vmm
