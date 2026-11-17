#include "GuestMemory.h"

#include <algorithm>
#include <stdexcept>

namespace vmm {

GuestMemory::GuestMemory(virtualiseur::whp::Partition& partition,
                         std::uint64_t ramBytes,
                         std::uint64_t framebufferBytes)
    : partition_(partition), ramBytes_(ramBytes)
{
    using namespace layout;
    constexpr std::uint64_t page = 4096;
    if (ramBytes < 128ull * 1024 * 1024 || (ramBytes % (2ull * 1024 * 1024)) != 0) {
        throw std::invalid_argument("La RAM invitée doit faire au moins 128 MiB, par pas de 2 MiB.");
    }

    lowRamEnd_ = (std::min)(ramBytes, kPciHoleStart);
    highRamBytes_ = ramBytes > kPciHoleStart ? ramBytes - kPciHoleStart : 0;

    (void)partition_.AllocateAndMapGuestMemory(0, kLegacyWindowStart);
    (void)partition_.AllocateAndMapGuestMemory(kLegacyWindowStart, kLegacyWindowSize);
    (void)partition_.AllocateAndMapGuestMemory(kHighMemoryStart, lowRamEnd_ - kHighMemoryStart);
    if (highRamBytes_ != 0) {
        (void)partition_.AllocateAndMapGuestMemory(kFourGiB, highRamBytes_);
    }
    if (framebufferBytes != 0) {
        framebufferBytes_ = (framebufferBytes + page - 1) & ~(page - 1);
        framebuffer_ = static_cast<std::uint8_t*>(
            partition_.AllocateAndMapGuestMemory(kFramebufferBase, framebufferBytes_, true));
    }

    // Keep the top page of conventional memory reserved, like the EBDA of a
    // BIOS machine. The kernel uses low RAM for its real-mode SMP trampoline.
    e820_.push_back({0, 0x9F000, 1});
    e820_.push_back({0x9F000, 0x100000 - 0x9F000, 2});
    e820_.push_back({kHighMemoryStart, lowRamEnd_ - kHighMemoryStart, 1});
    // The PCI hole is left as a gap so Linux can allocate PCI resources there,
    // except for the fixed system MMIO ranges.
    e820_.push_back({kIoApicBase, 0x1000, 2});
    e820_.push_back({kLocalApicBase, 0x1000, 2});
    if (highRamBytes_ != 0) e820_.push_back({kFourGiB, highRamBytes_, 1});
}

void GuestMemory::FramebufferDirtyRows(std::uint32_t stride, std::uint32_t height, DirtyRows& rows) const
{
    if (framebuffer_ == nullptr || stride == 0) return;
    std::vector<std::uint64_t> bitmap;
    if (!partition_.QueryDirtyPages(layout::kFramebufferBase, framebufferBytes_, bitmap)) {
        rows.Add(0, height);
        return;
    }
    const auto addRun = [&](std::uint64_t firstPage, std::uint64_t endPage) {
        const auto first = firstPage * 4096;
        const auto last = endPage * 4096;
        rows.Add(static_cast<std::uint32_t>(first / stride),
                 static_cast<std::uint32_t>((std::min<std::uint64_t>)(height, (last + stride - 1) / stride)));
    };
    // Runs of consecutive dirty pages become bands.
    std::uint64_t runStart = 0;
    bool inRun = false;
    const auto pages = static_cast<std::uint64_t>(bitmap.size()) * 64;
    for (std::uint64_t page = 0; page < pages; ++page) {
        const auto word = bitmap[static_cast<std::size_t>(page / 64)];
        if (word == 0 && !inRun) {
            page += 63 - (page % 64); // Skip clean words quickly.
            continue;
        }
        const bool dirty = (word & (1ull << (page % 64))) != 0;
        if (dirty && !inRun) {
            runStart = page;
            inRun = true;
        } else if (!dirty && inRun) {
            addRun(runStart, page);
            inRun = false;
        }
    }
    if (inRun) addRun(runStart, pages);
    rows.Clip(height);
}

bool GuestMemory::Read(std::uint64_t gpa, void* destination, std::size_t size) const noexcept
{
    auto* output = static_cast<std::uint8_t*>(destination);
    while (size != 0) {
        const std::size_t chunk = (std::min<std::size_t>)(size, 4096 - (gpa & 4095));
        const auto* source = Pointer(gpa, chunk);
        if (source == nullptr) return false;
        std::memcpy(output, source, chunk);
        output += chunk;
        gpa += chunk;
        size -= chunk;
    }
    return true;
}

bool GuestMemory::Write(std::uint64_t gpa, const void* source, std::size_t size) const noexcept
{
    const auto* input = static_cast<const std::uint8_t*>(source);
    while (size != 0) {
        const std::size_t chunk = (std::min<std::size_t>)(size, 4096 - (gpa & 4095));
        auto* destination = Pointer(gpa, chunk);
        if (destination == nullptr) return false;
        std::memcpy(destination, input, chunk);
        input += chunk;
        gpa += chunk;
        size -= chunk;
    }
    return true;
}

} // namespace vmm
