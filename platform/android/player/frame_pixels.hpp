#pragma once

#include "emberfall/render.hpp"
#include <jni.h>
#include <stdexcept>
#include <vector>

namespace emberfall::android {
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
