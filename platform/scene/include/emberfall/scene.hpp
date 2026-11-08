#pragma once

#include "emberfall/render.hpp"
#include "nnn/music.hpp"
#include "nnn/voice.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nnn { class FunctionRegistry; }
namespace emberfall::scene {

using Reader = std::function<bool(const std::string&, std::vector<std::uint8_t>&)>;
inline constexpr std::uint32_t first_layer = 4;
inline constexpr std::uint32_t second_layer = 5;
inline constexpr std::uint32_t nop_effect = 0;
inline constexpr std::uint32_t max_dimension = 2048;
inline constexpr std::uint64_t max_pixels = 4U * 1024U * 1024U;
inline constexpr std::size_t max_script_bytes = 256U * 1024U;
inline constexpr std::size_t max_name_bytes = 255;
inline constexpr std::size_t max_resource_bytes = 8U * 1024U * 1024U;
inline constexpr std::uint64_t max_total_resource_bytes = 16U * 1024U * 1024U;
inline constexpr std::uint64_t max_decoded_pixels = 8U * 1024U * 1024U;
inline constexpr std::uint32_t max_resource_reads = 8;
inline constexpr std::uint32_t max_callbacks = 256;
inline constexpr std::uint64_t max_script_work = 100000;

struct FrameResult {
    std::string error;
    emberfall::render::Surface surface;
    // Diagnostics are copied values, never VM/image pointers. Failure has no pixels.
    std::string story_name;
    std::uint32_t draw_pc = 0;
    std::int32_t draw_value = 0;
    std::uint32_t callbacks = 0;
    std::uint32_t resource_reads = 0;
    std::uint64_t resource_bytes = 0;
    std::uint64_t decoded_pixels = 0;
    std::uint64_t script_work = 0;
    std::uint32_t exec_calls = 0;
    // Successful blit submissions, including zero-area or fully clipped layers.
    std::uint32_t layers_drawn = 0;
    std::uint32_t child_script_reads = 0;
    std::uint64_t child_script_bytes = 0;
    std::size_t script_transitions = 0;
    std::uint32_t draw_mode = 0;
};

// Own session per call. Reader receives exact raw key bytes and must bound its own I/O.
// Returns the first nonzero DRAW yield, composed over black using Source32Mmx.
// Every error returns an empty zero-sized surface; no caller-owned pixels are modified.
FrameResult first_frame(const std::vector<std::uint8_t>& script, const Reader& read,
                        std::uint32_t width, std::uint32_t height);
FrameResult first_frame(const std::vector<std::uint8_t>& script, const Reader& read,
                        std::uint32_t width, std::uint32_t height, const nnn::FunctionRegistry* functions);
// An explicit child reader enables foreground SCRIPT/SUBSCRIPT/NEXT.
// Requests are relative spt/<raw basename>.spt paths. Each scene call owns one scheduler.
FrameResult first_frame(const std::vector<std::uint8_t>& script, const Reader& read,
                        std::uint32_t width, std::uint32_t height, const nnn::FunctionRegistry* functions,
                        const Reader& read_child);
// A caller with a music session supplies its current controller and immediate-stop backend.
FrameResult first_frame(const std::vector<std::uint8_t>& script, const Reader& read,
                        std::uint32_t width, std::uint32_t height, const nnn::FunctionRegistry* functions,
                        const Reader& read_child, nnn::MusicStopController& music,
                        const nnn::MusicStopOutput& stop_output, nnn::SkipFlags initial_skip = {});

struct VoiceSession {
    nnn::VoiceVolumeController controller;
    nnn::VoiceLiveInputs live; // The scene supplies skip_flags from its active script host.
    nnn::VoiceOutputs outputs;
    // Channels 0 and 1 use the same base volume and character table, but have different current names.
    // The single-channel current_voice_name getter remains valid for channel 0.
    std::function<std::string(nnn::Word)> current_name_for_channel;
};
FrameResult first_frame(const std::vector<std::uint8_t>& script, const Reader& read,
                        std::uint32_t width, std::uint32_t height, const nnn::FunctionRegistry* functions,
                        const Reader& read_child, nnn::MusicStopController& music,
                        const nnn::MusicStopOutput& stop_output, nnn::SkipFlags initial_skip, VoiceSession& voice);

} // namespace emberfall::scene
