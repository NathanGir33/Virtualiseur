#include "NinePServer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace vmm {
namespace {

// Message types (9P2000.L), T-message numbers; the reply is T + 1.
enum : std::uint8_t {
    kTlerror = 6,
    kTstatfs = 8,
    kTlopen = 12,
    kTlcreate = 14,
    kTgetattr = 24,
    kTsetattr = 26,
    kTreaddir = 40,
    kTmkdir = 72,
    kTrenameat = 74,
    kTunlinkat = 76,
    kTversion = 100,
    kTattach = 104,
    kTflush = 108,
    kTwalk = 110,
    kTread = 116,
    kTwrite = 118,
    kTclunk = 120,
};

// Linux errno values carried by Rlerror.
enum : std::uint32_t {
    kENOENT = 2,
    kEIO = 5,
    kEBADF = 9,
    kEACCES = 13,
    kEEXIST = 17,
    kEXDEV = 18,
    kENOTDIR = 20,
    kEISDIR = 21,
    kEINVAL = 22,
    kENOSPC = 28,
    kEROFS = 30,
    kENAMETOOLONG = 36,
    kENOTEMPTY = 39,
    kEPROTO = 71,
    kEOPNOTSUPP = 95,
};

// Open flags (P9_DOTL_*, the Linux values).
constexpr std::uint32_t kAccessMode = 3;
constexpr std::uint32_t kReadOnly = 0;
constexpr std::uint32_t kWriteOnly = 1;
constexpr std::uint32_t kExclusive = 0200;
constexpr std::uint32_t kTruncate = 01000;
constexpr std::uint32_t kAppend = 02000;

// Tsetattr valid bits.
constexpr std::uint32_t kSetMode = 0x1;
constexpr std::uint32_t kSetSize = 0x8;
constexpr std::uint32_t kSetAtime = 0x10;
constexpr std::uint32_t kSetMtime = 0x20;
constexpr std::uint32_t kSetAtimeValue = 0x80;
constexpr std::uint32_t kSetMtimeValue = 0x100;

constexpr std::uint32_t kRemoveDirectory = 0x200; // AT_REMOVEDIR.
constexpr std::uint8_t kQidDirectory = 0x80;
constexpr std::uint8_t kDirentDirectory = 4; // DT_DIR.
constexpr std::uint8_t kDirentRegular = 8;   // DT_REG.
constexpr std::uint64_t kGetattrAll = 0x3FFF; // BASIC (0x7FF) | BTIME | GEN | DATA_VERSION.
constexpr std::uint32_t kV9fsMagic = 0x01021997;
constexpr std::uint32_t kHeaderSize = 7;      // size[4] type[1] tag[2].
constexpr std::int64_t kUnixEpoch = 116444736000000000; // 1970-01-01 in FILETIME units.

struct Errno {
    std::uint32_t code;
};

std::uint32_t FromWin32(DWORD error)
{
    switch (error) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_DRIVE: case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        return kENOENT;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_CANT_ACCESS_FILE:
        return kEACCES;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return kEEXIST;
    case ERROR_DIR_NOT_EMPTY: return kENOTEMPTY;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return kENOSPC;
    case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME: case ERROR_INVALID_PARAMETER: return kEINVAL;
    case ERROR_NOT_SAME_DEVICE: return kEXDEV;
    case ERROR_DIRECTORY: return kENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE: return kENAMETOOLONG;
    case ERROR_WRITE_PROTECT: return kEROFS;
    case ERROR_NOT_SUPPORTED: case ERROR_INVALID_FUNCTION: return kEOPNOTSUPP;
    default: return kEIO;
    }
}

[[noreturn]] void ThrowLastError()
{
    throw Errno{FromWin32(GetLastError())};
}

class ScopedHandle final {
public:
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ScopedHandle() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE Get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

std::wstring Widen(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) throw Errno{kEINVAL};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string Narrow(const wchar_t* text, std::size_t count)
{
    if (count == 0) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(count), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(count), result.data(), length, nullptr, nullptr);
    return result;
}

bool EqualNoCase(const wchar_t* a, std::size_t aLength, const wchar_t* b, std::size_t bLength)
{
    return CompareStringOrdinal(a, static_cast<int>(aLength), b, static_cast<int>(bLength), TRUE) == CSTR_EQUAL;
}

// `path` is `root` or below it (case-insensitive, on a separator boundary).
bool UnderRoot(const std::wstring& path, const std::wstring& root)
{
    if (path.size() < root.size() || !EqualNoCase(path.data(), root.size(), root.data(), root.size())) return false;
    return path.size() == root.size() || path[root.size()] == L'\\';
}

// A single Linux file name usable as a Windows file name.
std::wstring ValidName(const std::string& name)
{
    if (name.empty() || name == "." || name == "..") throw Errno{kEINVAL};
    if (name.size() > 255) throw Errno{kENAMETOOLONG};
    auto wide = Widen(name);
    for (const wchar_t c : wide) {
        if (c < 0x20 || wcschr(L"\\/:*?\"<>|", c) != nullptr) throw Errno{kEINVAL};
    }
    if (wide.back() == L'.' || wide.back() == L' ') throw Errno{kEINVAL}; // Windows would drop them.
    // Device names (CON, NUL, COM1...) whatever the extension.
    const auto stem = wide.substr(0, wide.find(L'.'));
    static constexpr std::array<const wchar_t*, 22> kDevices{
        L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7",
        L"COM8", L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    for (const auto* device : kDevices) {
        if (EqualNoCase(stem.data(), stem.size(), device, wcslen(device))) throw Errno{kEINVAL};
    }
    return wide;
}

std::uint64_t ToUnixNs(const FILETIME& time)
{
    const auto ticks = (static_cast<std::int64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    const auto since = ticks - kUnixEpoch;
    return since <= 0 ? 0 : static_cast<std::uint64_t>(since) * 100;
}

FILETIME FromUnix(std::uint64_t seconds, std::uint64_t nanoseconds)
{
    const auto ticks = kUnixEpoch + static_cast<std::int64_t>(seconds) * 10'000'000 + static_cast<std::int64_t>(nanoseconds / 100);
    return FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
}

FILETIME Now()
{
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    return now;
}

} // namespace

// --- Message encoding -----------------------------------------------------------------------

class NinePServer::Reader final {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}
    std::uint8_t U8() { return Take<std::uint8_t>(); }
    std::uint16_t U16() { return Take<std::uint16_t>(); }
    std::uint32_t U32() { return Take<std::uint32_t>(); }
    std::uint64_t U64() { return Take<std::uint64_t>(); }
    std::string String()
    {
        const auto length = U16();
        const auto bytes = Bytes(length);
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    std::span<const std::uint8_t> Bytes(std::size_t count)
    {
        if (count > data_.size() - position_) throw Errno{kEPROTO};
        const auto bytes = data_.subspan(position_, count);
        position_ += count;
        return bytes;
    }

private:
    template <typename T>
    T Take()
    {
        T value{};
        std::memcpy(&value, Bytes(sizeof(T)).data(), sizeof(T));
        return value;
    }
    std::span<const std::uint8_t> data_;
    std::size_t position_ = 0;
};

class NinePServer::Writer final {
public:
    explicit Writer(std::vector<std::uint8_t>& buffer) : buffer_(buffer) {}
    void U8(std::uint8_t value) { Put(value); }
    void U16(std::uint16_t value) { Put(value); }
    void U32(std::uint32_t value) { Put(value); }
    void U64(std::uint64_t value) { Put(value); }
    void String(const std::string& text)
    {
        U16(static_cast<std::uint16_t>(text.size()));
        buffer_.insert(buffer_.end(), text.begin(), text.end());
    }
    void Qid(std::uint8_t type, std::uint64_t path)
    {
        U8(type);
        U32(0); // version: no cache coherency hints.
        U64(path);
    }
    void Qid(const Info& info) { Qid(info.Directory() ? kQidDirectory : 0, info.id); }
    // Room for `count` bytes written in place; returns where they start.
    std::uint8_t* Reserve(std::size_t count)
    {
        const auto offset = buffer_.size();
        buffer_.resize(offset + count);
        return buffer_.data() + offset;
    }
    void Shrink(std::size_t unused) { buffer_.resize(buffer_.size() - unused); }
    std::size_t Size() const noexcept { return buffer_.size(); }
    void Patch32(std::size_t offset, std::uint32_t value) { std::memcpy(buffer_.data() + offset, &value, 4); }

private:
    template <typename T>
    void Put(T value)
    {
        const auto offset = buffer_.size();
        buffer_.resize(offset + sizeof(T));
        std::memcpy(buffer_.data() + offset, &value, sizeof(T));
    }
    std::vector<std::uint8_t>& buffer_;
};

// --- Server ----------------------------------------------------------------------------------

NinePServer::NinePServer(const std::filesystem::path& root, bool readOnly) : readOnly_(readOnly)
{
    // The root as Windows finally resolves it: the reference for every check.
    const HANDLE handle = CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("dossier introuvable");
    ScopedHandle scoped(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        throw std::runtime_error("ce n'est pas un dossier");
    }
    std::wstring final(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(handle, final.data(), static_cast<DWORD>(final.size()),
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length == 0 || length >= final.size()) throw std::runtime_error("chemin du dossier illisible");
    final.resize(length);
    while (final.size() > 7 && final.back() == L'\\') final.pop_back(); // "\\?\C:\" -> "\\?\C:".
    rootFinal_ = final;
    if (rootFinal_.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        unc_ = true;
        rootPlain_ = L"\\\\" + rootFinal_.substr(8);
    } else if (rootFinal_.rfind(L"\\\\?\\", 0) == 0) {
        rootPlain_ = rootFinal_.substr(4);
    } else {
        throw std::runtime_error("chemin du dossier inattendu");
    }
}

NinePServer::~NinePServer()
{
    for (auto& [id, fid] : fids_) {
        if (fid.file != INVALID_HANDLE_VALUE) CloseHandle(fid.file);
    }
}

std::wstring NinePServer::HostPath(const std::vector<std::wstring>& components) const
{
    std::wstring plain = rootPlain_;
    for (const auto& component : components) {
        plain += L'\\';
        plain += component;
    }
    // Canonical form ("." and ".." resolved, separators normalized), checked
    // against the root before anything is opened.
    const DWORD needed = GetFullPathNameW(plain.c_str(), 0, nullptr, nullptr);
    if (needed == 0) ThrowLastError();
    std::wstring canonical(needed, L'\0');
    const DWORD length = GetFullPathNameW(plain.c_str(), needed, canonical.data(), nullptr);
    if (length == 0 || length >= needed) ThrowLastError();
    canonical.resize(length);
    if (!UnderRoot(canonical, rootPlain_)) throw Errno{kEACCES};
    return unc_ ? L"\\\\?\\UNC\\" + canonical.substr(2) : L"\\\\?\\" + canonical;
}

bool NinePServer::Inside(HANDLE handle) const
{
    std::wstring final(1024, L'\0');
    DWORD length = GetFinalPathNameByHandleW(handle, final.data(), static_cast<DWORD>(final.size()),
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length >= final.size()) {
        final.resize(length + 1);
        length = GetFinalPathNameByHandleW(handle, final.data(), static_cast<DWORD>(final.size()),
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    if (length == 0 || length >= final.size()) return false;
    final.resize(length);
    return UnderRoot(final, rootFinal_);
}

HANDLE NinePServer::Open(const std::wstring& path, DWORD access, DWORD disposition, DWORD flags) const
{
    const HANDLE handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, disposition, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS | flags,
                                      nullptr);
    if (handle == INVALID_HANDLE_VALUE) ThrowLastError();
    // A junction or symbolic link inside the share may lead outside of it.
    if (!Inside(handle)) {
        CloseHandle(handle);
        throw Errno{kEACCES};
    }
    return handle;
}

NinePServer::Info NinePServer::InfoOf(HANDLE handle)
{
    BY_HANDLE_FILE_INFORMATION data{};
    if (!GetFileInformationByHandle(handle, &data)) ThrowLastError();
    Info info;
    info.attributes = data.dwFileAttributes;
    info.id = (static_cast<std::uint64_t>(data.nFileIndexHigh) << 32) | data.nFileIndexLow;
    info.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    info.links = data.nNumberOfLinks;
    info.access = data.ftLastAccessTime;
    info.write = data.ftLastWriteTime;
    info.creation = data.ftCreationTime;
    return info;
}

NinePServer::Info NinePServer::Stat(const std::wstring& path) const
{
    ScopedHandle handle(Open(path, FILE_READ_ATTRIBUTES, OPEN_EXISTING));
    return InfoOf(handle.Get());
}

void NinePServer::RequireWritable() const
{
    if (readOnly_) throw Errno{kEROFS};
}

NinePServer::Fid& NinePServer::Lookup(std::uint32_t fid)
{
    const auto found = fids_.find(fid);
    if (found == fids_.end()) throw Errno{kEBADF};
    return found->second;
}

void NinePServer::Clunk(std::uint32_t fid)
{
    const auto found = fids_.find(fid);
    if (found == fids_.end()) return;
    if (found->second.file != INVALID_HANDLE_VALUE) CloseHandle(found->second.file);
    fids_.erase(found);
}

void NinePServer::Delete(const std::wstring& path)
{
    // Opening the entry itself (not what a link points to): deleting a
    // junction never touches its target.
    ScopedHandle handle(Open(path, DELETE, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT));
    FILE_DISPOSITION_INFO_EX posix{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                                   FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    if (SetFileInformationByHandle(handle.Get(), FileDispositionInfoEx, &posix, sizeof(posix))) return;
    const DWORD error = GetLastError();
    if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED && error != ERROR_INVALID_FUNCTION) {
        throw Errno{FromWin32(error)};
    }
    FILE_DISPOSITION_INFO classic{TRUE}; // File systems without POSIX semantics (FAT, network).
    if (!SetFileInformationByHandle(handle.Get(), FileDispositionInfo, &classic, sizeof(classic))) ThrowLastError();
}

void NinePServer::Rename(const std::wstring& from, const std::wstring& to)
{
    ScopedHandle handle(Open(from, DELETE | SYNCHRONIZE, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT));
    const auto bytes = to.size() * sizeof(wchar_t);
    std::vector<std::uint8_t> buffer(sizeof(FILE_RENAME_INFO) + bytes, 0);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(bytes);
    std::memcpy(info->FileName, to.data(), bytes);
    if (SetFileInformationByHandle(handle.Get(), FileRenameInfoEx, info, static_cast<DWORD>(buffer.size()))) return;
    const DWORD error = GetLastError();
    if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED && error != ERROR_INVALID_FUNCTION) {
        throw Errno{FromWin32(error)};
    }
    info->Flags = 0;
    info->ReplaceIfExists = TRUE;
    if (!SetFileInformationByHandle(handle.Get(), FileRenameInfo, info, static_cast<DWORD>(buffer.size()))) ThrowLastError();
}

void NinePServer::ReadDirectory(Fid& fid)
{
    fid.listing.clear();
    ScopedHandle handle(Open(HostPath(fid.components), FILE_LIST_DIRECTORY | SYNCHRONIZE, OPEN_EXISTING));
    // Same file index as GetFileInformationByHandle: readdir's d_ino matches stat's st_ino.
    std::vector<std::uint8_t> buffer(64 * 1024);
    FILE_INFO_BY_HANDLE_CLASS kind = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(handle.Get(), kind, buffer.data(), static_cast<DWORD>(buffer.size()))) {
            const DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_FILES) break;
            throw Errno{FromWin32(error)};
        }
        kind = FileIdBothDirectoryInfo;
        std::size_t offset = 0;
        for (;;) {
            const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
            DirEntry item;
            item.name = Narrow(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
            item.id = static_cast<std::uint64_t>(entry->FileId.QuadPart);
            item.directory = (entry->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            fid.listing.push_back(std::move(item));
            if (entry->NextEntryOffset == 0) break;
            offset += entry->NextEntryOffset;
        }
    }
}

std::vector<std::uint8_t> NinePServer::Handle(std::span<const std::uint8_t> request)
{
    std::vector<std::uint8_t> response;
    response.reserve(256);
    Writer out(response);
    std::uint16_t tag = 0xFFFF;
    std::uint8_t type = 0;
    try {
        Reader in(request);
        const auto size = in.U32();
        if (size < kHeaderSize || size > request.size()) throw Errno{kEPROTO};
        type = in.U8();
        tag = in.U16();
        Reader body(request.subspan(kHeaderSize, size - kHeaderSize));
        out.U32(0);
        out.U8(static_cast<std::uint8_t>(type + 1));
        out.U16(tag);
        Dispatch(type, body, out);
    } catch (const Errno& error) {
        response.clear();
        out.U32(0);
        out.U8(kTlerror + 1); // Rlerror.
        out.U16(tag);
        out.U32(error.code);
    } catch (const std::exception&) {
        response.clear();
        out.U32(0);
        out.U8(kTlerror + 1);
        out.U16(tag);
        out.U32(kEIO);
    }
    out.Patch32(0, static_cast<std::uint32_t>(response.size()));
    return response;
}

void NinePServer::Dispatch(std::uint8_t type, Reader& in, Writer& out)
{
    switch (type) {
    case kTversion: {
        const auto msize = in.U32();
        const auto version = in.String();
        for (auto& [id, fid] : fids_) {
            if (fid.file != INVALID_HANDLE_VALUE) CloseHandle(fid.file);
        }
        fids_.clear();
        msize_ = (std::min)(msize, kMaxMessageSize);
        out.U32(msize_);
        out.String(version.rfind("9P2000.L", 0) == 0 ? "9P2000.L" : "unknown");
        return;
    }
    case kTattach: {
        const auto fid = in.U32();
        (void)in.U32(); // afid: no authentication.
        (void)in.String(); // uname
        (void)in.String(); // aname: one tree per device.
        const auto uid = in.U32();
        const auto info = Stat(HostPath({}));
        Clunk(fid);
        Fid state;
        state.uid = uid == 0xFFFFFFFF ? 0 : uid;
        fids_[fid] = std::move(state);
        out.Qid(info);
        return;
    }
    case kTflush:
        (void)in.U16(); // oldtag: requests complete in order, nothing is in flight.
        return;
    case kTwalk: {
        const auto fid = in.U32();
        const auto newfid = in.U32();
        const auto count = in.U16();
        if (count > 16) throw Errno{kEINVAL};
        const auto& source = Lookup(fid);
        if (newfid != fid && fids_.contains(newfid)) throw Errno{kEINVAL};
        auto components = source.components;
        std::vector<Info> qids;
        for (std::uint16_t index = 0; index < count; ++index) {
            const auto name = in.String();
            try {
                if (name == "..") {
                    if (components.empty()) throw Errno{kEACCES}; // Never above the shared folder.
                    components.pop_back();
                } else if (name != ".") {
                    components.push_back(ValidName(name));
                }
                qids.push_back(Stat(HostPath(components)));
            } catch (const Errno&) {
                if (index == 0) throw;
                break; // Partial walk: the qids found so far, newfid untouched.
            }
        }
        if (qids.size() == count) {
            Fid walked;
            walked.components = std::move(components);
            walked.uid = source.uid;
            if (newfid == fid) Clunk(fid);
            fids_[newfid] = std::move(walked);
        }
        out.U16(static_cast<std::uint16_t>(qids.size()));
        for (const auto& info : qids) out.Qid(info);
        return;
    }
    case kTlopen: {
        auto& fid = Lookup(in.U32());
        const auto flags = in.U32();
        if (fid.open) throw Errno{kEINVAL};
        const auto path = HostPath(fid.components);
        const auto info = Stat(path);
        if (!info.Directory()) {
            const auto mode = flags & kAccessMode;
            const bool writing = mode != kReadOnly;
            if (writing) RequireWritable();
            const DWORD access = FILE_READ_ATTRIBUTES | (mode == kReadOnly ? GENERIC_READ : mode == kWriteOnly ? GENERIC_WRITE
                                                                                      : GENERIC_READ | GENERIC_WRITE);
            fid.file = Open(path, access, OPEN_EXISTING);
            if (writing && (flags & kTruncate) != 0) {
                FILE_END_OF_FILE_INFO end{};
                if (!SetFileInformationByHandle(fid.file, FileEndOfFileInfo, &end, sizeof(end))) ThrowLastError();
            }
            fid.append = (flags & kAppend) != 0;
        }
        fid.open = true;
        out.Qid(info);
        out.U32(0); // iounit: up to msize.
        return;
    }
    case kTlcreate: {
        auto& fid = Lookup(in.U32());
        const auto name = ValidName(in.String());
        const auto flags = in.U32();
        (void)in.U32(); // mode: see the class comment.
        (void)in.U32(); // gid
        RequireWritable();
        if (fid.open) throw Errno{kEINVAL};
        if (!Stat(HostPath(fid.components)).Directory()) throw Errno{kENOTDIR};
        auto components = fid.components;
        components.push_back(name);
        const auto mode = flags & kAccessMode;
        const DWORD access = FILE_READ_ATTRIBUTES | (mode == kReadOnly ? GENERIC_READ : mode == kWriteOnly ? GENERIC_WRITE
                                                                                  : GENERIC_READ | GENERIC_WRITE);
        const DWORD disposition = (flags & kExclusive) != 0 ? CREATE_NEW
                                  : (flags & kTruncate) != 0 ? CREATE_ALWAYS : OPEN_ALWAYS;
        const HANDLE file = Open(HostPath(components), access, disposition);
        Info info;
        try {
            info = InfoOf(file);
        } catch (...) {
            CloseHandle(file);
            throw;
        }
        fid.components = std::move(components);
        fid.file = file;
        fid.open = true;
        fid.append = (flags & kAppend) != 0;
        out.Qid(info);
        out.U32(0);
        return;
    }
    case kTread: {
        auto& fid = Lookup(in.U32());
        const auto offset = in.U64();
        auto count = in.U32();
        if (!fid.open) throw Errno{kEBADF};
        if (fid.file == INVALID_HANDLE_VALUE) throw Errno{kEISDIR};
        count = (std::min)(count, msize_ - kHeaderSize - 4);
        const auto countAt = out.Size();
        out.U32(0);
        auto* data = out.Reserve(count);
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        if (!ReadFile(fid.file, data, count, &read, &position) && GetLastError() != ERROR_HANDLE_EOF) ThrowLastError();
        out.Shrink(count - read);
        out.Patch32(countAt, read);
        return;
    }
    case kTwrite: {
        auto& fid = Lookup(in.U32());
        const auto offset = in.U64();
        const auto count = in.U32();
        const auto data = in.Bytes(count);
        RequireWritable();
        if (!fid.open) throw Errno{kEBADF};
        if (fid.file == INVALID_HANDLE_VALUE) throw Errno{kEISDIR};
        OVERLAPPED position{};
        if (fid.append) {
            position.Offset = position.OffsetHigh = 0xFFFFFFFF; // End of file.
        } else {
            position.Offset = static_cast<DWORD>(offset);
            position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        }
        DWORD written = 0;
        if (!WriteFile(fid.file, data.data(), count, &written, &position)) ThrowLastError();
        out.U32(written);
        return;
    }
    case kTclunk: {
        const auto fid = in.U32();
        (void)Lookup(fid);
        Clunk(fid);
        return;
    }
    case kTreaddir: {
        auto& fid = Lookup(in.U32());
        const auto offset = in.U64();
        const auto count = (std::min)(in.U32(), msize_ - kHeaderSize - 4);
        if (!fid.open || fid.file != INVALID_HANDLE_VALUE) throw Errno{kENOTDIR};
        if (offset == 0 || fid.listing.empty()) ReadDirectory(fid);
        const auto countAt = out.Size();
        out.U32(0);
        const auto start = out.Size();
        for (auto index = offset; index < fid.listing.size(); ++index) {
            const auto& entry = fid.listing[static_cast<std::size_t>(index)];
            const std::size_t size = 13 + 8 + 1 + 2 + entry.name.size();
            if (out.Size() - start + size > count) break;
            out.Qid(entry.directory ? kQidDirectory : 0, entry.id);
            out.U64(index + 1); // Offset of the next entry.
            out.U8(entry.directory ? kDirentDirectory : kDirentRegular);
            out.String(entry.name);
        }
        out.Patch32(countAt, static_cast<std::uint32_t>(out.Size() - start));
        return;
    }
    case kTgetattr: {
        const auto& fid = Lookup(in.U32());
        (void)in.U64(); // request_mask: everything is cheap to report.
        const auto info = fid.file != INVALID_HANDLE_VALUE ? InfoOf(fid.file) : Stat(HostPath(fid.components));
        std::uint32_t mode = info.Directory() ? 0040755 : 0100755;
        if ((info.attributes & FILE_ATTRIBUTE_READONLY) != 0 && !info.Directory()) mode &= ~0222u;
        out.U64(kGetattrAll);
        out.Qid(info);
        out.U32(mode);
        out.U32(fid.uid); // Owned by whoever looks: no Windows ACL mapping.
        out.U32(fid.uid);
        out.U64(info.Directory() ? 1 : info.links); // 1: "unknown" for find's leaf optimization.
        out.U64(0);       // rdev
        out.U64(info.size);
        out.U64(4096);    // blksize
        out.U64((info.size + 511) / 512);
        for (const auto* time : {&info.access, &info.write, &info.write, &info.creation}) {
            const auto ns = ToUnixNs(*time);
            out.U64(ns / 1'000'000'000);
            out.U64(ns % 1'000'000'000);
        }
        out.U64(0); // gen
        out.U64(0); // data_version
        return;
    }
    case kTsetattr: {
        const auto& fid = Lookup(in.U32());
        const auto valid = in.U32();
        const auto mode = in.U32();
        (void)in.U32(); // uid: no ownership on the host side.
        (void)in.U32(); // gid
        const auto size = in.U64();
        const auto atimeSeconds = in.U64();
        const auto atimeNanoseconds = in.U64();
        const auto mtimeSeconds = in.U64();
        const auto mtimeNanoseconds = in.U64();
        if ((valid & (kSetMode | kSetSize | kSetAtime | kSetMtime)) == 0) return; // Ownership or ctime only.
        RequireWritable();
        const auto path = HostPath(fid.components);
        if ((valid & kSetSize) != 0) {
            ScopedHandle handle(Open(path, GENERIC_WRITE, OPEN_EXISTING));
            FILE_END_OF_FILE_INFO end{};
            end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
            if (!SetFileInformationByHandle(handle.Get(), FileEndOfFileInfo, &end, sizeof(end))) ThrowLastError();
        }
        if ((valid & (kSetMode | kSetAtime | kSetMtime)) != 0) {
            ScopedHandle handle(Open(path, FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, OPEN_EXISTING));
            FILE_BASIC_INFO basic{};
            if (!GetFileInformationByHandleEx(handle.Get(), FileBasicInfo, &basic, sizeof(basic))) ThrowLastError();
            FILE_BASIC_INFO change{}; // Zero times: unchanged.
            change.FileAttributes = basic.FileAttributes;
            if ((valid & kSetMode) != 0 && (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                // Only the owner's write bit maps to something: the read-only attribute.
                if ((mode & 0200) != 0) change.FileAttributes &= ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
                else change.FileAttributes |= FILE_ATTRIBUTE_READONLY;
            }
            const auto toLarge = [](const FILETIME& time) {
                LARGE_INTEGER value{};
                value.LowPart = time.dwLowDateTime;
                value.HighPart = static_cast<LONG>(time.dwHighDateTime);
                return value;
            };
            if ((valid & kSetAtime) != 0) {
                change.LastAccessTime = toLarge((valid & kSetAtimeValue) != 0 ? FromUnix(atimeSeconds, atimeNanoseconds) : Now());
            }
            if ((valid & kSetMtime) != 0) {
                change.LastWriteTime = toLarge((valid & kSetMtimeValue) != 0 ? FromUnix(mtimeSeconds, mtimeNanoseconds) : Now());
            }
            if (change.FileAttributes == 0) change.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            if (!SetFileInformationByHandle(handle.Get(), FileBasicInfo, &change, sizeof(change))) ThrowLastError();
        }
        return;
    }
    case kTmkdir: {
        const auto& fid = Lookup(in.U32());
        const auto name = ValidName(in.String());
        (void)in.U32(); // mode
        (void)in.U32(); // gid
        RequireWritable();
        if (!Stat(HostPath(fid.components)).Directory()) throw Errno{kENOTDIR};
        auto components = fid.components;
        components.push_back(name);
        const auto path = HostPath(components);
        if (!CreateDirectoryW(path.c_str(), nullptr)) ThrowLastError();
        out.Qid(Stat(path));
        return;
    }
    case kTunlinkat: {
        const auto& fid = Lookup(in.U32());
        const auto name = ValidName(in.String());
        const auto flags = in.U32();
        RequireWritable();
        auto components = fid.components;
        components.push_back(name);
        const auto path = HostPath(components);
        const auto info = Stat(path);
        if ((flags & kRemoveDirectory) != 0 && !info.Directory()) throw Errno{kENOTDIR};
        if ((flags & kRemoveDirectory) == 0 && info.Directory()) throw Errno{kEISDIR};
        Delete(path);
        return;
    }
    case kTrenameat: {
        const auto& oldDirectory = Lookup(in.U32());
        const auto oldName = ValidName(in.String());
        const auto& newDirectory = Lookup(in.U32());
        const auto newName = ValidName(in.String());
        RequireWritable();
        auto from = oldDirectory.components;
        from.push_back(oldName);
        auto to = newDirectory.components;
        to.push_back(newName);
        if (!Stat(HostPath(newDirectory.components)).Directory()) throw Errno{kENOTDIR};
        Rename(HostPath(from), HostPath(to));
        // Fids below the old name now live below the new one.
        for (auto& [id, other] : fids_) {
            if (other.components.size() < from.size()) continue;
            bool below = true;
            for (std::size_t index = 0; index < from.size() && below; ++index) {
                below = EqualNoCase(other.components[index].data(), other.components[index].size(),
                                    from[index].data(), from[index].size());
            }
            if (!below) continue;
            std::vector<std::wstring> moved = to;
            moved.insert(moved.end(), other.components.begin() + static_cast<std::ptrdiff_t>(from.size()),
                         other.components.end());
            other.components = std::move(moved);
        }
        return;
    }
    case kTstatfs: {
        (void)Lookup(in.U32());
        ULARGE_INTEGER available{}, total{}, free{};
        const auto root = rootFinal_ + L"\\";
        if (!GetDiskFreeSpaceExW(root.c_str(), &available, &total, &free)) ThrowLastError();
        out.U32(kV9fsMagic);
        out.U32(4096);
        out.U64(total.QuadPart / 4096);
        out.U64(free.QuadPart / 4096);
        out.U64(available.QuadPart / 4096);
        out.U64(0); // files: unknown.
        out.U64(0);
        out.U64(0); // fsid
        out.U32(255);
        return;
    }
    default:
        throw Errno{kEOPNOTSUPP};
    }
}

} // namespace vmm
