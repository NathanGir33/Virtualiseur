#pragma once

#include "../core/Bus.h"
#include "WindowsHypervisorBackend.h"

#include <Windows.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace vmm {

// UEFI firmware flash at the top of the 4 GiB space, as QEMU's "pc" machine
// gives it to OVMF (two pflash_cfi01 units, 1 byte wide, 4 KiB blocks):
//
//   [base, base + varsSize)   variable store (the VM's vars.fd), writable
//   [.., 4 GiB)               firmware code (OVMF_CODE), read-only; it holds
//                             the reset vector at 0xFFFFFFF0
//
// Reads never leave the hypervisor in "read array" mode: the code is mapped
// read + execute, the store read-only. A write to the store (a page mapped
// read-only: a memory-access exit with GpaUnmapped == 0) reaches MmioWrite
// through the instruction emulator, which then advances RIP; any command
// other than Read Array unmaps the store so that every access is emulated
// (status register, CFI query...) until Read Array maps it back.
//
// Intel/Sharp command set, with QEMU's behaviour where OVMF depends on it
// (QemuFlashDetected: Clear Status clears the whole status register and
// returns to read array; Read Status then reads 0x00):
//   0xFF / 0x00   Read Array          0x70  Read Status
//   0x50          Clear Status        0x90  Read Identifier
//   0x98          CFI Query           0x10 / 0x40 + data  Program byte(s)
//   0x20 + 0xD0   Block Erase (4 KiB block filled with 0xFF)
// Programming is NOR-like: bits only go from 1 to 0 (old & new). Other
// commands set the status error bits.
//
// The store is written back to its file in bursts: 100 ms after the last
// program/erase once the firmware is back in read array mode (1 s at most),
// and when the VM stops: copy to "<vars>.tmp", FlushFileBuffers, then
// MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH). A
// "<vars>.lock" file held for the VM's lifetime keeps a second VM away.
class FirmwareFlash final : public MmioDevice {
public:
    FirmwareFlash(virtualiseur::whp::Partition& partition, const std::filesystem::path& code,
                  const std::filesystem::path& vars);
    ~FirmwareFlash() override;
    FirmwareFlash(const FirmwareFlash&) = delete;
    FirmwareFlash& operator=(const FirmwareFlash&) = delete;

    std::uint64_t Base() const noexcept { return base_; }
    std::uint64_t Size() const noexcept { return codeSize_ + varsSize_; }
    DeviceLock& Lock() noexcept { return lock_; }

    std::uint64_t MmioRead(std::uint64_t address, std::uint8_t size) override;
    void MmioWrite(std::uint64_t address, std::uint8_t size, std::uint64_t value) override;

    // Writes the store now if it changed (also done by the destructor).
    void Flush();

private:
    enum class Mode : std::uint8_t { Array, Status, Query, Identify };
    enum class Pending : std::uint8_t { None, Program, Erase };

    void SetMode(Mode mode);
    void Command(std::uint64_t offset, std::uint8_t command);
    std::uint8_t ReadByte(std::uint64_t offset) const;
    void Modified();
    void FlushWorker();
    bool WriteStore(const std::vector<std::uint8_t>& data);

    virtualiseur::whp::Partition& partition_;
    std::filesystem::path varsPath_;
    std::uint64_t base_ = 0;
    std::uint64_t varsSize_ = 0;
    std::uint64_t codeSize_ = 0;
    std::uint8_t* code_ = nullptr; // VirtualAlloc'd, page-aligned.
    std::uint8_t* vars_ = nullptr;
    HANDLE lockFile_ = INVALID_HANDLE_VALUE;
    std::array<std::uint8_t, 0x40> cfi_{};

    DeviceLock lock_; // MMIO accesses (route lock) and the flush copy.
    Mode mode_ = Mode::Array;
    Pending pending_ = Pending::None;
    std::uint8_t status_ = 0x80; // Ready.
    bool storeMapped_ = false;
    std::unordered_set<std::uint32_t> reported_; // Unsupported commands, logged once.
    bool codeWriteReported_ = false;

    // Write-back.
    std::mutex flushMutex_;
    std::condition_variable flushWake_;
    bool dirty_ = false;
    bool stopping_ = false;
    std::chrono::steady_clock::time_point lastChange_{};
    std::chrono::steady_clock::time_point firstChange_{};
    std::thread flusher_;
};

} // namespace vmm
