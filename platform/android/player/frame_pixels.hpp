#pragma once

#include "emberfall/render.hpp"
#include <android/bitmap.h>
#include <jni.h>
#include <stdexcept>
#include <vector>

namespace emberfall::android {
struct LockedBitmap {
    JNIEnv* env;
    jobject bitmap;
    AndroidBitmapInfo info{};
    void* data=nullptr;
    LockedBitmap(JNIEnv* env,jobject bitmap):env(env),bitmap(bitmap) {
        if (!bitmap || AndroidBitmap_getInfo(env,bitmap,&info)!=ANDROID_BITMAP_RESULT_SUCCESS ||
            info.format!=ANDROID_BITMAP_FORMAT_RGBA_8888 || !info.width || !info.height ||
            info.stride<static_cast<std::uint64_t>(info.width)*4 ||
            AndroidBitmap_lockPixels(env,bitmap,&data)!=ANDROID_BITMAP_RESULT_SUCCESS)
            throw std::runtime_error("Cannot access RGBA bitmap");
        if (!data) {
            AndroidBitmap_unlockPixels(env,bitmap);
            throw std::runtime_error("RGBA bitmap has no pixel buffer");
        }
    }
    ~LockedBitmap(){AndroidBitmap_unlockPixels(env,bitmap);}
    LockedBitmap(const LockedBitmap&)=delete;
    LockedBitmap& operator=(const LockedBitmap&)=delete;
    std::uint8_t* row(unsigned y) const {return static_cast<std::uint8_t*>(data)+static_cast<std::size_t>(y)*info.stride;}
};

inline jintArray frame_pixels(JNIEnv* env,const render::Surface& source,jint width,jint height) {
    if (width<1 || height<1 || width>4096 || height>4096 ||
        static_cast<std::int64_t>(width)*height>2*1024*1024)
        throw std::invalid_argument("Invalid capture dimensions");
    std::vector<jint> pixels(static_cast<std::size_t>(width)*height);
    for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
        const auto row=static_cast<unsigned>(y)*source.height/static_cast<unsigned>(height);
        const auto column=static_cast<unsigned>(x)*source.width/static_cast<unsigned>(width);
        const auto index=(static_cast<std::size_t>(row)*source.width+column)*3;
        pixels[static_cast<std::size_t>(y)*width+x]=static_cast<jint>(0xff000000U|
            (static_cast<std::uint32_t>(source.rgb[index])<<16U)|
            (static_cast<std::uint32_t>(source.rgb[index+1])<<8U)|source.rgb[index+2]);
    }
    auto out=env->NewIntArray(static_cast<jsize>(pixels.size()));
    if (out) env->SetIntArrayRegion(out,0,static_cast<jsize>(pixels.size()),pixels.data());
    return out;
}
}
