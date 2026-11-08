#pragma once

#include "emberfall/layers.hpp"
#include "nnn/archive.hpp"
#include "nnn/resource_path.hpp"
#include "nnn/script.hpp"
#include <filesystem>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

namespace emberfall::scene {
struct AssetStats {
    std::size_t hits=0, decodes=0, evictions=0, cached_bytes=0, packed_reads=0, loose_reads=0;
};

class GameAssets {
public:
    explicit GameAssets(const std::filesystem::path& root,std::size_t cache_limit,const nnn::resource::FilenameEncoder& encode={});
    // PC slot filenames are original relative names, including when they have no directory.
    std::shared_ptr<const render::Texture> image(const std::string& game_name, bool pc_only = false);
    bool has_image(const std::string& game_name) const;
    std::string pc_reference(const render::Texture& texture) const;
    nnn::ReadResult script(const std::string& relative) const;
    std::filesystem::path config(const std::string& name) const;
    const AssetStats& stats() const noexcept { return stats_; }

private:
    struct Pack {
        std::string stem, group;
        int level = -1;
        bool in_dwq = false, pc_pack = false;
        std::filesystem::path file;
        nnn::archive::Index index;
        std::unordered_map<std::string,std::string> names;
    };
    struct Graphic {
        const std::filesystem::path* file = nullptr;
        const Pack* pack = nullptr;
        const std::string* member = nullptr;
        std::string identity, name;
    };
    Graphic find_graphic(const std::string& name, bool pc_only = false) const;
    using Cached = std::pair<std::string,std::shared_ptr<const render::Texture>>;
    std::vector<Pack> packs_;
    std::map<std::string,std::filesystem::path> loose_, scripts_;
    std::unordered_map<std::string,std::string> bare_loose_;
    std::list<Cached> recent_;
    std::unordered_map<std::string,std::list<Cached>::iterator> cache_;
    std::size_t limit_;
    AssetStats stats_;
};
} // namespace emberfall::scene

