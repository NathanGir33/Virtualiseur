#include "Pflash.h"

#include "../core/Log.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace vmm {
namespace {

constexpr std::uint64_t kPage = 4096;
constexpr std::uint64_t kBlock = 4096; // Erase block, as QEMU gives OVMF.
constexpr std::uint64_t kFourGiB = 0x100000000ull;
constexpr std::uint64_t kMaxFlash = 16ull * 1024 * 1024; // Below the I/O APIC area.

std::vector<std::uint8_t> ReadImage(const std::filesystem::path& path, const char* what)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error(std::string("Firmware UEFI : ") + what + " introuvable (" + path.string() + ").");
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.empty() || data.size() % kPage != 0 || data.size() > kMaxFlash) {
        throw std::runtime_error(std::string("Firmware UEFI : ") + what + " de taille invalide (" + path.string() + ").");
    }
    return data;
}

std::uint8_t* AllocatePages(std::size_t size)
{
    auto* memory = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (memory == nullptr) throw std::runtime_error("Firmware UEFI : mémoire de la flash impossible à allouer.");
    return memory;
}

constexpr auto kReadOnly = static_cast<WHV_MAP_GPA_RANGE_FLAGS>(WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute);

} // namespace

FirmwareFlash::FirmwareFlash(virtualiseur::whp::Partition& partition, const std::filesystem::path& code,
                             const std::filesystem::path& vars)
    : partition_(partition), varsPath_(vars)
{
    // One VM per variable store.
    auto lockPath = vars;
    lockPath += L".lock";
    lockFile_ = CreateFileW(lockPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (lockFile_ == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Firmware UEFI : " + vars.string() + " est déjà utilisé par une autre machine.");
    }

    const auto codeImage = ReadImage(code, "OVMF_CODE");
    const auto varsImage = ReadImage(vars, "magasin de variables");
    codeSize_ = codeImage.size();
    varsSize_ = varsImage.size();
    if (codeSize_ + varsSize_ > kMaxFlash) throw std::runtime_error("Firmware UEFI : flash de plus de 16 Mio.");
    base_ = kFourGiB - codeSize_ - varsSize_;

    code_ = AllocatePages(codeSize_);
    vars_ = AllocatePages(varsSize_);
    std::memcpy(code_, codeImage.data(), codeSize_);
    std::memcpy(vars_, varsImage.data(), varsSize_);
    partition_.MapDeviceMemory(code_, base_ + varsSize_, codeSize_, kReadOnly);
    partition_.MapDeviceMemory(vars_, base_, varsSize_, kReadOnly);
    storeMapped_ = true;

    // CFI query table (byte-wide device, one erase region of 4 KiB blocks).
    const auto blocks = static_cast<std::uint32_t>(varsSize_ / kBlock);
    unsigned long sizeLog2 = 0;
    _BitScanForward64(&sizeLog2, varsSize_);
    const std::uint8_t table[][2] = {
        {0x10, 'Q'}, {0x11, 'R'}, {0x12, 'Y'},
        {0x13, 0x01}, {0x14, 0x00},       // Primary command set: Intel/Sharp.
        {0x15, 0x31}, {0x16, 0x00},       // Primary extended table at 0x31.
        {0x1B, 0x45}, {0x1C, 0x55},       // Vcc 4.5 - 5.5 V.
        {0x1F, 0x07}, {0x20, 0x00},       // Byte program 2^7 us; no buffered write.
        {0x21, 0x0A}, {0x22, 0x00},       // Block erase 2^10 ms; no chip erase.
        {0x23, 0x04}, {0x24, 0x00}, {0x25, 0x04}, {0x26, 0x00},
        {0x27, static_cast<std::uint8_t>(sizeLog2)},
        {0x28, 0x00}, {0x29, 0x00},       // x8 interface.
        {0x2A, 0x00}, {0x2B, 0x00},
        {0x2C, 0x01},                     // One erase block region:
        {0x2D, static_cast<std::uint8_t>((blocks - 1) & 0xFF)}, {0x2E, static_cast<std::uint8_t>((blocks - 1) >> 8)},
        {0x2F, static_cast<std::uint8_t>((kBlock >> 8) & 0xFF)}, {0x30, static_cast<std::uint8_t>(kBlock >> 16)},
        {0x31, 'P'}, {0x32, 'R'}, {0x33, 'I'}, {0x34, '1'}, {0x35, '0'}, {0x36, 0x00},
    };
    for (const auto& entry : table) cfi_[entry[0]] = entry[1];

    flusher_ = std::thread([this] { FlushWorker(); });
    VMM_INFO("Firmware UEFI : %s (%llu Kio) et variables %s (%llu Kio) en flash à 0x%llx.",
             code.filename().string().c_str(), static_cast<unsigned long long>(codeSize_ / 1024),
             vars.filename().string().c_str(), static_cast<unsigned long long>(varsSize_ / 1024),
             static_cast<unsigned long long>(base_));
}

FirmwareFlash::~FirmwareFlash()
{
    {
        const std::lock_guard guard(flushMutex_);
        stopping_ = true;
    }
    flushWake_.notify_all();
    if (flusher_.joinable()) flusher_.join();
    Flush(); // Last burst: the VM stops or resets.
    // The partition is deleted before its devices: unmapping is moot then.
    if (code_ != nullptr) VirtualFree(code_, 0, MEM_RELEASE);
    if (vars_ != nullptr) VirtualFree(vars_, 0, MEM_RELEASE);
    if (lockFile_ != INVALID_HANDLE_VALUE) CloseHandle(lockFile_);
}

void FirmwareFlash::SetMode(Mode mode)
{
    mode_ = mode;
    const bool mapped = mode == Mode::Array;
    if (mapped == storeMapped_) return;
    if (mapped) partition_.MapDeviceMemory(vars_, base_, varsSize_, kReadOnly);
    else partition_.UnmapDeviceMemory(base_, varsSize_);
    storeMapped_ = mapped;
}

std::uint8_t FirmwareFlash::ReadByte(std::uint64_t offset) const
{
    if (offset >= varsSize_) return offset - varsSize_ < codeSize_ ? code_[offset - varsSize_] : 0xFF;
    switch (mode_) {
    case Mode::Status: return status_;
    case Mode::Query: {
        const auto index = static_cast<std::size_t>(offset & 0xFF);
        return index < cfi_.size() ? cfi_[index] : 0;
    }
    case Mode::Identify: return (offset & 1) == 0 ? 0x89 : 0x18; // Intel, 28F128-like.
    case Mode::Array: break;
    }
    return vars_[offset];
}

std::uint64_t FirmwareFlash::MmioRead(std::uint64_t address, std::uint8_t size)
{
    std::uint64_t value = 0;
    const auto offset = address - base_;
    for (std::uint8_t index = 0; index < size && index < 8; ++index) {
        value |= static_cast<std::uint64_t>(ReadByte(offset + index)) << (index * 8);
    }
    return value;
}

void FirmwareFlash::MmioWrite(std::uint64_t address, std::uint8_t size, std::uint64_t value)
{
    const auto offset = address - base_;
    if (offset >= varsSize_) {
        // Code flash: read-only unit.
        if (!codeWriteReported_) {
            codeWriteReported_ = true;
            VMM_DEBUG("Firmware UEFI : écriture ignorée dans le code (0x%llx), lecture seule comme sous QEMU (sonde du firmware).",
                      static_cast<unsigned long long>(address));
        }
        return;
    }
    const auto first = static_cast<std::uint8_t>(value);
    if (pending_ == Pending::Program) {
        // NOR: programming only clears bits.
        for (std::uint8_t index = 0; index < size && index < 8 && offset + index < varsSize_; ++index) {
            vars_[offset + index] &= static_cast<std::uint8_t>(value >> (index * 8));
        }
        pending_ = Pending::None;
        status_ |= 0x80;
        SetMode(Mode::Status);
        Modified();
        return;
    }
    if (pending_ == Pending::Erase) {
        pending_ = Pending::None;
        if (first == 0xD0) {
            std::memset(vars_ + (offset & ~(kBlock - 1)), 0xFF, kBlock);
            status_ |= 0x80;
            Modified();
        } else {
            status_ |= 0x30; // Command sequence error.
        }
        SetMode(Mode::Status);
        return;
    }
    Command(offset, first);
}

void FirmwareFlash::Command(std::uint64_t, std::uint8_t command)
{
    switch (command) {
    case 0x00: case 0xF0: case 0xFF: SetMode(Mode::Array); break;
    case 0x10: case 0x40: pending_ = Pending::Program; SetMode(Mode::Status); break;
    case 0x20: pending_ = Pending::Erase; SetMode(Mode::Status); break;
    // As QEMU: the whole status register is cleared and reads go back to the
    // array (OVMF's flash detection expects Read Status to give 0x00 next).
    case 0x50: status_ = 0; SetMode(Mode::Array); break;
    case 0x70: SetMode(Mode::Status); break;
    case 0x90: SetMode(Mode::Identify); break;
    case 0x98: SetMode(Mode::Query); break;
    default:
        if (reported_.insert(command).second) VMM_WARN("Firmware UEFI : commande flash 0x%02x non prise en charge.", command);
        status_ |= 0x10;
        SetMode(Mode::Status);
        break;
    }
}

void FirmwareFlash::Modified()
{
    const auto now = std::chrono::steady_clock::now();
    {
        const std::lock_guard guard(flushMutex_);
        if (!dirty_) firstChange_ = now;
        dirty_ = true;
        lastChange_ = now;
    }
    flushWake_.notify_all();
}

void FirmwareFlash::FlushWorker()
{
    SetThreadDescription(GetCurrentThread(), L"Flash UEFI");
    std::unique_lock guard(flushMutex_);
    for (;;) {
        flushWake_.wait(guard, [this] { return stopping_ || dirty_; });
        if (stopping_) return;
        flushWake_.wait_for(guard, std::chrono::milliseconds(100), [this] { return stopping_; });
        if (stopping_) return; // The destructor flushes.
        const auto now = std::chrono::steady_clock::now();
        const bool quiet = now - lastChange_ >= std::chrono::milliseconds(100);
        const bool overdue = now - firstChange_ >= std::chrono::seconds(1);
        if (!quiet && !overdue) continue;
        guard.unlock();
        std::vector<std::uint8_t> copy;
        {
            const DeviceGuard device(lock_);
            // End of a burst: the firmware is back in read array mode.
            if (mode_ == Mode::Array || overdue) {
                const std::lock_guard state(flushMutex_);
                dirty_ = false;
                copy.assign(vars_, vars_ + varsSize_);
            }
        }
        if (!copy.empty() && !WriteStore(copy)) {
            const std::lock_guard state(flushMutex_);
            dirty_ = true; // Retried at the next change or at the end.
        }
        guard.lock();
    }
}

void FirmwareFlash::Flush()
{
    std::vector<std::uint8_t> copy;
    {
        const DeviceGuard device(lock_);
        const std::lock_guard state(flushMutex_);
        if (!dirty_) return;
        dirty_ = false;
        copy.assign(vars_, vars_ + varsSize_);
    }
    if (!WriteStore(copy)) {
        const std::lock_guard state(flushMutex_);
        dirty_ = true;
    }
}

bool FirmwareFlash::WriteStore(const std::vector<std::uint8_t>& data)
{
    auto temporary = varsPath_;
    temporary += L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = file != INVALID_HANDLE_VALUE;
    if (ok) {
        DWORD written = 0;
        ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size() &&
             FlushFileBuffers(file);
        CloseHandle(file);
    }
    ok = ok && MoveFileExW(temporary.c_str(), varsPath_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        VMM_WARN("Firmware UEFI : écriture de %s impossible (erreur %lu).", varsPath_.string().c_str(), GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    VMM_DEBUG("Firmware UEFI : variables écrites dans %s.", varsPath_.filename().string().c_str());
    return true;
}

} // namespace vmm
