#pragma once

#include "nnn/script.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace nnn {

inline constexpr std::size_t voice_channel_count = 4;
inline constexpr Value max_voice_fade_units = INT32_MAX / 100;
inline constexpr std::size_t max_voice_name_bytes = 63;
inline constexpr std::size_t max_voice_character_names = 4096;
inline constexpr std::size_t max_voice_mapping_groups = 256;
inline constexpr std::size_t max_voice_mapping_items = 65536;

struct VoiceCharacterProfile {
    // Original CNameList token order, including the initial pair and descriptions.
    std::vector<std::string> names;
    bool config_present = false;
    bool mapping_enabled = false;
    bool mapping_table_present = false;
    std::vector<std::vector<Value>> button_groups;
    std::array<Word, 12> packed_sliders{};
    Value slider_delta = 2;
};
struct VoiceAdjustmentResult { Status status; Value adjustment = 0; };
VoiceAdjustmentResult resolve_voice_adjustment(const std::string& current_name,
                                               const VoiceCharacterProfile& profile);

struct VoiceVolumeMapping {
    Status status;
    Value directsound_centibels = 0;
    float xaudio2_gain = 0.0F;
    double directsound_gain = 0.0;
};
// Explicit alternative scales, not one implicit Android percentage mapping.
VoiceVolumeMapping map_voice_volume(Value source_volume);

struct VoiceChannelState {
    std::array<Value, 16> work{};
    Value fade_count_ms = 0;
    Value fade_time_ms = 0;
    Value fade_from_volume = 0;
    Value fade_target_volume = 0;
    bool object_present = true;
    bool buffer_present = false;
    bool data_present = false;
};
struct VoiceState {
    std::array<VoiceChannelState, voice_channel_count> channels{};
    // Source NEXTFADE units, each100ms. No new NEXTFADE script opcode is enabled.
    Value next_fade_units = 0;
};
struct VoiceLiveInputs {
    std::function<SkipFlags()> skip_flags;
    std::function<Value()> base_voice_volume;
    std::function<std::string()> current_voice_name;
    std::function<VoiceAdjustmentResult(const std::string&)> character_adjust;
};
struct VoiceOutputs {
    // Physical backend boundary; source integer0..100, not an Android gain.
    std::function<void(Word channel, Value source_volume)> set_volume;
    std::function<void(Word channel)> stop;
};

struct VoiceStopHostState {
    // NULL pre-data is distinct from a present empty name.
    std::optional<std::string> pre_received_name;
    std::array<Value, voice_channel_count> no_wait_same_character{};
    std::array<bool, voice_channel_count> pooled{};
};
struct VoiceStopPolicy {
    Value auto_message = 0;
    bool effect_voice_dont_stop = false;
    bool enable_loop_voice_off_in_skip = false;
    bool enable_loop_voice_on_in_skip = false;
    bool skip_to_script_voice_stop = false;
    bool no_voice_is_default_wait = false;
};
struct VoiceStopLiveInputs {
    VoiceStopPolicy policy;
    std::function<SkipFlags()> skip_flags;
    std::function<bool()> message_skip;
    std::function<Value()> skip_mode;
    std::function<bool()> read_last_message;
    // Original SOUNDVOICESWITCH (including voice-switch fallback).
    std::function<Value()> sound_voice_switch;
    // Original totalVolumeUseFlag !=0 && TOTALSWITCH ==0, not volume==0.
    std::function<bool()> total_volume_off;
    std::function<std::string(Word channel)> current_voice_name;
};

// Owned four-channel work/fade state. No playback or voice-file loading.
// Live getters and output exceptions propagate; documented source writes remain.
class VoiceVolumeController final {
public:
    VoiceVolumeController() = default;
    VoiceVolumeController(const VoiceVolumeController&) = delete;
    VoiceVolumeController& operator=(const VoiceVolumeController&) = delete;
    VoiceVolumeController(VoiceVolumeController&&) = delete;
    VoiceVolumeController& operator=(VoiceVolumeController&&) = delete;
    const VoiceState& State() const noexcept;
    Status SetState(VoiceState state);
    Status SetWork(Word channel, Word index, Value value);
    Status SetNextFade(Value fade_units);
    // Exact host operation for script VOLUMEONLY_VOICE(channel,-999), channel0/1.
    // Bind live name/character getters to the selected channel at the call site.
    Status ApplyMinus999(Word channel, const VoiceLiveInputs& live, const VoiceOutputs& output = {});
    // Source-compatible channel0 entry.
    Status ApplyMinus999(const VoiceLiveInputs& live, const VoiceOutputs& output = {});
    const VoiceStopHostState& StopHostState() const noexcept;
    Status SetStopHostState(VoiceStopHostState state);
    // Handle the exact VOICE(-1,2) empty PreReceiveScriptData event separately.
    Status PrepareVoiceStop2();
    Status ApplyVoiceStop2(const VoiceStopLiveInputs& live, const VoiceOutputs& output = {});
    // Milliseconds; includes zero-ms ticks. Advances all four existing fades.
    Status Tick(Value elapsed_ms, const VoiceOutputs& output = {});

private:
    VoiceState state_{};
    VoiceStopHostState stop_host_state_{};
    bool busy_ = false;
    Status StopVoice2(const VoiceOutputs& output);
};

} // namespace nnn
