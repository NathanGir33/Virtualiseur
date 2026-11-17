#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace vmm {

// 9P2000.L file server over a Windows folder: the protocol the Linux "9p"
// file system speaks over virtio (mount -t 9p -o trans=virtio,version=9p2000.L).
//
// Scope: Tversion, Tattach, Twalk, Tlopen, Tlcreate, Tread, Twrite,
// Treaddir, Tgetattr, Tsetattr, Tmkdir, Tunlinkat, Trenameat, Tclunk,
// Tflush and Tstatfs. Every other message (xattrs, links, locks, fsync...)
// gets EOPNOTSUPP.
//
// Confinement: every host path is canonicalized and checked against the
// shared folder before it is opened, and every handle's final path (after
// junctions and symbolic links) is checked again; a path outside gets
// EACCES. Handles always share read, write and delete, as on Linux, and
// qid.path is the NTFS file index.
//
// Files keep Windows semantics where Linux has none: everything is owned by
// the user who looks at it, mode 0755 (no write bits for files marked
// read-only). Deleting and renaming use the POSIX flavour of NTFS (an open
// file can be deleted or replaced).
//
// Not thread-safe: one thread handles all the messages of a share.
class NinePServer final {
public:
    static constexpr std::uint32_t kMaxMessageSize = 1024 * 1024;

    // Throws if `root` is not an existing folder.
    NinePServer(const std::filesystem::path& root, bool readOnly);
    ~NinePServer();
    NinePServer(const NinePServer&) = delete;
    NinePServer& operator=(const NinePServer&) = delete;

    // Handles one T-message and returns the R-message (Rlerror on failure).
    std::vector<std::uint8_t> Handle(std::span<const std::uint8_t> request);

    // The shared folder, as Windows resolves it (\\?\ form).
    const std::wstring& RootPath() const noexcept { return rootFinal_; }
    bool ReadOnly() const noexcept { return readOnly_; }

private:
    struct DirEntry {
        std::string name; // UTF-8.
        std::uint64_t id = 0;
        bool directory = false;
    };
    struct Fid {
        std::vector<std::wstring> components; // Path below the root.
        std::uint32_t uid = 0;
        HANDLE file = INVALID_HANDLE_VALUE;   // Open regular file (Tlopen/Tlcreate).
        bool open = false;
        bool append = false;
        std::vector<DirEntry> listing;        // Directory read by Treaddir.
    };
    struct Info {
        DWORD attributes = 0;
        std::uint64_t id = 0;
        std::uint64_t size = 0;
        std::uint64_t links = 1;
        FILETIME access{}, write{}, creation{};
        bool Directory() const noexcept { return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
    };
    class Reader;
    class Writer;

    void Dispatch(std::uint8_t type, Reader& in, Writer& out);
    Fid& Lookup(std::uint32_t fid);
    void Clunk(std::uint32_t fid);

    // Canonical \\?\ path of `components` below the root; EACCES outside.
    std::wstring HostPath(const std::vector<std::wstring>& components) const;
    // Opens with full sharing, then checks the final path; EACCES outside.
    HANDLE Open(const std::wstring& path, DWORD access, DWORD disposition, DWORD flags = 0) const;
    bool Inside(HANDLE handle) const;
    Info Stat(const std::wstring& path) const;
    static Info InfoOf(HANDLE handle);
    void RequireWritable() const;
    void Delete(const std::wstring& path);
    void Rename(const std::wstring& from, const std::wstring& to);
    void ReadDirectory(Fid& fid);

    std::wstring rootFinal_;  // \\?\C:\... or \\?\UNC\server\share\...
    std::wstring rootPlain_;  // Same without the \\?\ prefix, for GetFullPathNameW.
    bool unc_ = false;
    bool readOnly_ = false;
    std::uint32_t msize_ = 8192;
    std::map<std::uint32_t, Fid> fids_;
};

} // namespace vmm
