#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace virtualiseur::whp {

// Exported machine (.vrtx): its settings and one flattened disk state, in a
// single file. Layout (little endian):
//
//   header   magic "VRTXMACH", format version, header size, block size
//            (64 KiB), disk virtual size, config size, CRC32 of the header
//   config   the machine settings (UTF-8 text), then its CRC32
//   blocks   one record per non-zero 64 KiB block of the disk:
//            {index, stored size, real size, flags, CRC32 of the real data}
//            then the stored bytes: XPRESS-compressed (Windows compression
//            API), or as is when compression does not shrink the block
//   end      a record with index ~0 carrying the number of blocks written,
//            so a truncated file is detected
//
// Every size read back is checked against its bound before anything is
// allocated; a CRC mismatch, an unknown flag or a short file is an error.
//
// `progress(done, total)` returns false to cancel (the function then throws
// ArchiveCancelled after removing what it was writing).
using ArchiveProgress = std::function<bool(std::uint64_t done, std::uint64_t total)>;

struct ArchiveCancelled final : std::exception {
    const char* what() const noexcept override { return "Opération annulée."; }
};

// Writes `target` (through a .tmp file renamed at the end) from the disk
// state `disk` (a raw image or the top layer of a chain, opened read-only).
void ExportMachine(const std::filesystem::path& disk, const std::string& config, const std::filesystem::path& target,
                   const ArchiveProgress& progress);

// Header and settings only (to propose a name and check the file).
struct ArchiveInfo {
    std::uint64_t virtualSize = 0;
    std::string config; // UTF-8.
};
ArchiveInfo ReadArchiveInfo(const std::filesystem::path& archive);

// Recreates the disk as a new sparse raw image `disk` (which must not exist).
// On any error or cancellation `disk` is deleted before the exception leaves.
void ImportDisk(const std::filesystem::path& archive, const std::filesystem::path& disk, const ArchiveProgress& progress);

} // namespace virtualiseur::whp
