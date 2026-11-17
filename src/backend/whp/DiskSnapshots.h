#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace virtualiseur::whp {

// Disk snapshots of one VM, without RAM: each snapshot names a frozen layer
// (LayerFile) or the raw base image; the VM writes to a "current" layer on
// top. The metadata lives next to the base image, in <stem>.snapshots.ini
// (UTF-8, rewritten atomically): the snapshots and the current layer. The
// base path stays the VM's disk; CurrentDisk() resolves what to open.
//
// Rules:
//  - Taking or restoring a snapshot always creates a new, empty current
//    layer; existing layers are never written. Restoring drops the old
//    current layer (the changes made since the snapshot it came from).
//  - Deleting a snapshot merges its layer with its child so that every
//    remaining layer keeps exactly the same view of the disk, and only then
//    updates the references: a crash in between loses nothing.
//  - The VM must be stopped while snapshots are taken, restored or deleted.
class SnapshotStore final {
public:
    struct Snapshot {
        std::uint32_t id = 0;
        std::wstring name;
        std::wstring description;
        std::wstring date;   // "YYYY-MM-DD HH:MM:SS", local time.
        std::wstring layer;  // File name (same folder): frozen layer or the base image.
        std::uint32_t parent = 0; // Snapshot this one derives from (0: none).
        // How it was taken: "off" (VM stopped), "frozen" (running, guest file
        // systems frozen by qemu-guest-agent) or "crash" (running without the
        // freeze: consistent as after a power cut).
        std::wstring state = L"off";
    };

    // Loads the store of `baseDisk` (empty when no snapshot was ever taken).
    explicit SnapshotStore(std::filesystem::path baseDisk);

    // What the VM opens: the current layer, or the base image itself.
    static std::filesystem::path CurrentDisk(const std::filesystem::path& baseDisk);

    const std::vector<Snapshot>& Snapshots() const noexcept { return snapshots_; }
    const Snapshot* Find(std::uint32_t id) const noexcept;
    std::filesystem::path CurrentLayer() const;
    // Snapshot the current state derives from (0: none).
    std::uint32_t CurrentParent() const;
    // Bytes taken on the host by the layer files (not the base image).
    std::uint64_t LayerBytes() const;

    std::uint32_t Take(const std::wstring& name, const std::wstring& description);

    // Running VM (the caller owns the open disk and switches it with
    // VirtualDisk::PushLayer): creates the new current layer over the current
    // one and records the snapshot. AbortLive undoes it if the switch fails.
    struct LiveTake {
        std::uint32_t id = 0;
        std::filesystem::path layer; // The new current layer.
        std::wstring previous;       // The previous current layer (empty: base).
    };
    LiveTake BeginLive(const std::wstring& name, const std::wstring& description, const std::wstring& state);
    void AbortLive(const LiveTake& take);
    void Restore(std::uint32_t id);
    void Remove(std::uint32_t id);

private:
    std::filesystem::path PathOf(const std::wstring& file) const { return directory_ / file; }
    std::filesystem::path MetadataPath() const;
    std::wstring NewLayerName();
    // Snapshot whose layer is the nearest ancestor of `layer` (0: none).
    std::uint32_t SnapshotAbove(const std::wstring& layer) const;
    // Layers in use: the current one, the snapshots' and all their ancestors.
    std::vector<std::wstring> LiveLayers() const;
    std::vector<std::wstring> ChildrenOf(const std::wstring& layer) const;
    void Load();
    void Save() const;
    void ComputeParents();
    // Throws if a VM has the current disk open.
    void RequireStopped() const;

    std::filesystem::path base_;
    std::filesystem::path directory_;
    std::wstring baseName_;
    std::wstring current_; // Empty: the base image.
    std::uint32_t next_ = 1;
    std::uint32_t nextLayer_ = 1;
    std::vector<Snapshot> snapshots_;
};

} // namespace virtualiseur::whp
