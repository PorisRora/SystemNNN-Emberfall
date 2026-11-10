#include "wipes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace emberfall::render {
namespace {
// The numeric families follow CAllSimpleWipe's registry.
struct Wipe {
    Surface& output;
    const Surface* before;
    const Surface* after;
    std::uint32_t frame,frames;
    int width,height;

    int scale(int length) const {
        return static_cast<int>((std::int64_t{length}*frame)/frames);
    }
    int remaining(int length) const {
        return static_cast<int>((std::int64_t{length}*(frames-frame))/frames);
    }
    void reverse() {
        std::swap(before,after);
        frame=frames-frame;
    }
    std::size_t offset(int x,int y) const {
        return (static_cast<std::size_t>(y)*output.width+static_cast<unsigned>(x))*3;
    }
    void rectangle(const Surface& source,int x,int y,int w,int h,int dx,int dy) const {
        const int left=std::max({0,-x,-dx}),right=std::min({w,width-x,width-dx});
        const int top=std::max({0,-y,-dy}),bottom=std::min({h,height-y,height-dy});
        if (left>=right || top>=bottom) return;
        const auto bytes=static_cast<std::size_t>(right-left)*3;
        for (int row=top;row<bottom;++row)
            std::memcpy(output.rgb.data()+offset(dx+left,dy+row),source.rgb.data()+offset(x+left,y+row),bytes);
    }
    void span(const Surface& source,int y,int left,int right) const {
        if (right<=left) return;
        const auto at=offset(left,y);
        std::memcpy(output.rgb.data()+at,source.rgb.data()+at,static_cast<std::size_t>(right-left)*3);
    }
    void pixel(const Surface& source,int x,int y,int dx,int dy) const {
        std::memcpy(output.rgb.data()+offset(dx,dy),source.rgb.data()+offset(x,y),3);
    }
    void repeat_pixel(const Surface& source,int x,int y,int left,int right) const {
        const auto* rgb=source.rgb.data()+offset(x,y);
        auto* dst=output.rgb.data()+offset(left,y);
        for (int col=left;col<right;++col,dst+=3) std::memcpy(dst,rgb,3);
    }
    void edge(int y,int x,int gradient) const {
        const int left=std::clamp(x,0,width),right=std::clamp(x+gradient,0,width);
        span(*after,y,0,left);
        span(*before,y,right,width);
        const int step=256/std::max(1,gradient);
        for (int col=left;col<right;++col) {
            const int alpha=256-(col-x)*step;
            const auto at=offset(col,y);
            for (std::size_t i=at;i<at+3;++i)
                output.rgb[i]=static_cast<Byte>(((before->rgb[i]*(256-alpha))>>8)+((after->rgb[i]*alpha)>>8));
        }
    }
};

unsigned next_random(std::uint32_t& seed) {
    // Use the Windows CRT's 15-bit sequence with a reproducible seed.
    seed=seed*214013U+2531011U;
    return (seed>>16U)&0x7fffU;
}

std::uint32_t random_at(std::uint64_t position) {
    // Advance the CRT generator's affine recurrence modulo 2^32 in logarithmic time.
    std::uint32_t seed=1,multiply=214013U,addend=2531011U;
    while (position) {
        if (position&1U) seed=seed*multiply+addend;
        addend*=multiply+1U;
        multiply*=multiply;
        position>>=1U;
    }
    return seed;
}

void curtain(const Wipe& wipe,bool vertical) {
    const int size=vertical?wipe.height:wipe.width,blocks=(size+31)/32;
    const int position=wipe.scale(blocks+16),complete=std::clamp(position-15,0,blocks);
    for (int at=0;at<size;at+=16) {
        const int block=at/32;
        const int exposed=block<complete?16:block>=position?0:std::max(0,std::min(position+1,15)-(block-complete));
        const int n=std::min(16,size-at),end=std::min(n,exposed);
        if (vertical) {
            wipe.rectangle(*wipe.after,0,at,wipe.width,end,0,at);
            wipe.rectangle(*wipe.before,0,at+end,wipe.width,n-end,0,at+end);
        } else {
            wipe.rectangle(*wipe.after,at,0,end,wipe.height,at,0);
            wipe.rectangle(*wipe.before,at+end,0,n-end,wipe.height,at+end,0);
        }
    }
}

void zebra(const Wipe& wipe) {
    const int half=wipe.height/2,exposed=wipe.scale(half);
    for (int y=0;y<wipe.height;++y) {
        const int row=half-y/2;
        const bool reveal=y%2==0 ? row<exposed : row>=half-exposed;
        wipe.span(reveal?*wipe.after:*wipe.before,y,0,wipe.width);
    }
}

void slide(const Wipe& wipe,bool shift_old) {
    const int start=wipe.remaining(wipe.width+wipe.height);
    for (int y=0;y<wipe.height;++y) {
        const int edge=std::clamp(start-y,0,wipe.width);
        wipe.rectangle(*wipe.before,shift_old?wipe.width-edge:0,y,edge,1,0,y);
        wipe.rectangle(*wipe.after,0,y,wipe.width-edge,1,edge,y);
    }
}

void circle(Wipe wipe,bool reverse,bool warp) {
    if (reverse) wipe.reverse();
    const int cx=wipe.width/2,cy=wipe.height/2;
    const int radius=wipe.scale(static_cast<int>(std::sqrt(cx*cx+cy*cy))+1);
    for (int y=0;y<wipe.height;++y) {
        const int dy=y-cy,square=radius*radius-dy*dy;
        const int span=square>0?static_cast<int>(std::sqrt(square)+0.5):0;
        const int left=std::max(0,cx-span);
        const int right=span>=cx && square>0 && !warp?wipe.width:std::min(wipe.width,cx+span);
        wipe.span(*wipe.before,y,0,left);
        if (warp) {
            // Warp uses the same mask as Circle but consumes both sources from
            // their left edges as successive spans are copied into the row.
            wipe.rectangle(*wipe.after,0,y,right-left,1,left,y);
            wipe.rectangle(*wipe.before,left,y,wipe.width-right,1,right,y);
        } else {
            wipe.span(*wipe.after,y,left,right);
            wipe.span(*wipe.before,y,right,wipe.width);
        }
    }
}

void clock(const Wipe& wipe) {
    const auto count=wipe.frame,total=wipe.frames;
    const auto quarter=total/4U,half=total/2U,third=static_cast<std::uint32_t>((3ULL*total)/4U);
    const int cx=wipe.width/2,cy=wipe.height/2;
    const double tangent=count==quarter||count==third?cx+1:std::abs(std::tan(6.28318*count/total));
    for (int y=0;y<wipe.height;++y) {
        const int dx=static_cast<int>(std::min(static_cast<double>(cx),std::abs(y-cy)*tangent+0.5));
        if (count<=quarter) {
            const int end=y<=cy?cx+dx:cx;
            wipe.span(*wipe.before,y,0,cx);
            wipe.span(*wipe.after,y,cx,end);
            wipe.span(*wipe.before,y,end,wipe.width);
        } else if (count<=third) {
            const int start=y<=cy?cx:cx+(count<=half?dx:-dx);
            wipe.span(*wipe.before,y,0,start);
            wipe.span(*wipe.after,y,start,wipe.width);
        } else {
            const int left=y<=cy?cx-dx:cx;
            wipe.span(*wipe.after,y,0,left);
            wipe.span(y<=cy?*wipe.before:*wipe.after,y,left,cx);
            wipe.span(*wipe.after,y,cx,wipe.width);
        }
    }
}

void move(Wipe wipe,int type) {
    if (type>=14) wipe.reverse();
    const int x=wipe.remaining(wipe.width),y=wipe.remaining(wipe.height);
    constexpr int dx[]={0,1,0,-1},dy[]={-1,0,1,0};
    const int direction=(type-10)%4;
    wipe.rectangle(*wipe.before,0,0,wipe.width,wipe.height,0,0);
    wipe.rectangle(*wipe.after,0,0,wipe.width,wipe.height,dx[direction]*x,dy[direction]*y);
}

void door(Wipe wipe,int type) {
    if (type>=20) wipe.reverse();
    if (type%2==0) {
        const int center=wipe.height/2,delta=wipe.remaining(center);
        wipe.rectangle(*wipe.before,0,center-delta,wipe.width,delta*2,0,center-delta);
        wipe.rectangle(*wipe.after,0,0,wipe.width,center,0,-delta);
        wipe.rectangle(*wipe.after,0,center,wipe.width,wipe.height-center,0,center+delta);
    } else {
        const int center=wipe.width/2,delta=wipe.remaining(center);
        wipe.rectangle(*wipe.before,center-delta,0,delta*2,wipe.height,center-delta,0);
        wipe.rectangle(*wipe.after,0,0,center,wipe.height,-delta,0);
        wipe.rectangle(*wipe.after,center,0,wipe.width-center,wipe.height,center+delta,0);
    }
}

void paint_over(const Wipe& wipe) {
    const int edge=wipe.scale(wipe.width*2);
    for (int y=0;y<wipe.height;++y) {
        if (edge<wipe.width) {
            wipe.repeat_pixel(*wipe.before,std::max(0,edge-1),y,0,edge);
            wipe.span(*wipe.before,y,edge,wipe.width);
        } else {
            const int exposed=edge-wipe.width;
            wipe.span(*wipe.after,y,0,exposed);
            wipe.repeat_pixel(*wipe.after,exposed,y,exposed,wipe.width);
        }
    }
}

void gravity(const Wipe& wipe,bool move_old) {
    const auto fall=std::max(1U,static_cast<unsigned>(9ULL*wipe.frames/17U));
    const auto first_bounce=std::max(1U,static_cast<unsigned>(6ULL*wipe.frames/17U));
    const auto elapsed=static_cast<double>(wipe.frame);
    const double gravity=2.0*wipe.height/(static_cast<double>(fall)*fall);
    const double velocity=gravity*fall;
    double height;
    if (wipe.frame<fall) {
        const auto t=elapsed/fall;
        height=wipe.height*(1.0-t*t);
    } else if (wipe.frame-fall<first_bounce) {
        const double t=elapsed-fall;
        height=velocity*t/3.0-gravity*t*t/2.0;
    } else {
        const double t=elapsed-fall-first_bounce;
        height=velocity*t/9.0-gravity*t*t/2.0;
    }
    const int offset=std::clamp(static_cast<int>(height+0.5),0,wipe.height);
    if (move_old) {
        wipe.rectangle(*wipe.before,0,0,wipe.width,offset,0,wipe.height-offset);
        wipe.rectangle(*wipe.after,0,0,wipe.width,wipe.height-offset,0,0);
    } else {
        wipe.rectangle(*wipe.before,0,wipe.height-offset,wipe.width,offset,0,wipe.height-offset);
        wipe.rectangle(*wipe.after,0,offset,wipe.width,wipe.height-offset,0,0);
    }
}

void raster(const Wipe& wipe,bool black_edges) {
    const bool second=wipe.frame>=wipe.frames/2U;
    const auto& source=second?*wipe.after:*wipe.before;
    const double amplitude=1.8*wipe.width*(second?wipe.frames-wipe.frame:wipe.frame)/wipe.frames;
    const double radians=3.14159*8.0/wipe.height;
    for (int y=0;y<wipe.height;++y) {
        const double phase=(y+10.0*wipe.frame)*radians;
        const int displacement=std::clamp(static_cast<int>(std::cos(phase)*amplitude+0.5),1-wipe.width,wipe.width-1);
        const int left=std::max(0,-displacement),right=wipe.width-std::max(0,displacement);
        wipe.rectangle(source,std::max(0,displacement),y,right-left,1,left,y);
        if (!black_edges) {
            wipe.repeat_pixel(source,0,y,0,left);
            wipe.repeat_pixel(source,wipe.width-1,y,right,wipe.width);
        } else {
            auto* row=wipe.output.rgb.data()+wipe.offset(0,y);
            std::fill(row,row+left*3,0);
            std::fill(row+right*3,row+wipe.width*3,0);
        }
    }
}

template<std::size_t N>
void shape_row(const Wipe& wipe,int y,const std::array<int,N>& lengths) {
    int x=0;
    bool reveal=false;
    for (const int length:lengths) {
        const int end=std::min(wipe.width,x+std::max(0,length));
        wipe.span(reveal?*wipe.after:*wipe.before,y,x,end);
        x=end;
        reveal=!reveal;
    }
    wipe.span(*wipe.before,y,x,wipe.width);
}

void shape_span(const Wipe& wipe,int y,int left,int right) {
    const int start=std::clamp(left,0,wipe.width),end=std::clamp(right+1,start,wipe.width);
    shape_row(wipe,y,std::array{start,end-start});
}

void shape_lobes(const Wipe& wipe,int y,int left,int right,int left2,int right2) {
    // The original six-segment mask uses an inclusive left lobe and counts
    // the center gap from the clamped right edge of that lobe.
    left=std::clamp(left,0,wipe.width);
    right=std::min(right,wipe.width-1);
    const int gap=std::min(left2,wipe.width)+1-std::max(0,right);
    shape_row(wipe,y,std::array{left,right+1-left,gap,right2-left2});
}

int scan_x(int from,int to,int row,int begin,int end) {
    return from+(to-from)*(row-begin)/std::max(1,end-begin);
}

void star(Wipe wipe,bool reverse) {
    if (reverse) wipe.reverse();
    const double radius=1.8*wipe.width*wipe.frame/wipe.frames;
    const double inner=radius*(3.0-std::sqrt(5.0))/2.0;
    const double sin18=std::sin(3.14159/10.0),cos18=std::cos(3.14159/10.0);
    const double sin36=std::sin(3.14159/5.0),cos36=std::cos(3.14159/5.0);
    const int cx=wipe.width/2,cy=wipe.height/2;
    const int y0=static_cast<int>(-radius+0.5),y1=static_cast<int>(-radius*sin18+0.5);
    const int y3=static_cast<int>(inner*sin18+0.5),y4=static_cast<int>(inner+0.5),y5=static_cast<int>(radius*cos36+0.5);
    const int x1=static_cast<int>(-inner*sin36+0.5),x2=static_cast<int>(-radius*cos18);
    const int x3=static_cast<int>(-inner*cos18+0.5),x5=static_cast<int>(-radius*sin36+0.5);
    for (int y=0;y<wipe.height;++y) {
        const int row=y-cy;
        if (row<y0 || row>=y5) wipe.span(*wipe.before,y,0,wipe.width);
        else if (row<y1) {
            const int x=scan_x(0,x1,row,y0,y1-1);
            shape_span(wipe,y,cx+x,cx-x);
        } else if (row<y3) {
            const int x=scan_x(x2,x3,row,y1,y3-1);
            shape_span(wipe,y,cx+x,cx-x);
        } else if (row<y4) {
            const int x=scan_x(x3,x5,row,y3,y5);
            shape_span(wipe,y,cx+x,cx-x);
        } else {
            const int x=scan_x(x3,x5,row,y3,y5-1),split=scan_x(0,x5,row,y4,y5-1);
            shape_lobes(wipe,y,cx+x,cx+split,cx-split,cx-x);
        }
    }
}

void heart(Wipe wipe,bool reverse) {
    if (reverse) wipe.reverse();
    const double radius=0.6*wipe.width*wipe.frame/wipe.frames;
    const double radius2=radius*radius,diagonal=radius*std::sqrt(2.0)/2.0;
    const int cx=wipe.width/2,cy=wipe.height/2;
    const int top=static_cast<int>(-radius-diagonal+0.5),middle=static_cast<int>(-diagonal+0.5);
    const int tip=static_cast<int>(radius*2.0+0.5),base_x=static_cast<int>(-radius-diagonal+0.5);
    const auto span_at=[&](int distance) {return std::sqrt(std::max(0.0,radius2-static_cast<double>(distance)*distance));};
    const auto round=[](double x){return static_cast<int>(x+0.5);};
    for (int y=0;y<wipe.height;++y) {
        const int row=y-cy;
        if (row<top || row>tip) wipe.span(*wipe.before,y,0,wipe.width);
        else if (row<=middle) {
            const auto x=span_at(middle-row);
            shape_lobes(wipe,y,round(cx-radius-x),round(cx-radius+x),round(cx+radius-x),round(cx+radius+x));
        } else if (row<=0) {
            const auto x=span_at(row-middle-1);
            shape_span(wipe,y,round(cx-radius-x),round(cx+radius+x));
        } else {
            const auto x=scan_x(base_x,0,row,1,tip);
            shape_span(wipe,y,cx+x,cx-x);
        }
    }
}

struct StarRow {
    int count=0;
    std::array<int,4> x{};

    void add(int position,bool right) {
        if (right && count==2) x[1]=std::max(x[1],position);
        else if (count<4 && (count%2!=0)==right) x[static_cast<std::size_t>(count++)]=position;
    }
};

void turning_star(Wipe wipe,bool reverse,bool slow) {
    if (reverse) wipe.reverse();
    const double radius=1.8*wipe.width*wipe.frame/wipe.frames;
    const double inner=radius*(3.0-std::sqrt(5.0))/2.0;
    const int rotation=wipe.scale(slow?216:720)%72;
    const int cx=wipe.width/2,cy=wipe.height/2;
    constexpr double radians=3.14159/180.0;
    std::array<Point,10> points{};
    for (std::size_t i=0;i<5;++i) {
        const int degree=rotation+198+static_cast<int>(i)*72;
        const auto outer=degree%360*radians,inside=(degree+36)%360*radians;
        points[i]={static_cast<int>(cx+radius*std::cos(outer)+0.5),static_cast<int>(cy+radius*std::sin(outer)+0.5)};
        points[i+5]={static_cast<int>(cx+inner*std::cos(inside)+0.5),static_cast<int>(cy+inner*std::sin(inside)+0.5)};
    }
    // Original insertion order distinguishes the left and right edges when
    // two outer points meet on the same scanline.
    struct Edge {std::size_t from,to;bool right;};
    constexpr std::array<Edge,10> edges{{
        {0,9,false},{0,5,true},{9,4,false},{8,4,true},{1,5,false},
        {1,6,true},{8,3,false},{7,3,true},{2,7,true},{6,2,true}
    }};
    std::vector<StarRow> rows(static_cast<std::size_t>(wipe.height));
    for (auto edge:edges) {
        auto start=points[edge.from],end=points[edge.to];
        if (end.y<start.y) {
            std::swap(start,end);
            edge.right=!edge.right;
        }
        for (int y=std::max(0,start.y);y<=std::min(wipe.height-1,end.y);++y)
            rows[static_cast<std::size_t>(y)].add(scan_x(start.x,end.x,y,start.y,end.y),edge.right);
    }
    for (int y=0;y<wipe.height;++y) {
        auto row=rows[static_cast<std::size_t>(y)];
        if (!row.count) {
            wipe.span(*wipe.before,y,0,wipe.width);
            continue;
        }
        for (int i=row.count;i<4;++i) row.x[static_cast<std::size_t>(i)]=wipe.width;
        for (auto& x:row.x) x=std::clamp(x,0,wipe.width);
        shape_row(wipe,y,std::array{row.x[0],row.x[1]-row.x[0],row.x[2]-row.x[1],row.x[3]-row.x[2]});
    }
}

struct ShardEdge {
    const Point* points;
    int last,index,direction;
    int row=0;

    Point point(int i) const {return points[std::clamp(i,0,last)];}
    int height() const {return point(index+direction).y-point(index).y;}
    int x() const {
        const auto start=point(index),end=point(index+direction);
        const auto fraction=1.0F/static_cast<float>(std::max(1,height()));
        return static_cast<int>(static_cast<float>(start.x)+
            (static_cast<float>(end.x-start.x)*static_cast<float>(row))*fraction+0.5F);
    }
    void advance() {
        if (++row>=height()) {index+=direction;row=0;}
    }
    void finish_previous() {index-=direction;row=height();}
};

void shatter(Wipe wipe,bool reverse) {
    if (reverse) wipe.reverse();
    wipe.rectangle(*wipe.after,0,0,wipe.width,wipe.height,0,0);
    std::array<Point,default_shard_mesh.vertices.size()> vertices{};
    for (std::size_t i=0;i<vertices.size();++i)
        vertices[i]=scale_shard_vertex(default_shard_mesh.vertices[i],wipe.width,wipe.height);

    const auto hold=wipe.frames/5U;
    const auto early=(10.0F*0.001F)*static_cast<float>(std::min(wipe.frame,hold));
    const auto burst=static_cast<float>(std::hypot(wipe.width,wipe.height))/30.0F*0.3F;
    const auto elapsed=static_cast<float>(wipe.frame-std::min(wipe.frame,hold));
    const auto duration=static_cast<float>(wipe.frames-hold);
    for (const auto& face:default_shard_mesh.faces) {
        const int count=face[0];
        std::array<Point,10> polygon{};
        Point center{};
        int bottom=0;
        for (int i=0;i<count;++i) {
            const auto point=vertices[static_cast<std::size_t>(face[static_cast<std::size_t>(i+1)])];
            polygon[static_cast<std::size_t>(i)]=point;
            center.x+=point.x;
            center.y+=point.y;
            bottom=std::max(bottom,point.y);
        }
        polygon[static_cast<std::size_t>(count)]=polygon[0];
        const int x=center.x/count-wipe.width/2,y=center.y/count-wipe.height/2;
        const double angle=std::atan2(static_cast<double>(y),static_cast<double>(x));
        const double speed=30.0*(1.0+std::max(1.0,std::hypot(x,y))/200.0);
        const auto offset=[&](float velocity) {
            return static_cast<int>(velocity*early+velocity*elapsed*burst/duration+0.5F);
        };
        const int dx=offset(static_cast<float>(std::cos(angle)*speed));
        const int dy=offset(static_cast<float>(std::sin(angle)*speed));
        const int top=polygon[0].y;
        if (bottom+dy<0 || top+dy>=wipe.height || dx<=-wipe.width || dx>=wipe.width) continue;
        ShardEdge left{polygon.data(),count,0,1},right{polygon.data(),count,count,-1};
        if (right.height()==0) --right.index;
        for (int sy=top;sy<=bottom && sy+dy<wipe.height;++sy) {
            const int from=left.x(),to=right.x();
            if (to>from && sy+dy>=0) wipe.rectangle(*wipe.before,from,sy,to-from+1,1,from+dx,sy+dy);
            left.advance();
            right.advance();
            if (sy==bottom-1 && (!left.height() || !right.height())) {
                left.finish_previous();
                right.finish_previous();
            }
        }
    }
}

void gaza(Wipe wipe,bool reverse,int gradient) {
    if (reverse) wipe.reverse();
    std::uint32_t seed=1;
    for (int y=0;y<wipe.height;++y) {
        const int start=-static_cast<int>(next_random(seed))*wipe.width/32768-gradient;
        const int end=wipe.width+static_cast<int>(next_random(seed))*wipe.width/65536;
        wipe.edge(y,start+wipe.scale(end-start),gradient);
    }
}

void beach(Wipe wipe,bool reverse,int gradient) {
    if (reverse) wipe.reverse();
    constexpr double radians=3.14159/180.0;
    constexpr std::array<int,3> phase{0,70,250},wave{2,3,7},time{35,53,21};
    const auto count=static_cast<int>(wipe.frame%360U);
    std::array<double,3> amplitude{8.0,5.0,2.0};
    for (std::size_t i=0;i<amplitude.size();++i)
        amplitude[i]*=wipe.width*(std::sin((count*time[i])%360*radians)+1.5)/500.0;
    const int start=static_cast<int>((std::int64_t{wipe.width}*13*wipe.frame)/(10LL*wipe.frames))-wipe.width*3/20;
    for (int y=0;y<wipe.height;++y) {
        double x=start;
        for (std::size_t i=0;i<amplitude.size();++i)
            x+=amplitude[i]*std::sin((phase[i]+count*17+wave[i]*y)%360*radians);
        wipe.edge(y,static_cast<int>(x+0.5),gradient);
    }
}

void mosaic(Wipe wipe) {
    const auto half=std::max(1U,wipe.frames/2U);
    if (wipe.frame>=half) wipe.reverse();
    const int block=static_cast<int>(std::clamp<std::uint64_t>((std::uint64_t{wipe.frame}+1)*64/half,1,64));
    std::uint32_t seed=wipe.frame+1;
    const auto limit=[&](int size){return static_cast<unsigned>(size%block==0?block:size%block);};
    const int dx=static_cast<int>(next_random(seed)%limit(wipe.width));
    const int dy=static_cast<int>(next_random(seed)%limit(wipe.height));
    for (int y=0;y<wipe.height;++y) {
        for (int x=0;x<wipe.width;++x) {
            const auto src=wipe.offset(x/block*block+dx,y/block*block+dy),dst=wipe.offset(x,y);
            std::memcpy(wipe.output.rgb.data()+dst,wipe.before->rgb.data()+src,3);
        }
    }
}

void random_strip(const Wipe& wipe) {
    // Each frame consumes one new CRT sample per row. Re-rendering a frame or
    // resuming mid-wipe recreates the same samples independently of draw order.
    auto seed=random_at(static_cast<std::uint64_t>(wipe.frame)*static_cast<unsigned>(wipe.height));
    for (int y=0;y<wipe.height;++y) {
        const auto& source=next_random(seed)%wipe.frames<wipe.frame?*wipe.after:*wipe.before;
        wipe.span(source,y,0,wipe.width);
    }
}

void add(Wipe wipe) {
    auto half=std::max(1U,wipe.frames/2U);
    if (wipe.frame>=half) {
        wipe.reverse();
        half=std::max(1U,wipe.frames-half);
    }
    const auto gain=static_cast<int>(std::min(256ULL,256ULL*wipe.frame/half));
    for (std::size_t i=0;i<wipe.output.rgb.size();++i)
        wipe.output.rgb[i]=static_cast<Byte>(std::min(255,(*wipe.before).rgb[i]+((*wipe.after).rgb[i]*gain>>8)));
}

void turn_page(const Wipe& wipe) {
    const int fold=wipe.scale(wipe.height);
    const int reflected=std::min(std::max(0,fold-1),wipe.height-fold);
    wipe.rectangle(*wipe.after,0,0,wipe.width,fold,0,0);
    for (int y=0;y<reflected;++y)
        wipe.rectangle(*wipe.after,0,fold-2-y,wipe.width,1,0,fold+y);
    wipe.rectangle(*wipe.before,0,fold+reflected,wipe.width,wipe.height-fold-reflected,0,fold+reflected);
}

void blind_rows(const Wipe& wipe,int first,int last,int travel,bool down) {
    const int direction=down?1:-1;
    int row=down?last-travel:first+travel;
    const int edge=std::clamp(row,first,last);
    const int start=down?first:edge,end=down?edge:last;
    wipe.rectangle(*wipe.before,0,start,wipe.width,end-start,0,start);
    int speed=0;
    for (int source=row;down?source<last:source>=first;source+=direction) {
        if (row>=first && row<last && source>=first && source<last)
            wipe.rectangle(*wipe.before,0,source,wipe.width,1,0,row);
        speed=std::min(8,speed+1);
        row+=direction*speed;
    }
}

void blinds(Wipe wipe,int type) {
    if (type%2) wipe.reverse();
    const int distance=wipe.height+wipe.height/8+28;
    wipe.rectangle(*wipe.after,0,0,wipe.width,wipe.height,0,0);
    if (type<4) blind_rows(wipe,0,wipe.height,wipe.scale(distance),type<2);
    else {
        const int half=wipe.height/2,travel=wipe.scale(distance/2);
        blind_rows(wipe,0,half,travel,false);
        blind_rows(wipe,half,wipe.height,travel,true);
    }
}

void multiply(Wipe wipe) {
    const auto half=std::max(1U,wipe.frames/2U);
    if (wipe.frame>half) wipe.reverse();
    const auto alpha=static_cast<int>(256ULL*wipe.frame/half);
    for (std::size_t i=0;i<wipe.output.rgb.size();++i) {
        const int before=wipe.before->rgb[i],after=wipe.after->rgb[i];
        wipe.output.rgb[i]=static_cast<Byte>((before*(256-alpha)+((before*after)>>8)*alpha)>>8);
    }
}

void push(const Wipe& wipe,int direction) {
    constexpr int dx[]={0,-1,0,1},dy[]={1,0,-1,0};
    const int x=wipe.scale(wipe.width)*dx[direction],y=wipe.scale(wipe.height)*dy[direction];
    wipe.rectangle(*wipe.before,0,0,wipe.width,wipe.height,x,y);
    wipe.rectangle(*wipe.after,0,0,wipe.width,wipe.height,x-wipe.width*dx[direction],y-wipe.height*dy[direction]);
}

void diagonal(Wipe wipe,int direction) {
    if (direction>=2) wipe.reverse();
    constexpr int gradient=64;
    const int offset=wipe.scale(gradient+wipe.width+wipe.height)-gradient;
    for (int y=0;y<wipe.height;++y) {
        const int distance=direction==1||direction==2?wipe.height-y:y;
        wipe.edge(y,offset-distance,gradient);
    }
}

int square_scale(int extent,std::uint32_t elapsed,std::uint32_t duration) {
    const auto numerator=std::uint64_t{elapsed}*elapsed;
    if (numerator<=UINT64_MAX/static_cast<unsigned>(extent))
        return static_cast<int>((numerator*static_cast<unsigned>(extent))/(std::uint64_t{duration}*duration));
    const double ratio=static_cast<double>(elapsed)/duration;
    return static_cast<int>(extent*ratio*ratio);
}

void slots(const Wipe& wipe,int count,bool horizontal) {
    if (wipe.frame>=wipe.frames-1U) {
        wipe.rectangle(*wipe.after,0,0,wipe.width,wipe.height,0,0);
        return;
    }
    constexpr std::array<int,5> turns{2,3,5,4,1};
    const int length=horizontal?wipe.width:wipe.height,breadth=horizontal?wipe.height:wipe.width;
    const int width=breadth/count;
    for (int i=0;i<count;++i) {
        const int start=i*width,size=i==count-1?breadth-start:width;
        const auto delay=static_cast<unsigned>(count+1-i);
        const auto duration=wipe.frames>delay?wipe.frames-delay:1U;
        const auto elapsed=wipe.frame<duration?duration-wipe.frame:0U;
        const int revolution=length*2;
        int position=square_scale((turns[static_cast<std::size_t>(i)]*2+1)*length,elapsed,duration)%revolution;
        if (i%2) position=-position;
        position=(position+length+revolution)%revolution;
        const auto band=[&](const Surface& source,int shift) {
            if (horizontal) wipe.rectangle(source,0,start,length,size,shift,start);
            else wipe.rectangle(source,start,0,size,length,start,shift);
        };
        band(*wipe.before,position-revolution);
        band(*wipe.after,position-length);
        band(*wipe.before,position);
    }
}

void zoom(Wipe wipe,int level) {
    const auto half=std::max(1U,wipe.frames/2U);
    if (wipe.frame>=half) wipe.reverse();
    const auto progress=std::min(wipe.frame,half);
    const int divisor=4<<level;
    const auto crop=[&](int size) {
        const int middle=size/2,radius=size/divisor/2;
        const auto interpolate=[&](int distance) {
            return static_cast<int>(std::int64_t{distance}*progress/half);
        };
        const int begin=interpolate(middle-radius);
        const int end=size+interpolate(middle+radius-size);
        return std::pair<int,int>{begin,std::max(1,end-begin)};
    };
    const auto [left,w]=crop(wipe.width);
    const auto [top,h]=crop(wipe.height);
    const int dx=w*65536/wipe.width,dy=h*65536/wipe.height;
    for (int y=0;y<wipe.height;++y) {
        const int sy=top+((y*dy)>>16);
        for (int x=0;x<wipe.width;++x)
            wipe.pixel(*wipe.before,left+((x*dx)>>16),sy,x,y);
    }
}

void center(Wipe wipe,bool reverse,bool transparent) {
    if (reverse) wipe.reverse();
    const int half_x=wipe.scale(wipe.width)/2,half_y=wipe.scale(wipe.height)/2;
    const int left=wipe.width/2-half_x,top=wipe.height/2-half_y;
    const int width=half_x*2,height=half_y*2;
    wipe.rectangle(*wipe.before,0,0,wipe.width,wipe.height,0,0);
    if (!width || !height) return;
    const int dx=wipe.width*65536/width,dy=wipe.height*65536/height;
    const int alpha=wipe.scale(100)*256/100;
    for (int y=0;y<height;++y) {
        const int sy=(y*dy)>>16;
        for (int x=0;x<width;++x) {
            const int sx=(x*dx)>>16;
            if (!transparent) wipe.pixel(*wipe.after,sx,sy,left+x,top+y);
            else {
                const auto* pixel=wipe.after->rgb.data()+wipe.offset(sx,sy);
                if (std::memcmp(pixel,wipe.after->rgb.data(),3)==0) continue;
                auto* dst=wipe.output.rgb.data()+wipe.offset(left+x,top+y);
                for (int c=0;c<3;++c)
                    dst[c]=static_cast<Byte>(((pixel[c]*alpha)>>8)+((dst[c]*(255-alpha))>>8));
            }
        }
    }
}

void pointed_edge(Wipe wipe,bool reverse,int gradient) {
    if (reverse) wipe.reverse();
    const int slope=(wipe.height/2)*2;
    const int position=wipe.scale(wipe.width+slope+gradient)-gradient;
    for (int y=0;y<wipe.height;++y) {
        const int distance=2*(y>=wipe.height/2?wipe.height-y:y);
        wipe.edge(y,position-(reverse?distance:slope-distance),gradient);
    }
}

void double_pointed_edge(Wipe wipe,bool reverse) {
    if (reverse) wipe.reverse();
    const int slope=wipe.height/2,position=wipe.scale(wipe.width/2+slope);
    for (int y=0;y<wipe.height;++y) {
        const int distance=y>=wipe.height/2?wipe.height-y:y;
        const int left=std::clamp(position-slope+distance,0,wipe.width/2);
        const int right=wipe.width-left;
        wipe.span(*wipe.after,y,0,left);
        wipe.span(*wipe.before,y,left,right);
        wipe.span(*wipe.after,y,right,wipe.width);
    }
}
}

bool supports_wipe(int type) noexcept {
    return type>=0&&type<=95;
}

void draw_wipe(Surface& output,const Surface& before,const Surface& after,int type,
               std::uint32_t frame,std::uint32_t frames) {
    const auto width=output.width,height=output.height;
    const auto pixels=static_cast<std::uint64_t>(width)*height;
    if (!pixels || width>max_dimension || height>max_dimension || pixels>max_pixels ||
        before.width!=width || before.height!=height || after.width!=width || after.height!=height)
        throw std::invalid_argument("Incompatible wipe surface dimensions");
    const auto bytes=static_cast<std::size_t>(pixels)*3;
    if (output.rgb.size()!=bytes || before.rgb.size()!=bytes || after.rgb.size()!=bytes)
        throw std::invalid_argument("Incompatible wipe pixel buffers");
    const Wipe wipe{output,&before,&after,frame,frames,static_cast<int>(output.width),static_cast<int>(output.height)};
    if (frame==0 || frame>=frames) {
        const auto& image=frame>=frames?after:before;
        std::copy(image.rgb.begin(),image.rgb.end(),output.rgb.begin());
    } else if (type==1||type==2) curtain(wipe,type==2);
    else if (type==3) zebra(wipe);
    else if (type==4||type==5) slide(wipe,type==5);
    else if (type>=6&&type<=8) circle(wipe,type==7,type==8);
    else if (type==9) clock(wipe);
    else if (type>=10&&type<=17) move(wipe,type);
    else if (type>=18&&type<=21) door(wipe,type);
    else if (type==22) paint_over(wipe);
    else if (type==23||type==73) gravity(wipe,type==73);
    else if (type==24||type==25) gaza(wipe,type==25,0);
    else if (type==26||type==27) raster(wipe,type==27);
    else if (type==28||type==29) star(wipe,type==29);
    else if (type==30||type==31) heart(wipe,type==31);
    else if (type==32||type==33||type==78||type==79) turning_star(wipe,type%2!=0,type>=78);
    else if (type==34||type==35) shatter(wipe,type==35);
    else if (type==36||type==37) beach(wipe,type==37,0);
    else if (type==38) mosaic(wipe);
    else if (type==39) random_strip(wipe);
    else if (type==40) add(wipe);
    else if (type==41) turn_page(wipe);
    else if (type>=42&&type<=47) blinds(wipe,type-42);
    else if (type>=48&&type<=53) gaza(wipe,type>=51,64<<((type-48)%3));
    else if (type>=54&&type<=59) beach(wipe,type>=57,64<<((type-54)%3));
    else if (type==60) multiply(wipe);
    else if (type>=61&&type<=64) push(wipe,type-61);
    else if (type>=65&&type<=68) diagonal(wipe,type-65);
    else if (type>=69&&type<=72) slots(wipe,type-67,false);
    else if (type>=74&&type<=77) slots(wipe,type-72,true);
    else if (type>=80&&type<=83) zoom(wipe,type-80);
    else if (type>=84&&type<=87) center(wipe,type%2!=0,type>=86);
    else if (type>=88&&type<=93) pointed_edge(wipe,type==89||type>=92,type<90?0:(type%2?256:64));
    else if (type==94||type==95) double_pointed_edge(wipe,type==95);
    else {
        const auto alpha=static_cast<unsigned>((std::uint64_t{frame}*256U)/frames);
        for (std::size_t i=0;i<output.rgb.size();++i)
            output.rgb[i]=static_cast<Byte>((before.rgb[i]*(256U-alpha)+after.rgb[i]*alpha+128U)>>8U);
    }
}
}
