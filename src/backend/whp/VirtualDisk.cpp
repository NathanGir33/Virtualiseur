#include "VirtualDisk.h"

#include <algorithm>
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
      readOnly_(std::exchange(other.readOnly_, false))
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
    if (!FlushFileBuffers(file_)) {
        ThrowLastError("FlushFileBuffers");
    }
}

void VirtualDisk::Close() noexcept
{
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
    sizeBytes_ = 0;
}

} // namespace virtualiseur::whp
