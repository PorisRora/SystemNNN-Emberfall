#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace nnn::resource {

// An actual non-ASCII UTF-8 filename component becomes original CP932 bytes.
// Return nullopt when the filename cannot be represented without substitution.
using FilenameEncoder=std::function<std::optional<std::string>(const std::string& utf8_component)>;

inline bool cp932_lead(unsigned char c) noexcept { return (c>=0x81U && c<=0x9fU) || (c>=0xe0U && c<=0xfcU); }
inline bool cp932_trail(unsigned char c) noexcept { return c>=0x40U && c<=0xfcU && c!=0x7fU; }
std::string fold_cp932(std::string value);
bool valid_component(std::string_view encoded) noexcept;
// Normalize only separators outside CP932 characters. An invalid path returns empty.
std::string normalize_cp932_path(std::string encoded);

std::optional<std::string> filename_key(const std::filesystem::path& component,const FilenameEncoder& encode={});
std::optional<std::string> relative_key(const std::filesystem::path& path,const FilenameEncoder& encode={});
std::filesystem::path find_directory(const std::filesystem::path& parent,const std::string& name,const FilenameEncoder& encode={});
std::filesystem::path find_file(const std::filesystem::path& parent,const std::string& name,const FilenameEncoder& encode={});

} // namespace nnn::resource
