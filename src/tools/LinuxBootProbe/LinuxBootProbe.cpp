#include <winsock2.h>

#include "LinuxBootProtocol.h"
#include "Iso9660Image.h"
#include "PciConfigMechanism1.h"
#include "VirtioNetDevice.h"
#include "SlirpNetworkBackend.h"
#include "VirtualDisk.h"

#include <WinHvEmulation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <conio.h>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")

namespace {

void ConfigureHostTerminal()
{
    // Debian's installer draws its text UI with ANSI escape sequences. Enable
    // VT rendering in classic conhost and use UTF-8 for French status output.
    (void)SetConsoleOutputCP(CP_UTF8);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (output != INVALID_HANDLE_VALUE && GetConsoleMode(output, &mode)) {
        (void)SetConsoleMode(output, mode | ENABLE_PROCESSED_OUTPUT |
            ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

class ComUart final {
public:
    explicit ComUart(std::uint16_t basePort, bool consoleOutput = false)
        : basePort_(basePort), consoleOutput_(consoleOutput) {}

    bool HandlesPort(std::uint16_t port) const noexcept
    {
        return port >= basePort_ && port <= basePort_ + 7;
    }

    std::uint8_t Read(std::uint16_t port)
    {
        const std::lock_guard lock(mutex_);
        const auto offset = static_cast<std::uint8_t>(port - basePort_);
        switch (offset) {
        case 0:
            if (lineControl_ & 0x80) return divisorLow_;
            if (receiveBuffer_.empty()) return 0;
            {
                const auto value = receiveBuffer_.front();
                receiveBuffer_.pop_front();
                return value;
            }
        case 1: return (lineControl_ & 0x80) ? divisorHigh_ : interruptEnable_;
        case 2:
            if ((interruptEnable_ & 0x01) != 0 && !receiveBuffer_.empty()) return 0x04;
            if ((interruptEnable_ & 0x02) != 0 && transmitInterruptPending_) {
                transmitInterruptPending_ = false;
                return 0x02;
            }
            return 0x01;
        case 3: return lineControl_;
        case 4: return modemControl_;
        case 5: return static_cast<std::uint8_t>(0x60 | (!receiveBuffer_.empty() ? 0x01 : 0));
        case 6: return 0xB0; // CTS, DSR and DCD asserted.
        case 7: return scratch_;
        default: return 0xFF;
        }
    }

    void Write(std::uint16_t port, std::uint8_t value)
    {
        const std::lock_guard lock(mutex_);
        const auto offset = static_cast<std::uint8_t>(port - basePort_);
        switch (offset) {
        case 0:
            if (lineControl_ & 0x80) {
                divisorLow_ = value;
            } else {
                const auto character = static_cast<char>(value);
                if (consoleOutput_) std::cout.put(character);
                std::cout.flush(); // UART output is byte-oriented; shells may print prompts without LF.
                if ((interruptEnable_ & 0x02) != 0) transmitInterruptPending_ = true;
            }
            break;
        case 1:
            if (lineControl_ & 0x80) divisorHigh_ = value;
            else {
                interruptEnable_ = value;
                if ((interruptEnable_ & 0x02) != 0) transmitInterruptPending_ = true;
            }
            break;
        case 3: lineControl_ = value; break;
        case 4: modemControl_ = value; break;
        case 7: scratch_ = value; break;
        default: break;
        }
    }

    bool HasInterrupt() const
    {
        const std::lock_guard lock(mutex_);
        return ((interruptEnable_ & 0x01) != 0 && !receiveBuffer_.empty()) ||
               ((interruptEnable_ & 0x02) != 0 && transmitInterruptPending_);
    }

    std::uint8_t InterruptEnable() const
    {
        const std::lock_guard lock(mutex_);
        return interruptEnable_;
    }

    void PollHostInput()
    {
        if (!consoleOutput_ || !_kbhit()) return;
        const int character = _getch();
        if (character < 0) return;
        const std::lock_guard lock(mutex_);
        receiveBuffer_.push_back(static_cast<std::uint8_t>(character));
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::uint8_t> receiveBuffer_;
    std::uint8_t interruptEnable_ = 0;
    bool transmitInterruptPending_ = false;
    std::uint8_t lineControl_ = 0;
    std::uint8_t modemControl_ = 0;
    std::uint8_t scratch_ = 0;
    std::uint8_t divisorLow_ = 1;
    std::uint8_t divisorHigh_ = 0;
    std::uint16_t basePort_;
    bool consoleOutput_;
};

class LegacyPic final {
public:
    std::optional<std::uint32_t> InterruptVector(std::uint8_t irq) const
    {
        const std::lock_guard lock(mutex_);
        if (irq < 8) {
            if ((masterMask_ & (1u << irq)) != 0 ||
                masterOffset_ < 0x10 || masterOffset_ > 0xF0) return std::nullopt;
            return static_cast<std::uint32_t>(masterOffset_) + irq;
        }
        if (irq >= 16 || (slaveMask_ & (1u << (irq - 8))) != 0 ||
            (masterMask_ & (1u << 2)) != 0 || slaveOffset_ < 0x10 || slaveOffset_ > 0xF0) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(slaveOffset_) + (irq - 8);
    }

    std::optional<std::uint32_t> TimerVector() const
    {
        return InterruptVector(0);
    }

    std::uint8_t Read(std::uint16_t port) const
    {
        const std::lock_guard lock(mutex_);
        switch (port) {
        case 0x20: case 0xA0: return 0; // No pending IRR bits in this first PIC model.
        case 0x21: return masterMask_;
        case 0xA1: return slaveMask_;
        case 0x4D0: return masterElcr_;
        case 0x4D1: return slaveElcr_;
        default: return 0xFF;
        }
    }

    void Write(std::uint16_t port, std::uint8_t value)
    {
        const std::lock_guard lock(mutex_);
        switch (port) {
        case 0x20: Command(value, masterInitStep_); break;
        case 0xA0: Command(value, slaveInitStep_); break;
        case 0x21: Data(value, masterInitStep_, masterOffset_, masterMask_); break;
        case 0xA1: Data(value, slaveInitStep_, slaveOffset_, slaveMask_); break;
        case 0x4D0: masterElcr_ = static_cast<std::uint8_t>(value & 0xF8); break;
        case 0x4D1: slaveElcr_ = static_cast<std::uint8_t>(value & 0xDE); break;
        default: break;
        }
    }

private:
    static void Command(std::uint8_t value, std::uint8_t& initStep) noexcept
    {
        if ((value & 0x10) != 0) {
            initStep = 1; // ICW1: expect vector base in ICW2.
        } else if ((value & 0x20) != 0) {
            // Non-specific EOI; the host interrupt request has already been delivered.
        }
    }

    static void Data(
        std::uint8_t value,
        std::uint8_t& initStep,
        std::uint8_t& offset,
        std::uint8_t& mask) noexcept
    {
        if (initStep == 1) {
            offset = value;
            initStep = 2;
        } else if (initStep == 2) {
            initStep = 3; // ICW3 cascade wiring.
        } else if (initStep == 3) {
            initStep = 0; // ICW4; auto-EOI is not modeled.
        } else {
            mask = value;
        }
    }

    mutable std::mutex mutex_;
    std::uint8_t masterOffset_ = 0x08;
    std::uint8_t slaveOffset_ = 0x70;
    std::uint8_t masterMask_ = 0xFF;
    std::uint8_t slaveMask_ = 0xFF;
    std::uint8_t masterElcr_ = 0;
    std::uint8_t slaveElcr_ = 0;
    std::uint8_t masterInitStep_ = 0;
    std::uint8_t slaveInitStep_ = 0;
};

class PiixIdeController final {
public:
    explicit PiixIdeController(virtualiseur::whp::VirtualDisk* disk) : disk_(disk) {}

    bool HandlesPort(std::uint16_t port) const noexcept
    {
        return (port >= 0x1F0 && port <= 0x1F7) || port == 0x3F6;
    }

    std::uint32_t Read(std::uint16_t port, std::uint8_t size)
    {
        if (disk_ == nullptr) return 0xFF;
        if (SlaveSelected()) {
            if (port == 0x1F0) return 0xFFFFFFFF;
            if (port == 0x1F7 || port == 0x3F6) return 0x00; // No slave device: not busy.
            if (port != 0x1F6) return 0xFF;
        }
        if (port == 0x1F0) return ReadData(size);
        switch (port) {
        case 0x1F1: return error_;
        case 0x1F2: return sectorCount_;
        case 0x1F3: return lbaLow_;
        case 0x1F4: return lbaMid_;
        case 0x1F5: return lbaHigh_;
        case 0x1F6: return driveHead_;
        case 0x1F7:
            irqPending_ = false;
            return status_;
        case 0x3F6: return status_;
        default: return 0xFF;
        }
    }

    void Write(std::uint16_t port, std::uint8_t size, std::uint32_t value)
    {
        if (disk_ == nullptr) return;
        const auto byte = static_cast<std::uint8_t>(value);
        if (port == 0x1F0) {
            if (!SlaveSelected()) WriteData(size, value);
            return;
        }
        switch (port) {
        case 0x1F1: feature_ = byte; break;
        case 0x1F2: sectorCount_ = byte; break;
        case 0x1F3: lbaLow_ = byte; break;
        case 0x1F4: lbaMid_ = byte; break;
        case 0x1F5: lbaHigh_ = byte; break;
        case 0x1F6: driveHead_ = byte; break;
        case 0x1F7:
            if (!SlaveSelected()) ExecuteCommand(byte);
            break;
        case 0x3F6:
            {
                const bool wasReset = (deviceControl_ & 0x04) != 0;
                deviceControl_ = byte;
                const bool isReset = (byte & 0x04) != 0;
                if (isReset != wasReset) Reset(isReset);
            }
            break;
        default: break;
        }
    }

    bool TakeInterruptRequest() noexcept
    {
        return std::exchange(irqRequested_, false);
    }

    bool DmaPending() const noexcept
    {
        return transfer_ == Transfer::DmaRead || transfer_ == Transfer::DmaWrite;
    }

    bool DmaReadsFromDisk() const noexcept { return transfer_ == Transfer::DmaRead; }

    std::size_t DmaByteCount() const noexcept
    {
        return static_cast<std::size_t>(sectorsRemaining_) *
               virtualiseur::whp::VirtualDisk::SectorSize;
    }

    void ReadDmaData(std::span<std::byte> destination) const
    {
        if (transfer_ != Transfer::DmaRead || destination.size() != DmaByteCount()) {
            throw std::runtime_error("Commande IDE DMA lecture invalide.");
        }
        disk_->ReadSectors(CurrentLba(), destination);
    }

    void WriteDmaData(std::span<const std::byte> source)
    {
        if (transfer_ != Transfer::DmaWrite || source.size() != DmaByteCount()) {
            throw std::runtime_error("Commande IDE DMA ecriture invalide.");
        }
        disk_->WriteSectors(CurrentLba(), source);
    }

    void CompleteDma(bool success) noexcept
    {
        if (!DmaPending()) return;
        if (!success) {
            error_ = 0x04; // ABRT.
            status_ = 0x51;
        } else {
            for (std::uint32_t sector = 0; sector < sectorsRemaining_; ++sector) {
                ++lbaLow_;
                if (lbaLow_ == 0 && ++lbaMid_ == 0) ++lbaHigh_;
            }
            error_ = 0;
            status_ = 0x50;
        }
        transfer_ = Transfer::None;
        sectorsRemaining_ = 0;
        RaiseInterrupt();
    }

    static constexpr std::uint8_t InterruptLine() noexcept { return 14; }

private:
    enum class Transfer : std::uint8_t { None, Identify, Read, Write, DmaRead, DmaWrite };

    bool SlaveSelected() const noexcept { return (driveHead_ & 0x10) != 0; }

    std::uint64_t CurrentLba() const noexcept
    {
        return static_cast<std::uint64_t>(lbaLow_) |
               (static_cast<std::uint64_t>(lbaMid_) << 8) |
               (static_cast<std::uint64_t>(lbaHigh_) << 16) |
               (static_cast<std::uint64_t>(driveHead_ & 0x0F) << 24);
    }

    void RaiseInterrupt() noexcept
    {
        irqPending_ = true;
        if ((deviceControl_ & 0x02) == 0) irqRequested_ = true;
    }

    void Reset(bool asserted) noexcept
    {
        error_ = 1;
        sectorCount_ = 1;
        lbaLow_ = 1;
        lbaMid_ = 0;
        lbaHigh_ = 0;
        driveHead_ = 0xA0;
        status_ = asserted ? 0x80 : 0x50; // BSY while SRST is held, ready on release.
        transfer_ = Transfer::None;
        transferOffset_ = 0;
        sectorsRemaining_ = 0;
        irqPending_ = false;
        irqRequested_ = false;
    }

    void ExecuteCommand(std::uint8_t command)
    {
        error_ = 0;
        transferOffset_ = 0;
        switch (command) {
        case 0xEC: // IDENTIFY DEVICE
            BuildIdentifyData();
            transfer_ = Transfer::Identify;
            sectorsRemaining_ = 1;
            status_ = 0x58; // DRDY | DRQ | DSC
            RaiseInterrupt();
            break;
        case 0x20: case 0x21: // READ SECTORS (with/without retry)
            transfer_ = Transfer::Read;
            sectorsRemaining_ = sectorCount_ == 0 ? 256u : sectorCount_;
            status_ = 0x50;
            LoadReadSector();
            break;
        case 0x30: case 0x31: // WRITE SECTORS (with/without retry)
            transfer_ = Transfer::Write;
            sectorsRemaining_ = sectorCount_ == 0 ? 256u : sectorCount_;
            transferOffset_ = 0;
            status_ = 0x58;
            RaiseInterrupt();
            break;
        case 0xC8: // READ DMA
            transfer_ = Transfer::DmaRead;
            sectorsRemaining_ = sectorCount_ == 0 ? 256u : sectorCount_;
            status_ = 0x80; // Wait for the bus-master engine to complete the transfer.
            break;
        case 0xCA: // WRITE DMA
            transfer_ = Transfer::DmaWrite;
            sectorsRemaining_ = sectorCount_ == 0 ? 256u : sectorCount_;
            status_ = 0x80;
            break;
        case 0xE7: // FLUSH CACHE
            disk_->Flush();
            transfer_ = Transfer::None;
            status_ = 0x50;
            RaiseInterrupt();
            break;
        case 0x90: // EXECUTE DEVICE DIAGNOSTIC
            error_ = 1;
            transfer_ = Transfer::None;
            status_ = 0x50;
            RaiseInterrupt();
            break;
        default:
            error_ = 0x04; // ABRT
            transfer_ = Transfer::None;
            status_ = 0x51; // DRDY | ERR
            RaiseInterrupt();
            break;
        }
    }

    void LoadReadSector()
    {
        try {
            if (transfer_ == Transfer::Identify) {
                transferOffset_ = 0;
                status_ = 0x58;
            } else {
                disk_->ReadSectors(CurrentLba(), std::as_writable_bytes(std::span(sector_)));
                transferOffset_ = 0;
                status_ = 0x58;
            }
            RaiseInterrupt();
        } catch (...) {
            error_ = 0x10; // IDNF
            status_ = 0x51;
            transfer_ = Transfer::None;
            sectorsRemaining_ = 0;
            RaiseInterrupt();
        }
    }

    void CompleteSector()
    {
        transferOffset_ = 0;
        if (transfer_ == Transfer::Identify) {
            transfer_ = Transfer::None;
            sectorsRemaining_ = 0;
            status_ = 0x50;
            return;
        }
        if (sectorsRemaining_ != 0) --sectorsRemaining_;
        if (transfer_ == Transfer::Write) {
            try {
                disk_->WriteSectors(CurrentLba(), std::as_bytes(std::span(sector_)));
            } catch (...) {
                error_ = 0x10;
                status_ = 0x51;
                transfer_ = Transfer::None;
                sectorsRemaining_ = 0;
                RaiseInterrupt();
                return;
            }
        }
        ++lbaLow_;
        if (lbaLow_ == 0) {
            if (++lbaMid_ == 0) ++lbaHigh_;
        }
        if (sectorsRemaining_ == 0) {
            transfer_ = Transfer::None;
            status_ = 0x50;
            RaiseInterrupt();
        } else if (transfer_ == Transfer::Read) {
            LoadReadSector();
        } else {
            status_ = 0x58;
            RaiseInterrupt();
        }
    }

    std::uint32_t ReadData(std::uint8_t size)
    {
        if ((status_ & 0x08) == 0 || transfer_ == Transfer::None) return 0xFFFF;
        if ((size != 1 && size != 2 && size != 4) ||
            transferOffset_ + size > sector_.size()) return 0xFFFF;
        std::uint32_t value = 0xFFFFFFFF;
        std::memcpy(&value, sector_.data() + transferOffset_, size);
        transferOffset_ += size;
        if (transferOffset_ == sector_.size()) CompleteSector();
        return value;
    }

    void WriteData(std::uint8_t size, std::uint32_t value)
    {
        if (transfer_ != Transfer::Write || (status_ & 0x08) == 0) return;
        const auto bytes = size == 1 || size == 2 || size == 4 ? size : 0u;
        if (bytes == 0 || transferOffset_ + bytes > sector_.size()) return;
        std::memcpy(sector_.data() + transferOffset_, &value, bytes);
        transferOffset_ += bytes;
        if (transferOffset_ == sector_.size()) CompleteSector();
    }

    void BuildIdentifyData()
    {
        sector_.fill(std::byte{0});
        auto setWord = [&](std::size_t index, std::uint16_t value) {
            std::memcpy(sector_.data() + index * 2, &value, sizeof(value));
        };
        auto setString = [&](std::size_t firstWord, std::size_t wordCount, const char* value) {
            std::string padded(wordCount * 2, ' ');
            padded.replace(0, (std::min)(padded.size(), std::strlen(value)), value);
            for (std::size_t index = 0; index < padded.size(); ++index) {
                sector_[firstWord * 2 + (index ^ 1)] =
                    static_cast<std::byte>(static_cast<unsigned char>(padded[index]));
            }
        };
        setWord(0, 0x0040);
        setWord(1, 16383);
        setWord(3, 16);
        setWord(6, 63);
        setString(10, 10, "VWHP0000000000000001");
        setString(23, 4, "1.0");
        setString(27, 20, "VIRTUALISEUR WHP IDE DISK");
        setWord(49, 0x0300); // DMA and LBA supported.
        setWord(63, 0x0007); // Multiword DMA modes 0-2 supported.
        const auto sectors = disk_->SectorCount();
        setWord(60, static_cast<std::uint16_t>(sectors & 0xFFFF));
        setWord(61, static_cast<std::uint16_t>((sectors >> 16) & 0xFFFF));
        setWord(83, 0x4000); // Word 83 is valid.
        setWord(100, static_cast<std::uint16_t>(sectors & 0xFFFF));
        setWord(101, static_cast<std::uint16_t>((sectors >> 16) & 0xFFFF));
        setWord(102, static_cast<std::uint16_t>((sectors >> 32) & 0xFFFF));
        setWord(103, static_cast<std::uint16_t>((sectors >> 48) & 0xFFFF));
    }

    virtualiseur::whp::VirtualDisk* disk_ = nullptr;
    std::array<std::byte, virtualiseur::whp::VirtualDisk::SectorSize> sector_{};
    Transfer transfer_ = Transfer::None;
    std::uint32_t sectorsRemaining_ = 0;
    std::size_t transferOffset_ = 0;
    std::uint8_t error_ = 1;
    std::uint8_t status_ = 0x50;
    std::uint8_t feature_ = 0;
    std::uint8_t sectorCount_ = 1;
    std::uint8_t lbaLow_ = 1;
    std::uint8_t lbaMid_ = 0;
    std::uint8_t lbaHigh_ = 0;
    std::uint8_t driveHead_ = 0xA0;
    std::uint8_t deviceControl_ = 0;
    bool irqPending_ = false;
    bool irqRequested_ = false;
};

class AtapiCdrom final {
public:
    explicit AtapiCdrom(virtualiseur::whp::Iso9660Image* image) : image_(image)
    {
        Reset();
    }

    bool HandlesPort(std::uint16_t port) const noexcept
    {
        return (port >= 0x170 && port <= 0x177) || port == 0x376;
    }

    std::uint32_t Read(std::uint16_t port, std::uint8_t size)
    {
        if (SlaveSelected() && port != 0x176) {
            return port == 0x177 || port == 0x376 ? 0x00 : 0xFF;
        }
        if (port == 0x170) return ReadData(size);
        switch (port) {
        case 0x171: return error_;
        case 0x172: return packetProtocolStarted_ ? interruptReason_ : sectorCount_;
        case 0x173: return lbaLow_;
        case 0x174: return byteCount_ & 0xFF;
        case 0x175: return byteCount_ >> 8;
        case 0x176: return driveHead_;
        case 0x177:
            irqPending_ = false;
            return status_;
        case 0x376: return status_;
        default: return 0xFF;
        }
    }

    void Write(std::uint16_t port, std::uint8_t size, std::uint32_t value)
    {
        const auto byte = static_cast<std::uint8_t>(value);
        if (SlaveSelected() && port != 0x176 && port != 0x376) return;
        if (port == 0x170) {
            WriteData(size, value);
            return;
        }
        switch (port) {
        case 0x171: features_ = byte; break;
        case 0x172: sectorCount_ = byte; break;
        case 0x173: lbaLow_ = byte; break;
        case 0x174: byteCount_ = static_cast<std::uint16_t>((byteCount_ & 0xFF00) | byte); break;
        case 0x175: byteCount_ = static_cast<std::uint16_t>((byteCount_ & 0x00FF) | (byte << 8)); break;
        case 0x176: driveHead_ = byte; break;
        case 0x177: ExecuteCommand(byte); break;
        case 0x376:
            {
                const bool wasReset = (deviceControl_ & 0x04) != 0;
                deviceControl_ = byte;
                const bool isReset = (byte & 0x04) != 0;
                if (isReset != wasReset) Reset(isReset);
            }
            break;
        default: break;
        }
    }

    bool TakeInterruptRequest() noexcept { return std::exchange(irqRequested_, false); }
    static constexpr std::uint8_t InterruptLine() noexcept { return 15; }

private:
    enum class Phase : std::uint8_t { Idle, Packet, Data };

    bool SlaveSelected() const noexcept { return (driveHead_ & 0x10) != 0; }

    void RaiseInterrupt() noexcept
    {
        irqPending_ = true;
        if ((deviceControl_ & 0x02) == 0 && (features_ & 0x02) == 0) irqRequested_ = true;
    }

    void SetByteCount(std::size_t count) noexcept
    {
        byteCount_ = static_cast<std::uint16_t>(count);
    }

    void Reset(bool asserted = false) noexcept
    {
        phase_ = Phase::Idle;
        // ATA/ATAPI-6 packet-device signature: status 00h and task-file
        // registers 01h, 01h, 14h, EBh. ata_piix uses it to identify the CD.
        status_ = asserted ? 0x80 : 0x00;
        error_ = 0x01;
        interruptReason_ = 0x01;
        sectorCount_ = 0x01;
        lbaLow_ = 0x01;
        byteCount_ = 0xEB14;
        transferLimit_ = 0;
        packetOffset_ = 0;
        response_.clear();
        responseOffset_ = 0;
        phaseLength_ = 0;
        irqPending_ = false;
        irqRequested_ = false;
        packetProtocolStarted_ = false;
    }

    void ExecuteCommand(std::uint8_t command)
    {
        error_ = 0;
        response_.clear();
        responseOffset_ = 0;
        packetOffset_ = 0;
        if (image_ == nullptr) {
            status_ = 0x50;
            return;
        }
        if (command == 0xA1) { // IDENTIFY PACKET DEVICE
            packetProtocolStarted_ = true;
            BuildIdentifyPacket();
            responseOffset_ = 0;
            phaseEnd_ = response_.size();
            phase_ = Phase::Data;
            status_ = 0x58;
            interruptReason_ = 2;
            SetByteCount(response_.size());
            phaseLength_ = response_.size();
            RaiseInterrupt();
        } else if (command == 0xA0) { // PACKET
            packetProtocolStarted_ = true;
            transferLimit_ = byteCount_;
            phase_ = Phase::Packet;
            status_ = 0x58;
            interruptReason_ = 1; // Command packet requested.
            SetByteCount(12);
            RaiseInterrupt();
        } else {
            AbortCommand();
        }
    }

    void WriteData(std::uint8_t size, std::uint32_t value)
    {
        if (phase_ != Phase::Packet || (status_ & 0x08) == 0) return;
        const auto bytes = size == 1 || size == 2 || size == 4 ? size : 0u;
        if (bytes == 0 || packetOffset_ + bytes > packet_.size()) return;
        std::memcpy(packet_.data() + packetOffset_, &value, bytes);
        packetOffset_ += bytes;
        if (packetOffset_ == packet_.size()) ExecutePacket();
    }

    std::uint32_t ReadData(std::uint8_t size)
    {
        if (phase_ != Phase::Data || (status_ & 0x08) == 0) return 0xFFFF;
        const auto bytes = size == 1 || size == 2 || size == 4 ? size : 0u;
        if (bytes == 0 || responseOffset_ + bytes > response_.size() ||
            responseOffset_ + bytes > phaseEnd_) return 0xFFFF;
        std::uint32_t value = 0xFFFFFFFF;
        std::memcpy(&value, response_.data() + responseOffset_, bytes);
        responseOffset_ += bytes;
        if (responseOffset_ == phaseEnd_) AdvanceDataPhase();
        return value;
    }

    void ExecutePacket()
    {
        const auto opcode = std::to_integer<std::uint8_t>(packet_[0]);
        switch (opcode) {
        case 0x00: // TEST UNIT READY
        case 0x1B: // START STOP UNIT
        case 0x1E: // PREVENT/ALLOW MEDIUM REMOVAL
        case 0x2B: // SEEK(10)
            CompleteCommand();
            return;
        case 0x03: // REQUEST SENSE
            response_.assign(18, std::byte{0});
            response_[0] = std::byte{0x70};
            response_[2] = static_cast<std::byte>(senseKey_);
            response_[7] = std::byte{10};
            response_[12] = static_cast<std::byte>(additionalSense_);
            senseKey_ = 0;
            additionalSense_ = 0;
            break;
        case 0x12: // INQUIRY
            response_.assign(36, std::byte{0});
            response_[0] = std::byte{0x05}; // CD/DVD device.
            response_[1] = std::byte{0x80}; // Removable medium.
            response_[2] = std::byte{0x05};
            response_[3] = std::byte{0x02};
            response_[4] = std::byte{31};
            CopyScsiString(8, 8, "VIRTWH");
            CopyScsiString(16, 16, "VIRTUAL CD-ROM");
            CopyScsiString(32, 4, "1.0 ");
            break;
        case 0x25: // READ CAPACITY(10)
            response_.assign(8, std::byte{0});
            WriteBigEndian32(response_.data(), static_cast<std::uint32_t>(image_->SizeBytes() / 2048 - 1));
            WriteBigEndian32(response_.data() + 4, 2048);
            break;
        case 0x23: // READ FORMAT CAPACITIES
            response_.assign(12, std::byte{0});
            response_[3] = std::byte{8};
            WriteBigEndian32(response_.data() + 4,
                static_cast<std::uint32_t>(image_->SizeBytes() / 2048));
            response_[8] = std::byte{2}; // Formatted media.
            WriteBigEndian24(response_.data() + 9, 2048);
            break;
        case 0x46: // GET CONFIGURATION, current profile
            response_.assign(8, std::byte{0});
            WriteBigEndian32(response_.data(), 4);
            WriteBigEndian16(response_.data() + 6, 0x0008); // CD-ROM profile.
            break;
        case 0x51: // READ DISC INFORMATION
            response_.assign(34, std::byte{0});
            WriteBigEndian16(response_.data(), 32);
            response_[2] = std::byte{0x0E}; // Complete, one session, finalized.
            response_[3] = std::byte{1};
            response_[4] = std::byte{1};
            response_[5] = std::byte{1};
            break;
        case 0x28: // READ(10)
            try {
                const auto lba = ReadBigEndian32(packet_.data() + 2);
                const auto blocks = ReadBigEndian16(packet_.data() + 7);
                const auto byteCount = static_cast<std::uint64_t>(blocks) * 2048;
                if (byteCount > 64ull * 1024 * 1024) throw std::out_of_range("ATAPI read too large");
                response_.resize(static_cast<std::size_t>(byteCount));
                if (!response_.empty()) image_->ReadLogicalBlocks(lba, response_);
            } catch (...) {
                SetSense(0x05, 0x21); // ILLEGAL REQUEST / LBA out of range.
                AbortCommand();
                return;
            }
            break;
        case 0x43: // READ TOC/PMA/ATIP, format 0: one data track
            response_.assign(20, std::byte{0});
            WriteBigEndian16(response_.data(), 18);
            response_[2] = std::byte{1};
            response_[3] = std::byte{1};
            response_[5] = std::byte{0x14};
            response_[6] = std::byte{1};
            response_[13] = std::byte{0x14};
            response_[14] = std::byte{0xAA};
            WriteBigEndian32(response_.data() + 16,
                static_cast<std::uint32_t>(image_->SizeBytes() / 2048));
            break;
        case 0x5A: // MODE SENSE(10), no changeable pages reported.
            response_.assign(8, std::byte{0});
            WriteBigEndian16(response_.data(), 6);
            break;
        default:
            SetSense(0x05, 0x20); // ILLEGAL REQUEST / invalid command.
            AbortCommand();
            return;
        }
        const std::size_t allocationLength = opcode == 0x12 || opcode == 0x03
            ? std::to_integer<std::uint8_t>(packet_[4])
            : opcode == 0x23 || opcode == 0x43 || opcode == 0x46 ||
              opcode == 0x51 || opcode == 0x5A
                ? ReadBigEndian16(packet_.data() + 7) : response_.size();
        if (response_.size() > allocationLength) response_.resize(allocationLength);
        responseOffset_ = 0;
        phase_ = Phase::Data;
        status_ = response_.empty() ? 0x50 : 0x58;
        interruptReason_ = 2;
        if (response_.empty()) {
            CompleteCommand();
            return;
        }
        StartDataPhase();
    }

    void StartDataPhase()
    {
        const auto limit = transferLimit_ == 0 ? 0xFFFEu :
            static_cast<std::size_t>(transferLimit_ & ~1u);
        phaseEnd_ = (std::min)(response_.size(), responseOffset_ + (std::max)(limit, std::size_t{2}));
        phaseLength_ = phaseEnd_ - responseOffset_;
        SetByteCount(phaseLength_);
        phase_ = Phase::Data;
        status_ = 0x58;
        interruptReason_ = 2;
        RaiseInterrupt();
    }

    void AdvanceDataPhase()
    {
        if (responseOffset_ < response_.size()) {
            StartDataPhase();
        } else {
            CompleteCommand();
        }
    }

    void CompleteCommand()
    {
        phase_ = Phase::Idle;
        status_ = 0x50;
        interruptReason_ = 3; // Command completed.
        SetByteCount(0);
        RaiseInterrupt();
    }

    void AbortCommand()
    {
        phase_ = Phase::Idle;
        status_ = 0x51;
        error_ = static_cast<std::uint8_t>((senseKey_ << 4) | 0x04);
        interruptReason_ = 3;
        SetByteCount(0);
        RaiseInterrupt();
    }

    void SetSense(std::uint8_t key, std::uint8_t additional) noexcept
    {
        senseKey_ = key;
        additionalSense_ = additional;
    }

    void BuildIdentifyPacket()
    {
        response_.assign(512, std::byte{0});
        auto setWord = [&](std::size_t index, std::uint16_t value) {
            std::memcpy(response_.data() + index * 2, &value, sizeof(value));
        };
        auto setString = [&](std::size_t firstWord, std::size_t count, const char* value) {
            for (std::size_t index = 0; index < count * 2; ++index) {
                const auto ch = index < std::strlen(value) ? value[index] : ' ';
                auto& byte = response_[(firstWord * 2) + (index ^ 1)];
                byte = static_cast<std::byte>(static_cast<unsigned char>(ch));
            }
        };
        setWord(0, 0x8580); // ATAPI, removable, 12-byte command packets.
        setWord(49, 0x0200);
        setString(10, 10, "VWHP0000000000000001");
        setString(23, 4, "1.0");
        setString(27, 20, "VIRTUAL CD-ROM");
    }

    void CopyScsiString(std::size_t offset, std::size_t length, const char* value)
    {
        for (std::size_t index = 0; index < length; ++index) {
            response_[offset + index] = static_cast<std::byte>(
                index < std::strlen(value) ? value[index] : ' ');
        }
    }

    static std::uint16_t ReadBigEndian16(const std::byte* data) noexcept
    {
        return static_cast<std::uint16_t>((std::to_integer<std::uint8_t>(data[0]) << 8) |
                                          std::to_integer<std::uint8_t>(data[1]));
    }

    static std::uint32_t ReadBigEndian32(const std::byte* data) noexcept
    {
        return (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[0])) << 24) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[1])) << 16) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[2])) << 8) |
               std::to_integer<std::uint8_t>(data[3]);
    }

    static void WriteBigEndian16(std::byte* data, std::uint16_t value) noexcept
    {
        data[0] = static_cast<std::byte>(value >> 8);
        data[1] = static_cast<std::byte>(value);
    }

    static void WriteBigEndian32(std::byte* data, std::uint32_t value) noexcept
    {
        data[0] = static_cast<std::byte>(value >> 24);
        data[1] = static_cast<std::byte>(value >> 16);
        data[2] = static_cast<std::byte>(value >> 8);
        data[3] = static_cast<std::byte>(value);
    }

    static void WriteBigEndian24(std::byte* data, std::uint32_t value) noexcept
    {
        data[0] = static_cast<std::byte>(value >> 16);
        data[1] = static_cast<std::byte>(value >> 8);
        data[2] = static_cast<std::byte>(value);
    }

    virtualiseur::whp::Iso9660Image* image_ = nullptr;
    std::array<std::byte, 12> packet_{};
    std::vector<std::byte> response_;
    std::size_t packetOffset_ = 0;
    std::size_t responseOffset_ = 0;
    std::size_t phaseLength_ = 0;
    std::size_t phaseEnd_ = 0;
    std::uint16_t byteCount_ = 0;
    std::uint16_t transferLimit_ = 0;
    std::uint8_t sectorCount_ = 0x01;
    std::uint8_t error_ = 0;
    std::uint8_t status_ = 0x50;
    std::uint8_t features_ = 0;
    std::uint8_t interruptReason_ = 0;
    std::uint8_t lbaLow_ = 0;
    std::uint8_t lbaMid_ = 0;
    std::uint8_t driveHead_ = 0xA0;
    std::uint8_t deviceControl_ = 0;
    std::uint8_t senseKey_ = 0;
    std::uint8_t additionalSense_ = 0;
    Phase phase_ = Phase::Idle;
    bool irqPending_ = false;
    bool irqRequested_ = false;
    bool packetProtocolStarted_ = false;
};

class PiixBusMasterIde final {
public:
    static constexpr std::uint16_t BasePort = 0xC000;

    bool HandlesPort(std::uint16_t port) const noexcept
    {
        return port >= BasePort && port < BasePort + 16;
    }

    std::uint32_t Read(std::uint16_t port, std::uint8_t size) const noexcept
    {
        std::uint32_t value = 0;
        const auto offset = static_cast<std::uint8_t>(port - BasePort);
        for (std::uint8_t index = 0; index < size && index < 4; ++index) {
            value |= static_cast<std::uint32_t>(ReadByte(static_cast<std::uint8_t>(offset + index)))
                     << (index * 8);
        }
        return value;
    }

    void Write(std::uint16_t port, std::uint8_t size, std::uint32_t value) noexcept
    {
        const auto offset = static_cast<std::uint8_t>(port - BasePort);
        for (std::uint8_t index = 0; index < size && index < 4; ++index) {
            WriteByte(static_cast<std::uint8_t>(offset + index),
                      static_cast<std::uint8_t>(value >> (index * 8)));
        }
    }

    bool IsActive(std::uint8_t channel) const noexcept
    {
        return channel < 2 && (command_[channel] & 0x01) != 0;
    }

    bool ReadsFromDisk(std::uint8_t channel) const noexcept
    {
        return channel < 2 && (command_[channel] & 0x08) != 0;
    }

    std::uint32_t PrdTableAddress(std::uint8_t channel) const noexcept
    {
        return channel < 2 ? prdAddress_[channel] & ~0x03u : 0;
    }

    void Complete(std::uint8_t channel, bool success) noexcept
    {
        if (channel >= 2) return;
        command_[channel] &= static_cast<std::uint8_t>(~0x01);
        status_[channel] &= static_cast<std::uint8_t>(~0x01);
        status_[channel] |= success ? 0x04 : 0x06; // Interrupt, or error plus interrupt.
    }

private:
    std::uint8_t ReadByte(std::uint8_t offset) const noexcept
    {
        const auto channel = static_cast<std::uint8_t>(offset / 8);
        const auto reg = static_cast<std::uint8_t>(offset % 8);
        if (channel >= 2) return 0xFF;
        if (reg == 0) return command_[channel];
        if (reg == 2) return status_[channel];
        if (reg >= 4) return static_cast<std::uint8_t>(prdAddress_[channel] >> ((reg - 4) * 8));
        return 0;
    }

    void WriteByte(std::uint8_t offset, std::uint8_t value) noexcept
    {
        const auto channel = static_cast<std::uint8_t>(offset / 8);
        const auto reg = static_cast<std::uint8_t>(offset % 8);
        if (channel >= 2) return;
        if (reg == 0) {
            command_[channel] = static_cast<std::uint8_t>(value & 0x09);
            if ((command_[channel] & 0x01) != 0) status_[channel] |= 0x01;
            else status_[channel] &= static_cast<std::uint8_t>(~0x01);
        } else if (reg == 2) {
            status_[channel] &= static_cast<std::uint8_t>(~(value & 0x06));
        } else if (reg >= 4) {
            const auto shift = static_cast<std::uint32_t>((reg - 4) * 8);
            prdAddress_[channel] = (prdAddress_[channel] & ~(0xFFu << shift)) |
                                   (static_cast<std::uint32_t>(value) << shift);
        }
    }

    std::array<std::uint8_t, 2> command_{};
    // Primary channel has a DMA-capable master disk; the secondary ATAPI
    // device does not advertise DMA support yet.
    std::array<std::uint8_t, 2> status_{0x20, 0x00};
    std::array<std::uint32_t, 2> prdAddress_{};
};

class LegacyDma final {
public:
    bool HandlesPort(std::uint16_t port) const noexcept
    {
        return port <= 0x0F || (port >= 0x80 && port <= 0x8F);
    }

    std::uint8_t Read(std::uint16_t port)
    {
        if (port >= 0x80) return pageRegisters_[port & 0x0F];
        if (port == 0x08) return 0; // No pending DMA terminal-count status.
        if (port == 0x0D) return 0;
        const auto value = registers_[port];
        if (port <= 0x07) bytePointer_ = !bytePointer_;
        return value;
    }

    void Write(std::uint16_t port, std::uint8_t value)
    {
        if (port >= 0x80) {
            pageRegisters_[port & 0x0F] = value;
            return;
        }
        if (port == 0x0C) {
            bytePointer_ = false;
            return;
        }
        if (port == 0x0D) {
            registers_.fill(0);
            bytePointer_ = false;
            return;
        }
        if (port <= 0x07) bytePointer_ = !bytePointer_;
        registers_[port] = value;
    }

private:
    std::array<std::uint8_t, 16> registers_{};
    std::array<std::uint8_t, 16> pageRegisters_{};
    bool bytePointer_ = false;
};

class Ps2Controller final {
public:
    std::uint8_t ReadStatus() const noexcept
    {
        const std::lock_guard lock(mutex_);
        return static_cast<std::uint8_t>(0x14 | (!output_.empty() ? 0x01 : 0) |
            (inputBufferFull_ ? 0x02 : 0) |
            (!output_.empty() && output_.front().auxiliary ? 0x20 : 0));
    }

    std::uint8_t ReadData() noexcept
    {
        const std::lock_guard lock(mutex_);
        if (output_.empty()) return 0;
        const auto value = output_.front().value;
        output_.pop_front();
        RequestOutputInterrupt();
        return value;
    }

    void WriteCommand(std::uint8_t command) noexcept
    {
        const std::lock_guard lock(mutex_);
        switch (command) {
        case 0x20: QueueOutput(commandByte_); break;
        case 0x60: expectCommandByte_ = true; break;
        case 0xAA: QueueOutput(0x55); break;
        case 0xAB: QueueOutput(0x00); break;
        case 0xA7: auxiliaryEnabled_ = false; commandByte_ |= 0x20; break;
        case 0xA8: auxiliaryEnabled_ = true; commandByte_ &= static_cast<std::uint8_t>(~0x20); break;
        case 0xA9: QueueOutput(0x00); break;
        case 0xAD: keyboardEnabled_ = false; commandByte_ |= 0x10; break;
        case 0xAE: keyboardEnabled_ = true; commandByte_ &= static_cast<std::uint8_t>(~0x10); break;
        case 0xD0: QueueOutput(outputPort_); break;
        case 0xD1: expectOutputPort_ = true; break;
        case 0xD2: expectKeyboardOutput_ = true; break;
        case 0xD3: expectAuxiliaryLoopback_ = true; break;
        case 0xD4: expectAuxiliaryCommand_ = true; break;
        case 0xFE: break; // Reset pulse is ignored in the single-VM bootstrap model.
        default: break;
        }
    }

    void WriteData(std::uint8_t value) noexcept
    {
        const std::lock_guard lock(mutex_);
        if (expectCommandByte_) {
            commandByte_ = value;
            keyboardEnabled_ = (value & 0x10) == 0;
            auxiliaryEnabled_ = (value & 0x20) == 0;
            expectCommandByte_ = false;
            return;
        }
        if (expectOutputPort_) {
            outputPort_ = value;
            expectOutputPort_ = false;
            return;
        }
        if (expectKeyboardOutput_) {
            expectKeyboardOutput_ = false;
            QueueOutput(value);
            return;
        }
        if (expectAuxiliaryLoopback_) {
            expectAuxiliaryLoopback_ = false;
            QueueOutput(value, true);
            return;
        }
        if (expectAuxiliaryCommand_) {
            expectAuxiliaryCommand_ = false;
            // No guest mouse is attached. Return RESEND immediately for every
            // device command (including RESET and GETID) so psmouse abandons
            // probing without waiting for ACK, self-test, or ID bytes.
            QueueOutput(0xFE, true);
            return;
        }
        if (!keyboardEnabled_) return;
        QueueOutput(0xFA); // ACK standard keyboard commands.
        switch (value) {
        case 0xFF: QueueOutput(0xAA); break; // Reset/self-test complete.
        case 0xF2: QueueOutput(0xAB); QueueOutput(0x83); break; // MF2 keyboard ID.
        case 0xF0: awaitingScanSet_ = true; break;
        case 0xED: awaitingLeds_ = true; break;
        default:
            if (awaitingScanSet_) awaitingScanSet_ = false;
            if (awaitingLeds_) awaitingLeds_ = false;
            break;
        }
    }

    void EnqueueScanCode(std::uint8_t value) noexcept
    {
        const std::lock_guard lock(mutex_);
        if (!keyboardEnabled_) return;
        QueueOutput(value);
    }

    std::uint8_t TakeInterruptRequests(std::uint8_t deliverableMask) noexcept
    {
        const std::lock_guard lock(mutex_);
        const auto requests = static_cast<std::uint8_t>((irq1Requested_ ? 0x01 : 0) |
            (irq12Requested_ ? 0x02 : 0)) & deliverableMask;
        if ((requests & 0x01) != 0) irq1Requested_ = false;
        if ((requests & 0x02) != 0) irq12Requested_ = false;
        return static_cast<std::uint8_t>(requests);
    }

private:
    struct OutputByte {
        std::uint8_t value;
        bool auxiliary;
    };

    void RequestOutputInterrupt() noexcept
    {
        if (output_.empty()) return;
        if (output_.front().auxiliary) {
            if (auxiliaryEnabled_ && (commandByte_ & 0x02) != 0) irq12Requested_ = true;
        } else if (keyboardEnabled_ && (commandByte_ & 0x01) != 0) {
            irq1Requested_ = true;
        }
    }

    void QueueOutput(std::uint8_t value, bool auxiliary = false) noexcept
    {
        output_.push_back({value, auxiliary});
        if (output_.size() == 1) RequestOutputInterrupt();
    }

    mutable std::mutex mutex_;
    std::deque<OutputByte> output_;
    // Window input arrives as PC Set-1 scan codes. Advertise the 8042
    // translation mode so Linux's atkbd driver decodes the stream as Set 1.
    std::uint8_t commandByte_ = 0x41;
    std::uint8_t outputPort_ = 0x03;
    bool keyboardEnabled_ = true;
    bool auxiliaryEnabled_ = false;
    bool inputBufferFull_ = false;
    bool expectCommandByte_ = false;
    bool expectOutputPort_ = false;
    bool expectKeyboardOutput_ = false;
    bool expectAuxiliaryLoopback_ = false;
    bool expectAuxiliaryCommand_ = false;
    bool awaitingScanSet_ = false;
    bool awaitingLeds_ = false;
    bool irq1Requested_ = false;
    bool irq12Requested_ = false;
};

class VgaTextRegisters final {
public:
    bool HandlesPort(std::uint16_t port) const noexcept
    {
        return port >= 0x3C0 && port <= 0x3DF;
    }

    std::uint8_t Read(std::uint16_t port) noexcept
    {
        switch (port) {
        case 0x3CC: return miscOutput_;
        case 0x3C0: return attributeIndex_;
        case 0x3C1: return attributeRegisters_[attributeIndex_ & 0x1F];
        case 0x3D4: return crtcIndex_;
        case 0x3D5: return crtcRegisters_[crtcIndex_];
        case 0x3DA:
            attributeDataPhase_ = false;
            return 0x08; // Display-enable / vertical-retrace approximation.
        default: return 0;
        }
    }

    void Write(std::uint16_t port, std::uint8_t value) noexcept
    {
        switch (port) {
        case 0x3C0:
            if (!attributeDataPhase_) attributeIndex_ = value & 0x1F;
            else attributeRegisters_[attributeIndex_ & 0x1F] = value;
            attributeDataPhase_ = !attributeDataPhase_;
            break;
        case 0x3C2: miscOutput_ = value; break;
        case 0x3C4: sequencerIndex_ = value; break;
        case 0x3C5: sequencerRegisters_[sequencerIndex_] = value; break;
        case 0x3CE: graphicsIndex_ = value; break;
        case 0x3CF: graphicsRegisters_[graphicsIndex_] = value; break;
        case 0x3D4: crtcIndex_ = value; break;
        case 0x3D5: crtcRegisters_[crtcIndex_] = value; break;
        default: break;
        }
    }

private:
    std::array<std::uint8_t, 32> attributeRegisters_{};
    std::array<std::uint8_t, 256> sequencerRegisters_{};
    std::array<std::uint8_t, 256> graphicsRegisters_{};
    std::array<std::uint8_t, 256> crtcRegisters_{};
    std::uint8_t attributeIndex_ = 0;
    std::uint8_t sequencerIndex_ = 0;
    std::uint8_t graphicsIndex_ = 0;
    std::uint8_t crtcIndex_ = 0;
    std::uint8_t miscOutput_ = 0;
    bool attributeDataPhase_ = false;
};

struct VgaWindowState {
    const volatile std::uint16_t* textMemory = nullptr;
    Ps2Controller* keyboard = nullptr;
    virtualiseur::whp::Partition* partition = nullptr;
    std::atomic<HWND> window = nullptr;
    std::atomic_bool closed = false;
    HFONT font = nullptr;
};

LRESULT CALLBACK VgaWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<VgaWindowState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<VgaWindowState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wParam, lParam);

    switch (message) {
    case WM_CREATE:
        state->font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            OEM_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");
        SetTimer(window, 1, 40, nullptr);
        return 0;
    case WM_TIMER:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        const auto keyData = static_cast<std::uint32_t>(lParam);
        const auto scan = static_cast<std::uint8_t>((keyData >> 16) & 0xFF);
        if (scan != 0) {
            const bool extended = (keyData & (1u << 24)) != 0;
            if (extended) state->keyboard->EnqueueScanCode(0xE0);
            const bool released = message == WM_KEYUP || message == WM_SYSKEYUP;
            state->keyboard->EnqueueScanCode(static_cast<std::uint8_t>(scan | (released ? 0x80 : 0)));
        }
        return 0;
    }
    case WM_PAINT: {
        constexpr int cellWidth = 10;
        constexpr int cellHeight = 18;
        constexpr std::array<COLORREF, 16> colors{
            RGB(0,0,0), RGB(0,0,170), RGB(0,170,0), RGB(0,170,170),
            RGB(170,0,0), RGB(170,0,170), RGB(170,85,0), RGB(170,170,170),
            RGB(85,85,85), RGB(85,85,255), RGB(85,255,85), RGB(85,255,255),
            RGB(255,85,85), RGB(255,85,255), RGB(255,255,85), RGB(255,255,255)};
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        const auto oldFont = SelectObject(dc, state->font);
        SetBkMode(dc, OPAQUE);
        std::array<std::uint16_t, 80> row{};
        std::array<wchar_t, 80> text{};
        for (int y = 0; y < 25; ++y) {
            for (int x = 0; x < 80; ++x) row[x] = state->textMemory[y * 80 + x];
            int start = 0;
            while (start < 80) {
                const auto attribute = static_cast<std::uint8_t>(row[start] >> 8);
                int end = start + 1;
                while (end < 80 && static_cast<std::uint8_t>(row[end] >> 8) == attribute) ++end;
                for (int x = start; x < end; ++x) {
                    const char ch = static_cast<char>(row[x] & 0xFF);
                    wchar_t wide = L' ';
                    if (ch >= 0x20) (void)MultiByteToWideChar(437, 0, &ch, 1, &wide, 1);
                    text[x - start] = wide;
                }
                SetTextColor(dc, colors[attribute & 0x0F]);
                SetBkColor(dc, colors[(attribute >> 4) & 0x07]);
                RECT run{start * cellWidth, y * cellHeight, end * cellWidth, (y + 1) * cellHeight};
                ExtTextOutW(dc, run.left, run.top, ETO_OPAQUE, &run,
                    text.data(), static_cast<UINT>(end - start), nullptr);
                start = end;
            }
        }
        SelectObject(dc, oldFont);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        state->closed.store(true, std::memory_order_release);
        (void)state->partition->CancelRunVirtualProcessor(0);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        if (state->font != nullptr) {
            DeleteObject(state->font);
            state->font = nullptr;
        }
        state->closed.store(true, std::memory_order_release);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

class VgaTextWindow final {
public:
    VgaTextWindow(virtualiseur::whp::Partition& partition, Ps2Controller& keyboard,
                  const volatile std::uint16_t* textMemory)
        : state_{textMemory, &keyboard, &partition}
    {
        thread_ = std::jthread([this](std::stop_token) {
            const auto instance = GetModuleHandleW(nullptr);
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.lpfnWndProc = VgaWindowProcedure;
            windowClass.hInstance = instance;
            windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
            windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            windowClass.lpszClassName = L"VirtualiseurGuestVgaText";
            if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                state_.closed.store(true, std::memory_order_release);
                return;
            }
            RECT bounds{0, 0, 800, 450};
            AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
            HWND window = CreateWindowExW(0, windowClass.lpszClassName,
                L"Virtualiseur \u2014 \u00e9cran invit\u00e9 (VGA texte)", WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left,
                bounds.bottom - bounds.top, nullptr, nullptr, instance, &state_);
            if (window == nullptr) {
                state_.closed.store(true, std::memory_order_release);
                return;
            }
            state_.window.store(window, std::memory_order_release);
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        });
    }

    ~VgaTextWindow()
    {
        thread_.request_stop();
        (void)state_.partition->CancelRunVirtualProcessor(0);
        if (const auto window = state_.window.load(std::memory_order_acquire)) {
            (void)PostMessageW(window, WM_CLOSE, 0, 0);
        }
        if (thread_.joinable()) thread_.join();
    }

    bool IsClosed() const noexcept { return state_.closed.load(std::memory_order_acquire); }

private:
    VgaWindowState state_;
    std::jthread thread_;
};

class Pit8254 final {
public:
    std::uint8_t Read(std::uint16_t port)
    {
        const std::lock_guard lock(mutex_);
        if (port != 0x40 && port != 0x42) return 0;
        const bool channelTwo = port == 0x42;
        const auto count = channelTwo ? CurrentChannelTwoCount() : CurrentCount();
        auto& accessMode = channelTwo ? channelTwoAccessMode_ : accessMode_;
        auto& readHighByte = channelTwo ? channelTwoReadHighByte_ : readHighByte_;
        switch (accessMode) {
        case 1: return static_cast<std::uint8_t>(count & 0xFF);
        case 2: return static_cast<std::uint8_t>(count >> 8);
        case 3: {
            const auto result = readHighByte ? static_cast<std::uint8_t>(count >> 8)
                                             : static_cast<std::uint8_t>(count & 0xFF);
            readHighByte = !readHighByte;
            return result;
        }
        default: return 0;
        }
    }

    void Write(std::uint16_t port, std::uint8_t value)
    {
        const std::lock_guard lock(mutex_);
        if (port == 0x43) {
            selectedChannel_ = static_cast<std::uint8_t>(value >> 6);
            if (selectedChannel_ == 0 || selectedChannel_ == 2) {
                accessMode_ = static_cast<std::uint8_t>((value >> 4) & 0x03);
                writeHighByte_ = false;
                readHighByte_ = false;
                if (accessMode_ == 0) accessMode_ = 3; // Counter latch: use low/high reads.
                if (selectedChannel_ == 2) {
                    channelTwoAccessMode_ = accessMode_;
                    channelTwoMode_ = static_cast<std::uint8_t>((value >> 1) & 0x07);
                    channelTwoWriteHighByte_ = false;
                    channelTwoReadHighByte_ = false;
                }
            }
            return;
        }
        if (port != 0x40 && port != 0x42) return;

        if (port == 0x42) {
            WriteChannelTwo(value);
            return;
        }

        switch (accessMode_) {
        case 1:
            reload_ = value;
            Restart();
            break;
        case 2:
            reload_ = static_cast<std::uint16_t>(value << 8);
            Restart();
            break;
        case 3:
            if (!writeHighByte_) {
                reload_ = static_cast<std::uint16_t>((reload_ & 0xFF00) | value);
                writeHighByte_ = true;
            } else {
                reload_ = static_cast<std::uint16_t>((reload_ & 0x00FF) |
                                                     (static_cast<std::uint16_t>(value) << 8));
                writeHighByte_ = false;
                Restart();
            }
            break;
        default:
            break;
        }
    }

    std::chrono::microseconds InterruptPeriod() const
    {
        const std::lock_guard lock(mutex_);
        const auto count = reload_ == 0 ? 65536ull : reload_;
        const auto periodUs = (count * 1000000ull) / kPitFrequency;
        return std::chrono::microseconds(periodUs < 100 ? 100 : periodUs);
    }

    void SetChannelTwoGate(bool enabled)
    {
        const std::lock_guard lock(mutex_);
        if (enabled != channelTwoGate_) {
            channelTwoGate_ = enabled;
            channelTwoStartedAt_ = std::chrono::steady_clock::now();
        }
    }

    bool ChannelTwoOutput() const
    {
        const std::lock_guard lock(mutex_);
        if (!channelTwoGate_ || channelTwoReload_ == 0) return false;
        const auto period = channelTwoReload_ == 0 ? 65536ull : channelTwoReload_;
        const auto elapsed = std::chrono::steady_clock::now() - channelTwoStartedAt_;
        const auto ticks = static_cast<std::uint64_t>(
            std::chrono::duration<double>(elapsed).count() * kPitFrequency);
        if (channelTwoMode_ == 0) return ticks >= period; // Mode 0 one-shot.
        if (channelTwoMode_ == 4) return ticks >= period; // Mode 4 software strobe.
        return (ticks % period) < 2; // Approximate OUT pulse for periodic modes.
    }

    std::array<std::uint64_t, 4> ChannelTwoSnapshot() const
    {
        const std::lock_guard lock(mutex_);
        const auto period = channelTwoReload_ == 0 ? 65536ull : channelTwoReload_;
        const auto elapsed = std::chrono::steady_clock::now() - channelTwoStartedAt_;
        const auto ticks = static_cast<std::uint64_t>(
            std::chrono::duration<double>(elapsed).count() * kPitFrequency);
        const bool output = channelTwoMode_ == 0 || channelTwoMode_ == 4
            ? channelTwoGate_ && ticks >= period
            : channelTwoGate_ && ((ticks % period) < 2);
        return {channelTwoReload_, channelTwoMode_, channelTwoGate_ ? 1ull : 0ull, output ? 1ull : 0ull};
    }

private:
    static constexpr std::uint64_t kPitFrequency = 1193182;

    std::uint16_t CurrentCount() const
    {
        const auto period = reload_ == 0 ? 65536ull : reload_;
        const auto elapsed = std::chrono::steady_clock::now() - startedAt_;
        const auto ticks = static_cast<std::uint64_t>(
            std::chrono::duration<double>(elapsed).count() * kPitFrequency);
        const auto remaining = period - (ticks % period);
        return static_cast<std::uint16_t>(remaining & 0xFFFF);
    }

    std::uint16_t CurrentChannelTwoCount() const
    {
        const auto period = channelTwoReload_ == 0 ? 65536ull : channelTwoReload_;
        if (!channelTwoGate_) return static_cast<std::uint16_t>(period & 0xFFFF);
        const auto elapsed = std::chrono::steady_clock::now() - channelTwoStartedAt_;
        const auto ticks = static_cast<std::uint64_t>(
            std::chrono::duration<double>(elapsed).count() * kPitFrequency);
        const auto remaining = ticks >= period &&
            (channelTwoMode_ == 0 || channelTwoMode_ == 4)
            ? 0ull : period - (ticks % period);
        return static_cast<std::uint16_t>(remaining & 0xFFFF);
    }

    void WriteChannelTwo(std::uint8_t value)
    {
        switch (channelTwoAccessMode_) {
        case 1:
            channelTwoReload_ = value;
            channelTwoGate_ = false;
            break;
        case 2:
            channelTwoReload_ = static_cast<std::uint16_t>(value << 8);
            channelTwoGate_ = false;
            break;
        case 3:
            if (!channelTwoWriteHighByte_) {
                channelTwoReload_ = static_cast<std::uint16_t>((channelTwoReload_ & 0xFF00) | value);
                channelTwoWriteHighByte_ = true;
            } else {
                channelTwoReload_ = static_cast<std::uint16_t>((channelTwoReload_ & 0x00FF) |
                    (static_cast<std::uint16_t>(value) << 8));
                channelTwoWriteHighByte_ = false;
                channelTwoGate_ = false;
            }
            break;
        default: break;
        }
    }

    void Restart() noexcept { startedAt_ = std::chrono::steady_clock::now(); }

    mutable std::mutex mutex_;
    std::uint16_t reload_ = 0;
    std::uint16_t channelTwoReload_ = 0;
    std::uint8_t accessMode_ = 3;
    std::uint8_t channelTwoAccessMode_ = 3;
    std::uint8_t channelTwoMode_ = 3;
    std::uint8_t selectedChannel_ = 0;
    bool writeHighByte_ = false;
    mutable bool readHighByte_ = false;
    bool channelTwoWriteHighByte_ = false;
    bool channelTwoReadHighByte_ = false;
    bool channelTwoGate_ = false;
    std::chrono::steady_clock::time_point startedAt_ = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point channelTwoStartedAt_ = std::chrono::steady_clock::now();
};

class CmosRtc final {
public:
    explicit CmosRtc(std::uint64_t guestRamSizeBytes)
    {
        registers_[0x0A] = 0x26;
        registers_[0x0B] = 0x02; // BCD encoding, 24-hour clock.
        registers_[0x0D] = 0x80; // Battery status valid.
        SetMemorySize(guestRamSizeBytes);
    }

    void Select(std::uint8_t value) noexcept
    {
        nmiDisabled_ = (value & 0x80) != 0;
        index_ = value & 0x7F;
    }

    std::uint8_t ReadIndex() const noexcept
    {
        return static_cast<std::uint8_t>(index_ | (nmiDisabled_ ? 0x80 : 0));
    }

    std::uint8_t ReadData()
    {
        if (index_ == 0x0C) {
            registers_[0x0C] = 0;
            return 0;
        }
        if (index_ == 0x0A) return static_cast<std::uint8_t>(registers_[0x0A] & 0x7F);
        if (index_ >= 0x00 && index_ <= 0x09) return ReadClockRegister(index_);
        return registers_[index_];
    }

    void WriteData(std::uint8_t value) noexcept
    {
        if (index_ == 0x0C || index_ == 0x0D) return;
        registers_[index_] = value;
    }

private:
    static std::uint8_t ToBcd(unsigned value) noexcept
    {
        return static_cast<std::uint8_t>(((value / 10) << 4) | (value % 10));
    }

    std::uint8_t Encode(unsigned value) const noexcept
    {
        return (registers_[0x0B] & 0x04) != 0 ? static_cast<std::uint8_t>(value) : ToBcd(value);
    }

    std::uint8_t ReadClockRegister(std::uint8_t reg) const
    {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm localTime{};
        localtime_s(&localTime, &now);
        const unsigned fullYear = static_cast<unsigned>(localTime.tm_year + 1900);
        switch (reg) {
        case 0x00: return Encode(static_cast<unsigned>(localTime.tm_sec));
        case 0x02: return Encode(static_cast<unsigned>(localTime.tm_min));
        case 0x04: return Encode(static_cast<unsigned>(localTime.tm_hour));
        case 0x06: return Encode(static_cast<unsigned>(localTime.tm_wday + 1));
        case 0x07: return Encode(static_cast<unsigned>(localTime.tm_mday));
        case 0x08: return Encode(static_cast<unsigned>(localTime.tm_mon + 1));
        case 0x09: return Encode(fullYear % 100);
        default: return registers_[reg];
        }
    }

    void SetMemorySize(std::uint64_t bytes) noexcept
    {
        constexpr std::uint64_t oneMiB = 1024ull * 1024;
        constexpr std::uint64_t sixteenMiB = 16ull * oneMiB;
        constexpr std::uint64_t sixtyFourKiB = 64ull * 1024;
        constexpr std::uint64_t max16Bit = 0xFFFF;
        const auto baseMemoryKiB = 640u;
        registers_[0x15] = static_cast<std::uint8_t>(baseMemoryKiB & 0xFF);
        registers_[0x16] = static_cast<std::uint8_t>(baseMemoryKiB >> 8);
        const auto extBelow16MiB = bytes > oneMiB ?
            (std::min<std::uint64_t>)(15ull * 1024, (bytes - oneMiB) / 1024) : 0;
        registers_[0x17] = static_cast<std::uint8_t>(extBelow16MiB & 0xFF);
        registers_[0x18] = static_cast<std::uint8_t>(extBelow16MiB >> 8);
        registers_[0x30] = registers_[0x17];
        registers_[0x31] = registers_[0x18];
        const auto extAbove16MiB = bytes > sixteenMiB ?
            (std::min<std::uint64_t>)(max16Bit, (bytes - sixteenMiB) / sixtyFourKiB) : 0;
        registers_[0x34] = static_cast<std::uint8_t>(extAbove16MiB & 0xFF);
        registers_[0x35] = static_cast<std::uint8_t>(extAbove16MiB >> 8);
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm localTime{};
        localtime_s(&localTime, &now);
        registers_[0x32] = ToBcd(static_cast<unsigned>(localTime.tm_year + 1900) / 100);
    }

    std::array<std::uint8_t, 128> registers_{};
    std::uint8_t index_ = 0;
    bool nmiDisabled_ = false;
};

class LocalApic final {
public:
    LocalApic()
    {
        registers_[0x20 / 16] = 0;
        registers_[0x30 / 16] = 0x00050014;
        registers_[0x80 / 16] = 0;
        registers_[0xA0 / 16] = 0;
        registers_[0xD0 / 16] = 0;
        registers_[0xE0 / 16] = 0xFFFFFFFF;
        registers_[0xF0 / 16] = 0x000001FF;
    }

    bool Contains(std::uint64_t gpa) const noexcept
    {
        return gpa >= 0xFEE00000ull && gpa < 0xFEE01000ull;
    }

    HRESULT Access(WHV_EMULATOR_MEMORY_ACCESS_INFO& access)
    {
        if (!Contains(access.GpaAddress) || access.AccessSize == 0 || access.AccessSize > sizeof(std::uint32_t)) {
            return E_INVALIDARG;
        }
        const auto offset = static_cast<std::uint32_t>(access.GpaAddress - 0xFEE00000ull);
        const auto registerOffset = offset & ~0x0Fu;
        if ((offset & 0x0F) + access.AccessSize > sizeof(std::uint32_t)) return E_INVALIDARG;
        auto& value = registers_[registerOffset / 16];
        const auto byteShift = (offset & 0x0F) * 8;
        if (access.Direction == 0) {
            for (std::uint8_t index = 0; index < access.AccessSize; ++index) {
                access.Data[index] = static_cast<std::uint8_t>(value >> (byteShift + index * 8));
            }
        } else {
            std::uint32_t written = 0;
            for (std::uint8_t index = 0; index < access.AccessSize; ++index) {
                written |= static_cast<std::uint32_t>(access.Data[index]) << (index * 8);
            }
            const auto widthMask = access.AccessSize == sizeof(std::uint32_t)
                ? 0xFFFFFFFFu
                : ((1u << (access.AccessSize * 8)) - 1u);
            const auto mask = widthMask << byteShift;
            if (registerOffset == 0x0B0) {
                registers_[0x0B0 / 16] = 0; // EOI clears in-service state in this single-vCPU model.
                ++eoiCount_;
            } else if (registerOffset != 0x020 && registerOffset != 0x030) {
                value = (value & ~mask) | ((written << byteShift) & mask);
            }
        }
        return S_OK;
    }

    std::uint64_t EoiCount() const noexcept { return eoiCount_; }
    std::uint32_t Register(std::uint32_t offset) const noexcept
    {
        return registers_[(offset & ~0x0Fu) / 16];
    }
    bool SoftwareEnabled() const noexcept { return (Register(0x0F0) & 0x100) != 0; }

private:
    std::array<std::uint32_t, 256> registers_{};
    std::uint64_t eoiCount_ = 0;
};

struct EmulatorContext {
    virtualiseur::whp::Partition* partition = nullptr;
    LocalApic apic;
    std::uint32_t virtualProcessorIndex = 0;
};

HRESULT CALLBACK EmulatorMemoryCallback(
    void* opaque,
    WHV_EMULATOR_MEMORY_ACCESS_INFO* access)
{
    auto& context = *static_cast<EmulatorContext*>(opaque);
    if (context.apic.Contains(access->GpaAddress)) {
        const auto result = context.apic.Access(*access);
        if (SUCCEEDED(result) && access->Direction != 0) {
            const auto offset = static_cast<std::uint32_t>(
                (access->GpaAddress - 0xFEE00000ull) & ~0x0Full);
            if (offset == 0x0B0) {
                context.partition->AcknowledgeExternalInterrupt(context.virtualProcessorIndex);
            } else {
                std::optional<WHV_REGISTER_NAME> registerName;
                switch (offset) {
                case 0x080: registerName = WHvX64RegisterApicTpr; break;
                case 0x0D0: registerName = WHvX64RegisterApicLdr; break;
                case 0x0F0: registerName = WHvX64RegisterApicSpurious; break;
                case 0x320: registerName = WHvX64RegisterApicLvtTimer; break;
                case 0x330: registerName = WHvX64RegisterApicLvtThermal; break;
                case 0x340: registerName = WHvX64RegisterApicLvtPerfmon; break;
                case 0x350: registerName = WHvX64RegisterApicLvtLint0; break;
                case 0x360: registerName = WHvX64RegisterApicLvtLint1; break;
                case 0x370: registerName = WHvX64RegisterApicLvtError; break;
                case 0x380: registerName = WHvX64RegisterApicInitCount; break;
                case 0x3E0: registerName = WHvX64RegisterApicDivide; break;
                default: break;
                }
                if (registerName) {
                    context.partition->SetLocalApicRegister(
                        context.virtualProcessorIndex, offset, context.apic.Register(offset));
                }
            }
        }
        return result;
    }
    auto* hostAddress = static_cast<std::uint8_t*>(
        context.partition->GetHostAddressForGuestPhysicalAddress(access->GpaAddress, access->AccessSize));
    if (hostAddress == nullptr) return E_FAIL;
    if (access->Direction == 0) {
        std::memcpy(access->Data, hostAddress, access->AccessSize);
    } else {
        std::memcpy(hostAddress, access->Data, access->AccessSize);
    }
    return S_OK;
}

HRESULT CALLBACK EmulatorIoCallback(void*, WHV_EMULATOR_IO_ACCESS_INFO*)
{
    return E_NOTIMPL;
}

HRESULT CALLBACK EmulatorGetRegisters(
    void* opaque,
    const WHV_REGISTER_NAME* names,
    UINT32 count,
    WHV_REGISTER_VALUE* values)
{
    try {
        auto& context = *static_cast<EmulatorContext*>(opaque);
        const auto registers = context.partition->GetVirtualProcessorRegisters(
            context.virtualProcessorIndex, std::span(names, count));
        std::copy(registers.begin(), registers.end(), values);
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT CALLBACK EmulatorSetRegisters(
    void* opaque,
    const WHV_REGISTER_NAME* names,
    UINT32 count,
    const WHV_REGISTER_VALUE* values)
{
    try {
        auto& context = *static_cast<EmulatorContext*>(opaque);
        context.partition->SetVirtualProcessorRegisters(
            context.virtualProcessorIndex, std::span(names, count), std::span(values, count));
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT CALLBACK EmulatorTranslateGvaPage(
    void* opaque,
    WHV_GUEST_VIRTUAL_ADDRESS gva,
    WHV_TRANSLATE_GVA_FLAGS flags,
    WHV_TRANSLATE_GVA_RESULT_CODE* resultCode,
    WHV_GUEST_PHYSICAL_ADDRESS* gpa)
{
    try {
        auto& context = *static_cast<EmulatorContext*>(opaque);
        const auto result = context.partition->TranslateGuestVirtualAddress(
            context.virtualProcessorIndex, gva, flags, *gpa);
        *resultCode = result.ResultCode;
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

class InstructionEmulator final {
public:
    InstructionEmulator()
    {
        WHV_EMULATOR_CALLBACKS callbacks{};
        callbacks.Size = sizeof(callbacks);
        callbacks.WHvEmulatorIoPortCallback = EmulatorIoCallback;
        callbacks.WHvEmulatorMemoryCallback = EmulatorMemoryCallback;
        callbacks.WHvEmulatorGetVirtualProcessorRegisters = EmulatorGetRegisters;
        callbacks.WHvEmulatorSetVirtualProcessorRegisters = EmulatorSetRegisters;
        callbacks.WHvEmulatorTranslateGvaPage = EmulatorTranslateGvaPage;
        const auto result = WHvEmulatorCreateEmulator(&callbacks, &handle_);
        if (FAILED(result)) {
            throw std::runtime_error("WHvEmulatorCreateEmulator a echoue.");
        }
    }

    ~InstructionEmulator()
    {
        if (handle_ != nullptr) (void)WHvEmulatorDestroyEmulator(handle_);
    }

    InstructionEmulator(const InstructionEmulator&) = delete;
    InstructionEmulator& operator=(const InstructionEmulator&) = delete;

    WHV_EMULATOR_HANDLE Get() const noexcept { return handle_; }

private:
    WHV_EMULATOR_HANDLE handle_ = nullptr;
};

std::uint64_t ParseRamSize(const char* value)
{
    const auto mib = std::stoull(value);
    if (mib < 64 || mib > 4096) {
        throw std::invalid_argument("La RAM doit etre comprise entre 64 et 4096 MiB.");
    }
    return mib * 1024ull * 1024ull;
}

std::uint64_t ParseDiskSize(const char* value)
{
    const auto mib = std::stoull(value);
    if (mib < 16 || mib > 1024ull * 1024) {
        throw std::invalid_argument("La taille du disque doit etre comprise entre 16 MiB et 1 TiB.");
    }
    return mib * 1024ull * 1024ull;
}

bool HasKernelArgument(const std::string& commandLine, std::string_view name)
{
    std::size_t begin = 0;
    while (begin < commandLine.size()) {
        while (begin < commandLine.size() && commandLine[begin] == ' ') ++begin;
        const auto end = commandLine.find(' ', begin);
        const auto length = (end == std::string::npos ? commandLine.size() : end) - begin;
        const std::string_view argument(commandLine.data() + begin, length);
        if (argument == name ||
            (argument.size() > name.size() && argument.substr(0, name.size()) == name &&
             argument[name.size()] == '=')) {
            return true;
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return false;
}

void AppendKernelArgument(std::string& commandLine, std::string_view argument)
{
    if (!commandLine.empty()) commandLine.push_back(' ');
    commandLine.append(argument);
}

struct LinuxBootFiles {
    std::filesystem::path kernel;
    std::filesystem::path initrd;
    std::string kernelIsoPath;
    std::string initrdIsoPath;
};

LinuxBootFiles ExtractLinuxBootFilesFromIso(const std::filesystem::path& isoPath)
{
    const std::array<std::pair<const char*, const char*>, 7> candidates{{
        {"casper/vmlinuz", "casper/initrd"},
        {"live/vmlinuz", "live/initrd.img"},
        {"live/vmlinuz", "live/initrd"},
        {"install.amd/vmlinuz", "install.amd/initrd.gz"},
        {"isolinux/vmlinuz", "isolinux/initrd.img"},
        {"boot/vmlinuz", "boot/initrd.img"},
        {"arch/boot/x86_64/vmlinuz-linux", "arch/boot/x86_64/archiso.img"},
    }};

    virtualiseur::whp::Iso9660Image iso(isoPath);
    const auto extractionDirectory = std::filesystem::temp_directory_path() /
        L"Virtualiseur" / L"LinuxBootProbe" / std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directories(extractionDirectory);

    for (const auto& [kernelInIso, initrdInIso] : candidates) {
        if (!iso.ContainsFile(kernelInIso) || !iso.ContainsFile(initrdInIso)) continue;
        LinuxBootFiles files{
            extractionDirectory / L"vmlinuz",
            extractionDirectory / L"initrd.img",
            kernelInIso,
            initrdInIso,
        };
        const auto kernelBytes = iso.ExtractFile(kernelInIso, files.kernel);
        const auto initrdBytes = iso.ExtractFile(initrdInIso, files.initrd);
        std::cout << "Fichiers de démarrage extraits automatiquement de l'ISO :\n"
                  << "  noyau: " << files.kernelIsoPath << " (" << kernelBytes << " octets)\n"
                  << "  initrd: " << files.initrdIsoPath << " (" << initrdBytes << " octets)\n";
        return files;
    }
    throw std::runtime_error(
        "ISO ISO9660 reconnue, mais aucun couple noyau/initrd Linux standard n'a été trouvé.");
}

void AdvanceRip(virtualiseur::whp::Partition& partition, const WHV_RUN_VP_EXIT_CONTEXT& exit)
{
    const auto length = exit.VpContext.InstructionLength;
    if (length == 0 || length > 15) {
        throw std::runtime_error("WHP a retourne une longueur d'instruction invalide.");
    }
    const WHV_REGISTER_NAME name = WHvX64RegisterRip;
    WHV_REGISTER_VALUE value{};
    value.Reg64 = exit.VpContext.Rip + length;
    partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
}

void VerifyAtapiCdrom(const std::filesystem::path& isoPath)
{
    virtualiseur::whp::Iso9660Image image(isoPath);
    AtapiCdrom cdrom(&image);
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };

    require(cdrom.Read(0x177, 1) == 0, "Status initial ATAPI attendu: 00h.");
    require(cdrom.Read(0x172, 1) == 1 && cdrom.Read(0x173, 1) == 1 &&
            cdrom.Read(0x174, 1) == 0x14 && cdrom.Read(0x175, 1) == 0xEB,
            "Signature de reset ATAPI incorrecte.");

    cdrom.Write(0x172, 1, 0x55);
    cdrom.Write(0x173, 1, 0xAA);
    require(cdrom.Read(0x172, 1) == 0x55 && cdrom.Read(0x173, 1) == 0xAA,
            "Echec du test de présence ATA par motifs de registres.");
    cdrom.Write(0x376, 1, 0x04);
    cdrom.Write(0x376, 1, 0x00);

    cdrom.Write(0x177, 1, 0xA1);
    (void)cdrom.TakeInterruptRequest();
    const auto identifyWord = cdrom.Read(0x170, 2);
    require(identifyWord == 0x8580, "IDENTIFY PACKET doit annoncer un lecteur ATAPI 12 octets.");
    for (std::size_t word = 1; word < 256; ++word) (void)cdrom.Read(0x170, 2);
    (void)cdrom.TakeInterruptRequest();

    const auto readPacketData = [&](const std::array<std::uint8_t, 12>& cdb,
                                    std::size_t expectedBytes) {
        cdrom.Write(0x174, 1, 0x00);
        cdrom.Write(0x175, 1, 0x20); // 8192-byte maximum PIO phase.
        cdrom.Write(0x177, 1, 0xA0);
        (void)cdrom.TakeInterruptRequest();
        for (std::size_t index = 0; index < cdb.size(); index += 2) {
            cdrom.Write(0x170, 2,
                static_cast<std::uint32_t>(cdb[index]) |
                (static_cast<std::uint32_t>(cdb[index + 1]) << 8));
        }
        (void)cdrom.TakeInterruptRequest();
        std::vector<std::byte> response(expectedBytes);
        for (std::size_t offset = 0; offset < response.size(); offset += 2) {
            const auto word = static_cast<std::uint16_t>(cdrom.Read(0x170, 2));
            response[offset] = static_cast<std::byte>(word & 0xFF);
            if (offset + 1 < response.size()) {
                response[offset + 1] = static_cast<std::byte>(word >> 8);
            }
        }
        (void)cdrom.TakeInterruptRequest();
        require(cdrom.Read(0x177, 1) == 0x50, "La commande ATAPI doit finir sans erreur.");
        return response;
    };

    std::array<std::uint8_t, 12> inquiry{};
    inquiry[0] = 0x12;
    inquiry[4] = 36;
    const auto inquiryData = readPacketData(inquiry, 36);
    require(inquiryData[0] == std::byte{0x05} &&
            std::string(reinterpret_cast<const char*>(inquiryData.data() + 16), 14) == "VIRTUAL CD-ROM",
            "INQUIRY ne décrit pas le lecteur virtuel.");

    std::array<std::uint8_t, 12> capacity{};
    capacity[0] = 0x25;
    const auto capacityData = readPacketData(capacity, 8);
    require(capacityData[4] == std::byte{0} && capacityData[5] == std::byte{0} &&
            capacityData[6] == std::byte{8} && capacityData[7] == std::byte{0},
            "READ CAPACITY doit annoncer des blocs de 2048 octets.");

    std::array<std::uint8_t, 12> readPvd{};
    readPvd[0] = 0x28;
    readPvd[5] = 16; // ISO 9660 Primary Volume Descriptor is logical block 16.
    readPvd[8] = 1;
    const auto pvd = readPacketData(readPvd, 2048);
    require(pvd[0] == std::byte{1} && pvd[1] == std::byte{'C'} &&
            pvd[2] == std::byte{'D'} && pvd[3] == std::byte{'0'} && pvd[4] == std::byte{'0'} &&
            pvd[5] == std::byte{'1'}, "READ(10) ne renvoie pas le descripteur ISO9660.");
    std::cout << "ATAPI OK: signature, test de presence, IDENTIFY PACKET, INQUIRY, "
                 "READ CAPACITY et READ(10) ISO 9660.\n";
}

int Run(int argc, char** argv)
{
    if (argc == 3 && std::string_view(argv[1]) == "--verify-atapi") {
        VerifyAtapiCdrom(std::filesystem::path(argv[2]));
        return 0;
    }
    if (argc < 2 || argc > 13) {
        std::cerr << "Usage:\n"
                  << "  LinuxBootProbe.exe <bzImage> [initrd] [RAM_MiB] [cmdline]\n"
                  << "  LinuxBootProbe.exe --iso <linux.iso> [RAM_MiB] [cmdline] "
                     "[--disk <raw.img> <size_MiB>] [--vga] [--net slirp]\n"
                  << "  LinuxBootProbe.exe --verify-atapi <linux.iso>\n";
        return 2;
    }

    std::filesystem::path kernelPath;
    std::filesystem::path initrdPath;
    std::filesystem::path isoPath;
    std::filesystem::path diskPath;
    std::uint64_t diskSizeBytes = 0;
    std::uint64_t ramSize = 512ull * 1024 * 1024;
    std::string commandLine =
        "console=ttyS0,115200n8 earlyprintk=serial,ttyS0,115200 rdinit=/init lapic tsc=reliable clocksource=jiffies";
    bool extractedFromIso = false;
    bool useVgaWindow = false;
    bool slirpRequested = false;
    if (std::string_view(argv[1]) == "--iso") {
        if (argc < 3) {
            throw std::invalid_argument("Indique le chemin de l'image ISO après --iso.");
        }
        isoPath = std::filesystem::path(argv[2]);
        auto files = ExtractLinuxBootFilesFromIso(isoPath);
        kernelPath = std::move(files.kernel);
        initrdPath = std::move(files.initrd);
        extractedFromIso = true;
        if (argc >= 4) ramSize = ParseRamSize(argv[3]);
        int nextArgument = 4;
        if (argc > nextArgument && std::string_view(argv[nextArgument]).substr(0, 2) != "--") {
            commandLine = argv[nextArgument++];
        }
        while (nextArgument < argc) {
            const std::string_view option(argv[nextArgument]);
            if (option == "--disk" && nextArgument + 2 < argc) {
                diskPath = argv[nextArgument + 1];
                diskSizeBytes = ParseDiskSize(argv[nextArgument + 2]);
                nextArgument += 3;
            } else if (option == "--vga") {
                useVgaWindow = true;
                ++nextArgument;
            } else if (option == "--net" && nextArgument + 1 < argc &&
                       std::string_view(argv[nextArgument + 1]) == "slirp") {
                slirpRequested = true;
                nextArgument += 2;
            } else {
                throw std::invalid_argument("Option ISO inconnue ou incomplète : " + std::string(option));
            }
        }
    } else {
        kernelPath = argv[1];
        if (argc >= 3) initrdPath = argv[2];
        if (argc >= 4) ramSize = ParseRamSize(argv[3]);
        if (argc >= 5) commandLine = argv[4];
    }

    if (extractedFromIso) {
        // The probe has a serial console but no VGA text adapter yet. Debian's
        // installer otherwise switches its frontend to a virtual terminal
        // after /init and appears to stop producing output on COM1.
        if (!HasKernelArgument(commandLine, "console")) {
            AppendKernelArgument(commandLine, "console=ttyS0,115200n8");
        }
        if (useVgaWindow && !HasKernelArgument(commandLine, "console=tty0")) {
            AppendKernelArgument(commandLine, "console=tty0");
        }
        if (!HasKernelArgument(commandLine, "DEBIAN_FRONTEND")) {
            AppendKernelArgument(commandLine, "DEBIAN_FRONTEND=text");
        }
        if (!HasKernelArgument(commandLine, "TERM")) {
            AppendKernelArgument(commandLine, "TERM=vt102");
        }
        for (const auto option : {"earlyprintk=serial,ttyS0,115200", "rdinit=/init",
                                  "lapic", "tsc=reliable", "clocksource=jiffies",
                                  "i8042.noaux=1"}) {
            const auto nameLength = std::string_view(option).find('=');
            const std::string_view name(option, nameLength == std::string_view::npos
                ? std::string_view(option).size() : nameLength);
            if (!HasKernelArgument(commandLine, name)) AppendKernelArgument(commandLine, option);
        }
    }

    const auto capabilities = virtualiseur::whp::QueryHostCapabilities();
    if (FAILED(capabilities.status) || !capabilities.apiAvailable || !capabilities.hypervisorPresent) {
        std::cerr << "WHP n'est pas disponible (HRESULT 0x" << std::hex
                  << static_cast<unsigned long>(capabilities.status) << ").\n";
        return 3;
    }

    std::optional<virtualiseur::whp::VirtualDisk> virtualDisk;
    if (!diskPath.empty()) {
        if (std::filesystem::exists(diskPath)) {
            virtualDisk.emplace(virtualiseur::whp::VirtualDisk::Open(diskPath));
            std::cout << "Disque IDE ouvert: " << diskPath.string() << " ("
                      << virtualDisk->SizeBytes() / (1024 * 1024) << " MiB).\n";
        } else {
            virtualDisk.emplace(
                virtualiseur::whp::VirtualDisk::CreateSparse(diskPath, diskSizeBytes));
            std::cout << "Disque IDE sparse cree: " << diskPath.string() << " ("
                      << diskSizeBytes / (1024 * 1024) << " MiB).\n";
        }
    }
    std::optional<virtualiseur::whp::Iso9660Image> isoMedia;
    if (extractedFromIso) {
        isoMedia.emplace(isoPath);
        std::cout << "Lecteur CD-ROM ATAPI raccorde: " << isoPath.string() << " ("
                  << isoMedia->SizeBytes() / (1024 * 1024) << " MiB).\n";
    }

    virtualiseur::whp::Partition partition(1, true);
    virtualiseur::whp::LinuxBootImage image(
        partition, kernelPath, initrdPath, commandLine, ramSize);
    image.ConfigureBootVcpu(0);
    {
        // WHP routes WHvRequestInterrupt through its emulated local APIC. Keep
        // that controller able to accept vectors during the early Linux boot,
        // before the guest has discovered ACPI and programmed its own SVR.
        partition.SetLocalApicRegister(0, 0x0F0, 0x000001FF);
    }
    EmulatorContext emulatorContext{&partition, {}, 0};
    InstructionEmulator instructionEmulator;
    if (extractedFromIso) {
        std::error_code ignored;
        std::filesystem::remove(kernelPath, ignored);
        std::filesystem::remove(initrdPath, ignored);
        std::filesystem::remove(kernelPath.parent_path(), ignored);
    }

    std::cout << "Noyau Linux charge en RAM invite: GPA=0x" << std::hex << image.KernelLoadAddress()
              << ", taille=0x" << image.KernelImageSize() << ", entree=0x" << image.EntryPoint() << "\n"
              << "boot_params=0x" << image.BootParamsAddress() << ", RAM=" << std::dec
              << image.GuestRamSize() / (1024 * 1024) << " MiB, initrd=" << image.InitrdSize() << " octets\n"
              << "Execution du point d'entree Linux via WHP...\n";

    std::string debugConsole;
    std::array<ComUart, 4> serialPorts{
        ComUart(0x3F8, true), ComUart(0x2F8), ComUart(0x3E8), ComUart(0x2E8)};
    LegacyPic pic;
    LegacyDma dma;
    PiixBusMasterIde busMasterIde;
    PiixIdeController ide(virtualDisk ? &*virtualDisk : nullptr);
    AtapiCdrom cdrom(isoMedia ? &*isoMedia : nullptr);
    Ps2Controller ps2;
    VgaTextRegisters vgaRegisters;
    std::optional<VgaTextWindow> vgaWindow;
    if (useVgaWindow) {
        const auto* textMemory = static_cast<const volatile std::uint16_t*>(
            partition.GetHostAddressForGuestPhysicalAddress(0xB8000, 80 * 25 * 2));
        if (textMemory == nullptr) {
            throw std::runtime_error("La mémoire VGA texte invitée n'est pas mappée.");
        }
        vgaWindow.emplace(partition, ps2, textMemory);
        std::cout << "Écran invité ouvert: VGA texte 80x25. Les codes physiques sont transmis; "
                     "choisis la disposition du clavier dans l'invité.\n";
    }
    Pit8254 pit;
    CmosRtc cmos(ramSize);
    virtualiseur::whp::PciConfigMechanism1 pci;
    std::unique_ptr<SlirpNetworkBackend> slirpBackend;
    VirtioNetDevice virtioNet(
        [&](std::uint64_t guestPhysicalAddress, std::size_t length) {
            return static_cast<std::uint8_t*>(
                partition.GetHostAddressForGuestPhysicalAddress(guestPhysicalAddress, length));
        },
        [&slirpBackend](std::span<const std::uint8_t> frame) {
            return slirpBackend && slirpBackend->Send(frame);
        });
    struct StopNetworkBackend final {
        std::unique_ptr<SlirpNetworkBackend>& backend;
        ~StopNetworkBackend() { backend.reset(); }
    } stopNetworkBackend{slirpBackend};
    if (slirpRequested) {
        slirpBackend = std::make_unique<SlirpNetworkBackend>(
            [&virtioNet](std::span<const std::uint8_t> frame) { (void)virtioNet.Receive(frame); });
        virtioNet.SetLinkUp(true);
        std::cout << "NAT libslirp actif: DHCP, DNS et passerelle automatiques "
                     "(réseau 10.0.2.0/24).\n";
    } else {
        std::cout << "Carte PCI VirtIO-net exposee (1AF4:1000, MAC 52:54:00:12:34:56). "
                     "Lien hors ligne; option --net slirp absente.\n";
    }
    std::uint8_t speakerControl = 0;
    std::atomic_bool executionTimedOut = false;
    std::atomic_bool watchdogCancellationSucceeded = false;
    std::atomic_uint64_t timerInterruptRequests = 0;
    std::atomic_uint64_t serialInterruptRequests = 0;
    std::atomic_uint64_t maskedTimerTicks = 0;
    std::atomic_uint64_t picEoiWrites = 0;
    std::atomic_uint32_t pendingIdeInterrupts = 0;
    std::atomic_bool pendingTimerInterrupt = false;
    const auto transferPrdBuffer = [&](std::span<std::byte> buffer,
                                       bool diskToGuest,
                                       std::uint32_t prdTable) {
        std::size_t transferred = 0;
        for (std::uint32_t index = 0; index < 4096; ++index) {
            const auto* descriptor = static_cast<const std::byte*>(
                partition.GetHostAddressForGuestPhysicalAddress(
                    static_cast<std::uint64_t>(prdTable) + index * 8ull, 8));
            if (descriptor == nullptr) return false;
            std::uint32_t guestAddress = 0;
            std::uint16_t byteCount = 0;
            std::uint16_t flags = 0;
            std::memcpy(&guestAddress, descriptor, sizeof(guestAddress));
            std::memcpy(&byteCount, descriptor + 4, sizeof(byteCount));
            std::memcpy(&flags, descriptor + 6, sizeof(flags));
            const std::size_t length = byteCount == 0 ? 65536u : byteCount;
            if (transferred + length > buffer.size() ||
                (guestAddress & 0xFFFFu) + length > 0x10000u) return false;
            auto* guestBuffer = static_cast<std::byte*>(
                partition.GetHostAddressForGuestPhysicalAddress(guestAddress, length));
            if (guestBuffer == nullptr) return false;
            if (diskToGuest) {
                std::memcpy(guestBuffer, buffer.data() + transferred, length);
            } else {
                std::memcpy(buffer.data() + transferred, guestBuffer, length);
            }
            transferred += length;
            if ((flags & 0x8000u) != 0) return transferred == buffer.size();
        }
        return false;
    };
    const auto completeDmaIfReady = [&]() {
        if (!busMasterIde.IsActive(0) || !ide.DmaPending()) return;
        bool success = false;
        try {
            const bool diskToGuest = ide.DmaReadsFromDisk();
            if (busMasterIde.ReadsFromDisk(0) != diskToGuest) {
                throw std::runtime_error("Direction bus-master IDE incompatible avec la commande ATA DMA.");
            }
            std::vector<std::byte> transfer(ide.DmaByteCount());
            if (diskToGuest) {
                ide.ReadDmaData(transfer);
                success = transferPrdBuffer(transfer, true, busMasterIde.PrdTableAddress(0));
            } else {
                success = transferPrdBuffer(transfer, false, busMasterIde.PrdTableAddress(0));
                if (success) ide.WriteDmaData(transfer);
            }
            if (!success) throw std::runtime_error("Table PRD IDE DMA invalide ou incomplete.");
        } catch (const std::exception& error) {
            std::cerr << "Echec du transfert BMDMA IDE: " << error.what() << "\n";
            success = false;
        }
        ide.CompleteDma(success);
        busMasterIde.Complete(0, success);
        if (ide.TakeInterruptRequest()) {
            if (const auto vector = pic.InterruptVector(PiixIdeController::InterruptLine())) {
                (void)partition.CancelRunVirtualProcessor(0);
                partition.RequestExternalInterrupt(0, *vector);
            } else {
                pendingIdeInterrupts.fetch_or(1u << PiixIdeController::InterruptLine(),
                                              std::memory_order_release);
            }
        }
    };
    const auto executionDeadline = std::chrono::steady_clock::now() + std::chrono::hours(24);
    std::jthread serialInput([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            serialPorts[0].PollHostInput();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    std::jthread irqClock([&](std::stop_token stop) {
        HANDLE waitableTimer = CreateWaitableTimerExW(
            nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
            TIMER_ALL_ACCESS);
        if (waitableTimer == nullptr) {
            waitableTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
        const auto waitUntil = [waitableTimer](std::chrono::steady_clock::time_point deadline) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (waitableTimer == nullptr || remaining <= std::chrono::steady_clock::duration::zero()) {
                std::this_thread::sleep_until(deadline);
                return;
            }
            const auto ticks100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100;
            LARGE_INTEGER dueTime{};
            dueTime.QuadPart = -(std::max<std::int64_t>)(1, ticks100ns);
            if (SetWaitableTimer(waitableTimer, &dueTime, 0, nullptr, nullptr, FALSE)) {
                (void)WaitForSingleObject(waitableTimer, INFINITE);
            } else {
                std::this_thread::sleep_until(deadline);
            }
        };
        auto nextTick = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            // The kernel's APIC timer is not modeled yet. Drive the legacy
            // timer at 250 Hz when the guest PIT requests a slower/default
            // interval, so kernel jiffies and timeout loops do not crawl.
            const auto period = (std::min)(pit.InterruptPeriod(), std::chrono::microseconds(4000));
            nextTick += period;
            waitUntil(nextTick);
            if (stop.stop_requested()) break;
            const auto now = std::chrono::steady_clock::now();
            if (now > nextTick + period) nextTick = now;
            pendingTimerInterrupt.store(true, std::memory_order_release);
            if (virtioNet.TakeInterrupt()) {
                pendingIdeInterrupts.fetch_or(1u << 11, std::memory_order_release);
            }
            const auto vector = pic.TimerVector();
            if (!vector) maskedTimerTicks.fetch_add(1, std::memory_order_relaxed);
            try {
                // WHvRunVirtualProcessor can be blocked while the guest masks
                // IRQ0. Keep waking it so pending IDE/ATAPI interrupts and
                // guest PIC-mask changes can still be observed.
                (void)partition.CancelRunVirtualProcessor(0);
                if (vector && pendingTimerInterrupt.exchange(false, std::memory_order_acq_rel)) {
                    partition.RequestExternalInterrupt(0, *vector);
                    timerInterruptRequests.fetch_add(1, std::memory_order_relaxed);
                }
                constexpr std::array<std::uint8_t, 4> uartIrqs{4, 3, 4, 3};
                for (std::size_t index = 0; index < serialPorts.size(); ++index) {
                    if (serialPorts[index].HasInterrupt()) {
                        if (const auto uartVector = pic.InterruptVector(uartIrqs[index])) {
                            (void)partition.CancelRunVirtualProcessor(0);
                            partition.RequestExternalInterrupt(0, *uartVector);
                            serialInterruptRequests.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                const auto keyboardVector = pic.InterruptVector(1);
                const auto auxiliaryVector = pic.InterruptVector(12);
                const auto deliverablePs2Irqs = static_cast<std::uint8_t>(
                    (keyboardVector ? 0x01 : 0) | (auxiliaryVector ? 0x02 : 0));
                const auto ps2Irqs = ps2.TakeInterruptRequests(deliverablePs2Irqs);
                if ((ps2Irqs & 0x01) != 0) {
                    (void)partition.CancelRunVirtualProcessor(0);
                    partition.RequestExternalInterrupt(0, *keyboardVector);
                }
                if ((ps2Irqs & 0x02) != 0) {
                    (void)partition.CancelRunVirtualProcessor(0);
                    partition.RequestExternalInterrupt(0, *auxiliaryVector);
                }
                for (const auto irq : {std::uint8_t{11}, std::uint8_t{14}, std::uint8_t{15}}) {
                    const auto bit = 1u << irq;
                    if ((pendingIdeInterrupts.load(std::memory_order_acquire) & bit) == 0) continue;
                    if (const auto ideVector = pic.InterruptVector(irq)) {
                        pendingIdeInterrupts.fetch_and(~bit, std::memory_order_acq_rel);
                        (void)partition.CancelRunVirtualProcessor(0);
                        partition.RequestExternalInterrupt(0, *ideVector);
                    }
                }
            } catch (const std::exception& error) {
                std::cerr << "Echec d'injection d'une interruption invite: " << error.what() << "\n";
                break;
            }
        }
        if (waitableTimer != nullptr) CloseHandle(waitableTimer);
    });
    std::jthread executionWatchdog([&](std::stop_token stop) {
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < executionDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!stop.stop_requested()) {
            executionTimedOut.store(true, std::memory_order_release);
            watchdogCancellationSucceeded.store(
                partition.CancelRunVirtualProcessor(0), std::memory_order_release);
        }
    });
    std::uint64_t port61Reads = 0;
    std::uint64_t pitChannelTwoReads = 0;
    std::uint64_t pitChannelTwoWrites = 0;
    std::uint64_t lastRip = 0;
    std::uint32_t lastExitReason = 0;
    std::uint16_t lastIoPort = 0;
    std::uint64_t ps2StatusReads = 0;
    bool guestWindowClosed = false;
    for (std::uint64_t exits = 0;; ++exits) {
        if (vgaWindow && vgaWindow->IsClosed()) {
            guestWindowClosed = true;
            break;
        }
        const auto exit = partition.RunVirtualProcessor(0);
        if (exit.ExitReason == WHvRunVpExitReasonCanceled) {
            if (executionTimedOut.load(std::memory_order_acquire)) break;
            if (vgaWindow && vgaWindow->IsClosed()) {
                guestWindowClosed = true;
                break;
            }
            continue; // The host clock interrupted the blocking WHP run call.
        }
        lastRip = exit.VpContext.Rip;
        lastExitReason = static_cast<std::uint32_t>(exit.ExitReason);
        if (exit.ExitReason == WHvRunVpExitReasonX64Halt) {
            std::cout << "Le noyau a execute HLT apres " << std::dec << exits << " sorties WHP.\n";
            return 0;
        }
        if (exit.ExitReason == WHvRunVpExitReasonX64Cpuid) {
            const auto& cpuid = exit.CpuidAccess;
            const std::array<WHV_REGISTER_NAME, 4> names{
                WHvX64RegisterRax, WHvX64RegisterRbx, WHvX64RegisterRcx, WHvX64RegisterRdx};
            std::array<WHV_REGISTER_VALUE, names.size()> values{};
            values[0].Reg64 = cpuid.DefaultResultRax;
            values[1].Reg64 = cpuid.DefaultResultRbx;
            values[2].Reg64 = cpuid.DefaultResultRcx;
            values[3].Reg64 = cpuid.DefaultResultRdx;
            partition.SetVirtualProcessorRegisters(0, names, values);
            AdvanceRip(partition, exit);
            continue;
        }
        if (exit.ExitReason == WHvRunVpExitReasonX64MsrAccess &&
            exit.MsrAccess.MsrNumber == 0xC0000080 /* IA32_EFER */) {
            const auto& msr = exit.MsrAccess;
            if (msr.AccessInfo.IsWrite) {
                const WHV_REGISTER_NAME name = WHvX64RegisterEfer;
                WHV_REGISTER_VALUE value{};
                value.Reg64 = (msr.Rdx << 32) | (msr.Rax & 0xFFFFFFFFull);
                partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
            } else {
                const WHV_REGISTER_NAME name = WHvX64RegisterEfer;
                const auto value = partition.GetVirtualProcessorRegisters(0, std::span(&name, 1)).front();
                const std::array<WHV_REGISTER_NAME, 2> names{WHvX64RegisterRax, WHvX64RegisterRdx};
                std::array<WHV_REGISTER_VALUE, names.size()> values{};
                values[0].Reg64 = value.Reg64 & 0xFFFFFFFFull;
                values[1].Reg64 = value.Reg64 >> 32;
                partition.SetVirtualProcessorRegisters(0, names, values);
            }
            AdvanceRip(partition, exit);
            continue;
        }
        if (exit.ExitReason == WHvRunVpExitReasonX64IoPortAccess) {
            const auto& io = exit.IoPortAccess;
            lastIoPort = io.PortNumber;
            if (!io.AccessInfo.IsWrite && io.PortNumber == 0x64) ++ps2StatusReads;
            if (busMasterIde.HandlesPort(io.PortNumber) && !io.AccessInfo.StringOp &&
                io.AccessInfo.AccessSize >= 1 && io.AccessInfo.AccessSize <= 4) {
                const auto accessSize = static_cast<std::uint8_t>(io.AccessInfo.AccessSize);
                if (io.AccessInfo.IsWrite) {
                    busMasterIde.Write(io.PortNumber, accessSize, static_cast<std::uint32_t>(io.Rax));
                    completeDmaIfReady();
                } else {
                    const auto data = busMasterIde.Read(io.PortNumber, accessSize);
                    const auto mask = accessSize == 4 ? 0xFFFFFFFFull :
                        ((1ull << (accessSize * 8)) - 1);
                    const WHV_REGISTER_NAME name = WHvX64RegisterRax;
                    WHV_REGISTER_VALUE value{};
                    value.Reg64 = accessSize == 4 ? data : ((io.Rax & ~mask) | data);
                    partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
                }
                AdvanceRip(partition, exit);
                continue;
            }
            if (ide.HandlesPort(io.PortNumber) || cdrom.HandlesPort(io.PortNumber)) {
                const bool isCdrom = cdrom.HandlesPort(io.PortNumber);
                const auto readPort = [&](std::uint16_t port, std::uint8_t size) {
                    return isCdrom ? cdrom.Read(port, size) : ide.Read(port, size);
                };
                const auto writePort = [&](std::uint16_t port, std::uint8_t size, std::uint32_t value) {
                    if (isCdrom) cdrom.Write(port, size, value);
                    else ide.Write(port, size, value);
                };
                const auto accessSize = static_cast<std::uint8_t>(io.AccessInfo.AccessSize);
                if (!io.AccessInfo.StringOp) {
                    if (io.AccessInfo.IsWrite) {
                        writePort(io.PortNumber, accessSize, static_cast<std::uint32_t>(io.Rax));
                    } else {
                        const auto data = readPort(io.PortNumber, accessSize);
                        const auto mask = accessSize == 4 ? 0xFFFFFFFFull :
                            ((1ull << (accessSize * 8)) - 1);
                        const WHV_REGISTER_NAME name = WHvX64RegisterRax;
                        WHV_REGISTER_VALUE value{};
                        value.Reg64 = accessSize == 4 ? data : ((io.Rax & ~mask) | data);
                        partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
                    }
                } else {
                    const std::uint64_t count = io.AccessInfo.RepPrefix ? io.Rcx : 1;
                    if (count > 65536 || accessSize == 0 || accessSize > 4) {
                        std::cerr << "Transfert IDE string invalide: count=" << count
                                  << ", taille=" << static_cast<unsigned>(accessSize) << ".\n";
                        break;
                    }
                    auto address = io.AccessInfo.IsWrite ? io.Rsi : io.Rdi;
                    const auto flags = io.AccessInfo.IsWrite
                        ? WHvTranslateGvaFlagValidateRead : WHvTranslateGvaFlagValidateWrite;
                    for (std::uint64_t index = 0; index < count; ++index) {
                        std::uint64_t gpa = 0;
                        const auto translation = partition.TranslateGuestVirtualAddress(0, address, flags, gpa);
                        if (translation.ResultCode != WHvTranslateGvaResultSuccess) {
                            throw std::runtime_error("Adresse GVA invalide pendant un transfert IDE PIO.");
                        }
                        auto* guestMemory = static_cast<std::byte*>(
                            partition.GetHostAddressForGuestPhysicalAddress(gpa, accessSize));
                        if (guestMemory == nullptr) {
                            throw std::runtime_error("Memoire invite absente pendant un transfert IDE PIO.");
                        }
                        if (io.AccessInfo.IsWrite) {
                            std::uint32_t data = 0;
                            std::memcpy(&data, guestMemory, accessSize);
                            writePort(io.PortNumber, accessSize, data);
                            address += accessSize;
                        } else {
                            const auto data = readPort(io.PortNumber, accessSize);
                            std::memcpy(guestMemory, &data, accessSize);
                            address += accessSize;
                        }
                    }
                    const std::array<WHV_REGISTER_NAME, 2> names{
                        io.AccessInfo.IsWrite ? WHvX64RegisterRsi : WHvX64RegisterRdi,
                        WHvX64RegisterRcx};
                    std::array<WHV_REGISTER_VALUE, names.size()> values{};
                    values[0].Reg64 = address;
                    values[1].Reg64 = io.AccessInfo.RepPrefix ? 0 : io.Rcx;
                    partition.SetVirtualProcessorRegisters(0, names, values);
                }
                completeDmaIfReady();
                const bool interruptRequested = isCdrom
                    ? cdrom.TakeInterruptRequest() : ide.TakeInterruptRequest();
                if (interruptRequested) {
                    const auto interruptLine = isCdrom
                        ? AtapiCdrom::InterruptLine() : PiixIdeController::InterruptLine();
                    const auto vector = pic.InterruptVector(interruptLine);
                    if (vector) {
                        (void)partition.CancelRunVirtualProcessor(0);
                        partition.RequestExternalInterrupt(0, *vector);
                    } else {
                        pendingIdeInterrupts.fetch_or(1u << interruptLine, std::memory_order_release);
                    }
                }
                AdvanceRip(partition, exit);
                continue;
            }
            if (vgaRegisters.HandlesPort(io.PortNumber) && !io.AccessInfo.StringOp &&
                io.AccessInfo.AccessSize >= 1 && io.AccessInfo.AccessSize <= 4 &&
                io.PortNumber + io.AccessInfo.AccessSize <= 0x3E0) {
                if (io.AccessInfo.IsWrite) {
                    for (std::uint8_t index = 0; index < io.AccessInfo.AccessSize; ++index) {
                        vgaRegisters.Write(static_cast<std::uint16_t>(io.PortNumber + index),
                            static_cast<std::uint8_t>(io.Rax >> (index * 8)));
                    }
                } else {
                    const WHV_REGISTER_NAME name = WHvX64RegisterRax;
                    WHV_REGISTER_VALUE value{};
                    std::uint64_t data = 0;
                    for (std::uint8_t index = 0; index < io.AccessInfo.AccessSize; ++index) {
                        data |= static_cast<std::uint64_t>(vgaRegisters.Read(
                            static_cast<std::uint16_t>(io.PortNumber + index))) << (index * 8);
                    }
                    const auto mask = io.AccessInfo.AccessSize == 4 ? 0xFFFFFFFFull :
                        ((1ull << (io.AccessInfo.AccessSize * 8)) - 1);
                    value.Reg64 = io.AccessInfo.AccessSize == 4 ? data : ((io.Rax & ~mask) | data);
                    partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
                }
                AdvanceRip(partition, exit);
                continue;
            }
            if (pci.HandlesPort(io.PortNumber) && !io.AccessInfo.StringOp &&
                (io.AccessInfo.AccessSize == 1 || io.AccessInfo.AccessSize == 2 ||
                 io.AccessInfo.AccessSize == 4)) {
                if (io.AccessInfo.IsWrite) {
                    pci.WritePort(io.PortNumber, static_cast<std::uint8_t>(io.AccessInfo.AccessSize),
                                  static_cast<std::uint32_t>(io.Rax));
                } else {
                    const auto mask = io.AccessInfo.AccessSize == 4 ? 0xFFFFFFFFull :
                        ((1ull << (io.AccessInfo.AccessSize * 8)) - 1);
                    const auto result = pci.ReadPort(
                        io.PortNumber, static_cast<std::uint8_t>(io.AccessInfo.AccessSize));
                    const WHV_REGISTER_NAME name = WHvX64RegisterRax;
                    WHV_REGISTER_VALUE value{};
                    value.Reg64 = io.AccessInfo.AccessSize == 4
                        ? result
                        : ((io.Rax & ~mask) | result);
                    partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
                }
                AdvanceRip(partition, exit);
                continue;
            }
            if (virtioNet.HandlesPort(io.PortNumber) && !io.AccessInfo.StringOp &&
                io.AccessInfo.AccessSize >= 1 && io.AccessInfo.AccessSize <= 4) {
                const auto size = static_cast<std::uint8_t>(io.AccessInfo.AccessSize);
                if (io.AccessInfo.IsWrite) {
                    virtioNet.Write(io.PortNumber, size, static_cast<std::uint32_t>(io.Rax));
                } else {
                    const auto data = virtioNet.Read(io.PortNumber, size);
                    const auto mask = size == 4 ? 0xFFFFFFFFull : ((1ull << (size * 8)) - 1);
                    const WHV_REGISTER_NAME name = WHvX64RegisterRax;
                    WHV_REGISTER_VALUE value{};
                    value.Reg64 = size == 4 ? data : ((io.Rax & ~mask) | data);
                    partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
                }
                if (virtioNet.TakeInterrupt()) {
                    constexpr std::uint8_t irq = 11;
                    if (const auto vector = pic.InterruptVector(irq)) {
                        (void)partition.CancelRunVirtualProcessor(0);
                        partition.RequestExternalInterrupt(0, *vector);
                    } else {
                        pendingIdeInterrupts.fetch_or(1u << irq, std::memory_order_release);
                    }
                }
                AdvanceRip(partition, exit);
                continue;
            }
            if (!io.AccessInfo.StringOp && io.AccessInfo.AccessSize == 1 &&
                (io.PortNumber == 0x70 || io.PortNumber == 0x71)) {
                if (io.AccessInfo.IsWrite) {
                    if (io.PortNumber == 0x70) cmos.Select(static_cast<std::uint8_t>(io.Rax));
                    else cmos.WriteData(static_cast<std::uint8_t>(io.Rax));
                } else {
                    const auto data = io.PortNumber == 0x70 ? cmos.ReadIndex() : cmos.ReadData();
                    const WHV_REGISTER_NAME name = WHvX64RegisterRax;
                    WHV_REGISTER_VALUE value{};
                    value.Reg64 = (io.Rax & ~0xFFull) | data;
                    partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
                }
                AdvanceRip(partition, exit);
                continue;
            }
            if (!io.AccessInfo.StringOp && io.AccessInfo.AccessSize == 1 &&
                (io.PortNumber == 0x60 || io.PortNumber == 0x64)) {
                if (io.AccessInfo.IsWrite) {
                    if (io.PortNumber == 0x64) ps2.WriteCommand(static_cast<std::uint8_t>(io.Rax));
                    else ps2.WriteData(static_cast<std::uint8_t>(io.Rax));
                } else {
                    const auto data = io.PortNumber == 0x64 ? ps2.ReadStatus() : ps2.ReadData();
                    const WHV_REGISTER_NAME name = WHvX64RegisterRax;
                    WHV_REGISTER_VALUE value{};
                    value.Reg64 = (io.Rax & ~0xFFull) | data;
                    partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
                }
                AdvanceRip(partition, exit);
                continue;
            }
            if (!io.AccessInfo.StringOp && io.AccessInfo.AccessSize == 1) {
                const auto uart = std::find_if(serialPorts.begin(), serialPorts.end(),
                    [&](const ComUart& device) { return device.HandlesPort(io.PortNumber); });
                if (uart != serialPorts.end()) {
                    if (io.AccessInfo.IsWrite) {
                        uart->Write(io.PortNumber, static_cast<std::uint8_t>(io.Rax));
                    } else {
                        const WHV_REGISTER_NAME name = WHvX64RegisterRax;
                        WHV_REGISTER_VALUE value{};
                        value.Reg64 = (io.Rax & ~0xFFull) | uart->Read(io.PortNumber);
                        partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
                    }
                    AdvanceRip(partition, exit);
                    continue;
                }
            }
            if (!io.AccessInfo.StringOp && io.AccessInfo.AccessSize == 1 &&
                (io.PortNumber == 0x20 || io.PortNumber == 0x21 ||
                 io.PortNumber == 0xA0 || io.PortNumber == 0xA1 ||
                 io.PortNumber == 0x4D0 || io.PortNumber == 0x4D1)) {
                if (io.AccessInfo.IsWrite) {
                    const auto value = static_cast<std::uint8_t>(io.Rax);
                    pic.Write(io.PortNumber, value);
                    if ((io.PortNumber == 0x20 || io.PortNumber == 0xA0) &&
                        (value & 0x20) != 0) {
                        picEoiWrites.fetch_add(1, std::memory_order_relaxed);
                        // Timer IRQs are injected through WHP's local APIC even
                        // while Linux is using the legacy PIC. Mirror its PIC
                        // EOI so WHP can release the in-service APIC vector.
                        partition.AcknowledgeExternalInterrupt(0);
                    }
                } else {
                    const WHV_REGISTER_NAME name = WHvX64RegisterRax;
                    WHV_REGISTER_VALUE value{};
                    value.Reg64 = (io.Rax & ~0xFFull) | pic.Read(io.PortNumber);
                    partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
                }
                AdvanceRip(partition, exit);
                continue;
            }
            if (!io.AccessInfo.StringOp && io.AccessInfo.AccessSize == 1 &&
                io.PortNumber >= 0x40 && io.PortNumber <= 0x43) {
                if (io.PortNumber == 0x42 && !io.AccessInfo.IsWrite) ++pitChannelTwoReads;
                if (io.PortNumber == 0x42 && io.AccessInfo.IsWrite) ++pitChannelTwoWrites;
                if (io.AccessInfo.IsWrite) {
                    pit.Write(io.PortNumber, static_cast<std::uint8_t>(io.Rax));
                } else {
                    const WHV_REGISTER_NAME name = WHvX64RegisterRax;
                    WHV_REGISTER_VALUE value{};
                    value.Reg64 = (io.Rax & ~0xFFull) | pit.Read(io.PortNumber);
                    partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
                }
                AdvanceRip(partition, exit);
                continue;
            }
            if (!io.AccessInfo.StringOp && io.AccessInfo.AccessSize == 1 &&
                dma.HandlesPort(io.PortNumber)) {
                if (io.AccessInfo.IsWrite) {
                    dma.Write(io.PortNumber, static_cast<std::uint8_t>(io.Rax));
                } else {
                    const WHV_REGISTER_NAME name = WHvX64RegisterRax;
                    WHV_REGISTER_VALUE value{};
                    value.Reg64 = (io.Rax & ~0xFFull) | dma.Read(io.PortNumber);
                    partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
                }
                AdvanceRip(partition, exit);
                continue;
            }
            if (!io.AccessInfo.StringOp && io.AccessInfo.AccessSize == 1 && io.PortNumber == 0x61) {
                if (!io.AccessInfo.IsWrite) ++port61Reads;
                if (io.AccessInfo.IsWrite) {
                    speakerControl = static_cast<std::uint8_t>(io.Rax);
                    pit.SetChannelTwoGate((speakerControl & 0x01) != 0);
                } else {
                    pit.SetChannelTwoGate((speakerControl & 0x01) != 0);
                    const WHV_REGISTER_NAME name = WHvX64RegisterRax;
                    WHV_REGISTER_VALUE value{};
                    const auto status = static_cast<std::uint8_t>(
                        (speakerControl & ~0x20u) | (pit.ChannelTwoOutput() ? 0x20u : 0));
                    value.Reg64 = (io.Rax & ~0xFFull) | status;
                    partition.SetVirtualProcessorRegisters(0, std::span(&name, 1), std::span(&value, 1));
                }
                AdvanceRip(partition, exit);
                continue;
            }
            if (io.PortNumber == 0x80 && io.AccessInfo.IsWrite && !io.AccessInfo.StringOp) {
                AdvanceRip(partition, exit); // Traditional POST delay port.
                continue;
            }
            if (io.PortNumber == 0x00E9 && io.AccessInfo.IsWrite &&
                !io.AccessInfo.StringOp && io.AccessInfo.AccessSize == 1) {
                debugConsole.push_back(static_cast<char>(io.Rax & 0xFF));
                AdvanceRip(partition, exit);
                continue;
            }
            std::cerr << "Acces E/S non gere (port 0x" << std::hex << io.PortNumber
                      << ") apres " << std::dec << exits << " sorties WHP.\n";
            break;
        }
        if (exit.ExitReason == WHvRunVpExitReasonX64MsrAccess) {
            std::cerr << "Acces MSR non gere (0x" << std::hex << exit.MsrAccess.MsrNumber
                      << ") apres " << std::dec << exits << " sorties WHP.\n";
            break;
        }
        if (exit.ExitReason == WHvRunVpExitReasonMemoryAccess) {
            const auto& access = exit.MemoryAccess;
            if (access.AccessInfo.GpaUnmapped && emulatorContext.apic.Contains(access.Gpa)) {
                WHV_EMULATOR_STATUS status{};
                const auto result = WHvEmulatorTryMmioEmulation(
                    instructionEmulator.Get(), &emulatorContext, &exit.VpContext, &access, &status);
                if (FAILED(result) || !status.EmulationSuccessful) {
                    std::cerr << "Echec d'emulation MMIO APIC: HRESULT=0x" << std::hex
                              << static_cast<unsigned long>(result)
                              << ", statut=0x" << status.AsUINT32 << ".\n";
                    break;
                }
                continue;
            }
            const char* accessType = access.AccessInfo.AccessType == WHvMemoryAccessRead ? "lecture" :
                access.AccessInfo.AccessType == WHvMemoryAccessWrite ? "ecriture" : "execution";
            std::cerr << "Acces memoire invite non gere: type=" << accessType
                      << ", GPA=0x" << std::hex << access.Gpa
                      << ", GVA=0x" << access.Gva
                      << ", GPA non mappee=" << (access.AccessInfo.GpaUnmapped ? "oui" : "non")
                      << ", instruction=";
            for (std::uint8_t index = 0; index < access.InstructionByteCount; ++index) {
                std::cerr << static_cast<unsigned>(access.InstructionBytes[index]) << ' ';
            }
            std::cerr << " après " << std::dec << exits << " sorties WHP.\n";
            break;
        }
        std::cerr << "Sortie WHP non geree: raison=0x" << std::hex
                  << static_cast<unsigned long>(exit.ExitReason) << ", RIP=0x"
                  << exit.VpContext.Rip << ", sorties=" << std::dec << exits << ".\n";
        break;
    }

    if (!debugConsole.empty()) {
        std::cout << "Sortie debug port 0xE9: " << debugConsole << "\n";
    }
    if (guestWindowClosed) {
        std::cout << "Fenêtre invitée fermée; arrêt de la VM.\n";
        return 0;
    }
    if (executionTimedOut.load(std::memory_order_acquire)) {
        const std::array<WHV_REGISTER_NAME, 8> names{
            WHvX64RegisterRip, WHvX64RegisterRflags, WHvX64RegisterRsp,
            WHvX64RegisterRax, WHvX64RegisterRcx, WHvX64RegisterRdx,
            WHvX64RegisterRsi, WHvX64RegisterRdi};
        try {
            const auto state = partition.GetVirtualProcessorRegisters(0, names);
            lastRip = state[0].Reg64;
            std::cout << "Registres apres annulation: RIP=0x" << std::hex << state[0].Reg64
                      << ", RFLAGS=0x" << state[1].Reg64 << ", RSP=0x" << state[2].Reg64
                      << ", RAX=0x" << state[3].Reg64 << ", RCX=0x" << state[4].Reg64
                      << ", RDX=0x" << state[5].Reg64 << ", RSI=0x" << state[6].Reg64
                      << ", RDI=0x" << state[7].Reg64 << std::dec
                      << ", annulation="
                      << (watchdogCancellationSucceeded.load(std::memory_order_acquire) ? "ok" : "non")
                      << ".\n";
            std::uint64_t instructionGpa = 0;
            const auto translation = partition.TranslateGuestVirtualAddress(
                0, state[0].Reg64, WHvTranslateGvaFlagValidateRead, instructionGpa);
            std::cout << "Adresse d'instruction: resultat=0x" << std::hex
                      << static_cast<unsigned>(translation.ResultCode)
                      << ", GPA=0x" << instructionGpa << ", octets=";
            const auto* instruction = static_cast<const std::uint8_t*>(
                partition.GetHostAddressForGuestPhysicalAddress(instructionGpa, 16));
            if (instruction != nullptr) {
                for (std::size_t index = 0; index < 16; ++index) {
                    std::cout << std::setw(2) << std::setfill('0')
                              << static_cast<unsigned>(instruction[index]) << ' ';
                }
                std::cout << std::setfill(' ');
                if (instruction[0] == 0x48 && instruction[1] == 0x8B && instruction[2] == 0x05) {
                    std::int32_t displacement = 0;
                    std::memcpy(&displacement, instruction + 3, sizeof(displacement));
                    const auto targetGva = state[0].Reg64 + 7 + displacement;
                    std::uint64_t targetGpa = 0;
                    const auto targetTranslation = partition.TranslateGuestVirtualAddress(
                        0, targetGva, WHvTranslateGvaFlagValidateRead, targetGpa);
                    const auto* target = static_cast<const std::uint64_t*>(
                        partition.GetHostAddressForGuestPhysicalAddress(targetGpa, sizeof(std::uint64_t)));
                    std::cout << "\nVariable lue: GVA=0x" << std::hex << targetGva
                              << ", GPA=0x" << targetGpa
                              << ", traduction=0x" << static_cast<unsigned>(targetTranslation.ResultCode)
                              << ", valeur=";
                    if (target != nullptr) std::cout << "0x" << *target;
                    else std::cout << "inaccessible";
                    std::cout << std::dec;
                }
            } else {
                std::cout << "inaccessible";
            }
            std::cout << std::dec << ".\n";
        } catch (const std::exception& error) {
            std::cerr << "Lecture des registres apres delai impossible: " << error.what() << "\n";
        }
        try {
            std::cout << "APIC WHP: ID=0x" << std::hex
                      << partition.GetLocalApicRegister(0, 0x020)
                      << ", ISR1=0x" << partition.GetLocalApicRegister(0, 0x110)
                      << ", IRR1=0x" << partition.GetLocalApicRegister(0, 0x210)
                      << ", TPR=0x" << partition.GetLocalApicRegister(0, 0x080)
                      << ", SVR=0x" << partition.GetLocalApicRegister(0, 0x0F0)
                      << std::dec << ".\n";
        } catch (const std::exception& error) {
            std::cerr << "Lecture de l'etat APIC WHP impossible: " << error.what() << "\n";
        }
    }
    std::cout << (executionTimedOut.load(std::memory_order_acquire)
                      ? "Delai de diagnostic atteint; dernier etat WHP=0x"
                      : "Limite d'execution atteinte; dernier exit WHP=0x")
              << std::hex << lastExitReason
              << ", RIP=0x" << lastRip << ", dernier port=0x" << lastIoPort << std::dec
              << ", lectures statut PS/2= " << ps2StatusReads << ", lectures PIT2= "
              << pitChannelTwoReads << ", ecritures PIT2= " << pitChannelTwoWrites
              << ", lectures port 0x61= " << port61Reads << ", port61=0x" << std::hex
              << static_cast<unsigned>(speakerControl) << ", UART1 IER=0x"
              << static_cast<unsigned>(serialPorts[0].InterruptEnable()) << ", UART1 IRQ="
              << (serialPorts[0].HasInterrupt() ? "pending" : "idle") << std::dec
              << ", interruptions PIT demandees="
              << timerInterruptRequests.load(std::memory_order_relaxed)
              << ", serie demandees=" << serialInterruptRequests.load(std::memory_order_relaxed)
              << ", ticks masques=" << maskedTimerTicks.load(std::memory_order_relaxed)
              << ", EOI PIC=" << picEoiWrites.load(std::memory_order_relaxed)
              << ", EOI APIC emules=" << emulatorContext.apic.EoiCount()
              << ", APIC logiciel actif=" << (emulatorContext.apic.SoftwareEnabled() ? "oui" : "non")
              << ".";
    const auto pitTwo = pit.ChannelTwoSnapshot();
    std::cout << ", PIT2 count=0x" << pitTwo[0] << ", mode=" << std::dec << pitTwo[1]
              << ", gate=" << pitTwo[2] << ", OUT=" << pitTwo[3] << ".\n";
    return 4;
}

} // namespace

int main(int argc, char** argv)
{
    ConfigureHostTerminal();
    try {
        return Run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Echec du demarrage Linux WHP: " << error.what() << "\n";
        return 1;
    }
}
