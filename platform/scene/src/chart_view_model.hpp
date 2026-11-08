#pragma once

#include "emberfall/chart_view.hpp"
#include "game_assets.hpp"
#include "nnn/chart_graph.hpp"

namespace emberfall::scene {

struct ChartInteraction {
    nnn::chart::Visit visit;
    nnn::chart::Hit hit;
    nnn::Value pointer_y=-1;
    nnn::Value pending=-1;
    nnn::Value staged=-1;
};

class ChartViewModel {
public:
    bool ready(const nnn::chart::Graph& graph,GameAssets& assets) {
        const auto size=dimensions(graph.first_atlas(),assets);
        return size.x>=graph.setup().slot_width && size.y>=graph.setup().slot_height;
    }
    std::shared_ptr<const ChartView> view(const nnn::chart::Graph& graph,const ChartInteraction& state,
            const nnn::chart::Progress& progress,const nnn::chart::RunState& run,
            const nnn::chart::ReadVariable& read,GameAssets& assets) {
        const auto& visit=state.visit;
        const auto* page=graph.page(visit.page);
        if (!page || !graph.node(visit.current)) return {};
        auto result=std::make_shared<ChartView>();
        const auto& setup=graph.setup();
        result->scroll=visit.scroll_y;
        result->limit=graph.scroll_limit(visit.page);
        result->extent=page->extent;
        result->back_extent=page->back_extent;
        result->slot_x=setup.slot_area.x;result->slot_y=setup.slot_area.y;
        result->slot_area_width=setup.slot_area.w;result->slot_area_height=setup.slot_area.h;
        result->slot_width=setup.slot_width;result->slot_height=setup.slot_height;
        result->mini_x=page->minimap_area.x;result->mini_y=page->minimap_area.y;
        result->mini_width=page->minimap_area.w;result->mini_height=page->minimap_area.h;
        result->mini_base_x=page->minimap_base.x;result->mini_base_y=page->minimap_base.y;
        result->pointer_y=state.pointer_y;result->pending=state.pending;
        result->page_percent=graph.percent(progress,visit.page+1);
        result->total_percent=graph.percent(progress,-1);
        result->minimap=page->minimap_asset;
        result->unseen=setup.unseen_asset;result->current=setup.current_asset;result->complete=setup.complete_asset;
        for (const auto& candidate:graph.pages()) {
            const auto available=graph.page_available(candidate.id,progress,run,visit.current,read);
            const int tab=candidate.id==visit.page ? 2 : available ? 1 : graph.page_open(candidate.id,read) ? 3 : 0;
            result->page_states.push_back(tab | (available && new_page(setup,candidate.id,read) ? 4 : 0));
        }
        if (!setup.script_tiles) {
            const auto center=dimensions(page->center_asset,assets);
            if (center.y>0) {
                result->center=page->center_asset;
                result->center_y=graph.center_y(page->id,visit.scroll_y,center.y);
            }
            std::int64_t y=graph.background_y(page->id,visit.scroll_y);
            for (const auto& asset:page->back_tiles) {
                if (y>=setup.height) break;
                const auto size=dimensions(asset,assets);
                if (size.y>0 && y+size.y>0) result->tiles.push_back({static_cast<int>(y),asset});
                y+=size.y;
            }
        }
        const auto atlas=dimensions(graph.first_atlas(),assets);
        for (const auto id:page->slots) {
            const auto& node=*graph.node(id);
            const auto style=graph.node_style(visit,id,state.hit.candidate,progress,run,read);
            const auto access=graph.access(progress,run,visit.current,id,read);
            ChartNodeView entry;
            entry.id=id;entry.x=node.center.x;entry.y=node.center.y;
            entry.style=static_cast<int>(style);entry.access=static_cast<int>(access);
            entry.routes=node.routes;entry.seen=progress.seen[static_cast<std::size_t>(id)];
            if (const auto cell=graph.atlas(id,atlas.x,atlas.y)) {
                entry.asset=cell->asset;
                entry.sx=cell->source.x;entry.sy=cell->source.y;entry.sw=cell->source.w;entry.sh=cell->source.h;
            }
            result->nodes.push_back(std::move(entry));
            for (std::size_t i=0;i<node.next.size();++i) {
                if (const auto line=graph.edge_style(visit,{id,i},state.hit.candidate,progress,run,read)) {
                    const auto target=graph.node(node.next[i])->center;
                    result->roads.push_back({node.center.x,node.center.y,target.x,target.y,static_cast<int>(*line)});
                }
            }
        }
        const auto description=graph.description(visit.page,state.hit.description,progress);
        result->description.assign(description.begin(),description.end());
        return result;
    }

    static bool new_page(const nnn::chart::Setup& setup,nnn::Value page,const nnn::chart::ReadVariable& read) {
        if (setup.new_mark_var.empty() || page<0 || page>=32) return false;
        const auto old=read(setup.new_mark_var);
        return old && (static_cast<nnn::Word>(*old)&(nnn::Word{1}<<static_cast<unsigned>(page)))==0;
    }

private:
    nnn::chart::Point dimensions(const std::string& asset,GameAssets& assets) {
        const auto found=dimensions_.find(asset);
        if (found!=dimensions_.end()) return found->second;
        nnn::chart::Point size;
        try {
            if (const auto texture=assets.image(asset)) size={texture->canvas_width,texture->canvas_height};
        } catch (const std::exception&) {
            // Broken optional art remains unavailable while other chart resources can render.
        }
        return dimensions_.emplace(asset,size).first->second;
    }
    std::unordered_map<std::string,nnn::chart::Point> dimensions_;
};
} // namespace emberfall::scene
