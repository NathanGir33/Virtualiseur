#include "Iso9660Image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace virtualiseur::whp {
namespace {

constexpr std::uint64_t kLogicalBlockSize = 2048;
constexpr std::uint64_t kFirstVolumeDescriptorLba = 16;
constexpr std::size_t kVolumeDescriptorSize = 2048;
constexpr std::size_t kRootDirectoryRecordOffset = 156;
constexpr std::uint8_t kDirectoryFlag = 0x02;
constexpr std::uint8_t kMultiExtentFlag = 0x80;

std::uint32_t ReadLittleEndian32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint32_t ReadBigEndian32(const std::uint8_t* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::uint32_t ReadBothEndian32(const std::uint8_t* bytes)
{
    const auto little = ReadLittleEndian32(bytes);
    const auto big = ReadBigEndian32(bytes + 4);
    if (little != big) {
        throw std::runtime_error("Champ ISO 9660 little/big endian incoherent.");
    }
    return little;
}

std::string NormalizeName(std::string name)
{
    if (const auto version = name.find(';'); version != std::string::npos) {
        name.resize(version);
    }
    if (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return name;
}

std::vector<std::string> SplitPath(const std::string& path)
{
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin < path.size()) {
        while (begin < path.size() && (path[begin] == '/' || path[begin] == '\\')) ++begin;
        const auto end = path.find_first_of("/\\", begin);
        if (end == std::string::npos) {
            if (begin < path.size()) result.push_back(NormalizeName(path.substr(begin)));
            break;
        }
        if (end > begin) result.push_back(NormalizeName(path.substr(begin, end - begin)));
        begin = end + 1;
    }
    return result;
}

} // namespace

Iso9660Image::Iso9660Image(const std::filesystem::path& imagePath)
    : imagePath_(imagePath), input_(imagePath, std::ios::binary)
{
    if (!input_) {
        throw std::runtime_error("Impossible d'ouvrir l'image ISO: " + imagePath.string());
    }
    input_.seekg(0, std::ios::end);
    const auto end = input_.tellg();
    if (end < static_cast<std::streamoff>((kFirstVolumeDescriptorLba + 1) * kLogicalBlockSize)) {
        throw std::runtime_error("Image trop courte pour contenir les descripteurs ISO 9660.");
    }
    imageSizeBytes_ = static_cast<std::uint64_t>(end);

    bool foundPrimary = false;
    std::array<std::uint8_t, kVolumeDescriptorSize> descriptor{};
    for (std::uint64_t index = 0; index < 64; ++index) {
        ReadAt((kFirstVolumeDescriptorLba + index) * kLogicalBlockSize,
               descriptor.data(), descriptor.size());
        if (descriptor[1] != 'C' || descriptor[2] != 'D' || descriptor[3] != '0' ||
            descriptor[4] != '0' || descriptor[5] != '1' || descriptor[6] != 1) {
            throw std::runtime_error("Descripteur ISO 9660 invalide.");
        }
        if (descriptor[0] == 1) {
            rootDirectory_ = ParseDirectoryRecord(
                descriptor.data() + kRootDirectoryRecordOffset,
                descriptor.size() - kRootDirectoryRecordOffset);
            if ((rootDirectory_.flags & kDirectoryFlag) == 0) {
                throw std::runtime_error("Le répertoire racine ISO 9660 est invalide.");
            }
            volumeLabel_.assign(reinterpret_cast<const char*>(descriptor.data() + 40), 32);
            while (!volumeLabel_.empty() && (volumeLabel_.back() == ' ' || volumeLabel_.back() == '\0')) {
                volumeLabel_.pop_back();
            }
            foundPrimary = true;
        }
        if (descriptor[0] == 255) break;
    }
    if (!foundPrimary) {
        throw std::runtime_error("L'image ne contient pas de descripteur de volume primaire ISO 9660.");
    }
}

bool Iso9660Image::ContainsFile(const std::string& isoRelativePath) const
{
    try {
        const auto entry = FindEntry(isoRelativePath);
        return (entry.flags & kDirectoryFlag) == 0;
    } catch (const std::runtime_error&) {
        return false;
    }
}

std::uint64_t Iso9660Image::SizeBytes() const noexcept
{
    return imageSizeBytes_;
}

void Iso9660Image::ReadLogicalBlocks(
    std::uint32_t firstBlock,
    std::span<std::byte> destination) const
{
    if ((destination.size() % LogicalBlockSize) != 0) {
        throw std::invalid_argument("Une lecture ISO doit contenir un nombre entier de blocs logiques.");
    }
    const auto offset = static_cast<std::uint64_t>(firstBlock) * LogicalBlockSize;
    ReadAt(offset, destination.data(), destination.size());
}

std::uint64_t Iso9660Image::ExtractFile(
    const std::string& isoRelativePath,
    const std::filesystem::path& destinationPath) const
{
    const auto entry = FindEntry(isoRelativePath);
    if ((entry.flags & kDirectoryFlag) != 0) {
        throw std::invalid_argument("Le chemin ISO demandé désigne un répertoire.");
    }
    if ((entry.flags & kMultiExtentFlag) != 0) {
        throw std::runtime_error("Les fichiers ISO multi-extent ne sont pas encore pris en charge.");
    }

    const auto offset = static_cast<std::uint64_t>(entry.extentLba) * kLogicalBlockSize;
    if (offset > imageSizeBytes_ || entry.sizeBytes > imageSizeBytes_ - offset) {
        throw std::runtime_error("Le fichier ISO dépasse les limites de l'image.");
    }
    if (!destinationPath.parent_path().empty()) {
        std::filesystem::create_directories(destinationPath.parent_path());
    }
    std::ofstream output(destinationPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Impossible de créer le fichier extrait: " + destinationPath.string());
    }

    constexpr std::size_t kChunkSize = 1024 * 1024;
    std::vector<std::uint8_t> buffer((std::min)(
        static_cast<std::size_t>(entry.sizeBytes), kChunkSize));
    std::uint64_t completed = 0;
    while (completed < entry.sizeBytes) {
        const auto chunk = static_cast<std::size_t>((std::min<std::uint64_t>)(
            entry.sizeBytes - completed, buffer.size()));
        ReadAt(offset + completed, buffer.data(), chunk);
        output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(chunk));
        if (!output) {
            throw std::runtime_error("Erreur d'écriture pendant l'extraction de l'ISO.");
        }
        completed += chunk;
    }
    return entry.sizeBytes;
}

std::vector<std::uint8_t> Iso9660Image::ReadWholeFile(const std::string& isoRelativePath) const
{
    const auto entry = FindEntry(isoRelativePath);
    if ((entry.flags & kDirectoryFlag) != 0) {
        throw std::invalid_argument("Le chemin ISO demandé désigne un répertoire.");
    }
    const auto offset = static_cast<std::uint64_t>(entry.extentLba) * kLogicalBlockSize;
    if (offset > imageSizeBytes_ || entry.sizeBytes > imageSizeBytes_ - offset) {
        throw std::runtime_error("Le fichier ISO dépasse les limites de l'image.");
    }
    std::vector<std::uint8_t> data(entry.sizeBytes);
    if (!data.empty()) ReadAt(offset, data.data(), data.size());
    return data;
}

Iso9660Image::Entry Iso9660Image::FindEntry(const std::string& isoRelativePath) const
{
    const auto components = SplitPath(isoRelativePath);
    if (components.empty()) {
        throw std::invalid_argument("Le chemin ISO est vide.");
    }

    Entry current = rootDirectory_;
    for (std::size_t index = 0; index < components.size(); ++index) {
        if ((current.flags & kDirectoryFlag) == 0) {
            throw std::runtime_error("Un composant du chemin ISO n'est pas un répertoire.");
        }
        current = FindInDirectory(current, components[index]);
        if (index + 1 < components.size() && (current.flags & kDirectoryFlag) == 0) {
            throw std::runtime_error("Un composant intermédiaire du chemin ISO n'est pas un répertoire.");
        }
    }
    return current;
}

Iso9660Image::Entry Iso9660Image::FindInDirectory(
    const Entry& directory,
    const std::string& component) const
{
    if (directory.sizeBytes > 64 * 1024 * 1024) {
        throw std::runtime_error("Répertoire ISO anormalement volumineux.");
    }
    const auto offset = static_cast<std::uint64_t>(directory.extentLba) * kLogicalBlockSize;
    if (offset > imageSizeBytes_ || directory.sizeBytes > imageSizeBytes_ - offset) {
        throw std::runtime_error("Répertoire ISO hors limites.");
    }

    std::vector<std::uint8_t> data(directory.sizeBytes);
    if (!data.empty()) ReadAt(offset, data.data(), data.size());
    std::size_t position = 0;
    while (position < data.size()) {
        const auto recordLength = data[position];
        if (recordLength == 0) {
            position = ((position / kLogicalBlockSize) + 1) * kLogicalBlockSize;
            continue;
        }
        if (recordLength > data.size() - position) {
            throw std::runtime_error("Enregistrement de répertoire ISO tronqué.");
        }
        const auto* record = data.data() + position;
        const auto recordNameLength = record[32];
        if (recordLength < 33 + recordNameLength) {
            throw std::runtime_error("Nom de fichier ISO tronqué.");
        }
        if (!(recordNameLength == 1 && (record[33] == 0 || record[33] == 1))) {
            const std::string name(reinterpret_cast<const char*>(record + 33), recordNameLength);
            // Rock Ridge keeps the real name ("vmlinuz-linux") where plain
            // ISO 9660 has a mangled one ("VMLINUZ_LINUX.;1"): try both.
            const auto rockRidge = RockRidgeName(record, recordLength);
            if (NormalizeName(name) == component || (!rockRidge.empty() && NormalizeName(rockRidge) == component)) {
                auto entry = ParseDirectoryRecord(record, recordLength);
                if ((entry.flags & kMultiExtentFlag) != 0) {
                    throw std::runtime_error("Les fichiers ISO multi-extent ne sont pas encore pris en charge.");
                }
                return entry;
            }
        }
        position += recordLength;
    }
    throw std::runtime_error("Fichier introuvable dans l'ISO: " + component);
}

void Iso9660Image::ReadAt(std::uint64_t offset, void* destination, std::size_t size) const
{
    if (offset > imageSizeBytes_ || size > imageSizeBytes_ - offset ||
        offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)()) ||
        size > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) {
        throw std::out_of_range("Lecture hors limites de l'image ISO.");
    }
    const std::lock_guard lock(inputMutex_);
    input_.clear();
    input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input_.read(static_cast<char*>(destination), static_cast<std::streamsize>(size))) {
        throw std::runtime_error("Lecture incomplète de l'image ISO.");
    }
}

std::string Iso9660Image::RockRidgeName(const std::uint8_t* record, std::size_t recordLength)
{
    // System Use area (SUSP): after the identifier and its padding byte.
    const std::size_t nameLength = record[32];
    std::size_t position = 33 + nameLength + ((nameLength % 2) == 0 ? 1 : 0);
    std::string name;
    while (position + 4 <= recordLength) {
        const auto* entry = record + position;
        const std::size_t length = entry[2];
        if (length < 4 || length > recordLength - position) break;
        if (entry[0] == 'S' && entry[1] == 'T') break; // SUSP terminator.
        if (entry[0] == 'N' && entry[1] == 'M' && length >= 5) {
            const auto flags = entry[4];
            if ((flags & 0x06) != 0) return {}; // "." or "..".
            name.append(reinterpret_cast<const char*>(entry + 5), length - 5);
            if ((flags & 0x01) == 0) return name; // No CONTINUE: complete.
        }
        position += length;
    }
    return name; // Continuation areas (CE) are not followed: empty or partial name is ignored by the caller.
}

Iso9660Image::Entry Iso9660Image::ParseDirectoryRecord(
    const std::uint8_t* record,
    std::size_t available)
{
    if (available < 34 || record[0] < 34 || record[0] > available) {
        throw std::runtime_error("Enregistrement ISO 9660 invalide.");
    }
    const auto recordLength = record[0];
    const auto identifierLength = record[32];
    if (recordLength < 33 + identifierLength) {
        throw std::runtime_error("Enregistrement ISO 9660 sans identifiant complet.");
    }
    Entry entry{};
    entry.extentLba = ReadBothEndian32(record + 2);
    entry.sizeBytes = ReadBothEndian32(record + 10);
    entry.flags = record[25];
    return entry;
}

} // namespace virtualiseur::whp
