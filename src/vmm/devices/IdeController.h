#pragma once

#include "../core/Bus.h"
#include "../core/GuestMemory.h"
#include "Pci.h"

#include "Iso9660Image.h"
#include "VirtualDisk.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace vmm {

// PIIX3 IDE function (8086:7010) in legacy mode: primary channel at
// 0x1F0/0x3F6 (IRQ 14), secondary at 0x170/0x376 (IRQ 15), and a bus-master
// DMA engine in BAR4. Each channel hosts one master device: an ATA disk
// (LBA28/LBA48, PIO and multiword DMA) or an ATAPI CD-ROM (PIO and DMA).
class IdeController final : public PciFunction, public IoDevice {
public:
    static constexpr std::uint16_t kBusMasterBase = 0xC000;

    IdeController(const GuestMemory& memory, InterruptSink& sink, IoBus& ioBus);

    void AttachDisk(std::uint32_t channel, virtualiseur::whp::VirtualDisk* disk);
    void AttachCdrom(std::uint32_t channel, virtualiseur::whp::Iso9660Image* image);
    bool CdromEjected() const noexcept;
    bool HasCdrom() const noexcept;
    // The guest has locked the disc in the drive (Linux does it while the
    // disc is mounted): replacing it from the host leaves a stale mount.
    bool CdromLocked() const noexcept;
    // Host-side media change (VM-wide lock held): inserts `image`, or ejects
    // the disc when it is nullptr. The guest is told through a unit attention
    // and a media event, as with a real drive.
    void ChangeCdromMedia(virtualiseur::whp::Iso9660Image* image);

    std::uint32_t IoRead(std::uint16_t port, std::uint8_t size) override;
    void IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value) override;

private:
    enum class Kind : std::uint8_t { None, Disk, Cdrom };
    enum class Transfer : std::uint8_t { None, PioIn, PioOut, DmaIn, DmaOut, Packet, AtapiPioIn, AtapiDmaIn };

    struct Channel {
        Kind kind = Kind::None;
        virtualiseur::whp::VirtualDisk* disk = nullptr;
        virtualiseur::whp::Iso9660Image* iso = nullptr;
        std::uint8_t irq = 14;

        // Task file (current and "previous" values for LBA48).
        std::uint8_t feature = 0, nsector = 0, lbaLow = 0, lbaMid = 0, lbaHigh = 0;
        std::uint8_t hobFeature = 0, hobNsector = 0, hobLbaLow = 0, hobLbaMid = 0, hobLbaHigh = 0;
        std::uint8_t select = 0xA0;
        std::uint8_t status = 0;
        std::uint8_t error = 0;
        std::uint8_t control = 0;

        Transfer transfer = Transfer::None;
        std::vector<std::uint8_t> buffer;
        std::size_t position = 0;
        std::size_t end = 0;
        std::uint64_t lba = 0;
        std::uint32_t sectorsRemaining = 0;
        bool lba48 = false;

        // ATAPI.
        std::array<std::uint8_t, 12> packet{};
        std::uint16_t byteCountLimit = 0;
        std::uint8_t senseKey = 0;
        std::uint8_t asc = 0;
        bool ejected = false;
        std::uint8_t mediaEvent = 0;  // Pending GET EVENT STATUS media event (2 new, 3 removed).
        bool unitAttention = false;   // Next command fails with "medium may have changed".
        bool locked = false;          // PREVENT ALLOW MEDIUM REMOVAL: the guest uses the disc (mounted).

        // Bus master registers.
        std::uint8_t bmCommand = 0;
        std::uint8_t bmStatus = 0;
        std::uint32_t bmPrd = 0;

        bool irqLine = false;
    };

    std::uint32_t ChannelRead(Channel& channel, std::uint32_t reg, std::uint8_t size);
    void ChannelWrite(Channel& channel, std::uint32_t reg, std::uint8_t size, std::uint32_t value);
    void ControlWrite(Channel& channel, std::uint8_t value);
    void BusMasterWrite(std::uint32_t offset, std::uint8_t value);
    std::uint8_t BusMasterRead(std::uint32_t offset) const;

    void SetSignature(Channel& channel);
    void RaiseIrq(Channel& channel);
    void LowerIrq(Channel& channel);
    void Abort(Channel& channel, std::uint8_t error = 0x04);
    void Complete(Channel& channel);

    void ExecuteAta(Channel& channel, std::uint8_t command);
    void ExecuteAtapiCommand(Channel& channel, std::uint8_t command);
    void BuildIdentify(Channel& channel);
    void BuildIdentifyPacket(Channel& channel);
    std::uint64_t TaskFileLba(const Channel& channel, bool lba48) const;
    std::uint32_t TaskFileCount(const Channel& channel, bool lba48) const;
    void SetTaskFileLba(Channel& channel, std::uint64_t lba);
    void LoadNextReadSector(Channel& channel);
    void FinishPioSector(Channel& channel);
    void TryStartDma(Channel& channel);

    void ExecutePacket(Channel& channel);
    void AtapiCheckCondition(Channel& channel, std::uint8_t key, std::uint8_t asc);
    void AtapiStartPioPhase(Channel& channel);
    void AtapiReply(Channel& channel, std::vector<std::uint8_t> data, std::size_t allocationLength);

    bool DmaTransfer(Channel& channel, std::uint8_t* data, std::size_t length, bool toGuest);

    const GuestMemory& memory_;
    InterruptSink& sink_;
    std::array<Channel, 2> channels_{};
};

} // namespace vmm
