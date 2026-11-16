#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>

#include <Windows.h>

namespace virtualiseur::whp {

// Simple sparse, raw disk image used by the in-house device models.
// The file has no container header: sector zero is guest LBA 0.
class VirtualDisk final {
public:
    static constexpr std::uint32_t SectorSize = 512;

    static VirtualDisk CreateSparse(const std::filesystem::path& path, std::uint64_t sizeBytes);
    static VirtualDisk Open(const std::filesystem::path& path, bool readOnly = false);

    ~VirtualDisk();
    VirtualDisk(const VirtualDisk&) = delete;
    VirtualDisk& operator=(const VirtualDisk&) = delete;
    VirtualDisk(VirtualDisk&& other) noexcept;
    VirtualDisk& operator=(VirtualDisk&& other) noexcept;

    std::uint64_t SizeBytes() const noexcept;
    std::uint64_t SectorCount() const noexcept;
    const std::filesystem::path& Path() const noexcept;
    bool IsReadOnly() const noexcept;

    void ReadSectors(std::uint64_t lba, std::span<std::byte> destination) const;
    void WriteSectors(std::uint64_t lba, std::span<const std::byte> source);
    void Flush();

private:
    VirtualDisk(HANDLE file, std::filesystem::path path, std::uint64_t sizeBytes, bool readOnly) noexcept;
    void Close() noexcept;

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::filesystem::path path_;
    std::uint64_t sizeBytes_ = 0;
    bool readOnly_ = false;
    mutable std::mutex ioMutex_;
};

} // namespace virtualiseur::whp
