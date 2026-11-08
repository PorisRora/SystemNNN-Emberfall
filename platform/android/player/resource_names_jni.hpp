#pragma once

#include "nnn/resource_path.hpp"
#include <jni.h>
#include <stdexcept>

namespace emberfall::android {
inline nnn::resource::FilenameEncoder filename_encoder(JNIEnv* env) {
    JavaVM* vm=nullptr;
    if (env->GetJavaVM(&vm)!=JNI_OK || !vm) throw std::runtime_error("Cannot access Android filename codec");
    return [vm](const std::string& utf8) -> std::optional<std::string> {
        if (utf8.size()>4096) return std::nullopt;
        void* current=nullptr;
        if (vm->GetEnv(&current,JNI_VERSION_1_6)!=JNI_OK)
            throw std::runtime_error("Filename lookup requires an attached Android thread");
        auto* jni=static_cast<JNIEnv*>(current);
        if (jni->PushLocalFrame(4)<0) throw std::runtime_error("Cannot allocate filename references");
        struct LocalFrame {
            JNIEnv* env;
            ~LocalFrame(){env->PopLocalFrame(nullptr);}
        } frame{jni};
        auto codec=jni->FindClass("com/tyranor/runtime/systemnnn/GameResourcePaths");
        if (!codec) throw std::runtime_error("Android filename codec is unavailable");
        auto encode=jni->GetStaticMethodID(codec,"encodeFilename","([B)[B");
        if (!encode) throw std::runtime_error("Android filename encoder is unavailable");
        auto length=static_cast<jsize>(utf8.size());
        auto input=jni->NewByteArray(length);
        if (!input) throw std::runtime_error("Cannot allocate filename input");
        jni->SetByteArrayRegion(input,0,length,reinterpret_cast<const jbyte*>(utf8.data()));
        if (jni->ExceptionCheck()) throw std::runtime_error("Cannot read filename input");
        auto encoded=static_cast<jbyteArray>(jni->CallStaticObjectMethod(codec,encode,input));
        if (jni->ExceptionCheck()) throw std::runtime_error("Android filename encoding failed");
        if (!encoded) return std::nullopt;
        length=jni->GetArrayLength(encoded);
        std::string bytes(static_cast<std::size_t>(length),'\0');
        jni->GetByteArrayRegion(encoded,0,length,reinterpret_cast<jbyte*>(bytes.data()));
        if (jni->ExceptionCheck()) throw std::runtime_error("Cannot read filename encoding");
        return bytes;
    };
}
} // namespace emberfall::android
