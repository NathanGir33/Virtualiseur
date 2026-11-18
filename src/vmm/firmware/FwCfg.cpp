#include "FwCfg.h"

#include "../core/Log.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace vmm {
namespace {

constexpr std::uint16_t kFileDirectory = 0x19;
constexpr std::uint32_t kDmaError = 0x01;
constexpr std::uint32_t kDmaRead = 0x02;
constexpr std::uint32_t kDmaSkip = 0x04;
constexpr std::uint32_t kDmaSelect = 0x08;
constexpr std::uint32_t kDmaWrite = 0x10;
constexpr std::uint32_t kMaxDmaLength = 64u * 1024 * 1024;
constexpr char kDmaSignature[] = "QEMU CFG";

std::uint32_t BigEndian32(const std::uint8_t* bytes)
{
    return (static_cast<std::uint32_t>(bytes[0]) << 24) | (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
}

std::uint64_t BigEndian64(const std::uint8_t* bytes)
{
    return (static_cast<std::uint64_t>(BigEndian32(bytes)) << 32) | BigEndian32(bytes + 4);
}

void PutBigEndian32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

} // namespace

void FwCfg::AddItem(std::uint16_t selector, std::vector<std::uint8_t> data)
{
    items_[selector] = std::move(data);
}

void FwCfg::AddFile(const std::string& name, std::vector<std::uint8_t> data)
{
    const auto selector = nextFile_++;
    fileNames_[selector] = name;
    items_[selector] = std::move(data);
}

void FwCfg::Finalize()
{
    // Sorted by name, as QEMU does.
    std::vector<std::pair<std::string, std::uint16_t>> files;
    for (const auto& [selector, name] : fileNames_) files.emplace_back(name, selector);
    std::sort(files.begin(), files.end());
    std::vector<std::uint8_t> directory;
    PutBigEndian32(directory, static_cast<std::uint32_t>(files.size()));
    for (const auto& [name, selector] : files) {
        PutBigEndian32(directory, static_cast<std::uint32_t>(items_[selector].size()));
        directory.push_back(static_cast<std::uint8_t>(selector >> 8));
        directory.push_back(static_cast<std::uint8_t>(selector));
        directory.push_back(0);
        directory.push_back(0);
        char field[56]{};
        std::memcpy(field, name.data(), (std::min<std::size_t>)(name.size(), sizeof(field) - 1));
        directory.insert(directory.end(), field, field + sizeof(field));
    }
    items_[kFileDirectory] = std::move(directory);
}

void FwCfg::Select(std::uint16_t selector)
{
    // Bit 14 asks for the write channel (refused), bit 15 the architecture items.
    selector_ = static_cast<std::uint16_t>(selector & 0xBFFF);
    offset_ = 0;
    if (!reported_.insert(selector_).second) return;
    if (const auto file = fileNames_.find(selector_); file != fileNames_.end()) {
        VMM_INFO("fw_cfg : le firmware lit « %s ».", file->second.c_str());
    } else if (items_.find(selector_) == items_.end()) {
        VMM_DEBUG("fw_cfg : élément 0x%04x absent (lu comme vide).", selector_);
    }
}

std::uint8_t FwCfg::NextByte()
{
    const auto item = items_.find(selector_);
    std::uint8_t value = 0;
    if (item != items_.end() && offset_ < item->second.size()) value = item->second[offset_];
    ++offset_;
    return value;
}

std::uint32_t FwCfg::IoRead(std::uint16_t port, std::uint8_t size)
{
    if (port >= kDmaPort && port < kDmaPort + 8) {
        std::uint32_t value = 0;
        for (std::uint8_t index = 0; index < size && port - kDmaPort + index < 8; ++index) {
            value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(kDmaSignature[port - kDmaPort + index])) << (index * 8);
        }
        return value;
    }
    if (port == kDataPort) {
        std::uint32_t value = 0;
        for (std::uint8_t index = 0; index < size; ++index) value |= static_cast<std::uint32_t>(NextByte()) << (index * 8);
        return value;
    }
    return 0;
}

void FwCfg::IoWrite(std::uint16_t port, std::uint8_t size, std::uint32_t value)
{
    if (port == kSelectorPort) {
        Select(static_cast<std::uint16_t>(value));
        return;
    }
    if (port >= kDmaPort && port < kDmaPort + 8) {
        // The guest wrote a byte-swapped 32-bit value: the port bytes, in
        // order, are the big-endian address.
        const auto first = port - kDmaPort;
        for (std::uint8_t index = 0; index < size && first + index < 8; ++index) {
            dmaAddress_[first + index] = static_cast<std::uint8_t>(value >> (index * 8));
        }
        if (first + size == 8) {
            const auto address = BigEndian64(dmaAddress_);
            std::memset(dmaAddress_, 0, sizeof(dmaAddress_));
            RunDma(address);
        }
    }
    // Writes to the data port: deprecated, ignored.
}

void FwCfg::RunDma(std::uint64_t address)
{
    std::uint8_t header[16]{};
    if (!memory_.Read(address, header, sizeof(header))) {
        VMM_WARN("fw_cfg : descripteur DMA hors de la RAM (0x%llx).", static_cast<unsigned long long>(address));
        return; // Nowhere to report the error.
    }
    const auto control = BigEndian32(header);
    const auto length = BigEndian32(header + 4);
    const auto target = BigEndian64(header + 8);
    if ((control & kDmaSelect) != 0) Select(static_cast<std::uint16_t>(control >> 16));

    bool error = false;
    if ((control & kDmaRead) != 0) {
        if (length > kMaxDmaLength) {
            error = true;
        } else {
            // Page by page: RAM is several host allocations.
            std::vector<std::uint8_t> chunk;
            std::uint32_t done = 0;
            while (done < length && !error) {
                const auto at = target + done;
                const auto step = static_cast<std::uint32_t>(
                    (std::min<std::uint64_t>)(length - done, 4096 - (at & 4095)));
                chunk.resize(step);
                for (auto& byte : chunk) byte = NextByte();
                error = !memory_.Write(at, chunk.data(), step);
                done += step;
            }
        }
    } else if ((control & kDmaSkip) != 0) {
        offset_ += length;
    } else if ((control & kDmaWrite) != 0) {
        error = true; // Items are read-only.
    }

    // Completion: control written last, once the payload is in guest memory.
    std::atomic_thread_fence(std::memory_order_release);
    const std::uint8_t status[4]{0, 0, 0, static_cast<std::uint8_t>(error ? kDmaError : 0)};
    (void)memory_.Write(address, status, sizeof(status));
    if (error) {
        VMM_WARN("fw_cfg : transfert DMA refusé (contrôle 0x%08x, %u octets vers 0x%llx).", control, length,
                 static_cast<unsigned long long>(target));
    }
}

} // namespace vmm
