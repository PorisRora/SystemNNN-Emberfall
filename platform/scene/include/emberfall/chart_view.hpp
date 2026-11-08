#pragma once

#include <string>
#include <vector>

namespace emberfall::scene {

struct ChartNodeView {
    int id=0,x=0,y=0;
    int style=-1,access=0;
    int sx=0,sy=0,sw=0,sh=0;
    int routes=0,seen=0;
    std::string asset;
};

struct ChartRoadView { int x1=0,y1=0,x2=0,y2=0,style=0; };
struct ChartTileView { int y=0; std::string asset; };

struct ChartView {
    int scroll=0,limit=0,extent=0,back_extent=0,center_y=0;
    int slot_x=0,slot_y=0,slot_area_width=0,slot_area_height=0,slot_width=0,slot_height=0;
    int mini_x=0,mini_y=0,mini_width=0,mini_height=0,mini_base_x=0,mini_base_y=0;
    int pointer_y=-1,pending=-1,page_percent=0,total_percent=0;
    std::vector<int> page_states; // 0 hidden, 1 available, 2 active, 3 disabled; bit 4 marks a new page.
    std::string center,minimap,unseen,current,complete;
    std::vector<ChartTileView> tiles;
    std::vector<ChartNodeView> nodes;
    std::vector<ChartRoadView> roads;
    std::vector<std::string> description;
};

} // namespace emberfall::scene
