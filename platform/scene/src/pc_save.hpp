#pragma once

#include "nnn/script.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace emberfall::scene::pc {

inline constexpr std::size_t max_save_bytes=16U*1024U*1024U;
inline constexpr std::size_t layer_count=32;
inline constexpr std::size_t vm_count=4;
inline constexpr std::size_t message_limit=64;
inline constexpr std::size_t log_limit=256;
inline constexpr std::uint32_t first_extension_code=100;

struct Header {
    std::uint32_t size=0, code=0, dummy1=0, dummy2=0;
    std::string name;
};

struct Info {
    int data_count=0, version=0, extension_count=0;
    bool has_omake=false, has_cutin=false, has_log=false;
};

struct DateTime {
    int year=0, month=0, day=0, weekday=0;
    int hour=0, minute=0, second=0, millisecond=0;
};

struct VmSlot {
    int script_number=0, pc=0, stack_pointer=0;
    // Positions 253 and 254 carry the SCRIPT and COMMAND call-table indices.
    std::array<std::int32_t,255> call_stack{};
};

struct Status {
    DateTime saved_at;
    int game_month=0, game_day=0, game_hour=0, game_weekday=0;
    int game_mode=0, save_count=0, adjust_version=0, run_mode=0;
    std::array<VmSlot,vm_count> vm{};
    int last_message_id=0, message_count=0, message_sub_mode=0, message_serial=0;
    int last_select_id=0, select_count=0, select_add_count=0, select_serial=0;
    int select_time_limit=0, auto_select=0, select_time_count=0, special_flag=0;
    int bgm_number=-1, bgm_plays=0, bgm_volume=0; // PC BGM number is zero-based.
    std::array<std::int32_t,4> bgm_params{};
    std::array<std::uint8_t,4> voice_exist_count{};
    int window_number=0, cursor_number=0, mouse_number=0, font_type=0;
    bool window_visible=false;
    int frame_ms=0, remaining=0, last_heroine=0, rename_layer=0;
    std::uint32_t layer_off=0;
    // The legacy 16-layer layout uses signed shorts. In a 32-layer game,
    // low and high signed bytes address layers 0..15 and 16..31 respectively.
    std::array<std::uint16_t,16> layer_variables{};
    int config_mask=0, skip_film=0, auto_message=0, cannot_click=0, cannot_skip=0;
    std::array<std::array<std::int32_t,16>,8> loop_sound{}, loop_voice{};
    std::array<std::string,8> loop_voice_names;
    std::string mini_comment, surname, given_name, script_filename;
    std::array<nnn::Byte,16> guid{};
    int large_comment_flag=0;
    std::array<std::string,4> large_comments, log_messages;
};

// Original Windows RECT storage. Effects normally treat right/bottom as size.
struct Rect { int left=0, top=0, right=0, bottom=0; };

struct Effect {
    int flag=0, picture=0, command=0;
    Rect src,dst,src_from,dst_from,src_to,dst_to;
    int count=0, count_max=0;
    std::array<std::int32_t,224> parameters{};
    bool not_transparent=false;
    int previous_effect=0;
};

struct Thumbnail {
    int width=0, height=0;
    std::vector<std::uint32_t> pixels; // Raw PC 32-bit screen colors.
};

struct LogEntry {
    std::string text;
    std::uint32_t color=0;
    std::array<std::string,8> voices;
};

enum class LogLayout { Legacy, MultiVoice };

struct Log {
    LogLayout layout=LogLayout::Legacy;
    int next=0, count=0, voice_count=1, tail=0;
    std::vector<LogEntry> entries; // Chronological, excluding unused ring slots.
};

struct UnsupportedBlock {
    Header header;
    std::vector<nnn::Byte> payload; // Does not include the common 32-byte header.
};

struct Save {
    Header header;
    Info info;
    Status status;
    Thumbnail thumbnail;
    std::vector<std::int32_t> vars;
    std::array<Effect,layer_count> effects;
    std::array<std::string,layer_count> filenames;
    std::array<std::string,message_limit> messages;
    std::optional<Log> log;
    std::vector<std::string> replacements;
    std::vector<UnsupportedBlock> unsupported_blocks;
};

// Throws std::runtime_error for malformed or unrecognized layouts. Strings
// retain the original game encoding. EXT numbers have no intrinsic payload
// type, so supply replacement_code only for a confirmed okikae block.
Save parse_save(const std::vector<nnn::Byte>& bytes,std::string_view expected_header="SAVEDATA",
                std::optional<std::uint32_t> replacement_code={});
std::vector<nnn::Byte> write_save(const Save& save);

// A chart checkpoint starts with this standalone code 2 block. Read both
// original widths and write the current 8112-byte layout.
Status parse_status_block(const nnn::Byte* bytes,std::size_t size);
std::vector<nnn::Byte> write_status_block(const Status& status);

} // namespace emberfall::scene::pc
