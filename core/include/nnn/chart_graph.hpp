#pragma once

#include "nnn/game_name_list.hpp"
#include "nnn/runtime_context.hpp"
#include <algorithm>
#include <array>
#include <bitset>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nnn::chart {

struct Point { Value x=0,y=0; };
struct Rect { Value x=0,y=0,w=0,h=0; };

struct Node {
    Value slot=0,page=0;
    Point center;
    Value routes=1;
    std::vector<Value> next,previous;
    std::string condition_var;
    std::vector<std::string> description,next_description;
};

struct Page {
    Value id=0,extent=0,back_extent=0;
    std::vector<Value> slots;
    std::vector<std::string> back_tiles;
    std::string center_asset,minimap_asset;
    Point minimap_base;
    Rect minimap_area;
};

enum class Function { SetRoute, GetChartPercent, CheckCompSlot, OpenChart, SlotOn, SlotOpen };
struct Command { Function function; std::string name; };
struct Setup {
    Value block_size=1000,basic_charts=0,width=0,height=0;
    Rect slot_area;
    Value slot_width=200,slot_height=50,center_layer=5,center_multiplier=200;
    Value scroll_speed=20,main_capture=1;
    bool capture_reverse=false,jump_minimap=true;
    bool all_click=true,warn_revisit=true,auto_next=false,script_tiles=false;
    Value select_sound=-1,over_sound=-1;
    std::string slot_var,work_var,display_page_var,open_var,percent_var,complete_var,new_mark_var;
    std::string unseen_asset,current_asset,complete_asset;
    std::vector<Command> commands;
};

struct AtlasSlice { std::string asset; Rect source; };

// These arrays follow one playthrough and are captured in each node slot.
struct RunState { std::vector<Byte> move_road; std::vector<Word> can_select; };

// Missing variables are unresolved, not equivalent to a resolved value zero.
using ReadVariable=std::function<std::optional<Value>(std::string_view)>;

enum class Access { Locked, Forward, Revisit };
enum class NodeStyle : Value { Unseen=-1, Seen, Hover, Forward, Current, CurrentHover, Disabled };
enum class EdgeStyle : Value { CurrentPath, Traversed, Unvisited, Forward, Hover };
struct Visit { Value current=-1,page=-1,scroll_y=0; };
struct Hit { Value candidate=-1,description=-1; Access access=Access::Locked; };
struct Branch { Value from,to; bool restore_node,confirm; };
struct Road { Value from; std::size_t edge; };
struct Checkpoint {
    Value slot,work_slot;
    std::optional<Value> mark_seen,display_page;
    std::optional<Road> road;
    RuntimeMode mode=RuntimeMode::SelectChart;
};
struct SlotOpen { Checkpoint checkpoint; Value restore_work; };
struct VariableWrite { std::string_view name; Value value; };
struct FunctionResult {
    bool progress_changed=false,run_changed=false,save_system=false;
    std::optional<VariableWrite> write;
    std::optional<SlotOpen> open_slot;
};

// Chart-node saves write 1000 variables but keep the current 200..299 on
// restore. Variables 1000 and above are absent and retain their live values.
using SavedVariables=std::array<Value,1000>;
inline constexpr bool restores_variable(std::size_t index) noexcept {
    return index<1000U && (index<200U || index>=300U);
}
template<class Read>
SavedVariables capture_variables(Read&& read) {
    SavedVariables saved{};
    for (std::size_t i=0;i<saved.size();++i) saved[i]=read(i);
    return saved;
}
template<class Write>
void restore_variables(const SavedVariables& saved,Write&& write) {
    for (std::size_t i=0;i<saved.size();++i)
        if (restores_variable(i)) write(i,saved[i]);
}

class Graph {
public:
    Graph(const std::filesystem::path& root,Value logical_width,Value logical_height,
        const resource::FilenameEncoder& encode={}) {
        const auto path=game_config::find(root,"init","selectchart.xtx",encode);
        if (path.empty()) return;
        const auto config=game_config::read_text(path);
        const game_config::NameListSetup list(config);
        const auto page_count=list.integer("chartListNumber",1);
        const auto block_size=list.integer("chartBlockSize",1000);
        if (page_count<1 || page_count>32 || block_size<1 || std::int64_t{page_count}*block_size>max_slots)
            throw std::runtime_error("Invalid chart page or slot count");
        if (logical_width<=0 || logical_height<=0 || logical_width>max_coord || logical_height>max_coord)
            throw std::runtime_error("Invalid chart canvas dimensions");
        setup_.block_size=block_size;
        setup_.basic_charts=std::clamp(list.integer("basicChartNumber",page_count),Value{0},page_count);
        setup_.width=logical_width;
        setup_.height=logical_height;
        read_setup(list,page_count);
        (void)atlas_name(0,0);
        const auto encrypted=list.integer("chartAngouFlag",0)!=0;
        const auto desc_limit=std::clamp(list.integer("chartDescMax",3),Value{0},Value{128});
        const auto bottom_space=bounded(list.integer("bottomSpace",60));
        const auto directory=resource::find_directory(root,"chart",encode);
        pages_.reserve(static_cast<std::size_t>(page_count));
        slot_indices_.assign(static_cast<std::size_t>(page_count)*static_cast<std::size_t>(block_size),-1);
        for (Value id=0;id<page_count;++id) {
            auto page=page_setup(list,id);
            const auto name="chart"+std::to_string(id+1)+(encrypted ? ".fxf" : ".xtx");
            const auto file=resource::find_file(directory,name,encode);
            if (file.empty()) throw std::runtime_error("Missing chart graph file: "+name);
            parse_page(page,game_config::read_text(file),desc_limit,bottom_space);
            pages_.push_back(std::move(page));
        }
        for (const auto& from:nodes_)
            for (const auto next:from.next)
                if (const auto index=node_index(next)) nodes_[*index].previous.push_back(from.slot);
    }

    bool configured() const noexcept { return !pages_.empty(); }
    const Setup& setup() const noexcept { return setup_; }
    const std::vector<Page>& pages() const noexcept { return pages_; }
    const Page* page(Value id) const noexcept {
        const auto index=static_cast<std::size_t>(id);
        return index<pages_.size() ? &pages_[index] : nullptr;
    }
    const Node* node(Value slot) const noexcept {
        const auto index=node_index(slot);
        return index ? &nodes_[*index] : nullptr;
    }
    std::size_t slot_capacity() const noexcept { return slot_indices_.size(); }

    std::string legacy_slot_path(Value slot) const {
        const auto* item=node(slot);
        if (!item) return {};
        return "chart"+std::to_string(item->page+1)+"/slot"+std::to_string(slot)+".dat";
    }
    std::string first_atlas() const { return configured() ? atlas_name(0,0) : std::string{}; }
    std::optional<AtlasSlice> atlas(Value slot,Value first_width,Value first_height) const {
        const auto* item=node(slot);
        if (!item || first_width<setup_.slot_width || first_height<setup_.slot_height ||
            first_width>max_coord || first_height>max_coord) return {};
        const auto columns=first_width/setup_.slot_width,rows=first_height/setup_.slot_height;
        const auto cell=slot-item->page*setup_.block_size;
        const auto sheet=static_cast<Value>(cell/(std::int64_t{columns}*rows));
        return AtlasSlice{atlas_name(item->page,sheet),
            {(cell%columns)*setup_.slot_width,((cell/columns)%rows)*setup_.slot_height,setup_.slot_width,setup_.slot_height}};
    }

    Value scroll_limit(Value page_id) const noexcept {
        const auto* item=page(page_id);
        return item ? std::max(Value{0},item->extent-setup_.height) : 0;
    }
    Point screen_point(Point center,Value scroll_y) const noexcept {
        return {saturate(std::int64_t{center.x}+setup_.slot_area.x),
            saturate(std::int64_t{center.y}+setup_.slot_area.y-scroll_y)};
    }
    Value background_y(Value page_id,Value scroll_y) const noexcept {
        const auto* item=page(page_id);
        const auto range=scroll_limit(page_id);
        if (!item || range==0 || item->back_extent<=setup_.height) return 0;
        return static_cast<Value>(-(std::int64_t{item->back_extent-setup_.height}*std::clamp(scroll_y,Value{0},range))/range);
    }
    Value center_y(Value page_id,Value scroll_y,Value texture_height) const noexcept {
        const auto* item=page(page_id);
        const auto range=scroll_limit(page_id);
        if (!item || range==0 || texture_height<=0 || item->back_extent<=setup_.height) return 0;
        const auto shift=std::int64_t{item->back_extent-setup_.height}*setup_.center_multiplier*
            std::clamp(scroll_y,Value{0},range)/(std::int64_t{range}*100);
        return static_cast<Value>((-shift)%texture_height);
    }

    Progress global_defaults() const {
        return {std::vector<Byte>(aligned(slot_capacity(),4U)),std::vector<Byte>(slot_capacity())};
    }
    RunState run_defaults() const {
        return {std::vector<Byte>(aligned(slot_capacity(),16U)),
            std::vector<Word>(selection_words(slot_capacity()),UINT32_MAX)};
    }
    bool valid(const Progress& progress) const noexcept {
        return progress.seen.size()==aligned(slot_capacity(),4U) && progress.traversed.size()==slot_capacity();
    }
    bool valid(const RunState& run) const noexcept {
        return run.move_road.size()==aligned(slot_capacity(),16U) &&
            run.can_select.size()==selection_words(slot_capacity());
    }
    void new_game(RunState& run) const {
        if (valid(run)) std::fill(run.move_road.begin(),run.move_road.end(),Byte{0});
    }

    bool mark_route(Progress& progress,Value slot,Value one_based_route=0) const noexcept {
        const auto* item=node(slot);
        if (!item || static_cast<std::size_t>(slot)>=progress.seen.size()) return false;
        if (item->routes==1) one_based_route=1;
        if (one_based_route<1 || one_based_route>8) return false;
        auto& flags=progress.seen[static_cast<std::size_t>(slot)];
        const auto updated=static_cast<Byte>(flags|(1U<<static_cast<unsigned>(one_based_route-1)));
        if (flags==updated) return false;
        flags=updated;
        return true;
    }
    bool seen(const Progress& progress,Value slot) const noexcept { return node(slot) && flags(progress,slot)!=0; }
    bool complete(const Progress& progress,Value slot) const noexcept {
        const auto* item=node(slot);
        return item && (flags(progress,slot)&route_mask(*item))==route_mask(*item);
    }
    Value percent(const Progress& progress,Value chart_argument) const noexcept {
        Value first=0,last=static_cast<Value>(pages_.size());
        if (chart_argument==0) last=setup_.basic_charts;
        else if (chart_argument!=-1) {
            if (chart_argument<1 || chart_argument>last) return 0;
            first=chart_argument-1;
            last=chart_argument;
        }
        std::uint64_t count=0,total=0;
        for (Value p=first;p<last;++p)
            for (const auto slot:pages_[static_cast<std::size_t>(p)].slots) {
                const auto* item=node(slot);
                count+=std::bitset<8>(flags(progress,slot)&route_mask(*item)).count();
                total+=static_cast<unsigned>(item->routes);
            }
        return count!=0 ? static_cast<Value>(std::max(std::uint64_t{1},count*100U/total)) : 0;
    }
    bool set_enabled(RunState& run,Value slot,bool enabled=true) const noexcept {
        const auto index=static_cast<std::size_t>(slot);
        if (index>=slot_capacity() || index/32U>=run.can_select.size()) return false;
        auto& word=run.can_select[index/32U];
        const auto bit=Word{1}<<(index%32U);
        const auto next=enabled ? word|bit : word&~bit;
        if (next==word) return false;
        word=next;
        return true;
    }
    bool record_edge(RunState& run,Road road) const noexcept {
        const auto* from=road_source(road.from);
        if (!from || road.edge>=from->next.size() || !node(from->next[road.edge]) ||
            static_cast<std::size_t>(road.from)>=run.move_road.size()) return false;
        auto& saved=run.move_road[static_cast<std::size_t>(road.from)];
        const auto next=static_cast<Byte>(road.edge+1U);
        if (next==saved) return false;
        saved=next;
        return true;
    }
    std::optional<std::size_t> edge_index(Value from,Value to) const noexcept {
        const auto* item=road_source(from);
        if (!item || !node(to)) return {};
        const auto edge=std::find(item->next.begin(),item->next.end(),to);
        if (edge==item->next.end()) return {};
        return static_cast<std::size_t>(edge-item->next.begin());
    }

    bool page_open(Value page_id,const ReadVariable& read={}) const {
        if (!page(page_id)) return false;
        if (page_id<setup_.basic_charts) return true;
        const auto bits=variable(read,setup_.open_var);
        return !bits || (static_cast<Word>(*bits)&(Word{1}<<static_cast<unsigned>(page_id-setup_.basic_charts)))!=0;
    }
    std::optional<Word> open_chart(Value one_based_page,Word previous) const noexcept {
        const auto bit=std::int64_t{one_based_page}-setup_.basic_charts-1;
        if (setup_.open_var.empty() || bit<0 || bit>=31 || !page(one_based_page-1)) return {};
        return previous|(Word{1}<<static_cast<unsigned>(bit));
    }
    Access access(const Progress& progress,const RunState& run,Value current,Value slot,const ReadVariable& read={}) const {
        if (!gate(run,slot,read)) return Access::Locked;
        if (edge_index(current,slot)) return Access::Forward;
        return setup_.all_click && seen(progress,slot) ? Access::Revisit : Access::Locked;
    }
    bool page_available(Value page_id,const Progress& progress,const RunState& run,Value current,
        const ReadVariable& read={}) const {
        const auto* item=page(page_id);
        if (!item || !page_open(page_id,read)) return false;
        return std::any_of(item->slots.begin(),item->slots.end(),[&](Value slot) {
            return gate(run,slot,read) && (edge_index(current,slot) || seen(progress,slot));
        });
    }
    std::optional<Branch> branch(const Progress& progress,const RunState& run,Value current,Value slot,
        const ReadVariable& read={}) const {
        if (!node(current)) return {};
        const auto route=access(progress,run,current,slot,read);
        if (route==Access::Locked) return {};
        const bool revisit=route==Access::Revisit;
        return Branch{current,slot,revisit,revisit && setup_.warn_revisit};
    }

    Visit begin_visit(Value current,Value retained_scroll=0,const ReadVariable& read={}) const {
        const auto* item=node(current);
        if (!item) return {};
        const auto stored=variable(read,setup_.display_page_var);
        const auto selected=stored && page(*stored) ? *stored : item->page;
        return {current,selected,visit_scroll(*item,selected,retained_scroll)};
    }
    bool change_page(Visit& visit,Value target,const Progress& progress,const RunState& run,
        const ReadVariable& read={}) const {
        const auto* current=node(visit.current);
        if (!current || target==visit.page || !page_available(target,progress,run,visit.current,read)) return false;
        visit.page=target;
        visit.scroll_y=visit_scroll(*current,target,visit.scroll_y);
        return true;
    }
    Hit hit(const Visit& visit,Point pointer,const Progress& progress,const RunState& run,
        const ReadVariable& read={}) const {
        const auto* item=page(visit.page);
        const auto* current=node(visit.current);
        if (!item || !current) return {};
        Hit result;
        for (auto i=item->slots.rbegin();i!=item->slots.rend();++i) {
            const auto slot=*i;
            const bool outgoing=edge_index(current->slot,slot).has_value();
            if (result.candidate<0 && (outgoing || (setup_.all_click && seen(progress,slot) && gate(run,slot,read))) &&
                slot_hit(*node(slot),visit.scroll_y,pointer)) {
                result.candidate=slot;
                result.access=access(progress,run,current->slot,slot,read);
            }
            if (result.description<0 && seen(progress,slot) && slot_hit(*node(slot),visit.scroll_y,pointer))
                result.description=slot;
            if (result.candidate>=0 && result.description>=0) break;
        }
        if (result.description<0 && current->page==visit.page) {
            for (const auto slot:current->next) {
                const auto* next=node(slot);
                if (next && gate(run,slot,read) && slot_hit(*next,visit.scroll_y,pointer)) {
                    result.description=slot;
                    break;
                }
            }
        }
        return result;
    }
    std::vector<std::string_view> description(Value page_id,Value slot,const Progress& progress) const {
        const auto* item=node(slot);
        if (!item || item->page!=page_id) return {};
        const bool use_next=!seen(progress,slot) && !item->next_description.empty();
        const auto& lines=use_next ? item->next_description : item->description;
        // CChartList::GetDesc bounds the alternate rows by the normal count.
        const auto count=std::min(lines.size(),item->description.size());
        return {lines.begin(),lines.begin()+static_cast<std::ptrdiff_t>(count)};
    }

    NodeStyle node_style(const Visit& visit,Value slot,Value hovered,const Progress& progress,const RunState& run,
        const ReadVariable& read={}) const {
        if (!node(slot)) return NodeStyle::Unseen;
        if (slot==visit.current) return slot==hovered ? NodeStyle::CurrentHover : NodeStyle::Current;
        const auto enabled=gate(run,slot,read);
        if (enabled && edge_index(visit.current,slot)) return slot==hovered ? NodeStyle::Hover : NodeStyle::Forward;
        if (!seen(progress,slot)) return NodeStyle::Unseen;
        if (!enabled) return NodeStyle::Disabled;
        return slot==hovered ? NodeStyle::Hover : NodeStyle::Seen;
    }
    std::optional<EdgeStyle> edge_style(const Visit& visit,Road road,Value hovered,const Progress& progress,
        const RunState& run,const ReadVariable& read={}) const {
        const auto* from=node(road.from);
        if (!from || from->page!=visit.page || road.edge>=from->next.size()) return {};
        const auto* next=node(from->next[road.edge]);
        if (!next || next->page!=from->page) return {};
        const auto a=screen_point(from->center,visit.scroll_y),b=screen_point(next->center,visit.scroll_y);
        const auto on_screen=[&](Point p) {
            return p.x>=-2 && p.x<setup_.width+2 && p.y>=-2 && p.y<setup_.height+2;
        };
        const bool spans_screen=(a.y<0 && b.y>=setup_.height) || (b.y<0 && a.y>=setup_.height);
        if (!on_screen(a) && !on_screen(b) && !spans_screen) return {};
        if (from->slot==visit.current && gate(run,next->slot,read))
            return next->slot==hovered ? EdgeStyle::Hover : EdgeStyle::Forward;
        const auto index=static_cast<std::size_t>(road.from);
        if (index<run.move_road.size() && run.move_road[index]==road.edge+1U) return EdgeStyle::CurrentPath;
        if (road.edge<8U && index<progress.traversed.size() && (progress.traversed[index]&(1U<<road.edge))!=0)
            return EdgeStyle::Traversed;
        return EdgeStyle::Unvisited;
    }

    // These are ordered checkpoint actions. The caller stages the writes and
    // captures each snapshot before advancing to the next action.
    std::vector<Checkpoint> enter_plan(Value slot,Value work_slot) const {
        const auto current=slot==0 ? 1 : slot;
        if (!node(current)) return {};
        auto first=checkpoint(current,work_slot,RuntimeMode::SelectChart);
        if (slot==0) {
            first.work_slot=0;
            first.road.reset();
        }
        std::vector<Checkpoint> plan{std::move(first)};
        const auto* item=road_source(slot);
        if (setup_.auto_next && item && item->next.size()==1U) {
            const auto* target=node(item->next.front());
            if (target && target->page!=item->page) {
                auto next=checkpoint(target->slot,slot,RuntimeMode::SelectChart);
                next.display_page=target->page;
                plan.push_back(std::move(next));
            }
        }
        return plan;
    }
    std::optional<Checkpoint> exit_plan(Value slot,Value work_slot) const {
        if (slot==0 || !node(slot)) return {};
        return checkpoint(slot,work_slot,RuntimeMode::Title);
    }
    std::optional<SlotOpen> slot_open_plan(Value target,Value slot,Value work_slot) const {
        if (!node(target) || !node(slot)) return {};
        return SlotOpen{{slot,target,target,std::nullopt,std::nullopt},work_slot};
    }
    FunctionResult apply_function(Function function,const std::vector<Value>& args,Progress& progress,RunState& run,
        const ReadVariable& read={}) const {
        FunctionResult result;
        if (std::none_of(setup_.commands.begin(),setup_.commands.end(),[&](const Command& command) {
            return command.function==function;
        })) return result;
        const auto arg=args.empty() ? 0 : args.front();
        const auto current=[&] { return variable(read,setup_.slot_var).value_or(0); };
        const auto write=[&](const std::string& name,Value value) {
            if (variable(read,name)) result.write=VariableWrite{name,value};
        };
        switch (function) {
        case Function::SetRoute:
            result.progress_changed=mark_route(progress,current(),arg);
            break;
        case Function::GetChartPercent:
            write(setup_.percent_var,percent(progress,arg));
            break;
        case Function::CheckCompSlot:
            write(setup_.complete_var,complete(progress,current()) ? 1 : 0);
            break;
        case Function::OpenChart:
            if (const auto bits=variable(read,setup_.open_var)) {
                if (const auto updated=open_chart(arg,static_cast<Word>(*bits))) {
                    const auto signed_bits=std::int64_t{*updated}-(*updated>INT32_MAX ? 0x100000000LL : 0LL);
                    result.write=VariableWrite{setup_.open_var,static_cast<Value>(signed_bits)};
                    result.save_system=true;
                }
            }
            break;
        case Function::SlotOn:
            result.run_changed=set_enabled(run,arg,args.size()<2U || args[1]!=0);
            break;
        case Function::SlotOpen:
            result.open_slot=slot_open_plan(arg,current(),variable(read,setup_.work_var).value_or(0));
            break;
        }
        return result;
    }

private:
    static constexpr Value max_slots=256*1024,max_coord=16*1024*1024,max_tiles=1024;
    static std::size_t aligned(std::size_t n,std::size_t alignment) noexcept { return (n+alignment-1U)&~(alignment-1U); }
    static Value bounded(std::int64_t n) {
        if (n< -max_coord || n>max_coord) throw std::runtime_error("Chart coordinate exceeds limit");
        return static_cast<Value>(n);
    }
    static Value saturate(std::int64_t n) noexcept {
        return static_cast<Value>(std::clamp(n,std::int64_t{std::numeric_limits<Value>::min()},
            std::int64_t{std::numeric_limits<Value>::max()}));
    }
    static Value numeric(std::string_view token) {
        if (token.empty()) throw std::runtime_error("Empty chart number");
        if (token.front()=='+') {
            token.remove_prefix(1);
            if (token.empty() || token.front()<'0' || token.front()>'9') throw std::runtime_error("Invalid chart number");
        }
        Value value=0;
        const auto* end=token.data()+token.size();
        const auto parsed=std::from_chars(token.data(),end,value);
        if (parsed.ec!=std::errc{} || parsed.ptr!=end) throw std::runtime_error("Invalid chart number");
        return value;
    }
    static std::string asset(std::string name) {
        auto path=resource::normalize_cp932_path("sys/"+name);
        if (path.empty()) throw std::runtime_error("Invalid chart asset path");
        return path;
    }
    std::string atlas_name(Value page_id,Value sheet) const {
        return asset(slot_tag_+std::to_string(page_id+1)+"_"+std::to_string(sheet+1));
    }
    void read_setup(const game_config::NameListSetup& list,Value page_count) {
        auto& setup=setup_;
        setup.slot_area.x=bounded(list.integer("slotAreaStartX",200));
        setup.slot_area.y=bounded(list.integer("slotAreaStartY",0));
        setup.slot_area.w=bounded(list.integer("slotAreaSizeX",setup.width-setup.slot_area.x));
        setup.slot_area.h=bounded(list.integer("slotAreaSizeY",setup.height-setup.slot_area.y));
        setup.slot_width=bounded(list.integer("slotSizeX",200));
        setup.slot_height=bounded(list.integer("slotSizeY",50));
        if (setup.slot_width<=0 || setup.slot_height<=0)
            throw std::runtime_error("Invalid chart slot sprite dimensions");
        setup.center_layer=std::clamp(list.integer("centerPicLayer",5),Value{0},Value{31});
        setup.center_multiplier=std::clamp(list.integer("centerMulti",200),Value{-10000},Value{10000});
        setup.scroll_speed=bounded(list.integer("scrollSpeed",20));
        setup.main_capture=list.integer("mainCaptureOk",1);
        setup.capture_reverse=list.integer("captureReverse",0)!=0;
        setup.jump_minimap=list.integer("autoJumpMiniWindow",1)!=0;
        setup.all_click=list.integer("allClickOk",1)!=0;
        setup.warn_revisit=list.integer("warningPrintFlag",1)!=0;
        setup.auto_next=list.integer("autoNextOn",0)!=0;
        setup.script_tiles=list.integer("backScriptFlag",0)!=0 && list.integer("picLoadBackScript",0)!=0;
        setup.select_sound=list.integer("selectSound",-1);
        setup.over_sound=list.integer("overSound",-1);
        slot_tag_=list.field("tagNameChartSlot").value_or("ta_chart_button");
        back_tag_=list.field("tagNameBack").value_or("chart_back");
        center_tag_=list.field("tagNameCenter").value_or("ta_chart_center");
        setup.unseen_asset=asset(std::string(list.field("filenameNotLook").value_or("ta_chart_button0")));
        setup.current_asset=asset(std::string(list.field("filenameButtonCursor").value_or("ta_chart_buttoncur")));
        setup.complete_asset=asset(std::string(list.field("filenameComp").value_or("ta_compmark")));
        setup.slot_var=list.field("slotVarName").value_or("\x83\x60\x83\x83\x81\x5b\x83\x67\x83\x58\x83\x8d\x83\x62\x83\x67\x94\xd4\x8d\x86");
        setup.work_var=list.field("workVarName").value_or("\x83\x8f\x81\x5b\x83\x4e\x83\x58\x83\x8d\x83\x62\x83\x67\x94\xd4\x8d\x86");
        if (list.integer("savePrintChartFlag",0)!=0)
            setup.display_page_var=list.field("workVarName2").value_or("\x83\x8f\x81\x5b\x83\x4e\x83\x58\x83\x8d\x83\x62\x83\x67\x94\xd4\x8d\x86\x82\x51");
        if (page_count>setup.basic_charts)
            setup.open_var=list.field("chartOpenVar").value_or("\x83\x60\x83\x83\x81\x5b\x83\x67\x83\x49\x81\x5b\x83\x76\x83\x93\x8f\xee\x95\xf1");
        setup.percent_var=list.field("chartPercentVarName").value_or("answer");
        setup.complete_var=list.field("checkCompVarName").value_or("answer");
        if (list.integer("newMarkPrintFlag",0)!=0)
            setup.new_mark_var=list.field("newMarkVarName").value_or("newchartflag");
        struct CommandKey { Function type; const char* enable; const char* alias; const char* name; Value standard; };
        constexpr std::array<CommandKey,6> keys{{
            {Function::SetRoute,"useSetRouteFunction","setRouteFunctionName","setroute",1},
            {Function::GetChartPercent,"useGetChartPercentFunction","getChartPercentFunctionName","getchartpercent",1},
            {Function::CheckCompSlot,"useCheckCompSlotFunction","checkCompSlotFunctionName","checkcompslot",1},
            {Function::OpenChart,"useOpenChartFunction","openChartFunctionName","openchart",1},
            {Function::SlotOn,"useSlotOnFunction","slotOnFunctionName","sloton",0},
            {Function::SlotOpen,"useSlotOpenFunction","slotOpenFunctionName","slotopen",0}
        }};
        for (const auto& key:keys) {
            const auto name=list.field(key.alias).value_or(key.name);
            if (list.integer(key.enable,key.standard)!=0 && !name.empty())
                setup.commands.push_back({key.type,std::string(name)});
        }
    }
    Page page_setup(const game_config::NameListSetup& list,Value id) const {
        Page page;
        page.id=id;
        const auto suffix=std::to_string(id+1);
        auto tiles=list.integer("backPicNumberCommon",2);
        if (const auto field=list.field("backPicNumber"+suffix)) {
            tiles=std::max(Value{1},game_config::name_number(*field));
            page.back_extent=bounded(std::int64_t{tiles}*setup_.height);
        } else if (const auto extent=list.field("backPicSizeY"+suffix)) {
            page.back_extent=bounded(game_config::name_number(*extent));
            tiles=(page.back_extent+setup_.height-1)/setup_.height;
        } else {
            const auto common=list.field("backPicSizeY");
            page.back_extent=bounded(common ? game_config::name_number(*common) : std::int64_t{tiles}*setup_.height);
        }
        tiles=std::max(Value{1},tiles);
        if (tiles>max_tiles) throw std::runtime_error("Chart background tile count exceeds limit");
        for (Value tile=1;tile<=tiles;++tile)
            page.back_tiles.push_back(asset(back_tag_+suffix+"_"+std::to_string(tile)));
        page.center_asset=asset(center_tag_+suffix);
        page.minimap_asset=asset(std::string(list.field("filenameMiniMapBase"+suffix)
            .value_or(list.field("filenameMiniMapBase").value_or("ta_chart_minimap02"))));
        const auto value=[&](std::string key,Value fallback) {
            return bounded(list.integer(key+suffix,list.integer(key,fallback)));
        };
        page.minimap_base={value("miniMapBasePrintX",10),value("miniMapBasePrintY",300)};
        page.minimap_area={bounded(std::int64_t{page.minimap_base.x}+value("miniMapPrintX",0)),
            bounded(std::int64_t{page.minimap_base.y}+value("miniMapPrintY",30)),
            value("miniMapSizeX",120),value("miniMapSizeY",180)};
        return page;
    }
    void parse_page(Page& page,std::string_view text,Value desc_limit,Value bottom_space) {
        std::size_t cursor=0;
        const auto token=[&]() { return game_config::bounded_name_token(text,cursor); };
        const auto integer=[&]() { return numeric(token().value_or("")); };
        const auto extent=std::max(Value{1800},integer());
        page.extent=bounded(std::int64_t{extent}+bottom_space);
        if (page.extent<=0) throw std::runtime_error("Invalid chart page extent");
        for (auto field=token();field;) {
            if (field->empty() || field->front()!='@') throw std::runtime_error("Missing chart node marker");
            Node item;
            item.slot=numeric(field->substr(1));
            item.page=page.id;
            if (item.slot<0 || item.slot/setup_.block_size!=page.id || node(item.slot))
                throw std::runtime_error("Invalid or duplicate chart slot");
            item.routes=std::max(Value{1},integer());
            if (item.routes>8) throw std::runtime_error("Chart internal route count exceeds flag byte");
            item.center={bounded(integer()),bounded(integer())};
            for (field=token();field && (field->empty() || field->front()!='@');field=token()) {
                const auto first=field->empty() ? '\0' : field->front();
                if ((first>='0' && first<='9') || first=='-') {
                    if (item.next.size()>=32U) throw std::runtime_error("Chart outgoing route count exceeds limit");
                    item.next.push_back(numeric(*field));
                } else if (first=='$') {
                    item.condition_var=field->substr(1);
                } else {
                    auto& lines=first=='!' ? item.next_description : item.description;
                    if (lines.size()>=static_cast<std::size_t>(desc_limit))
                        throw std::runtime_error("Chart description exceeds configured line count");
                    lines.emplace_back(first=='!' ? field->substr(1) : *field);
                }
            }
            slot_indices_[static_cast<std::size_t>(item.slot)]=static_cast<Value>(nodes_.size());
            page.slots.push_back(item.slot);
            nodes_.push_back(std::move(item));
        }
        if (page.slots.empty()) throw std::runtime_error("Chart page has no nodes");
    }
    std::optional<std::size_t> node_index(Value slot) const noexcept {
        const auto index=static_cast<std::size_t>(slot);
        if (index>=slot_indices_.size() || slot_indices_[index]<0) return {};
        return static_cast<std::size_t>(slot_indices_[index]);
    }
    static std::optional<Value> variable(const ReadVariable& read,std::string_view name) {
        return read && !name.empty() ? read(name) : std::nullopt;
    }
    const Node* road_source(Value slot) const noexcept {
        const auto* item=node(slot);
        if (item || slot!=0 || pages_.empty() || pages_.front().slots.empty()) return item;
        // CChartList's initial slot-zero entry resolves to the first row.
        return node(pages_.front().slots.front());
    }
    bool gate(const RunState& run,Value slot,const ReadVariable& read) const {
        const auto* item=node(slot);
        const auto index=static_cast<std::size_t>(slot);
        if (!item || index/32U>=run.can_select.size() || (run.can_select[index/32U]&(Word{1}<<(index%32U)))==0)
            return false;
        return variable(read,item->condition_var).value_or(1)!=0;
    }
    static Byte flags(const Progress& progress,Value slot) noexcept {
        const auto index=static_cast<std::size_t>(slot);
        return index<progress.seen.size() ? progress.seen[index] : Byte{0};
    }
    static unsigned route_mask(const Node& node) noexcept { return (1U<<static_cast<unsigned>(node.routes))-1U; }
    bool slot_hit(const Node& node,Value scroll_y,Point pointer) const noexcept {
        const auto center=screen_point(node.center,scroll_y);
        const auto w=setup_.slot_width,h=setup_.slot_height;
        if (center.x< -w || center.x>=setup_.width || center.y< -2*h || center.y>=setup_.height+h) return false;
        const auto dx=std::int64_t{pointer.x}-center.x+w/2;
        const auto dy=std::int64_t{pointer.y}-center.y+h/2;
        return dx>=0 && dx<w && dy>=0 && dy<h;
    }
    Value visit_scroll(const Node& current,Value page_id,Value retained_scroll) const noexcept {
        const auto y=current.page==page_id ? current.center.y-setup_.height/2 : retained_scroll;
        return std::clamp(y,Value{0},scroll_limit(page_id));
    }
    Checkpoint checkpoint(Value slot,Value previous,RuntimeMode mode) const {
        const auto edge=edge_index(previous,slot);
        return {slot,slot,slot,std::nullopt,edge ? std::optional<Road>{{previous,*edge}} : std::nullopt,mode};
    }
    Setup setup_;
    std::string slot_tag_,back_tag_,center_tag_;
    std::vector<Page> pages_;
    std::vector<Node> nodes_;
    std::vector<Value> slot_indices_;
};

} // namespace nnn::chart
