#include "effects.hpp"
#include "wipes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace emberfall::render::effects {
namespace {
int value(const Layer& layer,std::size_t index) { return layer.parameters[index]; }
int clamp_int(std::int64_t n) {
    return static_cast<int>(std::clamp(n,static_cast<std::int64_t>(std::numeric_limits<int>::min()),
                                      static_cast<std::int64_t>(std::numeric_limits<int>::max())));
}
std::uint32_t random_next(std::uint32_t& n) {
    n^=n<<13U;n^=n>>17U;n^=n<<5U;
    return n;
}
int random_below(Layer& layer,std::int64_t limit) {
    return static_cast<int>((random_next(layer.effect_state->seed)&0x7fffU)%std::max<std::int64_t>(1,limit));
}
int interpolate_at(const Layer& layer,int first,int last,int type,int delay,unsigned tick,int length) {
    if ((type/100)%2) std::swap(first,last);
    double t=progress(tick,length,type%100,delay);
    if (type/200) {
        std::uint32_t hash=layer.effect_state->seed ^ (tick*0x9e3779b9U);
        hash^=hash>>16U;hash*=0x7feb352dU;hash^=hash>>15U;hash*=0x846ca68bU;hash^=hash>>16U;
        t*=static_cast<double>(hash%10000U)*0.0001;
    }
    const double result=static_cast<double>(first)+(static_cast<double>(last)-first)*t+0.5;
    return static_cast<int>(std::clamp(result,static_cast<double>(std::numeric_limits<int>::min()),
                                       static_cast<double>(std::numeric_limits<int>::max())));
}
int interpolate(const Layer& layer,int first,int last,int type,int delay) {
    return interpolate_at(layer,first,last,type,delay,layer.count,static_cast<int>(duration(layer)));
}
int fixed_percent(const Layer& layer,int first,int last,int type,int delay) {
    return interpolate(layer,clamp_int(static_cast<std::int64_t>(first)*256),
        clamp_int(static_cast<std::int64_t>(last)*256),type,delay)/100;
}
int position(int from,int to,std::uint32_t count,unsigned length) {
    if (!length || count>=length) return to;
    const auto delta=static_cast<std::int64_t>(to)-from;
    const auto magnitude=std::abs(delta)*count;
    const auto offset=(magnitude+length/2)/length;
    return clamp_int(static_cast<std::int64_t>(from)+(delta<0?-offset:offset));
}
int spline(float t,float a,float b,float c,float d,bool cubic) {
    const float u=1.0F-t;
    const float v=cubic ? (u*u*u)*a+3.0F*(t*u*u)*b+3.0F*(t*t*u)*c+(t*t*t)*d :
                          (u*u)*a+2.0F*(t*u)*b+(t*t)*c;
    return static_cast<int>(std::clamp(static_cast<double>(v+0.5F),-10000000.0,10000000.0));
}
Rect destination(const Layer& layer) {
    const auto length=known(layer.effect)?duration(layer):layer.effect==0?99999U:layer.effect==8?1U:
        layer.effect==54?9999U:static_cast<unsigned>(std::max(1,layer.parameters.empty()?20:layer.parameters[0]));
    Rect dst=layer.from;
    dst.x=position(layer.from.x,layer.to.x,layer.count,length);
    dst.y=position(layer.from.y,layer.to.y,layer.count,length);
    if (layer.count>=length) { dst.width=layer.to.width;dst.height=layer.to.height; }
    return dst;
}
Point window_origin(const Surface& out,const Layer& layer,const Texture& texture) {
    const auto dst=destination(layer);
    return {static_cast<int>(std::clamp(-static_cast<std::int64_t>(dst.x),std::int64_t{0},
                static_cast<std::int64_t>(texture.canvas_width)-out.width)),
            static_cast<int>(std::clamp(-static_cast<std::int64_t>(dst.y),std::int64_t{0},
                static_cast<std::int64_t>(texture.canvas_height)-out.height))};
}
void normal_picture(Surface& out,const Texture& t,Rect src,Rect dst,bool alpha);
void ordinary(Surface& out,const Layer& layer,unsigned slot) {
    if (!layer.texture) return;
    Rect src=layer.source,dst=destination(layer);
    src.width=std::min(src.width,dst.width);src.height=std::min(src.height,dst.height);
    dst.width=src.width;dst.height=src.height;
    normal_picture(out,*layer.texture,src,dst,slot>=4 && slot!=30 && !layer.opaque);
}
void screen_grey(Surface& out,int weight,const std::array<int,3>* sepia) {
    for (std::size_t i=0;i<out.rgb.size();i+=3) {
        const int y=(out.rgb[i]*76+out.rgb[i+1]*150+out.rgb[i+2]*29)>>8;
        for (std::size_t j=0;j<3;++j) {
            const int target=sepia ? (y*std::clamp((*sepia)[j],0,255))>>8 : y;
            out.rgb[i+j]=static_cast<Byte>((out.rgb[i+j]*(256-weight)+target*weight)>>8);
        }
    }
}
void screen_add(Surface& out,const std::array<int,3>& add,int top,int height) {
    const int width=static_cast<int>(out.width);
    for (int y=std::max(0,top);y<std::min(static_cast<int>(out.height),top+height);++y)
        for (int x=0;x<width;++x) {
            const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
            for (std::size_t c=0;c<3;++c) out.rgb[at+c]=static_cast<Byte>(
                std::clamp(static_cast<std::int64_t>(out.rgb[at+c])+add[c],std::int64_t{0},std::int64_t{255}));
        }
}
template<class F> void picture_pixels(Surface& out,const Texture& texture,Rect src,Rect dst,F fn) {
    const int width=std::min(dst.width,src.width),height=std::min(dst.height,src.height);
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(dst.y)+height);
    const auto right=std::min<std::int64_t>(out.width,static_cast<std::int64_t>(dst.x)+width);
    for (int y=std::max(0,dst.y);y<bottom;++y)
        for (int x=std::max(0,dst.x);x<right;++x) {
            const auto sx=static_cast<std::int64_t>(src.x)+x-dst.x-texture.content.x;
            const auto sy=static_cast<std::int64_t>(src.y)+y-dst.y-texture.content.y;
            if (sx<0 || sy<0 || sx>=texture.content.width || sy>=texture.content.height) continue;
            const auto si=(static_cast<std::size_t>(sy)*texture.pixels.width+static_cast<unsigned>(sx))*4;
            const auto di=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
            fn(out.rgb.data()+di,texture.pixels.rgba.data()+si);
        }
}
template<class F> void character_pixels(Surface& out,const Layer& layer,F fn) {
    if (layer.texture) picture_pixels(out,*layer.texture,layer.source,destination(layer),fn);
}
std::array<int,4> texel(const Texture& t,int x,int y) {
    x-=t.content.x;y-=t.content.y;
    if (x<0 || y<0 || x>=t.content.width || y>=t.content.height) return {};
    const auto i=(static_cast<std::size_t>(y)*t.pixels.width+static_cast<unsigned>(x))*4;
    return {t.pixels.rgba[i],t.pixels.rgba[i+1],t.pixels.rgba[i+2],t.pixels.rgba[i+3]};
}
void normal_picture(Surface& out,const Texture& t,Rect src,Rect dst,bool alpha) {
    if (dst.x>=static_cast<int>(out.width) || dst.y>=static_cast<int>(out.height)) return;
    if (!alpha || t.has_mask) {
        paint(out,t,src,dst,100,{},alpha);
        return;
    }
    const auto key=texel(t,0,0);
    const auto right=std::min<std::int64_t>(out.width,static_cast<std::int64_t>(dst.x)+std::min(src.width,dst.width));
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(dst.y)+std::min(src.height,dst.height));
    for (int y=std::max(0,dst.y);y<bottom;++y) for (int x=std::max(0,dst.x);x<right;++x) {
        const auto sx=static_cast<std::int64_t>(src.x)+x-dst.x,sy=static_cast<std::int64_t>(src.y)+y-dst.y;
        if (sx<0 || sy<0 || sx>=t.canvas_width || sy>=t.canvas_height) continue;
        const auto color=texel(t,static_cast<int>(sx),static_cast<int>(sy));
        if (color[0]==key[0] && color[1]==key[1] && color[2]==key[2]) continue;
        const auto di=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        for (std::size_t c=0;c<3;++c) out.rgb[di+c]=static_cast<Byte>(color[c]);
    }
}
void shake(Surface& out,const Layer& layer) {
    if (layer.count>=duration(layer)) return;
    constexpr std::array<int,4> steps{-16,-12,16,12};
    const int shift=steps[(duration(layer)-layer.count)%steps.size()];
    const bool vertical=value(layer,1)!=0;
    const int span=static_cast<int>(vertical?out.height:out.width)-std::abs(shift);
    if (span<=0) return;
    const auto stride=static_cast<std::size_t>(out.width)*3;
    const auto unit=vertical?stride:3;
    const auto from=static_cast<std::size_t>(std::max(0,shift))*unit;
    const auto to=static_cast<std::size_t>(std::max(0,-shift))*unit;
    const auto size=static_cast<std::size_t>(span)*unit;
    if (vertical) {
        std::memmove(out.rgb.data()+to,out.rgb.data()+from,size);
    } else {
        for (std::size_t row=0;row<out.rgb.size();row+=stride)
            std::memmove(out.rgb.data()+row+to,out.rgb.data()+row+from,size);
    }
}
void relief(Surface& out,const Layer& layer) {
    const int amount=std::clamp(fixed_percent(layer,value(layer,3),value(layer,2),value(layer,1),value(layer,4)),0,256);
    // The software filter visits pairs, then repeats each row's penultimate pixel at the edge.
    const auto pairs=out.rgb.size()/6;
    const auto end=pairs>0?(pairs-1)*6:0;
    for (std::size_t i=0;i<end;++i) {
        const int current=out.rgb[i],difference=current-out.rgb[i+3];
        const int raised=128+(difference-(difference<0?1:0))/2;
        out.rgb[i]=static_cast<Byte>((raised*amount>>8)+(current*(256-amount)>>8));
    }
    const auto stride=static_cast<std::size_t>(out.width)*3;
    if (stride>=6)
        for (std::size_t end_row=stride;end_row<=out.rgb.size();end_row+=stride)
            std::memcpy(out.rgb.data()+end_row-3,out.rgb.data()+end_row-6,3);
}
void stretch(Surface& out,const Layer& layer) {
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    if (!layer.texture || layer.texture->canvas_width!=w || layer.texture->canvas_height!=h) return;
    const auto& texture=*layer.texture;
    const int factor=std::clamp(interpolate(layer,0,256,value(layer,5),value(layer,6)),0,256);
    const int x=std::clamp(value(layer,1),0,w-1),y=std::clamp(value(layer,2),0,h-1);
    const int right=x+std::clamp(value(layer,3),1,w-x),bottom=y+std::clamp(value(layer,4),1,h-y);
    const int left=x-x*factor/256,top=y-y*factor/256;
    const int dest_w=right+(w-right)*factor/256-left,dest_h=bottom+(h-bottom)*factor/256-top;
    int block=0;
    for (int divisor=2;divisor<=5;++divisor)
        if (w/divisor==dest_w && h/divisor==dest_h) { block=divisor;break; }
    const int step_y=h*256/dest_h;
    for (int dy=0;dy<dest_h;++dy) for (int dx=0;dx<dest_w;++dx) {
        std::array<int,4> pixel{};
        if (block==2) {
            const auto a=texel(texture,dx*2,dy*2),b=texel(texture,dx*2+1,dy*2),c=texel(texture,dx*2,dy*2+1);
            // The original two-pixel MMX filter doubles the lower-left sample.
            for (std::size_t k=0;k<3;++k) pixel[k]=(a[k]+b[k]+c[k]*2)/4;
        } else if (block>2) {
            for (int by=0;by<block;++by) for (int bx=0;bx<block;++bx) {
                const auto color=texel(texture,dx*block+bx,dy*block+by);
                for (std::size_t k=0;k<3;++k) pixel[k]+=color[k];
            }
            for (std::size_t k=0;k<3;++k) pixel[k]/=block*block;
        } else {
            pixel=texel(texture,dx*w/dest_w,dy*step_y/256);
        }
        const auto index=(static_cast<std::size_t>(top+dy)*out.width+static_cast<unsigned>(left+dx))*3;
        for (std::size_t c=0;c<3;++c) out.rgb[index+c]=static_cast<Byte>(pixel[c]);
    }
}
int balance_curve(int type,int light) {
    switch (type) {
    case 1: return 255-light;
    case 2: return light<128?128+light:light;
    case 3: return light;
    case 4: return std::max(0,(127-light)*2);
    case 5: return std::min(light,255-light)*2;
    case 6: return std::max(0,(light-128)*2);
    case 7: return light<85?255:std::max(0,(254-light)*3/2);
    case 8: return light>=85 && light<=170?255:std::min(light,255-light)*3/2+127;
    case 9: return light<170?light*3/2:255;
    default: return 255;
    }
}
void color_balance(Surface& out,const Layer& layer) {
    if (!layer.texture || !layer.texture->has_mask) return;
    std::array<int,3> shift{};
    for (std::size_t c=0;c<3;++c)
        shift[c]=interpolate(layer,value(layer,4+c),value(layer,1+c),value(layer,8),value(layer,9));
    std::array<std::array<int,3>,256> table{};
    constexpr std::array<float,3> weight{0.299F,0.587F,0.114F};
    for (std::size_t i=0;i<table.size();++i) {
        std::array<float,3> delta{};
        for (std::size_t c=0;c<3;++c)
            delta[c]=static_cast<float>(static_cast<std::int64_t>(shift[c])*
                balance_curve(value(layer,10+c),static_cast<int>(i))/256);
        const float adjust=value(layer,13)?delta[0]*weight[0]+delta[1]*weight[1]+delta[2]*weight[2]:0.0F;
        for (std::size_t c=0;c<3;++c)
            table[i][c]=static_cast<int>(std::clamp(delta[c]-adjust/weight[c]*0.33333F,-255.0F,255.0F));
    }
    character_pixels(out,layer,[&](Byte* dst,const Byte* src) {
        const int a=src[3];
        if (!a) return;
        const auto light=static_cast<std::size_t>((77*src[0]+150*src[1]+29*src[2])>>8);
        for (std::size_t c=0;c<3;++c) {
            const int shade=std::clamp(src[c]+table[light][c],0,255);
            dst[c]=static_cast<Byte>((shade*a+dst[c]*(256-a))>>8);
        }
    });
}
void psychedelic(Surface& out,const Layer& layer) {
    if (!layer.texture || !layer.texture->has_mask) return;
    std::array<int,3> multiply{},add{};
    for (std::size_t c=0;c<3;++c) {
        multiply[c]=fixed_percent(layer,value(layer,6+c),value(layer,3+c),value(layer,1),value(layer,2))&65535;
        add[c]=interpolate(layer,value(layer,12+c),value(layer,9+c),value(layer,15),value(layer,16))&255;
    }
    character_pixels(out,layer,[&](Byte* dst,const Byte* src) {
        const int a=src[3];
        if (!a) return;
        for (std::size_t c=0;c<3;++c) {
            const int shade=((((src[c]*multiply[c])&65535)>>8)+add[c])&255;
            dst[c]=static_cast<Byte>(a==255?shade:(shade*(a+1)+dst[c]*(255-a))>>8);
        }
    });
}
void chara_bure_tick(Layer& layer) {
    auto& p=layer.parameters;
    if (p[20]<p[21]) { ++p[20];return; }
    auto& seed=layer.effect_state->seed;
    const auto jitter=[&](int base,int range) {
        const auto divisor=std::max<std::int64_t>(1,static_cast<std::int64_t>(base)*range);
        const auto sample=static_cast<std::int64_t>(random_next(seed)&0x7fffU)%divisor;
        return clamp_int((static_cast<std::int64_t>(base)*100-sample)/100);
    };
    p[22]=p[24];p[23]=p[25];
    p[24]=jitter(p[1],p[4]);p[25]=jitter(p[2],p[4]);p[21]=jitter(p[3],p[5]);
    if ((random_next(seed)&0x7fffU)%100<50) p[24]=-p[24];
    if ((random_next(seed)&0x7fffU)%100<50) p[25]=-p[25];
    p[20]=0;
}
void chara_bure(Surface& out,const Layer& layer,unsigned slot) {
    if (!layer.texture) return;
    const auto tick=static_cast<unsigned>(std::max(0,value(layer,20)));
    const int length=value(layer,21),type=value(layer,6),delay=value(layer,7);
    const int dx=interpolate_at(layer,value(layer,22),value(layer,24),type,delay,tick,length);
    const int dy=interpolate_at(layer,value(layer,23),value(layer,25),type,delay,tick,length);
    const auto dst=destination(layer);
    const Rect src{0,0,layer.texture->canvas_width,layer.texture->canvas_height};
    normal_picture(out,*layer.texture,src,{clamp_int(static_cast<std::int64_t>(dst.x)+dx),
        clamp_int(static_cast<std::int64_t>(dst.y)+dy),src.width,src.height},slot>=4 && slot!=14 && !layer.opaque);
}
void translucent_picture(Surface& out,const Texture& texture,Rect src,Rect dst,int percent) {
    if (!texture.has_mask || percent<=0) return;
    const int weight=std::min(100,percent)*256/100;
    picture_pixels(out,texture,src,dst,[&](Byte* pixel,const Byte* sample) {
        const int mask=sample[3];
        if (!mask) return;
        const int src_weight=mask==255?weight:mask*weight/256;
        const int dst_weight=(mask==255?256:255)-src_weight;
        for (std::size_t c=0;c<3;++c)
            pixel[c]=static_cast<Byte>((sample[c]*src_weight+pixel[c]*dst_weight)>>8);
    });
}
int animation_index(unsigned count,int step,int patterns,bool loop) {
    const int index=static_cast<int>(count/static_cast<unsigned>(std::max(1,step)));
    return loop?index%patterns:std::min(index,patterns-1);
}
void buffer_animation(Surface& out,const std::array<Layer,32>& layers,const Layer& layer) {
    const int start=value(layer,4);
    const int frame=animation_index(layer.count,value(layer,2),std::max(1,value(layer,1)),value(layer,3)!=0);
    const auto index=std::clamp(static_cast<std::int64_t>(start)+frame,std::int64_t{0},std::int64_t{15});
    const auto& buffer=layers[static_cast<std::size_t>(index)];
    if (!buffer.texture) return;
    const auto dst=destination(buffer);
    const Rect src{0,0,dst.width,dst.height};
    if (start==0 || start==1) {
        normal_picture(out,*buffer.texture,src,dst,start==1);
    } else {
        const int percent=interpolate(layer,value(layer,7),value(layer,6),value(layer,8),value(layer,9));
        translucent_picture(out,*buffer.texture,src,dst,percent);
    }
}
void sprite_animation(Surface& out,const Layer& layer) {
    const int length=static_cast<int>(duration(layer));
    if (!layer.texture || layer.count>=static_cast<unsigned>(length)) return;
    const auto& texture=*layer.texture;
    const int w=value(layer,1),h=value(layer,2);
    if (w<1 || h<1) return;
    const int columns=texture.canvas_width/w,rows=texture.canvas_height/h;
    if (!columns || !rows) return;
    const int range=std::max(1,length-1),tick=std::min(static_cast<int>(layer.count),range-1);
    const auto interpolate_frame=[&](int start,int end) {
        return clamp_int(start+(static_cast<std::int64_t>(end)-start)*tick/range);
    };
    int index=tick/std::max(1,value(layer,3));
    if (value(layer,9)>0) index%=value(layer,9);
    if (index/columns>=rows) return;
    const Rect src{(index%columns)*w,(index/columns)*h,w,h};
    const Rect dst{interpolate_frame(value(layer,6),value(layer,4)),
                   interpolate_frame(value(layer,7),value(layer,5)),w,h};
    const int mode=value(layer,8);
    if (mode==2) {
        const int percent=interpolate_frame(value(layer,11),value(layer,10));
        if (percent!=100) translucent_picture(out,texture,src,dst,percent);
        else if (texture.has_mask) normal_picture(out,texture,src,dst,true);
    } else {
        normal_picture(out,texture,src,dst,mode!=0);
    }
}
void new_face_cycle(Layer& layer) {
    auto& p=layer.parameters;
    const bool speaking=(p[6]&1)!=0;
    const auto average=static_cast<std::int64_t>(std::max(0,p[speaking?10:8]));
    const auto variation=static_cast<std::int64_t>(std::max(0,p[speaking?11:9]));
    const auto range=std::max<std::int64_t>(1,2*variation*average/100);
    const auto sample=static_cast<std::int64_t>(random_next(layer.effect_state->seed)&0x7fffU)%range;
    p[13]=0;
    p[14]=std::max(1,clamp_int(average+sample-range/2));
}
void advance_face(Layer& layer) {
    auto& p=layer.parameters;
    layer.count=0;
    const auto tick=static_cast<std::int64_t>(p[13])+1;
    const auto frame=tick-p[14];
    if (frame<0) { p[13]=static_cast<int>(tick);return; }
    const auto count=static_cast<std::int64_t>(std::max(1,p[3]));
    const auto speed=static_cast<std::int64_t>(std::max(1,p[7]));
    const auto length=std::max<std::int64_t>(1,p[5]?count*2-2:count)*speed;
    const bool short_cycle=p[12] && frame==(count-1)*speed &&
        static_cast<int>((random_next(layer.effect_state->seed)&0x7fffU)%100)<p[12];
    const auto skip=short_cycle && p[5]?speed*2:0;
    if ((short_cycle && !p[5]) || frame+skip>=length || tick+skip>=std::numeric_limits<int>::max())
        new_face_cycle(layer);
    else p[13]=static_cast<int>(tick+skip);
}
void face_animation(Surface& out,const std::array<Layer,32>& layers,unsigned slot,bool speaking) {
    const auto& layer=layers[slot];
    const auto phase=static_cast<std::int64_t>(value(layer,13))-value(layer,14);
    const int mode=value(layer,6);
    if (phase<0 || ((mode&1) && !speaking)) return;
    const int buffer=value(layer,4);
    const auto index=buffer==-1?slot:static_cast<unsigned>(buffer);
    if (index>=layers.size() || !layers[index].texture) return;
    const auto& source=layers[index];
    const int w=value(layer,1),h=value(layer,2),count=std::max(1,value(layer,3));
    if (w<=0 || h<=0) return;
    auto frame=phase/std::max(1,value(layer,7));
    if (value(layer,5) && frame>=count) frame=2LL*count-1-frame;
    const auto x=std::max<std::int64_t>(0,frame)*w;
    if (x>=source.texture->canvas_width) return;
    const auto dst=destination(source);
    normal_picture(out,*source.texture,{static_cast<int>(x),0,w,h},{dst.x,dst.y,w,h},(mode&2)!=0);
}
void scaled_sprite(Surface& out,const Texture& texture,Rect src,Rect dst,int percent) {
    if (dst.width<1 || dst.height<1 || percent<1) return;
    constexpr std::int64_t unit=65536;
    const auto left=std::max(0,dst.x),top=std::max(0,dst.y);
    const auto right=std::min<std::int64_t>(out.width,static_cast<std::int64_t>(dst.x)+dst.width);
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(dst.y)+dst.height);
    if (left>=right || top>=bottom) return;
    const auto scale_x=src.width*unit/dst.width,scale_y=src.height*unit/dst.height;
    const auto source_x=src.x*unit+(static_cast<std::int64_t>(left)-dst.x)*src.width*unit/dst.width;
    const auto source_y=src.y*unit+(static_cast<std::int64_t>(top)-dst.y)*src.height*unit/dst.height;
    const int weight=std::min(100,percent)*256/100;
    const auto key=texel(texture,0,0);
    for (int y=top;y<bottom;++y) for (int x=left;x<right;++x) {
        const auto sample=texel(texture,static_cast<int>((source_x+(x-left)*scale_x)/unit),
                                        static_cast<int>((source_y+(y-top)*scale_y)/unit));
        if (texture.has_mask?sample[3]==0:sample==key) continue;
        const int alpha=texture.has_mask?sample[3]*weight/256:weight;
        const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        for (std::size_t c=0;c<3;++c)
            out.rgb[at+c]=static_cast<Byte>(alpha>=255?sample[c]:
                ((sample[c]*alpha)>>8)+((out.rgb[at+c]*(255-alpha))>>8));
    }
}
void sentence(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const auto& texture=*layer.texture;
    const int w=value(layer,1),h=value(layer,2);
    if (w<1 || h<1) return;
    const int columns=texture.canvas_width/w,rows=texture.canvas_height/h;
    if (!columns || !rows) return;
    const int cells=columns*rows;
    const int steps=std::clamp(value(layer,5),1,std::numeric_limits<int>::max()/cells);
    const int total=std::clamp(interpolate(layer,0,cells*steps,value(layer,6),value(layer,7)),0,cells*steps);
    const bool vertical=value(layer,3)!=0;
    const int count=total/steps,per_line=vertical?rows:columns;
    const int complete=count/per_line,partial=count%per_line;
    const auto dst=destination(layer);
    const auto at=[&](Rect source) {
        return Rect{clamp_int(static_cast<std::int64_t>(dst.x)+source.x),
                    clamp_int(static_cast<std::int64_t>(dst.y)+source.y),source.width,source.height};
    };
    const auto put=[&](Rect source) { normal_picture(out,texture,source,at(source),true); };
    if (vertical) {
        if (complete) put({(columns-complete)*w,0,complete*w,rows*h});
        if (partial) put({(columns-complete-1)*w,0,w,partial*h});
    } else {
        if (complete) put({0,0,columns*w,complete*h});
        if (partial) put({0,complete*h,partial*w,h});
    }
    const int progress=static_cast<int>(static_cast<std::int64_t>(total%steps)*100/steps);
    if (!progress) return;
    Rect cell{vertical?(columns-complete-1)*w:partial*w,vertical?partial*h:complete*h,w,h};
    switch (value(layer,4)) {
    case 0: put(cell);break;
    case 1: translucent_picture(out,texture,cell,at(cell),progress);break;
    case 2:case 3:case 5:case 6: {
        const int type=value(layer,4),scale=type>=5?200-progress:progress;
        const int dw=w*scale/100,dh=h*scale/100;
        const Rect shape{cell.x+(w-dw)/2,cell.y+(h-dh)/2,dw,dh};
        if (type==3) put(shape);
        else scaled_sprite(out,texture,cell,at(shape),type==6?progress:100);
        break;
    }
    case 4:
        if (vertical) cell.height=h*progress/100;
        else cell.width=w*progress/100;
        put(cell);break;
    default:break;
    }
}
void once(Surface& out,const Layer& layer,unsigned slot) {
    const auto count=static_cast<std::int64_t>(layer.count);
    const auto begin=static_cast<std::int64_t>(value(layer,1));
    if (!layer.effect_state->once_drawn || (count>=begin && count<begin+value(layer,2)))
        ordinary(out,layer,slot);
    layer.effect_state->once_drawn=true;
}
void full_animation(Surface& out,const Layer& layer,const AnimationTexture& animation) {
    if (!animation) return;
    const int number=value(layer,1);
    const auto frame=std::min(layer.count,std::max(1U,duration(layer))-1U)/
        static_cast<unsigned>(std::max(1,value(layer,2)));
    if (number<0 || number>999 || frame>999) return;
    const auto padded=[](unsigned n) { return std::to_string(n+1000U).substr(1); };
    const auto texture=animation("dwq/an/an"+padded(static_cast<unsigned>(number))+padded(frame)+".dwq");
    if (!texture) return;
    const Rect canvas{0,0,texture->canvas_width,texture->canvas_height};
    paint(out,*texture,canvas,canvas,100,{},false);
}
void blend_animation_frames(Surface& out,const Texture& first,Point a_pos,const Texture& second,
                            Point b_pos,Rect dst,int first_percent,int second_percent) {
    const int first_weight=(first_percent*256+50)/100,second_weight=(second_percent*256+50)/100;
    const bool mask=first.has_mask && second.has_mask;
    const auto right=std::min<std::int64_t>(out.width,static_cast<std::int64_t>(dst.x)+dst.width);
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(dst.y)+dst.height);
    for (int y=std::max(0,dst.y);y<bottom;++y) for (int x=std::max(0,dst.x);x<right;++x) {
        const auto a=texel(first,a_pos.x+x-dst.x,a_pos.y+y-dst.y),b=texel(second,b_pos.x+x-dst.x,b_pos.y+y-dst.y);
        const int wa=mask && a[3]!=255?first_weight*a[3]/4:first_weight*64;
        const int wb=mask && b[3]!=255?second_weight*b[3]/4:second_weight*64;
        const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        const int remaining=mask?std::max(0,16384-wa-wb):0;
        for (std::size_t c=0;c<3;++c)
            out.rgb[at+c]=static_cast<Byte>(std::min(255,((a[c]*wa)>>14)+((b[c]*wb)>>14)+
                ((out.rgb[at+c]*remaining)>>14)));
    }
}
void external_animation(Surface& out,const Layer& layer,unsigned slot,
                        const AnimationTexture& animation,bool off) {
    if (!animation) return;
    const auto image=[&](int frame) { return animation("bg/bganime"+std::to_string(frame)); };
    const auto texture=image(animation_frame(layer,off));
    if (!texture || value(layer,7)) return;
    const int percent=interpolate(layer,0,10000,0,0);
    const auto at=[&](int start,int end) { return clamp_int(start+(static_cast<std::int64_t>(end)-start)*percent/10000); };
    const Rect src{0,0,texture->canvas_width,texture->canvas_height};
    const Rect dst{at(layer.from.x,layer.to.x),at(layer.from.y,layer.to.y),src.width,src.height};
    const int transition=value(layer,5);
    if (value(layer,13) && transition>0 && layer.count<static_cast<unsigned>(transition)) {
        const auto previous=image(value(layer,15));
        if (previous) {
            const int remaining=100-static_cast<int>(static_cast<std::int64_t>(layer.count)*100/transition);
            const Rect overlap{dst.x,dst.y,std::min(previous->canvas_width,dst.width),
                               std::min(previous->canvas_height,dst.height)};
            blend_animation_frames(out,*previous,{},*texture,{},overlap,remaining,100-remaining);
            return;
        }
    }
    normal_picture(out,*texture,src,dst,slot>=4 && slot!=30 && !layer.opaque);
}
void turn_new(Surface& out,const Layer& l) {
    if (!l.texture || !l.texture->has_mask) return;
    const auto p=[&](std::size_t i){return value(l,i);};
    const int opacity=std::clamp(interpolate(l,p(12),p(11),p(17),p(18)),0,100);
    if (!opacity) return;
    const int zx=std::max(1,interpolate(l,clamp_int(static_cast<std::int64_t>(p(9))*100),
        clamp_int(static_cast<std::int64_t>(p(7))*100),p(15),p(16))/100);
    const int zy=std::max(1,interpolate(l,clamp_int(static_cast<std::int64_t>(p(10))*100),
        clamp_int(static_cast<std::int64_t>(p(8))*100),p(15),p(16))/100);
    const int angle=interpolate(l,clamp_int(static_cast<std::int64_t>(p(4))*10),
        clamp_int(static_cast<std::int64_t>(p(3))*10),p(13),p(14));
    const int normalized=(angle%3600+3600)%3600;
    const double theta=normalized*(3.14159265358979323846/1800.0);
    const double cs=static_cast<float>(std::cos(-theta)),sn=static_cast<float>(std::sin(-theta));
    std::array<int,3> add{};
    for (std::size_t c=0;c<3;++c) add[c]=interpolate(l,p(24+c),p(21+c),p(19),p(20));
    const auto dst=destination(l);
    const Texture& texture=*l.texture;
    const int cx=clamp_int(static_cast<std::int64_t>(texture.canvas_width/2)+p(5));
    const int cy=clamp_int(static_cast<std::int64_t>(texture.canvas_height/2)+p(6));
    const double screen_x=static_cast<double>(dst.x)+cx,screen_y=static_cast<double>(dst.y)+cy;
    double min_x=screen_x,max_x=screen_x,min_y=screen_y,max_y=screen_y;
    const double forward_cos=std::cos(theta),forward_sin=std::sin(theta);
    for (int yy : {0,texture.canvas_height}) for (int xx : {0,texture.canvas_width}) {
        const double dx=(static_cast<double>(xx)-cx)*zx/100,dy=(static_cast<double>(yy)-cy)*zy/100;
        const double x=screen_x+forward_cos*dx-forward_sin*dy,y=screen_y+forward_sin*dx+forward_cos*dy;
        min_x=std::min(min_x,x);max_x=std::max(max_x,x);
        min_y=std::min(min_y,y);max_y=std::max(max_y,y);
    }
    const int left=static_cast<int>(std::clamp(min_x-2,0.0,static_cast<double>(out.width)));
    const int right=static_cast<int>(std::clamp(max_x+2,0.0,static_cast<double>(out.width)));
    const int top=static_cast<int>(std::clamp(min_y-2,0.0,static_cast<double>(out.height)));
    const int bottom=static_cast<int>(std::clamp(max_y+2,0.0,static_cast<double>(out.height)));
    for (int y=top;y<bottom;++y) for (int x=left;x<right;++x) {
        const double dx=x-screen_x,dy=y-screen_y;
        const double fx=cx+(cs*dx-sn*dy)*100/zx+0.5,fy=cy+(sn*dx+cs*dy)*100/zy+0.5;
        if (fx<0 || fy<0 || fx>=texture.canvas_width || fy>=texture.canvas_height) continue;
        const int sx=static_cast<int>(fx),sy=static_cast<int>(fy);
        auto src=texel(texture,sx,sy);
        if (p(2)==1) {
            const double ax0=fx-sx,ay0=fy-sy;
            const double ax=ax0>=0.5?1.5-ax0:0.5+ax0,ay=ay0>=0.5?1.5-ay0:0.5+ay0;
            const double xx=fx+(ax0>=0.5?1:-1)*100.0/zx,yy=fy+(ay0>=0.5?1:-1)*100.0/zy;
            const int tx=xx>=0 && xx<texture.canvas_width?static_cast<int>(xx):sx;
            const int ty=yy>=0 && yy<texture.canvas_height?static_cast<int>(yy):sy;
            const auto s10=texel(texture,tx,sy),s01=texel(texture,sx,ty),s11=texel(texture,tx,ty);
            const int w00=static_cast<int>(ax*ay*256),w10=static_cast<int>((1-ax)*ay*256);
            const int w01=static_cast<int>(ax*(1-ay)*256),w11=static_cast<int>((1-ax)*(1-ay)*256);
            for (std::size_t c=0;c<4;++c) src[c]=(src[c]*w00+s10[c]*w10+s01[c]*w01+s11[c]*w11)>>8;
        }
        if (!src[3]) continue;
        const int alpha=256*src[3]*opacity/(100*255);
        const auto di=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        for (std::size_t c=0;c<3;++c) {
            const int color=static_cast<int>(std::clamp(static_cast<std::int64_t>(src[c])+add[c],std::int64_t{0},std::int64_t{255}));
            int v=color;
            if (p(1)==0 && (src[3]!=255 || opacity!=100)) v=(color*alpha+out.rgb[di+c]*(256-alpha))>>8;
            if (p(1)==1) v=std::min(255,out.rgb[di+c]+((color*alpha)>>8));
            if (p(1)==2) v=std::max(0,out.rgb[di+c]-((color*alpha)>>8));
            out.rgb[di+c]=static_cast<Byte>(v);
        }
    }
}
void turn(Surface& out,const Layer& l) {
    if (!l.texture) return;
    const auto p=[&](std::size_t i){return value(l,i);};
    const auto& texture=*l.texture;
    Rect dst=destination(l);
    if (p(25)==2 || p(25)==3) {
        const float t=static_cast<float>(interpolate(l,0,10000,p(34),p(35)))*0.0001F;
        const auto offset=[&](std::size_t axis) {
            const float a=static_cast<float>(p(26+axis)),b=static_cast<float>(p(28+axis));
            const float c=static_cast<float>(p(30+axis)),d=static_cast<float>(p(32+axis));
            return spline(t,a,b,c,d,p(25)==3);
        };
        dst.x=clamp_int(static_cast<std::int64_t>(dst.x)+offset(0));
        dst.y=clamp_int(static_cast<std::int64_t>(dst.y)+offset(1));
    }
    const int scale=std::max(1,interpolate(l,clamp_int(static_cast<std::int64_t>(p(6))*10),
        clamp_int(static_cast<std::int64_t>(p(5))*10),p(12),p(13))/10);
    const int a=interpolate(l,clamp_int(static_cast<std::int64_t>(p(2))*10),
        clamp_int(static_cast<std::int64_t>(p(1))*10),p(10),p(11));
    const int normalized=(a%3600+3600)%3600;
    const double theta=normalized*(3.14159/1800.0),co=(std::cos(theta)/scale)*100,si=(std::sin(theta)/scale)*100;
    const double screen_x=static_cast<double>(out.width/2)+dst.x,screen_y=static_cast<double>(out.height/2)+dst.y;
    const double origin_x=static_cast<double>(texture.canvas_width/2)+p(3),origin_y=static_cast<double>(texture.canvas_height/2)+p(4);
    const int opacity=std::clamp(interpolate(l,p(9),p(8),p(14),p(15)),0,100);
    const auto key=texel(texture,0,0);
    const int mode=p(7)==2 && !texture.has_mask?1:p(7);
    if (mode!=0 && mode!=1 && !texture.has_mask) return;
    std::array<int,3> add{};
    for (std::size_t c=0;c<3;++c) add[c]=p(16)?interpolate(l,p(20+c),p(17+c),p(23),p(24)):0;
    const auto fixed=[](double v) {
        v=std::clamp(v,-1000000000.0,1000000000.0);
        auto whole=static_cast<std::int64_t>(v);
        if (v<0) --whole;
        const auto fraction=static_cast<std::int64_t>((v-static_cast<double>(whole))*65536)&65535;
        return whole*65536+fraction;
    };
    const auto x0=fixed(co*-screen_x-si*-screen_y+origin_x+0.5);
    const auto y0=fixed(si*-screen_x+co*-screen_y+origin_y+0.5);
    const auto xx=fixed(co),xy=fixed(-si),yx=fixed(si),yy=fixed(co);
    for (int y=0;y<static_cast<int>(out.height);++y) for (int x=0;x<static_cast<int>(out.width);++x) {
        const auto sx=(x0+xx*x+xy*y)>>16,sy=(y0+yx*x+yy*y)>>16;
        if (sx<0 || sy<0 || sx>=texture.canvas_width || sy>=texture.canvas_height) continue;
        const auto src=texel(texture,static_cast<int>(sx),static_cast<int>(sy));
        if (mode==1 && src[0]==key[0] && src[1]==key[1] && src[2]==key[2]) continue;
        if (mode!=0 && mode!=1 && !src[3]) continue;
        const int alpha=mode==0 || mode==1?255:(src[3]*(256*opacity/100))>>8;
        const auto di=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        for (std::size_t c=0;c<3;++c) {
            const int col=static_cast<int>(std::clamp(static_cast<std::int64_t>(src[c])+add[c],std::int64_t{0},std::int64_t{255}));
            out.rgb[di+c]=static_cast<Byte>(alpha==255?col:((col*alpha)>>8)+((out.rgb[di+c]*(256-alpha))>>8));
        }
    }
}
void bura(Surface& out,const Layer& l) {
    const auto p=[&](std::size_t i){return value(l,i);};
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    const int count=static_cast<int>(l.count),length=std::max(1,static_cast<int>(duration(l)));
    const int cx=std::clamp(clamp_int(static_cast<std::int64_t>(p(11))+
        (static_cast<std::int64_t>(p(1))-p(11))*count/length),0,w);
    const int cy=std::clamp(clamp_int(static_cast<std::int64_t>(p(12))+
        (static_cast<std::int64_t>(p(2))-p(12))*count/length),0,h);
    const int mul=interpolate(l,clamp_int(static_cast<std::int64_t>(p(4))*100),
        clamp_int(static_cast<std::int64_t>(p(3))*100),p(7),p(8));
    const double zoom=std::max(100.0,mul/100.0);
    const int ps=interpolate(l,p(6),p(5),p(9),p(10));
    const int a=static_cast<int>(std::clamp(static_cast<std::int64_t>(ps)*256/100,std::int64_t{0},std::int64_t{256}));
    const int b=static_cast<int>(std::clamp((100-static_cast<std::int64_t>(ps))*256/100,std::int64_t{0},std::int64_t{256}));
    for (int half_y=0;half_y<2;++half_y) for (int half_x=0;half_x<2;++half_x) {
        const int span_x=half_x?w-cx:cx,span_y=half_y?h-cy:cy;
        if (!span_x || !span_y) continue;
        const double src_x=cx+(half_x?1:-1)*span_x*100.0/zoom;
        const double src_y=cy+(half_y?1:-1)*span_y*100.0/zoom;
        const auto fixed=[](double v){return static_cast<std::int64_t>(std::clamp(v*65536.0,-1.0e12,1.0e12));};
        const auto start_x=std::clamp(fixed(src_x),std::int64_t{0},(static_cast<std::int64_t>(w)<<16)-1);
        const auto start_y=std::clamp(fixed(src_y),std::int64_t{0},(static_cast<std::int64_t>(h)<<16)-1);
        const auto step_x=fixed((cx-src_x)/span_x),step_y=fixed((cy-src_y)/span_y);
        for (int yy=0;yy<span_y;++yy) {
            const int py=static_cast<int>((start_y+step_y*yy)>>16);
            if (py<0 || py>=h) continue;
            const int dy=half_y?h-1-yy:yy;
            for (int xx=0;xx<span_x;++xx) {
                const int px=static_cast<int>((start_x+step_x*xx)>>16);
                if (px<0 || px>=w) continue;
                const int dx=half_x?w-1-xx:xx;
                const auto src=(static_cast<std::size_t>(py)*out.width+static_cast<unsigned>(px))*3;
                const auto dst=(static_cast<std::size_t>(dy)*out.width+static_cast<unsigned>(dx))*3;
                for (std::size_t c=0;c<3;++c)
                    out.rgb[dst+c]=static_cast<Byte>((out.rgb[src+c]*a+out.rgb[dst+c]*b)>>8);
            }
        }
    }
}
void change(Surface& out,const std::array<Layer,32>& layers,unsigned slot) {
    const auto& l=layers[slot];
    if (!l.texture || value(l,1)<0 || value(l,1)>=static_cast<int>(layers.size())) return;
    const auto& other=layers[static_cast<unsigned>(value(l,1))];
    if (!other.texture || !other.texture->has_mask || other.texture->canvas_width!=l.texture->canvas_width ||
        other.texture->canvas_height!=l.texture->canvas_height) return;
    const int trans=interpolate(l,value(l,3),value(l,2),value(l,6),value(l,7));
    const int ratio=interpolate(l,100,0,value(l,4),value(l,5));
    const int a=std::clamp(clamp_int(static_cast<std::int64_t>(ratio)*trans/100),0,100);
    const int b=std::clamp(clamp_int((100-static_cast<std::int64_t>(ratio))*trans/100),0,100);
    const int ps1=(a*256+50)/100,ps2=(b*256+50)/100;
    const Rect dst=destination(l);
    const int w=std::min(dst.width,l.source.width),h=std::min(dst.height,l.source.height);
    const auto intersects=[&](const Texture& t) {
        const auto left=std::max<std::int64_t>(l.source.x,t.content.x);
        const auto top=std::max<std::int64_t>(l.source.y,t.content.y);
        const auto right=std::min(static_cast<std::int64_t>(l.source.x)+w,static_cast<std::int64_t>(t.content.x)+t.content.width);
        const auto bottom=std::min(static_cast<std::int64_t>(l.source.y)+h,static_cast<std::int64_t>(t.content.y)+t.content.height);
        return left<right && top<bottom;
    };
    if (!intersects(*l.texture) || !intersects(*other.texture)) return;
    const auto right=std::min<std::int64_t>(out.width,static_cast<std::int64_t>(dst.x)+w);
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(dst.y)+h);
    for (int y=std::max(0,dst.y);y<bottom;++y) for (int x=std::max(0,dst.x);x<right;++x) {
        const int sx=clamp_int(static_cast<std::int64_t>(l.source.x)+x-dst.x);
        const int sy=clamp_int(static_cast<std::int64_t>(l.source.y)+y-dst.y);
        if (sx<0 || sy<0 || sx>=l.texture->canvas_width || sy>=l.texture->canvas_height) continue;
        const auto contains=[&](const Texture& t) {
            return sx>=t.content.x && sy>=t.content.y && static_cast<std::int64_t>(sx)<static_cast<std::int64_t>(t.content.x)+t.content.width &&
                   static_cast<std::int64_t>(sy)<static_cast<std::int64_t>(t.content.y)+t.content.height;
        };
        if (!contains(*l.texture) && !contains(*other.texture)) continue;
        const auto s1=texel(*l.texture,sx,sy),s2=texel(*other.texture,sx,sy);
        const auto di=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        if (!l.texture->has_mask || !other.texture->has_mask) {
            for (std::size_t c=0;c<3;++c)
                out.rgb[di+c]=static_cast<Byte>(std::min(255,((s1[c]*ps1)>>8)+((s2[c]*ps2)>>8)));
            continue;
        }
        const int alpha1=(ps1*s1[3])>>8,alpha2=(ps2*s2[3])>>8,total=alpha1+alpha2;
        for (std::size_t c=0;c<3;++c) {
            const int src=std::clamp((s1[c]*alpha1+s2[c]*alpha2)>>8,0,255);
            out.rgb[di+c]=static_cast<Byte>(std::clamp((src*total+out.rgb[di+c]*(256-total))>>8,0,255));
        }
    }
}
void metal(Surface& out,const Layer& l) {
    if (!l.texture || !l.texture->has_mask) return;
    const auto p=[&](std::size_t i){return value(l,i);};
    const auto& texture=*l.texture;
    const Rect dst=destination(l);
    const int mode=p(1),size=std::max(0,p(2));
    const int grad=clamp_int(static_cast<std::int64_t>(p(3))*size/100);
    const int span=clamp_int(static_cast<std::int64_t>(size)+(mode==0?texture.canvas_height:
        mode==1?texture.canvas_width:static_cast<std::int64_t>(texture.canvas_width)+texture.canvas_height));
    const int start=interpolate(l,0,span,p(4),p(5));
    const auto right=std::min<std::int64_t>(out.width,static_cast<std::int64_t>(dst.x)+texture.canvas_width);
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(dst.y)+texture.canvas_height);
    const int top=std::max(0,dst.y);
    for (int y=top;y<bottom;++y) for (int x=std::max(0,dst.x);x<right;++x) {
        const auto sx=clamp_int(static_cast<std::int64_t>(x)-dst.x),sy=clamp_int(static_cast<std::int64_t>(y)-dst.y);
        const auto src=texel(texture,sx,sy);
        if (!src[3]) continue;
        std::int64_t k=0;
        if (mode==0) k=static_cast<std::int64_t>(sy)+size-start;
        else if (mode==1) k=static_cast<std::int64_t>(sx)+size-start;
        else {
            const auto index=static_cast<std::int64_t>(x)+(mode==2?static_cast<int>(out.height)-(y-top):y-top);
            k=index-(static_cast<std::int64_t>(start)-size+dst.x+dst.y);
        }
        const auto di=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        for (std::size_t c=0;c<3;++c) {
            std::int64_t add=0;
            if (k>=0 && k<size) {
                add=p(6+c);
                if (grad>0 && k<grad) add=add*k/grad;
                else if (grad>0 && k>=static_cast<std::int64_t>(size)-grad) add=add*(size-k)/grad;
                add=std::clamp(add,std::int64_t{0},std::int64_t{255});
            }
            const int col=static_cast<int>(std::clamp(static_cast<std::int64_t>(src[c])+add,std::int64_t{0},std::int64_t{255}));
            out.rgb[di+c]=static_cast<Byte>(src[3]==255?col:((col*src[3])>>8)+((out.rgb[di+c]*(256-src[3]))>>8));
        }
    }
}
void chara_mosaic(Surface& out,const Layer& l) {
    if (!l.texture || !l.texture->has_mask) return;
    const auto p=[&](std::size_t i){return value(l,i);};
    const auto& t=*l.texture;
    const Rect dst=destination(l);
    const auto w=static_cast<std::int64_t>(out.width),h=static_cast<std::int64_t>(out.height);
    const auto sx=p(3)==1?static_cast<std::int64_t>(t.canvas_width/2)+p(4):0;
    const auto sy=p(3)==1?static_cast<std::int64_t>(t.canvas_height/2)+p(5):0;
    const auto sw=p(3)==1?p(6):t.canvas_width,sh=p(3)==1?p(7):t.canvas_height;
    const auto tx=p(8)==1?static_cast<std::int64_t>(p(9)):0,ty=p(8)==1?static_cast<std::int64_t>(p(10)):0;
    const auto tw=p(8)==1?p(11):w,th=p(8)==1?p(12):h;
    const auto left=std::max({std::int64_t{0},static_cast<std::int64_t>(dst.x),static_cast<std::int64_t>(dst.x)+sx,tx});
    const auto top=std::max({std::int64_t{0},static_cast<std::int64_t>(dst.y),static_cast<std::int64_t>(dst.y)+sy,ty});
    const auto right=std::min({w,static_cast<std::int64_t>(dst.x)+t.canvas_width,
        static_cast<std::int64_t>(dst.x)+sx+sw,tx+tw});
    const auto bottom=std::min({h,static_cast<std::int64_t>(dst.y)+t.canvas_height,
        static_cast<std::int64_t>(dst.y)+sy+sh,ty+th});
    const auto dr=std::min(w,static_cast<std::int64_t>(dst.x)+t.canvas_width);
    const auto db=std::min(h,static_cast<std::int64_t>(dst.y)+t.canvas_height);
    for (int y=std::max(0,dst.y);y<db;++y) for (int x=std::max(0,dst.x);x<dr;++x) {
        if (x>=left && x<right && y>=top && y<bottom) continue;
        const auto src=texel(t,clamp_int(static_cast<std::int64_t>(x)-dst.x),clamp_int(static_cast<std::int64_t>(y)-dst.y));
        const int a=src[3];
        if (!a) continue;
        const auto di=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        for (std::size_t c=0;c<3;++c)
            out.rgb[di+c]=static_cast<Byte>(a==255?src[c]:((src[c]*a)>>8)+((out.rgb[di+c]*(256-a))>>8));
    }
    if (left>=right || top>=bottom) return;
    const int bw=std::max(1,p(1)),bh=std::max(1,p(2));
    std::uint32_t seed=l.effect_state->seed^(l.count*0x9e3779b9U);
    for (auto y=top;y<bottom;) {
        const int size_y=static_cast<int>(std::min<std::int64_t>(bh,bottom-y));
        for (auto x=left;x<right;) {
            const int size_x=static_cast<int>(std::min<std::int64_t>(bw,right-x));
            int rx=static_cast<int>(random_next(seed)%static_cast<unsigned>(size_x));
            int ry=static_cast<int>(random_next(seed)%static_cast<unsigned>(size_y));
            auto sample=texel(t,clamp_int(x+rx-dst.x),clamp_int(y+ry-dst.y));
            if (!sample[3] && p(13)==1) {
                const auto attempts=static_cast<std::int64_t>(size_x)*size_y;
                for (std::int64_t i=0;i<attempts && !sample[3];++i) {
                    rx=static_cast<int>(random_next(seed)%static_cast<unsigned>(size_x));
                    ry=static_cast<int>(random_next(seed)%static_cast<unsigned>(size_y));
                    sample=texel(t,clamp_int(x+rx-dst.x),clamp_int(y+ry-dst.y));
                }
                for (std::int64_t i=0;i<attempts;++i) {
                    if (++rx>=size_x) { rx=0;ry=(ry+1)%size_y; }
                    sample=texel(t,clamp_int(x+rx-dst.x),clamp_int(y+ry-dst.y));
                    if (sample[3]) break;
                }
            }
            if (sample[3]) for (int yy=0;yy<size_y;++yy) for (int xx=0;xx<size_x;++xx) {
                const auto di=(static_cast<std::size_t>(y+yy)*out.width+static_cast<unsigned>(x+xx))*3;
                const auto original=texel(t,clamp_int(x+xx-dst.x),clamp_int(y+yy-dst.y));
                const int percent=p(14)==0?(sample[3]==255?100:sample[3]*100/255):p(15);
                const int alpha=std::clamp(percent,0,100)*256/100;
                for (std::size_t c=0;c<3;++c) {
                    const int v=p(13)==0?(original[3]==255?sample[c]:
                        (sample[c]*original[3]+out.rgb[di+c]*(255-original[3]))>>8):
                        (sample[c]*alpha+out.rgb[di+c]*(256-alpha))>>8;
                    if (p(13)!=0 || original[3]) out.rgb[di+c]=static_cast<Byte>(v);
                }
            }
            x+=size_x;
        }
        y+=size_y;
    }
}
void box(Surface& out,int x,int y,int width,int height,const std::array<int,3>& color,int percent) {
    const int alpha=std::clamp(percent,0,100)*256/100;
    const auto right=std::min<std::int64_t>(out.width,static_cast<std::int64_t>(x)+width);
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(y)+height);
    for (int yy=std::max(0,y);yy<bottom;++yy) for (int xx=std::max(0,x);xx<right;++xx) {
        const auto i=(static_cast<std::size_t>(yy)*out.width+static_cast<unsigned>(xx))*3;
        for (std::size_t c=0;c<3;++c)
            out.rgb[i+c]=static_cast<Byte>((std::clamp(color[c],0,255)*alpha+out.rgb[i+c]*(256-alpha))>>8);
    }
}
int circle_extent(int width,int height) {
    const auto w=static_cast<std::int64_t>(width),h=static_cast<std::int64_t>(height);
    return static_cast<int>(std::sqrt(static_cast<double>(w*w+h*h/4))+0.5);
}
void circular_mask(Surface& out,const Layer& layer) {
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    const int end=value(layer,3);
    if (layer.count>=duration(layer) && end>=circle_extent(w,h)) return;
    const std::array<int,3> color{value(layer,5)&255,value(layer,6)&255,value(layer,7)&255};
    if (layer.count>=duration(layer) && end<=1) { box(out,0,0,w,h,color,100);return; }
    const auto length=static_cast<std::int64_t>(std::max(1U,duration(layer)));
    const auto count=std::min<std::int64_t>(duration(layer)?layer.count:1U,length);
    const auto start=static_cast<std::int64_t>(value(layer,4));
    const auto radius=std::max<std::int64_t>(1,start+((end-start)*count+length/2)/length);
    const auto cx=static_cast<std::int64_t>(value(layer,1)),cy=static_cast<std::int64_t>(value(layer,2));
    const auto bound=[&](std::int64_t x) { return static_cast<int>(std::clamp(x,std::int64_t{0},static_cast<std::int64_t>(w))); };
    for (int y=0;y<h;++y) {
        const auto dy=std::abs(y-cy);
        if (dy>=radius) { box(out,0,y,w,1,color,100);continue; }
        const auto extent=static_cast<std::int64_t>(std::sqrt(static_cast<double>((radius-dy)*(radius+dy)))+0.5);
        const int left=bound(cx-extent+1),right=bound(cx+extent);
        box(out,0,y,left,1,color,100);
        box(out,right,y,w-right,1,color,100);
    }
}
void remembered_edges(Surface& out,const Layer& layer) {
    constexpr std::array<double,4> temporal{0.0345698,0.002178934,0.04425678,0.001234786};
    constexpr std::array<double,4> phase{0,4,11,15},spatial{0.05145735,0.0362343,0.053232,0.03417};
    constexpr std::array<double,4> amplitude{15,23,17,20},swing{0.3,0.22,0.27,0.19},swing_phase{2,3,5,7};
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    const int arc=std::min(100,(h-1)/2);
    const auto count=layer.effect_state->distance;
    std::array<double,4> angle{},scale{};
    for (std::size_t i=0;i<angle.size();++i) {
        angle[i]=count*temporal[i]*3.14159+phase[i];
        scale[i]=amplitude[i]*(1.5+std::cos(count*swing[i]+swing_phase[i]));
    }
    const auto bounds=[&](int row) {
        std::array<double,4> offset{};
        for (std::size_t i=0;i<offset.size();++i) offset[i]=std::cos(angle[i]+row*spatial[i])*scale[i];
        return std::array<int,2>{static_cast<int>(offset[0]+offset[1]+0.5)+64,
                                 static_cast<int>(offset[2]+offset[3]+0.5)+w-64};
    };
    const auto upper=bounds(arc),lower=bounds(h-arc-1);
    for (int y=0;y<h;++y) {
        auto [left,right]=bounds(y);
        if (y<arc) {
            const int delta=static_cast<int>(std::sin((arc-y)*3.14159/(2*arc))*0.4*(upper[1]-upper[0]));
            left=std::min(w/2-1,left+delta);right=std::max(w/2+1,right-delta);
        } else if (y>=h-arc) {
            const int span=lower[1]-lower[0];
            const int delta=static_cast<int>(std::sin((y-(h-arc))*3.14159/(2*arc))*0.4*span);
            left=std::min(span/2-1,left+delta);right=std::max(span/2+1,right-delta);
        }
        const int start_left=left+64,start_right=right-64;
        const auto row=static_cast<std::size_t>(y)*out.width*3;
        const auto brighten=[&](int x,int amount) {
            for (std::size_t c=0;c<3;++c) {
                auto& channel=out.rgb[row+static_cast<std::size_t>(x)*3+c];
                channel=static_cast<Byte>(std::min(255,channel+amount));
            }
        };
        if (start_left>=0)
            for (int x=0;x<std::min(w,(start_left/2+1)*2);++x)
                brighten(x,(start_left/2-x/2)*4);
        if (start_right>=0 && start_right<w)
            for (int x=start_right/2*2;x<w/2*2;++x)
                brighten(x,(x/2-start_right/2)*4);
    }
}
void melt_down(Surface& out,const Layer& layer) {
    const int rows=static_cast<int>(out.height)-1;
    const int top=rows-std::clamp(interpolate(layer,0,rows,value(layer,1),value(layer,2)),0,rows);
    const auto stride=static_cast<std::size_t>(out.width)*3;
    const auto* src=out.rgb.data()+static_cast<std::size_t>(top)*stride;
    for (int y=top+1;y<=rows;++y)
        std::memcpy(out.rgb.data()+static_cast<std::size_t>(y)*stride,src,stride);
}
void scrolling_picture(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    const auto tick=duration(layer)?layer.count:1U;
    const auto wrap=[&](int speed,int length) {
        const auto phase=static_cast<std::int64_t>(speed)*tick%length;
        return static_cast<int>(phase<0?phase+length:phase);
    };
    const int dx=wrap(value(layer,1),w),dy=wrap(value(layer,2),h);
    for (int y:{dy-h,dy}) for (int x:{dx-w,dx})
        paint(out,*layer.texture,{0,0,w,h},{x,y,w,h},100,{},false);
}
void curtain(Surface& out,const Layer& layer) {
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    const int type=value(layer,6),delay=value(layer,7);
    const int top=std::clamp(interpolate(layer,0,value(layer,1),type,delay),0,h);
    const auto end=clamp_int(static_cast<std::int64_t>(h)-value(layer,1)-value(layer,2));
    const int bottom=std::clamp(interpolate(layer,0,end,type,delay),0,h-top);
    std::array<int,3> color{};
    for (std::size_t c=0;c<3;++c)
        color[c]=interpolate(layer,value(layer,8+c),value(layer,3+c),value(layer,11),value(layer,12))&255;
    box(out,0,0,w,top,color,100);
    box(out,0,h-bottom,w,bottom,color,100);
}
void fusuma(Surface& out,const std::array<Layer,32>& layers,const Layer& layer) {
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height),index=value(layer,2);
    if (!layer.texture || index<0 || static_cast<std::size_t>(index)>=layers.size()) return;
    const auto& back=layers[static_cast<std::size_t>(index)].texture;
    const auto covers=[&](const Texture& t) { return t.canvas_width>=w && t.canvas_height>=h; };
    if (!back || !covers(*layer.texture) || !covers(*back)) return;
    const int width=std::clamp(value(layer,1),0,w/2);
    const int step=std::clamp(interpolate(layer,0,width,value(layer,3),value(layer,4)),0,width);
    const auto strip=[&](const Texture& texture,int x,int from,int size) {
        paint(out,texture,{from,0,size,h},{x,0,size,h},100,{},false);
    };
    if (!step) { strip(*layer.texture,0,0,w);return; }
    const int outer=w/2-width,inner=width-step;
    strip(*layer.texture,0,0,outer);
    strip(*layer.texture,outer,outer+step,inner);
    strip(*layer.texture,w-outer-inner,w/2,inner);
    strip(*layer.texture,w-outer,w-outer,outer);
    strip(*back,outer+inner,outer+inner,step*2);
}
void character_flash(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const auto& texture=*layer.texture;
    const auto dst=destination(layer);
    const auto on=static_cast<std::int64_t>(std::max(1,value(layer,4)));
    if (layer.count>=duration(layer) || layer.count%(on+std::max(1,value(layer,5)))<on) {
        normal_picture(out,texture,layer.source,dst,true);
        return;
    }
    if (!texture.has_mask) return;
    const auto first_x=std::max<std::int64_t>({0,layer.source.x,static_cast<std::int64_t>(layer.source.x)-dst.x});
    const auto first_y=std::max<std::int64_t>({0,layer.source.y,static_cast<std::int64_t>(layer.source.y)-dst.y});
    if (first_x>=texture.canvas_width || first_y>=texture.canvas_height) return;
    const int key=texel(texture,static_cast<int>(first_x),static_cast<int>(first_y))[3];
    const std::array<Byte,3> color{static_cast<Byte>(value(layer,1)),static_cast<Byte>(value(layer,2)),
                                   static_cast<Byte>(value(layer,3))};
    picture_pixels(out,texture,layer.source,dst,[&](Byte* pixel,const Byte* sample) {
        if (sample[3]!=key) std::copy(color.begin(),color.end(),pixel);
    });
}
void target(Surface& out,const Layer& layer) {
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    const int line=std::clamp(value(layer,5),1,std::max(w,h));
    const int percent=interpolate(layer,clamp_int(static_cast<std::int64_t>(value(layer,12))*100),
                                  10000,value(layer,10),value(layer,11));
    const auto move=[&](int start,int end) { return clamp_int(start+(static_cast<std::int64_t>(end)-start)*percent/10000); };
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int x1=move(0,p(1)),x2=move(w-line,clamp_int(static_cast<std::int64_t>(p(1))+p(3)-line));
    const int y1=move(0,p(2)),y2=move(h-line,clamp_int(static_cast<std::int64_t>(p(2))+p(4)-line));
    const std::array<int,3> color{p(6),p(7),p(8)};
    const int type=p(9);
    for (const Rect rc:std::array<Rect,4>{{{x1,0,line,h},{x2,0,line,h},{0,y1,w,line},{0,y2,w,line}}}) {
        if (type==0 || type==1) { box(out,rc.x,rc.y,rc.width,rc.height,color,type==0?100:p(13));continue; }
        const auto right=std::min<std::int64_t>(w,static_cast<std::int64_t>(rc.x)+rc.width);
        const auto bottom=std::min<std::int64_t>(h,static_cast<std::int64_t>(rc.y)+rc.height);
        for (int y=std::max(0,rc.y);y<bottom;++y) for (int x=std::max(0,rc.x);x<right;++x) {
            const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
            for (std::size_t c=0;c<3;++c)
                out.rgb[at+c]=static_cast<Byte>(std::clamp(out.rgb[at+c]+std::clamp(color[c],-255,255),0,255));
        }
    }
}
void boss_death(Surface& out,const Layer& layer) {
    if (!layer.texture || layer.count>=duration(layer)) return;
    const auto& texture=*layer.texture;
    const int h=static_cast<int>(out.height),w=texture.canvas_width;
    const auto keep=static_cast<std::int64_t>(value(layer,1));
    const auto length=std::max<std::int64_t>(1,static_cast<std::int64_t>(duration(layer))-keep);
    const auto tick=std::clamp(static_cast<std::int64_t>(layer.count)-keep,std::int64_t{0},length);
    const int first=static_cast<int>(tick*h/length);
    const int jitter=std::max(0,interpolate(layer,value(layer,3),value(layer,2),value(layer,4),value(layer,5)));
    const int percent=interpolate(layer,value(layer,8),value(layer,7),value(layer,9),value(layer,10));
    const int left=destination(layer).x;
    auto seed=layer.effect_state->seed^(layer.count*0x9e3779b9U);
    const int rows=std::min(h-first,texture.canvas_height);
    for (int y=0;y<rows;++y) {
        const int shift=jitter?static_cast<int>((random_next(seed)&0x7fffU)%(2U*static_cast<unsigned>(jitter)))-jitter:0;
        const Rect src{0,y,w,1},dst{clamp_int(static_cast<std::int64_t>(left)+shift),first+y,w,1};
        if (value(layer,6)) translucent_picture(out,texture,src,dst,percent);
        else normal_picture(out,texture,src,dst,true);
    }
}
void camouflage(Surface& out,const Layer& layer) {
    if (!layer.texture || !layer.texture->has_mask) return;
    const auto& texture=*layer.texture;
    const auto dst=destination(layer);
    const int shift=std::max(0,interpolate(layer,value(layer,3),value(layer,1),value(layer,5),value(layer,6)));
    const auto x0=static_cast<std::int64_t>(dst.x)+texture.content.x;
    const auto y0=static_cast<std::int64_t>(dst.y)+texture.content.y;
    const auto right=std::min<std::int64_t>(out.width,x0+texture.content.width)-shift;
    const auto bottom=std::min<std::int64_t>(out.height,y0+texture.content.height);
    const auto offset=static_cast<std::size_t>(shift)*3;
    for (auto y=std::max<std::int64_t>(0,y0);y<bottom;++y) for (auto x=std::max<std::int64_t>(0,x0);x<right;++x) {
        const auto i=(static_cast<std::size_t>(y-y0)*texture.pixels.width+static_cast<std::size_t>(x-x0))*4;
        if (texture.pixels.rgba[i+3]==0) continue;
        const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<std::size_t>(x))*3;
        for (std::size_t c=0;c<3;++c) out.rgb[at+c]=out.rgb[at+offset+c];
    }
}
void simple_layer_wipe(Surface& out,const std::array<Layer,32>& layers,unsigned slot,const Surface& saved) {
    const auto& layer=layers[slot];
    const int type=value(layer,1),second=value(layer,2);
    if (!supports_wipe(type)) return;
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    Surface first_buffer,second_buffer;
    const auto source=[&](const Layer& item,bool use_saved,Surface& storage)->const Surface* {
        if (use_saved) return saved.rgb.empty()?nullptr:&saved;
        if (!item.texture || item.texture->canvas_width<w || item.texture->canvas_height<h) return nullptr;
        const auto origin=window_origin(out,item,*item.texture);
        storage={out.width,out.height,std::vector<Byte>(out.rgb.size())};
        paint(storage,*item.texture,{origin.x,origin.y,w,h},{0,0,w,h},100,{},false);
        return &storage;
    };
    if (!value(layer,7) && (second<0 || static_cast<std::size_t>(second)>=layers.size())) return;
    const auto* first=source(layer,value(layer,8)!=0,first_buffer);
    const auto* last=source(value(layer,7)?layer:layers[static_cast<std::size_t>(second)],value(layer,7)!=0,second_buffer);
    if (!first || !last) return;
    const auto scale=[](int p) { return clamp_int(static_cast<std::int64_t>(p)*100); };
    const int progress=std::clamp(interpolate(layer,scale(value(layer,6)),scale(value(layer,5)),
                                              value(layer,3),value(layer,4)),0,10000);
    draw_wipe(out,*first,*last,type,static_cast<unsigned>(progress),10000);
}
bool geometry_effect(int number) {
    return (number>=87 && number<=89) || number==95 || number==113 || (number>=114 && number<=118);
}
struct Ellipse {
    int x,y,rx,ry;

    std::array<int,2> span(int row,int width) const {
        const auto distance=static_cast<std::int64_t>(row)-y;
        if (std::abs(distance)>ry) return {};
        const auto normalized=static_cast<double>(distance)/ry;
        const auto radius=static_cast<int>(std::sqrt(std::max(0.0,1.0-normalized*normalized))*rx+0.5);
        const auto bound=[width](std::int64_t n) {
            return static_cast<int>(std::clamp(n,std::int64_t{0},static_cast<std::int64_t>(width)));
        };
        return {bound(static_cast<std::int64_t>(x)-radius),bound(static_cast<std::int64_t>(x)+radius+1)};
    }
};
void ellipse_overlay(Surface& out,const Ellipse& outer,const Ellipse* hole,
                     std::array<int,3> color,int percent,bool additive) {
    if (outer.rx<1 || outer.ry<1 || (hole && (hole->rx<1 || hole->ry<1))) return;
    const int strength=std::clamp(percent,0,100);
    const int gain=strength*256/100,remain=(100-strength)*256/100;
    for (auto& channel:color) channel=additive?
        static_cast<int>(std::clamp(static_cast<std::int64_t>(channel)*percent/100,std::int64_t{-255},std::int64_t{255})):
        std::clamp(channel,0,255)*gain;
    const int width=static_cast<int>(out.width);
    const auto fill=[&](int y,int start,int end) {
        auto pixel=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(start))*3;
        for (int x=start;x<end;++x,pixel+=3)
            for (std::size_t c=0;c<3;++c)
                out.rgb[pixel+c]=static_cast<Byte>(additive?std::clamp(out.rgb[pixel+c]+color[c],0,255):
                    (out.rgb[pixel+c]*remain+color[c])>>8);
    };
    const auto top=std::max<std::int64_t>(0,static_cast<std::int64_t>(outer.y)-outer.ry);
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(outer.y)+outer.ry+1);
    for (auto row=top;row<bottom;++row) {
        const auto y=static_cast<int>(row);
        const auto [left,right]=outer.span(y,width);
        if (hole) {
            const auto [inner_left,inner_right]=hole->span(y,width);
            fill(y,left,std::min(inner_left,right));
            fill(y,std::max(inner_right,left),right);
        } else fill(y,left,right);
    }
}
void geometry(Surface& out,const Layer& layer,unsigned slot) {
    const auto p=[&](std::size_t i){return value(layer,i);};
    if (layer.effect==88 || layer.effect==95 || layer.effect==113 || layer.effect==117) {
        // These commands all reduce to a full-screen RGB fill in the original.
        const std::size_t start=layer.effect==113?1:0;
        box(out,0,0,static_cast<int>(out.width),static_cast<int>(out.height),
            {p(start)&255,p(start+1)&255,p(start+2)&255},100);
        return;
    }
    ordinary(out,layer,slot);
    if (layer.effect==114 || layer.effect==118) {
        const auto count=static_cast<std::int64_t>(layer.count);
        const auto total=static_cast<std::int64_t>(duration(layer))-p(1)-p(2);
        const auto frame=count-p(1);
        if (total<=0 || frame<0 || frame>=total) return;
        const auto at=[&](std::size_t start,std::size_t end) {
            return clamp_int(static_cast<std::int64_t>(p(start))+(static_cast<std::int64_t>(p(end))-p(start))*frame/total);
        };
        const Ellipse area{at(13,15),at(14,16),at(3,5),at(4,6)};
        const std::array<int,3> color{at(7,10),at(8,11),at(9,12)};
        // GeoBoxAdd uses the ellipse fill in the PC implementation.
        if (layer.effect==118) ellipse_overlay(out,area,nullptr,color,100,false);
        else box(out,area.x,area.y,area.rx,area.ry,color,100);
        return;
    }
    const auto at=[&](std::size_t start,std::size_t end,std::size_t curve) {
        return interpolate(layer,p(start),p(end),p(curve),p(curve+1));
    };
    if (layer.effect==87 || layer.effect==115) {
        const Ellipse outer{at(9,1,11),at(10,2,11),at(13,3,15),at(14,4,15)};
        const std::array<int,3> color{at(17,5,20),at(18,6,20),at(19,7,20)};
        ellipse_overlay(out,outer,nullptr,color,at(22,8,23),layer.effect==115);
    } else {
        const Ellipse outer{at(13,1,17),at(14,2,17),at(19,3,23),at(20,4,23)};
        const Ellipse inner{at(15,5,17),at(16,6,17),at(21,7,23),at(22,8,23)};
        const std::array<int,3> color{at(25,9,28),at(26,10,28),at(27,11,28)};
        ellipse_overlay(out,outer,&inner,color,at(30,12,31),layer.effect==116);
    }
}
void line(Surface& out,float x0,float y0,float x1,float y1,const std::array<int,3>& color,int percent) {
    if (percent<=0) return;
    const float dx=x1-x0,dy=y1-y0;
    const float extent=std::max({std::abs(dx),std::abs(dy),1.0F});
    const int steps=static_cast<int>(std::min(extent+1,100000.0F));
    const float inverse=1.0F/extent;
    const int a=std::clamp(percent,0,100)*255/100;
    for (int i=0;i<steps;++i) {
        const float fraction=static_cast<float>(i)*inverse;
        const int x=static_cast<int>(std::clamp(static_cast<double>(x0+dx*fraction+0.5F),-100000.0,100000.0));
        const int y=static_cast<int>(std::clamp(static_cast<double>(y0+dy*fraction+0.5F),-100000.0,100000.0));
        if (x<0 || y<0 || x>=static_cast<int>(out.width) || y>=static_cast<int>(out.height)) continue;
        const auto di=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        for (std::size_t c=0;c<3;++c)
            out.rgb[di+c]=static_cast<Byte>((out.rgb[di+c]*(255-a)+std::clamp(color[c],0,255)*a)>>8);
    }
}
void taclip(Surface& out,const Layer& l) {
    if (!l.texture || !l.texture->has_mask) return;
    const auto p=[&](std::size_t i){return value(l,i);};
    const int v=interpolate(l,0,10000,p(33),p(34));
    const auto vector=[&](std::size_t end,std::size_t start) {
        return clamp_int(static_cast<std::int64_t>(p(start))+
            (static_cast<std::int64_t>(p(end))-p(start))*v/10000);
    };
    const int cx=vector(1,29),cy=vector(2,30);
    int vx=vector(3,31),vy=vector(4,32);
    if (!vx && !vy) vx=1;
    const bool square=p(5)!=0;
    if (square && (!vx || !vy)) return;
    const int ps=interpolate(l,clamp_int(static_cast<std::int64_t>(p(7))*100),
        clamp_int(static_cast<std::int64_t>(p(6))*100),p(8),p(9));
    const int alpha256=static_cast<int>(std::clamp(static_cast<std::int64_t>(ps)*255/10000,std::int64_t{0},std::int64_t{256}));
    const int fade=p(10)?std::clamp(interpolate(l,0,255,p(12),p(11)),0,255):255;
    const Rect dst=destination(l);
    const auto& texture=*l.texture;
    const auto right=std::min<std::int64_t>(out.width,static_cast<std::int64_t>(dst.x)+texture.canvas_width);
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(dst.y)+texture.canvas_height);
    const std::int64_t square_x2=static_cast<std::int64_t>(cx)+vx;
    const std::int64_t square_y2=static_cast<std::int64_t>(cy)+vy+(vy>0?-1:1);
    for (int y=std::max(0,dst.y);y<bottom;++y) for (int x=std::max(0,dst.x);x<right;++x) {
        bool visible;
        if (square) {
            visible=x>=std::min<std::int64_t>(cx,square_x2) && x<std::max<std::int64_t>(cx,square_x2) &&
                    y>=std::min<std::int64_t>(cy,square_y2) && y<=std::max<std::int64_t>(cy,square_y2);
        } else if (!vy) visible=vx>0?y>=cy:y<=cy;
        else if (!vx) visible=vy<0?x>=cx:x<=cx;
        else {
            const float f=static_cast<float>(vx)/static_cast<float>(vy);
            const double raw=static_cast<double>(static_cast<float>(static_cast<std::int64_t>(y)-cy)*f+(vy>=0?-1.0F:0.0F)+0.5F);
            const auto limit=static_cast<std::int64_t>(cx)+static_cast<int>(std::clamp(raw,-1.0e9,1.0e9));
            visible=vy<0?x>=limit:x<=limit;
        }
        if (!visible) continue;
        const auto src=texel(texture,clamp_int(static_cast<std::int64_t>(x)-dst.x),clamp_int(static_cast<std::int64_t>(y)-dst.y));
        if (!src[3]) continue;
        const int alpha=((src[3]+1)*alpha256)>>8;
        const auto di=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        for (std::size_t c=0;c<3;++c) {
            const int flash=static_cast<int>(std::clamp((255-static_cast<std::int64_t>(fade))*p(13+c)/255,std::int64_t{0},std::int64_t{255}));
            const int col=p(10)?std::min(255,((src[c]*fade)>>8)+flash):src[c];
            out.rgb[di+c]=static_cast<Byte>((col*alpha+out.rgb[di+c]*(256-alpha))>>8);
        }
    }
    const int line_size=std::clamp(p(16),0,static_cast<int>(std::max(out.width,out.height))*2);
    if (!line_size) return;
    const int line_percent=interpolate(l,p(21),p(17),p(22),p(23));
    std::array<int,3> color{};
    for (std::size_t c=0;c<3;++c) color[c]=interpolate(l,p(24+c),p(18+c),p(27),p(28));
    if (!square) {
        if (std::abs(static_cast<std::int64_t>(vx))>std::abs(static_cast<std::int64_t>(vy))) {
            const float f=static_cast<float>(vy)/static_cast<float>(vx);
            const float x0=static_cast<float>(-line_size),x1=static_cast<float>(out.width)+static_cast<float>(line_size);
            const float y0=(static_cast<float>(cx)+static_cast<float>(line_size))*-f+static_cast<float>(cy);
            const float y1=f*(static_cast<float>(out.width)+static_cast<float>(line_size)-static_cast<float>(cx))+static_cast<float>(cy);
            for (int i=0;i<line_size;++i) {
                const float offset=static_cast<float>(i)-static_cast<float>(line_size/2);
                line(out,x0,y0+offset,x1,y1+offset,color,line_percent);
            }
        } else {
            const float f=static_cast<float>(vx)/static_cast<float>(vy);
            const float y0=static_cast<float>(-line_size),y1=static_cast<float>(out.height)+static_cast<float>(line_size);
            const float x0=(static_cast<float>(cy)+static_cast<float>(line_size))*-f+static_cast<float>(cx);
            const float x1=f*(static_cast<float>(out.height)+static_cast<float>(line_size)-static_cast<float>(cy))+static_cast<float>(cx);
            for (int i=0;i<line_size;++i) {
                const float offset=static_cast<float>(i)-static_cast<float>(line_size/2);
                line(out,x0+offset,y0,x1+offset,y1,color,line_percent);
            }
        }
    } else {
        const auto x0=std::min<std::int64_t>(cx,square_x2),x1=std::max<std::int64_t>(cx,square_x2);
        const auto y0=std::min<std::int64_t>(cy,static_cast<std::int64_t>(cy)+vy);
        const auto y1=std::max<std::int64_t>(cy,static_cast<std::int64_t>(cy)+vy);
        const int left=clamp_int(x0-line_size/2),top=clamp_int(y0-line_size/2);
        const int width=clamp_int(x1-x0+line_size);
        box(out,left,top,width,line_size,color,line_percent);
        const int lower=std::max(clamp_int(y1-line_size/2),clamp_int(static_cast<std::int64_t>(top)+line_size));
        const int lower_height=clamp_int(std::max<std::int64_t>(0,y1-line_size/2+line_size-lower));
        box(out,left,lower,width,lower_height,color,line_percent);
        const int gap=clamp_int(static_cast<std::int64_t>(lower)-top-line_size);
        if (gap>0) {
            box(out,left,clamp_int(static_cast<std::int64_t>(top)+line_size),line_size,gap,color,line_percent);
            const int r=clamp_int(x1-line_size/2);
            if (static_cast<std::int64_t>(r)>=static_cast<std::int64_t>(left)+line_size)
                box(out,r,clamp_int(static_cast<std::int64_t>(top)+line_size),line_size,gap,color,line_percent);
        }
    }
}
std::array<int,2> blur_size(const Layer& l) {
    const int ps=interpolate(l,value(l,4),value(l,3),value(l,5),value(l,6));
    int x=std::clamp(value(l,1)*ps/100,1,256),y=std::clamp(value(l,2)*ps/100,1,256);
    if (value(l,7)) {
        if (x>1 && x%2==0) --x;
        if (y>1 && y%2==0) --y;
    }
    if (x*y>256) y=256/x;
    return {x,y};
}
void blur_screen_picture(Surface& out,const Layer& l,int kx,int ky) {
    if (!l.texture) return;
    const Rect dst=destination(l);
    const auto& t=*l.texture;
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    const auto src_x=-static_cast<std::int64_t>(dst.x),src_y=-static_cast<std::int64_t>(dst.y);
    if (src_x<0 || src_y<0 || src_x+w>dst.width || src_y+h>dst.height ||
        src_x+w>t.canvas_width || src_y+h>t.canvas_height) return;
    if (kx==1 && ky==1) {
        paint(out,t,{0,0,dst.width,dst.height},{dst.x,dst.y,dst.width,dst.height},100,{},false);
        return;
    }
    if (kx>w || ky>h) return;
    std::vector<std::array<int,3>> cols(static_cast<std::size_t>(w));
    const auto update=[&](int y,int direction) {
        for (int x=0;x<w;++x) {
            const auto pixel=texel(t,static_cast<int>(src_x)+x,static_cast<int>(src_y)+y);
            for (std::size_t c=0;c<3;++c) cols[static_cast<std::size_t>(x)][c]+=pixel[c]*direction;
        }
    };
    for (int y=0;y<ky;++y) update(y,1);
    const int end_x=w-(kx-1),end_y=h-(ky-1),div=std::min(32767,65536/(kx*ky));
    int previous_y=0;
    for (int y=0;y<h;++y) {
        const int source_y=y*end_y/h;
        if (source_y!=previous_y) {
            update(previous_y,-1);update(previous_y+ky,1);previous_y=source_y;
        }
        std::array<int,3> sums{};
        for (int x=0;x<kx;++x) for (std::size_t c=0;c<3;++c) sums[c]+=cols[static_cast<std::size_t>(x)][c];
        int previous_x=0;
        for (int x=0;x<w;++x) {
            const int source_x=x*end_x/w;
            if (source_x!=previous_x) {
                for (std::size_t c=0;c<3;++c)
                    sums[c]+=cols[static_cast<std::size_t>(previous_x+kx)][c]-cols[static_cast<std::size_t>(previous_x)][c];
                previous_x=source_x;
            }
            const auto di=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
            for (std::size_t c=0;c<3;++c) out.rgb[di+c]=static_cast<Byte>((((sums[c]>>1)*div)>>16)<<1);
        }
    }
}
void blur_character(Surface& out,const Layer& l,int kx,int ky,bool background) {
    if (!l.texture) return;
    const Rect dst=destination(l);
    const auto& t=*l.texture;
    if (kx==1 && ky==1) {
        normal_picture(out,t,{0,0,dst.width,dst.height},{dst.x,dst.y,dst.width,dst.height},!background);
        return;
    }
    if (!background && !t.has_mask) return;
    const int clip_left=std::max(0,t.content.x),clip_top=std::max(0,t.content.y);
    const int clip_right=static_cast<int>(std::min<std::int64_t>({t.canvas_width,dst.width,static_cast<std::int64_t>(t.content.x)+t.content.width}));
    const int clip_bottom=static_cast<int>(std::min<std::int64_t>({t.canvas_height,dst.height,static_cast<std::int64_t>(t.content.y)+t.content.height}));
    if (clip_left>=clip_right || clip_top>=clip_bottom) return;
    const int xl=kx/2,xr=kx-xl,yt=ky/2,yb=ky-yt;
    const int left=static_cast<int>(std::clamp(static_cast<std::int64_t>(dst.x)+clip_left-xl,std::int64_t{0},static_cast<std::int64_t>(out.width)));
    const int right=static_cast<int>(std::clamp(static_cast<std::int64_t>(dst.x)+clip_right+xr,std::int64_t{0},static_cast<std::int64_t>(out.width)));
    const int top=static_cast<int>(std::clamp(static_cast<std::int64_t>(dst.y)+clip_top-yt,std::int64_t{0},static_cast<std::int64_t>(out.height)));
    const int bottom=static_cast<int>(std::clamp(static_cast<std::int64_t>(dst.y)+clip_bottom+yb,std::int64_t{0},static_cast<std::int64_t>(out.height)));
    if (left>=right || top>=bottom) return;
    std::vector<std::array<int,4>> columns(static_cast<std::size_t>(clip_right-clip_left));
    const bool repeat_bottom=!background && value(l,8)!=0 && clip_bottom>1;
    const auto update=[&](int row,int direction,bool repeat) {
        if (row<clip_top) return;
        if (row>=clip_bottom) {
            if (!repeat) return;
            row=clip_bottom-1;
        }
        for (int x=clip_left;x<clip_right;++x) {
            const auto pixel=texel(t,x,row);
            auto& col=columns[static_cast<std::size_t>(x-clip_left)];
            const int alpha=background?1:pixel[3];
            for (std::size_t c=0;c<3;++c) col[c]+=pixel[c]*alpha*direction;
            col[3]+=alpha*direction;
        }
    };
    const int source_top=clamp_int(static_cast<std::int64_t>(top)-dst.y);
    for (int row=source_top-yt;row<source_top+yb;++row) update(row,1,false);
    const int div=std::min(32767,65536/(kx*ky));
    const auto column=[&](int x) {
        return x<clip_left || x>=clip_right?std::array<int,4>{}:columns[static_cast<std::size_t>(x-clip_left)];
    };
    for (int y=top;y<bottom;++y) {
        const int sy=clamp_int(static_cast<std::int64_t>(y)-dst.y);
        const int initial_x=clamp_int(static_cast<std::int64_t>(left)-dst.x);
        std::array<int,4> sum{};
        for (int x=initial_x-xl;x<initial_x+xr;++x) {
            const auto col=column(x);
            for (std::size_t c=0;c<4;++c) sum[c]+=col[c];
        }
        for (int x=left;x<right;++x) {
            if (sum[3]>0) {
                const auto di=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
                for (std::size_t c=0;c<3;++c) {
                    int pixel;
                    if (background) {
                        const int reciprocal=std::min(32767,32768/sum[3]);
                        pixel=std::clamp(((sum[c]>>1)*reciprocal)>>14,0,255);
                    } else {
                        const int scaled=(sum[c]>>16)*div+(((((sum[c]&65535)>>1)*div)>>16)<<1);
                        const int alpha=(((((sum[3]>>1)*div)>>16)<<1)+1);
                        pixel=std::clamp((scaled+out.rgb[di+c]*(255-alpha))>>8,0,255);
                    }
                    out.rgb[di+c]=static_cast<Byte>(pixel);
                }
            }
            const int sx=clamp_int(static_cast<std::int64_t>(x)-dst.x);
            const auto leaving=column(sx-xl),entering=column(sx+xr);
            for (std::size_t c=0;c<4;++c) sum[c]+=entering[c]-leaving[c];
        }
        update(sy-yt,-1,repeat_bottom);
        update(sy+yb,1,repeat_bottom);
    }
}
void lamp_visibility(Layer& layer) {
    auto& state=*layer.effect_state;
    if (state.lamp_mode==2) { state.lamp_visible=false;return; }
    if (state.lamp_mode==1) { state.lamp_visible=true;return; }
    const auto div=std::max<std::int64_t>(1,static_cast<std::int64_t>(value(layer,4))+value(layer,5));
    state.lamp_visible=state.lamp_time%div<value(layer,4);
    if (value(layer,6)>0 && random_next(state.seed)%100U<static_cast<unsigned>(value(layer,6)))
        state.lamp_visible=!state.lamp_visible;
}
void lamp_tick(Layer& layer) {
    auto& state=*layer.effect_state;
    if (layer.count>10000) layer.count=0;
    if (--state.lamp_time<0) {
        const int type=state.lamp_mode;
        int next=0,base=value(layer,1);
        if (!type) {
            const auto range=std::clamp(static_cast<std::int64_t>(value(layer,2))+value(layer,3),
                std::int64_t{1},std::int64_t{2147483647});
            if (random_next(state.seed)%static_cast<unsigned>(range)<static_cast<std::int64_t>(value(layer,2))) {
                next=1;base=value(layer,2);
            } else { next=2;base=value(layer,3); }
        }
        state.lamp_mode=next;
        auto time=static_cast<std::int64_t>(base);
        if (base>0) time+=static_cast<std::int64_t>(random_next(state.seed)%static_cast<unsigned>(base))-base/2;
        state.lamp_time=std::max(1,clamp_int(time));
    }
    lamp_visibility(layer);
}
void old_lamp(Surface& out,const Layer& layer) {
    if (!layer.texture || !layer.effect_state->lamp_visible) return;
    const auto& t=*layer.texture;const auto dst=destination(layer);
    normal_picture(out,t,{0,0,t.canvas_width,t.canvas_height},{dst.x,dst.y,t.canvas_width,t.canvas_height},true);
}
void scratch_spawn(Layer& layer,FilmScratch& scratch) {
    auto& state=*layer.effect_state;const auto p=[&](std::size_t i){return value(layer,i);};
    const auto rnd=[&](std::int64_t n) { return static_cast<int>(random_next(state.seed)%
        static_cast<unsigned>(std::clamp(n,std::int64_t{1},std::int64_t{2147483647}))); };
    scratch.change=rnd(20)+10;
    const auto jitter=std::max<std::int64_t>(1,static_cast<std::int64_t>(std::max(1,p(11)))*p(10)/100);
    auto delta=static_cast<std::int64_t>(rnd(jitter));if (rnd(100)<50) delta=-delta;
    scratch.life=std::max(1,clamp_int(static_cast<std::int64_t>(p(10))+delta));
    const auto range=std::max<std::int64_t>(1,static_cast<std::int64_t>(p(3))-p(6)+1);
    scratch.x=(rnd(range)+static_cast<std::int64_t>(p(1)))*256;
    scratch.speed=rnd(1024)+128;if (rnd(100)<50) scratch.speed=-scratch.speed;
}
void scratch_tick(Layer& layer) {
    auto& state=*layer.effect_state;const auto p=[&](std::size_t i){return value(layer,i);};
    for (auto& scratch:state.scratches) {
        if (--scratch.life<=0) scratch_spawn(layer,scratch);
        if (--scratch.change<=0) {
            scratch.change=static_cast<int>(random_next(state.seed)%20U)+10;
            scratch.speed=static_cast<int>(random_next(state.seed)%1024U)+128;
            if (random_next(state.seed)%100U<50) scratch.speed=-scratch.speed;
        }
        const auto minimum=static_cast<std::int64_t>(p(1))*256;
        const auto maximum=(static_cast<std::int64_t>(p(1))+p(3)-p(6)+1)*256;
        auto x=scratch.x+scratch.speed;
        if (x>=maximum) { x=maximum-1;scratch.speed=-scratch.speed; }
        if (x<minimum) { x=minimum;scratch.speed=-scratch.speed; }
        scratch.x=x;
        scratch.visible=static_cast<int>(random_next(state.seed)%100U)<p(12);
        if (scratch.visible) {
            const auto low=std::min(p(13),p(14)),high=std::max(p(13),p(14));
            const auto range=std::min<std::int64_t>(2147483647,static_cast<std::int64_t>(high)-low+1);
            scratch.percent=clamp_int(static_cast<std::int64_t>(low)+random_next(state.seed)%static_cast<unsigned>(range));
        }
    }
}
void old_film(Surface& out,const Layer& layer) {
    const std::array<int,3> color{value(layer,7),value(layer,8),value(layer,9)};
    for (const auto& scratch:layer.effect_state->scratches) if (scratch.visible)
        box(out,clamp_int(scratch.x/256),value(layer,2),value(layer,6),value(layer,4),color,scratch.percent);
}
void raster(Surface& out,const Layer& layer) {
    const int width=static_cast<int>(out.width),height=static_cast<int>(out.height);
    if (width<2) return;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int amplitude=interpolate(layer,clamp_int(static_cast<std::int64_t>(p(1))*100),
        clamp_int(static_cast<std::int64_t>(p(2))*100),p(3),p(4));
    if (amplitude<100) return;
    const bool increasing=p(2)>p(1);
    const auto count=increasing?static_cast<std::int64_t>(effects::duration(layer))-layer.count:layer.count;
    const int sign=increasing?-1:1;
    for (int y=0;y<height;++y) {
        const double theta=static_cast<double>(static_cast<std::int64_t>(y)*sign+count*10)*(3.14159*2/height*3);
        const double raw=std::cos(theta)*(amplitude/100.0)+0.5;
        int d=static_cast<int>(std::clamp(raw,static_cast<double>(-width),static_cast<double>(width)));
        if (d<=-width) d=1-width;
        if (d>=width-1) d=width-2;
        const auto row=static_cast<std::size_t>(y)*out.width*3;
        if (d>0) {
            for (int x=width-1;x>=d;--x) for (int c=0;c<3;++c)
                out.rgb[row+static_cast<std::size_t>(x*3+c)]=out.rgb[row+static_cast<std::size_t>((x-d)*3+c)];
            std::fill_n(out.rgb.data()+row,static_cast<std::size_t>(d)*3,Byte{0});
        } else if (d<0) {
            for (int x=0;x<width+d;++x) for (int c=0;c<3;++c)
                out.rgb[row+static_cast<std::size_t>(x*3+c)]=out.rgb[row+static_cast<std::size_t>((x-d)*3+c)];
            std::fill_n(out.rgb.data()+row+static_cast<std::size_t>(width+d)*3,static_cast<std::size_t>(-d)*3,Byte{0});
        }
    }
}
void character_raster(Surface& out,const Layer& layer) {
    if (!layer.texture || !layer.texture->has_mask) return;
    const auto& texture=*layer.texture;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int width=static_cast<int>(out.width),height=static_cast<int>(out.height);
    const int alpha=std::clamp(fixed_percent(layer,p(7),p(6),p(8),p(9)),0,256);
    if (!alpha) return;
    const auto dst=destination(layer);
    const double swing=interpolate(layer,0,clamp_int(static_cast<std::int64_t>(p(1))*100),p(4),p(5))*0.01;
    const double phase=static_cast<double>(layer.count)*p(2);
    const double frequency=3.14159*6.0*p(3)*0.01/height;
    const auto bottom=std::min<std::int64_t>(height,static_cast<std::int64_t>(dst.y)+texture.canvas_height);
    for (int y=std::max(0,dst.y);y<bottom;++y) {
        const int row=y-dst.y;
        const int shift=static_cast<int>(std::clamp(std::cos((row+phase)*frequency)*swing+0.5,
            static_cast<double>(1-width),static_cast<double>(std::max(0,width-2))));
        const int x=clamp_int(static_cast<std::int64_t>(dst.x)+shift);
        const int w=texture.canvas_width;
        picture_pixels(out,texture,{0,row,w,1},{x,y,w,1},[&](Byte* pixel,const Byte* sample) {
            const int weight=(sample[3]*alpha)>>8;
            if (!weight) return;
            for (std::size_t c=0;c<3;++c)
                pixel[c]=static_cast<Byte>(weight==255?sample[c]:(sample[c]*weight+pixel[c]*(256-weight))>>8);
        });
    }
}
void movie_warp(Surface& out,const Layer& layer) {
    if (!layer.texture || !layer.texture->has_mask) return;
    const auto& texture=*layer.texture;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int time=10000-interpolate(layer,0,10000,p(5),p(6));
    const int rotation=clamp_int(static_cast<std::int64_t>(p(3))*time/10000);
    const int opacity=std::clamp(interpolate(layer,p(8),p(7),p(9),p(10)),0,100);
    if (!opacity) return;
    const auto scaled=[](int n) { return clamp_int(static_cast<std::int64_t>(n)*10); };
    const double zoom=1000.0/std::max(1,interpolate(layer,scaled(p(12)),scaled(p(11)),p(13),p(14)));
    const auto dst=destination(layer);
    const auto center_x=static_cast<std::int64_t>(dst.x)+p(1),center_y=static_cast<std::int64_t>(dst.y)+p(2);
    const double max_dx=static_cast<double>(std::max(std::abs(center_x),std::abs(texture.canvas_width-center_x)));
    const double max_dy=static_cast<double>(std::max(std::abs(center_y),std::abs(texture.canvas_height-center_y)));
    const double max_distance=std::hypot(max_dx,max_dy)+2;
    const int radius=std::max(1,std::min(texture.canvas_width,texture.canvas_height)/2);
    static const auto turns=[] {
        std::array<std::array<double,2>,360> table{};
        for (std::size_t i=0;i<table.size();++i) {
            const double angle=static_cast<double>(i)*3.14159/180.0;
            table[i]={std::cos(angle),std::sin(angle)};
        }
        return table;
    }();
    for (unsigned y=0;y<out.height;++y) for (unsigned x=0;x<out.width;++x) {
        const double dx=static_cast<double>(x)-p(1),dy=static_cast<double>(y)-p(2);
        const double distance=std::sqrt(dx*dx+dy*dy),scaled_distance=distance*zoom;
        if (scaled_distance>max_distance) continue;
        const auto magnitude=std::max(1.0,std::trunc(scaled_distance));
        const double twist=std::trunc(rotation*std::trunc(scaled_distance*10)/(radius*10));
        const int degrees=static_cast<int>(std::fmod(twist,360.0));
        const auto turn=turns[static_cast<std::size_t>(std::abs(degrees))];
        const double sine=degrees<0?-turn[1]:turn[1];
        const double nx=distance==0?1.0:dx/distance,ny=distance==0?0.0:dy/distance;
        const auto sx=center_x+static_cast<std::int64_t>((nx*turn[0]-ny*sine)*magnitude+0.5);
        const auto sy=center_y+static_cast<std::int64_t>((ny*turn[0]+nx*sine)*magnitude+0.5);
        if (sx<0 || sy<0 || sx>=texture.canvas_width || sy>=texture.canvas_height) continue;
        const auto source=texel(texture,static_cast<int>(sx),static_cast<int>(sy));
        const int weight=source[3]*opacity/100;
        if (!weight) continue;
        const auto at=(static_cast<std::size_t>(y)*out.width+x)*3;
        for (std::size_t c=0;c<3;++c)
            out.rgb[at+c]=static_cast<Byte>(weight==255?source[c]:(source[c]*weight+out.rgb[at+c]*(256-weight))>>8);
    }
}
void slash_blur(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const auto& texture=*layer.texture;
    const auto dst=destination(layer);
    const int width=static_cast<int>(out.width),height=static_cast<int>(out.height);
    const auto origin_x=-static_cast<std::int64_t>(dst.x),origin_y=-static_cast<std::int64_t>(dst.y);
    if (origin_x<0 || origin_y<0 || origin_x+width>texture.canvas_width || origin_y+height>texture.canvas_height) return;
    const int type=value(layer,1);
    if (type<0 || type>3) return;
    const bool vertical=type<2;
    const int axis=vertical?height:width,lanes=vertical?width/2*2:height;
    const int percent=std::clamp(interpolate(layer,0,100,value(layer,2),value(layer,3)),0,100);
    const int span=std::clamp(percent*std::clamp(value(layer,4),1,255)/100,0,axis);
    if (span<=1) {
        normal_picture(out,texture,{0,0,texture.canvas_width,texture.canvas_height},
                       {dst.x,dst.y,texture.canvas_width,texture.canvas_height},false);
        return;
    }
    const int travel=percent*std::max(0,axis-std::max(span,value(layer,5)))/100;
    const int begin=type%2?0:travel,end=type%2?axis-travel-span:axis-span;
    const int reciprocal=std::min(32767,65536/span);
    const int x0=static_cast<int>(origin_x),y0=static_cast<int>(origin_y);
    for (int lane=0;lane<lanes;++lane) {
        const auto sample=[&](int position) {
            return vertical?texel(texture,x0+lane,y0+position):texel(texture,x0+position,y0+lane);
        };
        std::array<int,3> sum{};
        for (int i=0;i<span;++i) {
            const auto pixel=sample(begin+i);
            for (std::size_t c=0;c<3;++c) sum[c]+=pixel[c];
        }
        int current=begin;
        for (int step=0;step<axis;++step) {
            const int next=begin+(end-begin)*std::max(0,step-1)/axis;
            if (next>current) {
                const auto before=sample(current),after=sample(current+span);
                for (std::size_t c=0;c<3;++c) sum[c]+=after[c]-before[c];
                ++current;
            }
            const auto x=static_cast<unsigned>(vertical?lane:step),y=static_cast<unsigned>(vertical?step:lane);
            const auto at=(static_cast<std::size_t>(y)*out.width+x)*3;
            for (std::size_t c=0;c<3;++c) out.rgb[at+c]=static_cast<Byte>((sum[c]/2*reciprocal>>16)*2);
        }
    }
}
void kagerou(Surface& out,const Layer& layer) {
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int width=static_cast<int>(out.width),height=static_cast<int>(out.height);
    const int start=std::clamp(p(1),0,height-1),length=std::clamp(p(2),1,height-start);
    const double total=p(3)>0 && layer.count<static_cast<unsigned>(p(3))?
        static_cast<double>(layer.count)/p(3):1.0;
    const double mh=p(4)/100.0,mw=p(5)/100.0,ms=p(6)/100.0;
    const auto row=static_cast<std::size_t>(width)*3;
    for (int y=start;y<start+length;++y) {
        const double scale=3.14159*2/height;
        const double a=(y*(p(8)*mw)+p(9)*ms*layer.count)*scale;
        const double b=(y*(p(11)*mw)-p(12)*ms*layer.count)*scale;
        const double c=(y*(p(14)*mw)-p(15)*ms*layer.count)*scale;
        const double offset=total*(std::cos(a)*p(7)*mh+std::cos(b)*p(10)*mh+std::cos(c)*p(13)*mh)+0.5;
        const int d=static_cast<int>(std::clamp(offset,static_cast<double>(1-width),static_cast<double>(width-1)));
        const auto base=static_cast<std::size_t>(y)*row;
        if (d>=0) {
            for (int x=0;x<width-d;++x) for (std::size_t channel=0;channel<3;++channel) {
                const auto to=base+static_cast<std::size_t>(x)*3+channel;
                const auto from=base+static_cast<std::size_t>(x+d)*3+channel;
                out.rgb[to]=static_cast<Byte>(((out.rgb[to]*160)>>8)+((out.rgb[from]*96)>>8));
            }
        } else {
            for (int x=width-1;x>=-d;--x) for (std::size_t channel=0;channel<3;++channel) {
                const auto to=base+static_cast<std::size_t>(x)*3+channel;
                const auto from=base+static_cast<std::size_t>(x+d)*3+channel;
                out.rgb[to]=static_cast<Byte>(((out.rgb[to]*160)>>8)+((out.rgb[from]*96)>>8));
            }
        }
    }
}
void water(Surface& out,const Layer& layer) {
    const int width=static_cast<int>(out.width),height=static_cast<int>(out.height),start=value(layer,1);
    const int count=layer.effect_state->water_count,depth=std::max(1,height-start);
    for (int y=start;y<height;++y) {
        const int k=y-start,remaining=depth-k;
        const double theta=static_cast<double>(-static_cast<std::int64_t>(count)*3+
            static_cast<std::int64_t>(remaining)*remaining/depth*4)*(3.14159*2/100.0);
        int source=static_cast<int>(std::cos(theta)*8*(k+depth)/(depth*2)+0.5)+start-k-10;
        if (source>=start) source=start-1;
        source=std::max(0,source);
        const auto src=static_cast<std::size_t>(source)*out.width*3,dst=static_cast<std::size_t>(y)*out.width*3;
        if (src!=dst) std::copy_n(out.rgb.data()+src,static_cast<std::size_t>(width)*3,out.rgb.data()+dst);
    }
}
void zantetsu(Surface& out,const Layer& layer) {
    if (!layer.texture || !layer.texture->has_mask || out.width<3 || out.height<3) return;
    const auto& t=*layer.texture;const auto p=[&](std::size_t i){return value(layer,i);};
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    const int angle=std::clamp(p(1),-90,90),cx=std::clamp(p(3),1,w-2),cy=std::clamp(p(4),1,h-2);
    const double theta=static_cast<double>(angle)/360.0*(3.14159*2.0),vx=-std::sin(theta),vy=std::cos(theta);
    const double distance=static_cast<double>(interpolate(layer,0,10000,p(7),p(8)))*p(2)/10000.0;
    const auto offset=[&](double vector,double sign) {
        return static_cast<int>(std::clamp(vector*distance*sign+0.5,-1.0e9,1.0e9));
    };
    const int alpha=std::clamp(interpolate(layer,p(10),p(9),p(11),p(12)),0,100)*256/100;
    const auto dst=destination(layer);
    for (int side=0;side<2;++side) {
        const double sign=side==0?(p(6)==1 || p(6)==3?1.0:0.0):(p(6)==2?1.0:p(6)==3?-1.0:0.0);
        const int ox=offset(vx,sign),oy=offset(vy,sign);
        const auto left=static_cast<std::int64_t>(dst.x)+ox,top=static_cast<std::int64_t>(dst.y)+oy;
        const auto right=std::min<std::int64_t>(w,left+t.canvas_width),bottom=std::min<std::int64_t>(h,top+t.canvas_height);
        for (auto y=std::max<std::int64_t>(0,top);y<bottom;++y) {
            const auto sy=y-top;
            if (sy>=h) continue;
            int cut;
            if (angle==-90) cut=sy<cy?0:w;
            else if (angle==90) cut=sy<cy?w:0;
            else cut=std::clamp(clamp_int(static_cast<std::int64_t>(cx)+
                static_cast<int>(std::clamp(vx/vy*static_cast<double>(sy-cy)+0.5,-1.0e9,1.0e9))),0,w);
            for (auto x=std::max<std::int64_t>(0,left);x<right;++x) {
                const auto sx=x-left;
                if (side==0?sx>=cut:sx<cut) continue;
                const auto src=texel(t,clamp_int(sx),clamp_int(sy));
                if (!src[3]) continue;
                const int a=alpha==256?src[3]:(src[3]*alpha)>>8;
                const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<std::size_t>(x))*3;
                for (std::size_t c=0;c<3;++c) out.rgb[at+c]=static_cast<Byte>(
                    a==255?src[c]:((src[c]*a)>>8)+((out.rgb[at+c]*(256-a))>>8));
            }
        }
    }
}
void shards_init(Layer& layer,int width,int height,const FragmentMesh* mesh) {
    const auto p=[&](std::size_t i){return value(layer,i);};
    using Point=std::array<int,2>;
    std::vector<Point> vertices;
    std::vector<std::vector<int>> faces;
    const int shape=p(14),level=std::clamp(p(15),0,32);
    if (p(13)==2 && mesh) {
        vertices=mesh->vertices;faces=mesh->faces;
    } else if (p(13)==1 && shape>=0 && shape<=2) {
        const int rows=(level+1)*2,cols=rows*4/3,noise=std::clamp(p(16),0,4);
        const int block_x=width/cols,block_y=height/rows;
        const int rx=block_x*noise/10/(shape==2?2:1),ry=block_y*noise/10;
        const auto rnd=[&](int n) { return static_cast<int>(random_next(layer.effect_state->seed)%static_cast<unsigned>(std::max(1,n))); };
        if (shape<2) {
            const int stride=cols+(shape==0?2:1);
            for (int j=0;j<=rows;++j) for (int i=0;i<stride;++i) {
                int x=width*i/cols,y=height*j/rows;
                if (shape==0 && (j&1)) x-=block_x/2;
                if (noise>0) { x+=rnd(rx*2+1)-rx;y+=rnd(ry*2+1)-ry; }
                if (!j) y=0;
                if (j==rows) y=height-1;
                if (!i) x=0;
                if (shape==1 && i==cols) x=width-1;
                if (shape==0 && ((j&1)?i==cols+1:i>=cols)) x=width-1;
                vertices.push_back({x,y});
            }
            if (shape==0) {
                for (int j=0;j<rows;j+=2) {
                    const int up=stride*j,center=stride*(j+1),down=stride*(j+2);
                    for (int i=0;i<cols;++i) {
                        if (vertices[static_cast<std::size_t>(up+i)][1]<vertices[static_cast<std::size_t>(up+i+1)][1])
                            faces.push_back({up+i,center+i+1,up+i+1});
                        else faces.push_back({up+i+1,up+i,center+i+1});
                        faces.push_back({up+i,center+i,center+i+1});
                    }
                    faces.push_back({up+cols,center+cols,center+cols+1});
                    for (int i=0;i<cols;++i) {
                        if (vertices[static_cast<std::size_t>(center+i)][1]<vertices[static_cast<std::size_t>(center+i+1)][1])
                            faces.push_back({center+i,down+i,center+i+1});
                        else faces.push_back({center+i+1,center+i,down+i});
                        faces.push_back({center+i+1,down+i,down+i+1});
                    }
                    if (vertices[static_cast<std::size_t>(center+cols)][1]<vertices[static_cast<std::size_t>(center+cols+1)][1])
                        faces.push_back({center+cols,down+cols,center+cols+1});
                    else faces.push_back({center+cols+1,center+cols,down+cols});
                }
            } else {
                for (int j=0;j<rows;++j) for (int i=0;i<cols;++i) {
                    const int up=stride*j,down=stride*(j+1);
                    if (vertices[static_cast<std::size_t>(up+i)][1]<vertices[static_cast<std::size_t>(up+i+1)][1])
                        faces.push_back({up+i,down+i,down+i+1,up+i+1});
                    else faces.push_back({up+i+1,up+i,down+i,down+i+1});
                }
            }
        } else {
            int y_down=0;
            for (int j=0;j<rows;++j) {
                const int y_up=y_down;y_down=(j+1)*block_y;
                if (noise>0) y_down+=rnd(ry*2+1)-ry;
                if (j==rows-1) y_down=height-1;
                for (int i=0;i<=cols+1;++i) {
                    int x=i*block_x;if (j&1) x-=block_x/2;
                    if (noise>0) x+=rnd(rx*2+1)-rx;
                    if (!i) x=0;
                    if ((j&1)?i==cols+1:i>=cols) x=width-1;
                    vertices.push_back({x,y_up});vertices.push_back({x,y_down});
                }
            }
            for (int j=0;j<rows;++j) for (int i=0;i<cols+(j&1);++i) {
                const int left=(j*(cols+2)+i)*2,right=left+2;
                faces.push_back({left,left+1,right+1,right});
            }
        }
    } else {
        for (const auto& point:default_shard_vertices)
            vertices.push_back({std::min(width-1,point.x*width/640),std::min(height-1,point.y*height/480)});
        for (const auto& face:default_shard_faces) faces.emplace_back(face.begin()+1,face.begin()+1+face[0]);
    }
    auto& shards=layer.effect_state->shards;
    shards.reserve(faces.size());
    for (const auto& face:faces) {
        if (face.empty()) continue;
        const auto first=face.size()<vertices.size() ? face.size() : static_cast<std::size_t>(face.back());
        std::int64_t x=vertices[first][0],y=vertices[first][1];
        for (std::size_t i=0;i+1<face.size();++i) {
            x+=vertices[static_cast<std::size_t>(face[i])][0];
            y+=vertices[static_cast<std::size_t>(face[i])][1];
        }
        x/=static_cast<std::int64_t>(face.size());y/=static_cast<std::int64_t>(face.size());
        const double dx=static_cast<double>(x-p(3)),dy=static_cast<double>(y-p(4));
        const double radius=std::max(1.0,std::hypot(dx,dy));
        const double speed=p(1)*0.01*30.0*(1.0+radius/200.0),angle=std::atan2(dy,dx);
        Shard shard;
        shard.speed_x=static_cast<float>(std::cos(angle)*speed);
        shard.speed_y=static_cast<float>(std::sin(angle)*speed);
        shard.gravity=static_cast<float>(p(2))*0.01F;
        for (int i:face) shard.points.push_back(vertices[static_cast<std::size_t>(i)]);
        shards.push_back(std::move(shard));
    }
}
void shards(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const auto& texture=*layer.texture;const auto p=[&](std::size_t i){return value(layer,i);};
    const int width=static_cast<int>(out.width),height=static_cast<int>(out.height);
    const int time=interpolate(layer,0,static_cast<int>(duration(layer)),p(7),p(8));
    const int alpha=std::clamp(interpolate(layer,clamp_int(static_cast<std::int64_t>(p(10))*256),
        clamp_int(static_cast<std::int64_t>(p(9))*256),p(11),p(12))/100,0,256);
    for (const auto& shard:layer.effect_state->shards) {
        const int early=std::min(time,p(5));
        const double early_time=static_cast<double>(static_cast<std::int64_t>(early)*p(6))*0.001F;
        double dx=early_time*shard.speed_x,dy=early_time*shard.speed_y;
        const auto late=static_cast<std::int64_t>(time)-p(5);
        if (late>0) {
            const double t=static_cast<double>(late);
            dx+=shard.speed_x*t;dy+=shard.speed_y*t+0.5*shard.gravity*t*t;
        }
        if (dx< -width || dx>width*2.0 || dy< -height || dy>height*2.0) continue;
        const int ox=static_cast<int>(dx+0.5),oy=static_cast<int>(dy+0.5);
        auto points=shard.points;
        if (points.size()<3) continue;
        points.push_back(points[0]);
        const int n=static_cast<int>(points.size())-1;
        const auto at=[&](int index) -> std::array<int,2> { return points[static_cast<std::size_t>(std::clamp(index,0,n))]; };
        int min_y=points[0][1],max_y=min_y;
        for (const auto& point:points) { min_y=std::min(min_y,point[1]);max_y=std::max(max_y,point[1]); }
        const int size_y=max_y-min_y+1;
        int left=0,right=n;
        if (at(right)[1]==at(right-1)[1]) --right;
        int left_count=0,right_count=0;
        int left_span=at(left+1)[1]-at(left)[1],right_span=at(right-1)[1]-at(right)[1];
        for (int row=0;row<size_y;++row) {
            const auto y=static_cast<std::int64_t>(points[0][1])+oy+row,sy=static_cast<std::int64_t>(points[0][1])+row;
            if (y>=height) break;
            const float lf=static_cast<float>(at(left)[0])+
                (static_cast<float>(at(left+1)[0]-at(left)[0])*static_cast<float>(left_count))*
                (1.0F/static_cast<float>(std::max(1,left_span)))+0.5F;
            const float rf=static_cast<float>(at(right)[0])+
                (static_cast<float>(at(right-1)[0]-at(right)[0])*static_cast<float>(right_count))*
                (1.0F/static_cast<float>(std::max(1,right_span)))+0.5F;
            const int start=static_cast<int>(lf),end=static_cast<int>(rf);
            if (y>=0 && sy>=0 && sy<texture.canvas_height && end>start) {
                const auto from=std::max<std::int64_t>(0,static_cast<std::int64_t>(start)+ox);
                const auto to=std::min<std::int64_t>(width-1,static_cast<std::int64_t>(end)+ox);
                for (auto x=from;x<=to;++x) {
                    const auto sx=x-ox;
                    if (sx<0 || sx>=texture.canvas_width) continue;
                    const auto src=texel(texture,clamp_int(sx),clamp_int(sy));
                    const auto index=(static_cast<std::size_t>(y)*out.width+static_cast<std::size_t>(x))*3;
                    for (std::size_t c=0;c<3;++c)
                        out.rgb[index+c]=static_cast<Byte>(alpha==256?src[c]:
                            (src[c]*alpha+out.rgb[index+c]*(256-alpha))>>8);
                }
            }
            if (++left_count>=left_span) {
                ++left;left_count=0;left_span=at(left+1)[1]-at(left)[1];
            }
            if (++right_count>=right_span) {
                --right;right_count=0;right_span=at(right-1)[1]-at(right)[1];
            }
            if (row==size_y-2 && (!left_span || !right_span)) {
                --left;++right;
                left_span=at(left+1)[1]-at(left)[1];right_span=at(right-1)[1]-at(right)[1];
                left_count=left_span;right_count=right_span;
            }
        }
    }
}
void perspective(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const auto& t=*layer.texture;const auto p=[&](std::size_t i){return value(layer,i);};
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    if (t.canvas_width<1 || t.canvas_height<1) return;
    const auto dv=std::max(1U,effects::duration(layer));
    const auto lerp=[&](std::size_t first,std::size_t last) {
        return clamp_int(static_cast<std::int64_t>(p(first))+
            (static_cast<std::int64_t>(p(last))-p(first))*std::min(layer.count,dv)/dv);
    };
    const int dist1=lerp(6,1),dist2=lerp(7,2);
    int height=lerp(8,3),percent=lerp(9,4);
    const int top=std::max(0,lerp(12,10)),bottom=std::min(h-1,lerp(13,11));
    if (top>bottom) return;
    if (!height) height=1;
    percent=std::max(1,percent);
    const double th1=std::atan2(static_cast<double>(dist1),static_cast<double>(height));
    const double th2=std::atan2(static_cast<double>(dist2),static_cast<double>(height));
    const double radius=std::hypot(static_cast<double>(dist1),static_cast<double>(height));
    const double scale=100.0/(radius*percent);
    const int rows=bottom-top+1;
    const auto dst=destination(layer);
    const auto addx=-static_cast<std::int64_t>(dst.x)+t.canvas_width/2;
    const auto addy=-static_cast<std::int64_t>(dst.y);
    double yy=0.0;
    const bool masked=p(5)==2 && t.has_mask;
    for (int i=0;i<rows;++i) {
        const double angle=th1+(th2-th1)*i/rows;
        const double dot=height/std::cos(angle)*scale;
        if (!std::isfinite(dot) || dot<0 || !std::isfinite(yy) || yy>1.0e10) return;
        const auto sy=static_cast<std::int64_t>(yy+0.5)+addy;
        const auto step=static_cast<std::int64_t>(std::clamp(dot*65536.0,0.0,1.0e12));
        std::int64_t xfixed=addx*65536-step*(w/2);
        if (sy>=0 && sy<t.canvas_height) for (int x=0;x<w;++x,xfixed+=step) {
            const auto sx=xfixed>>16;
            if (sx<0 || sx>=t.canvas_width) continue;
            const auto src=texel(t,clamp_int(sx),clamp_int(sy));
            const auto at=(static_cast<std::size_t>(top+i)*out.width+static_cast<unsigned>(x))*3;
            for (std::size_t c=0;c<3;++c) out.rgb[at+c]=static_cast<Byte>(masked?
                (src[c]*src[3]+out.rgb[at+c]*(255-src[3]))>>8:src[c]);
        }
        yy+=dot;
    }
}
void mask_wipe(Surface& out,const std::array<Layer,32>& layers,unsigned slot,const Surface& saved) {
    const auto& l=layers[slot];const auto p=[&](std::size_t i){return value(l,i);};
    const int end_slot=p(1),mask_slot=p(2),w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    if (mask_slot<0 || mask_slot>=static_cast<int>(layers.size())) return;
    const auto& mask_layer=layers[static_cast<unsigned>(mask_slot)];
    if (!mask_layer.texture) return;
    const auto& mask=*mask_layer.texture;
    if (mask.canvas_width<w || mask.canvas_height<h) return;
    const Texture* start=p(6)?nullptr:l.texture.get();
    const Texture* end=nullptr;
    if (!p(7)) {
        if (end_slot<0 || end_slot>=static_cast<int>(layers.size())) return;
        end=layers[static_cast<unsigned>(end_slot)].texture.get();
    }
    if ((start && (start->canvas_width<w || start->canvas_height<h)) ||
        (end && (end->canvas_width<w || end->canvas_height<h))) return;
    if ((!p(6) && !start) || (!p(7) && !end)) return;
    if ((p(6) || p(7)) && (saved.width!=out.width || saved.height!=out.height ||
                            saved.rgb.size()!=out.rgb.size())) return;
    const auto mask_origin=window_origin(out,mask_layer,mask);
    const auto start_origin=start?window_origin(out,l,*start):Point{};
    const auto end_origin=end?window_origin(out,layers[static_cast<unsigned>(end_slot)],*end):Point{};
    const int progress=interpolate(l,p(9),p(8),p(4),p(5));
    const auto multi=std::clamp<std::int64_t>(p(3),0,127),minus=256*multi,wide=256+minus;
    const auto position=std::clamp(static_cast<std::int64_t>(progress)*wide/100,std::int64_t{0},wide)-minus;
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        const auto m=texel(mask,mask_origin.x+x,mask_origin.y+y);
        const auto a=start?texel(*start,start_origin.x+x,start_origin.y+y):std::array<int,4>{};
        const auto b=end?texel(*end,end_origin.x+x,end_origin.y+y):std::array<int,4>{};
        const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        for (std::size_t c=0;c<3;++c) {
            const auto weight=std::clamp(m[c]*multi+position,std::int64_t{0},std::int64_t{255});
            const auto first=start?a[c]:saved.rgb[at+c],last=end?b[c]:saved.rgb[at+c];
            out.rgb[at+c]=static_cast<Byte>((first*(256-weight)+last*weight)>>8);
        }
    }
}
void moving_mask_wipe(Surface& out,const std::array<Layer,32>& layers,unsigned slot,const Surface& saved) {
    const auto& layer=layers[slot];
    const auto p=[&](std::size_t i){return value(layer,i);};
    const auto scale=[](int value) { return clamp_int(static_cast<std::int64_t>(value)*512/100); };
    const int phase=interpolate(layer,scale(p(9)),scale(p(8)),p(3),p(4));
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    const Rect canvas{0,0,w,h};
    const bool has_saved=saved.width==out.width && saved.height==out.height && saved.rgb.size()==out.rgb.size();
    const auto from_index=[&](int index)->const Layer* {
        return index>=0 && static_cast<std::size_t>(index)<layers.size()?&layers[static_cast<std::size_t>(index)]:nullptr;
    };
    const auto picture=[&](const Layer* item)->const Texture* {
        return item && item->texture && item->texture->canvas_width>=w && item->texture->canvas_height>=h?
            item->texture.get():nullptr;
    };
    const bool get_first=p(5)!=0,get_last=p(6)!=0;
    const auto* last_layer=from_index(p(1));
    const auto* first=get_first?nullptr:picture(&layer);
    if (get_first?!has_saved:first==nullptr) return;
    if (!phase) {
        if (first) paint(out,*first,canvas,canvas,100,{},false);
        else out=saved;
        return;
    }
    const auto* last=get_last?nullptr:picture(last_layer);
    if (get_last?!has_saved:last==nullptr) return;
    if (phase>=512) {
        if (last) paint(out,*last,canvas,canvas,100,{},false);
        else out=saved;
        return;
    }
    const auto* mask_layer=from_index(p(2));
    const auto* mask=picture(mask_layer);
    if (!mask) return;
    const auto mask_at=window_origin(out,*mask_layer,*mask);
    const auto first_at=first?window_origin(out,layer,*first):Point{};
    const auto last_at=last?window_origin(out,*last_layer,*last):Point{};
    const int intensity=std::clamp(phase>256?512-phase:phase,0,256);
    const int weight=p(7)?(phase<256?0:128):std::clamp(phase/4,0,128);
    const auto offset=[&](int channel) {
        const int product=(channel-128)*intensity;
        return (product-(product<0?255:0))/256;
    };
    const auto sample=[&](const Texture* texture,Point origin,int x,int y) {
        if (texture) return texel(*texture,x+origin.x,y+origin.y);
        const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        return std::array<int,4>{saved.rgb[at],saved.rgb[at+1],saved.rgb[at+2],0};
    };
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        const auto vector=texel(*mask,x+mask_at.x,y+mask_at.y);
        const int sx=std::clamp(x+offset(vector[2]),0,w-1),sy=std::clamp(y+offset(vector[1]),0,h-1);
        const auto a=sample(first,first_at,sx,sy),b=sample(last,last_at,sx,sy);
        const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        for (std::size_t c=0;c<3;++c)
            out.rgb[at+c]=static_cast<Byte>(((a[c]*(128-weight))>>7)+((b[c]*weight)>>7));
    }
}
void dual_mask(Surface& out,const std::array<Layer,32>& layers,unsigned slot) {
    const auto& layer=layers[slot];
    const auto p=[&](std::size_t i){return value(layer,i);};
    const bool dappled=layer.effect==78;
    const int mask_slot=p(1);
    if (!layer.texture || !layer.texture->has_mask || mask_slot<0 ||
        static_cast<std::size_t>(mask_slot)>=layers.size()) return;
    const auto& mask=layers[static_cast<std::size_t>(mask_slot)];
    if (!mask.texture || !mask.texture->has_mask) return;
    const int origin=p(8);
    const auto first=destination(dappled && origin>=0 && static_cast<std::size_t>(origin)<layers.size()?
        layers[static_cast<std::size_t>(origin)]:layer);
    const auto second=destination(mask);
    const int left=std::max({0,first.x,second.x}),top=std::max({0,first.y,second.y});
    const auto right=std::min<std::int64_t>({out.width,static_cast<std::int64_t>(first.x)+layer.texture->canvas_width,
                                           static_cast<std::int64_t>(second.x)+mask.texture->canvas_width});
    const auto bottom=std::min<std::int64_t>({out.height,static_cast<std::int64_t>(first.y)+layer.texture->canvas_height,
                                            static_cast<std::int64_t>(second.y)+mask.texture->canvas_height});
    const auto scale=[](int value) { return clamp_int(static_cast<std::int64_t>(value)*100); };
    const int opacity=static_cast<int>(std::clamp(static_cast<std::int64_t>(
        interpolate(layer,scale(p(3)),scale(p(2)),p(4),p(5)))*256/10000,std::int64_t{0},std::int64_t{256}));
    const bool fade=p(6)!=0;
    const int strength=fade?std::clamp(interpolate(layer,0,255,p(7),p(8)),0,255):256;
    std::array<int,3> tint{};
    if (dappled) {
        for (std::size_t c=0;c<3;++c) tint[c]=std::clamp(interpolate(layer,p(5+c),p(2+c),p(9),p(10)),-255,255);
    } else if (fade) {
        for (std::size_t c=0;c<3;++c) tint[c]=(255-strength)*std::clamp(p(9+c),0,255)/255;
    }
    for (int y=top;y<bottom;++y) for (int x=left;x<right;++x) {
        const auto source=texel(*layer.texture,x-first.x,y-first.y);
        const auto matte=texel(*mask.texture,x-second.x,y-second.y);
        if (!source[3] || !matte[3]) continue;
        const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        if (dappled) {
            const int a=source[3]==255?256:source[3],b=matte[3]==255?256:matte[3];
            const int alpha=a*b>>8;
            for (std::size_t c=0;c<3;++c)
                out.rgb[at+c]=static_cast<Byte>(std::clamp(out.rgb[at+c]+tint[c]*alpha/256,0,255));
            continue;
        }
        const int alpha=(((matte[3]+1)*source[3])>>8)*opacity>>8;
        for (std::size_t c=0;c<3;++c) {
            const int pixel=((source[c]*strength)>>8)+tint[c];
            out.rgb[at+c]=static_cast<Byte>((pixel*alpha+out.rgb[at+c]*(256-alpha))>>8);
        }
    }
}
void ripple(Surface& out,const std::array<Layer,32>& layers,unsigned slot,const Surface& saved) {
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    if (w<32 || h<32) return;
    const auto& layer=layers[slot];
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int next=p(1);
    const auto& after=layers[static_cast<std::size_t>(std::clamp(next,0,static_cast<int>(layers.size())-1))];
    const auto* first=p(15)?nullptr:layer.texture.get();
    const auto* last=next==-1?first:p(16)?nullptr:after.texture.get();
    const bool first_saved=p(15)!=0,last_saved=next==-1?first_saved:p(16)!=0;
    const bool valid_saved=saved.width==out.width && saved.height==out.height && saved.rgb.size()==out.rgb.size();
    if ((first_saved?!valid_saved:!first) || (last_saved?!valid_saved:!last)) return;
    const auto start=destination(layer),end=next==-1?start:destination(after);
    const int length=std::max(1,static_cast<int>(duration(layer)));
    const int tick=interpolate(layer,0,p(0),p(13),p(14));
    const auto hold=static_cast<std::int64_t>(p(11));
    const auto time=std::max<std::int64_t>(1,length-hold);
    const auto elapsed=std::clamp(static_cast<std::int64_t>(tick)-hold,std::int64_t{0},time);
    const int last_weight=static_cast<int>((elapsed*100/time)*256/100);
    const int first_weight=static_cast<int>(((time-elapsed)*100/time)*256/100);
    const double strength=std::max(0.0,p(4)-static_cast<double>(tick)*std::clamp(p(8),0,100)*0.01);
    const auto range=static_cast<std::int64_t>(p(12))+10;
    const int wavelength=std::max(1,p(5));
    const auto front=static_cast<std::int64_t>(tick)*std::max(1,p(6));
    const auto rotation=-static_cast<std::int64_t>(tick)*p(7)%wavelength;
    const int max_distance=static_cast<int>(std::hypot(w,h));
    std::vector<int> amplitudes(static_cast<std::size_t>(max_distance)+1);
    for (int i=1;i<=max_distance;++i) {
        const auto depth=front-i;
        if (depth<0 || depth>=range) continue;
        const double envelope=depth<10?static_cast<double>(depth)/10:
            static_cast<double>(range-depth)/static_cast<double>(range-10);
        const double angle=static_cast<double>((rotation+i)%wavelength)*6.28318/wavelength;
        const auto sample=static_cast<std::int64_t>(std::cos(angle)*envelope*strength+0.5);
        const int wrapped=static_cast<int>((sample*256)&65535);
        amplitudes[static_cast<std::size_t>(i)]=wrapped>=32768?wrapped-65536:wrapped;
    }
    const auto sample=[&](const Texture* texture,std::int64_t x,std::int64_t y) {
        if (texture) {
            if (x<0 || y<0 || x>=texture->canvas_width || y>=texture->canvas_height) return std::array<int,4>{};
            return texel(*texture,static_cast<int>(x),static_cast<int>(y));
        }
        if (x<0 || y<0 || x>=w || y>=h) return std::array<int,4>{};
        const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<std::size_t>(x))*3;
        return std::array<int,4>{saved.rgb[at],saved.rgb[at+1],saved.rgb[at+2],0};
    };
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        const double dx=static_cast<double>(x)-p(2),dy=static_cast<double>(y)-p(3);
        const double distance=std::max(1.0,std::sqrt(dx*dx+dy*dy));
        const auto bucket=static_cast<std::size_t>(std::min(distance+0.5,static_cast<double>(max_distance)));
        const int strength_at=amplitudes[bucket];
        int tx=x,ty=y;
        if (strength_at) {
            // The third and fourth vector lanes share the second lane's X numerator.
            const int lane=x%4;
            const double normal_x=dx-(lane>1?lane-1:0);
            const int nx=static_cast<int>(normal_x*256/distance+0.5),ny=static_cast<int>(dy*256/distance+0.5);
            tx+=static_cast<int>(std::floor(nx*strength_at/65536.0));
            ty+=static_cast<int>(std::floor(ny*strength_at/65536.0));
        }
        const auto a=sample(first,static_cast<std::int64_t>(tx)-start.x,static_cast<std::int64_t>(ty)-start.y);
        const auto b=sample(last,static_cast<std::int64_t>(tx)-end.x,static_cast<std::int64_t>(ty)-end.y);
        const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        for (std::size_t c=0;c<3;++c)
            out.rgb[at+c]=static_cast<Byte>((a[c]*first_weight+b[c]*last_weight)>>8);
    }
}
void rainbow(Surface& out,const Layer& layer) {
    if (!layer.texture || !layer.texture->has_mask) return;
    constexpr std::array<std::array<int,3>,7> colors{{
        {255,0,0},{255,255,0},{0,255,0},{0,255,255},{0,0,255},{255,0,255},{255,0,0}}};
    const int weight=std::clamp(fixed_percent(layer,value(layer,2),value(layer,1),value(layer,3),value(layer,4)),0,256);
    const int h=static_cast<int>(out.height),segment=(h+5)/6;
    const auto& texture=*layer.texture;
    const auto dst=destination(layer);
    const auto bottom=std::min<std::int64_t>(h,static_cast<std::int64_t>(dst.y)+texture.canvas_height);
    for (int y=std::max(0,dst.y);y<bottom;++y) {
        const int phase=(y+h-(layer.effect_state->offset_y%h))%h;
        const auto index=static_cast<std::size_t>(phase/segment);
        std::array<int,3> tint{};
        for (std::size_t c=0;c<3;++c) tint[c]=(colors[index][c]+
            (colors[index+1][c]-colors[index][c])*(phase%segment)/segment)*weight>>8;
        const int w=texture.canvas_width;
        picture_pixels(out,texture,{0,y-dst.y,w,1},{dst.x,y,w,1},[&](Byte* pixel,const Byte* sample) {
            const int a=sample[3];
            if (!a) return;
            for (std::size_t c=0;c<3;++c) {
                const int shade=tint[c]+((sample[c]*(256-weight))>>8);
                pixel[c]=static_cast<Byte>(a==255?shade:((shade*a)>>8)+((pixel[c]*(256-a))>>8));
            }
        });
    }
}
void mask_appear(Surface& out,const std::array<Layer,32>& layers,unsigned slot) {
    const auto& l=layers[slot];
    if (!l.texture || !l.texture->has_mask || value(l,1)<0 || value(l,1)>=static_cast<int>(layers.size())) return;
    const auto& mask_layer=layers[static_cast<unsigned>(value(l,1))];
    if (!mask_layer.texture) return;
    const auto& t=*l.texture;const auto& mask=*mask_layer.texture;
    if (mask.canvas_width<static_cast<int>(out.width) || mask.canvas_height<static_cast<int>(out.height)) return;
    const int progress=interpolate(l,value(l,7),value(l,6),value(l,3),value(l,4));
    const auto multi=std::clamp<std::int64_t>(value(l,2),0,127);
    const auto minus=256*multi,wide=256+minus;
    const auto position=std::clamp(static_cast<std::int64_t>(progress)*wide/100,std::int64_t{0},wide)-minus;
    const auto dst=destination(l);
    const auto right=std::min<std::int64_t>(out.width,static_cast<std::int64_t>(dst.x)+t.canvas_width);
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(dst.y)+t.canvas_height);
    for (int y=std::max(0,dst.y);y<bottom;++y) for (int x=std::max(0,dst.x);x<right;++x) {
        const auto src=texel(t,x-dst.x,y-dst.y),m=texel(mask,x,y);
        if (!src[3]) continue;
        const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        for (std::size_t c=0;c<3;++c) {
            const auto channel=value(l,5)?255-m[c]:m[c];
            const auto weight=std::clamp(channel*multi+position,std::int64_t{0},std::int64_t{255});
            const int a=static_cast<int>((weight*src[3])>>8);
            out.rgb[at+c]=static_cast<Byte>((src[c]*a+out.rgb[at+c]*(256-a))>>8);
        }
    }
}
void zoom_outline(Surface& out,int cx,int cy,int vx,int vy,bool square,int line_size,
                  const std::array<int,3>& color,int percent) {
    line_size=std::clamp(line_size,0,static_cast<int>(std::max(out.width,out.height))*2);
    if (!line_size) return;
    if (!square) {
        if (std::abs(static_cast<std::int64_t>(vx))>std::abs(static_cast<std::int64_t>(vy))) {
            const float f=static_cast<float>(vy)/static_cast<float>(vx);
            const float x0=static_cast<float>(-line_size),x1=static_cast<float>(out.width)+static_cast<float>(line_size);
            const float y0=(static_cast<float>(cx)+static_cast<float>(line_size))*-f+static_cast<float>(cy);
            const float y1=f*(static_cast<float>(out.width)+static_cast<float>(line_size)-static_cast<float>(cx))+static_cast<float>(cy);
            for (int i=0;i<line_size;++i) {
                const float offset=static_cast<float>(i)-static_cast<float>(line_size/2);
                line(out,x0,y0+offset,x1,y1+offset,color,percent);
            }
        } else {
            const float f=static_cast<float>(vx)/static_cast<float>(vy);
            const float y0=static_cast<float>(-line_size),y1=static_cast<float>(out.height)+static_cast<float>(line_size);
            const float x0=(static_cast<float>(cy)+static_cast<float>(line_size))*-f+static_cast<float>(cx);
            const float x1=f*(static_cast<float>(out.height)+static_cast<float>(line_size)-static_cast<float>(cy))+static_cast<float>(cx);
            for (int i=0;i<line_size;++i) {
                const float offset=static_cast<float>(i)-static_cast<float>(line_size/2);
                line(out,x0+offset,y0,x1+offset,y1,color,percent);
            }
        }
    } else {
        const auto x0=std::min<std::int64_t>(cx,static_cast<std::int64_t>(cx)+vx);
        const auto x1=std::max<std::int64_t>(cx,static_cast<std::int64_t>(cx)+vx);
        const auto y0=std::min<std::int64_t>(cy,static_cast<std::int64_t>(cy)+vy);
        const auto y1=std::max<std::int64_t>(cy,static_cast<std::int64_t>(cy)+vy);
        const int left=clamp_int(x0-line_size/2),top=clamp_int(y0-line_size/2);
        const int span=clamp_int(x1-x0+line_size);
        box(out,left,top,span,line_size,color,percent);
        const int lower=std::max(clamp_int(y1-line_size/2),clamp_int(static_cast<std::int64_t>(top)+line_size));
        const int lower_height=clamp_int(std::max<std::int64_t>(0,y1-line_size/2+line_size-lower));
        box(out,left,lower,span,lower_height,color,percent);
        const int gap=clamp_int(static_cast<std::int64_t>(lower)-top-line_size);
        if (gap>0) {
            box(out,left,clamp_int(static_cast<std::int64_t>(top)+line_size),line_size,gap,color,percent);
            int right=clamp_int(x1-line_size/2),right_width=line_size;
            if (static_cast<std::int64_t>(right)<static_cast<std::int64_t>(left)+right_width) {
                right_width-=span;right=clamp_int(static_cast<std::int64_t>(left)+span);
            }
            if (right_width>0) box(out,right,clamp_int(static_cast<std::int64_t>(top)+line_size),right_width,gap,color,percent);
        }
    }
}
void zoom_in(Surface& out,const Layer& layer,bool clipped) {
    if (!layer.texture) return;
    const auto& t=*layer.texture;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    if (t.canvas_width<w || t.canvas_height<h || (!clipped && (t.canvas_width!=w || t.canvas_height!=h))) return;
    const int percent=interpolate(layer,clamp_int(static_cast<std::int64_t>(p(8))*100),
        clamp_int(static_cast<std::int64_t>(p(7))*100),p(5),p(6));
    const auto crop=[&](std::int64_t delta,int base) {
        return clamp_int(static_cast<std::int64_t>(base)+(delta*percent+5000)/10000);
    };
    const int x0=std::max(0,crop(p(1),0)),y0=std::max(0,crop(p(2),0));
    const int x1=std::min(w,crop(static_cast<std::int64_t>(p(1))+p(3)-1-w,w));
    const int y1=std::min(h,crop(static_cast<std::int64_t>(p(2))+p(4)-1-h,h));
    if (x1<=x0 || y1<=y0) return;
    const auto dx=static_cast<std::int64_t>(x1-x0)*65536/w,dy=static_cast<std::int64_t>(y1-y0)*65536/h;
    int cx=0,cy=0,vx=1,vy=0,square=0;
    if (clipped) {
        const int position=interpolate(layer,0,10000,p(18),p(19));
        const auto vector=[&](std::size_t end,std::size_t start) {
            return clamp_int(static_cast<std::int64_t>(p(start))+
                (static_cast<std::int64_t>(p(end))-p(start))*position/10000);
        };
        cx=vector(9,14);cy=vector(10,15);vx=vector(11,16);vy=vector(12,17);square=p(13);
        if (!vx && !vy) vx=1;
        if (square && (!vx || !vy)) return;
        if (square==1) {cx=p(9);cy=p(10);vx=p(11);vy=p(12);}
    }
    for (int y=0;y<h;++y) for (int x=0;x<w;++x) {
        bool visible=true;
        if (clipped) {
            if (square) {
                const auto end_x=static_cast<std::int64_t>(cx)+vx,end_y=static_cast<std::int64_t>(cy)+vy+(vy>0?-1:1);
                visible=x>=std::min<std::int64_t>(cx,end_x) && x<std::max<std::int64_t>(cx,end_x) &&
                    y>=std::min<std::int64_t>(cy,end_y) && y<=std::max<std::int64_t>(cy,end_y);
            } else if (!vy) visible=vx>0?y>=cy:y<=cy;
            else if (!vx) visible=vy<0?x>=cx:x<=cx;
            else {
                const float f=static_cast<float>(vx)/static_cast<float>(vy);
                const double raw=static_cast<double>(static_cast<float>(static_cast<std::int64_t>(y)-cy)*f+(vy>=0?-1.0F:0.0F)+0.5F);
                const auto limit=static_cast<std::int64_t>(cx)+static_cast<int>(std::clamp(raw,-1.0e9,1.0e9));
                visible=vy<0?x>=limit:x<=limit;
            }
        }
        if (!visible) continue;
        const auto src=texel(t,x0+static_cast<int>((x*dx)>>16),y0+static_cast<int>((y*dy)>>16));
        const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
        for (std::size_t c=0;c<3;++c) out.rgb[at+c]=static_cast<Byte>(src[c]);
    }
    if (clipped && p(20)>0) {
        std::array<int,3> color{};
        for (std::size_t i=0;i<3;++i) color[i]=interpolate(layer,p(28+i),p(22+i),p(31),p(32));
        zoom_outline(out,cx,cy,vx,vy,square!=0,p(20),color,interpolate(layer,p(25),p(21),p(26),p(27)));
    }
}
void chara_ghost(Surface& out,const Layer& layer) {
    if (!layer.texture || !layer.texture->has_mask) return;
    const auto& t=*layer.texture;const auto dst=destination(layer);
    if (t.canvas_width<=0 || t.canvas_height<=0) return;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int up=interpolate(layer,clamp_int(static_cast<std::int64_t>(p(1))*256),
        clamp_int(static_cast<std::int64_t>(p(2))*256),p(5),p(6))/100;
    const int down=interpolate(layer,clamp_int(static_cast<std::int64_t>(p(3))*256),
        clamp_int(static_cast<std::int64_t>(p(4))*256),p(5),p(6))/100;
    const auto right=std::min<std::int64_t>(out.width,static_cast<std::int64_t>(dst.x)+t.canvas_width);
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(dst.y)+t.canvas_height);
    for (int y=std::max(0,dst.y);y<bottom;++y) {
        const auto yy=static_cast<std::int64_t>(y)+std::max<std::int64_t>(0,-static_cast<std::int64_t>(dst.y));
        const int opacity=static_cast<int>(std::clamp(static_cast<std::int64_t>(up)+
            (static_cast<std::int64_t>(down)-up)*yy/t.canvas_height,std::int64_t{0},std::int64_t{256}));
        for (int x=std::max(0,dst.x);x<right;++x) {
            const auto src=texel(t,x-dst.x,y-dst.y);
            const int alpha=(src[3]*opacity)>>8;
            const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
            for (std::size_t c=0;c<3;++c) out.rgb[at+c]=static_cast<Byte>(
                src[3]==255 && opacity==256?src[c]:
                ((src[c]*alpha)>>8)+((out.rgb[at+c]*(256-alpha))>>8));
        }
    }
}
void old_firefly_spawn(Layer& layer,OldFirefly& fly,bool defaults=false) {
    auto& state=*layer.effect_state;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const auto rnd=[&](std::int64_t n) {
        return static_cast<std::int64_t>(random_next(state.seed))%
            std::clamp(n,std::int64_t{1},std::int64_t{2147483647});
    };
    const int x=defaults?600:p(2),y=defaults?300:p(3),w=defaults?100:p(4),h=defaults?100:p(5);
    fly.x=(static_cast<std::int64_t>(x)+rnd(w))*256;
    fly.y=(static_cast<std::int64_t>(y)+rnd(h))*256;
    const auto speed=[&](int v) {
        const auto base=static_cast<std::int64_t>(v)*256,delta=std::max<std::int64_t>(1,std::abs(base/2));
        return base+rnd(delta)-delta/2;
    };
    fly.speed_x=speed(defaults?-3:p(6));fly.speed_y=speed(defaults?-2:p(7));
    const int wobble=defaults?15:p(15);
    fly.period=clamp_int(rnd(static_cast<std::int64_t>(wobble)*4)+wobble);
    fly.phase=clamp_int(rnd(fly.period));
}
void old_firefly_tick(Layer& layer,int width,int height) {
    auto& state=*layer.effect_state;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int start=std::clamp(p(12),0,600),end=std::min(600,start+p(1));
    const double angle=p(16)*(3.14159*2/360.0);
    for (int i=start;i<end;++i) {
        auto& fly=state.old_fireflies[static_cast<std::size_t>(i)];
        if (!fly.period) { if (p(11)) old_firefly_spawn(layer,fly);continue; }
        fly.x+=fly.speed_x;fly.y+=fly.speed_y;
        fly.phase=(fly.phase+1)%fly.period;
        const double wave=std::cos(static_cast<double>(fly.phase)*3.14159*2/fly.period)*p(17)*256;
        if (p(16)==-1) {
            const auto delta=static_cast<std::int64_t>(std::clamp(wave*0.2+0.5,-1.0e12,1.0e12));
            if (std::abs(fly.speed_x)>std::abs(fly.speed_y)) fly.y+=delta;
            else fly.x+=delta;
        } else {
            fly.x+=static_cast<std::int64_t>(std::clamp(wave*0.2*std::cos(angle)+0.5,-1.0e12,1.0e12));
            fly.y+=static_cast<std::int64_t>(std::clamp(wave*0.2*std::sin(angle)+0.5,-1.0e12,1.0e12));
        }
        const auto x=fly.x/256,y=fly.y/256;
        if ((fly.speed_x<0?x<4:x>width-4) || (fly.speed_y<0?y<4:y>height-4)) fly.period=0;
    }
}
void old_fireflies(Surface& out,const Layer& layer) {
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int start=std::clamp(p(12),0,600),end=std::min(600,start+p(1));
    const auto duration=std::max(1U,effects::duration(layer));
    const auto percent=std::clamp<std::int64_t>(100+(static_cast<std::int64_t>(p(14))-100)*layer.count/duration,0,100);
    for (int i=start;i<end;++i) {
        const auto& fly=layer.effect_state->old_fireflies[static_cast<std::size_t>(i)];
        if (!fly.period) continue;
        const auto x=fly.x>>8,y=fly.y>>8;
        if (x<4 || y<4 || x>=static_cast<std::int64_t>(out.width)-4 || y>=static_cast<std::int64_t>(out.height)-4) continue;
        for (int yy=0;yy<8;++yy) for (int xx=0;xx<8;++xx) {
            const int dx=(xx-4)*2+1,dy=(yy-4)*2+1;
            const double radius=std::sqrt(static_cast<double>(dx*dx+dy*dy));
            const int attenuation=static_cast<int>(radius*radius*2);
            const auto at=(static_cast<std::size_t>(y-4+yy)*out.width+static_cast<std::size_t>(x-4+xx))*3;
            for (std::size_t c=0;c<3;++c) {
                const auto source=static_cast<std::int64_t>(p(8+c));
                const int amount=static_cast<int>(std::clamp((std::abs(source)-attenuation)*percent/100,std::int64_t{0},std::int64_t{255}));
                out.rgb[at+c]=static_cast<Byte>(std::clamp(static_cast<int>(out.rgb[at+c])+(source<0?-amount:amount),0,255));
            }
        }
    }
}
void rain_spawn(Layer& layer,RainDrop& drop,int width,int height,bool initial) {
    auto& state=*layer.effect_state;
    const auto rnd=[&](std::int64_t n) {
        return static_cast<std::int64_t>(random_next(state.seed))%
            std::clamp(n,std::int64_t{1},std::int64_t{2147483647});
    };
    const auto direction=static_cast<std::int64_t>(value(layer,1))+rnd(16)-8;
    const auto spread=std::abs(direction*height/256);
    drop.x=(rnd(static_cast<std::int64_t>(width)+spread*2)-spread)*256;
    drop.y=-rnd(value(layer,3))*256;
    drop.speed=clamp_int(value(layer,3)+rnd(std::max(1,value(layer,3)/2)));
    drop.length=clamp_int(std::max(1,value(layer,4)/2)+rnd(value(layer,4)));
    drop.direction=direction*256;
    if (initial) drop.y=(rnd(static_cast<std::int64_t>(height)+value(layer,4))-value(layer,4))*256;
}
void rain_tick(Layer& layer,int width,int height) {
    for (auto& drop:layer.effect_state->drops) {
        drop.x+=drop.direction*drop.speed/256;
        drop.y+=static_cast<std::int64_t>(drop.speed)*256;
        if (drop.y>=static_cast<std::int64_t>(height)*256) rain_spawn(layer,drop,width,height,false);
    }
}
void rain(Surface& out,const Layer& layer) {
    const std::array<int,3> color{std::clamp(value(layer,5),0,255),std::clamp(value(layer,6),0,255),
        std::clamp(value(layer,7),0,255)};
    for (const auto& drop:layer.effect_state->drops) {
        const auto x=(drop.x+128)>>8,y=drop.y/256;
        if (y>=out.height || y+drop.length<0) continue;
        const auto dx=(drop.direction*drop.length)>>16;
        const auto dy=static_cast<std::int64_t>(drop.length);
        const auto length=std::max<std::int64_t>(1,std::max(std::abs(dx),dy));
        const auto steps=std::min<std::int64_t>(length,1000000);
        for (std::int64_t i=0;i<=steps;++i) {
            const auto xx=x+(dx*i+length/2)/length,yy=y+(dy*i+length/2)/length;
            if (xx<0 || yy<0 || xx>=out.width || yy>=out.height) continue;
            const auto at=(static_cast<std::size_t>(yy)*out.width+static_cast<std::size_t>(xx))*3;
            for (std::size_t c=0;c<3;++c) out.rgb[at+c]=static_cast<Byte>(color[c]);
        }
    }
}
constexpr std::array<int,6> snow_size{64,48,32,24,16,8};
constexpr std::array<int,6> snow_row{0,64,112,144,168,184};
constexpr std::array<int,3> flower_size{32,16,8};
constexpr std::array<int,3> flower_row{0,32,48};
int falling_size(const Layer& layer,const Particle& particle) {
    if (layer.effect==12)
        return flower_size[static_cast<std::size_t>(std::clamp(particle.row,0,2))];
    return snow_size[static_cast<std::size_t>(std::clamp(particle.row,0,5))];
}
void falling_spawn(Layer& layer,Particle& particle,int width,int height,std::size_t index) {
    const bool flower=layer.effect==12;
    if (flower) {
        particle.row=index<50?2:index<85?1:0;
        particle.variant=0;
    } else {
        const int type=random_below(layer,25600);
        particle.row=type<1?0:type<10?1:type<2000?2:type<6000?3:type<14000?4:5;
        particle.variant=random_below(layer,4);
    }
    const int size=falling_size(layer,particle);
    const auto origin=[&](int limit) {
        const int low=random_below(layer,32768);
        const int high=layer.effect==57?0:32768*random_below(layer,16);
        return (low+high)%(limit*40)*64/size;
    };
    particle.x=origin(width);particle.y=origin(height);
    particle.velocity_x=flower?random_below(layer,600)-300:random_below(layer,300)-150;
    particle.velocity_y=flower?random_below(layer,150)+200:random_below(layer,200)+500;
    particle.phase=0;particle.period=random_below(layer,10)+(flower?30:20);
}
void falling_tick(Layer& layer,int width,int height) {
    const bool flower=layer.effect==12;
    auto& particles=layer.effect_state->particles;
    for (std::size_t i=0;i<particles.size();++i) {
        auto& particle=particles[i];
        const int period=std::clamp(particle.period,flower?30:20,flower?39:29);
        const int phase=(std::max(0,particle.phase)%period+1)%period;
        const double angle=(phase+(flower?period/4:0))*3.14159*2/period;
        const int dx=static_cast<int>((std::cos(angle+3.14159*2/period)-std::cos(angle))*(flower?2000:1000)+0.5);
        const int dy=flower?static_cast<int>((std::sin((phase%(period/2))*3.14159*2/period)-0.8)*300):0;
        particle.phase=phase;
        particle.x=clamp_int(static_cast<std::int64_t>(particle.x)+particle.velocity_x+dx);
        particle.y=clamp_int(static_cast<std::int64_t>(particle.y)+particle.velocity_y+dy);
        const auto y=static_cast<std::int64_t>(particle.y)*falling_size(layer,particle)/(40*64);
        if (y>=height) {
            falling_spawn(layer,particle,width,height,i);
            particle.y=-1000;
        }
    }
}
void falling_sprites(Surface& out,const Layer& layer,const AnimationTexture& assets) {
    if (!assets) return;
    const bool flower=layer.effect==12;
    auto texture=assets(flower?"sys/ta_eff_saku":"sys/ta_eff_snow");
    if (!texture) texture=assets(flower?"sys/eff_saku":"sys/eff_snow");
    if (!texture) return;
    const auto& particles=layer.effect_state->particles;
    const auto count=flower?std::min(particles.size(),static_cast<std::size_t>(std::max(0,value(layer,3)))):particles.size();
    const int start=!flower && value(layer,1)==-1?0:value(layer,1);
    const int end=!flower && value(layer,2)==-1?5:value(layer,2);
    for (std::size_t i=0;i<count;++i) {
        const auto& particle=particles[i];
        if (particle.row<start || particle.row>end || particle.row<0 || particle.row>(flower?2:5)) continue;
        const auto row=static_cast<std::size_t>(particle.row);
        const int size=falling_size(layer,particle),sy=flower?flower_row[row]:snow_row[row];
        int frame=particle.variant;
        if (flower) {
            const int period=std::clamp(particle.period,30,39);
            frame=(std::max(0,particle.phase)%period)*8/period;
            if (frame>=5) frame=8-frame;
        }
        if (frame<0 || frame>4) continue;
        const int x=clamp_int(static_cast<std::int64_t>(particle.x)*size/(40*64));
        const int y=clamp_int(static_cast<std::int64_t>(particle.y)*size/(40*64));
        normal_picture(out,*texture,{size*frame,sy,size,size},{x,y,size,size},true);
    }
}
void sparkle_spawn(Layer& layer,Particle& particle,int width,int height) {
    particle.phase=0;particle.period=random_below(layer,8)+5;
    const auto axis=[&](int start,int range,int size) {
        const auto position=static_cast<std::int64_t>(start)+random_below(layer,range)-32;
        return static_cast<int>(std::min<std::int64_t>(std::max<std::int64_t>(32,position),size-32));
    };
    particle.x=axis(value(layer,2),value(layer,4),width);
    particle.y=axis(value(layer,3),value(layer,5),height);
}
void sparkles_tick(Layer& layer,int width,int height) {
    for (auto& particle:layer.effect_state->particles) {
        if (particle.phase<std::clamp(particle.period,5,12)-1) ++particle.phase;
        else sparkle_spawn(layer,particle,width,height);
    }
}
std::size_t packed_count(const Layer& layer,std::size_t parameter,std::size_t stride,int max_count) {
    return std::min((layer.parameters.size()-16)/stride,
                    static_cast<std::size_t>(std::clamp(value(layer,parameter),0,max_count)));
}
int attachment(Layer& layer,int sides) {
    std::array<int,4> options{};
    int count=0;
    for (unsigned i=0;i<options.size();++i)
        if (sides&(1<<i)) options[static_cast<std::size_t>(count++)]=static_cast<int>(i+1);
    if (count<2) return options[0];
    return options[static_cast<std::size_t>(random_below(layer,count))];
}
void pin_sprite(const Layer& layer,std::int32_t* sprite,int side,int width,int height) {
    switch (side) {
    case 1: sprite[3]=value(layer,2)/2;break;
    case 2: sprite[2]=width-value(layer,1)/2;break;
    case 3: sprite[3]=height-value(layer,2)/2;break;
    case 4: sprite[2]=value(layer,1)/2;break;
    default:break;
    }
}
void sprite_spawn(Layer& layer,std::size_t index,int width,int height,bool initial) {
    auto* sprite=layer.parameters.data()+16+index*8;
    const int show=value(layer,8),hide=value(layer,9);
    const int phase=random_below(layer,static_cast<std::int64_t>(show)+hide);
    sprite[0]=phase<show || !initial?std::max(1,clamp_int(static_cast<std::int64_t>(show)/2+
        random_below(layer,show))):0;
    sprite[1]=std::max(1,clamp_int(static_cast<std::int64_t>(hide)/2+random_below(layer,hide)));
    sprite[2]=random_below(layer,width);sprite[3]=random_below(layer,height);
    sprite[4]=0;sprite[5]=0;
    if (layer.effect==27 && initial) sprite[6]=attachment(layer,value(layer,4));
    if (initial || layer.effect==108) pin_sprite(layer,sprite,layer.effect==27?sprite[6]:value(layer,4),width,height);
}
void sprites_tick(Layer& layer,int width,int height) {
    if (layer.count>10000) layer.count=0;
    const int attached=value(layer,4),frames=value(layer,6),rate=std::max(1,value(layer,7));
    const int jitter=std::max(0,value(layer,10));
    const auto count=packed_count(layer,3,8,16);
    for (std::size_t i=0;i<count;++i) {
        auto* sprite=layer.parameters.data()+16+i*8;
        if (sprite[0]>0) --sprite[0];
        else if (sprite[1]<=1) sprite_spawn(layer,i,width,height,false);
        else --sprite[1];
        if (frames>1) {
            if (sprite[5]>=rate-1) {
                sprite[5]=0;
                sprite[4]=(std::clamp(sprite[4],0,frames-1)+1)%frames;
            } else ++sprite[5];
        }
        const auto walk=[&](int current,int bound) {
            const auto step=random_below(layer,static_cast<std::int64_t>(jitter)*2+1);
            return static_cast<int>(std::clamp(static_cast<std::int64_t>(current)+step-jitter,
                std::int64_t{0},static_cast<std::int64_t>(bound-1)));
        };
        sprite[2]=walk(sprite[2],width);sprite[3]=walk(sprite[3],height);
        if (layer.effect==27 && attached && value(layer,11)>0 && random_below(layer,10000)<value(layer,11))
            sprite[6]=attachment(layer,attached);
        pin_sprite(layer,sprite,layer.effect==27?sprite[6]:attached,width,height);
    }
}
void wandering_sprites(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const int w=value(layer,1),h=value(layer,2),variants=std::max(1,value(layer,5));
    if (w<1 || h<1 || w>layer.texture->canvas_width || h>layer.texture->canvas_height) return;
    const auto count=packed_count(layer,3,8,16);
    for (std::size_t i=0;i<count;++i) {
        const auto* sprite=layer.parameters.data()+16+i*8;
        if (sprite[0]<=0 || sprite[4]<0 || sprite[4]>=layer.texture->canvas_height/h) continue;
        const Rect src{static_cast<int>(i%static_cast<unsigned>(variants))*w,sprite[4]*h,w,h};
        const Rect dst{clamp_int(static_cast<std::int64_t>(sprite[2])-w/2),
                       clamp_int(static_cast<std::int64_t>(sprite[3])-h/2),w,h};
        normal_picture(out,*layer.texture,src,dst,true);
    }
}
int fish_speed(Layer& layer) {
    const auto min=static_cast<std::int64_t>(value(layer,11));
    const auto speed=min+random_below(layer,static_cast<std::int64_t>(value(layer,12))-min+1);
    return clamp_int(random_below(layer,100)<50?-speed:speed);
}
void fish_spawn(Layer& layer,std::size_t index) {
    auto* fish=layer.parameters.data()+16+index*4;
    fish[0]=clamp_int(static_cast<std::int64_t>(value(layer,4))+
        random_below(layer,static_cast<std::int64_t>(value(layer,6))-value(layer,2)));
    fish[1]=clamp_int(static_cast<std::int64_t>(value(layer,5))+
        random_below(layer,static_cast<std::int64_t>(value(layer,7))-value(layer,3)));
    fish[2]=fish_speed(layer);fish[3]=0;
}
void fish_tick(Layer& layer) {
    if (layer.count>10000) layer.count=0;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const auto max_x=std::max<std::int64_t>(p(4),static_cast<std::int64_t>(p(4))+p(6)-p(2));
    const auto max_y=std::max<std::int64_t>(p(5),static_cast<std::int64_t>(p(5))+p(7)-p(3));
    const auto frames=std::max<std::int64_t>(1,static_cast<std::int64_t>(p(9))*p(10));
    const auto count=packed_count(layer,1,4,40);
    for (std::size_t i=0;i<count;++i) {
        auto* fish=layer.parameters.data()+16+i*4;
        if (p(13)>0 && random_below(layer,10000)<p(13)) {
            fish[2]=fish_speed(layer);fish[3]=0;
            fish[1]=clamp_int(std::clamp(static_cast<std::int64_t>(fish[1])+random_below(layer,5)-2,
                                        static_cast<std::int64_t>(p(5)),max_y));
        }
        const auto x=static_cast<std::int64_t>(fish[0])+fish[2];
        if ((x<p(4) && fish[2]<0) || (x>max_x && fish[2]>0)) fish[2]=clamp_int(-static_cast<std::int64_t>(fish[2]));
        fish[0]=clamp_int(std::clamp(x,static_cast<std::int64_t>(p(4)),max_x));
        fish[3]=static_cast<int>((static_cast<std::int64_t>(std::max(0,fish[3]))+1)%
                                std::min<std::int64_t>(frames,std::numeric_limits<int>::max()));
    }
}
void swimming_fish(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const int w=value(layer,2),h=value(layer,3);
    if (w<1 || h<1 || w>layer.texture->canvas_width || h>layer.texture->canvas_height) return;
    const int species=std::max(1,value(layer,8)),frames=std::max(1,value(layer,9));
    const int speed=std::max(1,value(layer,10));
    const auto count=packed_count(layer,1,4,40);
    for (std::size_t i=0;i<count;++i) {
        const auto* fish=layer.parameters.data()+16+i*4;
        const int col=static_cast<int>(i%static_cast<unsigned>(species))*2+(fish[2]>0?1:0);
        const int row=std::max(0,fish[3])/speed%frames;
        if (col>=layer.texture->canvas_width/w || row>=layer.texture->canvas_height/h) continue;
        normal_picture(out,*layer.texture,{col*w,row*h,w,h},{fish[0],fish[1],w,h},true);
    }
}
void spiral_sprites(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int w=p(1),h=p(2),variants=std::max(1,p(3)),angles=std::clamp(p(4),1,360);
    if (w<1 || h<1 || w>layer.texture->canvas_width || h>layer.texture->canvas_height) return;
    const auto range=std::max<std::int64_t>(1,std::abs(static_cast<std::int64_t>(p(10))-p(9)));
    const int count=std::clamp(p(5),0,10000),directions=std::max(1,p(6));
    const int offset=360/directions,angle_span=360/angles;
    static const auto circle=[] {
        std::array<std::array<int,2>,360> table{};
        for (std::size_t i=0;i<table.size();++i) {
            const double theta=static_cast<double>(i)*3.14159/180;
            table[i]={static_cast<int>(std::cos(theta)*256+0.5),static_cast<int>(std::sin(theta)*256+0.5)};
        }
        return table;
    }();
    for (int i=0;i<count;++i) {
        const auto tick=static_cast<std::int64_t>(layer.effect_state->distance)+i-p(11);
        if (tick<0) continue;
        const auto radius=(tick%range)*p(7)%range;
        const auto r=p(10)>=p(9)?p(9)+radius:std::max<std::int64_t>(0,p(9)-radius);
        const auto bearing=((tick%360)*p(8)+(i*360)/count+(i+1)*offset)%360;
        const int angle=static_cast<int>((bearing+360)%360);
        const auto& vector=circle[static_cast<std::size_t>(angle)];
        const auto x=clamp_int(p(12)+r*vector[0]/256-w/2),y=clamp_int(p(13)+r*vector[1]/256-h/2);
        const int row=i%variants,col=((angle+angle_span/2)%360)/angle_span%angles;
        if (col>=layer.texture->canvas_width/w || row>=layer.texture->canvas_height/h) continue;
        normal_picture(out,*layer.texture,{col*w,row*h,w,h},{x,y,w,h},true);
    }
}
int petal_pattern(const Layer& layer) {
    const auto frames=std::max(1,value(layer,18)),between=std::max(1,value(layer,22));
    return std::max(1,clamp_int(static_cast<std::int64_t>(frames)*between));
}
void petal_spawn(Layer& layer,Particle& particle) {
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int radius=std::max(0,p(8)),sway=std::max(0,p(17));
    particle.variant=random_below(layer,static_cast<std::int64_t>(std::max(1,p(5)))*petal_pattern(layer));
    const auto around=[&](int center) {
        return clamp_int((static_cast<std::int64_t>(center)+random_below(layer,static_cast<std::int64_t>(radius)*2)-
                          radius/2)*100);
    };
    particle.x=around(p(6));particle.y=around(p(7));
    const auto wind=[&](int value) {
        const auto speed=static_cast<std::int64_t>(value)*100;
        const auto range=std::max<std::int64_t>(1,std::abs(speed/2));
        return clamp_int(speed+random_below(layer,range)-range/2);
    };
    particle.velocity_x=wind(p(9));particle.velocity_y=wind(p(10));
    particle.period=clamp_int(static_cast<std::int64_t>(sway)/2+random_below(layer,sway)+2);
    particle.phase=random_below(layer,particle.period);
    particle.reverse=p(19)!=0 && random_below(layer,100)<50;
}
void petals_tick(Layer& layer,int width,int height) {
    if (layer.count>=std::max(1U,duration(layer)) || !layer.texture) return;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int pattern=petal_pattern(layer);
    const int w=std::max(0,p(2))/2,h=std::max(0,p(3))/2;
    const double sway_x=-static_cast<double>(p(10))*p(16)*0.2,sway_y=static_cast<double>(p(9))*p(16)*0.2;
    const auto count=std::min(layer.effect_state->particles.size(),static_cast<std::size_t>(std::max(0,p(1))));
    for (std::size_t i=0;i<count;++i) {
        auto& particle=layer.effect_state->particles[i];
        if (particle.variant<0) {
            if (!p(11)) continue;
            petal_spawn(layer,particle);
        }
        if (p(18)>1) {
            const int offset=particle.variant%pattern;
            const int next=particle.reverse?(offset?offset-1:pattern-1):(offset+1)%pattern;
            particle.variant=clamp_int(static_cast<std::int64_t>(particle.variant)-offset+next);
        }
        const int period=std::max(1,particle.period);
        particle.phase=(std::max(0,particle.phase)%period+1)%period;
        const double wave=std::sin(particle.phase*3.14159*2/period);
        const auto drift=[&](int current,int velocity,double sway) {
            const auto delta=static_cast<int>(std::clamp(wave*sway+0.5,-2147483648.0,2147483647.0));
            return clamp_int(static_cast<std::int64_t>(current)+velocity+delta);
        };
        particle.x=drift(particle.x,particle.velocity_x,sway_x);
        particle.y=drift(particle.y,particle.velocity_y,sway_y);
        const int x=particle.x/100,y=particle.y/100;
        if ((particle.velocity_x<0?x< -w:static_cast<std::int64_t>(x)>width+static_cast<std::int64_t>(w)) ||
            (particle.velocity_y<0?y< -h:static_cast<std::int64_t>(y)>height+static_cast<std::int64_t>(h)))
            particle.variant=-1;
    }
}
void petals(Surface& out,const Layer& layer) {
    if (!layer.texture || layer.count>=std::max(1U,duration(layer)) || value(layer,4)==-1) return;
    const auto& texture=*layer.texture;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int w=p(2),h=p(3),blend=std::max(1,p(22)),frames=std::max(1,p(18));
    if (w<1 || h<1) return;
    const int columns=texture.canvas_width/w,rows=texture.canvas_height/h;
    if (!columns || !rows) return;
    const int opacity=std::clamp(interpolate(layer,p(15),p(14),p(20),p(21)),0,100);
    if (!opacity) return;
    const auto tile=[&](int n) { return Point{(n%columns)*w,(n/columns)*h}; };
    const auto count=std::min(layer.effect_state->particles.size(),static_cast<std::size_t>(std::max(0,p(1))));
    for (std::size_t i=0;i<count;++i) {
        const auto& particle=layer.effect_state->particles[i];
        if (particle.variant<0) continue;
        const int index=particle.variant/blend;
        if (index/columns>=rows) continue;
        const auto a=tile(index);
        const Rect dst{particle.x/100-w/2,particle.y/100-h/2,w,h};
        if (blend==1) {
            const Rect src{a.x,a.y,w,h};
            if (opacity==100) normal_picture(out,texture,src,dst,true);
            else translucent_picture(out,texture,src,dst,opacity);
        } else {
            const int in_cycle=index%frames;
            const int next=particle.reverse?(in_cycle?in_cycle-1:frames-1):(in_cycle+1)%frames;
            const auto alternate=static_cast<std::int64_t>(index)-in_cycle+next;
            if (alternate/columns>=rows) continue;
            const int percent=static_cast<int>(static_cast<std::int64_t>(particle.variant%blend)*100/blend);
            blend_animation_frames(out,texture,a,texture,tile(static_cast<int>(alternate)),dst,
                                   (100-percent)*opacity/100,percent*opacity/100);
        }
    }
}
void rays_tick(Layer& layer) {
    for (auto& ray:layer.effect_state->particles) {
        if (ray.phase>1) { --ray.phase;continue; }
        ray.reverse=!ray.reverse;
        ray.phase=ray.reverse?60+random_below(layer,160):1+random_below(layer,2);
    }
}
void sun_rays(Surface& out,const Layer& layer) {
    const auto length=std::max(1U,duration(layer));
    const auto frame=duration(layer)?std::min(layer.count,length):1U;
    const int light=static_cast<int>(static_cast<std::uint64_t>(frame)*65536/length);
    const int width=static_cast<int>(out.width),height=static_cast<int>(out.height)/2;
    const int from_right=value(layer,1)!=0;
    const int direction=from_right?-1:1;
    const auto& rays=layer.effect_state->particles;
    for (std::size_t i=0;i<std::min<std::size_t>(5,rays.size());++i) {
        if (!rays[i].reverse) continue;
        const int index=static_cast<int>(i);
        const int start=(width*from_right+direction*(index-2)*96)*256;
        const int velocity=direction*(index*128+96);
        const int initial=light+(7-index)*height*256/7-336*256;
        for (int y=0;y<height;++y) {
            const int fixed=start+velocity*y;
            const int left=(fixed-(fixed<0?255:0))/256;
            const int shade=initial-y*256;
            for (int x=std::max(0,-left);x<48 && left+x<width;++x) {
                const int add=std::max(0,(shade+std::min(x,48-x)*2048)/256);
                if (!add) continue;
                const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(left+x))*3;
                for (std::size_t c=0;c<3;++c)
                    out.rgb[at+c]=static_cast<Byte>(std::min(255,out.rgb[at+c]+add));
            }
        }
    }
}
void flare_tick(Layer& layer) {
    const auto count=packed_count(layer,1,8,20);
    for (std::size_t i=0;i<count;++i) {
        auto* flare=layer.parameters.data()+16+i*8;
        flare[7]=flare[6]>1?1:0;
        if (flare[7]) --flare[6];
        else flare[6]=random_below(layer,60)+30;
    }
}
void sun_flare(Surface& out,const Layer& layer) {
    const auto length=static_cast<std::int64_t>(std::max(1U,duration(layer)));
    const auto frame=duration(layer)?std::min<std::int64_t>(layer.count,length):1;
    const auto at=[&](std::size_t start,std::size_t end) {
        return clamp_int(value(layer,start)+(static_cast<std::int64_t>(value(layer,end))-value(layer,start))*frame/length);
    };
    const int x0=at(2,4),y0=at(3,5),x1=at(6,8),y1=at(7,9);
    const auto count=packed_count(layer,1,8,20);
    for (std::size_t i=0;i<count;++i) {
        const auto* flare=layer.parameters.data()+16+i*8;
        if (!flare[7]) continue;
        const int x=clamp_int(x0+(static_cast<std::int64_t>(x1)-x0)*flare[0]/100);
        const int y=clamp_int(y0+(static_cast<std::int64_t>(y1)-y0)*flare[0]/100);
        const int radius=std::clamp(flare[1],1,135),inner=std::clamp(flare[2],0,radius-1);
        const Ellipse disk{x,y,radius,radius};
        const std::array<int,3> color{flare[3],flare[4],flare[5]};
        if (!inner) { ellipse_overlay(out,disk,nullptr,color,100,true);continue; }
        const int width=static_cast<int>(out.width),height=static_cast<int>(out.height);
        const auto top=std::max<std::int64_t>(0,static_cast<std::int64_t>(y)-radius);
        const auto bottom=std::min<std::int64_t>(height,static_cast<std::int64_t>(y)+radius+1);
        for (auto row=top;row<bottom;++row) {
            const auto [left,right]=disk.span(static_cast<int>(row),width);
            const auto distance=std::abs(row-y);
            const auto add=[&](int from,int to) {
                for (int column=from;column<to;++column) {
                    const auto pixel=(static_cast<std::size_t>(row)*out.width+static_cast<unsigned>(column))*3;
                    for (std::size_t c=0;c<3;++c)
                        out.rgb[pixel+c]=static_cast<Byte>(std::min(255,out.rgb[pixel+c]+(color[c]&255)));
                }
            };
            if (distance>inner) add(left,right);
            else {
                const auto extent=static_cast<int>(std::sqrt(static_cast<double>(inner*inner-distance*distance))+0.5);
                const auto low=std::min<std::int64_t>(width-1,static_cast<std::int64_t>(x)-extent);
                const auto high=std::max<std::int64_t>(left,static_cast<std::int64_t>(x)+extent);
                add(left,static_cast<int>(std::min<std::int64_t>(low,right)));
                if (high<right) add(static_cast<int>(high),right-1);
            }
        }
    }
}
void flare_init(Layer& layer) {
    auto& p=layer.parameters;
    int rings=0;
    const int count=value(layer,1);
    p.resize(std::max(p.size(),16U+static_cast<std::size_t>(count)*8));
    for (int i=0;i<count;++i) {
        auto* flare=p.data()+16+static_cast<std::size_t>(i)*8;
        flare[0]=(i+1)*150/count+random_below(layer,150/count);
        flare[1]=16+random_below(layer,64);
        if (i==count-1) flare[1]=12+random_below(layer,12);
        flare[2]=0;
        for (std::size_t c=0;c<3;++c) flare[3+c]=24+random_below(layer,64);
        if (rings<value(layer,10) && (random_below(layer,100)<18 || i==count-3)) {
            flare[1]=72+random_below(layer,64);
            flare[2]=std::max(3,flare[1]-20-random_below(layer,16));
            for (std::size_t c=0;c<3;++c) flare[3+c]=10+random_below(layer,24);
            ++rings;
        }
        flare[6]=20+random_below(layer,30);
    }
    flare_tick(layer);
}
void speed_streak_spawn(Layer& layer,Particle& streak,bool initial) {
    const auto p=[&](std::size_t i){return value(layer,i);};
    const auto length=static_cast<std::int64_t>(std::max(81,p(2)));
    const int speed=p(4);
    streak.y=clamp_int(static_cast<std::int64_t>(p(9))+random_below(layer,static_cast<std::int64_t>(p(11))-2));
    const auto base=static_cast<std::int64_t>(p(8));
    if (initial) streak.x=clamp_int(base-length+random_below(layer,p(10)+length*2));
    else if (speed<0) streak.x=clamp_int(base+p(10)+random_below(layer,length+1));
    else streak.x=clamp_int(base-length*2+random_below(layer,length+1));
    const auto variation=std::max(1,std::abs(speed/4));
    streak.velocity_x=clamp_int(static_cast<std::int64_t>(speed)+random_below(layer,2*variation)-variation);
    streak.variant=clamp_int(length+random_below(layer,length/2));
    streak.reverse=random_below(layer,2)!=0;
}
void speed_streak_tick(Layer& layer) {
    const auto left=static_cast<std::int64_t>(value(layer,8))-static_cast<std::int64_t>(value(layer,2))*125/100;
    const auto right=static_cast<std::int64_t>(value(layer,8))+value(layer,10);
    for (auto& streak:layer.effect_state->particles) {
        streak.x=clamp_int(static_cast<std::int64_t>(streak.x)+streak.velocity_x);
        if (value(layer,4)>0?streak.x>right:streak.x<left) speed_streak_spawn(layer,streak,false);
    }
}
void speed_streaks(Surface& out,const Layer& layer) {
    const int left=std::clamp(value(layer,8),0,static_cast<int>(out.width));
    const auto right=std::min<std::int64_t>(out.width,static_cast<std::int64_t>(value(layer,8))+value(layer,10));
    const std::array<int,3> color{value(layer,5)&255,value(layer,6)&255,value(layer,7)&255};
    for (const auto& streak:layer.effect_state->particles) {
        const auto start=static_cast<std::int64_t>(streak.x),end=start+streak.variant;
        const auto row=[&](std::int64_t y,int solid,int fade) {
            if (y<0 || y>=out.height) return;
            const auto first=std::max<std::int64_t>(left,start),last=std::min(right,end-solid);
            if (last>first) box(out,static_cast<int>(first),static_cast<int>(y),static_cast<int>(last-first),1,color,100);
            const auto origin=end-fade,tail=std::min(right,origin+64);
            for (auto x=std::max<std::int64_t>(left,origin);x<tail;++x) {
                const int opacity=255-static_cast<int>(x-origin)*4;
                const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<std::size_t>(x))*3;
                for (std::size_t c=0;c<3;++c)
                    out.rgb[at+c]=static_cast<Byte>((color[c]*opacity+out.rgb[at+c]*(255-opacity))>>8);
            }
        };
        if (streak.reverse) row(streak.y,80,80);
        row(static_cast<std::int64_t>(streak.y)+1,64,64);
        if (streak.reverse) row(static_cast<std::int64_t>(streak.y)+2,64,80);
    }
}
void staff_roll(Surface& out,const Layer& layer,unsigned slot,const AnimationTexture& assets) {
    if (!layer.texture) return;
    const int width=static_cast<int>(out.width),height=static_cast<int>(out.height);
    const auto p=[&](std::size_t i){return value(layer,i);};
    std::array<int,6> bounds{0,0,0,0,0,height};
    for (std::size_t i=1;i<5;++i) bounds[i]=std::clamp(p(7+i),bounds[i-1],height);
    const auto dst=destination(layer);
    const int count=std::clamp(p(12),1,4096),buffers=std::max(1,p(13)),mask_mode=p(1);
    for (int i=0;i<count;++i) {
        const auto start_y=static_cast<std::int64_t>(dst.y)+static_cast<std::int64_t>(layer.texture->canvas_height)*i;
        if (start_y>=height) break;
        const auto texture=i==0?layer.texture:
            assets && slot+1+static_cast<unsigned>((i-1)%buffers)<=16 && !layer.texture->name.empty()?
            assets(layer.texture->name+"_"+std::to_string(i+1)):nullptr;
        if (!texture || (mask_mode==2 && !texture->has_mask)) continue;
        const auto right=std::min<std::int64_t>(width,static_cast<std::int64_t>(dst.x)+texture->canvas_width);
        const auto bottom=std::min<std::int64_t>(height,start_y+texture->canvas_height);
        const auto key=texel(*texture,0,0);
        for (auto y=std::max<std::int64_t>(0,start_y);y<bottom;++y) {
            const auto segment=static_cast<std::size_t>(std::upper_bound(bounds.begin(),bounds.end(),y)-bounds.begin()-1);
            const auto start=static_cast<std::int64_t>(p(2+segment)),end=static_cast<std::int64_t>(p(3+segment));
            const auto percent=start+(end-start)*(y-bounds[segment])/(bounds[segment+1]-bounds[segment]);
            const int opacity=static_cast<int>(std::clamp(percent*256/100,std::int64_t{0},std::int64_t{256}));
            if (!opacity) continue;
            const Rect src{0,static_cast<int>(y-start_y),texture->canvas_width,1};
            const Rect row{dst.x,static_cast<int>(y),texture->canvas_width,1};
            if (right<=std::max(0,dst.x)) continue;
            picture_pixels(out,*texture,src,row,[&](Byte* pixel,const Byte* sample) {
                if (mask_mode==2?sample[3]==0:sample[0]==key[0] && sample[1]==key[1] && sample[2]==key[2]) return;
                const int weight=mask_mode==2?(sample[3]*opacity)>>8:opacity;
                for (std::size_t c=0;c<3;++c)
                    pixel[c]=static_cast<Byte>(((sample[c]*weight)>>8)+((pixel[c]*(256-weight))>>8));
            });
        }
    }
}
const Texture* referenced_picture(const std::array<Layer,32>& layers,unsigned self,int index) {
    if (index==-1) return layers[self].texture.get();
    return index>=0 && static_cast<std::size_t>(index)<layers.size()?
        layers[static_cast<std::size_t>(index)].texture.get():nullptr;
}
void waterfall(Surface& out,const std::array<Layer,32>& layers,unsigned slot) {
    const auto& layer=layers[slot];
    const auto p=[&](std::size_t i){return value(layer,i);};
    const auto* texture=referenced_picture(layers,slot,p(4));
    if (!texture || !texture->has_mask || p(7)!=2 || texture->canvas_height<1) return;
    const int w=texture->canvas_width,h=texture->canvas_height;
    const int length=static_cast<int>(duration(layer));
    int tick=std::min(length,std::max(0,layer.effect_state->distance));
    if (p(5)>0) {
        const auto period=(static_cast<std::int64_t>(h)*100+p(5)-1)/p(5);
        if (period<=length) tick=static_cast<int>(std::max(0,layer.effect_state->distance)%period);
    }
    const double v=p(5)*0.01,gravity=p(6)*0.01,shift=tick*v;
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(p(2))+p(3));
    for (int y=std::max(0,p(2));y<bottom;++y) {
        const auto row=static_cast<std::int64_t>(y)-p(2);
        const double square=v*v+2*gravity*static_cast<double>(row);
        if (square<=0) continue;
        const double speed=std::sqrt(square);
        const double phase=(gravity!=0.0?(speed-v)*v/gravity:static_cast<double>(row))-shift;
        const int wrapped=static_cast<int>(std::fmod(std::trunc(phase+0.5),h));
        const int source_y=(wrapped+h)%h;
        const int opacity=static_cast<int>(std::clamp(v/speed*256+0.5,0.0,256.0));
        picture_pixels(out,*texture,{0,source_y,w,1},{p(1),y,w,1},[&](Byte* pixel,const Byte* sample) {
            if (!sample[3]) return;
            const int source=sample[3]==255?opacity:sample[3]*opacity/256;
            const int target=(sample[3]==255?256:255)-source;
            for (std::size_t c=0;c<3;++c) pixel[c]=static_cast<Byte>((sample[c]*source+pixel[c]*target)>>8);
        });
    }
}
void displaced_row(Surface& out,const Texture& texture,int x,int y,int start,int row,int width,bool alpha) {
    const Rect src{start,row,width,1},dst{x,y,width,1};
    if (!alpha || !texture.has_mask) { normal_picture(out,texture,src,dst,alpha);return; }
    picture_pixels(out,texture,src,dst,[](Byte* pixel,const Byte* sample) {
        const int a=sample[3];
        if (!a) return;
        for (std::size_t c=0;c<3;++c)
            pixel[c]=static_cast<Byte>(a==255?sample[c]:((sample[c]*a)>>8)+((pixel[c]*(255-a))>>8));
    });
}
void sway(Surface& out,const std::array<Layer,32>& layers,unsigned slot) {
    const auto& layer=layers[slot];
    const auto p=[&](std::size_t i){return value(layer,i);};
    const auto* texture=referenced_picture(layers,slot,p(7));
    if (!texture || p(5)!=2) return;
    const auto dst=destination(layer);
    const int left=std::max(0,dst.x),top=std::max(0,dst.y);
    const auto right=std::min<std::int64_t>(out.width,static_cast<std::int64_t>(dst.x)+texture->canvas_width);
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(dst.y)+texture->canvas_height);
    if (left>=right || top>=bottom) return;
    const int width=static_cast<int>(right-left),height=static_cast<int>(bottom-top);
    const int loop=std::max(1,p(1)),length=std::max(1,static_cast<int>(duration(layer)));
    int tick=static_cast<int>(std::min(layer.count,static_cast<unsigned>(std::numeric_limits<int>::max())));
    if (p(1)>0) tick%=loop;
    tick=std::min(tick,length-1);
    const int turn=static_cast<int>(static_cast<std::int64_t>(std::clamp(p(4),0,100))*loop/100);
    const bool returning=tick>=turn;
    const double phase=static_cast<double>(returning?tick-turn:tick)/std::max(1,returning?loop-turn:turn);
    const double easing=(1-std::cos(phase*3.14159))*0.5;
    const double range=static_cast<double>(p(2))+p(3);
    const double base=returning?p(2)-range*easing:-static_cast<double>(p(3))+range*easing;
    const int frequency=std::max(1,p(10));
    for (int y=0;y<height;++y) {
        const double phase_y=static_cast<double>((static_cast<std::int64_t>(y)+p(12))%frequency)*(6.28318/frequency);
        const double flutter=p(9)>0?p(9)*std::cos(phase_y):0;
        const double amount=1-std::sin((p(8)?height-y:y)*(3.14159/(2.0*height)));
        const auto shift=static_cast<std::int64_t>((base+flutter)*amount+0.5);
        displaced_row(out,*texture,clamp_int(static_cast<std::int64_t>(left)+shift),top+y,left-dst.x,
                      top-dst.y+y,width,true);
    }
}
void grass_gust_spawn(Layer& layer,Particle& wave,bool initial) {
    const auto p=[&](std::size_t i){return value(layer,i);};
    wave.phase=random_below(layer,50);
    const int factor=50+random_below(layer,100);
    const auto power=clamp_int(static_cast<std::int64_t>(p(6))*p(8)/100);
    wave.variant=clamp_int(static_cast<std::int64_t>(power)*factor/100);
    wave.velocity_y=std::max(1,clamp_int(static_cast<std::int64_t>(p(9))*(70+random_below(layer,60))/100));
    wave.y=p(5)==0?p(4):p(3);
    wave.row=std::max(1,clamp_int(static_cast<std::int64_t>(p(10))*(80+random_below(layer,40))/100));
    if (initial) {
        const auto move=random_below(layer,static_cast<std::int64_t>(p(3))-p(4));
        wave.phase=0;wave.y=clamp_int(static_cast<std::int64_t>(wave.y)+(p(5)==0?move:-move));
    }
}
void grass_gust_tick(Layer& layer) {
    auto& waves=layer.effect_state->particles;
    const auto count=std::min(waves.size(),static_cast<std::size_t>(std::max(0,value(layer,7))));
    const bool outbound=value(layer,5)==0;
    for (std::size_t i=0;i<count;++i) {
        auto& wave=waves[i];
        if (wave.phase>0) { --wave.phase;continue; }
        wave.y=clamp_int(static_cast<std::int64_t>(wave.y)+(outbound?wave.velocity_y:-static_cast<std::int64_t>(wave.velocity_y)));
        if (outbound?wave.y>value(layer,3):wave.y<value(layer,4)) grass_gust_spawn(layer,wave,false);
    }
}
void grass(Surface& out,const std::array<Layer,32>& layers,unsigned slot) {
    const auto& layer=layers[slot];
    const auto p=[&](std::size_t i){return value(layer,i);};
    const auto* texture=referenced_picture(layers,slot,p(11));
    if (!texture) return;
    const int width=texture->canvas_width,height=static_cast<int>(out.height);
    const int size=std::clamp(p(2),1,height);
    const double near=std::clamp(p(4),1,std::numeric_limits<int>::max()-1);
    const double far=std::max(near+1,p(3)*1.0);
    const double project=far/(far-near)*size;
    const double phase=static_cast<double>(layer.count)*(p(5)==1?-1:1)*(3.14159*10/height);
    const auto dst=destination(layer);
    const auto bottom=std::min<std::int64_t>(height,static_cast<std::int64_t>(dst.y)+texture->canvas_height);
    const auto& waves=layer.effect_state->particles;
    const auto count=std::min(waves.size(),static_cast<std::size_t>(std::max(0,p(7))));
    for (int y=std::max(0,dst.y);y<bottom;++y) {
        const int row=y-dst.y;
        const double distance=project*near/std::max(0.1,project-size+row);
        const double scale=near/distance;
        double shift=std::trunc(p(6)*scale*std::cos(distance*0.08+phase)+0.5);
        if (row<size) for (std::size_t i=0;i<count;++i) {
            const auto& wave=waves[i];
            if (wave.phase>0) continue;
            const double from=std::max(1.0,static_cast<double>(wave.y)-wave.row);
            const double to=std::max(from+0.1,static_cast<double>(wave.y)+wave.row);
            const auto dot=[&](double depth) { return std::trunc(size-(depth-near)*project/depth); };
            if (row>=dot(to) && row<=dot(from))
                shift+=(1-std::cos((distance-from)*6.28318/(to-from)))*wave.variant*scale;
        }
        const auto offset=static_cast<std::int64_t>(std::clamp(shift+0.5,-4294967296.0,4294967296.0));
        displaced_row(out,*texture,clamp_int(static_cast<std::int64_t>(dst.x)+offset),y,0,row,width,false);
    }
}
void cloud_tick(Layer& layer) {
    layer.count=0;
    if (!layer.texture || layer.texture->canvas_width<1 || layer.texture->canvas_height<1) return;
    const auto wrap=[](int current,int velocity,int length) {
        const int limit=length*100;
        const auto phase=(static_cast<std::int64_t>(current)+velocity)%limit;
        return static_cast<int>(phase<0?phase+limit:phase);
    };
    auto& state=*layer.effect_state;
    state.offset_x=wrap(state.offset_x,value(layer,2),layer.texture->canvas_width);
    state.offset_y=wrap(state.offset_y,value(layer,3),layer.texture->canvas_height);
}
void clouds(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const auto& texture=*layer.texture;
    const int w=texture.canvas_width,h=texture.canvas_height;
    if (w<1 || h<1) return;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int rows=std::clamp(p(1),1,static_cast<int>(out.height));
    const double near=std::max(1,p(5)),far=std::max(near+1,p(6)*1.0);
    const double perspective=(far-near)/far;
    const double scale=100.0/std::max(1,p(7));
    const int half=static_cast<int>(out.width)/2;
    const bool mask=p(8)!=0 && texture.has_mask,tint=p(9)!=0;
    const int start=std::clamp(p(13),0,rows-1),length=rows-start;
    constexpr std::int64_t unit=65536;
    const auto span=w*unit;
    const auto& state=*layer.effect_state;
    for (int y=0;y<rows;++y) {
        const double depth=near/(1-(y+1)*perspective/rows);
        const double scale_x=scale*depth/near;
        const auto whole=static_cast<std::int64_t>(scale_x);
        const auto fraction=std::min<std::int64_t>(65535,static_cast<std::int64_t>(
            (scale_x-static_cast<double>(whole))*65536+0.5));
        const auto step=(whole%w)*unit+fraction;
        const auto center=half*unit+static_cast<std::int64_t>(state.offset_x)*unit/100;
        auto phase=(center-half*step)%span;
        if (phase<0) phase+=span;
        const double source_row=state.offset_y*0.01+(depth-near)*scale;
        const int sy=(static_cast<int>(std::fmod(std::trunc(source_row),h))+h)%h;
        std::array<int,3> shift{};
        if (tint && y>=start) for (std::size_t c=0;c<3;++c) {
            const auto channel=std::clamp(p(10+c),-255,255);
            shift[c]=(std::abs(channel)*(y-start)+length/2)/length*(channel<0?-1:1);
        }
        const unsigned columns=tint && !mask?out.width/2U*2U:out.width;
        for (unsigned x=0;x<columns;++x) {
            const auto source=texel(texture,static_cast<int>(phase/unit),sy);
            phase+=step;
            if (phase>=span) phase-=span;
            const int a=mask?source[3]:255;
            if (!a) continue;
            const auto at=(static_cast<std::size_t>(y)*out.width+x)*3;
            for (std::size_t c=0;c<3;++c) {
                const int shade=std::clamp(source[c]+shift[c],0,255);
                out.rgb[at+c]=static_cast<Byte>(a==255?shade:(shade*a+out.rgb[at+c]*(256-a))>>8);
            }
        }
    }
}
void shore_tick(Layer& layer) {
    auto& waves=layer.effect_state->particles;
    if (waves.size()!=2) return;
    constexpr std::array<std::array<int,3>,2> speed{{{8130,121,71},{105,40,8102}}};
    for (std::size_t i=0;i<waves.size();++i) {
        auto& wave=waves[i];
        wave.x=(wave.x&8191)+speed[i][0];wave.x&=8191;
        wave.y=(wave.y&8191)+speed[i][1];wave.y&=8191;
        wave.variant=(wave.variant&8191)+speed[i][2];wave.variant&=8191;
        wave.row=((wave.row&2047)+(value(layer,7+i)&2047))&2047;
    }
}
void shore(Surface& out,const Layer& layer) {
    const auto& waves=layer.effect_state->particles;
    if (waves.size()!=2) return;
    const auto p=[&](std::size_t i){return value(layer,i);};
    constexpr std::array<std::array<double,3>,2> amplitude{{{30,21,10},{30,23,10}}};
    constexpr std::array<std::array<double,3>,2> frequency{{{33.2,-18.0,16.7},{22.6,-17.1,35.7}}};
    std::array<std::array<double,3>,2> phase{};
    std::array<double,2> drift{};
    for (std::size_t i=0;i<waves.size();++i) {
        const auto& wave=waves[i];
        phase[i]={(wave.x&8191)*6.2831852/8192,(wave.y&8191)*6.2831852/8192,
                  (wave.variant&8191)*6.2831852/8192};
        drift[i]=(1+std::cos((wave.row&2047)*6.28318/2048))*p(9+i)*0.5;
    }
    const int w=static_cast<int>(out.width),h=static_cast<int>(out.height);
    const int start=std::clamp(p(5),0,h-1),height=std::clamp(p(6),0,h-start);
    const int first=std::clamp(p(1),0,100),second=first*2-first*first/100;
    const auto draw_strip=[&](int y,int left,int right,int amount) {
        const int keep=(100-amount)*128/100;
        std::array<int,3> add{};
        for (std::size_t c=0;c<3;++c) add[c]=std::clamp(p(2+c),0,255)*amount/100;
        for (int x=left;x<right;++x) {
            const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
            for (std::size_t c=0;c<3;++c)
                out.rgb[at+c]=static_cast<Byte>(((out.rgb[at+c]*keep)>>7)+add[c]);
        }
    };
    const auto bound=[&](double x) { return static_cast<int>(std::clamp(x,0.0,static_cast<double>(w))); };
    for (int y=0;y<height;++y) {
        const double perspective=std::max(1,p(14))*0.01;
        const double scale=perspective+(1-perspective)*(static_cast<double>(p(5))+y)/h;
        const double edge=(static_cast<double>(p(13))-w/2)*scale+w/2;
        std::array<int,2> breakwater{};
        for (std::size_t i=0;i<waves.size();++i) {
            double ripple=0;
            for (std::size_t j=0;j<3;++j)
                ripple+=amplitude[i][j]*std::cos(static_cast<double>(y)/h*frequency[i][j]+phase[i][j]);
            breakwater[i]=bound(edge-drift[i]*scale-ripple*p(11+i)*0.01);
        }
        const auto [left,right]=std::minmax(breakwater[0],breakwater[1]);
        draw_strip(start+y,left,right,first);
        draw_strip(start+y,right,std::max(1,bound(edge)),second);
    }
}
struct CondensationPatterns {
    std::array<std::array<Byte,32*32>,4> steam{};
    std::array<std::array<Byte,128*128>,2> wipe{};

    CondensationPatterns() {
        std::uint32_t seed=0x7f4a7c15U;
        for (auto& pattern:steam) for (int y=0;y<32;++y) for (int x=0;x<32;++x) {
            const int dx=x-16,dy=y-16;
            const int distance=static_cast<int>(std::sqrt(dx*dx+dy*dy));
            const int noise=static_cast<int>(random_next(seed)&7U);
            pattern[static_cast<std::size_t>(y*32+x)]=static_cast<Byte>(std::max(1,9-distance+noise));
        }
        for (auto& pattern:wipe) for (int y=0;y<128;++y) for (int x=0;x<128;++x) {
            const int dx=x-64,dy=y-64;
            const int distance=static_cast<int>(std::sqrt(dx*dx+dy*dy));
            const int noise=static_cast<int>(random_next(seed)&31U);
            pattern[static_cast<std::size_t>(y*128+x)]=static_cast<Byte>((20+noise)*std::max(0,64-distance)/64);
        }
    }
};
template<class F> void update_condensation(std::vector<Byte>& mask,const Texture& texture,Rect area,F delta) {
    const auto crop=texture.content;
    const int left=std::max(area.x,crop.x),top=std::max(area.y,crop.y);
    const int right=std::min(area.x+area.width,crop.x+crop.width),bottom=std::min(area.y+area.height,crop.y+crop.height);
    for (int y=top;y<bottom;++y) for (int x=left;x<right;++x) {
        const auto at=static_cast<std::size_t>(y-crop.y)*texture.pixels.width+static_cast<unsigned>(x-crop.x);
        mask[at]=static_cast<Byte>(std::clamp(mask[at]+delta(x-area.x,y-area.y),0,255));
    }
}
void bath_window_tick(Layer& layer,int width,int height,Point cursor) {
    layer.count=0;
    if (!layer.texture || !layer.texture->has_mask) return;
    const auto& texture=*layer.texture;
    const int w=texture.canvas_width,h=texture.canvas_height;
    if (w<1 || h<1) return;
    const auto area=static_cast<std::uint64_t>(texture.pixels.width)*texture.pixels.height;
    if (area>max_pixels || texture.pixels.rgba.size()!=area*4) return;
    auto& state=*layer.effect_state;
    if (state.condensation.size()!=area) {
        state.condensation.resize(static_cast<std::size_t>(area));
        for (std::size_t i=0;i<area;++i) state.condensation[i]=texture.pixels.rgba[i*4+3];
    }
    static const CondensationPatterns patterns;
    const int count=std::clamp(value(layer,3),0,32),columns=std::max(1,w/8);
    std::array<Point,32> steam{};
    for (int i=0;i<count;++i) {
        const int x=(random_below(layer,columns)-2)*8,y=random_below(layer,h)-16;
        steam[static_cast<std::size_t>(i)]={x,y};
    }
    int pattern=random_below(layer,4);
    for (int i=0;i<count;++i) {
        const auto pos=steam[static_cast<std::size_t>(i)];
        const auto& stamp=patterns.steam[static_cast<std::size_t>(pattern)];
        const int size=std::min(32,columns*8-pos.x);
        update_condensation(state.condensation,texture,{pos.x,pos.y,size,32},[&](int x,int y) {
            return stamp[static_cast<std::size_t>(y*32+x)];
        });
        pattern=(pattern+1)%4;
    }
    const bool moved=cursor.x!=state.offset_x || cursor.y!=state.offset_y;
    state.offset_x=cursor.x;state.offset_y=cursor.y;
    if (!moved || cursor.x<0 || cursor.x>=width || cursor.y<0 || cursor.y>=height) return;
    const int half=std::clamp(value(layer,1),8,128)/2;
    const Rect wipe{cursor.x-half,cursor.y-half,half*2,half*2};
    if (wipe.x>=w || wipe.y>=h) return;
    const auto& stamp=patterns.wipe[static_cast<std::size_t>(random_below(layer,2))];
    update_condensation(state.condensation,texture,wipe,[&](int x,int y) {
        return -stamp[static_cast<std::size_t>((y*128/wipe.height)*128+x*128/wipe.width)];
    });
}
void bath_window(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const auto& texture=*layer.texture;
    const Rect area{value(layer,4),value(layer,5),value(layer,6),value(layer,7)};
    const auto& mask=layer.effect_state->condensation;
    if (!texture.has_mask || mask.empty()) { normal_picture(out,texture,area,area,true);return; }
    picture_pixels(out,texture,area,area,[&](Byte* pixel,const Byte* source) {
        const auto index=static_cast<std::size_t>(source-texture.pixels.rgba.data())/4;
        if (index>=mask.size()) return;
        const int a=mask[index];
        if (!a) return;
        for (std::size_t c=0;c<3;++c)
            pixel[c]=static_cast<Byte>(a==255?source[c]:((source[c]*a)>>8)+((pixel[c]*(256-a))>>8));
    });
}
void sparkles(Surface& out,const Layer& layer,const AnimationTexture& assets) {
    if (!assets) return;
    const auto texture=assets("sys/kirakira");
    if (!texture || texture->canvas_width<64 || texture->canvas_height<64) return;
    for (const auto& particle:layer.effect_state->particles) {
        const int length=std::clamp(particle.period,5,12),phase=std::clamp(particle.phase,0,length);
        const int peak=(length*7+5)/10;
        const int brightness=phase<=peak?phase*100/peak:(length-phase)*100/std::max(1,length-peak);
        const int threshold=255-brightness*255/100;
        picture_pixels(out,*texture,{0,0,64,64},{particle.x,particle.y,64,64},[&](Byte* pixel,const Byte* sample) {
            const int add=std::max(0,sample[2]-threshold);
            for (std::size_t c=0;c<3;++c) pixel[c]=static_cast<Byte>(std::min(255,pixel[c]+add));
        });
    }
}
void noise_spawn(Layer& layer,NoiseBand& band,int height) {
    auto& state=*layer.effect_state;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const auto rnd=[&](std::int64_t n) {
        return static_cast<int>(random_next(state.seed)%static_cast<unsigned>(std::clamp(n,std::int64_t{1},std::int64_t{2147483647})));
    };
    auto remaining=static_cast<std::int64_t>(p(6));
    const auto life_delta=remaining*2/10;
    if (life_delta>0) remaining+=rnd(life_delta*2)-life_delta;
    band.remaining=std::max(1,clamp_int(remaining));
    auto length=static_cast<std::int64_t>(p(4));
    const auto length_delta=static_cast<std::int64_t>(p(5))*length/100;
    if (length_delta>0) length+=rnd(std::min<std::int64_t>(length_delta,1073741823)*2)-length_delta;
    band.length=static_cast<int>(std::clamp(length,std::int64_t{1},std::int64_t{1048576}));
    const int range=std::max(1,height-band.length);
    band.y=rnd(range);band.strength=20+rnd(60);
}
void noise_raster(Layer& layer,int width,int height) {
    auto& state=*layer.effect_state;
    state.raster.assign(static_cast<std::size_t>(height),0);
    if (!layer.texture) return;
    const auto p=[&](std::size_t i){return value(layer,i);};
    if (p(8)>0) for (auto& delta:state.raster) {
        const int amount=clamp_int((static_cast<std::int64_t>(random_next(state.seed)%static_cast<unsigned>(p(8)))+50)/100);
        if (amount) delta=amount*(random_next(state.seed)%2?1:-1);
    }
    const int wave=std::clamp(p(3),1,1048576),strength=std::clamp(p(2),-100000,100000);
    const auto dst=destination(layer);
    for (const auto& band:state.bands) {
        const auto power=std::max<std::int64_t>(1,static_cast<std::int64_t>(strength)*band.strength/100);
        const int half=std::max(1,band.length/2);
        int r=100;
        for (int y=0;y<band.length;++y) {
            if (y%wave==0) r=static_cast<int>(random_next(state.seed)%70U)+30;
            const int wave_mul=wave-(y%wave)*2,sign=y%(wave*2)>=wave?-1:1;
            int edge=y%half;
            if (y>=half) edge=y<half*2?half-edge:0;
            const auto delta=static_cast<std::int64_t>(r)*edge*wave_mul*power/(static_cast<std::int64_t>(half)*wave*100)*sign;
            const auto yy=static_cast<std::int64_t>(band.y)+dst.y+y;
            if (yy>=0 && yy<height) state.raster[static_cast<std::size_t>(yy)]=clamp_int(
                static_cast<std::int64_t>(state.raster[static_cast<std::size_t>(yy)])+delta);
        }
    }
    static_cast<void>(width);
}
void noise_tick(Layer& layer,int width,int height) {
    if (!layer.texture) return;
    auto& state=*layer.effect_state;
    const int picture_height=layer.texture->canvas_height;
    for (auto& band:state.bands) {
        if (--band.remaining<=0) noise_spawn(layer,band,picture_height);
        band.y=clamp_int(static_cast<std::int64_t>(band.y)+value(layer,7));
        if (band.y< -band.length || band.y>=picture_height) noise_spawn(layer,band,picture_height);
        band.strength+=static_cast<int>(random_next(state.seed)%10U)-6;
        if (random_next(state.seed)%100U<2) band.strength+=static_cast<int>(random_next(state.seed)%160U)-40;
        band.strength=std::clamp(band.strength,30,100);
    }
    noise_raster(layer,width,height);
}
void chara_noise(Surface& out,const Layer& layer) {
    if (!layer.texture || !layer.texture->has_mask) return;
    const auto& t=*layer.texture;
    const Rect dst=destination(layer);
    int alpha=256;
    if (value(layer,9)!=100 || value(layer,10)!=100)
        alpha=std::clamp(interpolate(layer,clamp_int(static_cast<std::int64_t>(value(layer,10))*256),
            clamp_int(static_cast<std::int64_t>(value(layer,9))*256),value(layer,11),value(layer,12))/100,0,256);
    const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(dst.y)+t.canvas_height);
    for (int y=std::max(0,dst.y);y<bottom;++y) {
        const auto py=static_cast<std::int64_t>(dst.x)+layer.effect_state->raster[static_cast<std::size_t>(y)];
        const auto right=std::min<std::int64_t>(out.width,py+t.canvas_width);
        for (auto x=std::max<std::int64_t>(0,py);x<right;++x) {
            const auto src=texel(t,clamp_int(x-py),y-dst.y);
            const int a=alpha*src[3]/256;
            if (!a) continue;
            const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
            for (std::size_t c=0;c<3;++c)
                out.rgb[at+c]=static_cast<Byte>(a>=255?src[c]:(src[c]*a+out.rgb[at+c]*(256-a))>>8);
        }
    }
}
void firefly_spawn(Layer& layer,Firefly& fly,bool random_start) {
    auto& state=*layer.effect_state;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const auto rnd=[&](int n){return static_cast<int>(random_next(state.seed)%static_cast<unsigned>(std::max(1,n)));};
    fly.x=static_cast<float>(clamp_int(static_cast<std::int64_t>(p(8))+rnd(p(10))));
    fly.y=static_cast<float>(clamp_int(static_cast<std::int64_t>(p(9))+rnd(p(11))));
    const auto ratio=[&](int r) {
        return r>0?std::max(1.0F,100.0F+static_cast<float>(rnd(clamp_int(static_cast<std::int64_t>(r)*2)))-
            static_cast<float>(r/2))/100.0F:1.0F;
    };
    fly.speed_x=static_cast<float>(p(12))*ratio(p(14));
    fly.speed_y=static_cast<float>(p(13))*ratio(p(14));
    fly.period=clamp_int(static_cast<std::int64_t>(rnd(clamp_int(static_cast<std::int64_t>(p(21))*4)))+p(21));
    fly.phase=rnd(fly.period);
    fly.in=std::max(0,static_cast<int>(std::clamp(static_cast<double>(p(15))*ratio(p(17)),0.0,10000000.0)));
    fly.out=std::max(0,static_cast<int>(std::clamp(static_cast<double>(p(16))*ratio(p(17)),0.0,10000000.0)));
    const auto keep=std::max<std::int64_t>(1,static_cast<std::int64_t>(p(0))-fly.in-fly.out);
    fly.keep=std::max(1,static_cast<int>(std::clamp(static_cast<double>(keep)*ratio(p(17)),0.0,10000000.0)));
    const int life=clamp_int(static_cast<std::int64_t>(fly.in)+fly.keep+fly.out);
    fly.frame=random_start?rnd(life):0;
}
void firefly_tick(Layer& layer,int width,int height) {
    const auto p=[&](std::size_t i){return value(layer,i);};
    const double angle=p(22)*(3.14159*2/360.0);
    for (auto& fly:layer.effect_state->fireflies) {
        if (!fly.period) { if (p(20)) firefly_spawn(layer,fly,false);continue; }
        ++fly.frame;
        if (fly.frame>fly.in+fly.keep+fly.out) {
            if (!p(19)) { fly.period=0;continue; }
            fly.frame=0;
        }
        fly.x+=fly.speed_x;fly.y+=fly.speed_y;
        fly.phase=(fly.phase+1)%fly.period;
        const double wave=std::cos(static_cast<double>(fly.phase)*3.14159*2/fly.period)*p(23);
        if (p(22)==-1) {
            if (std::abs(fly.speed_x)>std::abs(fly.speed_y)) fly.y+=static_cast<float>(wave*0.2+0.5);
            else fly.x+=static_cast<float>(wave*0.2+0.5);
        } else {
            fly.x+=static_cast<float>(wave*0.2*std::cos(angle)+0.5);
            fly.y+=static_cast<float>(wave*0.2*std::sin(angle)+0.5);
        }
        if ((fly.speed_x<0 ? fly.x<static_cast<float>(-static_cast<std::int64_t>(p(2))/2) :
                             fly.x>static_cast<float>(static_cast<std::int64_t>(width)+p(2)/2)) ||
            (fly.speed_y<0 ? fly.y<static_cast<float>(-static_cast<std::int64_t>(p(3))/2) :
                             fly.y>static_cast<float>(static_cast<std::int64_t>(height)+p(3)/2))) fly.period=0;
    }
}
void fireflies(Surface& out,const Layer& layer) {
    if (!layer.texture || !layer.texture->has_mask) return;
    const auto& t=*layer.texture;
    const auto p=[&](std::size_t i){return value(layer,i);};
    if (p(2)<=0 || p(3)<=0) return;
    const int cols=t.canvas_width/p(2),rows=t.canvas_height/p(3);
    if (cols<=0 || rows<=0) return;
    for (std::size_t i=0;i<layer.effect_state->fireflies.size();++i) {
        const auto& fly=layer.effect_state->fireflies[i];
        if (!fly.period) continue;
        const int x=static_cast<int>(std::clamp(static_cast<double>(fly.x),-10000000.0,10000000.0))-p(2)/2;
        const int y=static_cast<int>(std::clamp(static_cast<double>(fly.y),-10000000.0,10000000.0))-p(3)/2;
        int frame=fly.frame;
        if (p(5)>1) frame/=p(5);
        const int sx=(frame%cols)*p(2),sy=static_cast<int>(i%static_cast<std::size_t>(rows))*p(3);
        int percent=p(6);
        if (fly.frame<fly.in) percent=clamp_int(static_cast<std::int64_t>(p(6))+
            (static_cast<std::int64_t>(p(7))-p(6))*fly.frame/std::max(1,fly.in));
        else if (fly.frame<fly.in+fly.keep) percent=p(7);
        else if (fly.frame<fly.in+fly.keep+fly.out) percent=clamp_int(static_cast<std::int64_t>(p(7))-
            (static_cast<std::int64_t>(p(7))-p(6))*(fly.frame-fly.in-fly.keep)/std::max(1,fly.out));
        percent=std::min(std::max(percent,p(6)),p(7));
        if (!percent) continue;
        const auto signed_percent=static_cast<std::int64_t>(percent)*(p(4)==1?-1:1);
        const bool subtract=signed_percent<0;
        const int scale=static_cast<int>(std::min<std::int64_t>(65535,std::abs(signed_percent)*256/100));
        const auto right=std::min<std::int64_t>(out.width,static_cast<std::int64_t>(x)+p(2));
        const auto bottom=std::min<std::int64_t>(out.height,static_cast<std::int64_t>(y)+p(3));
        for (int yy=std::max(0,y);yy<bottom;++yy) for (int xx=std::max(0,x);xx<right;++xx) {
            const auto src=texel(t,sx+xx-x,sy+yy-y);
            if (!src[3]) continue;
            const auto at=(static_cast<std::size_t>(yy)*out.width+static_cast<unsigned>(xx))*3;
            for (std::size_t c=0;c<3;++c) {
                const int amount=std::min(255,(src[c]*scale)>>8);
                const int target=std::clamp(out.rgb[at+c]+(subtract?-amount:amount),0,255);
                out.rgb[at+c]=static_cast<Byte>(((target*src[3])>>8)+((out.rgb[at+c]*(256-src[3]))>>8));
            }
        }
    }
}
void suna_tick(Layer& layer) {
    auto& state=*layer.effect_state;
    const int w=std::max(0,value(layer,8)&~7),h=std::max(0,value(layer,9)&~1);
    const auto words=static_cast<std::size_t>(w)*static_cast<unsigned>(h)/16U;
    state.noise_phase^=1U;
    for (std::size_t i=0;i<words/2 && i*2+state.noise_phase<state.noise.size();++i)
        state.noise[i*2+state.noise_phase]^=(random_next(state.seed)&0x7fffU)|((random_next(state.seed)&0x7fffU)<<16U);
}
void suna(Surface& out,const Layer& layer) {
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int left=std::max(0,p(6)&~7),top=std::max(0,p(7)&~1);
    const int w=std::min(std::max(0,p(8)&~7),static_cast<int>(out.width)-left);
    const int h=std::min(std::max(0,p(9)&~1),static_cast<int>(out.height)-top);
    if (w<8 || h<2) return;
    const int alpha=p(1)?std::clamp(interpolate(layer,clamp_int(static_cast<std::int64_t>(p(3))*256/100),
        clamp_int(static_cast<std::int64_t>(p(2))*256/100),p(4),p(5)),0,256):256;
    if (!alpha) return;
    const auto& noise=layer.effect_state->noise;
    std::size_t index=0;
    for (int y=0;y<h/2;++y) for (int x=0;x<w/8;++x) {
        if (index>=noise.size()) return;
        const auto word=noise[index++]<<1U;
        for (int k=0;k<4;++k) {
            const int gray=static_cast<int>((word>>(k*8))&255U);
            for (int dy=0;dy<2;++dy) for (int dx=0;dx<2;++dx) {
                const auto at=(static_cast<std::size_t>(top+y*2+dy)*out.width+static_cast<unsigned>(left+x*8+k*2+dx))*3;
                for (std::size_t c=0;c<3;++c)
                    out.rgb[at+c]=static_cast<Byte>(alpha==256?gray:
                        ((gray*alpha)>>8)+((out.rgb[at+c]*(256-alpha))>>8));
            }
        }
    }
}
void rain2(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const auto& t=*layer.texture;
    const int w=t.canvas_width,h=t.canvas_height;
    if (w<=0 || h<=0) return;
    for (int n=value(layer,3)-1;n>=0;--n) {
        const auto& plane=layer.effect_state->rain[static_cast<std::size_t>(n)];
        const int x=clamp_int(plane.x/100),y=clamp_int(plane.y/100);
        const int start_x=x>0?-1:0,start_y=y>0?-1:0;
        const auto end_x=x+w<static_cast<int>(out.width)?
            (static_cast<std::int64_t>(out.width)-x-w+w-1)/w:0;
        const auto end_y=y+h<static_cast<int>(out.height)?
            (static_cast<std::int64_t>(out.height)-y-h+h-1)/h:0;
        for (auto j=static_cast<std::int64_t>(start_y);j<=end_y;++j)
            for (auto i=static_cast<std::int64_t>(start_x);i<=end_x;++i)
                normal_picture(out,t,{0,0,w,h},{clamp_int(static_cast<std::int64_t>(x)+i*w),
                    clamp_int(static_cast<std::int64_t>(y)+j*h),w,h},true);
    }
}
void rain4(Surface& out,const Layer& layer) {
    if (!layer.texture) return;
    const auto& t=*layer.texture;
    if (t.canvas_width<=0 || t.canvas_height<=0) return;
    const auto p=[&](std::size_t i){return value(layer,i);};
    const int count=p(1),high=p(2),low=p(3),range=high-low;
    std::array<int,4> distances{};
    int start=0,best=p(4)<0?0:10000;
    for (int i=0;i<count;++i) {
        auto d=static_cast<std::int64_t>(layer.effect_state->distance)+static_cast<std::int64_t>(range)*i/count;
        if (d>high) d=(d-low)%range+low;
        distances[static_cast<std::size_t>(i)]=clamp_int(d);
        if ((p(4)<0 && d>best) || (p(4)>=0 && d<best)) {best=clamp_int(d);start=i;}
    }
    const auto key=texel(t,0,0);
    for (int n=0;n<count;++n) {
        int d=distances[static_cast<std::size_t>(start)];
        if (!d) d=1;
        start=(start+(p(4)<0?-1:1)+count)%count;
        if (d<0) continue;
        int alpha=100;
        if (p(8)>0) {
            const auto from_low=static_cast<std::int64_t>(d)-low,from_high=static_cast<std::int64_t>(high)-d;
            if (from_low<p(8)) alpha=clamp_int(from_low*(100-static_cast<std::int64_t>(p(7)))/p(8)+p(7));
            else if (from_high<p(8)) alpha=clamp_int(from_high*(100-static_cast<std::int64_t>(p(7)))/p(8)+p(7));
        }
        alpha=std::clamp(alpha,1,100);
        const int w=clamp_int(static_cast<std::int64_t>(t.canvas_width)*100/d);
        const int h=clamp_int(static_cast<std::int64_t>(t.canvas_height)*100/d);
        if (w<=0 || h<=0) continue;
        const int x0=p(5)-w/2,y0=p(6)-h/2;
        const auto first_x=x0>0?-((static_cast<std::int64_t>(x0)+w-1)/w):0;
        const auto first_y=y0>0?-((static_cast<std::int64_t>(y0)+h-1)/h):0;
        const auto last_x=x0+w<static_cast<int>(out.width)?
            (static_cast<std::int64_t>(out.width)-x0-w+w-1)/w:0;
        const auto last_y=y0+h<static_cast<int>(out.height)?
            (static_cast<std::int64_t>(out.height)-y0-h+h-1)/h:0;
        const auto step_x=static_cast<std::int64_t>(t.canvas_width)*65536/w;
        const auto step_y=static_cast<std::int64_t>(t.canvas_height)*65536/h;
        for (auto j=first_y;j<=last_y;++j) for (auto i=first_x;i<=last_x;++i) {
            const auto tx=static_cast<std::int64_t>(x0)+i*w,ty=static_cast<std::int64_t>(y0)+j*h;
            const int left=static_cast<int>(std::max<std::int64_t>(0,tx));
            const int top=static_cast<int>(std::max<std::int64_t>(0,ty));
            const int right=static_cast<int>(std::min<std::int64_t>(out.width,tx+w));
            const int bottom=static_cast<int>(std::min<std::int64_t>(out.height,ty+h));
            const auto skip_x=(left-tx)*t.canvas_width*65536/w;
            const auto skip_y=(top-ty)*t.canvas_height*65536/h;
            for (int y=top;y<bottom;++y) for (int x=left;x<right;++x) {
                const int sx=static_cast<int>((skip_x+(x-left)*step_x)>>16);
                const int sy=static_cast<int>((skip_y+(y-top)*step_y)>>16);
                const auto src=texel(t,sx,sy);
                if (t.has_mask ? !src[3] : (src[0]==key[0] && src[1]==key[1] && src[2]==key[2])) continue;
                const int a=t.has_mask?(alpha==100?src[3]:(src[3]*(alpha*256/100))>>8):alpha*256/100;
                const auto at=(static_cast<std::size_t>(y)*out.width+static_cast<unsigned>(x))*3;
                for (std::size_t c=0;c<3;++c) out.rgb[at+c]=static_cast<Byte>(
                    alpha==100 && (!t.has_mask || a==255)?src[c]:
                    ((src[c]*a)>>8)+((out.rgb[at+c]*(255-a))>>8));
            }
        }
    }
}
}

bool known_v5(int number) {
    switch (number) {
    case 13:case 20:case 22:case 23:case 24:case 29:case 31:case 37:case 40:case 43:case 50:case 55:case 56:case 60:case 62:case 64:case 67:case 71:case 74:case 77:case 81:case 86:case 94:case 102:case 106:case 109:case 110:case 112:case 119:case 122:case 123:case 124:case 125:case 126:case 127:case 128:case 129:case 131:case 136:case 138:case 139:case 140:return true;
    default:return false;
    }
}

bool known(int number) {
    if (known_v5(number) || geometry_effect(number)) return true;
    switch (number) {
    case 12:case 14:case 15:case 18:case 19:case 26:case 27:case 28:case 32:case 33:case 36:case 39:case 42:
    case 48:case 51:case 57:case 59:case 63:case 68:case 69:case 70:case 72:case 75:case 76:case 78:
    case 79:case 80:case 82:case 83:case 84:case 85:case 90:case 91:case 92:case 93:case 96:case 97:
    case 98:case 101:case 103:case 104:case 105:case 107:case 108:case 111:case 120:case 121:
    case 130:case 132:case 133:case 137:return true;
    default:return false;
    }
}

bool continuous(int number) {
    switch (number) {
    case 12:case 13:case 14:case 15:case 24:case 26:case 29:case 42:case 51:case 55:case 56:case 57:case 71:
    case 83:case 85:case 91:case 92:case 93:case 96:case 102:case 106:case 109:case 132:case 138:return true;
    default:return false;
    }
}

void configure(Layer& layer,unsigned slot,int width,int height,const FragmentMesh* mesh) {
    const auto given=layer.parameters.size();
    if (!layer.effect_state) {
        layer.effect_state=std::make_shared<State>();
        layer.effect_state->seed=0x6d2b79f5U^(slot*0x9e3779b9U);
    }
    layer.parameters.resize(std::max<std::size_t>(given,36),0);
    auto& p=layer.parameters;
    const auto def=[&](std::size_t index,int v){if (given<=index) p[index]=v;};
    switch (layer.effect) {
    case 12:case 14:case 57: {
        const bool flower=layer.effect==12;
        def(0,20);def(1,0);def(2,flower?2:5);def(3,100);
        auto& particles=layer.effect_state->particles;
        particles.resize(flower?200U:100U);
        for (std::size_t i=0;i<particles.size();++i) falling_spawn(layer,particles[i],width,height,i);
        falling_tick(layer,width,height);
        break;
    }
    case 15:
        def(0,15);def(1,0);def(2,0);
        layer.effect_state->particles.resize(7);
        for (auto& ray:layer.effect_state->particles) {
            ray.phase=20+random_below(layer,40);
            ray.reverse=true;
        }
        rays_tick(layer);
        break;
    case 18:
        def(0,20);def(1,0);def(2,0);break;
    case 19:
        def(0,20);def(1,4);def(2,1);def(3,0);def(4,12);def(5,0);
        def(6,100);def(7,p[6]);def(8,0);def(9,10);def(10,0);
        p[1]=std::max(1,p[1]);p[2]=std::max(1,p[2]);break;
    case 26:
        layer.effect_state->distance=1;break;
    case 27:case 108:
        p[0]=999999;def(1,32);def(2,32);def(3,1);def(4,0);def(5,1);def(6,1);def(7,1);
        def(8,30);def(9,0);def(10,10);def(11,100);
        p[3]=std::clamp(p[3],0,16);
        p.resize(std::max(p.size(),16U+8U*static_cast<std::size_t>(p[3])));
        if (given<224) for (std::size_t i=0;i<packed_count(layer,3,8,16);++i)
            sprite_spawn(layer,i,width,height,true);
        break;
    case 28:
        def(0,20);def(1,width/2);def(2,height/2);def(3,60);
        def(4,static_cast<int>(std::sqrt(static_cast<double>(width)*width+static_cast<double>(height)*height/4.0)));
        def(5,0);def(6,p[5]);def(7,p[6]);break;
    case 32:
        def(0,20);def(1,0);def(2,0);break;
    case 33:
        def(0,20);def(1,0);def(2,100);def(3,0);def(4,0);break;
    case 36:
        def(0,20);def(1,width/4);def(2,height/4);
        p[1]=std::clamp(p[1],0,width-1);p[2]=std::clamp(p[2],0,height-1);
        def(3,width-2*p[1]);def(4,height-2*p[2]);def(5,0);def(6,0);break;
    case 39:
        def(0,20);def(1,32);def(2,0);def(3,0);break;
    case 42: {
        def(1,20);def(2,32);def(3,32);def(4,width-64);def(5,height-64);
        p[1]=std::clamp(p[1],1,20);
        p[2]=std::min(std::max(32,p[2]),width-32);p[3]=std::min(std::max(32,p[3]),height-32);
        p[4]=std::max(1,std::min(p[4],width-33-p[2]));p[5]=std::max(1,std::min(p[5],height-33-p[3]));
        auto& particles=layer.effect_state->particles;
        particles.resize(static_cast<std::size_t>(p[1]));
        for (auto& particle:particles) {
            sparkle_spawn(layer,particle,width,height);
            particle.phase=random_below(layer,particle.period);
        }
        sparkles_tick(layer,width,height);
        break;
    }
    case 48:
        def(0,20);def(1,0);def(2,8);def(3,p[2]);def(4,0);def(5,0);def(6,0);
        def(7,50);def(8,p[7]);def(9,0);def(10,0);
        if (p[1]>=p[0]) p[1]=std::max(0,p[0])-1;
        break;
    case 51:
        def(0,20);def(1,10);def(2,120);def(3,height-40);def(4,-100);def(5,-100);def(6,width-200);
        def(7,height*3/4);def(8,width/2);def(9,height/2);def(10,2);
        p[1]=std::clamp(p[1],1,20);
        flare_init(layer);break;
    case 59:
        def(0,20);def(1,0);def(2,13);def(3,0);def(4,0);def(5,100);def(6,0);def(7,0);def(8,0);
        p[1]=std::max(0,p[1]);break;
    case 63:
        def(0,20);def(1,64);def(2,64);def(3,1);def(4,0);def(5,0);def(6,p[4]);def(7,p[5]);
        def(8,0);def(9,0);def(10,100);def(11,100);def(12,0);def(13,0);def(14,0);break;
    case 68:
        def(0,20);def(1,100);def(2,100);def(3,0);def(4,p[3]);def(5,p[4]);def(6,0);def(7,0);
        def(8,p[3]);def(9,p[4]);def(10,p[5]);def(11,0);def(12,0);break;
    case 69:
        def(0,20);def(1,0);def(2,width/2);def(3,height/2);def(4,20);def(5,30);def(6,10);def(7,8);
        def(8,100);def(9,100);def(10,0);def(11,0);def(12,100);def(13,0);def(14,0);def(15,0);def(16,0);
        break;
    case 70: {
        def(0,20);def(1,100);def(2,100);def(3,100);def(4,0);def(5,1);def(6,600);def(7,300);
        def(8,100);def(9,-100);def(10,10);def(11,0);def(12,0);def(13,0);def(14,100);def(15,100);
        def(16,100);def(17,50);def(18,1);def(19,1);def(20,0);def(21,0);def(22,1);
        p[13]=std::clamp(p[13],0,299);p[1]=std::clamp(p[1],1,300-p[13]);
        auto& particles=layer.effect_state->particles;
        const auto count=static_cast<std::size_t>(p[1]);
        const auto existing=particles.size();
        particles.resize(std::max(existing,count));
        for (std::size_t i=0;i<count;++i) {
            if (!p[12]) petal_spawn(layer,particles[i]);
            else if (i>=existing) particles[i].variant=-1;
        }
        petals_tick(layer,width,height);
        break;
    }
    case 72:
        def(0,20);def(1,10);def(2,5);def(3,p[1]);def(4,p[2]);def(5,0);def(6,0);break;
    case 75:
        def(0,20);def(1,0);def(2,1);break;
    case 76:
        def(0,20);def(1,0);def(2,0);def(3,0);def(4,32);
        p[4]=std::clamp(p[4],1,255);def(5,p[4]+4);p[5]=std::max(p[5],p[4]+4);break;
    case 78:
        def(0,20);def(1,0);def(2,128);def(3,p[2]);def(4,p[3]);def(5,p[2]);def(6,p[3]);def(7,p[4]);
        def(8,-1);def(9,0);def(10,0);
        p[1]=std::clamp(p[1],0,31);p[8]=std::clamp(p[8],-1,31);break;
    case 79:
        def(0,20);def(1,0);def(2,1);def(3,0);break;
    case 80:
        def(0,20);def(1,1);def(2,0);def(3,50);def(4,100);def(5,100);def(6,50);def(7,0);
        def(8,100);def(9,200);def(10,280);def(11,380);def(12,1);def(13,0);
        if (p[12]==2) p[13]=1;
        else if (p[12]>2) p[13]=std::max(2,p[13]);
        break;
    case 82:
        def(0,9999);def(1,200);def(2,0);def(3,50);def(4,200);def(5,100);def(6,500);def(7,100);
        def(8,0);def(9,0);def(10,0);def(11,p[10]);def(12,p[11]);def(13,0);
        break;
    case 83:
        def(0,20);def(1,20);def(2,128);def(3,192);def(4,255);def(5,0);def(6,height);def(7,30);def(8,43);
        def(9,300);def(10,250);def(11,100);def(12,100);def(13,700);def(14,100);
        p[7]=std::max(1,p[7]);p[8]=std::max(1,p[8]);
        if (layer.effect_state->particles.empty()) {
            layer.effect_state->particles.resize(2);
            for (auto& wave:layer.effect_state->particles) {
                wave.x=random_below(layer,8192);wave.y=random_below(layer,8192);
                wave.variant=random_below(layer,8192);
            }
            for (auto& wave:layer.effect_state->particles) wave.row=random_below(layer,2048);
        }
        break;
    case 84:
        def(0,20);def(1,160);def(2,13);def(3,0);def(4,0);break;
    case 85:
        def(0,20);def(1,10);def(2,100);def(3,3);def(4,30);def(5,255);def(6,3);def(7,3);
        def(8,0);def(9,0);def(10,width);def(11,height);
        p[1]=std::clamp(p[1],0,100);p[2]=std::max(81,p[2]);
        p[8]=std::clamp(p[8],0,width-1);p[9]=std::clamp(p[9],0,height-1);
        p[10]=std::min(p[10],width-p[8]);p[11]=std::min(p[11],height-p[9]);
        if (p[11]<3) { p[9]=std::max(0,p[9]-2);p[11]=3; }
        layer.effect_state->particles.resize(static_cast<std::size_t>(p[1]));
        for (auto& streak:layer.effect_state->particles) speed_streak_spawn(layer,streak,true);
        break;
    case 90:
        def(0,20);def(1,255);def(2,p[1]);def(3,p[2]);def(4,1);def(5,1);break;
    case 91:
        def(0,20);def(1,0);def(2,0);def(3,24);def(4,-1);def(5,100);def(6,20);def(7,2);
        layer.effect_state->distance=clamp_int(layer.count);
        break;
    case 92:
        def(0,20);def(1,20);def(2,10);def(3,10);def(4,50);def(5,2);def(6,1);def(7,-1);def(8,0);
        def(9,0);def(10,1);def(11,1);def(12,0);break;
    case 93:
        def(0,20);def(1,height/2);def(2,height/2);def(3,500);def(4,100);def(5,0);def(6,100);
        def(7,5);def(8,100);def(9,50);def(10,20);def(11,-1);
        p[1]=std::clamp(p[1],0,height-1);p[2]=std::clamp(p[2],1,height-p[1]);
        p[4]=std::clamp(p[4],1,std::numeric_limits<int>::max()-1);
        p[3]=std::max(p[3],p[4]+1);
        p[7]=std::clamp(p[7],0,20);
        layer.effect_state->particles.resize(20);
        for (auto& wave:layer.effect_state->particles) grass_gust_spawn(layer,wave,true);
        break;
    case 96:
        def(0,20);def(1,100);def(2,p[1]);def(3,0);def(4,0);def(5,0);def(6,1);def(7,1);break;
    case 97:
        def(0,20);def(1,20);def(3,5);def(4,0);def(5,0);def(6,width);def(7,height);def(8,0);
        p[1]=p[1]>64?128:std::max(8,p[1]);p[2]=1;
        p[4]=std::clamp(p[4],0,width-1);p[5]=std::clamp(p[5],0,height-1);
        p[6]=std::clamp(p[6],1,width);p[7]=std::clamp(p[7],1,height);
        layer.effect_state->offset_x=-1;layer.effect_state->offset_y=-1;break;
    case 98:
        def(0,20);def(1,32);def(2,32);def(3,2);def(4,-1);def(5,0);def(6,0);def(7,1);
        def(8,40);def(9,80);def(10,4);def(11,80);def(12,0);
        if (given<224 || p[14]<1) new_face_cycle(layer);
        break;
    case 101:
        def(0,20);def(1,12);def(2,13);def(3,0);def(4,0);def(5,0);def(6,0);def(7,0);def(8,100);def(9,0);break;
    case 103:
        def(0,20);def(1,width/4);def(2,height/4);def(3,width/2);def(4,height/2);def(5,1);
        def(6,255);def(7,255);def(8,255);def(9,0);def(10,0);def(11,0);def(12,0);def(13,50);
        p[1]=std::clamp(p[1],0,width-1);p[2]=std::clamp(p[2],0,height-1);
        p[3]=std::clamp(p[3],1,width-p[1]);p[4]=std::clamp(p[4],1,height-p[2]);
        p[5]=p[6]>height?height:std::clamp(p[5],1,width);break;
    case 104:
        p[0]=999999;def(1,32);def(2,32);def(3,1);def(4,1);def(5,100);def(6,3);def(7,7);def(8,15);
        def(9,0);def(10,1000);def(11,0);def(12,width/2);def(13,height/2);
        layer.effect_state->distance=clamp_int(layer.count);
        break;
    case 105:
        def(0,20);def(1,width/2);def(2,height/2);def(3,180);def(4,0);def(5,0);def(6,0);
        def(7,100);def(8,p[7]);def(9,0);def(10,0);def(11,100);def(12,p[11]);def(13,0);def(14,0);break;
    case 107:
        def(0,20);def(1,32);def(2,10);def(3,100);def(4,0);def(5,0);def(6,100);def(7,p[6]);def(8,0);def(9,0);break;
    case 111:
        p[0]=999999;def(1,1);def(2,32);def(3,32);def(4,0);def(5,0);def(6,width);def(7,height);
        def(8,1);def(9,1);def(10,1);def(11,1);def(12,3);def(13,100);
        p[1]=std::clamp(p[1],0,40);p[12]=std::max(p[11],p[12]);
        p.resize(std::max(p.size(),16U+4U*static_cast<std::size_t>(p[1])));
        if (given<224) for (std::size_t i=0;i<packed_count(layer,1,4,40);++i) fish_spawn(layer,i);
        break;
    case 113:
        def(0,20);def(1,0);def(2,p[1]);def(3,p[2]);break;
    case 121:
        def(0,20);def(1,13);def(2,100);def(3,p[2]);def(4,0);def(5,0);
        def(6,0);def(7,0);def(8,0);def(9,255);def(10,255);def(11,255);break;
    case 133:
        def(0,20);def(1,32);def(2,32);def(3,0);def(4,0);def(5,1);def(6,0);def(7,0);break;
    case 120:
        def(0,20);def(1,0);def(2,p[1]);def(3,p[2]);
        for (std::size_t c=0;c<3;++c) { def(4+c,p[1+c]);def(10+c,0); }
        def(7,1);def(8,0);def(9,0);def(13,0);break;
    case 130:
        def(0,20);def(1,0);def(2,0);
        for (std::size_t c=0;c<3;++c) {
            def(3+c,200);def(6+c,p[3+c]);def(9+c,0);def(12+c,p[9+c]);
        }
        def(15,p[1]);def(16,p[2]);break;
    case 132:
        def(0,20);def(1,4);def(2,4);def(3,8);def(4,50);def(5,50);def(6,0);def(7,0);
        p[1]=std::max(0,p[1]);p[2]=std::max(0,p[2]);
        // A complete PC layer record already contains the live jitter interval.
        if (given<224) {
            std::fill_n(p.begin()+20,6,0);
            chara_bure_tick(layer);
        }
        break;
    case 137:
        def(0,20);def(1,4);def(2,0);def(3,1);def(4,1);def(5,5);def(6,0);def(7,0);
        p[1]=std::max(1,p[1]);p[4]=std::max(1,p[4]);
        if (given<224) { p[13]=0;p[15]=0; }
        break;
    case 13:
        def(0,20);def(1,30);def(2,100);def(3,60);def(4,80);def(5,255);def(6,p[5]);def(7,p[6]);
        p[1]=std::clamp(p[1],-1000000,1000000);p[2]=std::clamp(p[2],0,150);
        p[3]=std::clamp(p[3],1,1000000);p[4]=std::clamp(p[4],1,1000000);
        layer.effect_state->drops.resize(static_cast<std::size_t>(p[2]));
        for (auto& drop:layer.effect_state->drops) rain_spawn(layer,drop,width,height,true);
        break;
    case 20:
        def(0,20);def(1,64);def(2,p[1]);def(3,p[2]);def(4,0);def(5,0);break;
    case 22:
        def(0,9999);def(1,0);def(2,height);p[1]=std::clamp(p[1],0,height-1);p[2]=std::clamp(p[2],0,height-p[1]);
        def(3,0);def(4,100);def(5,100);def(6,100);def(7,9);def(8,7);def(9,11);
        def(10,4);def(11,17);def(12,-23);def(13,6);def(14,2);def(15,-5);break;
    case 23:
        def(0,60);def(1,0);def(2,64);def(3,0);def(4,0);
        if (p[1]>=width/2) p[1]=width/2-1;
        if (p[2]>=width/2) p[2]=width/2-1;
        break;
    case 24:
        def(0,20);def(1,height/2);def(2,100);def(3,0);p[1]=std::clamp(p[1],0,height-1);break;
    case 29:
        def(0,0);def(1,0);def(2,100);def(6,0);def(7,0);def(8,width);def(9,height);
        def(3,p[6]);def(4,0);def(5,0);
        p[6]=std::clamp(p[6],0,width-1);p[7]=std::clamp(p[7],0,height-1);
        p[8]=static_cast<int>(std::clamp(static_cast<std::int64_t>(p[8]),std::int64_t{0},static_cast<std::int64_t>(width-p[6])));
        p[9]=static_cast<int>(std::clamp(static_cast<std::int64_t>(p[9]),std::int64_t{0},static_cast<std::int64_t>(height-p[7])));
        layer.effect_state->noise.resize(static_cast<std::size_t>(width)*static_cast<unsigned>(height)/16U);
        for (auto& n:layer.effect_state->noise)
            n=(random_next(layer.effect_state->seed)&0x7fffU)|((random_next(layer.effect_state->seed)&0x7fffU)<<16U);
        suna_tick(layer);
        break;
    case 31:case 124:case 127:case 128:case 129:
        def(0,20);def(1,0);def(2,0);def(3,100);def(4,0);
        def(5,107);def(6,74);def(7,43);break;
    case 131:
        def(0,20);def(1,100);p[1]=std::clamp(p[1],-9999,9999);
        def(2,p[1]);p[2]=std::clamp(p[2],-9999,9999);def(3,0);def(4,0);break;
    case 40:
        def(0,20);def(1,13);def(2,100);def(3,p[2]);def(4,0);def(5,0);def(6,0);def(7,0);break;
    case 37:case 122:
        def(0,20);def(1,width/4);def(2,height/4);def(3,width/2);def(4,height/2);
        p[1]=std::clamp(p[1],0,width-1);p[2]=std::clamp(p[2],0,height-1);
        if (static_cast<std::int64_t>(p[1])+p[3]>width) p[3]=width-p[1];
        if (static_cast<std::int64_t>(p[2])+p[4]>height) p[4]=height-p[2];
        def(5,0);def(6,0);def(7,100);def(8,0);
        if (layer.effect==37) break;
        def(9,clamp_int(static_cast<std::int64_t>(p[1])+p[3]/2));
        def(10,clamp_int(static_cast<std::int64_t>(p[2])+p[4]/2));
        def(11,-60);def(12,-40);def(13,0);def(14,p[9]);def(15,p[10]);def(16,p[11]);def(17,p[12]);
        def(18,0);def(19,0);def(20,0);def(21,100);def(22,255);def(23,255);def(24,255);
        def(25,p[21]);def(26,0);def(27,0);def(28,p[22]);def(29,p[23]);def(30,p[24]);
        def(31,p[26]);def(32,p[27]);break;
    case 50:
        def(0,20);def(1,0);def(2,0);def(3,0);def(4,255);def(5,255);def(6,255);
        for (std::size_t i=7;i<=14;++i) def(i,0);
        def(15,0);def(16,height);p[15]=std::clamp(p[15],0,height-1);
        if (static_cast<std::int64_t>(p[15])+p[16]>height) p[16]=height-p[15];
        break;
    case 55:
        def(1,0);def(2,0);def(3,width);def(4,height);def(5,3);def(6,1);
        p[5]=std::clamp(p[5],0,10);p[6]=std::clamp(p[6],1,std::max(1,std::min({4,width,height})));
        p[1]=std::clamp(p[1],0,width-p[6]);p[2]=std::clamp(p[2],0,height-p[6]);
        if (static_cast<std::int64_t>(p[1])+p[3]>width) p[3]=width-p[1];
        if (static_cast<std::int64_t>(p[2])+p[4]>height) p[4]=height-p[2];
        def(7,128);def(8,128);def(9,128);def(10,200);def(11,80);def(12,30);def(13,100);def(14,100);
        layer.effect_state->scratches.resize(static_cast<std::size_t>(p[5]));
        for (auto& scratch:layer.effect_state->scratches) scratch_spawn(layer,scratch);
        scratch_tick(layer);break;
    case 56:
        def(0,20);def(1,5);def(2,30);def(3,2);def(4,3);def(5,0);def(6,50);def(7,100);
        p[3]=std::clamp(p[3],1,4);p[7]=std::max(1,p[7]);
        for (int i=0;i<p[3];++i) {
            auto& plane=layer.effect_state->rain[static_cast<std::size_t>(i)];
            if (!i) {
                plane.speed_x=clamp_int(static_cast<std::int64_t>(p[1])*100);
                plane.speed_y=clamp_int(static_cast<std::int64_t>(p[2])*100);
                plane.random_x=clamp_int(static_cast<std::int64_t>(p[4])*100);
                plane.random_y=clamp_int(static_cast<std::int64_t>(p[5])*100);
            } else {
                const auto& prior=layer.effect_state->rain[static_cast<std::size_t>(i-1)];
                plane.speed_x=clamp_int(prior.speed_x*p[6]/100);plane.speed_y=clamp_int(prior.speed_y*p[6]/100);
                plane.random_x=clamp_int(prior.random_x*p[6]/100);plane.random_y=clamp_int(prior.random_y*p[6]/100);
            }
        }
        break;
    case 43:
        def(0,10000);def(1,16);def(2,16);
        if (given<7) { p[3]=0;p[4]=0;p[5]=width;p[6]=height; }
        break;
    case 60:
        def(0,20);def(1,12);def(2,13);def(3,1);def(4,0);def(5,0);def(6,0);def(7,0);def(8,100);def(9,0);break;
    case 62:
        def(0,1);def(1,0);def(2,0);def(3,200);def(4,-100);def(5,300);def(6,-200);def(7,400);def(8,100);def(9,0);def(10,0);break;
    case 64:
        def(0,20);def(1,0);def(2,p[1]);def(3,0);def(4,0);
        if (given<7) p[5]=100;
        if (given<8) p[6]=p[5];
        def(7,0);def(8,100);def(9,p[8]);
        for (std::size_t i=10;i<=17;++i) def(i,0);
        def(18,p[17]);def(19,p[18]);def(20,p[17]);def(21,p[18]);def(22,p[19]);
        def(23,0);def(24,0);def(25,0);def(26,0);def(27,0);def(28,200);def(29,-100);
        def(30,300);def(31,-200);def(32,400);def(33,100);def(34,0);def(35,0);break;
    case 67:
        def(0,20);def(1,0);def(2,0);def(3,100);def(4,100);def(5,0);
        def(6,p[1]);def(7,p[2]);def(8,p[3]);def(9,p[4]);
        def(10,0);def(11,height);def(12,p[10]);def(13,p[11]);break;
    case 74:
        def(0,20);def(1,width/2);def(2,height/2);def(3,105);def(4,p[3]);def(5,20);def(6,p[5]);
        def(7,0);def(8,0);def(9,0);def(10,0);
        p[1]=std::min(std::max(1,p[1]),width-2);p[2]=std::min(std::max(1,p[2]),height-2);
        def(11,p[1]);def(12,p[2]);break;
    case 71:
        def(0,20);def(1,100);def(2,width);def(3,height-180);def(4,100);def(5,100);
        def(6,-2);def(7,-3);def(8,255);def(9,255);def(10,255);def(11,0);def(12,0);
        def(13,0);def(14,100);def(15,15);def(16,-1);def(17,20);
        p[1]=std::clamp(p[1],0,600);p[4]=std::max(1,p[4]);p[5]=std::max(1,p[5]);
        p[12]=std::clamp(p[12],0,600);p[13]=std::clamp(p[13],0,600-p[1]);p[15]=std::clamp(p[15],1,100000000);
        if (layer.effect_state->old_fireflies.empty()) {
            layer.effect_state->old_fireflies.resize(600);
            for (auto& fly:layer.effect_state->old_fireflies) old_firefly_spawn(layer,fly,true);
        }
        if (!p[12]) for (int i=p[13];i<p[13]+p[1];++i)
            old_firefly_spawn(layer,layer.effect_state->old_fireflies[static_cast<std::size_t>(i)]);
        old_firefly_tick(layer,width,height);
        break;
    case 77:case 125:case 126:
        def(0,20);def(1,8);p[1]=std::clamp(p[1],1,256);
        def(2,8);p[2]=std::clamp(p[2],1,256);
        if (p[1]*p[2]>256) p[2]=256/p[1];
        def(3,100);p[3]=std::clamp(p[3],0,100);def(4,p[3]);p[4]=std::clamp(p[4],0,100);
        def(5,0);def(6,0);p[6]=std::max(0,p[6]);def(7,0);def(8,0);break;
    case 81:
        def(0,20);def(1,100);def(2,100);def(3,width/2);def(4,height/2);def(5,0);def(6,100);
        if (p[6]>p[0]) p[6]=p[0];
        def(7,0);def(8,0);def(9,100);def(10,p[9]);def(11,0);def(12,0);
        def(13,0);def(14,0);def(15,0);def(16,0);def(17,0);
        if (!p[13]) for (std::size_t i=14;i<=17;++i) p[i]=0;
        else if (p[13]==2) for (std::size_t i=15;i<=17;++i) p[i]=0;
        shards_init(layer,width,height,mesh);break;
    case 86:
        def(0,20);def(1,0);def(2,100);def(3,width/2);def(4,height/2);def(5,2);def(6,1);
        def(7,0);def(8,0);def(9,100);def(10,0);def(11,0);def(12,0);break;
    case 87:case 115:
        def(0,20);def(1,width/2);def(2,height/2);def(3,100);def(4,100);
        def(5,128);def(6,128);def(7,128);def(8,100);def(9,p[1]);def(10,p[2]);def(11,0);def(12,0);
        def(13,p[3]);def(14,p[4]);def(15,0);def(16,0);def(17,p[5]);def(18,p[6]);def(19,p[7]);
        def(20,0);def(21,0);def(22,p[8]);def(23,0);def(24,0);break;
    case 88:case 95:case 117:
        def(0,0);def(1,p[0]);def(2,p[1]);break;
    case 89:case 116:
        def(0,20);def(1,width/2);def(2,height/2);def(3,100);def(4,100);
        def(5,width/2);def(6,height/2);def(7,50);def(8,50);def(9,128);def(10,128);def(11,128);def(12,100);
        def(13,p[1]);def(14,p[2]);def(15,p[5]);def(16,p[6]);def(17,0);def(18,0);
        def(19,p[3]);def(20,p[4]);def(21,p[7]);def(22,p[8]);def(23,0);def(24,0);
        def(25,p[9]);def(26,p[10]);def(27,p[11]);def(28,0);def(29,0);def(30,p[12]);def(31,0);def(32,0);break;
    case 114:case 118:
        def(0,20);def(1,0);def(2,0);def(3,100);def(4,100);def(5,p[3]);def(6,p[4]);
        def(7,128);def(8,128);def(9,128);def(10,p[7]);def(11,p[8]);def(12,p[9]);
        def(13,width/2);def(14,height/2);def(15,p[13]);def(16,p[14]);
        p[1]=std::max(0,p[1]);p[2]=std::max(0,p[2]);break;
    case 94:
        def(0,0);def(1,32);def(2,0);def(3,1);break;
    case 102:
        def(0,20);def(1,2);def(2,500);def(3,20);def(4,-20);
        def(5,width/2);def(6,height/2);def(7,0);def(8,60);
        p[1]=std::clamp(p[1],0,4);p[2]=std::max(10,p[2]);p[3]=std::max(10,p[3]);
        if (p[3]>=p[2]) p[3]=p[2]-1;
        p[5]=std::clamp(p[5],0,width-1);p[6]=std::clamp(p[6],0,height-1);
        break;
    case 106:
        def(0,20);def(1,2);def(2,32);def(3,2);def(4,40);def(5,50);def(6,50);
        def(7,0);def(8,0);def(9,100);def(10,p[9]);def(11,0);def(12,0);
        p[1]=std::clamp(p[1],0,16);
        layer.effect_state->bands.resize(static_cast<std::size_t>(p[1]));
        for (auto& band:layer.effect_state->bands)
            noise_spawn(layer,band,layer.texture?layer.texture->canvas_height:height);
        noise_raster(layer,width,height);
        break;
    case 109:
        p[0]=999999;def(1,20);def(2,0);def(3,40);def(4,1);def(5,1);def(6,0);
        lamp_visibility(layer);break;
    case 110:
        def(0,20);def(1,200);def(2,0);def(3,100);def(4,-100);def(5,0);def(6,0);break;
    case 123:
        def(0,20);def(1,13);def(2,1);def(3,0);def(4,0);def(5,0);def(6,100);def(7,0);break;
    case 112:
        def(0,20);def(1,0);def(2,64);def(3,25);def(4,0);def(5,0);
        def(6,255);def(7,p[6]);def(8,p[7]);break;
    case 119:
        def(0,1);def(1,0);def(2,0);def(3,200);def(4,-200);def(5,400);def(6,100);def(7,0);def(8,0);break;
    case 140:
        def(0,20);def(1,0);def(2,0);def(3,0);def(4,p[3]);def(5,0);def(6,0);
        def(7,100);def(8,100);def(9,p[7]);def(10,p[8]);def(11,100);def(12,p[11]);
        for (std::size_t i=13;i<=21;++i) def(i,0);
        def(22,p[21]);def(23,p[22]);def(24,p[23]);def(25,p[24]);def(26,0);break;
    case 139:
        def(0,20);def(1,8);def(2,8);def(3,0);def(4,0);def(5,0);def(6,64);def(7,64);
        def(8,0);def(9,0);def(10,0);def(11,width);def(12,height);def(13,1);def(14,0);def(15,100);break;
    case 136:
        def(0,20);def(1,400);def(2,300);def(3,-60);def(4,-40);def(5,0);def(6,100);def(7,p[6]);
        def(8,0);def(9,0);def(10,0);def(11,0);def(12,0);def(13,255);def(14,255);def(15,255);
        def(16,0);def(17,100);def(18,255);def(19,255);def(20,255);def(21,p[17]);def(22,0);def(23,0);
        def(24,p[18]);def(25,p[19]);def(26,p[20]);def(27,0);def(28,0);
        def(29,p[1]);def(30,p[2]);def(31,p[3]);def(32,p[4]);def(33,0);def(34,0);break;
    case 138:
        def(0,20);def(1,100);def(2,16);def(3,16);def(4,0);def(5,1);
        def(6,50);def(7,100);def(8,width/2);def(9,height/2);def(10,100);def(11,100);
        def(12,-2);def(13,-3);def(14,0);def(15,5);def(16,10);def(17,0);def(18,0);
        def(19,0);def(20,1);def(21,15);def(22,-1);def(23,20);
        p[1]=std::clamp(p[1],0,600);p[10]=std::max(1,p[10]);p[11]=std::max(1,p[11]);
        p[21]=std::clamp(p[21],1,100000000);
        layer.effect_state->fireflies.resize(static_cast<std::size_t>(p[1]));
        for (auto& fly:layer.effect_state->fireflies) firefly_spawn(layer,fly,p[18]>0);
        firefly_tick(layer,width,height);
        break;
    default:break;
    }
    static_cast<void>(width);
}

unsigned duration(const Layer& layer) {
    if (layer.effect==12 || layer.effect==24 || layer.effect==26 || layer.effect==42) return 99999;
    if (layer.effect==55) return 9999;
    if (layer.effect==94) return 9999;
    if (layer.effect==88 || layer.effect==95 || layer.effect==97 || layer.effect==98 ||
        layer.effect==113 || layer.effect==117) return 9999;
    return static_cast<unsigned>(std::max(0,value(layer,0)));
}

int animation_frame(const Layer& layer,bool off) {
    const int start=value(layer,2);
    if (off) return start;
    const int index=animation_index(layer.count,value(layer,4),std::max(1,value(layer,1)),
                                    value(layer,3)!=0 || value(layer,7)!=0);
    return clamp_int(static_cast<std::int64_t>(start)+index);
}

void advance(Layer& layer,int width,int height,Point cursor) {
    switch (layer.effect) {
    case 12:case 14:case 57: falling_tick(layer,width,height);return;
    case 15: rays_tick(layer);return;
    case 27:case 108: sprites_tick(layer,width,height);return;
    case 42: sparkles_tick(layer,width,height);return;
    case 51: flare_tick(layer);return;
    case 70: petals_tick(layer,width,height);return;
    case 82: cloud_tick(layer);return;
    case 83: shore_tick(layer);return;
    case 85: speed_streak_tick(layer);return;
    case 91:
        layer.effect_state->distance=layer.effect_state->distance<std::numeric_limits<int>::max()?
            layer.effect_state->distance+1:0;
        return;
    case 92: {
        auto& p=layer.parameters;
        if (p[6] && p[1]>0) layer.count%=static_cast<unsigned>(p[1]);
        p[12]=static_cast<int>((static_cast<std::int64_t>(p[12])+p[11])%std::max(1,p[10]));
        return;
    }
    case 93: grass_gust_tick(layer);return;
    case 96:
        layer.effect_state->offset_y=static_cast<int>((static_cast<std::int64_t>(layer.effect_state->offset_y)+
                                                      value(layer,6))%height);
        return;
    case 97: bath_window_tick(layer,width,height,cursor);return;
    case 104: {
        auto& tick=layer.effect_state->distance;
        tick=tick>=10000?value(layer,11):tick+1;
        layer.count=static_cast<unsigned>(std::max(0,tick));
        return;
    }
    case 111: fish_tick(layer);return;
    default:break;
    }
    if (layer.effect==98) { advance_face(layer);return; }
    if (layer.effect==132) { chara_bure_tick(layer);return; }
    if (layer.effect==26) {
        layer.effect_state->distance=layer.effect_state->distance<100000000?layer.effect_state->distance+1:0;
        return;
    }
    if ((layer.effect==18 || layer.effect==39 || layer.effect==48) && layer.count>=duration(layer)) {
        const bool defeated=layer.effect==48;
        if (!defeated && value(layer,layer.effect==18?2:3)) layer.count=0;
        else { layer.effect=0;layer.enabled=!defeated && static_cast<bool>(layer.texture);layer.effect_state.reset(); }
        return;
    }
    if (layer.effect==19 && value(layer,10)) {
        const auto period=static_cast<std::uint64_t>(std::max(1,value(layer,1)))*
                          static_cast<unsigned>(std::max(1,value(layer,2)));
        layer.count=static_cast<unsigned>(layer.count%period);
        return;
    }
    if (layer.effect==63 && value(layer,14)>0) {
        layer.count%=static_cast<unsigned>(value(layer,14));
        return;
    }
    if (layer.effect==137 && layer.count>=99999U && layer.count>=duration(layer) &&
        value(layer,3) && !value(layer,7)) {
        layer.count=0;
        return;
    }
    if (layer.effect==55) { scratch_tick(layer);return; }
    if (layer.effect==109) { lamp_tick(layer);return; }
    if (layer.effect==22) {
        const unsigned start=static_cast<unsigned>(std::max(0,value(layer,3)));
        if (layer.count>=start) layer.count=(layer.count-start)%9000+start;
        return;
    }
    if (layer.effect==24) { if (++layer.effect_state->water_count>1000000) layer.effect_state->water_count=0;return; }
    if (layer.effect==71) { old_firefly_tick(layer,width,height);return; }
    if (layer.effect==13) { rain_tick(layer,width,height);return; }
    if (layer.effect==106) { noise_tick(layer,width,height);return; }
    if (layer.effect==138) { firefly_tick(layer,width,height);return; }
    if (layer.effect==29) { suna_tick(layer);return; }
    if (layer.effect==56) {
        if (!layer.texture || layer.texture->canvas_width<=0 || layer.texture->canvas_height<=0) return;
        auto& state=*layer.effect_state;
        const int count=value(layer,3),tw=layer.texture->canvas_width,th=layer.texture->canvas_height;
        if (!state.rain_initialized) {
            for (int i=0;i<count;++i) {
                auto& p=state.rain[static_cast<std::size_t>(i)];
                p.x=random_next(state.seed)%static_cast<unsigned>(tw);
                p.y=random_next(state.seed)%static_cast<unsigned>(th);
            }
            state.rain_initialized=true;
        }
        for (int axis=0;axis<2;++axis) for (int i=0;i<count;++i) {
            auto& p=state.rain[static_cast<std::size_t>(i)];
            auto& speed=axis?p.speed_y:p.speed_x;
            const auto jitter=axis?p.random_y:p.random_x;
            if (jitter<=0 || random_next(state.seed)%static_cast<unsigned>(value(layer,7))) continue;
            auto delta=static_cast<std::int64_t>(random_next(state.seed)%static_cast<unsigned>(jitter));
            if (random_next(state.seed)%100<50) delta=-delta;
            const auto base=static_cast<std::int64_t>(value(layer,axis?2:1))*100;
            const auto max_delta=static_cast<std::int64_t>(value(layer,axis?5:4))*100;
            speed=std::max(speed+delta,base-max_delta);
            speed=std::min(speed,base+max_delta);
            if (axis) speed=std::max<std::int64_t>(100,speed);
        }
        const int bound_x=std::min(tw,width),bound_y=std::min(th,height);
        if (bound_x<=0 || bound_y<=0) return;
        for (int i=0;i<count;++i) {
            auto& p=state.rain[static_cast<std::size_t>(i)];
            p.x+=p.speed_x;p.y+=p.speed_y;
            const auto bx=static_cast<std::int64_t>(bound_x)*100,by=static_cast<std::int64_t>(bound_y)*100;
            if (p.x<=-bx) p.x+=bx*10;
            if (p.x>=bx || p.x<=-bx) p.x%=bx;
            if (p.y< -by) p.y+=by*10;
            if (p.y>=by || p.y< -by) p.y%=by;
        }
        return;
    }
    if (layer.effect==102) {
        const auto d=static_cast<std::int64_t>(layer.effect_state->distance)+value(layer,4);
        layer.effect_state->distance=value(layer,4)<0 && d<value(layer,3)?value(layer,2):
            value(layer,4)>=0 && d>value(layer,2)?value(layer,3):clamp_int(d);
        return;
    }
    if (layer.effect==40 && layer.count==duration(layer)) {
        const int trans=interpolate(layer,value(layer,3),value(layer,2),value(layer,6),value(layer,7));
        const int change=interpolate(layer,100,0,value(layer,4),value(layer,5));
        const int first=std::clamp(clamp_int(static_cast<std::int64_t>(change)*trans/100),0,100);
        const int second=std::clamp(clamp_int((100-static_cast<std::int64_t>(change))*trans/100),0,100);
        if (!first && second>0 && second<100) layer.parameters[1]=second;
        else if (!second && first>0 && first<100) layer.parameters[1]=first;
        return;
    }
    if (layer.effect!=94) return;
    const bool endpoint=layer.count>=duration(layer);
    const int step=endpoint && value(layer,3)==0 ? 0 : 1;
    layer.effect_state->offset_x=static_cast<int>((static_cast<std::int64_t>(layer.effect_state->offset_x)+
                                                  static_cast<std::int64_t>(value(layer,1))*step)%width);
    layer.effect_state->offset_y=static_cast<int>((static_cast<std::int64_t>(layer.effect_state->offset_y)+
                                                  static_cast<std::int64_t>(value(layer,2))*step)%height);
    if (endpoint && value(layer,3)!=0) layer.count=0;
}

void draw(Surface& out,const std::array<Layer,32>& layers,unsigned slot,const Surface& saved,
          const AnimationTexture& animation,bool animation_off,bool speaking) {
    const Layer& l=layers[slot];
    const auto p=[&](std::size_t i){return value(l,i);};
    if (geometry_effect(l.effect)) { geometry(out,l,slot);return; }
    switch (l.effect) {
    case 0: ordinary(out,l,slot);return;
    case 12:case 14:case 57: ordinary(out,l,slot);falling_sprites(out,l,animation);return;
    case 15: ordinary(out,l,slot);sun_rays(out,l);return;
    case 18: ordinary(out,l,slot);shake(out,l);return;
    case 19: buffer_animation(out,layers,l);return;
    case 26: ordinary(out,l,slot);remembered_edges(out,l);return;
    case 27:case 108: wandering_sprites(out,l);return;
    case 28: ordinary(out,l,slot);circular_mask(out,l);return;
    case 32: ordinary(out,l,slot);melt_down(out,l);return;
    case 33: ordinary(out,l,slot);relief(out,l);return;
    case 36: stretch(out,l);return;
    case 39: ordinary(out,l,slot);scrolling_picture(out,l);return;
    case 42: ordinary(out,l,slot);sparkles(out,l,animation);return;
    case 48: boss_death(out,l);return;
    case 51: ordinary(out,l,slot);sun_flare(out,l);return;
    case 59: simple_layer_wipe(out,layers,slot,saved);return;
    case 63: sprite_animation(out,l);return;
    case 68: ordinary(out,l,slot);curtain(out,l);return;
    case 69: ripple(out,layers,slot,saved);return;
    case 70: petals(out,l);return;
    case 72: camouflage(out,l);return;
    case 75: once(out,l,slot);return;
    case 76: slash_blur(out,l);return;
    case 78: dual_mask(out,layers,slot);return;
    case 79: full_animation(out,l,animation);return;
    case 80: staff_roll(out,l,slot,animation);return;
    case 82: clouds(out,l);return;
    case 83: ordinary(out,l,slot);shore(out,l);return;
    case 84: fusuma(out,layers,l);return;
    case 85: ordinary(out,l,slot);speed_streaks(out,l);return;
    case 90: character_flash(out,l);return;
    case 91: waterfall(out,layers,slot);return;
    case 92: sway(out,layers,slot);return;
    case 93: grass(out,layers,slot);return;
    case 96: rainbow(out,l);return;
    case 97: bath_window(out,l);return;
    case 98: face_animation(out,layers,slot,speaking);return;
    case 101: moving_mask_wipe(out,layers,slot,saved);return;
    case 103: ordinary(out,l,slot);target(out,l);return;
    case 104: spiral_sprites(out,l);return;
    case 105: movie_warp(out,l);return;
    case 107: character_raster(out,l);return;
    case 111: swimming_fish(out,l);return;
    case 120: color_balance(out,l);return;
    case 121: dual_mask(out,layers,slot);return;
    case 130: psychedelic(out,l);return;
    case 132: chara_bure(out,l,slot);return;
    case 133: sentence(out,l);return;
    case 137: external_animation(out,l,slot,animation,animation_off);return;
    default:break;
    }
    if (l.effect==13 || l.effect==20 || l.effect==22 || l.effect==23 || l.effect==24 || l.effect==29 || l.effect==31 || l.effect==43 || l.effect==50 || l.effect==55 || l.effect==71 || l.effect==74 || l.effect==128 || l.effect==129) ordinary(out,l,slot);
    if (l.effect==81) {
        shards(out,l);
    } else if (l.effect==67) {
        perspective(out,l);
    } else if (l.effect==60) {
        mask_wipe(out,layers,slot,saved);
    } else if (l.effect==55) {
        old_film(out,l);
    } else if (l.effect==109) {
        old_lamp(out,l);
    } else if (l.effect==23) {
        raster(out,l);
    } else if (l.effect==22) {
        kagerou(out,l);
    } else if (l.effect==24) {
        water(out,l);
    } else if (l.effect==86) {
        zantetsu(out,l);
    } else if (l.effect==123) {
        mask_appear(out,layers,slot);
    } else if (l.effect==37 || l.effect==122) {
        zoom_in(out,l,l.effect==122);
    } else if (l.effect==110) {
        chara_ghost(out,l);
    } else if (l.effect==71) {
        old_fireflies(out,l);
    } else if (l.effect==13) {
        rain(out,l);
    } else if (l.effect==106) {
        chara_noise(out,l);
    } else if (l.effect==138) {
        fireflies(out,l);
    } else if (l.effect==29) {
        suna(out,l);
    } else if (l.effect==56) {
        rain2(out,l);
    } else if (l.effect==102) {
        rain4(out,l);
    } else if (l.effect==140) {
        turn_new(out,l);
    } else if (l.effect==64) {
        turn(out,l);
    } else if (l.effect==40) {
        change(out,layers,slot);
    } else if (l.effect==112) {
        metal(out,l);
    } else if (l.effect==139) {
        chara_mosaic(out,l);
    } else if (l.effect==136) {
        taclip(out,l);
    } else if (l.effect==77 || l.effect==125 || l.effect==126) {
        const auto size=blur_size(l);
        if (l.effect==77) blur_screen_picture(out,l,size[0],size[1]);
        else blur_character(out,l,size[0],size[1],l.effect==126);
    } else if (l.effect==74) {
        bura(out,l);
    } else if (l.effect==20) {
        std::array<int,3> add{};
        for (std::size_t i=0;i<3;++i) add[i]=interpolate(l,0,p(i+1),p(4),p(5));
        screen_add(out,add,0,static_cast<int>(out.height));
    } else if (l.effect==43) {
        const int bw=std::max(1,p(1)),bh=std::max(1,p(2));
        const int left=std::clamp(p(3),0,static_cast<int>(out.width)),top=std::clamp(p(4),0,static_cast<int>(out.height));
        const int right=static_cast<int>(std::clamp(static_cast<std::int64_t>(p(3))+p(5),std::int64_t{0},static_cast<std::int64_t>(out.width)));
        const int bottom=static_cast<int>(std::clamp(static_cast<std::int64_t>(p(4))+p(6),std::int64_t{0},static_cast<std::int64_t>(out.height)));
        std::array<std::uint32_t,101> samples{};
        std::uint32_t seed=l.effect_state->seed^(l.count*0x9e3779b9U);
        for (auto& sample:samples) sample=random_next(seed)%16384U;
        std::size_t n=0;
        for (int y=top;bottom-y>=bh;y+=bh) for (int x=left;right-x>=bw;x+=bw) {
            const auto r=samples[n++%samples.size()];
            const int sx=x+static_cast<int>((r&255U)%static_cast<unsigned>(bw));
            const int sy=y+static_cast<int>((r>>8U)%static_cast<unsigned>(bh));
            const auto si=(static_cast<std::size_t>(sy)*out.width+static_cast<unsigned>(sx))*3;
            const std::array<Byte,3> color{out.rgb[si],out.rgb[si+1],out.rgb[si+2]};
            for (int yy=y;yy<y+bh;++yy) for (int xx=x;xx<x+bw;++xx) {
                const auto di=(static_cast<std::size_t>(yy)*out.width+static_cast<unsigned>(xx))*3;
                std::copy(color.begin(),color.end(),out.rgb.begin()+static_cast<std::ptrdiff_t>(di));
            }
        }
    } else if (l.effect==50) {
        std::array<int,3> first{},last{};
        for (std::size_t i=0;i<3;++i) {
            first[i]=interpolate(l,p(9+i),p(1+i),p(7),p(8));
            last[i]=interpolate(l,p(12+i),p(4+i),p(7),p(8));
        }
        const int h=p(16);
        for (int y=0;y<h;++y) {
            std::array<int,3> add{};
            for (std::size_t i=0;i<3;++i) add[i]=clamp_int(static_cast<std::int64_t>(first[i])+
                (static_cast<std::int64_t>(last[i])-first[i])*y/h);
            screen_add(out,add,p(15)+y,1);
        }
    } else if (l.effect==31) {
        const auto conv=[](int n){return clamp_int(254-(static_cast<std::int64_t>(n)*508)/100);};
        const int amount=std::clamp(interpolate(l,conv(p(4)),conv(p(3)),p(1),p(2)),-254,254);
        for (Byte& channel:out.rgb) channel=static_cast<Byte>(std::clamp(128+(static_cast<int>(channel)-128)*amount/256,0,255));
    } else if (l.effect==128 || l.effect==129) {
        const int weight=std::clamp(interpolate(l,clamp_int(static_cast<std::int64_t>(p(4))*256),
            clamp_int(static_cast<std::int64_t>(p(3))*256),p(1),p(2))/100,0,256);
        const std::array<int,3> color{p(5),p(6),p(7)};
        screen_grey(out,weight,l.effect==128?&color:nullptr);
    } else if (l.effect==124 || l.effect==127) {
        const int weight=std::clamp(interpolate(l,clamp_int(static_cast<std::int64_t>(p(4))*256),
            clamp_int(static_cast<std::int64_t>(p(3))*256),p(1),p(2))/100,0,256);
        const std::array<int,3> sepia{p(5)&255,p(6)&255,p(7)&255};
        character_pixels(out,l,[&](Byte* dst,const Byte* src) {
            const int a=src[3];
            if (!a) return;
            const int y=((src[2]*29+src[1]*150)>>8)+((src[0]*76)>>8);
            for (std::size_t c=0;c<3;++c) {
                const int shade=l.effect==127 ? (y*sepia[c])>>8 : y;
                const int v=(shade*weight+src[c]*(256-weight))>>8;
                dst[c]=static_cast<Byte>(a==255 ? v : (v*(a+1)+dst[c]*(255-a))>>8);
            }
        });
    } else if (l.effect==131) {
        if (!l.texture || !l.texture->has_mask) return;
        const int percent=interpolate(l,p(2),p(1),p(3),p(4));
        const int scale=(std::abs(percent)*256)/100;
        character_pixels(out,l,[&](Byte* dst,const Byte* src) {
            const int a=src[3];
            if (!a || !percent) return;
            for (std::size_t c=0;c<3;++c) {
                const int amount=std::min(255,(src[c]*scale)>>8);
                const int v=std::clamp(dst[c]+(percent<0?-amount:amount),0,255);
                dst[c]=static_cast<Byte>(((v*a)>>8)+((dst[c]*(256-a))>>8));
            }
        });
    } else if ((l.effect==62 || l.effect==119) && l.texture) {
        const bool cubic=l.effect==62;
        const int t10000=interpolate(l,0,10000,p(cubic?9U:7U),p(cubic?10U:8U));
        const float t=static_cast<float>(t10000)*0.0001F;
        const auto coord=[&](std::size_t axis) {
            const float a=static_cast<float>(p(1+axis)),b=static_cast<float>(p(3+axis)),c=static_cast<float>(p(5+axis));
            return spline(t,a,b,c,static_cast<float>(p(7+axis)),cubic);
        };
        const Rect dst=destination(l);
        const Rect src{0,0,l.texture->canvas_width,l.texture->canvas_height};
        normal_picture(out,*l.texture,src,{clamp_int(static_cast<std::int64_t>(dst.x)+coord(0)),
            clamp_int(static_cast<std::int64_t>(dst.y)+coord(1)),src.width,src.height},!(slot<4||slot==30));
    } else if (l.effect==94 && l.texture) {
        int x=l.effect_state->offset_x,y=l.effect_state->offset_y;
        const int width=static_cast<int>(out.width),height=static_cast<int>(out.height);
        if (x<0) x+=width;
        if (y<0) y+=height;
        for (int j=-1;j<=0;++j) for (int i=-1;i<=0;++i)
            paint(out,*l.texture,{0,0,width,height},{x+i*width,y+j*height,width,height},100,{},false);
    }
}

}
