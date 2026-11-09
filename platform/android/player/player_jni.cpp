#include "emberfall/game_session.hpp"
#include "frame_pixels.hpp"
#include "resource_names_jni.hpp"
#include "nnn/runtime_context.hpp"
#include <android/native_window_jni.h>
#include <android/log.h>
#include <jni.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace {
using emberfall::scene::GameSession;
struct StoryScreenLayer {
    StoryScreenLayer(std::shared_ptr<emberfall::render::ScreenLayerFrame> source,std::array<jint,3> stamp)
        :frame(std::move(source)),state(stamp) {}
    std::shared_ptr<emberfall::render::ScreenLayerFrame> frame;
    std::array<jint,3> state;
    std::mutex mutex;
    std::condition_variable ready;
    emberfall::render::Surface sample;
    bool committed=false;
};
using PublishedScreenLayer=std::shared_ptr<StoryScreenLayer>;
struct Player {
    std::unique_ptr<GameSession> game;
    ANativeWindow* window=nullptr;
    unsigned width=0,height=0;
    bool repaint=true;
    std::uint64_t remainder=0;
    bool title=false,loop_title=false;
    std::shared_ptr<emberfall::render::ScreenLayerFrame> screen_layer;
    PublishedScreenLayer published_screen_layer;
    std::array<jint,3> screen_layer_state{};
    std::uint64_t screen_layer_revision=0;
    std::string back_script;
    emberfall::render::Surface static_title;
    ~Player() { if (window) ANativeWindow_release(window); }
};
Player& player(jlong handle) { return *reinterpret_cast<Player*>(handle); }
void fail(JNIEnv* env,const char* message) {
    if (!env->ExceptionCheck()) env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),message);
}
jbyteArray bytes(JNIEnv* env,std::string_view value) {
    auto size=static_cast<jsize>(value.size());
    auto out=env->NewByteArray(size);
    if (out && size) env->SetByteArrayRegion(out,0,size,reinterpret_cast<const jbyte*>(value.data()));
    return out;
}
void array_text(JNIEnv* env,jobjectArray result,jsize index,const std::string& value) {
    auto encoded=bytes(env,value);
    if (encoded) { env->SetObjectArrayElement(result,index,encoded); env->DeleteLocalRef(encoded); }
}
jintArray int_array(JNIEnv* env,const std::vector<int>& values) {
    const auto size=static_cast<jsize>(values.size());
    auto array=env->NewIntArray(size);
    if (array && size) env->SetIntArrayRegion(array,0,size,values.data());
    return array;
}
std::vector<std::string> read_text_fields(JNIEnv* env,jobjectArray array,jsize max_fields,jsize max_bytes) {
    if (!array || env->GetArrayLength(array)>max_fields) throw std::invalid_argument("Invalid text field count");
    const auto count=env->GetArrayLength(array);
    std::vector<std::string> values;
    values.reserve(static_cast<std::size_t>(count));
    for (jsize i=0;i<count;++i) {
        auto item=static_cast<jbyteArray>(env->GetObjectArrayElement(array,i));
        if (!item) throw std::invalid_argument("Missing text field");
        const auto size=env->GetArrayLength(item);
        if (size>max_bytes) {
            env->DeleteLocalRef(item);
            throw std::invalid_argument("Text field exceeds byte limit");
        }
        std::string value(static_cast<std::size_t>(size),'\0');
        if (size) env->GetByteArrayRegion(item,0,size,reinterpret_cast<jbyte*>(value.data()));
        env->DeleteLocalRef(item);
        if (env->ExceptionCheck()) throw std::runtime_error("Unable to read text field");
        values.push_back(std::move(value));
    }
    return values;
}
std::string dialogue_body(const std::string& text,const std::string& speaker) {
    if (speaker.empty() || text.compare(0,speaker.size(),speaker)!=0) return text;
    auto start=speaker.size();
    if (start<text.size() && text[start]=='\r') ++start;
    if (start<text.size() && text[start]=='\n') ++start;
    return text.substr(start);
}
std::string resource_name(JNIEnv* env,jbyteArray raw) {
    if (!raw) return {};
    const auto n=env->GetArrayLength(raw);
    if (n>255) throw std::runtime_error("UI resource name too long");
    std::string value(static_cast<std::size_t>(n),'\0');
    if (n) env->GetByteArrayRegion(raw,0,n,reinterpret_cast<jbyte*>(value.data()));
    return value;
}
bool draws_story_ui(const Player& p) {
    return !p.title && p.game->story_draws_screen_layer();
}
std::array<jint,3> story_stamp(const Player& p) {
    const auto& state=p.game->state();
    return {static_cast<jint>(state.wait),static_cast<jint>(state.word),
        static_cast<jint>(state.dialogue.sequence)};
}
void flush_screen_capture(Player& p) {
    const auto& screen=p.published_screen_layer;
    if (!screen || !screen->frame->captures_screen() || !draws_story_ui(p) ||
        screen->state!=story_stamp(p) || p.screen_layer_revision!=p.game->screen_layer_revision()) return;
    std::unique_lock lock(screen->mutex);
    // A script may consume GET on the next tick. The UI only writes this
    // detached frame, so it can finish the draw while the game thread waits.
    if (p.window && !screen->committed)
        screen->ready.wait_for(lock,std::chrono::milliseconds(45),[&] { return !screen->sample.rgb.empty(); });
    if (screen->sample.rgb.empty() && screen->committed) return;
    auto sample=std::exchange(screen->sample,{});
    screen->committed=true;
    lock.unlock();
    if (sample.rgb.empty()) sample=screen->frame->capture_without_ui();
    p.game->commit_screen_capture(std::move(sample));
}
void present(Player& p) {
    p.game->capture_screen_layer(p.window && draws_story_ui(p));
    if (!p.window || (!p.repaint && !p.game->needs_redraw())) return;
    const auto& src=p.static_title.rgb.empty() ? p.game->surface() : p.static_title;
    ANativeWindow_Buffer dst{};
    if (ANativeWindow_lock(p.window,&dst,nullptr)!=0) { p.repaint=true; return; }
    if (dst.width==static_cast<int>(src.width) && dst.height==static_cast<int>(src.height) && dst.format==WINDOW_FORMAT_RGBA_8888) {
        auto* row=static_cast<std::uint8_t*>(dst.bits);
        const auto* rgb=src.rgb.data();
        for (unsigned y=0;y<src.height;++y,row+=static_cast<std::size_t>(dst.stride)*4) {
            auto* pixel=row;
            for (unsigned x=0;x<src.width;++x,pixel+=4,rgb+=3) {
                pixel[0]=rgb[0]; pixel[1]=rgb[1]; pixel[2]=rgb[2]; pixel[3]=255;
            }
        }
        p.repaint=false;
    } else p.repaint=true;
    if (ANativeWindow_unlockAndPost(p.window)!=0) p.repaint=true;
}
void frame_meta(JNIEnv* env,Player& p,jintArray meta) {
    const auto& f=p.game->state();
    const jint values[]={static_cast<jint>(f.wait),static_cast<jint>(f.word),static_cast<jint>(f.dialogue.sequence),
        static_cast<jint>(p.width),static_cast<jint>(p.height),static_cast<jint>(p.game->frame_ms()),
        static_cast<jint>(f.wait==emberfall::scene::Wait::Choice?f.choice_remaining_ms:f.remaining_frames)};
    env->SetIntArrayRegion(meta,0,7,values);
    if (env->GetArrayLength(meta)>=10) {
        const jint cursor=f.script_cursor;
        env->SetIntArrayRegion(meta,9,1,&cursor);
    }
    if (env->GetArrayLength(meta)>=11) {
        const jint message=static_cast<jint>(p.game->draw_message());
        env->SetIntArrayRegion(meta,10,1,&message);
    }
}
emberfall::scene::Wait step_title(Player& p) {
    emberfall::scene::InputState input;
    input.mouse_x=input.mouse_y=-1;
    const auto& state=p.game->step_ui_background(input,p.loop_title);
    if(state.wait==emberfall::scene::Wait::Error)throw std::runtime_error(state.error);
    return state.wait;
}
}
#define API extern "C" JNIEXPORT
#define FN(name) Java_com_tyranor_runtime_systemnnn_NativeGame_##name

namespace {
using RuntimeRef=std::shared_ptr<nnn::RuntimeContext>;
jlong open_game(JNIEnv* env,jbyteArray path,jint rate,emberfall::scene::SessionOptions options={}) {
        if (!path) throw std::invalid_argument("Missing game directory");
        const auto size=env->GetArrayLength(path);
        if (size<=0 || size>32768) throw std::runtime_error("Invalid game directory");
        std::string root(static_cast<std::size_t>(size),'\0');
        env->GetByteArrayRegion(path,0,size,reinterpret_cast<jbyte*>(root.data()));
        if (env->ExceptionCheck()) return 0;
        options.audio.sample_rate=static_cast<unsigned>(rate);
        options.queue_audio_events=false;
        options.filename_encoder=emberfall::android::filename_encoder(env);
        auto p=std::make_unique<Player>();
        p->game=std::make_unique<GameSession>(std::filesystem::u8path(root),options);
        const auto& surface=p->game->surface();
        p->width=surface.width; p->height=surface.height;
        return reinterpret_cast<jlong>(p.release());
}
}
API jlong JNICALL FN(open)(JNIEnv* env,jclass,jbyteArray path,jint rate) {
    try { return open_game(env,path,rate); }
    catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API jlong JNICALL FN(openWithRuntime)(JNIEnv* env,jclass,jbyteArray path,jint rate,jlong token) {
    try {
        if (!token) throw std::invalid_argument("Missing runtime context");
        emberfall::scene::SessionOptions options;
        options.runtime=*reinterpret_cast<const RuntimeRef*>(token);
        return open_game(env,path,rate,std::move(options));
    } catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API jlong JNICALL FN(openReplay)(JNIEnv* env,jclass,jbyteArray path,jint rate,jlong token,
                               jbyteArray script,jintArray films,jint hidden_layers) {
    try {
        if (!token || !films) throw std::invalid_argument("Missing replay context or film list");
        const auto count=env->GetArrayLength(films);
        if (count<=0 || count>4096) throw std::invalid_argument("Invalid replay film count");
        std::vector<jint> raw(static_cast<std::size_t>(count));
        env->GetIntArrayRegion(films,0,count,raw.data());
        if (env->ExceptionCheck()) return 0;
        emberfall::scene::SessionOptions options;
        options.entry=resource_name(env,script);
        if (!nnn::resource::valid_component(options.entry))
            throw std::invalid_argument("Invalid replay script name");
        options.runtime=*reinterpret_cast<const RuntimeRef*>(token);
        options.replay_hidden_layers=static_cast<std::uint32_t>(hidden_layers);
        for (jint film:raw) {
            if (film<0) throw std::invalid_argument("Invalid replay film number");
            options.replay_films.push_back(static_cast<nnn::Word>(film));
        }
        return open_game(env,path,rate,std::move(options));
    } catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API jlong JNICALL FN(retainContext)(JNIEnv* env,jclass,jlong handle) {
    try { return reinterpret_cast<jlong>(new RuntimeRef(player(handle).game->runtime())); }
    catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API jlong JNICALL FN(cloneContext)(JNIEnv* env,jclass,jlong token) {
    try {
        if (!token) throw std::invalid_argument("Missing runtime context");
        return reinterpret_cast<jlong>(new RuntimeRef(*reinterpret_cast<const RuntimeRef*>(token)));
    } catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API jlong JNICALL FN(forkContext)(JNIEnv* env,jclass,jlong token) {
    try {
        if (!token) throw std::invalid_argument("Missing runtime context");
        return reinterpret_cast<jlong>(new RuntimeRef((*reinterpret_cast<const RuntimeRef*>(token))->fork()));
    } catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API void JNICALL FN(releaseContext)(JNIEnv*,jclass,jlong token) {
    delete reinterpret_cast<RuntimeRef*>(token);
}
API void JNICALL FN(close)(JNIEnv*,jclass,jlong handle) {
    std::unique_ptr<Player> p(reinterpret_cast<Player*>(handle));
    if (!p) return;
    const auto scene=p->game->stats();
    const auto audio=p->game->audio().stats();
    __android_log_print(ANDROID_LOG_INFO,"SystemNNN","closed: images=%zu audioEvents=%llu decodedClips=%llu missingAudio=%llu audioErrors=%llu",
        scene.decodes,static_cast<unsigned long long>(audio.events),static_cast<unsigned long long>(audio.decoded),
        static_cast<unsigned long long>(audio.missing),static_cast<unsigned long long>(audio.errors));
}
API void JNICALL FN(surface)(JNIEnv* env,jclass,jlong handle,jobject surface) {
    auto& p=player(handle);
    if (p.window) ANativeWindow_release(p.window);
    p.window=surface ? ANativeWindow_fromSurface(env,surface) : nullptr;
    p.repaint=true;
    if (p.window) {
        ANativeWindow_setBuffersGeometry(p.window,static_cast<int>(p.width),static_cast<int>(p.height),WINDOW_FORMAT_RGBA_8888);
        present(p);
    } else p.game->capture_screen_layer(false);
}
API jboolean JNICALL FN(configureMenuBackground)(JNIEnv* env,jclass,jlong handle,jint mode,jbyteArray script,
                                                jbyteArray background,jboolean repeat) {
    try {
        auto& p=player(handle);
        p.title=true; p.loop_title=repeat; p.back_script=resource_name(env,script);
        p.static_title={};
        p.repaint=true;
        if (!p.back_script.empty()) {
            try { p.game->start_ui_background(p.back_script,mode);step_title(p); }
            catch (const std::exception& e) { p.back_script.clear(); __android_log_print(ANDROID_LOG_WARN,"SystemNNN","menu script: %s",e.what()); }
        }
        if (p.back_script.empty()) p.game->start_ui_background({},mode);
        const auto image=resource_name(env,background);
        if (p.back_script.empty() && !image.empty()) {
            try {
                auto texture=p.game->ui_texture(image);
                p.static_title={p.width,p.height,std::vector<emberfall::render::Byte>(std::size_t{p.width}*p.height*3)};
                emberfall::render::blit(texture->pixels,p.static_title,
                    {0,0,texture->content.width,texture->content.height},{texture->content.x,texture->content.y},
                    emberfall::render::BlendProfile::Source64Cpp);
            } catch (const std::exception& e) { __android_log_print(ANDROID_LOG_WARN,"SystemNNN","menu image: %s",e.what()); }
        }
        return !p.back_script.empty() || !p.static_title.rgb.empty();
    } catch (const std::exception& e) { fail(env,e.what()); return JNI_FALSE; }
}
API jint JNICALL FN(advanceOpening)(JNIEnv* env,jclass,jlong handle,jint frames) {
    try {
        auto& p=player(handle);
        const auto period=p.game->frame_ms();
        if (p.back_script.empty()) return static_cast<jint>(period);
        bool ended=false;
        for (int i=0;i<std::clamp(frames,1,64);++i) {
            using emberfall::scene::Wait;
            const auto wait=step_title(p);
            if (wait==Wait::Ended) { ended=true; break; }
            if (wait!=Wait::Running && wait!=Wait::Draw && wait!=Wait::Overlap)
                throw std::runtime_error("Opening background waits for unsupported input");
            if (p.game->frame_ms()!=period) break;
        }
        present(p);
        return ended?0:static_cast<jint>(p.game->frame_ms());
    } catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API void JNICALL FN(restartTitle)(JNIEnv* env,jclass,jlong handle) {
    try {
        auto& p=player(handle);
        if(!p.back_script.empty()){
            p.game->start_ui_background(p.back_script,4,true);
            step_title(p);
        }
        p.repaint=true;
        present(p);
    } catch(const std::exception& e) { fail(env,e.what()); }
}
static jintArray ui_sprite(JNIEnv* env,jlong handle,jbyteArray raw,int max_pixels,bool use_color_key=false) {
    try {
        auto t=player(handle).game->ui_texture(resource_name(env,raw));
        const auto w=t->canvas_width,h=t->canvas_height;
        if (w<=0 || h<=0 || static_cast<std::int64_t>(w)*h>max_pixels) return nullptr;
        std::vector<jint> pixels(2+static_cast<std::size_t>(w)*h);
        pixels[0]=w; pixels[1]=h;
        const auto rgb=[](const unsigned char* src) {
            return (std::uint32_t(src[0])<<16)|(std::uint32_t(src[1])<<8)|src[2];
        };
        const bool keyed=use_color_key && !t->has_mask;
        const auto key=keyed && t->content.x==0 && t->content.y==0 && !t->pixels.rgba.empty() ? rgb(t->pixels.rgba.data()) : 0U;
        for (int y=0;y<t->content.height;y++) {
            auto row=pixels.data()+2+(t->content.y+y)*w+t->content.x;
            const auto* src=t->pixels.rgba.data()+static_cast<std::size_t>(y)*t->pixels.width*4;
            for(int x=0;x<t->content.width;++x,src+=4){
                const auto color=rgb(src);
                row[x]=static_cast<jint>((std::uint32_t(keyed && color==key ? 0 : src[3])<<24)|color);
            }
        }
        auto result=env->NewIntArray(static_cast<jsize>(pixels.size()));
        if (result) env->SetIntArrayRegion(result,0,static_cast<jsize>(pixels.size()),pixels.data());
        return result;
    } catch (const std::exception& e) { __android_log_print(ANDROID_LOG_WARN,"SystemNNN","title button: %s",e.what()); return nullptr; }
}
API jintArray JNICALL FN(titleSprite)(JNIEnv* env,jclass,jlong handle,jbyteArray raw) {
    return ui_sprite(env,handle,raw,2*1024*1024);
}
API jintArray JNICALL FN(gallerySprite)(JNIEnv* env,jclass,jlong handle,jbyteArray raw) {
    return ui_sprite(env,handle,raw,6*1024*1024);
}
API jintArray JNICALL FN(keyedSprite)(JNIEnv* env,jclass,jlong handle,jbyteArray raw) {
    return ui_sprite(env,handle,raw,6*1024*1024,true);
}
API void JNICALL FN(tick)(JNIEnv* env,jclass,jlong handle,jint ms,jint x,jint y,jboolean advance,jint choice,jboolean replay,jintArray meta) {
    try {
        const auto length=meta ? env->GetArrayLength(meta) : 0;
        if (length<7 || ms<0) throw std::invalid_argument("Invalid frame input");
        const bool with_clock=length>=9;
        jint clock[2]{};
        if (replay && with_clock) {
            env->GetIntArrayRegion(meta,7,2,clock);
            if (env->ExceptionCheck()) return;
            if (clock[0]< -1) throw std::invalid_argument("Invalid journal clock mode");
        }
        auto& p=player(handle);
        if (p.title) {
            if (ms>0 && !p.back_script.empty()) {
                try {
                    const int frames=std::clamp(ms/static_cast<int>(p.game->frame_ms()),1,64);
                    for (int i=0;i<frames;++i)
                        if (step_title(p)==emberfall::scene::Wait::Ended && !p.loop_title) break;
                }
                catch(const std::exception& e) {
                    __android_log_print(ANDROID_LOG_WARN,"SystemNNN","title background: %s",e.what());
                    p.back_script.clear();
                }
            }
            present(p);
            const jint info[]={6,0,0,static_cast<jint>(p.width),static_cast<jint>(p.height),static_cast<jint>(p.game->frame_ms()),0};
            env->SetIntArrayRegion(meta,0,7,info);
            if (with_clock) {const jint empty[]={0,0};env->SetIntArrayRegion(meta,7,2,empty);}
            if (length>=10) {const jint cursor=0;env->SetIntArrayRegion(meta,9,1,&cursor);}
            if (length>=12) {
                const jint ended=!p.loop_title && !p.back_script.empty() &&
                    p.game->state().wait==emberfall::scene::Wait::Ended;
                env->SetIntArrayRegion(meta,11,1,&ended);
            }
            return;
        }
        if (replay) {
            const auto samples=static_cast<unsigned>(ms)*std::uint64_t{p.game->audio().sample_rate()}+p.remainder;
            p.game->audio().skip_frames(samples/1000);
            p.remainder=samples%1000;
        }
        emberfall::scene::InputState input;
        input.mouse_x=x; input.mouse_y=y; input.advance=advance; input.choice=static_cast<unsigned>(choice);
        input.play_movies=replay!=JNI_TRUE;
        input.expire_choices=replay!=JNI_TRUE;
        input.legacy_calculations=replay==JNI_TRUE && clock[0]==0;
        input.recorded_clock=replay==JNI_TRUE && clock[0]!=0;
        if (input.recorded_clock && clock[0]>0)
            input.clock_sample=emberfall::scene::ScriptClockSample{clock[0],clock[1]};
        if (!replay) flush_screen_capture(p);
        const auto& f=p.game->step_frame(static_cast<unsigned>(ms),input);
        if (f.wait==emberfall::scene::Wait::Error) throw std::runtime_error(f.script+" @"+std::to_string(f.word)+" "+f.error);
        const auto sampled=p.game->script_clock_sample();
        if (input.clock_sample && !sampled) throw std::runtime_error("Unused journal clock sample");
        if (!replay) present(p);
        frame_meta(env,p,meta);
        if (with_clock) {
            const jint values[]={sampled ? sampled->date : 0,sampled ? sampled->milliseconds : 0};
            env->SetIntArrayRegion(meta,7,2,values);
        }
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API jobjectArray JNICALL FN(content)(JNIEnv* env,jclass,jlong handle) {
    const auto& f=player(handle).game->state();
    const auto count=f.selection ? f.selection->options.size() : 0;
    auto result=env->NewObjectArray(static_cast<jsize>(2+count),env->FindClass("[B"),nullptr);
    if (!result) return nullptr;
    auto set=[&](int i,const std::string& value) {
        auto b=bytes(env,value);
        if (b) { env->SetObjectArrayElement(result,i,b); env->DeleteLocalRef(b); }
    };
    set(0,f.script); set(1,f.dialogue.display_bytes);
    for (std::size_t i=0;i<count && !env->ExceptionCheck();++i) set(static_cast<int>(i+2),f.selection->options[i]);
    return result;
}
API void JNICALL FN(frameInfo)(JNIEnv* env,jclass,jlong handle,jintArray meta) {
    if (!meta || env->GetArrayLength(meta)<7) {fail(env,"Missing frame metadata");return;}
    frame_meta(env,player(handle),meta);
}
API jint JNICALL FN(runtimeMode)(JNIEnv*,jclass,jlong handle) {
    return player(handle).game->mode();
}
API jlong JNICALL FN(systemMenuModes)(JNIEnv* env,jclass,jlong handle,jlong requested) {
    try {
        auto& p=player(handle);
        auto mask=static_cast<std::uint64_t>(requested);
        for (unsigned mode=1;mode<=49;++mode) {
            const auto bit=std::uint64_t{1}<<mode;
            if ((mask&bit) && !p.game->menu_extension_enabled(static_cast<nnn::RuntimeMode>(mode))) mask&=~bit;
        }
        return static_cast<jlong>(mask);
    }catch(const std::exception& e){fail(env,e.what());return 0;}
}
API jintArray JNICALL FN(modalInfo)(JNIEnv* env,jclass,jlong handle) {
    const auto& modal=player(handle).game->state().modal;
    if (!modal) return nullptr;
    std::vector<int> fields{modal->request,static_cast<int>(modal->mode),
        modal->return_mode ? static_cast<int>(*modal->return_mode) : 0};
    fields.insert(fields.end(),modal->arguments.begin(),modal->arguments.end());
    return int_array(env,fields);
}
API jintArray JNICALL FN(modalLimits)(JNIEnv* env,jclass,jlong handle) {
    const auto& modal=player(handle).game->state().modal;
    return modal ? int_array(env,modal->byte_limits) : nullptr;
}
API jobjectArray JNICALL FN(modalText)(JNIEnv* env,jclass,jlong handle) {
    const auto& modal=player(handle).game->state().modal;
    if (!modal) return nullptr;
    auto out=env->NewObjectArray(static_cast<jsize>(modal->text.size()),env->FindClass("[B"),nullptr);
    if (!out) return nullptr;
    for (std::size_t i=0;i<modal->text.size() && !env->ExceptionCheck();++i)
        array_text(env,out,static_cast<jsize>(i),modal->text[i]);
    return out;
}
API jintArray JNICALL FN(modalView)(JNIEnv* env,jclass,jlong handle) {
    const auto& modal=player(handle).game->state().modal;
    if (!modal) return nullptr;
    std::vector<int> fields{modal->page,modal->pages,modal->group,modal->selection,modal->balance};
    fields.reserve(5+2*modal->rows.size());
    for (const auto& row:modal->rows) {
        fields.push_back(row.id);
        fields.push_back(row.flags);
    }
    return int_array(env,fields);
}
API jobjectArray JNICALL FN(modalRows)(JNIEnv* env,jclass,jlong handle) {
    const auto& modal=player(handle).game->state().modal;
    if (!modal) return nullptr;
    auto out=env->NewObjectArray(static_cast<jsize>(3*modal->rows.size()),env->FindClass("[B"),nullptr);
    if (!out) return nullptr;
    jsize index=0;
    for (const auto& row:modal->rows) {
        if (env->ExceptionCheck()) break;
        array_text(env,out,index++,row.label);
        array_text(env,out,index++,row.asset);
        array_text(env,out,index++,row.detail);
    }
    return out;
}
API jintArray JNICALL FN(chartGeometry)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto& modal=player(handle).game->state().modal;
        if (!modal || !modal->chart) return nullptr;
        const auto& v=*modal->chart;
        std::vector<int> fields{v.scroll,v.limit,v.extent,v.center_y,
            v.slot_x,v.slot_y,v.slot_area_width,v.slot_area_height,v.slot_width,v.slot_height,
            v.mini_x,v.mini_y,v.mini_width,v.mini_height,v.mini_base_x,v.mini_base_y,
            v.pointer_y,v.pending,v.page_percent,v.total_percent,
            static_cast<int>(v.page_states.size()),static_cast<int>(v.tiles.size()),static_cast<int>(v.nodes.size()),
            static_cast<int>(v.roads.size()),static_cast<int>(v.description.size()),v.back_extent};
        fields.reserve(fields.size()+v.page_states.size()+v.tiles.size()+11*v.nodes.size()+5*v.roads.size());
        fields.insert(fields.end(),v.page_states.begin(),v.page_states.end());
        for (const auto& tile:v.tiles) fields.push_back(tile.y);
        for (const auto& node:v.nodes)
            fields.insert(fields.end(),{node.id,node.x,node.y,node.style,node.access,node.sx,node.sy,node.sw,node.sh,node.routes,node.seen});
        for (const auto& road:v.roads) fields.insert(fields.end(),{road.x1,road.y1,road.x2,road.y2,road.style});
        return int_array(env,fields);
    } catch (const std::exception& e) { fail(env,e.what());return nullptr; }
}
API jobjectArray JNICALL FN(chartText)(JNIEnv* env,jclass,jlong handle) {
    const auto& modal=player(handle).game->state().modal;
    if (!modal || !modal->chart) return nullptr;
    const auto& v=*modal->chart;
    auto out=env->NewObjectArray(static_cast<jsize>(5+v.tiles.size()+v.nodes.size()+v.description.size()),
        env->FindClass("[B"),nullptr);
    if (!out) return nullptr;
    jsize index=0;
    const auto append=[&](const std::string& value) {
        if (!env->ExceptionCheck()) array_text(env,out,index++,value);
    };
    append(v.center);append(v.minimap);append(v.unseen);append(v.current);append(v.complete);
    for (const auto& tile:v.tiles) append(tile.asset);
    for (const auto& node:v.nodes) append(node.asset);
    for (const auto& line:v.description) append(line);
    return out;
}
API void JNICALL FN(setChartStorage)(JNIEnv* env,jclass,jlong handle,jbyteArray path) {
    try {
        const auto directory=resource_name(env,path);
        if (env->ExceptionCheck()) return;
        if (directory.empty() || directory.find('\0')!=std::string::npos)
            throw std::invalid_argument("Invalid chart directory");
        player(handle).game->set_chart_storage(directory);
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API jobjectArray JNICALL FN(statusOverlay)(JNIEnv* env,jclass,jlong handle,jint type,jint mode) {
    if ((type!=38 && type!=49) || mode<1 || mode>49) return nullptr;
    try {
        auto& p=player(handle);
        const auto value=p.game->status_overlay(static_cast<nnn::RuntimeMode>(type),static_cast<nnn::RuntimeMode>(mode),p.title);
        if (!value) return nullptr;
        auto out=env->NewObjectArray(static_cast<jsize>(1+2*value->rows.size()),env->FindClass("[B"),nullptr);
        if (!out) return nullptr;
        nnn::snapshot::Writer info;
        info(value->group,value->selection);
        for (const auto& row:value->rows) info(row.id,row.flags);
        array_text(env,out,0,std::string(info.bytes.begin(),info.bytes.end()));
        jsize index=1;
        for (const auto& row:value->rows) {
            if (env->ExceptionCheck()) break;
            array_text(env,out,index++,row.label);
            array_text(env,out,index++,row.detail);
        }
        return out;
    } catch (const std::exception& e) { fail(env,e.what());return nullptr; }
}
API jint JNICALL FN(actModal)(JNIEnv* env,jclass,jlong handle,jint request,jint action,jint value) {
    if (request<=0) return -1;
    try {
        auto& p=player(handle);
        const auto result=p.game->act_modal(request,action,value);
        if (result==5 && p.title) {
            p.title=false;p.back_script.clear();p.static_title={};p.repaint=true;
        }
        return result;
    }
    catch (const std::exception& e) { fail(env,e.what());return -1; }
}
API jboolean JNICALL FN(openModal)(JNIEnv* env,jclass,jlong handle,jint mode,jint caller) {
    if (mode<=0 || mode>49 || caller<=0 || caller>49) return JNI_FALSE;
    try {
        auto& p=player(handle);
        if (!p.title && !p.game->menu_extension_enabled(static_cast<nnn::RuntimeMode>(mode))) return JNI_FALSE;
        return p.game->open_modal(static_cast<nnn::RuntimeMode>(mode),static_cast<nnn::RuntimeMode>(caller),p.title) ? JNI_TRUE : JNI_FALSE;
    } catch (const std::exception& e) { fail(env,e.what());return JNI_FALSE; }
}
API jint JNICALL FN(abortModal)(JNIEnv* env,jclass,jlong handle,jint request,jstring reason) {
    if (request<=0 || !reason) return -1;
    const char* text=env->GetStringUTFChars(reason,nullptr);
    if (!text) return -1;
    try {
        const auto result=player(handle).game->abort_modal(request,text);
        env->ReleaseStringUTFChars(reason,text);
        return result;
    } catch (const std::exception& e) {
        env->ReleaseStringUTFChars(reason,text);fail(env,e.what());return -1;
    }
}
API void JNICALL FN(advanceModal)(JNIEnv* env,jclass,jlong handle,jint request,jint frames) {
    if (request<=0 || frames<=0) return;
    try { player(handle).game->advance_modal(request,static_cast<unsigned>(std::min(frames,64))); }
    catch (const std::exception& e) { fail(env,e.what()); }
}
API jboolean JNICALL FN(modalSavable)(JNIEnv*,jclass,jlong handle) {
    return player(handle).game->modal_savable() ? JNI_TRUE : JNI_FALSE;
}
API jboolean JNICALL FN(completeModal)(JNIEnv* env,jclass,jlong handle,jint request,jboolean accepted,jint elapsed,jobjectArray fields) {
    if (request<=0 || elapsed<0) return JNI_FALSE;
    try {
        const auto values=read_text_fields(env,fields,16,255);
        return player(handle).game->complete_modal(request,accepted,static_cast<unsigned>(elapsed),values) ? JNI_TRUE : JNI_FALSE;
    } catch (const std::exception& e) { fail(env,e.what()); return JNI_FALSE; }
}
API jboolean JNICALL FN(storyExited)(JNIEnv*,jclass,jlong handle) {
    return player(handle).game->state().end_reason==emberfall::scene::EndReason::Exit;
}
API jbyteArray JNICALL FN(snapshot)(JNIEnv* env,jclass,jlong handle) {
    try {
        auto& p=player(handle);
        flush_screen_capture(p);
        const auto data=p.game->snapshot();
        auto out=env->NewByteArray(static_cast<jsize>(data.size()));
        if (out) env->SetByteArrayRegion(out,0,static_cast<jsize>(data.size()),reinterpret_cast<const jbyte*>(data.data()));
        return out;
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_WARN,"SystemNNN","Save journal only: %s",e.what());return nullptr;
    }
}
API void JNICALL FN(restorePcSave)(JNIEnv* env,jclass,jlong handle,jbyteArray raw,jintArray meta) {
    try {
        if (!raw || !meta || env->GetArrayLength(meta)<7) throw std::invalid_argument("Missing PC save data");
        const auto size=env->GetArrayLength(raw);
        if (size<=0 || size>16*1024*1024) throw std::invalid_argument("Invalid PC save length");
        std::vector<nnn::Byte> data(static_cast<std::size_t>(size));
        env->GetByteArrayRegion(raw,0,size,reinterpret_cast<jbyte*>(data.data()));
        if (env->ExceptionCheck()) return;
        auto& p=player(handle);
        p.game->restore_pc_save(data);
        p.published_screen_layer.reset();
        frame_meta(env,p,meta);
        p.repaint=true;
        present(p);
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API jbyteArray JNICALL FN(exportPcSave)(JNIEnv* env,jclass,jlong handle) {
    try {
        auto& p=player(handle);
        flush_screen_capture(p);
        const auto data=p.game->export_pc_save();
        const auto size=static_cast<jsize>(data.size());
        auto result=env->NewByteArray(size);
        if (result) env->SetByteArrayRegion(result,0,size,reinterpret_cast<const jbyte*>(data.data()));
        return result;
    } catch (const std::exception& e) { fail(env,e.what());return nullptr; }
}
API jboolean JNICALL FN(restoreSnapshot)(JNIEnv* env,jclass,jlong handle,jbyteArray raw,jint wait,jint word,jint line,
                                        jobjectArray content,jintArray meta,jboolean keep_music_position,jboolean migrate_legacy) {
    try {
        if (!raw || !content || !meta || env->GetArrayLength(meta)<7) return JNI_FALSE;
        const jsize length=env->GetArrayLength(raw),fields=env->GetArrayLength(content);
        if (length<=0 || static_cast<std::size_t>(length)>nnn::snapshot::max_bytes || fields<2 || fields>1024) return JNI_FALSE;
        nnn::snapshot::Bytes data(static_cast<std::size_t>(length));
        env->GetByteArrayRegion(raw,0,length,reinterpret_cast<jbyte*>(data.data()));
        if (env->ExceptionCheck()) return JNI_FALSE;
        std::vector<std::string> expected;
        jsize remaining=1024*1024;
        for (jsize i=0;i<fields;++i) {
            auto item=static_cast<jbyteArray>(env->GetObjectArrayElement(content,i));
            if (!item) return JNI_FALSE;
            const jsize n=env->GetArrayLength(item);
            if (n>remaining) { env->DeleteLocalRef(item);return JNI_FALSE; }
            remaining-=n;
            std::string value(static_cast<std::size_t>(n),'\0');
            if (n) env->GetByteArrayRegion(item,0,n,reinterpret_cast<jbyte*>(value.data()));
            env->DeleteLocalRef(item);
            if (env->ExceptionCheck()) return JNI_FALSE;
            expected.push_back(std::move(value));
        }
        auto& p=player(handle);
        p.game->restore_snapshot(data,[&](const emberfall::scene::FrameState& f){
            if (static_cast<jint>(f.wait)!=wait || static_cast<jint>(f.word)!=word || static_cast<jint>(f.dialogue.sequence)!=line ||
                f.script!=expected[0] || f.dialogue.display_bytes!=expected[1]) return false;
            if (!f.selection) return expected.size()==2;
            const auto& options=f.selection->options;
            return options.size()==expected.size()-2 && std::equal(options.begin(),options.end(),expected.begin()+2);
        },keep_music_position==JNI_TRUE,migrate_legacy==JNI_TRUE);
        p.published_screen_layer.reset();
        p.repaint=true;
        frame_meta(env,p,meta);
        present(p);
        __android_log_print(ANDROID_LOG_INFO,"SystemNNN","Native snapshot restored line=%d word=%d",static_cast<int>(line),static_cast<int>(word));
        return JNI_TRUE;
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_WARN,"SystemNNN","Native snapshot fallback: %s",e.what());return JNI_FALSE;
    }
}
API jobjectArray JNICALL FN(dialogue)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto& line=player(handle).game->state().dialogue;
        auto result=env->NewObjectArray(2,env->FindClass("[B"),nullptr);
        if (!result) return nullptr;
        array_text(env,result,0,line.speaker);
        if (!env->ExceptionCheck()) array_text(env,result,1,dialogue_body(line.display_bytes,line.speaker));
        return result;
    } catch (const std::exception& e) { fail(env,e.what()); return nullptr; }
}
API jobjectArray JNICALL FN(backlog)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto& history=player(handle).game->backlog();
        auto out=env->NewObjectArray(static_cast<jsize>(history.size()*7),env->FindClass("[B"),nullptr);
        if (!out) return nullptr;
        jsize index=0;
        for (const auto& line:history) {
            array_text(env,out,index++,line.speaker);
            if (env->ExceptionCheck()) return nullptr;
            array_text(env,out,index++,dialogue_body(line.text,line.speaker));
            for (const auto& voice:line.voices) {
                if (env->ExceptionCheck()) return nullptr;
                array_text(env,out,index++,voice);
            }
            if (env->ExceptionCheck()) return nullptr;
            std::string style;
            if (line.color>=0) {
                style.push_back(static_cast<char>(line.color>>16));
                style.push_back(static_cast<char>(line.color>>8));
                style.push_back(static_cast<char>(line.color));
                if (line.configuration_text) style.push_back(1);
            }
            array_text(env,out,index++,style);
        }
        return out;
    } catch (const std::exception& e) { fail(env,e.what()); return nullptr; }
}
API jlongArray JNICALL FN(backlogJumpIds)(JNIEnv* env,jclass,jlong handle) {
    const auto& history=player(handle).game->backlog();
    std::vector<jlong> ids;
    ids.reserve(history.size());
    for (const auto& line:history) ids.push_back(static_cast<jlong>(line.jump_id));
    const auto count=static_cast<jsize>(ids.size());
    auto out=env->NewLongArray(count);
    if (out && count) env->SetLongArrayRegion(out,0,count,ids.data());
    return out;
}
API void JNICALL FN(captureBacklogJump)(JNIEnv*,jclass,jlong handle,jint cursor) {
    try { player(handle).game->capture_backlog_jump(static_cast<unsigned>(std::max(0,cursor))); }
    catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_WARN,"SystemNNN","Backlog checkpoint skipped: %s",e.what());
    }
}
API jintArray JNICALL FN(backlogJump)(JNIEnv* env,jclass,jlong handle,jlong id) {
    const auto plan=player(handle).game->backlog_jump(static_cast<std::uint64_t>(id));
    if (!plan.journal_cursor) return nullptr;
    const jint fields[]={static_cast<jint>(plan.journal_cursor),static_cast<jint>(plan.keep_backlog),
        static_cast<jint>(plan.random_state),plan.mouse_x,plan.mouse_y};
    auto out=env->NewIntArray(5);
    if (out) env->SetIntArrayRegion(out,0,5,fields);
    return out;
}
API jboolean JNICALL FN(restoreBacklogJump)(JNIEnv* env,jclass,jlong handle,jlong id,jint cursor,jintArray meta) {
    if (!meta || env->GetArrayLength(meta)<7 || cursor<=0) return JNI_FALSE;
    auto& p=player(handle);
    try {
        if (!p.game->restore_backlog_jump(static_cast<std::uint64_t>(id),static_cast<unsigned>(cursor))) return JNI_FALSE;
        p.published_screen_layer.reset();
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_WARN,"SystemNNN","Backlog jump rejected: %s",e.what());
        return JNI_FALSE;
    }
    p.repaint=true;
    frame_meta(env,p,meta);
    present(p);
    return JNI_TRUE;
}
API void JNICALL FN(trimReplayedBacklog)(JNIEnv* env,jclass,jlong handle,jint keep) {
    try { player(handle).game->trim_replayed_backlog(static_cast<unsigned>(keep)); }
    catch (const std::exception& e) { fail(env,e.what()); }
}
API jintArray JNICALL FN(systemVariables)(JNIEnv* env,jclass,jlong handle) {
    const auto values=player(handle).game->runtime()->system_variables();
    auto out=env->NewIntArray(static_cast<jsize>(values.size()));
    if (out) env->SetIntArrayRegion(out,0,static_cast<jsize>(values.size()),values.data());
    return out;
}
API void JNICALL FN(seedLegacyReplayVariables)(JNIEnv*,jclass,jlong handle) {
    player(handle).game->seed_legacy_replay_variables();
}
API void JNICALL FN(setSystemVariables)(JNIEnv* env,jclass,jlong handle,jintArray input) {
    nnn::SystemVariables values{};
    if (!input || env->GetArrayLength(input)!=static_cast<jsize>(values.size())) {
        fail(env,"Invalid system variable count");return;
    }
    env->GetIntArrayRegion(input,0,static_cast<jsize>(values.size()),values.data());
    if (!env->ExceptionCheck()) player(handle).game->runtime()->set_system_variables(values);
}
API jint JNICALL FN(scriptRandomState)(JNIEnv*,jclass,jlong handle) {
    return static_cast<jint>(player(handle).game->script_random_state());
}
API void JNICALL FN(setScriptRandomState)(JNIEnv*,jclass,jlong handle,jint state) {
    player(handle).game->set_script_random_state(static_cast<std::uint32_t>(state));
}
API jintArray JNICALL FN(scriptProgress)(JNIEnv* env,jclass,jlong handle) {
    const auto values=player(handle).game->runtime()->script_progress();
    auto out=env->NewIntArray(static_cast<jsize>(values.size()));
    if (out) env->SetIntArrayRegion(out,0,static_cast<jsize>(values.size()),values.data());
    return out;
}
API void JNICALL FN(setScriptProgress)(JNIEnv* env,jclass,jlong handle,jintArray input) {
    nnn::ScriptProgress values{};
    if (!input || env->GetArrayLength(input)!=static_cast<jsize>(values.size())) {
        fail(env,"Invalid script progress count");return;
    }
    env->GetIntArrayRegion(input,0,static_cast<jsize>(values.size()),values.data());
    if (!env->ExceptionCheck()) player(handle).game->runtime()->set_script_progress(values);
}
API jobjectArray JNICALL FN(runtimeText)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto text=player(handle).game->runtime()->runtime_text();
        auto out=env->NewObjectArray(static_cast<jsize>(text.system_replacements.size()+2),env->FindClass("[B"),nullptr);
        if (!out) return nullptr;
        array_text(env,out,0,text.surname);
        if (!env->ExceptionCheck()) array_text(env,out,1,text.given_name);
        for (std::size_t i=0;i<text.system_replacements.size() && !env->ExceptionCheck();++i)
            array_text(env,out,static_cast<jsize>(i+2),text.system_replacements[i]);
        return out;
    } catch (const std::exception& e) { fail(env,e.what()); return nullptr; }
}
API jboolean JNICALL FN(restoreRuntimeText)(JNIEnv* env,jclass,jlong handle,jobjectArray fields) {
    try {
        nnn::RuntimeTextState text;
        auto values=read_text_fields(env,fields,static_cast<jsize>(text.system_replacements.size()+2),32);
        if (values.size()!=text.system_replacements.size()+2) return JNI_FALSE;
        text.surname=std::move(values[0]);
        text.given_name=std::move(values[1]);
        for (std::size_t i=0;i<text.system_replacements.size();++i)
            text.system_replacements[i]=std::move(values[i+2]);
        auto& game=*player(handle).game;
        if (!game.runtime()->restore_runtime_text(std::move(text),true)) return JNI_FALSE;
        game.sync_runtime_names();
        return JNI_TRUE;
    } catch (const std::exception& e) { fail(env,e.what()); return JNI_FALSE; }
}
API jintArray JNICALL FN(optionalProgress)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto state=player(handle).game->runtime()->optional_progress_view();
        const auto role_offset=state->terms.size()+state->voice_words.size();
        std::vector<jint> words(role_offset+state->voice_role_words.size());
        std::memcpy(words.data(),state->terms.data(),sizeof(state->terms));
        std::memcpy(words.data()+state->terms.size(),state->voice_words.data(),sizeof(state->voice_words));
        std::memcpy(words.data()+role_offset,state->voice_role_words.data(),sizeof(state->voice_role_words));
        return int_array(env,words);
    } catch (const std::exception& e) { fail(env,e.what()); return nullptr; }
}
API void JNICALL FN(restoreOptionalProgress)(JNIEnv* env,jclass,jlong handle,jintArray words) {
    try {
        nnn::RuntimeOptionalProgress state;
        const auto role_offset=state.terms.size()+state.voice_words.size();
        const auto legacy_count=static_cast<jsize>(role_offset);
        const auto count=words ? env->GetArrayLength(words) : 0;
        if (count!=legacy_count && count!=static_cast<jsize>(role_offset+state.voice_role_words.size()))
            throw std::invalid_argument("Invalid optional progress count");
        std::vector<jint> raw(static_cast<std::size_t>(count));
        env->GetIntArrayRegion(words,0,count,raw.data());
        if (env->ExceptionCheck()) return;
        std::memcpy(state.terms.data(),raw.data(),sizeof(state.terms));
        std::memcpy(state.voice_words.data(),raw.data()+state.terms.size(),sizeof(state.voice_words));
        const auto runtime=player(handle).game->runtime();
        state.voice_role_words=runtime->optional_progress_view()->voice_role_words;
        if (count!=legacy_count)
            for (std::size_t i=0;i<state.voice_role_words.size();++i)
                state.voice_role_words[i]|=static_cast<nnn::Word>(raw[role_offset+i]);
        runtime->restore_optional_progress(state);
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API jboolean JNICALL FN(voiceRoleHeard)(JNIEnv* env,jclass,jlong handle,jint character) {
    try { return player(handle).game->runtime()->voice_role_heard(character) ? JNI_TRUE : JNI_FALSE; }
    catch (const std::exception& e) { fail(env,e.what()); return JNI_FALSE; }
}
API jbyteArray JNICALL FN(chartProgress)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto flags=player(handle).game->runtime()->chart_progress();
        if (!flags || flags->traversed.empty()) return env->NewByteArray(0);
        nnn::snapshot::Writer out;
        out(static_cast<std::uint32_t>(flags->seen.size()),static_cast<std::uint32_t>(flags->traversed.size()));
        out.raw(flags->seen.data(),flags->seen.size());out.raw(flags->traversed.data(),flags->traversed.size());
        const auto count=static_cast<jsize>(out.bytes.size());
        auto result=env->NewByteArray(count);
        if (result) env->SetByteArrayRegion(result,0,count,reinterpret_cast<const jbyte*>(out.bytes.data()));
        return result;
    } catch (const std::exception& e) { fail(env,e.what());return nullptr; }
}
API void JNICALL FN(restoreChartProgress)(JNIEnv* env,jclass,jlong handle,jbyteArray raw) {
    try {
        if (!raw) throw std::invalid_argument("Missing chart progress");
        const auto size=env->GetArrayLength(raw);
        if (size<0 || static_cast<std::size_t>(size)>2U*nnn::chart::max_progress_slots+8U)
            throw std::invalid_argument("Invalid chart progress length");
        if (size==0) return;
        nnn::snapshot::Bytes bytes(static_cast<std::size_t>(size));
        env->GetByteArrayRegion(raw,0,size,reinterpret_cast<jbyte*>(bytes.data()));
        if (env->ExceptionCheck()) return;
        nnn::snapshot::Reader in(bytes);
        const auto seen=in.size(nnn::chart::max_progress_slots),traversed=in.size(nnn::chart::max_progress_slots);
        if (seen+traversed!=in.remaining()) throw std::invalid_argument("Invalid chart progress blocks");
        nnn::chart::Progress flags;
        const auto* seen_data=in.raw(seen);
        const auto* traversed_data=in.raw(traversed);
        flags.seen.assign(seen_data,seen_data+seen);
        flags.traversed.assign(traversed_data,traversed_data+traversed);
        if (!player(handle).game->runtime()->set_chart_progress(std::move(flags)))
            throw std::invalid_argument("Chart progress was rejected");
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API jint JNICALL FN(configMask)(JNIEnv*,jclass,jlong handle) {
    return player(handle).game->state().config_mask;
}
API jlong JNICALL FN(takeScriptUiEvents)(JNIEnv*,jclass,jlong handle) {
    auto& game=*player(handle).game;
    return (static_cast<jlong>(game.take_system_save_request())<<32U)|game.take_notice();
}
API jintArray JNICALL FN(placeStates)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto values=player(handle).game->places();
        const auto count=static_cast<jsize>(values.size());
        auto out=env->NewIntArray(count);
        if (out && count) env->SetIntArrayRegion(out,0,count,values.data());
        return out;
    } catch (const std::exception& e) { fail(env,e.what());return nullptr; }
}
API jint JNICALL FN(placeMode)(JNIEnv*,jclass,jlong handle) {
    return static_cast<jint>(player(handle).game->place_mode());
}
API jintArray JNICALL FN(placeCharacters)(JNIEnv* env,jclass,jlong handle) {
    return int_array(env,player(handle).game->place_characters());
}
API void JNICALL FN(replayBacklogVoice)(JNIEnv* env,jclass,jlong handle,jbyteArray name) {
    try {
        const auto voice=resource_name(env,name);
        const auto error=player(handle).game->audio().replay_backlog_voice(voice);
        if (!error.empty()) throw std::runtime_error(error);
        __android_log_print(ANDROID_LOG_INFO,"SystemNNN","Backlog voice replayed: %s",voice.c_str());
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API void JNICALL FN(suspendScriptAudio)(JNIEnv* env,jclass,jlong handle) {
    try { player(handle).game->audio().suspend_script_audio(); }
    catch (const std::exception& e) { fail(env,e.what()); }
}
API void JNICALL FN(restartLoopScriptAudio)(JNIEnv* env,jclass,jlong handle) {
    try { player(handle).game->audio().restart_loop_script_audio(); }
    catch (const std::exception& e) { fail(env,e.what()); }
}
API void JNICALL FN(stopScriptAudio)(JNIEnv* env,jclass,jlong handle) {
    try { player(handle).game->audio().stop_script_audio(); }
    catch (const std::exception& e) { fail(env,e.what()); }
}
API jintArray JNICALL FN(saveThumbnail)(JNIEnv* env,jclass,jlong handle,jint width,jint height) {
    try {
        auto& p=player(handle);
        const auto& source=p.title && !p.static_title.rgb.empty()?p.static_title:p.game->surface();
        p.repaint=true;
        return emberfall::android::frame_pixels(env,source,width,height);
    } catch (const std::exception& e) { fail(env,e.what()); return nullptr; }
}
API jint JNICALL FN(saveSlotArt)(JNIEnv* env,jclass,jlong handle) {
    try { return player(handle).game->save_slot_art(); }
    catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API jlong JNICALL FN(takeScreenLayer)(JNIEnv* env,jclass,jlong handle) {
    try {
        auto& p=player(handle);
        if (!p.window || !draws_story_ui(p) || !p.game->screen_layer_active()) {
            const bool clear=static_cast<bool>(p.screen_layer);
            p.screen_layer.reset();
            p.published_screen_layer.reset();
            return clear ? -1 : 0;
        }
        p.game->capture_screen_layer(true);
        auto frame=p.game->take_screen_layer();
        const auto revision=p.game->screen_layer_revision();
        if (!frame && p.screen_layer && (!p.published_screen_layer || p.screen_layer_revision!=revision)) {
            p.screen_layer.reset();
            p.published_screen_layer.reset();
            return -1;
        }
        const auto stamp=story_stamp(p);
        if (!frame && (!p.screen_layer || p.screen_layer_state==stamp)) return 0;
        if (frame) p.screen_layer=std::move(frame);
        p.screen_layer_revision=revision;
        p.screen_layer_state=stamp;
        p.published_screen_layer=std::make_shared<StoryScreenLayer>(p.screen_layer,stamp);
        auto result=std::make_unique<PublishedScreenLayer>(p.published_screen_layer);
        return reinterpret_cast<jlong>(result.release());
    } catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API jboolean JNICALL FN(compositeScreenLayer)(JNIEnv* env,jclass,jlong handle,jobject bitmap,jint wait,jint word,jint line,
                                             jboolean show_backdrop,jboolean record) {
    try {
        if (!handle) return false;
        const auto& screen=*reinterpret_cast<const PublishedScreenLayer*>(handle);
        if (screen->state!=std::array<jint,3>{wait,word,line}) return false;
        emberfall::android::LockedBitmap image(env,bitmap);
        if (image.info.width!=screen->frame->width() || image.info.height!=screen->frame->height()) return false;
        screen->frame->composite(image.row(0),image.info.stride,show_backdrop);
        if (record && screen->frame->captures_screen()) {
            auto sample=screen->frame->composed();
            {
                std::lock_guard lock(screen->mutex);
                screen->sample=std::move(sample);
            }
            screen->ready.notify_one();
        }
        return true;
    } catch (const std::exception& e) { fail(env,e.what()); return false; }
}
API void JNICALL FN(releaseScreenLayer)(JNIEnv*,jclass,jlong handle) {
    delete reinterpret_cast<PublishedScreenLayer*>(handle);
}
API void JNICALL FN(setVolumes)(JNIEnv* env,jclass,jlong handle,jint music,jint sound,jint voice,jint effect_voice) {
    try {
        auto& game=*player(handle).game;
        game.update_system_parameters({{nnn::NNNPARAM_MUSICVOLUME,music},{nnn::NNNPARAM_SOUNDVOLUME,sound},
            {nnn::NNNPARAM_SCRIPTSEVOLUME,sound},{nnn::NNNPARAM_VOICEVOLUME,voice},{nnn::NNNPARAM_SOUNDVOICEVOLUME,effect_voice}});
        auto& audio=game.audio();
        const auto state=audio.state();
        __android_log_print(ANDROID_LOG_INFO,"SystemNNN",
            "volumes: input music=%d sound=%d voice=%d effectVoice=%d; bgm=%d/%d voices=%d/%d,%d/%d,%d/%d,%d/%d (volume/active)",
            static_cast<int>(music),static_cast<int>(sound),static_cast<int>(voice),static_cast<int>(effect_voice),
            state.bgm.volume,static_cast<int>(state.bgm.active),
            state.voices[0].volume,static_cast<int>(state.voices[0].active),
            state.voices[1].volume,static_cast<int>(state.voices[1].active),
            state.voices[2].volume,static_cast<int>(state.voices[2].active),
            state.voices[3].volume,static_cast<int>(state.voices[3].active));
    }
    catch (const std::exception& e) { fail(env,e.what()); }
}
API jintArray JNICALL FN(dialogueProperties)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto& game=*player(handle).game;
        const auto& line=game.state().dialogue;
        const auto id=line.message_id;
        const std::array<jint,8> values{{id<=static_cast<nnn::Word>(INT32_MAX) ? static_cast<jint>(id) : -1-static_cast<jint>(~id),
            line.window_type,line.font_type,line.window_visible ? 1 : 0,game.options_enabled() ? 1 : 0,
            static_cast<jint>(line.command),static_cast<jint>(line.layout_command),static_cast<jint>(game.draw_message())}};
        auto result=env->NewIntArray(static_cast<jsize>(values.size()));
        if (result) env->SetIntArrayRegion(result,0,static_cast<jsize>(values.size()),values.data());
        return result;
    } catch (const std::exception& e) { fail(env,e.what()); return nullptr; }
}
API void JNICALL FN(setParameters)(JNIEnv* env,jclass,jlong handle,jintArray pairs) {
    try {
        if (!pairs) throw std::invalid_argument("Missing runtime parameters");
        const auto count=env->GetArrayLength(pairs);
        if (count%2!=0 || count>1144) throw std::invalid_argument("Invalid runtime parameter batch");
        std::vector<jint> input(static_cast<std::size_t>(count));
        if (count) env->GetIntArrayRegion(pairs,0,count,input.data());
        if (env->ExceptionCheck()) return;
        std::vector<std::pair<nnn::Word,nnn::Value>> values;
        values.reserve(input.size()/2);
        for (std::size_t i=0;i<input.size();i+=2) {
            if (input[i]<0) throw std::invalid_argument("Invalid runtime parameter identity");
            values.emplace_back(static_cast<nnn::Word>(input[i]),input[i+1]);
        }
        if (!player(handle).game->update_system_parameters(values)) throw std::invalid_argument("Unsupported runtime parameter identity");
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API jintArray JNICALL FN(parameters)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto state=player(handle).game->snapshot_parameters();
        std::vector<jint> values;
        for (nnn::Word id=0;id<nnn::NNNPARAM_CHARAVOICEVOLUME+nnn::runtime_character_volume_count;++id) {
            nnn::Value value=0;
            if (state.get(id,value)) { values.push_back(static_cast<jint>(id)); values.push_back(value); }
        }
        auto result=env->NewIntArray(static_cast<jsize>(values.size()));
        if (result) env->SetIntArrayRegion(result,0,static_cast<jsize>(values.size()),values.data());
        return result;
    } catch (const std::exception& e) { fail(env,e.what()); return nullptr; }
}
API jintArray JNICALL FN(displayParameters)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto& game=*player(handle).game;
        std::vector<jint> values;
        for (nnn::Word id=0;id<nnn::NNNPARAM_CHARAVOICEVOLUME+nnn::runtime_character_volume_count;++id)
            if (nnn::RuntimeParameters::contains(id)) { values.push_back(static_cast<jint>(id)); values.push_back(game.get_system_parameter(id)); }
        auto result=env->NewIntArray(static_cast<jsize>(values.size()));
        if (result) env->SetIntArrayRegion(result,0,static_cast<jsize>(values.size()),values.data());
        return result;
    } catch (const std::exception& e) { fail(env,e.what()); return nullptr; }
}
API jintArray JNICALL FN(auxiliaryState)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto context=player(handle).game->runtime();
        std::array<jint,36> values{};
        values[0]=context->system_voice_selection(); values[1]=context->message_window();
        const auto flags=context->voice_off();
        for (std::size_t i=0;i<flags.size();++i)
            values[i+2]=flags[i]<=static_cast<nnn::Word>(INT32_MAX) ? static_cast<jint>(flags[i]) : -1-static_cast<jint>(~flags[i]);
        const auto cursors=context->cursor_variants();
        values[34]=cursors[0];values[35]=cursors[1];
        auto result=env->NewIntArray(static_cast<jsize>(values.size()));
        if (result) env->SetIntArrayRegion(result,0,static_cast<jsize>(values.size()),values.data());
        return result;
    } catch (const std::exception& e) { fail(env,e.what()); return nullptr; }
}
API jboolean JNICALL FN(legacyVolumeDefaults)(JNIEnv* env,jclass,jlong handle) {
    try { return player(handle).game->runtime()->legacy_volume_defaults() ? JNI_TRUE : JNI_FALSE; }
    catch (const std::exception& e) { fail(env,e.what()); return JNI_FALSE; }
}
API void JNICALL FN(setAuxiliaryState)(JNIEnv* env,jclass,jlong handle,jintArray pairs) {
    try {
        if (!pairs) throw std::invalid_argument("Missing auxiliary state");
        const auto count=env->GetArrayLength(pairs);
        if (count%2!=0 || count>68) throw std::invalid_argument("Invalid auxiliary state batch");
        std::vector<jint> input(static_cast<std::size_t>(count));
        if (count) env->GetIntArrayRegion(pairs,0,count,input.data());
        if (env->ExceptionCheck()) return;
        auto& game=*player(handle).game; const auto context=game.runtime();
        auto theme=context->system_voice_selection(),window=context->message_window(); auto flags=context->voice_off();
        bool theme_changed=false,window_changed=false,flags_changed=false;
        for (std::size_t i=0;i<input.size();i+=2) {
            const auto index=input[i],value=input[i+1];
            if (index<0 || index>=34) throw std::invalid_argument("Invalid auxiliary state index");
            if (index==0) {
                if (value<0 || static_cast<unsigned>(value)>=game.audio().system_voice_selection_count())
                    throw std::invalid_argument("System voice theme unavailable");
                theme=value; theme_changed=true;
            } else if (index==1) { window=value; window_changed=true; }
            else { flags[static_cast<std::size_t>(index-2)]=static_cast<nnn::Word>(value); flags_changed=true; }
        }
        // No owner is changed until every supplied DTO index/value is checked.
        if (theme_changed) context->set_system_voice_selection(theme);
        if (window_changed) context->set_message_window(window);
        if (flags_changed) context->set_voice_off(flags);
        game.apply_runtime_parameters();
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API jbyteArray JNICALL FN(appendPrefix)(JNIEnv* env,jclass,jlong handle) {
    return bytes(env,player(handle).game->append_prefix());
}
API void JNICALL FN(setTextLength)(JNIEnv* env,jclass,jlong handle,jint length,jint visible) {
    try {
        if (length<0) throw std::invalid_argument("Invalid logical text length");
        const auto prefix=visible<0 ? std::nullopt : std::optional{static_cast<std::uint32_t>(visible)};
        player(handle).game->set_text_length(static_cast<std::uint32_t>(length),prefix);
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API jintArray JNICALL FN(textState)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto& game=*player(handle).game;
        const std::array<jint,3> values{{static_cast<jint>(std::min<std::uint32_t>(game.visible_text(),INT32_MAX)),
            static_cast<jint>(std::min<std::uint32_t>(game.text_length(),INT32_MAX)),game.text_complete() ? 1 : 0}};
        auto result=env->NewIntArray(3);
        if (result) env->SetIntArrayRegion(result,0,3,values.data());
        return result;
    } catch (const std::exception& e) { fail(env,e.what()); return nullptr; }
}
API jintArray JNICALL FN(settingsTextRates)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto& game=*player(handle).game;
        std::array<jint,10> rates{};
        for (int i=0;i<5;++i) {
            rates[i]=static_cast<jint>(game.settings_text_rate(i,false));
            rates[i+5]=static_cast<jint>(game.settings_text_rate(i,true));
        }
        auto result=env->NewIntArray(static_cast<jsize>(rates.size()));
        if (result) env->SetIntArrayRegion(result,0,static_cast<jsize>(rates.size()),rates.data());
        return result;
    } catch (const std::exception& e) { fail(env,e.what()); return nullptr; }
}
API jboolean JNICALL FN(requestAdvance)(JNIEnv* env,jclass,jlong handle) {
    try { return player(handle).game->request_advance() ? JNI_TRUE : JNI_FALSE; }
    catch (const std::exception& e) { fail(env,e.what()); return JNI_FALSE; }
}
API jint JNICALL FN(textCharacters)(JNIEnv* env,jclass,jlong handle,jint elapsed) {
    try {
        if (elapsed<0) throw std::invalid_argument("Invalid text elapsed time");
        const auto count=player(handle).game->text_characters(static_cast<std::uint32_t>(elapsed));
        return static_cast<jint>(std::min<std::uint32_t>(count,INT32_MAX));
    } catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API jboolean JNICALL FN(autoReady)(JNIEnv* env,jclass,jlong handle,jint elapsed,jboolean complete) {
    try {
        if (elapsed<0) throw std::invalid_argument("Invalid Auto elapsed time");
        return player(handle).game->auto_ready(static_cast<std::uint32_t>(elapsed),complete==JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
    } catch (const std::exception& e) { fail(env,e.what()); return JNI_FALSE; }
}
API void JNICALL FN(playSystemSe)(JNIEnv* env,jclass,jlong handle,jint id) {
    try {
        if (id<0) throw std::invalid_argument("Invalid system SE number");
        const auto error=player(handle).game->audio().play_system_se(static_cast<unsigned>(id));
        if (error.empty()) __android_log_print(ANDROID_LOG_INFO,"SystemNNN","UISE id=%d",static_cast<int>(id));
        if (!error.empty()) __android_log_print(ANDROID_LOG_WARN,"SystemNNN","system SE: %s",error.c_str());
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API void JNICALL FN(playButtonVoice)(JNIEnv* env,jclass,jlong handle,jint id) {
    try {
        if (id<0) throw std::invalid_argument("Invalid button voice number");
        const auto error=player(handle).game->audio().play_button_voice(static_cast<unsigned>(id));
        if (!error.empty()) __android_log_print(ANDROID_LOG_WARN,"SystemNNN","button voice: %s",error.c_str());
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API void JNICALL FN(playSettingsClick)(JNIEnv* env,jclass,jlong handle,jint se,jint voice,jint volume_type,
                                      jboolean ignore_voice_switch,jboolean ignore_character_mute) {
    try {
        auto& audio=player(handle).game->audio();
        if (se>=0) {
            const auto error=audio.play_system_se(static_cast<unsigned>(se),0,volume_type!=0);
            if (!error.empty()) __android_log_print(ANDROID_LOG_WARN,"SystemNNN","Settings SE: %s",error.c_str());
        }
        if (voice>=0) {
            const auto error=audio.play_button_voice(static_cast<unsigned>(voice),ignore_voice_switch==JNI_TRUE,
                ignore_character_mute==JNI_TRUE);
            if (!error.empty()) __android_log_print(ANDROID_LOG_WARN,"SystemNNN","Settings voice: %s",error.c_str());
        }
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API void JNICALL FN(playConfigFeedback)(JNIEnv* env,jclass,jlong handle,jint slider,jint position,jint steps) {
    try {
        const auto error=player(handle).game->audio().play_config_feedback(static_cast<unsigned>(slider),position,steps);
        if (!error.empty()) __android_log_print(ANDROID_LOG_WARN,"SystemNNN","Settings slider: %s",error.c_str());
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API void JNICALL FN(setSkip)(JNIEnv* env,jclass,jlong handle,jboolean skip) {
    try {
        auto& game=*player(handle).game; const bool enabled=skip==JNI_TRUE;
        if (game.skip_active()!=enabled) __android_log_print(ANDROID_LOG_INFO,"SystemNNN","skip: %s",enabled ? "on" : "off");
        game.set_skip_active(enabled);
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API jboolean JNICALL FN(skipActive)(JNIEnv* env,jclass,jlong handle) {
    try { return player(handle).game->skip_active() ? JNI_TRUE : JNI_FALSE; }
    catch (const std::exception& e) { fail(env,e.what()); return JNI_FALSE; }
}
API jboolean JNICALL FN(skipEffective)(JNIEnv* env,jclass,jlong handle) {
    try { return player(handle).game->skip_effective() ? JNI_TRUE : JNI_FALSE; }
    catch (const std::exception& e) { fail(env,e.what()); return JNI_FALSE; }
}
API jintArray JNICALL FN(readState)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto ids=player(handle).game->runtime()->snapshot_read();
        std::vector<jint> values; values.reserve(ids.size());
        for (const auto id:ids) values.push_back(id<=static_cast<nnn::Word>(INT32_MAX) ? static_cast<jint>(id) : -1-static_cast<jint>(~id));
        auto result=env->NewIntArray(static_cast<jsize>(values.size()));
        if (result && !values.empty()) env->SetIntArrayRegion(result,0,static_cast<jsize>(values.size()),values.data());
        return result;
    } catch (const std::exception& e) { fail(env,e.what()); return nullptr; }
}
API void JNICALL FN(restoreReadState)(JNIEnv* env,jclass,jlong handle,jintArray raw) {
    try {
        if (!raw) throw std::invalid_argument("Missing read state");
        const auto count=env->GetArrayLength(raw);
        if (count>100000) throw std::invalid_argument("Read state exceeds limit");
        std::vector<jint> values(static_cast<std::size_t>(count));
        if (count) env->GetIntArrayRegion(raw,0,count,values.data());
        if (env->ExceptionCheck()) return;
        std::vector<nnn::Word> ids; ids.reserve(values.size());
        for (const auto value:values) ids.push_back(static_cast<nnn::Word>(value));
        if (!player(handle).game->runtime()->restore_read(ids)) throw std::invalid_argument("Invalid read state");
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API jboolean JNICALL FN(markRead)(JNIEnv* env,jclass,jlong handle,jint id) {
    try { return player(handle).game->runtime()->mark_read(static_cast<nnn::Word>(id)) ? JNI_TRUE : JNI_FALSE; }
    catch (const std::exception& e) { fail(env,e.what()); return JNI_FALSE; }
}
API jboolean JNICALL FN(isRead)(JNIEnv* env,jclass,jlong handle,jint id) {
    try { return player(handle).game->runtime()->is_read(static_cast<nnn::Word>(id)) ? JNI_TRUE : JNI_FALSE; }
    catch (const std::exception& e) { fail(env,e.what()); return JNI_FALSE; }
}
API jlong JNICALL FN(galleryRevision)(JNIEnv* env,jclass,jlong handle) {
    try { return static_cast<jlong>(player(handle).game->runtime()->gallery_revision()); }
    catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API jint JNICALL FN(movieNumber)(JNIEnv*,jclass,jlong handle) {
    return static_cast<jint>(player(handle).game->state().movie);
}
API jint JNICALL FN(movieVolume)(JNIEnv* env,jclass,jlong handle) {
    try {
        auto& game=*player(handle).game;
        return game.get_system_parameter(nnn::NNNPARAM_MOVIESWITCH)
            ? game.get_system_parameter(nnn::NNNPARAM_MOVIEVOLUME) : 0;
    } catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API jintArray JNICALL FN(choiceInfo)(JNIEnv* env,jclass,jlong handle) {
    const auto& game=*player(handle).game;
    const auto& choice=game.state().selection;
    if (!choice) return nullptr;
    const jint values[]={static_cast<jint>(game.state().choice_limit_ms),choice->auto_select,choice->special,
        static_cast<jint>(game.runtime()->selected_choices(choice->serial)),static_cast<jint>(game.state().choice_remaining_ms)};
    auto out=env->NewIntArray(5);
    if (out) env->SetIntArrayRegion(out,0,5,values);
    return out;
}
API jboolean JNICALL FN(choiceTimedOut)(JNIEnv*,jclass,jlong handle) {
    return player(handle).game->state().choice_timed_out?JNI_TRUE:JNI_FALSE;
}
API jobjectArray JNICALL FN(choiceMessages)(JNIEnv* env,jclass,jlong handle) {
    const auto& choice=player(handle).game->state().selection;
    const auto count=choice?choice->messages.size():0;
    auto out=env->NewObjectArray(static_cast<jsize>(count),env->FindClass("[B"),nullptr);
    if (!out) return nullptr;
    for (std::size_t i=0;i<count && !env->ExceptionCheck();++i)
        array_text(env,out,static_cast<jsize>(i),choice->messages[i]);
    return out;
}
API jintArray JNICALL FN(galleryState)(JNIEnv* env,jclass,jlong handle) {
    try {
        const auto ids=player(handle).game->runtime()->snapshot_gallery();
        const std::vector<jint> values(ids.begin(),ids.end());
        auto out=env->NewIntArray(static_cast<jsize>(values.size()));
        if (out && !values.empty()) env->SetIntArrayRegion(out,0,static_cast<jsize>(values.size()),values.data());
        return out;
    } catch (const std::exception& e) { fail(env,e.what()); return nullptr; }
}
static void update_gallery(JNIEnv* env,jlong handle,jintArray keys,bool replace) {
    try {
        if (!keys) throw std::invalid_argument("Missing gallery progress");
        const auto count=env->GetArrayLength(keys);
        if (static_cast<std::size_t>(count)>nnn::RuntimeContext::max_gallery_unlocks)
            throw std::invalid_argument("Gallery progress exceeds limit");
        std::vector<jint> input(static_cast<std::size_t>(count));
        if (count) env->GetIntArrayRegion(keys,0,count,input.data());
        if (env->ExceptionCheck()) return;
        const std::vector<nnn::Word> values(input.begin(),input.end());
        const auto context=player(handle).game->runtime();
        if (!(replace ? context->replace_gallery(values) : context->merge_gallery(values)))
            throw std::invalid_argument("Invalid gallery progress");
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API void JNICALL FN(mergeGallery)(JNIEnv* env,jclass,jlong handle,jintArray keys) { update_gallery(env,handle,keys,false); }
API void JNICALL FN(replaceGallery)(JNIEnv* env,jclass,jlong handle,jintArray keys) { update_gallery(env,handle,keys,true); }
API jint JNICALL FN(musicNumber)(JNIEnv* env,jclass,jlong handle,jbyteArray name) {
    try { return player(handle).game->audio().music_number(resource_name(env,name)); }
    catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API jint JNICALL FN(galleryVariable)(JNIEnv* env,jclass,jlong handle,jbyteArray name) {
    try { return player(handle).game->named_variable(resource_name(env,name)); }
    catch (const std::exception& e) { fail(env,e.what()); return 0; }
}
API jint JNICALL FN(menuVariable)(JNIEnv* env,jclass,jlong handle,jbyteArray name) {
    try {auto& p=player(handle);return p.game->named_variable(resource_name(env,name),p.title);}
    catch (const std::exception& e) {fail(env,e.what());return 0;}
}
API jint JNICALL FN(openingVariable)(JNIEnv* env,jclass,jlong handle,jbyteArray name) {
    try {return player(handle).game->named_variable(resource_name(env,name),false,-1);}
    catch (const std::exception& e) {fail(env,e.what());return -1;}
}
API void JNICALL FN(menuMusic)(JNIEnv* env,jclass,jlong handle,jint number,jint plays,jint volume,jint fade,jboolean restart) {
    try {
        const auto error=player(handle).game->audio().play_menu_music(number,plays,volume,fade,restart==JNI_TRUE);
        if (!error.empty()) throw std::runtime_error(error);
    } catch (const std::exception& e) { fail(env,e.what()); }
}
API void JNICALL FN(playGalleryMusic)(JNIEnv* env,jclass,jlong handle,jbyteArray name,jint plays,jint volume) {
    try { player(handle).game->audio().play_music_preview(resource_name(env,name),plays,volume); }
    catch (const std::exception& e) { fail(env,e.what()); }
}
API void JNICALL FN(stopGalleryMusic)(JNIEnv* env,jclass,jlong handle) {
    try { player(handle).game->audio().stop_music_preview(); }
    catch (const std::exception& e) { fail(env,e.what()); }
}
API void JNICALL FN(pauseGalleryMusic)(JNIEnv* env,jclass,jlong handle,jboolean paused) {
    try { player(handle).game->audio().pause_music_preview(paused==JNI_TRUE); }
    catch (const std::exception& e) { fail(env,e.what()); }
}
API void JNICALL FN(galleryMusicVolume)(JNIEnv* env,jclass,jlong handle,jint volume) {
    try { player(handle).game->audio().set_music_preview_volume(volume); }
    catch (const std::exception& e) { fail(env,e.what()); }
}
API jboolean JNICALL FN(musicWave)(JNIEnv* env,jclass,jlong handle,jfloatArray wave) {
    try {
        const jsize count=wave?env->GetArrayLength(wave):0;
        if (count<64 || count>1024) throw std::invalid_argument("Invalid spectrum window");
        std::array<float,1024> samples{};
        if (!player(handle).game->audio().music_wave(samples.data(),static_cast<std::size_t>(count))) return JNI_FALSE;
        env->SetFloatArrayRegion(wave,0,count,samples.data());
        return JNI_TRUE;
    } catch (const std::exception& e) { fail(env,e.what()); return JNI_FALSE; }
}
API void JNICALL FN(audio)(JNIEnv* env,jclass,jlong handle,jobject output,jint frames) {
    auto* data=static_cast<std::int16_t*>(env->GetDirectBufferAddress(output));
    if (!data || frames<0 || std::int64_t{frames}*4>env->GetDirectBufferCapacity(output)) { fail(env,"Invalid audio buffer"); return; }
    try { player(handle).game->audio().pull(data,static_cast<std::size_t>(frames)); }
    catch (const std::exception& e) { fail(env,e.what()); }
}
