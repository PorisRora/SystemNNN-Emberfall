// Additional acceptance paths not visited by the game's opening. Uses installed assets only.
#include "audio_assets.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace nnn;
using namespace nnn::audio;
using Path=std::filesystem::path;
void check(bool condition,const std::string& what) { if (!condition) throw std::runtime_error(what); }
void send(GameAudio& mix,Word op,std::vector<Value> args,const std::string& name={}) {
    SystemEvent e; e.opcode=op; e.parameters=std::move(args); if (!name.empty()) e.text=name;
    auto error=mix.submit(e); if (!error.empty()) throw std::runtime_error(error);
}
void tick(GameAudio& mix,std::uint64_t frames) {
    std::array<std::int16_t,2048> pcm;
    while (frames) {
        auto n=static_cast<std::size_t>(std::min<std::uint64_t>(frames,pcm.size()/2));
        mix.pull(pcm.data(),n); frames-=n;
    }
}
void playlist(const Path& root,int id,const std::vector<std::string>& names,std::size_t repeat) {
    Settings s; s.music_volume=100;
    AudioAssets input(root,0); const auto score=input.bgm(id);
    check(score.parts.size()==names.size(),"Wrong number of MML segments");
    for (std::size_t i=0;i<names.size();++i)
        check(score.parts[i]->info.name==names[i] && score.parts[i]->info.channels==2 &&
              score.parts[i]->info.rate==s.sample_rate,"Unexpected native segment: "+names[i]);
    GameAudio mix(root,s); send(mix,music,{id,0,0,0,0});
    // Independent reference: the original decoded segments concatenated in the expected order.
    // An irregular device buffer crosses each transition. Compare every sample through the next loop.
    std::uint64_t end=73; for (const auto& c:score.parts) end+=c->info.frames;
    std::size_t part=0,i=0; std::array<std::int16_t,1994> pcm;
    for (std::uint64_t frame=0;frame<end;) {
        auto n=static_cast<std::size_t>(std::min<std::uint64_t>(997,end-frame));
        mix.pull(pcm.data(),n);
        for (std::size_t j=0;j<n;++j) {
            const auto& c=*score.parts[part];
            if (pcm[j*2]!=c.pcm[i*2] || pcm[j*2+1]!=c.pcm[i*2+1])
                throw std::runtime_error(score.name+": PCM differs at frame "+std::to_string(frame+j));
            if (++i==c.info.frames) { i=0; if (++part==score.parts.size()) part=repeat; }
        }
        frame+=n;
    }
    const auto stats=mix.stats();
    check(stats.music_loops==1 && stats.music_segment_changes==names.size() && mix.state().bgm.segment==names[repeat],"Loop did not return to its designated segment");
    GameAudio restored(root,s); send(restored,music,{id,0,0,0,0});
    restored.skip_frames(end);
    std::array<std::int16_t,512> live{},resumed{};
    mix.pull(live.data(),live.size()/2); restored.pull(resumed.data(),resumed.size()/2);
    check(live==resumed,"Restoring across a loop changed the next PCM samples: "+score.name);
    std::cout<<score.name<<": "<<end<<" stereo frames match original segments; returns to "<<names[repeat]<<'\n';
}
void channels(const Path& root) {
    Settings s; s.sample_rate=48000; s.record_events=true;
    GameAudio mix(root,s);
    mix.set_character_slider("AA",5);
    send(mix,voice,{0,0,-20},"AA020001"); // Pack offset zero is a valid member.
    send(mix,voice,{0,1},"ee020001");
    send(mix,voice,{0,2,0,1},"aa081006"); // Loose chapter patch.
    send(mix,voice,{0,3,0,1},"01_ok01"); // System voice fallback.
    auto st=mix.state();
    check(st.voices[0].volume==70 && st.voices[2].volume==90,"charaname role volume");
    auto log=mix.stats().log;
    const std::vector<std::string> paths={"cdvaw/aa.vpk","cdvaw/ee.vpk","cdvaw/aa/aa081006.vaw","vaw/sys/01_ok01.vaw"};
    for (std::size_t i=0;i<paths.size();++i)
        check(st.voices[i].active && log[i].clips.at(0).source==paths[i],"Incorrect voice channel or route: "+paths[i]);
    tick(mix,4800);
    for (const auto& t:mix.state().voices) check(t.source_frame>0,"Voice did not advance");
    send(mix,voice,{-1,2}); st=mix.state();
    check(st.voices[0].active && st.voices[1].active && !st.voices[2].active && st.voices[3].active,"VOICE stop affected another channel");
    send(mix,volume_voice,{0,-999});
    check(mix.state().voices[0].active && !mix.state().voices[0].volume,"Immediate -999 should mute while time continues");
    send(mix,volume_voice,{0,-101}); check(mix.state().voices[0].volume==1,"-101 should set gain 1");
    send(mix,next_fade_voice,{2}); send(mix,volume_voice,{1,-999}); tick(mix,4800);
    check(mix.state().voices[1].volume==40 && mix.state().voices[1].fading,"Voice fade midpoint");
    tick(mix,4800); check(!mix.state().voices[1].active && mix.state().voices[3].active,"Voice fade endpoint or channel isolation");
    send(mix,voice,{-1,0});
    const auto loop=log[3].clips.at(0);
    tick(mix,loop.frames*48000/loop.rate+17);
    check(mix.state().voices[3].active && mix.state().voices[3].looping,"Environment voice did not loop");
    send(mix,sound,{0,7,0,1}); send(mix,next_fade_se,{2}); send(mix,volume_se,{7,-40}); tick(mix,4800);
    check(mix.state().sounds[7].volume==60,"SE fade midpoint"); tick(mix,4800);
    check(mix.state().sounds[7].volume==40,"SE fade endpoint");
    check(mix.play_system_se(0,7).empty(),"System SE lookup failed"); tick(mix,512);
    check(mix.state().sounds[7].active && mix.stats().log.back().clips.at(0).source=="vaw/sys/01_ok01.vaw","System SE overrode script SE");
    send(mix,next_fade_se,{1}); send(mix,sound,{-1,7}); tick(mix,4800);
    check(!mix.state().sounds[7].active,"SE did not stop at fade endpoint");
    send(mix,voice,{-1,3}); send(mix,music,{2,0,0,2,0}); tick(mix,4800);
    check(mix.state().bgm.volume==40,"BGM fade in at device sample rate"); tick(mix,4800);
    send(mix,volume_music,{-40,2}); tick(mix,4800); check(mix.state().bgm.volume==60,"BGM volume ramp");
    send(mix,music,{-2,-20}); check(mix.state().bgm.volume==60,"MUSIC(-2) should change live gain");
    send(mix,set_voice_flag,{4}); check(mix.voice_flag_set(4) && mix.state().bgm.volume==60 && !mix.state().bgm.fading,"Incorrect voice flag or BGM state");
    mix.set_next_music_fade(2); send(mix,music,{-1}); tick(mix,4800);
    check(mix.state().bgm.fading && mix.state().bgm.volume==30,"Pending music fade");
    tick(mix,4800); check(!mix.state().bgm.active,"Music fade endpoint");
    const auto a=mix.stats(); check(!a.errors && !a.missing && a.cache_hits && a.voice_frames && a.sound_frames && a.music_frames,"Incomplete audio routing");
    std::cout<<"48000 Hz: 4 independent voices, loose/packed/system routes, role gain, looping, SE/UI channel isolation and gain/stop ramps passed\n";
}
void live_volumes(const Path& root) {
    Settings s; s.sample_rate=48000;
    GameAudio mix(root,s),reference(root,s);
    for (auto* audio:{&mix,&reference}) {
        audio->set_character_slider("AA",5);
        send(*audio,music,{2,0,-10,0,0});
        send(*audio,voice,{0,0,-20},"AA020001");
        send(*audio,voice,{0,1},"ee020001");
        send(*audio,voice,{0,2,0,1},"aa081006");
        send(*audio,voice,{0,3,0,1},"01_ok01");
        send(*audio,sound,{0,7,10,1});
        check(audio->play_system_se(0).empty(),"Live UI SE could not start");
    }
    mix.set_volumes(40,30,50,60);
    auto state=mix.state();
    check(state.bgm.volume==30 && state.voices[0].volume==40 && state.voices[1].volume==50 &&
          state.voices[2].volume==70 && state.voices[3].volume==60 && state.sounds[7].volume==40,
          "Live settings lost script delta/role gain or category isolation");
    mix.set_volumes(0,0,0,0);
    state=mix.state();
    check(state.bgm.volume==1 && state.voices[2].volume==10 && state.sounds[7].volume==10,
          "Raw category0 bypassed source delta/role arithmetic");
    mix.set_master_volume(0);
    std::array<std::int16_t,8192> muted{},normal{};
    mix.pull(muted.data(),muted.size()/2); reference.pull(normal.data(),normal.size()/2);
    check(std::all_of(muted.begin(),muted.end(),[](auto sample){return sample==0;}),"Master0 did not mute every active bus");
    check(std::any_of(normal.begin(),normal.end(),[](auto sample){return sample!=0;}),"Real acceptance sources were silent");
    state=mix.state(); const auto expected=reference.state();
    check(state.bgm.source_frame==expected.bgm.source_frame,"Muted BGM stopped advancing");
    for (std::size_t i=0;i<state.voices.size();++i)
        check(state.voices[i].active && state.voices[i].source_frame==expected.voices[i].source_frame,"Mute restarted or stopped a voice");
    mix.set_volumes(80,80,80,80);
    mix.set_master_volume(100);
    mix.pull(muted.data(),muted.size()/2); reference.pull(normal.data(),normal.size()/2);
    check(muted==normal,"Restoring category settings changed playback position or script gain");
    std::cout<<"Live category settings: source raw0 arithmetic, master mute and sample-exact resume preserve deltas/roles\n";
}
void live_fades(const Path& root) {
    Settings s; s.sample_rate=48000;
    GameAudio mix(root,s),reference(root,s);
    for (auto* audio:{&mix,&reference}) send(*audio,music,{2,0,-20,4,0});
    tick(mix,2400); tick(reference,2400);
    const auto position=mix.state().bgm.source_frame;
    mix.set_volumes(40,80,80,80);
    check(mix.state().bgm.fading && mix.state().bgm.source_frame==position && mix.state().bgm.volume==3,
          "Live BGM setting reset the fade or source");
    tick(mix,7200); tick(reference,7200);
    check(mix.state().bgm.volume==10 && mix.state().bgm.fading,"Live fade did not retain its original elapsed time");
    mix.set_volumes(80,80,80,80);
    std::array<std::int16_t,8192> changed{},normal{};
    mix.pull(changed.data(),changed.size()/2); reference.pull(normal.data(),normal.size()/2);
    check(changed==normal,"Restored active fade differs from uninterrupted playback");
    for (auto* audio:{&mix,&reference}) send(*audio,music,{11,0,10,4,4});
    tick(mix,4800); tick(reference,4800);
    mix.set_volumes(0,80,80,80);
    mix.set_master_volume(0);
    mix.pull(changed.data(),changed.size()/2); reference.pull(normal.data(),normal.size()/2);
    check(std::all_of(changed.begin(),changed.end(),[](auto sample){return sample==0;}),"Outgoing crossfade BGM bypassed mute");
    mix.set_volumes(80,80,80,80);
    mix.set_master_volume(100);
    mix.pull(changed.data(),changed.size()/2); reference.pull(normal.data(),normal.size()/2);
    check(changed==normal && mix.state().bgm.fading,"Crossfade clocks or endpoints changed after muting");
    std::cout<<"Live fade settings: in-flight gain and simultaneous incoming/outgoing BGM keep their clock and sample position\n";
}
void runtime_settings(const Path& root) {
    Settings settings; settings.sample_rate=48000;
    std::array<std::int16_t,8192> actual{},expected{};
    GameAudio master(root,settings),unscaled(root,settings);
    for (auto* audio:{&master,&unscaled}) send(*audio,music,{2,0,-20,0,0});
    master.set_master_volume(25);
    master.pull(actual.data(),actual.size()/2); unscaled.pull(expected.data(),expected.size()/2);
    for (std::size_t i=0;i<actual.size();++i)
        check(std::abs(static_cast<int>(actual[i])-std::lround(expected[i]*0.25))<=1,"Master gain is not applied after mixing");
    master.set_master_volume(0);
    master.pull(actual.data(),actual.size()/2); unscaled.skip_frames(actual.size()/2);
    check(std::all_of(actual.begin(),actual.end(),[](auto sample){return sample==0;}),"Master0 did not silence active music");
    master.set_master_volume(100);
    master.pull(actual.data(),actual.size()/2); unscaled.pull(expected.data(),expected.size()/2);
    check(actual==expected,"Master gain restarted or changed script gain");

    GameAudio split(root,settings),script(root,settings),system(root,settings);
    for (auto* audio:{&split,&script}) send(*audio,sound,{0,0,0,1});
    for (auto* audio:{&split,&system}) check(audio->play_system_se(0).empty(),"System SE did not start");
    split.set_se_volumes(0,80);
    split.pull(actual.data(),actual.size()/2); system.pull(expected.data(),expected.size()/2); script.skip_frames(actual.size()/2);
    check(actual==expected,"Script SE slider changed system SE");
    split.set_se_volumes(80,0);
    split.pull(actual.data(),actual.size()/2); script.pull(expected.data(),expected.size()/2);
    check(actual==expected,"System SE slider changed script SE");

    GameAudio role(root,settings),unchanged(root,settings);
    for (auto* audio:{&role,&unchanged}) {
        audio->set_character_volume(0,0,true);
        send(*audio,next_fade_voice,{4}); send(*audio,voice,{0,0,-20,1},"AA020001");
        tick(*audio,4800);
    }
    role.set_character_volume(0,5,true);
    check(role.state().voices[0].volume==18 && role.state().voices[0].fading,"Live role slider lost fade progress or adjustment");
    role.set_character_volume(0,5,false);
    role.pull(actual.data(),actual.size()/2); unchanged.skip_frames(actual.size()/2);
    check(std::any_of(actual.begin(),actual.end(),[](auto sample){return sample!=0;}) && role.state().voices[0].active,
          "Role off changed an already playing voice instead of gating future plays");
    role.set_character_volume(0,0,true);
    role.pull(actual.data(),actual.size()/2); unchanged.pull(expected.data(),expected.size()/2);
    check(actual==expected,"Role switch changed fade or source position");
    role.set_character_volume(0,0,false);
    send(role,voice,{0,0,0,1},"AA020001");
    check(!role.state().voices[0].active && role.voice_duration_ms(0)>0,"Disabled role played a new voice or lost its full message duration");
    role.set_character_volume(0,0,true);
    send(role,voice,{0,0,0,1},"AA020001");
    check(role.state().voices[0].active,"Enabling role did not permit the next voice");
    GameAudio raw_off(root,settings);
    std::array<Word,32> off_flags{};
    const auto ee_group=raw_off.character_slider_index(4);
    off_flags[ee_group%32]|=Word{1}<<(ee_group/32);
    raw_off.set_voice_off(off_flags);
    send(raw_off,voice,{0,1,0,1},"ee020001");
    send(raw_off,voice,{0,0,0,1},"AA020001");
    check(!raw_off.state().voices[1].active && raw_off.state().voices[0].active,
          "Original interleaved voice_off words were treated as a conventional bitset");

    settings.record_events=true;
    GameAudio theme(root,settings);
    send(theme,voice,{0,0,0,1},"AA020001");
    theme.set_system_voice_selection(1);
    check(theme.play_button_voice(0).empty(),"Theme button voice could not decode/play");
    const auto log=theme.stats().log.back();
    check(log.action=="system_voice.play" && log.clips.at(0).name=="aa180001" && theme.state().voices[0].active,
          "Theme did not select buttonVoice1 or replaced main speech");
    send(theme,voice,{-1,0});
    theme.set_se_volumes(80,0);
    theme.pull(actual.data(),actual.size()/2);
    check(std::any_of(actual.begin(),actual.end(),[](auto sample){return sample!=0;}),"Button voice incorrectly used the system-SE category");
    theme.set_volumes(80,80,0,80);
    theme.pull(actual.data(),actual.size()/2);
    check(std::all_of(actual.begin(),actual.end(),[](auto sample){return sample==0;}),"Button voice bypassed VOICE volume");
    theme.set_volumes(80,80,80,80);
    theme.pull(actual.data(),actual.size()/2);
    check(std::any_of(actual.begin(),actual.end(),[](auto sample){return sample!=0;}),"Muted button voice did not resume");
    check(theme.play_button_voice(0).empty() && theme.stats().log.back().action=="system_voice.duplicate","Same currently playing button voice was restarted");
    std::cout<<"Runtime settings: master postmix, independent script/system SE, live role gain/future-play gates and actual theme button voice passed\n";
}
void config_feedback(const Path& root) {
    Settings settings; settings.record_events=true;
    GameAudio mix(root,settings);
    const auto played=[&](const char* action,const char* name){
        const auto log=mix.stats().log.back();
        check(log.action==action && log.clips.size()==1 && log.clips.front().name==name,
              std::string("Unexpected configuration cue: ")+log.action+" for "+name);
    };
    check(mix.play_button_voice(50).empty(),"Could not preview the selected character");
    played("system_voice.play","ff180053");
    mix.set_system_voice_selection(6);
    check(mix.play_button_voice(1).empty(),"Could not play a themed tab");
    played("system_voice.play","ff180002");
    check(mix.play_system_se(11).empty(),"Could not resolve a themed system sound");
    played("system_se.play","ff180001");
    check(mix.play_config_feedback(3,0,100).empty(),"Could not preview the low voice position");
    played("system_voice.play","ff180018");
    check(mix.play_config_feedback(3,100,100).empty(),"Could not preview the high voice position");
    played("system_voice.play","ff180109");
    check(mix.play_config_feedback(0,0,4).empty(),"Could not preview the text slider");
    played("system_voice.play","ff180035");
    check(mix.play_config_feedback(7,4,4).empty(),"Could not preview the auto slider");
    played("system_voice.play","ff180039");
    check(mix.play_config_feedback(6,100,100).empty(),"Could not preview the window slider");
    played("system_voice.play","ff180052");
    check(mix.play_config_feedback(9,50,100).empty(),"Could not preview the configured SE table");
    played("config_se.play","06_page");
    check(mix.play_button_voice(51).empty(),"Could not play the old theme's farewell");
    played("system_voice.play","ff180054");
    mix.set_system_voice_selection(0);
    check(mix.play_button_voice(1).empty(),"Could not resolve the disabled theme table");
    played("system_voice.play","00_null");
    std::cout<<"Config slider endpoints, theme system sounds and previous-theme farewell passed\n";
}
void source_switches(const Path& root) {
    Settings settings; settings.sample_rate=48000; settings.default_music_fade_out=0;
    RuntimeParameters p;
    for (const auto id:{NNNPARAM_MUSICVOLUME,NNNPARAM_SOUNDVOLUME,NNNPARAM_SCRIPTSEVOLUME,NNNPARAM_VOICEVOLUME,NNNPARAM_SOUNDVOICEVOLUME}) p.set(id,80);
    p.set(NNNPARAM_TOTALVOLUME,100); p.set(NNNPARAM_SCRIPTSESWITCH,1);
    GameAudio mix(root,settings),reference(root,settings);
    mix.apply_runtime_parameters(p);
    for (auto* audio:{&mix,&reference}) send(*audio,music,{2,0,-20,0,0});
    tick(mix,2400); tick(reference,2400);
    const auto position=mix.state().bgm.source_frame;
    p.set(NNNPARAM_MUSICVOLUME,0); mix.apply_runtime_parameters(p);
    check(mix.state().bgm.active && mix.state().bgm.source_frame==position,"Music volume0 restarted/stopped the source");
    std::array<std::int16_t,8192> actual{},expected{};
    mix.pull(actual.data(),actual.size()/2); reference.skip_frames(actual.size()/2);
    check(mix.state().bgm.volume==1 && std::any_of(actual.begin(),actual.end(),[](auto x){return x!=0;}),
          "Runtime MUSIC explicit delta did not retain source minimum1 at raw volume0");
    p.set(NNNPARAM_MUSICVOLUME,80); mix.apply_runtime_parameters(p);
    mix.pull(actual.data(),actual.size()/2); reference.pull(expected.data(),expected.size()/2);
    check(actual==expected,"Runtime music volume restore changed script delta or source clock");
    p.set(NNNPARAM_MUSICSWITCH,0); mix.apply_runtime_parameters(p);
    check(!mix.state().bgm.active,"MusicSwitch0 did not stop BGM");
    p.set(NNNPARAM_MUSICSWITCH,1); mix.apply_runtime_parameters(p);
    check(mix.state().bgm.active && mix.state().bgm.source_frame==0 && mix.state().bgm.volume==80,
          "MusicSwitch1 did not replay last table/loop with source defaults");
    GameAudio restarted(root,settings); send(restarted,music,{2,0});
    mix.pull(actual.data(),actual.size()/2); restarted.pull(expected.data(),expected.size()/2);
    check(actual==expected,"Music switch replay PCM differs from a fresh source-default play");
    const auto resumed=mix.state().bgm.source_frame;
    p.set(NNNPARAM_MESSAGESPEED,3); mix.apply_runtime_parameters(p);
    check(mix.state().bgm.source_frame==resumed,"Unrelated runtime update restarted BGM");
    p.set(NNNPARAM_TOTALSWITCH,0); mix.apply_runtime_parameters(p);
    check(!mix.state().bgm.active,"TotalSwitch0 did not stop BGM");
    p.set(NNNPARAM_TOTALSWITCH,1); mix.apply_runtime_parameters(p);
    check(mix.state().bgm.active && mix.state().bgm.source_frame==0,"TotalSwitch1 did not replay BGM");
    send(mix,voice,{0,0,0,1},"AA020001");
    p.set(NNNPARAM_VOICEVOLUME,0); mix.apply_runtime_parameters(p);
    send(mix,voice,{0,0,20,1},"AA020001");
    check(mix.state().voices[0].volume==20,"Canonical raw VOICE0 discarded positive script delta");
    send(mix,volume_voice,{0,-101});
    check(mix.state().voices[0].volume==1,"Canonical raw VOICE0 discarded source -101 override");
    send(mix,sound,{0,7,0,1}); tick(mix,512);
    const auto speech=mix.state().voices[0].source_frame,se=mix.state().sounds[7].source_frame;
    p.set(NNNPARAM_VOICESWITCH,0); p.set(NNNPARAM_SCRIPTSESWITCH,0); mix.apply_runtime_parameters(p);
    check(mix.state().voices[0].active && mix.state().voices[0].source_frame==speech && mix.state().sounds[7].active,
          "Voice/SE switch stopped a current source");
    send(mix,sound,{0,7,0,1});
    check(mix.state().sounds[7].source_frame==se,"Disabled ScriptSE started/replaced a source");
    send(mix,voice,{0,0,0,1},"AA020001");
    check(!mix.state().voices[0].active && mix.voice_duration_ms(0)>0,"Disabled voice gate did not preserve source load/duration semantics");
    std::cout<<"Native runtime switches: music/total stop+default replay; raw0 follows source arithmetic and keeps clock; voice/SE gate future playback\n";
}
}
int main(int argc,char** argv) {
    try {
        if (argc!=2 && !(argc==3 && std::string(argv[2])=="--config"))
            throw std::invalid_argument("nnn-audio-check DEADENDAEGIS_ROOT [--config]");
        const Path root=argv[1];
        config_feedback(root);
        if (argc==3) return 0;
        channels(root);
        live_volumes(root);
        live_fades(root);
        runtime_settings(root);
        source_switches(root);
        playlist(root,2,{"mgw_bgm01_1","mgw_bgm01_2"},1);
        playlist(root,11,{"mgw_bgm10_a1","mgw_bgm10_a2","mgw_bgm10_a3"},1);
        playlist(root,19,{"mgw_bgm17"},0);
        playlist(root,34,{"short"},0); // Direct WGQ; this entry has no MML.
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

