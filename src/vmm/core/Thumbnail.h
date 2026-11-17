#pragma once

#include <cstdint>
#include <filesystem>

namespace vmm {

// Writes a PNG of the screen `pixels` (BGRX, `stride` bytes per row), scaled
// down to at most `maxWidth` pixels wide (box filter). The image is written
// to `<path>.tmp` then renamed over `path` (MoveFileExW REPLACE_EXISTING), so
// a reader never sees a half-written file. Returns false on failure.
bool WritePngThumbnail(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::size_t stride,
                       const std::filesystem::path& path, std::uint32_t maxWidth);

} // namespace vmm
