#pragma once

#include "emberfall/layers.hpp"

namespace emberfall::render::effects {

struct RainPlane {
    std::int64_t x=0,y=0,speed_x=0,speed_y=0,random_x=0,random_y=0;
};

struct RainDrop {
    std::int64_t x=0,y=0,direction=0;
    int speed=0,length=0;
};

struct Firefly {
    float x=0,y=0,speed_x=0,speed_y=0;
    int frame=0,in=0,keep=0,out=0,phase=0,period=0;
};

struct OldFirefly {
    std::int64_t x=0,y=0,speed_x=0,speed_y=0;
    int phase=0,period=0;
};

struct NoiseBand {
    int remaining=0,y=0,length=0,strength=0;
};

struct FilmScratch {
    std::int64_t x=0,speed=0;
    int change=0,life=0,percent=0;
    bool visible=false;
};

struct Shard {
    std::vector<std::array<int,2>> points;
    float speed_x=0,speed_y=0,gravity=0;
};

struct Particle {
    int x=0,y=0,velocity_x=0,velocity_y=0;
    int phase=0,period=0,variant=0,row=0;
    bool reverse=false;
};

struct State {
    int offset_x=0,offset_y=0;
    int distance=0;
    std::uint32_t seed=0;
    std::array<RainPlane,4> rain{};
    bool rain_initialized=false;
    std::vector<std::uint32_t> noise;
    unsigned noise_phase=0;
    std::vector<Firefly> fireflies;
    std::vector<NoiseBand> bands;
    std::vector<int> raster;
    std::vector<RainDrop> drops;
    std::vector<OldFirefly> old_fireflies;
    int water_count=1;
    std::vector<FilmScratch> scratches;
    int lamp_mode=0,lamp_time=0;
    bool lamp_visible=false;
    std::vector<Shard> shards;
    bool once_drawn=false;
    std::vector<Particle> particles;
    std::vector<Byte> condensation;
};

using AnimationTexture=std::function<std::shared_ptr<const Texture>(const std::string&)>;

bool known(int number);
bool known_v5(int number);
bool continuous(int number);
void configure(Layer& layer,unsigned slot,int width,int height,const FragmentMesh* mesh=nullptr);
unsigned duration(const Layer& layer);
void advance(Layer& layer,int width,int height,Point cursor={-1,-1});
int animation_frame(const Layer& layer,bool off);
void draw(Surface& out,const std::array<Layer,32>& layers,unsigned slot,const Surface& saved,
          const AnimationTexture& animation={},bool animation_off=false,bool speaking=false);
double progress(double tick,int length,int type,int delay);
void paint(Surface& out,const Texture& texture,Rect src,Rect dst,int percent,
           std::array<int,3> add,bool alpha);

}
