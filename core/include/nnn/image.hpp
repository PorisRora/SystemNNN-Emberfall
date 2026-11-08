#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace nnn::image {

using Byte = std::uint8_t;
inline constexpr std::size_t no_byte = std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t max_input_bytes = 64U * 1024U * 1024U;
inline constexpr std::size_t max_pixels = 16U * 1024U * 1024U;
inline constexpr std::uint32_t max_dimension = 8192U;

enum class ErrorCode {
    Ok,
    NotFound,
    ReadFailed,
    UnsupportedFormat,
    InvalidFormat,
    LimitExceeded,
};

struct Status {
    ErrorCode code = ErrorCode::Ok;
    std::size_t byte_offset = no_byte;
    std::string detail;
    explicit operator bool() const noexcept { return code == ErrorCode::Ok; }
};

const char* error_name(ErrorCode code) noexcept;

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t source_bit_depth = 0;
    // Owning RGB8 data: top-left origin, rows from top to bottom, no row padding.
    // A successful decode has exactly width * height * 3 bytes; no alpha is inferred.
    std::vector<Byte> rgb;
};

struct ImageResult {
    Status status;
    Image image;
};

// Bounded memory-only decode of a complete JPEG payload to top-left RGB8.
// The caller owns the surrounding file format and verifies its image dimensions.
ImageResult decode_jpeg(const Byte* bytes,std::size_t length);

// Restricted, uncompressed, unmasked PACKTYPE=0 DWQ profile. No Win32 dependency.
// The original text-only prefix supplies its dimensions through the embedded DIB.
// The game loader treats the two embedded BMP sizes as advisory and validates
// the complete padded row layout against the actual member bytes.
// Failed results always contain the default zero-sized, empty Image.
enum class BitmapSizeFields { Exact, Advisory };
ImageResult decode_dwq(const std::vector<Byte>& bytes, BitmapSizeFields sizes=BitmapSizeFields::Exact);

// Ordinary-file entry point. Files beyond max_input_bytes are rejected before
// allocation. A missing path is NotFound; other open/read failures are ReadFailed.
ImageResult load_dwq(const std::string& filename);

} // namespace nnn::image
