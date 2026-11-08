#include "nnn/image.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <utility>

namespace nnn::image {
namespace {

constexpr std::size_t dwq_header_bytes = 64;
constexpr std::size_t bitmap_file_header_bytes = 14;
constexpr std::size_t bitmap_info_header_bytes = 40;
constexpr std::size_t palette_offset = dwq_header_bytes + bitmap_file_header_bytes + bitmap_info_header_bytes;
constexpr char legacy_dwq_header[] = "IF PACKTYPE==0  CUT THIS 64 BYTETHEN REMAKE BMP PACKTYPE=0      ";
static_assert(sizeof(legacy_dwq_header) - 1U == dwq_header_bytes);

ImageResult failure(ErrorCode code, std::size_t offset, const char* detail) {
    return {{code, offset, detail}, {}};
}

std::uint16_t u16(const std::vector<Byte>& bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset]) |
                                      static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

std::uint32_t u32(const std::vector<Byte>& bytes, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::int64_t signed32(std::uint32_t value) noexcept {
    // Converting out-of-range uint32_t directly to int32_t is implementation-defined.
    return value <= 0x7fffffffU ? static_cast<std::int64_t>(value)
                              : static_cast<std::int64_t>(value) - 0x100000000LL;
}

bool checked_add(std::size_t left, std::size_t right, std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) return false;
    result = left + right;
    return true;
}

bool checked_multiply(std::size_t left, std::size_t right, std::size_t& result) noexcept {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) return false;
    result = left * right;
    return true;
}

template <std::size_t Size>
bool matches(const std::vector<Byte>& bytes, std::size_t offset, const char (&text)[Size]) noexcept {
    for (std::size_t index = 0; index + 1U < Size; ++index) {
        if (bytes[offset + index] != static_cast<Byte>(text[index])) return false;
    }
    return true;
}

bool spaces(const std::vector<Byte>& bytes, std::size_t begin, std::size_t end) noexcept {
    for (std::size_t index = begin; index < end; ++index) {
        if (bytes[index] != static_cast<Byte>(' ')) return false;
    }
    return true;
}

ImageResult decode(const std::vector<Byte>& bytes, BitmapSizeFields sizes) {
    if (bytes.size() > max_input_bytes) return failure(ErrorCode::LimitExceeded, no_byte, "DWQ input exceeds byte limit");
    if (bytes.size() < dwq_header_bytes) return failure(ErrorCode::InvalidFormat, bytes.size(), "Truncated 64-byte DWQ header");
    const bool canonical = matches(bytes, 0, "BMP             ");
    if (!canonical && !matches(bytes, 0, legacy_dwq_header)) {
        return failure(ErrorCode::UnsupportedFormat, 0, "Unrecognized BMP DWQ prefix");
    }
    if (canonical && (!spaces(bytes, 16, 32) || !spaces(bytes, 44, 48))) {
        return failure(ErrorCode::UnsupportedFormat, 16, "Noncanonical DWQ reserved fields are unsupported");
    }
    if (!matches(bytes, 48, "PACKTYPE=0      ")) {
        return failure(ErrorCode::UnsupportedFormat, 48, "Only unmasked PACKTYPE=0 is supported");
    }
    // DWQ bytes 32..35 are opaque. The type-0 writer does not store a size there.
    if (bytes.size() < palette_offset) return failure(ErrorCode::InvalidFormat, bytes.size(), "Truncated bitmap headers");
    if (!matches(bytes, 64, "BM")) return failure(ErrorCode::UnsupportedFormat, 64, "Embedded bitmap signature is unsupported");
    if (u16(bytes, 70) != 0 || u16(bytes, 72) != 0) {
        return failure(ErrorCode::InvalidFormat, 70, "Bitmap reserved words must be zero");
    }
    if (u32(bytes, 78) != bitmap_info_header_bytes) {
        return failure(ErrorCode::UnsupportedFormat, 78, "Only a 40-byte bitmap info header is supported");
    }
    if (u16(bytes, 90) != 1) return failure(ErrorCode::InvalidFormat, 90, "Bitmap planes must equal one");
    const auto bit_depth = u16(bytes, 92);
    if (bit_depth != 8 && bit_depth != 24) return failure(ErrorCode::UnsupportedFormat, 92, "Only 8-bit and 24-bit DWQ data is supported");
    if (u32(bytes, 94) != 0) return failure(ErrorCode::UnsupportedFormat, 94, "Compressed bitmap data is unsupported");

    const auto bitmap_width = signed32(u32(bytes, 82));
    const auto bitmap_height = signed32(u32(bytes, 86));
    const auto height_magnitude = bitmap_height < 0 ? -bitmap_height : bitmap_height;
    // The legacy prefix contains only text. Its canvas comes from the DIB.
    const auto dwq_width = canonical ? signed32(u32(bytes, 36)) : bitmap_width;
    const auto dwq_height = canonical ? signed32(u32(bytes, 40)) : height_magnitude;
    if (bitmap_width <= 0 || bitmap_height == 0 || dwq_width <= 0 || dwq_height <= 0) {
        return failure(ErrorCode::InvalidFormat, canonical ? 36U : 82U,
                       "Widths and DWQ height must be positive; bitmap height must be nonzero");
    }
    if (bitmap_width > max_dimension || height_magnitude > max_dimension ||
        dwq_width > max_dimension || dwq_height > max_dimension) {
        return failure(ErrorCode::LimitExceeded, 82, "Image dimension exceeds limit");
    }
    if (dwq_width != bitmap_width || dwq_height != height_magnitude) {
        return failure(ErrorCode::InvalidFormat, 36, "DWQ and bitmap dimensions disagree");
    }
    const auto width = static_cast<std::size_t>(bitmap_width);
    const auto height = static_cast<std::size_t>(height_magnitude);
    std::size_t pixels = 0;
    if (!checked_multiply(width, height, pixels) || pixels > max_pixels) {
        return failure(ErrorCode::LimitExceeded, 82, "Image pixel count exceeds limit");
    }

    const auto colors_used = u32(bytes, 110);
    const auto colors_important = u32(bytes, 114);
    std::size_t palette_count = 0;
    if (bit_depth == 8) {
        if (colors_used > 256) return failure(ErrorCode::InvalidFormat, 110, "Palette count exceeds 256 entries");
        palette_count = colors_used == 0 ? 256U : static_cast<std::size_t>(colors_used);
        if (colors_important > palette_count) return failure(ErrorCode::InvalidFormat, 114, "Important colors exceed palette count");
    } else if (colors_used != 0 || colors_important != 0) {
        return failure(ErrorCode::UnsupportedFormat, 110, "Truecolor palette tables are unsupported");
    }

    std::size_t palette_bytes = 0;
    std::size_t row_bytes = 0;
    std::size_t rounded_row_bytes = 0;
    std::size_t row_payload = 0;
    std::size_t rgb_bytes = 0;
    std::size_t bitmap_pixel_offset = 0;
    std::size_t bitmap_bytes = 0;
    std::size_t total_bytes = 0;
    if (!checked_multiply(palette_count, 4, palette_bytes) ||
        !checked_multiply(width, bit_depth / 8U, row_bytes) ||
        !checked_add(row_bytes, 3, rounded_row_bytes)) {
        return failure(ErrorCode::LimitExceeded, 82, "Image row arithmetic exceeds addressable size");
    }
    const auto row_stride = rounded_row_bytes & ~static_cast<std::size_t>(3);
    if (!checked_multiply(row_stride, height, row_payload) ||
        !checked_multiply(pixels, 3, rgb_bytes) ||
        !checked_add(bitmap_file_header_bytes + bitmap_info_header_bytes, palette_bytes, bitmap_pixel_offset) ||
        !checked_add(bitmap_pixel_offset, row_payload, bitmap_bytes) ||
        !checked_add(dwq_header_bytes, bitmap_bytes, total_bytes) || total_bytes > max_input_bytes) {
        return failure(ErrorCode::LimitExceeded, 82, "Image size arithmetic exceeds resource limit");
    }
    if (u32(bytes, 74) != bitmap_pixel_offset) {
        return failure(ErrorCode::InvalidFormat, 74, "Bitmap pixel offset must follow the contiguous palette");
    }
    if (sizes == BitmapSizeFields::Exact) {
        if (u32(bytes, 66) != bitmap_bytes) {
            return failure(ErrorCode::InvalidFormat, 66, "Bitmap file size disagrees with computed layout");
        }
        const auto declared_row_payload = u32(bytes, 98);
        if (declared_row_payload != 0 && declared_row_payload != row_payload) {
            return failure(ErrorCode::InvalidFormat, 98, "Bitmap image size disagrees with padded rows");
        }
    }
    if (bytes.size() != total_bytes) {
        return failure(ErrorCode::InvalidFormat, std::min(bytes.size(), total_bytes),
                       bytes.size() < total_bytes ? "Truncated palette or pixel rows" : "Trailing DWQ payload is unsupported by this profile");
    }

    Image image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.source_bit_depth = bit_depth;
    image.rgb.resize(rgb_bytes);
    const auto data_offset = dwq_header_bytes + bitmap_pixel_offset;
    for (std::size_t y = 0; y < height; ++y) {
        const auto source_row = data_offset + y * row_stride;
        const auto output_row = y * width * 3U;
        if (bit_depth == 24) {
            // Type-0 writer already swaps BMP BGR to RGB and normalizes row order.
            std::copy_n(bytes.data() + source_row, row_bytes, image.rgb.data() + output_row);
        } else {
            for (std::size_t x = 0; x < width; ++x) {
                const auto palette_index = static_cast<std::size_t>(bytes[source_row + x]);
                if (palette_index >= palette_count) {
                    return failure(ErrorCode::InvalidFormat, source_row + x, "Pixel index exceeds declared palette");
                }
                const auto color = palette_offset + palette_index * 4U;
                const auto output = output_row + x * 3U;
                image.rgb[output] = bytes[color + 2U];
                image.rgb[output + 1U] = bytes[color + 1U];
                image.rgb[output + 2U] = bytes[color];
            }
        }
    }
    return {{}, std::move(image)};
}

} // namespace

const char* error_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::ReadFailed: return "ReadFailed";
    case ErrorCode::UnsupportedFormat: return "UnsupportedFormat";
    case ErrorCode::InvalidFormat: return "InvalidFormat";
    case ErrorCode::LimitExceeded: return "LimitExceeded";
    }
    return "Unknown";
}

ImageResult decode_dwq(const std::vector<Byte>& bytes, BitmapSizeFields sizes) {
    try {
        return decode(bytes, sizes);
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::LimitExceeded, no_byte, "Cannot allocate bounded image storage");
    } catch (const std::length_error&) {
        return failure(ErrorCode::LimitExceeded, no_byte, "Image storage exceeds container limit");
    }
}

ImageResult load_dwq(const std::string& filename) {
    try {
        namespace fs = std::filesystem;
        const fs::path path(filename);
        std::error_code error;
        const auto state = fs::status(path, error);
        if (error) {
            const bool missing = error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory;
            return failure(missing ? ErrorCode::NotFound : ErrorCode::ReadFailed, no_byte, "Cannot inspect DWQ file");
        }
        if (!fs::exists(state)) return failure(ErrorCode::NotFound, no_byte, "DWQ file was not found");
        if (!fs::is_regular_file(state)) return failure(ErrorCode::ReadFailed, no_byte, "DWQ input must be an ordinary file");
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return failure(ErrorCode::ReadFailed, no_byte, "Cannot open DWQ file");
        const auto end = file.tellg();
        if (end < std::ifstream::pos_type(0)) return failure(ErrorCode::ReadFailed, no_byte, "Cannot determine DWQ file length");
        const auto length = static_cast<std::uint64_t>(static_cast<std::streamoff>(end));
        if (length > max_input_bytes) return failure(ErrorCode::LimitExceeded, no_byte, "DWQ input exceeds byte limit");
        std::vector<Byte> bytes(static_cast<std::size_t>(length));
        file.seekg(0, std::ios::beg);
        if (!file) return failure(ErrorCode::ReadFailed, no_byte, "Cannot seek DWQ file");
        if (!bytes.empty()) {
            const auto expected = static_cast<std::streamsize>(bytes.size());
            file.read(reinterpret_cast<char*>(bytes.data()), expected);
            if (!file || file.gcount() != expected) return failure(ErrorCode::ReadFailed, no_byte, "DWQ file read was incomplete");
        }
        if (file.peek() != std::ifstream::traits_type::eof() || !file.eof() || file.bad()) {
            return failure(ErrorCode::ReadFailed, no_byte, "DWQ file length changed or final read failed");
        }
        return decode_dwq(bytes);
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::LimitExceeded, no_byte, "Cannot allocate bounded DWQ input storage");
    } catch (const std::length_error&) {
        return failure(ErrorCode::LimitExceeded, no_byte, "DWQ input exceeds container limit");
    } catch (const std::filesystem::filesystem_error&) {
        return failure(ErrorCode::ReadFailed, no_byte, "DWQ file path or metadata could not be read");
    }
}

} // namespace nnn::image
