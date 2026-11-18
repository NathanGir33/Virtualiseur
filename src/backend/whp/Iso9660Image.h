#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace virtualiseur::whp {

// Read-only ISO 9660 primary-volume reader for host-side boot asset extraction.
// This is an ISO file parser, not a guest optical-drive device.
class Iso9660Image final {
public:
    static constexpr std::uint32_t LogicalBlockSize = 2048;

    explicit Iso9660Image(const std::filesystem::path& imagePath);

    Iso9660Image(const Iso9660Image&) = delete;
    Iso9660Image& operator=(const Iso9660Image&) = delete;

    bool ContainsFile(const std::string& isoRelativePath) const;
    std::uint64_t SizeBytes() const noexcept;
    // Volume identifier of the primary volume descriptor, trailing spaces removed.
    const std::string& VolumeLabel() const noexcept { return volumeLabel_; }
    void ReadLogicalBlocks(std::uint32_t firstBlock, std::span<std::byte> destination) const;
    std::uint64_t ExtractFile(
        const std::string& isoRelativePath,
        const std::filesystem::path& destinationPath) const;
    std::vector<std::uint8_t> ReadWholeFile(const std::string& isoRelativePath) const;

private:
    struct Entry {
        std::uint32_t extentLba = 0;
        std::uint32_t sizeBytes = 0;
        std::uint8_t flags = 0;
    };

    Entry FindEntry(const std::string& isoRelativePath) const;
    Entry FindInDirectory(const Entry& directory, const std::string& component) const;
    void ReadAt(std::uint64_t offset, void* destination, std::size_t size) const;
    static Entry ParseDirectoryRecord(const std::uint8_t* record, std::size_t available);
    // Rock Ridge alternate name (NM entries of the System Use area), empty if none.
    static std::string RockRidgeName(const std::uint8_t* record, std::size_t recordLength);

    std::filesystem::path imagePath_;
    mutable std::ifstream input_;
    mutable std::mutex inputMutex_;
    std::uint64_t imageSizeBytes_ = 0;
    Entry rootDirectory_{};
    std::string volumeLabel_;
};

} // namespace virtualiseur::whp
