#pragma once

#include "nnn/script.hpp"
#include <vector>

namespace nnn::chart {

inline constexpr std::size_t max_progress_slots=256U*1024U;
inline constexpr std::size_t selection_words(std::size_t slots) noexcept {
    return (((slots+31U)/32U)+3U)&~std::size_t{3};
}
inline constexpr std::size_t max_selection_words=selection_words(max_progress_slots);

// One byte per source node in each of the original mita/tsuka tables.
struct Progress {
    std::vector<Byte> seen,traversed;
};
struct SelectionSeed {
    std::size_t slots;
    std::vector<Word> enabled;
};

} // namespace nnn::chart
