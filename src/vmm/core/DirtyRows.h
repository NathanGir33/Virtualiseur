#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vmm {

// Changed rows of a screen, as up to kMaxBands disjoint bands [top, bottom)
// sorted from top to bottom. Two distant updates (a clock at the top, a
// cursor at the bottom) stay two bands instead of one range spanning the
// whole screen. When the bands run out, the two closest are merged.
class DirtyRows final {
public:
    static constexpr std::size_t kMaxBands = 8;

    struct Band {
        std::uint32_t top;
        std::uint32_t bottom;
    };

    void Clear() noexcept { count_ = 0; }
    bool Empty() const noexcept { return count_ == 0; }
    std::span<const Band> Bands() const noexcept { return {bands_.data(), count_}; }

    void Add(std::uint32_t top, std::uint32_t bottom) noexcept
    {
        if (bottom <= top) return;
        // Absorb every band that overlaps or touches [top, bottom).
        std::size_t kept = 0;
        for (std::size_t index = 0; index < count_; ++index) {
            const auto band = bands_[index];
            if (band.bottom >= top && band.top <= bottom) {
                if (band.top < top) top = band.top;
                if (band.bottom > bottom) bottom = band.bottom;
            } else {
                bands_[kept++] = band;
            }
        }
        // Insert in order; the array has one spare slot for this.
        std::size_t at = kept;
        while (at > 0 && bands_[at - 1].top > top) {
            bands_[at] = bands_[at - 1];
            --at;
        }
        bands_[at] = {top, bottom};
        count_ = kept + 1;
        if (count_ > kMaxBands) MergeClosest();
    }

    void Add(const DirtyRows& other) noexcept
    {
        for (const auto& band : other.Bands()) Add(band.top, band.bottom);
    }

    // Keeps the bands inside [0, height).
    void Clip(std::uint32_t height) noexcept
    {
        std::size_t kept = 0;
        for (std::size_t index = 0; index < count_; ++index) {
            auto band = bands_[index];
            if (band.top >= height) continue;
            if (band.bottom > height) band.bottom = height;
            bands_[kept++] = band;
        }
        count_ = kept;
    }

private:
    // Adjacent bands are disjoint and sorted: joining two neighbours covers
    // only the gap between them.
    void MergeClosest() noexcept
    {
        std::size_t best = 0;
        std::uint32_t bestGap = UINT32_MAX;
        for (std::size_t index = 0; index + 1 < count_; ++index) {
            const auto gap = bands_[index + 1].top - bands_[index].bottom;
            if (gap < bestGap) {
                bestGap = gap;
                best = index;
            }
        }
        bands_[best].bottom = bands_[best + 1].bottom;
        for (std::size_t index = best + 1; index + 1 < count_; ++index) bands_[index] = bands_[index + 1];
        --count_;
    }

    std::array<Band, kMaxBands + 1> bands_{};
    std::size_t count_ = 0;
};

} // namespace vmm
