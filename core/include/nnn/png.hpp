#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace nnn::png {

using Byte = std::uint8_t;
inline constexpr std::size_t no_byte = std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t max_input_bytes = 64U * 1024U * 1024U;
inline constexpr std::size_t max_pixels = 16U * 1024U * 1024U;
inline constexpr std::uint32_t max_dimension = 8192U;
inline constexpr std::size_t max_chunks = 65536U;
inline constexpr std::size_t max_ancillary_chunk_bytes = 4U * 1024U * 1024U;
inline constexpr std::size_t max_ancillary_bytes = 8U * 1024U * 1024U;
inline constexpr std::size_t max_decoder_work_bytes = 16U * 1024U * 1024U;

enum class ErrorCode { Ok, NotFound, ReadFailed, UnsupportedFormat, InvalidFormat, LimitExceeded };

struct Status {
    ErrorCode code = ErrorCode::Ok;
    std::size_t byte_offset = no_byte;
    std::string detail;
    explicit operator bool() const noexcept { return code == ErrorCode::Ok; }
};

const char* error_name(ErrorCode code) noexcept;

struct Margins {
    std::uint32_t top = 0;
    std::uint32_t left = 0;
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;
};

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t source_color_type = 0;
    bool has_alpha = false;
    Margins margins;
    // Owned RGBA8, top-left origin, row-major and straight (non-premultiplied).
    // The full canvas is retained; margins are metadata, not applied cropping.
    std::vector<Byte> rgba;
};

struct ImageResult {
    Status status;
    Image image;
};

// Restricted DWQ PACKTYPE=8A: 8-bit RGB/RGBA, non-interlaced PNG only.
// Metadata/color profiles are bounded and skipped; pixel channels are not transformed.
// Every failure returns the default zero-sized Image with empty RGBA bytes.
enum class PayloadTail { Exact, LegacyDwq };
ImageResult decode_dwq8a(const std::vector<Byte>& bytes,PayloadTail tail=PayloadTail::Exact);

// DWQ PACKTYPE=8/8A PNG, including standard indexed, grayscale, alpha and Adam7 layouts.
// Output is always RGBA8. Only PACKTYPE=8A supplies stored cut margins.
ImageResult decode_dwq_png(const std::vector<Byte>& bytes,PayloadTail tail=PayloadTail::Exact);

// Read-only ordinary-file adapter. Rechecks exact file length on every call.
ImageResult load_dwq8a(const std::string& filename);

} // namespace nnn::png
