#pragma once

#include "DiskLayer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <vector>

#include <Windows.h>

namespace virtualiseur::whp {

// Disk image used by the in-house device models: a sparse raw image (no
// container header: sector zero is guest LBA 0), or a chain of layers
// (LayerFile) over such an image. With layers, only the top one is written;
// the layers below and the raw base are opened read-only.
class VirtualDisk final {
public:
    static constexpr std::uint32_t SectorSize = 512;

    static VirtualDisk CreateSparse(const std::filesystem::path& path, std::uint64_t sizeBytes);
    // `path` is a raw image or the top layer of a chain (parents resolved in
    // the same folder).
    static VirtualDisk Open(const std::filesystem::path& path, bool readOnly = false);

    ~VirtualDisk();
    VirtualDisk(const VirtualDisk&) = delete;
    VirtualDisk& operator=(const VirtualDisk&) = delete;
    VirtualDisk(VirtualDisk&& other) noexcept;
    VirtualDisk& operator=(VirtualDisk&& other) noexcept;

    std::uint64_t SizeBytes() const noexcept;
    std::uint64_t SectorCount() const noexcept;
    const std::filesystem::path& Path() const noexcept;
    // Number of layers over the raw base (0: plain raw image).
    std::size_t LayerCount() const noexcept { return layers_.size(); }
    bool IsReadOnly() const noexcept;

    void ReadSectors(std::uint64_t lba, std::span<std::byte> destination) const;
    void WriteSectors(std::uint64_t lba, std::span<const std::byte> source);
    void Flush();
    // Running VM, live snapshot: flushes the current top (layer or raw
    // image), reopens it read-only and puts the empty layer `newTop` (whose
    // parent it is) on top. In-flight I/O completes first; new I/O waits.
    void PushLayer(const std::filesystem::path& newTop);

private:
    VirtualDisk(HANDLE file, std::filesystem::path path, std::uint64_t sizeBytes, bool readOnly) noexcept;
    void Close() noexcept;
    // Reads part of `block` from the first layer at or below `fromLayer` that has it, else from the base.
    void ReadFromChain(std::size_t fromLayer, std::uint64_t block, std::uint32_t offset, std::span<std::byte> destination) const;

    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::filesystem::path path_;
    std::uint64_t sizeBytes_ = 0;
    bool readOnly_ = false;
    std::vector<LayerFile> layers_; // Top first; file_ is then the read-only raw base.
    mutable std::mutex ioMutex_;
    // Shared by every I/O, exclusive while PushLayer swaps the top.
    mutable std::shared_mutex switchMutex_;
};

} // namespace virtualiseur::whp
