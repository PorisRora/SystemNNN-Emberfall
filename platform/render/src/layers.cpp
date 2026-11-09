#include "emberfall/layers.hpp"
#include "effects.hpp"
#include "wipes.hpp"
#include "nnn/snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace emberfall::render {
namespace {
template<class A,class R> void rectangle_fields(A& a,R& r) { a(r.x,r.y,r.width,r.height); }
template<class A,class S> void surface_fields(A& a,S& s) { a(s.width,s.height,s.rgb); }
template<class A,class S> void effect_fields(A& a,S& s,bool once_state,bool particle_state) {
    a(s.offset_x,s.offset_y,s.distance,s.seed,s.rain_initialized);
    for (auto& p:s.rain) a(p.x,p.y,p.speed_x,p.speed_y,p.random_x,p.random_y);
    a.records(s.noise,max_pixels/16,[](auto& ar,auto& v){ar(v);});
    a(s.noise_phase,s.raster,s.water_count,s.lamp_mode,s.lamp_time,s.lamp_visible);
    a.records(s.fireflies,65536,[](auto& ar,auto& v){ar(v.x,v.y,v.speed_x,v.speed_y,v.frame,v.in,v.keep,v.out,v.phase,v.period);});
    a.records(s.bands,65536,[](auto& ar,auto& v){ar(v.remaining,v.y,v.length,v.strength);});
    a.records(s.drops,65536,[](auto& ar,auto& v){ar(v.x,v.y,v.direction,v.speed,v.length);});
    a.records(s.old_fireflies,65536,[](auto& ar,auto& v){ar(v.x,v.y,v.speed_x,v.speed_y,v.phase,v.period);});
    a.records(s.scratches,65536,[](auto& ar,auto& v){ar(v.x,v.speed,v.change,v.life,v.percent,v.visible);});
    a.records(s.shards,4096,[](auto& ar,auto& v){ar(v.points,v.speed_x,v.speed_y,v.gravity);});
    if (once_state) a(s.once_drawn);
    if (particle_state) a.records(s.particles,300,[](auto& ar,auto& v) {
        ar(v.x,v.y,v.velocity_x,v.velocity_y,v.phase,v.period,v.variant,v.row,v.reverse);
    });
}
using Color = std::array<int,3>;
constexpr double pi=3.14159265358979323846;
int arg(const Layer& l,std::size_t i,int def=0) { return i<l.parameters.size() ? l.parameters[i] : def; }
int duration_of(const Layer& l) {
    if (effects::known(l.effect)) return static_cast<int>(effects::duration(l));
    if (l.effect==0) return 99999;
    if (l.effect==8) return 1;
    if (l.effect==54) return 9999;
    return std::max(1,arg(l,0,20));
}
int extent(double value) { return static_cast<int>(std::lround(std::clamp(value,-10000000.0,10000000.0))); }
int mix(int a,int b,double t) { return extent(a+(static_cast<double>(b)-a)*t); }
unsigned channel(int v) { return static_cast<unsigned>(std::clamp(v,0,255)); }

double curve(double tick, int length, int type=0, int delay=0) {
    delay=std::max(0,delay);
    const double duration=std::max(1,length), d=std::max(1,delay);
    const double c=std::clamp(tick,0.0,duration);
    double t=c/duration;
    const int mode=std::max(type,0)%100;
    const double phase=std::fmod(c,2*d)/d;
    switch (mode%15) {
    case 1: t=(c-delay)/std::max(1.0,duration-delay); break;
    case 2: t=c<delay ? c/d : 1; break;
    case 3: t=c<delay ? c/d : (duration-c)/std::max(1.0,duration-delay); break;
    case 4: t=c<delay ? c/d : c>=duration-delay ? (duration-c)/d : 1; break;
    case 5: t=1-std::abs(1-phase); break;
    case 6: t=std::abs(1-phase); break;
    case 7: t=std::fmod(c,d)/d; break;
    case 8: t=1-std::fmod(c,d)/d; break;
    case 9: {
        const double last=duration-4*d, half=std::max(1.0,std::floor(last/2.0)), offset=c-4*d;
        t=c<=4*d ? 1-std::abs(1-phase) : (offset<=half ? offset : last-offset)/half;
        break;
    }
    case 10: t=c<=2*d ? std::fmod(c,d)/d : (c-2*d)/std::max(1.0,duration-2*d); break;
    default: break;
    }
    t=std::clamp(t,0.0,1.0);
    switch (mode/15) {
    case 2: t*=t; break;
    case 3: break; // The original default custom exponent is 100 percent.
    case 4: t=std::sqrt(t); break;
    case 5: t=(1-std::cos(t*3.14159))*0.5; break;
    default: break;
    }
    return ((type/100)%2) ? 1-t : t;
}
Color color(const Layer& l,std::size_t i,int def=0) {
    const int r=arg(l,i,def),g=arg(l,i+1,r);
    return {r,g,arg(l,i+2,g)};
}
void tint(Surface& out,Color value,int alpha) {
    if (alpha<=0) return;
    const std::array<unsigned,3> c{channel(value[0]),channel(value[1]),channel(value[2])};
    if (alpha>=255 && c[0]==c[1] && c[1]==c[2]) { std::fill(out.rgb.begin(),out.rgb.end(),static_cast<Byte>(c[0])); return; }
    const unsigned a=channel(alpha);
    for (std::size_t i=0; i<out.rgb.size(); i+=3)
        for (unsigned j=0; j<3; ++j) out.rgb[i+j]=static_cast<Byte>((out.rgb[i+j]*(255U-a)+c[j]*a+127U)/255U);
}

void picture(Surface& out,const Texture& t,Rect src,Rect dst,int percent,Color add,bool use_alpha=true) {
    if (percent<=0 || src.width<=0 || src.height<=0 || dst.width<=0 || dst.height<=0) return;
    const Rect cut=t.content;
    const int x0=std::max(0,dst.x), y0=std::max(0,dst.y);
    const int x1=std::min(static_cast<int>(out.width),dst.x+dst.width);
    const int y1=std::min(static_cast<int>(out.height),dst.y+dst.height);
    if (x1<=x0 || y1<=y0) return;
    if (src.width==dst.width && src.height==dst.height && percent>=100 && add==Color{} && use_alpha) {
        const int sx=std::max(src.x,cut.x),sy=std::max(src.y,cut.y);
        const int ex=std::min(src.x+src.width,cut.x+cut.width),ey=std::min(src.y+src.height,cut.y+cut.height);
        if (ex>sx && ey>sy)
            blit(t.pixels,out,{sx-cut.x,sy-cut.y,ex-sx,ey-sy},{dst.x+sx-src.x,dst.y+sy-src.y},BlendProfile::Source32Mmx);
        return;
    }
    const unsigned opacity=static_cast<unsigned>(std::min(100,percent));
    for (int y=y0; y<y1; ++y) {
        const int sy=src.y+static_cast<int>((std::int64_t{y-dst.y}*src.height)/dst.height)-cut.y;
        if (sy<0 || sy>=cut.height) continue;
        auto di=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x0))*3;
        for (int x=x0; x<x1; ++x,di+=3) {
            const int sx=src.x+static_cast<int>((std::int64_t{x-dst.x}*src.width)/dst.width)-cut.x;
            if (sx<0 || sx>=cut.width) continue;
            const auto si=(static_cast<std::size_t>(sy)*t.pixels.width+static_cast<unsigned>(sx))*4;
            const unsigned a=((use_alpha ? t.pixels.rgba[si+3] : 255U)*opacity+50U)/100U;
            if (!a) continue;
            for (unsigned j=0;j<3;++j) {
                const unsigned c=channel(t.pixels.rgba[si+j]+std::clamp(add[j],-255,255));
                if (a==255U) out.rgb[di+j]=static_cast<Byte>(c);
                else out.rgb[di+j]=static_cast<Byte>(((c*a)>>8U)+((out.rgb[di+j]*(256U-a))>>8U));
            }
        }
    }
}

void shift_screen(Surface& out,int dx,int dy,Color fill,bool color_edges) {
    if (dx==0 && dy==0) return;
    const auto old=out.rgb;
    if (color_edges) tint(out,fill,255);
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    const int begin=std::max(0,dx),end=std::min(w,w+dx);
    if (end<=begin) return;
    for (int y=std::max(0,dy);y<std::min(h,h+dy);++y) {
        const auto src=(static_cast<std::size_t>(y-dy)*out.width+static_cast<unsigned>(begin-dx))*3;
        const auto dst=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(begin))*3;
        std::copy_n(old.data()+src,static_cast<std::size_t>(end-begin)*3,out.rgb.data()+dst);
    }
}
bool known(int e) {
    switch (e) {
    case -1: case 0: case 1: case 2: case 8: case 9: case 10: case 11: case 17:
    case 52: case 54: case 61: case 65: case 66: case 73: case 99: case 100: return true;
    default: return false;
    }
}
bool animated(int e) { return e==9 || e==10 || e==11 || e==17 || e==52 || e==61 || e==65 || e==99 || e==100; }
double phase(const Layer& l,int loop,int length,int type,int delay) {
    const double n=std::max(1,length),c=l.count;
    return curve(loop ? std::fmod(c,n) : c,length,type,delay);
}
void scale_about(Rect& dst,double sx,double sy,double anchor_x,double anchor_y,double center_x,double center_y) {
    dst.x=extent(center_x-anchor_x*sx); dst.y=extent(center_y-anchor_y*sy);
    dst.width=extent(dst.width*sx); dst.height=extent(dst.height*sy);
}
void draw_capture_picture(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const double time=curve(layer.count,duration_of(layer));
    Rect dst=time>=1 ? layer.to : layer.from,src=layer.source;
    dst.x=mix(layer.from.x,layer.to.x,time);dst.y=mix(layer.from.y,layer.to.y,time);
    dst.width=src.width=std::min(src.width,dst.width);
    dst.height=src.height=std::min(src.height,dst.height);
    picture(out,*layer.texture,src,dst,100,{},!layer.opaque);
}
bool reads_capture(const Layer& layer) {
    switch (layer.effect) {
    case 2: return layer.count || arg(layer,4);
    case 59: return arg(layer,7) || arg(layer,8);
    case 60: return arg(layer,6) || arg(layer,7);
    case 69: return arg(layer,15) || (arg(layer,1)!=-1 && arg(layer,16));
    case 101: return arg(layer,5) || arg(layer,6);
    default: return false;
    }
}
void draw(Surface& out,const std::array<Layer,32>& slots,unsigned slot,const Surface& saved,Surface& save_target,
          const Layers::TextureReader& animation,bool animation_off,bool speaking,bool chart=false) {
    const auto& l=slots[slot];
    if (!l.enabled || l.effect==66) return; // BUFFER keeps the picture available without drawing the slot.
    if (effects::known(l.effect) || (chart && l.effect==0)) {
        effects::draw(out,slots,slot,saved,animation,animation_off,speaking); return;
    }
    const int duration=duration_of(l);
    const double time=curve(l.count,duration);
    if (l.effect==73) { draw_capture_picture(out,l); save_target=out; return; }
    if (l.effect==54) { tint(out,color(l,1),255); return; }
    if ((l.effect==10 || l.effect==11 || l.effect==17) && l.texture) {
        Rect src=l.source;
        const auto& size=time>=1.0 ? l.to : l.from;
        src.width=std::min(src.width,size.width); src.height=std::min(src.height,size.height);
        picture(out,*l.texture,src,{mix(l.from.x,l.to.x,time),mix(l.from.y,l.to.y,time),src.width,src.height},100,{},!l.opaque);
    }
    if (l.effect==10 || l.effect==11) {
        const double f=curve(l.count,duration,arg(l,4),arg(l,5));
        tint(out,color(l,1),extent(255*(l.effect==10 ? 1-f : f))); return;
    }
    if (l.effect==17) {
        const auto on=std::max(0,arg(l,1,1)),off=std::max(0,arg(l,2,on));
        const auto period=std::max<std::int64_t>(1,std::int64_t{on}+off);
        if (l.count>=static_cast<unsigned>(duration) || l.count%period<on) tint(out,color(l,3,255),255);
        return;
    }
    if (l.effect==2 && !saved.rgb.empty() && (l.count || arg(l,4))) { out=saved; return; }
    int shake_x=0, shake_y=0;
    if (l.effect==100) {
        const double strength=arg(l,11) ? curve(l.count,duration,arg(l,12),arg(l,13)) : 1.0;
        auto wave=[&](int amplitude,int speed) {
            double p=std::fmod(1.0*arg(l,6)+std::fmod(static_cast<double>(l.count)*speed,100.0)+50.0,100.0);
            if (p<0) p+=100;
            return extent(amplitude*strength*(1.0-std::abs(p-50.0)/25.0));
        };
        shake_x=wave(arg(l,1,20),arg(l,3,25)); shake_y=wave(arg(l,2),arg(l,4,25));
        if (arg(l,5)==1) { shift_screen(out,shake_x,shake_y,color(l,8),arg(l,7)!=0); return; }
    }
    if (!l.texture) {
        if (l.effect==2) tint(out,color(l,1),255);
        return;
    }
    const auto& tex=*l.texture;
    Rect dst=l.from;
    dst.x=mix(l.from.x,l.to.x,time); dst.y=mix(l.from.y,l.to.y,time);
    if (time>=1.0) { dst.width=l.to.width; dst.height=l.to.height; }
    Rect src=l.source;
    src.width=std::min(src.width,dst.width); src.height=std::min(src.height,dst.height);
    dst.width=src.width; dst.height=src.height;
    int opacity=100;
    bool use_alpha=true;
    Color add{};
    if (l.effect==8) opacity=arg(l,1,50);
    if (l.effect==9) {
        const double f=curve(l.count,duration,arg(l,3),arg(l,4));
        opacity=mix(arg(l,2,100),arg(l,1,50),f);
        if (arg(l,5)) {
            const int x=mix(arg(l,10,arg(l,6)),arg(l,6),f),y=mix(arg(l,11,arg(l,7)),arg(l,7),f);
            const int w=mix(arg(l,12,arg(l,8,static_cast<int>(out.width))),arg(l,8,static_cast<int>(out.width)),f);
            const int h=mix(arg(l,13,arg(l,9,static_cast<int>(out.height))),arg(l,9,static_cast<int>(out.height)),f);
            const int left=std::max(dst.x,x),top=std::max(dst.y,y);
            src.x+=left-dst.x; src.y+=top-dst.y;
            dst.width=src.width=std::min(dst.x+dst.width,x+w)-left;
            dst.height=src.height=std::min(dst.y+dst.height,y+h)-top;
            dst.x=left; dst.y=top;
        }
        if (arg(l,14)) {
            const double t=curve(l.count,duration,arg(l,21),arg(l,22));
            for (unsigned j=0;j<3;++j) add[j]=mix(arg(l,18+j,arg(l,15+j)),arg(l,15+j),t);
        }
    } else if (l.effect==65) {
        const double f=curve(l.count,duration,arg(l,7),arg(l,8));
        const int ex=arg(l,3,100),ey=arg(l,4,ex);
        const double sx=mix(arg(l,5,ex),ex,f)/100.0, sy=mix(arg(l,6,ey),ey,f)/100.0;
        if (arg(l,9)) opacity=mix(arg(l,11,100),arg(l,10,100),curve(l.count,duration,arg(l,12),arg(l,13)));
        src={0,0,tex.canvas_width,tex.canvas_height}; dst.width=src.width; dst.height=src.height;
        scale_about(dst,sx,sy,src.width*0.5+arg(l,1),src.height*0.5+arg(l,2),dst.x+0.5*out.width,dst.y+0.5*out.height);
        if (arg(l,14)) {
            const double t=curve(l.count,duration,arg(l,21),arg(l,22));
            for (unsigned j=0;j<3;++j) add[j]=mix(arg(l,18+j,arg(l,15+j)),arg(l,15+j),t);
        }
    } else if (l.effect==61) {
        double f=phase(l,arg(l,3,1),arg(l,4,duration),arg(l,5),arg(l,6));
        if (arg(l,1)==1) f=(1-std::cos(f*pi))*0.5;
        dst.x+=mix(arg(l,7),arg(l,9),f); dst.y+=mix(arg(l,8),arg(l,10),f);
        opacity=mix(arg(l,12,arg(l,11,100)),arg(l,11,100),phase(l,arg(l,13,arg(l,3,1)),arg(l,14,arg(l,4,duration)),arg(l,15,arg(l,5)),arg(l,16,arg(l,6))));
        const double z=phase(l,arg(l,19,arg(l,3,1)),arg(l,20,arg(l,4,duration)),arg(l,21,arg(l,5)),arg(l,22,arg(l,6)));
        const int ex=arg(l,23,100),ey=arg(l,24,100);
        const double sx=mix(arg(l,25,ex),ex,z)/100.0,sy=mix(arg(l,26,ey),ey,z)/100.0;
        src={0,0,tex.canvas_width,tex.canvas_height}; dst.width=src.width; dst.height=src.height;
        scale_about(dst,sx,sy,src.width*0.5,src.height*0.5,dst.x+src.width*0.5,dst.y+src.height*0.5);
        use_alpha=arg(l,2,1)!=0;
        const double c=phase(l,arg(l,27,arg(l,3,1)),arg(l,28,arg(l,4,duration)),arg(l,29,arg(l,5)),arg(l,30,arg(l,6)));
        for (unsigned j=0;j<3;++j) add[j]=mix(arg(l,34+j,arg(l,31+j)),arg(l,31+j),c);
    } else if (l.effect==99) {
        const auto target=color(l,1);
        const double t=curve(l.count,duration,arg(l,8),arg(l,9));
        for (unsigned j=0;j<3;++j) add[j]=mix(arg(l,4+j,target[j]),target[j],t);
        use_alpha=arg(l,7,1)!=0;
    } else if (l.effect==100) {
        dst.x+=shake_x; dst.y+=shake_y;
    }
    picture(out,tex,src,dst,opacity,add,use_alpha&&!l.opaque);
}
} // namespace

double effects::progress(double tick,int length,int type,int delay) { return curve(tick,length,type,delay); }
void effects::paint(Surface& out,const Texture& tex,Rect src,Rect dst,int percent,std::array<int,3> add,bool alpha) {
    picture(out,tex,src,dst,percent,add,alpha);
}
void paint_texture(Surface& out,const Texture& texture,Rect source,Point origin,int opacity) {
    picture(out,texture,source,{origin.x,origin.y,source.width,source.height},opacity,{});
}

Layers::Layers(int width,int height) {
    if (width<=0 || height<=0 || width>static_cast<int>(max_dimension) || height>static_cast<int>(max_dimension)
        || static_cast<std::uint64_t>(width)*static_cast<unsigned>(height)>max_pixels)
        throw std::invalid_argument("Invalid game canvas size");
    output_={static_cast<unsigned>(width),static_cast<unsigned>(height),std::vector<Byte>(static_cast<std::size_t>(width)*static_cast<unsigned>(height)*3)};
    clear_all();
}
void Layers::clear(unsigned slot) {
    auto& l=slots_.at(slot);
    l={}; l.source=l.from=l.to={0,0,static_cast<int>(output_.width),static_cast<int>(output_.height)};
    invalidate();
}
void Layers::clear_all() { for (unsigned i=0;i<slots_.size();++i) clear(i); }
void Layers::bind(unsigned slot,std::shared_ptr<const Texture> texture,bool preload) {
    auto& l=slots_.at(slot);
    if (l.effect==97 && l.effect_state && (l.texture!=texture || arg(l,8)!=0))
        l.effect_state->condensation.clear();
    l.texture=std::move(texture);
    if (!preload && l.texture) {
        l.enabled=true;
        l.source=l.from=l.to={0,0,l.texture->canvas_width,l.texture->canvas_height};
    }
    invalidate();
}
void Layers::rectangle(unsigned slot,Rect from,Rect to) {
    auto& l=slots_.at(slot); l.from=from; l.to=to; invalidate();
}
bool Layers::effect(unsigned slot,int number,std::vector<std::int32_t> parameters,const FragmentMesh* mesh) {
    if (number==-1) { clear(slot); return true; }
    auto& l=slots_.at(slot);
    if (number==1) return true; // MAE retains the preceding effect and its counter.
    if (((number==40 || number==59 || number==123) && parameters.size()<2) ||
        ((number==60 || number==101) && parameters.size()<3)) return true;
    const bool continues=number==l.effect && (number==70 || number==71) && parameters.size()>12 && parameters[12]!=0;
    auto state=continues || (number==83 && l.effect==83)?l.effect_state:nullptr;
    const bool continues_animation=number==137 && l.effect==137 && l.enabled && l.parameters.size()>7 && l.parameters[7]==0;
    const int previous_animation=continues_animation ? effects::animation_frame(l,animation_off_) : 0;
    l.effect=number; l.parameters=std::move(parameters); l.enabled=true; l.opaque=false; l.count=0; invalidate();
    l.effect_state=std::move(state);
    if (effects::known(number)) effects::configure(l,slot,static_cast<int>(output_.width),static_cast<int>(output_.height),mesh);
    if (continues_animation) { l.parameters[13]=1;l.parameters[15]=previous_animation; }
    return known(number) || effects::known(number);
}
bool Layers::restore_layer(unsigned slot,Layer layer,const FragmentMesh* mesh) {
    auto& l=slots_.at(slot);
    l=std::move(layer);
    const auto number=l.effect;
    if (effects::known(number)) effects::configure(l,slot,static_cast<int>(width()),static_cast<int>(height()),mesh);
    invalidate();
    return known(number) || effects::known(number);
}
unsigned Layers::duration(unsigned slot) const { return static_cast<unsigned>(duration_of(slots_.at(slot))); }
void Layers::set_speaking(bool value) noexcept {
    if (speaking_==value) return;
    speaking_=value;
    for (const auto& layer:slots_) if (layer.enabled && layer.effect==98) { invalidate();break; }
}
namespace {
using ChartRanges=std::array<std::pair<unsigned,unsigned>,3>;
ChartRanges chart_ranges(unsigned center) noexcept {
    return {{{0,center},{center+1,14},{14,16}}};
}
bool chart_layer(const Layer& layer) noexcept { return layer.enabled && layer.effect!=-1 && layer.effect!=66; }
bool chart_composite_layer(const Layer& layer) noexcept {
    if (!chart_layer(layer)) return false;
    // Only source-over or opaque drawing can be extracted as a separate
    // overlay. All other effects need the actual pixels beneath their slot.
    switch (layer.effect) {
    case 0: case 1: case 8: case 9: case 10: case 11: case 17: case 19:
    case 36: case 39: case 52: case 54: case 61: case 62: case 63: case 65:
    case 75: case 79: case 94: case 98: case 99: case 119: case 137:
        return false;
    default:
        return true;
    }
}
template<class Plane> bool chart_extent(const Plane& plane,std::size_t row_bytes,unsigned height) {
    return plane.pixels && plane.stride>=row_bytes && plane.size>=row_bytes &&
        height-1U<=(plane.size-row_bytes)/plane.stride;
}
void chart_over(Surface& out,ChartLayerSource pane) {
    if (!pane.pixels) return;
    auto* dst=out.rgb.data();
    for (unsigned y=0;y<out.height;++y) {
        const auto* src=pane.pixels+static_cast<std::size_t>(y)*pane.stride;
        for (unsigned x=0;x<out.width;++x,dst+=3,src+=4) {
            const unsigned alpha=pane.opaque?255U:src[3];
            if (alpha==255) std::copy_n(src,3,dst);
            else if (alpha)
                for (unsigned c=0;c<3;++c)
                    dst[c]=static_cast<Byte>(std::min(255U,src[c]+(dst[c]*(255U-alpha)+127U)/255U));
        }
    }
}

bool keeps_once_pixels(const std::array<Layer,32>& layers,std::uint32_t hidden) {
    // In the PC renderer these background slots suppress the pre-draw clear.
    constexpr std::array<unsigned,5> background{0,1,2,3,30};
    const bool has_background=std::any_of(background.begin(),background.end(),[&](unsigned slot) {
        const auto& layer=layers[slot];
        return layer.enabled && ((layer.texture && layer.effect!=66) || layer.effect==54 || layer.effect==79);
    });
    if (!has_background) return false;
    for (unsigned slot=0;slot<layers.size();++slot) {
        const auto& layer=layers[slot];
        if (!(hidden&(std::uint32_t{1}<<slot)) && layer.enabled && layer.effect==75 && layer.texture) return true;
    }
    return false;
}
}
bool Layers::screen_layer_active() const noexcept {
    const auto& layer=slots_.back();
    return !overlapping() && (screen_backdrop_ ||
        (!(hidden_&(std::uint32_t{1}<<31U)) && layer.enabled && layer.effect!=-1 && layer.effect!=66));
}

void Layers::capture_screen_layer(bool enable,OverlapDecoration backdrop) {
    const bool decorated=enable && static_cast<bool>(backdrop);
    if (capture_screen_layer_==enable && static_cast<bool>(screen_backdrop_)==decorated) return;
    capture_screen_layer_=enable;
    screen_backdrop_=decorated ? std::move(backdrop) : OverlapDecoration{};
    if (!enable) screen_layer_.reset();
    else invalidate();
}

bool ScreenLayerFrame::captures_screen() const noexcept {
    return draw_last_ && slots_.back().enabled && slots_.back().effect==73;
}

Surface ScreenLayerFrame::capture_without_ui() const {
    auto result=scene_;
    if (captures_screen()) draw_capture_picture(result,slots_.back());
    return result;
}

void ScreenLayerFrame::composite(Byte* pixels,std::size_t stride,bool show_backdrop) {
    if (!pixels || stride<std::size_t{width()}*4) throw std::invalid_argument("Invalid message plane");
    output_.width=width();
    output_.height=height();
    output_.rgb.resize(scene_.rgb.size());
    const auto& background=show_backdrop && !decorated_scene_.rgb.empty() ? decorated_scene_ : scene_;
    for (unsigned y=0;y<height();++y) {
        const auto* overlay=pixels+std::size_t{y}*stride;
        const auto offset=std::size_t{y}*width()*3;
        const auto* scene=background.rgb.data()+offset;
        auto* result=output_.rgb.data()+offset;
        for (unsigned x=0;x<width();++x,overlay+=4,scene+=3,result+=3) {
            const unsigned remain=255U-overlay[3];
            for (unsigned channel=0;channel<3;++channel)
                result[channel]=static_cast<Byte>(std::min(255U,overlay[channel]+(scene[channel]*remain+127U)/255U));
        }
    }
    const auto animation=[this](const std::string& name) {
        for (const auto& item:animation_)
            if (item.first==name) return item.second;
        return std::shared_ptr<const Texture>{};
    };
    const auto& state=slots_.back().effect_state;
    const bool once_drawn=state && state->once_drawn;
    if (captures_screen()) draw_capture_picture(output_,slots_.back());
    else if (draw_last_) draw(output_,slots_,31,saved_,saved_,animation,animation_off_,speaking_);
    if (state) state->once_drawn=once_drawn;
    const auto* rgb=output_.rgb.data();
    for (unsigned y=0;y<height();++y) {
        auto* row=pixels+std::size_t{y}*stride;
        for (unsigned x=0;x<width();++x,row+=4,rgb+=3) {
            std::copy_n(rgb,3,row);
            row[3]=255;
        }
    }
}

void Layers::compose(Surface& out,const OverlapDecoration& decorate) {
    if (saved_background_ && !saved_.rgb.empty()) out=saved_;
    else if (keeps_once_pixels(slots_,hidden_)) {
        if (&out!=&output_) out=output_;
    } else if (out.width!=output_.width || out.height!=output_.height)
        out={output_.width,output_.height,std::vector<Byte>(output_.rgb.size())};
    else std::fill(out.rgb.begin(),out.rgb.end(),0);
    for (unsigned i=0;i+1<slots_.size();++i)
        if (!(hidden_&(std::uint32_t{1}<<i))) draw(out,slots_,i,saved_,saved_,animation_textures_,animation_off_,speaking_);
    constexpr unsigned last=31;
    if (&out==&output_) screen_layer_.reset();
    if (decorate) {
        // Keep a plain endpoint for resuming the script. The wipe shows the PC
        // target with its message window below the final screen effect.
        overlap_display_=out;
        decorate(overlap_display_);
        if (hidden_&(std::uint32_t{1}<<last)) return;
        const auto& layer=slots_[last];
        auto* once=layer.effect==75 ? layer.effect_state.get() : nullptr;
        const bool was_drawn=once && once->once_drawn;
        if (layer.effect!=73) draw(out,slots_,last,saved_,saved_,animation_textures_,animation_off_,speaking_);
        if (once) once->once_drawn=was_drawn;
        draw(overlap_display_,slots_,last,saved_,saved_,animation_textures_,animation_off_,speaking_);
        return;
    }
    const bool draw_last=!(hidden_&(std::uint32_t{1}<<last));
    if (!capture_screen_layer_ || &out!=&output_ || !screen_layer_active()) {
        if (draw_last) draw(out,slots_,last,saved_,saved_,animation_textures_,animation_off_,speaking_);
        return;
    }

    std::unique_ptr<ScreenLayerFrame> frame;
    TextureReader animation;
    try {
        frame=std::make_unique<ScreenLayerFrame>();
        frame->scene_=out;
        if (screen_backdrop_) {
            frame->decorated_scene_=out;
            screen_backdrop_(frame->decorated_scene_);
        }
        frame->saved_=saved_;
        frame->slots_=slots_;
        frame->draw_last_=draw_last;
        // The effect can reference other slots' art and rectangles. Its own
        // animation state is the only one it reads or writes during drawing.
        for (unsigned slot=0;slot<last;++slot) frame->slots_[slot].effect_state.reset();
        if (slots_[last].effect_state)
            frame->slots_[last].effect_state=std::make_shared<effects::State>(*slots_[last].effect_state);
        frame->animation_off_=animation_off_;
        frame->speaking_=speaking_;
        if (animation_textures_) animation=[this,&frame](const std::string& name) {
            auto texture=animation_textures_(name);
            if (frame) try { frame->animation_.emplace_back(name,texture); }
            catch (const std::bad_alloc&) { frame.reset(); }
            return texture;
        };
    } catch (const std::bad_alloc&) {
        if (draw_last) draw(out,slots_,last,saved_,saved_,animation_textures_,animation_off_,speaking_);
        return;
    }
    // GET uses the actual host message plane. Do not replace the buffer that
    // the earlier script layers just consumed with this provisional scene.
    if (frame->captures_screen()) draw_capture_picture(out,slots_.back());
    else if (draw_last) draw(out,slots_,last,saved_,saved_,animation,animation_off_,speaking_);
    screen_layer_=std::move(frame);
}
unsigned Layers::chart_layer_mask(unsigned center) const noexcept {
    if (center>=slots_.size() || overlapping()) return 0;
    const auto ranges=chart_ranges(center);
    unsigned mask=0;
    for (unsigned group=0;group<ranges.size();++group) {
        const auto [first,last]=ranges[group];
        // PrintBackScriptLayers uses PrintLayers without an eye check or clear.
        if (first<last && std::any_of(slots_.begin()+first,slots_.begin()+last,chart_layer))
            mask|=1U<<group;
    }
    return mask;
}
bool Layers::chart_needs_composite(unsigned center) const noexcept {
    if (center>=slots_.size() || overlapping()) return false;
    for (const auto& [first,last]:chart_ranges(center))
        if (first<last && std::any_of(slots_.begin()+first,slots_.begin()+last,chart_composite_layer)) return true;
    return false;
}
unsigned Layers::draw_chart_layers(unsigned center,const std::array<ChartLayerTarget,3>& targets,ChartLayerScratch& scratch) {
    const auto mask=chart_layer_mask(center);
    if (!mask) return 0;
    if (chart_needs_composite(center)) throw std::logic_error("Chart effects require the composed backdrop");
    const auto row_bytes=std::size_t{width()}*4;
    for (unsigned group=0;group<targets.size();++group) {
        if ((mask&(1U<<group)) && !chart_extent(targets[group],row_bytes,height()))
            throw std::invalid_argument("Invalid chart layer target extent");
    }
    for (auto* surface:{&scratch.black,&scratch.white}) {
        surface->width=width();surface->height=height();surface->rgb.resize(output_.rgb.size());
    }
    scratch.black_saved.rgb.clear();scratch.white_saved.rgb.clear();
    const auto saved=[&](const Surface& local) -> const Surface& { return local.rgb.empty() ? saved_ : local; };
    const auto ranges=chart_ranges(center);
    bool once_changed=false;
    for (unsigned group=0;group<ranges.size();++group) {
        if (!(mask&(1U<<group))) continue;
        auto& black=scratch.black;
        auto& white=scratch.white;
        std::fill(black.rgb.begin(),black.rgb.end(),Byte{0});
        std::fill(white.rgb.begin(),white.rgb.end(),Byte{255});
        const auto [first,last]=ranges[group];
        for (unsigned slot=first;slot<last;++slot) {
            const auto& layer=slots_[slot];
            if (!chart_layer(layer)) continue;
            auto* once=layer.effect==75 ? layer.effect_state.get() : nullptr;
            const bool was_drawn=once && once->once_drawn;
            draw(black,slots_,slot,saved(scratch.black_saved),scratch.black_saved,animation_textures_,animation_off_,speaking_,true);
            if (once) once->once_drawn=was_drawn;
            draw(white,slots_,slot,saved(scratch.white_saved),scratch.white_saved,animation_textures_,animation_off_,speaking_,true);
            once_changed|=once && once->once_drawn!=was_drawn;
        }
        const auto& target=targets[group];
        const auto* b=black.rgb.data();
        const auto* w=white.rgb.data();
        for (unsigned y=0;y<height();++y) {
            auto* pixel=target.pixels+static_cast<std::size_t>(y)*target.stride;
            for (unsigned x=0;x<width();++x,pixel+=4,b+=3,w+=3) {
                std::copy_n(b,3,pixel);
                // Reconstruct coverage with the original renderer's integer
                // rounding and keep the black matte RGB premultiplied.
                const auto remaining=std::min({w[0]-b[0],w[1]-b[1],w[2]-b[2]});
                pixel[3]=static_cast<Byte>(255-std::max(0,remaining));
            }
        }
    }
    if (once_changed) ++revision_;
    return mask;
}
bool Layers::draw_chart_composite(unsigned center,ChartLayerSource base,ChartLayerSource center_plane,
                                  ChartLayerSource content,ChartLayerTarget target,ChartLayerScratch& scratch) {
    if (!chart_needs_composite(center)) return false;
    const auto row_bytes=std::size_t{width()}*4;
    if (!chart_extent(base,row_bytes,height()) || !chart_extent(target,row_bytes,height()) ||
        (center_plane.pixels && !chart_extent(center_plane,row_bytes,height())) ||
        (content.pixels && !chart_extent(content,row_bytes,height())))
        throw std::invalid_argument("Invalid chart composition extent");

    auto& out=scratch.black;
    out.width=width();out.height=height();out.rgb.resize(output_.rgb.size());
    auto* dst=out.rgb.data();
    for (unsigned y=0;y<height();++y) {
        const auto* src=base.pixels+static_cast<std::size_t>(y)*base.stride;
        for (unsigned x=0;x<width();++x,dst+=3,src+=4) {
            if (!base.opaque && src[3]!=255) throw std::invalid_argument("Chart base must be opaque");
            std::copy_n(src,3,dst);
        }
    }

    bool state_changed=false;
    const auto ranges=chart_ranges(center);
    for (unsigned group=0;group<ranges.size();++group) {
        const auto [first,last]=ranges[group];
        for (unsigned slot=first;slot<last;++slot) {
            const auto& layer=slots_[slot];
            if (!chart_layer(layer)) continue;
            auto* once=layer.effect==75 ? layer.effect_state.get() : nullptr;
            const bool was_drawn=once && once->once_drawn;
            state_changed|=layer.effect==73 && saved_.rgb!=out.rgb;
            draw(out,slots_,slot,saved_,saved_,animation_textures_,animation_off_,speaking_,true);
            state_changed|=once && once->once_drawn!=was_drawn;
        }
        if (group==0) chart_over(out,center_plane);
        else if (group==1) chart_over(out,content);
    }
    if (state_changed) invalidate();
    const auto* src=out.rgb.data();
    for (unsigned y=0;y<height();++y) {
        auto* pixel=target.pixels+static_cast<std::size_t>(y)*target.stride;
        for (unsigned x=0;x<width();++x,pixel+=4,src+=3) {
            std::copy_n(src,3,pixel);
            pixel[3]=255;
        }
    }
    return true;
}
void Layers::saved_screen(Surface frame,bool background) {
    commit_screen_capture(std::move(frame));
    saved_background_=background; invalidate();
}
void Layers::commit_screen_capture(Surface frame) {
    if (frame.width!=output_.width || frame.height!=output_.height || frame.rgb.size()!=output_.rgb.size())
        throw std::invalid_argument("Saved screen dimensions differ from the game canvas");
    bool used=saved_background_;
    for (unsigned i=0;!used && i<slots_.size();++i)
        used=slots_[i].enabled && !(hidden_&(std::uint32_t{1}<<i)) && reads_capture(slots_[i]);
    if (used && frame.rgb!=saved_.rgb) invalidate();
    saved_=std::move(frame);
}
void Layers::advance(std::uint32_t frames,Point cursor) {
    if (!frames) return;
    if (overlapping()) {
        overlap_count_+=std::min(frames,overlap_time_-overlap_count_);
        if (overlap_count_==overlap_time_) finish_overlap();
        else invalidate();
        return;
    }
    for (auto& l:slots_) {
        if (!l.enabled) continue;
        if (effects::known(l.effect)) {
            const auto length=effects::duration(l);
            for (std::uint32_t i=0;i<frames;++i) {
                if (l.count>=length && (l.effect!=94 || l.parameters[3]==0) && !effects::continuous(l.effect)) break;
                if (l.count<length) ++l.count;
                const bool reached=l.count>=length;
                effects::advance(l,static_cast<int>(output_.width),static_cast<int>(output_.height),cursor);
                if (reached) l.from=l.to;
                invalidate();
            }
            if (l.count>=length) l.from=l.to;
            continue;
        }
        const auto length=static_cast<unsigned>(duration_of(l));
        const auto previous=l.count;
        l.count+=std::min(frames,length-std::min(length,l.count));
        if (l.count==previous) continue;
        if (animated(l.effect) || l.from.x!=l.to.x || l.from.y!=l.to.y ||
            l.from.width!=l.to.width || l.from.height!=l.to.height || l.effect==2) invalidate();
        if (l.count==length) {
            if (l.effect==52 || l.effect==100 || l.effect==10) {
                l.from=l.to; l.effect=0; l.enabled=static_cast<bool>(l.texture);
            } else if (l.effect==11 || l.effect==17) {
                const auto c=l.effect==11 ? color(l,1) : color(l,3,255);
                l.effect=54; l.parameters={0,c[0],c[1],c[2]};
            }
        }
    }
}
void Layers::prepare_overlap() { previous_=surface(); }
bool Layers::start_overlap(std::uint32_t frames,int type,const OverlapDecoration& decorate) {
    if (previous_.rgb.empty()) previous_=output_;
    overlap_display_={};
    compose(target_,decorate); overlap_time_=frames; overlap_count_=0; invalidate();
    const bool supported=supports_wipe(type);
    overlap_type_=supported?type:0;
    if (!frames) finish_overlap();
    return supported;
}
void Layers::finish_overlap() {
    if (!target_.rgb.empty()) { output_=std::move(target_); output_changed_=true; }
    previous_={}; target_={}; overlap_time_=overlap_count_=0; overlap_type_=0; dirty_=false; ++revision_;
}
void Layers::resume_overlap_scene() {
    if (!overlapping() && has_overlap_decoration()) {
        overlap_display_={};
        output_changed_=true;
    }
}
bool Layers::overlapping() const noexcept { return overlap_time_!=0; }

void Layers::save_snapshot(nnn::snapshot::Writer& out,bool decorated_overlap) const {
    const bool rebuild_wipe=overlapping() && decorated_overlap;
    out(overlap_time_,overlap_count_,hidden_,dirty_ || rebuild_wipe,output_changed_,saved_background_);
    out(overlap_type_);
    // A stable frame is a cache of the layer composition. A prepared overlap
    // may still have a previous frame, even before overlap_time_ becomes nonzero.
    const bool one_shot_picture=!dirty_ && std::any_of(slots_.begin(),slots_.end(),[](const Layer& layer){
        return layer.enabled && layer.effect==75 && layer.effect_state && layer.effect_state->once_drawn;
    });
    const bool retained_once_base=!saved_background_ && keeps_once_pixels(slots_,hidden_);
    if (!rebuild_wipe && (overlapping() || decorated_overlap || one_shot_picture || retained_once_base)) surface_fields(out,output_);
    else out(output_.width,output_.height,std::vector<Byte>{});
    for (const auto* s:{&previous_,&target_,&saved_}) surface_fields(out,*s);
    for (const auto& l:slots_) {
        const auto identity=l.texture?(l.texture->asset_id.empty()?l.texture->name:l.texture->asset_id):std::string{};
        out(identity,l.parameters,l.count,l.effect,l.enabled,l.effect_state!=nullptr);
        out(l.opaque);
        rectangle_fields(out,l.source);rectangle_fields(out,l.from);rectangle_fields(out,l.to);
        if (l.effect_state) {
            effect_fields(out,*l.effect_state,true,true);
            if (l.effect==97) out(l.effect_state->condensation);
        }
    }
    if (decorated_overlap) surface_fields(out,overlap_display_);
}
void Layers::load_snapshot(nnn::snapshot::Reader& in,const std::function<std::shared_ptr<const Texture>(const std::string&)>& texture,
                           bool compact_output,bool typed_overlap,bool opacity_field,bool once_state,bool particle_state,bool decorated_overlap) {
    using nnn::snapshot::invalid;
    const auto width=output_.width,height=output_.height;
    const auto bytes=output_.rgb.size();
    in(overlap_time_,overlap_count_,hidden_,dirty_,output_changed_,saved_background_);
    overlap_type_=0;
    if (typed_overlap) in(overlap_type_);
    if (!supports_wipe(overlap_type_) || (!overlapping() && overlap_type_)) invalid("Invalid snapshot wipe type");
    Surface rendered;
    for (auto* s:{&rendered,&previous_,&target_,&saved_}) {
        surface_fields(in,*s);
        if (s!=&rendered && s->rgb.empty() && s->width==0 && s->height==0) continue;
        if (s==&rendered && compact_output && (!overlapping() || decorated_overlap) &&
            s->rgb.empty() && s->width==width && s->height==height) continue;
        if (s->width!=width || s->height!=height || s->rgb.size()!=bytes) invalid("Invalid snapshot surface dimensions");
    }
    if ((overlap_time_ && (overlap_count_>=overlap_time_ || previous_.rgb.empty() || target_.rgb.empty())) ||
        (!overlap_time_ && overlap_count_)) invalid("Invalid snapshot overlap");
    for (unsigned slot=0;slot<slots_.size();++slot) {
        auto& l=slots_[slot];
        std::string name;bool state=false;
        in(name,l.parameters,l.count,l.effect,l.enabled,state);
        if (opacity_field) in(l.opaque);
        rectangle_fields(in,l.source);rectangle_fields(in,l.from);rectangle_fields(in,l.to);
        const bool old_fallback=effects::known(l.effect) && !effects::known_v5(l.effect) && !state;
        if (name.size()>Texture::max_asset_id_bytes || name.find('\0')!=std::string::npos || l.parameters.size()>4096 ||
            (effects::known(l.effect) && !old_fallback && (!state || l.parameters.size()<36))) invalid("Invalid snapshot layer");
        if (!name.empty()) {
            l.texture=texture(name);
            if (!l.texture) invalid("Snapshot texture is unavailable");
        }
        if (state) {
            l.effect_state=std::make_shared<effects::State>();
            effect_fields(in,*l.effect_state,once_state,particle_state);
            const auto& effect=*l.effect_state;
            if ((!effect.raster.empty() && effect.raster.size()!=height) ||
                (!effect.noise.empty() && effect.noise.size()!=static_cast<std::size_t>(width)*height/16))
                invalid("Invalid snapshot effect canvas");
            if (particle_state && l.effect==97) {
                const auto area=l.texture && l.texture->has_mask?
                    static_cast<std::uint64_t>(l.texture->pixels.width)*l.texture->pixels.height:0;
                const auto size=in.size(static_cast<std::size_t>(std::min(area,max_pixels)));
                if (size) {
                    if (size!=area) invalid("Invalid condensation mask dimensions");
                    const auto* bytes=in.raw(size);
                    l.effect_state->condensation.assign(bytes,bytes+size);
                }
            }
        } else if (old_fallback) {
            const auto count=l.count;
            effects::configure(l,slot,static_cast<int>(width),static_cast<int>(height));
            l.count=count;
        }
    }
    overlap_display_={};
    if (decorated_overlap) {
        surface_fields(in,overlap_display_);
        if (overlap_display_.width!=width || overlap_display_.height!=height || overlap_display_.rgb.size()!=bytes)
            invalid("Invalid snapshot decorated transition");
    }
    if (rendered.rgb.empty()) dirty_=true;
    else output_=std::move(rendered);
    output_changed_=true; ++revision_;
}
const Surface& Layers::surface() {
    output_changed_=false;
    if (!overlapping() && has_overlap_decoration()) return overlap_display_;
    if (!dirty_) return output_;
    if (!overlapping()) compose(output_);
    else draw_wipe(output_,previous_,has_overlap_decoration()?overlap_display_:target_,overlap_type_,overlap_count_,overlap_time_);
    dirty_=false;
    return output_;
}
} // namespace emberfall::render
