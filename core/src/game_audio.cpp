#include "audio_assets.hpp"
#include "nnn/voice.hpp"
#include "nnn/game_config.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <limits>
#include <set>
#include <sstream>

namespace nnn::audio {
namespace {
int percent(std::int64_t v) { return static_cast<int>(std::clamp<std::int64_t>(v,0,100)); }
int level(int base,int delta,std::int64_t adjustment=0) {
    return delta==-101 ? 1 : percent(std::int64_t{base}+delta+adjustment);
}
int parameter(const std::vector<Value>& a,std::size_t index,int fallback=0) {
    return index<a.size() ? a[index] : fallback;
}
int config_number(const std::string& text,const std::string& wanted,int fallback) {
    std::istringstream lines(text);
    for (std::string line;std::getline(lines,line);) {
        std::istringstream row(line); std::string key; int value=0;
        if (row>>key>>value && game_config::ascii_lower(key)==game_config::ascii_lower(wanted)) return value;
    }
    return fallback;
}
std::uint64_t fade_frames(int tenths,unsigned rate) {
    return static_cast<std::uint64_t>(std::max(0,tenths))*rate/10U;
}
struct Ramp {
    double from=0,to=0;
    std::uint64_t time=0,total=0;
    double now() const {
        return time<total ? from+(to-from)*static_cast<double>(time)/static_cast<double>(total) : to;
    }
    void set(double target,std::uint64_t frames) {
        from=now(); to=target; time=0; total=frames;
    }
};
// Keep each fade endpoint's response to every possible category setting. This
// preserves script deltas, role gain and clipped endpoints even when a setting
// changes midway through successive fades; the audio callback still reads O(1).
constexpr std::size_t category_levels=101,character_levels=32;
using GainCurve=std::array<double,category_levels*character_levels>;
GainCurve gain_curve(int delta,int adjustment=0,bool bgm=false,int step_scale=0,bool dynamic_role=false,
                     bool bgm_minimum=true,int reference_steps=0) {
    GainCurve result{};
    for (std::size_t role=0;role<character_levels;++role) {
        const auto role_adjust=dynamic_role ? (static_cast<std::int64_t>(role)-16-reference_steps)*step_scale : adjustment;
        for (std::size_t i=0;i<category_levels;++i) {
            const int base=static_cast<int>(i);
            result[role*category_levels+i]=bgm ? std::max(bgm_minimum ? 1 : 0,percent(std::int64_t{base}+delta)) : level(base,delta,role_adjust);
        }
    }
    return result;
}
struct GainRamp {
    GainCurve from{},to{};
    std::uint64_t time=0,total=0;
    int base=80,reference_base=80;
    std::size_t role=16;
    double at(std::size_t i) const {
        return time<total ? from[i]+(to[i]-from[i])*static_cast<double>(time)/static_cast<double>(total) : to[i];
    }
    std::size_t index(int category) const { return role*category_levels+static_cast<std::size_t>(category); }
    double now() const { return at(index(base)); }
    void set_base(int value) {
        base=percent(value);
        reference_base=base;
    }
    void set(const GainCurve& target,std::uint64_t frames) {
        for (std::size_t i=0;i<from.size();++i) from[i]=at(i);
        to=target; time=0; total=frames;
    }
    void set(double target,std::uint64_t frames) {
        GainCurve values{}; values.fill(target); set(values,frames);
    }
};
enum class UiGain { Sound, Voice, Movie };
struct Track {
    std::shared_ptr<const Program> program;
    std::size_t part=0;
    std::uint64_t phase=0; // Source frame multiplied by the output rate; no rounding at block boundaries.
    int remaining=0;
    GainRamp gain;
    Ramp pan;
    bool active=false,stop_after_ramp=false,volume_allowed=false,volume_ramp=false;
    int character=-1;
    int source_number=-1;
    bool system_sound=false;
    UiGain ui_gain=UiGain::Sound; // Only the transient system-sound bus uses this.
    std::size_t next_part() const {
        if (part+1<program->parts.size()) return part+1;
        if (program->loop_start) return *program->loop_start;
        if (remaining==0 || remaining>1) return 0;
        return program->parts.size();
    }
    void start(std::shared_ptr<const Program> p,int plays,const GainCurve& volume,int base,std::uint64_t fade,
               int character_index=-1,int character_steps=0) {
        program=std::move(p); part=0; phase=0; remaining=std::max(0,plays);
        active=true; volume_allowed=true; stop_after_ramp=false; volume_ramp=false;
        character=character_index;
        gain={}; gain.role=static_cast<std::size_t>(std::clamp(character_steps,-16,15)+16);
        gain.set_base(base); gain.set(volume,fade); pan={};
    }
    void stop(std::uint64_t fade) {
        volume_allowed=false; volume_ramp=false;
        if (!active || !fade) { active=false; stop_after_ramp=false; gain.set(0,0); return; }
        gain.set(0,fade); stop_after_ramp=true;
    }
    void volume(const GainCurve& value,std::uint64_t duration) {
        if (!active || !volume_allowed) return;
        gain.set(value,duration);
        volume_ramp=duration!=0;
        stop_after_ramp=duration && value[gain.index(gain.reference_base)]==0;
    }
    void set_base(int base) {
        gain.set_base(base);
        // Muting a category does not create a new stop. Explicit script stops
        // retain their endpoint; gain ramps follow the re-evaluated script target.
        if (volume_allowed && volume_ramp && gain.time<gain.total)
            stop_after_ramp=gain.to[gain.index(gain.reference_base)]==0;
    }
    void set_role(int steps) {
        gain.role=static_cast<std::size_t>(std::clamp(steps,-16,15)+16);
        if (volume_allowed && volume_ramp && gain.time<gain.total)
            stop_after_ramp=gain.to[gain.index(gain.reference_base)]==0;
    }
    TrackState state(unsigned output_rate) const {
        if (!program || !active) return {};
        return {active,program->loop_start.has_value() || remaining!=1,gain.time<gain.total,
                program->name,program->parts[part]->info.name,static_cast<int>(std::lround(gain.now())),phase/output_rate};
    }
    void cross_boundaries(unsigned rate,std::uint64_t& changes,std::uint64_t& loops) {
        while (active) {
            const auto& current=program->parts[part]->info;
            const auto end=current.frames*rate;
            if (phase<end) break;
            const auto following=next_part();
            if (following==program->parts.size()) { active=false; volume_allowed=false; break; }
            if (following<=part) {
                ++loops;
                if (!program->loop_start && remaining>1) --remaining;
            }
            ++changes;
            phase=(phase-end)*program->parts[following]->info.rate/current.rate;
            part=following;
        }
    }
    void skip(unsigned rate,std::uint64_t frames,std::uint64_t& changes,std::uint64_t& loops) {
        while (active && frames) {
            const auto& clip=program->parts[part]->info;
            const auto until_end=(clip.frames*rate-phase+clip.rate-1)/clip.rate;
            auto step=std::min(frames,until_end);
            if (stop_after_ramp) step=std::min(step,gain.total-gain.time);
            gain.time+=std::min(step,gain.total-gain.time);
            pan.time+=std::min(step,pan.total-pan.time);
            if (stop_after_ramp && gain.time==gain.total) { active=false; volume_allowed=false; return; }
            phase+=step*clip.rate;
            frames-=step;
            cross_boundaries(rate,changes,loops);
        }
    }
    std::array<double,2> sample(unsigned rate,std::uint64_t& changes,std::uint64_t& loops) {
        if (!active) return {};
        const Clip& c=*program->parts[part];
        const auto i=phase/rate;
        const unsigned fraction=static_cast<unsigned>(phase%rate);
        const Clip* other=&c; std::uint64_t j=i+1;
        if (j>=c.info.frames) {
            const auto next=next_part();
            if (next<program->parts.size()) { other=program->parts[next].get(); j=0; }
            else j=i;
        }
        const double amp=gain.now()/100.0,balance=pan.now();
        std::array<double,2> out{};
        for (unsigned k=0;k<2;++k) {
            const auto a=c.pcm[static_cast<std::size_t>(i)*c.info.channels+std::min(k,c.info.channels-1)];
            const auto b=other->pcm[static_cast<std::size_t>(j)*other->info.channels+std::min(k,other->info.channels-1)];
            const double v=a+(b-a)*static_cast<double>(fraction)/rate;
            out[k]=v*amp*(k==0 ? 1-std::max(0.0,balance) : 1+std::min(0.0,balance));
        }
        if (gain.time<gain.total) ++gain.time;
        if (pan.time<pan.total) ++pan.time;
        if (stop_after_ramp && gain.time==gain.total) { active=false; volume_allowed=false; return out; }
        phase+=c.info.rate;
        cross_boundaries(rate,changes,loops);
        return out;
    }
};
struct Meter {
    std::uint64_t frames=0,nonzero=0,clipped=0,bgm=0,speech=0,se=0,multi=0,changes=0,loops=0;
    int peak=0;
};
enum class SampleOrigin {
    Script, PcVoice, SystemSound, SystemSoundAtVoiceVolume, ButtonVoice,
    ConfigSound, ConfigVoice, ConfigMovieSound, BacklogVoice, MenuVoice
};
struct VoicePreview { bool ignore_switch=false,ignore_role=false; };
double pan_value(int x) {
    if (x>=128 && x<=255) x-=256;
    return std::clamp(static_cast<double>(x)/127.0,-1.0,1.0);
}
} // namespace

bool is_audio_command(Word op) noexcept {
    switch (op) {
    case music: case voice: case sound: case next_fade_se: case next_fade_voice:
    case volume_se: case volume_voice: case volume_music: case set_voice_flag: return true;
    default: return false;
    }
}

struct GameAudio::Impl {
    Settings settings;
    AudioAssets files;
    VoiceCharacterProfile characters;
    std::map<std::string,std::size_t> character_slots;
    std::array<bool,72> character_enabled;
    std::set<int> voice_flags;
    std::array<Track,4> voices;
    std::array<Track,8> sounds,system_sounds;
    Track song,outgoing;
    bool song_paused=false;
    std::array<Track,2> button_voices;
    Track backlog_voice;
    bool script_audio_suspended=false;
    unsigned next_button_voice=0;
    bool alternate_button_voices=true;
    struct ConfigCue { int number=0,divisions=0; bool voice=false; };
    std::array<ConfigCue,10> config_cues{};
    bool config_themed=false,config_uses_button_voice=false,config_voice_same_buffer=false;
    bool system_voice_role_gain=false;
    int system_sound_channel=0,movie_volume=100;
    bool music_on=true,total_on=true,system_se_on=true,script_se_on=true,voice_on=true,effect_voice_on=true;
    bool continue_voice=false;
    std::optional<std::pair<int,int>> last_music; // Original table number and loop count, not script gain/fade overrides.
    int fade_se=0,fade_voice=0;
    std::optional<int> next_music;
    mutable std::mutex commands,mixer;
    Statistics totals;
    Meter meter;
    Impl(const std::filesystem::path& root,Settings config,const resource::FilenameEncoder& encode)
        :settings(std::move(config)),files(settings.enabled ? AudioAssets(root,settings.cache_bytes,encode) : AudioAssets{}) {
        if (settings.sample_rate<8000 || settings.sample_rate>192000) throw std::invalid_argument("Invalid output sample rate");
        settings.music_volume=percent(settings.music_volume); settings.sound_volume=percent(settings.sound_volume);
        settings.voice_volume=percent(settings.voice_volume); settings.effect_voice_volume=percent(settings.effect_voice_volume);
        settings.master_volume=percent(settings.master_volume);
        settings.system_sound_volume=settings.system_sound_volume<0 ? settings.sound_volume : percent(settings.system_sound_volume);
        character_enabled.fill(true);
        if (!settings.enabled) return;
        const auto game=game_config::read(root,"init","game.xtx");
        if (settings.default_music_fade_in<0) settings.default_music_fade_in=std::max(0,config_number(game,"defaultMusicFadeInTime",0));
        if (settings.default_music_fade_out<0) settings.default_music_fade_out=std::max(0,config_number(game,"defaultMusicFadeOutTime",5));
        std::istringstream lines(game);
        for (std::string line;std::getline(lines,line);) {
            std::istringstream row(line); std::string key; int value=0;
            if (row>>key>>value && game_config::ascii_lower(key)=="buttonvoicenumber") {
                alternate_button_voices=value==2; break;
            }
        }
        characters.slider_delta=config_number(game,"charaVoiceVolumeDelta",settings.character_volume_step);
        // The portable mixer uses the source's linear/XAudio gain scale.
        characters.slider_delta=config_number(game,"charaVoiceVolumeDeltaXAudio2",characters.slider_delta);
        characters.config_present=true; // Default character button table maps the first named role to slider zero.
        const auto& rows=files.characters();
        for (std::size_t i=0;i<rows.size();++i) {
            characters.names.push_back(rows[i]); characters.names.emplace_back();
            if (i>0) character_slots.emplace(rows[i],i-1);
        }
        const auto config_text=game_config::read(root,"init","config.xtx");
        system_voice_role_gain=config_number(game,"enableButtonVoiceCharaVolume",0)!=0;
        if (config_number(config_text,"extSeFlag",0)!=0) {
            constexpr const char* kinds[]={"messageSpeed","music","sound","voice","movie","soundvoice",
                "windowPercent","autoSpeed","total","scriptse"};
            constexpr int divisions[]={3,0,0,9,0,0,0,3,0,0};
            for (std::size_t i=0;i<config_cues.size();++i) {
                const std::string kind=kinds[i];
                config_cues[i]={
                    config_number(config_text,kind+"SeNumber",i==0 ? config_number(config_text,"messageSeNumber",0) : 0),
                    config_number(config_text,kind+"SeBunkatsu",divisions[i]),
                    config_number(config_text,kind+"IsVoice",i==0 ? config_number(config_text,"messageIsVoice",1) : i==7)!=0
                };
            }
            config_themed=config_number(config_text,"useConfigSeVoice",0)!=0;
            config_uses_button_voice=config_number(config_text,"barVoiceIsSystemVoice",0)!=0;
            config_voice_same_buffer=config_number(config_text,"voiceSameBufferFlag",0)!=0;
        }
        characters.mapping_enabled=config_number(config_text,"voiceCutButtonCustomFlag",0)!=0;
        const auto groups=config_number(config_text,"voiceCutButtonNumber",0);
        characters.mapping_table_present=config_number(config_text,"voiceCutButtonFlag",1)!=0 && groups>0;
        if (characters.mapping_enabled && characters.mapping_table_present) {
            if (groups<0 || groups>static_cast<int>(max_voice_mapping_groups)) throw AudioError("Character mapping group limit");
            std::size_t total=0; int next_character=1;
            for (int row=0;row<groups;++row) {
                const auto suffix=std::to_string(row+1);
                const int count=config_number(config_text,"voiceOffNumberByButton"+suffix,1);
                if (count<0 || static_cast<std::size_t>(count)>max_voice_mapping_items-total) throw AudioError("Character mapping item limit");
                total+=static_cast<std::size_t>(count); std::vector<Value> group;
                for (int k=0;k<count;++k) {
                    next_character=config_number(config_text,"voiceOffButton"+suffix+"chara"+std::to_string(k+1),next_character);
                    group.push_back(next_character);
                    if (next_character==INT32_MAX) throw AudioError("Character mapping identifier overflow");
                    ++next_character;
                }
                characters.button_groups.push_back(std::move(group));
            }
        }
    }
    template<std::size_t N>
    static Track& channel(std::array<Track,N>& tracks,int i) {
        if (i<0 || static_cast<std::size_t>(i)>=N) throw AudioError("Channel outside audio bus: "+std::to_string(i));
        return tracks[static_cast<std::size_t>(i)];
    }
    int adjustment(const std::string& name) const {
        if (characters.names.empty()) return 0;
        const auto result=resolve_voice_adjustment(name,characters);
        if (!result.status) throw AudioError(result.status.detail);
        return result.adjustment;
    }
    int character_number(const std::string& name) const {
        if (name.empty()) return 0;
        auto byte=[](const std::string& value,std::size_t i) {
            return (i<value.size() ? static_cast<unsigned char>(value[i]) : 0U)|0x20U;
        };
        for (std::size_t i=2;i<characters.names.size();i+=2) {
            const auto& prefix=characters.names[i];
            if (!prefix.empty() && byte(name,0)==byte(prefix,0) && byte(name,1)==byte(prefix,1))
                return static_cast<int>(i/2);
        }
        return 0;
    }
    int character_index(const std::string& name) const {
        const auto index=character_number(name)-1;
        return index>=0 && static_cast<std::size_t>(index)<character_enabled.size() ? index : -1;
    }
    int character_steps(std::size_t index) const {
        const auto bits=(characters.packed_sliders[index/6]>>(static_cast<unsigned>(index%6)*5U))&31U;
        return static_cast<int>(bits)-(bits&16U ? 32 : 0);
    }
    std::size_t slider_index(std::size_t role) const {
        if (characters.mapping_enabled && characters.mapping_table_present)
            for (std::size_t group=0;group<characters.button_groups.size();++group)
                for (const auto member:characters.button_groups[group])
                    if (member==static_cast<Value>(role+1)) return group;
        return role;
    }
    void update_character(std::size_t index,int steps,bool enabled) {
        const auto adjustment=static_cast<std::int64_t>(steps)*characters.slider_delta;
        if (adjustment<INT32_MIN || adjustment>INT32_MAX) throw std::invalid_argument("Character volume adjustment overflow");
        const auto group=slider_index(index);
        if (group>=72) throw std::invalid_argument("Character slider group outside supported range");
        auto& word=characters.packed_sliders[group/6];
        const unsigned shift=static_cast<unsigned>(group%6)*5U;
        word=(word&~(31U<<shift))|((static_cast<Word>(steps)&31U)<<shift);
        for (std::size_t role=0;role<character_enabled.size();++role)
            if (slider_index(role)==group) character_enabled[role]=enabled;
        std::lock_guard<std::mutex> lock(mixer);
        for (auto& track:voices) if (track.character==static_cast<int>(group)) track.set_role(steps);
        for (auto& track:button_voices) if (track.character==static_cast<int>(group)) track.set_role(steps);
        if (backlog_voice.character==static_cast<int>(group)) backlog_voice.set_role(steps);
    }
    std::uint64_t fade(int units) const { return fade_frames(units,settings.sample_rate); }
    void bgm(const SystemEvent& e,EventInfo& info,SkipFlags skip) {
        if (e.parameters.empty()) throw AudioError("MUSIC missing track number");
        const auto& a=e.parameters; int id=a[0];
        if (id>0) {
            if (!files.has_bgm(id)) throw AudioError("Invalid MUSIC number");
            last_music=std::make_pair(id,parameter(a,1));
        }
        if (skip.skip_next_command && skip.skip_effect_command) { info.action="music.skipped"; return; }
        if (id==-1 || id==-2) {
            std::lock_guard<std::mutex> lock(mixer);
            if (id==-1) {
                last_music.reset();
                song_paused=false;
                const int units=parameter(a,1,-1);
                if (music_on) song.stop(fade(units<0 ? next_music.value_or(settings.default_music_fade_out) : units));
                info.action="music.stop";
            } else {
                if (a.size()>1) song.volume(gain_curve(a[1],0,true),fade(next_music.value_or(0)));
                info.action="music.gain";
            }
            next_music.reset(); return;
        }
        if (id<=0) throw AudioError("Invalid MUSIC number");
        if (!music_on || !total_on) { info.action="music.disabled"; next_music.reset(); return; }
        auto p=std::make_shared<Program>(files.bgm(id));
        info.action="music.play";
        for (const auto& c:p->parts) info.clips.push_back(c->info);
        const int delta=parameter(a,2,-9999);
        const auto vol=gain_curve(delta==-9999 ? 0 : delta,0,true,0,false,delta!=-9999);
        const int in=parameter(a,3,-1),out=parameter(a,4,-1);
        std::lock_guard<std::mutex> lock(mixer);
        song_paused=false;
        outgoing=std::move(song);
        outgoing.stop(fade(out<0 ? next_music.value_or(settings.default_music_fade_out) : out));
        song.start(std::move(p),parameter(a,1),vol,settings.music_volume,fade(in<0 ? next_music.value_or(settings.default_music_fade_in) : in));
        next_music.reset(); ++totals.plays;
    }
    int ui_volume(UiGain gain) const {
        if (gain==UiGain::Voice) return settings.voice_volume;
        if (gain==UiGain::Movie) return movie_volume;
        return settings.system_sound_volume;
    }
    void sample_event(const SystemEvent& e,EventInfo& info,SkipFlags skip,SampleOrigin origin,VoicePreview preview) {
        const auto& a=e.parameters;
        const bool speech=e.opcode==voice;
        const bool pc_voice=origin==SampleOrigin::PcVoice;
        const bool system=origin!=SampleOrigin::Script && !pc_voice;
        const bool button_voice=origin==SampleOrigin::ButtonVoice;
        const bool config_voice=origin==SampleOrigin::ConfigVoice;
        const bool config_sound=config_voice || origin==SampleOrigin::ConfigSound || origin==SampleOrigin::ConfigMovieSound;
        const bool system_at_voice_volume=origin==SampleOrigin::SystemSoundAtVoiceVolume;
        const auto ui_gain=config_voice || system_at_voice_volume ? UiGain::Voice :
            origin==SampleOrigin::ConfigMovieSound ? UiGain::Movie : UiGain::Sound;
        const bool menu_voice=origin==SampleOrigin::MenuVoice;
        const bool replay=origin==SampleOrigin::BacklogVoice || (menu_voice && script_audio_suspended);
        if (a.size()<2) throw AudioError("Sample event missing number or channel");
        const int id=a[0];
        const int ch=config_sound ? (config_voice && config_voice_same_buffer ? system_sound_channel : 1-system_sound_channel) : a[1];
        info.channel=ch;
        // Validate the requested bus before gates, as for ordinary script events.
        Track& tr=button_voice ? button_voices[next_button_voice] : replay ? backlog_voice :
            speech ? channel(voices,ch) : system ? channel(system_sounds,ch) : channel(sounds,ch);
        auto& pending=speech ? fade_voice : fade_se;
        const auto duration=system ? 0U : fade(pending);
        if (!system) pending=0;
        const std::string bus=button_voice ? "system_voice" : config_voice ? "config_voice" :
            config_sound ? "config_se" : menu_voice ? "menu_voice" : replay ? "backlog_voice" :
            speech ? "voice" : system ? "system_se" : "se";
        if (speech && !system && id==-1 && ch<2 && continue_voice && !skip.skip_next_command) {
            info.action="voice.continued"; return;
        }
        if (id==-1 || (skip.skip_next_command && !system)) {
            std::lock_guard<std::mutex> lock(mixer);
            if (id==-1 || (tr.active && tr.remaining==0)) tr.stop(duration);
            info.action=bus+(id==-1 ? ".stop" : ".skipped"); return;
        }
        if (id<0) throw AudioError("Invalid sample number");
        if (speech && (!e.text || e.text->empty())) throw AudioError("VOICE has no resource name");
        const bool system_category=system || (!speech && a.size()>12 && a[12]!=0);
        // Script voice loads replace the channel even when its future play is
        // gated. SE gates leave an existing source running (PlayScriptSe).
        if (speech && !menu_voice) { std::lock_guard<std::mutex> lock(mixer); tr.stop(0); }
        const auto name=button_voice ? files.button_voice_name(id) : config_sound ? files.config_sound_name(id,config_themed) :
            speech ? *e.text : std::string{};
        if (config_sound && name.empty()) { info.action=bus+".unavailable"; return; }
        const bool role_gain=speech || button_voice || (system_at_voice_volume && system_voice_role_gain);
        const auto role_name=system_at_voice_volume && role_gain ? files.default_system_sound_name(id) : name;
        const int named_role=role_gain || config_voice ? character_index(role_name) : -1;
        const auto group=named_role<0 ? std::size_t{72} : slider_index(static_cast<std::size_t>(named_role));
        const int role=role_gain && group<72 ? static_cast<int>(group) : -1;
        const bool role_allowed=system_at_voice_volume || preview.ignore_role || named_role<0 ||
            character_enabled[static_cast<std::size_t>(named_role)];
        const bool voice_allowed=(speech && ch>=2 ? effect_voice_on : voice_on) || preview.ignore_switch;
        const bool allowed=total_on && ((speech || button_voice || ui_gain==UiGain::Voice) ?
            voice_allowed && role_allowed : system_category ? system_se_on : script_se_on);
        if (!allowed && (!speech || menu_voice)) { info.action=bus+".disabled"; return; }
        if (menu_voice) { std::lock_guard<std::mutex> lock(mixer); tr.stop(0); }
        if (button_voice) {
            std::lock_guard<std::mutex> lock(mixer);
            for (const auto& track:button_voices)
                if (track.active && track.program && track.program->name==resource::fold_cp932(name)) { info.action=bus+".duplicate"; return; }
        }
        auto clip=button_voice || config_sound ? files.system_wave(name) : speech ? files.speech(name) : files.se(id,system);
        auto p=std::make_shared<Program>(); p->name=clip->info.name; p->parts.push_back(clip);
        info.action=bus+".play"; info.clips.push_back(clip->info);
        if (!allowed) {
            std::lock_guard<std::mutex> lock(mixer);
            tr.program=std::move(p); info.action=bus+".disabled"; return;
        }
        const int base=button_voice ? settings.voice_volume : speech ? (ch<2 ? settings.voice_volume : settings.effect_voice_volume)
                              : system_category ? ui_volume(ui_gain) : settings.sound_volume;
        const auto steps=role>=0 ? character_steps(static_cast<std::size_t>(role)) : 0;
        const auto vol=gain_curve(parameter(a,2),!pc_voice && role_gain ? adjustment(role_name) : 0,
            false,characters.slider_delta,role>=0,true,pc_voice?steps:0);
        std::lock_guard<std::mutex> lock(mixer);
        if (button_voice && alternate_button_voices) next_button_voice=(next_button_voice+1U)%2U;
        if (ch<2 && (config_sound || origin==SampleOrigin::SystemSound || system_at_voice_volume))
            system_sound_channel=ch;
        auto& target=button_voice ? button_voices[next_button_voice] : tr;
        target.start(std::move(p),parameter(a,3)!=0 ? 0 : 1,vol,base,duration,role,steps);
        target.system_sound=system_category;
        target.ui_gain=ui_gain;
        target.source_number=id;
        target.pan.set(pan_value(parameter(a,4)),0);
        if (parameter(a,7)>0)
            target.pan.set(pan_value(parameter(a,8)),fade(a[7]));
        ++totals.plays;
    }
    std::string submit(const SystemEvent& e,const std::string& script,Word pc,SkipFlags skip,
                       SampleOrigin origin=SampleOrigin::Script,bool* voice_started=nullptr,VoicePreview preview={}) {
        if (voice_started) *voice_started=false;
        if (!settings.enabled || !is_audio_command(e.opcode)) return {};
        std::lock_guard<std::mutex> command_lock(commands);
        if (origin==SampleOrigin::BacklogVoice && !script_audio_suspended) return {};
        // Finished voices must not pin evicted PCM. Reclaim from the game thread, outside the device lock.
        {
            std::array<std::shared_ptr<const Program>,25> retired;
            std::lock_guard<std::mutex> lock(mixer);
            std::size_t i=0;
            auto retire=[&](Track& t){ if (!t.active && t.program) retired[i++]=std::move(t.program); };
            retire(song); retire(outgoing);
            retire(backlog_voice);
            for (auto& t:button_voices) retire(t);
            for (auto& t:voices) retire(t);
            for (auto& t:sounds) retire(t);
            for (auto& t:system_sounds) retire(t);
        }
        EventInfo info; info.opcode=e.opcode; info.word=pc; info.script=script;
        if (settings.record_events) info.parameters=e.parameters;
        ++totals.events; ++totals.opcodes[e.opcode];
        const auto previous=totals.plays;
        try {
            const auto& a=e.parameters;
            if (a.empty()) throw AudioError("Audio command has no arguments");
            if (e.opcode==music) bgm(e,info,skip);
            else if (e.opcode==voice || e.opcode==sound) sample_event(e,info,skip,origin,preview);
            else if (e.opcode==volume_voice || e.opcode==volume_se) {
                if (a.size()<2) throw AudioError("Volume command missing channel or gain");
                const bool v=e.opcode==volume_voice;
                int& pending=v ? fade_voice : fade_se;
                const auto duration=fade(pending); pending=0;
                info.channel=a[0]; info.action=v ? "voice.gain" : "se.gain";
                std::lock_guard<std::mutex> lock(mixer);
                auto& tr=v ? channel(voices,a[0]) : channel(sounds,a[0]);
                if (!skip.skip_next_command && tr.active && tr.volume_allowed) {
                    tr.volume(gain_curve(a[1],v ? adjustment(tr.program->name) : 0,false,characters.slider_delta,v && tr.character>=0),duration);
                }
            } else if (e.opcode==next_fade_voice || e.opcode==next_fade_se) {
                (e.opcode==next_fade_voice ? fade_voice : fade_se)=std::max(0,a[0]);
                info.action=e.opcode==next_fade_voice ? "voice.next_fade" : "se.next_fade";
            } else if (e.opcode==volume_music) {
                std::lock_guard<std::mutex> lock(mixer);
                song.volume(gain_curve(a[0],0,true),fade(parameter(a,1,next_music.value_or(0))));
                next_music.reset();
                info.action="music.gain";
            } else { voice_flags.insert(a[0]); info.action="voice.flag"; }
        } catch (const AudioError& ex) {
            if (ex.missing) ++totals.missing; else ++totals.errors;
            info.error=ex.what();
        } catch (const std::exception& ex) { ++totals.errors; info.error=ex.what(); }
        if (info.error.empty() && previous==totals.plays) ++totals.controls;
        if (voice_started) *voice_started=info.error.empty() && info.action=="voice.play";
        const auto error=info.error;
        if (settings.record_events) {
            { std::lock_guard<std::mutex> lock(mixer); info.at_output_frame=meter.frames; }
            totals.log.push_back(std::move(info));
        }
        return error;
    }

    void render(std::int16_t* output,std::size_t count) {
        std::lock_guard<std::mutex> lock(mixer);
        struct Source { Track* track; unsigned bus; };
        std::array<Source,24> sources{};
        std::size_t active=0;
        auto include=[&](Track& t,unsigned bus){ if (t.active) sources[active++]={&t,bus}; };
        if (!song_paused) include(song,0);
        include(outgoing,0);
        for (auto& t:button_voices) include(t,1);
        if (script_audio_suspended) include(backlog_voice,1);
        else {
            for (auto& t:voices) include(t,1);
            for (auto& t:sounds) include(t,2);
        }
        for (auto& t:system_sounds) include(t,t.ui_gain==UiGain::Voice ? 1 : 2);
        if (active==0) { std::fill_n(output,count*2,0); meter.frames+=count; return; }
        std::uint64_t unused_changes=0,unused_loops=0;
        const double master=settings.master_volume/100.0;
        for (std::size_t i=0;i<count;++i) {
            double left=0,right=0;
            std::array<bool,3> audible{};
            for (std::size_t j=0;j<active;++j) {
                const auto s=sources[j];
                const auto v=s.track->sample(settings.sample_rate,s.bus==0 ? meter.changes : unused_changes,
                                             s.bus==0 ? meter.loops : unused_loops);
                left+=v[0]; right+=v[1];
                if (std::abs(v[0])>=0.5 || std::abs(v[1])>=0.5) audible[s.bus]=true;
            }
            const auto l=std::lround(left*master), r=std::lround(right*master);
            const auto lo=std::clamp(l,-32768L,32767L),ro=std::clamp(r,-32768L,32767L);
            output[i*2]=static_cast<std::int16_t>(lo); output[i*2+1]=static_cast<std::int16_t>(ro);
            meter.clipped+=l!=lo || r!=ro; meter.nonzero+=lo!=0 || ro!=0;
            meter.peak=std::max(meter.peak,static_cast<int>(std::max(std::abs(lo),std::abs(ro))));
            meter.bgm+=audible[0]; meter.speech+=audible[1]; meter.se+=audible[2];
            meter.multi+=static_cast<int>(audible[0])+audible[1]+audible[2]>=2;
        }
        meter.frames+=count;
    }
};

GameAudio::GameAudio(const std::filesystem::path& root,Settings settings,const resource::FilenameEncoder& encode)
    :impl_(std::make_unique<Impl>(root,std::move(settings),encode)) {}
GameAudio::~GameAudio()=default;
void GameAudio::stop_script_audio() {
    std::lock_guard<std::mutex> command(impl_->commands);
    std::lock_guard<std::mutex> lock(impl_->mixer);
    for (auto& voice:impl_->voices) voice.stop(0);
    for (auto& sound:impl_->sounds) sound.stop(0);
    impl_->backlog_voice.stop(0);
    impl_->script_audio_suspended=false;
}
void GameAudio::suspend_script_audio() {
    std::lock_guard<std::mutex> command(impl_->commands);
    std::lock_guard<std::mutex> lock(impl_->mixer);
    if (impl_->script_audio_suspended) return;
    impl_->script_audio_suspended=true;
    const auto suspend=[](Track& track) {
        if (!track.active) return;
        if (track.remaining!=0 || !track.volume_allowed) {
            track.stop(0);
            return;
        }
        // AllStop ends the current fade and leaves the loop's target gain intact.
        track.gain.time=track.gain.total;
        track.volume_ramp=false;
        track.stop_after_ramp=false;
    };
    for (auto& track:impl_->voices) suspend(track);
    for (auto& track:impl_->sounds) suspend(track);
}
void GameAudio::restart_loop_script_audio() {
    std::lock_guard<std::mutex> command(impl_->commands);
    std::lock_guard<std::mutex> lock(impl_->mixer);
    if (!impl_->script_audio_suspended) return;
    impl_->script_audio_suspended=false;
    impl_->backlog_voice.stop(0);
    // The PC restarts each live loop when entering the dialogue or choice mode again.
    const auto restart=[&](Track& track,bool enabled) {
        if (!track.active) return;
        if (impl_->total_on && enabled) track.phase=0;
        else track.stop(0);
    };
    for (std::size_t channel=0;channel<impl_->voices.size();++channel) {
        auto& track=impl_->voices[channel];
        if (!track.active) continue;
        const auto role=impl_->character_index(track.program->name);
        const bool enabled=role<0 || impl_->character_enabled[static_cast<std::size_t>(role)];
        restart(track,enabled && (channel<2 ? impl_->voice_on : impl_->effect_voice_on));
    }
    for (auto& track:impl_->sounds)
        restart(track,track.system_sound ? impl_->system_se_on : impl_->script_se_on);
}
std::string GameAudio::replay_backlog_voice(const std::string& name) {
    SystemEvent cue;
    cue.opcode=voice;
    cue.text=name;
    cue.parameters={0,0};
    return impl_->submit(cue,"backlog",0,{},SampleOrigin::BacklogVoice);
}
std::string GameAudio::replay_menu_voice(const std::string& name) {
    SystemEvent cue;
    cue.opcode=voice;
    cue.text=name;
    cue.parameters={0,0};
    return impl_->submit(cue,"listenvoice",0,{},SampleOrigin::MenuVoice);
}
bool GameAudio::has_menu_voice(const std::string& name) {
    if (!impl_->settings.enabled) return true;
    std::lock_guard<std::mutex> lock(impl_->commands);
    return impl_->files.has_speech(name);
}
int GameAudio::music_number(const std::string& name) const { return impl_->files.bgm_number(name); }
std::string GameAudio::play_menu_music(int number,int plays,int volume,int fade_in,bool restart) {
    SystemEvent cue;
    cue.opcode=music;
    if (restart) {
        cue.parameters={-1,-1};
        const auto error=impl_->submit(cue,"menu",0,{});
        if (!error.empty()) return error;
    }
    // Common menus may request a placeholder number outside the game's table.
    if (!impl_->files.has_bgm(number)) return {};
    cue.parameters={number,plays,volume,fade_in,-1};
    return impl_->submit(cue,"menu",0,{});
}
void GameAudio::play_music_preview(const std::string& name,int plays,int volume) {
    if (!impl_->settings.enabled) return;
    std::lock_guard<std::mutex> command(impl_->commands);
    auto program=std::make_shared<Program>(impl_->files.bgm(name));
    std::lock_guard<std::mutex> mix(impl_->mixer);
    impl_->song_paused=false;
    impl_->last_music.reset();
    impl_->outgoing={};
    impl_->song.start(std::move(program),plays,gain_curve(0),percent(volume),0);
}
void GameAudio::pause_music_preview(bool paused) {
    std::lock_guard<std::mutex> mix(impl_->mixer);
    impl_->song_paused=paused;
}
void GameAudio::stop_music_preview() {
    std::lock_guard<std::mutex> command(impl_->commands);
    std::lock_guard<std::mutex> mix(impl_->mixer);
    impl_->song={}; impl_->outgoing={};
    impl_->song_paused=false;
    impl_->last_music.reset();
}
void GameAudio::set_music_preview_volume(int volume) {
    std::lock_guard<std::mutex> mix(impl_->mixer);
    impl_->song.set_base(percent(volume));
}
bool GameAudio::music_wave(float* samples,std::size_t count) const {
    std::lock_guard<std::mutex> mix(impl_->mixer);
    const auto& track=impl_->song;
    if (!track.active || impl_->song_paused || !samples || count>1024) return false;
    const auto& clip=*track.program->parts[track.part];
    const auto rate=impl_->settings.sample_rate;
    for (std::size_t i=0;i<count;++i) {
        const auto at=(track.phase+i*clip.info.rate)/rate;
        if (at>=clip.info.frames) { samples[i]=0; continue; }
        const auto sample=static_cast<std::size_t>(at)*clip.info.channels;
        const auto right=sample+std::min(1U,clip.info.channels-1);
        samples[i]=(clip.pcm[sample]+clip.pcm[right])/65536.0F;
    }
    return true;
}

namespace {
struct ScriptTracks {
    Track song,outgoing;
    std::array<Track,4> voices;
    std::array<Track,8> sounds;
};
template<class A,class T> void track_fields(A& ar,T& t) {
    ar(t.part,t.phase,t.remaining,t.stop_after_ramp,t.volume_allowed,t.volume_ramp,t.character,t.system_sound,
        t.gain.time,t.gain.total,t.gain.base,t.gain.reference_base,t.gain.role,t.gain.from,t.gain.to,
        t.pan.from,t.pan.to,t.pan.time,t.pan.total);
}
}

PcAudioState GameAudio::pc_state() const {
    std::lock_guard<std::mutex> commands(impl_->commands);
    std::lock_guard<std::mutex> mixer(impl_->mixer);
    PcAudioState state;
    if (impl_->last_music) {
        state.music_number=impl_->last_music->first-1;
        state.music_plays=impl_->last_music->second;
        const auto& song=impl_->song;
        state.music_volume=song.active ? static_cast<int>(std::lround(song.gain.to[song.gain.index(song.gain.base)])) :
            impl_->settings.music_volume;
    }
    const auto sample=[&](const Track& track,std::array<Value,16>& work) {
        work[0]=track.remaining==0?1:0;
        work[1]=track.source_number;
        const auto& gain=track.gain;
        work[2]=gain.to[gain.index(0)]==1 && gain.to[gain.index(100)]==1 ? -101 :
            static_cast<int>(std::lround(gain.to[gain.index(gain.base)]))-gain.base;
        work[3]=static_cast<int>(std::lround(track.pan.now()*127));
        if (track.pan.time<track.pan.total) {
            const auto remaining=track.pan.total-track.pan.time;
            const auto tenths=remaining/std::max(1U,impl_->settings.sample_rate/10U);
            work[6]=static_cast<Value>(std::min<std::uint64_t>(tenths,INT32_MAX));
            work[7]=static_cast<int>(std::lround(track.pan.to*127));
        }
    };
    for (std::size_t i=0;i<impl_->sounds.size();++i) {
        const auto& track=impl_->sounds[i];
        if (track.active && track.volume_allowed && track.remaining==0) sample(track,state.sounds[i]);
    }
    for (std::size_t i=0;i<impl_->voices.size();++i) {
        const auto& track=impl_->voices[i];
        if (track.active && track.volume_allowed) {
            sample(track,state.voices[i]);
            state.voice_names[i]=track.program->name;
        }
    }
    return state;
}

void GameAudio::save_snapshot(snapshot::Writer& out) const {
    auto tracks=std::make_unique<ScriptTracks>();
    std::lock_guard<std::mutex> command(impl_->commands);
    bool paused;
    {
        std::lock_guard<std::mutex> mix(impl_->mixer);
        tracks->song=impl_->song;
        tracks->outgoing=impl_->outgoing;
        tracks->voices=impl_->voices;
        tracks->sounds=impl_->sounds;
        paused=impl_->song_paused;
    }
    out(impl_->settings.sample_rate,paused,impl_->fade_se,impl_->fade_voice,
        impl_->last_music.has_value(),impl_->next_music.has_value());
    if (impl_->last_music) out(impl_->last_music->first,impl_->last_music->second);
    if (impl_->next_music) out(*impl_->next_music);
    out(std::vector<int>(impl_->voice_flags.begin(),impl_->voice_flags.end()));
    const auto track=[&](const Track& t) {
        out(t.active);
        if (!t.active) return;
        const auto& info=t.program->parts[t.part]->info;
        out(t.program->name,t.source_number,info.name,info.frames,info.rate);
        track_fields(out,t);
    };
    track(tracks->song);track(tracks->outgoing);
    for (const auto& t:tracks->voices) track(t);
    for (const auto& t:tracks->sounds) track(t);
}

void GameAudio::load_snapshot(snapshot::Reader& in,bool keep_music_position,
                              std::optional<unsigned> jump_voice_mask,bool finish_script_fades) {
    read_snapshot(in,keep_music_position,jump_voice_mask,finish_script_fades,false);
}

void GameAudio::inspect_snapshot(snapshot::Reader& in) {
    Settings settings;
    settings.enabled=false;
    GameAudio state({},settings);
    state.read_snapshot(in,false,{},false,true);
}

void GameAudio::read_snapshot(snapshot::Reader& in,bool keep_music_position,
                              std::optional<unsigned> jump_voice_mask,bool finish_script_fades,bool inspect) {
    using snapshot::invalid;
    std::lock_guard<std::mutex> command(impl_->commands);
    unsigned rate=0;bool paused=false,last=false,next=false;
    int se_fade=0,voice_fade=0;
    in(rate,paused,se_fade,voice_fade,last,next);
    if (rate<8000 || rate>192000 || (!inspect && rate!=impl_->settings.sample_rate))
        invalid("Invalid snapshot audio sample rate");
    std::optional<std::pair<int,int>> last_music;
    std::optional<int> next_music;
    if (last) {
        last_music.emplace();in(last_music->first,last_music->second);
        if (!inspect && !impl_->files.has_bgm(last_music->first)) invalid("Invalid snapshot BGM number");
    }
    if (next) { next_music.emplace();in(*next_music); }
    std::vector<int> flags;in(flags);
    std::set<int> voice_flags(flags.begin(),flags.end());
    std::array<std::shared_ptr<const Program>,2> music;
    {
        std::lock_guard<std::mutex> mix(impl_->mixer);
        music={impl_->song.program,impl_->outgoing.program};
    }
    const auto track=[&](Track& t,int kind) {
        in(t.active);
        if (!t.active) return;
        if (!inspect && !impl_->settings.enabled) invalid("Cannot restore active audio when playback is disabled");
        std::string name,segment;std::uint64_t frames=0;unsigned source_rate=0;
        in(name,t.source_number,segment,frames,source_rate);track_fields(in,t);
        if (name.empty() || name.size()>255 || name.find('\0')!=std::string::npos || t.remaining<0 ||
            t.gain.time>t.gain.total || t.pan.time>t.pan.total || t.gain.base<0 || t.gain.base>100 ||
            t.gain.reference_base<0 || t.gain.reference_base>100 || t.gain.role>=character_levels ||
            t.character< -1 || t.character>=72 || std::abs(t.pan.from)>1 || std::abs(t.pan.to)>1)
            invalid("Invalid snapshot audio track");
        if (source_rate==0 || t.phase/rate>=frames || (kind!=0 && t.part!=0))
            invalid("Invalid snapshot audio position");
        for (const auto* curve:{&t.gain.from,&t.gain.to}) for (const auto gain:*curve)
            if (gain<0 || gain>100) invalid("Invalid snapshot audio gain");
        if (inspect) return;
        const auto matches=[&](const Program& program) {
            if (t.part>=program.parts.size()) return false;
            const auto& info=program.parts[t.part]->info;
            return segment==info.name && frames==info.frames && source_rate==info.rate;
        };
        if (kind==0) {
            for (const auto& program:music) {
                if (program && program->name==name && matches(*program)) { t.program=program;break; }
            }
            if (!t.program) t.program=std::make_shared<Program>(impl_->files.bgm(name));
        }
        else {
            Program p;
            p.name=name;p.parts.push_back(kind==1?impl_->files.speech(name):impl_->files.se(t.source_number,false));
            t.program=std::make_shared<Program>(std::move(p));
        }
        if (!matches(*t.program))
            invalid("Snapshot audio resource has changed");
    };
    auto tracks=std::make_unique<ScriptTracks>();
    track(tracks->song,0);track(tracks->outgoing,0);
    for (auto& t:tracks->voices) track(t,1);
    for (auto& t:tracks->sounds) track(t,2);
    if (in.remaining()) invalid("Unexpected data at end of snapshot");
    if (inspect) return;
    {
        std::lock_guard<std::mutex> mix(impl_->mixer);
        if (jump_voice_mask) {
            const auto& settings=impl_->settings;
            for (auto* t:{&tracks->song,&tracks->outgoing}) {
                t->set_base(settings.music_volume);
                if (!impl_->total_on || !impl_->music_on) t->stop(0);
            }
            const auto restart=[&](Track& t,bool enabled,bool replay_voice) {
                if (!t.active) return;
                if (!impl_->total_on || !enabled || !t.volume_allowed ||
                    (t.remaining!=0 && !replay_voice) || (finish_script_fades && t.stop_after_ramp)) {
                    t.stop(0);
                    return;
                }
                t.phase=0;
                if (finish_script_fades) {
                    t.gain.time=t.gain.total;
                    t.volume_ramp=false;
                }
            };
            for (std::size_t i=0;i<tracks->voices.size();++i) {
                auto& t=tracks->voices[i];
                t.set_base(i<2 ? settings.voice_volume : settings.effect_voice_volume);
                const auto role=t.active ? impl_->character_index(t.program->name) : -1;
                const bool enabled=role<0 || impl_->character_enabled[static_cast<std::size_t>(role)];
                if (t.character>=0) t.set_role(impl_->character_steps(static_cast<std::size_t>(t.character)));
                restart(t,enabled && (i<2 ? impl_->voice_on : impl_->effect_voice_on),(*jump_voice_mask&(1U<<i))!=0);
            }
            for (auto& t:tracks->sounds) {
                t.set_base(t.system_sound ? settings.system_sound_volume : settings.sound_volume);
                restart(t,t.system_sound ? impl_->system_se_on : impl_->script_se_on,false);
            }
        }
        auto& restored=tracks->song;
        const auto& current=impl_->song;
        if (keep_music_position && current.active && restored.active && current.program==restored.program &&
            !paused && !impl_->song_paused && !current.stop_after_ramp && !restored.stop_after_ramp &&
            (current.program->loop_start || (current.remaining==0 && restored.remaining==0))) {
            restored.part=current.part;
            restored.phase=current.phase;
        }
        std::swap(impl_->song,restored);std::swap(impl_->outgoing,tracks->outgoing);impl_->song_paused=paused;
        impl_->voices.swap(tracks->voices);impl_->sounds.swap(tracks->sounds);
        impl_->script_audio_suspended=false;
        impl_->backlog_voice.stop(0);
        impl_->last_music=last_music;impl_->next_music=next_music;impl_->fade_se=se_fade;impl_->fade_voice=voice_fade;
        impl_->voice_flags.swap(voice_flags);
    }
}
std::string GameAudio::submit(const SystemEvent& e,const std::string& script,Word word,SkipFlags skip,bool* voice_started) {
    return impl_->submit(e,script,word,skip,SampleOrigin::Script,voice_started);
}
std::string GameAudio::submit_pc_voice(const SystemEvent& e,const std::string& script,Word word) {
    if (e.opcode!=voice) throw std::invalid_argument("PC voice state requires a VOICE event");
    return impl_->submit(e,script,word,{},SampleOrigin::PcVoice);
}
void GameAudio::pull(std::int16_t* stereo,std::size_t frames) {
    if (!stereo && frames) throw std::invalid_argument("Missing stereo PCM output");
    while (frames) { const auto n=std::min<std::size_t>(frames,512); impl_->render(stereo,n); stereo+=n*2; frames-=n; }
}
void GameAudio::skip_frames(std::uint64_t frames) {
    std::lock_guard<std::mutex> lock(impl_->mixer);
    std::uint64_t changes=0,loops=0;
    auto skip=[&](Track& track){ track.skip(sample_rate(),frames,changes,loops); };
    skip(impl_->song); skip(impl_->outgoing);
    for (auto& t:impl_->button_voices) skip(t);
    if (impl_->script_audio_suspended) skip(impl_->backlog_voice);
    else {
        for (auto& t:impl_->voices) skip(t);
        for (auto& t:impl_->sounds) skip(t);
    }
    for (auto& t:impl_->system_sounds) skip(t);
}
void GameAudio::set_character_slider(const std::string& name,int steps) {
    std::lock_guard<std::mutex> lock(impl_->commands);
    const auto i=impl_->character_slots.find(resource::fold_cp932(name));
    if (i==impl_->character_slots.end()) return;
    const auto index=i->second;
    if (index>=impl_->character_enabled.size()) return;
    impl_->update_character(index,std::clamp(steps,-16,15),impl_->character_enabled[index]);
}
void GameAudio::set_character_volume(unsigned index,int steps,bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->commands);
    if (index>=impl_->character_enabled.size() || index+1>=impl_->characters.names.size()/2)
        throw std::invalid_argument("Character volume index unavailable");
    impl_->update_character(index,std::clamp(steps,-10,5),enabled);
}
unsigned GameAudio::character_volume_count() const noexcept {
    const auto size=impl_->characters.names.size()/2;
    return static_cast<unsigned>(std::min<std::size_t>(size ? size-1 : 0,impl_->character_enabled.size()));
}
unsigned GameAudio::character_slider_index(unsigned role) const {
    if (role>=character_volume_count()) throw std::invalid_argument("Character volume index unavailable");
    return static_cast<unsigned>(impl_->slider_index(role));
}
unsigned GameAudio::system_voice_selection_count() const noexcept { return impl_->files.system_voice_selection_count(); }
int GameAudio::voice_character_number(const std::string& name) const { return impl_->character_number(name); }
void GameAudio::set_voice_off(const std::array<Word,32>& flags) {
    std::lock_guard<std::mutex> lock(impl_->commands);
    for (std::size_t role=0;role<impl_->character_enabled.size();++role) {
        const auto group=impl_->slider_index(role);
        // CCommonSystemFile uses32 interleaved words, not a conventional bitset.
        impl_->character_enabled[role]=group>=1024 || (flags[group%32]&(Word{1}<<(group/32)))==0;
    }
}
void GameAudio::apply_runtime_parameters(const RuntimeParameters& p) {
    if (!impl_->settings.enabled) return;
    std::lock_guard<std::mutex> command_lock(impl_->commands);
    const bool music_on=p.get(NNNPARAM_MUSICSWITCH)!=0,total_on=p.get(NNNPARAM_TOTALSWITCH)!=0;
    const bool stop_music=(!music_on && impl_->music_on) || (!total_on && impl_->total_on);
    const bool replay_music=music_on && total_on && ((!impl_->music_on && music_on) || (!impl_->total_on && total_on));
    constexpr unsigned roles=72; // Parameters are slider groups, not character IDs.
    // Validate role arithmetic before changing any output state.
    for (unsigned i=0;i<roles;++i) {
        const auto adjustment=static_cast<std::int64_t>(p.get(NNNPARAM_CHARAVOICEVOLUME+i))*impl_->characters.slider_delta;
        if (adjustment<INT32_MIN || adjustment>INT32_MAX) throw std::invalid_argument("Character volume adjustment overflow");
    }
    impl_->music_on=music_on; impl_->total_on=total_on;
    impl_->system_se_on=p.get(NNNPARAM_SOUNDSWITCH)!=0; impl_->script_se_on=p.get(NNNPARAM_SCRIPTSESWITCH)!=0;
    impl_->voice_on=p.get(NNNPARAM_VOICESWITCH)!=0; impl_->effect_voice_on=p.get(NNNPARAM_SOUNDVOICESWITCH)!=0;
    impl_->continue_voice=p.get(NNNPARAM_CONTINUEVOICESWITCH)!=0;
    {
        std::lock_guard<std::mutex> lock(impl_->mixer);
        auto& s=impl_->settings;
        s.music_volume=percent(p.get(NNNPARAM_MUSICVOLUME)); s.master_volume=percent(p.get(NNNPARAM_TOTALVOLUME));
        s.sound_volume=percent(p.get(NNNPARAM_SCRIPTSEVOLUME)); s.system_sound_volume=percent(p.get(NNNPARAM_SOUNDVOLUME));
        s.voice_volume=percent(p.get(NNNPARAM_VOICEVOLUME)); s.effect_voice_volume=percent(p.get(NNNPARAM_SOUNDVOICEVOLUME));
        impl_->movie_volume=percent(p.get(NNNPARAM_MOVIEVOLUME));
        impl_->song.set_base(s.music_volume); impl_->outgoing.set_base(s.music_volume);
        for (std::size_t i=0;i<impl_->voices.size();++i)
            impl_->voices[i].set_base(i<2 ? s.voice_volume : s.effect_voice_volume);
        for (auto& t:impl_->sounds) t.set_base(t.system_sound ? s.system_sound_volume : s.sound_volume);
        for (auto& t:impl_->system_sounds) t.set_base(impl_->ui_volume(t.ui_gain));
        for (auto& t:impl_->button_voices) t.set_base(s.voice_volume);
        impl_->backlog_voice.set_base(s.voice_volume);
        for (unsigned i=0;i<roles;++i) {
            const auto steps=p.get(NNNPARAM_CHARAVOICEVOLUME+i);
            auto& bits=impl_->characters.packed_sliders[i/6]; const auto shift=(i%6)*5U;
            bits=(bits&~(31U<<shift))|((static_cast<Word>(steps)&31U)<<shift);
            for (auto& t:impl_->voices) if (t.character==static_cast<int>(i)) t.set_role(steps);
            for (auto& t:impl_->button_voices) if (t.character==static_cast<int>(i)) t.set_role(steps);
            for (auto& t:impl_->system_sounds) if (t.character==static_cast<int>(i)) t.set_role(steps);
            if (impl_->backlog_voice.character==static_cast<int>(i)) impl_->backlog_voice.set_role(steps);
        }
        if (stop_music) impl_->song.stop(impl_->fade(s.default_music_fade_out));
    }
    if (replay_music && impl_->last_music) {
        SystemEvent event; event.opcode=music; event.parameters={impl_->last_music->first,impl_->last_music->second};
        EventInfo info; info.opcode=music; info.action="music.switch_replay";
        // Switch playback uses PlayMusic(number,loop)'s defaults, never a pending
        // script fade or the prior command's delta/fade arguments.
        const auto pending=impl_->next_music; impl_->next_music.reset();
        try { impl_->bgm(event,info,{}); }
        catch (const AudioError& error) {
            if (error.missing) ++impl_->totals.missing; else ++impl_->totals.errors;
            info.error=error.what();
        }
        catch (...) { impl_->next_music=pending; throw; }
        impl_->next_music=pending;
        if (impl_->settings.record_events) {
            { std::lock_guard<std::mutex> lock(impl_->mixer); info.at_output_frame=impl_->meter.frames; }
            impl_->totals.log.push_back(std::move(info));
        }
    }
}
void GameAudio::set_master_volume(int volume) {
    std::lock_guard<std::mutex> command_lock(impl_->commands);
    std::lock_guard<std::mutex> mixer_lock(impl_->mixer);
    impl_->settings.master_volume=percent(volume);
}
void GameAudio::set_se_volumes(int script,int system) {
    std::lock_guard<std::mutex> command_lock(impl_->commands);
    std::lock_guard<std::mutex> mixer_lock(impl_->mixer);
    impl_->settings.sound_volume=percent(script); impl_->settings.system_sound_volume=percent(system);
    for (auto& track:impl_->sounds) track.set_base(track.system_sound ? impl_->settings.system_sound_volume : impl_->settings.sound_volume);
    for (auto& track:impl_->system_sounds) track.set_base(impl_->ui_volume(track.ui_gain));
}
void GameAudio::set_system_voice_selection(unsigned index) {
    if (!impl_->settings.enabled) return;
    std::lock_guard<std::mutex> lock(impl_->commands);
    impl_->files.set_system_voice_selection(index);
}
void GameAudio::set_next_music_fade(int units) {
    std::lock_guard<std::mutex> lock(impl_->commands); impl_->next_music=std::max(0,units);
}
void GameAudio::set_volumes(int music,int sound,int voice,int effect_voice) {
    std::lock_guard<std::mutex> command_lock(impl_->commands);
    std::lock_guard<std::mutex> mixer_lock(impl_->mixer);
    auto& s=impl_->settings;
    s.music_volume=percent(music); s.sound_volume=percent(sound);
    s.system_sound_volume=s.sound_volume;
    s.voice_volume=percent(voice); s.effect_voice_volume=percent(effect_voice);
    impl_->song.set_base(s.music_volume); impl_->outgoing.set_base(s.music_volume);
    for (std::size_t i=0;i<impl_->voices.size();++i)
        impl_->voices[i].set_base(i<2 ? s.voice_volume : s.effect_voice_volume);
    for (auto& t:impl_->sounds) t.set_base(s.sound_volume);
    for (auto& t:impl_->system_sounds) t.set_base(impl_->ui_volume(t.ui_gain));
    for (auto& t:impl_->button_voices) t.set_base(s.voice_volume);
    impl_->backlog_voice.set_base(s.voice_volume);
}
std::string GameAudio::play_system_se(unsigned id,unsigned channel_id,bool voice_volume) {
    if (id>INT32_MAX || channel_id>INT32_MAX) throw std::invalid_argument("Invalid system SE");
    SystemEvent e; e.opcode=sound; e.parameters={static_cast<Value>(id),static_cast<Value>(channel_id)};
    return impl_->submit(e,"system",0,{},voice_volume ? SampleOrigin::SystemSoundAtVoiceVolume : SampleOrigin::SystemSound);
}
std::string GameAudio::play_button_voice(unsigned id,bool ignore_voice_switch,bool ignore_character_mute) {
    if (id>INT32_MAX) throw std::invalid_argument("Invalid button voice");
    SystemEvent e; e.opcode=sound; e.parameters={static_cast<Value>(id),0};
    return impl_->submit(e,"buttonVoice",0,{},SampleOrigin::ButtonVoice,nullptr,{ignore_voice_switch,ignore_character_mute});
}
std::string GameAudio::play_config_feedback(unsigned slider,int position,int steps) {
    if (slider>=impl_->config_cues.size() || steps<1 || steps>10000 || position<0 || position>steps)
        throw std::invalid_argument("Invalid configuration slider preview");
    const auto cue=impl_->config_cues[slider];
    if (cue.number<=0) return {};
    std::int64_t index=cue.number-1;
    if (cue.divisions>0 && position>0) {
        const int bucket_width=std::max(1,steps/cue.divisions);
        index+=position==steps ? std::int64_t{cue.divisions}+1 :
            std::min(cue.divisions,(position+bucket_width-1)/bucket_width);
    }
    if (index>INT32_MAX) return {};
    if (cue.voice && impl_->config_uses_button_voice)
        return index>0 ? play_button_voice(static_cast<unsigned>(index-1)) : std::string{};
    SystemEvent e; e.opcode=sound; e.parameters={static_cast<Value>(index),0};
    const auto origin=cue.voice ? SampleOrigin::ConfigVoice : slider==4 ? SampleOrigin::ConfigMovieSound : SampleOrigin::ConfigSound;
    return impl_->submit(e,"configSe",0,{},origin);
}
bool GameAudio::voice_flag_set(int flag) const {
    std::lock_guard<std::mutex> lock(impl_->commands); return impl_->voice_flags.count(flag)!=0;
}
std::vector<int> GameAudio::legacy_voice_flags() const {
    std::lock_guard<std::mutex> lock(impl_->commands);
    return {impl_->voice_flags.begin(),impl_->voice_flags.end()};
}
std::uint32_t GameAudio::voice_duration_ms(unsigned channel) const {
    if (channel>=impl_->voices.size()) throw std::invalid_argument("Invalid voice channel");
    std::lock_guard<std::mutex> lock(impl_->mixer);
    const auto& track=impl_->voices[channel];
    if (!track.program) return 0;
    std::uint64_t length=0;
    for (const auto& clip:track.program->parts) length+=clip->info.frames*1000U/clip->info.rate;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(length,std::numeric_limits<std::uint32_t>::max()));
}
unsigned GameAudio::sample_rate() const noexcept { return impl_->settings.sample_rate; }
Statistics GameAudio::stats() const {
    std::lock_guard<std::mutex> guard(impl_->commands);
    auto s=impl_->totals; s.decoded=impl_->files.decodes; s.cache_hits=impl_->files.hits;
    std::lock_guard<std::mutex> lock(impl_->mixer);
    const auto& m=impl_->meter;
    s.output_frames=m.frames;s.nonzero_frames=m.nonzero;s.clipped_frames=m.clipped;s.peak=m.peak;
    s.music_frames=m.bgm;s.voice_frames=m.speech;s.sound_frames=m.se;s.overlapping_frames=m.multi;
    s.music_segment_changes=m.changes;s.music_loops=m.loops; return s;
}
PlaybackState GameAudio::state() const {
    std::lock_guard<std::mutex> lock(impl_->mixer);
    PlaybackState result; const unsigned rate=sample_rate();
    result.bgm=impl_->song.state(rate);
    for(std::size_t i=0;i<result.voices.size();++i) result.voices[i]=impl_->voices[i].state(rate);
    for(std::size_t i=0;i<result.sounds.size();++i) result.sounds[i]=impl_->sounds[i].state(rate);
    return result;
}
} // namespace nnn::audio
