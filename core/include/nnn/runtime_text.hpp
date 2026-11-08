#pragma once

#include "nnn/resource_path.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace nnn {

// The Windows SYSTEMDATA name fields hold 16 bytes including the terminator.
// COkikaeData and CCommonSystemFile limit replacement text to 32 raw bytes.
inline constexpr std::size_t runtime_name_bytes = 15;
inline constexpr std::size_t runtime_replacement_bytes = 32;
inline constexpr std::size_t runtime_replacement_count = 100;
using RuntimeReplacementTable = std::array<std::string, runtime_replacement_count>;

struct RuntimeTextState {
    std::string surname, given_name;
    RuntimeReplacementTable system_replacements{};

    bool operator==(const RuntimeTextState& other) const {
        return surname==other.surname && given_name==other.given_name &&
            system_replacements==other.system_replacements;
    }
};

inline bool valid_runtime_text(std::string_view bytes, std::size_t limit) noexcept {
    return bytes.size()<=limit && bytes.find('\0')==std::string_view::npos;
}

// Truncate legacy config text without cutting a CP932 or SJIS tunnel pair.
inline std::string bound_runtime_text(std::string_view bytes, std::size_t limit) {
    std::size_t end=0;
    while (end<bytes.size() && end<limit && bytes[end]!='\0') {
        if (resource::cp932_lead(static_cast<unsigned char>(bytes[end]))) {
            if (end+1>=bytes.size() || end+1>=limit || bytes[end+1]=='\0') break;
            end+=2;
        } else {
            ++end;
        }
    }
    return std::string(bytes.substr(0,end));
}

// CMyMessage::GetCommand recognizes names and local/system replacements here.
// Leave other message commands to the layout path. Replacement bytes are
// appended verbatim rather than interpreted as new commands.
template<class Local, class System>
std::string expand_text_replacements(std::string_view raw, bool one_byte_text,
    std::string_view surname, std::string_view given_name, Local&& local, System&& system) {
    if (raw.find('#')==std::string_view::npos) return std::string(raw);
    const auto surname_token=one_byte_text ? std::string_view("L") : std::string_view("\x90\xa9");
    const auto given_token=one_byte_text ? std::string_view("F") : std::string_view("\x96\xbc");
    const auto system_token=one_byte_text ? std::string_view("S") : std::string_view("\x83\x56");
    const auto digit=[&](std::size_t p) {
        if (p>=raw.size()) return -1;
        if (one_byte_text) return static_cast<unsigned char>(raw[p])-'0';
        if (p+1>=raw.size() || static_cast<unsigned char>(raw[p])!=0x82U) return -1;
        return static_cast<unsigned char>(raw[p+1])-0x4f;
    };
    std::string result;
    result.reserve(raw.size());
    for (std::size_t i=0;i<raw.size();) {
        if (raw[i]=='#' && i+1<raw.size()) {
            const auto next=i+1;
            const auto token=raw.substr(next,surname_token.size());
            if (token==surname_token || token==given_token) {
                result.append(token==surname_token ? surname : given_name);
                i=next+token.size();
                continue;
            }
            const bool use_system=token==system_token;
            std::size_t at=next+(use_system ? token.size() : 0);
            unsigned index=0;
            // GetNewSkip/GetOkikae count up to 2*codeByte digits. A system
            // marker without digits refers to slot zero.
            for (unsigned n=0;n<(one_byte_text ? 2U : 4U);++n) {
                const auto value=digit(at);
                if (value<0 || value>9) break;
                index=index*10U+static_cast<unsigned>(value);
                at+=one_byte_text ? 1U : 2U;
            }
            if (at!=next) {
                if (index<runtime_replacement_count) {
                    if (use_system) result.append(system(index));
                    else result.append(local(index));
                }
                i=at;
                continue;
            }
        }
        const std::size_t width=!one_byte_text && i+1<raw.size() &&
            resource::cp932_lead(static_cast<unsigned char>(raw[i])) ? 2U : 1U;
        result.append(raw.substr(i,width));
        i+=width;
    }
    return result;
}

} // namespace nnn
