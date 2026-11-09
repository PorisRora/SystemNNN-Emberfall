#include "emberfall/game_session.hpp"
#include "chart_node_store.hpp"
#include "chart_view_model.hpp"
#include "game_assets.hpp"
#include "pc_save.hpp"
#include "nnn/scheduler.hpp"
#include "nnn/game_config.hpp"
#include "nnn/game_name_list.hpp"
#include "nnn/inventory_catalog.hpp"
#include "nnn/optional_catalog.hpp"
#include "nnn/player_behavior.hpp"
#include "nnn/progress_views.hpp"
#include "nnn/reference_selection.hpp"
#include "nnn/runtime_text.hpp"
#include "nnn/status_party.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>

namespace emberfall::scene {
namespace fs=std::filesystem;
using nnn::resource::fold_cp932;
namespace {
constexpr nnn::Word print=0x22220001U, lprint=0x22220002U, append=0x22220003U;
constexpr nnn::Word draw=0x22220004U, overlap=0x22220005U, select=0x22220006U;
constexpr nnn::Word prepare=0x33330011U, clear_all=0x33330012U, effect=0x33330013U, clear=0x33330014U;
constexpr nnn::Word load=0x33330015U, rect=0x33330016U, preload=0x3333001fU;
constexpr nnn::Word end=0x55560005U, next=0x55560006U, exit_script=0x5556000bU;
constexpr nnn::Word end_film=0x5556000cU, set_cg=0x33330018U, set_film=0x3333001aU, set_cg_by_var=0x33330061U;
constexpr nnn::Word set_demo_flag=0x3333001eU, frame_control=0x33330028U, default_frame=0x33330029U;
constexpr nnn::Word start_message=0x3333001bU, start_koma=0x3333001cU;
constexpr nnn::Word film_name=0x33330050U, story_name=0x33330051U, film_type_time=0x33330052U;
constexpr nnn::Word config_mask=0x33330053U, var_control_layer=0x33330063U;
constexpr nnn::Word set_term=0x3333006dU;
constexpr unsigned variable_count=32768;
constexpr unsigned first_system_variable=200;
constexpr std::uint32_t snapshot_magic=0x31534e4eU, snapshot_version=13;
constexpr int default_next_message_type=3;
constexpr int heroine_limit=12,object_limit=1024;
constexpr std::size_t backlog_capacity=100;
constexpr std::size_t backlog_jump_budget=24U*1024U*1024U;
constexpr std::array<int,13> days_of_month{{0,31,29,31,30,31,30,31,31,30,31,30,31}};
constexpr std::array<const char*,8> clock_fields{{"systemyear","systemmonth","systemday","systemhour",
    "systemminute","systemsecond","systemweek","systemmillisecond"}};
enum class ModalAction { Back, Choose, Page, Hover, Confirm, Group, Resume, Scroll, Commit };
enum class ModalResult { Rejected=-1, Updated, ChangedMode, Exit, Unavailable, LeaveReady, EnterStory };

template<class A,class D> void dialogue_fields(A& a,D& d) {
    a(d.script,d.speaker,d.raw_bytes,d.display_bytes,d.message_id,d.table_index,d.command,d.sequence,
        d.window_type,d.font_type,d.window_visible);
}
template<class A,class L> void backlog_fields(A& a,L& l) { a(l.speaker,l.text,l.voices,l.color,l.configuration_text); }
template<class A,class S> void skip_fields(A& a,S& s) {
    a(s.skip_f4_mode,s.skip_f5_mode,s.skip_next_command,s.skip_effect_command,s.skip_to_film_end);
}

std::string file_text(const fs::path& path) { return nnn::game_config::read_text(path); }
std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string s;std::getline(stream,s);) {
        while (!s.empty() && (s.back()=='\r' || s.back()==' ' || s.back()=='\t')) s.pop_back();
        if (!s.empty()) lines.push_back(std::move(s));
    }
    return lines;
}
std::vector<std::string> fields(const std::string& text) {
    std::vector<std::string> out;
    for (const auto& s:lines_of(text)) for (std::size_t p=0;p<s.size();) {
        const auto comma=s.find(',',p);
        out.push_back(s.substr(p,comma==std::string::npos ? comma : comma-p));
        if (comma==std::string::npos) break;
        p=comma+1;
    }
    return out;
}
int number(const std::string& s,int fallback=0) {
    int out=0;
    const auto r=std::from_chars(s.data(),s.data()+s.size(),out);
    return r.ec==std::errc() && r.ptr==s.data()+s.size() ? out : fallback;
}
int config_int(const std::string& text,const std::string& name,int fallback) {
    for (const auto& line:lines_of(text)) {
        std::istringstream s(line); std::string key; int value;
        if (s>>key>>value && fold_cp932(key)==fold_cp932(name)) return value;
    }
    return fallback;
}
std::string config_text(const std::string& text,const std::string& name,std::string fallback) {
    for (const auto& line:lines_of(text)) {
        const auto start=line.find_first_not_of(" \t");
        if (start==std::string::npos) continue;
        const auto split=line.find_first_of(" \t",start);
        if (split==std::string::npos || fold_cp932(line.substr(start,split-start))!=fold_cp932(name)) continue;
        const auto value=line.find_first_not_of(" \t",split);
        if (value!=std::string::npos) return line.substr(value);
    }
    return fallback;
}
bool optional_class_created(const std::string& init,const std::string& name) {
    const bool is_default=config_int(init,name+"IsDefault",0)!=0;
    return config_int(init,name+"ClassDisable",is_default ? 0 : 1)==0;
}
bool valid_edited_text(const std::string& text,bool one_byte,bool kanji) {
    if (text.find('\0')!=std::string::npos) return false;
    if (one_byte) {
        return std::all_of(text.begin(),text.end(),[&](unsigned char c) {
            return kanji ? c>=0x20 && c!=0x7f : c>=0x20 && c<=0x7e && c!='#';
        });
    }
    if (text.size()%2) return false;
    for (std::size_t i=0;i<text.size();i+=2) {
        const auto lead=static_cast<unsigned char>(text[i]);
        if (!nnn::resource::cp932_lead(lead) || (kanji && lead>=0xf0)) return false;
    }
    return true;
}
int config_rgb(const std::string& text,const std::string& name,int r,int g,int b) {
    return (std::clamp(config_int(text,name+"R",r),0,255)<<16) |
           (std::clamp(config_int(text,name+"G",g),0,255)<<8) |
            std::clamp(config_int(text,name+"B",b),0,255);
}
std::string stem(std::string name) {
    name=nnn::resource::normalize_cp932_path(std::move(name));
    const auto slash=name.find_last_of('/');
    if (slash!=std::string::npos) name.erase(0,slash+1);
    if (name.size()>=4 && name.compare(name.size()-4,4,".spt")==0) name.resize(name.size()-4);
    return name;
}

int weekday(std::int64_t year,int month_day) {
    int month=month_day/100;
    if (month<=2) { --year; month+=12; }
    return static_cast<int>((year+year/4-year/100+year/400+(13LL*month+8)/5+month_day%100)%7);
}

nnn::Value pc_random(std::uint32_t& state) {
    const auto draw=[&] {
        state=214013U*state+2531011U;
        return static_cast<nnn::Value>((state>>16U)&0x7fffU);
    };
    for (int i=0;i<50;++i) {
        const auto r=draw();
        if (r<30000) return r%10000;
    }
    return draw()%10000;
}

ScriptClockSample local_clock_now() {
    using namespace std::chrono;
    const auto now=system_clock::now();
    const auto seconds=floor<std::chrono::seconds>(now);
    const auto timestamp=system_clock::to_time_t(seconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local,&timestamp)!=0) throw std::runtime_error("Unable to read the local clock");
#else
    if (!localtime_r(&timestamp,&local)) throw std::runtime_error("Unable to read the local clock");
#endif
    const auto elapsed=duration_cast<milliseconds>(now-seconds).count();
    return {(1900+local.tm_year)*10000+(1+local.tm_mon)*100+local.tm_mday,
            ((local.tm_hour*60+local.tm_min)*60+std::min(local.tm_sec,59))*1000+static_cast<int>(elapsed)};
}

bool valid_clock_sample(const ScriptClockSample& sample) {
    const int year=sample.date/10000, month=sample.date/100%100, day=sample.date%100;
    if (year<1 || year>65535 || month<1 || month>12 || day<1 ||
        sample.milliseconds<0 || sample.milliseconds>=86400000) return false;
    const bool leap=year%4==0 && (year%100!=0 || year%400==0);
    return day<=days_of_month[static_cast<unsigned>(month)]-(month==2 && !leap?1:0);
}

std::array<int,8> local_clock_fields(const ScriptClockSample& sample) {
    return {sample.date/10000,sample.date/100%100,sample.date%100,sample.milliseconds/3600000,
            sample.milliseconds/60000%60,sample.milliseconds/1000%60,weekday(sample.date/10000,sample.date%10000),
            sample.milliseconds%1000};
}

bool pc_space(char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n'; }

bool cg_animation_separator(const std::string& text,std::size_t start,std::size_t comma) {
    while (start<comma && (text[start]=='*' || text[start]=='@' || text[start]=='!')) ++start;
    if (start+1>=comma || text[start++]!='#') return false;
    const auto digit=[](char c){return c>='0' && c<='9';};
    for (;start<comma;++start) if (!digit(text[start])) return false;
    auto end=comma+1;
    while (end<text.size() && digit(text[end])) ++end;
    return end>comma+1 && end<text.size() && text[end]=='/';
}

std::vector<std::string> pc_list(const std::string& text,bool cg=false) {
    if (text.find('\0')!=std::string::npos) throw std::runtime_error("Invalid NUL in SystemNNN list");
    std::vector<std::string> tokens;
    for (std::size_t cursor=0;cursor<text.size();) {
        if (text[cursor]==',') ++cursor;
        while (cursor<text.size() && pc_space(text[cursor])) ++cursor;
        if (cursor==text.size()) break;
        const auto start=cursor;
        while (cursor<text.size()) {
            const auto c=static_cast<unsigned char>(text[cursor]);
            if (pc_space(text[cursor]) || (c==',' && (!cg || !cg_animation_separator(text,start,cursor)))) break;
            const bool lead=(c>=0x81U && c<=0x9fU) || (c>=0xe0U && c<=0xfcU);
            cursor+=lead && cursor+1<text.size() ? 2U : 1U;
        }
        if (cursor-start>255 || tokens.size()==65536) throw std::runtime_error("SystemNNN list exceeds limits");
        tokens.push_back(text.substr(start,cursor-start));
    }
    return tokens;
}

constexpr nnn::Word gallery_key(nnn::GalleryKind kind,nnn::Word group,nnn::Word index) {
    return (static_cast<nnn::Word>(kind)<<28U)|(group<<16U)|index;
}

std::vector<std::size_t> read_cg_counts(const fs::path& root) {
    std::vector<std::size_t> counts;
    std::size_t total=0;
    for (unsigned i=1;i<=256;++i) {
        const char tens=static_cast<char>(i<100 ? '0'+i/10 : 'A'+(i-100)/10);
        const auto name=std::string("cglist")+tens+static_cast<char>('0'+i%10)+".xtx";
        const auto path=nnn::game_config::find(root,"spt",name);
        if (path.empty()) break;
        const auto count=pc_list(file_text(path),true).size();
        total+=count;
        if (total>65536) throw std::runtime_error("CG completion table exceeds limits");
        counts.push_back(count);
    }
    return counts;
}

std::vector<std::vector<nnn::Word>> read_scene_requirements(const fs::path& root) {
    const auto text=nnn::game_config::read(root,"spt","scene.dat");
    if (text.empty()) return {};
    nnn::snapshot::Bytes bytes(text.begin(),text.end());
    nnn::snapshot::Reader in(bytes);
    std::uint32_t size=0;
    in(size);
    if ((size && size!=bytes.size()) || bytes.size()%4!=0)
        throw std::runtime_error("Invalid scene.dat size");
    std::vector<std::vector<nnn::Word>> scenes;
    const auto groups=in.size(256);
    for (std::size_t group=0;group<groups;++group) {
        const auto count=in.size(4096);
        if (count>65536-scenes.size()) throw std::runtime_error("Scene completion table exceeds limits");
        for (std::size_t scene=0;scene<count;++scene) {
            nnn::Value script=0;
            in(script);
            const auto project=script<0 ? -static_cast<std::int64_t>(script) : script;
            if (project>0xfff) throw std::runtime_error("Invalid scene script number");
            const auto films=in.size(4096);
            auto& required=scenes.emplace_back();
            for (std::size_t i=0;i<films;++i) {
                nnn::Value film=0;
                in(film);
                if (film>65536 || film< -65536) throw std::runtime_error("Invalid scene film number");
                if (film>0) required.push_back(gallery_key(nnn::GalleryKind::Film,static_cast<nnn::Word>(project),
                                                           static_cast<nnn::Word>(film-1)));
            }
        }
    }
    if (size && in.remaining()) throw std::runtime_error("Unexpected scene.dat tail");
    return scenes;
}

int completion_percent(std::size_t unlocked,std::size_t total) {
    if (!unlocked || !total) return 0;
    return static_cast<int>(std::max<std::size_t>(1,unlocked*100U/total));
}
std::string speaking(const std::string& raw) {
    // Older scripts may omit the palette entry for a named quotation.
    const auto br=raw.find_first_of("\r\n");
    if (br==std::string::npos || br==0 || br>72) return {};
    const auto text=raw.find_first_not_of("\r\n",br);
    if (text==std::string::npos) return {};
    if (raw[text]=='"' || raw.compare(text,2,"\x81\x67")==0 || raw.compare(text,2,"\x81\x75")==0 ||
        raw.compare(text,2,"\x81\x77")==0) return raw.substr(0,br);
    return {};
}
std::string compact_message(const std::string& text) {
    std::string result;
    for (const auto c:text) if (c!='\r' && c!='\n' && c!=' ' && c!='\t') result+=c;
    return result;
}
render::Rect pc_rectangle(const pc::Rect& rect) {
    constexpr int limit=1000000;
    if (rect.left< -limit || rect.left>limit || rect.top< -limit || rect.top>limit ||
        rect.right<0 || rect.right>limit || rect.bottom<0 || rect.bottom>limit)
        throw std::runtime_error("Invalid PC layer rectangle");
    // SystemNNN stores extent in the fields named right and bottom.
    return {rect.left,rect.top,rect.right,rect.bottom};
}
pc::DateTime current_pc_time() {
    const std::time_t now=std::time(nullptr);
    std::tm parts{};
#ifdef _WIN32
    if (localtime_s(&parts,&now)!=0) return {};
#else
    if (!localtime_r(&now,&parts)) return {};
#endif
    return {parts.tm_year+1900,parts.tm_mon+1,parts.tm_mday,parts.tm_wday,
            parts.tm_hour,parts.tm_min,parts.tm_sec,0};
}
std::vector<std::string> message_rows(const std::string& text) {
    std::vector<std::string> rows;
    for (std::size_t start=0;start<text.size();) {
        const auto end=text.find_first_of("\r\n",start);
        rows.push_back(text.substr(start,end==std::string::npos ? end : end-start));
        if (end==std::string::npos) break;
        start=end+1;
        if (text[end]=='\r' && start<text.size() && text[start]=='\n') ++start;
    }
    return rows;
}
} // namespace

struct GameSession::Impl final : nnn::CScriptCallBack {
    std::shared_ptr<nnn::RuntimeContext> shared_runtime;
    std::unique_ptr<nnn::PlayerBehavior> behavior;
    std::shared_ptr<GameAssets> shared_assets;
    GameAssets& assets;
    nnn::audio::GameAudio playback;
    bool queue_audio_events;
    const bool decorative_preview;
    std::optional<nnn::Value> ui_mode;
    std::optional<std::array<int,2>> ui_click;
    std::string ui_script;
    int window_type=0, font_type=0, next_window=-1, next_message_speed=0,option_off=0;
    int next_message_type=default_next_message_type;
    std::size_t append_prefix_bytes=0;
    bool draw_window_with_message=false;
    bool overlap_message_window=true;
    int long_background_mode=0;
    std::string overlap_window_config,long_background_config;
    int auto_message=0,cannot_click=0,cannot_skip=0;
    bool auto_message_kill_shift=false,cannot_skip_kill_shift=false,effect_gate=false;
    bool background_unlocks=false, skip_cg=true, skip_film=true, skip_music=true, skip_voice_roles=true, ignore_hidden_cg=false;
    bool message_has_voice=false;
    std::uint32_t message_voice_ms=0;
    std::uint32_t speaking_ms=0;
    std::array<std::string,4> backlog_voices;
    std::deque<BacklogEntry> backlog;
    struct JumpPoint {
        BacklogEntry line;
        unsigned cursor;
        std::size_t backlog_size;
        std::uint32_t random_state;
        int mouse_x,mouse_y;
        nnn::snapshot::Bytes state;
    };
    std::deque<JumpPoint> jumps;
    std::uint64_t next_jump_id=1;
    std::size_t jump_bytes=0;
    bool jump_enabled=false,append_jump=false,jump_pending=false;
    unsigned jump_voice_mask=0;
    bool jump_finish_fades=false;
    bool animation_enabled=false,animation_off=false;
    bool system_save_requested=false,script_notices=false;
    unsigned pending_notice=0;
    std::string choice_heading,choice_timeup;
    std::vector<std::string> choice_separator;
    int choice_heading_color=0,choice_color=0,choice_dot_color=0,choice_timeup_color=0;
    unsigned choice_appear_frames=0,choice_appear_remaining=0;
    bool choice_draws_scene=false;
    std::mutex parameter_output_mutex;
    std::uint64_t applied_parameter_revision=0;
    nnn::Value applied_system_voice=0;
    std::unique_ptr<render::Layers> renderer;
    nnn::FunctionRegistry registry;
    std::unique_ptr<nnn::ScriptScheduler> vm;
    FrameState frame;
    SessionStats counters;
    std::vector<AudioEvent> audio;
    std::vector<std::string> messages;
    std::set<std::string> reported;
    std::array<nnn::Value,variable_count> vars{};
    std::unique_ptr<const decltype(vars)> ui_initial_vars;
    bool preview_story_dirty=true;
    unsigned system_variable_end=300;
    nnn::SystemVariables legacy_system_seed{};
    std::unordered_map<nnn::Word,nnn::Word> extended_variables;
    std::unordered_map<nnn::Word,std::uint32_t> layer_visibility;
    std::array<nnn::Value,32> layer_variables{};
    bool require_variant_graphic=false;
    std::unordered_map<std::string,unsigned> ids, script_ids;
    std::vector<std::string> project_scripts;
    std::set<std::string> speaker_names;
    std::vector<std::string> replacements;
    std::vector<std::string> answer_strings;
    unsigned compare_result_var=variable_count,get_result_var=variable_count;
    bool result_enabled=false,scene_list_enabled=true;
    int auto_result_notice=0;
    std::string surname,given_name;
    bool one_byte_text=false;
    bool standalone_script=false;
    std::optional<std::uint32_t> replacement_extension;
    std::string pc_save_header="SAVEDATA";
    unsigned save_slot_art_var=variable_count;
    nnn::Value pc_save_slot_art=30;
    bool pc_export_layout=false,pc_save_log=true;
    bool clear_auto_after_load=false;
    int pc_log_voices=1;
    pc::LogLayout pc_log_layout=pc::LogLayout::Legacy;
    int pc_thumbnail_width=182,pc_thumbnail_height=102;
    unsigned game_version=0,pc_layers=16;
    std::vector<nnn::Word> replay_films;
    std::size_t replay_film=0;
    std::uint32_t hidden_layers=0;
    InputState input;
    unsigned period=20, default_period=20, remainder=0;
    std::uint32_t draw_remaining=0;
    std::size_t movie_count=0;
    std::optional<nnn::Word> movie_command;
    std::unordered_map<nnn::Word,nnn::RuntimeMode> modal_commands;
    struct ModalMusic {
        int number=-1,plays=0,volume=0,fade=-1;
        bool restart=false;
    };
    struct NameInput {
        std::array<bool,2> editable{true,true};
        std::array<bool,2> fixed{};
        std::array<std::string,2> fixed_text;
        std::array<int,2> limits{};
        nnn::Word status=variable_count,time=variable_count;
        bool game_default=false,allow_empty=false,cancel=true;
        ModalMusic music;
    } name_input;
    struct KanjiInput {
        std::array<int,4> limits{};
        ModalMusic music;
    } kanji_input;
    bool name_input_available=false,kanji_input_available=false;
    std::optional<nnn::reference_selection::Hints> hints;
    std::optional<nnn::reference_selection::Zukan> zukan;
    std::unordered_map<std::string,bool> hint_picture_presence;
    std::optional<nnn::Word> zukan_function;
    ModalMusic select_hint_music,print_hint_music,zukan_music;
    bool select_hint_available=false,print_hint_available=false,zukan_available=false;
    int hint_group=1;
    std::unique_ptr<nnn::TermCatalog> term_catalog;
    std::unique_ptr<nnn::VoiceCatalog> voice_catalog;
    std::array<std::int8_t,nnn::runtime_term_count> term_body_state{};
    ModalMusic term_music,voice_music;
    bool term_available=false,voice_available=false;
    bool usable_terms=false,usable_voices=false;
    std::unordered_map<std::string,bool> voice_sources;
    int term_group=0;
    std::unique_ptr<nnn::AchievementCatalog> achievement_catalog;
    std::unique_ptr<nnn::ResultCatalog> result_catalog;
    std::optional<nnn::ResultCatalog::Visit> result_visit;
    ModalMusic achievement_music,result_music;
    bool achievement_available=false,result_available=false;
    std::unique_ptr<nnn::inventory::ItemCatalog> item_catalog;
    std::unique_ptr<nnn::inventory::ShopCatalog> shop_catalog;
    std::optional<nnn::inventory::ShopCatalog::Visit> shop_visit;
    ModalMusic item_music,shop_music;
    bool item_available=false,shop_available=false;
    std::array<std::unique_ptr<nnn::status_party::Catalog>,2> status_catalogs;
    std::optional<nnn::status_party::Visit> status_visit;
    std::array<ModalMusic,2> status_music;
    std::array<bool,2> status_available{};
    bool chart_available=false,chart_failed=false;
    std::unique_ptr<nnn::chart::Graph> chart_graph;
    std::optional<nnn::chart::RunState> chart_run;
    std::optional<ChartNodeStore> chart_store;
    std::optional<ChartInteraction> chart_visit;
    std::shared_ptr<const nnn::chart::Progress> chart_legacy_progress;
    std::shared_ptr<const nnn::PreviewStoryState> chart_title_story;
    std::unordered_map<nnn::Word,nnn::chart::Function> chart_functions;
    ChartViewModel chart_view_model;
    ModalMusic chart_music;
    fs::path chart_private_root;
    int chart_scroll=0;
    std::string heroine_config,object_config;
    std::vector<std::string> heroine_pictures;
    nnn::Word heroine_result=variable_count,object_result=variable_count,object_add_result=variable_count;
    nnn::Word object_type=variable_count,object_hover=variable_count;
    std::array<std::string,32> object_previews;
    int last_heroine=0;
    int next_modal_request=0;
    nnn::Value mode_before_modal=static_cast<nnn::Value>(nnn::RuntimeMode::Nothing);
    Wait wait_before_modal=Wait::Running;
    std::optional<nnn::RuntimeMode> modal_caller;
    struct SuspendedModal {
        ScriptModal request;
        nnn::Value mode_before;
        Wait wait_before;
        std::optional<nnn::RuntimeMode> caller;
    };
    std::optional<SuspendedModal> suspended_modal;
    std::array<nnn::Word,256> menu_extension_vars{};
    struct Place {
        nnn::Word appear=variable_count,selectable=variable_count,character=variable_count;
    };
    struct PlaceConfig {
        std::optional<nnn::Word> command;
        bool available=false;
        std::vector<Place> choices;
        nnn::Word result=variable_count,hover=variable_count;
        bool test_appear=false,test_select=false,has_character=false,test_character=false;
        ModalMusic music;
        std::array<std::string,32> previews;
    } select_place,select_place2;
    unsigned mode_id=30, x_id=40, y_id=41, trigger_id=42, click_id=45;
    unsigned random_id=variable_count,date_id=variable_count,year_id=variable_count;
    unsigned week_id=variable_count,holiday_id=variable_count;
    bool initial_calendar=true;
    unsigned direct3d_id=variable_count,pixel_shader_id=variable_count,vertex_shader_id=variable_count;
    unsigned cg_percent_id=variable_count,scene_percent_id=variable_count;
    std::array<unsigned,8> clock_ids{};
    std::vector<std::array<int,3>> holidays;
    std::function<ScriptClockSample()> clock_source;
    nnn::resource::FilenameEncoder filename_encoder;
    std::optional<ScriptClockSample> sampled_clock;
    std::uint32_t random_state=1,legacy_random_state=1;
    bool seed_from_clock=false;
    std::optional<std::vector<std::size_t>> cg_counts;
    std::optional<std::vector<std::vector<nnn::Word>>> scene_requirements;
    std::optional<std::uint64_t> cg_revision,scene_revision;
    int cg_percent=0,scene_percent=0;

    Impl(const fs::path& root,const SessionOptions& options,bool decorative=false,std::shared_ptr<GameAssets> peer={})
        : shared_runtime(options.runtime ? options.runtime : std::make_shared<nnn::RuntimeContext>(root)),
          shared_assets(peer ? std::move(peer) : std::make_shared<GameAssets>(root,options.cache_bytes,options.filename_encoder)),
          assets(*shared_assets),playback(root,options.audio,options.filename_encoder),
          queue_audio_events(options.queue_audio_events),decorative_preview(decorative),replay_films(options.replay_films),
          hidden_layers(options.replay_hidden_layers),clock_source(options.local_clock),filename_encoder(options.filename_encoder) {
        const bool legacy_volume_defaults=shared_runtime->legacy_volume_defaults();
        behavior=shared_runtime->with_parameters([&](nnn::RuntimeParameters& parameters) {
            return std::make_unique<nnn::PlayerBehavior>(parameters,root,legacy_volume_defaults);
        });
        apply_parameters();
        auto init=nnn::game_config::read(root,"init","game.xtx");
        if (!decorative_preview) {
            auto print_config=nnn::game_config::read(root,"init","printmessage.xtx");
            draw_window_with_message=config_int(print_config,"drawWindowWithMessageFlag",0)!=0;
            overlap_message_window=config_int(init,"noMessageWindowFlag",0)==0;
            if (overlap_message_window)
                overlap_window_config=nnn::game_config::read(root,"init","messageWindow.xtx");
            long_background_mode=std::max(0,config_int(print_config,"LPrintBGMode",0));
            if (long_background_mode) long_background_config=std::move(print_config);
        }
        const int seed=config_int(nnn::game_config::read(root,"init","mainControl.xtx"),"initRandomSeed",-1);
        legacy_random_state=options.random_seed.value_or(seed>0 ? static_cast<std::uint32_t>(seed) : 1U);
        seed_from_clock=!options.random_seed && seed==-1;
        random_state=seed_from_clock ? static_cast<std::uint32_t>(std::time(nullptr)) : legacy_random_state;
        pc_save_header=config_text(init,"saveHeaderName","SAVEDATA").substr(0,15);
        const auto version_part=[&](const char* key){return static_cast<unsigned>(std::clamp(config_int(init,key,0),0,255));};
        game_version=(version_part("highVersion")<<16U)|(version_part("middleVersion")<<8U)|version_part("lowVersion");
        pc_layers=config_int(init,"layerExpand",16)>=32 ? 32U : 16U;
        if (config_int(init,"facePrintMode",0)==0 && config_text(init,"autoExtDataMode1","").empty() &&
            config_text(init,"autoExtDataSub1","").empty()) replacement_extension=pc::first_extension_code;
        pc_export_layout=replacement_extension && config_int(init,"useCutinFlag",0)==0 && config_int(init,"useExtClass",0)==0;
        clear_auto_after_load=config_int(init,"clearAutoAfterLoad",0)!=0;
        pc_save_log=config_int(init,"saveLog",1)!=0;
        const auto log_config=nnn::game_config::read(root,"init","backlog.xtx");
        pc_log_voices=std::clamp(config_int(log_config,"voiceMultiCount",1),1,8);
        if (pc_log_voices>1) pc_log_layout=pc::LogLayout::MultiVoice;
        const auto data_file=nnn::game_config::read(root,"init","dataFile.xtx");
        pc_thumbnail_width=std::clamp(config_int(data_file,"picSizeX",config_int(data_file,"areaSizeX",132)),1,512);
        pc_thumbnail_height=std::clamp(config_int(data_file,"picSizeY",config_int(data_file,"areaSizeY",100)),1,512);
        one_byte_text=config_int(init,"codeByte",2)==1;
        sync_runtime_names();
        standalone_script=options.standalone_script;
        scene_list_enabled=config_int(init,"noSceneListFlag",0)==0;
        result_enabled=config_int(init,"resultFlag",0)!=0;
        auto_result_notice=config_int(init,"autoResultNotice",0);
        const auto answer_count=std::clamp(config_int(init,"answerStringNumber",0),0,4096);
        for (int i=1;i<=answer_count;++i)
            answer_strings.push_back(config_text(init,"answerString"+std::to_string(i),"").substr(0,126));
        for (const auto& line:lines_of(nnn::game_config::read(root,"spt","namecol.xtx"))) {
            std::istringstream input(line);
            std::vector<std::string> words;
            for (std::string token;input>>token;) words.push_back(std::move(token));
            if (words.size()<4 || words.front().front()=='@' || words.front().compare(0,2,"//")==0) continue;
            std::string name=words.front();
            for (std::size_t i=1;i+3<words.size();++i) name+=' '+words[i];
            speaker_names.insert(std::move(name));
        }
        jump_enabled=config_int(init,"jumpFlag",0)!=0;
        append_jump=config_int(init,"enableAppendJump",0)!=0;
        jump_voice_mask=static_cast<unsigned>(config_int(init,"playLoadVoiceFlag",0))&15U;
        jump_finish_fades=config_int(init,"enableJumpToFade",0)!=0;
        animation_enabled=config_int(init,"animeEffectEnable",0)!=0;
        const auto config=nnn::game_config::read(root,"init","config.xtx");
        animation_off=config_int(config,"expRadioNumber",0)>0 && config_int(config,"initAnimeSwitch",-1)>0;
        script_notices=config_int(init,"noticeFlag",0)!=0;
        const auto select_config=nnn::game_config::read(root,"init","selectmessage.xtx");
        choice_draws_scene=config_int(select_config,"backPrintMode",0)==2;
        choice_heading=config_text(select_config,"backlogMessage","[SELECT]");
        choice_heading_color=config_rgb(select_config,"backlogMessageColor",64,192,255);
        choice_color=config_rgb(select_config,"backlogSelectColor",192,232,255);
        choice_dot_color=config_rgb(select_config,"dotColor",128,128,128);
        choice_timeup=config_text(select_config,"timeupMessage","Time out");
        choice_timeup_color=config_rgb(select_config,"timeupColor",255,64,128);
        if (config_int(select_config,"firstAppearType",0)!=0)
            choice_appear_frames=static_cast<unsigned>(std::clamp(config_int(select_config,"firstAppearTime",10),0,1000));
        require_variant_graphic=config_int(init,"varControlLoadDWQErrorPrint",0)!=0;
        const int dots=std::clamp(config_int(select_config,"selectedDotNumber",3),0,8);
        for (int i=1;i<=dots;++i)
            choice_separator.push_back(config_text(select_config,"selectedDotMessage"+std::to_string(i),"                        ."));
        auto_message_kill_shift=config_int(init,"autoMessageCheckKillShift",0)!=0;
        cannot_skip_kill_shift=config_int(init,"cannotSkipCheckKillShift",0)!=0;
        background_unlocks=config_int(init,"backScriptSetCGOkFlag",0)!=0;
        skip_cg=config_int(init,"mustSetCGFlag",1)!=0;
        skip_film=config_int(init,"mustSetFilmFlag",1)!=0;
        skip_music=config_int(init,"mustSetMusicFlag",1)!=0;
        skip_voice_roles=config_int(init,"mustSetVoiceFlag",1)!=0;
        ignore_hidden_cg=config_int(init,"offLayerDontSetCG",0)!=0;
        auto setup=file_text(assets.config("exesetup.xtx"));
        const int width=options.width ? options.width : config_int(setup,"mainScreenWindowSizeX1",1280);
        const int height=options.height ? options.height : config_int(setup,"mainScreenWindowSizeY1",720);
        period=options.frame_ms ? options.frame_ms : static_cast<unsigned>(std::clamp(config_int(init,"frameTime",20),1,1000));
        default_period=period;
        renderer=make_renderer(width,height);
        auto varfile=assets.config("var2.fxf");
        const bool extended=!varfile.empty() && (config_int(init,"varType",1)!=0 || assets.config("var.fxf").empty());
        system_variable_end=extended ? 1200 : 300;
        if (!extended) varfile=assets.config("var.fxf");
        const auto names=fields(file_text(varfile));
        for (std::size_t i=0;i<names.size() && i<vars.size();++i)
            if (!names[i].empty()) ids.emplace(fold_cp932(names[i]),static_cast<unsigned>(i));
        configure_menu_extensions(init);
        compare_result_var=variable_id(config_text(init,"compareResultVarName",""));
        get_result_var=variable_id(config_text(init,"getResultVarName",""));
        if (config_int(init,"specialNumberForSaveSetFlag",0)) {
            constexpr auto default_slot_art_var="\x8e\x63\x82\xe8\x93\xfa\x90\x94"; // 残り日数 (CP932)
            save_slot_art_var=variable_id(config_text(init,"specialNumberForSaveVarName",default_slot_art_var));
        }
        load_variable_defaults(vars,&legacy_system_seed);
        for (const auto& group:{std::make_pair("expmode",nnn::NNNPARAM_EXPMODE),std::make_pair("expcheck",nnn::NNNPARAM_EXPCHECK)})
            for (nnn::Word i=0;i<5;++i) {
                const auto found=ids.find(group.first+std::to_string(i+1));
                if (found!=ids.end()) extended_variables.emplace(found->second,group.second+i);
            }
        auto bind=[&](const char* name,unsigned& id){const auto it=ids.find(name); if (it!=ids.end()) id=it->second;};
        bind("mode",mode_id); bind("mousex",x_id); bind("mousey",y_id); bind("trig1",trigger_id); bind("click1",click_id);
        bind("rnd",random_id); bind("date",date_id); bind("week",week_id); bind("holiday",holiday_id);
        if (config_int(init,"notUseYearVar",0)==0) bind("year",year_id);
        clock_ids.fill(variable_count);
        for (std::size_t i=0;i<clock_fields.size();++i) bind(clock_fields[i],clock_ids[i]);
        bind("direct3d",direct3d_id); bind("pixelshader",pixel_shader_id); bind("vertexshader",vertex_shader_id);
        bind("cgpercent",cg_percent_id); bind("scenepercent",scene_percent_id);
        if (holiday_id!=variable_count && config_int(init,"useSaijistu",0)!=0) {
            try {
                const auto entries=pc_list(nnn::game_config::read(root,"nya","saijitsu.xtx"));
                for (std::size_t i=0;i+2<entries.size();i+=3)
                    holidays.push_back({number(entries[i]),number(entries[i+1]),number(entries[i+2])});
            } catch (const std::exception& e) { warn(e.what()); }
        }
        for (unsigned i=0;i<layer_variables.size();++i) {
            const auto id=variable_id(config_text(init,"layerOffVar"+std::to_string(i),""));
            if (id<variable_count) layer_visibility[id]|=std::uint32_t{1}<<i;
        }
        const auto project=lines_of(file_text(assets.config("project.xtx")));
        for (std::size_t i=0;i<project.size();++i) {
            project_scripts.push_back(stem(project[i]));
            script_ids.emplace(project_scripts.back(),static_cast<unsigned>(i));
        }
        movie_count=std::min<std::size_t>(4096,fields(file_text(assets.config("movielist.xtx"))).size()/2);
        const auto commands=file_text(assets.config("command.xtx"));
        const auto command_id=[&](const std::string& name) -> std::optional<nnn::Word> {
            if (!config_int(init,name+"IsDefault",0) || !optional_class_created(init,name)) return {};
            std::size_t cursor=0;
            for (nnn::Word index=0;const auto token=nnn::game_config::name_token(commands,cursor);++index)
                if (fold_cp932(std::string(*token))==name) return index/2;
            return {};
        };
        if (movie_count) movie_command=command_id("printmovie");
        if (optional_class_created(init,"selectplace"))
            select_place=configure_places(command_id("selectplace"),nnn::game_config::read(root,"init","selectPlace.xtx"));
        if (optional_class_created(init,"selectplace2"))
            select_place2=configure_places(command_id("selectplace2"),nnn::game_config::read(root,"init","selectPlace2.xtx"));
        name_input_available=optional_class_created(init,"nameinput");
        kanji_input_available=optional_class_created(init,"kanjiinput");
        if (name_input_available) {
            if (const auto command=command_id("nameinput")) modal_commands.emplace(*command,nnn::RuntimeMode::NameInput);
            configure_name_input(nnn::game_config::read(root,"init","nameInput.xtx"));
        }
        if (kanji_input_available) {
            if (const auto command=command_id("kanjiinput")) modal_commands.emplace(*command,nnn::RuntimeMode::KanjiInput);
            configure_kanji_input(nnn::game_config::read(root,"init","kanjiInput.xtx"),
                nnn::game_config::read(root,"nya","softkey.xtx"));
        }
        configure_references(root,init);
        configure_selection_modes(root,init);
        term_available=optional_class_created(init,"printterm");
        voice_available=optional_class_created(init,"listenvoice");
        achievement_available=optional_class_created(init,"printachievement");
        result_available=optional_class_created(init,"printresult");
        item_available=optional_class_created(init,"printitem");
        shop_available=optional_class_created(init,"shop");
        status_available={optional_class_created(init,"printstatus"),optional_class_created(init,"printparty")};
        chart_available=optional_class_created(init,"selectchart");
        for (const auto& [name,mode]:{
                std::make_pair("selecthint",nnn::RuntimeMode::SelectHint),
                std::make_pair("printhint",nnn::RuntimeMode::PrintHint),
                std::make_pair("selectzukan",nnn::RuntimeMode::SelectZukan),
                std::make_pair("printterm",nnn::RuntimeMode::PrintTerm),
                std::make_pair("listenvoice",nnn::RuntimeMode::ListenVoice),
                std::make_pair("printachievement",nnn::RuntimeMode::PrintAchievement),
                std::make_pair("printresult",nnn::RuntimeMode::MyPrintResult),
                std::make_pair("selectheroin",nnn::RuntimeMode::SelectHeroin),
                std::make_pair("selectobject",nnn::RuntimeMode::SelectObject),
                std::make_pair("printitem",nnn::RuntimeMode::PrintItem),
                std::make_pair("shop",nnn::RuntimeMode::Shop),
                std::make_pair("printstatus",nnn::RuntimeMode::PrintStatus),
                std::make_pair("printparty",nnn::RuntimeMode::PrintParty),
                std::make_pair("selectchart",nnn::RuntimeMode::SelectChart),
                std::make_pair("printcalendar",nnn::RuntimeMode::PrintCalendar),
                std::make_pair("cardsystem",nnn::RuntimeMode::CardSystem)}) {
            if (const auto command=command_id(name)) modal_commands.emplace(*command,mode);
        }
        const auto movie_config=nnn::game_config::read(root,"init","printMovie.xtx");
        const int opening=config_int(movie_config,"mustOpeningMovieFlagOn",0);
        if (!decorative_preview && opening>0 && static_cast<std::size_t>(opening)<=movie_count)
            shared_runtime->unlock_gallery(nnn::GalleryKind::Movie,0,static_cast<nnn::Word>(opening-1));
        auto funcfile=assets.config("function.xtx");
        if (!funcfile.empty()) {
            const auto text=file_text(funcfile);
            auto cfg=nnn::parse_function_registry(std::vector<nnn::Byte>(text.begin(),text.end()));
            if (!cfg.status) throw std::runtime_error(cfg.status.detail);
            registry=std::move(cfg.registry);
        }
        if (zukan) {
            for (nnn::Word i=0;i<registry.Size();++i) {
                std::string name;
                if (registry.GetName(i,name) && fold_cp932(name)=="zukanon") {
                    zukan_function=i;
                    break;
                }
            }
        }
        vm=make_scheduler();
        const auto diagnostic=shared_runtime->defaults_diagnostic();
        if (!diagnostic.empty()) warn(diagnostic);
        if (decorative_preview) {
            ui_initial_vars=std::make_unique<const decltype(vars)>(vars);
            ui_mode=static_cast<nnn::Value>(nnn::RuntimeMode::Nothing);
            return;
        }
        const auto entry="spt/"+stem(options.entry)+".spt";
        const auto status=vm->LoadFile(entry);
        if (!status) throw std::runtime_error(entry+": "+status.detail);
        if (!replay_films.empty()) {
            if (replay_films.size()>4096U) throw std::invalid_argument("Too many replay films");
            // Validate every selected film before exposing the replay to the player.
            for (auto it=replay_films.rbegin();it!=replay_films.rend();++it) {
                const auto seek=vm->SeekFilm(*it);
                if (!seek) throw std::runtime_error(entry+": "+seek.detail);
            }
            renderer->hide_layers(hidden_layers);
        }
        frame.script=entry;
    }
    std::unique_ptr<nnn::ScriptScheduler> make_scheduler() {
        return std::make_unique<nnn::ScriptScheduler>(*this,[this](const std::string& path){return assets.script(path);},
                                                       registry.IsValid() ? &registry : nullptr);
    }
    void bind_ui_runtime(std::shared_ptr<nnn::RuntimeContext> runtime) {
        if (!runtime || runtime==shared_runtime) return;
        if (runtime->root().lexically_normal()!=shared_runtime->root().lexically_normal())
            throw std::invalid_argument("UI runtime directory differs from the game directory");
        // PlayerBehavior keeps a reference to its owning RuntimeParameters.
        runtime->with_parameters([&](nnn::RuntimeParameters& parameters) { behavior->rebind_parameters(parameters); });
        shared_runtime=std::move(runtime);
        cg_revision.reset(); scene_revision.reset();
    }
    std::shared_ptr<const nnn::PreviewStoryState> preview_seed(const decltype(vars)& values,
            const std::vector<std::string>& text,const std::string& last,const std::string& first) const {
        auto seed=std::make_shared<nnn::PreviewStoryState>();
        seed->vars=values;
        std::fill(seed->vars.begin()+first_system_variable,seed->vars.begin()+system_variable_end,0);
        seed->replacements=text;
        seed->surname=last;seed->given_name=first;
        return seed;
    }
    void publish_preview_story(bool external_modal=false) {
        if (!preview_story_dirty || decorative_preview || (ui_mode && !external_modal) || !replay_films.empty()) return;
        shared_runtime->set_preview_story(preview_seed(vars,replacements,surname,given_name));
        preview_story_dirty=false;
    }
    void seed_ui_story() {
        if (const auto seed=shared_runtime->preview_story()) {
            vars=seed->vars;
            replacements=seed->replacements;
            surname=seed->surname;given_name=seed->given_name;
            initial_calendar=false;
        } else {
            vars=*ui_initial_vars;
            replacements.clear();
            sync_runtime_names();
            initial_calendar=true;
        }
    }
    void apply_parameters() {
        if (decorative_preview) return;
        std::lock_guard<std::mutex> lock(parameter_output_mutex);
        // Take the revision before the snapshot: a concurrent later update is
        // picked up again on the next frame, never marked applied prematurely.
        const auto revision=shared_runtime->parameter_revision();
        if (revision==applied_parameter_revision) return;
        auto parameters=shared_runtime->snapshot_effective_parameters();
        if (!shared_runtime->routing().separate_script_se) {
            parameters.set(nnn::NNNPARAM_SCRIPTSESWITCH,parameters.get(nnn::NNNPARAM_SOUNDSWITCH));
            parameters.set(nnn::NNNPARAM_SCRIPTSEVOLUME,parameters.get(nnn::NNNPARAM_SOUNDVOLUME));
        }
        playback.apply_runtime_parameters(parameters);
        playback.set_voice_off(shared_runtime->voice_off());
        const auto theme=shared_runtime->system_voice_selection();
        if (theme!=applied_system_voice) {
            if (theme>=0 && static_cast<unsigned>(theme)<playback.system_voice_selection_count()) {
                playback.set_system_voice_selection(static_cast<unsigned>(theme));
                applied_system_voice=theme;
            } else warn("Unavailable system voice selection: "+std::to_string(theme));
        }
        applied_parameter_revision=revision;
    }
    void begin_line(bool has_voice,std::uint32_t voice_ms,int speed,bool append_text=false) {
        shared_runtime->with_parameters([&](nnn::RuntimeParameters&) {
            behavior->begin_line(has_voice,voice_ms,speed,append_text);
        });
    }
    void set_mode(nnn::RuntimeMode mode) {
        if (!ui_mode) shared_runtime->set_mode(mode);
    }
    bool skip_blocked() const { return auto_message>0 || cannot_skip>0; }
    bool click_blocked() const { return auto_message>0 || cannot_click>0; }
    void sync_skip() {
        auto flags=GetSkipFlags();
        const bool raw=!ui_mode && shared_runtime->skip_active();
        if (!raw) effect_gate=false;
        const bool active=raw && !skip_blocked();
        flags.skip_next_command=active;
        flags.skip_effect_command=active && !effect_gate;
        SetSkipFlags(flags);
    }
    void mark_current_read() {
        if (!ui_mode && frame.wait==Wait::Dialogue && !shared_runtime->mark_read(frame.dialogue.message_id))
            warn("Read-history capacity reached; existing read records are retained");
    }
    void warn(const std::string& message) {
        if (reported.insert(message).second) {
            messages.push_back(message);
            std::clog<<"SystemNNN: "<<message<<'\n';
        }
    }
    void location() {
        const auto* active=vm->GetVM(vm->ActiveMode());
        frame.script=active->Identity(); frame.word=active->GetPC();
    }
    nnn::Word identity(nnn::Value value) {
        location();
        const auto file=script_ids.find(stem(frame.script));
        const unsigned serial=static_cast<unsigned>(value)&0xffffU;
        return file==script_ids.end() ? serial : ((file->second&0xffffU)<<16U)|serial;
    }
    std::shared_ptr<const render::Texture> texture(const std::string& name) {
        if (name.empty()) { warn("Empty graphic name"); return {}; }
        try { return assets.image(name); }
        catch (const std::exception& e) { warn("Graphic "+name+": "+e.what()); return {}; }
    }
    std::shared_ptr<const render::Texture> message_art(const std::string& name) {
        return texture(name.find_first_of("/\\")==std::string::npos ? "sys/"+name : name);
    }
    void paint_overlap_window(render::Surface& out) {
        const auto& config=overlap_window_config;
        const int configs=std::clamp(config_int(config,"windowNumberConfig",1),1,16);
        const int scripts=std::clamp(config_int(config,"windowNumberScript",1),1,16);
        const int fonts=std::clamp(config_int(config,"windowNumberFontSizeType",1),1,256/(configs*scripts));
        const int variant=std::clamp(shared_runtime->message_window(),0,configs-1)+1;
        const int script=std::clamp(window_type,0,scripts-1)+1;
        const int font=std::clamp(font_type,0,fonts-1);
        std::string key="filenameWindow";
        if (scripts>1 && configs>1) key+=std::to_string(script)+"_"+std::to_string(variant);
        else key+=std::to_string(scripts>1?script:variant);
        if (font) key+="_Font"+std::to_string(font);
        const auto art=message_art(config_text(config,key,config_text(config,"filenameWindow","ta_meswin")));
        if (!art) return;
        const render::Point origin{std::clamp(config_int(config,"printX",0),-1000000,1000000),
            std::clamp(config_int(config,"printY",300),-1000000,1000000)};
        const int width=std::clamp(config_int(config,"sizeX",640),0,static_cast<int>(render::max_dimension));
        const int height=std::clamp(config_int(config,"sizeY",180),0,static_cast<int>(render::max_dimension));
        const int layers=config_int(config,"notTransWindowUse",0);
        const int opacity=100-std::clamp(shared_runtime->get_system_parameter(nnn::NNNPARAM_WINDOWPERCENT),0,100);
        if (layers&2) render::paint_texture(out,*art,{0,height*((layers&1)?2:1),width,height},origin);
        render::paint_texture(out,*art,{0,0,width,height},origin,opacity);
        if (layers&1) render::paint_texture(out,*art,{0,height,width,height},origin);
    }
    void paint_long_background(render::Surface& out) {
        const auto& config=long_background_config;
        const auto parameter=[&](const std::string& key,int fallback) { return config_int(config,"lprintBG"+key,fallback); };
        if (long_background_mode&2) {
            const int width=static_cast<int>(out.width),height=static_cast<int>(out.height);
            const render::Rect clip{parameter("ClipX",0),parameter("ClipY",0),
                parameter("ClipSizeX",width),parameter("ClipSizeY",height)};
            const auto apply=[&](const std::string& type,auto edit) {
                const int x=parameter(type+"ClipX",clip.x),y=parameter(type+"ClipY",clip.y);
                const int w=parameter(type+"ClipSizeX",clip.width),h=parameter(type+"ClipSizeY",clip.height);
                const int right=static_cast<int>(std::clamp<std::int64_t>(std::int64_t{x}+w,0,width));
                const int bottom=static_cast<int>(std::clamp<std::int64_t>(std::int64_t{y}+h,0,height));
                for (int row=std::max(0,y);row<bottom;++row)
                    for (int col=std::max(0,x);col<right;++col)
                        edit(out.rgb.data()+(static_cast<std::size_t>(row)*out.width+static_cast<unsigned>(col))*3);
            };
            if (parameter("ScreenEffectNega",0))
                apply("Nega",[](render::Byte* rgb) { for (unsigned c=0;c<3;++c) rgb[c]=255-rgb[c]; });
            if (parameter("ScreenEffectGrey",0))
                apply("Grey",[](render::Byte* rgb) {
                    const auto grey=static_cast<render::Byte>((76*rgb[0]+150*rgb[1]+29*rgb[2])>>8);
                    std::fill_n(rgb,3,grey);
                });
            const auto channels=[&](const std::string& tag,int fallback,int inherit) {
                const int r=parameter(tag+"R",fallback),g=parameter(tag+"G",r),b=parameter(tag+"B",g);
                const int green=g==inherit ? r : g;
                return std::array<int,3>{r,green,b==inherit ? green : b};
            };
            const auto multi=channels("Multi",100,-1);
            if (multi!=std::array<int,3>{100,100,100})
                apply("Multi",[&](render::Byte* rgb) {
                    for (unsigned c=0;c<3;++c)
                        rgb[c]=static_cast<render::Byte>((rgb[c]*(std::clamp(multi[c],0,100)*256/100))>>8);
                });
            const auto add=channels("Delta",0,-9999);
            if (add!=std::array<int,3>{})
                apply("AddSub",[&](render::Byte* rgb) {
                    for (unsigned c=0;c<3;++c)
                        rgb[c]=static_cast<render::Byte>(std::clamp(rgb[c]+std::clamp(add[c],-255,255),0,255));
                });
        }
        if (long_background_mode&1) {
            const auto art=message_art(config_text(config,"lprintBGFileName","ta_fullMessageWindow"));
            if (art) render::paint_texture(out,*art,{0,0,art->canvas_width,art->canvas_height},
                {std::clamp(parameter("PrintX",0),-1000000,1000000),std::clamp(parameter("PrintY",0),-1000000,1000000)});
        }
    }
    render::Layers::OverlapDecoration overlap_decoration() {
        if (ui_mode) return {};
        if (next_message_type==0 && overlap_message_window && frame.dialogue.window_visible)
            return [this](render::Surface& out) { paint_overlap_window(out); };
        if ((next_message_type==1 || next_message_type==2) && long_background_mode)
            return [this](render::Surface& out) { paint_long_background(out); };
        return {};
    }
    std::unique_ptr<render::Layers> make_renderer(int width,int height) {
        auto layers=std::make_unique<render::Layers>(width,height);
        layers->animation_textures([this](const std::string& name) -> std::shared_ptr<const render::Texture> {
            if (!animation_enabled && name.compare(0,10,"bg/bganime")==0) return {};
            return texture(name);
        });
        layers->set_animation_off(animation_off);
        return layers;
    }
    bool slot(int index) {
        if (index>=0 && index<32) return true;
        warn("Layer outside 0..31: "+std::to_string(index));
        return false;
    }
    void cg(const std::vector<nnn::Value>& a) {
        if (a.size()<2 || a[0]<=0 || a[1]<=0) return;
        if ((ui_mode && !background_unlocks) || (GetSkipFlags().skip_next_command && !skip_cg)) return;
        if (ignore_hidden_cg && a.size()>2 && a[2]>=0 && a[2]<32 &&
            (renderer->hidden_layers()&(std::uint32_t{1}<<a[2]))) return;
        if (shared_runtime->unlock_gallery(nnn::GalleryKind::Cg,static_cast<nnn::Word>(a[0]-1),
                                           static_cast<nnn::Word>(a[1]-1))) ++counters.cg_unlocks;
    }
    void film(const std::vector<nnn::Value>& a) {
        if (a.empty() || a[0]<=0 || (ui_mode && !background_unlocks) ||
            (GetSkipFlags().skip_next_command && !skip_film)) return;
        location();
        const auto script=script_ids.find(stem(frame.script));
        if (script!=script_ids.end()) shared_runtime->unlock_gallery(nnn::GalleryKind::Film,
            script->second,static_cast<nnn::Word>(a[0]-1));
    }
    void remember(BacklogEntry entry) {
        if (entry.text.size()>16384U) return;
        if (backlog.size()>=backlog_capacity) {
            forget_jump(backlog.front().jump_id);
            backlog.pop_front();
        }
        backlog.push_back(std::move(entry));
    }
    auto find_jump(std::uint64_t id) const {
        return std::find_if(jumps.cbegin(),jumps.cend(),[id](const JumpPoint& point){return point.line.jump_id==id;});
    }
    void forget_jump(std::uint64_t id) {
        if (!id) return;
        const auto point=find_jump(id);
        if (point==jumps.cend()) return;
        jump_bytes-=point->state.size();
        jumps.erase(point);
    }
    void capture_jump(unsigned cursor) {
        if (!jump_pending || frame.wait!=Wait::Dialogue) return;
        jump_pending=false;
        auto& line=backlog.back();
        const auto id=line.jump_id ? line.jump_id : next_jump_id++;
        forget_jump(line.jump_id);
        line.jump_id=0;
        if (!cursor) return;
        nnn::snapshot::Writer out;
        // The visible log survives the rewind in place. Checkpoints never
        // include earlier checkpoint data or copies of the whole log.
        save_snapshot(out,false);
        if (out.bytes.size()>backlog_jump_budget) return;
        auto saved=line;
        saved.jump_id=id;
        jumps.push_back({std::move(saved),cursor,backlog.size(),random_state,input.mouse_x,input.mouse_y,std::move(out.bytes)});
        line.jump_id=id;
        jump_bytes+=jumps.back().state.size();
        while (jump_bytes>backlog_jump_budget) {
            const auto evicted=jumps.front().line.jump_id;
            for (auto& row:backlog) if (row.jump_id==evicted) row.jump_id=0;
            forget_jump(evicted);
        }
    }
    BacklogJump jump_info(std::uint64_t id) const {
        if (!id || ui_mode || !replay_films.empty()) return {};
        const auto point=find_jump(id);
        if (point==jumps.cend()) return {};
        const auto line=std::find_if(backlog.cbegin(),backlog.cend(),[id](const BacklogEntry& entry){return entry.jump_id==id;});
        if (line==backlog.cend()) return {};
        const auto keep=static_cast<unsigned>(std::distance(backlog.cbegin(),line)+1);
        return {point->cursor,keep<point->backlog_size ? keep : 0U,point->random_state,point->mouse_x,point->mouse_y};
    }
    bool restore_jump(std::uint64_t id,unsigned cursor) {
        if (!cursor || jump_info(id).journal_cursor!=cursor) return false;
        const auto point=find_jump(id);
        const auto line=std::find_if(backlog.cbegin(),backlog.cend(),[id](const BacklogEntry& entry){return entry.jump_id==id;});
        std::deque<BacklogEntry> prefix(backlog.cbegin(),line);
        prefix.push_back(point->line);
        nnn::snapshot::Reader in(point->state);
        load_snapshot(in,{},true,true);
        backlog.swap(prefix);
        while (jumps.back().line.jump_id!=id) {
            jump_bytes-=jumps.back().state.size();
            jumps.pop_back();
        }
        jump_pending=false;
        return true;
    }
    void trim_backlog(unsigned keep) {
        while (backlog.size()>keep) {
            forget_jump(backlog.front().jump_id);
            backlog.pop_front();
        }
    }
    void clear_jumps() {
        jumps.clear();
        jump_bytes=0;
        jump_pending=false;
    }
    void remember_choice(const nnn::ScriptSelection& choice) {
        if (ui_mode) return;
        remember({{}," ",{},choice_heading_color});
        remember({{},choice_heading,{},choice_heading_color,true});
        for (const auto& text:choice.messages) remember({{},text,{},0xffffff});
        for (const auto& text:choice.options) remember({{},text,{},0xffffff});
    }
    unsigned choice_limit(nnn::Value limit,const std::array<nnn::Value,variable_count>& values) const {
        if (limit>=0) return static_cast<unsigned>(limit);
        const auto index=static_cast<std::uint64_t>(-static_cast<std::int64_t>(limit));
        if (index>=values.size()) return 0;
        const auto id=static_cast<unsigned>(index);
        const auto value=system_variable(id) ? shared_runtime->system_variable(id-first_system_variable) : values[id];
        return static_cast<unsigned>(std::max(0,value));
    }
    void confirm_choice(const nnn::ScriptSelection& choice,unsigned option,bool timeout) {
        if (ui_mode) return;
        if (option>0 && option<=choice.options.size()) {
            const auto from_end=choice.options.size()-option+1;
            if (from_end<=backlog.size()) backlog[backlog.size()-from_end].color=choice_color;
        }
        // A timed selection can enter a hidden CASE with no Backlog row to color.
        if (option>0 && choice.serial>=0 && choice.serial<1024)
            shared_runtime->unlock_gallery(nnn::GalleryKind::Choice,static_cast<nnn::Word>(choice.serial),(option-1)&31U);
        if (timeout) remember({{},choice_timeup,{},choice_timeup_color,true});
        for (const auto& separator:choice_separator) remember({{},separator,{},choice_dot_color,true});
    }
    void set_period(unsigned value) {
        period=value;
        remainder%=period;
    }
    std::string speaker(const std::string& text) const {
        const auto end=text.find_first_of("\r\n");
        if (end!=std::string::npos && speaker_names.find(text.substr(0,end))!=speaker_names.end()) return text.substr(0,end);
        return speaking(text);
    }
    std::string replace_dialogue(const std::string& raw) const {
        return nnn::expand_text_replacements(raw,one_byte_text,surname,given_name,
            [&](std::size_t index) { return local_replacement(index); },
            [&](std::size_t index) { return shared_runtime->system_replacement(index); });
    }
    std::optional<render::FragmentMesh> fragment_mesh(const std::vector<nnn::Value>& parameters) {
        if (parameters.size()<=14 || parameters[13]!=2) return {};
        const auto name="hahen"+std::to_string(parameters[14])+".xtx";
        try {
            const auto file=nnn::game_config::find(shared_runtime->root(),"nya",name);
            if (file.empty() || fs::file_size(file)>256U*1024U) throw std::runtime_error("missing or oversized mesh");
            auto text=file_text(file);
            std::replace(text.begin(),text.end(),',',' ');
            std::ostringstream tokens;
            for (const auto& line:lines_of(text)) {
                const auto at=line.find_first_not_of(" \t");
                if (at!=std::string::npos && line[at]!='@' && line.compare(at,2,"//")!=0) tokens<<line<<' ';
            }
            std::istringstream input(tokens.str());
            const auto read=[&](int minimum,int maximum) {
                std::string token;
                if (!(input>>token)) throw std::runtime_error("truncated mesh");
                int value=number(token,INT32_MIN);
                if (value==INT32_MIN && token!="-2147483648") throw std::runtime_error("invalid mesh integer");
                if (value<minimum || value>maximum) throw std::runtime_error("mesh field out of bounds");
                return value;
            };
            read(INT32_MIN,INT32_MAX); // Reserved dataFlag.
            const int vertex_count=read(3,8192);
            render::FragmentMesh mesh;
            mesh.vertices.reserve(static_cast<std::size_t>(vertex_count));
            for (int i=0;i<vertex_count;++i)
                mesh.vertices.push_back({read(0,static_cast<int>(renderer->width())-1),read(0,static_cast<int>(renderer->height())-1)});
            const int face_count=read(1,4096);
            mesh.faces.reserve(static_cast<std::size_t>(face_count));
            for (int i=0;i<face_count;++i) {
                const int corners=read(3,15);
                auto& face=mesh.faces.emplace_back();
                for (int j=0;j<corners;++j) face.push_back(read(0,vertex_count-1));
            }
            std::string tail;
            if (input>>tail) throw std::runtime_error("trailing mesh data");
            return mesh;
        } catch (const std::exception& error) {
            warn("HAHEN "+name+": "+error.what()+"; using the default mesh");
            return {};
        }
    }
    nnn::Word variable_id(const std::string& name) const {
        const auto it=ids.find(fold_cp932(name));
        return it==ids.end() ? variable_count : it->second;
    }
    void configure_menu_extensions(const std::string& init) {
        if (nnn::game_config::ascii_lower(init).find("expmodecheckbyvarnumber")==std::string::npos) return;
        try {
            const nnn::game_config::NameListSetup setup(init);
            const auto count=std::clamp(setup.integer("expModeCheckByVarNumber",0),0,4096);
            for (int i=1;i<=count;++i) {
                const auto suffix=std::to_string(i);
                const auto mode=setup.integer("expModeCheckByVarMode"+suffix,0);
                if (mode<=0 || static_cast<std::size_t>(mode)>=menu_extension_vars.size()) continue;
                if (const auto name=setup.field("expModeCheckByVarName"+suffix))
                    menu_extension_vars[static_cast<std::size_t>(mode)]=variable_id(std::string(*name));
            }
        } catch (const std::exception& e) {
            warn("System menu extension checks: "+std::string(e.what()));
        }
    }
    bool menu_extension_enabled(nnn::RuntimeMode mode) const {
        const auto index=static_cast<std::size_t>(mode);
        if (index==0 || index>=menu_extension_vars.size()) return false;
        if (mode==nnn::RuntimeMode::SelectPlace2 &&
            (!select_place2.available || select_place2.result>=variable_count ||
             shared_runtime->mode()==static_cast<nnn::Value>(mode))) return false;
        const auto id=menu_extension_vars[index];
        return id==0 || id>=variable_count || variable_value(id)!=0;
    }
    void sync_runtime_names() {
        if (shared_runtime->reset_name_on_title()) {
            auto defaults=shared_runtime->game_default_names();
            surname=std::move(defaults[0]);given_name=std::move(defaults[1]);
        } else {
            auto text=shared_runtime->runtime_text();
            surname=std::move(text.surname);given_name=std::move(text.given_name);
        }
        preview_story_dirty=true;
    }
    static ModalMusic modal_music(const std::string& config) {
        return {config_int(config,"bgmNumber",-1),config_int(config,"bgmLoop",0),config_int(config,"bgmVolume",0),
            config_int(config,"bgmFadeInTime",-1),config_int(config,"bgmRestartFlag",0)!=0};
    }
    void configure_name_input(const std::string& config) {
        const auto shared_limit=config_int(config,"max",4);
        for (std::size_t i=0;i<name_input.editable.size();++i) {
            const std::string prefix=i==0 ? "sei" : "mei";
            name_input.editable[i]=config_int(config,prefix+"Flag",1)!=0;
            name_input.fixed[i]=!name_input.editable[i] && config_int(config,prefix+"FixFlag",0)!=0;
            if (name_input.fixed[i]) {
                const auto fallback=i==0 ? "\x8e\x4f\x90\xe7\x89\x40" : "\x82\xe0\x82\xe5\x82\xe0\x82\xc6";
                name_input.fixed_text[i]=nnn::bound_runtime_text(config_text(config,prefix+"FixData",fallback),
                    nnn::runtime_name_bytes);
            }
            name_input.limits[i]=std::min(static_cast<int>(nnn::runtime_name_bytes),
                2*std::clamp(config_int(config,prefix+"Max",shared_limit),0,static_cast<int>(nnn::runtime_name_bytes)));
        }
        // The original retains field 0 in m_seimeiType when no field was enabled.
        if (!name_input.editable[0] && !name_input.editable[1]) name_input.editable[0]=true;
        name_input.status=variable_id(config_text(config,"statusVarName",""));
        name_input.time=variable_id(config_text(config,"timeVarName",""));
        name_input.game_default=config_int(config,"useDefaultName",0)!=0;
        name_input.allow_empty=config_int(config,"nullNameOk",0)!=0;
        name_input.cancel=config_int(config,"returnOk",1)!=0;
        name_input.music=modal_music(config);
    }
    void configure_kanji_input(const std::string& config,const std::string& softkey) {
        const auto fallback=config_int(softkey,"kakuteiTextMax",6);
        for (std::size_t i=0;i<kanji_input.limits.size();++i) {
            const auto override=config_int(config,"kakuteiTextMax"+std::to_string(i+1),0);
            const int storage=static_cast<int>(i<2 ? nnn::runtime_replacement_bytes : nnn::runtime_name_bytes);
            const int width=one_byte_text ? 1 : 2;
            kanji_input.limits[i]=width*std::clamp(override>0 ? override : fallback,0,storage/width);
        }
        kanji_input.music=modal_music(config);
    }
    static nnn::reference_selection::Setup reference_setup(const std::string& config) {
        auto fields=std::make_shared<std::unordered_map<std::string,std::string>>();
        std::size_t cursor=0;
        while (const auto key=nnn::game_config::name_token(config,cursor)) {
            const auto value=nnn::game_config::name_token(config,cursor);
            if (!value) break;
            fields->emplace(fold_cp932(std::string(*key)),*value);
        }
        return {
            [fields](const std::string& key,int fallback) {
                const auto it=fields->find(fold_cp932(key));
                if (it==fields->end()) return fallback;
                std::istringstream value(it->second);
                int number=0;
                return value>>number ? number : 0;
            },
            [fields](const std::string& key,const std::string& fallback) {
                const auto it=fields->find(fold_cp932(key));
                return it==fields->end() ? fallback : it->second;
            }
        };
    }
    std::string read_optional_setup(const fs::path& root,const std::string& name) {
        try { return nnn::game_config::read(root,"init",name,filename_encoder); }
        catch (const std::exception& e) { warn(name+": "+e.what());return {}; }
    }
    void configure_references(const fs::path& root,const std::string& init) {
        const auto select_config=optional_class_created(init,"selecthint") ? read_optional_setup(root,"selectHint.xtx") : "";
        const auto print_config=optional_class_created(init,"printhint") ? read_optional_setup(root,"printHint.xtx") : "";
        select_hint_available=!select_config.empty();
        print_hint_available=!print_config.empty();
        if (select_hint_available || print_hint_available) {
            hints.emplace(reference_setup(select_config),reference_setup(print_config));
            select_hint_music=modal_music(select_config);
            print_hint_music=modal_music(print_config);
        }
        if (optional_class_created(init,"selectzukan")) {
            const auto config=read_optional_setup(root,"selectZukan.xtx");
            zukan_available=!config.empty();
            zukan.emplace(reference_setup(config));
            zukan_music=modal_music(config);
        }
    }
    void configure_selection_modes(const fs::path& root,const std::string& init) {
        if (optional_class_created(init,"selectheroin")) {
            heroine_config=read_optional_setup(root,"selectHeroin.xtx");
            heroine_result=variable_id(config_text(heroine_config,"returnVarName",""));
            if (!heroine_config.empty()) {
                try {
                    const auto list=nnn::game_config::read(root,"nya","heroin.xtx",filename_encoder);
                    std::size_t cursor=0;
                    while (heroine_pictures.size()<4096) {
                        const auto picture=nnn::game_config::bounded_name_token(list,cursor);
                        if (!picture) break;
                        heroine_pictures.emplace_back(*picture);
                    }
                } catch (const std::exception& e) { warn("SELECTHEROIN portrait list: "+std::string(e.what())); }
            }
        }
        if (optional_class_created(init,"selectobject")) {
            object_config=read_optional_setup(root,"selectObject.xtx");
            object_type=variable_id(config_text(object_config,"objectTypeVarName",""));
            object_hover=variable_id(config_text(object_config,"onButtonVarName",""));
            object_result=variable_id(config_text(object_config,"resultVarName",
                "\x94\xc4\x97\x70\x91\x49\x91\xf0\x94\xd4\x8d\x86"));
            object_add_result=variable_id(config_text(object_config,"resultAddVarName",
                "\x94\xc4\x97\x70\x92\xc7\x89\xc1\x91\x49\x91\xf0\x94\xd4\x8d\x86"));
            if (config_int(object_config,"autoPicLoad",0)!=0)
                for (unsigned i=0;i<object_previews.size();++i)
                    object_previews[i]=config_text(object_config,"autoPicLayerTag"+std::to_string(i),"");
        }
    }
    nnn::reference_selection::Read reference_read() {
        return [this](const std::string& name) { return GetVarData(variable_id(name)); };
    }
    nnn::reference_selection::Write reference_write() {
        return [this](const std::string& name,nnn::Value value) { SetVarData(variable_id(name),value); };
    }
    static void set_modal_page(ScriptModal& request,nnn::reference_selection::Page page) {
        std::vector<ScriptModalRow> rows;
        rows.reserve(page.rows.size());
        for (auto& row:page.rows)
            rows.push_back({row.id,row.flags,std::move(row.label),std::move(row.asset),std::move(row.detail)});
        request.page=page.page;request.pages=page.pages;
        request.group=page.group;request.selection=page.selection;
        request.rows=std::move(rows);
    }
    bool hint_picture_present(const std::string& name) {
        auto key=nnn::resource::normalize_cp932_path(name);
        if (key.empty()) return false;
        if (key.find('/')==std::string::npos) key="sys/"+key;
        const auto found=hint_picture_presence.find(key);
        if (found!=hint_picture_presence.end()) return found->second;
        if (hint_picture_presence.size()>=4096) return false;
        const bool exists=assets.has_image(key);
        hint_picture_presence.emplace(std::move(key),exists);
        return exists;
    }
    std::optional<nnn::reference_selection::Page> first_hint_page(int group) {
        const auto read=reference_read();
        auto first=hints->pictures(read,group,0);
        const auto has_picture=[&](const nnn::reference_selection::Page& page) {
            return std::any_of(page.rows.begin(),page.rows.end(),[&](const auto& row){return hint_picture_present(row.asset);});
        };
        if (has_picture(first)) return first;
        for (int page=1;page<first.pages;++page) {
            auto candidate=hints->pictures(read,group,page);
            if (has_picture(candidate)) return candidate;
        }
        return {};
    }
    std::optional<nnn::reference_selection::Page> select_hint_page() {
        if (!select_hint_available || !print_hint_available) return {};
        auto page=hints->groups(reference_read());
        page.rows.erase(std::remove_if(page.rows.begin(),page.rows.end(),[&](const auto& row){
            return !first_hint_page(row.id);
        }),page.rows.end());
        if (page.rows.empty()) return {};
        return page;
    }
    int new_modal_request() {
        if (next_modal_request==std::numeric_limits<int>::max()) next_modal_request=0;
        return ++next_modal_request;
    }
    void play_modal_music(const ModalMusic& music) {
        const auto error=playback.play_menu_music(music.number,music.plays,music.volume,music.fade,music.restart);
        if (!error.empty()) warn(error);
    }
    void enter_modal(ScriptModal request,const ModalMusic& music,std::optional<nnn::RuntimeMode> caller) {
        if (!caller) location();
        if (frame.wait==Wait::Modal) {
            suspended_modal=SuspendedModal{std::move(*frame.modal),mode_before_modal,wait_before_modal,modal_caller};
            request.return_mode=suspended_modal->request.mode;
        }
        mode_before_modal=shared_runtime->mode();
        wait_before_modal=frame.wait;
        modal_caller=caller;
        if (request.mode!=nnn::RuntimeMode::SelectChart) shared_runtime->set_skip_active(false);
        ClearSkipFlags();
        play_modal_music(music);
        request.request=new_modal_request();
        set_mode(request.mode);
        frame.modal=std::move(request);
        frame.wait=Wait::Modal;
    }
    bool can_nest_modal(nnn::RuntimeMode mode,nnn::RuntimeMode caller) const {
        if (suspended_modal || !frame.modal || caller!=frame.modal->mode || mode==caller) return false;
        if (caller==nnn::RuntimeMode::SelectChart)
            return chart_visit && chart_visit->pending<0 && chart_visit->staged<0;
        return caller==nnn::RuntimeMode::SelectHeroin || caller==nnn::RuntimeMode::SelectObject;
    }
    bool begin_modal(nnn::RuntimeMode mode,const std::vector<nnn::Value>& args,
                     std::optional<nnn::RuntimeMode> back_mode={},bool from_title=false) {
        const bool nested=frame.wait==Wait::Modal;
        if (nested && (from_title || !back_mode || !can_nest_modal(mode,*back_mode))) return false;
        const auto previous=nested ? frame.modal->request : 0;
        const bool fixed_return=!back_mode || from_title;
        ScriptModal request;
        request.mode=mode;
        request.return_mode=back_mode;
        request.arguments=args;
        if (mode==nnn::RuntimeMode::NameInput && name_input_available) {
            auto fallback=name_input.game_default ? shared_runtime->game_default_names() : shared_runtime->initial_names();
            auto start=fallback;
            if (!name_input.game_default) {
                auto global=shared_runtime->runtime_text();
                start={std::move(global.surname),std::move(global.given_name)};
            }
            for (std::size_t i=0;i<start.size();++i) {
                if (name_input.fixed[i]) start[i]=name_input.fixed_text[i];
                if (name_input.editable[i])
                    start[i]=nnn::bound_runtime_text(start[i],static_cast<std::size_t>(name_input.limits[i]));
            }
            request.text={std::move(start[0]),std::move(start[1]),std::move(fallback[0]),std::move(fallback[1])};
            request.byte_limits.assign(name_input.limits.begin(),name_input.limits.end());
            enter_modal(std::move(request),name_input.music,back_mode);
        } else if (mode==nnn::RuntimeMode::KanjiInput && kanji_input_available) {
            const auto target=args.size()>1 ? args[1] : 0;
            if (target<0 || static_cast<std::size_t>(target)>=kanji_input.limits.size()) {
                warn("Invalid KANJIINPUT target type "+std::to_string(target));
                return false;
            }
            request.text={""};
            request.byte_limits={kanji_input.limits[static_cast<std::size_t>(target)]};
            if (request.byte_limits.front()==0) {
                warn("KANJIINPUT has no usable text capacity");
                return false;
            }
            enter_modal(std::move(request),kanji_input.music,back_mode);
        } else if (mode==nnn::RuntimeMode::SelectHint && select_hint_available) {
            if (auto page=select_hint_page()) {
                set_modal_page(request,std::move(*page));
                enter_modal(std::move(request),select_hint_music,back_mode);
            } else warn("SELECTHINT has no available hint groups");
        } else if (mode==nnn::RuntimeMode::PrintHint && print_hint_available) {
            if (auto page=first_hint_page(hint_group)) {
                set_modal_page(request,std::move(*page));
                enter_modal(std::move(request),print_hint_music,back_mode);
            } else warn("PRINTHINT group has no usable pictures: "+std::to_string(hint_group));
        } else if (mode==nnn::RuntimeMode::SelectZukan && zukan_available) {
            zukan->clear(reference_write());
            set_modal_page(request,zukan->grid(reference_read(),0));
            enter_modal(std::move(request),zukan_music,back_mode);
        } else if (mode==nnn::RuntimeMode::SelectHeroin) {
            if (setup_heroine(request,last_heroine,live_selection_variables()))
                enter_modal(std::move(request),modal_music(heroine_config),back_mode);
        } else if (mode==nnn::RuntimeMode::SelectObject) {
            if (!args.empty()) SetVarData(object_type,args.front());
            if (setup_objects(request,live_selection_variables())) {
                SetVarData(object_hover,0);
                enter_modal(std::move(request),modal_music(object_config),back_mode);
            }
        } else if (mode==nnn::RuntimeMode::PrintTerm && term_available && prepare_terms()) {
            set_term_page(request,term_group,0);
            enter_modal(std::move(request),term_music,back_mode);
        } else if (mode==nnn::RuntimeMode::ListenVoice && voice_available && prepare_voices()) {
            if (fixed_return) request.return_mode=nnn::RuntimeMode::Omake;
            set_voice_page(request,0,0);
            enter_modal(std::move(request),voice_music,back_mode);
        } else if (mode==nnn::RuntimeMode::PrintAchievement && prepare_achievements()) {
            if (fixed_return) request.return_mode=nnn::RuntimeMode::Omake;
            for (const auto& entry:achievement_catalog->entries(*shared_runtime))
                request.rows.push_back({entry.id+1,entry.completed ? ScriptModalRow::Selected : 0,
                    {},{},std::to_string(entry.percent)});
            enter_modal(std::move(request),achievement_music,back_mode);
        } else if (mode==nnn::RuntimeMode::MyPrintResult && prepare_results()) {
            if (fixed_return) request.return_mode=result_catalog->next_mode();
            result_visit=result_catalog->begin(*shared_runtime);
            set_result_page(request);
            enter_modal(std::move(request),result_music,back_mode);
        } else if (mode==nnn::RuntimeMode::PrintItem && prepare_inventory(item_catalog,item_available,item_music,"printItem.xtx")) {
            set_inventory_page(request,0,0);
            enter_modal(std::move(request),item_music,back_mode);
        } else if (mode==nnn::RuntimeMode::Shop && prepare_inventory(shop_catalog,shop_available,shop_music,"shop.xtx")) {
            shop_visit=shop_catalog->begin(modal_variables());
            set_inventory_page(request,0,0);
            enter_modal(std::move(request),shop_music,back_mode);
        } else if (mode==nnn::RuntimeMode::PrintStatus || mode==nnn::RuntimeMode::PrintParty) {
            const auto index=status_index(mode);
            if (prepare_status(index)) {
                status_visit=status_catalogs[index]->begin(modal_variables());
                set_status_page(request);
                enter_modal(std::move(request),status_music[index],back_mode);
            }
        } else if (mode==nnn::RuntimeMode::SelectPlace2 && back_mode && !from_title &&
                   select_place2.available && check_place(mode,select_place2)) {
            enter_modal(std::move(request),select_place2.music,back_mode);
            playback.suspend_script_audio();
        } else if (mode==nnn::RuntimeMode::SelectChart && prepare_chart_view(request,back_mode.has_value())) {
            enter_modal(std::move(request),chart_music,back_mode);
        } else if (mode==nnn::RuntimeMode::PrintCalendar) {
            warn("PRINTCALENDAR has no default class in upstream SystemNNN");
        } else if (mode==nnn::RuntimeMode::CardSystem) {
            warn("CARDSYSTEM upstream class has no completion or cancel action");
        } else if (mode==nnn::RuntimeMode::SelectHint || mode==nnn::RuntimeMode::PrintHint ||
                   mode==nnn::RuntimeMode::SelectZukan) {
            warn("Optional reference mode has no usable setup: "+std::to_string(static_cast<int>(mode)));
        }
        return frame.wait==Wait::Modal && frame.modal->request!=previous;
    }
    using SelectionVariableReader=std::function<nnn::Value(nnn::Word)>;
    SelectionVariableReader live_selection_variables() {
        return [this](nnn::Word id) { return GetVarData(id); };
    }
    int heroine_slots() const { return std::clamp(config_int(heroine_config,"heroinPrintMax",4),1,heroine_limit); }
    int object_types() const { return std::clamp(config_int(object_config,"selectObjectMax",256),0,object_limit); }
    std::string object_setup(int type) {
        return read_optional_setup(shared_runtime->root(),"selectObjectType"+std::to_string(type)+".xtx");
    }
    bool setup_heroine(ScriptModal& request,int preferred,const SelectionVariableReader& read) {
        if (heroine_config.empty() || heroine_result>=variable_count) {
            warn("SELECTHEROIN has no usable setup or return variable");
            return false;
        }
        const int count=heroine_slots();
        const auto next_var=variable_id(config_text(heroine_config,"nextButtonVarName",""));
        if (config_int(heroine_config,"nextButtonFlag",0)!=0 && read(next_var)!=0) request.group|=1;
        for (int id=0;id<count;++id) {
            const auto suffix=std::to_string(id+1);
            const auto variable=variable_id(config_text(heroine_config,"heroinVarName"+suffix,""));
            const auto face=read(variable);
            if (face<=0) continue;
            const auto index=static_cast<std::uint64_t>(face-1)*static_cast<unsigned>(count)+static_cast<unsigned>(id);
            if (index>=heroine_pictures.size()) continue;
            auto asset=nnn::resource::normalize_cp932_path(heroine_pictures[static_cast<std::size_t>(index)]);
            if (asset.empty()) continue;
            if (asset.find('/')==std::string::npos) asset="ta/"+asset;
            if (!assets.has_image(asset)) continue;
            if (id==preferred) request.page=static_cast<int>(request.rows.size());
            request.rows.push_back({id+1,ScriptModalRow::Enabled,
                config_text(heroine_config,"heroinName"+suffix,""),std::move(asset),{}});
        }
        if (!request.rows.empty() && config_int(heroine_config,"okButtonFlag",1)!=0) request.group|=2;
        if (!request.group) {
            warn("SELECTHEROIN has no available selection or Next action");
            return false;
        }
        if (!request.rows.empty()) {
            request.pages=static_cast<int>(request.rows.size());
            auto& selected=request.rows[static_cast<std::size_t>(request.page)];
            request.selection=selected.id;
            selected.flags|=ScriptModalRow::Selected;
        }
        return true;
    }
    bool setup_objects(ScriptModal& request,const SelectionVariableReader& read) {
        if (object_config.empty() || object_result>=variable_count) {
            warn("SELECTOBJECT has no usable setup or result variable");
            return false;
        }
        const auto type=read(object_type);
        if (type<1 || type>object_types()) {
            warn("SELECTOBJECT has an invalid type: "+std::to_string(type));
            return false;
        }
        request.group=type;
        const auto config=object_setup(type);
        const int count=std::clamp(config_int(config,"objectNumber",0),0,object_limit);
        const int additions=std::clamp(config_int(config,"addObjectNumber",0),0,object_limit);
        const bool attached_only=config_int(config,"addOnlyFlag",0)!=0;
        std::vector<int> owner(static_cast<std::size_t>(additions)+1U);
        std::vector<int> attached(static_cast<std::size_t>(count)+1U);
        std::vector<bool> visible(static_cast<std::size_t>(count)+1U);
        for (int add=1;add<=additions;++add) {
            const auto var=variable_id(config_text(config,"addVarName"+std::to_string(add),""));
            const auto place=read(var);
            if (place>0 && place<=count) {
                owner[static_cast<std::size_t>(add)]=place;
                attached[static_cast<std::size_t>(place)]=add;
            }
        }
        std::string picture;
        for (int id=1;id<=count;++id) {
            const auto key=std::to_string(id);
            const auto appear=variable_id(config_text(config,"objectAppearVarName"+key,""));
            const auto selectable=variable_id(config_text(config,"objectSelectOkVarName"+key,""));
            picture=config_text(config,"object"+key+"FileName",picture);
            const auto i=static_cast<std::size_t>(id);
            visible[i]=appear>=variable_count || read(appear)!=0;
            if (!visible[i]) continue;
            const bool ok=(selectable>=variable_count || read(selectable)!=0) && (!attached_only || attached[i]!=0);
            request.rows.push_back({id,ok ? ScriptModalRow::Enabled : 0,
                config_text(config,"object"+key+"Name",""),picture,std::to_string(attached[i])});
        }
        picture.clear();
        for (int add=1;add<=additions;++add) {
            const auto key=std::to_string(add);
            picture=config_text(config,"add"+key+"FileName",picture);
            const auto place=owner[static_cast<std::size_t>(add)];
            if (!place) continue;
            const auto i=static_cast<std::size_t>(place);
            const bool ok=visible[i] && attached[i]==add;
            request.rows.push_back({-add,ok ? ScriptModalRow::Enabled : 0,
                config_text(config,"add"+key+"Name",""),picture,std::to_string(place)});
        }
        if (std::none_of(request.rows.begin(),request.rows.end(),[](const auto& row){
            return (row.flags&ScriptModalRow::Enabled)!=0;
        })) {
            warn("SELECTOBJECT has no available selection for type "+std::to_string(type));
            return false;
        }
        return true;
    }
    bool object_hover_valid(const ScriptModal& request,int value) const {
        if (value==0) return true;
        if (value<0) return false;
        const auto row=std::find_if(request.rows.begin(),request.rows.end(),[value](const auto& item){return item.id==value;});
        return row!=request.rows.end() &&
            ((row->flags&ScriptModalRow::Enabled)!=0 || config_int(object_config,"autoPicCannotOk",0)!=0);
    }
    ModalResult resume_selection(ScriptModal& request) {
        ScriptModal rebuilt;
        rebuilt.mode=request.mode;
        rebuilt.return_mode=request.return_mode;
        rebuilt.arguments=request.arguments;
        const auto read=live_selection_variables();
        const bool object=request.mode==nnn::RuntimeMode::SelectObject;
        if (object) SetVarData(object_hover,0);
        if (!(object ? setup_objects(rebuilt,read) : setup_heroine(rebuilt,last_heroine,read))) {
            dismiss_modal();
            return ModalResult::Exit;
        }
        rebuilt.request=new_modal_request();
        request=std::move(rebuilt);
        return ModalResult::Updated;
    }
    ModalResult act_heroine(ScriptModal& request,ModalAction action,int value) {
        if (action==ModalAction::Resume) return resume_selection(request);
        if (action==ModalAction::Page && value>=0 && static_cast<std::size_t>(value)<request.rows.size() &&
            static_cast<int>(request.rows.size())>=config_int(heroine_config,"minTurnNumber",1)) {
            request.rows[static_cast<std::size_t>(request.page)].flags&=~ScriptModalRow::Selected;
            request.page=value;
            auto& row=request.rows[static_cast<std::size_t>(value)];
            request.selection=row.id;
            row.flags|=ScriptModalRow::Selected;
            last_heroine=row.id-1;
            return ModalResult::Updated;
        }
        if (action!=ModalAction::Confirm ||
            (value==0 ? (request.group&1)==0 : (request.group&2)==0 || value!=request.selection))
            return ModalResult::Rejected;
        SetVarData(heroine_result,value==0 ? 1 : value);
        dismiss_modal();
        return ModalResult::Exit;
    }
    ModalResult act_object(ScriptModal& request,ModalAction action,int value) {
        if (action==ModalAction::Resume) return resume_selection(request);
        if (action==ModalAction::Hover) {
            if (!object_hover_valid(request,value)) return ModalResult::Rejected;
            SetVarData(object_hover,value);
            request.selection=value;
            return ModalResult::Updated;
        }
        const auto found=std::find_if(request.rows.begin(),request.rows.end(),[value](const auto& row){return row.id==value;});
        if (action!=ModalAction::Choose || found==request.rows.end() || (found->flags&ScriptModalRow::Enabled)==0)
            return ModalResult::Rejected;
        SetVarData(object_result,value>0 ? value : number(found->detail));
        SetVarData(object_add_result,value>0 ? number(found->detail) : -value);
        dismiss_modal();
        return ModalResult::Exit;
    }
    bool commit_name_input(const ScriptModal& request,bool accepted,unsigned elapsed_frames,const std::vector<std::string>& values) {
        if (!accepted && !name_input.cancel) return false;
        if (accepted) {
            if (values.size()!=2) return false;
            std::array<std::string,2> result;
            for (std::size_t i=0;i<result.size();++i) {
                if (!name_input.editable[i]) result[i]=request.text[i];
                else if (values[i].empty()) {
                    if (!name_input.allow_empty) return false;
                    result[i]=request.text[i+2];
                } else {
                    if (values[i].size()>static_cast<std::size_t>(request.byte_limits[i]) ||
                        (values[i]!=request.text[i] && !valid_edited_text(values[i],one_byte_text,false))) return false;
                    result[i]=values[i];
                }
            }
            if (!shared_runtime->set_system_name(result[0],result[1])) return false;
            preview_story_dirty=preview_story_dirty || surname!=result[0] || given_name!=result[1];
            surname=std::move(result[0]);given_name=std::move(result[1]);
            if (replay_films.empty()) system_save_requested=true;
        }
        SetVarData(name_input.status,accepted ? 1 : 0);
        SetVarData(name_input.time,static_cast<nnn::Value>(std::min(elapsed_frames,9999U)));
        return true;
    }
    bool commit_kanji_input(const ScriptModal& request,const std::vector<std::string>& values) {
        if (values.size()!=1 || values.front().empty() ||
            values.front().size()>static_cast<std::size_t>(request.byte_limits.front()) ||
            !valid_edited_text(values.front(),one_byte_text,true)) return false;
        const auto target=request.arguments.empty() ? 0 : request.arguments[0];
        const auto type=request.arguments.size()>1 ? request.arguments[1] : 0;
        const auto& text=values.front();
        if (type<2 && (target<0 || static_cast<std::size_t>(target)>=nnn::runtime_replacement_count)) {
            // The PC setters ignore an out-of-table index and resume after OK.
            warn("Invalid KANJIINPUT replacement index "+std::to_string(target));
            return true;
        }
        if (type==0) {
            const auto index=static_cast<std::size_t>(target);
            preview_story_dirty=preview_story_dirty || local_replacement(index)!=text;
            if (index<replacements.size()) replacements[index]=text;
            else {
                auto next=replacements;
                for (auto i=next.size();i<index;++i) next.push_back(local_replacement(i));
                next.push_back(text);
                replacements.swap(next);
            }
        } else if (type==1) {
            if (!shared_runtime->set_system_replacement(static_cast<std::size_t>(target),text)) return false;
        } else {
            std::string name=text;
            if (type==2) {
                if (!shared_runtime->set_system_name(name,std::nullopt)) return false;
                preview_story_dirty=preview_story_dirty || surname!=name;
                surname.swap(name);
            } else {
                if (!shared_runtime->set_system_name(std::nullopt,name)) return false;
                preview_story_dirty=preview_story_dirty || given_name!=name;
                given_name.swap(name);
            }
        }
        if (type!=0 && replay_films.empty()) system_save_requested=true;
        return true;
    }
    bool complete_modal(int id,bool accepted,unsigned elapsed_frames,const std::vector<std::string>& values) {
        if (frame.wait!=Wait::Modal || !frame.modal || frame.modal->request!=id) return false;
        const auto& request=*frame.modal;
        switch (request.mode) {
        case nnn::RuntimeMode::NameInput:
            if (!commit_name_input(request,accepted,elapsed_frames,values)) return false;
            break;
        case nnn::RuntimeMode::KanjiInput:
            if (!accepted || !commit_kanji_input(request,values)) return false;
            break;
        default: return false;
        }
        dismiss_modal();
        return true;
    }
    void dismiss_modal() {
        const auto target=frame.modal->return_mode;
        if (frame.modal->mode==nnn::RuntimeMode::MyPrintResult) result_visit.reset();
        if (frame.modal->mode==nnn::RuntimeMode::Shop) shop_visit.reset();
        if (frame.modal->mode==nnn::RuntimeMode::PrintStatus || frame.modal->mode==nnn::RuntimeMode::PrintParty)
            status_visit.reset();
        if (frame.modal->mode==nnn::RuntimeMode::SelectChart && chart_visit) {
            chart_scroll=chart_visit->visit.scroll_y;
            chart_visit.reset();
            chart_title_story.reset();
        }
        if (suspended_modal) {
            auto parent=std::move(*suspended_modal);
            suspended_modal.reset();
            mode_before_modal=parent.mode_before;
            wait_before_modal=parent.wait_before;
            modal_caller=parent.caller;
            frame.modal=std::move(parent.request);
            set_mode(frame.modal->mode);
            if (frame.modal->mode==nnn::RuntimeMode::SelectChart) play_modal_music(chart_music);
            else play_modal_music(modal_music(frame.modal->mode==nnn::RuntimeMode::SelectHeroin ? heroine_config : object_config));
            return;
        }
        shared_runtime->set_mode(!modal_caller && target ? static_cast<nnn::Value>(*target) : mode_before_modal);
        frame.modal.reset();
        frame.wait=wait_before_modal;
    }
    bool discard_modal_selection() {
        const auto* active=vm->GetVM(vm->ActiveMode());
        return !active->PendingSelection() || vm->DiscardPendingSelection();
    }
    void resume_story_after_modal() {
        suspended_modal.reset();
        modal_caller.reset();
        frame.modal->return_mode.reset();
        frame.selection.reset();
        frame.choice_limit_ms=frame.choice_remaining_ms=choice_appear_remaining=0;
        frame.choice_timed_out=false;
        mode_before_modal=static_cast<nnn::Value>(nnn::RuntimeMode::Nothing);
        wait_before_modal=Wait::Running;
        dismiss_modal();
    }
    int abort_modal(int request,const std::string& reason,unsigned elapsed_frames) {
        if (frame.wait!=Wait::Modal || !frame.modal || frame.modal->request!=request) return -1;
        const auto mode=frame.modal->mode;
        warn("Optional mode "+std::to_string(static_cast<int>(mode))+" aborted: "+reason.substr(0,256));
        if (mode==nnn::RuntimeMode::NameInput) {
            SetVarData(name_input.status,0);
            SetVarData(name_input.time,static_cast<nnn::Value>(std::min(elapsed_frames,9999U)));
        } else if (mode==nnn::RuntimeMode::SelectZukan && zukan) {
            zukan->clear(reference_write());
        }
        const auto caller=modal_caller;
        if (!caller) frame.modal->return_mode.reset();
        dismiss_modal();
        return caller ? static_cast<int>(*caller) : 0;
    }
    bool prepare_terms() {
        if (term_catalog) return usable_terms;
        try {
            const auto& root=shared_runtime->root();
            auto catalog=std::make_unique<nnn::TermCatalog>(root,filename_encoder);
            term_music=modal_music(nnn::game_config::read(root,"init","printTerm.xtx",filename_encoder));
            term_catalog=std::move(catalog);
            if (term_catalog->configured()) {
                for (std::size_t group=0;group<term_catalog->category_count();++group) {
                    const auto entries=term_catalog->entries(static_cast<nnn::Value>(group),*shared_runtime);
                    if (std::any_of(entries.begin(),entries.end(),[&](const auto& entry){return term_body_available(entry.id);})) {
                        usable_terms=true;
                        return true;
                    }
                }
            }
            warn("PRINTTERM has no usable term entries");
        } catch (const std::exception& e) { warn("PRINTTERM: "+std::string(e.what())); }
        return false;
    }
    bool prepare_voices() {
        if (voice_catalog) return usable_voices;
        try {
            const auto& root=shared_runtime->root();
            auto catalog=std::make_unique<nnn::VoiceCatalog>(root,filename_encoder);
            voice_music=modal_music(nnn::game_config::read(root,"init","listenVoice.xtx",filename_encoder));
            voice_catalog=std::move(catalog);
            if (!voice_catalog->flag_table_error().empty()) warn("LISTENVOICE: "+voice_catalog->flag_table_error());
            if (voice_catalog->missing_flag_ranges()!=0)
                warn("LISTENVOICE: disabled entries without a named voice flag range: "+
                     std::to_string(voice_catalog->missing_flag_ranges()));
            usable_voices=voice_catalog->configured() && has_usable_voice();
            if (usable_voices) return true;
            warn("LISTENVOICE has no usable voice entries");
        } catch (const std::exception& e) { warn("LISTENVOICE: "+std::string(e.what())); }
        return false;
    }
    bool prepare_achievements() {
        if (!achievement_available) return false;
        if (achievement_catalog) return achievement_catalog->configured();
        try {
            const auto& root=shared_runtime->root();
            auto catalog=std::make_unique<nnn::AchievementCatalog>(root,filename_encoder);
            achievement_music=modal_music(nnn::game_config::read(root,"init","printAchievement.xtx",filename_encoder));
            const bool configured=catalog->configured();
            achievement_catalog=std::move(catalog);
            if (configured) return true;
            warn("PRINTACHIEVEMENT has no configured achievements");
        } catch (const std::exception& e) {
            warn("PRINTACHIEVEMENT: "+std::string(e.what()));
            achievement_available=false;
        }
        return false;
    }
    bool prepare_results() {
        if (!result_available) return false;
        if (result_catalog) return result_catalog->configured();
        try {
            const auto& root=shared_runtime->root();
            auto catalog=std::make_unique<nnn::ResultCatalog>(root,filename_encoder);
            result_music=modal_music(nnn::game_config::read(root,"init","printResult.xtx",filename_encoder));
            result_catalog=std::move(catalog);
            if (result_catalog->configured()) return true;
            warn("PRINTRESULT has no usable result setup");
        } catch (const std::exception& e) {
            warn("PRINTRESULT: "+std::string(e.what()));
            result_available=false;
        }
        return false;
    }
    template<class Catalog>
    bool prepare_inventory(std::unique_ptr<Catalog>& catalog,bool& available,ModalMusic& music,const char* name) {
        if (!available) return false;
        if (catalog) return catalog->configured();
        try {
            const auto& root=shared_runtime->root();
            catalog=std::make_unique<Catalog>(root,filename_encoder);
            music=modal_music(read_optional_setup(root,name));
            if (catalog->configured()) return true;
            warn(std::string(name)+" has no usable catalogue");
        } catch (const std::exception& e) {
            warn(std::string(name)+": "+e.what());
            available=false;
        }
        return false;
    }
    nnn::inventory::ReadVariable modal_variables(const nnn::PreviewStoryState* story=nullptr) {
        return [this,story](std::string_view name) -> std::optional<nnn::inventory::Variable> {
            const auto id=variable_id(std::string(name));
            if (id>=variable_count) return {};
            return nnn::inventory::Variable{id,story && !system_variable(id) ? story->vars[id] : GetVarData(id)};
        };
    }
    bool commit_modal_writes(const std::vector<nnn::inventory::VariableWrite>& writes) {
        std::vector<nnn::SystemVariableWrite> system;
        for (const auto& write:writes) {
            if (write.id>=variable_count) return false;
            if (system_variable(write.id)) system.push_back({write.id-first_system_variable,write.expected,write.value});
            else if (vars[write.id]!=write.expected) return false;
        }
        if (!system.empty() && !shared_runtime->compare_exchange_system_variables(system)) return false;
        for (const auto& write:writes) {
            if (system_variable(write.id)) update_variable_layers(write.id,write.value);
            else SetVarData(write.id,write.value);
        }
        if (!system.empty() && replay_films.empty()) system_save_requested=true;
        return true;
    }
    void set_inventory_page(ScriptModal& request,int group,int page,int selected=0) {
        const auto read=modal_variables();
        nnn::reference_selection::Page data;
        data.group=group;
        data.page=page;
        request.text.clear();
        const auto add=[&](const auto& row,int flags,std::string detail) {
            if (row.id+1==selected) {
                data.selection=selected;
                flags|=ScriptModalRow::Selected;
                request.text.assign(row.description.begin(),row.description.end());
            }
            data.rows.push_back({row.id+1,flags,std::string(row.name),row.asset,std::move(detail)});
        };
        if (request.mode==nnn::RuntimeMode::Shop) {
            const auto view=shop_catalog->page(group,page,*shop_visit,read);
            request.balance=view.balance;
            data.pages=std::max(1,view.page_count);
            for (const auto& row:view.rows) {
                int flags=row.detail_selectable ? ScriptModalRow::Enabled : ScriptModalRow::Locked;
                if (row.is_new) flags|=ScriptModalRow::New;
                if (row.sold_out) flags|=ScriptModalRow::SoldOut;
                if (row.purchase_enabled) flags|=ScriptModalRow::PurchaseEnabled;
                add(row,flags,row.price ? std::to_string(*row.price) : std::string{});
            }
        } else {
            const auto view=item_catalog->page(group,page,read);
            data.pages=std::max(1,view.page_count);
            for (const auto& row:view.rows) {
                const int flags=row.detail_selectable ? ScriptModalRow::Enabled : ScriptModalRow::Locked;
                add(row,flags,std::to_string(row.availability));
            }
        }
        set_modal_page(request,std::move(data));
    }
    ModalResult act_inventory(ScriptModal& request,ModalAction action,int value) {
        const bool shop=request.mode==nnn::RuntimeMode::Shop;
        if (action==ModalAction::Back) {
            if (request.selection==0) {
                dismiss_modal();
                return ModalResult::Exit;
            }
            set_inventory_page(request,request.group,shop ? 0 : request.page);
            request.request=new_modal_request();
            return ModalResult::Updated;
        }
        if (action==ModalAction::Group) {
            const auto groups=shop ? shop_catalog->category_count() : item_catalog->category_count();
            if (value<0 || static_cast<std::size_t>(value)>=groups) return ModalResult::Rejected;
            set_inventory_page(request,value,0);
            request.request=new_modal_request();
            return ModalResult::Updated;
        }
        if (action==ModalAction::Page && value>=0 && value<request.pages && value!=request.page &&
            (!shop || request.selection==0)) {
            set_inventory_page(request,request.group,value);
            request.request=new_modal_request();
            return ModalResult::Updated;
        }
        if (action==ModalAction::Choose && value>0 && request.selection==0 &&
            std::any_of(request.rows.begin(),request.rows.end(),[value](const auto& row){
                return row.id==value && (row.flags&ScriptModalRow::Enabled)!=0;
            })) {
            if (shop) {
                const auto seen=shop_catalog->plan_seen(request.group,value-1,modal_variables());
                if (seen && !commit_modal_writes({*seen})) return ModalResult::Rejected;
            }
            set_inventory_page(request,request.group,request.page,value);
            request.request=new_modal_request();
            return ModalResult::Updated;
        }
        if (shop && action==ModalAction::Confirm && value>0 && value==request.selection) {
            const auto purchase=shop_catalog->plan_purchase(request.group,value-1,modal_variables());
            if (!purchase || !commit_modal_writes(purchase->writes)) {
                set_inventory_page(request,request.group,request.page,value);
                return ModalResult::Updated;
            }
            set_inventory_page(request,request.group,0);
            request.request=new_modal_request();
            return ModalResult::Updated;
        }
        return ModalResult::Rejected;
    }
    static std::size_t status_index(nnn::RuntimeMode mode) {
        return mode==nnn::RuntimeMode::PrintParty ? 1U : 0U;
    }
    bool prepare_status(std::size_t index) {
        if (!status_available[index]) return false;
        auto& catalog=status_catalogs[index];
        if (catalog) return catalog->configured();
        const bool party=index!=0;
        const char* name=party ? "printParty.xtx" : "printStatus.xtx";
        try {
            const auto setup=read_optional_setup(shared_runtime->root(),name);
            const auto faces=party && !setup.empty() && config_int(setup,"oldVersion",0)==0 ?
                read_optional_setup(shared_runtime->root(),"faceStatusList.xtx") : std::string{};
            catalog=std::make_unique<nnn::status_party::Catalog>(party,setup,faces);
            status_music[index]=modal_music(setup);
            if (catalog->configured()) return true;
            warn(std::string(name)+" has no usable status data");
        } catch (const std::exception& e) {
            warn(std::string(name)+": "+e.what());
            status_available[index]=false;
        }
        return false;
    }
    void set_status_page(ScriptModal& request) {
        const auto& catalog=*status_catalogs[status_index(request.mode)];
        auto& visit=*status_visit;
        const auto read=modal_variables();
        catalog.refresh(visit,read);
        status_rows(request,catalog,visit,read,false,nullptr);
    }
    void status_rows(ScriptModal& request,const nnn::status_party::Catalog& catalog,
                     const nnn::status_party::Visit& visit,const nnn::inventory::ReadVariable& read,
                     bool overlay,const nnn::PreviewStoryState* story) {
        request.group=catalog.preset(read);
        request.selection=overlay ? 100 : catalog.progress(visit);
        request.rows.clear();
        for (const auto& control:catalog.controls(visit,read)) {
            const int flags=(!overlay && control.enabled ? ScriptModalRow::Enabled : 0) |
                            (control.checked ? ScriptModalRow::Selected : 0);
            request.rows.push_back({control.id,flags,{},{},std::to_string(control.value)});
        }
        int next=10000;
        for (std::size_t i=0;i<catalog.bindings().size();++i)
            request.rows.push_back({next++,0,catalog.bindings()[i].key,{},std::to_string(catalog.data(visit,i,read))});
        if (catalog.local_text())
            for (std::size_t i=0;i<nnn::runtime_replacement_count;++i)
                request.rows.push_back({next++,0,"local"+std::to_string(i),{},local_replacement(i,story)});
        if (catalog.system_text()) {
            auto text=shared_runtime->runtime_text();
            for (std::size_t i=0;i<text.system_replacements.size();++i)
                request.rows.push_back({next++,0,"system"+std::to_string(i),{},std::move(text.system_replacements[i])});
        }
    }
    std::optional<ScriptModal> status_overlay(nnn::RuntimeMode type,nnn::RuntimeMode mode,bool prefer_story) {
        if (type!=nnn::RuntimeMode::PrintStatus && type!=nnn::RuntimeMode::PrintParty) return {};
        const auto index=status_index(type);
        if (!prepare_status(index)) return {};
        const auto& catalog=*status_catalogs[index];
        if (!catalog.overlay_for(mode)) return {};
        const auto story=prefer_story ? shared_runtime->preview_story() : nullptr;
        const auto read=modal_variables(story.get());
        const auto visit=catalog.begin(read);
        if (!catalog.overlay_visible(visit,read)) return {};
        ScriptModal overlay;
        overlay.mode=type;
        status_rows(overlay,catalog,visit,read,true,story.get());
        return overlay;
    }
    ModalResult act_status(ScriptModal& request,ModalAction action,int value) {
        if (action==ModalAction::Back) {
            dismiss_modal();
            return ModalResult::Exit;
        }
        if (action!=ModalAction::Choose && action!=ModalAction::Page && action!=ModalAction::Group)
            return ModalResult::Rejected;
        using Action=nnn::status_party::Action;
        const auto native=action==ModalAction::Page ? Action::Move :
            action==ModalAction::Group ? Action::Preset : Action::Choose;
        auto next=*status_visit;
        const auto& catalog=*status_catalogs[status_index(request.mode)];
        const auto read=modal_variables();
        catalog.refresh(next,read);
        const auto writes=catalog.apply(next,native,value,read);
        if (!writes || !commit_modal_writes(*writes)) return ModalResult::Rejected;
        status_visit=std::move(next);
        set_status_page(request);
        request.request=new_modal_request();
        return ModalResult::Updated;
    }
    bool configure_chart() {
        if (!chart_available || chart_failed) return false;
        if (chart_graph) return true;
        try {
            const auto& root=shared_runtime->root();
            auto graph=std::make_unique<nnn::chart::Graph>(root,static_cast<int>(renderer->width()),
                static_cast<int>(renderer->height()),filename_encoder);
            if (!graph->configured()) throw std::runtime_error("Missing chart setup");
            chart_music=modal_music(read_optional_setup(root,"selectChart.xtx"));
            chart_store.emplace(nnn::resource::find_directory(root,"sav",filename_encoder));
            chart_store->private_root(chart_private_root);
            const auto& commands=graph->setup().commands;
            std::set<std::string> registered;
            for (nnn::Word id=0;id<registry.Size();++id) {
                std::string name;
                if (!registry.GetName(id,name)) continue;
                name=fold_cp932(std::move(name));
                const auto found=std::find_if(commands.begin(),commands.end(),[&](const auto& command) {
                    return fold_cp932(command.name)==name;
                });
                if (found!=commands.end() && registered.insert(name).second)
                    chart_functions.emplace(id,found->function);
            }
            chart_graph=std::move(graph);
            return true;
        } catch (const std::exception& e) {
            warn("SELECTCHART: "+std::string(e.what()));
            chart_failed=true;
            return false;
        }
    }
    std::shared_ptr<const nnn::chart::Progress> chart_progress() {
        if (auto live=shared_runtime->chart_progress()) {
            if (chart_graph->valid(*live)) return live;
            warn("SELECTCHART progress does not match the configured chart size");
            return {};
        }
        if (!chart_legacy_progress) {
            auto progress=chart_graph->global_defaults();
            try { progress=chart_store->legacy_progress(*chart_graph); }
            catch (const std::exception& e) { warn("SELECTCHART legacy progress: "+std::string(e.what())); }
            chart_legacy_progress=std::make_shared<const nnn::chart::Progress>(std::move(progress));
        }
        return chart_legacy_progress;
    }
    nnn::chart::RunState& current_chart_run() {
        if (!chart_run) {
            chart_run=chart_graph->run_defaults();
            const auto inherited=shared_runtime->chart_selection();
            if (inherited && inherited->slots==chart_graph->slot_capacity())
                chart_run->can_select=inherited->enabled;
        }
        return *chart_run;
    }
    nnn::chart::ReadVariable chart_variables(const std::array<nnn::Value,variable_count>* saved=nullptr) {
        return [this,saved,title=chart_title_story](std::string_view name) -> std::optional<nnn::Value> {
            const auto id=variable_id(std::string(name));
            if (id>=variable_count) return {};
            const auto* local=saved ? saved : title ? &title->vars : nullptr;
            return local && !system_variable(id) ? (*local)[id] : GetVarData(id);
        };
    }
    bool seed_chart_title_story() {
        chart_title_story=shared_runtime->preview_story();
        if (chart_title_story) return true;
        try {
            auto story=std::make_shared<nnn::PreviewStoryState>();
            load_variable_defaults(story->vars);
            auto names=shared_runtime->runtime_text();
            story->surname=std::move(names.surname);
            story->given_name=std::move(names.given_name);
            chart_title_story=std::move(story);
            return true;
        } catch (const std::exception& e) {
            warn("SELECTCHART title story defaults: "+std::string(e.what()));
            return false;
        }
    }
    void set_chart_display_page(nnn::Value page) {
        const auto id=variable_id(chart_graph->setup().display_page_var);
        if (id>=variable_count) return;
        if (!chart_title_story || system_variable(id)) {
            SetVarData(id,page);
        } else if (chart_title_story->vars[id]!=page) {
            auto story=std::make_shared<nnn::PreviewStoryState>(*chart_title_story);
            story->vars[id]=page;
            chart_title_story=story;
            shared_runtime->set_preview_story(std::move(story));
        }
    }
    void write_chart_progress(nnn::chart::Progress progress) {
        if (shared_runtime->set_chart_progress(std::move(progress))) system_save_requested=true;
    }
    bool chart_checkpoint(const nnn::chart::Checkpoint& plan,const nnn::chart::Progress& progress,
                          std::optional<nnn::Value> restore_work={}) {
        if (input.legacy_calculations || input.recorded_clock) return false;
        const auto& setup=chart_graph->setup();
        const auto slot=variable_id(setup.slot_var),work=variable_id(setup.work_var);
        const auto display=variable_id(setup.display_page_var);
        try {
            ChartNodeSave saved;
            saved.status=make_chart_status(plan.mode);
            saved.vars=nnn::chart::capture_variables([&](std::size_t i){return GetVarData(static_cast<nnn::Word>(i));});
            saved.run=current_chart_run();
            if (slot<saved.vars.size()) saved.vars[slot]=plan.slot;
            if (work<saved.vars.size()) saved.vars[work]=plan.work_slot;
            if (plan.display_page && display<saved.vars.size()) saved.vars[display]=*plan.display_page;
            if (plan.road) chart_graph->record_edge(saved.run,*plan.road);
            auto updated=progress;
            const bool marked=plan.mark_seen && chart_graph->mark_route(updated,*plan.mark_seen);
            chart_store->write(plan.slot,saved,*chart_graph);
            SetVarData(slot,plan.slot);
            SetVarData(work,restore_work.value_or(plan.work_slot));
            if (plan.display_page) SetVarData(display,*plan.display_page);
            chart_run=std::move(saved.run);
            if (marked) write_chart_progress(std::move(updated));
            return true;
        } catch (const std::exception& e) {
            warn("SELECTCHART could not checkpoint node "+std::to_string(plan.slot)+": "+e.what());
            return false;
        }
    }
    bool chart_function(nnn::Word id,const std::string& name,const std::vector<nnn::Value>& args) {
        if (name.empty() || !configure_chart()) return false;
        const auto function=chart_functions.find(id);
        if (function==chart_functions.end()) return false;
        const auto flags=chart_progress();
        if (!flags) return true;
        auto updated=*flags;
        const auto action=chart_graph->apply_function(function->second,args,updated,current_chart_run(),chart_variables());
        if (action.write) SetVarData(variable_id(std::string(action.write->name)),action.write->value);
        if (action.progress_changed) write_chart_progress(std::move(updated));
        if (action.run_changed) shared_runtime->set_chart_selection(chart_graph->slot_capacity(),chart_run->can_select);
        if (action.save_system) system_save_requested=true;
        if (action.open_slot) chart_checkpoint(action.open_slot->checkpoint,*flags,action.open_slot->restore_work);
        return true;
    }
    void save_chart_exit() {
        if (!configure_chart()) return;
        const auto progress=chart_progress();
        if (!progress) return;
        const auto read=chart_variables();
        const auto& setup=chart_graph->setup();
        const auto plan=chart_graph->exit_plan(read(setup.slot_var).value_or(0),read(setup.work_var).value_or(0));
        if (plan) chart_checkpoint(*plan,*progress);
    }
    bool set_chart_page(ScriptModal& request,const ChartInteraction& state,const nnn::chart::Progress& progress,
                        const nnn::chart::RunState& run,const nnn::chart::ReadVariable& read) {
        auto view=chart_view_model.view(*chart_graph,state,progress,run,read,assets);
        if (!view) return false;
        request.page=state.visit.page;
        request.pages=static_cast<int>(view->page_states.size());
        request.group=state.hit.candidate;
        request.selection=state.visit.current;
        request.chart=std::move(view);
        return true;
    }
    bool prepare_chart_view(ScriptModal& request,bool external) {
        if (!configure_chart() || !replay_films.empty()) return false;
        if (!chart_view_model.ready(*chart_graph,assets)) {
            warn("SELECTCHART first slot atlas is unavailable");
            return false;
        }
        auto progress=chart_progress();
        if (!progress) return false;
        const auto read=chart_variables();
        const auto& setup=chart_graph->setup();
        auto current=read(setup.slot_var);
        if (!current) {
            warn("SELECTCHART has no bound current slot variable");
            return false;
        }
        if (!external) {
            const auto plan=chart_graph->enter_plan(*current,read(setup.work_var).value_or(0));
            if (plan.empty()) {
                warn("SELECTCHART current slot is outside the chart");
                return false;
            }
            bool saved=false;
            for (const auto& step:plan) {
                if (!chart_checkpoint(step,*progress)) break;
                saved=true;
                current=step.slot;
                progress=chart_progress();
            }
            if (!saved) return false;
        } else if (*current==0 && !chart_graph->node(*current) && chart_graph->node(1)) {
            current=1;
        }
        ChartInteraction next;
        next.visit=chart_graph->begin_visit(*current,chart_scroll,read);
        if (!set_chart_page(request,next,*progress,current_chart_run(),read)) return false;
        chart_visit=std::move(next);
        return true;
    }
    static nnn::chart::Point chart_pointer(int packed) noexcept {
        const auto bits=static_cast<std::uint32_t>(packed);
        const auto signed_half=[](std::uint32_t half) {
            return static_cast<int>(half)-(half>=0x8000U ? 65536 : 0);
        };
        return {signed_half(bits>>16U),signed_half(bits&0xffffU)};
    }
    void chart_sound(int sound) {
        if (sound<=0) return;
        const auto error=playback.play_system_se(static_cast<unsigned>(sound-1));
        if (!error.empty()) warn(error);
    }
    ModalResult select_chart_branch(ScriptModal& request,ChartInteraction& state,const nnn::chart::Branch& branch,
                                   const nnn::chart::Progress& progress,const nnn::chart::ReadVariable& read) {
        const auto& setup=chart_graph->setup();
        const auto page=chart_graph->node(branch.to)->page;
        const bool new_page=chart_graph->page_available(page,progress,*chart_run,branch.from,read) &&
            ChartViewModel::new_page(setup,page,read);
        const bool external=modal_caller.has_value();
        const bool rewind=branch.restore_node || chart_title_story;
        bool resumed=true;
        if (rewind) resumed=restore_chart_node(branch);
        else if (external) resumed=discard_modal_selection();
        if (!resumed) {
            state.pending=-1;
            request.request=new_modal_request();
            set_chart_page(request,state,progress,*chart_run,read);
            return ModalResult::Updated;
        }
        if (!rewind) {
            SetVarData(variable_id(setup.work_var),branch.from);
            SetVarData(variable_id(setup.slot_var),branch.to);
        }
        if (new_page) {
            const auto id=variable_id(setup.new_mark_var);
            const auto value=static_cast<nnn::Word>(GetVarData(id))|
                (nnn::Word{1}<<static_cast<unsigned>(page));
            const auto signed_bits=std::int64_t{value}-(value>INT32_MAX ? 0x100000000LL : 0LL);
            SetVarData(id,static_cast<nnn::Value>(signed_bits));
        }
        if (external) resume_story_after_modal();
        else dismiss_modal();
        return external ? ModalResult::EnterStory : ModalResult::Exit;
    }
    ModalResult act_chart(ScriptModal& request,ModalAction action,int value) {
        auto& state=*chart_visit;
        if (action==ModalAction::Back && state.pending<0 && modal_caller) {
            dismiss_modal();
            return ModalResult::Exit;
        }
        const auto progress=chart_progress();
        if (!progress) return ModalResult::Unavailable;
        auto& run=*chart_run;
        const auto read=chart_variables();
        const auto& setup=chart_graph->setup();
        if (state.staged>=0) {
            if (action==ModalAction::Commit) {
                const auto target=std::exchange(state.staged,-1);
                if (const auto branch=chart_graph->branch(*progress,run,state.visit.current,target,read))
                    return select_chart_branch(request,state,*branch,*progress,read);
                state.pending=-1;
                request.request=new_modal_request();
                return set_chart_page(request,state,*progress,run,read) ? ModalResult::Updated : ModalResult::Unavailable;
            }
            if (action!=ModalAction::Resume) return ModalResult::Rejected;
            state.staged=-1;
            return set_chart_page(request,state,*progress,run,read) ? ModalResult::Updated : ModalResult::Unavailable;
        }
        if (state.pending>=0) {
            if (action==ModalAction::Confirm && value==1) {
                if (chart_graph->branch(*progress,run,state.visit.current,state.pending,read)) {
                    state.staged=state.pending;
                    return ModalResult::LeaveReady;
                }
            } else if (action!=ModalAction::Back && !(action==ModalAction::Confirm && value==0)) {
                return ModalResult::Rejected;
            }
            state.pending=-1;
            request.request=new_modal_request();
        } else if (action==ModalAction::Scroll) {
            state.visit.scroll_y=std::clamp(value,0,chart_graph->scroll_limit(state.visit.page));
            state.hit={};
            state.pointer_y=-1;
        } else if (action==ModalAction::Page) {
            if (!chart_graph->change_page(state.visit,value,*progress,run,read)) return ModalResult::Rejected;
            set_chart_display_page(state.visit.page);
            state.hit={};
            state.pointer_y=-1;
        } else if (action==ModalAction::Hover || action==ModalAction::Choose) {
            const auto pointer=chart_pointer(value);
            const bool inside=pointer.x>=0 && pointer.y>=0 && pointer.x<setup.width && pointer.y<setup.height;
            if (action==ModalAction::Choose && !inside) return ModalResult::Rejected;
            const auto hit=inside ? chart_graph->hit(state.visit,pointer,*progress,run,read) : nnn::chart::Hit{};
            if (action==ModalAction::Choose) {
                const auto branch=chart_graph->branch(*progress,run,state.visit.current,hit.candidate,read);
                if (!branch) return ModalResult::Rejected;
                chart_sound(setup.select_sound);
                if (!branch->confirm) {
                    state.staged=branch->to;
                    return ModalResult::LeaveReady;
                }
                state.pending=branch->to;
                request.request=new_modal_request();
            } else if (hit.candidate!=state.hit.candidate && hit.access!=nnn::chart::Access::Locked) {
                chart_sound(setup.over_sound);
            }
            state.hit=hit;
            state.pointer_y=inside ? pointer.y : -1;
        } else if (action!=ModalAction::Resume) {
            return ModalResult::Rejected;
        }
        return set_chart_page(request,state,*progress,*chart_run,chart_variables()) ?
            ModalResult::Updated : ModalResult::Unavailable;
    }
    void set_result_page(ScriptModal& request) {
        const auto& visit=*result_visit;
        nnn::reference_selection::Page page;
        page.page=visit.page;page.pages=result_catalog->page_count();
        page.group=visit.hovered+1;page.selection=visit.pending_ack+1;
        const int first=page.page*result_catalog->per_page();
        const int end=std::min(result_catalog->result_count(),first+result_catalog->per_page());
        const auto plate=result_catalog->plate_name(page.page);
        for (int id=first;id<end;++id) {
            auto row=result_catalog->entry(id,visit);
            if (!row || (!row->visible && id!=visit.pending_ack)) continue;
            int flags=row->hoverable ? ScriptModalRow::Enabled : 0;
            if (id==visit.pending_ack) flags|=ScriptModalRow::New;
            if (id==visit.hovered) flags|=ScriptModalRow::Selected;
            page.rows.push_back({id+1,flags,std::move(row->label),
                id==visit.pending_ack ? result_catalog->dialog_name(id) : plate,{}});
        }
        set_modal_page(request,std::move(page));
        request.text.clear();
        if (auto row=result_catalog->entry(visit.hovered,visit)) {
            if (result_catalog->description_type()==0) request.text=std::move(row->lines);
            else if (result_catalog->description_type()==1 && !row->scrolling_text.empty())
                request.text.push_back(std::move(row->scrolling_text));
        }
    }
    ModalResult act_result(ScriptModal& request,ModalAction action,int value) {
        auto& visit=*result_visit;
        if (action==ModalAction::Back && visit.pending_ack==-1) {
            playback.suspend_script_audio();
            dismiss_modal();
            return ModalResult::Exit;
        }
        switch (action) {
        case ModalAction::Confirm:
            if (value<=0 || !result_catalog->acknowledge(visit,value-1,*shared_runtime)) return ModalResult::Rejected;
            if (replay_films.empty()) system_save_requested=true;
            request.request=new_modal_request();
            break;
        case ModalAction::Page:
            if (!result_catalog->set_page(visit,value)) return ModalResult::Rejected;
            break;
        case ModalAction::Hover:
            if (value<0 || !result_catalog->hover(visit,value-1)) return ModalResult::Rejected;
            break;
        default: return ModalResult::Rejected;
        }
        set_result_page(request);
        return ModalResult::Updated;
    }
    void advance_modal(int id,unsigned frames) {
        if (!frame.modal || frame.modal->request!=id) return;
        if (frame.modal->mode==nnn::RuntimeMode::PrintStatus && status_visit) {
            const auto& catalog=*status_catalogs[0];
            catalog.advance(*status_visit,frames);
            frame.modal->selection=catalog.progress(*status_visit);
            return;
        }
        if (frame.modal->mode!=nnn::RuntimeMode::MyPrintResult || !result_visit || result_visit->hovered<0) return;
        auto& visit=*result_visit;
        frames=std::min(frames,1000U);
        while (frames-- && (visit.voice_remaining>0 || visit.scroll_ticks<visit.scroll_limit)) {
            const auto voice=result_catalog->tick_hover(visit);
            if (!voice.empty()) {
                const auto error=playback.replay_menu_voice(std::string(voice));
                if (!error.empty()) warn("PRINTRESULT: "+error);
            }
        }
    }
    static nnn::reference_selection::Page catalog_page(int group,int page,int per_page,std::size_t count) {
        nnn::reference_selection::Page result;
        result.group=group;
        result.pages=std::max(1,(static_cast<int>(count)+per_page-1)/per_page);
        result.page=std::clamp(page,0,result.pages-1);
        return result;
    }
    std::vector<std::string> term_description(int id) {
        try { return term_catalog->text(id); }
        catch (const std::exception& e) {
            warn("PRINTTERM description: "+std::string(e.what()));
            return {};
        }
    }
    bool term_body_available(int id) {
        auto& state=term_body_state[static_cast<std::size_t>(id)];
        if (!state) state=term_description(id).empty() ? -1 : 1;
        return state>0;
    }
    void set_term_page(ScriptModal& request,int group,int page,int selected=0) {
        auto entries=term_catalog->entries(group,*shared_runtime);
        entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const nnn::TermEntry& entry) {
            return !term_body_available(entry.id);
        }),entries.end());
        const int per_page=term_catalog->per_page();
        auto result=catalog_page(group,page,per_page,entries.size());
        result.selection=selected;
        const auto first=static_cast<std::size_t>(result.page*per_page);
        const auto end=std::min(entries.size(),first+static_cast<std::size_t>(per_page));
        for (auto i=first;i<end;++i) {
            auto& entry=entries[i];
            const int id=entry.id+1;
            int flags=ScriptModalRow::Enabled;
            if (entry.unlocked) {
                if (entry.is_new) flags|=ScriptModalRow::New;
                if (id==selected) flags|=ScriptModalRow::Selected;
            } else {
                // The original shows question marks but allows opening the entry's description.
                flags|=ScriptModalRow::Locked;
                entry.title=nnn::TermCatalog::unknown_title(one_byte_text);
            }
            result.rows.push_back({id,flags,std::move(entry.title),{},{}});
        }
        set_modal_page(request,std::move(result));
    }
    nnn::VoiceCatalog::ResolveVariable voice_variable() const {
        return [this](std::string_view name) {
            const auto id=variable_id(std::string(name));
            return id==variable_count ? -1 : static_cast<nnn::Value>(id);
        };
    }
    nnn::VoiceCatalog::ReadVariable voice_value() {
        return [this](nnn::Value id) { return GetVarData(static_cast<nnn::Word>(id)); };
    }
    bool voice_present(const std::string& filename) {
        const auto key=fold_cp932(filename);
        const auto found=voice_sources.find(key);
        if (found!=voice_sources.end()) return found->second;
        const bool present=playback.has_menu_voice(filename);
        voice_sources.emplace(key,present);
        return present;
    }
    bool has_usable_voice() {
        const auto resolve=voice_variable();
        for (std::size_t group=0;group<voice_catalog->character_count();++group) {
            const auto entries=voice_catalog->entries(static_cast<nnn::Value>(group),*shared_runtime,resolve);
            if (std::any_of(entries.begin(),entries.end(),[&](const auto& entry) {
                return entry.valid_source && voice_present(entry.filename);
            })) return true;
        }
        return false;
    }
    void set_voice_page(ScriptModal& request,int group,int page) {
        const auto count=voice_catalog->entry_count(group);
        const auto resolve=voice_variable();
        const auto read=voice_value();
        const bool has_flags=voice_catalog->has_flag_table();
        std::vector<nnn::Value> visible;
        if (!has_flags) {
            for (std::size_t i=0;i<count;++i) {
                const auto index=static_cast<nnn::Value>(i);
                const auto entry=voice_catalog->entry(group,index,*shared_runtime,resolve,read);
                if (entry && !entry->flag_dependent) visible.push_back(index);
            }
        }
        const auto visible_count=has_flags ? count : visible.size();
        const int per_page=voice_catalog->per_page();
        auto result=catalog_page(group,page,per_page,visible_count);
        const auto first=static_cast<std::size_t>(result.page*per_page);
        const auto end=std::min(visible_count,first+static_cast<std::size_t>(per_page));
        for (auto i=first;i<end;++i) {
            const auto index=has_flags ? static_cast<nnn::Value>(i) : visible[i];
            auto entry=voice_catalog->entry(group,index,*shared_runtime,resolve,read);
            if (entry && entry->available && voice_present(entry->filename)) result.rows.push_back({index+1,ScriptModalRow::Enabled,
                std::move(entry->title),std::move(entry->filename),{}});
            else result.rows.push_back({index+1,ScriptModalRow::Locked,
                std::string(voice_catalog->unknown_title(one_byte_text)),{},{}});
        }
        set_modal_page(request,std::move(result));
    }
    ModalResult act_term(ScriptModal& request,ModalAction action,int value) {
        if (action==ModalAction::Back) {
            dismiss_modal();
            return ModalResult::Exit;
        }
        if (action==ModalAction::Group && value>=0 &&
            static_cast<std::size_t>(value)<term_catalog->category_count()) {
            set_term_page(request,value,0);
            request.text.clear();
            term_group=value;
            return ModalResult::Updated;
        }
        if (action==ModalAction::Page && value>=0 && value<request.pages && value!=request.page) {
            set_term_page(request,request.group,value);
            request.text.clear();
            return ModalResult::Updated;
        }
        if (action!=ModalAction::Choose ||
            std::none_of(request.rows.begin(),request.rows.end(),[value](const auto& row){return row.id==value;}))
            return ModalResult::Rejected;
        if (request.selection==value) return ModalResult::Updated;
        auto description=term_description(value-1);
        if (description.empty()) {
            term_body_state[static_cast<std::size_t>(value-1)]=-1;
            set_term_page(request,request.group,request.page);
            request.text.clear();
            return ModalResult::Updated;
        }
        if (shared_runtime->set_term_read(value-1) && replay_films.empty()) system_save_requested=true;
        set_term_page(request,request.group,request.page,value);
        request.text=std::move(description);
        return ModalResult::Updated;
    }
    ModalResult act_voice(ScriptModal& request,ModalAction action,int value) {
        if (action==ModalAction::Back) {
            dismiss_modal();
            return ModalResult::Exit;
        }
        if (action==ModalAction::Group && value>=0 &&
            static_cast<std::size_t>(value)<voice_catalog->character_count()) {
            set_voice_page(request,value,0);
            return ModalResult::Updated;
        }
        if (action==ModalAction::Page && value>=0 && value<request.pages && value!=request.page) {
            set_voice_page(request,request.group,value);
            return ModalResult::Updated;
        }
        if (action!=ModalAction::Choose ||
            std::none_of(request.rows.begin(),request.rows.end(),[value](const auto& row){
                return row.id==value && (row.flags&ScriptModalRow::Enabled)!=0;
            })) return ModalResult::Rejected;
        const auto entry=voice_catalog->entry(request.group,value-1,*shared_runtime,voice_variable(),voice_value());
        if (!entry || !entry->available || !voice_present(entry->filename)) return ModalResult::Rejected;
        const auto error=playback.replay_menu_voice(entry->filename);
        if (!error.empty()) {
            warn("LISTENVOICE: "+error);
            voice_sources[fold_cp932(entry->filename)]=false;
            usable_voices=has_usable_voice();
            if (!usable_voices) return ModalResult::Unavailable;
            set_voice_page(request,request.group,request.page);
        }
        return ModalResult::Updated;
    }
    void change_hint_mode(nnn::RuntimeMode mode,nnn::reference_selection::Page page,const ModalMusic& music) {
        auto& request=*frame.modal;
        set_modal_page(request,std::move(page));
        request.request=new_modal_request();
        request.mode=mode;
        set_mode(mode);
        play_modal_music(music);
    }
    ModalResult act_modal(int id,ModalAction action,int value) {
        if (frame.wait!=Wait::Modal || !frame.modal || frame.modal->request!=id) return ModalResult::Rejected;
        auto& request=*frame.modal;
        switch (request.mode) {
        case nnn::RuntimeMode::PrintAchievement:
            if (action==ModalAction::Back) {
                dismiss_modal();
                return ModalResult::Exit;
            }
            break;
        case nnn::RuntimeMode::MyPrintResult: return act_result(request,action,value);
        case nnn::RuntimeMode::SelectHeroin: return act_heroine(request,action,value);
        case nnn::RuntimeMode::SelectObject: return act_object(request,action,value);
        case nnn::RuntimeMode::PrintItem:
        case nnn::RuntimeMode::Shop: return act_inventory(request,action,value);
        case nnn::RuntimeMode::PrintStatus:
        case nnn::RuntimeMode::PrintParty: return act_status(request,action,value);
        case nnn::RuntimeMode::SelectChart: return act_chart(request,action,value);
        case nnn::RuntimeMode::SelectPlace2:
            if (action!=ModalAction::Choose || !modal_caller || !can_choose_place(select_place2,value) ||
                !discard_modal_selection()) return ModalResult::Rejected;
            commit_place(select_place2,value);
            resume_story_after_modal();
            return ModalResult::EnterStory;
        case nnn::RuntimeMode::PrintTerm: return act_term(request,action,value);
        case nnn::RuntimeMode::ListenVoice: return act_voice(request,action,value);
        case nnn::RuntimeMode::SelectHint:
            if (action==ModalAction::Back) {
                dismiss_modal();
                return ModalResult::Exit;
            }
            if (action==ModalAction::Choose && std::any_of(request.rows.begin(),request.rows.end(),
                    [value](const auto& row){return row.id==value;})) {
                auto page=first_hint_page(value);
                if (!page) {
                    if (auto parent=select_hint_page()) {
                        set_modal_page(request,std::move(*parent));
                        return ModalResult::Updated;
                    }
                    dismiss_modal();
                    return ModalResult::Exit;
                }
                change_hint_mode(nnn::RuntimeMode::PrintHint,std::move(*page),print_hint_music);
                hint_group=value;
                return ModalResult::ChangedMode;
            }
            break;
        case nnn::RuntimeMode::PrintHint:
            if (action==ModalAction::Back) {
                if (auto page=select_hint_page()) {
                    change_hint_mode(nnn::RuntimeMode::SelectHint,std::move(*page),select_hint_music);
                    return ModalResult::ChangedMode;
                }
                dismiss_modal();
                return ModalResult::Exit;
            }
            if (action==ModalAction::Page && value>=0 && value<request.pages && value!=request.page) {
                set_modal_page(request,hints->pictures(reference_read(),hint_group,value));
                return ModalResult::Updated;
            }
            break;
        case nnn::RuntimeMode::SelectZukan:
            if (request.selection!=0) {
                if (action!=ModalAction::Back && action!=ModalAction::Confirm) break;
                zukan->clear(reference_write());
                set_modal_page(request,zukan->grid(reference_read(),0));
                request.request=new_modal_request();
                return ModalResult::Updated;
            }
            if (action==ModalAction::Back) {
                dismiss_modal();
                return ModalResult::Exit;
            }
            if (action==ModalAction::Choose && zukan->choose(reference_read(),reference_write(),value)) {
                set_modal_page(request,zukan->grid(reference_read(),value));
                request.request=new_modal_request();
                return ModalResult::Updated;
            }
            break;
        default: break;
        }
        return ModalResult::Rejected;
    }
    PlaceConfig configure_places(std::optional<nnn::Word> command,const std::string& config) const {
        PlaceConfig setup;
        setup.command=command;
        setup.available=true;
        const auto count=static_cast<unsigned>(std::clamp(config_int(config,"placeNumber",1),1,64));
        setup.test_appear=config_int(config,"placeAppearFlag",0)!=0;
        setup.test_select=config_int(config,"selectFlag",0)!=0;
        setup.has_character=config_int(config,"charaFlag",0)!=0;
        setup.test_character=setup.has_character && config_int(config,"charaOnlyFlag",1)!=0;
        setup.result=variable_id(config_text(config,"selectedPlaceVarName","\x8f\xea\x8f\x8a\x91\x49\x91\xf0\x94\xd4\x8d\x86"));
        setup.hover=variable_id(config_text(config,"onButtonVarName",""));
        for (unsigned i=1;i<=count;++i) {
            const auto index=std::to_string(i);
            setup.choices.push_back({variable_id(config_text(config,"placeAppearVarName"+index,"")),
                variable_id(config_text(config,"placeSelectOkVarName"+index,"")),
                variable_id(config_text(config,"placeCharaVarName"+index,""))});
        }
        setup.music=modal_music(config);
        if (config_int(config,"autoPicLoad",0)!=0)
            for (unsigned i=0;i<setup.previews.size();++i)
                setup.previews[i]=config_text(config,"autoPicLayerTag"+std::to_string(i),"");
        return setup;
    }
    const PlaceConfig* place_config(nnn::Value mode) const {
        switch (static_cast<nnn::RuntimeMode>(mode)) {
        case nnn::RuntimeMode::SelectPlace: return select_place.available ? &select_place : nullptr;
        case nnn::RuntimeMode::SelectPlace2: return select_place2.available ? &select_place2 : nullptr;
        default: return nullptr;
        }
    }
    const std::array<std::string,32>* ui_preview_tags() const {
        if (!ui_mode) return nullptr;
        if (const auto* setup=place_config(*ui_mode)) return &setup->previews;
        return *ui_mode==static_cast<nnn::Value>(nnn::RuntimeMode::SelectObject) && !object_config.empty() ?
            &object_previews : nullptr;
    }
    int place_state(const PlaceConfig& setup,const Place& place) {
        if (setup.test_appear && GetVarData(place.appear)==0) return 0;
        if ((setup.test_select && GetVarData(place.selectable)==0) ||
            (setup.test_character && GetVarData(place.character)<=0)) return 1;
        return 2;
    }
    bool check_place(nnn::RuntimeMode mode,const PlaceConfig& setup) {
        if (setup.result>=variable_count) {
            const std::string command=mode==nnn::RuntimeMode::SelectPlace2 ? "SELECTPLACE2" : "SELECTPLACE";
            warn(command+" has no valid selectedPlaceVarName");
            return false;
        }
        if (mode==nnn::RuntimeMode::SelectPlace2 &&
            std::none_of(setup.choices.begin(),setup.choices.end(),[&](const auto& place){
                return place_state(setup,place)==2;
            })) {
            warn("SELECTPLACE2 has no selectable places");
            return false;
        }
        return true;
    }
    bool can_choose_place(const PlaceConfig& setup,std::int64_t choice) {
        return choice>0 && static_cast<std::uint64_t>(choice)<=setup.choices.size() &&
            place_state(setup,setup.choices[static_cast<std::size_t>(choice)-1U])==2;
    }
    void commit_place(const PlaceConfig& setup,nnn::Value choice) {
        SetVarData(setup.result,choice);
        SetVarData(setup.hover,choice);
        playback.restart_loop_script_audio();
    }
    void begin_place(nnn::RuntimeMode mode,const PlaceConfig& setup) {
        if (!check_place(mode,setup)) return;
        location();
        SetVarData(setup.hover,0);
        shared_runtime->set_skip_active(false);
        ClearSkipFlags();
        playback.suspend_script_audio();
        const auto current=playback.state().bgm;
        const auto& music=setup.music;
        if (music.number>0 && (music.restart || !current.active || playback.music_number(current.name)!=music.number)) {
            nnn::SystemEvent cue;
            cue.opcode=nnn::audio::music;
            cue.parameters={music.number,music.plays,music.volume,music.fade,music.restart?0:-1};
            const auto error=playback.submit(cue,frame.script,frame.word);
            if (!error.empty()) warn(error);
        }
        frame.wait=Wait::Place;
        set_mode(mode);
    }

    void apply_next_window() {
        if (next_window==0 || next_window==1) frame.dialogue.window_visible=next_window==1;
        next_window=-1;
    }

    bool ReceiveSystemEvent(const nnn::SystemEvent& e) override {
        const auto& a=e.parameters;
        if (e.opcode==set_term || e.opcode==nnn::audio::set_voice_flag) {
            if (!a.empty()) {
                const bool changed=e.opcode==set_term ? shared_runtime->set_term_unlocked(a[0]) :
                    shared_runtime->set_voice_played(a[0]);
                if (changed && replay_films.empty()) system_save_requested=true;
            }
            if (e.opcode==set_term) return true;
        }
        if (e.payload==nnn::SystemPayload::Dialogue) {
            location();
            auto& d=frame.dialogue;
            const bool appending=e.opcode==append && d.layout_command!=0;
            d.command=e.opcode; d.script=frame.script; ++d.sequence;
            if (!appending) d.layout_command=e.opcode==lprint ? lprint : print;
            d.table_index=a.empty() ? 0 : static_cast<unsigned>(a.front());
            d.message_id=identity(e.primary);
            if (!ui_mode) {
                shared_runtime->check_dialogue_skip(d.message_id);
                sync_skip();
            }
            d.raw_bytes=e.text.value_or("");
            append_prefix_bytes=e.opcode==append && d.layout_command==print ? d.display_bytes.size() : 0;
            if (e.opcode==append) d.display_bytes+=replace_dialogue(d.raw_bytes);
            else d.display_bytes=replace_dialogue(d.raw_bytes);
            d.speaker=speaker(d.display_bytes);
            d.window_type=window_type; d.font_type=font_type;
            d.window_visible=d.layout_command==print;
            apply_next_window();
            jump_pending=false;
            if (!ui_mode && !d.display_bytes.empty() && d.display_bytes.size()<=16384U) {
                const bool joined=e.opcode==append && !backlog.empty() && backlog.back().color<0;
                if (joined) {
                    backlog.back().speaker=d.speaker;
                    backlog.back().text=d.display_bytes;
                    for (std::size_t i=0;i<backlog_voices.size();++i)
                        if (!backlog_voices[i].empty()) backlog.back().voices[i]=std::move(backlog_voices[i]);
                } else {
                    remember({d.speaker,d.display_bytes,std::move(backlog_voices)});
                }
                jump_pending=jump_enabled && replay_films.empty() && (e.opcode!=append || append_jump);
            }
            backlog_voices={};
            begin_line(message_has_voice,message_voice_ms,next_message_speed,appending);
            next_message_speed=0;
            return true;
        }
        if (nnn::audio::is_audio_command(e.opcode)) {
            if (decorative_preview) return true;
            location();
            const auto flags=GetSkipFlags();
            if (e.opcode==nnn::audio::voice && !a.empty() && a[0]>=0 && e.text &&
                (skip_voice_roles || (!flags.skip_next_command && !shared_runtime->skip_active()))) {
                const int character=playback.voice_character_number(*e.text);
                if (shared_runtime->mark_voice_role_heard(character) && replay_films.empty()) system_save_requested=true;
            }
            if (!ui_mode && e.opcode==nnn::audio::voice && a.size()>=2 && a[0]>=0 && a[1]>=0 && a[1]<4 &&
                (a.size()<4 || a[3]==0) && e.text && e.text->size()<=255U)
                backlog_voices[static_cast<unsigned>(a[1])]=*e.text;
            if (e.opcode==nnn::audio::music && !a.empty() && a[0]>0 &&
                (skip_music || !flags.skip_next_command || !flags.skip_effect_command))
                shared_runtime->unlock_gallery(nnn::GalleryKind::Music,0,static_cast<nnn::Word>(a[0]-1));
            bool voice_started=false;
            const auto error=playback.submit(e,frame.script,frame.word,flags,&voice_started);
            if (!error.empty()) warn("Audio "+frame.script+" at "+std::to_string(frame.word)+": "+error);
            if (error.empty() && e.opcode==nnn::audio::voice && a.size()>=2 &&
                a[0]>=0 && a[1]>=0 && a[1]<2 && !GetSkipFlags().skip_next_command) {
                message_has_voice=true;
                message_voice_ms=playback.voice_duration_ms(static_cast<unsigned>(a[1]));
                if (voice_started) { speaking_ms=message_voice_ms;renderer->set_speaking(speaking_ms!=0); }
            }
            if (queue_audio_events) audio.push_back({frame.script,frame.word,e});
            ++counters.audio_events;
            return true;
        }
        if (e.opcode==load || e.opcode==preload) {
            if (slot(e.primary)) {
                const auto layer=static_cast<unsigned>(e.primary);
                if (const auto* tags=ui_preview_tags();tags && !(*tags)[layer].empty()) return true;
                const auto name=e.text.value_or("");
                const auto variable=layer_variables[layer];
                std::shared_ptr<const render::Texture> image;
                if (e.opcode==load && variable>0) {
                    const auto variant=GetVarData(static_cast<nnn::Word>(variable));
                    if (variant>1) {
                        const auto alternate=name+"__"+std::to_string(variant);
                        image=texture(alternate);
                        if (!image && require_variant_graphic) {
                            warn("Missing layer variant "+alternate);
                            return true;
                        }
                    }
                }
                if (!image) image=texture(name);
                if (image) renderer->bind(layer,std::move(image),e.opcode==preload);
            }
            return true;
        }
        switch (e.opcode) {
        case set_demo_flag:
            if (!a.empty()) {
                frame.demo_mode=a[0]==2?0:a[0];
                shared_runtime->set_system_parameter(nnn::NNNPARAM_AUTOMODE,frame.demo_mode);
            }
            return true;
        case frame_control:
            if (!a.empty() && a[0]>=0) set_period(a[0]?static_cast<unsigned>(std::min(1000,a[0])):default_period);
            return true;
        case default_frame: set_period(default_period); return true;
        case film_name: frame.film_name=e.text.value_or("").substr(0,62); return true;
        case story_name: frame.story_name=e.text.value_or("").substr(0,62); return true;
        case film_type_time: return true; // The upstream callback reserves FILMTYPETIME.
        case config_mask: if (!a.empty() && a[0]!=0) frame.config_mask=std::max(0,a[0]); return true;
        case var_control_layer:
            if (a.size()>=2 && slot(a[0]) && a[1]>=0 && static_cast<unsigned>(a[1])<vars.size())
                layer_variables[static_cast<unsigned>(a[0])]=a[1];
            return true;
        case 0x33330054U: if (!a.empty()) window_type=a[0]; return true;
        case 0x33330056U: if (!a.empty()) frame.script_cursor=a[0]; return true;
        case 0x3333005fU: if (!a.empty()) font_type=a[0]; return true;
        case 0x3333005aU: if (!a.empty()) option_off=a[0]==-1 ? 0 : a[0]; return true;
        case 0x33330057U: case 0x33330058U: case 0x33330059U: {
            const int value=a.empty() || a[0]==-1 ? 0 : a[0];
            if (e.opcode==0x33330057U) auto_message=value;
            else if (e.opcode==0x33330058U) cannot_click=value;
            else cannot_skip=value;
            if (!ui_mode && ((auto_message>0 && auto_message_kill_shift && e.opcode==0x33330057U) ||
                             (cannot_skip>0 && cannot_skip_kill_shift && e.opcode==0x33330059U)))
                shared_runtime->set_skip_active(false);
            sync_skip(); return true;
        }
        case start_message:
            if (a.size()==1) next_window=-1;
            if (a.size()>1 && a[1]>=0 && a[1]<=2) next_window=a[1]-1;
            message_has_voice=false; message_voice_ms=0;
            backlog_voices={};
            if (!ui_mode && !a.empty() && a[0]!=-1) {
                shared_runtime->check_dialogue_skip(identity(a[0]));
                sync_skip();
            }
            return true;
        case start_koma:
            if (!a.empty()) {
                next_message_type=a.size()>1 ? a[1] : default_next_message_type;
                // STARTKOMA sets the current flag now and supplies the raw
                // pending mode, which STARTMESSAGE can replace before DRAW.
                next_window=a.size()>2 ? a[2] : -1;
                frame.dialogue.window_visible=next_window==2 || (next_window!=1 && next_message_type==0);
            }
            if (!ui_mode && !a.empty() && a[0]!=-1) {
                const auto flags=GetSkipFlags();
                if (flags.skip_next_command && flags.skip_effect_command &&
                    shared_runtime->get_system_parameter(nnn::NNNPARAM_SKIPMODE)==nnn::NNNSKIPMODE_NORMAL &&
                    !shared_runtime->is_read(identity(a[0]))) effect_gate=true;
                sync_skip();
            }
            return true;
        case 0x3333006fU:
            next_message_speed=a.empty() ? 0 : std::clamp(a[0],0,5);
            return true;
        case 0x33330017U: frame.dialogue.window_visible=true; return true;
        case rect:
            if (a.size()>=9 && slot(a[0])) {
                auto bounds=[&](std::size_t n) {
                    return render::Rect{std::clamp(a[n],-1000000,1000000),std::clamp(a[n+1],-1000000,1000000),
                                        std::clamp(a[n+2],0,1000000),std::clamp(a[n+3],0,1000000)};
                };
                renderer->rectangle(static_cast<unsigned>(a[0]),bounds(1),bounds(5));
            }
            return true;
        case effect:
            if (a.size()>=2 && slot(a[0])) {
                std::vector<nnn::Value> parameters(a.begin()+2,a.end());
                const auto mesh=a[1]==81 ? fragment_mesh(parameters) : std::optional<render::FragmentMesh>{};
                if (!renderer->effect(static_cast<unsigned>(a[0]),a[1],std::move(parameters),mesh ? &*mesh : nullptr))
                    warn("Effect "+std::to_string(a[1])+" uses basic alpha drawing");
            }
            return true;
        case clear_all: renderer->clear_all(); layer_variables.fill(0); return true;
        case clear: if (!a.empty() && slot(a[0])) renderer->clear(static_cast<unsigned>(a[0])); return true;
        case prepare: renderer->prepare_overlap(); return true;
        case overlap: {
            if (frame.dialogue.layout_command) frame.dialogue.layout_command=print;
            location();
            const int type=a.empty()?0:a[0];
            if (!renderer->start_overlap(static_cast<unsigned>(a.size()>1?std::max(0,a[1]):0),type,overlap_decoration()))
                warn("Unsupported wipe "+std::to_string(type)+" uses crossfade");
            ++counters.overlaps; return true;
        }
        case draw:
            if (frame.dialogue.layout_command) frame.dialogue.layout_command=print;
            apply_next_window();
            location(); draw_remaining=static_cast<unsigned>(a.empty() ? 0 : std::max(0,a[0]));
            ++counters.draws; return true;
        case select:
            frame.dialogue.window_visible=false;
            location(); return true;
        case set_cg: cg(a); return true;
        case set_film: film(a); return true;
        case set_cg_by_var:
            if (a.size()==5 && a[3]>=0 && static_cast<unsigned>(a[3])<vars.size()) {
                const auto value=GetVarData(static_cast<nnn::Word>(a[3]));
                if (a[4]==0 || a[4]==1 ? value==0 || value==1 : value==a[4]) cg(a);
            }
            return true;
        default: return false;
        }
    }
    void UnhandledInstruction(nnn::WordIndex,nnn::Word,nnn::Word op) override {
        std::ostringstream s; s<<"Unhandled command 0x"<<std::hex<<op; warn(s.str());
    }
    void ReceiveScriptCommand(nnn::Word,nnn::Value,const std::string&,nnn::Value) override {}
    void PreReceiveScriptData(nnn::Word,nnn::Value,const std::string&) override {}
    void ReceiveUserCommand0(nnn::Word command,const std::vector<nnn::Value>& args) override {
        if (ui_mode) return;
        if (select_place.command==command) { begin_place(nnn::RuntimeMode::SelectPlace,select_place);return; }
        if (select_place2.command==command) { begin_place(nnn::RuntimeMode::SelectPlace2,select_place2);return; }
        if (const auto found=modal_commands.find(command);found!=modal_commands.end()) {
            begin_modal(found->second,args);return;
        }
        if (movie_command!=command) { warn("Unsupported user command "+std::to_string(command));return; }
        const int number=args.empty()?1:args.front();
        if (number<1 || static_cast<std::size_t>(number)>movie_count) {
            warn("Invalid PRINTMOVIE index "+std::to_string(number));
            return;
        }
        location();
        shared_runtime->unlock_gallery(nnn::GalleryKind::Movie,0,static_cast<nnn::Word>(number-1));
        shared_runtime->set_skip_active(false);
        ClearSkipFlags();
        playback.stop_script_audio();
        playback.stop_music_preview();
        frame.movie=static_cast<unsigned>(number);
    }
    void hide_layers(std::uint32_t bits,bool hidden) {
        const auto previous=renderer->hidden_layers();
        renderer->hide_layers(hidden ? previous|bits : (previous&~bits)|hidden_layers);
    }
    void ReceiveUserFunction0(nnn::Word id,const std::vector<nnn::Value>& args) override {
        std::string name;
        registry.GetName(id,name);
        name=fold_cp932(std::move(name));
        if (name=="stopf5") {
            if (!ui_mode) shared_runtime->set_skip_active(false);
            ClearSkipFlags();
        } else if (name=="savesystemfile") {
            if (!ui_mode && replay_films.empty()) system_save_requested=true;
        } else if (name=="layer") {
            if (!args.empty() && slot(args[0]))
                hide_layers(std::uint32_t{1}<<args[0],args.size()>1 && args[1]==0);
        } else if (name=="notice") {
            if (!ui_mode && script_notices && !args.empty() && args[0]>0)
                pending_notice=static_cast<unsigned>(args[0]);
        } else if (!input.legacy_calculations && zukan_function==id) {
            if (!args.empty()) zukan->unlock(reference_read(),reference_write(),args[0]);
        } else if (!input.legacy_calculations && builtin_user_function(name,args)) {
            return;
        } else if (!ui_mode && replay_films.empty() && !input.legacy_calculations && !input.recorded_clock &&
                   chart_function(id,name,args)) {
            return;
        } else {
            warn("Unsupported user function "+(name.empty()?std::to_string(id):name));
        }
    }
    std::string local_replacement(std::size_t index,const nnn::PreviewStoryState* story=nullptr) const {
        const auto& table=story ? story->replacements : replacements;
        if (index<table.size()) return table[index];
        return shared_runtime->local_replacement_defaults()[index];
    }
    bool builtin_user_function(const std::string& name,const std::vector<nnn::Value>& args) {
        if (name=="adjustdate") {
            if (date_id!=variable_count) {
                const auto date=GetVarData(date_id);
                const auto month=date/100,day=date%100;
                if (month<0 || month>=static_cast<int>(days_of_month.size())) warn("adjustdate: invalid month");
                else if (day>days_of_month[static_cast<std::size_t>(month)])
                    SetVarData(date_id,(month+1)*100+day-days_of_month[static_cast<std::size_t>(month)]);
            }
        } else if (name=="check" || name=="checksys") {
            const auto from=args.size()>1 ? args[0] : 0;
            const auto to=args.empty() ? 0 : (args.size()>1 ? args[1] : args[0]);
            bool same=false;
            if (from>=0 && from<100 && to>0 && static_cast<std::size_t>(to)<=answer_strings.size()) {
                const auto index=static_cast<std::size_t>(from);
                same=(name=="check" ? local_replacement(index) : shared_runtime->system_replacement(index))==
                    answer_strings[static_cast<std::size_t>(to-1)];
            }
            SetVarData(compare_result_var,same ? 1 : 0);
        } else if (name=="setresult" && result_enabled) {
            if (!args.empty() && shared_runtime->set_result_if_clear(args[0],args.size()>1 ? args[1] : 1)) {
                if (replay_films.empty()) system_save_requested=true;
                if (script_notices && !ui_mode && auto_result_notice>0) {
                    const auto notice=static_cast<std::int64_t>(args[0])+auto_result_notice-1;
                    if (notice>0 && notice<=INT32_MAX) pending_notice=static_cast<unsigned>(notice);
                }
            }
        } else if (name=="getresult" && result_enabled && get_result_var!=variable_count) {
            if (!args.empty())
                SetVarData(get_result_var,shared_runtime->result_value(args[0])>=(args.size()>1 ? args[1] : 1) ? 1 : 0);
        } else if (name=="setachievement") {
            if (achievement_available && !achievement_catalog && prepare_achievements())
                achievement_catalog->entries(*shared_runtime);
            shared_runtime->set_achievement(args.empty() ? 0 : args[0],args.size()>1 ? args[1] : 100);
            if (replay_films.empty()) system_save_requested=true;
        } else {
            return false;
        }
        return true;
    }
    bool system_variable(unsigned id) const { return id>=first_system_variable && id<system_variable_end; }
    void load_variable_defaults(decltype(vars)& target,nnn::SystemVariables* legacy_system=nullptr) const {
        const auto values=fields(file_text(assets.config(system_variable_end==1200 ? "varinitdata2.xtx" : "varinitdata.xtx")));
        for (std::size_t i=0;i<values.size() && i<target.size();++i) {
            if (!system_variable(static_cast<unsigned>(i))) target[i]=number(values[i]);
            else if (legacy_system) (*legacy_system)[i-first_system_variable]=number(values[i]);
        }
    }
    nnn::Value variable_value(nnn::Word id) const {
        if (system_variable(id)) return shared_runtime->system_variable(id-first_system_variable);
        return id<vars.size() ? vars[id] : 0;
    }
    nnn::Value GetVarData(nnn::Word id) override { return variable_value(id); }
    int script_year() { return year_id==variable_count ? 2000 : GetVarData(year_id); }
    ScriptClockSample clock_value(bool script) {
        if (script && sampled_clock) return *sampled_clock;
        if (script && input.recorded_clock && !input.clock_sample)
            throw std::runtime_error("The input journal is missing a system clock sample");
        const auto sample=script && input.clock_sample ? *input.clock_sample :
            clock_source ? clock_source() : local_clock_now();
        if (!valid_clock_sample(sample)) throw std::runtime_error("Invalid script clock sample");
        if (script) sampled_clock=sample;
        return sample;
    }
    int cg_completion() {
        if (!cg_counts) {
            try { cg_counts=read_cg_counts(shared_runtime->root()); }
            catch (const std::exception& e) { warn(e.what()); cg_counts.emplace(); }
        }
        const auto revision=shared_runtime->gallery_revision();
        if (cg_revision==revision) return cg_percent;
        const auto keys=shared_runtime->snapshot_gallery();
        std::size_t total=0,unlocked=0;
        for (const auto count:*cg_counts) total+=count;
        for (const auto key:keys) {
            if ((key>>28U)!=static_cast<nnn::Word>(nnn::GalleryKind::Cg)) continue;
            const auto group=(key>>16U)&0xfffU,index=key&0xffffU;
            if (group<cg_counts->size() && index<(*cg_counts)[group]) ++unlocked;
        }
        cg_revision=revision;
        return cg_percent=completion_percent(unlocked,total);
    }
    int scene_completion() {
        if (!scene_list_enabled) return 0;
        if (!scene_requirements) {
            try { scene_requirements=read_scene_requirements(shared_runtime->root()); }
            catch (const std::exception& e) { warn(e.what()); scene_requirements.emplace(); }
        }
        const auto revision=shared_runtime->gallery_revision();
        if (scene_revision==revision) return scene_percent;
        const auto keys=shared_runtime->snapshot_gallery();
        const auto unlocked=std::count_if(scene_requirements->begin(),scene_requirements->end(),[&](const auto& films) {
            return std::all_of(films.begin(),films.end(),[&](auto film){return std::binary_search(keys.begin(),keys.end(),film);});
        });
        scene_revision=revision;
        return scene_percent=completion_percent(static_cast<std::size_t>(unlocked),scene_requirements->size());
    }
    nnn::Value calculated_variable(nnn::Word id,bool script,const nnn::PreviewStoryState* story=nullptr) {
        if (id&0x8000U) return static_cast<int>(id&0x7fffU);
        if (id==mode_id) return ui_mode ? *ui_mode : shared_runtime->mode();
        if (id==x_id) return input.mouse_x;
        if (id==y_id) return input.mouse_y;
        if (id==trigger_id) return ui_mode ? input.held || input.advance : input.advance || input.choice!=0;
        if (id==click_id) return ui_mode ? input.advance : input.held || input.advance;
        const auto alias=extended_variables.find(id);
        if (alias!=extended_variables.end()) return shared_runtime->get_system_parameter(alias->second);
        if (id>=variable_count || (script && input.legacy_calculations)) return GetVarData(id);
        if (id==random_id) {
            auto state=random_state;
            return pc_random(script ? random_state : state);
        }
        if (id==week_id || id==holiday_id) {
            const auto year=script_year(), date=GetVarData(date_id);
            const auto week=weekday(year,date);
            if (id==week_id) return week;
            return week==0 || week==6 ||
                std::find(holidays.begin(),holidays.end(),std::array<int,3>{year,date/100,date%100})!=holidays.end();
        }
        const auto clock=std::find(clock_ids.begin(),clock_ids.end(),id);
        if (clock!=clock_ids.end())
            return local_clock_fields(clock_value(script))[static_cast<std::size_t>(clock-clock_ids.begin())];
        if (id==direct3d_id) return 2;
        if (id==pixel_shader_id || id==vertex_shader_id) return 0;
        if (id==cg_percent_id) return cg_completion();
        if (id==scene_percent_id) return scene_completion();
        if (story && !system_variable(id)) return story->vars[id];
        return GetVarData(id);
    }
    nnn::Value GetCalcuVarData(nnn::Word id) override { return calculated_variable(id,true); }
    void SetVarData(nnn::Word id,nnn::Value value) override {
        if (system_variable(id)) shared_runtime->set_system_variable(id-first_system_variable,value);
        else if (id<vars.size() && vars[id]!=value) {
            vars[id]=value;
            preview_story_dirty=true;
        }
        update_variable_layers(id,value);
    }
    void update_variable_layers(nnn::Word id,nnn::Value value) {
        if (const auto layer=layer_visibility.find(id);layer!=layer_visibility.end()) hide_layers(layer->second,value!=0);
    }
    void SetCalcuVarData(nnn::Word id,nnn::Value value) override {
        if (id==mode_id) {
            if (ui_mode) ui_mode=value;
            else shared_runtime->set_mode(value);
        }
        SetVarData(id,value);
    }

    void restore_pc_audio(const pc::Status& status,bool best_effort=false) {
        const auto submit=[&](const nnn::SystemEvent& cue) {
            const auto error=cue.opcode==nnn::audio::voice ? playback.submit_pc_voice(cue,frame.script,frame.word) :
                playback.submit(cue,frame.script,frame.word);
            if (!error.empty()) {
                if (!best_effort) throw std::runtime_error("PC save audio: "+error);
                warn("SELECTCHART restored audio: "+error);
            }
        };
        if (status.bgm_number>=0) {
            if (status.bgm_number==INT32_MAX || status.bgm_plays<0 || status.bgm_volume<0 || status.bgm_volume>100)
                throw std::runtime_error("Invalid PC save BGM");
            const auto volume=std::clamp(shared_runtime->get_system_parameter(nnn::NNNPARAM_MUSICVOLUME),0,100);
            nnn::SystemEvent cue;
            cue.opcode=nnn::audio::music;
            cue.parameters={status.bgm_number+1,status.bgm_plays,status.bgm_volume-volume,0,0};
            submit(cue);
        } else if (best_effort) {
            nnn::SystemEvent cue;
            cue.opcode=nnn::audio::music;
            cue.parameters={-1,0};
            submit(cue);
        }
        const auto sample=[&](const auto& work,std::size_t channel,bool voice,const std::string& name) {
            nnn::SystemEvent cue;
            cue.opcode=voice ? nnn::audio::voice : nnn::audio::sound;
            if (voice) cue.text=name;
            cue.parameters={std::max(0,work[1]),static_cast<nnn::Value>(channel),work[2],work[0],
                            work[3],work[4],work[5],work[6],work[7],work[8],work[9],work[10]};
            submit(cue);
        };
        for (std::size_t i=0;i<status.loop_sound.size();++i)
            if (status.loop_sound[i][0]) sample(status.loop_sound[i],i,false,{});
        for (std::size_t i=0;i<status.voice_exist_count.size();++i)
            if (!status.loop_voice_names[i].empty() &&
                (status.loop_voice[i][0] || (status.voice_exist_count[i] && (jump_voice_mask&(1U<<i)))))
                sample(status.loop_voice[i],i,true,status.loop_voice_names[i]);
    }
    std::array<nnn::PcScriptSlot,nnn::script_mode_count> pc_positions(const pc::Status& status) const {
        std::array<nnn::PcScriptSlot,nnn::script_mode_count> positions;
        positions[0].path="spt/sys.spt";
        for (std::size_t i=0;i<=static_cast<std::size_t>(status.run_mode);++i) {
            auto& position=positions[i];
            const auto& slot=status.vm[i];
            if (i!=0) {
                const auto index=static_cast<std::size_t>(slot.script_number);
                if (index>=project_scripts.size()) throw std::runtime_error("PC save script is outside the project table");
                position.path="spt/"+project_scripts[index]+".spt";
            }
            position.pc=static_cast<nnn::WordIndex>(slot.pc);
            for (int j=0;j<slot.stack_pointer;++j)
                position.stack.push_back(static_cast<nnn::Word>(slot.call_stack[static_cast<std::size_t>(j)]));
            position.script_call=static_cast<nnn::Word>(slot.call_stack[253]);
            position.command_call=static_cast<nnn::Word>(slot.call_stack[254]);
        }
        return positions;
    }
    nnn::Value save_slot_art() const {
        return save_slot_art_var<variable_count ? variable_value(static_cast<nnn::Word>(save_slot_art_var)) : pc_save_slot_art;
    }
    void capture_pc_status(pc::Status& status,nnn::RuntimeMode mode) const {
        status.saved_at=current_pc_time();
        status.game_mode=static_cast<int>(mode);
        status.run_mode=static_cast<int>(vm->ActiveMode());
        status.frame_ms=static_cast<int>(period);status.remaining=30;
        status.last_heroine=last_heroine;
        status.window_number=window_type;status.font_type=font_type;status.window_visible=frame.dialogue.window_visible;
        status.cursor_number=frame.script_cursor;
        status.config_mask=frame.config_mask;status.layer_off=renderer->hidden_layers();
        status.auto_message=auto_message;status.cannot_click=cannot_click;status.cannot_skip=cannot_skip;
        status.surname=surname;status.given_name=given_name;
        const auto audio=playback.pc_state();
        status.bgm_number=audio.music_number;status.bgm_plays=audio.music_plays;status.bgm_volume=audio.music_volume;
        status.loop_sound=audio.sounds;status.loop_voice=audio.voices;
        for (std::size_t i=0;i<audio.voice_names.size();++i) {
            status.loop_voice_names[i]=audio.voice_names[i];
            status.voice_exist_count[i]=audio.voice_names[i].empty()?0:1;
        }
        for (unsigned i=0;i<pc_layers;++i) {
            const auto binding=layer_variables[i];
            if (binding<0 || binding>(pc_layers==32?127:32767)) throw std::runtime_error("Layer binding exceeds the PC save layout");
            status.layer_variables[i%16]|=static_cast<std::uint16_t>(static_cast<unsigned>(binding)<<(i>=16?8U:0U));
        }
    }
    pc::Status make_chart_status(nnn::RuntimeMode mode) const {
        pc::Status status;
        capture_pc_status(status,mode);
        status.save_count=1;
        const bool adjusted=vm->pc_adjustment_ids();
        status.adjust_version=adjusted ? 1 : 0;
        const auto wait=mode==nnn::RuntimeMode::Title ? nnn::PcScriptWait::Raw : nnn::PcScriptWait::Command;
        std::array<nnn::PcScriptSlot,nnn::script_mode_count> positions;
        const auto result=vm->capture_pc_positions(positions,adjusted,wait);
        if (!result) throw std::runtime_error(result.detail);
        if (positions[0].path!="spt/sys.spt") throw std::runtime_error("Chart checkpoint requires the game control script");
        for (std::size_t i=0;i<=static_cast<std::size_t>(status.run_mode);++i) {
            const auto& position=positions[i];
            auto& slot=status.vm[i];
            if (position.pc>INT32_MAX || position.stack.size()>=250U)
                throw std::runtime_error("Chart script position exceeds the PC save format");
            if (i!=0) {
                const auto id=script_ids.find(stem(position.path));
                if (id==script_ids.end()) throw std::runtime_error("Chart script is outside the PC project table");
                slot.script_number=static_cast<int>(id->second);
            }
            slot.pc=static_cast<int>(position.pc);
            slot.stack_pointer=static_cast<int>(position.stack.size());
            for (std::size_t j=0;j<position.stack.size();++j) {
                if (position.stack[j]>INT32_MAX) throw std::runtime_error("Invalid chart SUB call frame");
                slot.call_stack[j]=static_cast<int>(position.stack[j]);
            }
            slot.call_stack[253]=position.script_call<=INT32_MAX ? static_cast<int>(position.script_call) : -1;
            slot.call_stack[254]=position.command_call<=INT32_MAX ? static_cast<int>(position.command_call) : -1;
        }
        return status;
    }
    bool restore_chart_node(const nnn::chart::Branch& branch) {
        std::optional<ChartNodeSave> node;
        std::unique_ptr<nnn::ScriptScheduler> next_vm;
        std::unique_ptr<render::Layers> next_renderer;
        std::vector<std::string> title_replacements;
        const auto title=chart_title_story;
        const auto checkpoint=branch.restore_node ? branch.to : branch.from;
        const auto slot=variable_id(chart_graph->setup().slot_var);
        try {
            node=chart_store->read(checkpoint,*chart_graph);
            if (!node) throw std::runtime_error("Node checkpoint is missing");
            const auto& status=node->status;
            const auto mode=static_cast<nnn::RuntimeMode>(status.game_mode);
            if (mode!=nnn::RuntimeMode::SelectChart && mode!=nnn::RuntimeMode::Title)
                throw std::runtime_error("Invalid chart checkpoint mode");
            if ((nnn::chart::restores_variable(slot) && node->vars[slot]!=checkpoint) ||
                status.frame_ms<0 || status.frame_ms>1000 ||
                (status.bgm_number>=0 && (status.bgm_number==INT32_MAX || status.bgm_plays<0 ||
                 status.bgm_volume<0 || status.bgm_volume>100)) ||
                !nnn::valid_runtime_text(status.surname,nnn::runtime_name_bytes) ||
                !nnn::valid_runtime_text(status.given_name,nnn::runtime_name_bytes))
                throw std::runtime_error("Invalid chart checkpoint values");
            const auto wait=mode==nnn::RuntimeMode::Title ? nnn::PcScriptWait::Raw : nnn::PcScriptWait::Command;
            next_vm=make_scheduler();
            const auto restored=next_vm->restore_pc_positions(static_cast<nnn::ScriptMode>(status.run_mode),
                pc_positions(status),status.adjust_version!=0,wait,0,false);
            if (!restored) throw std::runtime_error(restored.detail);
            if (title) {
                next_renderer=make_renderer(static_cast<int>(renderer->width()),static_cast<int>(renderer->height()));
                title_replacements=title->replacements;
            }
        } catch (const std::exception& e) {
            warn("SELECTCHART could not resume node "+std::to_string(checkpoint)+": "+e.what());
            return false;
        }
        if (title) {
            renderer.swap(next_renderer);
            vars=title->vars;
            replacements.swap(title_replacements);
            ui_mode.reset();ui_script.clear();ui_click.reset();
            input={};sampled_clock.reset();
            initial_calendar=false;
            frame.end_reason=EndReason::None;frame.error.clear();
            frame.dialogue={};
            frame.remaining_frames=draw_remaining=remainder=0;
            frame.movie=0;frame.demo_mode=0;
            frame.film_name.clear();frame.story_name.clear();
            option_off=next_message_speed=0;next_window=-1;
            next_message_type=default_next_message_type;
            backlog.clear();backlog_voices={};clear_jumps();
        }
        frame.selection.reset();
        frame.choice_limit_ms=frame.choice_remaining_ms=choice_appear_remaining=0;
        frame.choice_timed_out=false;
        speaking_ms=message_voice_ms=0;message_has_voice=false;
        renderer->set_speaking(false);
        const auto& status=node->status;
        vm.swap(next_vm);
        location();
        chart_run=std::move(node->run);
        shared_runtime->set_chart_selection(chart_graph->slot_capacity(),chart_run->can_select);
        renderer->hide_layers(status.layer_off);
        for (unsigned i=0;i<pc_layers;++i) {
            const auto packed=static_cast<unsigned>(status.layer_variables[i%16]);
            const auto binding=pc_layers==16 ? packed : (packed>>(i>=16?8U:0U))&255U;
            layer_variables[i]=binding<=(pc_layers==16 ? 32767U : 127U) ? static_cast<nnn::Value>(binding) : 0;
        }
        const auto work=variable_id(chart_graph->setup().work_var);
        if (branch.restore_node) {
            SetVarData(work,branch.from);
            SetVarData(slot,branch.to);
        }
        nnn::chart::restore_variables(node->vars,[&](std::size_t i,nnn::Value value) {
            const auto id=static_cast<nnn::Word>(i);
            if (system_variable(id) && GetVarData(id)!=value) system_save_requested=true;
            SetVarData(id,value);
        });
        if (!branch.restore_node) {
            SetVarData(work,branch.from);
            SetVarData(slot,branch.to);
        }
        if (status.frame_ms>0) set_period(static_cast<unsigned>(status.frame_ms));
        last_heroine=status.last_heroine>=0 && status.last_heroine<heroine_limit ? status.last_heroine : 0;
        frame.config_mask=status.config_mask;
        frame.dialogue.window_visible=status.window_visible;
        window_type=std::max(0,status.window_number);font_type=std::max(0,status.font_type);
        frame.script_cursor=status.cursor_number;
        auto_message=status.auto_message;cannot_click=status.cannot_click;cannot_skip=status.cannot_skip;
        surname=status.surname;given_name=status.given_name;
        const auto global=shared_runtime->runtime_text();
        if (surname!=global.surname || given_name!=global.given_name) {
            shared_runtime->set_system_name(surname,given_name);
            system_save_requested=true;
        }
        if (branch.restore_node && clear_auto_after_load) {
            shared_runtime->set_system_parameter(nnn::NNNPARAM_AUTOMODE,0);
            shared_runtime->set_skip_active(false);
            ClearSkipFlags();
        }
        preview_story_dirty=true;
        playback.stop_script_audio();
        restore_pc_audio(status,true);
        return true;
    }
    void load_pc_save(const std::vector<nnn::Byte>& bytes) {
        const auto saved=pc::parse_save(bytes,pc_save_header,replacement_extension);
        if (saved.log) { pc_log_layout=saved.log->layout;pc_log_voices=saved.log->voice_count; }
        if (!saved.unsupported_blocks.empty())
            throw std::runtime_error("Unsupported PC save block "+std::to_string(saved.unsupported_blocks.front().header.code));
        if (saved.info.version<0 || static_cast<unsigned>(saved.info.version)>game_version ||
            (static_cast<unsigned>(saved.info.version)>>8U)!=(game_version>>8U))
            throw std::runtime_error("PC save version differs from this game");
        const bool relocate=static_cast<unsigned>(saved.info.version)<game_version;
        const auto& status=saved.status;
        const auto mode=static_cast<nnn::RuntimeMode>(status.game_mode);
        if (mode!=nnn::RuntimeMode::PrintMessage && mode!=nnn::RuntimeMode::SelectMessage)
            throw std::runtime_error("Unsupported PC save mode "+std::to_string(status.game_mode));
        const bool dialogue=mode==nnn::RuntimeMode::PrintMessage;
        const auto active=static_cast<std::size_t>(status.run_mode);
        const auto positions=pc_positions(status);
        const auto id=static_cast<nnn::Word>(dialogue ? status.last_message_id : status.last_select_id);
        const auto waiting=dialogue ? nnn::PcScriptWait::Dialogue : nnn::PcScriptWait::Selection;
        const auto result=vm->restore_pc_positions(static_cast<nnn::ScriptMode>(active),positions,status.adjust_version!=0,waiting,id,relocate);
        if (!result) throw std::runtime_error(result.detail);
        location();

        const bool extended=system_variable_end==1200;
        const auto local_end=extended ? 2200U : 1000U;
        if (saved.vars.size()!=(extended ? 2224U : 1024U)) throw std::runtime_error("PC save variable layout differs from this game");
        std::copy_n(saved.vars.begin(),first_system_variable,vars.begin());
        std::copy(saved.vars.begin()+system_variable_end,saved.vars.begin()+local_end,vars.begin()+system_variable_end);
        pc_save_slot_art=status.remaining;
        last_heroine=status.last_heroine>=0 && status.last_heroine<heroine_limit ? status.last_heroine : 0;
        renderer->hide_layers(status.layer_off);
        for (unsigned i=0;i<pc_layers;++i) {
            const auto packed=static_cast<unsigned>(status.layer_variables[i%16]);
            const auto binding=pc_layers==16 ? packed : (packed>>(i>=16?8U:0U))&255U;
            const auto limit=pc_layers==16 ? 32767U : 127U;
            layer_variables[i]=binding<=limit ? static_cast<nnn::Value>(binding) : 0;
            const auto& source=saved.effects[i];
            if (!source.flag) continue;
            if (source.count<0 || source.count_max<0 || source.count>10000000)
                throw std::runtime_error("Invalid PC layer effect counter");
            render::Layer layer;
            layer.enabled=true;layer.opaque=source.not_transparent;
            layer.effect=source.command;
            layer.count=static_cast<unsigned>(source.count);
            layer.source=pc_rectangle(source.src);layer.from=pc_rectangle(source.dst_from);layer.to=pc_rectangle(source.dst_to);
            if (!saved.filenames[i].empty()) layer.texture=assets.image(saved.filenames[i],true);
            layer.parameters.assign(source.parameters.begin(),source.parameters.end());
            const auto mesh=layer.effect==81 ? fragment_mesh(layer.parameters) : std::optional<render::FragmentMesh>{};
            const auto effect=layer.effect;
            if (!renderer->restore_layer(i,std::move(layer),mesh ? &*mesh : nullptr))
                warn("PC save effect "+std::to_string(effect)+" uses basic alpha drawing");
        }
        if (status.frame_ms>0) {
            if (status.frame_ms>1000) throw std::runtime_error("Invalid PC frame time");
            set_period(static_cast<unsigned>(status.frame_ms));
        }
        frame.config_mask=status.config_mask;
        window_type=std::max(0,status.window_number);font_type=std::max(0,status.font_type);
        frame.script_cursor=status.cursor_number;
        auto_message=status.auto_message;cannot_click=status.cannot_click;cannot_skip=status.cannot_skip;
        replacements=saved.replacements;surname=status.surname;given_name=status.given_name;
        if (saved.log) for (const auto& old:saved.log->entries) {
            BacklogEntry line;
            line.text=replace_dialogue(old.text);
            line.color=static_cast<int>(old.color&0xffffffU);
            std::copy_n(old.voices.begin(),line.voices.size(),line.voices.begin());
            remember(std::move(line));
        }
        const auto* script=vm->GetVM(vm->ActiveMode());
        if (dialogue) {
            const auto message=script->saved_dialogue();
            if (!message || status.message_count<=0) throw std::runtime_error("Missing PC save message");
            auto& line=frame.dialogue;
            line.script=frame.script;line.raw_bytes=message->text;line.command=message->command;
            line.layout_command=status.message_sub_mode==static_cast<int>(lprint)?lprint:print;
            line.table_index=message->table_index;line.message_id=identity(static_cast<nnn::Value>(message->id&0xffffU));
            line.sequence=1;
            line.window_type=window_type;line.font_type=font_type;line.window_visible=status.window_visible;
            for (int i=0;i<status.message_count;++i) {
                if (i) line.display_bytes+="\r\n";
                line.display_bytes+=replace_dialogue(saved.messages[static_cast<std::size_t>(i)]);
            }
            const auto expected=compact_message(replace_dialogue(message->text));
            const auto actual=compact_message(line.display_bytes);
            if (expected.empty() || actual.find(expected)==std::string::npos)
                throw std::runtime_error("PC save dialogue does not match this installation");
            line.speaker=speaker(line.display_bytes);
            frame.word=message->word;
            frame.wait=Wait::Dialogue;
        } else {
            frame.selection=*script->PendingSelection();
            const auto option_count=static_cast<std::size_t>(status.select_count-status.select_add_count);
            if (option_count!=frame.selection->options.size()) throw std::runtime_error("PC save selection differs from the script");
            for (std::size_t i=0;i<option_count;++i) {
                const auto& original=saved.messages[i+static_cast<std::size_t>(status.select_add_count)];
                if (compact_message(original)!=compact_message(frame.selection->options[i]))
                    throw std::runtime_error("PC save option does not match this installation");
            }
            frame.selection->auto_select=status.auto_select;frame.selection->special=status.special_flag;
            frame.choice_limit_ms=static_cast<unsigned>(std::max(0,status.select_time_limit));
            frame.choice_remaining_ms=static_cast<unsigned>(std::clamp(status.select_time_count,0,status.select_time_limit>0?status.select_time_limit:0));
            frame.wait=Wait::Choice;
        }
        restore_pc_audio(status);
        const auto first_voice=std::max(playback.voice_duration_ms(0),playback.voice_duration_ms(1));
        message_has_voice=first_voice!=0;message_voice_ms=first_voice;
        begin_line(message_has_voice,message_voice_ms,0);
        set_mode(mode);
        shared_runtime->set_skip_active(false);
        shared_runtime->set_system_parameter(nnn::NNNPARAM_AUTOMODE,0);
        initial_calendar=false;
        renderer->surface();
        const auto global=shared_runtime->runtime_text();
        if (surname!=global.surname || given_name!=global.given_name) {
            if (!shared_runtime->set_system_name(surname,given_name)) throw std::runtime_error("Invalid PC save names");
            system_save_requested=true;
        }
        publish_preview_story();
    }

    pc::Save make_pc_save() {
        if (!pc_export_layout || ui_mode || !replay_films.empty() || (frame.wait!=Wait::Dialogue && frame.wait!=Wait::Choice))
            throw std::runtime_error("This game mode or save layout cannot be exported to the PC engine");
        pc::Save saved;
        saved.header.name=pc_save_header;saved.info.version=static_cast<int>(game_version);
        auto& status=saved.status;
        capture_pc_status(status,frame.wait==Wait::Dialogue ? nnn::RuntimeMode::PrintMessage : nnn::RuntimeMode::SelectMessage);
        status.remaining=save_slot_art();
        status.adjust_version=1;
        for (std::size_t i=0;i<=static_cast<std::size_t>(status.run_mode);++i) {
            auto& slot=saved.status.vm[i];
            const auto* script=vm->GetVM(static_cast<nnn::ScriptMode>(i));
            slot.pc=static_cast<int>(script->GetPC());
            if (i!=0) {
                const auto id=script_ids.find(stem(script->Identity()));
                if (id==script_ids.end()) throw std::runtime_error("Active script is outside the PC project table");
                slot.script_number=static_cast<int>(id->second);
            }
            slot.stack_pointer=static_cast<int>(script->GetCallDepth());
            for (int j=0;j<slot.stack_pointer;++j) {
                nnn::CallFrame call;
                if (!script->GetCallFrame(static_cast<std::size_t>(j),call) || call.id>INT32_MAX)
                    throw std::runtime_error("Invalid PC export SUB frame");
                slot.call_stack[static_cast<std::size_t>(j)]=static_cast<int>(call.id);
            }
            nnn::Word call=0;
            if (script->GetScriptCallSlot(call) && call<=INT32_MAX) slot.call_stack[253]=static_cast<int>(call);
            else if (i<static_cast<std::size_t>(status.run_mode)) throw std::runtime_error("Missing PC export SCRIPT slot");
        }
        const auto* script=vm->GetVM(vm->ActiveMode());
        std::vector<std::string> rows;
        if (frame.wait==Wait::Dialogue) {
            const auto message=script->saved_dialogue();
            if (!message || message->id>INT32_MAX) throw std::runtime_error("No exportable dialogue at the current script position");
            status.last_message_id=static_cast<int>(message->id);
            status.message_serial=static_cast<int>(frame.dialogue.message_id);
            status.message_sub_mode=static_cast<int>(frame.dialogue.layout_command);
            // A complete script message can be saved as original bytes, so PC
            // reads it with the currently active system substitutions.
            const auto& raw=frame.dialogue.raw_bytes;
            const bool raw_complete=frame.dialogue.command!=append && replace_dialogue(raw)==frame.dialogue.display_bytes;
            rows=message_rows(raw_complete ? raw : frame.dialogue.display_bytes);
            status.message_count=static_cast<int>(rows.size());
        } else {
            const auto id=script->saved_selection_id();
            if (!id || *id>INT32_MAX || !frame.selection) throw std::runtime_error("No exportable choice at the current script position");
            const auto& choice=*frame.selection;
            status.last_select_id=static_cast<int>(*id);status.select_serial=choice.serial;
            status.select_time_limit=static_cast<int>(frame.choice_limit_ms);
            status.select_time_count=static_cast<int>(frame.choice_remaining_ms);
            status.auto_select=choice.auto_select;status.special_flag=choice.special;
            rows=choice.messages;status.select_add_count=static_cast<int>(rows.size());
            rows.insert(rows.end(),choice.options.begin(),choice.options.end());
            status.select_count=static_cast<int>(rows.size());
        }
        if (rows.empty() || rows.size()>saved.messages.size()) throw std::runtime_error("PC save has too many message rows");
        std::move(rows.begin(),rows.end(),saved.messages.begin());
        saved.vars.assign(vars.begin(),vars.begin()+(system_variable_end==1200?2224:1024));
        std::fill(saved.vars.begin()+first_system_variable,saved.vars.begin()+system_variable_end,0);
        for (unsigned i=0;i<pc_layers;++i) {
            const auto& layer=renderer->slots()[i];
            if (!layer.enabled) continue;
            auto& effect=saved.effects[i];
            effect.flag=1;effect.picture=layer.texture&&layer.effect!=54?static_cast<int>(i):-1;effect.command=layer.effect;
            effect.count=static_cast<int>(layer.count);effect.count_max=static_cast<int>(renderer->duration(i));
            effect.previous_effect=effect.command;effect.not_transparent=layer.opaque;
            const auto rectangle=[](const render::Rect& r){return pc::Rect{r.x,r.y,r.width,r.height};};
            effect.src=effect.src_from=effect.src_to=rectangle(layer.source);
            effect.dst_from=rectangle(layer.from);effect.dst_to=rectangle(layer.to);
            effect.dst=effect.count>=effect.count_max?effect.dst_to:effect.dst_from;
            if (layer.parameters.size()>effect.parameters.size()) throw std::runtime_error("Layer parameters exceed the PC save layout");
            std::copy(layer.parameters.begin(),layer.parameters.end(),effect.parameters.begin());
            if (layer.texture) {
                saved.filenames[i]=assets.pc_reference(*layer.texture);
                if (saved.filenames[i].empty()) throw std::runtime_error("Layer texture has no equivalent PC resource path");
            }
        }
        const auto& image=renderer->surface();
        auto& thumb=saved.thumbnail;
        thumb.width=pc_thumbnail_width;thumb.height=pc_thumbnail_height;
        const auto width=static_cast<unsigned>(thumb.width),height=static_cast<unsigned>(thumb.height);
        thumb.pixels.reserve(static_cast<std::size_t>(width)*height);
        for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
            const auto offset=(static_cast<std::size_t>(y)*image.height/height*image.width+
                static_cast<std::size_t>(x)*image.width/width)*3;
            thumb.pixels.push_back((std::uint32_t{image.rgb[offset]}<<16U)|(std::uint32_t{image.rgb[offset+1]}<<8U)|image.rgb[offset+2]);
        }
        if (pc_save_log) {
            auto& log=saved.log.emplace();
            log.layout=pc_log_layout;
            log.voice_count=pc_log_voices;
            for (const auto& line:backlog) {
                auto lines=message_rows(line.text);
                for (std::size_t i=0;i<lines.size();++i) {
                    pc::LogEntry item;
                    item.text=std::move(lines[i]);item.color=line.color<0?0xffffffU:static_cast<unsigned>(line.color);
                    if (i==0) {
                        std::size_t voice=0;
                        for (const auto& name:line.voices) {
                            if (voice==static_cast<std::size_t>(log.voice_count)) break;
                            if (!name.empty() && name.size()<16) item.voices[voice++]=name;
                        }
                    }
                    log.entries.push_back(std::move(item));
                }
            }
            if (log.entries.size()>pc::log_limit) log.entries.erase(log.entries.begin(),log.entries.end()-pc::log_limit);
        }
        for (std::size_t i=0;i<nnn::runtime_replacement_count;++i)
            saved.replacements.push_back(local_replacement(i));
        return saved;
    }

    static bool savable_modal_mode(nnn::RuntimeMode mode) {
        return mode==nnn::RuntimeMode::SelectHeroin || mode==nnn::RuntimeMode::SelectObject ||
               mode==nnn::RuntimeMode::SelectChart;
    }
    bool external_place() const {
        return frame.wait==Wait::Modal && frame.modal && frame.modal->mode==nnn::RuntimeMode::SelectPlace2;
    }
    bool modal_savable() const {
        if (external_place()) return !ui_mode && replay_films.empty();
        return !ui_mode && replay_films.empty() && frame.wait==Wait::Modal && frame.modal &&
            !modal_caller && !frame.modal->return_mode && wait_before_modal==Wait::Running &&
            savable_modal_mode(frame.modal->mode) && (frame.modal->mode!=nnn::RuntimeMode::SelectChart ||
                (chart_run && chart_visit && chart_visit->pending<0 && chart_visit->staged<0));
    }
    void save_snapshot(nnn::snapshot::Writer& out,bool include_backlog=true) const {
        // PC saves the place screen as the current wait. Its suspended caller
        // is only needed to handle a failed live visit before confirmation.
        const bool save_place=external_place();
        const auto wait=save_place ? Wait::Place : frame.wait;
        const bool pending_modal=!save_place && modal_savable();
        if (ui_mode || !replay_films.empty() || (wait>Wait::Choice && wait!=Wait::Place && !pending_modal))
            nnn::snapshot::invalid("Cannot snapshot this game mode");
        const bool decorated_overlap=renderer->has_overlap_decoration();
        const bool draw_message_format=decorated_overlap || next_message_type!=default_next_message_type;
        const bool cursor_format=frame.script_cursor!=0 || draw_message_format;
        const bool chart_format=chart_available || cursor_format;
        out(snapshot_magic,decorated_overlap ? snapshot_version : draw_message_format ? 12U : cursor_format ? 11U : chart_format ? 10U : 9U,
            static_cast<std::uint8_t>(last_heroine));
        if (cursor_format) out(frame.script_cursor);
        if (draw_message_format) out(next_message_type);
        out(default_period,
            static_cast<std::uint8_t>(wait),frame.script,frame.word,frame.remaining_frames);
        if (pending_modal) {
            const auto& request=*frame.modal;
            out(static_cast<nnn::Value>(request.mode),mode_before_modal,request.arguments,
                request.group,request.page,request.selection);
            if (request.mode==nnn::RuntimeMode::SelectChart)
                out(chart_visit->visit.scroll_y,chart_visit->hit.description,chart_visit->pointer_y);
        }
        dialogue_fields(out,frame.dialogue);
        out(vars,window_type,font_type,next_window,next_message_speed,option_off,auto_message,cannot_click,cannot_skip,
            effect_gate,message_has_voice,message_voice_ms,backlog_voices,remainder,draw_remaining,
            input.mouse_x,input.mouse_y,input.advance,input.held,input.choice,
            shared_runtime->mode(),shared_runtime->return_mode(),shared_runtime->skip_active(),
            shared_runtime->get_system_parameter(nnn::NNNPARAM_AUTOMODE));
        out(period,layer_variables,frame.demo_mode,frame.config_mask,frame.film_name,frame.story_name,
            save_place ? 0U : frame.choice_limit_ms,save_place ? 0U : frame.choice_remaining_ms,
            save_place ? 0U : choice_appear_remaining);
        out(frame.dialogue.layout_command,replacements,surname,given_name,
            !save_place && frame.selection ? frame.selection->auto_select : 0,
            !save_place && frame.selection ? frame.selection->special : 0);
        out(speaking_ms,random_state);
        out(static_cast<std::uint8_t>(pc_log_layout),static_cast<std::uint8_t>(pc_log_voices),initial_calendar);
        const auto flags=GetSkipFlags();skip_fields(out,flags);
        out.size(include_backlog ? backlog.size() : 0,backlog_capacity);
        if (include_backlog) for (const auto& line:backlog) backlog_fields(out,line);
        if (chart_format) {
            out(chart_run.has_value());
            if (chart_run) {
                out(chart_scroll);
                out.records(chart_run->move_road,nnn::chart::max_progress_slots,
                    [](auto& writer,nnn::Byte edge){writer(edge);});
                out(chart_run->can_select);
            }
        }
        behavior->save_snapshot(out);
        vm->save_snapshot(out,chart_format,!save_place);
        renderer->save_snapshot(out,decorated_overlap);
        playback.save_snapshot(out);
    }

    void load_snapshot(nnn::snapshot::Reader& in,const std::function<bool(const FrameState&)>& verify,
                       bool keep_music_position,bool jumping=false,bool migrate_legacy_system_variables=false) {
        using nnn::snapshot::invalid;
        if (ui_mode || !replay_films.empty()) invalid("Cannot restore into a decorative or replay session");
        struct Progress {
            FrameState frame;
            std::array<nnn::Value,variable_count> vars{};
            int window_type=0,font_type=0,next_window=-1,next_message_speed=0,option_off=0;
            int next_message_type=default_next_message_type;
            int auto_message=0,cannot_click=0,cannot_skip=0;
            bool effect_gate=false,message_has_voice=false;
            std::uint32_t message_voice_ms=0,speaking_ms=0,random_state=1;
            bool initial_calendar=false;
            std::uint8_t log_layout=0,log_voices=1,last_heroine=0;
            std::array<std::string,4> backlog_voices;
            unsigned remainder=0;
            unsigned period=20,choice_appear_remaining=0;
            std::uint32_t draw_remaining=0;
            std::array<nnn::Value,32> layer_variables{};
            InputState input;
            std::deque<BacklogEntry> backlog;
            std::vector<std::string> replacements;
            std::string surname,given_name;
            nnn::Value choice_auto=0,choice_special=0;
            nnn::Value suspended_mode=static_cast<nnn::Value>(nnn::RuntimeMode::Nothing);
            std::optional<nnn::chart::RunState> chart_run;
            std::optional<ChartInteraction> chart_visit;
            int chart_scroll=0;
        };
        auto staged=std::make_unique<Progress>();
        auto& next=*staged;
        std::uint32_t magic=0,version=0,frame_period=0;std::uint8_t waiting=0;
        in(magic,version);
        if (magic!=snapshot_magic || version<1 || version>snapshot_version) invalid("Unsupported native session snapshot");
        if (version>=9) {
            in(next.last_heroine);
            if (next.last_heroine>=heroine_limit) invalid("Invalid last heroine in snapshot");
        }
        if (version>=11) in(next.frame.script_cursor);
        if (version>=12) in(next.next_message_type);
        in(frame_period,waiting,next.frame.script,next.frame.word,next.frame.remaining_frames);
        const bool pending_modal=version>=9 && waiting==static_cast<std::uint8_t>(Wait::Modal);
        if (frame_period!=default_period ||
            (!pending_modal && waiting>static_cast<std::uint8_t>(Wait::Choice) &&
             (version<4 || waiting!=static_cast<std::uint8_t>(Wait::Place))))
            invalid("Unsupported native session snapshot");
        next.frame.wait=static_cast<Wait>(waiting);
        ScriptModal saved_modal;
        if (pending_modal) {
            nnn::Value mode=0;
            in(mode,next.suspended_mode,saved_modal.arguments,saved_modal.group,saved_modal.page,saved_modal.selection);
            saved_modal.mode=static_cast<nnn::RuntimeMode>(mode);
            if (!savable_modal_mode(saved_modal.mode)) invalid("Invalid saved modal mode");
            if (saved_modal.mode==nnn::RuntimeMode::SelectChart) {
                if (version<10) invalid("Unsupported chart snapshot version");
                auto& state=next.chart_visit.emplace();
                state.visit.current=saved_modal.selection;
                state.visit.page=saved_modal.page;
                state.hit.candidate=saved_modal.group;
                in(state.visit.scroll_y,state.hit.description,state.pointer_y);
            }
        }
        dialogue_fields(in,next.frame.dialogue);
        nnn::Value mode=0,return_mode=0,automatic=0;bool skip=false;
        in(next.vars,next.window_type,next.font_type,next.next_window,next.next_message_speed,next.option_off,
            next.auto_message,next.cannot_click,next.cannot_skip,next.effect_gate,next.message_has_voice,next.message_voice_ms,
            next.backlog_voices,next.remainder,next.draw_remaining,next.input.mouse_x,next.input.mouse_y,
            next.input.advance,next.input.held,next.input.choice,mode,return_mode,skip,automatic);
        if (next.frame.wait==Wait::Place) {
            const auto* setup=place_config(mode);
            if (!setup || setup->result>=variable_count) invalid("Invalid place selection mode in snapshot");
        }
        next.period=default_period;
        if (version>=3) in(next.period,next.layer_variables,next.frame.demo_mode,next.frame.config_mask,
            next.frame.film_name,next.frame.story_name,next.frame.choice_limit_ms,next.frame.choice_remaining_ms,next.choice_appear_remaining);
        auto& dialogue=next.frame.dialogue;
        if (version>=4) in(dialogue.layout_command);
        else dialogue.layout_command=dialogue.command==lprint ? lprint : dialogue.command ? print : 0;
        if (dialogue.layout_command!=0 && dialogue.layout_command!=print && dialogue.layout_command!=lprint)
            invalid("Invalid dialogue layout mode");
        if (version>=5) {
            in(next.replacements,next.surname,next.given_name,next.choice_auto,next.choice_special);
            if (next.replacements.size()>nnn::runtime_replacement_count ||
                !nnn::valid_runtime_text(next.surname,nnn::runtime_name_bytes) ||
                !nnn::valid_runtime_text(next.given_name,nnn::runtime_name_bytes) ||
                std::any_of(next.replacements.begin(),next.replacements.end(),[](const std::string& text){
                    return !nnn::valid_runtime_text(text,63);
                }))
                invalid("Invalid PC replacement state");
        } else {
            next.surname=surname;next.given_name=given_name;
        }
        next.log_layout=static_cast<std::uint8_t>(pc_log_layout);
        next.log_voices=static_cast<std::uint8_t>(pc_log_voices);
        if (version>=7) in(next.speaking_ms);
        next.random_state=legacy_random_state;
        if (version>=8) {
            in(next.random_state,next.log_layout,next.log_voices,next.initial_calendar);
            if (next.log_layout>static_cast<std::uint8_t>(pc::LogLayout::MultiVoice) ||
                next.log_voices<1 || next.log_voices>8 || (next.log_layout==0 && next.log_voices!=1))
                invalid("Invalid PC backlog format in snapshot");
            if (next.initial_calendar && (next.frame.wait!=Wait::Running || next.frame.word!=0))
                invalid("Invalid initial calendar state in snapshot");
        }
        for (const auto id:next.layer_variables)
            if (id<0 || static_cast<unsigned>(id)>=next.vars.size()) invalid("Invalid layer variable binding");
        const auto& position=next.frame;
        if (!next.period || next.period>1000 || next.remainder>=next.period ||
             next.frame.film_name.size()>62 || next.frame.story_name.size()>62 ||
             next.frame.choice_remaining_ms>next.frame.choice_limit_ms || next.frame.choice_limit_ms>INT32_MAX ||
             next.choice_appear_remaining>choice_appear_frames || position.script.empty() || position.script.size()>255 ||
            position.script.find('\0')!=std::string::npos || position.dialogue.sequence>INT32_MAX ||
            (position.wait==Wait::Draw && position.remaining_frames!=next.draw_remaining))
            invalid("Invalid native session position");
        nnn::SkipFlags flags;skip_fields(in,flags);
        const auto count=in.size(backlog_capacity);
        for (std::size_t i=0;i<count;++i) {
            BacklogEntry line;backlog_fields(in,line);
            if (line.text.size()>16384 || line.color< -1 || line.color>0xffffff) invalid("Invalid snapshot backlog");
            next.backlog.push_back(std::move(line));
        }
        if (version>=10) {
            bool has_chart=false;
            in(has_chart);
            if (has_chart) {
                auto& run=next.chart_run.emplace();
                in(next.chart_scroll);
                in.records(run.move_road,nnn::chart::max_progress_slots,[](auto& reader,nnn::Byte& edge){reader(edge);});
                in.records(run.can_select,nnn::chart::max_selection_words,
                    [](auto& reader,nnn::Word& word){reader(word);});
                if (!configure_chart() || !chart_graph->valid(run) || next.chart_scroll<0 ||
                    next.chart_scroll>16*1024*1024)
                    invalid("Invalid chart run state in snapshot");
            }
            if (next.chart_visit && !next.chart_run) invalid("Chart visit has no run state");
        }
        auto next_behavior=std::make_unique<nnn::PlayerBehavior>(*behavior);
        next_behavior->load_snapshot(in);
        auto next_vm=make_scheduler();
        next_vm->load_snapshot(in,vm.get(),version>=10);
        const auto* active=next_vm->GetVM(next_vm->ActiveMode());
        const auto* selection=active->PendingSelection();
        if ((position.wait==Wait::Choice)!=static_cast<bool>(selection)) invalid("Snapshot selection does not match the VM");
        if (selection) {
            next.frame.selection=*selection;
            if (version>=5) {
                next.frame.selection->auto_select=next.choice_auto;
                next.frame.selection->special=next.choice_special;
            }
            if (version<3) {
                next.frame.choice_limit_ms=choice_limit(selection->time_limit,next.vars);
                next.frame.choice_remaining_ms=next.frame.choice_limit_ms;
            }
        } else if (next.frame.choice_limit_ms || next.choice_appear_remaining) invalid("Unexpected selection clock");
        bool consume_modal=false;
        if (pending_modal) {
            const auto command=active->saved_user_command();
            const auto mapping=command ? modal_commands.find(command->command) : modal_commands.end();
            if (active->Identity()!=position.script || active->GetPC()!=position.word || !command ||
                command->argument_count!=saved_modal.arguments.size() || mapping==modal_commands.end() ||
                mapping->second!=saved_modal.mode || mode!=static_cast<nnn::Value>(saved_modal.mode))
                invalid("Snapshot modal does not match the VM");
            const auto system=shared_runtime->system_variables();
            const SelectionVariableReader read=[&](nnn::Word id) -> nnn::Value {
                if (system_variable(id)) return system[id-first_system_variable];
                return id<next.vars.size() ? next.vars[id] : 0;
            };
            ScriptModal rebuilt;
            rebuilt.mode=saved_modal.mode;
            rebuilt.arguments=std::move(saved_modal.arguments);
            if (rebuilt.mode==nnn::RuntimeMode::SelectHeroin) {
                const auto slots=heroine_slots();
                if (saved_modal.selection<0 || saved_modal.selection>slots ||
                    saved_modal.page<0 || saved_modal.page>=slots || saved_modal.group<1 || saved_modal.group>3 ||
                    (saved_modal.selection==0 && (saved_modal.group!=1 || saved_modal.page!=0)))
                    invalid("Invalid saved heroine selection");
                consume_modal=!setup_heroine(rebuilt,saved_modal.selection-1,read);
            } else if (rebuilt.mode==nnn::RuntimeMode::SelectObject) {
                if (saved_modal.page!=0 || saved_modal.group<1 || saved_modal.group>object_types() ||
                    saved_modal.selection<0 || saved_modal.selection>object_limit)
                    invalid("Invalid saved object selection");
                const auto saved_setup=object_setup(saved_modal.group);
                if (!saved_setup.empty() && saved_modal.selection>std::clamp(config_int(saved_setup,"objectNumber",0),0,object_limit))
                    invalid("Saved object hover is outside the configured list");
                const bool type_is_hover=object_type<variable_count && object_type==object_hover;
                const SelectionVariableReader object_read=[&](nnn::Word id) {
                    return type_is_hover && id==object_type ? saved_modal.group : read(id);
                };
                if (!system_variable(object_type) && object_read(object_type)!=saved_modal.group)
                    invalid("Saved object type differs from the script variable");
                consume_modal=!setup_objects(rebuilt,object_read);
                if (!consume_modal && rebuilt.group==saved_modal.group && object_hover_valid(rebuilt,saved_modal.selection))
                    rebuilt.selection=saved_modal.selection;
                if ((consume_modal || rebuilt.selection!=saved_modal.selection) &&
                    object_hover<variable_count && !system_variable(object_hover) && !type_is_hover)
                    next.vars[object_hover]=0;
            } else {
                auto& state=*next.chart_visit;
                auto& visit=state.visit;
                const auto belongs=[&](int id) {
                    const auto* node=chart_graph->node(id);
                    return id==-1 || (node && node->page==visit.page);
                };
                if (!chart_graph->node(visit.current) || !chart_graph->page(visit.page) ||
                    visit.scroll_y<0 || visit.scroll_y>chart_graph->scroll_limit(visit.page) ||
                    state.pointer_y< -1 || state.pointer_y>=chart_graph->setup().height ||
                    !belongs(state.hit.candidate) || !belongs(state.hit.description))
                    invalid("Invalid chart visit in snapshot");
                const auto chart_read=chart_variables(&next.vars);
                const auto slot=variable_id(chart_graph->setup().slot_var);
                const auto current=chart_read(chart_graph->setup().slot_var);
                if (current && *current!=visit.current) {
                    if (!system_variable(slot)) invalid("Chart visit differs from the saved current slot");
                    state={};
                    state.visit=chart_graph->begin_visit(*current,next.chart_scroll,chart_read);
                }
                const auto progress=chart_progress();
                if (progress)
                    state.hit.access=chart_graph->access(*progress,*next.chart_run,visit.current,state.hit.candidate,chart_read);
                consume_modal=!current || !progress || !chart_view_model.ready(*chart_graph,assets) ||
                    !set_chart_page(rebuilt,state,*progress,*next.chart_run,chart_read);
            }
            if (!consume_modal) next.frame.modal=std::move(rebuilt);
        }
        if (verify && !verify(position)) invalid("Native snapshot does not match the save position");
        if (consume_modal) {
            next.frame.wait=Wait::Running;
            mode=next.suspended_mode;
            next.chart_visit.reset();
        }
        auto next_renderer=make_renderer(static_cast<int>(renderer->width()),static_cast<int>(renderer->height()));
        next_renderer->load_snapshot(in,[&](const std::string& name){return assets.image(name);},
            version>=2,version>=3,version>=5,version>=6,version>=7,version>=13);
        if (version>=13 && position.wait!=Wait::Overlap && position.wait!=Wait::Running)
            invalid("Decorated transition outside its frame wait");
        next_renderer->set_speaking(next.speaking_ms!=0);
        if (position.wait==Wait::Overlap && next_renderer->overlap_remaining()!=position.remaining_frames)
            invalid("Invalid snapshot transition");
        if (jumping) next_renderer->surface();
        std::optional<std::array<std::string,2>> restored_names;
        if (version>=5) {
            const auto global=shared_runtime->runtime_text();
            if (next.surname!=global.surname || next.given_name!=global.given_name)
                restored_names=std::array<std::string,2>{next.surname,next.given_name};
        }
        const auto restored_story=preview_seed(next.vars,next.replacements,next.surname,next.given_name);

        // Audio validates the remaining payload before swapping tracks under
        // the mixer lock. Its address stays stable for the PCM consumer.
        if (jumping) playback.load_snapshot(in,keep_music_position,jump_voice_mask,jump_finish_fades);
        else playback.load_snapshot(in,keep_music_position);
        if (next.frame.modal) next.frame.modal->request=new_modal_request();
        frame=std::move(next.frame);vars=next.vars;
        append_prefix_bytes=0;
        if (frame.dialogue.layout_command &&
            (frame.wait==Wait::Draw || frame.wait==Wait::Overlap ||
             (frame.wait==Wait::Running && mode==static_cast<nnn::Value>(nnn::RuntimeMode::PrintOverlap))))
            frame.dialogue.layout_command=print;
        // Older native snapshots carried a copy of the system segment.
        // Its canonical state now lives in RuntimeContext across all restores.
        const auto system_begin=vars.begin()+first_system_variable,system_end=vars.begin()+system_variable_end;
        if (version<9 && migrate_legacy_system_variables &&
            std::any_of(system_begin,system_end,[](nnn::Value value){return value!=0;})) {
            auto values=shared_runtime->system_variables();
            std::copy(system_begin,system_end,values.begin());
            shared_runtime->set_system_variables(values);
        }
        std::fill(system_begin,system_end,0);
        window_type=next.window_type;font_type=next.font_type;next_window=next.next_window;
        next_message_type=next.next_message_type;
        next_message_speed=next.next_message_speed;option_off=next.option_off;
        auto_message=next.auto_message;cannot_click=next.cannot_click;cannot_skip=next.cannot_skip;
        effect_gate=next.effect_gate;message_has_voice=next.message_has_voice;message_voice_ms=next.message_voice_ms;
        speaking_ms=next.speaking_ms;
        pc_log_layout=static_cast<pc::LogLayout>(next.log_layout);
        pc_log_voices=next.log_voices;
        random_state=next.random_state;
        initial_calendar=next.initial_calendar;
        last_heroine=next.last_heroine;
        result_visit.reset();shop_visit.reset();status_visit.reset();modal_caller.reset();suspended_modal.reset();
        chart_run=std::move(next.chart_run);chart_visit=std::move(next.chart_visit);chart_scroll=next.chart_scroll;
        chart_title_story.reset();
        if (chart_run) shared_runtime->set_chart_selection(chart_graph->slot_capacity(),chart_run->can_select);
        mode_before_modal=next.suspended_mode;wait_before_modal=Wait::Running;
        sampled_clock.reset();
        backlog_voices=std::move(next.backlog_voices);backlog.swap(next.backlog);
        replacements.swap(next.replacements);surname.swap(next.surname);given_name.swap(next.given_name);
        remainder=next.remainder;draw_remaining=next.draw_remaining;input=next.input;
        period=next.period;layer_variables=next.layer_variables;choice_appear_remaining=next.choice_appear_remaining;
        behavior.swap(next_behavior);vm.swap(next_vm);renderer.swap(next_renderer);
        audio.clear();system_save_requested=restored_names.has_value();pending_notice=0;SetSkipFlags(flags);
        if (restored_names) shared_runtime->set_system_name(std::move((*restored_names)[0]),std::move((*restored_names)[1]));
        if (!jumping) {
            // Old slots kept these unlocks only in the audio payload. Merge
            // them into global progress without replacing flags from newer visits.
            for (const int flag:playback.legacy_voice_flags())
                if (shared_runtime->set_voice_played(flag)) system_save_requested=true;
        }
        shared_runtime->set_mode(mode);shared_runtime->set_return_mode(return_mode);
        shared_runtime->set_skip_active(skip);
        shared_runtime->set_system_parameter(nnn::NNNPARAM_AUTOMODE,automatic);
        shared_runtime->set_preview_story(restored_story);
        preview_story_dirty=false;
        if (frame.wait==Wait::Place) playback.suspend_script_audio();
    }

    const FrameState& step(std::uint32_t dt,const InputState& in) {
        const bool resume_movie=frame.wait==Wait::Movie;
        if (!resume_movie) sampled_clock.reset();
        frame.choice_timed_out=false;
        apply_parameters();
        if (frame.wait==Wait::Ended || frame.wait==Wait::Error || frame.wait==Wait::Modal) return frame;
        if (frame.wait!=Wait::Overlap) renderer->resume_overlap_scene();
        if (initial_calendar) {
            initial_calendar=false;
            if (!in.legacy_calculations) {
                SetVarData(date_id,101);
                SetVarData(year_id,2000);
            }
        }
        if (frame.wait==Wait::Place) {
            const auto* setup=place_config(shared_runtime->mode());
            if (!setup || !can_choose_place(*setup,in.choice)) return frame;
            commit_place(*setup,static_cast<nnn::Value>(in.choice));
            frame.wait=Wait::Running;
        }
        if (resume_movie) {
            frame.wait=Wait::Running; frame.movie=0;
        } else input=in;
        if (ui_mode) {
            if (input.advance) ui_click=std::array<int,2>{input.mouse_x,input.mouse_y};
            // A click in a decorative script is a variable update, not a
            // request to skip the script's current DRAW or overlap.
            input.advance=false;
        }
        if (!resume_movie && input.skip && !ui_mode) {
            shared_runtime->set_skip_active(*input.skip);
            if (frame.wait==Wait::Dialogue) shared_runtime->check_dialogue_skip(frame.dialogue.message_id);
        }
        sync_skip();
        const auto time=static_cast<std::uint64_t>(remainder)+(resume_movie?0U:dt);
        const auto ticks=static_cast<unsigned>(time/period);
        remainder=static_cast<unsigned>(time%period);
        if (!resume_movie && speaking_ms!=0) {
            speaking_ms-=std::min(speaking_ms,dt);
            if (!speaking_ms) renderer->set_speaking(false);
        }
        renderer->advance(ticks,{input.mouse_x,input.mouse_y});
        if (frame.wait==Wait::Dialogue && !input.advance) return frame;
        if (frame.wait==Wait::Dialogue) mark_current_read();
        if (frame.wait==Wait::Choice) {
            if (!frame.selection) return frame;
            unsigned chosen=input.choice;
            bool timeout=chosen==timeout_choice && frame.choice_limit_ms!=0;
            if (!chosen && choice_appear_remaining) {
                choice_appear_remaining-=std::min(choice_appear_remaining,ticks);
                return frame;
            }
            if (!chosen && frame.choice_limit_ms) {
                frame.choice_remaining_ms-=std::min(frame.choice_remaining_ms,dt);
                timeout=input.expire_choices && frame.choice_remaining_ms==0;
            }
            if (timeout) {
                const auto target=frame.selection->auto_select;
                chosen=target==0?static_cast<unsigned>(frame.selection->options.size())+1:static_cast<unsigned>(target);
            }
            else if (!chosen || chosen>frame.selection->options.size()) return frame;
            auto status=vm->Choose(chosen,timeout);
            if (!status) { frame.error=status.detail; frame.wait=Wait::Error; return frame; }
            if (!ui_mode) { confirm_choice(*frame.selection,chosen,timeout); shared_runtime->complete_choice(); sync_skip(); }
            frame.choice_timed_out=timeout;
            frame.choice_limit_ms=frame.choice_remaining_ms=choice_appear_remaining=0;
            frame.selection.reset(); ++counters.choices;
        }
        if (frame.wait==Wait::Draw) {
            draw_remaining-=std::min(draw_remaining,ticks);
            frame.remaining_frames=draw_remaining;
            if (draw_remaining && !input.advance) return frame;
        }
        if (frame.wait==Wait::Overlap) {
            if (input.advance && frame.demo_mode==0 && renderer->overlapping()) renderer->finish_overlap();
            frame.remaining_frames=renderer->overlap_remaining();
            if (!renderer->overlapping()) frame.wait=Wait::Running;
            return frame; // Present the exact endpoint before executing the next script command.
        }
        frame.wait=Wait::Running;
        frame.remaining_frames=0;
        const auto click=ui_click;
        ui_click.reset();
        if (click) {
            input.advance=true;input.mouse_x=(*click)[0];input.mouse_y=(*click)[1];
        }
        const auto finish=[&](EndReason reason) -> const FrameState& {
            ui_click=click;
            location();frame.wait=Wait::Ended;frame.end_reason=reason;
            return frame;
        };
        std::uint64_t remaining=250000;
        while (remaining) {
            const auto result=vm->Step(remaining);
            if (!result.status) {
                if (result.status.code==nnn::ErrorCode::BudgetExhausted) return frame;
                location(); frame.error=result.status.detail; frame.wait=Wait::Error; break;
            }
            remaining-=std::min(remaining,result.work_used);
            counters.file_transfers+=result.transitions;
            if (result.transitions) continue;
            if (frame.wait==Wait::Place || frame.wait==Wait::Modal) return frame;
            if (frame.movie) {
                if (input.play_movies) { frame.wait=Wait::Movie; set_mode(nnn::RuntimeMode::PrintMovie); return frame; }
                frame.movie=0;
            }
            switch (result.return_code) {
            case print: frame.wait=Wait::Dialogue; set_mode(nnn::RuntimeMode::PrintMessage); return frame;
            case select:
                frame.selection=*vm->GetVM(vm->ActiveMode())->PendingSelection();
                frame.choice_limit_ms=choice_limit(frame.selection->time_limit,vars);
                frame.choice_remaining_ms=frame.choice_limit_ms;
                choice_appear_remaining=choice_appear_frames;
                remember_choice(*frame.selection);
                frame.wait=Wait::Choice; set_mode(nnn::RuntimeMode::SelectMessage); return frame;
            case draw: frame.wait=Wait::Draw; frame.remaining_frames=draw_remaining; set_mode(nnn::RuntimeMode::Draw); return frame;
            case overlap: frame.wait=Wait::Overlap; frame.remaining_frames=renderer->overlap_remaining(); set_mode(nnn::RuntimeMode::PrintOverlap); return frame;
            case end_film:
                if (replay_films.empty()) break;
                if (++replay_film==replay_films.size()) {
                    return finish(EndReason::ReplayComplete);
                } else {
                    const auto seek=vm->SeekFilm(replay_films[replay_film]);
                    if (!seek) { location(); frame.error=seek.detail; frame.wait=Wait::Error; return frame; }
                }
                break;
            case end:
                if (ui_mode || !replay_films.empty() || standalone_script) return finish(EndReason::End);
                // Foreground END starts the control script over. NEXT from a
                // child has already popped its owner in ScriptScheduler.
                if (!vm->RestartRoot(vm->GetVM(nnn::ScriptMode::Control)->Identity())) {
                    location();frame.error="Unable to restart the control script";frame.wait=Wait::Error;
                    return frame;
                }
                break;
            case next: return finish(EndReason::Next);
            case exit_script:
                if (!ui_mode && replay_films.empty() && !standalone_script) {
                    save_chart_exit();
                    shared_runtime->set_skip_active(false);
                    set_mode(nnn::RuntimeMode::Title);
                }
                return finish(EndReason::Exit);
            default: break;
            }
        }
        return frame;
    }
};

GameSession::GameSession(const fs::path& root,SessionOptions options) : data_(std::make_unique<Impl>(root,options)) {}
GameSession::GameSession(std::unique_ptr<Impl> data) : data_(std::move(data)) {}
GameSession::~GameSession()=default;
std::unique_ptr<GameSession> GameSession::create_ui_preview(const fs::path& root,SessionOptions options,const GameSession* peer) {
    if (options.runtime && options.runtime->root().lexically_normal()!=root.lexically_normal())
        throw std::invalid_argument("UI runtime directory differs from the game directory");
    if (peer && (!peer->data_->decorative_preview || peer->data_->shared_runtime->root().lexically_normal()!=root.lexically_normal()))
        throw std::invalid_argument("UI asset peer must be a decorative session for the same game");
    options.audio.enabled=false;
    options.queue_audio_events=false;
    return std::unique_ptr<GameSession>(new GameSession(std::make_unique<Impl>(root,options,true,
        peer ? peer->data_->shared_assets : nullptr)));
}
const FrameState& GameSession::step_frame(std::uint32_t dt,const InputState& input) {
    const auto& frame=data_->step(dt,input);
    data_->publish_preview_story();
    return frame;
}
const FrameState& GameSession::state() const { return data_->frame; }
bool GameSession::complete_modal(int id,bool accepted,unsigned elapsed_frames,const std::vector<std::string>& values) {
    if (!data_->complete_modal(id,accepted,elapsed_frames,values)) return false;
    data_->publish_preview_story(data_->modal_caller.has_value());
    return true;
}
int GameSession::act_modal(int request,int action,int value) {
    const bool title_chart=static_cast<bool>(data_->chart_title_story);
    const auto result=data_->act_modal(request,static_cast<ModalAction>(action),value);
    if (result!=ModalResult::Rejected && (!title_chart || result==ModalResult::EnterStory))
        data_->publish_preview_story(data_->modal_caller.has_value());
    return static_cast<int>(result);
}
void GameSession::advance_modal(int request,unsigned frames) { data_->advance_modal(request,frames); }
void GameSession::set_chart_storage(const fs::path& private_root) {
    data_->chart_private_root=private_root;
    if (data_->chart_store) data_->chart_store->private_root(private_root);
}
std::optional<ScriptModal> GameSession::status_overlay(nnn::RuntimeMode type,nnn::RuntimeMode mode,bool prefer_story) {
    return data_->status_overlay(type,mode,prefer_story);
}
bool GameSession::modal_savable() const { return data_->modal_savable(); }
bool GameSession::menu_extension_enabled(nnn::RuntimeMode mode) const { return data_->menu_extension_enabled(mode); }
bool GameSession::open_modal(nnn::RuntimeMode mode,nnn::RuntimeMode back_mode,bool from_title) {
    auto& d=*data_;
    if (d.decorative_preview || !d.replay_films.empty() || (from_title && d.frame.wait==Wait::Modal) ||
        (!from_title && (d.frame.wait==Wait::Ended || d.frame.wait==Wait::Error || !d.menu_extension_enabled(mode))))
        return false;
    if (from_title && mode==nnn::RuntimeMode::SelectChart) {
        if (d.seed_chart_title_story() && d.begin_modal(mode,{},back_mode,true)) return true;
        d.chart_title_story.reset();
        return false;
    }
    if (from_title) {
        if (const auto seed=d.shared_runtime->preview_story()) {
            d.vars=seed->vars;
            d.replacements=seed->replacements;
            d.initial_calendar=false;
            d.preview_story_dirty=true;
        }
    }
    if (!d.begin_modal(mode,{},back_mode,from_title)) return false;
    d.publish_preview_story(true);
    return true;
}
int GameSession::abort_modal(int request,const std::string& reason,unsigned elapsed_frames) {
    const bool title_chart=static_cast<bool>(data_->chart_title_story);
    const auto destination=data_->abort_modal(request,reason,elapsed_frames);
    if (destination>=0 && !title_chart) data_->publish_preview_story(data_->modal_caller.has_value());
    return destination;
}
void GameSession::sync_runtime_names() { data_->sync_runtime_names(); }
int GameSession::overlap_type() const noexcept { return data_->renderer->overlap_type(); }
std::vector<int> GameSession::places() const {
    std::vector<int> out;
    if (const auto* setup=data_->place_config(static_cast<nnn::Value>(place_mode())))
        for (const auto& place:setup->choices) out.push_back(data_->place_state(*setup,place));
    return out;
}
std::vector<int> GameSession::place_characters() const {
    std::vector<int> out;
    if (const auto* setup=data_->place_config(static_cast<nnn::Value>(place_mode()))) {
        out.reserve(setup->choices.size());
        for (const auto& place:setup->choices)
            out.push_back(setup->has_character && data_->place_state(*setup,place)==2 ?
                std::max(0,data_->GetVarData(place.character)) : 0);
    }
    return out;
}
nnn::RuntimeMode GameSession::place_mode() const {
    if (data_->external_place()) return nnn::RuntimeMode::SelectPlace2;
    if (data_->frame.wait!=Wait::Place) return nnn::RuntimeMode::Nothing;
    return static_cast<nnn::RuntimeMode>(data_->shared_runtime->mode());
}
nnn::snapshot::Bytes GameSession::snapshot() const {
    nnn::snapshot::Writer out;data_->save_snapshot(out);return std::move(out.bytes);
}
void GameSession::restore_pc_save(const std::vector<nnn::Byte>& bytes) {
    if (data_->ui_mode || !data_->replay_films.empty()) nnn::snapshot::invalid("Cannot import a PC save in this mode");
    SessionOptions options;
    options.width=static_cast<int>(data_->renderer->width());options.height=static_cast<int>(data_->renderer->height());
    options.frame_ms=data_->default_period;options.audio.sample_rate=data_->playback.sample_rate();
    options.queue_audio_events=data_->queue_audio_events;options.runtime=data_->shared_runtime->fork();
    options.standalone_script=data_->standalone_script;
    options.random_seed=data_->random_state;
    options.local_clock=data_->clock_source;
    options.filename_encoder=data_->filename_encoder;
    auto candidate=std::make_unique<Impl>(options.runtime->root(),options);
    candidate->chart_private_root=data_->chart_private_root;
    candidate->load_pc_save(bytes);
    data_.swap(candidate);
}
std::vector<nnn::Byte> GameSession::export_pc_save() { return pc::write_save(data_->make_pc_save()); }
nnn::Value GameSession::save_slot_art() const { return data_->save_slot_art(); }
void GameSession::restore_snapshot(const nnn::snapshot::Bytes& bytes,const std::function<bool(const FrameState&)>& verify,
                                   bool keep_music_position,bool migrate_legacy_system_variables) {
    nnn::snapshot::Reader in(bytes);
    data_->load_snapshot(in,verify,keep_music_position,false,migrate_legacy_system_variables);
    data_->clear_jumps();
}
const std::deque<BacklogEntry>& GameSession::backlog() const { return data_->backlog; }
void GameSession::seed_legacy_replay_variables() { data_->shared_runtime->set_system_variables(data_->legacy_system_seed); }
void GameSession::capture_backlog_jump(unsigned cursor) { data_->capture_jump(cursor); }
BacklogJump GameSession::backlog_jump(std::uint64_t id) const { return data_->jump_info(id); }
bool GameSession::restore_backlog_jump(std::uint64_t id,unsigned cursor) { return data_->restore_jump(id,cursor); }
void GameSession::trim_replayed_backlog(unsigned keep) {
    if (keep==0 || keep>backlog_capacity || keep>data_->backlog.size()) nnn::snapshot::invalid("Invalid replayed backlog limit");
    data_->trim_backlog(keep);
}
const render::Surface& GameSession::surface() { return data_->renderer->surface(); }
bool GameSession::screen_layer_active() const { return data_->renderer->screen_layer_active(); }
bool GameSession::story_draws_screen_layer() const {
    const auto wait=data_->frame.wait;
    return wait==Wait::Dialogue || wait==Wait::Draw || (wait==Wait::Choice && data_->choice_draws_scene);
}
std::uint64_t GameSession::screen_layer_revision() const { return data_->renderer->composition_revision(); }
void GameSession::capture_screen_layer(bool enable) {
    render::Layers::OverlapDecoration backdrop;
    if (enable && draw_message()==DrawMessage::LongBackground && data_->long_background_mode)
        backdrop=[impl=data_.get()](render::Surface& out) { impl->paint_long_background(out); };
    data_->renderer->capture_screen_layer(enable,std::move(backdrop));
}
std::unique_ptr<render::ScreenLayerFrame> GameSession::take_screen_layer() {
    data_->renderer->surface();
    return data_->renderer->take_screen_layer();
}
void GameSession::commit_screen_capture(render::Surface frame) {
    data_->renderer->commit_screen_capture(std::move(frame));
}
render::Layers& GameSession::chart_preview_layers() {
    if (!data_->decorative_preview || data_->ui_mode!=static_cast<nnn::Value>(nnn::RuntimeMode::SelectChart))
        throw std::logic_error("Chart layers require a SELECTCHART preview");
    return *data_->renderer;
}
bool GameSession::needs_redraw() const { return data_->renderer->needs_redraw(); }
const std::array<render::Layer,32>& GameSession::layers() const { return data_->renderer->slots(); }
std::shared_ptr<const render::Texture> GameSession::ui_texture(const std::string& name) { return data_->assets.image(name); }
void GameSession::set_ui_variable(const std::string& name,nnn::Value value) {
    const auto i=data_->ids.find(fold_cp932(name));
    if (i!=data_->ids.end()) data_->SetCalcuVarData(i->second,value);
}
void GameSession::set_ui_place_preview(unsigned place) {
    const auto* tags=data_->ui_preview_tags();
    if (!tags) return;
    const auto* setup=data_->ui_mode ? data_->place_config(*data_->ui_mode) : nullptr;
    if (place>(setup ? setup->choices.size() : static_cast<std::size_t>(object_limit))) return;
    data_->SetVarData(setup ? setup->hover : data_->object_hover,static_cast<nnn::Value>(place));
    for (unsigned i=0;i<tags->size();++i) {
        const auto& tag=(*tags)[i];
        if (!tag.empty()) data_->renderer->bind(i,data_->texture(tag+std::to_string(place)),true);
    }
}
nnn::Value GameSession::named_variable(const std::string& name,bool prefer_story,nnn::Value fallback) const {
    const auto i=data_->ids.find(fold_cp932(name));
    if (i==data_->ids.end()) return fallback;
    const auto story=prefer_story ? data_->shared_runtime->preview_story() : nullptr;
    return data_->calculated_variable(i->second,false,story.get());
}
void GameSession::reset_ui_background(const std::string& name,int mode,std::shared_ptr<nnn::RuntimeContext> runtime) {
    auto& d=*data_;
    if (!d.decorative_preview) throw std::logic_error("Reset requires a decorative preview");
    d.bind_ui_runtime(std::move(runtime));
    // Unlike a page restart, a new visit must not retain the old layers or
    // saved story screen. RestartRoot still keeps the already decoded script.
    start_ui_background(name,mode,!d.ui_script.empty());
    d.seed_ui_story();
    d.layer_variables.fill(0);
    d.input={}; d.sampled_clock.reset();
    d.random_state=d.seed_from_clock ? static_cast<std::uint32_t>(std::time(nullptr)) : d.legacy_random_state;
    d.period=d.default_period;
    d.window_type=d.font_type=d.next_message_speed=0; d.next_window=-1;
    d.auto_message=d.cannot_click=d.cannot_skip=d.option_off=0;
    d.choice_appear_remaining=0; d.effect_gate=false;
    d.message_has_voice=false; d.message_voice_ms=d.speaking_ms=0;
    d.begin_line(false,0,0);
    d.ClearSkipFlags();
}
bool GameSession::claim_ui_background(std::shared_ptr<nnn::RuntimeContext> runtime,const nnn::RuntimeContext::PreviewState& before) {
    auto& d=*data_;
    if (!d.decorative_preview || !runtime || !(d.shared_runtime->preview_state()==before) ||
        !(runtime->preview_state()==before)) return false;
    d.bind_ui_runtime(std::move(runtime));
    return true;
}
void GameSession::start_ui_background(const std::string& name,int mode,bool fresh) {
    const auto entry=name.empty()?std::string{}:"spt/"+stem(name)+".spt";
    const bool restarting=!entry.empty() && !fresh && data_->ui_mode.has_value() && data_->ui_script==entry;
    if (!entry.empty() && !data_->vm->RestartRoot(entry)) {
        const auto status=data_->vm->LoadFile(entry);
        if (!status) throw std::runtime_error(entry+": "+status.detail);
    }
    if ((mode==static_cast<int>(nnn::RuntimeMode::Opening) || mode==static_cast<int>(nnn::RuntimeMode::Title)) &&
        (data_->ui_mode!=mode || fresh)) data_->set_period(data_->default_period);
    data_->ui_mode=mode;
    if (!restarting) data_->ui_click.reset();
    data_->ui_script=entry;
    // CBackExecClass::ReStart resets execution, draw wait and overlap count,
    // while retaining the effect layers. Page scripts use MAE to reuse those
    // layers; clearing them here loses the background on a page/END restart.
    if (fresh) data_->renderer=data_->make_renderer(static_cast<int>(data_->renderer->width()),static_cast<int>(data_->renderer->height()));
    else if (!restarting) data_->renderer->clear_all();
    else if (data_->renderer->overlapping()) data_->renderer->finish_overlap();
    data_->frame={}; data_->frame.script=entry;
    data_->draw_remaining=0; data_->remainder=0;
    data_->frame.movie=0;
}
const FrameState& GameSession::step_ui_background(const InputState& input,bool loop) {
    if (data_->ui_script.empty()) throw std::logic_error("No decorative script");
    data_->step(0,input);
    if (loop && data_->frame.wait==Wait::Ended) {
        start_ui_background(data_->ui_script,*data_->ui_mode);
        data_->step(0,input);
    }
    // CBackExecClass runs the script when its wait expires, then calculates
    // the effects and consumes one frame of the new wait, including on entry.
    if (data_->frame.wait==Wait::Error || data_->frame.wait==Wait::Ended ||
        data_->frame.wait==Wait::Modal) return data_->frame;
    data_->renderer->advance(1,{data_->input.mouse_x,data_->input.mouse_y});
    if (data_->frame.wait==Wait::Draw && data_->draw_remaining)
        data_->frame.remaining_frames=--data_->draw_remaining;
    if (data_->frame.wait==Wait::Overlap) {
        data_->frame.remaining_frames=data_->renderer->overlap_remaining();
        if (!data_->renderer->overlapping()) data_->frame.wait=Wait::Running;
    }
    return data_->frame;
}
void GameSession::set_ui_saved_screen(render::Surface frame,bool background) {
    if (!data_->ui_mode) throw std::logic_error("Saved screen requires a decorative session");
    data_->renderer->saved_screen(std::move(frame),background);
}
const std::vector<std::string>& GameSession::warnings() const { return data_->messages; }
std::vector<AudioEvent> GameSession::take_audio_events() { std::vector<AudioEvent> events; events.swap(data_->audio); return events; }
bool GameSession::take_system_save_request() { return std::exchange(data_->system_save_requested,false); }
unsigned GameSession::take_notice() { return std::exchange(data_->pending_notice,0U); }
nnn::audio::GameAudio& GameSession::audio() { return data_->playback; }
const nnn::audio::GameAudio& GameSession::audio() const { return data_->playback; }
SessionStats GameSession::stats() const {
    auto s=data_->counters; const auto& a=data_->assets.stats();
    s.cache_hits=a.hits; s.cached_bytes=a.cached_bytes; s.decodes=a.decodes; s.evictions=a.evictions;
    s.packed_reads=a.packed_reads; s.loose_reads=a.loose_reads; return s;
}
nnn::Value GameSession::variable(unsigned i) const {
    return i<variable_count ? data_->calculated_variable(i,false) : 0;
}
std::uint32_t GameSession::script_random_state() const { return data_->random_state; }
void GameSession::set_script_random_state(std::uint32_t state) { data_->random_state=state; }
std::optional<ScriptClockSample> GameSession::script_clock_sample() const { return data_->sampled_clock; }
unsigned GameSession::frame_ms() const { return data_->period; }
nnn::Value GameSession::mode() const { return data_->ui_mode ? *data_->ui_mode : data_->shared_runtime->mode(); }
std::shared_ptr<nnn::RuntimeContext> GameSession::runtime() const { return data_->shared_runtime; }
nnn::Value GameSession::get_system_parameter(nnn::Word id) const { return data_->shared_runtime->get_system_parameter(id); }
bool GameSession::set_system_parameter(nnn::Word id,nnn::Value value) {
    if (!data_->shared_runtime->set_system_parameter(id,value)) return false;
    data_->apply_parameters(); return true;
}
bool GameSession::update_system_parameters(const std::vector<std::pair<nnn::Word,nnn::Value>>& values) {
    if (!data_->shared_runtime->update_system_parameters(values)) return false;
    data_->apply_parameters(); return true;
}
nnn::RuntimeParameters GameSession::snapshot_parameters() const { return data_->shared_runtime->snapshot_parameters(); }
void GameSession::apply_runtime_parameters() { data_->apply_parameters(); }
void GameSession::begin_player_line(bool has_voice,std::uint32_t voice_ms,int speed) { data_->begin_line(has_voice,voice_ms,speed); }
std::uint32_t GameSession::text_characters(std::uint32_t elapsed) {
    return data_->shared_runtime->with_parameters([&](nnn::RuntimeParameters&) { return data_->behavior->text_characters(elapsed); });
}
int GameSession::text_delay_ms() const {
    return data_->shared_runtime->with_parameters([&](const nnn::RuntimeParameters&) { return data_->behavior->text_delay_ms(); });
}
std::string_view GameSession::append_prefix() const noexcept {
    return std::string_view(data_->frame.dialogue.display_bytes).substr(0,data_->append_prefix_bytes);
}
void GameSession::set_text_length(std::uint32_t length,std::optional<std::uint32_t> visible) {
    if (!data_->append_prefix_bytes) visible.reset();
    data_->shared_runtime->with_parameters([&](nnn::RuntimeParameters&) { data_->behavior->set_text_length(length,visible); });
    data_->append_prefix_bytes=0;
}
std::uint32_t GameSession::visible_text() const {
    return data_->shared_runtime->with_parameters([&](const nnn::RuntimeParameters&) { return data_->behavior->visible_text(); });
}
std::uint32_t GameSession::text_length() const {
    return data_->shared_runtime->with_parameters([&](const nnn::RuntimeParameters&) { return data_->behavior->text_length(); });
}
bool GameSession::text_complete() const {
    return data_->shared_runtime->with_parameters([&](const nnn::RuntimeParameters&) { return data_->behavior->text_complete(); });
}
bool GameSession::request_advance() {
    if (data_->frame.wait==Wait::Draw || data_->frame.wait==Wait::Overlap) return !data_->click_blocked();
    if (data_->auto_message>0) return false;
    const bool advance=data_->shared_runtime->with_parameters([&](nnn::RuntimeParameters&) {
        if (data_->cannot_click>0 && !data_->behavior->text_complete()) return false;
        return data_->behavior->request_advance();
    });
    if (advance) data_->mark_current_read();
    return advance;
}
bool GameSession::auto_ready(std::uint32_t elapsed,bool complete) {
    return data_->shared_runtime->with_parameters([&](nnn::RuntimeParameters&) { return data_->behavior->auto_ready(elapsed,complete,data_->auto_message>0); });
}
std::uint32_t GameSession::settings_text_rate(int index,bool automatic) const noexcept {
    return data_->behavior->settings_text_rate(index,automatic);
}
std::int64_t GameSession::auto_remaining_ms() const {
    return data_->shared_runtime->with_parameters([&](const nnn::RuntimeParameters&) { return data_->behavior->auto_remaining_ms(); });
}
bool GameSession::continue_voice() const {
    return data_->shared_runtime->with_parameters([&](const nnn::RuntimeParameters&) { return data_->behavior->continue_voice(); });
}
bool GameSession::auto_continue() const {
    return data_->shared_runtime->with_parameters([&](const nnn::RuntimeParameters&) { return data_->behavior->auto_continue(); });
}
bool GameSession::auto_skip() const {
    return data_->shared_runtime->with_parameters([&](const nnn::RuntimeParameters&) { return data_->behavior->auto_skip(); });
}
bool GameSession::skip_read_only() const {
    return data_->shared_runtime->with_parameters([&](const nnn::RuntimeParameters&) { return data_->behavior->skip_read_only(); });
}
void GameSession::set_skip_active(bool enabled) {
    if (data_->ui_mode) return;
    data_->shared_runtime->set_skip_active(enabled);
    if (data_->frame.wait==Wait::Dialogue) data_->shared_runtime->check_dialogue_skip(data_->frame.dialogue.message_id);
    data_->sync_skip();
}
bool GameSession::skip_active() const { return !data_->ui_mode && data_->shared_runtime->skip_active(); }
bool GameSession::skip_effective() const { return skip_active() && !data_->skip_blocked(); }
ScriptRestrictions GameSession::script_restrictions() const { return {data_->auto_message,data_->cannot_click,data_->cannot_skip,data_->option_off}; }
bool GameSession::options_enabled() const { return data_->option_off==0 && data_->auto_message<=0; }
DrawMessage GameSession::draw_message() const {
    if (data_->frame.wait!=Wait::Draw || data_->ui_mode) return DrawMessage::None;
    const int type=data_->next_message_type;
    if (type==1 || type==2) return DrawMessage::LongBackground;
    if ((type==0 || type==3) && data_->frame.dialogue.window_visible)
        return data_->draw_window_with_message ? DrawMessage::RetainedMessage : DrawMessage::Window;
    return DrawMessage::None;
}
} // namespace emberfall::scene
