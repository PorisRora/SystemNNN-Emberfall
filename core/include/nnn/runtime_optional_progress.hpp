#pragma once

#include "nnn/script.hpp"

namespace nnn {

// Original SYS records VOICEFLAG (10), TERM (15) and CHARAVOICEFLAG (16).
inline constexpr std::size_t runtime_voice_role_count=256;
inline constexpr std::size_t runtime_voice_role_word_count=runtime_voice_role_count/32U;
inline constexpr std::size_t runtime_term_count=1024;
inline constexpr std::size_t runtime_voice_flag_count=65536U*8U;
inline constexpr std::size_t runtime_voice_word_count=runtime_voice_flag_count/32U;
inline constexpr Word runtime_term_unlocked=1U, runtime_term_read=2U;

struct RuntimeOptionalProgress {
    std::array<Word,runtime_term_count> terms{};
    std::array<Word,runtime_voice_word_count> voice_words{};
    std::array<Word,runtime_voice_role_word_count> voice_role_words{};

    bool operator==(const RuntimeOptionalProgress& other) const noexcept {
        return terms==other.terms && voice_words==other.voice_words && voice_role_words==other.voice_role_words;
    }
    bool term_unlocked(Value index) const noexcept { return (term_flags(index)&runtime_term_unlocked)!=0; }
    bool term_read(Value index) const noexcept { return (term_flags(index)&runtime_term_read)!=0; }
    bool voice_played(Value index) const noexcept {
        const auto id=static_cast<std::size_t>(index);
        return id<runtime_voice_flag_count && (voice_words[id/32U]&(Word{1}<<(id%32U)))!=0;
    }
    bool voice_role_heard(Value character) const noexcept {
        const auto id=static_cast<std::size_t>(character);
        return id>0 && id<runtime_voice_role_count && (voice_role_words[id/32U]&(Word{1}<<(id%32U)))!=0;
    }
private:
    Word term_flags(Value index) const noexcept {
        const auto id=static_cast<std::size_t>(index);
        return id<terms.size() ? terms[id] : Word{0};
    }
};

} // namespace nnn
