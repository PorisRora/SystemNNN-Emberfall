#include "nnn/voice.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace nnn {
namespace {
class VoiceGuard {
public:
    explicit VoiceGuard(bool& busy) : busy_(busy) { busy_ = true; }
    ~VoiceGuard() { busy_ = false; }
    VoiceGuard(const VoiceGuard&) = delete;
    VoiceGuard& operator=(const VoiceGuard&) = delete;
private:
    bool& busy_;
};

Status busy_status() { return {ErrorCode::Busy, no_word, "Voice controller operation already active"}; }
Status invalid(const char* detail) { return {ErrorCode::InvalidFormat, no_word, detail}; }
Status rejected(const char* detail) { return {ErrorCode::HostRejected, no_word, detail}; }
bool signed_word(std::int64_t value) noexcept { return value >= INT32_MIN && value <= INT32_MAX; }

Status validate(const VoiceState& state) {
    if (state.next_fade_units < 0 || state.next_fade_units > max_voice_fade_units) {
        return invalid("Voice next fade is outside the nonnegative safe100ms domain");
    }
    for (const auto& channel : state.channels) {
        if (channel.fade_count_ms < 0 || channel.fade_time_ms < 0 ||
            channel.fade_from_volume < 0 || channel.fade_from_volume > 100 ||
            channel.fade_target_volume < 0 || channel.fade_target_volume > 100) {
            return invalid("Invalid voice fade state");
        }
    }
    return {};
}

struct TickPlan {
    bool active = false;
    Value count = 0;
    Value time = 0;
    Value volume = 0;
    bool stop = false;
};
} // namespace

const VoiceState& VoiceVolumeController::State() const noexcept { return state_; }

Status VoiceVolumeController::SetState(VoiceState state) {
    if (busy_) return busy_status();
    const auto status = validate(state);
    if (!status) return status;
    state_ = state;
    return {};
}

Status VoiceVolumeController::SetWork(Word channel, Word index, Value value) {
    if (busy_) return busy_status();
    if (channel >= state_.channels.size() || index >= state_.channels[0].work.size()) {
        return invalid("Voice work index is outside the four-channel16-word domain");
    }
    state_.channels[channel].work[index] = value;
    return {};
}

Status VoiceVolumeController::SetNextFade(Value fade_units) {
    if (busy_) return busy_status();
    if (fade_units < 0 || fade_units > max_voice_fade_units) {
        return invalid("Voice next fade is outside the nonnegative safe100ms domain");
    }
    state_.next_fade_units = fade_units;
    return {};
}

Status VoiceVolumeController::ApplyMinus999(const VoiceLiveInputs& live, const VoiceOutputs& output) {
    return ApplyMinus999(0, live, output);
}

Status VoiceVolumeController::ApplyMinus999(Word channel_number, const VoiceLiveInputs& live, const VoiceOutputs& output) {
    if (busy_) return busy_status();
    if (channel_number > 1) return {ErrorCode::UnsupportedInstruction, no_word, "Voice volume command supports channel0/1"};
    VoiceGuard guard(busy_);
    if (!live.skip_flags) return rejected("Voice operation requires a live skip getter");
    const auto flags = live.skip_flags();
    auto& channel = state_.channels[channel_number];
    if (flags.skip_next_command || channel.work[15] == 0) {
        state_.next_fade_units = 0;
        return {};
    }
    if (!live.base_voice_volume || !live.current_voice_name || !live.character_adjust) {
        return rejected("Voice operation requires live base, name and character getters");
    }
    // The source binds a name pointer before base lookup, but reads the string
    // inside the later character lookup. Copy the live name at that later point.
    const Value base = live.base_voice_volume();
    const std::int64_t base_sum = -999LL + static_cast<std::int64_t>(base);
    if (!signed_word(base_sum)) return invalid("Voice base addition exceeds the source signed32 domain");
    const std::string name = live.current_voice_name();
    if (name.size() > max_voice_name_bytes || name.find('\0') != std::string::npos) {
        return invalid("Invalid live voice name");
    }
    const auto adjustment = live.character_adjust(name);
    if (!adjustment.status) return adjustment.status;
    const std::int64_t sum = base_sum + static_cast<std::int64_t>(adjustment.adjustment);
    if (!signed_word(sum)) return invalid("Voice character addition exceeds the source signed32 domain");
    const Value volume = static_cast<Value>(std::clamp<std::int64_t>(sum, 0, 100));
    const Value fade_units = state_.next_fade_units;
    if (fade_units == 0) {
        if (!channel.object_present) return rejected("Voice controller object is absent");
        if (channel.buffer_present && !output.set_volume) return rejected("Voice buffer requires a volume output adapter");
    }
    channel.fade_count_ms = 0;
    channel.fade_time_ms = fade_units * 100;
    if (fade_units == 0) {
        if (channel.buffer_present) output.set_volume(channel_number, volume);
    } else {
        channel.fade_from_volume = channel.fade_target_volume;
    }
    channel.fade_target_volume = volume;
    channel.work[2] = -999;
    state_.next_fade_units = 0;
    return {};
}

const VoiceStopHostState& VoiceVolumeController::StopHostState() const noexcept { return stop_host_state_; }

Status VoiceVolumeController::SetStopHostState(VoiceStopHostState state) {
    if (busy_) return busy_status();
    if (state.pre_received_name && (state.pre_received_name->size() > max_voice_name_bytes ||
                                   state.pre_received_name->find('\0') != std::string::npos)) {
        return invalid("Invalid pre-received voice name");
    }
    stop_host_state_ = std::move(state);
    return {};
}

Status VoiceVolumeController::PrepareVoiceStop2() {
    if (busy_) return busy_status();
    stop_host_state_.pre_received_name.reset();
    return {};
}

Status VoiceVolumeController::StopVoice2(const VoiceOutputs& output) {
    auto& channel = state_.channels[2];
    const Value fade_units = state_.next_fade_units;
    Value from = channel.fade_target_volume;
    if (fade_units == 0) {
        if (!channel.object_present) return rejected("Voice stop controller object is absent");
        if (channel.data_present && channel.buffer_present && !output.stop) {
            return rejected("Voice stop requires a physical stop adapter");
        }
    } else if (channel.fade_time_ms > 0 && channel.fade_count_ms < channel.fade_time_ms) {
        const auto product = (static_cast<std::int64_t>(channel.fade_target_volume) - channel.fade_from_volume) * channel.fade_count_ms;
        if (!signed_word(product)) return invalid("Voice stop interpolation exceeds the source signed32 domain");
        const auto interpolated = static_cast<std::int64_t>(channel.fade_from_volume) + product / channel.fade_time_ms;
        from = static_cast<Value>(std::clamp<std::int64_t>(interpolated, 0, 100));
    }
    channel.fade_from_volume = channel.fade_target_volume;
    channel.fade_target_volume = 0;
    channel.fade_count_ms = 0;
    channel.fade_time_ms = fade_units * 100;
    if (fade_units == 0) {
        if (channel.data_present && channel.buffer_present) output.stop(2);
    } else {
        channel.fade_from_volume = from;
    }
    channel.work[15] = 0;
    return {};
}

Status VoiceVolumeController::ApplyVoiceStop2(const VoiceStopLiveInputs& live, const VoiceOutputs& output) {
    if (busy_) return busy_status();
    VoiceGuard guard(busy_);
    stop_host_state_.no_wait_same_character[2] = 0;
    auto& channel = state_.channels[2];
    bool effect_ok = live.policy.effect_voice_dont_stop;
    if (channel.work[0] != 0 ? live.policy.enable_loop_voice_off_in_skip : live.policy.enable_loop_voice_on_in_skip) {
        effect_ok = false;
    }
    if (!live.skip_flags) return rejected("Voice stop requires live skip flags");
    bool skipping = live.skip_flags().skip_next_command;
    if (!skipping) {
        if (!live.message_skip) return rejected("Voice stop requires live message-skip state");
        skipping = live.message_skip();
    }
    if (skipping && live.policy.auto_message == 0 && !effect_ok) {
        if (channel.work[0] != 0) {
            const auto status = StopVoice2(output);
            if (!status) return status;
            channel.work[0] = 0;
        } else if (live.policy.skip_to_script_voice_stop && channel.work[1] != -1) {
            if (!live.current_voice_name) return rejected("Voice stop requires live channel names");
            const auto before_name = live.current_voice_name(0);
            if (before_name.size() > max_voice_name_bytes || before_name.find('\0') != std::string::npos) {
                return invalid("Invalid comparison voice name");
            }
            const auto current_name = live.current_voice_name(2);
            if (current_name.size() > max_voice_name_bytes || current_name.find('\0') != std::string::npos) {
                return invalid("Invalid stopped voice name");
            }
            // The source's matching-prefix block is empty; IsPlaying is commented out.
            const auto status = StopVoice2(output);
            if (!status) return status;
            channel.work[1] = -1;
            channel.work[0] = 0;
            stop_host_state_.pooled[2] = true;
        }
    }
    if (!live.skip_flags().skip_next_command) {
        if (!live.message_skip) return rejected("Voice stop requires live message-skip state");
        const bool message_skip = live.message_skip();
        bool allowed = true;
        if (message_skip && live.policy.auto_message == 0 && !effect_ok) {
            if (!live.skip_mode) return rejected("Voice stop requires live skip mode");
            if (live.skip_mode() == 1) {
                allowed = false;
            } else {
                if (!live.read_last_message) return rejected("Voice stop requires live read-message state");
                if (live.read_last_message()) allowed = false;
            }
        }
        if (allowed && live.policy.no_voice_is_default_wait) {
            if (!live.sound_voice_switch) return rejected("Voice stop requires live sound-voice switch");
            Value enabled = live.sound_voice_switch();
            if (!live.total_volume_off) return rejected("Voice stop requires live total-volume gate");
            if (live.total_volume_off()) enabled = 0;
            if (enabled == 0) allowed = false;
        }
        if (allowed) {
            const auto status = StopVoice2(output);
            if (!status) return status;
            channel.work[0] = 0;
            stop_host_state_.pooled[2] = false;
        }
    }
    state_.next_fade_units = 0;
    return {};
}

Status VoiceVolumeController::Tick(Value elapsed_ms, const VoiceOutputs& output) {
    if (busy_) return busy_status();
    VoiceGuard guard(busy_);
    if (elapsed_ms < 0) return invalid("Voice elapsed time must be nonnegative milliseconds");
    std::array<TickPlan, voice_channel_count> plans{};
    // Reject unavailable adapters and source-undefined arithmetic before any
    // mutation. Output exceptions later preserve completed source writes.
    for (std::size_t index = 0; index < state_.channels.size(); ++index) {
        const auto& channel = state_.channels[index];
        if (channel.fade_time_ms <= 0) continue;
        if (!channel.object_present) return rejected("Active voice fade has no controller object");
        const auto count = static_cast<std::int64_t>(channel.fade_count_ms) + elapsed_ms;
        if (!signed_word(count)) return invalid("Voice fade count exceeds the source signed32 domain");
        const auto local_count = std::min(count, static_cast<std::int64_t>(channel.fade_time_ms));
        const auto product = (static_cast<std::int64_t>(channel.fade_target_volume) - channel.fade_from_volume) * local_count;
        if (!signed_word(product)) return invalid("Voice fade interpolation exceeds the source signed32 domain");
        const auto interpolated = static_cast<std::int64_t>(channel.fade_from_volume) + product / channel.fade_time_ms;
        const Value volume = static_cast<Value>(std::clamp<std::int64_t>(interpolated, 0, 100));
        const bool stop = volume == 0 && channel.fade_target_volume <= 0;
        if (channel.buffer_present && !output.set_volume) return rejected("Voice buffer requires a volume output adapter");
        if (stop && channel.data_present && channel.buffer_present && !output.stop) {
            return rejected("Voice fade completion requires a stop output adapter");
        }
        plans[index] = {true, static_cast<Value>(count), count >= channel.fade_time_ms ? 0 : channel.fade_time_ms, volume, stop};
    }
    for (std::size_t index = 0; index < plans.size(); ++index) {
        const auto& plan = plans[index];
        if (!plan.active) continue;
        auto& channel = state_.channels[index];
        channel.fade_count_ms = plan.count;
        channel.fade_time_ms = plan.time;
        const Word channel_number = static_cast<Word>(index);
        if (channel.buffer_present) output.set_volume(channel_number, plan.volume);
        if (plan.stop) {
            if (channel.data_present && channel.buffer_present) output.stop(channel_number);
            channel.fade_time_ms = 0;
            channel.work[0] = 0;
            channel.work[15] = 0;
        }
    }
    return {};
}

} // namespace nnn
