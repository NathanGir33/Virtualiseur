#include "BootSources.h"

#include "../core/Log.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <sstream>
#include <stdexcept>

namespace vmm {
namespace {

using Bytes = std::vector<std::uint8_t>;

template <typename T>
T Load(const std::uint8_t* data)
{
    T value{};
    std::memcpy(&value, data, sizeof(T));
    return value;
}

// --- Raw disk access ------------------------------------------------------------

class DiskReader {
public:
    explicit DiskReader(const virtualiseur::whp::VirtualDisk& disk) : disk_(disk) {}

    void Read(std::uint64_t offset, void* destination, std::size_t size) const
    {
        constexpr std::uint64_t sector = 512;
        const auto first = offset / sector;
        const auto last = (offset + size + sector - 1) / sector;
        Bytes buffer(static_cast<std::size_t>((last - first) * sector));
        disk_.ReadSectors(first, std::as_writable_bytes(std::span(buffer)));
        std::memcpy(destination, buffer.data() + (offset - first * sector), size);
    }

    std::uint64_t Size() const noexcept { return disk_.SizeBytes(); }

private:
    const virtualiseur::whp::VirtualDisk& disk_;
};

// --- ext2/3/4 read-only reader --------------------------------------------------

class ExtFileSystem {
public:
    static constexpr std::uint32_t kRootInode = 2;

    ExtFileSystem(const DiskReader& disk, std::uint64_t partitionOffset)
        : disk_(disk), base_(partitionOffset)
    {
        std::array<std::uint8_t, 1024> super{};
        disk_.Read(base_ + 1024, super.data(), super.size());
        if (Load<std::uint16_t>(&super[0x38]) != 0xEF53) throw std::runtime_error("pas un système ext");
        blockSize_ = 1024u << Load<std::uint32_t>(&super[0x18]);
        if (blockSize_ > 65536) throw std::runtime_error("taille de bloc ext invalide");
        firstDataBlock_ = Load<std::uint32_t>(&super[0x14]);
        inodesPerGroup_ = Load<std::uint32_t>(&super[0x28]);
        const auto revision = Load<std::uint32_t>(&super[0x4C]);
        inodeSize_ = revision == 0 ? 128 : Load<std::uint16_t>(&super[0x58]);
        incompat_ = Load<std::uint32_t>(&super[0x60]);
        if ((incompat_ & 0x10) != 0) throw std::runtime_error("ext4 meta_bg non pris en charge");
        is64Bit_ = (incompat_ & 0x80) != 0;
        descriptorSize_ = is64Bit_ ? Load<std::uint16_t>(&super[0xFE]) : 32;
        if (descriptorSize_ < 32) descriptorSize_ = 32;
        std::memcpy(uuid_.data(), &super[0x68], 16);
    }

    std::string Uuid() const
    {
        static constexpr char hex[] = "0123456789abcdef";
        std::string text;
        for (std::size_t index = 0; index < 16; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) text.push_back('-');
            text.push_back(hex[uuid_[index] >> 4]);
            text.push_back(hex[uuid_[index] & 0x0F]);
        }
        return text;
    }

    struct Inode {
        std::uint16_t mode = 0;
        std::uint64_t size = 0;
        std::uint32_t flags = 0;
        std::array<std::uint8_t, 60> block{};
        bool IsDirectory() const noexcept { return (mode & 0xF000) == 0x4000; }
        bool IsSymlink() const noexcept { return (mode & 0xF000) == 0xA000; }
        bool IsRegular() const noexcept { return (mode & 0xF000) == 0x8000; }
    };

    Inode ReadInode(std::uint32_t number) const
    {
        if (number == 0 || inodesPerGroup_ == 0) throw std::runtime_error("inode invalide");
        const auto group = (number - 1) / inodesPerGroup_;
        const auto index = (number - 1) % inodesPerGroup_;
        const auto descriptorOffset = static_cast<std::uint64_t>(firstDataBlock_ + 1) * blockSize_ +
                                      static_cast<std::uint64_t>(group) * descriptorSize_;
        std::array<std::uint8_t, 64> descriptor{};
        disk_.Read(base_ + descriptorOffset, descriptor.data(), descriptorSize_ >= 64 ? 64 : 32);
        std::uint64_t table = Load<std::uint32_t>(&descriptor[8]);
        if (is64Bit_ && descriptorSize_ >= 64) table |= static_cast<std::uint64_t>(Load<std::uint32_t>(&descriptor[0x28])) << 32;
        Bytes raw(inodeSize_ < 128 ? 128 : inodeSize_);
        disk_.Read(base_ + table * blockSize_ + static_cast<std::uint64_t>(index) * inodeSize_, raw.data(), 128);
        Inode inode;
        inode.mode = Load<std::uint16_t>(&raw[0]);
        inode.size = Load<std::uint32_t>(&raw[4]) | (static_cast<std::uint64_t>(Load<std::uint32_t>(&raw[0x6C])) << 32);
        inode.flags = Load<std::uint32_t>(&raw[0x20]);
        std::memcpy(inode.block.data(), &raw[0x28], 60);
        return inode;
    }

    Bytes ReadData(const Inode& inode, std::uint64_t maximum = 512ull * 1024 * 1024) const
    {
        if (inode.size > maximum) throw std::runtime_error("fichier trop volumineux");
        Bytes data(static_cast<std::size_t>(inode.size), 0);
        if ((inode.flags & 0x10000000) != 0) { // Inline data.
            std::memcpy(data.data(), inode.block.data(), (std::min<std::size_t>)(data.size(), 60));
            return data;
        }
        if ((inode.flags & 0x80000) != 0) {
            ReadExtentNode(inode.block.data(), 60, data, 0);
        } else {
            ReadBlockMap(inode, data);
        }
        return data;
    }

    struct DirectoryEntry {
        std::string name;
        std::uint32_t inode;
    };

    std::vector<DirectoryEntry> List(const Inode& directory) const
    {
        const auto data = ReadData(directory, 64ull * 1024 * 1024);
        std::vector<DirectoryEntry> entries;
        std::size_t offset = 0;
        while (offset + 8 <= data.size()) {
            const auto inode = Load<std::uint32_t>(&data[offset]);
            const auto recordLength = Load<std::uint16_t>(&data[offset + 4]);
            const std::size_t nameLength = (incompat_ & 0x02) != 0 ? data[offset + 6] : Load<std::uint16_t>(&data[offset + 6]);
            if (recordLength < 8 || offset + recordLength > data.size()) {
                offset = (offset / blockSize_ + 1) * blockSize_; // Skip a damaged block.
                continue;
            }
            if (inode != 0 && nameLength != 0 && offset + 8 + nameLength <= data.size()) {
                entries.push_back({std::string(reinterpret_cast<const char*>(&data[offset + 8]), nameLength), inode});
            }
            offset += recordLength;
        }
        return entries;
    }

    // Resolves an absolute path, following symbolic links.
    std::optional<std::uint32_t> Resolve(const std::string& path, int depth = 0) const
    {
        if (depth > 8) return std::nullopt;
        std::vector<std::string> parts;
        std::stringstream stream(path);
        std::string part;
        while (std::getline(stream, part, '/')) {
            if (!part.empty() && part != ".") parts.push_back(part);
        }
        std::uint32_t current = kRootInode;
        std::vector<std::uint32_t> stack{kRootInode};
        for (std::size_t index = 0; index < parts.size(); ++index) {
            if (parts[index] == "..") {
                if (stack.size() > 1) stack.pop_back();
                current = stack.back();
                continue;
            }
            const auto directory = ReadInode(current);
            if (!directory.IsDirectory()) return std::nullopt;
            std::optional<std::uint32_t> next;
            for (const auto& entry : List(directory)) {
                if (entry.name == parts[index]) { next = entry.inode; break; }
            }
            if (!next) return std::nullopt;
            const auto inode = ReadInode(*next);
            if (inode.IsSymlink()) {
                std::string target;
                if (inode.size < 60 && (inode.flags & 0x80000) == 0) {
                    target.assign(reinterpret_cast<const char*>(inode.block.data()), static_cast<std::size_t>(inode.size));
                } else {
                    const auto data = ReadData(inode, 4096);
                    target.assign(data.begin(), data.end());
                }
                std::string base;
                if (!target.empty() && target[0] != '/') {
                    for (std::size_t prefix = 0; prefix < index; ++prefix) base += "/" + parts[prefix];
                    base += "/";
                }
                std::string rest;
                for (std::size_t suffix = index + 1; suffix < parts.size(); ++suffix) rest += "/" + parts[suffix];
                return Resolve(base + target + rest, depth + 1);
            }
            current = *next;
            stack.push_back(current);
        }
        return current;
    }

    std::optional<Bytes> ReadFile(const std::string& path) const
    {
        try {
            const auto number = Resolve(path);
            if (!number) return std::nullopt;
            const auto inode = ReadInode(*number);
            if (!inode.IsRegular()) return std::nullopt;
            return ReadData(inode);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    std::vector<std::string> ListNames(const std::string& path) const
    {
        std::vector<std::string> names;
        try {
            const auto number = Resolve(path);
            if (!number) return names;
            const auto inode = ReadInode(*number);
            if (!inode.IsDirectory()) return names;
            for (const auto& entry : List(inode)) names.push_back(entry.name);
        } catch (const std::exception&) {
        }
        return names;
    }

private:
    void CopyBlocks(std::uint64_t physical, std::uint64_t logical, std::uint64_t count, Bytes& data) const
    {
        const auto start = logical * blockSize_;
        if (start >= data.size()) return;
        const auto bytes = (std::min<std::uint64_t>)(count * blockSize_, data.size() - start);
        disk_.Read(base_ + physical * blockSize_, data.data() + start, static_cast<std::size_t>(bytes));
    }

    void ReadExtentNode(const std::uint8_t* node, std::size_t size, Bytes& data, int depthGuard) const
    {
        if (depthGuard > 8 || size < 12 || Load<std::uint16_t>(node) != 0xF30A) throw std::runtime_error("extent invalide");
        const auto entries = Load<std::uint16_t>(node + 2);
        const auto depth = Load<std::uint16_t>(node + 6);
        for (std::uint32_t index = 0; index < entries && 12 + (index + 1) * 12 <= size; ++index) {
            const auto* entry = node + 12 + index * 12;
            if (depth == 0) {
                const auto logical = Load<std::uint32_t>(entry);
                auto length = static_cast<std::uint32_t>(Load<std::uint16_t>(entry + 4));
                const bool uninitialized = length > 32768;
                if (uninitialized) length -= 32768;
                const auto physical = (static_cast<std::uint64_t>(Load<std::uint16_t>(entry + 6)) << 32) |
                                      Load<std::uint32_t>(entry + 8);
                if (!uninitialized) CopyBlocks(physical, logical, length, data);
            } else {
                const auto leaf = Load<std::uint32_t>(entry + 4) |
                                  (static_cast<std::uint64_t>(Load<std::uint16_t>(entry + 8)) << 32);
                Bytes child(blockSize_);
                disk_.Read(base_ + leaf * blockSize_, child.data(), child.size());
                ReadExtentNode(child.data(), child.size(), data, depthGuard + 1);
            }
        }
    }

    void ReadBlockMap(const Inode& inode, Bytes& data) const
    {
        const auto perBlock = blockSize_ / 4;
        const auto blocks = (data.size() + blockSize_ - 1) / blockSize_;
        std::uint64_t logical = 0;
        std::function<void(std::uint32_t, int)> walk = [&](std::uint32_t block, int level) {
            if (logical >= blocks) return;
            if (level == 0) {
                if (block != 0) CopyBlocks(block, logical, 1, data);
                ++logical;
                return;
            }
            if (block == 0) {
                std::uint64_t span = 1;
                for (int step = 0; step < level; ++step) span *= perBlock;
                logical += span;
                return;
            }
            std::vector<std::uint32_t> pointers(perBlock);
            disk_.Read(base_ + static_cast<std::uint64_t>(block) * blockSize_, pointers.data(), blockSize_);
            for (const auto pointer : pointers) {
                if (logical >= blocks) return;
                walk(pointer, level - 1);
            }
        };
        for (int index = 0; index < 15 && logical < blocks; ++index) {
            const auto block = Load<std::uint32_t>(inode.block.data() + index * 4);
            walk(block, index < 12 ? 0 : index - 11);
        }
    }

    const DiskReader& disk_;
    std::uint64_t base_;
    std::uint32_t blockSize_ = 1024;
    std::uint32_t firstDataBlock_ = 0;
    std::uint32_t inodesPerGroup_ = 0;
    std::uint32_t inodeSize_ = 128;
    std::uint32_t incompat_ = 0;
    std::uint32_t descriptorSize_ = 32;
    bool is64Bit_ = false;
    std::array<std::uint8_t, 16> uuid_{};
};

// --- Partition tables ---------------------------------------------------------------

std::vector<std::uint64_t> FindPartitions(const DiskReader& disk)
{
    std::vector<std::uint64_t> offsets;
    std::array<std::uint8_t, 512> mbr{};
    disk.Read(0, mbr.data(), mbr.size());
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        offsets.push_back(0); // Unpartitioned file system.
        return offsets;
    }
    const auto* entries = &mbr[0x1BE];
    if (entries[4] == 0xEE) {
        std::array<std::uint8_t, 512> header{};
        disk.Read(512, header.data(), header.size());
        if (std::memcmp(header.data(), "EFI PART", 8) == 0) {
            const auto entryLba = Load<std::uint64_t>(&header[0x48]);
            const auto count = (std::min<std::uint32_t>)(Load<std::uint32_t>(&header[0x50]), 256);
            const auto entrySize = Load<std::uint32_t>(&header[0x54]);
            if (entrySize >= 128 && entrySize <= 4096) {
                Bytes table(static_cast<std::size_t>(count) * entrySize);
                disk.Read(entryLba * 512, table.data(), table.size());
                for (std::uint32_t index = 0; index < count; ++index) {
                    const auto* entry = &table[static_cast<std::size_t>(index) * entrySize];
                    bool used = false;
                    for (int byte = 0; byte < 16; ++byte) used |= entry[byte] != 0;
                    if (used) offsets.push_back(Load<std::uint64_t>(entry + 0x20) * 512);
                }
            }
        }
        return offsets;
    }
    for (int index = 0; index < 4; ++index) {
        const auto* entry = entries + index * 16;
        const auto type = entry[4];
        const auto start = static_cast<std::uint64_t>(Load<std::uint32_t>(entry + 8));
        if (type == 0 || start == 0) continue;
        if (type == 0x05 || type == 0x0F || type == 0x85) {
            std::uint64_t ebr = start;
            for (int guard = 0; guard < 64 && ebr != 0; ++guard) {
                std::array<std::uint8_t, 512> record{};
                disk.Read(ebr * 512, record.data(), record.size());
                if (record[510] != 0x55 || record[511] != 0xAA) break;
                const auto* logical = &record[0x1BE];
                const auto logicalStart = Load<std::uint32_t>(logical + 8);
                if (logical[4] != 0 && logicalStart != 0) offsets.push_back((ebr + logicalStart) * 512);
                const auto next = Load<std::uint32_t>(logical + 16 + 8);
                ebr = logical[16 + 4] != 0 && next != 0 ? start + next : 0;
            }
            continue;
        }
        offsets.push_back(start * 512);
    }
    return offsets;
}

// --- Boot configuration parsing -------------------------------------------------------

std::string Trim(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string StripGrubDevice(std::string path)
{
    if (!path.empty() && path[0] == '(') {
        const auto close = path.find(')');
        if (close != std::string::npos) path = path.substr(close + 1);
    }
    return path;
}

struct KernelEntry {
    std::string kernel;
    std::string initrd;
    std::string arguments;
};

std::optional<KernelEntry> ParseGrubConfig(const std::string& text)
{
    std::istringstream stream(text);
    std::string line;
    bool inEntry = false;
    KernelEntry entry;
    while (std::getline(stream, line)) {
        const auto trimmed = Trim(line);
        if (trimmed.rfind("menuentry ", 0) == 0) {
            if (inEntry && !entry.kernel.empty()) break;
            inEntry = true;
            entry = {};
            continue;
        }
        if (!inEntry) continue;
        std::istringstream words(trimmed);
        std::string command;
        words >> command;
        if (command == "linux" || command == "linux16" || command == "linuxefi") {
            std::string path;
            words >> path;
            entry.kernel = StripGrubDevice(path);
            std::string word;
            while (words >> word) {
                if (word.find('$') != std::string::npos) continue; // GRUB variables: $vt_handoff, ${extra_cmdline}, iso-scan/filename=${iso_path}.
                if (!entry.arguments.empty()) entry.arguments.push_back(' ');
                entry.arguments += word;
            }
        } else if ((command == "initrd" || command == "initrd16" || command == "initrdefi") && entry.initrd.empty()) {
            std::string path;
            words >> path;
            entry.initrd = StripGrubDevice(path);
        } else if (trimmed == "}" && !entry.kernel.empty()) {
            break;
        }
    }
    if (entry.kernel.empty()) return std::nullopt;
    return entry;
}

std::optional<KernelEntry> ParseBlsEntries(const ExtFileSystem& fs, const std::string& directory)
{
    auto names = fs.ListNames(directory);
    std::sort(names.begin(), names.end(), std::greater<>());
    for (const auto& name : names) {
        if (name.size() < 6 || name.substr(name.size() - 5) != ".conf") continue;
        const auto data = fs.ReadFile(directory + "/" + name);
        if (!data) continue;
        std::istringstream stream(std::string(data->begin(), data->end()));
        std::string line;
        KernelEntry entry;
        while (std::getline(stream, line)) {
            std::istringstream words(Trim(line));
            std::string key;
            words >> key;
            std::string rest;
            std::getline(words, rest);
            rest = Trim(rest);
            if (key == "linux") entry.kernel = rest;
            else if (key == "initrd" && entry.initrd.empty()) entry.initrd = rest;
            else if (key == "options") entry.arguments = rest;
        }
        if (!entry.kernel.empty() && entry.arguments.find('$') == std::string::npos) return entry;
    }
    return std::nullopt;
}

} // namespace

// --- ISO boot ----------------------------------------------------------------------------

std::optional<BootSelection> FindIsoBoot(const virtualiseur::whp::Iso9660Image& iso, bool graphicalInstaller)
{
    struct Profile {
        const char* kernel;
        const char* initrd;
        const char* arguments;
        const char* description;
        bool graphical;
    };
    // Debian first (its installer gets the preseed and the guest tools): a
    // live ISO also carries the installer, the live system wins.
    static const std::array<Profile, 4> debian{{
        {"live/vmlinuz", "live/initrd.img", "boot=live components", "Debian/Kali live", false},
        {"install.amd/gtk/vmlinuz", "install.amd/gtk/initrd.gz", "", "installateur Debian graphique", true},
        {"install.amd/vmlinuz", "install.amd/initrd.gz", "", "installateur Debian", false},
        {"install/vmlinuz", "install/initrd.gz", "", "installateur Debian", false},
    }};
    // Fallbacks when the ISO's own boot configuration cannot be used.
    static const std::array<Profile, 9> others{{
        {"casper/vmlinuz", "casper/initrd", "boot=casper", "Ubuntu live", false},
        {"casper/vmlinuz", "casper/initrd.gz", "boot=casper", "Ubuntu live", false},
        {"casper/vmlinuz", "casper/initrd.lz", "boot=casper", "Ubuntu live", false},
        {"arch/boot/x86_64/vmlinuz-linux", "arch/boot/x86_64/initramfs-linux.img",
         "archisobasedir=arch archisolabel={LABEL}", "Arch Linux", false},
        {"images/pxeboot/vmlinuz", "images/pxeboot/initrd.img",
         "root=live:CDLABEL={LABEL} rd.live.image inst.stage2=hd:LABEL={LABEL}", "Fedora/RHEL", false},
        {"boot/x86_64/loader/linux", "boot/x86_64/loader/initrd", "root=live:CDLABEL={LABEL} rd.live.image",
         "ISO kiwi (openSUSE, Fedora)", false},
        {"isolinux/vmlinuz", "isolinux/initrd.img", "", "ISO Linux générique", false},
        {"boot/vmlinuz", "boot/initrd.img", "", "ISO Linux générique", false},
    }};

    std::string label;
    for (const char character : iso.VolumeLabel()) {
        if (character == ' ') label += "\\x20";
        else label.push_back(character);
    }
    const auto select = [&](const Profile& profile) {
        BootSelection selection;
        selection.image.kernel = iso.ReadWholeFile(profile.kernel);
        selection.image.initrd = iso.ReadWholeFile(profile.initrd);
        std::string arguments = profile.arguments;
        for (std::size_t position; (position = arguments.find("{LABEL}")) != std::string::npos;) {
            arguments.replace(position, 7, label);
        }
        selection.image.commandLine = arguments;
        selection.description = std::string(profile.description) + " (" + profile.kernel + ")";
        return selection;
    };
    const auto present = [&](const Profile& profile) {
        return iso.ContainsFile(profile.kernel) && iso.ContainsFile(profile.initrd);
    };

    if (present(debian[0])) return select(debian[0]);
    for (int pass = 0; pass < 2; ++pass) {
        for (std::size_t index = 1; index < debian.size(); ++index) {
            // First pass honours the installer flavour requested by the user.
            if (pass == 0 && debian[index].graphical != graphicalInstaller) continue;
            if (present(debian[index])) return select(debian[index]);
        }
    }

    // Any other distribution: what its own GRUB menu would boot (first
    // entry with a kernel), as on a real PC.
    for (const char* config : {"boot/grub2/grub.cfg", "boot/grub/grub.cfg", "EFI/BOOT/grub.cfg"}) {
        if (!iso.ContainsFile(config)) continue;
        const auto data = iso.ReadWholeFile(config);
        const auto entry = ParseGrubConfig(std::string(data.begin(), data.end()));
        if (!entry || entry->initrd.empty() || !iso.ContainsFile(entry->kernel) || !iso.ContainsFile(entry->initrd)) continue;
        BootSelection selection;
        selection.image.kernel = iso.ReadWholeFile(entry->kernel);
        selection.image.initrd = iso.ReadWholeFile(entry->initrd);
        selection.image.commandLine = entry->arguments;
        selection.description = std::string("menu de démarrage de l'ISO (") + config + " : " + entry->kernel + ")";
        return selection;
    }

    for (const auto& profile : others) {
        if (present(profile)) return select(profile);
    }
    return std::nullopt;
}

// --- Disk boot -----------------------------------------------------------------------------

std::optional<BootSelection> FindDiskBoot(const virtualiseur::whp::VirtualDisk& disk)
{
    const DiskReader reader(disk);
    std::vector<std::uint64_t> partitions;
    try {
        partitions = FindPartitions(reader);
    } catch (const std::exception& error) {
        VMM_WARN("Disque: table de partitions illisible: %s", error.what());
        return std::nullopt;
    }

    for (const auto offset : partitions) {
        std::unique_ptr<ExtFileSystem> fs;
        try {
            fs = std::make_unique<ExtFileSystem>(reader, offset);
        } catch (const std::exception&) {
            continue;
        }

        std::optional<KernelEntry> entry;
        for (const char* directory : {"/boot/loader/entries", "/loader/entries"}) {
            if (!entry) entry = ParseBlsEntries(*fs, directory);
        }
        for (const char* path : {"/boot/grub/grub.cfg", "/grub/grub.cfg", "/boot/grub2/grub.cfg", "/grub2/grub.cfg"}) {
            if (entry) break;
            if (const auto config = fs->ReadFile(path)) {
                entry = ParseGrubConfig(std::string(config->begin(), config->end()));
            }
        }
        if (!entry) {
            for (const auto& [kernel, initrd] : std::array<std::pair<const char*, const char*>, 2>{{
                     {"/vmlinuz", "/initrd.img"}, {"/boot/vmlinuz", "/boot/initrd.img"}}}) {
                if (fs->Resolve(kernel)) {
                    entry = KernelEntry{kernel, initrd, "root=UUID=" + fs->Uuid() + " ro quiet"};
                    break;
                }
            }
        }
        if (!entry) continue;

        auto kernel = fs->ReadFile(entry->kernel);
        if (!kernel && entry->kernel.rfind("/boot/", 0) == 0) kernel = fs->ReadFile(entry->kernel.substr(5));
        if (!kernel) {
            VMM_WARN("Disque: noyau %s introuvable sur la partition.", entry->kernel.c_str());
            continue;
        }
        std::optional<Bytes> initrd;
        if (!entry->initrd.empty()) {
            initrd = fs->ReadFile(entry->initrd);
            if (!initrd && entry->initrd.rfind("/boot/", 0) == 0) initrd = fs->ReadFile(entry->initrd.substr(5));
        }
        BootSelection selection;
        selection.image.kernel = std::move(*kernel);
        if (initrd) selection.image.initrd = std::move(*initrd);
        selection.image.commandLine = entry->arguments;
        if (selection.image.commandLine.find("root=") == std::string::npos) {
            selection.image.commandLine = "root=UUID=" + fs->Uuid() + " ro " + selection.image.commandLine;
        }
        selection.description = "système installé sur le disque (" + entry->kernel + ")";
        return selection;
    }
    return std::nullopt;
}

} // namespace vmm
