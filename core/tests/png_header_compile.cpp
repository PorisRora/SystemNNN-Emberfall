#include "nnn/png.hpp"

static_assert(sizeof(nnn::png::Byte) == 1);
static_assert(nnn::png::max_dimension == 8192U);
static_assert(nnn::png::max_pixels == 16777216U);
