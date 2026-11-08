#pragma once
#include <cstdint>
#include <vector>

namespace emberfall::render {

using Byte = std::uint8_t;
inline constexpr std::uint32_t max_dimension = 8192;
inline constexpr std::uint64_t max_pixels = 16U * 1024U * 1024U;

// Both match specific upstream source branches, not Android SRC_OVER.
// This enum does not claim the supplied game's binary matches that source.
enum class BlendProfile { Source32Mmx, Source64Cpp };
enum class Status { Ok, InvalidImage, InvalidRectangle, InvalidProfile };
struct Point { std::int32_t x = 0, y = 0; };
struct Rect { std::int32_t x = 0, y = 0, width = 0, height = 0; };
struct RgbaImage {
    std::uint32_t width = 0, height = 0;
    // Full canvas, top-left, straight RGBA. No color conversion or cut margins.
    std::vector<Byte> rgba;
};
struct Surface {
    std::uint32_t width = 0, height = 0;
    // Opaque top-left RGB surface. The native screen's unused high byte is not
    // interpreted as transparency. Presentation adds alpha255, if needed.
    std::vector<Byte> rgb;
};

// All validation finishes before writes. Source rectangle must be inside its
// image; destination edges clip it. Zero area is valid. No scaling or aliasing.
// Callers must serialize access to the destination and source image.
Status blit(const RgbaImage& source, Surface& destination, Rect source_rect,
            Point destination_origin, BlendProfile profile) noexcept;

bool supports_wipe(int type) noexcept;
void draw_wipe(Surface& output,const Surface& before,const Surface& after,int type,
               std::uint32_t frame,std::uint32_t frames);

} // namespace emberfall::render
