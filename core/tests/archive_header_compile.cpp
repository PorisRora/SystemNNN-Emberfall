#include "nnn/archive.hpp"

static_assert(nnn::archive::max_entries > 0U);
static_assert(sizeof(nnn::archive::Byte) == 1U);

namespace {
[[maybe_unused]] nnn::archive::Index default_archive_index;
}
