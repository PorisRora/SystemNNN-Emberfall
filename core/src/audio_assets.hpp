#pragma once

#include "nnn/game_audio.hpp"
#include <list>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace nnn::audio {
struct AudioError : std::runtime_error {
    AudioError(std::string detail,bool absent=false) : std::runtime_error(std::move(detail)),missing(absent) {}
    bool missing;
};
struct Clip {
    ClipInfo info;
    std::vector<std::int16_t> pcm;
};
using AudioClip=std::shared_ptr<const Clip>;
struct Program {
    std::string name;
    std::vector<AudioClip> parts;
    std::optional<std::size_t> loop_start;
};
class AudioAssets {
public:
    AudioAssets()=default;
    AudioAssets(const std::filesystem::path& root,std::size_t limit,const resource::FilenameEncoder& encode={});
    AudioClip speech(const std::string& name);
    bool has_speech(const std::string& name);
    AudioClip se(int number,bool system);
    AudioClip system_wave(const std::string& name);
    std::string default_system_sound_name(int number) const;
    std::string button_voice_name(int number) const;
    std::string config_sound_name(int number,bool themed) const;
    void set_system_voice_selection(unsigned index);
    unsigned system_voice_selection_count() const noexcept { return system_voice_count_; }
    bool has_bgm(int number) const noexcept { return number>0 && static_cast<std::size_t>(number)<=bgms_.size(); }
    Program bgm(int one_based_number);
    Program bgm(const std::string& name);
    int bgm_number(const std::string& name) const;
    const std::vector<std::string>& characters() const { return characters_; }
    std::uint64_t decodes=0,hits=0;
private:
    using Path=std::filesystem::path;
    struct Member {std::uint64_t begin,end;};
    struct Pack {Path file; std::unordered_map<std::string,Member> members;};
    struct Cached {std::string key; AudioClip clip;};
    Path root_;
    std::size_t limit_=0,bytes_=0;
    std::map<std::string,Path> speech_,sys_,se_,wgq_,mml_,tables_,packs_;
    std::map<std::string,Pack> indexes_;
    std::set<std::string> failed_voice_indexes_;
    std::vector<std::string> bgms_,sounds_,system_sounds_,button_voices_,characters_;
    std::vector<std::string> default_system_sounds_,config_sounds_,themed_config_sounds_;
    unsigned system_voice_count_=0,system_voice_=0;
    std::list<Cached> recent_;
    std::unordered_map<std::string,std::list<Cached>::iterator> cache_;
    const Pack& voice_index(const std::string& prefix);
    const Path* loose_speech(const std::string& key,const std::string& prefix) const;
    AudioClip read(const Path& file,const std::string& name,Member range,bool music);
    AudioClip loose(const Path& file,const std::string& name,bool music);
};
} // namespace nnn::audio
