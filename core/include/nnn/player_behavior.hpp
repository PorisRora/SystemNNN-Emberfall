#pragma once

#include "nnn/game_config.hpp"
#include "nnn/runtime_parameters.hpp"
#include "nnn/snapshot.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>

namespace nnn {

// Source: commonprintmessage.cpp513-627,1385-1425,2614-2649,3730-3752.
// Rendering belongs to the caller; speed selection and Auto timing share the
// same RuntimeParameters used by the rest of the portable game session.
class PlayerBehavior {
public:
    PlayerBehavior(RuntimeParameters& parameters,const std::filesystem::path& root,bool legacy_volume_defaults=false)
        : parameters_(&parameters) {
        const auto print=game_config::read(root,"init","printmessage.xtx");
        const auto config=game_config::read(root,"init","config.xtx");
        const int voice_wait=number(print,"voiceLastWait",500);
        const int silent_wait=number(print,"noVoiceLastWait",2000);
        auto_message_last_wait_=number(print,"autoMessageLastWait",2000);
        auto_message_wait_voice_=number(print,"autoMessageWaitVoiceFlag",0)!=0;
        auto_message_voice_last_wait_=number(print,"autoMessageVoiceLastWait",voice_wait);
        for (std::size_t i=0;i<5;++i) {
            const auto suffix=std::to_string(i+1);
            voice_wait_[i]=number(print,"voiceLastWait"+suffix,voice_wait);
            silent_wait_[i]=number(print,"noVoiceLastWait"+suffix,silent_wait);
            if (number(print,"customMessageSpeed",0)!=0)
                text_rates_[i]=rate(print,"messageSpeed"+suffix,text_rates_[i]);
        }
        auto_rates_=text_rates_;
        auto_voice_wait_=voice_wait_; auto_silent_wait_=silent_wait_;
        const bool custom_auto_wait=number(print,"useAutoModeWait",0)!=0;
        // Classic uses the Auto slider with the ordinary voiced/unvoiced
        // waits. The later custom Auto wait table is configured separately.
        use_auto_wait_=legacy_volume_defaults||custom_auto_wait;
        for (std::size_t i=0;i<5;++i) {
            const auto suffix=std::to_string(i+1);
            if (number(print,"customAutoMessageSpeed",0)!=0)
                auto_rates_[i]=rate(print,"autoMessageSpeed"+suffix,auto_rates_[i]);
            if (custom_auto_wait) {
                auto_voice_wait_[i]=number(print,"voiceLastWaitAuto"+suffix,voice_wait_[i]);
                auto_silent_wait_[i]=number(print,"noVoiceLastWaitAuto"+suffix,silent_wait_[i]);
            }
        }
        // Classic always selects reveal speed from MESSAGESPEED. For newer
        // builds CCommonConfig::CheckVolumeExist also checks the slider count.
        use_auto_slider_for_text_=!legacy_volume_defaults && number(config,"volumeNumber",6)>7 &&
            number(config,"autoSpeedVolumeExistFlag",0)!=0;
    }
    void rebind_parameters(RuntimeParameters& parameters) noexcept { parameters_=&parameters; }

    // has_voice is the source's sticky message-have-voice flag, not IsPlaying.
    // voice_length_ms is the recorded full clip duration. script_speed is the
    // message-effect override:0 uses settings,1..5 select a source speed row.
    void begin_line(bool has_voice,std::uint32_t voice_length_ms,int script_speed=0,bool append=false) noexcept {
        has_voice_=has_voice; script_speed_=std::clamp(script_speed,0,5);
        if (!append) { text_remainder_=0; visible_=0; text_length_=0; }
        length_known_=false; auto_frame_eligible_=false;
        const auto selected=script_speed_ ? static_cast<std::size_t>(script_speed_-1)
                                         : index(use_auto_wait_ ? NNNPARAM_AUTOSPEEDSLIDER : NNNPARAM_MESSAGESPEED);
        const auto& waits=has_voice_ ? (use_auto_wait_ ? auto_voice_wait_ : voice_wait_)
                                    : (use_auto_wait_ ? auto_silent_wait_ : silent_wait_);
        auto_remaining_ms_=(has_voice_ ? static_cast<std::int64_t>(voice_length_ms) : 0)+waits[selected];
        auto_message_elapsed_=0;
        auto_message_voice_remaining_=has_voice ? static_cast<std::int64_t>(voice_length_ms)+auto_message_voice_last_wait_ : 1;
    }

    // CalcuFrameMoji's CPS*elapsed plus carry, including batching at fast speeds.
    // Call once per native elapsed interval; UI only consumes the returned count.
    std::uint32_t text_characters(std::uint32_t elapsed_ms) noexcept {
        // CalcuPrintMode tests print-end before CalcuPrinting: the frame which
        // reveals the last character returns before CheckAuto can spend time.
        auto_frame_eligible_=text_complete();
        if (auto_frame_eligible_) return 0;
        const auto work=static_cast<std::uint64_t>(characters_per_second())*elapsed_ms+text_remainder_;
        text_remainder_=work%1000U;
        const auto delta=static_cast<std::uint32_t>(std::min<std::uint64_t>(work/1000U,std::numeric_limits<std::uint32_t>::max()));
        if (length_known_) visible_+=std::min(delta,text_length_-visible_);
        return delta;
    }
    // Frontends provide decoded/layout character count, not reveal decisions.
    void set_text_length(std::uint32_t length,std::optional<std::uint32_t> visible={}) noexcept {
        text_length_=length; visible_=std::min(visible.value_or(visible_),length); length_known_=true;
        auto_frame_eligible_=text_complete();
    }
    std::uint32_t visible_text() const noexcept { return visible_; }
    std::uint32_t text_length() const noexcept { return text_length_; }
    bool text_complete() const noexcept { return length_known_ && visible_==text_length_; }
    bool request_advance() noexcept {
        if (!length_known_) return false;
        if (!text_complete()) { visible_=text_length_; text_remainder_=0; auto_frame_eligible_=false; return false; }
        return true;
    }
    std::uint32_t characters_per_second() const noexcept {
        // Message-effect overrides use the auto table even with Auto off
        // (CalcuFrameMoji1410-1420), unlike the ordinary setting branch.
        if (script_speed_) return auto_rates_[static_cast<std::size_t>(script_speed_-1)];
        const bool automatic=parameters_->get(NNNPARAM_AUTOMODE)!=0;
        const auto selected=index(automatic && use_auto_slider_for_text_ ? NNNPARAM_AUTOSPEEDSLIDER : NNNPARAM_MESSAGESPEED);
        return (automatic ? auto_rates_ : text_rates_)[selected];
    }
    std::uint32_t settings_text_rate(int index,bool automatic) const noexcept {
        return (automatic ? auto_rates_ : text_rates_)[static_cast<std::size_t>(std::clamp(index,0,4))];
    }
    // Scheduling hint only. text_characters retains exact frame/carry behavior;
    // one-character-per-delay cannot represent the source's fastest batching.
    int text_delay_ms() const noexcept {
        const auto speed=characters_per_second();
        return speed ? static_cast<int>((1000U+static_cast<std::uint64_t>(speed)-1U)/speed) : -1;
    }
    // CalcuPrintMode returns before CheckAuto until the text is complete. Once
    // complete, call on every frame, even with Auto off (which resets count to1).
    // There is deliberately no IsPlaying OR condition or character-count delay.
    bool auto_ready(std::uint32_t elapsed_ms,bool text_complete,bool script_auto_message=false) noexcept {
        if (script_auto_message) auto_message_voice_remaining_-=elapsed_ms;
        if (length_known_) {
            if (!this->text_complete()) return false;
            if (!auto_frame_eligible_) { auto_frame_eligible_=true; return false; }
        } else if (!text_complete) return false;
        if (script_auto_message) {
            const bool ready=auto_message_elapsed_>=auto_message_last_wait_ &&
                             (!auto_message_wait_voice_ || auto_message_voice_remaining_<=0);
            auto_message_elapsed_+=elapsed_ms;
            return ready;
        }
        if (parameters_->get(NNNPARAM_AUTOMODE)==0) { auto_remaining_ms_=1; return false; }
        auto_remaining_ms_=std::max<std::int64_t>(-1,auto_remaining_ms_-elapsed_ms);
        return auto_remaining_ms_<=0;
    }
    std::int64_t auto_remaining_ms() const noexcept { return auto_remaining_ms_; }
    bool continue_voice() const noexcept { return parameters_->get(NNNPARAM_CONTINUEVOICESWITCH)!=0; }
    bool auto_continue() const noexcept { return parameters_->get(NNNPARAM_AUTOCONTINUESWITCH)!=0; }
    bool auto_skip() const noexcept { return parameters_->get(NNNPARAM_AUTOSKIPSWITCH)!=0; }
    bool skip_read_only() const noexcept { return parameters_->get(NNNPARAM_SKIPMODE)==0; }
    void save_snapshot(snapshot::Writer& out) const { snapshot_fields(out,*this); }
    void load_snapshot(snapshot::Reader& in) {
        snapshot_fields(in,*this);
        if (script_speed_<0 || script_speed_>5 || text_remainder_>=1000 || visible_>text_length_ || auto_message_elapsed_<0)
            snapshot::invalid("Invalid snapshot dialogue clock");
    }
    static void inspect_snapshot(snapshot::Reader& in) {
        PlayerBehavior state;
        state.load_snapshot(in);
    }

private:
    PlayerBehavior()=default;
    template<class A,class B> static void snapshot_fields(A& a,B& b) {
        a(b.has_voice_,b.script_speed_,b.text_remainder_,b.visible_,b.text_length_,b.length_known_,b.auto_frame_eligible_,
            b.auto_remaining_ms_,b.auto_message_elapsed_,b.auto_message_voice_remaining_);
    }
    static int number(const std::string& text,const std::string& wanted,int fallback) {
        std::istringstream file(text);
        for (std::string line;std::getline(file,line);) {
            std::istringstream row(line); std::string key; int value=0;
            if (row>>key>>value && game_config::ascii_lower(key)==game_config::ascii_lower(wanted)) return value;
        }
        return fallback;
    }
    static std::uint32_t rate(const std::string& text,const std::string& key,std::uint32_t fallback) {
        const auto value=number(text,key,static_cast<int>(fallback));
        if (value<0) throw std::runtime_error("Negative message speed: "+key);
        return static_cast<std::uint32_t>(value);
    }
    std::size_t index(Word parameter) const noexcept {
        return static_cast<std::size_t>(std::clamp<Value>(parameters_->get(parameter),0,4));
    }
    RuntimeParameters* parameters_=nullptr;
    std::array<std::uint32_t,5> text_rates_{{20,40,60,120,10000}},auto_rates_{};
    std::array<int,5> voice_wait_{},silent_wait_{},auto_voice_wait_{},auto_silent_wait_{};
    bool use_auto_wait_=false,use_auto_slider_for_text_=false,has_voice_=false;
    int script_speed_=0;
    std::uint64_t text_remainder_=0;
    std::uint32_t visible_=0,text_length_=0;
    bool length_known_=false,auto_frame_eligible_=false;
    std::int64_t auto_remaining_ms_=1;
    int auto_message_last_wait_=2000,auto_message_voice_last_wait_=500;
    bool auto_message_wait_voice_=false;
    std::int64_t auto_message_elapsed_=0,auto_message_voice_remaining_=1;
};
} // namespace nnn
