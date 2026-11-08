#pragma once

#include "nnn/chart_progress.hpp"

#include "nnn/runtime_parameter_defaults.hpp"
#include <algorithm>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace nnn {

// Numeric identities from commonMode.h. This is shared session state, not a
// replacement for the original CCommon's renderers, input or platform owners.
enum class RuntimeMode : Value {
    Nothing=1, Logo=2, Opening=3, Title=4, Load=5, Save=6, Backlog=7, ListenBgm=8,
    SelectMessage=9, PrintMessage=10, Draw=11, Config=12, SystemMenu=13,
    PrintOverlap=14, Omake=15, SelectCgCharacter=16, SelectCg=17, SelectScene=18,
    SelectSceneCharacter=20, CdError=21, SelectPlace=22, PrintCg=23, SelectZukan=24,
    SelectMovieCharacter=25, SelectMovie=26, PrintMovie=27, NameInput=29,
    PrintCalendar=30, SelectHeroin=31, CodecError=32, MyPrintResult=33,
    SelectChart=34, SelectPlace2=35, SelectHint=36, PrintStatus=38, PrintHint=39,
    Minigame=40, SelectObject=41, PrintAchievement=42, PrintTerm=43, ListenVoice=44,
    CardSystem=45, KanjiInput=46, PrintItem=47, Shop=48, PrintParty=49
};
inline constexpr std::array<std::string_view,50> system_mode_names{{
    "","nothing","logo","opening","title",
    "load","save","backlog","listenbgm","selectmessage",
    "printmessage","draw","config","systemmenu","printoverrap",
    "omake","selectcgchara","selectcg","selectscene","",
    "selectscenechara","","selectplace","printcg","selectzukan",
    "selectmoviechara","selectmovie","printmovie","","nameinput",
    "printcalendar","selectheroin","codecerror","printresult","selectchart",
    "selectplace2","selecthint","","printstatus","printhint",
    "minigame","selectobject","printachievement","printterm","listenvoice",
    "cardsystem","kanjiinput","printitem","shop","printparty"
}};

enum class GalleryKind : Word { Cg=1, Film=2, Music=3, Movie=4, Choice=5 };

// The last committed story values are inputs to decorative scripts. A preview
// seeds its own private VM from this state and can never mutate the source.
struct PreviewStoryState {
    std::array<Value,32768> vars{};
    std::vector<std::string> replacements;
    std::string surname,given_name;
};
struct SystemVariableWrite {
    std::size_t index;
    Value expected,value;
};

class RuntimeContext {
public:
    struct PreviewState {
        RuntimeParameters parameters;
        std::uint64_t gallery;
        SystemVariables variables;
        ScriptProgress progress;
        Value mode, return_mode;
        bool skip;
        Value system_voice, window, message_cursor, mouse_cursor;
        std::array<Word,32> voice_off;
        RuntimeTextState text;
        std::shared_ptr<const RuntimeOptionalProgress> optional_progress;
        std::shared_ptr<const PreviewStoryState> story;
        std::shared_ptr<const chart::Progress> chart_progress;
        std::shared_ptr<const chart::SelectionSeed> chart_selection;
        bool operator==(const PreviewState& other) const {
            return parameters==other.parameters && gallery==other.gallery && variables==other.variables &&
                progress==other.progress && mode==other.mode && return_mode==other.return_mode && skip==other.skip &&
                system_voice==other.system_voice && window==other.window && message_cursor==other.message_cursor &&
                mouse_cursor==other.mouse_cursor && voice_off==other.voice_off && text==other.text && story==other.story &&
                chart_progress==other.chart_progress && chart_selection==other.chart_selection &&
                (optional_progress==other.optional_progress ||
                    (optional_progress && other.optional_progress && *optional_progress==*other.optional_progress));
        }
    };
    static constexpr std::size_t max_read_messages=100000;
    static constexpr std::size_t max_gallery_unlocks=150000;
    explicit RuntimeContext(std::filesystem::path root, RuntimeDefaultsOptions options={})
        : root_(std::move(root)), initial_(load_initial_runtime_parameters(root_,{},options)) {}

    RuntimeContext(const RuntimeContext&)=delete;
    RuntimeContext& operator=(const RuntimeContext&)=delete;

    const std::filesystem::path& root() const noexcept { return root_; }
    // Loading/replaying a candidate must not change the live session's settings
    // or mode on failure. The VM, renderer and audio are deliberately not copied.
    std::shared_ptr<RuntimeContext> fork() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto copy=std::shared_ptr<RuntimeContext>(new RuntimeContext(root_,initial_,revision_,mode_,return_mode_,read_,skip_active_));
        copy->gallery_=gallery_;
        copy->gallery_revision_=gallery_revision_;
        copy->achievement_completion_=achievement_completion_;
        copy->preview_story_=preview_story_;
        copy->chart_progress_=chart_progress_;
        copy->chart_selection_=chart_selection_;
        return copy;
    }
    // A prepared menu frame can be reused while its script inputs and progress stay unchanged.
    PreviewState preview_state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {initial_.parameters,gallery_revision_,initial_.system_variables,initial_.script_progress,mode_,return_mode_,skip_active_,
            initial_.system_voice_selection,initial_.message_window,initial_.message_cursor,initial_.mouse_cursor,initial_.voice_off,
            initial_.runtime_text,initial_.optional_progress,preview_story_,chart_progress_,chart_selection_};
    }
    std::shared_ptr<const PreviewStoryState> preview_story() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return preview_story_;
    }
    void set_preview_story(std::shared_ptr<const PreviewStoryState> state) {
        std::lock_guard<std::mutex> lock(mutex_);
        preview_story_=std::move(state);
    }
    std::shared_ptr<const chart::Progress> chart_progress() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return chart_progress_;
    }
    bool set_chart_progress(chart::Progress progress) {
        const auto nodes=progress.traversed.size();
        if (nodes>chart::max_progress_slots || progress.seen.size()!=((nodes+3U)&~std::size_t{3})) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (chart_progress_ && chart_progress_->seen==progress.seen && chart_progress_->traversed==progress.traversed) return true;
        chart_progress_=std::make_shared<const chart::Progress>(std::move(progress));
        return true;
    }
    // The PC chart class keeps these flags across New Game in the same process.
    // Story slots also carry the flags, but the game-wide progress file does not.
    std::shared_ptr<const chart::SelectionSeed> chart_selection() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return chart_selection_;
    }
    void set_chart_selection(std::size_t slots,const std::vector<Word>& selection) {
        if (slots==0 || slots>chart::max_progress_slots || selection.size()!=chart::selection_words(slots)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (chart_selection_ && chart_selection_->slots==slots && chart_selection_->enabled==selection) return;
        chart_selection_=std::make_shared<const chart::SelectionSeed>(chart::SelectionSeed{slots,selection});
    }

    Value get_system_parameter(Word id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return get_unlocked(id);
    }
    bool get_system_parameter(Word id, Value& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!RuntimeParameters::contains(id)) return false;
        out=get_unlocked(id);
        return true;
    }
    bool set_system_parameter(Word id, Value value) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!RuntimeParameters::contains(id)) return false;
        set_unlocked(id,value);
        ++revision_;
        return true;
    }
    // Unknown IDs reject the whole batch. Recognized but configuration-disabled
    // writes are successful no-ops, matching SetSystemParam rather than aliases.
    bool update_system_parameters(const std::vector<std::pair<Word,Value>>& values) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry:values)
            if (!RuntimeParameters::contains(entry.first)) return false;
        for (const auto& entry:values) set_unlocked(entry.first,entry.second);
        if (!values.empty()) ++revision_;
        return true;
    }
    RuntimeParameters snapshot_parameters() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initial_.parameters;
    }
    // A transient view for output adapters. Persistence uses the raw snapshot.
    RuntimeParameters snapshot_effective_parameters() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto result=initial_.parameters;
        for (const auto id:{NNNPARAM_MOVIEVOLUME,NNNPARAM_SOUNDVOICEVOLUME,
                           NNNPARAM_MOVIESWITCH,NNNPARAM_SOUNDVOICESWITCH})
            result.set(id,get_unlocked(id));
        if (!initial_.routing.total_volume_use) {
            result.set(NNNPARAM_TOTALVOLUME,100);
            result.set(NNNPARAM_TOTALSWITCH,1);
        }
        return result;
    }
    std::uint64_t parameter_revision() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return revision_;
    }
    Value system_variable(std::size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);return initial_.system_variables.at(index);
    }
    void set_system_variable(std::size_t index,Value value) {
        std::lock_guard<std::mutex> lock(mutex_);initial_.system_variables.at(index)=value;
    }
    bool compare_exchange_system_variables(const std::vector<SystemVariableWrite>& writes) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& variables=initial_.system_variables;
        for (const auto& write:writes)
            if (write.index>=variables.size() || variables[write.index]!=write.expected) return false;
        for (const auto& write:writes) variables[write.index]=write.value;
        return true;
    }
    SystemVariables system_variables() const {
        std::lock_guard<std::mutex> lock(mutex_);return initial_.system_variables;
    }
    void set_system_variables(const SystemVariables& values) {
        std::lock_guard<std::mutex> lock(mutex_);initial_.system_variables=values;
    }
    ScriptProgress script_progress() const {
        std::lock_guard<std::mutex> lock(mutex_); return initial_.script_progress;
    }
    void set_script_progress(const ScriptProgress& values) {
        std::lock_guard<std::mutex> lock(mutex_); initial_.script_progress=values;
    }
    Value result_value(Value index) const {
        if (index<0 || static_cast<std::size_t>(index)>=script_result_count) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        return initial_.script_progress[static_cast<std::size_t>(index)];
    }
    bool set_result_if_clear(Value index,Value value) {
        if (index<0 || static_cast<std::size_t>(index)>=script_result_count) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& result=initial_.script_progress[static_cast<std::size_t>(index)];
        if (result!=0) return false;
        result=value;
        return true;
    }
    bool acknowledge_result(Value index) {
        if (index<1 || static_cast<std::size_t>(index)>=script_result_count) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& result=initial_.script_progress[static_cast<std::size_t>(index)];
        if (result!=1) return false;
        result=2;
        return true;
    }
    void set_achievement(Value index,Value value) {
        if (index< -1 || index>=static_cast<Value>(script_achievement_count)) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (index==-1) std::fill(initial_.script_progress.begin()+script_result_count,initial_.script_progress.end(),0);
        else initial_.script_progress[script_result_count+static_cast<std::size_t>(index)]=value;
    }
    // CCommonPrintAchievement keeps completed badges for this run, even if
    // the underlying global flag values are later reset by a script.
    Value latch_achievement_completion(std::size_t index,Value percent) const {
        if (index>=achievement_completion_.size()) return percent;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& completion=achievement_completion_[index];
        if (completion>=100) return completion;
        if (percent>=100) completion=percent;
        return percent;
    }
    std::shared_ptr<const RuntimeOptionalProgress> optional_progress_view() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initial_.optional_progress;
    }
    RuntimeOptionalProgress optional_progress() const { return *optional_progress_view(); }
    void restore_optional_progress(const RuntimeOptionalProgress& progress) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!(*initial_.optional_progress==progress))
            initial_.optional_progress=std::make_shared<RuntimeOptionalProgress>(progress);
    }
    bool term_unlocked(Value index) const { return optional_progress_view()->term_unlocked(index); }
    bool term_read(Value index) const { return optional_progress_view()->term_read(index); }
    bool voice_played(Value index) const { return optional_progress_view()->voice_played(index); }
    bool set_term_unlocked(Value index,bool enabled=true) {
        return set_optional_bit(&RuntimeOptionalProgress::terms,static_cast<std::size_t>(index),runtime_term_unlocked,enabled);
    }
    bool set_term_read(Value index,bool enabled=true) {
        return set_optional_bit(&RuntimeOptionalProgress::terms,static_cast<std::size_t>(index),runtime_term_read,enabled);
    }
    bool set_voice_played(Value index,bool enabled=true) {
        const auto id=static_cast<std::size_t>(index);
        return set_optional_bit(&RuntimeOptionalProgress::voice_words,id/32U,Word{1}<<(id%32U),enabled);
    }
    std::string system_replacement(std::size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto& replacements=initial_.runtime_text.system_replacements;
        return index<replacements.size() ? replacements[index] : std::string{};
    }
    bool set_system_replacement(std::size_t index, std::string text) {
        if (index>=runtime_replacement_count || !valid_runtime_text(text,runtime_replacement_bytes)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        initial_.runtime_text.system_replacements[index]=std::move(text);
        return true;
    }
    bool set_system_name(std::optional<std::string> surname, std::optional<std::string> given_name) {
        if ((surname && !valid_runtime_text(*surname,runtime_name_bytes)) ||
            (given_name && !valid_runtime_text(*given_name,runtime_name_bytes))) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (surname) initial_.runtime_text.surname=std::move(*surname);
        if (given_name) initial_.runtime_text.given_name=std::move(*given_name);
        return true;
    }
    RuntimeTextState runtime_text() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initial_.runtime_text;
    }
    // A startup restore applies the original *InitData rules to blank names.
    // A snapshot/replay baseline can request an exact restore instead.
    bool restore_runtime_text(RuntimeTextState text, bool startup=false) {
        if (!valid_runtime_text(text.surname,runtime_name_bytes) ||
            !valid_runtime_text(text.given_name,runtime_name_bytes) ||
            !std::all_of(text.system_replacements.begin(),text.system_replacements.end(),[](const std::string& value) {
                return valid_runtime_text(value,runtime_replacement_bytes);
            })) return false;
        if (startup) runtime_defaults_detail::seed_empty_names(initial_,text);
        std::lock_guard<std::mutex> lock(mutex_);
        initial_.runtime_text=std::move(text);
        return true;
    }
    std::array<std::string,2> initial_names() const { return initial_.initial_names; }
    std::array<std::string,2> game_default_names() const { return initial_.game_default_names; }
    bool reset_name_on_title() const noexcept { return initial_.reset_name_on_title; }
    const RuntimeReplacementTable& local_replacement_defaults() const noexcept { return initial_.local_replacement_defaults; }

    // The callback runs under the context lock. Do not call another context
    // method from it. Parameter-dependent objects may retain this reference
    // only when every later use also goes through with_parameters. The
    // setters above provide the original Get/Set routing. Direct raw changes
    // here also advance the revision, without silently applying that routing.
    template<class Function> decltype(auto) with_parameters(Function&& function) {
        std::lock_guard<std::mutex> lock(mutex_);
        RawMutation mutation(*this);
        return std::invoke(std::forward<Function>(function),initial_.parameters);
    }
    template<class Function> decltype(auto) with_parameters(Function&& function) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::invoke(std::forward<Function>(function),std::as_const(initial_.parameters));
    }

    RuntimeParameterRouting routing() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initial_.routing;
    }
    bool total_volume_off() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initial_.routing.total_volume_use && initial_.parameters.get(NNNPARAM_TOTALSWITCH)==0;
    }
    std::filesystem::path defaults_source() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initial_.source;
    }
    bool loaded_original_defaults() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initial_.loaded_original_defaults;
    }
    bool legacy_volume_defaults() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initial_.legacy_volume_defaults;
    }
    std::string defaults_diagnostic() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return initial_.diagnostic;
    }

    // Dedicated source fields have no NNNPARAM identity.
    Value system_voice_selection() const {
        std::lock_guard<std::mutex> lock(mutex_); return initial_.system_voice_selection;
    }
    void set_system_voice_selection(Value value) {
        std::lock_guard<std::mutex> lock(mutex_); initial_.system_voice_selection=value; ++revision_;
    }
    Value message_window() const {
        std::lock_guard<std::mutex> lock(mutex_); return initial_.message_window;
    }
    void set_message_window(Value value) {
        std::lock_guard<std::mutex> lock(mutex_); initial_.message_window=value; ++revision_;
    }
    std::array<Value,2> cursor_variants() const {
        std::lock_guard<std::mutex> lock(mutex_);return {{initial_.message_cursor,initial_.mouse_cursor}};
    }
    std::array<Word,32> voice_off() const {
        std::lock_guard<std::mutex> lock(mutex_); return initial_.voice_off;
    }
    void set_voice_off(const std::array<Word,32>& value) {
        std::lock_guard<std::mutex> lock(mutex_); initial_.voice_off=value; ++revision_;
    }
    Value mode() const {
        std::lock_guard<std::mutex> lock(mutex_); return mode_;
    }
    void set_mode(Value value) {
        std::lock_guard<std::mutex> lock(mutex_); mode_=value;
    }
    void set_mode(RuntimeMode value) { set_mode(static_cast<Value>(value)); }
    Value return_mode() const {
        std::lock_guard<std::mutex> lock(mutex_); return return_mode_;
    }
    void set_return_mode(Value value) {
        std::lock_guard<std::mutex> lock(mutex_); return_mode_=value;
    }
    void set_return_mode(RuntimeMode value) { set_return_mode(static_cast<Value>(value)); }

    // The script project number occupies the high16 bits and its message
    // serial the low16. Serialffff is the source's untracked/already-read line.
    bool mark_read(Word identity) {
        std::lock_guard<std::mutex> lock(mutex_);
        if ((identity&0xffffU)==0xffffU || read_.find(identity)!=read_.end()) return true;
        if (read_.size()>=max_read_messages) return false;
        read_.insert(identity);
        return true;
    }
    bool is_read(Word identity) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return is_read_unlocked(identity);
    }
    std::vector<Word> snapshot_read() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return {read_.begin(),read_.end()};
    }
    bool restore_read(const std::vector<Word>& identities) {
        if (identities.size()>max_read_messages) return false;
        std::set<Word> candidate;
        for (const auto identity:identities)
            if ((identity&0xffffU)!=0xffffU) candidate.insert(identity);
        std::lock_guard<std::mutex> lock(mutex_);
        read_.swap(candidate);
        return true;
    }
    // The group is a zero-based CG album or SPT project index. Music uses group zero.
    bool unlock_gallery(GalleryKind kind,Word group,Word index) {
        if (group>0xfffU || index>0xffffU) return false;
        const auto key=(static_cast<Word>(kind)<<28U)|(group<<16U)|index;
        if (!valid_gallery_key(key)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (gallery_.size()>=max_gallery_unlocks || !gallery_.insert(key).second) return false;
        ++gallery_revision_;
        return true;
    }
    std::uint64_t gallery_revision() const {
        std::lock_guard<std::mutex> lock(mutex_); return gallery_revision_;
    }
    std::vector<Word> snapshot_gallery() const {
        std::lock_guard<std::mutex> lock(mutex_); return {gallery_.begin(),gallery_.end()};
    }
    Word selected_choices(Value serial) const {
        if (serial<0 || serial>=1024) return 0;
        const Word first=(static_cast<Word>(GalleryKind::Choice)<<28U)|(static_cast<Word>(serial)<<16U);
        std::lock_guard<std::mutex> lock(mutex_);
        Word mask=0;
        for (auto it=gallery_.lower_bound(first);it!=gallery_.end() && *it<first+32U;++it)
            mask|=Word{1}<<(*it-first);
        return mask;
    }
    bool merge_gallery(const std::vector<Word>& keys) {
        if (keys.size()>max_gallery_unlocks ||
            !std::all_of(keys.begin(),keys.end(),valid_gallery_key)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto candidate=gallery_;
        candidate.insert(keys.begin(),keys.end());
        if (candidate.size()>max_gallery_unlocks) return false;
        if (candidate.size()!=gallery_.size()) {
            gallery_.swap(candidate);
            ++gallery_revision_;
        }
        return true;
    }
    // A replay baseline is installed on a disposable candidate only.
    bool replace_gallery(const std::vector<Word>& keys) {
        if (keys.size()>max_gallery_unlocks || !std::all_of(keys.begin(),keys.end(),valid_gallery_key)) return false;
        std::set<Word> candidate(keys.begin(),keys.end());
        std::lock_guard<std::mutex> lock(mutex_);
        if (gallery_!=candidate) { gallery_.swap(candidate); ++gallery_revision_; }
        return true;
    }
    bool skip_active() const {
        std::lock_guard<std::mutex> lock(mutex_); return skip_active_;
    }
    void set_skip_active(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_); skip_active_=enabled;
    }
    // Enforced before exposing a new dialogue, without marking that line read.
    bool check_dialogue_skip(Word identity) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (skip_active_ && initial_.parameters.get(NNNPARAM_SKIPMODE)==NNNSKIPMODE_NORMAL && !is_read_unlocked(identity))
            skip_active_=false;
        return skip_active_;
    }
    // commonselectmessage.cpp789/1342 calls CheckAndAutoOff on confirmation,
    // not on entry. Nonzero210 and211 mean turn Auto and Skip off respectively.
    void complete_choice() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initial_.parameters.get(NNNPARAM_AUTOCONTINUESWITCH)!=0 && initial_.parameters.get(NNNPARAM_AUTOMODE)!=0) {
            initial_.parameters.set(NNNPARAM_AUTOMODE,0);
            ++revision_;
        }
        if (initial_.parameters.get(NNNPARAM_AUTOSKIPSWITCH)!=0) skip_active_=false;
    }

private:
    template<std::size_t Count>
    bool set_optional_bit(std::array<Word,Count> RuntimeOptionalProgress::* field,std::size_t index,Word bit,bool enabled) {
        if (index>=Count) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& progress=initial_.optional_progress;
        const Word current=((*progress).*field)[index];
        const Word next=enabled ? current|bit : current&~bit;
        if (current==next) return false;
        if (progress.use_count()!=1) progress=std::make_shared<RuntimeOptionalProgress>(*progress);
        ((*progress).*field)[index]=next;
        return true;
    }
    static bool valid_gallery_key(Word key) noexcept {
        const auto kind=static_cast<GalleryKind>(key>>28U);
        return kind==GalleryKind::Cg || kind==GalleryKind::Film ||
               (kind==GalleryKind::Music && (key&0x0fffffffU)<256U) ||
               (kind==GalleryKind::Movie && (key&0x0fffffffU)<4096U) ||
               (kind==GalleryKind::Choice && ((key>>16U)&0xfffU)<1024U && (key&0xffffU)<32U);
    }
    struct RawMutation {
        explicit RawMutation(RuntimeContext& owner) : context(owner),before(owner.initial_.parameters) {}
        ~RawMutation() {
            const Word limit=NNNPARAM_CHARAVOICEVOLUME+static_cast<Word>(runtime_character_volume_count);
            for (Word id=0;id<limit;++id) {
                if (RuntimeParameters::contains(id) && before.get(id)!=context.initial_.parameters.get(id)) {
                    ++context.revision_; break;
                }
            }
        }
        RuntimeContext& context;
        RuntimeParameters before;
    };
    RuntimeContext(std::filesystem::path root,InitialRuntimeParameters initial,
                   std::uint64_t revision,Value mode,Value return_mode,std::set<Word> read,bool skip_active)
        : root_(std::move(root)),initial_(std::move(initial)),revision_(revision),mode_(mode),return_mode_(return_mode),
          read_(std::move(read)),skip_active_(skip_active) {}
    bool is_read_unlocked(Word identity) const {
        return (identity&0xffffU)==0xffffU || read_.find(identity)!=read_.end();
    }
    Value get_unlocked(Word id) const noexcept {
        const auto& route=initial_.routing;
        if (id==NNNPARAM_MOVIEVOLUME || id==NNNPARAM_MOVIESWITCH) {
            const bool volume=id==NNNPARAM_MOVIEVOLUME;
            if (route.movie_volume_not_use==1) id=volume ? NNNPARAM_MUSICVOLUME : NNNPARAM_MUSICSWITCH;
            else if (route.movie_volume_not_use==2) id=volume ? NNNPARAM_SOUNDVOLUME : NNNPARAM_SOUNDSWITCH;
            else if (route.movie_volume_not_use==3) id=volume ? NNNPARAM_VOICEVOLUME : NNNPARAM_VOICESWITCH;
        } else if (id==NNNPARAM_SOUNDVOICEVOLUME && route.sound_voice_volume_not_use!=0) {
            id=route.sound_voice_volume_not_use==1 ? NNNPARAM_VOICEVOLUME : NNNPARAM_SOUNDVOLUME;
        } else if (id==NNNPARAM_SOUNDVOICESWITCH && route.sound_voice_volume_not_use!=0) {
            id=NNNPARAM_VOICESWITCH;
        }
        return initial_.parameters.get(id);
    }
    void set_unlocked(Word id,Value value) noexcept {
        if ((id==NNNPARAM_MOVIEVOLUME || id==NNNPARAM_MOVIESWITCH) && initial_.routing.movie_volume_not_use!=0) return;
        if ((id==NNNPARAM_SOUNDVOICEVOLUME || id==NNNPARAM_SOUNDVOICESWITCH) && initial_.routing.sound_voice_volume_not_use!=0) return;
        if ((id==NNNPARAM_TOTALVOLUME || id==NNNPARAM_TOTALSWITCH) && !initial_.routing.total_volume_use) return;
        initial_.parameters.set(id,value);
        if (id==initial_.message_cursor_parameter) initial_.message_cursor=value;
        if (id==initial_.mouse_cursor_parameter) initial_.mouse_cursor=value;
    }
    const std::filesystem::path root_;
    mutable std::mutex mutex_;
    InitialRuntimeParameters initial_; // The only canonical parameter store.
    std::uint64_t revision_=1;
    Value mode_=static_cast<Value>(RuntimeMode::Nothing);
    Value return_mode_=static_cast<Value>(RuntimeMode::Nothing);
    std::set<Word> read_;
    std::set<Word> gallery_;
    std::uint64_t gallery_revision_=0;
    mutable std::array<Value,script_achievement_count> achievement_completion_{};
    bool skip_active_=false;
    std::shared_ptr<const PreviewStoryState> preview_story_;
    std::shared_ptr<const chart::Progress> chart_progress_;
    std::shared_ptr<const chart::SelectionSeed> chart_selection_;
};
} // namespace nnn
