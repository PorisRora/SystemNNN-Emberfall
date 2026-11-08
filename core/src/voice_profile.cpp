#include "nnn/voice.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace nnn {
namespace {

inline constexpr std::size_t max_profile_token_bytes = 127;

VoiceAdjustmentResult adjustment_error(const char* detail) {
    return {{ErrorCode::InvalidFormat, no_word, detail}, 0};
}

unsigned char folded_prefix_byte(const std::string& name, std::size_t index) noexcept {
    // Source compares two C-string bytes, including a one-byte name's NUL.
    const auto byte = index < name.size() ? static_cast<unsigned char>(name[index]) : 0U;
    return static_cast<unsigned char>(byte | 0x20U);
}

} // namespace

VoiceAdjustmentResult resolve_voice_adjustment(const std::string& current_name,
                                               const VoiceCharacterProfile& profile) {
    if (current_name.size() > max_voice_name_bytes || current_name.find('\0') != std::string::npos) {
        return adjustment_error("Invalid current voice name");
    }
    // The bounded empty-name policy does not inspect an unused profile.
    if (current_name.empty()) return {};
    if (profile.names.size() > max_voice_character_names) {
        return adjustment_error("Voice character profile exceeds token limit");
    }
    for (std::size_t index = 0; index < profile.names.size(); ++index) {
        const auto& name = profile.names[index];
        if (name.size() > max_profile_token_bytes || name.find('\0') != std::string::npos) {
            return adjustment_error("Invalid voice character token");
        }
        if (index >= 2U && index % 2U == 0U && name.empty()) {
            return adjustment_error("Empty voice character prefix");
        }
    }

    const auto first = folded_prefix_byte(current_name, 0);
    const auto second = folded_prefix_byte(current_name, 1);
    std::size_t character = 0;
    for (std::size_t index = 2; index < profile.names.size(); index += 2U) {
        const auto& prefix = profile.names[index];
        if (first == folded_prefix_byte(prefix, 0) && second == folded_prefix_byte(prefix, 1)) {
            character = index / 2U;
            break;
        }
    }
    if (character == 0U) return {};

    std::size_t slider_index = character;
    if (profile.config_present) {
        // Even disabled/absent/unmatched configuration falls back to cn,
        // followed by the caller's unconditional minus one.
        slider_index = character - 1U;
        if (profile.mapping_enabled && profile.mapping_table_present) {
            if (profile.button_groups.size() > max_voice_mapping_groups) {
                return adjustment_error("Voice mapping exceeds group limit");
            }
            std::size_t items = 0;
            for (const auto& group : profile.button_groups) {
                if (group.size() > max_voice_mapping_items - items) {
                    return adjustment_error("Voice mapping exceeds item limit");
                }
                items += group.size();
            }
            bool mapped = false;
            for (std::size_t row = 0; row < profile.button_groups.size() && !mapped; ++row) {
                for (const Value member : profile.button_groups[row]) {
                    if (member == static_cast<Value>(character)) {
                        // Source returns row+1, then the host subtracts1.
                        slider_index = row;
                        mapped = true;
                        break;
                    }
                }
            }
        }
    }

    constexpr std::size_t sliders_per_word = 6;
    if (slider_index >= profile.packed_sliders.size() * sliders_per_word) return {};
    const auto shift = static_cast<unsigned int>((slider_index % sliders_per_word) * 5U);
    const Word raw = (profile.packed_sliders[slider_index / sliders_per_word] >> shift) & 31U;
    const Value slider = raw < 16U ? static_cast<Value>(raw) : static_cast<Value>(raw) - 32;
    const auto product = static_cast<std::int64_t>(slider) * static_cast<std::int64_t>(profile.slider_delta);
    if (product < std::numeric_limits<Value>::min() || product > std::numeric_limits<Value>::max()) {
        return adjustment_error("Voice character adjustment exceeds signed32 range");
    }
    return {{}, static_cast<Value>(product)};
}

VoiceVolumeMapping map_voice_volume(Value source_volume) {
    VoiceVolumeMapping result;
    if (source_volume < 0 || source_volume > 100) {
        result.status = {ErrorCode::InvalidFormat, no_word, "Voice source volume must be in0..100"};
        return result;
    }
    result.directsound_centibels = source_volume * 100 - 10000;
    result.xaudio2_gain = static_cast<float>(source_volume) * 0.01F;
    result.directsound_gain = std::pow(10.0, static_cast<double>(result.directsound_centibels) / 2000.0);
    return result;
}

} // namespace nnn
