#pragma once

#include "nnn/game_name_list.hpp"
#include "nnn/runtime_context.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nnn {

struct AchievementEntry {
    Value id=0,percent=0;
    bool completed=false;
};

class AchievementCatalog {
public:
    explicit AchievementCatalog(const std::filesystem::path& root,const resource::FilenameEncoder& encode={}) {
        const auto config=game_config::read(root,"init","printachievement.xtx",encode);
        const game_config::NameListSetup setup(config);
        const auto count=std::clamp(setup.integer("achievementNumber",0),Value{0},static_cast<Value>(script_achievement_count));
        definitions_.resize(static_cast<std::size_t>(count));
        for (std::size_t i=0;i<definitions_.size();++i) {
            auto& item=definitions_[i];
            const auto slot=std::to_string(i+1);
            const auto type=game_config::ascii_lower(std::string(setup.field("achievementType"+slot).value_or("")));
            if (type=="ending") item.type=1;
            else if (type=="endingrange") item.type=2;
            else if (!type.empty() && type.front()>='0' && type.front()<='9') item.type=game_config::name_number(type);
            const auto parameters=std::clamp(setup.integer("achievementParamNumber"+slot,0),Value{0},
                static_cast<Value>(item.params.size()));
            item.count=static_cast<std::size_t>(parameters);
            for (std::size_t j=0;j<item.count;++j)
                item.params[j]=setup.integer("achievement"+slot+"Param"+std::to_string(j+1),0);
        }
    }

    bool configured() const noexcept { return !definitions_.empty(); }
    std::size_t size() const noexcept { return definitions_.size(); }

    // Call at initialization and on entering the view. The source latches
    // completed badges in the CCommonPrintAchievement instance.
    std::vector<AchievementEntry> entries(const RuntimeContext& runtime) const {
        const auto progress=runtime.script_progress();
        std::vector<AchievementEntry> rows;
        rows.reserve(definitions_.size());
        for (std::size_t id=0;id<definitions_.size();++id) {
            const auto value=runtime.latch_achievement_completion(id,percent(definitions_[id],progress));
            rows.push_back({static_cast<Value>(id),value,value>=100});
        }
        return rows;
    }

private:
    struct Definition {
        Value type=0;
        std::array<Value,16> params{};
        std::size_t count=0;
    };

    static Value percent(const Definition& item,const ScriptProgress& progress) noexcept {
        const auto value=[&](Value id) -> Value {
            const auto index=static_cast<std::size_t>(id);
            return index<script_achievement_count ? progress[script_result_count+index] : 0;
        };
        std::int64_t sum=0,divisor=0;
        if (item.type==1 && item.count>0) {
            divisor=static_cast<std::int64_t>(item.count);
            for (std::size_t i=0;i<item.count;++i) sum+=value(item.params[i]);
        } else if (item.type==2 && item.count>=2) {
            const auto first=item.params[0],last=item.params[1];
            divisor=static_cast<std::int64_t>(last)-first+1;
            // GetAchievement returns zero outside the SYS array. Keep those
            // zeroes in the divisor without looping over unbounded input.
            const auto end=std::min(last,static_cast<Value>(script_achievement_count-1));
            for (auto i=std::max(Value{0},first);i<=end;++i) sum+=value(i);
        }
        if (divisor<=0) return 0;
        const auto mean=sum/divisor;
        return mean==0 && sum>0 ? 1 : static_cast<Value>(mean);
    }

    std::vector<Definition> definitions_;
};

struct ResultEntry {
    Value id=0,state=0;
    bool visible=false,hoverable=false;
    std::string label,voice;
    std::vector<std::string> lines;
    std::string scrolling_text;
};

class ResultCatalog {
public:
    struct Visit {
        std::vector<Value> states;
        Value page=0,pending_ack=-1,hovered=-1,voice_remaining=0,scroll_ticks=0,scroll_limit=0;
    };

    explicit ResultCatalog(const std::filesystem::path& root,const resource::FilenameEncoder& encode={}) {
        const auto setup_path=game_config::find(root,"init","printresult.xtx",encode);
        if (setup_path.empty()) return;
        const auto game_text=game_config::read(root,"init","game.xtx",encode);
        if (game_config::NameListSetup(game_text).integer("resultFlag",0)==0) return;
        const auto config=game_config::read_text(setup_path);
        const game_config::NameListSetup setup(config);
        const auto count=setup.integer("resultsMax",20);
        const auto columns=setup.integer("resultsNumberX",5),rows=setup.integer("resultsNumberY",2);
        if (count<=0 || columns<=0 || rows<=0) return;
        result_count_=std::min(count,static_cast<Value>(script_result_count-1));
        columns_=std::min(columns,static_cast<Value>(script_result_count));
        rows_=std::min(rows,static_cast<Value>(script_result_count));
        next_mode_=mode_number(setup.field("nextMode").value_or(""));
        dialog_=setup.integer("dialogFlag",1)!=0;
        voice_=setup.integer("voiceFlag",0)!=0;
        voice_wait_=std::max(Value{0},setup.integer("voiceStartWait",25));
        scroll_wait_=std::max(Value{0},setup.integer("scrollStartWait",150));
        scroll_speed_=std::max(Value{1},setup.integer("scrollSpeed",3));
        message_font_=std::max(Value{1},setup.integer("messageFontSize",22));
        description_type_=setup.integer("descPrintType",1);
        message_blocks_=std::clamp(setup.integer("messageBlockMax",4),Value{0},Value{6});
        plate_tag_=setup.field("filenameResultsTag").value_or("ta_sys_common_resultPlate");
        dialog_tag_=setup.field("filenameDialogTag").value_or("ta_sys_common_result_getdialog");

        const auto name=setup.field("resultDescName").value_or("nya\\resultDesc.xtx");
        const auto path=game_config::find_name_list(root,std::string(name),encode);
        const auto data=game_config::read_text(path);
        std::size_t cursor=0;
        const auto limit=(static_cast<std::size_t>(result_count_)+1U)*fields_per_row;
        while (description_.size()<limit) {
            const auto field=game_config::bounded_name_token(data,cursor);
            if (!field) break;
            description_.emplace_back(*field);
        }
    }

    bool configured() const noexcept { return result_count_>0; }
    Value result_count() const noexcept { return result_count_; }
    Value columns() const noexcept { return columns_; }
    Value rows() const noexcept { return rows_; }
    Value per_page() const noexcept { return columns_*rows_; }
    Value page_count() const noexcept { return (result_count_+per_page()-1)/per_page(); }
    RuntimeMode next_mode() const noexcept { return next_mode_; }
    bool dialog_enabled() const noexcept { return dialog_; }
    bool voice_enabled() const noexcept { return voice_; }
    Value voice_wait() const noexcept { return voice_wait_; }
    Value description_type() const noexcept { return description_type_; }
    Value scroll_start_wait() const noexcept { return scroll_wait_; }
    Value scroll_speed() const noexcept { return scroll_speed_; }

    std::string plate_name(Value page) const {
        return page>=0 && page<page_count() && !plate_tag_.empty() ? plate_tag_+std::to_string(page+1) : std::string{};
    }
    std::string dialog_name(Value id) const {
        if (!dialog_ || id<0 || id>=result_count_ || dialog_tag_.empty()) return {};
        return dialog_tag_+(id<9 ? "0" : "")+std::to_string(id+1);
    }

    Visit begin(const RuntimeContext& runtime) const {
        const auto progress=runtime.script_progress();
        Visit visit;
        visit.states.assign(progress.begin()+1,progress.begin()+1+result_count_);
        find_pending(visit);
        return visit;
    }

    std::optional<ResultEntry> entry(Value id,const Visit& visit) const {
        if (!known_slot(visit,id) || visit.states[static_cast<std::size_t>(id)]<=0) return {};
        ResultEntry result;
        result.id=id;
        result.state=visit.states[static_cast<std::size_t>(id)];
        result.visible=result.state>=(dialog_ ? 2 : 1);
        result.hoverable=visit.pending_ack==-1;
        result.label=field(id+1,0);
        result.voice=voice_name(id);
        for (Value i=0;i<message_blocks_;++i) {
            // In the source the fixed-row variant reads the preceding row.
            result.lines.emplace_back(message(id,static_cast<std::size_t>(i)));
            result.scrolling_text+=message(id+1,static_cast<std::size_t>(i));
        }
        return result;
    }

    std::vector<ResultEntry> entries(const Visit& visit) const {
        std::vector<ResultEntry> rows;
        for (Value i=0;i<result_count_;++i)
            if (auto item=entry(i,visit)) rows.push_back(std::move(*item));
        return rows;
    }

    bool acknowledge(Visit& visit,Value id,RuntimeContext& runtime) const {
        if (!dialog_ || id!=visit.pending_ack || !known_slot(visit,id)) return false;
        auto& state=visit.states[static_cast<std::size_t>(id)];
        if (state!=1 || !runtime.acknowledge_result(id+1)) return false;
        state=2;
        find_pending(visit);
        return true;
    }

    bool set_page(Visit& visit,Value page) const {
        if (visit.pending_ack!=-1 || page<0 || page>=page_count() || page==visit.page) return false;
        visit.page=page;
        hover(visit,-1);
        return true;
    }

    bool hover(Visit& visit,Value id) const {
        if (visit.pending_ack!=-1 || !known_slot(visit,id) || id/per_page()!=visit.page ||
            visit.states[static_cast<std::size_t>(id)]<=0) id=-1;
        if (id==visit.hovered) return false;
        visit.hovered=id;
        visit.voice_remaining=voice_wait_;
        visit.scroll_ticks=0;
        visit.scroll_limit=id>=0 ? scroll_limit(id) : 0;
        return true;
    }

    // Advance only on frames where the same slot is still under the pointer.
    std::string_view tick_hover(Visit& visit) const noexcept {
        if (visit.pending_ack!=-1 || !known_slot(visit,visit.hovered)) return {};
        if (visit.scroll_ticks<visit.scroll_limit) ++visit.scroll_ticks;
        if (visit.voice_remaining>0 && --visit.voice_remaining==0 && voice_) return voice_name(visit.hovered);
        return {};
    }

private:
    static constexpr std::size_t fields_per_row=8;
    static RuntimeMode mode_number(std::string_view raw) {
        // CCommonSystemModeList's names and numeric fallback.
        if (raw.empty()) return RuntimeMode::Omake;
        const auto folded=game_config::ascii_lower(std::string(raw));
        const auto mode=std::find(system_mode_names.begin()+1,system_mode_names.end(),folded);
        if (mode!=system_mode_names.end()) return static_cast<RuntimeMode>(mode-system_mode_names.begin());
        const auto number=game_config::name_number(raw);
        return number>0 && number<100 ? static_cast<RuntimeMode>(number) : RuntimeMode::Omake;
    }

    bool known_slot(const Visit& visit,Value id) const noexcept {
        return id>=0 && id<result_count_ && static_cast<std::size_t>(id)<visit.states.size();
    }
    void find_pending(Visit& visit) const noexcept {
        visit.pending_ack=-1;
        if (!dialog_) return;
        for (Value i=0;i<result_count_;++i) {
            if (known_slot(visit,i) && visit.states[static_cast<std::size_t>(i)]==1) {
                visit.pending_ack=i;
                visit.page=i/per_page();
                return;
            }
        }
    }
    std::string_view field(Value row,std::size_t column) const noexcept {
        const auto index=static_cast<std::size_t>(row)*fields_per_row+column;
        return index<description_.size() ? std::string_view(description_[index]) : std::string_view{};
    }
    std::string_view message(Value row,std::size_t block) const noexcept {
        const auto text=field(row,2U+block);
        return !text.empty() && text.front()=='@' ? std::string_view{} : text;
    }
    std::string_view voice_name(Value id) const noexcept {
        const auto voice=field(id+1,1);
        return !voice.empty() && voice.front()=='!' ? std::string_view{} : voice;
    }
    Value scroll_limit(Value id) const noexcept {
        std::size_t length=0;
        for (Value i=0;i<message_blocks_;++i) length+=message(id+1,static_cast<std::size_t>(i)).size();
        const auto over=std::max<std::int64_t>(0,static_cast<std::int64_t>(length/2)-30);
        const auto frames=static_cast<std::int64_t>(scroll_wait_)+over*message_font_/scroll_speed_;
        return static_cast<Value>(std::min<std::int64_t>(frames,std::numeric_limits<Value>::max()));
    }

    RuntimeMode next_mode_=RuntimeMode::Omake;
    Value result_count_=0,columns_=5,rows_=2;
    bool dialog_=true,voice_=false;
    Value voice_wait_=25,scroll_wait_=150,scroll_speed_=3,message_font_=22;
    Value description_type_=1,message_blocks_=4;
    std::string plate_tag_,dialog_tag_;
    std::vector<std::string> description_;
};

} // namespace nnn
