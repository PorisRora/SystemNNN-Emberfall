#include <android/native_window_jni.h>
#include <jni.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
constexpr int audio_rate = 48000;
constexpr std::int64_t max_pixels = 4096LL * 2304;

void check(int status, const char* operation) {
    if (status >= 0) return;
    char detail[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(status, detail, sizeof(detail));
    throw std::runtime_error(std::string(operation) + ": " + detail);
}

struct Format {
    AVFormatContext* ptr = nullptr;
    ~Format() { avformat_close_input(&ptr); }
};
struct Codec {
    AVCodecContext* ptr = nullptr;
    ~Codec() { avcodec_free_context(&ptr); }
};
struct Frame {
    AVFrame* ptr = av_frame_alloc();
    ~Frame() { av_frame_free(&ptr); }
};
struct Packet {
    AVPacket* ptr = av_packet_alloc();
    ~Packet() { av_packet_free(&ptr); }
};
struct Window {
    ANativeWindow* ptr = nullptr;
    ~Window() { if (ptr) ANativeWindow_release(ptr); }
};

struct Decoder {
    Format format;
    Codec codec;
    Frame frame;
    Packet packet;
    Window window;
    SwsContext* scale = nullptr;
    SwrContext* samples = nullptr;
    int stream = -1;
    bool end_of_input = false;
    std::int64_t start_us = 0, next_us = 0;

    ~Decoder() {
        sws_freeContext(scale);
        swr_free(&samples);
    }

    bool open(const std::string& path, AVMediaType type) {
        format.ptr = avformat_alloc_context();
        if (!format.ptr || !frame.ptr || !packet.ptr) throw std::bad_alloc();
        format.ptr->max_streams = 16;
        format.ptr->probesize = 1024 * 1024;
        format.ptr->max_analyze_duration = 1000000;
        check(avformat_open_input(&format.ptr, path.c_str(), nullptr, nullptr), "Open movie");
        check(avformat_find_stream_info(format.ptr, nullptr), "Probe movie streams");
        const AVCodec* decoder = nullptr;
        stream = av_find_best_stream(format.ptr, type, -1, -1, &decoder, 0);
        if (stream == AVERROR_STREAM_NOT_FOUND && type == AVMEDIA_TYPE_AUDIO) return false;
        check(stream, "Movie track");
        for (unsigned i = 0; i < format.ptr->nb_streams; ++i)
            if (i != static_cast<unsigned>(stream)) format.ptr->streams[i]->discard = AVDISCARD_ALL;
        auto* selected = format.ptr->streams[stream];
        codec.ptr = avcodec_alloc_context3(decoder);
        if (!codec.ptr) throw std::bad_alloc();
        check(avcodec_parameters_to_context(codec.ptr, selected->codecpar), "Movie parameters");
        codec.ptr->max_pixels = max_pixels;
        codec.ptr->thread_count = type == AVMEDIA_TYPE_VIDEO ? 2 : 1;
        check(avcodec_open2(codec.ptr, decoder, nullptr), "Movie codec");
        if (format.ptr->start_time != AV_NOPTS_VALUE) start_us = format.ptr->start_time;
        return true;
    }

    std::int64_t next() {
        av_frame_unref(frame.ptr);
        for (;;) {
            const int result = avcodec_receive_frame(codec.ptr, frame.ptr);
            if (result == AVERROR_EOF) return -1;
            if (result >= 0) break;
            if (result != AVERROR(EAGAIN)) check(result, "Decode frame");
            if (end_of_input) return -1;
            do {
                av_packet_unref(packet.ptr);
                const int read = av_read_frame(format.ptr, packet.ptr);
                if (read == AVERROR_EOF) {
                    end_of_input = true;
                    break;
                }
                check(read, "Read movie");
            } while (packet.ptr->stream_index != stream);
            check(avcodec_send_packet(codec.ptr, end_of_input ? nullptr : packet.ptr), "Decode packet");
        }
        const auto* selected = format.ptr->streams[stream];
        const auto timestamp = frame.ptr->best_effort_timestamp;
        const auto pts = timestamp == AV_NOPTS_VALUE ? next_us
            : std::max<std::int64_t>(0, av_rescale_q(timestamp, selected->time_base, AV_TIME_BASE_Q) - start_us);
        const auto duration = frame.ptr->duration;
        next_us = pts + (duration > 0 ? av_rescale_q(duration, selected->time_base, AV_TIME_BASE_Q) : 33333);
        return pts;
    }

    bool present() {
        auto* picture = frame.ptr;
        if (!window.ptr || picture->width <= 0 || picture->height <= 0 ||
            static_cast<std::int64_t>(picture->width) * picture->height > max_pixels) return false;
        scale = sws_getCachedContext(scale, picture->width, picture->height, static_cast<AVPixelFormat>(picture->format),
            codec.ptr->width, codec.ptr->height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!scale) throw std::runtime_error("Movie pixel format");
        ANativeWindow_Buffer buffer{};
        if (ANativeWindow_lock(window.ptr, &buffer, nullptr) != 0) return false;
        bool drawn = false;
        if (buffer.width == codec.ptr->width && buffer.height == codec.ptr->height && buffer.stride >= buffer.width) {
            std::uint8_t* pixels[4] = {static_cast<std::uint8_t*>(buffer.bits)};
            const int stride[4] = {buffer.stride * 4};
            drawn = sws_scale(scale, picture->data, picture->linesize, 0, picture->height, pixels, stride) == buffer.height;
        }
        return ANativeWindow_unlockAndPost(window.ptr) == 0 && drawn;
    }

    int pcm(std::int16_t* output, int frames) {
        const auto* data = frame.ptr;
        const int channels = data->ch_layout.nb_channels;
        if (channels < 1 || channels > 8 || data->sample_rate < 8000 || data->sample_rate > 192000 ||
            data->nb_samples < 0 || data->nb_samples > 32768) throw std::runtime_error("Movie audio bounds");
        const auto format_id = static_cast<AVSampleFormat>(data->format);
        if (!samples) {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            check(swr_alloc_set_opts2(&samples, &stereo, AV_SAMPLE_FMT_S16, audio_rate, &data->ch_layout,
                format_id, data->sample_rate, 0, nullptr), "Movie audio format");
            check(swr_init(samples), "Movie resampler");
        }
        const std::uint8_t* planes[8]{};
        std::copy_n(data->extended_data, av_sample_fmt_is_planar(format_id) ? channels : 1, planes);
        std::uint8_t* dest = reinterpret_cast<std::uint8_t*>(output);
        const int count = swr_convert(samples, &dest, frames, planes, data->nb_samples);
        check(count, "Movie resample");
        return count * 2;
    }
};

Decoder& decoder(jlong token) { return *reinterpret_cast<Decoder*>(token); }
void failure(JNIEnv* env, const std::exception& error) {
    if (!env->ExceptionCheck()) env->ThrowNew(env->FindClass("java/lang/IllegalStateException"), error.what());
}
} // namespace

#define MOVIE(name) Java_com_tyranor_runtime_systemnnn_MoviePlayback_##name

extern "C" JNIEXPORT jlong JNICALL MOVIE(open)(JNIEnv* env, jclass, jbyteArray path, jobject surface) {
    try {
        const auto length = env->GetArrayLength(path);
        if (length < 1 || length > 4096) throw std::runtime_error("Invalid movie path");
        std::string name(static_cast<std::size_t>(length), '\0');
        env->GetByteArrayRegion(path, 0, length, reinterpret_cast<jbyte*>(name.data()));
        if (env->ExceptionCheck()) return 0;
        if (name.front() != '/' || name.find('\0') != std::string::npos) throw std::runtime_error("Invalid movie path");
        auto value = std::make_unique<Decoder>();
        if (!value->open(name, surface ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO)) return 0;
        if (surface) {
            const int w = value->codec.ptr->width, h = value->codec.ptr->height;
            if (w < 1 || h < 1 || static_cast<std::int64_t>(w) * h > max_pixels)
                throw std::runtime_error("Movie dimensions exceed the limit");
            value->window.ptr = ANativeWindow_fromSurface(env, surface);
            if (!value->window.ptr || ANativeWindow_setBuffersGeometry(value->window.ptr, w, h, WINDOW_FORMAT_RGBA_8888) != 0)
                throw std::runtime_error("Movie surface unavailable");
        }
        return reinterpret_cast<jlong>(value.release());
    } catch (const std::exception& error) { failure(env, error); return 0; }
}

extern "C" JNIEXPORT jintArray JNICALL MOVIE(size)(JNIEnv* env, jclass, jlong token) {
    const auto* c = decoder(token).codec.ptr;
    const jint values[] = {c->width, c->height};
    auto result = env->NewIntArray(2);
    if (result) env->SetIntArrayRegion(result, 0, 2, values);
    return result;
}

extern "C" JNIEXPORT jlong JNICALL MOVIE(next)(JNIEnv* env, jclass, jlong token, jint catch_up) {
    try {
        auto& movie = decoder(token);
        movie.codec.ptr->skip_frame = catch_up>=2 ? AVDISCARD_NONKEY : catch_up==1 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
        return movie.next();
    }
    catch (const std::exception& error) { failure(env, error); return -1; }
}

extern "C" JNIEXPORT jboolean JNICALL MOVIE(present)(JNIEnv* env, jclass, jlong token) {
    try { return decoder(token).present(); }
    catch (const std::exception& error) { failure(env, error); return false; }
}

extern "C" JNIEXPORT jint JNICALL MOVIE(samples)(JNIEnv* env, jclass, jlong token, jshortArray result) {
    auto* output = env->GetShortArrayElements(result, nullptr);
    if (!output) return 0;
    try {
        const auto count = decoder(token).pcm(output, env->GetArrayLength(result) / 2);
        env->ReleaseShortArrayElements(result, output, 0);
        return count;
    } catch (const std::exception& error) {
        env->ReleaseShortArrayElements(result, output, JNI_ABORT);
        failure(env, error);
        return 0;
    }
}

extern "C" JNIEXPORT void JNICALL MOVIE(close)(JNIEnv*, jclass, jlong token) {
    delete reinterpret_cast<Decoder*>(token);
}
