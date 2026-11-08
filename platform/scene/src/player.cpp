#include "emberfall/game_session.hpp"
#include "audio_export.hpp"
#include "pc_save.hpp"
#include <png.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
using namespace emberfall;
namespace fs=std::filesystem;

unsigned integer(const std::string& s) {
    unsigned v=0;
    const auto r=std::from_chars(s.data(),s.data()+s.size(),v);
    if (r.ec!=std::errc() || r.ptr!=s.data()+s.size()) throw std::invalid_argument("Invalid integer: "+s);
    return v;
}
std::string hex(const std::string& s) {
    static constexpr char digits[]="0123456789abcdef";
    std::string out;
    for (char c:s) { const auto b=static_cast<unsigned char>(c); out+=digits[b>>4U]; out+=digits[b&15U]; }
    return out;
}
std::string quote(const std::string& s) {
    std::ostringstream out; out<<'"';
    for (char c:s) {
        const auto b=static_cast<unsigned char>(c);
        if (c=='"' || c=='\\') out<<'\\'<<c;
        else if (b<32U || b>=127U) out<<"\\u00"<<std::hex<<std::setfill('0')<<std::setw(2)<<static_cast<unsigned>(b)<<std::dec;
        else out<<c;
    }
    out<<'"'; return out.str();
}
void capture(scene::GameSession& game,const fs::path& path) {
    const auto& screen=game.surface();
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    png_image png{}; png.version=PNG_IMAGE_VERSION; png.width=screen.width; png.height=screen.height; png.format=PNG_FORMAT_RGB;
    if (!png_image_write_to_file(&png,path.string().c_str(),0,screen.rgb.data(),0,nullptr)) {
        const std::string error=png.message; png_image_free(&png); throw std::runtime_error(error);
    }
    png_image_free(&png);
    auto metadata=path; metadata.replace_extension(".json");
    const auto& f=game.state(); const auto& d=f.dialogue; const auto stats=game.stats();
    std::ofstream log(metadata);
    log<<"{\n  \"script\": "<<quote(f.script)<<", \"word\": "<<f.word<<", \"width\": "<<screen.width<<", \"height\": "<<screen.height
       <<", \"line\": "<<d.sequence<<", \"message_id\": "<<d.message_id<<", \"text_index\": "<<d.table_index
       <<",\n  \"speaker_hex\": "<<quote(hex(d.speaker))<<", \"raw_hex\": "<<quote(hex(d.raw_bytes))
       <<", \"display_hex\": "<<quote(hex(d.display_bytes))<<",\n  \"draws\": "<<stats.draws<<", \"overlaps\": "<<stats.overlaps
       <<", \"audio_events\": "<<stats.audio_events<<", \"transfers\": "<<stats.file_transfers<<",\n  \"layers\": [";
    bool first=true;
    for (std::size_t i=0;i<game.layers().size();++i) {
        const auto& l=game.layers()[i]; if (!l.enabled) continue;
        if (!first) log<<','; first=false;
        log<<"\n    {\"slot\": "<<i<<", \"effect\": "<<l.effect<<", \"tick\": "<<l.count
           <<", \"from\": ["<<l.from.x<<','<<l.from.y<<','<<l.from.width<<','<<l.from.height<<']';
        if (l.texture) {
            const auto& t=*l.texture;
            log<<", \"asset\": "<<quote(t.name)<<", \"source\": "<<quote(t.source)<<", \"canvas\": ["<<t.canvas_width<<','<<t.canvas_height
               <<"], \"content\": ["<<t.content.x<<','<<t.content.y<<','<<t.content.width<<','<<t.content.height<<']';
        }
        log<<'}';
    }
    log<<"\n  ]\n}\n";
    if (!log) throw std::runtime_error("Could not write frame state");
    std::cout<<"Captured "<<path.string()<<" at "<<f.script<<" word "<<f.word<<" line "<<d.sequence<<'\n';
}

void import_pc(scene::GameSession& game,const fs::path& path) {
    const auto length=fs::file_size(path);
    if (length==0 || length>scene::pc::max_save_bytes) throw std::runtime_error("Invalid PC save file size");
    std::vector<nnn::Byte> bytes(static_cast<std::size_t>(length));
    std::ifstream input(path,std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Could not read PC save file");
    game.restore_pc_save(bytes);
    std::cout<<"Imported "<<path.string()<<" at "<<game.state().script<<" word "<<game.state().word<<'\n';
}

void export_pc(scene::GameSession& game,const fs::path& path) {
    if (fs::exists(path)) throw std::runtime_error("PC save destination already exists");
    const auto bytes=game.export_pc_save();
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    std::ofstream output(path,std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("Could not write PC save file");
    std::cout<<"Exported "<<path.string()<<" at "<<game.state().script<<" word "<<game.state().word<<'\n';
}

struct Options {
    fs::path root,pc_save;
    scene::SessionOptions session;
    scene::AudioOutputOptions audio;
    unsigned limit=60,choice=1;
    std::vector<unsigned> places;
    bool list=false, verify=false, click_draw=false;
    std::map<unsigned,fs::path> captures,pc_exports;
};
Options arguments(int argc,char** argv) {
    if (argc<2) throw std::invalid_argument("nnn-scene-player GAME [--entry FILE] [--pc-save FILE] [--export-pc LINE FILE] [--limit N] [--choice N] [--places N,N,...] [--capture LINE PNG] [--dialogue-ms N] [--wait-voice] [--wav FILE --wav-start-ms N --wav-ms N] [--audio-stats JSON] [--sample-rate HZ] [--verify]");
    Options o; o.root=argv[1];
    for (int i=2;i<argc;++i) {
        const std::string key=argv[i];
        auto value=[&]() -> std::string {
            if (++i>=argc) throw std::invalid_argument("Missing value for "+key);
            return argv[i];
        };
        if (key=="--entry") { o.session.entry=value();o.session.standalone_script=true; }
        else if (key=="--pc-save") o.pc_save=value();
        else if (key=="--limit") o.limit=integer(value());
        else if (key=="--choice") o.choice=integer(value());
        else if (key=="--places") {
            std::istringstream input(value());
            for (std::string item;std::getline(input,item,',');) {
                const auto place=integer(item);
                if (!place || place>64 || o.places.size()>=4096) throw std::invalid_argument("Invalid place sequence");
                o.places.push_back(place);
            }
            if (o.places.empty()) throw std::invalid_argument("Missing place sequence");
        }
        else if (key=="--dialogue-ms") o.audio.dialogue_ms=integer(value());
        else if (key=="--wait-voice") o.audio.wait_voice=true;
        else if (key=="--sample-rate") o.session.audio.sample_rate=integer(value());
        else if (key=="--wav") o.audio.wav=value();
        else if (key=="--wav-start-ms") o.audio.start_ms=integer(value());
        else if (key=="--wav-ms") o.audio.duration_ms=integer(value());
        else if (key=="--audio-stats") { o.audio.report=value(); o.session.audio.record_events=true; }
        else if (key=="--cache-mib") o.session.cache_bytes=static_cast<std::size_t>(integer(value()))*1024U*1024U;
        else if (key=="--capture" || key=="--export-pc") {
            const auto line=integer(value()); const auto target=value();
            if (!line) throw std::invalid_argument("Dialogue sequence must be positive");
            (key=="--capture" ? o.captures : o.pc_exports).emplace(line,target);
        } else if (key=="--list") o.list=true;
        else if (key=="--verify") o.verify=true;
        else if (key=="--click-draw") o.click_draw=true;
        else throw std::invalid_argument("Unknown option: "+key);
    }
    if (!o.captures.empty()) o.limit=std::max(o.limit,o.captures.rbegin()->first);
    if (!o.pc_exports.empty()) o.limit=std::max(o.limit,o.pc_exports.rbegin()->first);
    if (!o.choice || !o.limit) throw std::invalid_argument("Limit and choice must be positive");
    return o;
}

int play(const Options& o) {
    using scene::Wait;
    scene::GameSession game(o.root,o.session);
    if (!o.pc_save.empty()) import_pc(game,o.pc_save);
    scene::AudioExport output(o.audio,game.audio().sample_rate(),game.audio().stats().events);
    std::vector<scene::ChoiceTrace> choices;
    auto step=[&](unsigned elapsed,const scene::InputState& input=scene::InputState{}) -> const scene::FrameState& {
        output.advance(game.audio(),elapsed); return game.step_frame(elapsed,input);
    };
    scene::InputState in; in.mouse_x=150; in.mouse_y=75;
    unsigned dt=0,captured=0,exported=0,checked_fades=0;
    std::size_t next_place=0;
    bool complete=false;
    std::uint64_t read_audio=0;
    for (unsigned calls=0;calls<500000U;++calls) {
        const auto& f=step(dt,in);
        auto events=game.take_audio_events(); read_audio+=events.size();
        in.advance=false; in.choice=0; dt=game.frame_ms();
        if (f.wait==Wait::Error) throw std::runtime_error(f.script+" at "+std::to_string(f.word)+": "+f.error);
        if (f.wait==Wait::Ended) { complete=true; break; }
        if (f.wait==Wait::Dialogue) {
            const auto n=static_cast<unsigned>(f.dialogue.sequence);
            if (o.list) {
                std::cout<<"line "<<n<<" "<<f.script<<" @"<<f.word;
                for (const auto& layer:game.layers()) if (layer.enabled && layer.texture && layer.effect!=66)
                    std::cout<<" "<<layer.texture->name<<"/"<<layer.effect;
                for (std::size_t i=0;i<game.layers().size();++i) if (game.layers()[i].enabled)
                    std::cout<<" slot="<<i<<":"<<game.layers()[i].effect;
                std::cout<<'\n';
            }
            const auto dest=o.captures.find(n);
            if (dest!=o.captures.end()) { capture(game,dest->second); ++captured; }
            const auto save=o.pc_exports.find(n);
            if (save!=o.pc_exports.end()) { export_pc(game,save->second); ++exported; }
            if (o.verify) {
                const auto pc=f.word; const auto text=f.dialogue.raw_bytes;
                const auto& held=step(0);
                if (held.wait!=Wait::Dialogue || held.word!=pc || held.dialogue.sequence!=n || held.dialogue.raw_bytes!=text ||
                    !game.take_audio_events().empty()) throw std::runtime_error("Dialogue did not remain pending");
            }
            if (o.audio.dialogue_ms) dt=o.audio.dialogue_ms;
            if (o.audio.wait_voice) {
                // Advancing is a reader action. Let a spoken line finish while the VM is blocked.
                unsigned waited=dt;
                output.advance(game.audio(),dt); dt=0;
                for (;;) {
                    const auto tracks=game.audio().state().voices;
                    if (std::none_of(tracks.begin(),tracks.begin()+2,[](const auto& t){return t.active && !t.looping;})) break;
                    output.advance(game.audio(),10); waited+=10;
                }
                if (game.step_frame(waited).wait!=Wait::Dialogue) throw std::runtime_error("Dialogue advanced while reading");
            }
            if (n>=o.limit) { output.advance(game.audio(),dt); complete=true; break; }
            in.advance=true;
        } else if (f.wait==Wait::Choice) {
            if (!f.selection || f.selection->options.empty()) throw std::runtime_error("Empty selection");
            const auto count=static_cast<unsigned>(f.selection->options.size());
            if (o.verify && step(0).wait!=Wait::Choice) throw std::runtime_error("Choice advanced without input");
            in.choice=std::min(count,o.choice);
            choices.push_back({f.script,f.word,in.choice,count});
        } else if (f.wait==Wait::Place) {
            const auto places=game.places();
            const auto requested=next_place<o.places.size() ? o.places[next_place] : 0;
            if (!requested || requested>places.size() || places[requested-1]!=2) {
                std::cout<<"SELECTPLACE waiting at "<<f.script<<" word "<<f.word<<" available";
                for (std::size_t i=0;i<places.size();++i) if (places[i]==2) std::cout<<' '<<i+1;
                if (requested) std::cout<<" (requested "<<requested<<" is unavailable)";
                std::cout<<'\n';
                complete=true;break;
            }
            ++next_place;in.choice=requested;
            choices.push_back({f.script,f.word,in.choice,static_cast<unsigned>(places.size())});
        } else if (f.wait==Wait::Draw || f.wait==Wait::Overlap) {
            const unsigned frames=f.remaining_frames;
            if (o.click_draw && f.wait==Wait::Draw) { in.advance=true; continue; }
            // Check a genuine changing transition against both independently retained endpoints.
            if (o.verify && !checked_fades && f.wait==Wait::Overlap && game.overlap_type()==0 && frames>=2U && frames<1000U) {
                const auto from=game.surface();
                const auto middle=frames/2U;
                const auto period=game.frame_ms();
                if (step(middle*period).wait!=Wait::Overlap) throw std::runtime_error("Early overlap completion");
                const auto half=game.surface();
                if (step((frames-middle)*period).wait!=Wait::Running) throw std::runtime_error("Overlap did not complete");
                const auto& to=game.surface();
                if (from.rgb!=to.rgb) {
                    const auto a=middle*256U/frames;
                    for (std::size_t i=0;i<to.rgb.size();++i)
                        if (half.rgb[i]!=(from.rgb[i]*(256U-a)+to.rgb[i]*a+128U)/256U) throw std::runtime_error("Crossfade pixel mismatch");
                    ++checked_fades;
                }
                dt=0;
            } else dt=static_cast<unsigned>(std::min<std::uint64_t>(std::uint64_t{frames}*game.frame_ms(),UINT32_MAX));
        }
    }
    auto s=game.stats();
    if (!complete) throw std::runtime_error("Frame limit reached before the requested dialogue or script end");
    if (read_audio!=s.audio_events || captured!=o.captures.size() || exported!=o.pc_exports.size())
        throw std::runtime_error("Incomplete session capture, PC export or audio delivery");
    output.finish(game,o.session.entry,choices);
    std::cout<<"Session lines="<<game.state().dialogue.sequence<<" draws="<<s.draws<<" overlaps="<<s.overlaps<<" choices="<<s.choices
             <<" transfers="<<s.file_transfers<<" audio="<<s.audio_events<<" decodes="<<s.decodes<<" hits="<<s.cache_hits
             <<" evictions="<<s.evictions<<" cached_bytes="<<s.cached_bytes<<" packs="<<s.packed_reads<<" loose="<<s.loose_reads
             <<" changing_crossfade_verified="<<checked_fades<<" warnings="<<game.warnings().size()<<'\n';
    return 0;
}
} // namespace
int main(int argc,char** argv) {
    try { return play(arguments(argc,argv)); }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
