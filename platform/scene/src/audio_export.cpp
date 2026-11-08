#include "audio_export.hpp"
#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace emberfall::scene {
namespace {
void ensure_parent(const std::filesystem::path& f) {
    if (f.has_parent_path()) std::filesystem::create_directories(f.parent_path());
}
std::string json(std::string_view s) {
    std::ostringstream out; out<<'"';
    for (unsigned char c:s) {
        if (c=='"' || c=='\\') out<<'\\'<<c;
        else if (c<32) out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<int>(c)<<std::dec;
        else out<<c;
    }
    out<<'"'; return out.str();
}
void field(std::ostream& stream,std::uint32_t value,unsigned bytes) {
    for (unsigned i=0;i<bytes;++i) stream.put(static_cast<char>((value>>(8*i))&255));
}
void wave_header(std::ostream& stream,unsigned rate,std::uint32_t frames) {
    stream.write("RIFF",4); field(stream,36+frames*4,4); stream.write("WAVEfmt ",8); field(stream,16,4);
    field(stream,1,2); field(stream,2,2); field(stream,rate,4); field(stream,rate*4,4);
    field(stream,4,2); field(stream,16,2); stream.write("data",4); field(stream,frames*4,4);
}
}
AudioExport::AudioExport(const AudioOutputOptions& options,unsigned rate,std::size_t restored_events)
:options_(options),rate_(rate),restored_events_(restored_events),begin_(std::uint64_t(rate)*options.start_ms/1000),
 end_(std::uint64_t(rate)*(std::uint64_t(options.start_ms)+options.duration_ms)/1000) {
    if (!options.wav.empty()) {
        if (end_<=begin_ || end_-begin_>(UINT32_MAX-36)/4) throw std::invalid_argument("WAV window exceeds RIFF size or is empty");
        ensure_parent(options.wav); wave_.exceptions(std::ios::badbit|std::ios::failbit);
        wave_.open(options.wav,std::ios::binary); wave_header(wave_,rate_,0);
    }
}
void AudioExport::advance(nnn::audio::GameAudio& audio,unsigned ms) {
    auto units=std::uint64_t(ms)*rate_+remainder_;
    auto frames=units/1000; remainder_=static_cast<unsigned>(units%1000);
    std::array<std::int16_t,4096> pcm;
    std::array<char,8192> bytes;
    while (frames) {
        const auto n=std::min<std::uint64_t>(frames,pcm.size()/2);
        audio.pull(pcm.data(),static_cast<std::size_t>(n));
        const auto first=std::max(elapsed_,begin_),last=std::min(elapsed_+n,end_);
        if (wave_.is_open() && first<last) {
            std::size_t pos=0;
            for (auto i=first-elapsed_;i<last-elapsed_;++i) {
                nonzero_+=(pcm[i*2]!=0 || pcm[i*2+1]!=0);
                for (unsigned k=0;k<2;++k) {
                    auto v=static_cast<std::uint16_t>(pcm[i*2+k]);
                    bytes[pos++]=static_cast<char>(v&255); bytes[pos++]=static_cast<char>(v>>8);
                }
            }
            wave_.write(bytes.data(),static_cast<std::streamsize>(pos)); written_+=last-first;
        }
        elapsed_+=n; frames-=n;
    }
}
void AudioExport::finish(const GameSession& game,const std::string& entry,const std::vector<ChoiceTrace>& choices) {
    if (wave_.is_open()) {
        wave_.seekp(0); wave_header(wave_,rate_,static_cast<std::uint32_t>(written_)); wave_.close();
        if (written_!=end_-begin_) throw std::runtime_error("Requested WAV window extends beyond this session");
    }
    const auto a=game.audio().stats();
    if (a.output_frames!=elapsed_) throw std::runtime_error("Mixer and scene clocks diverged");
    if (!options_.report.empty()) {
        ensure_parent(options_.report); std::ofstream s; s.exceptions(std::ios::failbit|std::ios::badbit);
        s.open(options_.report); const auto f=game.state(); const auto g=game.stats();
        s<<"{\n  \"entry\": "<<json(entry)<<", \"last_script\": "<<json(f.script)<<", \"lines\": "<<f.dialogue.sequence
         <<", \"draws\": "<<g.draws<<", \"overlaps\": "<<g.overlaps<<", \"transfers\": "<<g.file_transfers<<", \"choices\": [";
        for (std::size_t i=0;i<choices.size();++i) {
            const auto& c=choices[i]; if (i) s<<',';
            s<<"{\"script\":"<<json(c.script)<<",\"word\":"<<c.word<<",\"selected\":"<<c.selected<<",\"count\":"<<c.count<<"}";
        }
        s<<"],\n  \"sample_rate\": "<<rate_<<", \"channels\": 2, \"dialogue_ms\": "<<options_.dialogue_ms
         <<", \"wait_voice\": "<<(options_.wait_voice?"true":"false")<<", \"events\": "<<a.events<<", \"restored_events\": "<<restored_events_
         <<", \"plays\": "<<a.plays<<", \"controls\": "<<a.controls
         <<", \"decoded\": "<<a.decoded<<", \"cache_hits\": "<<a.cache_hits<<", \"missing\": "<<a.missing<<", \"errors\": "<<a.errors
         <<",\n  \"output_frames\": "<<elapsed_<<", \"nonzero_frames\": "<<a.nonzero_frames<<", \"clipped_frames\": "<<a.clipped_frames
         <<", \"peak\": "<<a.peak<<", \"music_frames\": "<<a.music_frames<<", \"voice_frames\": "<<a.voice_frames
         <<", \"se_frames\": "<<a.sound_frames<<", \"overlapping_bus_frames\": "<<a.overlapping_frames
         <<", \"music_segment_changes\": "<<a.music_segment_changes<<", \"music_loops\": "<<a.music_loops
         <<",\n  \"wav\": "<<json(options_.wav.filename().string())<<", \"wav_first_frame\": "<<begin_
         <<", \"wav_frames\": "<<written_<<", \"wav_nonzero_frames\": "<<nonzero_<<",\n  \"trace\": [";
        for (std::size_t i=0;i<a.log.size();++i) {
            const auto& e=a.log[i]; if (i) s<<',';
            s<<"\n    {\"frame\":"<<e.at_output_frame<<",\"script\":"<<json(e.script)<<",\"word\":"<<e.word
             <<",\"opcode\":\"0x"<<std::hex<<e.opcode<<std::dec<<"\",\"action\":"<<json(e.action)<<",\"ch\":"<<e.channel<<",\"args\":[";
            for (std::size_t j=0;j<e.parameters.size();++j) { if(j) s<<','; s<<e.parameters[j]; }
            s<<"],\"clips\":[";
            for (std::size_t j=0;j<e.clips.size();++j) {
                const auto& c=e.clips[j]; if(j) s<<',';
                s<<"{\"name\":"<<json(c.name)<<",\"source\":"<<json(c.source)<<",\"hz\":"<<c.rate
                 <<",\"channels\":"<<c.channels<<",\"frames\":"<<c.frames<<'}';
            }
            s<<"],\"error\":"<<json(e.error)<<'}';
        }
        s<<"\n  ]\n}\n"; s.close();
    }
    if (a.missing || a.errors || a.events!=game.stats().audio_events+restored_events_ || a.events!=a.plays+a.controls)
        throw std::runtime_error("Incomplete audio delivery: "+std::to_string(a.missing)+" missing, "+std::to_string(a.errors)+" errors");
}
} // namespace emberfall::scene

