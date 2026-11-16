#include "IdeController.h"

#include "../core/Log.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace vmm {
namespace {

constexpr std::uint8_t kStatusErr = 0x01;
constexpr std::uint8_t kStatusDrq = 0x08;
constexpr std::uint8_t kStatusDsc = 0x10;
constexpr std::uint8_t kStatusDrdy = 0x40;
constexpr std::uint8_t kStatusBsy = 0x80;
constexpr std::uint8_t kReady = kStatusDrdy | kStatusDsc;
constexpr std::size_t kSectorSize = 512;
constexpr std::size_t kCdBlockSize = 2048;

std::uint16_t Be16(const std::uint8_t* data) noexcept
{
    return static_cast<std::uint16_t>((data[0] << 8) | data[1]);
}

std::uint32_t Be32(const std::uint8_t* data) noexcept
{
    return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | data[3];
}

void PutBe16(std::uint8_t* data, std::uint32_t value) noexcept
{
    data[0] = static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value);
}

void PutBe32(std::uint8_t* data, std::uint32_t value) noexcept
{
    data[0] = static_cast<std::uint8_t>(value >> 24);
    data[1] = static_cast<std::uint8_t>(value >> 16);
    data[2] = static_cast<std::uint8_t>(value >> 8);
    data[3] = static_cast<std::uint8_t>(value);
}

void PutAddress(std::uint8_t* data, std::uint32_t lba, bool msf) noexcept
{
    if (!msf) {
        PutBe32(data, lba);
        return;
    }
    const auto frames = lba + 150;
    data[0] = 0;
    data[1] = static_cast<std::uint8_t>(frames / (60 * 75));
    data[2] = static_cast<std::uint8_t>((frames / 75) % 60);
    data[3] = static_cast<std::uint8_t>(frames % 75);
}

void PutAtaString(std::uint8_t* identify, std::size_t firstWord, std::size_t words, const char* text)
{
    const std::size_t length = std::strlen(text);
    for (std::size_t index = 0; index < words * 2; ++index) {
        const char character = index < length ? text[index] : ' ';
        identify[firstWord * 2 + (index ^ 1)] = static_cast<std::uint8_t>(character);
    }
}

void PutWord(std::uint8_t* identify, std::size_t word, std::uint16_t value) noexcept
{
    identify[word * 2] = static_cast<std::uint8_t>(value);
    identify[word * 2 + 1] = static_cast<std::uint8_t>(value >> 8);
}

} // namespace

IdeController::IdeController(const GuestMemory& memory, InterruptSink& sink, IoBus& ioBus)
    : PciFunction(0x8086, 0x7010, 0x01, 0x01, 0x80, 0x00), memory_(memory), sink_(sink)
{
    AttachIoBus(&ioBus);
    // Both channels enabled in the PIIX IDE timing registers.
    config_[0x41] = 0x80;
    config_[0x43] = 0x80;
    for (std::uint32_t offset = 0x40; offset < 0x48; ++offset) writeMask_[offset] = 0xFF;
    config_[0x04] = 0x05; // I/O decoding and bus mastering.
    SetIoBar(4, kBusMasterBase, 16, this);
    channels_[0].irq = 14;
    channels_[1].irq = 15;
    ioBus.Register(0x1F0, 8, this);
    ioBus.Register(0x3F6, 1, this);
    ioBus.Register(0x170, 8, this);
    ioBus.Register(0x376, 1, this);
}

void IdeController::AttachDisk(std::uint32_t channel, virtualiseur::whp::VirtualDisk* disk)
{
    auto& target = channels_[channel];
    target.kind = Kind::Disk;
    target.disk = disk;
    target.bmStatus |= 0x20; // Drive 0 DMA capable.
    SetSignature(target);
    target.status = kReady;
}

void IdeController::AttachCdrom(std::uint32_t channel, virtualiseur::whp::Iso9660Image* image)
{
    auto& target = channels_[channel];
    target.kind = Kind::Cdrom;
    target.iso = image;
    target.ejected = image == nullptr;
    target.bmStatus |= 0x20;
    SetSignature(target);
    target.status = 0;
}

bool IdeController::CdromEjected() const noexcept
{
    for (const auto& channel : channels_) {
        if (channel.kind == Kind::Cdrom) return channel.ejected;
    }
    return true;
}

// --- Port dispatch -------------------------------------------------------------

std::uint32_t IdeController::IoRead(std::uint16_t port, std::uint8_t size)
{
    if (port >= 0x1F0 && port <= 0x1F7) return ChannelRead(channels_[0], port - 0x1F0, size);
    if (port >= 0x170 && port <= 0x177) return ChannelRead(channels_[1], port - 0x170, size);
    if (port == 0x3F6 || port == 0x376) {
        auto& channel = channels_[port == 0x3F6 ? 0 : 1];
        if (channel.kind == Kind::None) return 0xFF;
        if ((channel.select & 0x10) != 0) return 0x00;
        return channel.status;
    }
    const auto barBase = static_cast<std::uint16_t>((config_[0x20] | (config_[0x21] << 8)) & 0xFFFC);
    const auto offset = static_cast<std::uint32_t>(port - barBase);
    std::uint32_t value = 0;
    for (std::uint8_t index = 0; index < size; ++index) {
        value |= static_cast<std::uint32_t>(BusMasterRead(offset + index)) << (index * 8);
    }
    return value;
}

void IdeController::IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value)
{
    if (port >= 0x1F0 && port <= 0x1F7) return ChannelWrite(channels_[0], port - 0x1F0, size, value);
    if (port >= 0x170 && port <= 0x177) return ChannelWrite(channels_[1], port - 0x170, size, value);
    if (port == 0x3F6 || port == 0x376) return ControlWrite(channels_[port == 0x3F6 ? 0 : 1], static_cast<std::uint8_t>(value));
    const auto barBase = static_cast<std::uint16_t>((config_[0x20] | (config_[0x21] << 8)) & 0xFFFC);
    const auto offset = static_cast<std::uint32_t>(port - barBase);
    for (std::uint8_t index = 0; index < size; ++index) {
        BusMasterWrite(offset + index, static_cast<std::uint8_t>(value >> (index * 8)));
    }
}

std::uint8_t IdeController::BusMasterRead(std::uint32_t offset) const
{
    if (offset >= 16) return 0xFF;
    const auto& channel = channels_[offset / 8];
    switch (offset % 8) {
    case 0: return channel.bmCommand;
    case 2: return channel.bmStatus;
    case 4: case 5: case 6: case 7:
        return static_cast<std::uint8_t>(channel.bmPrd >> ((offset % 8 - 4) * 8));
    default: return 0;
    }
}

void IdeController::BusMasterWrite(std::uint32_t offset, std::uint8_t value)
{
    if (offset >= 16) return;
    auto& channel = channels_[offset / 8];
    switch (offset % 8) {
    case 0: {
        const bool wasActive = (channel.bmCommand & 0x01) != 0;
        channel.bmCommand = static_cast<std::uint8_t>(value & 0x09);
        if ((value & 0x01) != 0) {
            channel.bmStatus |= 0x01;
            if (!wasActive) TryStartDma(channel);
        } else {
            channel.bmStatus &= static_cast<std::uint8_t>(~0x01);
        }
        break;
    }
    case 2:
        channel.bmStatus = static_cast<std::uint8_t>(
            (channel.bmStatus & ~(value & 0x06) & ~0x60) | (value & 0x60));
        break;
    case 4: case 5: case 6: case 7: {
        const auto shift = (offset % 8 - 4) * 8;
        channel.bmPrd = (channel.bmPrd & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(value) << shift);
        channel.bmPrd &= ~3u;
        break;
    }
    default: break;
    }
}

std::uint32_t IdeController::ChannelRead(Channel& channel, std::uint32_t reg, std::uint8_t size)
{
    if (channel.kind == Kind::None) return size == 4 ? 0xFFFFFFFFu : (size == 2 ? 0xFFFFu : 0xFFu);
    const bool slave = (channel.select & 0x10) != 0;
    const bool hob = (channel.control & 0x80) != 0;
    switch (reg) {
    case 0: {
        if (slave) return 0xFFFF;
        const bool dataIn = channel.transfer == Transfer::PioIn || channel.transfer == Transfer::AtapiPioIn;
        if (!dataIn || (channel.status & kStatusDrq) == 0) return 0xFFFF;
        std::uint32_t value = 0;
        const std::size_t bytes = (std::min<std::size_t>)(size, channel.end - channel.position);
        std::memcpy(&value, channel.buffer.data() + channel.position, bytes);
        channel.position += bytes;
        if (channel.position >= channel.end) {
            if (channel.transfer == Transfer::PioIn) {
                FinishPioSector(channel);
            } else if (channel.position < channel.buffer.size()) {
                AtapiStartPioPhase(channel);
            } else {
                Complete(channel);
            }
        }
        return value;
    }
    case 1: return hob ? channel.hobFeature : channel.error;
    case 2: return hob ? channel.hobNsector : channel.nsector;
    case 3: return hob ? channel.hobLbaLow : channel.lbaLow;
    case 4: return hob ? channel.hobLbaMid : channel.lbaMid;
    case 5: return hob ? channel.hobLbaHigh : channel.lbaHigh;
    case 6: return channel.select;
    case 7:
        if (slave) return 0x00;
        LowerIrq(channel);
        return channel.status;
    default: return 0xFF;
    }
}

void IdeController::ChannelWrite(Channel& channel, std::uint32_t reg, std::uint8_t size, std::uint32_t value)
{
    const auto byte = static_cast<std::uint8_t>(value);
    if (reg >= 1 && reg <= 5) channel.control &= static_cast<std::uint8_t>(~0x80);
    switch (reg) {
    case 0: {
        if (channel.kind == Kind::None || (channel.select & 0x10) != 0) return;
        const bool dataOut = channel.transfer == Transfer::PioOut || channel.transfer == Transfer::Packet;
        if (!dataOut || (channel.status & kStatusDrq) == 0) return;
        const std::size_t bytes = (std::min<std::size_t>)(size, channel.end - channel.position);
        std::memcpy(channel.buffer.data() + channel.position, &value, bytes);
        channel.position += bytes;
        if (channel.position >= channel.end) {
            if (channel.transfer == Transfer::Packet) {
                std::memcpy(channel.packet.data(), channel.buffer.data(), 12);
                ExecutePacket(channel);
            } else {
                FinishPioSector(channel);
            }
        }
        return;
    }
    case 1: channel.hobFeature = channel.feature; channel.feature = byte; return;
    case 2: channel.hobNsector = channel.nsector; channel.nsector = byte; return;
    case 3: channel.hobLbaLow = channel.lbaLow; channel.lbaLow = byte; return;
    case 4: channel.hobLbaMid = channel.lbaMid; channel.lbaMid = byte; return;
    case 5: channel.hobLbaHigh = channel.lbaHigh; channel.lbaHigh = byte; return;
    case 6: channel.select = static_cast<std::uint8_t>(byte | 0xA0); return;
    case 7:
        if (channel.kind == Kind::None || (channel.select & 0x10) != 0) return;
        channel.error = 0;
        channel.transfer = Transfer::None;
        if (channel.kind == Kind::Disk) ExecuteAta(channel, byte);
        else ExecuteAtapiCommand(channel, byte);
        return;
    default: return;
    }
}

void IdeController::ControlWrite(Channel& channel, std::uint8_t value)
{
    const bool wasReset = (channel.control & 0x04) != 0;
    channel.control = value;
    if ((value & 0x02) != 0) LowerIrq(channel);
    const bool reset = (value & 0x04) != 0;
    if (channel.kind == Kind::None) return;
    if (reset && !wasReset) {
        channel.status = kStatusBsy;
        channel.transfer = Transfer::None;
    } else if (!reset && wasReset) {
        SetSignature(channel);
        channel.status = channel.kind == Kind::Disk ? kReady : 0x00;
        channel.bmCommand = 0;
        channel.bmStatus &= static_cast<std::uint8_t>(~0x07);
    }
}

// --- Helpers ---------------------------------------------------------------------

void IdeController::SetSignature(Channel& channel)
{
    channel.nsector = 1;
    channel.lbaLow = 1;
    if (channel.kind == Kind::Cdrom) {
        channel.lbaMid = 0x14;
        channel.lbaHigh = 0xEB;
    } else {
        channel.lbaMid = 0;
        channel.lbaHigh = 0;
    }
    channel.select = 0xA0;
    channel.error = 0x01;
}

void IdeController::RaiseIrq(Channel& channel)
{
    if ((channel.control & 0x02) != 0) return; // nIEN
    if (channel.irqLine) sink_.SetGsi(IsaIrqToGsi(channel.irq), false);
    channel.irqLine = true;
    sink_.SetGsi(IsaIrqToGsi(channel.irq), true);
}

void IdeController::LowerIrq(Channel& channel)
{
    if (!channel.irqLine) return;
    channel.irqLine = false;
    sink_.SetGsi(IsaIrqToGsi(channel.irq), false);
}

void IdeController::Abort(Channel& channel, std::uint8_t error)
{
    channel.error = error;
    channel.status = static_cast<std::uint8_t>(kReady | kStatusErr);
    channel.transfer = Transfer::None;
    if (channel.kind == Kind::Cdrom) channel.nsector = 3;
    RaiseIrq(channel);
}

void IdeController::Complete(Channel& channel)
{
    channel.transfer = Transfer::None;
    channel.status = kReady;
    if (channel.kind == Kind::Cdrom) {
        channel.nsector = 3; // I/O | C/D: command complete.
        channel.lbaMid = 0;
        channel.lbaHigh = 0;
    }
    RaiseIrq(channel);
}

std::uint64_t IdeController::TaskFileLba(const Channel& channel, bool lba48) const
{
    if (lba48) {
        return static_cast<std::uint64_t>(channel.lbaLow) |
               (static_cast<std::uint64_t>(channel.lbaMid) << 8) |
               (static_cast<std::uint64_t>(channel.lbaHigh) << 16) |
               (static_cast<std::uint64_t>(channel.hobLbaLow) << 24) |
               (static_cast<std::uint64_t>(channel.hobLbaMid) << 32) |
               (static_cast<std::uint64_t>(channel.hobLbaHigh) << 40);
    }
    return static_cast<std::uint64_t>(channel.lbaLow) |
           (static_cast<std::uint64_t>(channel.lbaMid) << 8) |
           (static_cast<std::uint64_t>(channel.lbaHigh) << 16) |
           (static_cast<std::uint64_t>(channel.select & 0x0F) << 24);
}

std::uint32_t IdeController::TaskFileCount(const Channel& channel, bool lba48) const
{
    if (lba48) {
        const auto count = static_cast<std::uint32_t>(channel.nsector | (channel.hobNsector << 8));
        return count == 0 ? 65536u : count;
    }
    return channel.nsector == 0 ? 256u : channel.nsector;
}

void IdeController::SetTaskFileLba(Channel& channel, std::uint64_t lba)
{
    channel.lbaLow = static_cast<std::uint8_t>(lba);
    channel.lbaMid = static_cast<std::uint8_t>(lba >> 8);
    channel.lbaHigh = static_cast<std::uint8_t>(lba >> 16);
    if (channel.lba48) {
        channel.hobLbaLow = static_cast<std::uint8_t>(lba >> 24);
        channel.hobLbaMid = static_cast<std::uint8_t>(lba >> 32);
        channel.hobLbaHigh = static_cast<std::uint8_t>(lba >> 40);
    } else {
        channel.select = static_cast<std::uint8_t>((channel.select & 0xF0) | ((lba >> 24) & 0x0F));
    }
}

// --- ATA disk ------------------------------------------------------------------------

void IdeController::BuildIdentify(Channel& channel)
{
    channel.buffer.assign(512, 0);
    auto* id = channel.buffer.data();
    const auto sectors = channel.disk->SectorCount();
    const auto chsSectors = (std::min<std::uint64_t>)(sectors, 16383ull * 16 * 63);
    PutWord(id, 0, 0x0040);
    PutWord(id, 1, static_cast<std::uint16_t>((std::max<std::uint64_t>)(1, chsSectors / (16 * 63))));
    PutWord(id, 3, 16);
    PutWord(id, 6, 63);
    PutAtaString(id, 10, 10, "VWHP-DISK-0001");
    PutAtaString(id, 23, 4, "1.0");
    PutAtaString(id, 27, 20, "VIRTUALISEUR HARDDISK");
    PutWord(id, 47, 0x8000);
    PutWord(id, 49, 0x0300); // DMA and LBA supported.
    PutWord(id, 51, 0x0200);
    PutWord(id, 53, 0x0006); // Words 64-70 and 88 valid.
    PutWord(id, 54, static_cast<std::uint16_t>(chsSectors / (16 * 63)));
    PutWord(id, 55, 16);
    PutWord(id, 56, 63);
    const auto lba28 = (std::min<std::uint64_t>)(sectors, 0x0FFFFFFF);
    PutWord(id, 57, static_cast<std::uint16_t>(chsSectors));
    PutWord(id, 58, static_cast<std::uint16_t>(chsSectors >> 16));
    PutWord(id, 60, static_cast<std::uint16_t>(lba28));
    PutWord(id, 61, static_cast<std::uint16_t>(lba28 >> 16));
    PutWord(id, 63, 0x0407); // MWDMA 0-2 supported, mode 2 selected.
    PutWord(id, 64, 0x0003); // PIO modes 3-4.
    PutWord(id, 65, 120);
    PutWord(id, 66, 120);
    PutWord(id, 67, 120);
    PutWord(id, 68, 120);
    PutWord(id, 80, 0x00F0); // ATA/ATAPI-4 to 7.
    PutWord(id, 81, 0x0016);
    PutWord(id, 82, 0x4000);
    PutWord(id, 83, 0x7400); // LBA48, FLUSH CACHE, FLUSH CACHE EXT.
    PutWord(id, 84, 0x4000);
    PutWord(id, 85, 0x4000);
    PutWord(id, 86, 0x3400);
    PutWord(id, 87, 0x4000);
    PutWord(id, 100, static_cast<std::uint16_t>(sectors));
    PutWord(id, 101, static_cast<std::uint16_t>(sectors >> 16));
    PutWord(id, 102, static_cast<std::uint16_t>(sectors >> 32));
    PutWord(id, 103, static_cast<std::uint16_t>(sectors >> 48));
}

void IdeController::LoadNextReadSector(Channel& channel)
{
    try {
        channel.buffer.resize(kSectorSize);
        channel.disk->ReadSectors(channel.lba, std::as_writable_bytes(std::span(channel.buffer)));
    } catch (const std::exception& error) {
        VMM_WARN("IDE: lecture LBA %llu impossible: %s", static_cast<unsigned long long>(channel.lba), error.what());
        Abort(channel, 0x10);
        return;
    }
    channel.position = 0;
    channel.end = kSectorSize;
    channel.transfer = Transfer::PioIn;
    channel.status = static_cast<std::uint8_t>(kReady | kStatusDrq);
    RaiseIrq(channel);
}

void IdeController::FinishPioSector(Channel& channel)
{
    if (channel.transfer == Transfer::PioIn && channel.end == 512 && channel.sectorsRemaining == 0) {
        // IDENTIFY data fully read.
        channel.transfer = Transfer::None;
        channel.status = kReady;
        return;
    }
    if (channel.transfer == Transfer::PioOut) {
        try {
            channel.disk->WriteSectors(channel.lba, std::as_bytes(std::span(channel.buffer.data(), kSectorSize)));
        } catch (const std::exception& error) {
            VMM_WARN("IDE: écriture LBA %llu impossible: %s", static_cast<unsigned long long>(channel.lba), error.what());
            Abort(channel, 0x10);
            return;
        }
    }
    SetTaskFileLba(channel, channel.lba);
    ++channel.lba;
    --channel.sectorsRemaining;
    if (channel.sectorsRemaining == 0) {
        const bool write = channel.transfer == Transfer::PioOut;
        channel.transfer = Transfer::None;
        channel.status = kReady;
        if (write) RaiseIrq(channel);
        return;
    }
    if (channel.transfer == Transfer::PioIn) {
        LoadNextReadSector(channel);
    } else {
        channel.position = 0;
        channel.end = kSectorSize;
        channel.status = static_cast<std::uint8_t>(kReady | kStatusDrq);
        RaiseIrq(channel);
    }
}

void IdeController::ExecuteAta(Channel& channel, std::uint8_t command)
{
    const auto sectorCount = channel.disk->SectorCount();
    const auto checkRange = [&](std::uint64_t lba, std::uint32_t count) {
        if (lba > sectorCount || count > sectorCount - lba) {
            Abort(channel, 0x10); // IDNF
            return false;
        }
        return true;
    };
    switch (command) {
    case 0xEC: // IDENTIFY DEVICE
        BuildIdentify(channel);
        channel.position = 0;
        channel.end = 512;
        channel.sectorsRemaining = 0;
        channel.transfer = Transfer::PioIn;
        channel.status = static_cast<std::uint8_t>(kReady | kStatusDrq);
        RaiseIrq(channel);
        return;
    case 0x20: case 0x21: case 0x24: { // READ SECTORS (EXT)
        channel.lba48 = command == 0x24;
        channel.lba = TaskFileLba(channel, channel.lba48);
        channel.sectorsRemaining = TaskFileCount(channel, channel.lba48);
        if (!checkRange(channel.lba, channel.sectorsRemaining)) return;
        LoadNextReadSector(channel);
        return;
    }
    case 0x30: case 0x31: case 0x34: { // WRITE SECTORS (EXT)
        channel.lba48 = command == 0x34;
        channel.lba = TaskFileLba(channel, channel.lba48);
        channel.sectorsRemaining = TaskFileCount(channel, channel.lba48);
        if (!checkRange(channel.lba, channel.sectorsRemaining)) return;
        channel.buffer.assign(kSectorSize, 0);
        channel.position = 0;
        channel.end = kSectorSize;
        channel.transfer = Transfer::PioOut;
        channel.status = static_cast<std::uint8_t>(kReady | kStatusDrq);
        return;
    }
    case 0xC8: case 0xC9: case 0x25: case 0xCA: case 0xCB: case 0x35: { // READ/WRITE DMA (EXT)
        channel.lba48 = command == 0x25 || command == 0x35;
        channel.lba = TaskFileLba(channel, channel.lba48);
        channel.sectorsRemaining = TaskFileCount(channel, channel.lba48);
        if (!checkRange(channel.lba, channel.sectorsRemaining)) return;
        const bool read = command == 0xC8 || command == 0xC9 || command == 0x25;
        channel.transfer = read ? Transfer::DmaIn : Transfer::DmaOut;
        channel.status = static_cast<std::uint8_t>(kReady | kStatusDrq);
        TryStartDma(channel);
        return;
    }
    case 0xE7: case 0xEA: // FLUSH CACHE (EXT)
        try {
            channel.disk->Flush();
        } catch (const std::exception& error) {
            VMM_WARN("IDE: vidage du cache disque impossible: %s", error.what());
        }
        Complete(channel);
        return;
    case 0x40: case 0x41: case 0x42: // READ VERIFY
    case 0x10: case 0x91: case 0xEF: // RECALIBRATE, INITIALIZE DEVICE PARAMETERS, SET FEATURES
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6: // Power management.
        Complete(channel);
        return;
    case 0xE5: // CHECK POWER MODE: active.
        channel.nsector = 0xFF;
        Complete(channel);
        return;
    case 0x90: // EXECUTE DEVICE DIAGNOSTIC
        SetSignature(channel);
        channel.status = kReady;
        RaiseIrq(channel);
        return;
    default:
        VMM_DEBUG("IDE: commande ATA 0x%02x non prise en charge.", command);
        Abort(channel);
        return;
    }
}

// --- DMA ---------------------------------------------------------------------------------

bool IdeController::DmaTransfer(Channel& channel, std::uint8_t* data, std::size_t length, bool toGuest)
{
    if (!BusMasterEnabled()) return false;
    std::size_t done = 0;
    std::uint32_t prd = channel.bmPrd;
    for (std::uint32_t index = 0; index < 8192 && done < length; ++index, prd += 8) {
        std::uint32_t address = 0;
        std::uint32_t control = 0;
        if (!memory_.ReadValue(prd, address) || !memory_.ReadValue(prd + 4, control)) return false;
        std::size_t count = control & 0xFFFF;
        if (count == 0) count = 65536;
        count = (std::min)(count, length - done);
        const bool ok = toGuest ? memory_.Write(address & ~1u, data + done, count)
                                : memory_.Read(address & ~1u, data + done, count);
        if (!ok) return false;
        done += count;
        if ((control & 0x80000000u) != 0) break;
    }
    return done == length;
}

void IdeController::TryStartDma(Channel& channel)
{
    if ((channel.bmCommand & 0x01) == 0) return;
    const auto transfer = channel.transfer;
    if (transfer != Transfer::DmaIn && transfer != Transfer::DmaOut && transfer != Transfer::AtapiDmaIn) return;

    bool success = false;
    try {
        if (transfer == Transfer::AtapiDmaIn) {
            success = DmaTransfer(channel, channel.buffer.data(), channel.buffer.size(), true);
        } else {
            const std::size_t bytes = static_cast<std::size_t>(channel.sectorsRemaining) * kSectorSize;
            channel.buffer.resize(bytes);
            if (transfer == Transfer::DmaIn) {
                channel.disk->ReadSectors(channel.lba, std::as_writable_bytes(std::span(channel.buffer)));
                success = DmaTransfer(channel, channel.buffer.data(), bytes, true);
            } else {
                success = DmaTransfer(channel, channel.buffer.data(), bytes, false);
                if (success) channel.disk->WriteSectors(channel.lba, std::as_bytes(std::span(channel.buffer)));
            }
            if (success) {
                channel.lba += channel.sectorsRemaining;
                SetTaskFileLba(channel, channel.lba - 1);
                channel.sectorsRemaining = 0;
            }
        }
    } catch (const std::exception& error) {
        VMM_WARN("IDE: transfert DMA en échec: %s", error.what());
        success = false;
    }

    channel.bmStatus = static_cast<std::uint8_t>((channel.bmStatus & ~0x01) | 0x04 | (success ? 0 : 0x02));
    if (!success) {
        Abort(channel, transfer == Transfer::AtapiDmaIn ? 0x44 : 0x10);
        return;
    }
    channel.buffer.clear();
    Complete(channel);
}

// --- ATAPI -------------------------------------------------------------------------------

void IdeController::BuildIdentifyPacket(Channel& channel)
{
    channel.buffer.assign(512, 0);
    auto* id = channel.buffer.data();
    PutWord(id, 0, 0x8580); // ATAPI CD-ROM, removable, 12-byte packets.
    PutAtaString(id, 10, 10, "VWHP-CDROM-0001");
    PutAtaString(id, 23, 4, "1.0");
    PutAtaString(id, 27, 20, "VIRTUALISEUR DVD-ROM");
    PutWord(id, 49, 0x0300); // DMA and LBA.
    PutWord(id, 53, 0x0006);
    PutWord(id, 63, 0x0407);
    PutWord(id, 64, 0x0003);
    PutWord(id, 65, 120);
    PutWord(id, 66, 120);
    PutWord(id, 67, 120);
    PutWord(id, 68, 120);
    PutWord(id, 71, 30);
    PutWord(id, 72, 30);
    PutWord(id, 80, 0x0070); // ATAPI-4 to 6.
}

void IdeController::ExecuteAtapiCommand(Channel& channel, std::uint8_t command)
{
    switch (command) {
    case 0xA0: // PACKET
        channel.byteCountLimit = static_cast<std::uint16_t>(channel.lbaMid | (channel.lbaHigh << 8));
        channel.buffer.assign(12, 0);
        channel.position = 0;
        channel.end = 12;
        channel.transfer = Transfer::Packet;
        channel.nsector = 1; // C/D = 1: command packet expected.
        channel.status = static_cast<std::uint8_t>(kReady | kStatusDrq);
        return;
    case 0xA1: // IDENTIFY PACKET DEVICE
        BuildIdentifyPacket(channel);
        channel.position = 0;
        channel.end = 512;
        channel.sectorsRemaining = 0;
        channel.transfer = Transfer::PioIn;
        channel.status = static_cast<std::uint8_t>(kReady | kStatusDrq);
        RaiseIrq(channel);
        return;
    case 0x08: // DEVICE RESET
    case 0x90: // EXECUTE DEVICE DIAGNOSTIC
        SetSignature(channel);
        channel.status = 0;
        if (command == 0x90) RaiseIrq(channel);
        return;
    case 0xEC: // IDENTIFY DEVICE must abort with the packet signature.
        SetSignature(channel);
        channel.error = 0x04;
        channel.status = static_cast<std::uint8_t>(kReady | kStatusErr);
        RaiseIrq(channel);
        return;
    case 0xEF: case 0xE0: case 0xE1: case 0xE3: case 0xE7:
        Complete(channel);
        return;
    default:
        Abort(channel);
        return;
    }
}

void IdeController::AtapiCheckCondition(Channel& channel, std::uint8_t key, std::uint8_t asc)
{
    channel.senseKey = key;
    channel.asc = asc;
    channel.error = static_cast<std::uint8_t>((key << 4) | 0x04);
    channel.status = static_cast<std::uint8_t>(kReady | kStatusErr);
    channel.nsector = 3;
    channel.transfer = Transfer::None;
    RaiseIrq(channel);
}

void IdeController::AtapiStartPioPhase(Channel& channel)
{
    std::size_t limit = channel.byteCountLimit;
    if (limit == 0 || limit == 0xFFFF) limit = 0xFFFE;
    limit &= ~static_cast<std::size_t>(1);
    const auto chunk = (std::min)(channel.buffer.size() - channel.position, limit);
    channel.end = channel.position + chunk;
    channel.lbaMid = static_cast<std::uint8_t>(chunk);
    channel.lbaHigh = static_cast<std::uint8_t>(chunk >> 8);
    channel.nsector = 2; // I/O = 1: data to host.
    channel.transfer = Transfer::AtapiPioIn;
    channel.status = static_cast<std::uint8_t>(kReady | kStatusDrq);
    RaiseIrq(channel);
}

void IdeController::AtapiReply(Channel& channel, std::vector<std::uint8_t> data, std::size_t allocationLength)
{
    if (data.size() > allocationLength) data.resize(allocationLength);
    if (data.empty()) {
        Complete(channel);
        return;
    }
    channel.buffer = std::move(data);
    channel.position = 0;
    if ((channel.feature & 0x01) != 0) {
        channel.transfer = Transfer::AtapiDmaIn;
        channel.nsector = 2;
        channel.status = static_cast<std::uint8_t>(kReady | kStatusDrq);
        TryStartDma(channel);
        return;
    }
    AtapiStartPioPhase(channel);
}

void IdeController::ExecutePacket(Channel& channel)
{
    const auto* cdb = channel.packet.data();
    const auto opcode = cdb[0];
    const std::uint64_t totalBlocks = channel.iso != nullptr ? channel.iso->SizeBytes() / kCdBlockSize : 0;
    const bool mediumPresent = channel.iso != nullptr && !channel.ejected;

    switch (opcode) {
    case 0x00: // TEST UNIT READY
        if (!mediumPresent) return AtapiCheckCondition(channel, 0x02, 0x3A);
        return Complete(channel);
    case 0x03: { // REQUEST SENSE
        std::vector<std::uint8_t> sense(18, 0);
        sense[0] = 0x70;
        sense[2] = channel.senseKey;
        sense[7] = 10;
        sense[12] = channel.asc;
        channel.senseKey = 0;
        channel.asc = 0;
        return AtapiReply(channel, std::move(sense), cdb[4]);
    }
    case 0x12: { // INQUIRY
        if ((cdb[1] & 0x01) != 0) return AtapiCheckCondition(channel, 0x05, 0x24);
        std::vector<std::uint8_t> inquiry(36, ' ');
        inquiry[0] = 0x05; // CD/DVD
        inquiry[1] = 0x80; // Removable.
        inquiry[2] = 0x00;
        inquiry[3] = 0x21; // ATAPI, response format 1.
        inquiry[4] = 31;
        inquiry[5] = inquiry[6] = inquiry[7] = 0;
        std::memcpy(&inquiry[8], "VIRTWHP ", 8);
        std::memcpy(&inquiry[16], "VIRTUAL DVD-ROM ", 16);
        std::memcpy(&inquiry[32], "1.0 ", 4);
        return AtapiReply(channel, std::move(inquiry), Be16(cdb + 3));
    }
    case 0x1A: case 0x5A: { // MODE SENSE (6/10)
        const bool ten = opcode == 0x5A;
        const auto page = static_cast<std::uint8_t>(cdb[2] & 0x3F);
        std::vector<std::uint8_t> header(ten ? 8 : 4, 0);
        std::vector<std::uint8_t> body;
        if (page == 0x2A || page == 0x3F) {
            body.assign(22, 0);
            body[0] = 0x2A;
            body[1] = 0x14;
            body[2] = 0x03; // Reads CD-R and CD-RW.
            body[4] = 0x00;
            body[6] = 0x29; // Tray, eject, lock.
            PutBe16(&body[8], 7056);
            PutBe16(&body[10], 256);
            PutBe16(&body[12], 512);
            PutBe16(&body[14], 7056);
        }
        std::vector<std::uint8_t> reply = header;
        reply.insert(reply.end(), body.begin(), body.end());
        if (ten) PutBe16(reply.data(), static_cast<std::uint32_t>(reply.size() - 2));
        else reply[0] = static_cast<std::uint8_t>(reply.size() - 1);
        return AtapiReply(channel, std::move(reply), ten ? Be16(cdb + 7) : cdb[4]);
    }
    case 0x1B: { // START STOP UNIT
        const bool loadEject = (cdb[4] & 0x02) != 0;
        const bool start = (cdb[4] & 0x01) != 0;
        if (loadEject) {
            channel.ejected = !start || channel.iso == nullptr;
            if (channel.ejected) VMM_INFO("CD-ROM: le système invité a éjecté le média.");
        }
        return Complete(channel);
    }
    case 0x1E: case 0x2B: case 0xBB: case 0x35: // PREVENT/ALLOW, SEEK, SET CD SPEED, SYNC CACHE
        return Complete(channel);
    case 0x25: { // READ CAPACITY
        if (!mediumPresent) return AtapiCheckCondition(channel, 0x02, 0x3A);
        std::vector<std::uint8_t> capacity(8, 0);
        PutBe32(capacity.data(), static_cast<std::uint32_t>(totalBlocks - 1));
        PutBe32(capacity.data() + 4, kCdBlockSize);
        return AtapiReply(channel, std::move(capacity), 8);
    }
    case 0x28: case 0xA8: { // READ (10/12)
        if (!mediumPresent) return AtapiCheckCondition(channel, 0x02, 0x3A);
        const auto lba = Be32(cdb + 2);
        const auto count = opcode == 0x28 ? static_cast<std::uint32_t>(Be16(cdb + 7)) : Be32(cdb + 6);
        if (count == 0) return Complete(channel);
        if (lba > totalBlocks || count > totalBlocks - lba || count > 32768) {
            return AtapiCheckCondition(channel, 0x05, 0x21);
        }
        std::vector<std::uint8_t> data(static_cast<std::size_t>(count) * kCdBlockSize);
        try {
            channel.iso->ReadLogicalBlocks(lba, std::as_writable_bytes(std::span(data)));
        } catch (const std::exception& error) {
            VMM_WARN("CD-ROM: lecture du bloc %u impossible: %s", lba, error.what());
            return AtapiCheckCondition(channel, 0x03, 0x11);
        }
        const auto length = data.size();
        return AtapiReply(channel, std::move(data), length);
    }
    case 0x23: { // READ FORMAT CAPACITIES
        std::vector<std::uint8_t> reply(12, 0);
        reply[3] = 8;
        PutBe32(&reply[4], static_cast<std::uint32_t>(totalBlocks));
        reply[8] = mediumPresent ? 0x02 : 0x03;
        reply[9] = 0;
        reply[10] = 0x08; // 2048-byte blocks.
        return AtapiReply(channel, std::move(reply), Be16(cdb + 7));
    }
    case 0x43: { // READ TOC/PMA/ATIP
        if (!mediumPresent) return AtapiCheckCondition(channel, 0x02, 0x3A);
        const bool msf = (cdb[1] & 0x02) != 0;
        std::uint8_t format = static_cast<std::uint8_t>(cdb[2] & 0x0F);
        if (format == 0 && (cdb[9] & 0xC0) != 0) format = static_cast<std::uint8_t>(cdb[9] >> 6);
        std::vector<std::uint8_t> toc;
        if (format == 1) { // Session information.
            toc.assign(12, 0);
            PutBe16(toc.data(), 10);
            toc[2] = 1;
            toc[3] = 1;
            toc[5] = 0x14;
            toc[6] = 1;
            PutAddress(&toc[8], 0, msf);
        } else if (format == 0) {
            toc.assign(20, 0);
            PutBe16(toc.data(), 18);
            toc[2] = 1;
            toc[3] = 1;
            toc[5] = 0x14;
            toc[6] = 1;
            PutAddress(&toc[8], 0, msf);
            toc[13] = 0x14;
            toc[14] = 0xAA;
            PutAddress(&toc[16], static_cast<std::uint32_t>(totalBlocks), msf);
        } else {
            return AtapiCheckCondition(channel, 0x05, 0x24);
        }
        return AtapiReply(channel, std::move(toc), Be16(cdb + 7));
    }
    case 0x46: { // GET CONFIGURATION
        std::vector<std::uint8_t> reply(8, 0);
        PutBe32(reply.data(), 4);
        PutBe16(&reply[6], mediumPresent ? 0x0010 : 0x0000); // DVD-ROM profile.
        return AtapiReply(channel, std::move(reply), Be16(cdb + 7));
    }
    case 0x4A: { // GET EVENT STATUS NOTIFICATION
        if ((cdb[1] & 0x01) == 0) return AtapiCheckCondition(channel, 0x05, 0x24);
        std::vector<std::uint8_t> reply;
        if ((cdb[4] & 0x10) != 0) {
            reply.assign(8, 0);
            PutBe16(reply.data(), 6);
            reply[2] = 0x04;
            reply[3] = 0x10;
            reply[4] = channel.mediaChanged ? 0x02 : 0x00;
            reply[5] = mediumPresent ? 0x02 : 0x00;
            channel.mediaChanged = false;
        } else {
            reply.assign(4, 0);
            PutBe16(reply.data(), 2);
            reply[2] = 0x80;
            reply[3] = 0x10;
        }
        return AtapiReply(channel, std::move(reply), Be16(cdb + 7));
    }
    case 0x51: { // READ DISC INFORMATION
        if (!mediumPresent) return AtapiCheckCondition(channel, 0x02, 0x3A);
        std::vector<std::uint8_t> reply(34, 0);
        PutBe16(reply.data(), 32);
        reply[2] = 0x0E; // Complete disc, last session complete.
        reply[3] = 1;
        reply[4] = 1;
        reply[5] = 1;
        reply[6] = 1;
        return AtapiReply(channel, std::move(reply), Be16(cdb + 7));
    }
    case 0xBD: { // MECHANISM STATUS
        std::vector<std::uint8_t> reply(8, 0);
        reply[5] = 1;
        return AtapiReply(channel, std::move(reply), Be16(cdb + 8));
    }
    default:
        VMM_DEBUG("ATAPI: commande SCSI 0x%02x non prise en charge.", opcode);
        return AtapiCheckCondition(channel, 0x05, 0x20);
    }
}

} // namespace vmm
