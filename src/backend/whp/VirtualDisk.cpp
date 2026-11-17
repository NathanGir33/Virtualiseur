#include "VirtualDisk.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace virtualiseur::whp {
namespace {

[[noreturn]] void ThrowLastError(const char* operation)
{
    throw std::system_error(
        static_cast<int>(GetLastError()), std::system_category(), operation);
}

void ValidateDiskSize(std::uint64_t sizeBytes)
{
    if (sizeBytes < VirtualDisk::SectorSize ||
        (sizeBytes % VirtualDisk::SectorSize) != 0 ||
        sizeBytes > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
        throw std::invalid_argument("La taille du disque doit etre alignee sur 512 octets et tenir dans un fichier Windows.");
    }
}

} // namespace

VirtualDisk::VirtualDisk(
    HANDLE file,
    std::filesystem::path path,
    std::uint64_t sizeBytes,
    bool readOnly) noexcept
    : file_(file), path_(std::move(path)), sizeBytes_(sizeBytes), readOnly_(readOnly)
{
}

VirtualDisk VirtualDisk::CreateSparse(
    const std::filesystem::path& path,
    std::uint64_t sizeBytes)
{
    ValidateDiskSize(sizeBytes);
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error) {
            throw std::system_error(error, "Creation du dossier du disque virtuel");
        }
    }

    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ThrowLastError("CreateFileW(CREATE_NEW)");
    }

    try {
        DWORD bytesReturned = 0;
        if (!DeviceIoControl(
                file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytesReturned, nullptr)) {
            ThrowLastError("FSCTL_SET_SPARSE");
        }
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(sizeBytes);
        if (!SetFilePointerEx(file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
            ThrowLastError("SetEndOfFile");
        }
        if (!FlushFileBuffers(file)) {
            ThrowLastError("FlushFileBuffers");
        }
        return VirtualDisk(file, path, sizeBytes, false);
    } catch (...) {
        CloseHandle(file);
        (void)DeleteFileW(path.c_str());
        throw;
    }
}

VirtualDisk VirtualDisk::Open(const std::filesystem::path& path, bool readOnly)
{
    if (LayerFile::IsLayer(path)) {
        // Walk the chain down to the raw base; only the top layer is writable.
        std::vector<LayerFile> layers;
        auto current = path;
        while (LayerFile::IsLayer(current)) {
            if (layers.size() >= 4096) throw std::runtime_error("Chaîne de couches de disque trop longue (boucle ?).");
            layers.push_back(LayerFile::Open(current, layers.empty() && !readOnly));
            if (layers.back().VirtualSize() != layers.front().VirtualSize()) {
                throw std::runtime_error("Couches de disque de tailles différentes : " + current.string());
            }
            current = current.parent_path() / layers.back().ParentName();
        }
        HANDLE base = CreateFileW(current.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (base == INVALID_HANDLE_VALUE) ThrowLastError("CreateFileW(base du disque)");
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(base, &size) || static_cast<std::uint64_t>(size.QuadPart) < layers.front().VirtualSize()) {
            CloseHandle(base);
            throw std::runtime_error("Image de base plus petite que ses couches : " + current.string());
        }
        VirtualDisk disk(base, path, layers.front().VirtualSize(), readOnly);
        disk.layers_ = std::move(layers);
        return disk;
    }

    HANDLE file = CreateFileW(
        path.c_str(),
        readOnly ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ThrowLastError("CreateFileW(OPEN_EXISTING)");
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        const auto error = GetLastError();
        CloseHandle(file);
        SetLastError(error);
        ThrowLastError("GetFileSizeEx");
    }
    try {
        ValidateDiskSize(static_cast<std::uint64_t>(size.QuadPart));
    } catch (...) {
        CloseHandle(file);
        throw;
    }
    return VirtualDisk(file, path, static_cast<std::uint64_t>(size.QuadPart), readOnly);
}

VirtualDisk::~VirtualDisk()
{
    Close();
}

VirtualDisk::VirtualDisk(VirtualDisk&& other) noexcept
    : file_(std::exchange(other.file_, INVALID_HANDLE_VALUE)),
      path_(std::move(other.path_)),
      sizeBytes_(std::exchange(other.sizeBytes_, 0)),
      readOnly_(std::exchange(other.readOnly_, false)),
      layers_(std::move(other.layers_))
{
}

VirtualDisk& VirtualDisk::operator=(VirtualDisk&& other) noexcept
{
    if (this != &other) {
        Close();
        file_ = std::exchange(other.file_, INVALID_HANDLE_VALUE);
        path_ = std::move(other.path_);
        sizeBytes_ = std::exchange(other.sizeBytes_, 0);
        readOnly_ = std::exchange(other.readOnly_, false);
        layers_ = std::move(other.layers_);
    }
    return *this;
}

std::uint64_t VirtualDisk::SizeBytes() const noexcept { return sizeBytes_; }
std::uint64_t VirtualDisk::SectorCount() const noexcept { return sizeBytes_ / SectorSize; }
const std::filesystem::path& VirtualDisk::Path() const noexcept { return path_; }
bool VirtualDisk::IsReadOnly() const noexcept { return readOnly_; }

void VirtualDisk::ReadSectors(std::uint64_t lba, std::span<std::byte> destination) const
{
    if (destination.size() % SectorSize != 0) {
        throw std::invalid_argument("Une lecture disque doit contenir un nombre entier de secteurs.");
    }
    const auto sectors = destination.size() / SectorSize;
    if (lba > SectorCount() || sectors > SectorCount() - lba) {
        throw std::out_of_range("Lecture au-dela de la capacite du disque virtuel.");
    }
    if (destination.empty()) return;
    const std::shared_lock switchLock(switchMutex_);

    if (!layers_.empty()) {
        // Block by block, each from the topmost layer that has it.
        const std::lock_guard lock(ioMutex_);
        std::uint64_t offset = lba * SectorSize;
        std::size_t done = 0;
        while (done < destination.size()) {
            const auto block = offset / LayerFile::kBlockSize;
            const auto inner = static_cast<std::uint32_t>(offset % LayerFile::kBlockSize);
            const auto count = (std::min<std::size_t>)(destination.size() - done, LayerFile::kBlockSize - inner);
            ReadFromChain(0, block, inner, destination.subspan(done, count));
            done += count;
            offset += count;
        }
        return;
    }

    // Positional I/O through OVERLAPPED offsets: concurrent requests from
    // several device threads do not share a file pointer.
    std::size_t completed = 0;
    while (completed < destination.size()) {
        const auto chunk = static_cast<DWORD>((std::min)(
            destination.size() - completed,
            static_cast<std::size_t>(64u * 1024 * 1024)));
        const auto offset = lba * SectorSize + completed;
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD transferred = 0;
        if (!ReadFile(file_, destination.data() + completed, chunk, &transferred, &position)) {
            ThrowLastError("ReadFile");
        }
        if (transferred == 0) {
            throw std::runtime_error("Fin de fichier inattendue lors de la lecture du disque virtuel.");
        }
        completed += transferred;
    }
}

void VirtualDisk::WriteSectors(std::uint64_t lba, std::span<const std::byte> source)
{
    if (readOnly_) {
        throw std::logic_error("Le disque virtuel est ouvert en lecture seule.");
    }
    if (source.size() % SectorSize != 0) {
        throw std::invalid_argument("Une ecriture disque doit contenir un nombre entier de secteurs.");
    }
    const auto sectors = source.size() / SectorSize;
    if (lba > SectorCount() || sectors > SectorCount() - lba) {
        throw std::out_of_range("Ecriture au-dela de la capacite du disque virtuel.");
    }
    if (source.empty()) return;
    const std::shared_lock switchLock(switchMutex_);

    if (!layers_.empty()) {
        const std::lock_guard lock(ioMutex_);
        auto& top = layers_.front();
        std::vector<std::byte> whole;
        std::uint64_t offset = lba * SectorSize;
        std::size_t done = 0;
        while (done < source.size()) {
            const auto block = offset / LayerFile::kBlockSize;
            const auto inner = static_cast<std::uint32_t>(offset % LayerFile::kBlockSize);
            const auto count = (std::min<std::size_t>)(source.size() - done, LayerFile::kBlockSize - inner);
            const auto part = source.subspan(done, count);
            if (top.Has(block)) {
                top.Write(block, inner, part);
            } else if (count == LayerFile::kBlockSize) {
                top.Append(block, part);
            } else {
                // First partial write to this block: read the whole 64 KiB from
                // the layers below, apply the change in memory, write it all.
                whole.resize(LayerFile::kBlockSize);
                ReadFromChain(1, block, 0, whole);
                std::memcpy(whole.data() + inner, part.data(), count);
                top.Append(block, whole);
            }
            done += count;
            offset += count;
        }
        return;
    }

    std::size_t completed = 0;
    while (completed < source.size()) {
        const auto chunk = static_cast<DWORD>((std::min)(
            source.size() - completed,
            static_cast<std::size_t>(64u * 1024 * 1024)));
        const auto offset = lba * SectorSize + completed;
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD transferred = 0;
        if (!WriteFile(file_, source.data() + completed, chunk, &transferred, &position)) {
            ThrowLastError("WriteFile");
        }
        if (transferred == 0) {
            throw std::runtime_error("Ecriture nulle inattendue dans le disque virtuel.");
        }
        completed += transferred;
    }
}

void VirtualDisk::Flush()
{
    if (readOnly_) return;
    const std::shared_lock switchLock(switchMutex_);
    if (!layers_.empty()) {
        const std::lock_guard lock(ioMutex_);
        layers_.front().Flush();
        return;
    }
    if (!FlushFileBuffers(file_)) {
        ThrowLastError("FlushFileBuffers");
    }
}

void VirtualDisk::PushLayer(const std::filesystem::path& newTop)
{
    if (readOnly_) throw std::logic_error("Disque en lecture seule : pas d'instantané.");
    // Waits for in-flight requests; the guest's next ones wait for the swap.
    const std::unique_lock switchLock(switchMutex_);
    const std::lock_guard ioLock(ioMutex_);

    // Open the new top first: if it fails, nothing has changed.
    auto top = LayerFile::Open(newTop, true);
    if (top.VirtualSize() != sizeBytes_) throw std::runtime_error("Nouvelle couche de taille différente.");
    const auto current = layers_.empty() ? path_ : layers_.front().Path();
    if (_wcsicmp(top.ParentName().c_str(), current.filename().c_str()) != 0) {
        throw std::runtime_error("La nouvelle couche ne repose pas sur la couche actuelle.");
    }

    // The current top becomes the snapshot: everything written so far must
    // be on disk before it is closed and reopened read-only.
    if (layers_.empty()) {
        if (!FlushFileBuffers(file_)) ThrowLastError("FlushFileBuffers");
        CloseHandle(file_);
        file_ = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) ThrowLastError("CreateFileW(base en lecture seule)");
    } else {
        layers_.front().Flush();
        const auto path = layers_.front().Path();
        layers_.front() = LayerFile{};
        layers_.front() = LayerFile::Open(path, false);
    }
    layers_.insert(layers_.begin(), std::move(top));
    path_ = newTop;
}

std::vector<std::uint64_t> VirtualDisk::DataBlocks() const
{
    const auto blockCount = (sizeBytes_ + LayerFile::kBlockSize - 1) / LayerFile::kBlockSize;
    std::vector<bool> present(static_cast<std::size_t>(blockCount), false);
    for (const auto& layer : layers_) {
        for (const auto block : layer.AllocatedBlocks()) present[static_cast<std::size_t>(block)] = true;
    }
    // Allocated ranges of the raw base (a non-sparse file: one range, all of it).
    FILE_ALLOCATED_RANGE_BUFFER query{};
    query.FileOffset.QuadPart = 0;
    query.Length.QuadPart = static_cast<LONGLONG>(sizeBytes_);
    std::vector<FILE_ALLOCATED_RANGE_BUFFER> ranges(256);
    for (;;) {
        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(file_, FSCTL_QUERY_ALLOCATED_RANGES, &query, sizeof(query), ranges.data(),
                                        static_cast<DWORD>(ranges.size() * sizeof(ranges[0])), &returned, nullptr);
        const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error != ERROR_MORE_DATA) {
            std::fill(present.begin(), present.end(), true); // Unknown layout: read everything.
            break;
        }
        const auto count = returned / sizeof(ranges[0]);
        for (std::size_t index = 0; index < count; ++index) {
            const auto start = static_cast<std::uint64_t>(ranges[index].FileOffset.QuadPart);
            const auto end = (std::min)(sizeBytes_, start + static_cast<std::uint64_t>(ranges[index].Length.QuadPart));
            for (auto block = start / LayerFile::kBlockSize; block * LayerFile::kBlockSize < end; ++block) {
                present[static_cast<std::size_t>(block)] = true;
            }
        }
        if (ok || count == 0) break;
        // ERROR_MORE_DATA: continue after the last range returned.
        const auto& last = ranges[count - 1];
        const auto next = last.FileOffset.QuadPart + last.Length.QuadPart;
        query.Length.QuadPart -= next - query.FileOffset.QuadPart;
        query.FileOffset.QuadPart = next;
    }
    std::vector<std::uint64_t> blocks;
    for (std::size_t index = 0; index < present.size(); ++index) {
        if (present[index]) blocks.push_back(index);
    }
    return blocks;
}

void VirtualDisk::Flatten(const std::filesystem::path& source, const std::filesystem::path& target,
                          const std::function<bool(std::uint64_t, std::uint64_t)>& progress)
{
    // Read-only: the parent layers stay shareable with a running VM.
    const auto disk = Open(source, true);
    const HANDLE file = CreateFileW(target.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) ThrowLastError("CreateFileW(clone)");
    try {
        // Sparse before the size is set: the unwritten blocks never use space.
        DWORD returned = 0;
        if (!DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr)) {
            ThrowLastError("FSCTL_SET_SPARSE");
        }
        LARGE_INTEGER end{};
        end.QuadPart = static_cast<LONGLONG>(disk.SizeBytes());
        if (!SetFilePointerEx(file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) ThrowLastError("SetEndOfFile");

        const auto blocks = disk.DataBlocks();
        std::vector<std::byte> data(LayerFile::kBlockSize);
        std::uint64_t done = 0;
        for (const auto block : blocks) {
            const auto offset = block * LayerFile::kBlockSize;
            const auto length = static_cast<std::size_t>((std::min<std::uint64_t>)(LayerFile::kBlockSize, disk.SizeBytes() - offset));
            const auto part = std::span(data).first(length);
            disk.ReadSectors(offset / SectorSize, part);
            const bool zero = std::all_of(part.begin(), part.end(), [](std::byte value) { return value == std::byte{0}; });
            if (!zero) WriteAt(file, offset, part.data(), part.size());
            if (++done % 64 == 0 && progress && !progress(done, blocks.size())) {
                throw std::runtime_error("Clonage annulé.");
            }
        }
        if (!FlushFileBuffers(file)) ThrowLastError("FlushFileBuffers");
        if (progress) (void)progress(blocks.size(), blocks.size());
        CloseHandle(file);
    } catch (...) {
        CloseHandle(file);
        (void)DeleteFileW(target.c_str());
        throw;
    }
}

void VirtualDisk::ReadFromChain(std::size_t fromLayer, std::uint64_t block, std::uint32_t offset,
                                std::span<std::byte> destination) const
{
    for (auto index = fromLayer; index < layers_.size(); ++index) {
        if (layers_[index].Has(block)) {
            layers_[index].Read(block, offset, destination);
            return;
        }
    }
    // The raw base has every block (holes of the sparse file read as zeros).
    ReadAt(file_, block * LayerFile::kBlockSize + offset, destination.data(), destination.size());
}

void VirtualDisk::Close() noexcept
{
    layers_.clear();
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
    sizeBytes_ = 0;
}

} // namespace virtualiseur::whp
