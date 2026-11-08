#pragma once

#include "nnn/resource_path.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace nnn::game_config {
inline constexpr std::size_t max_text_bytes=4U*1024U*1024U;

inline std::string ascii_lower(std::string text) {
    for (char& c:text) if (c>='A' && c<='Z') c=static_cast<char>(c+'a'-'A');
    return text;
}

using resource::find_file;

// CNameList::LoadInit tries the encrypted init2 setup before the text setup.
// Script-side lists keep their FXF beside the corresponding XTX.
inline std::filesystem::path find(const std::filesystem::path& root,const std::string& folder,const std::string& name,
                                  const resource::FilenameEncoder& encode={}) {
    if (!resource::valid_component(name))
        throw std::invalid_argument("Invalid configuration basename");
    const auto key=resource::fold_cp932(name);
    const bool setup=ascii_lower(folder)=="init";
    const bool text=key.size()>=4 && key.compare(key.size()-4,4,".xtx")==0;
    if (setup && text) {
        auto encrypted=find_file(resource::find_directory(root,"init2"),name.substr(0,name.size()-4)+".fxf",encode);
        if (!encrypted.empty()) return encrypted;
    }
    const auto directory=resource::find_directory(root,folder,encode);
    auto path=find_file(directory,name,encode);
    if (setup || !text || !path.empty()) return path;
    return find_file(directory,name.substr(0,name.size()-4)+".fxf",encode);
}

inline std::string read_text(const std::filesystem::path& path) {
    if (path.empty() || !std::filesystem::is_regular_file(path)) return {};
    const auto size=std::filesystem::file_size(path);
    if (size>max_text_bytes) throw std::runtime_error("Configuration exceeds text limit: "+path.u8string());
    std::ifstream input(path,std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read configuration: "+path.u8string());
    std::string text(static_cast<std::size_t>(size),'\0');
    if (size && !input.read(text.data(),static_cast<std::streamsize>(size)))
        throw std::runtime_error("Short configuration read: "+path.u8string());
    char extra=0;
    if (input.get(extra) || input.bad() || !input.eof()) throw std::runtime_error("Configuration changed while reading: "+path.u8string());
    if (ascii_lower(path.extension().u8string())==".fxf")
        for (char& c:text) c=static_cast<char>(static_cast<unsigned char>(c)^0xffU);
    return text;
}

inline std::string read(const std::filesystem::path& root,const std::string& folder,const std::string& name,
                        const resource::FilenameEncoder& encode={}) {
    return read_text(find(root,folder,name,encode));
}
} // namespace nnn::game_config
