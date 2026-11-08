#pragma once

#include "nnn/script.hpp"
#include "nnn/runtime_parameters.hpp"
#include "nnn/resource_path.hpp"
#include "nnn/snapshot.hpp"
#include <filesystem>
#include <map>
#include <optional>

namespace nnn::audio {
inline constexpr Word music=0x33330007U, voice=0x33330008U, sound=0x33330009U;
inline constexpr Word next_fade_se=0x33330066U, next_fade_voice=0x33330067U;
inline constexpr Word volume_se=0x33330068U, volume_voice=0x33330069U, volume_music=0x3333006aU;
inline constexpr Word set_voice_flag=0x3333006cU;
bool is_audio_command(Word opcode) noexcept;

struct Settings {
    unsigned sample_rate=44100;
    int music_volume=80, sound_volume=80, voice_volume=80, effect_voice_volume=80;
    int character_volume_step=2, default_music_fade_in=-1, default_music_fade_out=-1; // -1 reads game defaults(0/5 if absent).
    bool record_events=false;
    std::size_t cache_bytes=64U*1024U*1024U;
    int master_volume=100, system_sound_volume=-1; // -1 initially follows sound_volume.
    bool enabled=true; // Decorative sessions can skip all audio resource loading.
};
struct ClipInfo {
    std::string name, source;
    unsigned rate=0, channels=0;
    std::uint64_t frames=0;
};
struct EventInfo {
    Word opcode=0, word=0;
    std::string script, action, error;
    int channel=-1;
    std::vector<Value> parameters;
    std::vector<ClipInfo> clips;
    std::uint64_t at_output_frame=0;
};
struct Statistics {
    std::uint64_t events=0, plays=0, controls=0, decoded=0, cache_hits=0, missing=0, errors=0;
    std::uint64_t output_frames=0, nonzero_frames=0, clipped_frames=0;
    std::uint64_t music_frames=0, voice_frames=0, sound_frames=0, overlapping_frames=0;
    std::uint64_t music_segment_changes=0, music_loops=0;
    int peak=0;
    std::map<Word,std::uint64_t> opcodes;
    std::vector<EventInfo> log;
};
struct TrackState {
    bool active=false, looping=false, fading=false;
    std::string name, segment;
    int volume=0;
    std::uint64_t source_frame=0;
};
struct PlaybackState {
    TrackState bgm;
    std::array<TrackState,4> voices;
    std::array<TrackState,8> sounds;
};
struct PcAudioState {
    int music_number=-1,music_plays=0,music_volume=0;
    std::array<std::array<Value,16>,8> sounds{},voices{};
    std::array<std::string,4> voice_names;
};

// Submit from the game thread. Pull interleaved stereo PCM16 from one audio consumer.
// I/O and Vorbis decoding happen before a track enters the mixer.
class GameAudio {
public:
    explicit GameAudio(const std::filesystem::path& game_root,Settings settings={},const resource::FilenameEncoder& encode={});
    ~GameAudio();
    GameAudio(const GameAudio&)=delete;
    GameAudio& operator=(const GameAudio&)=delete;
    std::string submit(const SystemEvent& event,const std::string& script={},Word word=0,SkipFlags skip={},
                       bool* voice_started=nullptr);
    // A PC slot already includes the saved role correction in its VOICE delta.
    std::string submit_pc_voice(const SystemEvent& event,const std::string& script={},Word word=0);
    void pull(std::int16_t* stereo,std::size_t frames);
    // Advance sources without generating output while reconstructing a saved session.
    void skip_frames(std::uint64_t frames);
    void set_character_slider(const std::string& prefix,int steps);
    // Thread-safe live category settings, clamped0..100. Does not restart tracks or fades.
    void set_volumes(int music,int sound,int voice,int effect_voice);
    void set_master_volume(int volume);
    void set_se_volumes(int script,int system);
    // Zero-based named role after the charaname table header. Gain changes are
    // live; enabled gates future plays, as the source's character-off flag does.
    void set_character_volume(unsigned index,int steps,bool enabled);
    unsigned character_volume_count() const noexcept;
    unsigned character_slider_index(unsigned role) const;
    unsigned system_voice_selection_count() const noexcept;
    void set_voice_off(const std::array<Word,32>& flags);
    // GetSystemParam's effective snapshot (including configuration aliases).
    // Volumes update live; only music/total switch transitions stop/replay BGM.
    void apply_runtime_parameters(const RuntimeParameters& parameters);
    void set_system_voice_selection(unsigned index);
    void set_next_music_fade(int tenths);
    std::string play_system_se(unsigned zero_based_id,unsigned channel=0,bool voice_volume=false);
    std::string play_button_voice(unsigned zero_based_id,bool ignore_voice_switch=false,bool ignore_character_mute=false);
    // CCommonConfig slider preview, evaluated once when the capture ends.
    std::string play_config_feedback(unsigned slider,int position,int steps);
    void stop_script_audio();
    // Menus stop one-shot script sounds while retaining the scene's loop sources.
    void suspend_script_audio();
    void restart_loop_script_audio();
    std::string replay_backlog_voice(const std::string& name);
    std::string replay_menu_voice(const std::string& name);
    bool has_menu_voice(const std::string& name);
    int music_number(const std::string& name) const;
    std::string play_menu_music(int number,int plays,int volume,int fade_in,bool restart);
    void play_music_preview(const std::string& name,int plays,int volume);
    void stop_music_preview();
    void pause_music_preview(bool paused);
    void set_music_preview_volume(int volume);
    bool music_wave(float* samples,std::size_t count) const;
    bool voice_flag_set(int flag) const;
    std::vector<int> legacy_voice_flags() const;
    // Full decoded duration, independent of playback position or audible gain.
    std::uint32_t voice_duration_ms(unsigned channel) const;
    unsigned sample_rate() const noexcept;
    Statistics stats() const;
    PlaybackState state() const;
    PcAudioState pc_state() const;
    void save_snapshot(snapshot::Writer& out) const;
    // The audio payload ends the session snapshot. When requested, an already
    // playing loop of the same program keeps its current sample position.
    void load_snapshot(snapshot::Reader& in,bool keep_music_position=false,
                       std::optional<unsigned> jump_voice_mask={},bool finish_script_fades=false);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace nnn::audio
