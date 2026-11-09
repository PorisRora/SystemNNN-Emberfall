#include "emberfall/game_session.hpp"
#include "frame_pixels.hpp"
#include "nnn/image.hpp"
#include "nnn/reference_selection.hpp"
#include "resource_names_jni.hpp"
#include <android/native_window_jni.h>
#include <android/log.h>
#include <jni.h>
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <stdexcept>

namespace {
using emberfall::scene::GameSession;
using emberfall::render::ChartLayerSource;
using emberfall::render::ChartLayerTarget;
using emberfall::render::Layers;
using emberfall::render::Surface;
using emberfall::render::Texture;
using emberfall::android::LockedBitmap;
struct UiWipe {
    Surface before,after,output;
    UiWipe(unsigned width,unsigned height):before{width,height,{}},after{width,height,{}},
        output{width,height,std::vector<std::uint8_t>(static_cast<std::size_t>(width)*height*3)}{}
};
bool wipe_dimensions(jint width,jint height) {
    return width>0 && height>0 && width<=4096 && height<=4096 &&
        static_cast<std::int64_t>(width)*height<=2*1024*1024;
}
void read_wipe_bitmap(JNIEnv* env,jobject image,Surface& dst) {
    LockedBitmap source(env,image);
    dst.rgb.resize(static_cast<std::size_t>(dst.width)*dst.height*3);
    std::vector<std::size_t> columns(dst.width);
    for (unsigned x=0;x<dst.width;++x)
        columns[x]=static_cast<std::size_t>(static_cast<std::uint64_t>(x)*source.info.width/dst.width)*4;
    auto* output=dst.rgb.data();
    for (unsigned y=0;y<dst.height;++y) {
        const auto* row=source.row(static_cast<unsigned>(static_cast<std::uint64_t>(y)*source.info.height/dst.height));
        for (const auto pixel:columns) {
            std::copy_n(row+pixel,3,output);
            output+=3;
        }
    }
}
struct Preview {
    struct TextureStamp {
        std::weak_ptr<const Texture> texture;
        std::uint64_t id=0;
    };
    std::unique_ptr<GameSession> game;
    ANativeWindow* window=nullptr;
    std::string script,variable;
    int mode=12,page=1;
    bool restart_on_page=false,loop=true;
    bool primed=false,repaint=true;
    std::weak_ptr<nnn::RuntimeContext> warm_owner;
    std::optional<nnn::RuntimeContext::PreviewState> warm_state;
    emberfall::render::ChartLayerScratch chart_scratch;
    std::array<TextureStamp,32> chart_textures;
    std::uint64_t chart_texture_serial=0,chart_layer_serial=0,last_chart_revision=0;
    int last_chart_center=-1;
    ~Preview(){if(window)ANativeWindow_release(window);}
    bool place_mode() const {
        return mode==static_cast<int>(nnn::RuntimeMode::SelectPlace) || mode==static_cast<int>(nnn::RuntimeMode::SelectPlace2);
    }
    bool object_mode() const {return mode==static_cast<int>(nnn::RuntimeMode::SelectObject);}
    bool zukan_mode() const {return mode==static_cast<int>(nnn::RuntimeMode::SelectZukan);}
    bool chart_mode() const {return mode==static_cast<int>(nnn::RuntimeMode::SelectChart);}
    int bound_page(int n) const {
        if(place_mode())return std::clamp(n,0,64);
        if(object_mode())return std::clamp(n,0,1024);
        if(zukan_mode())return std::clamp(n,0,nnn::reference_selection::max_zukan_entries);
        return std::clamp(n,1,32);
    }
    void apply_page(){
        if(place_mode()||object_mode())game->set_ui_place_preview(static_cast<unsigned>(page));
        else if(!variable.empty())game->set_ui_variable(variable,page);
    }
    void restart(){
        // Install the decorative mode and one-based page before any VM step.
        game->start_ui_background(script,mode);
        apply_page();
        repaint=true;
    }
};
std::string raw(JNIEnv* env,jbyteArray bytes,int maximum){
    if(!bytes)return {};
    auto size=env->GetArrayLength(bytes);if(size<0||size>maximum)throw std::runtime_error("UI argument exceeds limit");
    std::string s(static_cast<std::size_t>(size),'\0');
    if(size)env->GetByteArrayRegion(bytes,0,size,reinterpret_cast<jbyte*>(s.data()));
    if(s.find('\0')!=std::string::npos)throw std::runtime_error("NUL in UI argument");return s;
}
void fail(JNIEnv* env,const char* s){if(!env->ExceptionCheck())env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),s);}
Preview& preview(jlong handle){if(!handle)throw std::runtime_error("Closed UI session");return *reinterpret_cast<Preview*>(handle);}
std::shared_ptr<nnn::RuntimeContext> context(jlong token){
    return token ? *reinterpret_cast<const std::shared_ptr<nnn::RuntimeContext>*>(token) : nullptr;
}
Preview& prepared_preview(jlong handle){
    auto& p=preview(handle);
    if(!p.primed)throw std::logic_error("UI background has no initial frame");
    return p;
}
unsigned chart_slot(jint slot) {
    if (slot<0 || slot>=32) throw std::invalid_argument("Invalid chart layer slot");
    return static_cast<unsigned>(slot);
}
void require_chart_canvas(const Layers& layers) {
    if (!wipe_dimensions(static_cast<jint>(layers.width()),static_cast<jint>(layers.height())))
        throw std::invalid_argument("Invalid chart layer canvas");
}
bool require_chart_bitmap(const Layers& layers,const LockedBitmap& image,bool allow_opaque) {
    const auto alpha=image.info.flags&ANDROID_BITMAP_FLAGS_ALPHA_MASK;
    const bool opaque=alpha==ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE;
    if (image.info.width!=layers.width() || image.info.height!=layers.height() ||
        (alpha!=ANDROID_BITMAP_FLAGS_ALPHA_PREMUL && !(allow_opaque && opaque)))
        throw std::invalid_argument("Chart bitmap must be logical-size premultiplied RGBA");
    return opaque;
}
ChartLayerTarget chart_target(const LockedBitmap& image) {
    return {image.row(0),image.info.stride,std::size_t{image.info.stride}*image.info.height};
}
bool usable_chart_texture(const Texture* texture) {
    if (!texture) return false;
    const auto w=texture->canvas_width,h=texture->canvas_height;
    constexpr auto dimension_limit=static_cast<int>(nnn::image::max_dimension);
    if (w<=0 || h<=0 || w>dimension_limit || h>dimension_limit) return false;
    const auto& crop=texture->content;
    const auto& pixels=texture->pixels;
    return crop.x>=0 && crop.y>=0 && crop.width>0 && crop.height>0 &&
        crop.width<=w-crop.x && crop.height<=h-crop.y &&
        pixels.width==static_cast<unsigned>(crop.width) && pixels.height==static_cast<unsigned>(crop.height) &&
        pixels.rgba.size()==std::size_t{pixels.width}*pixels.height*4;
}
std::uint32_t texture_rgb(const std::uint8_t* src) noexcept {
    return (std::uint32_t{src[0]}<<16U)|(std::uint32_t{src[1]}<<8U)|src[2];
}
// Return the logical image, restoring the cropped DWQ margins. Tiles use the
// opaque Put path. The repeated center uses Blt's mask or top-left color key.
jintArray chart_texture_pixels(JNIEnv* env,const Texture& texture,bool transparent,jint budget) {
    constexpr int pixel_limit=4*1024*1024;
    const int width=texture.canvas_width,height=texture.canvas_height;
    const int area=width*height;
    const bool include_pixels=width<=4096 && height<=4096 && area<=budget && area<=pixel_limit;
    auto result=env->NewIntArray(include_pixels ? 2+area : 2);
    if (!result) return nullptr;
    const jint dimensions[]{width,height};
    env->SetIntArrayRegion(result,0,2,dimensions);
    if (!include_pixels || env->ExceptionCheck()) return result;
    constexpr int batch=16384;
    std::array<jint,batch> pixels;
    const auto& crop=texture.content;
    const bool keyed=transparent && !texture.has_mask;
    const auto key=keyed && crop.x==0 && crop.y==0 ? texture_rgb(texture.pixels.rgba.data()) : 0U;
    const auto outside=transparent ? jint{0} : static_cast<jint>(0xff000000U);
    const int rows=batch/width;
    for (int top=0;top<height;top+=rows) {
        const int count=std::min(rows,height-top);
        std::fill_n(pixels.data(),width*count,outside);
        for (int y=std::max(top,crop.y);y<std::min(top+count,crop.y+crop.height);++y) {
            const auto* src=texture.pixels.rgba.data()+static_cast<std::size_t>(y-crop.y)*texture.pixels.width*4U;
            auto* row=pixels.data()+(y-top)*width+crop.x;
            for (int x=0;x<crop.width;++x,src+=4) {
                const auto color=texture_rgb(src);
                const unsigned alpha=transparent ? (keyed ? (color==key ? 0U : 255U) : src[3]) : 255U;
                row[x]=static_cast<jint>((alpha<<24U)|color);
            }
        }
        env->SetIntArrayRegion(result,2+top*width,width*count,pixels.data());
        if (env->ExceptionCheck()) return nullptr;
    }
    return result;
}
void step(Preview& p,const emberfall::scene::InputState& input){
    const auto& state=p.game->step_ui_background(input,p.loop);
    if(state.wait==emberfall::scene::Wait::Error)throw std::runtime_error(state.error);
}
void present(Preview& p){
    if(!p.window||(!p.repaint&&!p.game->needs_redraw()))return;
    const auto& src=p.game->surface();ANativeWindow_Buffer dst{};
    if(ANativeWindow_lock(p.window,&dst,nullptr)!=0){p.repaint=true;return;}
    if(dst.width==static_cast<int>(src.width)&&dst.height==static_cast<int>(src.height)&&dst.format==WINDOW_FORMAT_RGBA_8888){
        auto row=static_cast<unsigned char*>(dst.bits);const auto* rgb=src.rgb.data();
        for(unsigned y=0;y<src.height;++y,row+=static_cast<std::size_t>(dst.stride)*4){
            auto pixel=row;for(unsigned x=0;x<src.width;++x,pixel+=4,rgb+=3){pixel[0]=rgb[0];pixel[1]=rgb[1];pixel[2]=rgb[2];pixel[3]=255;}
        }p.repaint=false;
    }else p.repaint=true;
    if(ANativeWindow_unlockAndPost(p.window)!=0)p.repaint=true;
}
}
#define API extern "C" JNIEXPORT
#define FN(n) Java_com_tyranor_runtime_systemnnn_NativeUiPreview_##n
API jlong JNICALL FN(create)(JNIEnv* env,jclass,jbyteArray path,jlong runtime,jlong asset_peer){
    try{
        const auto root=raw(env,path,32768);
        if(root.empty())throw std::invalid_argument("Missing UI game directory");
        emberfall::scene::SessionOptions options;
        options.filename_encoder=emberfall::android::filename_encoder(env);
        options.runtime=context(runtime);
        options.cache_bytes=48U*1024U*1024U;
        auto p=std::make_unique<Preview>();
        p->game=GameSession::create_ui_preview(std::filesystem::u8path(root),std::move(options),
            asset_peer ? preview(asset_peer).game.get() : nullptr);
        return reinterpret_cast<jlong>(p.release());
    }catch(const std::exception& e){fail(env,e.what());return 0;}
}
API jboolean JNICALL FN(reset)(JNIEnv* env,jclass,jlong handle,jbyteArray script,jint mode,jbyteArray variable,
                               jint page,jboolean restart_on_page,jboolean loop,jlong runtime,jboolean speculative){
    try{
        auto& p=preview(handle);
        if(p.window)throw std::logic_error("Detach the UI background surface before resetting it");
        auto name=raw(env,script,255);
        auto page_variable=raw(env,variable,255);
        if(name.empty()||mode<1||mode>64)throw std::invalid_argument("Invalid UI background configuration");
        p.primed=false;
        auto live=context(runtime);
        auto bound=live;
        p.warm_state.reset();p.warm_owner.reset();
        if(speculative && live){
            bound=live->fork();
            p.warm_owner=live;p.warm_state=bound->preview_state();
        }
        p.game->reset_ui_background(name,mode,std::move(bound));
        if(p.chart_mode() && mode!=static_cast<int>(nnn::RuntimeMode::SelectChart)) p.chart_scratch={};
        p.chart_textures={};p.last_chart_center=-1;
        p.script=std::move(name);p.variable=std::move(page_variable);
        p.mode=mode;p.page=p.bound_page(page);p.restart_on_page=restart_on_page==JNI_TRUE;
        p.loop=loop==JNI_TRUE;
        p.apply_page();p.repaint=true;
        return JNI_TRUE;
    }catch(const std::exception& e){fail(env,e.what());return JNI_FALSE;}
}
API jboolean JNICALL FN(claim)(JNIEnv* env,jclass,jlong handle,jlong runtime){
    try{
        auto& p=prepared_preview(handle);
        auto live=context(runtime);
        if(p.window||!p.warm_state||p.warm_owner.lock()!=live||
           !p.game->claim_ui_background(std::move(live),*p.warm_state))return JNI_FALSE;
        p.warm_state.reset();p.warm_owner.reset();
        return JNI_TRUE;
    }catch(const std::exception& e){fail(env,e.what());return JNI_FALSE;}
}
API void JNICALL FN(surface)(JNIEnv* env,jclass,jlong handle,jobject surface){
    try{
        auto& p=surface ? prepared_preview(handle) : preview(handle);
        if(surface && p.warm_state)throw std::logic_error("Claim the prepared menu before binding its window");
        if(p.window)ANativeWindow_release(p.window);
        p.window=surface?ANativeWindow_fromSurface(env,surface):nullptr;p.repaint=true;
        if(p.window){
            const auto& s=p.game->surface();
            ANativeWindow_setBuffersGeometry(p.window,s.width,s.height,WINDOW_FORMAT_RGBA_8888);
            present(p);
        }
    }catch(const std::exception& e){fail(env,e.what());}
}
API void JNICALL FN(savedScreen)(JNIEnv* env,jclass,jlong handle,jintArray pixels,jint width,jint height){
    try {
        if (!pixels || width<1 || height<1 || width>4096 || height>4096 ||
            static_cast<std::int64_t>(width)*height>2*1024*1024 || env->GetArrayLength(pixels)!=width*height)
            throw std::invalid_argument("Invalid menu screen dimensions");
        auto& p=preview(handle);
        if(p.script.empty()||p.primed)throw std::logic_error("Set the saved UI screen before priming");
        emberfall::render::Surface frame{static_cast<unsigned>(width),static_cast<unsigned>(height),{}};
        frame.rgb.resize(static_cast<std::size_t>(width)*height*3);
        constexpr jsize batch=4096;
        jint values[batch];
        for (jsize offset=0;offset<width*height;offset+=batch){
            const auto count=std::min(batch,width*height-offset);
            env->GetIntArrayRegion(pixels,offset,count,values);
            if (env->ExceptionCheck()) return;
            for (jsize i=0;i<count;++i){
                const auto pixel=static_cast<std::uint32_t>(values[i]);
                const auto at=static_cast<std::size_t>(offset+i)*3;
                frame.rgb[at]=static_cast<std::uint8_t>(pixel>>16);
                frame.rgb[at+1]=static_cast<std::uint8_t>(pixel>>8);
                frame.rgb[at+2]=static_cast<std::uint8_t>(pixel);
            }
        }
        p.game->set_ui_saved_screen(std::move(frame));p.repaint=true;
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API void JNICALL FN(page)(JNIEnv* env,jclass,jlong handle,jint page){
    try{
        auto& p=prepared_preview(handle);int n=p.bound_page(page);
        if(n!=p.page){
            p.page=n;
            // Zukan saves the current backdrop before entering detail. The
            // grid and cursor live outside the decorative surface.
            if(p.zukan_mode()&&n>0)p.game->set_ui_saved_screen(p.game->surface(),false);
            if(p.restart_on_page){
                p.restart();
                emberfall::scene::InputState input;input.mouse_x=input.mouse_y=-1;
                step(p,input);
                present(p);
            }
            else {p.apply_page();if(p.place_mode()||p.object_mode())present(p);}
        }
    }
    catch(const std::exception& e){fail(env,e.what());}
}
API void JNICALL FN(prime)(JNIEnv* env,jclass,jlong handle){
    try{
        auto& p=preview(handle);
        if(p.primed)return;
        emberfall::scene::InputState input;input.mouse_x=input.mouse_y=-1;
        step(p,input);
        // The chart composes its initial frame from split ranges. Keep its
        // one-shot effects available for that first presentation.
        if(!p.chart_mode()) p.game->surface();
        p.primed=true;
    }catch(const std::exception& e){fail(env,e.what());}
}
API void JNICALL FN(capture)(JNIEnv* env,jclass,jlong handle,jobject bitmap){
    try{
        const auto& src=prepared_preview(handle).game->surface();
        LockedBitmap dst(env,bitmap);
        const auto width=dst.info.width,height=dst.info.height;
        if(!wipe_dimensions(width,height))throw std::invalid_argument("Invalid menu capture dimensions");
        std::vector<std::size_t> columns(width);
        for(unsigned x=0;x<width;++x)columns[x]=static_cast<std::size_t>(std::uint64_t{x}*src.width/width)*3;
        for(unsigned y=0;y<height;++y){
            const auto* row=src.rgb.data()+static_cast<std::size_t>(std::uint64_t{y}*src.height/height)*src.width*3;
            auto* pixel=dst.row(y);
            for(auto offset:columns){
                pixel[0]=row[offset];pixel[1]=row[offset+1];pixel[2]=row[offset+2];pixel[3]=255;
                pixel+=4;
            }
        }
    }catch(const std::exception& e){fail(env,e.what());}
}
API jint JNICALL FN(chartLayerMask)(JNIEnv* env,jclass,jlong handle,jint center){
    try{
        const auto& layers=prepared_preview(handle).game->chart_preview_layers();
        return static_cast<jint>(layers.chart_layer_mask(chart_slot(center)));
    }catch(const std::exception& e){fail(env,e.what());return 0;}
}
API jlong JNICALL FN(chartLayerRevision)(JNIEnv* env,jclass,jlong handle,jint center){
    try{
        auto& p=prepared_preview(handle);
        const auto& layers=p.game->chart_preview_layers();
        if (!layers.chart_layer_mask(chart_slot(center))) {p.last_chart_center=-1;return 0;}
        if (p.last_chart_center!=center || p.last_chart_revision!=layers.composition_revision()) {
            p.last_chart_center=center;p.last_chart_revision=layers.composition_revision();++p.chart_layer_serial;
        }
        return static_cast<jlong>(p.chart_layer_serial);
    }catch(const std::exception& e){fail(env,e.what());return 0;}
}
API jboolean JNICALL FN(chartNeedsComposite)(JNIEnv* env,jclass,jlong handle,jint center){
    try {
        const auto& layers=prepared_preview(handle).game->chart_preview_layers();
        return layers.chart_needs_composite(chart_slot(center)) ? JNI_TRUE : JNI_FALSE;
    } catch (const std::exception& e) {fail(env,e.what());return JNI_FALSE;}
}
API jint JNICALL FN(chartLayers)(JNIEnv* env,jclass,jlong handle,jint center,jobject before,jobject after,jobject foreground){
    try{
        auto& p=prepared_preview(handle);
        auto& layers=p.game->chart_preview_layers();
        const auto slot=chart_slot(center);
        const auto mask=layers.chart_layer_mask(slot);
        if (!mask) return 0;
        if (layers.chart_needs_composite(slot)) throw std::logic_error("Chart effects require the composed backdrop");
        require_chart_canvas(layers);
        const std::array<jobject,3> images{before,after,foreground};
        std::array<std::optional<LockedBitmap>,3> locks;
        std::array<ChartLayerTarget,3> targets;
        for (unsigned group=0;group<images.size();++group) {
            if (!(mask&(1U<<group))) continue;
            for (unsigned prior=0;prior<group;++prior)
                if (locks[prior] && env->IsSameObject(images[prior],images[group])==JNI_TRUE)
                    throw std::invalid_argument("Chart layer targets must be distinct");
            auto& image=locks[group].emplace(env,images[group]);
            require_chart_bitmap(layers,image,false);
            targets[group]=chart_target(image);
        }
        return static_cast<jint>(layers.draw_chart_layers(slot,targets,p.chart_scratch));
    }catch(const std::exception& e){fail(env,e.what());return 0;}
}
API jboolean JNICALL FN(chartComposite)(JNIEnv* env,jclass,jlong handle,jint center,
                                        jobject base,jobject center_plane,jobject content,jobject output){
    try {
        auto& p=prepared_preview(handle);
        auto& layers=p.game->chart_preview_layers();
        const auto slot=chart_slot(center);
        if (!layers.chart_needs_composite(slot)) return JNI_FALSE;
        require_chart_canvas(layers);
        const std::array<jobject,4> images{base,center_plane,content,output};
        std::array<std::optional<LockedBitmap>,4> locks;
        std::array<ChartLayerSource,3> sources;
        ChartLayerTarget target;
        for (std::size_t i=0;i<images.size();++i) {
            if (!images[i] && (i==1 || i==2)) continue;
            for (std::size_t prior=0;prior<i;++prior)
                if (locks[prior] && env->IsSameObject(images[prior],images[i])==JNI_TRUE)
                    throw std::invalid_argument("Chart composition bitmaps must be distinct");
            auto& image=locks[i].emplace(env,images[i]);
            const auto opaque=require_chart_bitmap(layers,image,true);
            const auto bytes=chart_target(image);
            if (i<sources.size()) sources[i]={bytes.pixels,bytes.stride,bytes.size,opaque};
            else target=bytes;
        }
        return layers.draw_chart_composite(slot,sources[0],sources[1],sources[2],target,p.chart_scratch) ?
            JNI_TRUE : JNI_FALSE;
    } catch (const std::exception& e) {fail(env,e.what());return JNI_FALSE;}
}
API jlongArray JNICALL FN(chartTextureIds)(JNIEnv* env,jclass,jlong handle,jint tiles,jint center){
    try{
        if (tiles<0 || tiles>4) throw std::invalid_argument("Invalid chart script tile count");
        auto& p=prepared_preview(handle);
        const auto& slots=p.game->chart_preview_layers().slots();
        const auto center_slot=chart_slot(center);
        std::array<jlong,5> ids{};
        for (jint i=0;i<=tiles;++i) {
            const auto slot=i==tiles ? center_slot : static_cast<unsigned>(i);
            const auto& texture=slots[slot].texture;
            auto& stamp=p.chart_textures[slot];
            if (!usable_chart_texture(texture.get())) {stamp={};continue;}
            if (stamp.texture.lock()!=texture) {
                stamp.texture=texture;stamp.id=++p.chart_texture_serial;
            }
            ids[static_cast<std::size_t>(i)]=static_cast<jlong>(stamp.id);
        }
        auto result=env->NewLongArray(tiles+1);
        if (result) env->SetLongArrayRegion(result,0,tiles+1,ids.data());
        return result;
    }catch(const std::exception& e){fail(env,e.what());return nullptr;}
}
API jintArray JNICALL FN(chartTextureWithinBudget)(JNIEnv* env,jclass,jlong handle,jint slot,jboolean transparent,jint budget){
    try{
        const auto& layers=prepared_preview(handle).game->chart_preview_layers();
        const auto& texture=layers.slots()[chart_slot(slot)].texture;
        if (!usable_chart_texture(texture.get())) return nullptr;
        return chart_texture_pixels(env,*texture,transparent==JNI_TRUE,budget);
    }catch(const std::exception& e){fail(env,e.what());return nullptr;}
}
API void JNICALL FN(tick)(JNIEnv* env,jclass,jlong handle,jint x,jint y,jboolean clicked,jboolean held,jint frames){
    try{
        if(frames<1||frames>1024)throw std::invalid_argument("Invalid UI background frame count");
        auto& p=prepared_preview(handle);
        emberfall::scene::InputState input;input.mouse_x=x;input.mouse_y=y;
        input.advance=clicked==JNI_TRUE;input.held=held==JNI_TRUE;
        for(int frame=0;frame<frames;++frame){
            step(p,input);
            input.advance=false;
        }
        present(p);
    }catch(const std::exception& e){fail(env,e.what());}
}
API void JNICALL FN(close)(JNIEnv*,jclass,jlong handle){delete reinterpret_cast<Preview*>(handle);}
API jlong JNICALL FN(createWipe)(JNIEnv* env,jclass,jobject before,jobject after,jint color,jint width,jint height) {
    try {
        if (!wipe_dimensions(width,height))
            throw std::invalid_argument("Invalid UI wipe dimensions");
        auto wipe=std::make_unique<UiWipe>(static_cast<unsigned>(width),static_cast<unsigned>(height));
        read_wipe_bitmap(env,before,wipe->before);
        if (after) read_wipe_bitmap(env,after,wipe->after);
        else {
            auto& dst=wipe->after.rgb;
            dst.resize(wipe->before.rgb.size());
            const auto argb=static_cast<std::uint32_t>(color);
            for (std::size_t p=0;p<dst.size();p+=3) {
                dst[p]=static_cast<std::uint8_t>(argb>>16);dst[p+1]=static_cast<std::uint8_t>(argb>>8);
                dst[p+2]=static_cast<std::uint8_t>(argb);
            }
        }
        return reinterpret_cast<jlong>(wipe.release());
    } catch(const std::exception& e) {fail(env,e.what());return 0;}
}
API void JNICALL FN(drawWipe)(JNIEnv* env,jclass,jlong handle,jobject image,jint type,jint frame,jint frames) {
    try {
        if (!handle || !emberfall::render::supports_wipe(type) || frame<0 || frames<1 || frame>frames || frames>1000)
            throw std::invalid_argument("Invalid UI wipe frame");
        auto& wipe=*reinterpret_cast<UiWipe*>(handle);
        emberfall::render::draw_wipe(wipe.output,wipe.before,wipe.after,type,
            static_cast<unsigned>(frame),static_cast<unsigned>(frames));
        LockedBitmap target(env,image);
        if (target.info.width!=wipe.output.width || target.info.height!=wipe.output.height)
            throw std::invalid_argument("UI wipe target dimensions differ");
        const auto* source=wipe.output.rgb.data();
        for (unsigned y=0;y<wipe.output.height;++y) {
            auto* dst=target.row(y);
            for (unsigned x=0;x<wipe.output.width;++x,dst+=4,source+=3) {
                std::copy_n(source,3,dst);dst[3]=255;
            }
        }
    } catch(const std::exception& e) {fail(env,e.what());}
}
API void JNICALL FN(closeWipe)(JNIEnv*,jclass,jlong handle){delete reinterpret_cast<UiWipe*>(handle);}
