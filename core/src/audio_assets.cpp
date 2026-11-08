#include "audio_assets.hpp"
#include "nnn/game_config.hpp"

#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb/stb_vorbis.c"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace nnn::audio {
namespace fs=std::filesystem;
using resource::fold_cp932;
namespace {
using Bytes=std::vector<std::uint8_t>;
constexpr std::size_t max_container=64U*1024U*1024U, max_pcm=128U*1024U*1024U;

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]}<<8U) | (std::uint32_t{p[2]}<<16U) | (std::uint32_t{p[3]}<<24U);
}
std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(std::uint16_t{p[0]} | (std::uint16_t{p[1]}<<8U));
}
bool tag(const std::uint8_t* p,const char* s) { return std::memcmp(p,s,4)==0; }

Bytes read_bytes(const fs::path& file,std::uint64_t begin,std::uint64_t end,std::size_t maximum) {
    if (end<begin || end-begin>maximum || end>fs::file_size(file)) throw AudioError("Invalid file interval: "+file.u8string());
    std::ifstream in(file,std::ios::binary);
    if (!in || !in.seekg(static_cast<std::streamoff>(begin))) throw AudioError("Cannot seek "+file.u8string());
    Bytes result(static_cast<std::size_t>(end-begin));
    if (!result.empty() && !in.read(reinterpret_cast<char*>(result.data()),static_cast<std::streamsize>(result.size())))
        throw AudioError("Short read: "+file.u8string());
    return result;
}
std::string text_file(const fs::path& file) {
    auto data=read_bytes(file,0,fs::file_size(file),2U*1024U*1024U);
    return std::string(data.begin(),data.end());
}
std::vector<std::string> names(std::string text) {
    std::vector<std::string> out;
    if (text.compare(0,3,"\xef\xbb\xbf")==0) text.erase(0,3);
    std::size_t at=0;
    while (at<text.size()) {
        const auto end=text.find_first_of("\r\n",at);
        auto row=text.substr(at,end==std::string::npos ? end : end-at);
        const auto comma=row.find(',');
        row.resize(comma==std::string::npos ? row.size() : comma);
        const auto start=row.find_first_not_of(" \t");
        if (start!=std::string::npos) out.push_back(fold_cp932(row.substr(start,row.find_last_not_of(" \t")-start+1)));
        else if (comma!=std::string::npos) out.emplace_back(); // Preserve an explicitly empty numbered slot.
        if (end==std::string::npos) break;
        at=text.find_first_not_of("\r\n",end);
        if (at==std::string::npos) break;
    }
    return out;
}
std::string item(const std::vector<std::string>& table,int id,const char* kind) {
    if (id<0 || static_cast<std::size_t>(id)>=table.size() || table[static_cast<std::size_t>(id)].empty())
        throw AudioError(std::string(kind)+" ID unavailable: "+std::to_string(id),true);
    return table[static_cast<std::size_t>(id)];
}
struct Sequence {std::vector<std::string> names; std::optional<std::size_t> loop;};
class Mml {
    const std::string text_;
    std::size_t at_=0;
    static bool separator(char c) { return c==' ' || c=='\t' || c=='\n' || c=='\r' || c==','; }
    void spaces() { while (at_<text_.size() && separator(text_[at_])) ++at_; }
    void append(Sequence& dest,const Sequence& src,unsigned repeats) {
        if (dest.loop) return; // Everything after an infinite group is unreachable.
        if (src.names.empty() || repeats>4096U || src.names.size()*std::size_t{repeats}>4096U-dest.names.size())
            throw AudioError("Invalid or oversized MML group");
        if (src.loop) dest.loop=dest.names.size()+*src.loop;
        for (unsigned i=0;i<repeats;++i) dest.names.insert(dest.names.end(),src.names.begin(),src.names.end());
    }
    Sequence group(unsigned depth) {
        if (depth>16) throw AudioError("MML nesting too deep");
        Sequence result;
        for (;;) {
            spaces();
            if (at_==text_.size()) { if (depth) throw AudioError("Unclosed MML group"); return result; }
            const char c=text_[at_];
            if (c==']') { if (!depth) throw AudioError("Unexpected MML ]"); ++at_; return result; }
            if (c=='[') {
                ++at_; spaces(); unsigned count=0;
                const auto first=at_;
                while (at_<text_.size() && text_[at_]>='0' && text_[at_]<='9') ++at_;
                if (first!=at_ && std::from_chars(text_.data()+first,text_.data()+at_,count).ec!=std::errc())
                    throw AudioError("MML repeat count overflow");
                spaces();
                if (at_<text_.size() && text_[at_]==':') ++at_;
                auto nested=group(depth+1);
                const auto start=result.names.size();
                append(result,nested,count && !nested.loop ? count : 1);
                if (!count && !result.loop) result.loop=start;
            } else {
                const auto first=at_;
                while (at_<text_.size() && !separator(text_[at_]) && text_[at_]!='[' && text_[at_]!=']') {
                    const bool pair=resource::cp932_lead(static_cast<unsigned char>(text_[at_])) && at_+1<text_.size() &&
                        resource::cp932_trail(static_cast<unsigned char>(text_[at_+1]));
                    at_+=pair ? 2U : 1U;
                }
                if (at_-first>255 || result.names.size()>=4096) throw AudioError("MML segment limit exceeded");
                auto key=fold_cp932(text_.substr(first,at_-first));
                if (!result.loop) result.names.push_back(std::move(key));
            }
        }
    }
public:
    explicit Mml(std::string text):text_(std::move(text)) {}
    Sequence parse() { auto p=group(0); if(p.names.empty()) throw AudioError("Empty MML score"); return p; }
};

std::size_t wrapper_offset(const Bytes& b,bool music_file) {
    if (b.size()<68) throw AudioError("Truncated audio wrapper");
    const std::string header(b.begin(),b.begin()+64);
    std::size_t at=64;
    if (music_file) {
        if (header.find("PACKTYPE=6")==std::string::npos || header.find("OGG")==std::string::npos)
            throw AudioError("Invalid WGQ PACKTYPE=6 header");
    } else {
        if (b[57]!='0' && b[57]!='2') throw AudioError("Unsupported VAW PACKTYPE="+std::string(1,static_cast<char>(b[57])));
        if (b.size()<108 || !tag(b.data()+64,"RIFF") ||
            !tag(b.data()+72,"WAVE") || !tag(b.data()+76,"fmt ")) throw AudioError("Invalid VAW RIFF/WAVE header");
        const auto fmt=le32(b.data()+80);
        if (fmt<16 || fmt>b.size()-92U || !tag(b.data()+84U+fmt,"data")) throw AudioError("Invalid VAW fmt/data boundary");
        at=92U+fmt;
    }
    return at;
}

std::uint64_t check_pages(const std::uint8_t* data,std::size_t size) {
    static const auto crc_table=[] {
        std::array<std::uint32_t,256> t{};
        for (std::uint32_t i=0;i<256;++i) {
            auto c=i<<24U;
            for (unsigned bit=0;bit<8;++bit) c=(c<<1U)^((c&0x80000000U) ? 0x04c11db7U : 0U);
            t[i]=c;
        }
        return t;
    }();
    std::size_t at=0; std::uint32_t serial=0,sequence=0; bool end=false;
    std::uint64_t samples=0;
    while (at<size) {
        const auto* p=data+at;
        if (end || size-at<27U || !tag(p,"OggS") || p[4]!=0) throw AudioError("Invalid Ogg page header");
        const auto flags=p[5]; const auto segments=p[26];
        if (!at) { if (!(flags&2U)) throw AudioError("Missing Ogg BOS"); serial=le32(p+14); }
        if ((at && (flags&2U)) || le32(p+14)!=serial || le32(p+18)!=sequence++) throw AudioError("Discontinuous Ogg stream");
        std::size_t page=27U+segments;
        if (page>size-at) throw AudioError("Truncated Ogg lacing");
        for (unsigned i=0;i<segments;++i) page+=p[27+i];
        if (page>size-at) throw AudioError("Truncated Ogg page body");
        std::uint32_t crc=0;
        for (std::size_t i=0;i<page;++i) {
            const auto byte=i>=22 && i<26 ? 0U : p[i];
            crc=(crc<<8U)^crc_table[(crc>>24U)^byte];
        }
        if (crc!=le32(p+22)) throw AudioError("Ogg page checksum mismatch");
        if (flags&4U) { end=true; samples=std::uint64_t{le32(p+6)} | (std::uint64_t{le32(p+10)}<<32U); }
        at+=page;
    }
    if (!end || !samples) throw AudioError("Missing Ogg EOS or empty Vorbis stream");
    return samples;
}
AudioClip decode(const Bytes& b,bool music_file,std::string name,std::string source) {
    const auto offset=wrapper_offset(b,music_file);
    if (!music_file && b[57]=='0') {
        const auto format=le16(b.data()+84),channels=le16(b.data()+86);
        const auto rate=le32(b.data()+88);
        const auto align=le16(b.data()+96),bits=le16(b.data()+98);
        if (format!=1 || channels<1 || channels>2 || !rate || rate>192000 ||
            (bits!=8 && bits!=16) || align!=channels*bits/8U) throw AudioError("Invalid VAW PCM format");
        const auto size=le32(b.data()+offset-4U);
        if (size>b.size()-offset) throw AudioError("Truncated VAW PCM data");
        const auto frames=size/align;
        if (!frames || frames>max_pcm/(sizeof(std::int16_t)*channels)) throw AudioError("Invalid VAW PCM extent");
        auto clip=std::make_shared<Clip>();
        clip->info={std::move(name),std::move(source),rate,channels,frames};
        const auto total=static_cast<std::size_t>(frames)*channels;
        clip->pcm.resize(total);
        for (std::size_t i=0;i<total;++i) {
            if (bits==8) clip->pcm[i]=static_cast<std::int16_t>((static_cast<int>(b[offset+i])-128)*256);
            else {
                const auto sample=le16(b.data()+offset+i*2U);
                clip->pcm[i]=static_cast<std::int16_t>(sample<32768U ? static_cast<int>(sample) : static_cast<int>(sample)-65536);
            }
        }
        return clip;
    }
    if (b.size()-offset<4 || !tag(b.data()+offset,"OggS")) throw AudioError("Missing OggS at wrapper boundary");
    const auto* ogg=b.data()+offset; const auto size=b.size()-offset;
    auto frames=check_pages(ogg,size);
    int error=0;
    using Decoder=std::unique_ptr<stb_vorbis,decltype(&stb_vorbis_close)>;
    Decoder dec(stb_vorbis_open_memory(ogg,static_cast<int>(size),&error,nullptr),stb_vorbis_close);
    if (!dec) throw AudioError("Vorbis header rejected: "+std::to_string(error));
    const auto info=stb_vorbis_get_info(dec.get());
    if (info.channels<1 || info.channels>2 || !info.sample_rate || info.sample_rate>192000 ||
        frames>max_pcm/(sizeof(std::int16_t)*static_cast<unsigned>(info.channels))) throw AudioError("Invalid Vorbis format or PCM extent");
    auto clip=std::make_shared<Clip>();
    clip->info={std::move(name),std::move(source),info.sample_rate,static_cast<unsigned>(info.channels),frames};
    const auto total=static_cast<std::size_t>(frames)*clip->info.channels;
    clip->pcm.resize(total);
    std::size_t filled=0;
    while (filled<total) {
        const int capacity=static_cast<int>(std::min<std::size_t>(total-filled,16384));
        const int n=stb_vorbis_get_samples_short_interleaved(dec.get(),info.channels,clip->pcm.data()+filled,capacity);
        if (n<=0) throw AudioError("Vorbis ended before its final sample");
        filled+=static_cast<std::size_t>(n)*clip->info.channels;
    }
    if (stb_vorbis_get_error(dec.get())!=VORBIS__no_error) throw AudioError("Vorbis decode error");
    return clip;
}
} // namespace

AudioAssets::AudioAssets(const Path& root,std::size_t limit,const resource::FilenameEncoder& encode)
    :root_(root),limit_(limit),system_voice_count_(1) {
    std::istringstream game(game_config::read(root,"init","game.xtx"));
    for (std::string line;std::getline(game,line);) {
        std::istringstream row(line); std::string key; int value=0;
        if (row>>key>>value && game_config::ascii_lower(key)=="usesystemvoice") {
            if (value<0 || value>255) throw AudioError("System voice theme count outside limit");
            system_voice_count_=static_cast<unsigned>(value)+1; break;
        }
    }
    bgms_=names(game_config::read(root,"spt","bgmlist.xtx"));
    sounds_=names(game_config::read(root,"spt","selist.xtx"));
    default_system_sounds_=names(game_config::read(root,"nya","systemse.xtx"));
    system_sounds_=default_system_sounds_;
    button_voices_=names(game_config::read(root,"nya","buttonVoice0.xtx"));
    config_sounds_=names(game_config::read(root,"nya","configse.xtx"));
    characters_=names(game_config::read(root,"spt","charaname.xtx"));
    const auto scan=[](const Path& dir,auto accept) {
        if (!dir.empty()) for (const auto& f:fs::directory_iterator(dir)) accept(f);
    };
    scan(resource::find_directory(root,"cdvaw"),[&](const auto& f) {
        const auto ext=game_config::ascii_lower(f.path().extension().u8string());
        if (f.is_directory()) {
            const auto key=resource::filename_key(f.path().filename(),encode);
            if (!key) return;
            scan(f.path(),[&](const auto& member) {
                if (!member.is_regular_file() || game_config::ascii_lower(member.path().extension().u8string())!=".vaw") return;
                if (const auto name=resource::filename_key(member.path().stem(),encode)) speech_[*key+"/"+*name]=member.path();
            });
        } else if ((ext==".vtb" || ext==".vpk") && f.is_regular_file()) {
            if (const auto key=resource::filename_key(f.path().stem(),encode)) (ext==".vtb" ? tables_ : packs_)[*key]=f.path();
        }
    });
    auto loose_dir=[&](const Path& dir,auto& dest) {
        scan(dir,[&](const auto& f) {
            if (!f.is_regular_file() || game_config::ascii_lower(f.path().extension().u8string())!=".vaw") return;
            if (const auto key=resource::filename_key(f.path().stem(),encode)) dest[*key]=f.path();
        });
    };
    const auto vaw=resource::find_directory(root,"vaw");
    loose_dir(resource::find_directory(vaw,"se"),se_); loose_dir(resource::find_directory(vaw,"sys"),sys_);
    scan(resource::find_directory(root,"wgq"),[&](const auto& f) {
        const auto ext=game_config::ascii_lower(f.path().extension().u8string());
        if ((ext!=".wgq" && ext!=".mml") || !f.is_regular_file()) return;
        if (const auto key=resource::filename_key(f.path().stem(),encode)) (ext==".wgq" ? wgq_ : mml_)[*key]=f.path();
    });
}
const AudioAssets::Pack& AudioAssets::voice_index(const std::string& prefix) {
    auto existing=indexes_.find(prefix);
    if(existing!=indexes_.end()) return existing->second;
    const auto t=tables_.find(prefix),p=packs_.find(prefix);
    if(t==tables_.end() || p==packs_.end()) throw AudioError("Missing voice package "+prefix,true);
    auto b=read_bytes(t->second,0,fs::file_size(t->second),4U*1024U*1024U);
    const auto size=fs::file_size(p->second);
    if (b.size()<12 || b.size()%12 ||
        !std::all_of(b.end()-12,b.end()-4,[](auto x){return x==0;}) || le32(b.data()+b.size()-4)!=size)
        throw AudioError("Invalid VTB sentinel or VPK length: "+prefix);
    Pack pack{p->second,{}};
    for (std::size_t i=0;i+12<b.size();i+=12) {
        const auto* bytes=b.data()+i;
        const auto* nul=std::find(bytes,bytes+8,0);
        if (nul==bytes || !std::all_of(nul,bytes+8,[](auto x){return x==0;})) throw AudioError("Invalid VTB name");
        auto name=fold_cp932(std::string(reinterpret_cast<const char*>(bytes),static_cast<std::size_t>(nul-bytes)));
        const auto begin=le32(bytes+8),end=le32(bytes+20);
        if (begin>=end || end>size || !pack.members.emplace(name,Member{begin,end}).second) throw AudioError("Invalid VTB member: "+name);
    }
    return indexes_.emplace(prefix,std::move(pack)).first->second;
}
AudioClip AudioAssets::read(const Path& file,const std::string& name,Member m,bool music_file) {
    const auto source=file.lexically_relative(root_).generic_u8string();
    const auto key=source+"@"+std::to_string(m.begin);
    const auto found=cache_.find(key);
    if(found!=cache_.end()) { ++hits; recent_.splice(recent_.begin(),recent_,found->second); return found->second->clip; }
    auto bytes=read_bytes(file,m.begin,m.end,max_container);
    AudioClip clip;
    try { clip=decode(bytes,music_file,name,source); }
    catch(const AudioError& e) { throw AudioError(source+" ["+name+"]: "+e.what()); }
    ++decodes;
    const auto weight=clip->pcm.size()*sizeof(std::int16_t);
    if (weight<=limit_) {
        while (!recent_.empty() && bytes_>limit_-weight) {
            auto& v=recent_.back(); bytes_-=v.clip->pcm.size()*sizeof(std::int16_t); cache_.erase(v.key); recent_.pop_back();
        }
        recent_.push_front({key,clip}); cache_[key]=recent_.begin(); bytes_+=weight;
    }
    return clip;
}
AudioClip AudioAssets::loose(const Path& p,const std::string& name,bool music_file) {
    return read(p,name,{0,fs::file_size(p)},music_file);
}
const AudioAssets::Path* AudioAssets::loose_speech(const std::string& key,const std::string& prefix) const {
    const auto local=speech_.find(prefix+"/"+key);
    if (local!=speech_.end()) return &local->second;
    const auto system=sys_.find(key);
    return system!=sys_.end() ? &system->second : nullptr;
}
bool AudioAssets::has_speech(const std::string& raw) {
    const auto key=fold_cp932(raw),prefix=key.substr(0,2);
    if (loose_speech(key,prefix)) return true;
    if (failed_voice_indexes_.count(prefix)) return false;
    try {
        const auto& pack=voice_index(prefix);
        return pack.members.count(key)!=0;
    } catch (const std::exception&) {
        failed_voice_indexes_.insert(prefix);
        return false;
    }
}
AudioClip AudioAssets::speech(const std::string& raw) {
    const auto key=fold_cp932(raw),prefix=key.substr(0,2);
    if (const auto* local=loose_speech(key,prefix)) return loose(*local,key,false);
    const auto& pack=voice_index(prefix);
    const auto member=pack.members.find(key);
    if(member==pack.members.end()) throw AudioError("Voice member missing: "+key,true);
    return read(pack.file,key,member->second,false);
}
AudioClip AudioAssets::se(int id,bool system) {
    const auto key=item(system ? system_sounds_ : sounds_,id,system ? "System SE" : "SE");
    if (system) return system_wave(key);
    const auto f=se_.find(key);
    if(f==se_.end()) throw AudioError("Missing SE: "+key,true);
    return loose(f->second,key,false);
}
void AudioAssets::set_system_voice_selection(unsigned index) {
    if (index>=system_voice_count_) throw AudioError("System voice theme outside configured range");
    const auto suffix=std::to_string(index)+".xtx";
    const auto button_name="buttonVoice"+suffix;
    auto system=index ? names(game_config::read(root_,"nya","systemvoice"+suffix)) : default_system_sounds_;
    auto buttons=names(game_config::read(root_,"nya",button_name));
    // The source permits an absent button-voice list (a theme can contain only SE).
    if (index && system.empty()) throw AudioError("System voice theme unavailable: "+std::to_string(index),true);
    auto config=index ? names(game_config::read(root_,"nya","configsevoice"+suffix)) : std::vector<std::string>{};
    system_sounds_=std::move(system); button_voices_=std::move(buttons);
    themed_config_sounds_=std::move(config); system_voice_=index;
}
AudioClip AudioAssets::system_wave(const std::string& name) {
    const auto found=sys_.find(name);
    if (found==sys_.end()) throw AudioError("Missing system wave: "+name,true);
    return loose(found->second,name,false);
}
std::string AudioAssets::default_system_sound_name(int number) const {
    return number>=0 && static_cast<std::size_t>(number)<default_system_sounds_.size() ? default_system_sounds_[number] : "";
}
std::string AudioAssets::button_voice_name(int number) const { return item(button_voices_,number,"Button voice"); }
std::string AudioAssets::config_sound_name(int number,bool themed) const {
    if (number<0 || static_cast<std::size_t>(number)>=config_sounds_.size()) return {};
    const auto& table=themed && system_voice_!=0 ? themed_config_sounds_ : config_sounds_;
    return static_cast<std::size_t>(number)<table.size() ? table[number] : "";
}
Program AudioAssets::bgm(int id) {
    return bgm(item(bgms_,id-1,"BGM"));
}
int AudioAssets::bgm_number(const std::string& name) const {
    const auto found=std::find(bgms_.begin(),bgms_.end(),fold_cp932(name));
    return found==bgms_.end() ? 0 : static_cast<int>(found-bgms_.begin()+1);
}
Program AudioAssets::bgm(const std::string& name) {
    Program p; p.name=fold_cp932(name);
    const auto m=mml_.find(p.name);
    auto sequence=m==mml_.end() ? Sequence{{p.name},{}} : Mml(text_file(m->second)).parse();
    p.loop_start=sequence.loop;
    for(const auto& segment:sequence.names) {
        const auto f=wgq_.find(segment);
        if(f==wgq_.end()) throw AudioError("Missing WGQ segment: "+segment,true);
        p.parts.push_back(loose(f->second,segment,true));
    }
    return p;
}
} // namespace nnn::audio
