#include "pc_save.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace emberfall::scene::pc {
namespace {

constexpr std::size_t header_size=32;
constexpr std::size_t legacy_status_size=7088, status_size=8112;
constexpr std::size_t max_extensions=256;
constexpr std::size_t max_blocks=8+3+max_extensions+32;
constexpr std::size_t max_thumbnail_pixels=4U*1024U*1024U;
constexpr int print_mode=10, select_mode=9;
constexpr std::uint32_t last_extension_code=first_extension_code+max_extensions-1;

[[noreturn]] void invalid(std::string_view reason) {
    throw std::runtime_error("PC save: "+std::string(reason));
}

class View {
public:
    View(const nnn::Byte* data,std::size_t size) : data_(data),size_(size) {}
    std::size_t size() const { return size_; }
    const nnn::Byte* at(std::size_t offset,std::size_t length) const {
        if (offset>size_ || length>size_-offset) invalid("field exceeds block");
        return data_+offset;
    }
    std::uint32_t u32(std::size_t offset) const {
        const auto* p=at(offset,4);
        return std::uint32_t(p[0]) | (std::uint32_t(p[1])<<8) |
            (std::uint32_t(p[2])<<16) | (std::uint32_t(p[3])<<24);
    }
    std::int32_t i32(std::size_t offset) const {
        const auto word=u32(offset);
        if (word<=std::uint32_t(std::numeric_limits<std::int32_t>::max())) return static_cast<std::int32_t>(word);
        return -1-static_cast<std::int32_t>(~word);
    }
    std::uint16_t u16(std::size_t offset) const {
        const auto* p=at(offset,2);
        return static_cast<std::uint16_t>(unsigned(p[0]) | (unsigned(p[1])<<8));
    }
    template<std::size_t N>
    void words(std::size_t offset,std::array<std::int32_t,N>& result) const {
        for (auto& value:result) { value=i32(offset); offset+=4; }
    }
    std::string text(std::size_t offset,std::size_t width,bool required=true) const {
        const auto* first=reinterpret_cast<const char*>(at(offset,width));
        const auto* end=static_cast<const char*>(std::memchr(first,0,width));
        if (end) return {first,end};
        if (required) invalid("unterminated fixed-width string");
        return {};
    }
private:
    const nnn::Byte* data_;
    std::size_t size_;
};

class OutputBlock {
public:
    OutputBlock(std::vector<nnn::Byte>& output,std::size_t length,std::uint32_t code,std::string_view name)
        : bytes_(output),offset_(output.size()),length_(length) {
        if (length<header_size || length>max_save_bytes-offset_) invalid("output block exceeds capacity");
        bytes_.resize(offset_+length,0);
        u32(0,static_cast<std::uint32_t>(length));u32(4,code);text(16,16,name);
    }
    void u32(std::size_t at,std::uint32_t value) {
        auto* p=field(at,4);
        for (unsigned i=0;i<4;++i) p[i]=static_cast<nnn::Byte>(value>>(i*8U));
    }
    void i32(std::size_t at,std::int32_t value) { u32(at,static_cast<std::uint32_t>(value)); }
    void u16(std::size_t at,std::uint16_t value) {
        auto* p=field(at,2);
        p[0]=static_cast<nnn::Byte>(value);p[1]=static_cast<nnn::Byte>(value>>8U);
    }
    void text(std::size_t at,std::size_t width,std::string_view value) {
        if (value.size()>=width || value.find('\0')!=std::string_view::npos) invalid("output string does not fit");
        if (!value.empty()) std::memcpy(field(at,width),value.data(),value.size());
    }
    template<std::size_t N> void raw(std::size_t at,const std::array<nnn::Byte,N>& value) {
        std::copy(value.begin(),value.end(),field(at,N));
    }
    template<class T> void words(std::size_t at,const T& values) {
        for (const auto value:values) { i32(at,value);at+=4; }
    }
    void rect(std::size_t at,const Rect& value) {
        words(at,std::array<int,4>{value.left,value.top,value.right,value.bottom});
    }
private:
    nnn::Byte* field(std::size_t at,std::size_t length) {
        if (at>length_ || length>length_-at) invalid("output field exceeds block");
        return bytes_.data()+offset_+at;
    }
    std::vector<nnn::Byte>& bytes_;
    std::size_t offset_,length_;
};

Header read_header(View view) {
    return {view.u32(0),view.u32(4),view.u32(8),view.u32(12),view.text(16,16)};
}

struct Block {
    Header header;
    View view;
};

bool sjis_lead(unsigned char c) {
    return (c>=0x81 && c<=0x9f) || (c>=0xe0 && c<=0xfc);
}

bool safe_component(std::string_view part) {
    return !part.empty() && part.back()!='.' && part.back()!=' ';
}

// Resource names are relative CP932 paths. A backslash can be part of a
// two-byte character, so recognize separators only outside those pairs.
bool safe_resource(std::string_view name) {
    std::size_t start=0;
    for (std::size_t i=0;i<name.size();++i) {
        const auto c=static_cast<unsigned char>(name[i]);
        if (sjis_lead(c)) {
            if (++i>=name.size()) return false;
            const auto trail=static_cast<unsigned char>(name[i]);
            if (trail<0x40 || trail==0x7f || trail>0xfc) return false;
        } else if (c=='/' || c=='\\') {
            if (!safe_component(name.substr(start,i-start))) return false;
            start=i+1;
        } else if (c<0x20 || c==0x7f || c==':' || c=='*' || c=='?' ||
                   c=='"' || c=='<' || c=='>' || c=='|') {
            return false;
        }
    }
    return safe_component(name.substr(start));
}

std::string resource(View view,std::size_t offset,std::size_t width,bool active) {
    auto name=view.text(offset,width,active);
    if (!name.empty() && !safe_resource(name)) {
        if (active) invalid("invalid active resource name");
        name.clear();
    }
    return name;
}

Info read_info(View view) {
    if (view.size()!=64) invalid("unsupported INFO layout");
    Info info;
    info.data_count=view.i32(32);
    info.version=view.i32(36);
    info.extension_count=view.i32(40);
    const auto omake=view.u32(44),cutin=view.u32(48),log=view.u32(52);
    if (info.data_count<8 || static_cast<std::size_t>(info.data_count)>max_blocks ||
        info.version<0 || info.version>0xffffff ||
        info.extension_count<0 || static_cast<std::size_t>(info.extension_count)>max_extensions ||
        omake>1 || cutin>1 || log>1) invalid("invalid INFO values");
    info.has_omake=omake!=0;
    info.has_cutin=cutin!=0;
    info.has_log=log!=0;
    return info;
}

void read_vm_slots(View view,Status& status) {
    const std::array<std::size_t,vm_count> script_fields{{100,104,108,124}};
    for (std::size_t i=0;i<status.vm.size();++i) {
        auto& slot=status.vm[i];
        slot.script_number=view.i32(script_fields[i]);
        slot.pc=view.i32(1888+i*4);
        const auto stack=1952+i*1024;
        slot.stack_pointer=view.i32(stack);
        view.words(stack+4,slot.call_stack);
        if (i>static_cast<std::size_t>(status.run_mode)) continue;
        if (slot.pc<0 || (i!=0 && slot.script_number<0) ||
            slot.stack_pointer<0 || slot.stack_pointer>=250) invalid("invalid active VM slot");
        if (std::any_of(slot.call_stack.begin(),slot.call_stack.begin()+slot.stack_pointer,
                        [](int entry){ return entry<0; })) invalid("invalid active return stack");
        if (status.adjust_version==1 && i<static_cast<std::size_t>(status.run_mode) && slot.call_stack[253]<0)
            invalid("invalid parent script call index");
    }
}

Status read_status(View view) {
    if (view.size()!=legacy_status_size && view.size()!=status_size)
        invalid("unsupported GAMESTATUS layout");
    Status status;
    status.saved_at={view.i32(32),view.i32(36),view.i32(40),view.i32(44),
                     view.i32(48),view.i32(52),view.i32(56),view.i32(60)};
    status.game_month=view.i32(64); status.game_day=view.i32(68);
    status.game_hour=view.i32(72); status.game_weekday=view.i32(76);
    status.game_mode=view.i32(80); status.save_count=view.i32(84);
    status.adjust_version=view.i32(88); status.run_mode=view.i32(96);
    if (status.game_mode<0 || status.game_mode>255 || status.adjust_version<0 || status.adjust_version>1 ||
        status.run_mode<0 || static_cast<std::size_t>(status.run_mode)>=vm_count)
        invalid("unsupported PC save mode or script layout");
    read_vm_slots(view,status);
    status.last_message_id=view.i32(112); status.message_count=view.i32(116);
    status.message_sub_mode=view.i32(120); status.message_serial=view.i32(204);
    status.last_select_id=view.i32(128); status.select_count=view.i32(132);
    status.select_add_count=view.i32(136); status.select_serial=view.i32(140);
    if (status.game_mode==print_mode &&
        (status.message_count<0 || static_cast<std::size_t>(status.message_count)>message_limit))
        invalid("invalid message count");
    if (status.game_mode==select_mode &&
        (status.select_count<0 || static_cast<std::size_t>(status.select_count)>message_limit ||
         status.select_add_count<0 || status.select_add_count>status.select_count))
        invalid("invalid selection message count");
    status.bgm_number=view.i32(144); status.bgm_plays=view.i32(148); status.bgm_volume=view.i32(152);
    if (status.bgm_number< -1) invalid("invalid BGM number");
    const auto* voice_counts=view.at(156,status.voice_exist_count.size());
    std::copy_n(voice_counts,status.voice_exist_count.size(),status.voice_exist_count.begin());
    view.words(160,status.bgm_params);
    status.window_visible=view.i32(176)!=0; status.frame_ms=view.i32(180);
    status.remaining=view.i32(184); status.last_heroine=view.i32(188); status.layer_off=view.u32(192);
    status.font_type=view.i32(196); status.rename_layer=view.i32(200);
    for (std::size_t i=0;i<status.loop_sound.size();++i) {
        auto& sound=status.loop_sound[i];
        view.words(208+i*64,sound);
        if (sound[0]!=0 && sound[1]<0) invalid("invalid active looping sound number");
    }
    status.window_number=view.i32(720); status.cursor_number=view.i32(724); status.mouse_number=view.i32(728);
    for (std::size_t i=0;i<status.loop_voice.size();++i) {
        auto& voice=status.loop_voice[i];
        view.words(736+i*64,voice);
        const bool active=i<status.voice_exist_count.size() && (voice[0]!=0 || status.voice_exist_count[i]!=0);
        auto& name=status.loop_voice_names[i];
        name=resource(view,1248+i*64,64,active);
        if (i<status.voice_exist_count.size() && voice[0]!=0 && name.empty())
            invalid("missing active looping voice filename");
    }
    status.mini_comment=view.text(1760,32);
    status.surname=view.text(1792,16); status.given_name=view.text(1808,16);
    status.script_filename=view.text(1824,16,false);
    const auto* guid=view.at(1840,status.guid.size());
    std::copy_n(guid,status.guid.size(),status.guid.begin());
    for (std::size_t i=0;i<status.layer_variables.size();++i)
        status.layer_variables[i]=view.u16(1856+i*2);
    status.config_mask=view.i32(1904); status.skip_film=view.i32(1908);
    status.select_time_limit=view.i32(1920); status.auto_select=view.i32(1924);
    status.select_time_count=view.i32(1928); status.special_flag=view.i32(1932);
    status.auto_message=view.i32(1936); status.cannot_click=view.i32(1940); status.cannot_skip=view.i32(1944);
    status.large_comment_flag=view.i32(6048);
    for (std::size_t i=0;i<status.large_comments.size();++i)
        status.large_comments[i]=view.text(6064+i*256,256,status.large_comment_flag!=0);
    if (view.size()==status_size) {
        // Old saves predate this preview text. The current PC writer copies
        // its active rows without explicitly writing a terminator.
        for (std::size_t i=0;i<status.log_messages.size();++i)
            status.log_messages[i]=view.text(legacy_status_size+i*256,256,false);
    }
    return status;
}

void append_status(std::vector<nnn::Byte>& bytes,const Status& status,bool full) {
    OutputBlock state(bytes,full?status_size:legacy_status_size,2,"GAMESTATUS");
    const auto& time=status.saved_at;
    state.words(32,std::array<int,8>{time.year,time.month,time.day,time.weekday,time.hour,time.minute,time.second,time.millisecond});
    state.words(64,std::array<int,9>{status.game_month,status.game_day,status.game_hour,status.game_weekday,status.game_mode,
        status.save_count,status.adjust_version,0,status.run_mode});
    state.words(112,std::array<int,3>{status.last_message_id,status.message_count,status.message_sub_mode});
    state.words(128,std::array<int,7>{status.last_select_id,status.select_count,status.select_add_count,status.select_serial,
        status.bgm_number,status.bgm_plays,status.bgm_volume});
    std::uint32_t voice=0;
    for (unsigned i=0;i<status.voice_exist_count.size();++i) voice|=std::uint32_t{status.voice_exist_count[i]}<<(8U*i);
    state.u32(156,voice);state.words(160,status.bgm_params);
    state.words(176,std::array<int,4>{status.window_visible?1:0,status.frame_ms,status.remaining,status.last_heroine});
    state.u32(192,status.layer_off);
    state.words(196,std::array<int,3>{status.font_type,status.rename_layer,status.message_serial});
    for (std::size_t i=0;i<status.loop_sound.size();++i) state.words(208+i*64,status.loop_sound[i]);
    state.words(720,std::array<int,3>{status.window_number,status.cursor_number,status.mouse_number});
    for (std::size_t i=0;i<status.loop_voice.size();++i) {
        state.words(736+i*64,status.loop_voice[i]);state.text(1248+i*64,64,status.loop_voice_names[i]);
    }
    state.text(1760,32,status.mini_comment);state.text(1792,16,status.surname);state.text(1808,16,status.given_name);
    state.text(1824,16,status.script_filename);
    for (std::size_t i=0;i<status.layer_variables.size();++i) state.u16(1856+i*2,status.layer_variables[i]);
    state.words(1904,std::array<int,11>{status.config_mask,status.skip_film,0,0,status.select_time_limit,status.auto_select,
        status.select_time_count,status.special_flag,status.auto_message,status.cannot_click,status.cannot_skip});
    constexpr std::array<std::size_t,vm_count> script_fields{{100,104,108,124}};
    for (std::size_t i=0;i<vm_count;++i) {
        const auto& slot=status.vm[i];
        state.i32(script_fields[i],slot.script_number);state.i32(1888+i*4,slot.pc);
        state.i32(1952+i*1024,slot.stack_pointer);state.words(1956+i*1024,slot.call_stack);
    }
    state.i32(6048,status.large_comment_flag);
    for (std::size_t i=0;i<status.large_comments.size();++i) state.text(6064+i*256,256,status.large_comments[i]);
    if (full) {
        state.raw(1840,status.guid);
        for (std::size_t i=0;i<status.log_messages.size();++i)
            state.text(legacy_status_size+i*256,256,status.log_messages[i]);
    }
}

Thumbnail read_thumbnail(View view) {
    constexpr std::size_t pixels_offset=48;
    Thumbnail image;
    image.width=view.i32(32); image.height=view.i32(36);
    if (image.width<1 || image.width>4096 || image.height<1 || image.height>4096 ||
        std::uint64_t(image.width)*std::uint64_t(image.height)>max_thumbnail_pixels)
        invalid("invalid thumbnail dimensions");
    const auto count=static_cast<std::size_t>(image.width)*static_cast<std::size_t>(image.height);
    if (view.size()!=pixels_offset+count*4) invalid("thumbnail extent does not match dimensions");
    image.pixels.resize(count);
    for (std::size_t i=0;i<count;++i) image.pixels[i]=view.u32(pixels_offset+i*4);
    return image;
}

Rect read_rect(View view,std::size_t offset) {
    return {view.i32(offset),view.i32(offset+4),view.i32(offset+8),view.i32(offset+12)};
}

void read_effects(View effects,View filenames,Save& save) {
    constexpr std::size_t effect_size=1024, name_size=256;
    if (effects.size()!=header_size+layer_count*effect_size ||
        filenames.size()!=header_size+layer_count*name_size)
        invalid("unsupported effect or filename layout");
    for (std::size_t i=0;i<layer_count;++i) {
        const auto base=header_size+i*effect_size;
        auto& effect=save.effects[i];
        effect.flag=effects.i32(base); effect.picture=effects.i32(base+4); effect.command=effects.i32(base+8);
        effect.src=read_rect(effects,base+12); effect.dst=read_rect(effects,base+28);
        effect.src_from=read_rect(effects,base+44); effect.dst_from=read_rect(effects,base+60);
        effect.src_to=read_rect(effects,base+76); effect.dst_to=read_rect(effects,base+92);
        effect.count=effects.i32(base+108); effect.count_max=effects.i32(base+112);
        effects.words(base+116,effect.parameters);
        effect.not_transparent=effects.i32(base+1012)!=0; effect.previous_effect=effects.i32(base+1016);
        save.filenames[i]=resource(filenames,header_size+i*name_size,name_size,effect.flag!=0);
    }
}

Log read_log(View view) {
    constexpr std::size_t row_width=1024, voice_width=16;
    constexpr auto legacy_prefix=header_size+32, current_prefix=header_size+40;
    constexpr auto text_bytes=log_limit*row_width, color_bytes=log_limit*4, voice_bytes=log_limit*voice_width;
    std::size_t prefix=0, max_voices=0;
    if (view.size()==legacy_prefix+color_bytes+text_bytes+voice_bytes) {
        prefix=legacy_prefix; max_voices=1;
    } else if (view.size()==current_prefix+color_bytes+text_bytes+voice_bytes*8) {
        prefix=current_prefix; max_voices=8;
    } else {
        invalid("unsupported LOG layout");
    }
    Log log;
    log.layout=max_voices==1 ? LogLayout::Legacy : LogLayout::MultiVoice;
    log.next=view.i32(32); log.count=view.i32(36); log.voice_count=view.i32(40); log.tail=view.i32(44);
    if (log.next<0 || static_cast<std::size_t>(log.next)>=log_limit ||
        log.count<0 || static_cast<std::size_t>(log.count)>log_limit ||
        log.voice_count<0 || static_cast<std::size_t>(log.voice_count)>max_voices)
        invalid("invalid backlog ring position or voice count");
    log.voice_count=std::max(1,log.voice_count);
    log.entries.reserve(static_cast<std::size_t>(log.count));
    const auto first=(static_cast<std::size_t>(log.next)+log_limit-static_cast<std::size_t>(log.count))%log_limit;
    const auto text_offset=prefix+color_bytes, voice_offset=text_offset+text_bytes;
    for (std::size_t i=0;i<static_cast<std::size_t>(log.count);++i) {
        const auto slot=(first+i)%log_limit;
        LogEntry entry;
        entry.color=view.u32(prefix+slot*4);
        entry.text=view.text(text_offset+slot*row_width,row_width);
        const auto voices=static_cast<std::size_t>(log.voice_count);
        for (std::size_t channel=0;channel<voices;++channel)
            entry.voices[channel]=resource(view,voice_offset+(slot*voices+channel)*voice_width,voice_width,true);
        log.entries.push_back(std::move(entry));
    }
    return log;
}

} // namespace

Status parse_status_block(const nnn::Byte* bytes,std::size_t size) {
    if (!bytes || (size!=legacy_status_size && size!=status_size)) invalid("unsupported GAMESTATUS layout");
    const View view(bytes,size);
    const auto header=read_header(view);
    if (header.size!=size || header.code!=2 || header.name!="GAMESTATUS")
        invalid("invalid standalone GAMESTATUS header");
    return read_status(view);
}

std::vector<nnn::Byte> write_status_block(const Status& status) {
    std::vector<nnn::Byte> bytes;
    append_status(bytes,status,true);
    (void)read_status(View(bytes.data(),bytes.size()));
    return bytes;
}

Save parse_save(const std::vector<nnn::Byte>& bytes,std::string_view expected_header,
                std::optional<std::uint32_t> replacement_code) {
    if (bytes.size()<header_size || bytes.size()>max_save_bytes) invalid("invalid file size");
    if (replacement_code && (*replacement_code<first_extension_code || *replacement_code>last_extension_code))
        invalid("invalid okikae extension code");
    const View input(bytes.data(),bytes.size());
    std::map<std::uint32_t,Block> blocks;
    for (std::size_t offset=0;offset<bytes.size();) {
        const View remaining(input.at(offset,header_size),bytes.size()-offset);
        auto header=read_header(remaining);
        if (header.size<header_size || header.size>remaining.size()) invalid("invalid block extent");
        if (offset==0 && (header.code!=0 || header.size!=header_size || expected_header.empty() ||
                          header.name!=expected_header.substr(0,15))) invalid("wrong save header");
        if (blocks.size()>=max_blocks) invalid("too many blocks");
        const auto size=header.size,code=header.code;
        if (!blocks.emplace(code,Block{std::move(header),View(remaining.at(0,size),size)}).second)
            invalid("duplicate block");
        offset+=size;
    }

    const auto required=[&](std::uint32_t code,std::string_view name) -> View {
        const auto found=blocks.find(code);
        if (found==blocks.end() || found->second.header.name!=name)
            invalid("missing or mislabeled "+std::string(name)+" block");
        return found->second.view;
    };
    Save save;
    save.header=blocks.at(0).header;
    save.info=read_info(required(1,"INFO"));
    save.status=read_status(required(2,"GAMESTATUS"));
    save.thumbnail=read_thumbnail(required(3,"MINICG"));
    const auto vars=required(4,"GAMEVAR");
    if (vars.size()!=header_size+1024*4 && vars.size()!=header_size+2224*4)
        invalid("unsupported GAMEVAR layout");
    save.vars.resize((vars.size()-header_size)/4);
    for (std::size_t i=0;i<save.vars.size();++i) save.vars[i]=vars.i32(header_size+i*4);
    read_effects(required(5,"EFFECT"),required(6,"EFFECTFILENAME"),save);
    const auto messages=required(7,"MESSAGE");
    constexpr std::size_t message_width=1024;
    if (messages.size()!=header_size+message_limit*message_width) invalid("unsupported MESSAGE layout");
    const int active_messages=save.status.game_mode==print_mode ? save.status.message_count :
        (save.status.game_mode==select_mode ? save.status.select_count : 0);
    for (std::size_t i=0;i<save.messages.size();++i)
        save.messages[i]=messages.text(header_size+i*message_width,message_width,i<static_cast<std::size_t>(active_messages));

    const auto& info=save.info;
    if ((blocks.count(8)!=0)!=info.has_omake || (blocks.count(9)!=0)!=info.has_cutin ||
        (blocks.count(10)!=0)!=info.has_log) invalid("optional blocks disagree with INFO");
    if (info.has_omake && required(8,"OMAKECLASS").size()!=header_size+3200*4)
        invalid("unsupported OMAKECLASS layout");
    if (info.has_cutin && required(9,"CUTIN").size()!=header_size+64*4*4)
        invalid("unsupported CUTIN layout");
    if (info.has_log) save.log=read_log(required(10,"LOG"));
    const auto expected_ext_end=first_extension_code+static_cast<std::uint32_t>(info.extension_count);
    for (auto code=first_extension_code;code<expected_ext_end;++code)
        required(code,"EXT"+std::to_string(code-first_extension_code+1));
    // The PC writer excludes LOG from INFO.dataKosuu. Unknown blocks are kept
    // opaque, so let them contribute to the count without assuming a schema.
    const auto declared_min=8+info.extension_count+int(info.has_omake)+int(info.has_cutin);
    if (info.data_count<declared_min || static_cast<std::size_t>(info.data_count)>blocks.size())
        invalid("block count disagrees with INFO");
    for (const auto& item:blocks) {
        const auto code=item.first;
        const auto& block=item.second;
        if (code<8 || code==10) continue;
        if (code>=expected_ext_end && code<=last_extension_code) invalid("undeclared extension block");
        if (replacement_code && code==*replacement_code) {
            constexpr std::size_t replacement_count=100, replacement_width=64;
            if (block.view.size()!=header_size+replacement_count*replacement_width)
                invalid("unsupported okikae layout");
            save.replacements.reserve(replacement_count);
            for (std::size_t i=0;i<replacement_count;++i)
                save.replacements.push_back(block.view.text(header_size+i*replacement_width,replacement_width));
        } else {
            const auto size=block.view.size()-header_size;
            const auto* payload=block.view.at(header_size,size);
            save.unsupported_blocks.push_back({block.header,{payload,payload+size}});
        }
    }
    return save;
}

std::vector<nnn::Byte> write_save(const Save& save) {
    if (!save.unsupported_blocks.empty() || save.info.has_cutin || save.info.has_omake)
        invalid("cannot write unresolved PC state");
    if (save.header.name.empty() || (save.vars.size()!=1024 && save.vars.size()!=2224) ||
        (!save.replacements.empty() && save.replacements.size()!=100))
        invalid("invalid PC output layout");
    const auto& thumb=save.thumbnail;
    if (thumb.width<=0 || thumb.height<=0 || thumb.width>4096 || thumb.height>4096 ||
        static_cast<std::uint64_t>(thumb.width)*static_cast<unsigned>(thumb.height)!=thumb.pixels.size() ||
        thumb.pixels.size()>max_thumbnail_pixels)
        invalid("invalid output thumbnail");
    const bool extended=!save.replacements.empty();
    std::vector<nnn::Byte> bytes;
    OutputBlock header(bytes,32,0,save.header.name);
    OutputBlock info(bytes,64,1,"INFO");
    info.words(32,std::array<int,6>{8+(extended?1:0),save.info.version,extended?1:0,0,0,save.log?1:0});
    append_status(bytes,save.status,false);
    OutputBlock image(bytes,48+thumb.pixels.size()*4,3,"MINICG");
    image.i32(32,thumb.width);image.i32(36,thumb.height);
    for (std::size_t i=0;i<thumb.pixels.size();++i) image.u32(48+i*4,thumb.pixels[i]);
    OutputBlock vars(bytes,header_size+save.vars.size()*4,4,"GAMEVAR");
    vars.words(header_size,save.vars);
    OutputBlock effects(bytes,header_size+layer_count*1024,5,"EFFECT");
    OutputBlock filenames(bytes,header_size+layer_count*256,6,"EFFECTFILENAME");
    for (std::size_t i=0;i<layer_count;++i) {
        const auto at=header_size+i*1024;
        const auto& effect=save.effects[i];
        effects.words(at,std::array<int,3>{effect.flag,effect.picture,effect.command});
        effects.rect(at+12,effect.src);effects.rect(at+28,effect.dst);
        effects.rect(at+44,effect.src_from);effects.rect(at+60,effect.dst_from);
        effects.rect(at+76,effect.src_to);effects.rect(at+92,effect.dst_to);
        effects.i32(at+108,effect.count);effects.i32(at+112,effect.count_max);
        effects.words(at+116,effect.parameters);
        effects.i32(at+1012,effect.not_transparent?1:0);effects.i32(at+1016,effect.previous_effect);
        filenames.text(header_size+i*256,256,save.filenames[i]);
    }
    OutputBlock messages(bytes,header_size+message_limit*1024,7,"MESSAGE");
    for (std::size_t i=0;i<message_limit;++i) messages.text(header_size+i*1024,1024,save.messages[i]);
    if (save.log) {
        const auto& log=*save.log;
        const bool modern=log.layout==LogLayout::MultiVoice;
        const std::size_t voice_capacity=modern?8U:1U;
        if (log.entries.size()>log_limit || log.voice_count<1 || static_cast<std::size_t>(log.voice_count)>voice_capacity)
            invalid("invalid output backlog");
        const std::size_t prefix=header_size+(modern?40U:32U);
        const auto voice_stride=static_cast<std::size_t>(log.voice_count);
        const auto color_at=prefix,text_at=color_at+log_limit*4,voice_at=text_at+log_limit*1024;
        OutputBlock backlog(bytes,voice_at+log_limit*16*voice_capacity,10,"LOG");
        const auto count=static_cast<int>(log.entries.size());
        const auto next=count%static_cast<int>(log_limit);
        const auto tail=(next-1+static_cast<int>(log_limit))%static_cast<int>(log_limit);
        backlog.words(32,std::array<int,4>{next,count,log.voice_count,tail});
        for (std::size_t i=0;i<log.entries.size();++i) {
            const auto& line=log.entries[i];
            backlog.u32(color_at+i*4,line.color);
            backlog.text(text_at+i*1024,1024,line.text);
            for (std::size_t channel=0;channel<voice_stride;++channel)
                backlog.text(voice_at+(i*voice_stride+channel)*16,16,line.voices[channel]);
        }
    }
    if (extended) {
        OutputBlock replacement(bytes,header_size+100*64,first_extension_code,"EXT1");
        for (std::size_t i=0;i<save.replacements.size();++i) replacement.text(header_size+i*64,64,save.replacements[i]);
    }
    parse_save(bytes,save.header.name,extended?std::optional<std::uint32_t>{first_extension_code}:std::nullopt);
    return bytes;
}

} // namespace emberfall::scene::pc
