#include "emberfall/render.hpp"
#include <algorithm>
#include <cstddef>

namespace emberfall::render {
namespace {
bool dimensions(std::uint32_t width, std::uint32_t height, std::size_t bytes, unsigned channels) noexcept {
    return width > 0 && height > 0 && width <= max_dimension && height <= max_dimension
        && std::uint64_t{width} * height <= max_pixels
        && std::uint64_t{width} * height * channels == bytes;
}
}

Status blit(const RgbaImage& source, Surface& destination, Rect rect, Point origin, BlendProfile profile) noexcept {
    if (profile != BlendProfile::Source32Mmx && profile != BlendProfile::Source64Cpp) return Status::InvalidProfile;
    if (!dimensions(source.width, source.height, source.rgba.size(), 4)
        || !dimensions(destination.width, destination.height, destination.rgb.size(), 3)) return Status::InvalidImage;
    if (rect.x < 0 || rect.y < 0 || rect.width < 0 || rect.height < 0
        || std::uint64_t{static_cast<std::uint32_t>(rect.x)} + static_cast<std::uint32_t>(rect.width) > source.width
        || std::uint64_t{static_cast<std::uint32_t>(rect.y)} + static_cast<std::uint32_t>(rect.height) > source.height) return Status::InvalidRectangle;
    const std::int64_t dx = origin.x, dy = origin.y;
    const std::int64_t first_x = std::max<std::int64_t>(0, -dx);
    const std::int64_t first_y = std::max<std::int64_t>(0, -dy);
    const std::int64_t after_x = std::min<std::int64_t>(rect.width, std::int64_t{destination.width} - dx);
    const std::int64_t after_y = std::min<std::int64_t>(rect.height, std::int64_t{destination.height} - dy);
    for (std::int64_t y = first_y; y < after_y; ++y) {
        for (std::int64_t x = first_x; x < after_x; ++x) {
            const auto si = static_cast<std::size_t>(((std::int64_t{rect.y} + y) * source.width + rect.x + x) * 4);
            const auto di = static_cast<std::size_t>(((dy + y) * destination.width + dx + x) * 3);
            const unsigned alpha = source.rgba[si + 3];
            if (alpha == 0) continue;
            for (unsigned channel = 0; channel < 3; ++channel) {
                const unsigned src = source.rgba[si + channel];
                const unsigned dst = destination.rgb[di + channel];
                unsigned mixed;
                if (alpha == 255) mixed = src;
                else if (profile == BlendProfile::Source32Mmx) mixed = ((src * alpha) >> 8U) + ((dst * (256U - alpha)) >> 8U);
                else mixed = (src * alpha + dst * (256U - alpha)) >> 8U;
                destination.rgb[di + channel] = static_cast<Byte>(mixed);
            }
        }
    }
    return Status::Ok;
}

} // namespace emberfall::render
