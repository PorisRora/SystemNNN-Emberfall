#pragma once

#include "nnn/image.hpp"

namespace nnn::dwq {

struct Margins {
    std::uint32_t top=0,left=0,right=0,bottom=0;
};

struct Image {
    std::uint32_t width=0,height=0;
    bool has_mask=false;
    Margins margins;
    // Full, top-down canvas with straight RGBA8 pixels. Margins are metadata.
    std::vector<image::Byte> rgba;
};

struct ImageResult {
    image::Status status;
    Image image;
};

// Original DWQ envelopes with uncompressed BMP (0,2), packed indexed BMP (3),
// JPEG (5,7), and PNG (8). Every error leaves the image empty.
ImageResult decode(const std::vector<image::Byte>& bytes);

} // namespace nnn::dwq
