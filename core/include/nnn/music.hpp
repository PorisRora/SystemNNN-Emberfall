#pragma once

#include "nnn/script.hpp"

#include <array>
#include <functional>

namespace nnn {

struct MusicState {
    Value music_switch = 1;
    Value current_music = -1;
    Value last_music = -1;
    Value target_volume = 0;
    Value volume_data = 50;
    Word active_slot = 0;
    // Controller object presence, independent of logical track/playing state.
    std::array<bool, 2> output_present{{false, false}};
};

// StopBGM boundary: active object, fade=false, fade_time=0. The source ignores
// StopBGM's bool return, including false for an absent worker thread.
using MusicStopOutput = std::function<bool(Word active_slot, bool fade, Value fade_time)>;

// Stop-only host logic. No playback, volume mutation API or audio backend.
// Single-threaded; read State() during output to observe the source write order.
class MusicStopController final {
public:
    MusicStopController() = default;
    MusicStopController(const MusicStopController&) = delete;
    MusicStopController& operator=(const MusicStopController&) = delete;
    MusicStopController(MusicStopController&&) = delete;
    MusicStopController& operator=(MusicStopController&&) = delete;
    const MusicState& State() const noexcept;
    Status SetState(MusicState state);
    // Missing output for a present active object rejects before state writes.
    // Output exceptions propagate; target is already0, track IDs remain old.
    Status StopNow(SkipFlags flags, const MusicStopOutput& output = {});

private:
    MusicState state_{};
    bool busy_ = false;
};

} // namespace nnn
