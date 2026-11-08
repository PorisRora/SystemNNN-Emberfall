#include "nnn/image.hpp"

static_assert(sizeof(nnn::image::Byte) == 1, "Image byte is one octet");
static_assert(nnn::image::max_dimension == 8192U, "Image dimension resource limit");
static_assert(nnn::image::max_pixels == 16U * 1024U * 1024U, "Image pixel resource limit");
static_assert(nnn::image::max_input_bytes == 64U * 1024U * 1024U, "Image file resource limit");
