#pragma once

#include "nnn/script.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace nnn::detail {

inline constexpr Word identify_data = 0x66660001;
inline constexpr Word identify_control = 0x66660002;
inline constexpr Word identify_command = 0x66660003;
inline constexpr Word identify_function = 0x66660004;
inline constexpr Word identify_calcu = 0x66660005;
inline constexpr Word identify_systemcommand = 0x66660006;
inline constexpr Word identify_systemfunction = 0x66660007;
inline constexpr Word data_header = 0x55550001;
inline constexpr Word data_table = 0x55550002;
inline constexpr Word control_nop = 0x55560001;
inline constexpr Word control_if = 0x55560002;
inline constexpr Word control_elsif = 0x55560003;
inline constexpr Word control_else = 0x55560004;
inline constexpr Word control_end = 0x55560005;
inline constexpr Word control_next = 0x55560006;
inline constexpr Word control_return = 0x55560007;
inline constexpr Word control_sub = 0x55560009;
inline constexpr Word control_script = 0x5556000a;
inline constexpr Word control_subscript = 0x55560016;
inline constexpr Word control_case = 0x55560008;
inline constexpr Word control_exit = 0x5556000b;
inline constexpr Word control_endfilm = 0x5556000c;
inline constexpr Word control_endkoma = 0x5556000e;
inline constexpr Word control_while = 0x55560011;
inline constexpr Word control_endif = 0x55560013;
inline constexpr Word control_scriptjump = 0x55560015;
inline constexpr Word control_goto = 0x55560020;
inline constexpr Word control_open = 0xa000007b;
inline constexpr Word control_close = 0xa000007d;
inline constexpr std::size_t max_block_depth = 64;
inline constexpr Word command_print = 0x22220001;
inline constexpr Word command_lprint = 0x22220002;
inline constexpr Word command_append = 0x22220003;
inline constexpr Word command_select = 0x22220006;
inline constexpr Word command_draw = 0x22220004;
inline constexpr Word command_overrap = 0x22220005;
inline constexpr Word function_storyname = 0x33330051;
inline constexpr Word function_filmname = 0x33330050;
inline constexpr Word function_filmtypetime = 0x33330052;
inline constexpr Word function_music = 0x33330007;
inline constexpr Word function_voice = 0x33330008;
inline constexpr Word function_volumeonly_voice = 0x33330069;
inline constexpr Word function_prepareoverrap = 0x33330011;
inline constexpr Word function_clearalleffect = 0x33330012;
inline constexpr Word function_seteffect = 0x33330013;
inline constexpr Word function_cleareffectlayer = 0x33330014;
inline constexpr Word function_loaddwq = 0x33330015;
inline constexpr Word function_seteffectrect = 0x33330016;
inline constexpr Word function_nomessagewindow = 0x33330017;
inline constexpr Word function_startmessage = 0x3333001b;
inline constexpr Word function_startkoma = 0x3333001c;
inline constexpr Word function_preloaddwq = 0x3333001f;
inline constexpr Word table_label = 0x44440003;
inline constexpr Word user_command_return = 0x11110001;
inline constexpr Word token_var = 0x80000000;
inline constexpr Word token_number = 0x90000000;
inline constexpr Word token_number32 = 0x91000000;
inline constexpr Word token_expression = 0xa0000000;
inline constexpr Word token_assign = 0xa000003d;
inline constexpr Word token_equal = 0xa300003d;
// Exact upstream Enzan classes, not operator-spelling-derived aliases:
// SCRIPT_CALCU_ENZAN, SCRIPT_CALCU_COMPARE, SCRIPT_CALCU_COMPAREEQUAL.
inline constexpr Word token_add = 0xa000002b;
inline constexpr Word token_subtract = 0xa000002d;
inline constexpr Word token_multiply = 0xa000002a;
inline constexpr Word token_less = 0xa400003c;
inline constexpr Word token_greater = 0xa400003e;
inline constexpr Word token_less_equal = 0xa500003c;
inline constexpr Word token_greater_equal = 0xa500003e;
inline constexpr Word token_not_equal = 0xa5000021;

struct Record {
    WordIndex pc;
    Word length;
    Word identify;
    Word code;
    Word command_call_slot=no_word;
};

inline constexpr bool has_string_argument(Word code) noexcept {
    return code == function_storyname || code == function_filmname ||
           code == function_loaddwq || code == function_preloaddwq;
}

struct TextTable {
    std::vector<char> bytes;
    std::vector<std::size_t> offsets;
    bool get(Word index, std::string& result) const {
        if (index >= offsets.size()) return false;
        result.assign(bytes.data() + offsets[index]);
        return true;
    }
};

struct Program {
    std::vector<Word> words;
    std::uint64_t fingerprint=0;
    std::vector<Record> records;
    // An index at each record start; no_word elsewhere.
    std::vector<WordIndex> record_index;
    TextTable messages, strings;
    std::vector<Value> constants;
    std::vector<WordIndex> labels, film_koma_offsets, koma_targets;
    std::array<std::vector<Word>, 4> adjustments; // SUB, SELECT, COMMAND, SCRIPT
    // A fallthrough at the end of a WHILE body repeats it unless the compiler supplied a back jump.
    std::vector<WordIndex> loop_back;
    // Invocation-time value snapshot, never a borrowed registry pointer.
    bool has_function_registry = false;
    std::size_t function_registry_size = 0;
    Word stop_f5_id = no_word;
    ScriptControlProfile script_control_profile = ScriptControlProfile::Closed;
};

// Avoid implementation-defined unsigned-to-signed conversions.
inline Value value_from_word(Word word) noexcept {
    if (word <= INT32_MAX) return static_cast<Value>(word);
    return -1 - static_cast<Value>(UINT32_MAX - word);
}

inline Value short_value(Word token) noexcept {
    const Word value = token & 0xffffU;
    return value < 0x8000U ? static_cast<Value>(value) : static_cast<Value>(value) - 0x10000;
}

Status validate_program(Program& program);

} // namespace nnn::detail
