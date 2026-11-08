#include "nnn/png.hpp"
#include "png_decode.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <utility>

namespace nnn::png {
namespace {

ImageResult failure(ErrorCode code, std::size_t offset, const char* detail) {
    return {{code, offset, detail}, {}};
}

std::uint32_t le32(const std::vector<Byte>& bytes, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint32_t be32(const std::vector<Byte>& bytes, std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
           static_cast<std::uint32_t>(bytes[offset + 3U]);
}

template <std::size_t Size>
bool matches(const std::vector<Byte>& bytes, std::size_t offset, const char (&text)[Size]) noexcept {
    for (std::size_t i = 0; i + 1U < Size; ++i) if (bytes[offset + i] != static_cast<Byte>(text[i])) return false;
    return true;
}

bool alphabetic(Byte byte) noexcept {
    return (byte >= static_cast<Byte>('A') && byte <= static_cast<Byte>('Z')) ||
           (byte >= static_cast<Byte>('a') && byte <= static_cast<Byte>('z'));
}

bool type_is(const std::vector<Byte>& bytes, std::size_t type, const char* name) noexcept {
    return std::memcmp(bytes.data() + type, name, 4U) == 0;
}

struct DataSpan { std::size_t offset; std::size_t size; };

struct Pass { std::size_t stride; std::size_t rows; };

struct Scanlines {
    std::array<Pass, 7> passes{};
    std::size_t count = 0;
    std::size_t size = 0;

    void add(std::size_t columns, std::size_t rows, std::size_t pixel_bits) {
        const auto stride = (columns * pixel_bits + 7U) / 8U + 1U;
        passes[count++] = {stride, rows};
        size += stride * rows;
    }
};

Scanlines scanlines(std::size_t width, std::size_t height, std::size_t pixel_bits, bool interlaced) {
    Scanlines result;
    if (!interlaced) {
        result.add(width, height, pixel_bits);
        return result;
    }
    struct Adam7 { std::size_t x, y, step_x, step_y; };
    constexpr std::array<Adam7, 7> grid{{
        {0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
        {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}
    }};
    for (const auto& pass : grid) {
        if (width <= pass.x || height <= pass.y) continue;
        result.add((width - pass.x + pass.step_x - 1U) / pass.step_x,
                   (height - pass.y + pass.step_y - 1U) / pass.step_y, pixel_bits);
    }
    return result;
}

class InflateState {
public:
    z_stream stream{};
    bool initialized = false;
    ~InflateState() { if (initialized) inflateEnd(&stream); }
    InflateState() = default;
    InflateState(const InflateState&) = delete;
    InflateState& operator=(const InflateState&) = delete;
};

Status validate_deflate(const std::vector<Byte>& bytes, const std::vector<DataSpan>& spans,
                        const Scanlines& layout) {
    InflateState state;
    const int started = inflateInit(&state.stream);
    if (started != Z_OK) return {ErrorCode::LimitExceeded, no_byte, "Cannot initialize bounded PNG inflate validator"};
    state.initialized = true;
    std::array<Byte, 32768> output{};
    std::size_t produced_total = 0;
    std::size_t next_filter = 0;
    std::size_t pass = 0;
    std::size_t rows = 0;
    bool ended = false;
    for (const auto& span : spans) {
        if (ended) {
            if (span.size != 0U) return {ErrorCode::InvalidFormat, span.offset, "Compressed bytes follow the PNG zlib stream"};
            continue;
        }
        state.stream.next_in = const_cast<Byte*>(bytes.data() + span.offset);
        state.stream.avail_in = static_cast<uInt>(span.size);
        for (;;) {
            state.stream.next_out = output.data();
            state.stream.avail_out = static_cast<uInt>(output.size());
            const auto previous_input = state.stream.avail_in;
            const int result = inflate(&state.stream, Z_NO_FLUSH);
            const auto produced = output.size() - static_cast<std::size_t>(state.stream.avail_out);
            if (produced > layout.size - produced_total) {
                return {ErrorCode::InvalidFormat, span.offset, "PNG inflate output exceeds the checked canvas"};
            }
            const auto after = produced_total + produced;
            while (next_filter < after) {
                if (output[next_filter - produced_total] > 4U) return {ErrorCode::InvalidFormat, span.offset, "PNG row uses an invalid filter byte"};
                const auto& current = layout.passes[pass];
                next_filter += current.stride;
                if (++rows == current.rows) {
                    ++pass;
                    rows = 0;
                }
            }
            produced_total = after;
            if (result == Z_STREAM_END) {
                if (state.stream.avail_in != 0U) return {ErrorCode::InvalidFormat, span.offset, "Trailing bytes or a second zlib stream in IDAT"};
                ended = true;
                break;
            }
            if (result == Z_MEM_ERROR) return {ErrorCode::LimitExceeded, span.offset, "PNG inflate validator allocation failed"};
            if (result == Z_BUF_ERROR && state.stream.avail_in == 0U && produced == 0U) break;
            if (result != Z_OK) return {ErrorCode::InvalidFormat, span.offset, "Invalid or truncated PNG zlib stream"};
            if (produced == 0U && previous_input == state.stream.avail_in) {
                return {ErrorCode::InvalidFormat, span.offset, "PNG zlib stream made no progress"};
            }
            if (state.stream.avail_in == 0U && state.stream.avail_out != 0U) break;
        }
    }
    if (!ended || produced_total != layout.size) return {ErrorCode::InvalidFormat, no_byte, "PNG zlib stream has no complete exact-size end"};
    return {};
}

bool keyword_end(const std::vector<Byte>& bytes, std::size_t begin, std::size_t end, std::size_t& terminator) {
    terminator = begin;
    while (terminator < end && bytes[terminator] != 0U && terminator - begin < 80U) ++terminator;
    return terminator > begin && terminator < end && bytes[terminator] == 0U && terminator - begin <= 79U;
}

std::size_t channels_for_color(Byte color) {
    switch (color) {
    case 0U:
    case 3U: return 1U;
    case 2U: return 3U;
    case 4U: return 2U;
    case 6U: return 4U;
    default: return 0U;
    }
}

bool depth_supported(Byte color, Byte depth) {
    if (depth == 8U) return true;
    if (depth == 16U) return color != 3U;
    return (color == 0U || color == 3U) && (depth == 1U || depth == 2U || depth == 4U);
}

bool transparency_valid(const std::vector<Byte>& bytes, std::size_t data, std::size_t length,
                        Byte color, Byte depth, std::size_t palette_size) {
    if (color == 3U) return length != 0U && length <= palette_size;
    if ((color != 0U && color != 2U) || length != 2U * channels_for_color(color)) return false;
    const auto limit = 1U << depth;
    for (std::size_t at = data; at < data + length; at += 2U) {
        const auto sample = (static_cast<unsigned>(bytes[at]) << 8U) | bytes[at + 1U];
        if (sample >= limit) return false;
    }
    return true;
}

ImageResult decode(const std::vector<Byte>& bytes, PayloadTail tail, bool strict) {
    if (bytes.size() > max_input_bytes) return failure(ErrorCode::LimitExceeded, no_byte, "DWQ input exceeds byte limit");
    if (bytes.size() < 64U) return failure(ErrorCode::InvalidFormat, bytes.size(), "Truncated 64-byte DWQ header");
    const bool has_margins = bytes[58U] == static_cast<Byte>('A');
    if (strict) {
        if (!matches(bytes, 0U, "PNG             ") || !matches(bytes, 48U, "PACKTYPE=8A     ")) {
            return failure(ErrorCode::UnsupportedFormat, 0U, "Only canonical PNG PACKTYPE=8A is supported");
        }
        if (!matches(bytes, 44U, "    ")) return failure(ErrorCode::UnsupportedFormat, 44U, "Unknown DWQ reserved header variant");
    } else if (!matches(bytes, 0U, "PNG") || !matches(bytes, 48U, "PACKTYPE=8") ||
               (!has_margins && bytes[58U] != static_cast<Byte>(' ') && bytes[58U] != 0U)) {
        return failure(ErrorCode::UnsupportedFormat, 0U, "Expected PNG PACKTYPE=8 or 8A");
    }
    const auto width = le32(bytes, 36U);
    const auto stored_height = le32(bytes, 40U);
    const auto height = !strict && (stored_height & 0x80000000U) != 0U ? 0U - stored_height : stored_height;
    if (width == 0U || height == 0U || width > 0x7fffffffU || height > 0x7fffffffU) {
        return failure(ErrorCode::InvalidFormat, 36U, "DWQ width and height must be positive signed32 values");
    }
    if (width > max_dimension || height > max_dimension) return failure(ErrorCode::LimitExceeded, 36U, "Canvas dimension exceeds limit");
    const auto pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (pixels > max_pixels) return failure(ErrorCode::LimitExceeded, 36U, "Canvas pixel count exceeds limit");
    Margins margins;
    if (has_margins) {
        margins = {le32(bytes, 16U), le32(bytes, 20U), le32(bytes, 24U), le32(bytes, 28U)};
        if (static_cast<std::uint64_t>(margins.left) + margins.right >= width ||
            static_cast<std::uint64_t>(margins.top) + margins.bottom >= height) {
            return failure(ErrorCode::InvalidFormat, 16U, "Cut margins must leave a positive region within the full canvas");
        }
    }
    if (le32(bytes, 32U) != bytes.size() - 64U) return failure(ErrorCode::InvalidFormat, 32U, "DWQ PNG size differs from exact payload length");
    constexpr std::array<Byte, 8> signature{137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
    if (bytes.size() - 64U < signature.size() || !std::equal(signature.begin(), signature.end(), bytes.begin() + 64)) {
        return failure(ErrorCode::InvalidFormat, 64U, "Missing or truncated PNG signature");
    }
    std::vector<nnn_png_segment> feed;
    std::vector<DataSpan> compressed;
    feed.push_back({bytes.data() + 64U, 8U});
    std::size_t position = 72U;
    std::size_t chunks = 0;
    std::size_t ancillary_bytes = 0;
    std::size_t palette_size = 0;
    unsigned singleton_flags = 0U;
    bool seen_header = false;
    bool seen_idat = false;
    bool ended_idat = false;
    bool seen_end = false;
    bool seen_transparency = false;
    Byte color_type = 0;
    Byte depth = 0;
    Byte interlace = 0;
    while (position < bytes.size()) {
        if (++chunks > max_chunks) return failure(ErrorCode::LimitExceeded, position, "PNG chunk count exceeds limit");
        if (bytes.size() - position < 12U) return failure(ErrorCode::InvalidFormat, position, "Truncated PNG chunk framing");
        const auto length = static_cast<std::size_t>(be32(bytes, position));
        if (length > 0x7fffffffU || length > bytes.size() - position - 12U) return failure(ErrorCode::InvalidFormat, position, "PNG chunk length exceeds its bounded input");
        const auto type = position + 4U;
        const auto data = position + 8U;
        const auto next = position + 12U + length;
        for (std::size_t i = 0; i < 4U; ++i) if (!alphabetic(bytes[type + i])) return failure(ErrorCode::InvalidFormat, type + i, "PNG chunk type must contain letters only");
        if ((bytes[type + 2U] & 0x20U) != 0U) return failure(ErrorCode::InvalidFormat, type + 2U, "PNG chunk reserved type bit is set");
        const auto actual_crc = static_cast<std::uint32_t>(crc32(0L, bytes.data() + type, static_cast<uInt>(length + 4U)));
        if (actual_crc != be32(bytes, data + length)) return failure(ErrorCode::InvalidFormat, data + length, "PNG chunk CRC mismatch");
        const bool ihdr = type_is(bytes, type, "IHDR");
        const bool idat = type_is(bytes, type, "IDAT");
        const bool iend = type_is(bytes, type, "IEND");
        const bool plte = type_is(bytes, type, "PLTE");
        const bool trns = type_is(bytes, type, "tRNS");
        if (!seen_header && !ihdr) return failure(ErrorCode::InvalidFormat, type, "IHDR must be the first PNG chunk");
        if (ihdr) {
            if (seen_header || chunks != 1U || length != 13U) return failure(ErrorCode::InvalidFormat, position, "PNG requires exactly one 13-byte first IHDR");
            seen_header = true;
            const auto png_width = be32(bytes, data);
            const auto png_height = be32(bytes, data + 4U);
            if (png_width == 0U || png_height == 0U) return failure(ErrorCode::InvalidFormat, data, "PNG canvas must be nonzero");
            if (png_width > max_dimension || png_height > max_dimension ||
                static_cast<std::uint64_t>(png_width) * png_height > max_pixels) return failure(ErrorCode::LimitExceeded, data, "PNG canvas exceeds resource limit");
            if (png_width != width || png_height != height) return failure(ErrorCode::InvalidFormat, data, "PNG IHDR and DWQ dimensions disagree");
            depth = bytes[data + 8U];
            color_type = bytes[data + 9U];
            interlace = bytes[data + 12U];
            if (strict && (depth != 8U || (color_type != 2U && color_type != 6U) || interlace != 0U))
                return failure(ErrorCode::UnsupportedFormat, data + 8U, "Only 8-bit noninterlaced RGB/RGBA PNG is supported");
            if (channels_for_color(color_type) == 0U || !depth_supported(color_type, depth) ||
                bytes[data + 10U] != 0U || bytes[data + 11U] != 0U || interlace > 1U) {
                return failure(ErrorCode::UnsupportedFormat, data + 8U, "Unsupported PNG image profile");
            }
            feed.push_back({bytes.data() + position, next - position});
        } else if (!strict && plte) {
            const auto entries = length / 3U;
            if (seen_idat || seen_transparency || palette_size != 0U || length % 3U != 0U ||
                entries == 0U || entries > 256U || color_type == 0U || color_type == 4U ||
                (color_type == 3U && entries > (1U << depth))) {
                return failure(ErrorCode::InvalidFormat, type, "Invalid, duplicate or out-of-order PNG palette");
            }
            palette_size = entries;
            if (color_type == 3U) feed.push_back({bytes.data() + position, next - position});
        } else if (!strict && trns) {
            if (seen_idat || seen_transparency || !transparency_valid(bytes, data, length, color_type, depth, palette_size)) {
                return failure(ErrorCode::InvalidFormat, type, "Invalid, duplicate or out-of-order PNG transparency");
            }
            seen_transparency = true;
            feed.push_back({bytes.data() + position, next - position});
        } else if (idat) {
            if (ended_idat) return failure(ErrorCode::InvalidFormat, type, "PNG IDAT chunks must be consecutive");
            if (color_type == 3U && palette_size == 0U) return failure(ErrorCode::InvalidFormat, type, "Indexed PNG requires a palette before IDAT");
            seen_idat = true;
            compressed.push_back({data, length});
            feed.push_back({bytes.data() + position, next - position});
        } else if (iend) {
            // Some legacy DWQ writers leave bytes from the prior compressed image
            // in the member. Validate IEND and feed only the PNG prefix to libpng.
            const auto trailing=bytes.size()-next;
            const bool bounded_legacy=tail==PayloadTail::LegacyDwq && trailing<=64U*1024U;
            if (!seen_idat || length != 0U || (trailing!=0U && !bounded_legacy))
                return failure(ErrorCode::InvalidFormat, position, "IEND must be empty, follow IDAT and end the PNG stream");
            seen_end = true;
            feed.push_back({bytes.data() + position, next - position});
            position = next;
            break;
        } else {
            if (seen_idat) ended_idat = true;
            if ((bytes[type] & 0x20U) == 0U || trns ||
                type_is(bytes, type, "acTL") || type_is(bytes, type, "fcTL") || type_is(bytes, type, "fdAT")) {
                return failure(ErrorCode::UnsupportedFormat, type, "Unsupported critical, transparency or animation chunk");
            }
            if (length > max_ancillary_chunk_bytes || length > max_ancillary_bytes - ancillary_bytes) {
                return failure(ErrorCode::LimitExceeded, position, "PNG ancillary data exceeds byte limit");
            }
            ancillary_bytes += length;
            const bool itxt = type_is(bytes, type, "iTXt");
            const bool text = type_is(bytes, type, "tEXt");
            const bool phys = type_is(bytes, type, "pHYs");
            const bool iccp = type_is(bytes, type, "iCCP");
            const bool srgb = type_is(bytes, type, "sRGB");
            const bool chrm = type_is(bytes, type, "cHRM");
            const bool gama = type_is(bytes, type, "gAMA");
            unsigned flag = 0U;
            if (gama) flag = 1U;
            if (chrm) flag = 2U;
            if (phys) flag = 4U;
            if (srgb) flag = 8U;
            if (iccp) flag = 16U;
            if (flag != 0U) {
                if (seen_idat || (singleton_flags & flag) != 0U) return failure(ErrorCode::InvalidFormat, type, "PNG singleton metadata is duplicated or follows IDAT");
                if ((srgb && (singleton_flags & 16U) != 0U) || (iccp && (singleton_flags & 8U) != 0U)) return failure(ErrorCode::InvalidFormat, type, "sRGB and iCCP must not coexist");
                singleton_flags |= flag;
            }
            if ((gama && length != 4U) || (chrm && length != 32U) || (phys && length != 9U) || (srgb && length != 1U)) {
                return failure(ErrorCode::InvalidFormat, position, "PNG fixed-size ancillary chunk has a wrong length");
            }
            if ((srgb && bytes[data] > 3U) || (phys && bytes[data + 8U] > 1U)) return failure(ErrorCode::InvalidFormat, data, "Invalid PNG metadata enumeration");
            if (text || itxt || iccp) {
                std::size_t keyword = 0;
                if (!keyword_end(bytes, data, data + length, keyword)) return failure(ErrorCode::InvalidFormat, data, "PNG text/profile keyword is missing or too long");
                if (iccp) {
                    if (data + length - keyword < 3U || bytes[keyword + 1U] != 0U) return failure(ErrorCode::InvalidFormat, keyword, "PNG iCCP framing is invalid");
                } else if (itxt) {
                    if (data + length - keyword < 5U || bytes[keyword + 1U] > 1U || bytes[keyword + 2U] != 0U) return failure(ErrorCode::InvalidFormat, keyword, "PNG iTXt compression framing is invalid");
                    auto cursor = keyword + 3U;
                    while (cursor < data + length && bytes[cursor] != 0U) ++cursor;
                    if (cursor == data + length) return failure(ErrorCode::InvalidFormat, cursor, "PNG iTXt language terminator is missing");
                    ++cursor;
                    while (cursor < data + length && bytes[cursor] != 0U) ++cursor;
                    if (cursor == data + length) return failure(ErrorCode::InvalidFormat, cursor, "PNG iTXt translated-keyword terminator is missing");
                }
            }
            // Opaque ancillary payloads are never decompressed, interpreted as
            // color management, or passed to libpng. Their original CRCs were checked.
        }
        position = next;
    }
    if (!seen_header || !seen_idat || !seen_end) return failure(ErrorCode::InvalidFormat, position, "PNG is missing a complete IHDR/IDAT/IEND sequence");
    const auto layout = scanlines(width, height, channels_for_color(color_type) * depth, interlace != 0U);
    const auto validated = validate_deflate(bytes, compressed, layout);
    if (!validated) return {validated, {}};
    Image image;
    image.rgba.resize(pixels * 4U);
    nnn_png_decode_report report{};
    const int decoded = nnn_png_decode_segments(feed.data(), feed.size(), width, height, depth, color_type, interlace,
                                               image.rgba.data(), image.rgba.size(), max_decoder_work_bytes, &report);
    if (decoded != NNN_PNG_DECODE_OK || report.remaining_work_bytes != 0U) {
        const auto code = decoded == NNN_PNG_DECODE_LIMIT ? ErrorCode::LimitExceeded : ErrorCode::InvalidFormat;
        return failure(code, no_byte, report.message[0] != '\0' ? report.message : "Bounded PNG decoding or cleanup failed");
    }
    image.width = width;
    image.height = height;
    image.source_color_type = color_type;
    image.has_alpha = color_type == 4U || color_type == 6U || seen_transparency;
    image.margins = margins;
    return {{}, std::move(image)};
}

ImageResult decode_checked(const std::vector<Byte>& bytes, PayloadTail tail, bool strict) {
    try {
        return decode(bytes, tail, strict);
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::LimitExceeded, no_byte, "Cannot allocate bounded PNG storage");
    } catch (const std::length_error&) {
        return failure(ErrorCode::LimitExceeded, no_byte, "PNG storage exceeds container limit");
    }
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

ImageResult decode_dwq8a(const std::vector<Byte>& bytes,PayloadTail tail) {
    return decode_checked(bytes, tail, true);
}

ImageResult decode_dwq_png(const std::vector<Byte>& bytes,PayloadTail tail) {
    return decode_checked(bytes, tail, false);
}

ImageResult load_dwq8a(const std::string& filename) {
    try {
        namespace fs = std::filesystem;
        const fs::path path(filename);
        std::error_code error;
        const auto state = fs::status(path, error);
        if (error) {
            const bool missing = error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory;
            return failure(missing ? ErrorCode::NotFound : ErrorCode::ReadFailed, no_byte, "Cannot inspect DWQ8A file");
        }
        if (!fs::exists(state)) return failure(ErrorCode::NotFound, no_byte, "DWQ8A file was not found");
        if (!fs::is_regular_file(state)) return failure(ErrorCode::ReadFailed, no_byte, "DWQ8A input must be an ordinary file");
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return failure(ErrorCode::ReadFailed, no_byte, "Cannot open DWQ8A file");
        const auto end = file.tellg();
        if (end < std::ifstream::pos_type(0)) return failure(ErrorCode::ReadFailed, no_byte, "Cannot determine DWQ8A file length");
        const auto length = static_cast<std::uint64_t>(static_cast<std::streamoff>(end));
        if (length > max_input_bytes) return failure(ErrorCode::LimitExceeded, no_byte, "DWQ8A input exceeds byte limit");
        std::vector<Byte> bytes(static_cast<std::size_t>(length));
        file.seekg(0, std::ios::beg);
        if (!file) return failure(ErrorCode::ReadFailed, no_byte, "Cannot seek DWQ8A file");
        if (!bytes.empty()) {
            const auto expected = static_cast<std::streamsize>(bytes.size());
            file.read(reinterpret_cast<char*>(bytes.data()), expected);
            if (!file || file.gcount() != expected) return failure(ErrorCode::ReadFailed, no_byte, "DWQ8A read was incomplete");
        }
        if (file.peek() != std::ifstream::traits_type::eof() || !file.eof() || file.bad()) return failure(ErrorCode::ReadFailed, no_byte, "DWQ8A length changed or final read failed");
        return decode_dwq8a(bytes);
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::LimitExceeded, no_byte, "Cannot allocate bounded DWQ8A file storage");
    } catch (const std::length_error&) {
        return failure(ErrorCode::LimitExceeded, no_byte, "DWQ8A file exceeds container limit");
    } catch (const std::filesystem::filesystem_error&) {
        return failure(ErrorCode::ReadFailed, no_byte, "Cannot inspect DWQ8A file path");
    }
}

} // namespace nnn::png
