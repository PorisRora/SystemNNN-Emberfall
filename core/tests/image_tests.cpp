#include <nnn/image.hpp>
#include <nnn/dwq.hpp>

#include "images/fixtures/expected_cases.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using nnn::image::Byte;
using nnn::image::ErrorCode;
using nnn::image::ImageResult;
using Bytes = std::vector<Byte>;
using Color = std::array<Byte, 3>;

void require(bool condition, const std::string& description) {
    if (!condition) throw std::runtime_error(description);
}

struct Tests {
    int passed = 0;
    int failed = 0;
    void run(const std::string& name, const std::function<void()>& body) {
        try {
            body();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
};

void put16(Bytes& bytes, std::size_t at, std::uint16_t value) {
    bytes.at(at) = static_cast<Byte>(value & 255U);
    bytes.at(at + 1U) = static_cast<Byte>((value >> 8U) & 255U);
}

void put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32U; shift += 8U) {
        bytes.at(at + shift / 8U) = static_cast<Byte>((value >> shift) & 255U);
    }
}

// These helpers write the independently specified frozen profile. Pixel and
// palette values below are fixed test data; no decoder or upstream output is
// used to construct either the expected colors or the bytes.
Bytes header(std::uint32_t width, std::uint32_t height, std::uint16_t depth,
             std::uint32_t palette_count, bool negative_height) {
    const std::size_t row = (static_cast<std::size_t>(width) * (depth / 8U) + 3U) & ~std::size_t{3};
    const std::size_t palette_bytes = static_cast<std::size_t>(palette_count) * 4U;
    Bytes bytes(118U + palette_bytes + row * height, Byte{0});
    std::fill(bytes.begin(), bytes.begin() + 64, Byte{' '});
    bytes[0] = 'B'; bytes[1] = 'M'; bytes[2] = 'P';
    const std::string tag = "PACKTYPE=0";
    std::copy(tag.begin(), tag.end(), bytes.begin() + 48);
    put32(bytes, 36, width);
    put32(bytes, 40, height);
    bytes[64] = 'B'; bytes[65] = 'M';
    put32(bytes, 66, static_cast<std::uint32_t>(bytes.size() - 64U));
    put32(bytes, 74, 54U + palette_count * 4U);
    put32(bytes, 78, 40U);
    put32(bytes, 82, width);
    put32(bytes, 86, negative_height ? 0U - height : height);
    put16(bytes, 90, 1U);
    put16(bytes, 92, depth);
    put32(bytes, 110, palette_count);
    return bytes;
}

Bytes rgb24(std::uint32_t width, std::uint32_t height, const Bytes& colors,
            bool negative_height = false) {
    require(colors.size() == static_cast<std::size_t>(width) * height * 3U, "fixed RGB data size");
    Bytes bytes = header(width, height, 24U, 0U, negative_height);
    const std::size_t active = static_cast<std::size_t>(width) * 3U;
    const std::size_t row = (active + 3U) & ~std::size_t{3};
    for (std::size_t y = 0; y < height; ++y) {
        const auto start = bytes.begin() + static_cast<std::ptrdiff_t>(118U + y * row);
        std::fill(start, start + static_cast<std::ptrdiff_t>(row), Byte{0xA5});
        std::copy_n(colors.begin() + static_cast<std::ptrdiff_t>(y * active), active, start);
    }
    return bytes;
}

Bytes indexed8(std::uint32_t width, std::uint32_t height,
               const std::vector<Color>& palette, const Bytes& indices,
               bool negative_height = false) {
    require(indices.size() == static_cast<std::size_t>(width) * height, "fixed index data size");
    Bytes bytes = header(width, height, 8U, static_cast<std::uint32_t>(palette.size()), negative_height);
    for (std::size_t i = 0; i < palette.size(); ++i) {
        bytes[118U + i * 4U] = palette[i][2];
        bytes[119U + i * 4U] = palette[i][1];
        bytes[120U + i * 4U] = palette[i][0];
        bytes[121U + i * 4U] = 0xF3; // Ignored reserved byte, not alpha.
    }
    const std::size_t row = (static_cast<std::size_t>(width) + 3U) & ~std::size_t{3};
    const std::size_t pixel_start = 118U + palette.size() * 4U;
    for (std::size_t y = 0; y < height; ++y) {
        const auto start = bytes.begin() + static_cast<std::ptrdiff_t>(pixel_start + y * row);
        std::fill(start, start + static_cast<std::ptrdiff_t>(row), Byte{0xD7});
        std::copy_n(indices.begin() + static_cast<std::ptrdiff_t>(y * width), width, start);
    }
    return bytes;
}

Bytes packed_bmp(std::uint32_t width,std::uint32_t height,const std::vector<Color>& palette,const Bytes& records) {
    auto bmp=indexed8(width,height,palette,Bytes(std::size_t{width}*height));
    bmp.erase(bmp.begin(),bmp.begin()+64);
    // Original packed BMP fields may still describe the uncompressed rows.
    bmp.resize(54U+4U*palette.size());
    bmp.insert(bmp.end(),records.begin(),records.end());
    return bmp;
}

Bytes masked_dwq(Bytes color,const Bytes& mask,char type) {
    color[57]=static_cast<Byte>(type);
    color.insert(color.end(),mask.begin(),mask.end());
    return color;
}

void dwq_failure(const Bytes& bytes,ErrorCode code=ErrorCode::InvalidFormat) {
    const auto result=nnn::dwq::decode(bytes);
    require(!result.status && result.status.code==code,"rejected malformed DWQ with specific status");
    require(result.image.width==0 && result.image.height==0 && result.image.rgba.empty(),
            "malformed DWQ returned partial pixels");
}

void success(const ImageResult& result, std::uint32_t width, std::uint32_t height,
             std::uint16_t depth, const Bytes& expected) {
    require(static_cast<bool>(result.status), std::string("unexpected ") + nnn::image::error_name(result.status.code)
            + ": " + result.status.detail);
    require(result.image.width == width && result.image.height == height, "dimensions");
    require(result.image.source_bit_depth == depth, "source bit depth");
    require(result.image.rgb == expected, "complete manually fixed RGB pixels");
    require(result.image.rgb.size() == static_cast<std::size_t>(width) * height * 3U, "tight RGB8 layout");
}

void failure(const ImageResult& result, ErrorCode expected) {
    require(!static_cast<bool>(result.status), "failure must have false status");
    require(result.status.code == expected, std::string("expected ") + nnn::image::error_name(expected)
            + ", got " + nnn::image::error_name(result.status.code) + ": " + result.status.detail);
    require(result.image.width == 0U && result.image.height == 0U && result.image.source_bit_depth == 0U
            && result.image.rgb.empty(), "failed result contains no partial image");
}

Bytes read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "open fixture " + path.string());
    return Bytes(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void write(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(stream), "open test file for writing");
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(stream), "write test file");
}

} // namespace

int main() {
    Tests tests;
    const Bytes corners{255, 0, 0, 0, 255, 0, 0, 0, 255, 240, 191, 23};
    const Bytes basic24 = rgb24(2U, 2U, corners);
    const std::vector<Color> palette{{240, 1, 7}, {3, 220, 11}, {13, 17, 245}};
    const Bytes indexed_pixels{240, 1, 7, 3, 220, 11, 13, 17, 245, 240, 1, 7};
    const Bytes basic8 = indexed8(2U, 2U, palette, Bytes{0, 1, 2, 0});

    tests.run("unified DWQ keeps type-0 RGB order and unmasked opacity",[&] {
        const auto result=nnn::dwq::decode(basic24);
        require(result.status && !result.image.has_mask && result.image.width==2 && result.image.height==2,
                "unmasked type-0 layout");
        require(result.image.rgba==Bytes{255,0,0,255,0,255,0,255,0,0,255,255,240,191,23,255},
                "type-0 physical order and opacity");
    });
    tests.run("legacy type-0 accepts oversized BMP declarations with complete rows",[&] {
        auto bytes=basic24;
        put32(bytes,66,72U); // 54-byte header and 16 padded row bytes, plus two.
        put32(bytes,98,18U);
        failure(nnn::image::decode_dwq(bytes),ErrorCode::InvalidFormat);
        const auto result=nnn::dwq::decode(bytes);
        require(result.status && result.image.rgba==nnn::dwq::decode(basic24).image.rgba,
                "legacy header sizes do not alter the decoded pixels");
        auto bad_offset=bytes;
        put32(bad_offset,74,55U);
        dwq_failure(bad_offset);
        auto truncated=bytes;
        truncated.pop_back();
        dwq_failure(truncated);
        truncated.resize(bytes.size()-8U); // One full padded row.
        dwq_failure(truncated);
    });
    tests.run("text-only type-0 header takes dimensions from the indexed DIB",[&] {
        auto bytes=basic8;
        const std::string header="IF PACKTYPE==0  CUT THIS 64 BYTETHEN REMAKE BMP PACKTYPE=0      ";
        require(header.size()==64U,"legacy text prefix extent");
        std::copy(header.begin(),header.end(),bytes.begin());
        success(nnn::image::decode_dwq(bytes),2U,2U,8U,indexed_pixels);

        put32(bytes,66,static_cast<std::uint32_t>(bytes.size()-62U)); // Two-byte bfSize surplus.
        failure(nnn::image::decode_dwq(bytes),ErrorCode::InvalidFormat);
        const auto decoded=nnn::dwq::decode(bytes);
        require(decoded.status && decoded.image.width==2U && decoded.image.height==2U &&
                decoded.image.rgba==nnn::dwq::decode(basic8).image.rgba,"legacy DWQ indexed row order and opacity");

        auto corrupt=bytes;
        corrupt[29]='X';
        dwq_failure(corrupt,ErrorCode::UnsupportedFormat);
        corrupt=bytes;
        put32(corrupt,82,0U);
        dwq_failure(corrupt);
        bytes.pop_back();
        dwq_failure(bytes);
    });
    tests.run("type 2 keeps physical RGB rows and reads the independent mask red palette",[&] {
        const std::vector<Color> colors{{0,255,20},{90,13,77},{255,8,22}};
        const auto mask=packed_bmp(2,2,colors,Bytes{1,0,1,3,1});
        auto bytes=masked_dwq(basic24,mask,'2');
        put32(bytes,32,0xfefefefeU); // Size field is not used by the uncompressed color segment.
        put32(bytes,66,72U);
        put32(bytes,98,18U);
        for(std::size_t at=16;at<32;++at)bytes[at]=0xff;
        const Bytes expected{255,0,0,90,0,255,0,0,0,0,255,255,240,191,23,90};
        const auto result=nnn::dwq::decode(bytes);
        require(result.status && result.image.has_mask && result.image.rgba==expected,
                "packed mask alpha is palette red and uses the previous row");
        require(result.image.margins.left==0 && result.image.margins.right==0,
                "non-A header bytes did not become margins");
        bytes[58]='A';
        put32(bytes,16,0);put32(bytes,20,1);put32(bytes,24,0);put32(bytes,28,1);
        const auto cut=nnn::dwq::decode(bytes);
        require(cut.status && cut.image.margins.left==1 && cut.image.margins.bottom==1 && cut.image.rgba==expected,
                "A stores validated margins without discarding the RGBA canvas");
        put32(bytes,24,1);
        dwq_failure(bytes);
    });
    tests.run("type 3 independently expands packed color and alpha and validates their boundaries",[&] {
        const std::vector<Color> colors{{17,34,51},{200,3,9},{15,185,33},{55,60,233}};
        const std::vector<Color> alpha{{0,99,70},{31,190,200},{209,2,10},{255,1,0}};
        const auto rgb=packed_bmp(4,2,colors,Bytes{1,2,0,1,1,0,1,3,0,1,2});
        const auto mask=packed_bmp(4,2,alpha,Bytes{0,1,1,2,3,3,3,3,3});
        auto head=header(4,2,8,4,false);head.resize(64);
        put32(head,32,static_cast<std::uint32_t>(rgb.size()));
        head.insert(head.end(),rgb.begin(),rgb.end());
        const auto bytes=masked_dwq(head,mask,'3');
        const auto result=nnn::dwq::decode(bytes);
        const Bytes expected{200,3,9,0,15,185,33,31,17,34,51,209,200,3,9,255,
                             200,3,9,255,200,3,9,209,17,34,51,31,55,60,233,0};
        require(result.status && result.image.rgba==expected,"separate color and alpha XOR streams");

        auto invalid=bytes;
        put32(invalid,32,static_cast<std::uint32_t>(rgb.size()+1));dwq_failure(invalid);
        invalid=bytes;put32(invalid,head.size()+18,3);dwq_failure(invalid);
        invalid=bytes;put32(invalid,head.size()+46,257);dwq_failure(invalid);
        invalid=bytes;invalid[64+54+16+3]=5;dwq_failure(invalid); // Zero run crosses a scanline.
        invalid=bytes;invalid[64+54+16]=4;dwq_failure(invalid); // Literal delta yields an invalid index.
        invalid=bytes;invalid.pop_back();dwq_failure(invalid);
        invalid=bytes;invalid.push_back(0);dwq_failure(invalid);
    });
    tests.run("old masked DWQ derives the original top and side guards",[&] {
        const auto mask=packed_bmp(7,5,{{0,100,33},{128,19,9}},Bytes{0,7,0,7,0,3,1,0,3,0,3,1,0,3,0,7});
        const auto result=nnn::dwq::decode(masked_dwq(rgb24(7,5,Bytes(7U*5U*3U,42)),mask,'2'));
        require(result.status && result.image.margins.top==1 && result.image.margins.left==2 &&
                result.image.margins.right==1 && result.image.margins.bottom==0,"CutDataByMask guard edges");
        require(result.image.rgba[(2U*7U+3U)*4U+3U]==128 && result.image.rgba[(3U*7U+3U)*4U+3U]==0,
                "only the nonzero mask sample is visible");
    });

    for (const auto& fixture : dw005_fixture::fixtures) {
        tests.run(std::string("fixed fixture ") + fixture.name, [&] {
            const Bytes bytes = read(std::filesystem::path(NNN_IMAGE_FIXTURE_DIR) / fixture.binary_filename);
            success(nnn::image::decode_dwq(bytes), fixture.width, fixture.height,
                    fixture.source_bit_depth, fixture.expected_rgb);
        });
    }

    tests.run("independent 24-bit corners preserve R/G/B and physical top row", [&] {
        success(nnn::image::decode_dwq(basic24), 2U, 2U, 24U, corners);
    });
    tests.run("negative BMP height does not trigger a second row inversion", [&] {
        success(nnn::image::decode_dwq(rgb24(2U, 2U, corners, true)), 2U, 2U, 24U, corners);
    });
    tests.run("non-gray palette expands BGR quads and ignores reserved bytes", [&] {
        success(nnn::image::decode_dwq(basic8), 2U, 2U, 8U, indexed_pixels);
    });
    tests.run("8-bit negative height preserves physical top row", [&] {
        success(nnn::image::decode_dwq(indexed8(2U, 2U, palette, Bytes{0, 1, 2, 0}, true)),
                2U, 2U, 8U, indexed_pixels);
    });
    tests.run("opaque DWQ word at byte 32 is not a type-0 payload length", [&] {
        Bytes bytes = basic24;
        put32(bytes, 32U, 0xFEDCBA98U);
        success(nnn::image::decode_dwq(bytes), 2U, 2U, 24U, corners);
    });
    tests.run("declared image size may be exact padded row bytes", [&] {
        Bytes bytes = basic24;
        put32(bytes, 98U, 16U);
        success(nnn::image::decode_dwq(bytes), 2U, 2U, 24U, corners);
    });
    tests.run("resolution metadata is ignored", [&] {
        Bytes bytes = basic24;
        put32(bytes, 102U, 0xFFFFFFFFU);
        put32(bytes, 106U, 0x80000000U);
        success(nnn::image::decode_dwq(bytes), 2U, 2U, 24U, corners);
    });
    tests.run("palette importance is metadata within palette count", [&] {
        Bytes bytes = basic8;
        put32(bytes, 114U, 3U);
        success(nnn::image::decode_dwq(bytes), 2U, 2U, 8U, indexed_pixels);
    });
    tests.run("zero palette count means all 256 entries", [&] {
        std::vector<Color> full(256U, Color{0, 0, 0});
        full[0] = Color{11, 22, 33};
        full[255] = Color{240, 3, 129};
        Bytes bytes = indexed8(2U, 1U, full, Bytes{255, 0});
        put32(bytes, 110U, 0U);
        success(nnn::image::decode_dwq(bytes), 2U, 1U, 8U, Bytes{240, 3, 129, 11, 22, 33});
    });
    tests.run("successful pixel vector owns its storage", [&] {
        Bytes bytes = basic24;
        const ImageResult image = nnn::image::decode_dwq(bytes);
        std::fill(bytes.begin(), bytes.end(), Byte{0});
        success(image, 2U, 2U, 24U, corners);
    });
    tests.run("every truncated prefix of a valid 24-bit file is invalid", [&] {
        for (std::size_t size = 0; size < basic24.size(); ++size) {
            failure(nnn::image::decode_dwq(Bytes(basic24.begin(), basic24.begin() + static_cast<std::ptrdiff_t>(size))),
                    ErrorCode::InvalidFormat);
        }
    });
    tests.run("every truncated prefix of a small-palette file is invalid", [&] {
        for (std::size_t size = 0; size < basic8.size(); ++size) {
            failure(nnn::image::decode_dwq(Bytes(basic8.begin(), basic8.begin() + static_cast<std::ptrdiff_t>(size))),
                    ErrorCode::InvalidFormat);
        }
    });
    tests.run("DWQ signature type mask and reserved variants stay unsupported", [&] {
        for (const auto& mutation : std::vector<std::pair<std::size_t, Byte>>{
                 {0U, 'J'}, {3U, '+'}, {16U, 0U}, {31U, 'A'}, {44U, 0U}, {47U, 'X'},
                 {48U, 'p'}, {57U, '1'}, {57U, '2'}, {57U, '3'}, {57U, '5'},
                 {57U, '7'}, {57U, '8'}, {58U, 'A'}, {63U, 0U}}) {
            Bytes bytes = basic24;
            bytes[mutation.first] = mutation.second;
            failure(nnn::image::decode_dwq(bytes), ErrorCode::UnsupportedFormat);
        }
    });
    tests.run("unsupported embedded BMP signature and DIB sizes", [&] {
        Bytes bytes = basic24;
        bytes[64U] = 'Z';
        failure(nnn::image::decode_dwq(bytes), ErrorCode::UnsupportedFormat);
        for (const std::uint32_t size : {12U, 41U, 108U, 124U}) {
            bytes = basic24;
            put32(bytes, 78U, size);
            failure(nnn::image::decode_dwq(bytes), ErrorCode::UnsupportedFormat);
        }
    });
    tests.run("unsupported bit depths and compressed modes", [&] {
        for (const std::uint16_t depth : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{4},
                                         std::uint16_t{16}, std::uint16_t{32}}) {
            Bytes bytes = basic24;
            put16(bytes, 92U, depth);
            failure(nnn::image::decode_dwq(bytes), ErrorCode::UnsupportedFormat);
        }
        for (const std::uint32_t compression : {1U, 2U, 3U, 4U, 5U, 0xFFFFFFFFU}) {
            Bytes bytes = basic24;
            put32(bytes, 94U, compression);
            failure(nnn::image::decode_dwq(bytes), ErrorCode::UnsupportedFormat);
        }
    });
    tests.run("truecolor palette metadata is unsupported", [&] {
        for (const std::size_t offset : {110U, 114U}) {
            Bytes bytes = basic24;
            put32(bytes, offset, 1U);
            failure(nnn::image::decode_dwq(bytes), ErrorCode::UnsupportedFormat);
        }
    });
    tests.run("zero or negative width and zero height are invalid", [&] {
        for (const std::uint32_t width : {0U, 0xFFFFFFFFU, 0x80000000U}) {
            Bytes bytes = basic24;
            put32(bytes, 36U, width); put32(bytes, 82U, width);
            failure(nnn::image::decode_dwq(bytes), ErrorCode::InvalidFormat);
        }
        Bytes bytes = basic24;
        put32(bytes, 40U, 0U); put32(bytes, 86U, 0U);
        failure(nnn::image::decode_dwq(bytes), ErrorCode::InvalidFormat);
    });
    tests.run("huge signed dimensions and INT_MIN height have bounded limit failures", [&] {
        for (const auto& dimensions : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
                 {8193U, 1U}, {1U, 8193U}, {0x7FFFFFFFU, 0x7FFFFFFFU}, {1U, 0x80000000U}}) {
            Bytes bytes = basic24;
            put32(bytes, 36U, dimensions.first); put32(bytes, 82U, dimensions.first);
            put32(bytes, 40U, dimensions.second == 0x80000000U ? 1U : dimensions.second);
            put32(bytes, 86U, dimensions.second);
            failure(nnn::image::decode_dwq(bytes), ErrorCode::LimitExceeded);
        }
    });
    tests.run("pixel product limit precedes payload and allocation", [&] {
        Bytes bytes = basic24;
        put32(bytes, 36U, 4097U); put32(bytes, 82U, 4097U);
        put32(bytes, 40U, 4096U); put32(bytes, 86U, 4096U);
        failure(nnn::image::decode_dwq(bytes), ErrorCode::LimitExceeded);
    });
    tests.run("maximum supported width accepts a thin valid image", [&] {
        Bytes expected(static_cast<std::size_t>(nnn::image::max_dimension) * 3U, Byte{0});
        for (std::size_t i = 0; i < expected.size(); i += 3U) {
            expected[i] = 128U; expected[i + 1U] = 1U; expected[i + 2U] = 254U;
        }
        success(nnn::image::decode_dwq(rgb24(nnn::image::max_dimension, 1U, expected)),
                nnn::image::max_dimension, 1U, 24U, expected);
    });
    tests.run("maximum supported height accepts a thin negative-height image", [&] {
        Bytes expected(static_cast<std::size_t>(nnn::image::max_dimension) * 3U, Byte{31});
        success(nnn::image::decode_dwq(rgb24(1U, nnn::image::max_dimension, expected, true)),
                1U, nnn::image::max_dimension, 24U, expected);
    });
    tests.run("input limit rejects before parsing", [&] {
        failure(nnn::image::decode_dwq(Bytes(nnn::image::max_input_bytes + 1U, Byte{0})), ErrorCode::LimitExceeded);
    });
    tests.run("DWQ dimensions must positively match embedded BMP", [&] {
        for (const std::size_t offset : {36U, 40U}) {
            for (const std::uint32_t value : {0U, 3U, 0xFFFFFFFFU}) {
                Bytes bytes = basic24;
                put32(bytes, offset, value);
                failure(nnn::image::decode_dwq(bytes), ErrorCode::InvalidFormat);
            }
        }
    });
    tests.run("BMP reserved words and plane count are validated", [&] {
        for (const std::size_t offset : {70U, 72U, 90U}) {
            Bytes bytes = basic24;
            put16(bytes, offset, offset == 90U ? 2U : 1U);
            failure(nnn::image::decode_dwq(bytes), ErrorCode::InvalidFormat);
        }
    });
    tests.run("BF sizes and offsets cannot hide gaps or wrong bases", [&] {
        for (const std::uint32_t offset : {0U, 53U, 55U, 118U, 0xFFFFFFFFU}) {
            Bytes bytes = basic24;
            put32(bytes, 74U, offset);
            failure(nnn::image::decode_dwq(bytes), ErrorCode::InvalidFormat);
        }
        for (const std::uint32_t size : {0U, 69U, 71U, 134U, 0xFFFFFFFFU}) {
            Bytes bytes = basic24;
            put32(bytes, 66U, size);
            failure(nnn::image::decode_dwq(bytes), ErrorCode::InvalidFormat);
        }
    });
    tests.run("nonzero image size must include exact row padding", [&] {
        for (const std::uint32_t size : {12U, 15U, 17U, 0xFFFFFFFFU}) {
            Bytes bytes = basic24;
            put32(bytes, 98U, size);
            failure(nnn::image::decode_dwq(bytes), ErrorCode::InvalidFormat);
        }
    });
    tests.run("out-of-range palette counts and importance are invalid", [&] {
        for (const std::uint32_t count : {257U, 0xFFFFFFFFU}) {
            Bytes bytes = basic8;
            put32(bytes, 110U, count);
            failure(nnn::image::decode_dwq(bytes), ErrorCode::InvalidFormat);
        }
        Bytes bytes = basic8;
        put32(bytes, 114U, 4U);
        failure(nnn::image::decode_dwq(bytes), ErrorCode::InvalidFormat);
    });
    tests.run("last invalid palette index returns an entirely empty failure", [&] {
        Bytes bytes = basic8;
        bytes[135U] = 3U;
        failure(nnn::image::decode_dwq(bytes), ErrorCode::InvalidFormat);
        bytes[135U] = 255U;
        failure(nnn::image::decode_dwq(bytes), ErrorCode::InvalidFormat);
    });
    tests.run("one or many trailing bytes are never an implicit mask", [&] {
        for (const std::size_t extra : {1U, 4U, 17U}) {
            Bytes bytes = basic24;
            bytes.insert(bytes.end(), extra, Byte{0});
            failure(nnn::image::decode_dwq(bytes), ErrorCode::InvalidFormat);
        }
    });
    tests.run("file entry reads current bytes and distinguishes missing and directory", [&] {
        const std::filesystem::path work(NNN_IMAGE_TEST_WORK_DIR);
        std::filesystem::create_directories(work);
        const auto path = work / "fresh-image.dwq";
        const auto missing = work / "never-created-image.dwq";
        std::error_code ignored;
        std::filesystem::remove(missing, ignored);
        write(path, basic24);
        success(nnn::image::load_dwq(path.string()), 2U, 2U, 24U, corners);
        write(path, basic8);
        success(nnn::image::load_dwq(path.string()), 2U, 2U, 8U, indexed_pixels);
        write(path, Bytes{1, 2, 3});
        failure(nnn::image::load_dwq(path.string()), ErrorCode::InvalidFormat);
        failure(nnn::image::load_dwq(missing.string()), ErrorCode::NotFound);
        failure(nnn::image::load_dwq(work.string()), ErrorCode::ReadFailed);
    });
    tests.run("file input size limit is checked before allocating or reading", [&] {
        const std::filesystem::path work(NNN_IMAGE_TEST_WORK_DIR);
        std::filesystem::create_directories(work);
        const auto path = work / "over-limit.dwq";
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            require(static_cast<bool>(file), "create sparse size-limit test file");
            file.seekp(static_cast<std::streamoff>(nnn::image::max_input_bytes));
            file.put('\0');
            require(static_cast<bool>(file), "write sparse size-limit test file");
        }
        failure(nnn::image::load_dwq(path.string()), ErrorCode::LimitExceeded);
    });
    tests.run("JPEG DWQ keeps RGB and composes an independent packed mask", [&] {
        const auto core = std::filesystem::path(NNN_IMAGE_FIXTURE_DIR).parent_path().parent_path().parent_path();
        const auto jpeg = read(core / "third_party/libpng/pngbar.jpg");
        const auto color = nnn::image::decode_jpeg(jpeg.data(), jpeg.size());
        require(static_cast<bool>(color.status), "decode bundled baseline JPEG: " + color.status.detail);
        constexpr std::uint32_t width = 88U, height = 31U;
        require(color.image.width == width && color.image.height == height && color.image.source_bit_depth == 24U &&
                color.image.rgb.size() == std::size_t{width} * height * 3U, "JPEG reports tight RGB8 with the original canvas");

        auto type5 = header(width, height, 24U, 0U, false);
        type5.resize(64U);
        type5[57] = '5';
        put32(type5, 32U, static_cast<std::uint32_t>(jpeg.size()));
        type5.insert(type5.end(), jpeg.begin(), jpeg.end());
        const auto opaque = nnn::dwq::decode(type5);
        require(static_cast<bool>(opaque.status) && !opaque.image.has_mask, "type 5 is opaque");
        Bytes rows(width, Byte{1});
        for (std::uint32_t y = 1; y < height; ++y) rows.insert(rows.end(), {0U, static_cast<Byte>(width)});
        const auto type7 = masked_dwq(type5, packed_bmp(width, height, {{0,0,0}, {129,0,0}}, rows), '7');
        const auto masked = nnn::dwq::decode(type7);
        require(static_cast<bool>(masked.status) && masked.image.has_mask, "type 7 loads its independent mask");
        require(opaque.image.rgba.size() == std::size_t{width} * height * 4U &&
                masked.image.rgba.size() == opaque.image.rgba.size(), "both DWQ variants keep the full canvas");
        for (std::size_t src = 0, dst = 0; src < color.image.rgb.size(); src += 3U, dst += 4U) {
            for (std::size_t c = 0; c < 3U; ++c) {
                require(opaque.image.rgba[dst + c] == color.image.rgb[src + c] &&
                        masked.image.rgba[dst + c] == color.image.rgb[src + c], "packing keeps the original JPEG color");
            }
            require(opaque.image.rgba[dst + 3U] == 255U && masked.image.rgba[dst + 3U] == 129U,
                    "type 7 changes only alpha");
        }

        failure(nnn::image::decode_jpeg(jpeg.data(), jpeg.size() - 1U), ErrorCode::InvalidFormat);
        const std::array<Byte, 2> marker{0xff, 0xc0};
        const auto sof = std::search(jpeg.begin(), jpeg.end(), marker.begin(), marker.end());
        require(sof != jpeg.end() && jpeg.end() - sof >= 9, "fixture has a baseline SOF marker");
        const auto at = static_cast<std::size_t>(sof - jpeg.begin());
        auto oversized = jpeg;
        oversized[at + 7U] = 0x20U;
        oversized[at + 8U] = 0x01U;
        failure(nnn::image::decode_jpeg(oversized.data(), oversized.size()), ErrorCode::LimitExceeded);
    });
    tests.run("public status bool and stable error names", [&] {
        const nnn::image::Status ok;
        require(static_cast<bool>(ok), "default status is Ok");
        for (const ErrorCode code : {ErrorCode::Ok, ErrorCode::NotFound, ErrorCode::ReadFailed,
                                    ErrorCode::UnsupportedFormat, ErrorCode::InvalidFormat, ErrorCode::LimitExceeded}) {
            require(nnn::image::error_name(code) != nullptr && std::string(nnn::image::error_name(code)).size() > 0U,
                    "named public error code");
        }
    });

    std::cout << "IMAGE RESULT " << tests.passed << " passed, " << tests.failed << " failed\n";
    return tests.failed == 0 ? 0 : 1;
}
