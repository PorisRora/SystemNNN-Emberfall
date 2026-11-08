#pragma once

#include "nnn/game_config.hpp"
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

namespace nnn::game_config {

// CNameList consumes one comma before whitespace, so adjacent commas can
// represent an empty field. Skip a whole encoded character even when a
// translated game's second byte looks like a separator.
inline std::optional<std::string_view> name_token(std::string_view text,std::size_t& cursor) {
    const auto space=[](char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n'; };
    if (cursor<text.size() && text[cursor]==',') ++cursor;
    while (cursor<text.size() && space(text[cursor])) ++cursor;
    if (cursor==text.size() || text[cursor]=='\0') return {};
    const auto start=cursor;
    while (cursor<text.size() && text[cursor]!='\0' && !space(text[cursor]) && text[cursor]!=',') {
        const auto first=static_cast<unsigned char>(text[cursor]);
        cursor+=resource::cp932_lead(first) && cursor+1<text.size() && text[cursor+1]!='\0' ? 2U : 1U;
    }
    return text.substr(start,cursor-start);
}

inline std::optional<std::string_view> bounded_name_token(std::string_view text,std::size_t& cursor) {
    auto value=name_token(text,cursor);
    if (value && value->size()>128U)
        throw std::runtime_error("Name list token exceeds CNameList limit");
    return value;
}

template<std::size_t N>
bool read_name_row(std::string_view text,std::size_t& cursor,std::array<std::string_view,N>& fields) {
    for (auto& field:fields) {
        const auto value=bounded_name_token(text,cursor);
        if (!value) return false;
        field=*value;
    }
    return true;
}

// CNameList setup numbers accept a signed decimal prefix.
inline std::int32_t name_number(std::string_view text) noexcept {
    if (text.empty()) return 0;
    if (text.front()=='+') {
        text.remove_prefix(1);
        if (text.empty() || text.front()<'0' || text.front()>'9') return 0;
    }
    std::int32_t value=0;
    const auto converted=std::from_chars(text.data(),text.data()+text.size(),value);
    if (converted.ec==std::errc::result_out_of_range)
        return text.front()=='-' ? std::numeric_limits<std::int32_t>::min() : std::numeric_limits<std::int32_t>::max();
    return value;
}

class NameListSetup {
public:
    explicit NameListSetup(std::string_view text) {
        std::size_t cursor=0;
        std::array<std::string_view,2> pair;
        while (read_name_row(text,cursor,pair))
            fields_.emplace(ascii_lower(std::string(pair[0])),std::string(pair[1]));
    }
    std::optional<std::string_view> field(std::string key) const {
        const auto it=fields_.find(ascii_lower(std::move(key)));
        if (it==fields_.end()) return {};
        return it->second;
    }
    std::int32_t integer(std::string key,std::int32_t fallback) const {
        const auto value=field(std::move(key));
        return value ? name_number(*value) : fallback;
    }
private:
    std::map<std::string,std::string> fields_;
};

// Script-side lists can specify a game-relative path instead of a basename.
inline std::filesystem::path find_name_list(const std::filesystem::path& root,std::string relative,
    const resource::FilenameEncoder& encode={}) {
    const auto name=resource::normalize_cp932_path(std::move(relative));
    if (name.empty()) return {};
    auto directory=root;
    std::size_t start=0;
    for (auto end=name.find('/');end!=std::string::npos;end=name.find('/',start)) {
        directory=resource::find_directory(directory,name.substr(start,end-start),encode);
        start=end+1;
    }
    const auto basename=name.substr(start);
    auto path=find_file(directory,basename,encode);
    if (path.empty() && basename.size()>=4 && basename.compare(basename.size()-4,4,".xtx")==0)
        path=find_file(directory,basename.substr(0,basename.size()-4)+".fxf",encode);
    return path;
}

} // namespace nnn::game_config
