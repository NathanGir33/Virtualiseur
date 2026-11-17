#include "DiskLayer.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace virtualiseur::whp {
namespace {

constexpr char kMagic[8] = {'V', 'R', 'T', 'L', 'A', 'Y', 'R', '1'};
constexpr std::uint32_t kVersion = 1;

// On-disk header, little endian, padded to kHeaderSize.
#pragma pack(push, 1)
struct Header {
    char magic[8];
    std::uint32_t version;
    std::uint32_t blockSize;
    std::uint64_t virtualSize;
    std::uint64_t blockCount;
    std::uint64_t tableOffset;
    std::uint64_t dataOffset;
    std::uint32_t parentLength; // UTF-16 code units.
    wchar_t parent[LayerFile::kMaxParentName];
};
#pragma pack(pop)
static_assert(sizeof(Header) <= LayerFile::kHeaderSize);

[[noreturn]] void ThrowLastError(const char* operation)
{
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

std::uint64_t AlignUp(std::uint64_t value, std::uint64_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

std::uint64_t FileSize(HANDLE file)
{
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) ThrowLastError("GetFileSizeEx");
    return static_cast<std::uint64_t>(size.QuadPart);
}

} // namespace

void ReadAt(HANDLE file, std::uint64_t offset, void* destination, std::size_t size)
{
    auto* bytes = static_cast<std::uint8_t*>(destination);
    std::size_t done = 0;
    while (done < size) {
        const auto chunk = static_cast<DWORD>((std::min<std::size_t>)(size - done, 64u * 1024 * 1024));
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset + done);
        position.OffsetHigh = static_cast<DWORD>((offset + done) >> 32);
        DWORD transferred = 0;
        if (!ReadFile(file, bytes + done, chunk, &transferred, &position)) {
            if (GetLastError() != ERROR_HANDLE_EOF) ThrowLastError("ReadFile");
            transferred = 0;
        }
        if (transferred == 0) {
            std::memset(bytes + done, 0, size - done); // Past the end of the file: zeros.
            return;
        }
        done += transferred;
    }
}

void WriteAt(HANDLE file, std::uint64_t offset, const void* source, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(source);
    std::size_t done = 0;
    while (done < size) {
        const auto chunk = static_cast<DWORD>((std::min<std::size_t>)(size - done, 64u * 1024 * 1024));
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset + done);
        position.OffsetHigh = static_cast<DWORD>((offset + done) >> 32);
        DWORD transferred = 0;
        if (!WriteFile(file, bytes + done, chunk, &transferred, &position)) ThrowLastError("WriteFile");
        if (transferred == 0) throw std::runtime_error("Écriture nulle inattendue dans une couche de disque.");
        done += transferred;
    }
}

bool LayerFile::IsLayer(const std::filesystem::path& path)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char magic[sizeof(kMagic)] = {};
    DWORD read = 0;
    const bool ok = ReadFile(file, magic, sizeof(magic), &read, nullptr) && read == sizeof(magic) &&
                    std::memcmp(magic, kMagic, sizeof(kMagic)) == 0;
    CloseHandle(file);
    return ok;
}

std::wstring LayerFile::ParentOf(const std::filesystem::path& path)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::vector<std::uint8_t> block(kHeaderSize, 0);
    DWORD read = 0;
    const bool ok = ReadFile(file, block.data(), kHeaderSize, &read, nullptr) && read == kHeaderSize;
    CloseHandle(file);
    const auto* header = reinterpret_cast<const Header*>(block.data());
    if (!ok || std::memcmp(header->magic, kMagic, sizeof(kMagic)) != 0 || header->parentLength == 0 ||
        header->parentLength > kMaxParentName) {
        return {};
    }
    return std::wstring(header->parent, header->parentLength);
}

std::uint64_t LayerFile::SizeOf(const std::filesystem::path& path)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    std::vector<std::uint8_t> block(kHeaderSize, 0);
    DWORD read = 0;
    const bool ok = ReadFile(file, block.data(), kHeaderSize, &read, nullptr) && read == kHeaderSize;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    CloseHandle(file);
    const auto* header = reinterpret_cast<const Header*>(block.data());
    if (ok && std::memcmp(header->magic, kMagic, sizeof(kMagic)) == 0) return header->virtualSize;
    return static_cast<std::uint64_t>(size.QuadPart);
}

void LayerFile::Create(const std::filesystem::path& path, const std::wstring& parentName, std::uint64_t virtualSize)
{
    if (parentName.empty() || parentName.size() > kMaxParentName ||
        parentName.find_first_of(L"\\/") != std::wstring::npos) {
        throw std::invalid_argument("Nom de couche parente invalide.");
    }
    if (virtualSize == 0 || virtualSize % 512 != 0) throw std::invalid_argument("Taille de disque invalide.");
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) ThrowLastError("CreateFileW(couche)");
    try {
        DWORD ignored = 0;
        (void)DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ignored, nullptr); // Zero table: free.
        std::vector<std::uint8_t> block(kHeaderSize, 0);
        auto* header = reinterpret_cast<Header*>(block.data());
        std::memcpy(header->magic, kMagic, sizeof(kMagic));
        header->version = kVersion;
        header->blockSize = kBlockSize;
        header->virtualSize = virtualSize;
        header->blockCount = (virtualSize + kBlockSize - 1) / kBlockSize;
        header->tableOffset = kHeaderSize;
        header->dataOffset = AlignUp(kHeaderSize + header->blockCount * sizeof(std::uint64_t), kBlockSize);
        header->parentLength = static_cast<std::uint32_t>(parentName.size());
        std::memcpy(header->parent, parentName.data(), parentName.size() * sizeof(wchar_t));
        WriteAt(file, 0, block.data(), block.size());
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(header->dataOffset);
        if (!SetFilePointerEx(file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) ThrowLastError("SetEndOfFile");
        if (!FlushFileBuffers(file)) ThrowLastError("FlushFileBuffers");
        CloseHandle(file);
    } catch (...) {
        CloseHandle(file);
        (void)DeleteFileW(path.c_str());
        throw;
    }
}

LayerFile LayerFile::Open(const std::filesystem::path& path, bool writable)
{
    LayerFile layer;
    // Read-only layers may be opened by several VMs at once (linked clones).
    layer.file_ = CreateFileW(path.c_str(), writable ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                              nullptr);
    if (layer.file_ == INVALID_HANDLE_VALUE) ThrowLastError("CreateFileW(couche)");
    layer.path_ = path;
    layer.writable_ = writable;

    std::vector<std::uint8_t> block(kHeaderSize, 0);
    ReadAt(layer.file_, 0, block.data(), block.size());
    const auto* header = reinterpret_cast<const Header*>(block.data());
    const auto expectedBlocks = (header->virtualSize + kBlockSize - 1) / kBlockSize;
    if (std::memcmp(header->magic, kMagic, sizeof(kMagic)) != 0 || header->version != kVersion ||
        header->blockSize != kBlockSize || header->virtualSize == 0 || header->blockCount != expectedBlocks ||
        header->tableOffset != kHeaderSize ||
        header->dataOffset < AlignUp(kHeaderSize + header->blockCount * sizeof(std::uint64_t), kBlockSize) ||
        header->parentLength == 0 || header->parentLength > kMaxParentName) {
        throw std::runtime_error("Couche de disque invalide : " + path.string());
    }
    layer.virtualSize_ = header->virtualSize;
    layer.tableOffset_ = header->tableOffset;
    layer.dataOffset_ = header->dataOffset;
    layer.parent_.assign(header->parent, header->parentLength);

    layer.table_.assign(static_cast<std::size_t>(header->blockCount), 0);
    ReadAt(layer.file_, layer.tableOffset_, layer.table_.data(), layer.table_.size() * sizeof(std::uint64_t));

    // Crash check: a block whose data did not fully reach the file is absent.
    const auto size = FileSize(layer.file_);
    for (auto& entry : layer.table_) {
        if (entry == 0) continue;
        if (entry % kBlockSize != 0 || entry < layer.dataOffset_ || entry > size || size - entry < kBlockSize) {
            entry = 0;
            ++layer.ignored_;
        }
    }
    layer.next_ = (std::max)(layer.dataOffset_, AlignUp(size, kBlockSize));
    return layer;
}

LayerFile::~LayerFile()
{
    Close();
}

LayerFile::LayerFile(LayerFile&& other) noexcept
{
    *this = std::move(other);
}

LayerFile& LayerFile::operator=(LayerFile&& other) noexcept
{
    if (this != &other) {
        Close();
        file_ = std::exchange(other.file_, INVALID_HANDLE_VALUE);
        path_ = std::move(other.path_);
        parent_ = std::move(other.parent_);
        virtualSize_ = other.virtualSize_;
        tableOffset_ = other.tableOffset_;
        dataOffset_ = other.dataOffset_;
        next_ = other.next_;
        ignored_ = other.ignored_;
        writable_ = other.writable_;
        table_ = std::move(other.table_);
    }
    return *this;
}

void LayerFile::Close() noexcept
{
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
}

std::vector<std::uint64_t> LayerFile::AllocatedBlocks() const
{
    std::vector<std::uint64_t> blocks;
    for (std::uint64_t index = 0; index < table_.size(); ++index) {
        if (table_[index] != 0) blocks.push_back(index);
    }
    return blocks;
}

void LayerFile::Read(std::uint64_t block, std::uint32_t offset, std::span<std::byte> destination) const
{
    if (!Has(block) || offset + destination.size() > kBlockSize) throw std::out_of_range("Bloc de couche absent.");
    ReadAt(file_, table_[block] + offset, destination.data(), destination.size());
}

void LayerFile::Write(std::uint64_t block, std::uint32_t offset, std::span<const std::byte> source)
{
    if (!writable_) throw std::logic_error("Couche figée : écriture refusée.");
    if (!Has(block) || offset + source.size() > kBlockSize) throw std::out_of_range("Bloc de couche absent.");
    WriteAt(file_, table_[block] + offset, source.data(), source.size());
}

void LayerFile::Append(std::uint64_t block, std::span<const std::byte> data)
{
    if (!writable_) throw std::logic_error("Couche figée : écriture refusée.");
    if (block >= table_.size() || Has(block) || data.size() != kBlockSize) throw std::out_of_range("Bloc de couche invalide.");
    // Data first, then the table entry: a crash in between leaves the block
    // absent (or, once the data is complete, present), never half-written.
    const auto offset = next_;
    WriteAt(file_, offset, data.data(), data.size());
    WriteAt(file_, tableOffset_ + block * sizeof(std::uint64_t), &offset, sizeof(offset));
    table_[block] = offset;
    next_ = offset + kBlockSize;
}

void LayerFile::SetParent(const std::wstring& parentName)
{
    if (!writable_) throw std::logic_error("Couche ouverte en lecture seule.");
    if (parentName.empty() || parentName.size() > kMaxParentName || parentName.find_first_of(L"\\/") != std::wstring::npos) {
        throw std::invalid_argument("Nom de couche parente invalide.");
    }
    // Length and name live in the first header sector: one write.
    std::vector<std::uint8_t> block(kHeaderSize, 0);
    ReadAt(file_, 0, block.data(), block.size());
    auto* header = reinterpret_cast<Header*>(block.data());
    header->parentLength = static_cast<std::uint32_t>(parentName.size());
    std::memset(header->parent, 0, sizeof(header->parent));
    std::memcpy(header->parent, parentName.data(), parentName.size() * sizeof(wchar_t));
    WriteAt(file_, 0, block.data(), block.size());
    if (!FlushFileBuffers(file_)) ThrowLastError("FlushFileBuffers");
    parent_ = parentName;
}

void LayerFile::Flush()
{
    if (writable_ && !FlushFileBuffers(file_)) ThrowLastError("FlushFileBuffers");
}

} // namespace virtualiseur::whp
