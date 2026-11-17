#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <Windows.h>

namespace virtualiseur::whp {

// One layer of a layered disk: the blocks written since its parent was
// frozen. A chain of layers ends on a raw disk image (the base).
//
// File layout:
//   0       header (4 KiB): magic "VRTLAYR1", version, block size, virtual
//           size, block count, table and data offsets, parent file name
//           (same folder, UTF-16).
//   4 KiB   flat allocation table: one little-endian uint64_t per 64 KiB
//           block, the file offset of its data, 0 when the block is absent
//           (read from the parent). Loaded in memory when the file opens.
//   data    64 KiB blocks, appended at the end of the file as they are
//           first written (data first, then its table entry).
//
// On open, a table entry that is misaligned, before the data area or whose
// block runs past the end of the file (written before a crash) is ignored:
// the block reads from the parent again.
class LayerFile final {
public:
    static constexpr std::uint32_t kBlockSize = 64 * 1024;
    static constexpr std::uint32_t kHeaderSize = 4096;
    static constexpr std::size_t kMaxParentName = 1024;

    // True if `path` starts with the layer magic (otherwise: a raw image).
    static bool IsLayer(const std::filesystem::path& path);
    // Parent file name of a layer, empty for a raw image or an unreadable file.
    // Works while a VM has the layer open.
    static std::wstring ParentOf(const std::filesystem::path& path);
    // Virtual size of a layer or raw image (0 if unreadable).
    static std::uint64_t SizeOf(const std::filesystem::path& path);
    // Creates an empty layer over `parentName` (a file of the same folder).
    static void Create(const std::filesystem::path& path, const std::wstring& parentName, std::uint64_t virtualSize);
    static LayerFile Open(const std::filesystem::path& path, bool writable);

    LayerFile() = default;
    ~LayerFile();
    LayerFile(const LayerFile&) = delete;
    LayerFile& operator=(const LayerFile&) = delete;
    LayerFile(LayerFile&& other) noexcept;
    LayerFile& operator=(LayerFile&& other) noexcept;

    const std::filesystem::path& Path() const noexcept { return path_; }
    const std::wstring& ParentName() const noexcept { return parent_; }
    std::uint64_t VirtualSize() const noexcept { return virtualSize_; }
    std::uint64_t BlockCount() const noexcept { return table_.size(); }
    // Entries dropped by the crash check at open time.
    std::uint64_t IgnoredEntries() const noexcept { return ignored_; }

    bool Has(std::uint64_t block) const noexcept { return block < table_.size() && table_[block] != 0; }
    std::vector<std::uint64_t> AllocatedBlocks() const;
    // Part of a present block.
    void Read(std::uint64_t block, std::uint32_t offset, std::span<std::byte> destination) const;
    void Write(std::uint64_t block, std::uint32_t offset, std::span<const std::byte> source);
    // Allocates an absent block with its full 64 KiB of data.
    void Append(std::uint64_t block, std::span<const std::byte> data);
    // Rewrites the parent name in the header (deleting a snapshot).
    void SetParent(const std::wstring& parentName);
    void Flush();

private:
    void Close() noexcept;

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::filesystem::path path_;
    std::wstring parent_;
    std::uint64_t virtualSize_ = 0;
    std::uint64_t tableOffset_ = 0;
    std::uint64_t dataOffset_ = 0;
    std::uint64_t next_ = 0; // Where the next block goes.
    std::uint64_t ignored_ = 0;
    bool writable_ = false;
    std::vector<std::uint64_t> table_;
};

// Positional I/O helpers shared with the raw base image.
void ReadAt(HANDLE file, std::uint64_t offset, void* destination, std::size_t size);
void WriteAt(HANDLE file, std::uint64_t offset, const void* source, std::size_t size);

} // namespace virtualiseur::whp
