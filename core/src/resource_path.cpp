#include "nnn/resource_path.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <iconv.h>
#endif

namespace nnn::resource {
namespace fs=std::filesystem;
namespace {

bool ascii(std::string_view value) {
    return std::all_of(value.begin(),value.end(),[](char c){return static_cast<unsigned char>(c)<0x80U;});
}

std::optional<std::string> encode_host(const std::string& utf8) {
#ifdef _WIN32
    const auto wide=fs::u8path(utf8).wstring();
    if (wide.size()>static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
    const auto count=static_cast<int>(wide.size());
    BOOL substituted=FALSE;
    const int size=WideCharToMultiByte(932,WC_NO_BEST_FIT_CHARS,wide.data(),count,nullptr,0,nullptr,&substituted);
    if (size<=0 || substituted) return {};
    std::string encoded(static_cast<std::size_t>(size),'\0');
    if (WideCharToMultiByte(932,WC_NO_BEST_FIT_CHARS,wide.data(),count,encoded.data(),size,nullptr,&substituted)!=size || substituted)
        return {};
    return encoded;
#elif defined(__linux__) && !defined(__ANDROID__)
    const auto decoder=iconv_open("CP932","UTF-8");
    if (decoder==reinterpret_cast<iconv_t>(-1)) return {};
    std::string encoded(utf8.size(),'\0');
    auto* in=const_cast<char*>(utf8.data());
    auto* out=encoded.data();
    std::size_t remaining=utf8.size(),space=encoded.size();
    const auto converted=iconv(decoder,&in,&remaining,&out,&space);
    iconv_close(decoder);
    if (converted!=0 || remaining!=0) return {};
    encoded.resize(encoded.size()-space);
    return encoded;
#else
    (void)utf8;
    return {};
#endif
}

fs::path find_child(const fs::path& parent,const std::string& name,bool directory,const FilenameEncoder& encode) {
    if (parent.empty() || !valid_component(name)) return {};
    const auto wanted=[directory](const fs::path& path) {
        return directory ? fs::is_directory(path) : fs::is_regular_file(path);
    };
    const bool ascii_name=ascii(name);
    if (ascii_name) {
        const auto exact=parent/name;
        if (wanted(exact)) return exact;
    }
    if (!fs::is_directory(parent)) return {};
    const auto key=fold_cp932(name);
    fs::path found;
    for (const auto& entry:fs::directory_iterator(parent)) {
        const auto component=entry.path().filename();
        if (ascii_name && !ascii(component.u8string())) continue;
        const auto candidate=filename_key(component,encode);
        if (candidate && *candidate==key && wanted(entry.path()) && (found.empty() || entry.path()<found)) found=entry.path();
    }
    return found;
}

} // namespace

std::string fold_cp932(std::string value) {
    for (std::size_t i=0;i<value.size();++i) {
        const auto c=static_cast<unsigned char>(value[i]);
        if (cp932_lead(c) && i+1<value.size()) { ++i;continue; }
        if (c>='A' && c<='Z') value[i]=static_cast<char>(c+'a'-'A');
    }
    return value;
}

bool valid_component(std::string_view encoded) noexcept {
    if (encoded.empty() || encoded.back()=='.' || encoded.back()==' ') return false;
    for (std::size_t i=0;i<encoded.size();++i) {
        const auto c=static_cast<unsigned char>(encoded[i]);
        if (cp932_lead(c)) {
            if (++i>=encoded.size() || !cp932_trail(static_cast<unsigned char>(encoded[i]))) return false;
        } else if (c<0x20U || c==0x7fU || c=='/' || c=='\\' || c==':' || c=='*' || c=='?' ||
                   c=='"' || c=='<' || c=='>' || c=='|') return false;
    }
    return true;
}

std::string normalize_cp932_path(std::string encoded) {
    std::size_t start=0;
    for (std::size_t i=0;i<encoded.size();++i) {
        if (cp932_lead(static_cast<unsigned char>(encoded[i])) && i+1<encoded.size()) { ++i;continue; }
        if (encoded[i]!='/' && encoded[i]!='\\') continue;
        if (!valid_component(std::string_view(encoded).substr(start,i-start))) return {};
        encoded[i]='/';start=i+1;
    }
    if (!valid_component(std::string_view(encoded).substr(start))) return {};
    return fold_cp932(std::move(encoded));
}

std::optional<std::string> filename_key(const fs::path& component,const FilenameEncoder& encode) {
    if (component!=component.filename()) return {};
    auto utf8=component.u8string();
    if (ascii(utf8)) return valid_component(utf8) ? std::optional<std::string>{fold_cp932(std::move(utf8))} : std::nullopt;
    auto encoded=encode ? encode(utf8) : encode_host(utf8);
    if (!encoded || !valid_component(*encoded)) return {};
    return fold_cp932(std::move(*encoded));
}

std::optional<std::string> relative_key(const fs::path& path,const FilenameEncoder& encode) {
    if (path.empty() || path.has_root_path()) return {};
    std::string key;
    for (const auto& component:path) {
        const auto part=filename_key(component,encode);
        if (!part) return {};
        if (!key.empty()) key+='/';
        key+=*part;
    }
    return key;
}

fs::path find_directory(const fs::path& parent,const std::string& name,const FilenameEncoder& encode) {
    return find_child(parent,name,true,encode);
}

fs::path find_file(const fs::path& parent,const std::string& name,const FilenameEncoder& encode) {
    return find_child(parent,name,false,encode);
}

} // namespace nnn::resource
