#pragma once

#include "nnn/game_name_list.hpp"
#include "nnn/runtime_optional_progress.hpp"
#include "nnn/runtime_parameters.hpp"
#include "nnn/runtime_text.hpp"

#include <array>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace nnn {

struct RuntimeParameterRouting {
    Value movie_volume_not_use = 0;
    Value sound_voice_volume_not_use = 1;
    bool total_volume_use = false;
    bool separate_script_se = true;
};

// These select the original CCommonSystemFile filenames, not game names.
// Classic is varType0; Extended is varType1 without __SYSTEMNNN_VER2__;
// Version2 is varType1 with that build flag. Trial/network both use the t prefix.
enum class RuntimeSystemFileVersion { Automatic, Classic, Extended, Version2 };
enum class RuntimeSystemEdition { Automatic, Full, TrialOrNetwork };
struct RuntimeDefaultsOptions {
    RuntimeSystemFileVersion version = RuntimeSystemFileVersion::Automatic;
    RuntimeSystemEdition edition = RuntimeSystemEdition::Automatic;
};
using RuntimeDefaultsLog = std::function<void(const std::string&)>;
using SystemVariables = std::array<Value,1000>;
inline constexpr std::size_t script_result_count=1024, script_achievement_count=1024;
using ScriptProgress = std::array<Value,script_result_count+script_achievement_count>;

struct InitialRuntimeParameters {
    RuntimeParameters parameters;
    RuntimeParameterRouting routing;
    std::filesystem::path source;
    bool loaded_original_defaults = false;
    bool has_system_flag2 = false;
    bool legacy_volume_defaults = false;
    std::string diagnostic;
    // Source fields with dedicated methods, rather than fabricated NNNPARAM IDs.
    Value system_voice_selection = 0;
    Value message_window = 0;
    Value message_cursor = 0, mouse_cursor = 0;
    Word message_cursor_parameter = 0, mouse_cursor_parameter = 0;
    SystemVariables system_variables{};
    ScriptProgress script_progress{};
    std::shared_ptr<RuntimeOptionalProgress> optional_progress=std::make_shared<RuntimeOptionalProgress>();
    RuntimeTextState runtime_text;
    std::array<std::string,2> initial_names, game_default_names;
    std::array<bool,2> seed_empty_names{};
    bool reset_name_on_title = false;
    RuntimeReplacementTable local_replacement_defaults;
    std::array<Word, 32> voice_off{};
};

namespace runtime_defaults_detail {
inline constexpr std::size_t max_default_file_bytes = 4U * 1024U * 1024U;
inline constexpr char error_name[] = "\x83\x47\x83\x89\x81\x5b\x96\xbc";

inline Word word(const std::vector<Byte>& bytes, std::size_t offset) noexcept {
    return static_cast<Word>(bytes[offset]) |
        (static_cast<Word>(bytes[offset + 1U]) << 8U) |
        (static_cast<Word>(bytes[offset + 2U]) << 16U) |
        (static_cast<Word>(bytes[offset + 3U]) << 24U);
}
inline Value signed_word(const std::vector<Byte>& bytes, std::size_t offset) noexcept {
    const auto value = word(bytes, offset);
    return value <= static_cast<Word>(std::numeric_limits<Value>::max())
        ? static_cast<Value>(value)
        : static_cast<Value>(-1 - static_cast<Value>(~value));
}
inline Value config_number(const std::string& text, const std::string& wanted, Value fallback) {
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        std::istringstream row(line);
        std::string key;
        Value value = 0;
        if (row >> key >> value && game_config::ascii_lower(key) == game_config::ascii_lower(wanted)) return value;
    }
    return fallback;
}
inline std::optional<std::string_view> config_name(std::string_view text, std::string_view wanted) {
    std::size_t cursor=0;
    while (const auto key=game_config::name_token(text,cursor)) {
        const auto value=game_config::name_token(text,cursor);
        if (!value) break;
        if (game_config::ascii_lower(std::string(*key))==wanted) return value;
    }
    return {};
}
inline void seed_empty_names(const InitialRuntimeParameters& initial, RuntimeTextState& text) {
    if (text.surname.empty() && initial.seed_empty_names[0]) text.surname=initial.initial_names[0];
    if (text.given_name.empty() && initial.seed_empty_names[1]) text.given_name=initial.initial_names[1];
}
inline void load_local_replacements(const std::filesystem::path& root, const std::string& game,
                                    RuntimeReplacementTable& defaults) {
    const bool one_byte=config_number(game,"codeByte",2)==1;
    for (std::size_t i=0;i<defaults.size();++i) {
        std::string value=one_byte ? "replace" : "\x92\x75\x82\xab\x8a\xb7\x82\xa6\x95\xb6\x8e\x9a\x97\xf1";
        if (!one_byte) value.push_back(static_cast<char>(0x82));
        value.push_back(static_cast<char>((one_byte ? '0' : 0x4f)+i/10));
        if (!one_byte) value.push_back(static_cast<char>(0x82));
        value.push_back(static_cast<char>((one_byte ? '0' : 0x4f)+i%10));
        defaults[i]=std::move(value);
    }
    if (config_number(game,"useDefaultOkikae",0)==0) return;
    const auto data=game_config::read(root,"nya","okikae.xtx");
    std::size_t cursor=0;
    while (const auto key=game_config::name_token(data,cursor)) {
        const auto value=game_config::name_token(data,cursor);
        if (!value) break;
        if (key->empty()) continue;
        // The supplied CGameCallBack overlays every system slot from
        // SYSTEMFILE after COkikaeData reads this list, including empty slots.
        if (key->front()=='S' || key->front()=='s') continue;
        std::size_t index=0;
        const auto* end=key->data()+key->size();
        const auto number=std::from_chars(key->data(),end,index);
        if (number.ec==std::errc{} && number.ptr==end && index<defaults.size())
            defaults[index]=bound_runtime_text(*value,runtime_replacement_bytes);
    }
}
inline std::string text_field(const std::vector<Byte>& bytes,std::size_t offset,std::size_t width,std::size_t limit) {
    const auto* first=reinterpret_cast<const char*>(bytes.data()+offset);
    return bound_runtime_text(std::string_view(first,width),limit);
}
inline std::vector<Byte> read_file(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > max_default_file_bytes) throw std::runtime_error("System default archive exceeds 4 MiB");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open system default archive");
    std::vector<Byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Short system default archive read");
    char extra = 0;
    if (input.get(extra) || input.bad() || !input.eof()) throw std::runtime_error("System default archive changed while reading");
    return bytes;
}
inline bool signature(const std::vector<Byte>& bytes, std::size_t offset, const char* expected) noexcept {
    for (std::size_t i = 0; i < 15U; ++i)
        if (bytes[offset + 16U + i] != static_cast<Byte>(expected[i])) return false;
    return true;
}

// This is the known little-endian, 32-bit-int SYSTEMDATA layout in the supplied
// commonSystemFile.h. Larger/different layouts are rejected rather than read
// through C++ struct casts. Other record payloads are skipped after bounds/code
// checks. Nothing from a PC user's .sav file is opened here.
inline void decode(const std::vector<Byte>& bytes, InitialRuntimeParameters& initial) {
    if (bytes.size() < 64U || word(bytes, 0) != 64U || word(bytes, 4U) != 0U ||
        !signature(bytes, 0, "FILE INFO      ")) throw std::runtime_error("Unrecognized system default archive header");
    const auto count = word(bytes, 32U);
    constexpr std::array<Word, 16> codes{{0U, 1U, 5U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U, 16U, 17U, 18U}};
    if (count < 2U || count > codes.size()) throw std::runtime_error("Unsupported system default archive record count");
    auto candidate = initial;
    candidate.optional_progress=std::make_shared<RuntimeOptionalProgress>(*initial.optional_progress);
    std::size_t offset = 64U;
    for (std::size_t index = 1U; index < count; ++index) {
        if (bytes.size() - offset < 32U) throw std::runtime_error("Truncated system default archive record");
        const auto length = static_cast<std::size_t>(word(bytes, offset));
        const auto code = word(bytes, offset + 4U);
        if (length < 32U || length % 4U != 0U || length > bytes.size() - offset || code != codes[index])
            throw std::runtime_error("Invalid system default archive record extent/order");
        if (code == 1U) {
            if (length != 2048U || !signature(bytes, offset, "SYSTEM DATA    "))
                throw std::runtime_error("Unsupported SYSTEMDATA layout");
            constexpr std::array<std::array<Word, 2>, 18> fields{{
                {{NNNPARAM_SCREENMODE, 21U}}, {{NNNPARAM_MESSAGESPEED, 34U}}, {{NNNPARAM_SKIPMODE, 35U}},
                {{NNNPARAM_AUTOMODE, 41U}}, {{NNNPARAM_MUSICVOLUME, 44U}}, {{NNNPARAM_VOICEVOLUME, 45U}},
                {{NNNPARAM_SOUNDVOLUME, 46U}}, {{NNNPARAM_MOVIEVOLUME, 47U}}, {{NNNPARAM_MUSICSWITCH, 48U}},
                {{NNNPARAM_VOICESWITCH, 49U}}, {{NNNPARAM_SOUNDSWITCH, 50U}}, {{NNNPARAM_MOVIESWITCH, 51U}},
                {{NNNPARAM_SOUNDVOICEVOLUME, 53U}}, {{NNNPARAM_SOUNDVOICESWITCH, 54U}}, {{NNNPARAM_AUTOSPEEDSLIDER, 55U}},
                {{NNNPARAM_WINDOWPERCENT, 483U}}, {{NNNPARAM_TOTALVOLUME, 492U}}, {{NNNPARAM_TOTALSWITCH, 493U}}
            }};
            for (const auto& field : fields)
                candidate.parameters.set(field[0], signed_word(bytes, offset + static_cast<std::size_t>(field[1]) * 4U));
            for (std::size_t group = 0; group < 12U; ++group) {
                const auto packed = word(bytes, offset + (356U + group) * 4U);
                for (std::size_t slot = 0; slot < 6U; ++slot)
                    candidate.parameters.set(NNNPARAM_CHARAVOICEVOLUME + static_cast<Word>(group * 6U + slot),
                        static_cast<Value>((packed >> (slot * 5U)) & 31U));
            }
            for (std::size_t i = 0; i < candidate.voice_off.size(); ++i)
                candidate.voice_off[i] = word(bytes, offset + (384U + i) * 4U);
            for (Word i=0;i<runtime_extended_parameter_count;++i) {
                candidate.parameters.set(NNNPARAM_EXPMODE+i,signed_word(bytes,offset+(448U+i)*4U));
                candidate.parameters.set(NNNPARAM_EXPCHECK+i,signed_word(bytes,offset+(464U+i)*4U));
            }
            candidate.message_window = signed_word(bytes, offset + 480U * 4U);
            candidate.message_cursor = signed_word(bytes, offset + 481U * 4U);
            candidate.mouse_cursor = signed_word(bytes, offset + 482U * 4U);
            candidate.runtime_text.surname=text_field(bytes,offset+56U*4U,16U,runtime_name_bytes);
            candidate.runtime_text.given_name=text_field(bytes,offset+60U*4U,16U,runtime_name_bytes);
        } else if (code == 6U) {
            if ((length!=544U && length!=4128U) || !signature(bytes,offset,"SYSTEMVAR      "))
                throw std::runtime_error("Unsupported SYSTEMVAR layout");
            const auto count=length==544U ? 100U : candidate.system_variables.size();
            for (std::size_t i=0;i<count;++i)
                candidate.system_variables[i]=signed_word(bytes,offset+32U+i*4U);
        } else if (code==13U && length==6432U && signature(bytes,offset,"OKIKAE         ")) {
            auto& system=candidate.runtime_text.system_replacements;
            for (std::size_t i=0;i<system.size();++i)
                system[i]=text_field(bytes,offset+32U+64U*i,64U,runtime_replacement_bytes);
        } else if (code==14U && length==4128U && signature(bytes,offset,"ACHIEVEMENT    ")) {
            for (std::size_t i=0;i<script_achievement_count;++i)
                candidate.script_progress[script_result_count+i]=signed_word(bytes,offset+32U+i*4U);
        } else if (code==15U && length==32U+runtime_term_count*4U && signature(bytes,offset,"TERM           ")) {
            for (std::size_t i=0;i<runtime_term_count;++i)
                candidate.optional_progress->terms[i]=word(bytes,offset+32U+i*4U);
        } else if (code==16U && length==32U+runtime_voice_word_count*4U && signature(bytes,offset,"CHARAVOICEFLAG ")) {
            for (std::size_t i=0;i<runtime_voice_word_count;++i)
                candidate.optional_progress->voice_words[i]=word(bytes,offset+32U+i*4U);
        } else if (code == 17U) {
            if (length != 1056U || !signature(bytes, offset, "SYSTEMFLAG2    "))
                throw std::runtime_error("Unsupported SYSTEMFLAG2 layout");
            candidate.has_system_flag2=true;
            constexpr std::array<std::array<Word, 2>, 7> fields{{
                {{NNNPARAM_CONTINUEVOICESWITCH, 9U}}, {{NNNPARAM_AUTOCONTINUESWITCH, 10U}},
                {{NNNPARAM_CHANGEREADMESSAGECOLORSWITCH, 11U}}, {{NNNPARAM_SCRIPTSEVOLUME, 12U}},
                {{NNNPARAM_SCRIPTSESWITCH, 13U}}, {{NNNPARAM_CHANGESELECTMESSAGECOLORSWITCH, 15U}},
                {{NNNPARAM_AUTOSKIPSWITCH, 16U}}
            }};
            for (const auto& field : fields)
                candidate.parameters.set(field[0], signed_word(bytes, offset + static_cast<std::size_t>(field[1]) * 4U));
            candidate.system_voice_selection = signed_word(bytes, offset + 14U * 4U);
        }
        offset += length;
    }
    initial = std::move(candidate);
}
} // namespace runtime_defaults_detail

// Loads installation defaults only, before Android/private persisted overrides.
// Automatic selection requires an unambiguous installation .org candidate.
// Missing, ambiguous, corrupt or unknown layouts retain CreateInitData values
// and report that fallback. A caller with a known original build/edition can
// select its exact filename through options. No .sav filename is considered.
inline InitialRuntimeParameters load_initial_runtime_parameters(
    const std::filesystem::path& root, const RuntimeDefaultsLog& log = {}, RuntimeDefaultsOptions options = {}) {
    InitialRuntimeParameters result;
    result.initial_names.fill(runtime_defaults_detail::error_name);
    result.game_default_names.fill(runtime_defaults_detail::error_name);
    auto note = [&](const std::string& message) {
        if (!result.diagnostic.empty()) result.diagnostic += "; ";
        result.diagnostic += message;
    };
    std::string game;
    RuntimeSystemFileVersion source_version=RuntimeSystemFileVersion::Automatic;
    try {
        game=game_config::read(root, "init", "game.xtx");
        result.routing.movie_volume_not_use = runtime_defaults_detail::config_number(game, "movieVolumeNotUse", 0);
        result.routing.sound_voice_volume_not_use = runtime_defaults_detail::config_number(game, "soundVoiceVolumeNotUse", 1);
        result.routing.total_volume_use = runtime_defaults_detail::config_number(game, "totalVolumeUseFlag", 0) != 0;
        const auto radio_binding=[&](const char* name) -> Word {
            const auto index=runtime_defaults_detail::config_number(game,name,0);
            return index>=1 && index<=static_cast<Value>(runtime_extended_parameter_count)
                ? NNNPARAM_EXPMODE+static_cast<Word>(index-1) : 0;
        };
        result.message_cursor_parameter=radio_binding("expModeButtonIsMessageCursor");
        result.mouse_cursor_parameter=radio_binding("expModeButtonIsMouse");
        result.reset_name_on_title=runtime_defaults_detail::config_number(game,"resetNameByStartTitle",0)!=0;
        constexpr std::array<const char*,2> init_keys{{"seiinitdata","meiinitdata"}};
        constexpr std::array<const char*,2> clear_keys{{"seiClearFlag","meiClearFlag"}};
        constexpr std::array<const char*,2> default_keys{{"defaultnamesei","defaultnamemei"}};
        for (std::size_t i=0;i<init_keys.size();++i) {
            if (const auto value=runtime_defaults_detail::config_name(game,init_keys[i])) {
                result.initial_names[i]=bound_runtime_text(*value,runtime_name_bytes);
                result.seed_empty_names[i]=runtime_defaults_detail::config_number(game,clear_keys[i],0)==0;
            }
            if (const auto value=runtime_defaults_detail::config_name(game,default_keys[i]))
                result.game_default_names[i]=bound_runtime_text(*value,runtime_name_bytes);
        }
    } catch (const std::exception& error) {
        note(std::string("Game parameter routing uses source defaults: ") + error.what());
    }
    try {
        runtime_defaults_detail::load_local_replacements(root,game,result.local_replacement_defaults);
    } catch (const std::exception& error) {
        note(std::string("Local text defaults use source placeholders: ") + error.what());
    }
    try {
        std::vector<std::pair<std::filesystem::path,RuntimeSystemFileVersion>> candidates;
        const auto directory=resource::find_directory(root,"sav");
        for (const auto version : {RuntimeSystemFileVersion::Version2, RuntimeSystemFileVersion::Extended, RuntimeSystemFileVersion::Classic}) {
            if (options.version != RuntimeSystemFileVersion::Automatic && options.version != version) continue;
            const std::string suffix = version == RuntimeSystemFileVersion::Version2 ? "3" : version == RuntimeSystemFileVersion::Extended ? "2" : "";
            for (const auto edition : {RuntimeSystemEdition::Full, RuntimeSystemEdition::TrialOrNetwork}) {
                if (options.edition != RuntimeSystemEdition::Automatic && options.edition != edition) continue;
                const std::string name = (edition == RuntimeSystemEdition::Full ? "sysfile" : "tsysfile") + suffix + ".org";
                const auto path = game_config::find_file(directory,name);
                if (!path.empty()) candidates.emplace_back(path,version);
            }
        }
        if (candidates.empty()) {
            note("No installation system-default .org archive; using CreateInitData defaults");
        } else if (candidates.size() != 1U) {
            note("Ambiguous system-default archive version/edition; using CreateInitData defaults");
        } else {
            result.source = candidates.front().first;
            runtime_defaults_detail::decode(runtime_defaults_detail::read_file(result.source), result);
            result.loaded_original_defaults = true;
            source_version=candidates.front().second;
        }
    } catch (const std::exception& error) {
        // decode commits only after every declared record is checked.
        note(std::string("System defaults use CreateInitData fallback: ") + error.what());
    }
    try {
        using runtime_defaults_detail::config_number;
        using runtime_defaults_detail::config_name;
        const auto config=game_config::read(root,"init","config.xtx");
        const auto volume_count=config_number(config,"volumeNumber",0);
        const bool legacy_controls=volume_count>0 && volume_count<=8 &&
            config_name(config,"soundbarfilename").has_value() && config_name(config,"soundvolumeprintx").has_value();
        const bool old_archive=source_version==RuntimeSystemFileVersion::Classic || source_version==RuntimeSystemFileVersion::Extended;
        const bool split=result.has_system_flag2 || source_version==RuntimeSystemFileVersion::Version2 ||
            config_number(game,"scriptseVolumeUseFlag",0)!=0 || config_number(config,"scriptseVolumeExistFlag",0)!=0 ||
            config_name(config,"scriptsebarfilename").has_value();
        // Pre-Ver2 PlayScriptSe shares SOUND switch and gain. A missing archive
        // alone cannot identify an installation's audio layout.
        result.routing.separate_script_se=split || !(old_archive || legacy_controls);
        // Classic CCommonConfig starts each declared slider enabled before
        // reading its ExistFlag. Recognize the same authored layout without .org.
        result.legacy_volume_defaults=old_archive || (!split && legacy_controls);
    } catch (const std::exception& error) {
        note(std::string("Sound routing uses separate controls: ")+error.what());
    }
    runtime_defaults_detail::seed_empty_names(result,result.runtime_text);
    try {
        const auto path = game_config::find(root,"sav","result.org");
        if (!path.empty()) {
            const auto bytes = runtime_defaults_detail::read_file(path);
            if (bytes.size() != script_result_count * 4U)
                throw std::runtime_error("unrecognized result.org size");
            for (std::size_t i = 0; i < script_result_count; ++i)
                result.script_progress[i] = runtime_defaults_detail::signed_word(bytes, i * 4U);
        }
    } catch (const std::exception& error) {
        note(std::string("Result defaults use zero: ") + error.what());
    }
    if (log && !result.diagnostic.empty()) log(result.diagnostic);
    return result;
}

} // namespace nnn
