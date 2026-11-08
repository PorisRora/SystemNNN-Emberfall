#include "nnn/music.hpp"

namespace nnn {
namespace {
class StopGuard {
public:
    explicit StopGuard(bool& busy) : busy_(busy) { busy_ = true; }
    ~StopGuard() { busy_ = false; }
    StopGuard(const StopGuard&) = delete;
    StopGuard& operator=(const StopGuard&) = delete;
private:
    bool& busy_;
};
} // namespace

const MusicState& MusicStopController::State() const noexcept { return state_; }

Status MusicStopController::SetState(MusicState state) {
    if (busy_) return {ErrorCode::Busy, no_word, "Music stop operation already active"};
    if (state.active_slot >= state.output_present.size()) {
        return {ErrorCode::InvalidFormat, no_word, "Music active slot must be0 or1"};
    }
    state_ = state;
    return {};
}

Status MusicStopController::StopNow(SkipFlags flags, const MusicStopOutput& output) {
    if (busy_) return {ErrorCode::Busy, no_word, "Music stop operation already active"};
    StopGuard guard(busy_);
    if (flags.skip_next_command && flags.skip_effect_command) return {};
    if (state_.music_switch == 0) {
        state_.current_music = -1;
        state_.last_music = -1;
        return {};
    }
    const Word active = state_.active_slot;
    if (active >= state_.output_present.size()) {
        return {ErrorCode::InvalidFormat, no_word, "Music active slot must be0 or1"};
    }
    const bool present = state_.output_present[active];
    if (present && !output) {
        return {ErrorCode::HostRejected, no_word, "Present music output requires an explicit stop adapter"};
    }
    // Preserve controller entry -> StopBGM boundary -> host return write order.
    state_.target_volume = 0;
    if (present) {
        // CMusicControl::StopMusic ignores the original BOOL result, including
        // false for a backend without a worker. Exceptions remain observable.
        (void)output(active, false, 0);
    }
    state_.current_music = -1;
    state_.last_music = -1;
    return {};
}

} // namespace nnn
