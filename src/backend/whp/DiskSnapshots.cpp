#include "DiskSnapshots.h"

#include "DiskLayer.h"

#include <Windows.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace virtualiseur::whp {
namespace {

std::string ToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring FromUtf8(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

// One line per value in the metadata file.
std::wstring SingleLine(std::wstring text)
{
    for (auto& c : text) {
        if (c == L'\r' || c == L'\n') c = L' ';
    }
    return text;
}

std::wstring Now()
{
    const auto now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    wchar_t text[32] = {};
    wcsftime(text, 32, L"%Y-%m-%d %H:%M:%S", &local);
    return text;
}

std::uint64_t AllocatedBytes(const std::filesystem::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    DWORD high = 0;
    const DWORD low = GetCompressedFileSizeW(path.c_str(), &high); // Sparse files: what is really used.
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
        return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }
    return (static_cast<std::uint64_t>(high) << 32) | low;
}

} // namespace

SnapshotStore::SnapshotStore(std::filesystem::path baseDisk)
    : base_(std::move(baseDisk)), directory_(base_.parent_path()), baseName_(base_.filename().wstring())
{
    Load();
    ComputeParents();
}

std::filesystem::path SnapshotStore::MetadataPath() const
{
    return directory_ / (base_.stem().wstring() + L".snapshots.ini");
}

std::filesystem::path SnapshotStore::CurrentDisk(const std::filesystem::path& baseDisk)
{
    const SnapshotStore store(baseDisk);
    return store.CurrentLayer();
}

std::filesystem::path SnapshotStore::CurrentLayer() const
{
    return current_.empty() ? base_ : PathOf(current_);
}

const SnapshotStore::Snapshot* SnapshotStore::Find(std::uint32_t id) const noexcept
{
    for (const auto& snapshot : snapshots_) {
        if (snapshot.id == id) return &snapshot;
    }
    return nullptr;
}

void SnapshotStore::Load()
{
    std::ifstream file(MetadataPath(), std::ios::binary);
    if (!file) return;
    std::string line;
    Snapshot* section = nullptr;
    bool store = false;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            const auto name = line.substr(1, line.size() - 2);
            store = name == "store";
            section = nullptr;
            if (name.rfind("snapshot.", 0) == 0) {
                Snapshot snapshot;
                snapshot.id = static_cast<std::uint32_t>(std::strtoul(name.c_str() + 9, nullptr, 10));
                if (snapshot.id != 0) {
                    snapshots_.push_back(snapshot);
                    section = &snapshots_.back();
                }
            }
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const auto key = line.substr(0, equals);
        const auto value = FromUtf8(line.substr(equals + 1));
        if (store) {
            if (key == "current") current_ = value;
            else if (key == "next") next_ = (std::max<std::uint32_t>)(1, static_cast<std::uint32_t>(std::wcstoul(value.c_str(), nullptr, 10)));
        } else if (section != nullptr) {
            if (key == "name") section->name = value;
            else if (key == "description") section->description = value;
            else if (key == "date") section->date = value;
            else if (key == "layer") section->layer = value;
        }
    }
    // A snapshot without a usable layer cannot be restored: drop it.
    std::erase_if(snapshots_, [this](const Snapshot& snapshot) {
        return snapshot.layer.empty() || !std::filesystem::exists(PathOf(snapshot.layer));
    });
    if (!current_.empty() && !std::filesystem::exists(PathOf(current_))) {
        throw std::runtime_error("Couche active du disque introuvable : " + PathOf(current_).string());
    }
    for (const auto& snapshot : snapshots_) next_ = (std::max)(next_, snapshot.id + 1);
}

void SnapshotStore::Save() const
{
    std::ostringstream text;
    text << "# Instantanés du disque " << ToUtf8(baseName_) << " (Virtualiseur). Ne pas modifier à la main.\n";
    text << "[store]\ncurrent=" << ToUtf8(current_) << "\nnext=" << next_ << "\n";
    for (const auto& snapshot : snapshots_) {
        text << "\n[snapshot." << snapshot.id << "]\n"
             << "name=" << ToUtf8(SingleLine(snapshot.name)) << "\n"
             << "description=" << ToUtf8(SingleLine(snapshot.description)) << "\n"
             << "date=" << ToUtf8(snapshot.date) << "\n"
             << "layer=" << ToUtf8(snapshot.layer) << "\n";
    }
    // Write a temporary file, then replace: the metadata is never half-written.
    const auto target = MetadataPath();
    auto temporary = target;
    temporary += L".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const auto data = text.str();
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.flush();
        if (!file) throw std::runtime_error("Écriture des métadonnées d'instantanés impossible.");
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "MoveFileExW(instantanés)");
    }
}

std::wstring SnapshotStore::NewLayerName()
{
    for (;;) {
        // Layer files have their own numbering (snapshot ids are next_).
        auto name = base_.stem().wstring() + L"-" + std::to_wstring(nextLayer_++) + L".layer";
        if (!std::filesystem::exists(PathOf(name))) return name;
    }
}

std::uint32_t SnapshotStore::SnapshotAbove(const std::wstring& layer) const
{
    // Walk down from `layer`'s parent until a layer named by a snapshot.
    auto current = LayerFile::ParentOf(PathOf(layer));
    for (int depth = 0; !current.empty() && depth < 4096; ++depth) {
        for (const auto& snapshot : snapshots_) {
            if (_wcsicmp(snapshot.layer.c_str(), current.c_str()) == 0) return snapshot.id;
        }
        current = LayerFile::ParentOf(PathOf(current));
    }
    return 0;
}

void SnapshotStore::ComputeParents()
{
    for (auto& snapshot : snapshots_) snapshot.parent = SnapshotAbove(snapshot.layer);
}

std::uint32_t SnapshotStore::CurrentParent() const
{
    if (current_.empty()) return 0;
    // The current layer's parent is a snapshot layer itself (taken or restored).
    const auto parent = LayerFile::ParentOf(PathOf(current_));
    for (const auto& snapshot : snapshots_) {
        if (_wcsicmp(snapshot.layer.c_str(), parent.c_str()) == 0) return snapshot.id;
    }
    return SnapshotAbove(current_);
}

std::vector<std::wstring> SnapshotStore::LiveLayers() const
{
    std::vector<std::wstring> live;
    const auto add = [&](std::wstring layer) {
        for (int depth = 0; !layer.empty() && depth < 4096; ++depth) {
            if (std::any_of(live.begin(), live.end(), [&](const std::wstring& known) { return _wcsicmp(known.c_str(), layer.c_str()) == 0; })) return;
            live.push_back(layer);
            layer = LayerFile::ParentOf(PathOf(layer));
        }
    };
    add(current_.empty() ? baseName_ : current_);
    for (const auto& snapshot : snapshots_) add(snapshot.layer);
    return live;
}

std::vector<std::wstring> SnapshotStore::ChildrenOf(const std::wstring& layer) const
{
    std::vector<std::wstring> children;
    for (const auto& candidate : LiveLayers()) {
        if (_wcsicmp(LayerFile::ParentOf(PathOf(candidate)).c_str(), layer.c_str()) == 0) children.push_back(candidate);
    }
    return children;
}

std::uint64_t SnapshotStore::LayerBytes() const
{
    std::uint64_t total = 0;
    for (const auto& layer : LiveLayers()) {
        if (_wcsicmp(layer.c_str(), baseName_.c_str()) != 0) total += AllocatedBytes(PathOf(layer));
    }
    return total;
}

void SnapshotStore::RequireStopped() const
{
    // A running VM keeps its current layer open for writing: an exclusive
    // open fails with a sharing violation.
    const auto path = CurrentLayer();
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION) {
            throw std::runtime_error("La machine utilise ce disque : éteins-la d'abord.");
        }
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Disque de la machine");
    }
    CloseHandle(file);
}

std::uint32_t SnapshotStore::Take(const std::wstring& name, const std::wstring& description)
{
    RequireStopped();
    const auto top = current_.empty() ? baseName_ : current_;
    const auto size = LayerFile::SizeOf(PathOf(top));
    if (size == 0) throw std::runtime_error("Disque de la machine illisible : " + PathOf(top).string());
    // The current layer freezes as the snapshot; writes go to a new layer.
    const auto layer = NewLayerName();
    LayerFile::Create(PathOf(layer), top, size);
    Snapshot snapshot;
    snapshot.id = next_++;
    snapshot.name = name;
    snapshot.description = description;
    snapshot.date = Now();
    snapshot.layer = top;
    snapshots_.push_back(snapshot);
    current_ = layer;
    try {
        Save();
    } catch (...) {
        std::filesystem::remove(PathOf(layer));
        throw;
    }
    ComputeParents();
    return snapshot.id;
}

void SnapshotStore::Restore(std::uint32_t id)
{
    RequireStopped();
    const auto* snapshot = Find(id);
    if (snapshot == nullptr) throw std::invalid_argument("Instantané inconnu.");
    // Always a new current layer over the snapshot: frozen layers stay untouched.
    const auto size = LayerFile::SizeOf(PathOf(snapshot->layer));
    const auto layer = NewLayerName();
    LayerFile::Create(PathOf(layer), snapshot->layer, size);
    const auto previous = current_;
    current_ = layer;
    try {
        Save();
    } catch (...) {
        current_ = previous;
        std::filesystem::remove(PathOf(layer));
        throw;
    }
    // The previous current layer held the changes made since its snapshot.
    if (!previous.empty()) {
        const auto live = LiveLayers();
        const bool used = std::any_of(live.begin(), live.end(), [&](const std::wstring& known) { return _wcsicmp(known.c_str(), previous.c_str()) == 0; });
        if (!used) {
            std::error_code ignored;
            std::filesystem::remove(PathOf(previous), ignored);
        }
    }
}

void SnapshotStore::Remove(std::uint32_t id)
{
    RequireStopped();
    const auto found = std::find_if(snapshots_.begin(), snapshots_.end(), [id](const Snapshot& snapshot) { return snapshot.id == id; });
    if (found == snapshots_.end()) throw std::invalid_argument("Instantané inconnu.");
    const auto layer = found->layer;
    const bool isBase = _wcsicmp(layer.c_str(), baseName_.c_str()) == 0;
    const auto children = ChildrenOf(layer);
    const auto rename = [&](const std::wstring& from, const std::wstring& to) {
        for (auto& snapshot : snapshots_) {
            if (_wcsicmp(snapshot.layer.c_str(), from.c_str()) == 0) snapshot.layer = to;
        }
        if (_wcsicmp(current_.c_str(), from.c_str()) == 0) current_ = to;
    };
    std::vector<std::byte> block(LayerFile::kBlockSize);

    if (children.size() == 1) {
        // Merge the child into this layer: the child overrides every block it
        // has, so its view does not change while its blocks are copied down.
        const auto child = children.front();
        auto source = LayerFile::Open(PathOf(child), false);
        if (isBase) {
            const HANDLE base = CreateFileW(PathOf(layer).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (base == INVALID_HANDLE_VALUE) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreateFileW(base)");
            }
            try {
                const auto size = source.VirtualSize();
                for (const auto index : source.AllocatedBlocks()) {
                    source.Read(index, 0, block);
                    const auto offset = index * LayerFile::kBlockSize;
                    const auto length = (std::min<std::uint64_t>)(LayerFile::kBlockSize, size - offset);
                    WriteAt(base, offset, block.data(), static_cast<std::size_t>(length));
                }
                if (!FlushFileBuffers(base)) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "FlushFileBuffers");
            } catch (...) {
                CloseHandle(base);
                throw;
            }
            CloseHandle(base);
        } else {
            auto target = LayerFile::Open(PathOf(layer), true);
            for (const auto index : source.AllocatedBlocks()) {
                source.Read(index, 0, block);
                if (target.Has(index)) target.Write(index, 0, block);
                else target.Append(index, block);
            }
            target.Flush();
        }
        // This layer now shows exactly what the child showed: it takes its place.
        for (const auto& grandchild : ChildrenOf(child)) LayerFile::Open(PathOf(grandchild), true).SetParent(layer);
        snapshots_.erase(std::find_if(snapshots_.begin(), snapshots_.end(), [id](const Snapshot& snapshot) { return snapshot.id == id; }));
        rename(child, layer);
        Save();
        source = LayerFile{};
        std::error_code ignored;
        std::filesystem::remove(PathOf(child), ignored);
    } else if (children.empty()) {
        // Nothing builds on it any more (its branch was restored away).
        snapshots_.erase(found);
        Save();
        if (!isBase) {
            std::error_code ignored;
            std::filesystem::remove(PathOf(layer), ignored);
        }
    } else {
        if (isBase) {
            throw std::runtime_error("Plusieurs branches partent de cet instantané, qui est le disque de base : "
                                     "supprime d'abord les instantanés des autres branches.");
        }
        // Copy this layer's blocks into each child where absent (their views
        // do not change), then hang the children on this layer's parent.
        const auto source = LayerFile::Open(PathOf(layer), false);
        for (const auto& child : children) {
            auto target = LayerFile::Open(PathOf(child), true);
            for (const auto index : source.AllocatedBlocks()) {
                if (target.Has(index)) continue;
                source.Read(index, 0, block);
                target.Append(index, block);
            }
            target.Flush();
        }
        for (const auto& child : children) LayerFile::Open(PathOf(child), true).SetParent(source.ParentName());
        snapshots_.erase(found);
        Save();
    }
    ComputeParents();
    if (children.size() > 1) {
        std::error_code ignored;
        std::filesystem::remove(PathOf(layer), ignored);
    }
}

} // namespace virtualiseur::whp
