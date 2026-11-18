#include "MachineArchive.h"

#include "DiskLayer.h"
#include "VirtualDisk.h"

#include <Windows.h>
#include <compressapi.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

#pragma comment(lib, "Cabinet.lib")

namespace virtualiseur::whp {
namespace {

constexpr char kMagic[8] = {'V', 'R', 'T', 'X', 'M', 'A', 'C', 'H'};
constexpr std::uint32_t kVersion = 2;         // Written; version 1 (no firmware section) is still read.
constexpr std::uint32_t kMaxVars = 16u << 20; // OVMF stores are 128 KiB to 4 MiB.
constexpr std::uint32_t kBlockSize = LayerFile::kBlockSize;
constexpr std::uint64_t kMaxVirtualSize = 64ull << 40; // 64 TiB.
constexpr std::uint32_t kMaxConfig = 1u << 20;         // 1 MiB of settings text.
constexpr std::uint64_t kEndIndex = ~0ull;
constexpr std::uint32_t kFlagCompressed = 1;

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t headerSize;
    std::uint32_t blockSize;
    std::uint32_t reserved;
    std::uint64_t virtualSize;
    std::uint32_t configSize;
    std::uint32_t headerCrc; // CRC32 of this header with this field at 0.
};
struct BlockHeader {
    std::uint64_t index;      // Block number, or kEndIndex for the end record.
    std::uint32_t storedSize; // Bytes that follow in the file.
    std::uint32_t realSize;   // Bytes of the block once decompressed.
    std::uint32_t flags;      // kFlagCompressed or 0 (stored as is).
    std::uint32_t crc;        // CRC32 of the real (decompressed) bytes.
};
#pragma pack(pop)

[[noreturn]] void ThrowLastError(const char* operation)
{
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

[[noreturn]] void Corrupt(const char* why)
{
    throw std::runtime_error(std::string("Fichier .vrtx invalide ou abîmé : ") + why + ".");
}

std::uint32_t Crc32(const void* data, std::size_t size)
{
    using Function = ULONG(NTAPI*)(ULONG, const BYTE*, INT);
    static const auto compute = reinterpret_cast<Function>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlComputeCrc32"));
    if (compute == nullptr) throw std::runtime_error("RtlComputeCrc32 indisponible.");
    return compute(0, static_cast<const BYTE*>(data), static_cast<INT>(size));
}

std::uint32_t HeaderCrc(FileHeader header)
{
    header.headerCrc = 0;
    return Crc32(&header, sizeof(header));
}

// Expected real size of block `index` (the last one may be short).
std::uint32_t BlockLength(std::uint64_t index, std::uint64_t virtualSize)
{
    const auto offset = index * kBlockSize;
    return static_cast<std::uint32_t>((std::min<std::uint64_t>)(kBlockSize, virtualSize - offset));
}

class Handle final {
public:
    explicit Handle(HANDLE handle) noexcept : handle_(handle) {}
    ~Handle() { Close(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const noexcept { return handle_; }
    void Close() noexcept
    {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

// Sequential, buffered output.
class Output final {
public:
    explicit Output(HANDLE file) : file_(file) { buffer_.reserve(1u << 20); }
    void Write(const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        buffer_.insert(buffer_.end(), bytes, bytes + size);
        if (buffer_.size() >= (1u << 20)) Flush();
    }
    void Flush()
    {
        std::size_t done = 0;
        while (done < buffer_.size()) {
            DWORD written = 0;
            if (!WriteFile(file_, buffer_.data() + done, static_cast<DWORD>(buffer_.size() - done), &written, nullptr) ||
                written == 0) {
                ThrowLastError("WriteFile(.vrtx)");
            }
            done += written;
        }
        buffer_.clear();
    }

private:
    HANDLE file_;
    std::vector<std::uint8_t> buffer_;
};

// Sequential, buffered input with exact reads.
class Input final {
public:
    explicit Input(HANDLE file) : file_(file) {}
    void Read(void* data, std::size_t size)
    {
        auto* bytes = static_cast<std::uint8_t*>(data);
        while (size != 0) {
            if (position_ == filled_) {
                DWORD read = 0;
                if (!ReadFile(file_, buffer_, sizeof(buffer_), &read, nullptr)) ThrowLastError("ReadFile(.vrtx)");
                if (read == 0) Corrupt("fichier tronqué");
                position_ = 0;
                filled_ = read;
            }
            const auto count = (std::min<std::size_t>)(size, filled_ - position_);
            std::memcpy(bytes, buffer_ + position_, count);
            position_ += count;
            bytes += count;
            size -= count;
            consumed_ += count;
        }
    }
    std::uint64_t Consumed() const noexcept { return consumed_; }

private:
    HANDLE file_;
    std::uint8_t buffer_[1u << 20];
    std::size_t position_ = 0;
    std::size_t filled_ = 0;
    std::uint64_t consumed_ = 0;
};

} // namespace

namespace {

// The whole variable store, read while a running VM may replace it
// (MoveFileExW): shared for reading, writing and deletion.
std::vector<std::uint8_t> ReadVarsStore(const std::filesystem::path& path)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.Get() == INVALID_HANDLE_VALUE) ThrowLastError("CreateFileW(vars.fd)");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.Get(), &size)) ThrowLastError("GetFileSizeEx(vars.fd)");
    if (size.QuadPart <= 0 || size.QuadPart > kMaxVars) throw std::runtime_error("Magasin de variables UEFI (vars.fd) de taille impossible.");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.Get(), data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size()) {
        ThrowLastError("ReadFile(vars.fd)");
    }
    return data;
}

// An OVMF variable store is a firmware volume: "_FVH" at offset 40.
bool LooksLikeVarsStore(const std::vector<std::uint8_t>& data)
{
    return data.size() >= 64 && std::memcmp(data.data() + 40, "_FVH", 4) == 0;
}

} // namespace

void ExportMachine(const std::filesystem::path& diskPath, const std::string& config, const std::filesystem::path& varsPath,
                   const std::filesystem::path& target, const ArchiveProgress& progress)
{
    if (config.size() > kMaxConfig) throw std::invalid_argument("Configuration trop grande.");
    std::vector<std::uint8_t> vars;
    if (!varsPath.empty()) {
        vars = ReadVarsStore(varsPath);
        if (!LooksLikeVarsStore(vars)) throw std::runtime_error("vars.fd n'est pas un magasin de variables UEFI.");
    }
    const auto disk = VirtualDisk::Open(diskPath, true); // Read-only: shareable with a running VM's layers.
    const auto blocks = disk.DataBlocks();

    auto temporary = target;
    temporary += L".tmp";
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.Get() == INVALID_HANDLE_VALUE) ThrowLastError("CreateFileW(.vrtx)");
    COMPRESSOR_HANDLE compressor = nullptr;
    try {
        if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS, nullptr, &compressor)) ThrowLastError("CreateCompressor");
        Output out(file.Get());
        FileHeader header{};
        std::memcpy(header.magic, kMagic, sizeof(kMagic));
        header.version = kVersion;
        header.headerSize = sizeof(FileHeader);
        header.blockSize = kBlockSize;
        header.virtualSize = disk.SizeBytes();
        header.configSize = static_cast<std::uint32_t>(config.size());
        header.headerCrc = HeaderCrc(header);
        out.Write(&header, sizeof(header));
        out.Write(config.data(), config.size());
        const auto configCrc = Crc32(config.data(), config.size());
        out.Write(&configCrc, sizeof(configCrc));
        // Firmware section: size, CRC32, bytes (size 0: no store).
        const auto varsSize = static_cast<std::uint32_t>(vars.size());
        const auto varsCrc = Crc32(vars.data(), vars.size());
        out.Write(&varsSize, sizeof(varsSize));
        out.Write(&varsCrc, sizeof(varsCrc));
        out.Write(vars.data(), vars.size());

        std::vector<std::byte> data(kBlockSize);
        std::vector<std::uint8_t> packed(kBlockSize);
        std::uint64_t written = 0;
        std::uint64_t done = 0;
        for (const auto index : blocks) {
            const auto length = BlockLength(index, header.virtualSize);
            const auto part = std::span(data).first(length);
            disk.ReadSectors(index * kBlockSize / VirtualDisk::SectorSize, part);
            ++done;
            if (std::all_of(part.begin(), part.end(), [](std::byte value) { return value == std::byte{0}; })) {
                if (progress && done % 64 == 0 && !progress(done, blocks.size())) throw ArchiveCancelled();
                continue; // Holes stay holes: not stored at all.
            }
            BlockHeader record{};
            record.index = index;
            record.realSize = length;
            record.crc = Crc32(part.data(), length);
            // Compressed only if strictly smaller; an incompressible block
            // (already compressed data) does not fit the buffer: stored as is.
            SIZE_T compressed = 0;
            const BOOL ok = Compress(compressor, part.data(), length, packed.data(), length - 1, &compressed);
            if (ok && compressed != 0 && compressed < length) {
                record.flags = kFlagCompressed;
                record.storedSize = static_cast<std::uint32_t>(compressed);
                out.Write(&record, sizeof(record));
                out.Write(packed.data(), compressed);
            } else {
                if (!ok && GetLastError() != ERROR_INSUFFICIENT_BUFFER) ThrowLastError("Compress");
                record.flags = 0;
                record.storedSize = length;
                out.Write(&record, sizeof(record));
                out.Write(part.data(), length);
            }
            ++written;
            if (progress && done % 16 == 0 && !progress(done, blocks.size())) throw ArchiveCancelled();
        }
        // End record: how many blocks precede it.
        BlockHeader end{};
        end.index = kEndIndex;
        end.storedSize = end.realSize = sizeof(written);
        end.crc = Crc32(&written, sizeof(written));
        out.Write(&end, sizeof(end));
        out.Write(&written, sizeof(written));
        out.Flush();
        if (!FlushFileBuffers(file.Get())) ThrowLastError("FlushFileBuffers(.vrtx)");
        file.Close();
        CloseCompressor(compressor);
        compressor = nullptr;
        if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            ThrowLastError("MoveFileExW(.vrtx)");
        }
        if (progress) (void)progress(blocks.size(), blocks.size());
    } catch (...) {
        if (compressor != nullptr) CloseCompressor(compressor);
        file.Close();
        (void)DeleteFileW(temporary.c_str());
        throw;
    }
}

namespace {

FileHeader ReadHeader(Input& in)
{
    FileHeader header{};
    in.Read(&header, sizeof(header));
    if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) Corrupt("ce n'est pas une machine exportée");
    if (header.version != 1 && header.version != kVersion) Corrupt("version de format inconnue");
    if (header.headerSize != sizeof(FileHeader) || header.blockSize != kBlockSize) Corrupt("en-tête incohérent");
    if (HeaderCrc(header) != header.headerCrc) Corrupt("en-tête altéré (CRC)");
    if (header.virtualSize == 0 || header.virtualSize % VirtualDisk::SectorSize != 0 || header.virtualSize > kMaxVirtualSize) {
        Corrupt("taille de disque impossible");
    }
    if (header.configSize > kMaxConfig) Corrupt("configuration trop grande");
    return header;
}

std::string ReadConfig(Input& in, const FileHeader& header)
{
    std::string config(header.configSize, '\0'); // Bounded by kMaxConfig above.
    in.Read(config.data(), config.size());
    std::uint32_t crc = 0;
    in.Read(&crc, sizeof(crc));
    if (Crc32(config.data(), config.size()) != crc) Corrupt("configuration altérée (CRC)");
    return config;
}

// Version 2: the UEFI variable store, checked (bound, CRC32, firmware
// volume) before anything is written. Version 1: none.
std::vector<std::uint8_t> ReadVars(Input& in, const FileHeader& header)
{
    if (header.version < 2) return {};
    std::uint32_t size = 0;
    std::uint32_t crc = 0;
    in.Read(&size, sizeof(size));
    in.Read(&crc, sizeof(crc));
    if (size > kMaxVars) Corrupt("magasin de variables UEFI trop grand");
    std::vector<std::uint8_t> vars(size);
    in.Read(vars.data(), vars.size());
    if (Crc32(vars.data(), vars.size()) != crc) Corrupt("magasin de variables UEFI altéré (CRC)");
    if (size != 0 && !LooksLikeVarsStore(vars)) Corrupt("magasin de variables UEFI invalide");
    return vars;
}

} // namespace

ArchiveInfo ReadArchiveInfo(const std::filesystem::path& archive)
{
    Handle file(CreateFileW(archive.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.Get() == INVALID_HANDLE_VALUE) ThrowLastError("CreateFileW(.vrtx)");
    auto in = std::make_unique<Input>(file.Get());
    const auto header = ReadHeader(*in);
    ArchiveInfo info;
    info.version = header.version;
    info.virtualSize = header.virtualSize;
    info.config = ReadConfig(*in, header);
    info.varsSize = static_cast<std::uint32_t>(ReadVars(*in, header).size());
    return info;
}

bool ImportDisk(const std::filesystem::path& archive, const std::filesystem::path& diskPath, const std::filesystem::path& varsPath,
                const ArchiveProgress& progress)
{
    Handle source(CreateFileW(archive.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (source.Get() == INVALID_HANDLE_VALUE) ThrowLastError("CreateFileW(.vrtx)");
    LARGE_INTEGER archiveSize{};
    if (!GetFileSizeEx(source.Get(), &archiveSize)) ThrowLastError("GetFileSizeEx(.vrtx)");
    auto in = std::make_unique<Input>(source.Get());
    const auto header = ReadHeader(*in);
    (void)ReadConfig(*in, header);
    const auto vars = ReadVars(*in, header); // Written once the whole disk checked out.
    const bool writeVars = !vars.empty() && !varsPath.empty();
    bool varsCreated = false;

    Handle disk(CreateFileW(diskPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (disk.Get() == INVALID_HANDLE_VALUE) ThrowLastError("CreateFileW(disque importé)");
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    try {
        // Sparse before the size is set: only the blocks written use space.
        DWORD returned = 0;
        if (!DeviceIoControl(disk.Get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr)) ThrowLastError("FSCTL_SET_SPARSE");
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(header.virtualSize);
        if (!SetFilePointerEx(disk.Get(), end, nullptr, FILE_BEGIN) || !SetEndOfFile(disk.Get())) ThrowLastError("SetEndOfFile");
        if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS, nullptr, &decompressor)) ThrowLastError("CreateDecompressor");

        const auto blockCount = (header.virtualSize + kBlockSize - 1) / kBlockSize;
        std::vector<bool> seen(static_cast<std::size_t>(blockCount), false);
        std::vector<std::uint8_t> stored(kBlockSize); // Fixed size: stored sizes are checked against it first.
        std::vector<std::uint8_t> data(kBlockSize);
        std::uint64_t count = 0;
        for (;;) {
            BlockHeader record{};
            in->Read(&record, sizeof(record));
            if (record.index == kEndIndex) {
                std::uint64_t expected = 0;
                if (record.storedSize != sizeof(expected) || record.realSize != sizeof(expected) || record.flags != 0) {
                    Corrupt("fin de fichier incohérente");
                }
                in->Read(&expected, sizeof(expected));
                if (Crc32(&expected, sizeof(expected)) != record.crc || expected != count) Corrupt("nombre de blocs incohérent");
                if (in->Consumed() != static_cast<std::uint64_t>(archiveSize.QuadPart)) Corrupt("données après la fin");
                break;
            }
            // Bounds before anything is read or allocated.
            if (record.index >= blockCount) Corrupt("bloc hors du disque");
            if (seen[static_cast<std::size_t>(record.index)]) Corrupt("bloc en double");
            if (record.realSize != BlockLength(record.index, header.virtualSize)) Corrupt("taille de bloc incohérente");
            if (record.storedSize == 0 || record.storedSize > kBlockSize) Corrupt("taille stockée impossible");
            if ((record.flags & ~kFlagCompressed) != 0) Corrupt("drapeaux de bloc inconnus");
            if (record.flags == 0 && record.storedSize != record.realSize) Corrupt("bloc brut de taille incohérente");
            in->Read(stored.data(), record.storedSize);
            const std::uint8_t* real = stored.data();
            if (record.flags == kFlagCompressed) {
                SIZE_T produced = 0;
                if (!Decompress(decompressor, stored.data(), record.storedSize, data.data(), record.realSize, &produced) ||
                    produced != record.realSize) {
                    Corrupt("bloc compressé illisible");
                }
                real = data.data();
            }
            if (Crc32(real, record.realSize) != record.crc) Corrupt("bloc altéré (CRC)");
            WriteAt(disk.Get(), record.index * kBlockSize, real, record.realSize);
            seen[static_cast<std::size_t>(record.index)] = true;
            ++count;
            if (progress && count % 16 == 0 &&
                !progress(in->Consumed(), static_cast<std::uint64_t>(archiveSize.QuadPart))) {
                throw ArchiveCancelled();
            }
        }
        if (!FlushFileBuffers(disk.Get())) ThrowLastError("FlushFileBuffers(disque importé)");
        CloseDecompressor(decompressor);
        decompressor = nullptr;
        disk.Close();
        if (writeVars) {
            Handle store(CreateFileW(varsPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (store.Get() == INVALID_HANDLE_VALUE) ThrowLastError("CreateFileW(vars.fd importé)");
            varsCreated = true; // From here on, ours to delete on failure.
            DWORD written = 0;
            if (!WriteFile(store.Get(), vars.data(), static_cast<DWORD>(vars.size()), &written, nullptr) || written != vars.size() ||
                !FlushFileBuffers(store.Get())) {
                ThrowLastError("WriteFile(vars.fd importé)");
            }
        }
        if (progress) (void)progress(static_cast<std::uint64_t>(archiveSize.QuadPart), static_cast<std::uint64_t>(archiveSize.QuadPart));
        return writeVars;
    } catch (...) {
        if (decompressor != nullptr) CloseDecompressor(decompressor);
        disk.Close();
        (void)DeleteFileW(diskPath.c_str());
        if (varsCreated) (void)DeleteFileW(varsPath.c_str());
        throw;
    }
}

} // namespace virtualiseur::whp
