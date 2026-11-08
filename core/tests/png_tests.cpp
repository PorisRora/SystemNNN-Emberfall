#include "nnn/png.hpp"
#include "png/fixtures/expected_cases.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Byte = std::uint8_t;
using Bytes = std::vector<Byte>;
using ErrorCode = nnn::png::ErrorCode;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Tests {
    int passed = 0;
    int failed = 0;
    template <typename Function>
    void run(const std::string& name, Function function) {
        try { function(); ++passed; std::cout << "PASS: " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cout << "FAIL: " << name << ": " << error.what() << '\n'; }
    }
};

Bytes read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "open fixed PNG fixture");
    return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void be32(Bytes& output, std::uint32_t value) {
    for (unsigned shift : {24U, 16U, 8U, 0U}) output.push_back(static_cast<Byte>(value >> shift));
}

void le32_at(Bytes& output, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0; index < 4U; ++index) output[offset + index] = static_cast<Byte>(value >> (index * 8U));
}

void be32_at(Bytes& output, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0; index < 4U; ++index) output[offset + index] = static_cast<Byte>(value >> ((3U - index) * 8U));
}

std::uint32_t crc(const Bytes& data) {
    std::uint32_t value = 0xffffffffU;
    for (Byte byte : data) {
        value ^= byte;
        for (unsigned bit = 0; bit < 8U; ++bit) value = (value >> 1U) ^ ((value & 1U) != 0U ? 0xedb88320U : 0U);
    }
    return value ^ 0xffffffffU;
}

struct Chunk { std::string type; Bytes data; };

Bytes chunk(const Chunk& item) {
    require(item.type.size() == 4U, "test chunk type has four bytes");
    Bytes result;
    be32(result, static_cast<std::uint32_t>(item.data.size()));
    Bytes content;
    for (char byte : item.type) content.push_back(static_cast<Byte>(byte));
    content.insert(content.end(), item.data.begin(), item.data.end());
    result.insert(result.end(), content.begin(), content.end());
    be32(result, crc(content));
    return result;
}

// Independent stored-DEFLATE assembly from fixed scanline bytes; no PNG encoder
// or new decoder is used to create the expected image.
Bytes zlib_stored(const Bytes& raw) {
    require(raw.size() <= 65535U, "small fixed DEFLATE block only");
    const auto size = static_cast<std::uint16_t>(raw.size());
    Bytes result{0x78U, 0x01U, 0x01U, static_cast<Byte>(size), static_cast<Byte>(size >> 8U),
                 static_cast<Byte>(~size), static_cast<Byte>(static_cast<std::uint16_t>(~size) >> 8U)};
    result.insert(result.end(), raw.begin(), raw.end());
    std::uint32_t a = 1, b = 0;
    for (Byte byte : raw) { a = (a + byte) % 65521U; b = (b + a) % 65521U; }
    be32(result, (b << 16U) | a);
    return result;
}

Bytes ihdr(std::uint32_t width, std::uint32_t height, Byte color = 6, Byte depth = 8,
           Byte compression = 0, Byte filter = 0, Byte interlace = 0) {
    Bytes result;
    be32(result, width); be32(result, height);
    result.insert(result.end(), {depth, color, compression, filter, interlace});
    return result;
}

Bytes png(const std::vector<Chunk>& chunks) {
    Bytes result{137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
    for (const auto& item : chunks) {
        const auto bytes = chunk(item);
        result.insert(result.end(), bytes.begin(), bytes.end());
    }
    return result;
}

Bytes dwq(const Bytes& data, std::uint32_t width = 2, std::uint32_t height = 2) {
    const std::string header = "PNG                                             PACKTYPE=8A     ";
    require(header.size() == 64U, "fixed original type8A template");
    Bytes result(header.begin(), header.end());
    for (std::size_t offset : {16U, 20U, 24U, 28U}) le32_at(result, offset, 0);
    le32_at(result, 32, static_cast<std::uint32_t>(data.size()));
    le32_at(result, 36, width); le32_at(result, 40, height);
    result.insert(result.end(), data.begin(), data.end());
    return result;
}

const Bytes fixed_rgba{201U, 12U, 34U, 0U, 5U, 210U, 70U, 1U,
                       17U, 31U, 240U, 127U, 245U, 220U, 8U, 255U};
const Bytes fixed_scanlines{0U, 201U, 12U, 34U, 0U, 5U, 210U, 70U, 1U,
                            0U, 17U, 31U, 240U, 127U, 245U, 220U, 8U, 255U};

std::vector<Chunk> base_chunks() {
    return {{"IHDR", ihdr(2, 2)}, {"IDAT", zlib_stored(fixed_scanlines)}, {"IEND", {}}};
}

template <typename Result>
void empty_failure(const Result& result) {
    require(!static_cast<bool>(result.status), "bad data must fail");
    require(result.image.width == 0U && result.image.height == 0U && result.image.source_color_type == 0U && !result.image.has_alpha,
            "failure has zero image metadata");
    require(result.image.rgba.empty(), "failure has no partial RGBA pixels");
    require(result.image.margins.top == 0U && result.image.margins.left == 0U && result.image.margins.right == 0U && result.image.margins.bottom == 0U,
            "failure has no partial margins");
}

template <typename Result>
void failure(const Result& result, ErrorCode code) {
    empty_failure(result);
    require(result.status.code == code, "expected typed PNG failure");
}

template <typename Result>
void success(const Result& result, std::uint32_t width, std::uint32_t height, Byte color, const Bytes& expected) {
    require(static_cast<bool>(result.status), "known-valid PNG must decode");
    require(result.image.width == width && result.image.height == height && result.image.source_color_type == color, "fixed image metadata");
    require(result.image.rgba == expected, "every fixed RGBA byte matches");
}
} // namespace

int main() {
    Tests tests;
    for (const auto& fixture : dw007_fixture::fixtures) {
        tests.run(std::string("fixed fixture ") + fixture.name, [&] {
            const auto result = nnn::png::decode_dwq8a(read(std::filesystem::path(NNN_PNG_FIXTURE_DIR) / fixture.binary_filename));
            success(result, fixture.width, fixture.height, fixture.source_color_type, fixture.expected_rgba);
            require(result.image.margins.top == fixture.margin_top && result.image.margins.left == fixture.margin_left
                    && result.image.margins.right == fixture.margin_right && result.image.margins.bottom == fixture.margin_bottom,
                    "all original cut margins retained without cropping");
        });
    }
    const auto basic = dwq(png(base_chunks()));
    tests.run("independent stored-DEFLATE seed preserves nonpremultiplied RGBA", [&] {
        success(nnn::png::decode_dwq8a(basic), 2, 2, 6, fixed_rgba);
    });
    tests.run("decoded pixels own their storage after input mutation and release", [&] {
        Bytes bytes = basic;
        const auto result = nnn::png::decode_dwq8a(bytes);
        bytes.assign(3, 0); bytes.shrink_to_fit();
        success(result, 2, 2, 6, fixed_rgba);
    });
    tests.run("all truncated prefixes fail without partial image", [&] {
        for (std::size_t size = 0; size < basic.size(); ++size) {
            const Bytes prefix(basic.begin(), basic.begin() + static_cast<std::ptrdiff_t>(size));
            empty_failure(nnn::png::decode_dwq8a(prefix));
        }
    });
    tests.run("input above the 64 MiB bound fails before format parsing", [&] {
        const Bytes bytes(64U * 1024U * 1024U + 1U, 0U);
        failure(nnn::png::decode_dwq8a(bytes), ErrorCode::LimitExceeded);
    });
    tests.run("DWQ type signatures and unknown reserved profile are unsupported", [&] {
        for (std::size_t offset : {0U, 44U, 57U, 58U}) {
            Bytes bytes = basic; bytes[offset] ^= 1U;
            failure(nnn::png::decode_dwq8a(bytes), ErrorCode::UnsupportedFormat);
        }
    });
    tests.run("DWQ declared payload length is exact", [&] {
        for (std::uint32_t size : {0U, static_cast<std::uint32_t>(basic.size() - 65U), static_cast<std::uint32_t>(basic.size() - 63U)}) {
            Bytes bytes = basic; le32_at(bytes, 32, size);
            failure(nnn::png::decode_dwq8a(bytes), ErrorCode::InvalidFormat);
        }
        Bytes extra = basic; extra.push_back(0);
        failure(nnn::png::decode_dwq8a(extra), ErrorCode::InvalidFormat);
    });
    tests.run("PNG signature is checked inside the DWQ envelope", [&] {
        Bytes bytes = basic; bytes[64] ^= 1U;
        failure(nnn::png::decode_dwq8a(bytes), ErrorCode::InvalidFormat);
    });
    tests.run("zero and negative DWQ dimensions do not become unsigned allocations", [&] {
        for (std::size_t offset : {36U, 40U}) {
            for (std::uint32_t value : {0U, 0xffffffffU, 0x80000000U}) {
                Bytes bytes = basic; le32_at(bytes, offset, value);
                failure(nnn::png::decode_dwq8a(bytes), ErrorCode::InvalidFormat);
            }
        }
    });
    tests.run("DWQ and IHDR dimensions must agree", [&] {
        Bytes bytes = basic; le32_at(bytes, 36, 3);
        failure(nnn::png::decode_dwq8a(bytes), ErrorCode::InvalidFormat);
        bytes = basic; le32_at(bytes, 40, 3);
        failure(nnn::png::decode_dwq8a(bytes), ErrorCode::InvalidFormat);
    });
    tests.run("margins retain the full original canvas", [&] {
        Bytes bytes = basic;
        le32_at(bytes, 16, 1); le32_at(bytes, 20, 1);
        const auto result = nnn::png::decode_dwq8a(bytes);
        success(result, 2, 2, 6, fixed_rgba);
        require(result.image.margins.top == 1 && result.image.margins.left == 1, "margins remain metadata");
    });
    tests.run("negative huge or completely removing cut margins fail", [&] {
        for (std::size_t offset : {16U, 20U, 24U, 28U}) {
            for (std::uint32_t value : {2U, 0xffffffffU, 0x80000000U}) {
                Bytes bytes = basic; le32_at(bytes, offset, value);
                failure(nnn::png::decode_dwq8a(bytes), ErrorCode::InvalidFormat);
            }
        }
        Bytes bytes = basic; le32_at(bytes, 16, 1); le32_at(bytes, 28, 1);
        failure(nnn::png::decode_dwq8a(bytes), ErrorCode::InvalidFormat);
        bytes = basic; le32_at(bytes, 20, 1); le32_at(bytes, 24, 1);
        failure(nnn::png::decode_dwq8a(bytes), ErrorCode::InvalidFormat);
    });
    tests.run("dimension and pixel limits precede raster inflation", [&] {
        for (const auto& size : {std::pair<std::uint32_t, std::uint32_t>{8193, 1}, {1, 8193}, {8192, 2049}}) {
            const auto data = dwq(png({{"IHDR", ihdr(size.first, size.second)}, {"IDAT", zlib_stored(fixed_scanlines)}, {"IEND", {}}}), size.first, size.second);
            failure(nnn::png::decode_dwq8a(data), ErrorCode::LimitExceeded);
        }
    });
    tests.run("maximum single-row width is valid with tight RGBA output", [&] {
        Bytes raw{0}, expected;
        for (std::size_t i = 0; i < 8192U; ++i) { raw.insert(raw.end(), {7U, 11U, 19U}); expected.insert(expected.end(), {7U, 11U, 19U, 255U}); }
        const auto bytes = dwq(png({{"IHDR", ihdr(8192, 1, 2)}, {"IDAT", zlib_stored(raw)}, {"IEND", {}}}), 8192, 1);
        success(nnn::png::decode_dwq8a(bytes), 8192, 1, 2, expected);
    });
    tests.run("maximum single-column height is valid", [&] {
        Bytes raw, expected;
        for (std::size_t i = 0; i < 8192U; ++i) { raw.insert(raw.end(), {0U, 7U, 11U, 19U}); expected.insert(expected.end(), {7U, 11U, 19U, 255U}); }
        const auto bytes = dwq(png({{"IHDR", ihdr(1, 8192, 2)}, {"IDAT", zlib_stored(raw)}, {"IEND", {}}}), 1, 8192);
        success(nnn::png::decode_dwq8a(bytes), 1, 8192, 2, expected);
    });
    tests.run("unsupported PNG color types never silently convert", [&] {
        for (Byte color : {Byte{0}, Byte{3}, Byte{4}, Byte{1}, Byte{7}}) {
            auto chunks = base_chunks(); chunks[0].data = ihdr(2, 2, color);
            failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::UnsupportedFormat);
        }
    });
    tests.run("unsupported depth methods and interlace never silently convert", [&] {
        for (Byte depth : {Byte{1}, Byte{2}, Byte{4}, Byte{16}}) {
            auto chunks = base_chunks(); chunks[0].data = ihdr(2, 2, 6, depth);
            failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::UnsupportedFormat);
        }
        for (std::size_t field : {10U, 11U, 12U}) {
            auto chunks = base_chunks(); chunks[0].data[field] = 1;
            failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::UnsupportedFormat);
        }
    });
    tests.run("indexed PNG keeps palette RGB and bounds indices in type 8", [&] {
        const auto wrap = [](const std::vector<Chunk>& chunks) {
            auto bytes = dwq(png(chunks), 3, 1);
            bytes[58] = 0;
            std::fill(bytes.begin() + 16, bytes.begin() + 32, Byte{255});
            return bytes;
        };
        std::vector<Chunk> chunks{{"IHDR", ihdr(3, 1, 3, 2)},
                                  {"PLTE", {10, 20, 30, 40, 50, 60, 70, 80, 90}},
                                  {"tRNS", {0, 127}}, {"IDAT", zlib_stored({0, 0x18})}, {"IEND", {}}};
        const auto indexed = nnn::png::decode_dwq_png(wrap(chunks));
        success(indexed, 3, 1, 3, {10,20,30,0, 40,50,60,127, 70,80,90,255});
        require(indexed.image.has_alpha && indexed.image.margins.left == 0U && indexed.image.margins.bottom == 0U,
                "type 8 recognizes transparency and ignores stored cut metadata");
        chunks[3].data = zlib_stored({0, 0x1c});
        failure(nnn::png::decode_dwq_png(wrap(chunks)), ErrorCode::InvalidFormat);
    });
    tests.run("16-bit PNG uses high bytes and preserves explicit alpha", [&] {
        std::vector<Chunk> chunks{{"IHDR", ihdr(2, 1, 4, 16)},
                                  {"IDAT", zlib_stored({0, 0x12,0x34,0x80,0xff, 0xac,0xcd,0x00,0xf0})}, {"IEND", {}}};
        const auto gray = nnn::png::decode_dwq_png(dwq(png(chunks), 2, 1));
        success(gray, 2, 1, 4, {0x12,0x12,0x12,0x80, 0xac,0xac,0xac,0});
        require(gray.image.has_alpha, "gray+alpha has a real mask");
        chunks[0].data = ihdr(1, 1, 2, 16);
        chunks[1].data = zlib_stored({0, 0x12,0x34,0x56,0x78,0x9a,0xbc});
        const auto rgb = nnn::png::decode_dwq_png(dwq(png(chunks), 1, 1));
        success(rgb, 1, 1, 2, {0x12,0x56,0x9a,255});
        require(!rgb.image.has_alpha, "opaque filler is not a source mask");
    });
    tests.run("Adam7 validates scanline filters after empty passes", [&] {
        Bytes raw{0,0, 0,40, 0,120,160, 0,20, 0,140, 0,60,80,100};
        std::vector<Chunk> chunks{{"IHDR", ihdr(3, 3, 0, 8, 0, 0, 1)},
                                  {"IDAT", zlib_stored(raw)}, {"IEND", {}}};
        Bytes expected;
        for (Byte value : {Byte{0}, Byte{20}, Byte{40}, Byte{60}, Byte{80}, Byte{100}, Byte{120}, Byte{140}, Byte{160}}) {
            expected.insert(expected.end(), {value, value, value, 255U});
        }
        success(nnn::png::decode_dwq_png(dwq(png(chunks), 3, 3)), 3, 3, 0, expected);
        raw[11] = 5;
        chunks[1].data = zlib_stored(raw);
        failure(nnn::png::decode_dwq_png(dwq(png(chunks), 3, 3)), ErrorCode::InvalidFormat);
    });
    tests.run("IHDR must be first unique and exactly thirteen bytes", [&] {
        auto chunks = base_chunks(); chunks.insert(chunks.begin(), {"tEXt", {'k', 0, 'v'}});
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        chunks = base_chunks(); chunks.insert(chunks.begin() + 1, chunks[0]);
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        for (std::size_t length : {0U, 12U, 14U}) {
            chunks = base_chunks(); chunks[0].data.resize(length);
            failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        }
    });
    tests.run("critical chunk CRC failure is rejected", [&] {
        for (std::size_t position : {0U, 1U, 2U}) {
            const auto chunks = base_chunks();
            Bytes data{137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
            for (std::size_t i = 0; i < chunks.size(); ++i) {
                auto encoded = chunk(chunks[i]); if (i == position) encoded.back() ^= 1U;
                data.insert(data.end(), encoded.begin(), encoded.end());
            }
            failure(nnn::png::decode_dwq8a(dwq(data)), ErrorCode::InvalidFormat);
        }
    });
    tests.run("ignored ancillary chunks still require correct CRC", [&] {
        auto chunks = base_chunks(); chunks.insert(chunks.begin() + 1, {"caBX", {1U, 200U, 50U}});
        Bytes data{137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
        for (const auto& item : chunks) {
            auto encoded = chunk(item); if (item.type == "caBX") encoded.back() ^= 1U;
            data.insert(data.end(), encoded.begin(), encoded.end());
        }
        failure(nnn::png::decode_dwq8a(dwq(data)), ErrorCode::InvalidFormat);
    });
    tests.run("unbounded declared chunk sizes cannot overflow a range", [&] {
        for (std::uint32_t length : {0xffffffffU, 0x80000000U, 0x7fffffffU}) {
            Bytes bytes = basic; be32_at(bytes, 72, length);
            empty_failure(nnn::png::decode_dwq8a(bytes));
        }
    });
    tests.run("unknown critical chunks are unsupported", [&] {
        auto chunks = base_chunks(); chunks.insert(chunks.begin() + 1, {"ABCD", {}});
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::UnsupportedFormat);
    });
    tests.run("chunk type bytes must be letters with the reserved bit clear", [&] {
        for (const std::string& type : {std::string("aa1a"), std::string("abca")}) {
            auto chunks = base_chunks(); chunks.insert(chunks.begin() + 1, {type, {}});
            failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        }
    });
    tests.run("palette transparency and APNG chunks are explicitly unsupported", [&] {
        for (const std::string& type : {std::string("PLTE"), std::string("tRNS"), std::string("acTL"), std::string("fcTL"), std::string("fdAT")}) {
            auto chunks = base_chunks(); chunks.insert(chunks.begin() + 1, {type, {0, 0, 0}});
            failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::UnsupportedFormat);
        }
    });
    tests.run("unknown ancillary data does not change the decoded pixels", [&] {
        auto chunks = base_chunks(); chunks.insert(chunks.begin() + 1, {"caBX", {1U, 200U, 50U}});
        chunks.insert(chunks.end() - 1, {"ruSt", {9U, 20U, 0U}});
        success(nnn::png::decode_dwq8a(dwq(png(chunks))), 2, 2, 6, fixed_rgba);
    });
    tests.run("color and text metadata do not change numerical channels", [&] {
        auto chunks = base_chunks();
        chunks.insert(chunks.begin() + 1, {"gAMA", {0U, 0U, 177U, 143U}});
        chunks.insert(chunks.begin() + 2, {"sRGB", {0U}});
        chunks.insert(chunks.begin() + 3, {"pHYs", {0U, 0U, 11U, 19U, 0U, 0U, 11U, 19U, 1U}});
        chunks.insert(chunks.begin() + 4, {"tEXt", {'k', 0, 'v'}});
        chunks.insert(chunks.begin() + 5, {"iTXt", {'d', 0, 0, 0, 0, 0, 'v'}});
        success(nnn::png::decode_dwq8a(dwq(png(chunks))), 2, 2, 6, fixed_rgba);
    });
    tests.run("legal ancillary data after the final IDAT is retained only as metadata", [&] {
        auto chunks = base_chunks(); chunks.insert(chunks.end() - 1, {"tEXt", {'k', 0, 'v'}});
        success(nnn::png::decode_dwq8a(dwq(png(chunks))), 2, 2, 6, fixed_rgba);
    });
    tests.run("known fixed-length ancillary headers have exact lengths", [&] {
        for (const auto& item : {Chunk{"gAMA", Bytes(3, 0)}, Chunk{"cHRM", Bytes(31, 0)},
                                 Chunk{"pHYs", Bytes(8, 0)}, Chunk{"sRGB", {}}}) {
            auto chunks = base_chunks(); chunks.insert(chunks.begin() + 1, item);
            failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        }
    });
    tests.run("singleton metadata cannot duplicate or appear after IDAT", [&] {
        for (const auto& item : {Chunk{"gAMA", Bytes(4, 0)}, Chunk{"cHRM", Bytes(32, 0)},
                                 Chunk{"pHYs", Bytes(9, 0)}, Chunk{"sRGB", Bytes(1, 0)}, Chunk{"iCCP", {'p', 0, 0, 1}}}) {
            auto chunks = base_chunks(); chunks.insert(chunks.begin() + 1, item); chunks.insert(chunks.begin() + 1, item);
            failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
            chunks = base_chunks(); chunks.insert(chunks.end() - 1, item);
            failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        }
    });
    tests.run("iCCP and sRGB cannot both claim the profile", [&] {
        auto chunks = base_chunks(); chunks.insert(chunks.begin() + 1, {"iCCP", {'p', 0, 0, 1}});
        chunks.insert(chunks.begin() + 1, {"sRGB", {0}});
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
    });
    tests.run("ancillary per-chunk and cumulative memory bounds are checked", [&] {
        Bytes large(nnn::png::max_ancillary_chunk_bytes + 1U, 0U); large[0] = 'k';
        auto chunks = base_chunks(); chunks.insert(chunks.begin() + 1, {"caBX", large});
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::LimitExceeded);
        chunks = base_chunks();
        large.assign(3U * 1024U * 1024U, 0U); large[0] = 'k';
        for (unsigned i = 0; i < 3U; ++i) chunks.insert(chunks.begin() + 1, {"iTXt", large});
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::LimitExceeded);
    });
    tests.run("chunk count is finite even for zero-length ancillary chunks", [&] {
        auto chunks = base_chunks();
        chunks.insert(chunks.begin() + 1, nnn::png::max_chunks, Chunk{"tEXt", {'k', 0}});
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::LimitExceeded);
    });
    tests.run("consecutive multi-IDAT including empty chunks decodes one stream", [&] {
        const auto compressed = zlib_stored(fixed_scanlines);
        const Bytes first(compressed.begin(), compressed.begin() + 1);
        const Bytes rest(compressed.begin() + 1, compressed.end());
        success(nnn::png::decode_dwq8a(dwq(png({{"IHDR", ihdr(2, 2)}, {"IDAT", first}, {"IDAT", {}}, {"IDAT", rest}, {"IEND", {}}}))), 2, 2, 6, fixed_rgba);
    });
    tests.run("IDAT chunks cannot resume after ancillary data", [&] {
        const auto compressed = zlib_stored(fixed_scanlines);
        const Bytes first(compressed.begin(), compressed.begin() + 3);
        const Bytes rest(compressed.begin() + 3, compressed.end());
        failure(nnn::png::decode_dwq8a(dwq(png({{"IHDR", ihdr(2, 2)}, {"IDAT", first}, {"tEXt", {'k', 0, 'v'}}, {"IDAT", rest}, {"IEND", {}}}))), ErrorCode::InvalidFormat);
    });
    tests.run("missing empty or incomplete IDAT streams fail", [&] {
        failure(nnn::png::decode_dwq8a(dwq(png({{"IHDR", ihdr(2, 2)}, {"IEND", {}}}))), ErrorCode::InvalidFormat);
        failure(nnn::png::decode_dwq8a(dwq(png({{"IHDR", ihdr(2, 2)}, {"IDAT", {}}, {"IEND", {}}}))), ErrorCode::InvalidFormat);
        auto chunks = base_chunks(); chunks[1].data.resize(chunks[1].data.size() - 1U);
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
    });
    tests.run("trailing compressed bytes or concatenated zlib streams fail", [&] {
        auto chunks = base_chunks(); chunks[1].data.push_back(0);
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        chunks = base_chunks(); const auto extra = chunks[1].data;
        chunks[1].data.insert(chunks[1].data.end(), extra.begin(), extra.end());
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
    });
    tests.run("bad zlib checksum and malformed DEFLATE are invalid", [&] {
        auto chunks = base_chunks(); chunks[1].data.back() ^= 1U;
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        chunks = base_chunks(); chunks[1].data[5] ^= 1U;
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
    });
    tests.run("decompressed raster is exact and cannot silently grow", [&] {
        for (int adjustment : {-1, 1}) {
            Bytes raw = fixed_scanlines;
            if (adjustment < 0) raw.pop_back(); else raw.push_back(0);
            auto chunks = base_chunks(); chunks[1].data = zlib_stored(raw);
            failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        }
    });
    tests.run("unknown row filters are rejected even with valid CRC and zlib checksum", [&] {
        for (Byte filter : {Byte{5}, Byte{255}}) {
            Bytes raw = fixed_scanlines; raw[9] = filter;
            auto chunks = base_chunks(); chunks[1].data = zlib_stored(raw);
            failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        }
    });
    tests.run("IEND is mandatory empty unique and final", [&] {
        auto chunks = base_chunks(); chunks.pop_back();
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        chunks = base_chunks(); chunks.back().data = {0};
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        chunks = base_chunks(); chunks.push_back({"IEND", {}});
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        chunks = base_chunks(); chunks.push_back({"tEXt", {'k', 0, 'v'}});
        failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
    });
    tests.run("chunk ranges include their CRC and cannot cross payload end", [&] {
        Bytes data = png(base_chunks()); data.pop_back();
        failure(nnn::png::decode_dwq8a(dwq(data)), ErrorCode::InvalidFormat);
        data = png(base_chunks()); data.insert(data.end(), {0, 0, 0});
        failure(nnn::png::decode_dwq8a(dwq(data)), ErrorCode::InvalidFormat);
        success(nnn::png::decode_dwq8a(dwq(data),nnn::png::PayloadTail::LegacyDwq),2,2,6,fixed_rgba);
        data.insert(data.end(),65536U,0x5aU);
        failure(nnn::png::decode_dwq8a(dwq(data),nnn::png::PayloadTail::LegacyDwq),ErrorCode::InvalidFormat);
    });
    tests.run("RGB source gains opaque alpha without changing channel order", [&] {
        const Bytes raw{0, 10, 20, 30, 40, 50, 60, 0, 70, 80, 90, 100, 110, 120};
        const Bytes expected{10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255};
        const auto bytes = dwq(png({{"IHDR", ihdr(2, 2, 2)}, {"IDAT", zlib_stored(raw)}, {"IEND", {}}}));
        success(nnn::png::decode_dwq8a(bytes), 2, 2, 2, expected);
    });
    tests.run("independent hand-filtered Average and Paeth rows match fixed RGB", [&] {
        const Bytes expected{10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255};
        for (const Bytes& raw : {Bytes{3, 10, 20, 30, 35, 40, 45, 3, 65, 70, 75, 45, 45, 45},
                                 Bytes{4, 10, 20, 30, 30, 30, 30, 4, 60, 60, 60, 30, 30, 30}}) {
            const auto bytes = dwq(png({{"IHDR", ihdr(2, 2, 2)}, {"IDAT", zlib_stored(raw)}, {"IEND", {}}}));
            success(nnn::png::decode_dwq8a(bytes), 2, 2, 2, expected);
        }
    });
    tests.run("failed decodes do not affect a previously owned successful result", [&] {
        const auto result = nnn::png::decode_dwq8a(basic);
        for (unsigned i = 0; i < 32U; ++i) {
            auto chunks = base_chunks(); chunks[1].data.back() ^= 1U;
            failure(nnn::png::decode_dwq8a(dwq(png(chunks))), ErrorCode::InvalidFormat);
        }
        success(result, 2, 2, 6, fixed_rgba);
    });
    tests.run("ordinary-file adapter rereads and separates missing and unreadable inputs", [&] {
        const auto work = std::filesystem::path(NNN_PNG_TEST_WORK_DIR);
        std::filesystem::create_directories(work);
        const auto path = work / "fresh-png.dwq";
        auto write = [&](const Bytes& bytes) {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            require(static_cast<bool>(file), "create synthetic file");
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            require(static_cast<bool>(file), "write synthetic file");
        };
        write(basic); const auto retained = nnn::png::load_dwq8a(path.string());
        success(retained, 2, 2, 6, fixed_rgba);
        write(Bytes{1, 2, 3}); failure(nnn::png::load_dwq8a(path.string()), ErrorCode::InvalidFormat);
        success(retained, 2, 2, 6, fixed_rgba);
        const auto absent = work / "never-created.dwq";
        std::error_code ignored; std::filesystem::remove(absent, ignored);
        failure(nnn::png::load_dwq8a(absent.string()), ErrorCode::NotFound);
        failure(nnn::png::load_dwq8a(work.string()), ErrorCode::ReadFailed);
    });
    tests.run("public status and typed names remain self-contained", [&] {
        const nnn::png::Status ok;
        require(static_cast<bool>(ok), "default status is Ok");
        for (ErrorCode code : {ErrorCode::Ok, ErrorCode::NotFound, ErrorCode::ReadFailed, ErrorCode::UnsupportedFormat, ErrorCode::InvalidFormat, ErrorCode::LimitExceeded})
            require(nnn::png::error_name(code) != nullptr && !std::string(nnn::png::error_name(code)).empty(), "every status has a name");
    });

    std::cout << "PNG RESULT " << tests.passed << " passed, " << tests.failed << " failed\n";
    return tests.failed == 0 ? 0 : 1;
}
