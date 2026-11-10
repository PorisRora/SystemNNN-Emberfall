#include "emberfall/render.hpp"
#include "emberfall/layers.hpp"
#include "../src/effects.hpp"
#include "../src/wipes.hpp"
#include "../../../core/include/nnn/snapshot.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace emberfall::render;
namespace {
unsigned checks = 0;
void require(bool value) { ++checks; if (!value) { std::fprintf(stderr, "FAILED check %u\n", checks); std::exit(1); } }
Surface surface() { return {4, 3, std::vector<Byte>(4 * 3 * 3, 255)}; }
RgbaImage source() {
    return {3, 2, {255, 1, 2, 0, 3, 254, 4, 1, 5, 6, 253, 127,
                  252, 7, 8, 128, 9, 251, 10, 254, 11, 12, 250, 255}};
}
std::shared_ptr<Texture> pattern(int width,int height) {
    auto t=std::make_shared<Texture>();
    t->canvas_width=width;t->canvas_height=height;t->content={0,0,width,height};
    t->pixels.width=static_cast<unsigned>(width);t->pixels.height=static_cast<unsigned>(height);
    for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
        t->pixels.rgba.push_back(static_cast<Byte>(20+x*37+y*11));
        t->pixels.rgba.push_back(static_cast<Byte>(40+x*13+y*29));
        t->pixels.rgba.push_back(static_cast<Byte>(60+x*19+y*7));
        t->pixels.rgba.push_back(255);
    }
    return t;
}
std::vector<Byte> rgb(const Texture& t) {
    std::vector<Byte> out;
    for (std::size_t i=0;i<t.pixels.rgba.size();i+=4)
        out.insert(out.end(),t.pixels.rgba.begin()+static_cast<std::ptrdiff_t>(i),t.pixels.rgba.begin()+static_cast<std::ptrdiff_t>(i+3));
    return out;
}
void equal_frame(Layers& layers,const std::vector<Byte>& expected,int effect,int count) {
    const auto& actual=layers.surface().rgb;
    if (actual!=expected) {
        for (std::size_t i=0;i<std::min(actual.size(),expected.size());++i) if (actual[i]!=expected[i]) {
            std::fprintf(stderr,"effect %d count %d byte %zu actual %u expected %u\n",effect,count,i,actual[i],expected[i]);
            break;
        }
        require(false);
    }
    require(true);
}
void chart_layer_tests() {
    Layers layers(3,2);
    ChartLayerScratch scratch;
    std::array<std::vector<Byte>,3> pixels;
    std::array<ChartLayerTarget,3> targets;
    for (std::size_t i=0;i<pixels.size();++i) {
        pixels[i].assign(32,0x7d);
        targets[i]={pixels[i].data(),16,pixels[i].size()};
    }
    require(layers.draw_chart_layers(5,{},scratch)==0 && scratch.frame.rgb.empty());
    auto half=pattern(1,1);
    half->pixels.rgba={255,0,0,128};
    auto keyed=pattern(2,1);
    keyed->has_mask=false;
    keyed->pixels.rgba={40,80,120,255,0,200,0,255};
    auto dark=pattern(1,1);
    dark->pixels.rgba={0,0,0,255};
    layers.bind(0,half,true); // Raw texture for a map tile, not a printed layer.
    layers.bind(4,half,false);
    layers.bind(5,keyed,false);
    layers.bind(6,keyed,false);
    layers.bind(14,dark,false);
    layers.bind(16,half,false);
    layers.hide_layers(UINT32_MAX); // PrintBackScriptLayers ignores eye flags.
    require(layers.chart_layer_mask(5)==7 && layers.draw_chart_layers(5,targets,scratch)==7);
    const auto pixel=[&](std::size_t group,std::size_t x) {
        const auto* data=pixels[group].data()+x*4;
        return std::array{data[0],data[1],data[2],data[3]};
    };
    require(pixel(0,0)==std::array<Byte,4>{127,0,0,128} && pixel(0,1)==std::array<Byte,4>{0,0,0,0});
    require(pixel(1,0)==std::array<Byte,4>{0,0,0,0} && pixel(1,1)==std::array<Byte,4>{0,200,0,255});
    require(pixel(2,0)==std::array<Byte,4>{0,0,0,255} && pixels[2][12]==0x7d);
    std::array<std::array<Byte,12>,3> reduced{};
    std::array<ChartLayerTarget,3> reduced_targets;
    for (std::size_t i=0;i<reduced.size();++i) {
        reduced[i].fill(0x7d);
        reduced_targets[i]={reduced[i].data(),reduced[i].size(),reduced[i].size()};
    }
    require(layers.draw_chart_layers(5,reduced_targets,scratch,2,1)==7);
    for (std::size_t i=0;i<reduced.size();++i)
        require(std::equal(reduced[i].begin(),reduced[i].begin()+8,pixels[i].begin()) && reduced[i][8]==0x7d);
    const auto stable=layers.composition_revision();
    layers.advance(1);
    require(layers.composition_revision()==stable);
    auto invalid=targets;
    invalid[1].stride=11;
    pixels[0][0]=0x7d;
    bool rejected=false;
    try { layers.draw_chart_layers(5,invalid,scratch); } catch (const std::invalid_argument&) { rejected=true; }
    require(rejected && pixels[0][0]==0x7d);
    layers.clear(6);
    targets[1]={};
    require(layers.composition_revision()!=stable && layers.draw_chart_layers(5,targets,scratch)==5);
    layers.prepare_overlap();
    require(layers.start_overlap(2) && layers.chart_layer_mask(5)==0 && layers.draw_chart_layers(5,{},scratch)==0);
}
void chart_composite_tests() {
    Layers layers(3,2);
    ChartLayerScratch scratch;
    std::array<Byte,32> base{},center{},content{},output{};
    for (std::size_t y=0;y<2;++y) for (std::size_t x=0;x<3;++x) {
        auto* dst=base.data()+y*16+x*4;
        const std::array<Byte,4> color{70,100,190,255};
        std::copy(color.begin(),color.end(),dst);
    }
    center={20,40,80,255,64,0,0,128};
    content[8]=30;content[9]=60;content[10]=90;content[11]=255;
    output.fill(0x7d);
    const ChartLayerSource base_plane{base.data(),16,base.size()};
    const ChartLayerTarget target{output.data(),16,output.size()};
    require(layers.effect(5,20,{1,200,200,200,0,0}) && layers.effect(16,20,{1,200,200,200,0,0}));
    require(!layers.chart_needs_composite(5) && !layers.draw_chart_composite(5,base_plane,{},{},target,scratch));
    require(layers.effect(4,20,{1,40,0,0,0,0}) && layers.effect(6,20,{1,0,30,0,0,0}) &&
        layers.effect(14,20,{1,0,0,70,0,0}));
    layers.advance(1);
    bool rejected=false;
    try { layers.draw_chart_layers(5,{},scratch); } catch (const std::logic_error&) { rejected=true; }
    require(rejected && layers.chart_needs_composite(5));
    require(layers.draw_chart_composite(5,base_plane,{center.data(),16,center.size()},
        {content.data(),16,content.size()},target,scratch));
    // The center covers the first addition. Content covers both preceding
    // groups. Slot 14 adds blue after content, including under its opacity.
    const std::array<Byte,32> expected{
        20,70,150,255,119,80,165,255,30,60,160,255,0x7d,0x7d,0x7d,0x7d,
        110,130,255,255,110,130,255,255,110,130,255,255,0x7d,0x7d,0x7d,0x7d};
    require(output==expected);
    std::array<Byte,12> reduced;
    reduced.fill(0x7d);
    require(layers.draw_chart_composite(5,base_plane,{center.data(),16,center.size()},
        {content.data(),16,content.size()},{reduced.data(),reduced.size(),reduced.size()},scratch,2,1));
    require(std::equal(reduced.begin(),reduced.begin()+8,expected.begin()) && reduced[8]==0x7d);
}
void wipe_tests() {
    auto first=pattern(17,13),second=pattern(17,13);
    first->name="first";second->name="second";
    for (std::size_t i=0;i<second->pixels.rgba.size();i+=4) {
        second->pixels.rgba[i]=static_cast<Byte>(255-second->pixels.rgba[i]);
        second->pixels.rgba[i+1]=static_cast<Byte>(255-second->pixels.rgba[i+1]);
    }
    const Surface before{17,13,rgb(*first)},after{17,13,rgb(*second)};
    auto a=before,b=after;
    const auto rejected=[&](Surface output,const Surface& from,const Surface& to){
        try {draw_wipe(output,from,to,38,1,2);return false;}
        catch (const std::invalid_argument&) {return true;}
    };
    require(rejected({17,13,{}},before,after));
    require(rejected(a,{13,17,before.rgb},after));
    require(rejected(a,before,{17,13,{}}));
    std::array<Surface,3> crossfade{before,before,before};
    for (std::size_t i=0;i<crossfade.size();++i)
        draw_wipe(crossfade[i],before,after,0,static_cast<unsigned>(i+1)*4U,16);
    require(!supports_wipe(-1) && !supports_wipe(96));
    for (int type=0;type<=95;++type) {
        require(supports_wipe(type));
        draw_wipe(a,before,after,type,0,16);require(a.rgb==before.rgb);
        draw_wipe(b,before,after,type,16,16);require(b.rgb==after.rgb);
        // An intermediate frame has no dependence on previous output pixels.
        bool distinct=type==0;
        for (std::size_t i=0;i<crossfade.size();++i) {
            const auto tick=static_cast<unsigned>(i+1)*4U;
            std::fill(a.rgb.begin(),a.rgb.end(),0x33);
            std::fill(b.rgb.begin(),b.rgb.end(),0xcc);
            draw_wipe(a,before,after,type,tick,16);
            draw_wipe(b,before,after,type,tick,16);
            require(a.rgb==b.rgb);
            distinct|=a.rgb!=crossfade[i].rgb;
        }
        require(distinct);
    }
    const auto pixel=[](const Surface& frame,int x,int y) {
        const auto at=(static_cast<std::size_t>(y)*frame.width+static_cast<unsigned>(x))*3;
        return std::array{frame.rgb[at],frame.rgb[at+1],frame.rgb[at+2]};
    };
    draw_wipe(a,before,after,4,8,16);
    draw_wipe(b,before,after,5,8,16);
    require(pixel(a,9,6)==pixel(after,0,6) && pixel(a,0,6)==pixel(before,0,6));
    require(pixel(b,9,6)==pixel(after,0,6) && pixel(b,0,6)==pixel(before,8,6));
    draw_wipe(a,before,after,8,4,16);
    require(pixel(a,6,6)==pixel(after,0,6) && pixel(a,10,6)==pixel(before,6,6));
    draw_wipe(a,before,after,41,8,16);
    require(pixel(a,2,6)==pixel(after,2,4) && pixel(a,2,12)==pixel(before,2,12));
    draw_wipe(a,before,after,84,8,16);
    draw_wipe(b,before,after,86,8,16);
    require(pixel(a,4,3)==pixel(after,0,0) && pixel(b,4,3)==pixel(before,4,3));
    draw_wipe(a,before,after,60,8,16);
    require(a.rgb[0]==(before.rgb[0]*after.rgb[0]>>8));

    Layers live(17,13),restored(17,13);
    live.bind(0,first,false);live.prepare_overlap();live.bind(0,second,false);
    require(live.start_overlap(16,39));live.advance(5);
    nnn::snapshot::Writer saved;live.save_snapshot(saved);
    nnn::snapshot::Reader reader(saved.bytes);
    restored.load_snapshot(reader,[&](const std::string& name){return name=="first"?first:second;},true,true);
    require(restored.surface().rgb==live.surface().rgb);
    restored.advance(3);for (int i=0;i<3;++i) live.advance(1);
    require(restored.surface().rgb==live.surface().rgb);
    restored.advance(8);require(restored.surface().rgb==after.rgb);
}
void geometry_effects() {
    auto background=pattern(11,9);
    Layers layers(11,9);
    layers.bind(0,background,false);
    require(layers.effect(4,89,{20,5,4,4,3,5,4,1,1,221,15,3,100}));
    const auto pixels=layers.surface().rgb;
    const auto original=rgb(*background);
    const auto at=[](int x,int y){return static_cast<std::size_t>((y*11+x)*3);};
    require(pixels[at(5,4)]==original[at(5,4)] && pixels[at(0,4)]==original[at(0,4)]);
    require(pixels[at(1,4)]==221 && pixels[at(1,4)+1]==15 && pixels[at(1,4)+2]==3);
}
void animation_effects() {
    auto atlas=pattern(4,1);
    Layers sprite(2,1);
    sprite.bind(4,atlas,false);
    require(sprite.effect(4,63,{4,2,1,1}));
    require(sprite.surface().rgb[0]==atlas->pixels.rgba[0]);
    sprite.advance(1);require(sprite.surface().rgb[0]==atlas->pixels.rgba[8]);
    sprite.advance(3);require(sprite.surface().rgb[0]==0);

    auto frame=pattern(1,1);frame->name="frame";
    Layers full(1,1);
    std::string requested;
    full.animation_textures([&](const std::string& name){requested=name;return frame;});
    require(full.effect(0,79,{8,5,2}));
    full.advance(4);require(full.surface().rgb==rgb(*frame) && requested=="dwq/an/an005002.dwq");

    Layers one_shot(1,1),restored(1,1);
    one_shot.bind(4,frame,false);
    require(one_shot.effect(4,75,{20,8,1}));
    one_shot.advance(3);
    require(one_shot.surface().rgb==rgb(*frame));
    require(one_shot.surface().rgb==rgb(*frame));
    nnn::snapshot::Writer saved;one_shot.save_snapshot(saved);
    nnn::snapshot::Reader reader(saved.bytes);
    restored.load_snapshot(reader,[&](const std::string&){return frame;});
    require(restored.surface().rgb==rgb(*frame));
    restored.advance(1);one_shot.advance(1);
    require(restored.surface().rgb==one_shot.surface().rgb && restored.surface().rgb[0]==0);

    auto background=pattern(2,1);background->name="background";
    Layers backdrop(2,1),loaded(2,1);
    backdrop.bind(0,background,false);
    require(backdrop.effect(0,75,{20,0,1}));
    require(backdrop.surface().rgb==rgb(*background));
    backdrop.advance(1);
    nnn::snapshot::Writer dirty;backdrop.save_snapshot(dirty);
    nnn::snapshot::Reader pending(dirty.bytes);
    loaded.load_snapshot(pending,[&](const std::string&){return background;});
    require(loaded.surface().rgb==rgb(*background) && loaded.surface().rgb==backdrop.surface().rgb);
    loaded.bind(1,frame,false);loaded.rectangle(1,{1,0,1,1},{1,0,1,1});
    require(loaded.surface().rgb==std::vector<Byte>({20,40,60,20,40,60}));
}
void condensation_effect() {
    auto glass=pattern(16,16);glass->name="glass";
    Layers scene(16,16),loaded(16,16);
    scene.bind(4,glass,false);
    require(scene.effect(4,97,{20,8,1,0,0,0,16,16,1}));
    const auto original=scene.surface().rgb;
    scene.advance(1,{8,8});
    const auto wiped=scene.surface().rgb;
    require(wiped!=original && glass->pixels.rgba[(8*16+8)*4+3]==255);
    scene.advance(1,{8,8});
    require(scene.surface().rgb==wiped);
    nnn::snapshot::Writer saved;scene.save_snapshot(saved);
    nnn::snapshot::Reader reader(saved.bytes);
    loaded.load_snapshot(reader,[&](const std::string&){return glass;});
    require(loaded.surface().rgb==wiped);
    scene.advance(1,{9,8});loaded.advance(1,{9,8});
    require(loaded.surface().rgb==scene.surface().rgb && loaded.surface().rgb!=wiped);
    loaded.bind(4,glass,false);
    require(loaded.surface().rgb==original);
}
void three(Layers& layers,int effect,const std::function<std::vector<Byte>(int)>& expected) {
    for (int count=0;count<=2;++count) {
        if (count) layers.advance(1);
        equal_frame(layers,expected(count),effect,count);
    }
}
int lerp(int a,int b,int n,int duration=2) {
    return static_cast<int>(a+(b-a)*(static_cast<double>(n)/duration)+0.5);
}
std::uint32_t next_random(std::uint32_t& n) {
    n^=n<<13U;n^=n>>17U;n^=n<<5U;
    return n;
}
void rain4_tests() {
    auto t=pattern(2,2);
    Layers l(4,4);l.bind(4,t,false);
    require(l.effect(4,102,{2,1,200,100,-25,2,2,0,50}));
    three(l,102,[](int n) {
        std::vector<Byte> pixels(4U*4U*3U);
        if (n==2) for (std::size_t i=0;i<pixels.size();i+=3) {
            pixels[i]=9;pixels[i+1]=19;pixels[i+2]=29;
        }
        return pixels;
    });
    require(l.slots()[4].effect_state->distance==175);
    l.advance(1);require(l.slots()[4].effect_state->distance==150);
    l.advance(1);require(l.slots()[4].effect_state->distance==125);
    l.advance(1);require(l.slots()[4].effect_state->distance==100);
    l.advance(1);require(l.slots()[4].effect_state->distance==200);
    Layers bulk(4,4),single(4,4);
    bulk.bind(4,t,false);single.bind(4,t,false);
    require(bulk.effect(4,102,{2,1,200,100,-25,2,2,0,50}));
    require(single.effect(4,102,{2,1,200,100,-25,2,2,0,50}));
    bulk.advance(9);for (int i=0;i<9;++i) single.advance(1);
    require(bulk.slots()[4].effect_state->distance==single.slots()[4].effect_state->distance);
    require(bulk.surface().rgb==single.surface().rgb);
    require(bulk.effect(4,102,{0,99,10,100,1,-5,99}));
    require(bulk.slots()[4].parameters[1]==4 && bulk.slots()[4].parameters[3]==9);
    require(bulk.slots()[4].parameters[5]==0 && bulk.slots()[4].parameters[6]==3);
    bulk.advance(1);require(bulk.slots()[4].effect_state->distance==1);
}
void rain2_tests() {
    auto t=pattern(2,2);
    Layers l(4,4);l.bind(4,t,false);
    require(l.effect(4,56,{2,1,2,3,0,0,50,100}));
    const auto& planes=l.slots()[4].effect_state->rain;
    require(planes[0].speed_x==100 && planes[1].speed_x==50 && planes[2].speed_x==25);
    require(planes[0].speed_y==200 && planes[1].speed_y==100 && planes[2].speed_y==50);
    three(l,56,[&](int n) {
        std::vector<Byte> pixels(4U*4U*3U);
        for (int y=0;y<4;++y) for (int x=0;x<4;++x) {
            const int sx=(x+(n==1?1:0))%2,sy=y%2;
            for (int c=0;c<3;++c)
                pixels[static_cast<std::size_t>((y*4+x)*3+c)]=t->pixels.rgba[static_cast<std::size_t>((sy*2+sx)*4+c)];
        }
        return pixels;
    });
    require(l.slots()[4].effect_state->rain_initialized);
    require(planes[0].x>=0 && planes[0].x<2 && planes[0].y>=0 && planes[0].y<2);
    Layers bulk(4,4),single(4,4);
    bulk.bind(4,t,false);single.bind(4,t,false);
    require(bulk.effect(4,56,{2,1,2,4,3,2,50,1}));
    require(single.effect(4,56,{2,1,2,4,3,2,50,1}));
    bulk.advance(17);for (int i=0;i<17;++i) single.advance(1);
    for (std::size_t i=0;i<4;++i) {
        const auto& a=bulk.slots()[4].effect_state->rain[i];
        const auto& b=single.slots()[4].effect_state->rain[i];
        require(a.x==b.x && a.y==b.y && a.speed_x==b.speed_x && a.speed_y==b.speed_y);
        require(a.speed_x>=-200 && a.speed_x<=400 && a.speed_y>=100 && a.speed_y<=400);
    }
    require(bulk.surface().rgb==single.surface().rgb);
    require(bulk.effect(4,56,{0,1,1,99,0,0,50,0}));
    require(bulk.slots()[4].parameters[3]==4 && bulk.slots()[4].parameters[7]==1);
    bulk.advance(1);require(bulk.slots()[4].effect_state->rain_initialized);
}
void suna_tests() {
    auto t=pattern(8,2);
    Layers l(8,2);l.bind(4,t,false);
    require(l.effect(4,29,{2,1,100,0,0,0,0,0,8,2}));
    const auto word=l.slots()[4].effect_state->noise[0]<<1U;
    three(l,29,[&](int n) {
        auto pixels=rgb(*t);
        for (int y=0;y<2;++y) for (int x=0;x<8;++x) for (int c=0;c<3;++c) {
            const int gray=static_cast<int>((word>>((x/2)*8))&255U),alpha=n*128;
            const auto i=static_cast<std::size_t>((y*8+x)*3+c);
            pixels[i]=static_cast<Byte>(n==2?gray:((gray*alpha)>>8)+((pixels[i]*(256-alpha))>>8));
        }
        return pixels;
    });
    auto larger=pattern(16,4);Layers bulk(16,4),single(16,4);
    bulk.bind(4,larger,false);single.bind(4,larger,false);
    require(bulk.effect(4,29,{0}));require(single.effect(4,29,{0}));
    const auto initial=bulk.slots()[4].effect_state->noise;
    bulk.advance(7);for (int i=0;i<7;++i) single.advance(1);
    require(bulk.slots()[4].effect_state->noise==single.slots()[4].effect_state->noise);
    require(initial!=bulk.slots()[4].effect_state->noise);
    require(bulk.surface().rgb==single.surface().rgb);
}
void firefly_tests() {
    auto t=pattern(1,1);
    const std::vector<std::int32_t> args{2,1,1,1,0,1,0,100,2,2,1,1,0,0,0,1,1,0,0,0,0,1,0,0};
    Layers l(8,8);l.bind(4,t,false);require(l.effect(4,138,args));
    require(l.slots()[4].effect_state->fireflies.size()==1);
    three(l,138,[](int n) {
        std::vector<Byte> pixels(8U*8U*3U);
        if (n<2) {
            const int pos=n?3:2;
            const auto i=static_cast<std::size_t>((pos*8+pos)*3);
            pixels[i]=19;pixels[i+1]=39;pixels[i+2]=59;
        }
        return pixels;
    });
    const auto& fly=l.slots()[4].effect_state->fireflies[0];
    require(fly.frame==3 && fly.x==3.5F && fly.y==3.5F);
    l.advance(1);require(fly.period==0);
    const std::vector<std::int32_t> random_args{5,30,1,1,0,1,0,100,8,3,3,4,0,0,0,1,1,40,1,1,1,4,0,0};
    Layers bulk(32,16),single(32,16);
    bulk.bind(4,t,false);single.bind(4,t,false);
    require(bulk.effect(4,138,random_args));require(single.effect(4,138,random_args));
    for (const auto& f:bulk.slots()[4].effect_state->fireflies)
        require(f.x>=8.5F && f.x<11.5F && f.y>=3.5F && f.y<7.5F);
    bulk.advance(17);for (int i=0;i<17;++i) single.advance(1);
    const auto& a=bulk.slots()[4].effect_state->fireflies;
    const auto& b=single.slots()[4].effect_state->fireflies;
    for (std::size_t i=0;i<a.size();++i)
        require(a[i].x==b[i].x && a[i].y==b[i].y && a[i].frame==b[i].frame && a[i].period==b[i].period);
    require(bulk.surface().rgb==single.surface().rgb);
    require(bulk.effect(4,138,{0,9999,1,1,0,1,0,100,0,0,0,0}));
    require(bulk.slots()[4].effect_state->fireflies.size()==600);
    require(bulk.slots()[4].parameters[10]==1 && bulk.slots()[4].parameters[11]==1);
}
void chara_noise_tests() {
    auto bg=pattern(2,2),t=pattern(2,2);
    for (std::size_t i=3;i<t->pixels.rgba.size();i+=4) t->pixels.rgba[i]=128;
    Layers l(2,2);l.bind(0,bg,false);l.bind(4,t,false);
    require(l.effect(4,106,{2,0,0,1,2,0,2,0,0,100,0,0,0}));
    const auto base=rgb(*bg);
    three(l,106,[&](int n) {
        auto result=base;const int a=n*32;
        for (int i=0;i<4;++i) for (int c=0;c<3;++c) {
            const auto di=static_cast<std::size_t>(i*3+c),si=static_cast<std::size_t>(i*4+c);
            result[di]=static_cast<Byte>((t->pixels.rgba[si]*a+base[di]*(256-a))>>8);
        }
        return result;
    });
    auto large=pattern(8,8);Layers bulk(8,8),single(8,8);
    bulk.bind(4,large,false);single.bind(4,large,false);
    const std::vector<std::int32_t> args{3,2,2,1,2,0,2,1,0};
    require(bulk.effect(4,106,args));require(single.effect(4,106,args));
    std::array<int,2> first_y{};
    for (std::size_t i=0;i<2;++i) {
        const auto& band=bulk.slots()[4].effect_state->bands[i];
        require(band.remaining==2 && band.length==2 && band.y>=0 && band.y<6);
        require(band.strength>=20 && band.strength<80);first_y[i]=band.y;
    }
    bulk.advance(1);single.advance(1);
    for (std::size_t i=0;i<2;++i) {
        const auto& band=bulk.slots()[4].effect_state->bands[i];
        require(band.remaining==1 && band.y==first_y[i]+1);
        require(band.strength>=30 && band.strength<=100);
    }
    bulk.advance(16);for (int i=0;i<16;++i) single.advance(1);
    const auto& a=*bulk.slots()[4].effect_state;const auto& b=*single.slots()[4].effect_state;
    require(a.raster==b.raster);
    for (std::size_t i=0;i<a.bands.size();++i)
        require(a.bands[i].remaining==b.bands[i].remaining && a.bands[i].y==b.bands[i].y &&
                a.bands[i].length==b.bands[i].length && a.bands[i].strength==b.bands[i].strength);
    require(bulk.surface().rgb==single.surface().rgb);
    require(bulk.effect(4,106,{0,99}));require(bulk.slots()[4].effect_state->bands.size()==16);
}
void rain_tests() {
    Layers l(64,64);require(l.effect(4,13,{2,0,5,1,2,255,128,64}));
    const auto initial=l.slots()[4].effect_state->drops;
    require(initial.size()==5);
    for (const auto& d:initial) {
        require(d.direction>=-8*256 && d.direction<=7*256);
        const auto spread=std::abs((d.direction/256)*64/256);
        require(d.x>=-spread*256 && d.x<(64+spread)*256);
        require(d.y>=-2*256 && d.y<64*256 && d.speed==1 && d.length>=1 && d.length<=2);
    }
    three(l,13,[&](int) {
        std::vector<Byte> pixels(64U*64U*3U);
        for (const auto& d:l.slots()[4].effect_state->drops) {
            const auto x=(d.x+128)>>8,y=d.y/256;
            if (y>=64 || y+d.length<0) continue;
            const auto dx=(d.direction*d.length)>>16,dy=static_cast<std::int64_t>(d.length);
            const auto length=std::max<std::int64_t>(1,std::max(std::abs(dx),dy));
            for (std::int64_t i=0;i<=length;++i) {
                const auto xx=x+(dx*i+length/2)/length,yy=y+(dy*i+length/2)/length;
                if (xx<0 || yy<0 || xx>=64 || yy>=64) continue;
                const auto at=static_cast<std::size_t>((yy*64+xx)*3);
                pixels[at]=255;pixels[at+1]=128;pixels[at+2]=64;
            }
        }
        return pixels;
    });
    bool moved=false;
    for (std::size_t i=0;i<initial.size();++i) if (initial[i].y<62*256) {
        const auto& d=l.slots()[4].effect_state->drops[i];
        require(d.x==initial[i].x+2*(initial[i].direction*initial[i].speed/256));
        require(d.y==initial[i].y+2*initial[i].speed*256);moved=true;
    }
    require(moved);
    Layers bulk(64,64),single(64,64);
    require(bulk.effect(4,13,{0,30,150,60,80}));require(single.effect(4,13,{0,30,150,60,80}));
    bulk.advance(15);for (int i=0;i<15;++i) single.advance(1);
    for (std::size_t i=0;i<150;++i) {
        const auto& a=bulk.slots()[4].effect_state->drops[i];const auto& b=single.slots()[4].effect_state->drops[i];
        require(a.x==b.x && a.y==b.y && a.direction==b.direction && a.speed==b.speed && a.length==b.length);
    }
    require(bulk.surface().rgb==single.surface().rgb);
    require(bulk.effect(4,13,{0,0,1000,0,0}));
    require(bulk.slots()[4].effect_state->drops.size()==150);
    require(bulk.slots()[4].parameters[3]==1 && bulk.slots()[4].parameters[4]==1);
}
void old_firefly_tests() {
    auto bg=pattern(16,16);
    for (std::size_t i=0;i<bg->pixels.rgba.size();i+=4) {
        bg->pixels.rgba[i]=100;bg->pixels.rgba[i+1]=100;bg->pixels.rgba[i+2]=100;
    }
    const std::vector<std::int32_t> args{2,1,8,8,1,1,0,0,100,-100,0,0,0,0,0,1,0,0};
    Layers l(16,16);l.bind(0,bg,false);require(l.effect(4,71,args));
    require(l.slots()[4].effect_state->old_fireflies[0].x==8*256);
    require(l.slots()[4].effect_state->old_fireflies[0].y==8*256);
    three(l,71,[](int n) {
        std::vector<Byte> pixels(16U*16U*3U,100);
        const int percent=100-n*50;
        for (int y=0;y<8;++y) for (int x=0;x<8;++x) {
            const int dx=(x-4)*2+1,dy=(y-4)*2+1;
            const double radius=std::sqrt(static_cast<double>(dx*dx+dy*dy));
            const int attenuation=static_cast<int>(radius*radius*2);
            const int amount=std::clamp((100-attenuation)*percent/100,0,255);
            const auto at=static_cast<std::size_t>(((y+4)*16+x+4)*3);
            pixels[at]=static_cast<Byte>(std::min(255,100+amount));
            pixels[at+1]=static_cast<Byte>(std::max(0,100-amount));
        }
        return pixels;
    });
    const auto* before=l.slots()[4].effect_state.get();
    auto preserve=args;preserve[12]=1;
    require(l.effect(4,71,preserve));
    require(l.slots()[4].effect_state.get()==before);
    Layers bulk(32,32),single(32,32);
    auto random_args=args;random_args[1]=20;random_args[2]=8;random_args[3]=8;
    random_args[4]=5;random_args[5]=5;random_args[6]=1;random_args[7]=1;random_args[11]=1;
    require(bulk.effect(4,71,random_args));require(single.effect(4,71,random_args));
    bulk.advance(20);for (int i=0;i<20;++i) single.advance(1);
    const auto& a=bulk.slots()[4].effect_state->old_fireflies;
    const auto& b=single.slots()[4].effect_state->old_fireflies;
    for (std::size_t i=0;i<20;++i)
        require(a[i].x==b[i].x && a[i].y==b[i].y && a[i].period==b[i].period && a[i].phase==b[i].phase);
    require(bulk.surface().rgb==single.surface().rgb);
}
void chara_ghost_tests() {
    auto bg=pattern(4,4),t=pattern(4,4);
    for (std::size_t i=3;i<t->pixels.rgba.size();i+=8) t->pixels.rgba[i]=128;
    Layers l(4,4);l.bind(0,bg,false);l.bind(4,t,false);
    require(l.effect(4,110,{2,100,0,0,100,0,0}));
    const auto base=rgb(*bg);
    three(l,110,[&](int n) {
        auto pixels=base;const int up=(2-n)*128,down=n*128;
        for (int y=0;y<4;++y) for (int x=0;x<4;++x) {
            const int opacity=std::clamp(up+(down-up)*y/4,0,256);
            const auto src=static_cast<std::size_t>((y*4+x)*4),dst=static_cast<std::size_t>((y*4+x)*3);
            const int mask=t->pixels.rgba[src+3],alpha=mask*opacity/256;
            for (std::size_t c=0;c<3;++c) pixels[dst+c]=static_cast<Byte>(
                mask==255 && opacity==256?t->pixels.rgba[src+c]:
                ((t->pixels.rgba[src+c]*alpha)>>8)+((base[dst+c]*(256-alpha))>>8));
        }
        return pixels;
    });
    t->has_mask=false;
    require(l.effect(4,110,{2}));equal_frame(l,base,110,0);
}
void zoom_tests() {
    auto t=pattern(4,4);
    for (const int effect:{37,122}) {
        Layers l(4,4);l.bind(4,t,false);
        const std::vector<std::int32_t> args=effect==37?std::vector<std::int32_t>{2,1,1,2,2,0,0,100,0}:
            std::vector<std::int32_t>{2,1,1,2,2,0,0,100,0,1,1,0,1,0};
        require(l.effect(4,effect,args));
        three(l,effect,[&](int n) {
            const int percent=n*5000,x0=(percent+5000)/10000,x1=4+(-2*percent+5000)/10000;
            const int step=(x1-x0)*65536/4;
            std::vector<Byte> pixels(4U*4U*3U);
            for (int y=0;y<4;++y) for (int x=0;x<4;++x) {
                if (effect==122 && x>1) continue;
                const int sx=x0+(x*step>>16),sy=x0+(y*step>>16);
                const auto src=static_cast<std::size_t>((sy*4+sx)*4),dst=static_cast<std::size_t>((y*4+x)*3);
                for (std::size_t c=0;c<3;++c) pixels[dst+c]=t->pixels.rgba[src+c];
            }
            return pixels;
        });
    }
}
void mask_appear_tests() {
    auto bg=pattern(2,2),t=pattern(2,2),mask=pattern(2,2);
    t->pixels.rgba[3]=128;
    for (int reverse:{0,1}) {
        Layers l(2,2);l.bind(0,bg,false);l.bind(4,t,false);l.bind(5,mask,false);
        require(l.effect(5,66,{}));require(l.effect(4,123,{2,5,1,0,0,reverse,100,0}));
        const auto base=rgb(*bg);
        three(l,123,[&](int n) {
            auto pixels=base;const int shift=n*256-256;
            for (int i=0;i<4;++i) for (int c=0;c<3;++c) {
                const auto src=static_cast<std::size_t>(i*4+c),dst=static_cast<std::size_t>(i*3+c);
                const int m=reverse?255-mask->pixels.rgba[src]:mask->pixels.rgba[src];
                const int weight=std::clamp(m+shift,0,65535-0xff00),a=weight*t->pixels.rgba[static_cast<std::size_t>(i*4+3)]/256;
                pixels[dst]=static_cast<Byte>((t->pixels.rgba[src]*a+base[dst]*(256-a))>>8);
            }
            return pixels;
        });
    }
}
void zantetsu_tests() {
    auto t=pattern(6,6);Layers l(6,6);l.bind(4,t,false);
    require(l.effect(4,86,{2,0,2,3,3,2,1,0,0,100,100,0,0}));
    three(l,86,[&](int n) {
        std::vector<Byte> pixels(6U*6U*3U);
        for (int y=0;y<6;++y) for (int x=0;x<6;++x) {
            const int sy=x<3?y-n:y;
            if (sy<0) continue;
            const auto src=static_cast<std::size_t>((sy*6+x)*4),dst=static_cast<std::size_t>((y*6+x)*3);
            for (std::size_t c=0;c<3;++c) pixels[dst+c]=t->pixels.rgba[src+c];
        }
        return pixels;
    });
    t->has_mask=false;require(l.effect(4,86,{2}));
    equal_frame(l,std::vector<Byte>(6U*6U*3U),86,0);
}
void water_tests() {
    auto t=pattern(4,12);Layers l(4,12);l.bind(0,t,false);
    require(l.effect(4,24,{2,6,100,0}));
    const auto base=rgb(*t);
    three(l,24,[&](int n) {
        auto pixels=base;
        for (int y=6;y<12;++y) {
            const int k=y-6,depth=6;
            const double theta=(-(n+1)*3+(depth-k)*(depth-k)/depth*4)*(3.14159*2/100.0);
            int source=static_cast<int>(std::cos(theta)*8*(k+depth)/(depth*2)+0.5)+6-k-10;
            if (source>=6) source=5;
            source=std::max(0,source);
            for (int x=0;x<4;++x) for (int c=0;c<3;++c)
                pixels[static_cast<std::size_t>((y*4+x)*3+c)]=base[static_cast<std::size_t>((source*4+x)*3+c)];
        }
        return pixels;
    });
    require(l.slots()[4].effect_state->water_count==3);
    Layers bulk(4,12),single(4,12);bulk.bind(0,t,false);single.bind(0,t,false);
    require(bulk.effect(4,24,{0,6}));require(single.effect(4,24,{0,6}));
    bulk.advance(23);for (int i=0;i<23;++i) single.advance(1);
    require(bulk.slots()[4].effect_state->water_count==24);
    require(bulk.surface().rgb==single.surface().rgb);
}
void kagerou_tests() {
    auto t=pattern(4,4);Layers l(4,4);l.bind(0,t,false);
    require(l.effect(4,22,{2,0,4,2,100,100,100,1,0,0,0,0,0,0,0,0}));
    const auto base=rgb(*t);
    three(l,22,[&](int n) {
        auto pixels=base;const int d=n?1:0;
        for (int y=0;y<4;++y) for (int x=0;x<4-d;++x) for (int c=0;c<3;++c) {
            const auto to=static_cast<std::size_t>((y*4+x)*3+c),from=static_cast<std::size_t>((y*4+x+d)*3+c);
            pixels[to]=static_cast<Byte>(((base[to]*160)>>8)+((base[from]*96)>>8));
        }
        return pixels;
    });
}
void raster_tests() {
    auto t=pattern(4,4);Layers l(4,4);l.bind(0,t,false);
    require(l.effect(4,23,{2,0,1,0,0}));const auto base=rgb(*t);
    three(l,23,[&](int n) {
        auto pixels=base;
        if (n==2) {
            pixels[0]=0;pixels[1]=0;pixels[2]=0;
            for (int x=1;x<4;++x) for (int c=0;c<3;++c)
                pixels[static_cast<std::size_t>(x*3+c)]=base[static_cast<std::size_t>((x-1)*3+c)];
        }
        return pixels;
    });
}
void old_film_tests() {
    Layers l(8,4);const std::vector<std::int32_t> args{0,0,0,8,4,1,1,255,0,0,100,0,100,100,100};
    require(l.effect(4,55,args));require(l.slots()[4].effect_state->scratches.size()==1);
    const auto initial=l.slots()[4].effect_state->scratches[0];
    require(initial.x>=0 && initial.x<8*256 && initial.visible && initial.percent==100);
    three(l,55,[&](int) {
        std::vector<Byte> pixels(8U*4U*3U);
        const int x=static_cast<int>(l.slots()[4].effect_state->scratches[0].x/256);
        for (int y=0;y<4;++y) pixels[static_cast<std::size_t>((y*8+x)*3)]=255;
        return pixels;
    });
    Layers bulk(8,4),single(8,4);
    require(bulk.effect(4,55,args));require(single.effect(4,55,args));
    bulk.advance(37);for (int i=0;i<37;++i) single.advance(1);
    const auto& a=bulk.slots()[4].effect_state->scratches[0];
    const auto& b=single.slots()[4].effect_state->scratches[0];
    require(a.x==b.x && a.speed==b.speed && a.change==b.change && a.life==b.life && a.visible==b.visible);
    require(bulk.surface().rgb==single.surface().rgb);
    require(bulk.effect(4,55,{0,0,0,8,4,20,0}));
    require(bulk.slots()[4].effect_state->scratches.size()==10 && bulk.slots()[4].parameters[6]==1);
    auto bg=pattern(8,4);Layers timed(8,4);timed.bind(0,bg,false);
    auto fixed=args;fixed[10]=10;fixed[11]=0;fixed[13]=50;fixed[14]=50;
    require(timed.effect(4,55,fixed));const auto base=rgb(*bg);
    three(timed,55,[&](int n) {
        const auto& scratch=timed.slots()[4].effect_state->scratches[0];
        require(scratch.life==9-n && scratch.percent==50 && scratch.visible);
        auto pixels=base;const int x=static_cast<int>(scratch.x/256);
        for (int y=0;y<4;++y) for (int c=0;c<3;++c) {
            const auto i=static_cast<std::size_t>((y*8+x)*3+c);
            pixels[i]=static_cast<Byte>(((c==0?255:0)*128+base[i]*128)>>8);
        }
        return pixels;
    });
    timed.advance(6);require(timed.slots()[4].effect_state->scratches[0].life==1);
    timed.advance(1);require(timed.slots()[4].effect_state->scratches[0].life==10);
}
void old_lamp_tests() {
    auto t=pattern(2,2);Layers l(2,2);l.bind(4,t,false);
    const std::vector<std::int32_t> args{2,1,0,1,1,1,0};
    require(l.effect(4,109,args));const auto full=rgb(*t),empty=std::vector<Byte>(12);
    three(l,109,[&](int n) { return n==0?full:empty; });
    require(l.slots()[4].effect_state->lamp_mode==2 && l.slots()[4].effect_state->lamp_time==0);
    l.advance(1);require(l.slots()[4].effect_state->lamp_mode==0);
    Layers bulk(2,2),single(2,2);bulk.bind(4,t,false);single.bind(4,t,false);
    require(bulk.effect(4,109,{0,4,2,5,1,1,30}));require(single.effect(4,109,{0,4,2,5,1,1,30}));
    bulk.advance(37);for (int i=0;i<37;++i) single.advance(1);
    const auto& a=*bulk.slots()[4].effect_state;const auto& b=*single.slots()[4].effect_state;
    require(a.lamp_mode==b.lamp_mode && a.lamp_time==b.lamp_time && a.lamp_visible==b.lamp_visible);
    require(bulk.surface().rgb==single.surface().rgb);
    t->has_mask=false;
    require(l.effect(4,109,args));auto keyed=full;
    keyed[0]=0;keyed[1]=0;keyed[2]=0;
    equal_frame(l,keyed,109,0);
}
void mask_wipe_tests() {
    auto start=pattern(2,2),end=pattern(2,2),mask=pattern(2,2),bg=pattern(2,2);
    for (std::size_t i=0;i<end->pixels.rgba.size();i+=4) {
        end->pixels.rgba[i]=200;end->pixels.rgba[i+1]=120;end->pixels.rgba[i+2]=60;
        bg->pixels.rgba[i]=30;bg->pixels.rgba[i+1]=50;bg->pixels.rgba[i+2]=70;
    }
    for (int get:{0,1}) {
        Layers l(2,2);l.bind(0,bg,false);require(l.effect(2,73,{}));
        l.bind(4,start,false);l.bind(5,end,false);l.bind(6,mask,false);
        require(l.effect(5,66,{}));require(l.effect(6,66,{}));
        require(l.effect(4,60,{2,5,6,1,0,0,get,0,100,0}));
        const auto first=rgb(get?*bg:*start),last=rgb(*end);
        three(l,60,[&](int n) {
            std::vector<Byte> pixels(12);const int shift=n*256-256;
            for (int i=0;i<4;++i) for (int c=0;c<3;++c) {
                const auto dst=static_cast<std::size_t>(i*3+c),mi=static_cast<std::size_t>(i*4+c);
                const int weight=std::clamp(mask->pixels.rgba[mi]+shift,0,65535-0xff00);
                pixels[dst]=static_cast<Byte>((first[dst]*(256-weight)+last[dst]*weight)>>8);
            }
            return pixels;
        });
    }
}
void perspective_tests() {
    auto t=pattern(6,6);Layers l(6,6);l.bind(4,t,false);
    require(l.effect(4,67,{2,0,0,100,200,0,0,0,100,100,0,5,0,5}));
    three(l,67,[&](int n) {
        const double dot=100.0/(100+n*50);
        const auto step=static_cast<std::int64_t>(dot*65536);
        const auto origin=3*65536-step*3;
        std::vector<Byte> pixels(6U*6U*3U);
        for (int y=0;y<6;++y) for (int x=0;x<6;++x) {
            const int sx=static_cast<int>((origin+step*x)>>16),sy=static_cast<int>(y*dot+0.5);
            const auto src=static_cast<std::size_t>((sy*6+sx)*4),dst=static_cast<std::size_t>((y*6+x)*3);
            for (std::size_t c=0;c<3;++c) pixels[dst+c]=t->pixels.rgba[src+c];
        }
        return pixels;
    });
    auto bg=pattern(2,2),masked=pattern(2,2);
    for (std::size_t i=0;i<masked->pixels.rgba.size();i+=4) {
        masked->pixels.rgba[i]=200;masked->pixels.rgba[i+1]=100;masked->pixels.rgba[i+2]=40;
        masked->pixels.rgba[i+3]=128;
    }
    Layers m(2,2);m.bind(0,bg,false);m.bind(4,masked,false);
    require(m.effect(4,67,{2,0,0,100,100,2}));
    const auto base=rgb(*bg);
    three(m,67,[&](int) {
        auto pixels=base;
        for (int i=0;i<4;++i) for (int c=0;c<3;++c) {
            const auto di=static_cast<std::size_t>(i*3+c),si=static_cast<std::size_t>(i*4+c);
            pixels[di]=static_cast<Byte>((masked->pixels.rgba[si]*128+base[di]*127)>>8);
        }
        return pixels;
    });
}
void shard_tests() {
    auto t=pattern(8,8);Layers l(8,8);l.bind(4,t,false);
    const std::vector<std::int32_t> args{2,0,0,4,4,0,0,0,0,100,0,0,0,1,1,0,0,0};
    require(l.effect(4,81,args));require(l.slots()[4].effect_state->shards.size()==4);
    const auto src=static_cast<std::size_t>((2*8+1)*4),dst=static_cast<std::size_t>((2*8+1)*3);
    for (int n=0;n<3;++n) {
        const auto pixels=l.surface().rgb;
        for (std::size_t c=0;c<3;++c)
            require(pixels[dst+c]==static_cast<Byte>((t->pixels.rgba[src+c]*(n*128))>>8));
        if (n<2) l.advance(1);
    }
    auto moving=args;moving[1]=100;moving[2]=100;
    require(l.effect(4,81,moving));
    const auto& first=l.slots()[4].effect_state->shards[0];
    const double radius=std::sqrt(8.0),speed=30.0*(1.0+radius/200.0);
    require(std::abs(first.speed_x-static_cast<float>(std::cos(std::atan2(-2.0,-2.0))*speed))<0.0001F);
    require(std::abs(first.speed_y-static_cast<float>(std::sin(std::atan2(-2.0,-2.0))*speed))<0.0001F);
    require(first.gravity==1.0F);
    auto generated=args;generated[14]=0;
    require(l.effect(4,81,generated));require(l.slots()[4].effect_state->shards.size()==10);
    generated[14]=2;
    require(l.effect(4,81,generated));require(l.slots()[4].effect_state->shards.size()==5);
    generated[13]=0;
    require(l.effect(4,81,generated));require(l.slots()[4].effect_state->shards.size()==32);
    generated[13]=1;generated[14]=1;generated[16]=4;
    Layers bulk(8,8),single(8,8);bulk.bind(4,t,false);single.bind(4,t,false);
    require(bulk.effect(4,81,generated));require(single.effect(4,81,generated));
    const auto& a=bulk.slots()[4].effect_state->shards;
    const auto& b=single.slots()[4].effect_state->shards;
    for (std::size_t i=0;i<a.size();++i) require(a[i].points==b[i].points);
    bulk.advance(2);single.advance(1);single.advance(1);
    require(bulk.surface().rgb==single.surface().rgb);
}
void screen_layer_over_messages() {
    Layers layers(4,1);
    layers.capture_screen_layer(true);
    require(layers.effect(31,100,{20,1,0,25,25,1}));
    const auto native_scene=layers.surface().rgb;
    auto screen=layers.take_screen_layer();
    require(screen && screen->width()==4 && screen->height()==1);

    // A premultiplied red message pixel at column 1 moves to column 2.
    const std::array<Byte,16> message{0,0,0,0, 128,0,0,128, 0,0,0,0, 0,0,0,0};
    auto painted=message;
    screen->composite(painted.data(),painted.size());
    require(painted==std::array<Byte,16>{0,0,0,255, 0,0,0,255, 128,0,0,255, 0,0,0,255});
    require(layers.surface().rgb==native_scene);

    require(layers.effect(31,54,{0,0,255,0}));
    layers.surface();
    auto cover=layers.take_screen_layer();
    require(static_cast<bool>(cover));
    painted=message;
    cover->composite(painted.data(),painted.size());
    require(painted==std::array<Byte,16>{0,255,0,255, 0,255,0,255, 0,255,0,255, 0,255,0,255});

    layers.clear(31);
    layers.surface();
    require(!layers.screen_layer_active() && !layers.take_screen_layer());
    painted=message;
    screen->composite(painted.data(),painted.size());
    require(painted[8]==128 && painted[4]==0); // Detached from the live renderer.

    require(layers.effect(31,73,{}));
    layers.surface();
    auto capture=layers.take_screen_layer();
    require(capture && capture->captures_screen());
    painted=message;
    capture->composite(painted.data(),painted.size());
    require(capture->capture_without_ui().rgb==native_scene);
    layers.commit_screen_capture(capture->composed());
    layers.clear(31);
    require(layers.effect(0,2,{0,0,0,0,1}));
    require(layers.surface().rgb==std::vector<Byte>{0,0,0,128,0,0,0,0,0,0,0,0});
    layers.commit_screen_capture(capture->capture_without_ui());
    require(layers.needs_redraw() && layers.surface().rgb==native_scene);
}

void screen_effects() {
    const auto t=pattern(4,4);
    const auto initial=rgb(*t);
    {
        Layers l(4,4);l.bind(0,t,false);require(l.effect(8,20,{2,100,20,-20}));
        three(l,20,[&](int n) {
            auto result=initial;
            const int add[3]={lerp(0,100,n),lerp(0,20,n),lerp(0,-20,n)};
            for (std::size_t i=0;i<result.size();++i) result[i]=static_cast<Byte>(std::clamp(initial[i]+add[i%3],0,255));
            return result;
        });
    }
    {
        Layers l(4,4);l.bind(0,t,false);
        require(l.effect(8,50,{2,20,40,60,100,120,140,0,0,0,0,0,0,0,0,1,2}));
        three(l,50,[&](int n) {
            auto result=initial;
            for (int y=1;y<3;++y) for (int x=0;x<4;++x) for (int c=0;c<3;++c) {
                const int first=lerp(0,20+20*c,n),last=lerp(0,100+20*c,n);
                const int addition=first+(last-first)*(y-1)/2;
                const auto i=static_cast<std::size_t>((y*4+x)*3+c);
                result[i]=static_cast<Byte>(std::min(255,initial[i]+addition));
            }
            return result;
        });
    }
    {
        Layers l(4,4);l.bind(0,t,false);require(l.effect(8,31,{2,0,0,100,0}));
        three(l,31,[&](int n) {
            auto result=initial;
            const int a=lerp(254,-254,n);
            for (Byte& c:result) c=static_cast<Byte>(std::clamp(128+(static_cast<int>(c)-128)*a/256,0,255));
            return result;
        });
    }
    for (const int effect : {128,129}) {
        Layers l(4,4);l.bind(0,t,false);require(l.effect(8,effect,{2}));
        three(l,effect,[&](int n) {
            auto result=initial;const int weight=lerp(0,25600,n)/100;
            for (std::size_t i=0;i<result.size();i+=3) {
                const int y=(initial[i]*76+initial[i+1]*150+initial[i+2]*29)>>8;
                const int sepia[3]={107,74,43};
                for (std::size_t c=0;c<3;++c) {
                    const int shade=effect==128?(y*sepia[c])>>8:y;
                    result[i+c]=static_cast<Byte>((initial[i+c]*(256-weight)+shade*weight)>>8);
                }
            }
            return result;
        });
    }
    {
        Layers l(4,4);l.bind(0,t,false);require(l.effect(8,43,{2,2,2}));
        three(l,43,[&](int n) {
            auto result=initial;
            std::uint32_t random=(0x6d2b79f5U^(8U*0x9e3779b9U))^(static_cast<unsigned>(n)*0x9e3779b9U);
            for (int by=0;by<4;by+=2) for (int bx=0;bx<4;bx+=2) {
                const auto r=next_random(random)%16384U;
                const auto sample=static_cast<std::size_t>(((by+static_cast<int>((r>>8)%2))*4+bx+static_cast<int>((r&255)%2))*3);
                for (int y=by;y<by+2;++y) for (int x=bx;x<bx+2;++x)
                    for (int c=0;c<3;++c) result[static_cast<std::size_t>((y*4+x)*3+c)]=initial[sample+static_cast<std::size_t>(c)];
            }
            return result;
        });
    }
}
void character_colors() {
    const auto t=pattern(4,4);const auto initial=rgb(*t);
    for (const int effect : {124,127}) {
        Layers l(4,4);l.bind(4,t,false);require(l.effect(4,effect,{2}));
        three(l,effect,[&](int n) {
            auto result=initial;const int weight=lerp(0,25600,n)/100;
            for (std::size_t i=0;i<result.size();i+=3) {
                const int y=((initial[i+2]*29+initial[i+1]*150)>>8)+((initial[i]*76)>>8);
                const int sepia[3]={107,74,43};
                for (std::size_t c=0;c<3;++c) {
                    const int shade=effect==127?(y*sepia[c])>>8:y;
                    result[i+c]=static_cast<Byte>((initial[i+c]*(256-weight)+shade*weight)>>8);
                }
            }
            return result;
        });
    }
    {
        Layers l(4,4);l.bind(0,t,false);l.bind(4,t,false);require(l.effect(4,131,{2,100,0}));
        three(l,131,[&](int n) {
            auto result=initial;const int ps=lerp(0,100,n),scale=ps*256/100;
            if (ps) for (std::size_t i=0;i<result.size();++i) {
                const int sum=std::min(255,initial[i]+((initial[i]*scale)>>8));
                result[i]=static_cast<Byte>(((sum*255)>>8)+((initial[i])>>8));
            }
            return result;
        });
    }
}
void movement_effects() {
    const auto pixel=pattern(1,1);
    for (const int effect : {62,119}) {
        Layers l(8,3);l.bind(4,pixel,false);
        const std::vector<std::int32_t> params=effect==62?
            std::vector<std::int32_t>{2,0,0,2,0,4,0,6,0}:
            std::vector<std::int32_t>{2,0,0,2,0,4,0};
        require(l.effect(4,effect,params));
        three(l,effect,[&](int n) {
            std::vector<Byte> result(8*3*3);
            const float time=static_cast<float>(n)/2,rest=1-time;
            const int x=effect==62?static_cast<int>(3*rest*rest*time*2+3*rest*time*time*4+time*time*time*6+0.5F):
                                     static_cast<int>(2*rest*time*2+time*time*4+0.5F);
            for (int c=0;c<3;++c) result[static_cast<std::size_t>(x*3+c)]=pixel->pixels.rgba[static_cast<std::size_t>(c)];
            return result;
        });
    }
    for (const int effect : {62,119}) {
        Layers l(12,1);l.bind(4,pixel,false);
        require(l.effect(4,effect,effect==62?std::vector<std::int32_t>{3,1,0,7,0,2,0,8,0}:
                                             std::vector<std::int32_t>{3,1,0,7,0,2,0}));
        for (int n=0;n<=3;++n) {
            const float t=static_cast<float>((n*10000+1)/3)*0.0001F,u=1.0F-t;
            const float q0=u*u,q1=t*u,q2=t*t;
            const float x=effect==62?(q0*u)*1+3*(q1*u)*7+3*(q2*u)*2+(q2*t)*8:
                                       q0*1+2*q1*7+q2*2;
            std::vector<Byte> expected(36);
            const auto at=static_cast<std::size_t>(static_cast<int>(x+0.5F))*3U;
            std::copy_n(pixel->pixels.rgba.begin(),3,expected.begin()+static_cast<std::ptrdiff_t>(at));
            equal_frame(l,expected,effect,n);
            if (n!=3) l.advance(1);
        }
    }
    {
        Layers l(1,1);l.bind(4,pixel,false);l.rectangle(4,{-1,0,1,1},{0,0,1,1});
        require(l.effect(4,119,{2,0,0,0,0,0,0}));
        three(l,119,[&](int n) { return n?rgb(*pixel):std::vector<Byte>(3); });
    }
    {
        auto t=pattern(4,4);Layers l(4,4);l.bind(4,t,false);require(l.effect(4,94,{123,1,1,1}));
        const auto expected=[&](int n) {
            std::vector<Byte> result(4*4*3);
            for (int y=0;y<4;++y) for (int x=0;x<4;++x) for (int c=0;c<3;++c)
                result[static_cast<std::size_t>((y*4+x)*3+c)]=
                    t->pixels.rgba[static_cast<std::size_t>((((y-n)%4+4)%4*4+((x-n)%4+4)%4)*4+c)];
            return result;
        };
        equal_frame(l,expected(0),94,0);l.advance(4999);equal_frame(l,expected(4999),94,4999);
        l.advance(5000);equal_frame(l,expected(9999),94,9999);require(l.slots()[4].count==0);
    }
    const auto t=pattern(2,2);
    for (const int effect : {64,140}) {
        Layers l(4,4);l.bind(4,t,false);
        require(l.effect(4,effect,effect==64?std::vector<std::int32_t>{2,180,0,0,0,100,100,0}:
                                             std::vector<std::int32_t>{2,0,0,180,0}));
        three(l,effect,[&](int n) {
            std::vector<Byte> result(4*4*3);
            const int old[3][4][4]={
                {{1,1,0,0},{2,1,1,0},{1,2,0,1},{2,2,1,1}},
                {{1,2,1,0},{2,2,1,1},{1,3,0,0},{2,3,0,1}},
                {{2,2,1,1},{3,2,0,1},{2,3,1,0},{3,3,0,0}}
            };
            const int modern[3][4][4]={
                {{0,0,0,0},{1,0,1,0},{0,1,0,1},{1,1,1,1}},
                {{1,0,0,1},{2,0,0,0},{1,1,1,1},{2,1,1,0}},
                {{1,1,1,1},{2,1,0,1},{1,2,1,0},{2,2,0,0}}
            };
            for (int i=0;i<4;++i) {
                const int* q=effect==64?old[n][i]:modern[n][i];
                for (int c=0;c<3;++c)
                    result[static_cast<std::size_t>((q[1]*4+q[0])*3+c)]=
                        t->pixels.rgba[static_cast<std::size_t>((q[3]*2+q[2])*4+c)];
            }
            return result;
        });
    }
}
void composition_effects() {
    const auto t=pattern(4,4);const auto initial=rgb(*t);
    {
        Layers l(4,4);l.bind(0,t,false);require(l.effect(8,74,{2,2,2,200,100,100,100}));
        three(l,74,[&](int n) {
            auto result=initial;
            const double z=100.0+50*n;
            for (int qy=0;qy<2;++qy) for (int qx=0;qx<2;++qx) {
                const double x0=2+(qx?1:-1)*2*100.0/z,y0=2+(qy?1:-1)*2*100.0/z;
                const int fx=std::clamp(static_cast<int>(x0*65536),0,4*65536-1);
                const int fy=std::clamp(static_cast<int>(y0*65536),0,4*65536-1);
                const int stepx=static_cast<int>((2-x0)/2*65536),stepy=static_cast<int>((2-y0)/2*65536);
                for (int yy=0;yy<2;++yy) for (int xx=0;xx<2;++xx) {
                    const int x=qx?3-xx:xx,y=qy?3-yy:yy;
                    const int sx=(fx+stepx*xx)>>16,sy=(fy+stepy*yy)>>16;
                    for (int c=0;c<3;++c)
                        result[static_cast<std::size_t>((y*4+x)*3+c)]=result[static_cast<std::size_t>((sy*4+sx)*3+c)];
                }
            }
            return result;
        });
    }
    {
        auto red=pattern(1,1),blue=pattern(1,1);red->pixels.rgba={255,0,0,255};blue->pixels.rgba={0,0,255,255};
        Layers l(1,1);l.bind(4,red,false);l.bind(5,blue,false);require(l.effect(5,66,{}));require(l.effect(4,40,{2,5}));
        three(l,40,[&](int n) {
            const int a=(lerp(100,0,n)*256+50)/100,b=((100-lerp(100,0,n))*256+50)/100;
            const int a1=a*255>>8,a2=b*255>>8,total=a1+a2;
            return std::vector<Byte>{static_cast<Byte>(((255*a1>>8)*total)>>8),0,
                                     static_cast<Byte>(((255*a2>>8)*total)>>8)};
        });
    }
    {
        auto t=pattern(1,1);Layers l(1,1);l.bind(4,t,false);require(l.effect(4,40,{2,5,80,80}));
        l.advance(2);require(l.slots()[4].parameters[1]==80);
        require(l.effect(4,40,{1}));require(l.slots()[4].parameters[1]==80);
    }
    {
        Layers l(4,4);l.bind(4,t,false);require(l.effect(4,112,{2,0,2,0,0,0,100,0,0}));
        three(l,112,[&](int n) {
            auto result=initial;const int start=lerp(0,6,n);
            for (int y=0;y<4;++y) if (y+2-start>=0 && y+2-start<2) for (int x=0;x<4;++x) {
                const auto i=static_cast<std::size_t>((y*4+x)*3);
                result[i]=static_cast<Byte>(std::min(255,initial[i]+100));
            }
            return result;
        });
    }
    {
        Layers l(4,4);l.bind(4,t,false);require(l.effect(4,136,{2,1,0,0,1,0,100,0}));
        three(l,136,[&](int n) {
            std::vector<Byte> result(initial.size());const int ps=lerp(0,10000,n)*255/10000;
            for (int y=0;y<4;++y) for (int x=0;x<=1;++x) for (int c=0;c<3;++c) {
                const auto i=static_cast<std::size_t>((y*4+x)*3+c);
                result[i]=static_cast<Byte>((initial[i]*ps)>>8);
            }
            return result;
        });
    }
    {
        auto blank=pattern(14,145);
        for (std::size_t i=3;i<blank->pixels.rgba.size();i+=4) blank->pixels.rgba[i]=0;
        Layers l(14,145);l.bind(4,blank,false);
        require(l.effect(4,136,{2,-2,64,-7,-42,0,100,100,0,0,0,0,0,255,255,255,1}));
        const float ratio=-7.0F/-42.0F;
        const float x0=static_cast<float>(64+1)*-ratio-2.0F;
        const float x1=ratio*static_cast<float>(145+1-64)-2.0F;
        const float y0=-1.0F,y1=146.0F,dx=x1-x0,dy=y1-y0;
        const float extent=std::max(std::abs(dx),std::abs(dy)),inverse=1.0F/extent;
        std::vector<Byte> expected(14U*145U*3U);
        for (int i=0;i<static_cast<int>(extent+1);++i) {
            const float phase=static_cast<float>(i)*inverse;
            const int x=static_cast<int>(x0+dx*phase+0.5F),y=static_cast<int>(y0+dy*phase+0.5F);
            if (x>=0 && x<14 && y>=0 && y<145)
                std::fill_n(expected.begin()+static_cast<std::ptrdiff_t>((y*14+x)*3),3,254);
        }
        three(l,136,[&](int){return expected;});
    }
    {
        const auto texture=pattern(2,2);Layers l(2,2);l.bind(4,texture,false);require(l.effect(4,139,{2,2,2}));
        three(l,139,[&](int n) {
            std::uint32_t random=(0x6d2b79f5U^(4U*0x9e3779b9U))^(static_cast<unsigned>(n)*0x9e3779b9U);
            const auto x=next_random(random)%2,y=next_random(random)%2;
            std::vector<Byte> result(12);
            for (int i=0;i<4;++i) for (int c=0;c<3;++c)
                result[static_cast<std::size_t>(i*3+c)]=texture->pixels.rgba[static_cast<std::size_t>((y*2+x)*4+static_cast<unsigned>(c))];
            return result;
        });
    }
}
void blur_effects() {
    const auto t=pattern(4,4);const auto initial=rgb(*t);
    {
        Layers l(4,4);l.bind(4,t,false);require(l.effect(4,77,{2,4,2,100,0}));
        three(l,77,[&](int n) {
            if (n==0) return initial;
            std::vector<Byte> result(initial.size());
            const int kx=n==1?2:4,ky=n==1?1:2,div=std::min(32767,65536/(kx*ky));
            for (int y=0;y<4;++y) for (int x=0;x<4;++x) for (int c=0;c<3;++c) {
                const int sx=x*(4-kx+1)/4,sy=y*(4-ky+1)/4;
                int sum=0;
                for (int j=0;j<ky;++j) for (int i=0;i<kx;++i)
                    sum+=initial[static_cast<std::size_t>(((sy+j)*4+sx+i)*3+c)];
                result[static_cast<std::size_t>((y*4+x)*3+c)]=static_cast<Byte>((((sum>>1)*div)>>16)<<1);
            }
            return result;
        });
    }
    auto masked=pattern(4,4);
    masked->pixels.rgba[3]=0;masked->pixels.rgba[7]=128;masked->pixels.rgba[11]=64;
    for (const int effect : {125,126}) {
        Layers l(4,4);l.bind(4,masked,false);require(l.effect(4,effect,{2,4,2,100,0}));
        three(l,effect,[&](int n) {
            std::vector<Byte> result(4*4*3);
            if (!n) {
                for (int i=0;i<16;++i) for (int c=0;c<3;++c) {
                    const int col=masked->pixels.rgba[static_cast<std::size_t>(i*4+c)];
                    const int alpha=masked->pixels.rgba[static_cast<std::size_t>(i*4+3)];
                    result[static_cast<std::size_t>(i*3+c)]=static_cast<Byte>(effect==126||alpha==255?col:(col*alpha)>>8);
                }
                return result;
            }
            const int kx=n==1?2:4,ky=n==1?1:2,div=std::min(32767,65536/(kx*ky));
            for (int y=0;y<4;++y) for (int x=0;x<4;++x) {
                int sum[4]={};
                for (int sy=y-ky/2;sy<y+ky-ky/2;++sy) for (int sx=x-kx/2;sx<x+kx-kx/2;++sx) {
                    if (sx<0 || sy<0 || sx>=4 || sy>=4) continue;
                    const auto i=static_cast<std::size_t>((sy*4+sx)*4);
                    const int a=effect==126?1:masked->pixels.rgba[i+3];
                    for (int c=0;c<3;++c) sum[c]+=masked->pixels.rgba[i+static_cast<std::size_t>(c)]*a;
                    sum[3]+=a;
                }
                if (!sum[3]) continue;
                for (int c=0;c<3;++c) {
                    const int pixel=effect==126?std::clamp(((sum[c]>>1)*std::min(32767,32768/sum[3]))>>14,0,255):
                        std::clamp(((sum[c]>>16)*div+(((((sum[c]&65535)>>1)*div)>>16)<<1))>>8,0,255);
                    result[static_cast<std::size_t>((y*4+x)*3+c)]=static_cast<Byte>(pixel);
                }
            }
            return result;
        });
    }
}
void easing_tests() {
    auto white=pattern(1,1);white->pixels.rgba={255,255,255,255};
    {
        Layers l(1,1);l.bind(4,white,false);require(l.effect(4,9,{10,100,0,4,8}));
        l.advance(7);
        const int percent=static_cast<int>(100.0*7/8+0.5),alpha=(255*percent+50)/100;
        require(l.surface().rgb==std::vector<Byte>(3,static_cast<Byte>((255*alpha)>>8)));
    }
    {
        Layers l(1,1);l.bind(4,white,false);require(l.effect(4,9,{13,100,0,9,2}));
        l.advance(9);
        const int percent=50,alpha=(255*percent+50)/100;
        require(l.surface().rgb==std::vector<Byte>(3,static_cast<Byte>((255*alpha)>>8)));
    }
    {
        Layers l(1,1);l.bind(4,white,false);require(l.effect(4,9,{10,100,0,2,0}));
        require(l.surface().rgb==std::vector<Byte>({255,255,255}));
    }
    {
        auto t=pattern(1,1);t->pixels.rgba={25,50,100,0};
        Layers l(1,1);l.bind(0,t,false);require(l.effect(0,20,{2,0,0,0}));
        require(l.surface().rgb==std::vector<Byte>({25,50,100}));
    }
    {
        auto t=pattern(4,4);Layers bulk(4,4),single(4,4);
        bulk.bind(4,t,false);single.bind(4,t,false);
        require(bulk.effect(4,94,{0,3,-2,1}));require(single.effect(4,94,{0,3,-2,1}));
        bulk.advance(17);for (int i=0;i<17;++i) single.advance(1);
        require(bulk.surface().rgb==single.surface().rgb);require(bulk.slots()[4].count==single.slots()[4].count);
    }
}
void mask_presence_tests() {
    auto unmasked=pattern(2,2);unmasked->has_mask=false;
    const auto initial=rgb(*unmasked);
    auto keyed=initial;std::fill_n(keyed.begin(),3,0);
    for (const int effect : {112,131,136,139,140}) {
        Layers l(2,2);l.bind(4,unmasked,false);require(l.effect(4,effect,{2}));
        three(l,effect,[](int){return std::vector<Byte>(12);});
    }
    {
        Layers l(2,2);l.bind(4,unmasked,false);require(l.effect(4,125,{2,2,2,100,0}));
        three(l,125,[&](int count){return count==2?std::vector<Byte>(12):keyed;});
    }
    for (const int effect : {62,119}) {
        Layers l(2,2);l.bind(4,unmasked,false);
        require(l.effect(4,effect,std::vector<std::int32_t>(effect==62?11U:9U)));
        three(l,effect,[&](int){return keyed;});
    }
    {
        Layers l(4,4);l.bind(4,unmasked,false);require(l.effect(4,64,{2,0,0,0,0,100,100,2}));
        three(l,64,[&](int) {
            std::vector<Byte> result(48);
            for (int y=0;y<2;++y) for (int x=0;x<2;++x) for (int c=0;c<3;++c)
                if (x||y) result[static_cast<std::size_t>(((y+1)*4+x+1)*3+c)]=initial[static_cast<std::size_t>((y*2+x)*3+c)];
            return result;
        });
    }
    {
        auto red=pattern(1,1),blue=pattern(1,1);
        red->pixels.rgba={255,0,0,255};red->has_mask=false;
        blue->pixels.rgba={0,0,255,0};
        Layers l(1,1);l.bind(4,red,false);l.bind(5,blue,false);
        require(l.effect(5,66,{}));require(l.effect(4,40,{2,5}));
        three(l,40,[](int n) {
            const int first=(lerp(100,0,n)*256+50)/100,second=((100-lerp(100,0,n))*256+50)/100;
            return std::vector<Byte>{static_cast<Byte>((255*first)>>8),0,static_cast<Byte>((255*second)>>8)};
        });
    }
    {
        auto red=pattern(1,1),blue=pattern(1,1);blue->has_mask=false;
        Layers l(1,1);l.bind(4,red,false);l.bind(5,blue,false);
        require(l.effect(5,66,{}));require(l.effect(4,40,{2,5}));
        three(l,40,[](int){return std::vector<Byte>(3);});
    }
}
}
int main() {
    chart_layer_tests();
    chart_composite_tests();
    const auto image = source();
    auto dst = surface();
    require(blit(image, dst, {0, 0, 3, 2}, {0, 0}, BlendProfile::Source32Mmx) == Status::Ok);
    const std::array<Byte, 18> expected = {255,255,255,254,254,254,130,130,253,253,130,131,9,250,10,11,12,250};
    for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 3; ++x) for (unsigned c = 0; c < 3; ++c)
        require(dst.rgb[(y * 4 + x) * 3 + c] == expected[(y * 3 + x) * 3 + c]);
    // Check clipping through independent inverse coordinate lookup over the
    // destination, while the implementation iterates the clipped source span.
    for (int sy = 0; sy <= 2; ++sy) for (int sx = 0; sx <= 3; ++sx)
    for (int rh = 0; rh <= 2 - sy; ++rh) for (int rw = 0; rw <= 3 - sx; ++rw)
    for (int dy = -4; dy <= 4; ++dy) for (int dx = -5; dx <= 5; ++dx) {
        dst = surface();
        require(blit(image, dst, {sx, sy, rw, rh}, {dx, dy}, BlendProfile::Source32Mmx) == Status::Ok);
        for (int y = 0; y < 3; ++y) for (int x = 0; x < 4; ++x) for (int c = 0; c < 3; ++c) {
            Byte want = 255;
            if (x >= dx && y >= dy && x - dx < rw && y - dy < rh)
                want = expected[static_cast<unsigned>(((sy + y - dy) * 3 + sx + x - dx) * 3 + c)];
            require(dst.rgb[static_cast<unsigned>((y * 4 + x) * 3 + c)] == want);
        }
    }
    const std::array<Rect, 8> bad = {{{-1,0,1,1},{0,-1,1,1},{0,0,-1,1},{0,0,1,-1},
                                    {3,0,1,1},{0,2,1,1},{INT_MAX,0,1,1},{0,0,INT_MAX,1}}};
    for (const auto rect : bad) {
        dst = surface(); const auto before = dst.rgb;
        require(blit(image, dst, rect, {0,0}, BlendProfile::Source32Mmx) == Status::InvalidRectangle);
        require(dst.rgb == before);
    }
    for (const auto origin : {Point{INT_MIN,INT_MIN}, Point{INT_MAX,INT_MAX}, Point{INT_MIN,0}, Point{0,INT_MAX}}) {
        dst = surface(); const auto before = dst.rgb;
        require(blit(image, dst, {0,0,3,2}, origin, BlendProfile::Source32Mmx) == Status::Ok);
        require(dst.rgb == before);
    }
    for (unsigned bad_kind = 0; bad_kind < 5; ++bad_kind) {
        auto input = image; dst = surface();
        if (bad_kind == 0) input.rgba.pop_back();
        if (bad_kind == 1) input.width = UINT_MAX;
        if (bad_kind == 2) input.width = 0;
        if (bad_kind == 3) dst.rgb.pop_back();
        if (bad_kind == 4) dst.height = UINT_MAX;
        const auto before = dst.rgb;
        require(blit(input, dst, {0,0,1,1}, {0,0}, BlendProfile::Source32Mmx) == Status::InvalidImage);
        require(dst.rgb == before);
    }
    dst = surface(); const auto before = dst.rgb;
    require(blit(image, dst, {0,0,1,1}, {0,0}, static_cast<BlendProfile>(3)) == Status::InvalidProfile);
    require(dst.rgb == before);
    // Source32 rounds each product separately; Source64 rounds their sum.
    RgbaImage white{1,1,{255,255,255,128}}; Surface pixel{1,1,{255,255,255}};
    require(blit(white, pixel, {0,0,1,1}, {0,0}, BlendProfile::Source32Mmx) == Status::Ok && pixel.rgb[0] == 254);
    pixel.rgb.assign(3,255);
    require(blit(white, pixel, {0,0,1,1}, {0,0}, BlendProfile::Source64Cpp) == Status::Ok && pixel.rgb[0] == 255);
    // Model the Android presenter: it only asks for surface() when the renderer
    // advertises a changed frame. Both a timed and a clicked transition must
    // publish their exact target, including the zero-duration case.
    for (const int completion : {0,1,2}) {
        Layers layers(1,1);
        auto red=std::make_shared<Texture>();
        red->canvas_width=red->canvas_height=1;red->content={0,0,1,1};red->pixels={1,1,{255,0,0,255}};
        auto green=std::make_shared<Texture>(*red);green->pixels.rgba={0,255,0,255};
        layers.bind(0,red,false);layers.effect(0,0,{});
        require(layers.surface().rgb==std::vector<Byte>({255,0,0}));
        layers.prepare_overlap();layers.bind(0,green,false);
        layers.start_overlap(completion==2 ? 0U : 10U);
        if(completion!=2){
            require(layers.needs_redraw());require(layers.surface().rgb==std::vector<Byte>({255,0,0}));
            layers.advance(5);require(layers.needs_redraw());
            require(layers.surface().rgb==std::vector<Byte>({128,128,0}));
            require(!layers.needs_redraw());
            if(completion==0)layers.advance(5);else layers.finish_overlap();
        }
        require(!layers.overlapping());require(layers.needs_redraw());
        require(layers.surface().rgb==std::vector<Byte>({0,255,0}));require(!layers.needs_redraw());
        layers.advance(1);require(!layers.needs_redraw());
    }
    screen_effects();character_colors();movement_effects();composition_effects();blur_effects();easing_tests();mask_presence_tests();rain4_tests();rain2_tests();suna_tests();firefly_tests();chara_noise_tests();rain_tests();old_firefly_tests();chara_ghost_tests();zoom_tests();mask_appear_tests();zantetsu_tests();water_tests();kagerou_tests();raster_tests();old_film_tests();old_lamp_tests();mask_wipe_tests();perspective_tests();shard_tests();
    screen_layer_over_messages();wipe_tests();
    geometry_effects();animation_effects();condensation_effect();
    std::printf("PASS compositor: %u assertions\n", checks);
}
