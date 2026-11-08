#include "game_assets.hpp"
#include "nnn/dwq.hpp"
#include "nnn/game_config.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace emberfall::scene {
namespace fs = std::filesystem;
namespace resource = nnn::resource;
namespace {
constexpr std::string_view pack_id="@pack/", root_pack_id="@root-pack/", loose_id="@loose/";
constexpr std::string_view dwq_directory="dwq/";
constexpr std::string_view groups[]={"bg","ev","sc","sm","sys","ta"};

std::string picture_key(const std::string& raw) {
    if (raw.size()>render::Texture::max_asset_id_bytes) return {};
    return resource::normalize_cp932_path(raw);
}

std::string without_extension(std::string key) {
    if (key.size()>=4 && key.compare(key.size()-4,4,".dwq")==0) key.resize(key.size()-4);
    return key;
}

std::string basename(const std::string& key) {
    const auto slash=key.rfind('/');
    return slash==std::string::npos ? key : key.substr(slash+1);
}

std::string_view picture_group(const std::string& name) {
    // OpenDWQFile checks the full path and recognizes the two system subfamilies first.
    if (name.compare(0,6,"sys/sc")==0) return "sc";
    if (name.compare(0,6,"sys/sm")==0) return "sm";
    for (const std::string_view group:{"ta","bg","ev","sys"})
        if (name.compare(0,group.size(),group)==0) return group;
    return {};
}

std::string pack_reference(const std::string& group,const std::string& base) {
    if (group=="sc" || group=="sm") return "sys/"+group+'/'+base;
    if (group=="sys" && (base.compare(0,2,"sc")==0 || base.compare(0,2,"sm")==0))
        return "sys/sys/"+base;
    return group.empty() ? base : group+'/'+base;
}

int configured_pack_levels(const fs::path& root) {
    std::istringstream init(nnn::game_config::read(root,"init","game.xtx"));
    for (std::string line;std::getline(init,line);) {
        std::istringstream values(line);
        std::string key;
        int count=0;
        if (values>>key>>count && nnn::game_config::ascii_lower(key)=="dwqpacklevel") return count;
    }
    // Some standalone asset sets omit the setup file. Discover their available layers.
    return std::numeric_limits<int>::max();
}

std::shared_ptr<render::Texture> decode(const std::vector<nnn::Byte>& bytes) {
    auto result=nnn::dwq::decode(bytes);
    if(!result.status)throw std::runtime_error(result.status.detail);
    auto& img=result.image;
    const auto& margins=img.margins;
    const auto width=img.width-margins.left-margins.right;
    const auto height=img.height-margins.top-margins.bottom;
    auto texture=std::make_shared<render::Texture>();
    texture->has_mask=img.has_mask;
    texture->canvas_width=static_cast<int>(img.width);
    texture->canvas_height=static_cast<int>(img.height);
    texture->content={static_cast<int>(margins.left),static_cast<int>(margins.top),
        static_cast<int>(width),static_cast<int>(height)};
    auto& cropped=texture->pixels;
    cropped.width=width;cropped.height=height;
    if(width==img.width && height==img.height)cropped.rgba=std::move(img.rgba);
    else {
        cropped.rgba.resize(std::size_t{width}*height*4U);
        for(std::uint32_t y=0;y<height;++y) {
            const auto offset=(std::size_t{y+margins.top}*img.width+margins.left)*4U;
            std::copy_n(img.rgba.data()+offset,std::size_t{width}*4U,cropped.rgba.data()+std::size_t{y}*width*4U);
        }
    }
    return texture;
}
} // namespace

GameAssets::GameAssets(const fs::path& root,std::size_t cache_limit,const resource::FilenameEncoder& encode) : limit_(cache_limit) {
    const auto scripts=resource::find_directory(root,"spt");
    if (scripts.empty()) throw std::runtime_error("Missing spt directory");
    for (const auto& f : fs::directory_iterator(scripts)) {
        if (!f.is_regular_file()) continue;
        if (const auto key=resource::filename_key(f.path().filename(),encode)) scripts_[*key]=f.path();
    }
    const auto dir=resource::find_directory(root,"dwq");
    std::map<std::pair<bool,std::string>,fs::path> tables, data;
    std::vector<fs::path> loose_files;
    auto index_file=[&](const fs::directory_entry& f) {
        if (!f.is_regular_file()) return;
        const auto ext=nnn::game_config::ascii_lower(f.path().extension().u8string());
        if (ext==".dwq") loose_files.push_back(f.path());
        else if ((ext==".gtb" || ext==".gpk") && (f.path().parent_path()==dir || f.path().parent_path()==root)) {
            const auto key=resource::filename_key(f.path().stem(),encode);
            if (!key) return;
            const auto name=std::make_pair(f.path().parent_path()==dir,*key);
            if (ext==".gtb") tables[name]=f.path();
            else data[name]=f.path();
        }
    };
    for (const auto& f : fs::directory_iterator(root)) {
        index_file(f);
        const auto key=nnn::game_config::ascii_lower(f.path().filename().u8string());
        if (f.is_directory() && std::find(std::begin(groups),std::end(groups),key)!=std::end(groups))
            for (const auto& child : fs::recursive_directory_iterator(f.path(),fs::directory_options::skip_permission_denied)) index_file(child);
    }
    if (!dir.empty())
        for (const auto& f : fs::recursive_directory_iterator(dir,fs::directory_options::skip_permission_denied)) index_file(f);
    std::sort(loose_files.begin(),loose_files.end());
    for (const auto& f : loose_files) {
        auto relative=f.lexically_relative(root);
        relative.replace_extension();
        const auto path=resource::relative_key(relative,encode);
        if (!path || path->size()+loose_id.size()>render::Texture::max_asset_id_bytes) continue;
        loose_.emplace(*path,f);
        const auto base=basename(*path);
        if (f.parent_path()==dir) bare_loose_.insert_or_assign(base,*path);
        else bare_loose_.emplace(base,*path);
    }
    const int levels=configured_pack_levels(root);
    for (const auto& item : tables) {
        const auto gpk=data.find(item.first);
        if (gpk==data.end()) continue;
        Pack pack;
        pack.in_dwq=item.first.first;
        pack.stem=item.first.second;
        for (const auto group:groups) {
            if (pack.stem.compare(0,group.size(),group)!=0) continue;
            const auto* suffix=pack.stem.data()+group.size();
            const auto* end=pack.stem.data()+pack.stem.size();
            int level=-1;
            const auto parsed=std::from_chars(suffix,end,level);
            if (parsed.ec==std::errc{} && parsed.ptr==end && level>=0 &&
                std::string_view(suffix,static_cast<std::size_t>(end-suffix))==std::to_string(level)) {
                pack.level=level;
                pack.pc_pack=pack.in_dwq;
            }
            if (pack.level>=0 || suffix==end) pack.group=group;
            break;
        }
        if (pack.level>=0 && pack.level>=levels) continue;
        auto index=nnn::archive::load_index(item.second.string(),gpk->second.string());
        if (!index.status) throw std::runtime_error(pack.stem+".gtb: "+index.status.detail);
        pack.file=gpk->second;
        pack.index=std::move(index.index);
        for (const auto& e : pack.index.entries()) {
            const auto base=basename(without_extension(picture_key(e.name)));
            // PC folds the requested basename, then compares it with the raw GTB name.
            if (!base.empty() && (!pack.pc_pack || base==e.name)) pack.names.emplace(base,e.name);
        }
        packs_.push_back(std::move(pack));
    }
    std::sort(packs_.begin(),packs_.end(),[](const Pack& a,const Pack& b) {
        if (a.group!=b.group) return a.group<b.group;
        if (a.level!=b.level) return a.level>b.level;
        if (a.in_dwq!=b.in_dwq) return a.in_dwq;
        return a.stem<b.stem;
    });
}

fs::path GameAssets::config(const std::string& name) const {
    if (!resource::valid_component(name)) return {};
    auto key=resource::fold_cp932(name);
    auto found=scripts_.find(key);
    if (found==scripts_.end() && key.size()>=4 && key.compare(key.size()-4,4,".xtx")==0) {
        key.replace(key.size()-4,4,".fxf");
        found=scripts_.find(key);
    }
    return found==scripts_.end() ? fs::path{} : found->second;
}

nnn::ReadResult GameAssets::script(const std::string& relative) const {
    const std::string prefix="spt/";
    const auto key=resource::normalize_cp932_path(relative);
    const auto path=config(key.compare(0,prefix.size(),prefix)==0 ? key.substr(prefix.size()) : key);
    if (path.empty()) return {{nnn::ErrorCode::NotFound,nnn::no_word,"Missing game script: "+relative},{},0};
    return nnn::read_script_file(path.string());
}

GameAssets::Graphic GameAssets::find_graphic(const std::string& name,bool pc_only) const {
    auto key=picture_key(name);
    if (key.empty()) return {};
    const auto packed=[&](const Pack& pack,const std::string& base) -> Graphic {
        const auto member=pack.names.find(base);
        if (member==pack.names.end()) return {};
        return {&pack.file,&pack,&member->second,std::string(pack.in_dwq?pack_id:root_pack_id)+pack.stem+'/'+base,
            pack_reference(pack.group,base)};
    };
    const auto loose=[&](const std::string& path) -> Graphic {
        const auto file=loose_.find(path);
        if (file==loose_.end()) return {};
        return {&file->second,nullptr,nullptr,std::string(loose_id)+path,
            path.compare(0,dwq_directory.size(),dwq_directory)==0 ? path.substr(dwq_directory.size()) : path};
    };
    const bool root_pack=key.compare(0,root_pack_id.size(),root_pack_id)==0;
    if (!pc_only && (root_pack || key.compare(0,pack_id.size(),pack_id)==0)) {
        const auto prefix=root_pack?root_pack_id:pack_id;
        const auto slash=key.find('/',prefix.size());
        if (slash==std::string::npos) return {};
        const auto stem=key.substr(prefix.size(),slash-prefix.size());
        for (const auto& pack:packs_) if (pack.in_dwq!=root_pack && pack.stem==stem) return packed(pack,key.substr(slash+1));
        return {};
    }
    if (!pc_only && key.compare(0,loose_id.size(),loose_id)==0) return loose(key.substr(loose_id.size()));

    key=without_extension(std::move(key));
    const auto base=basename(key);
    if (base.empty()) return {};
    if (pc_only || key.find('/')!=std::string::npos) {
        const auto group=picture_group(key);
        if (!group.empty()) for (const auto& pack:packs_) {
            if (pack.group!=group || (pc_only && !pack.pc_pack)) continue;
            if (auto found=packed(pack,base);found.file) return found;
        }
        auto found=loose(std::string(dwq_directory)+key);
        if (found.file || pc_only) return found;
        return loose(key);
    }

    // Old saves and callers may only provide a basename.
    const auto file=bare_loose_.find(base);
    if (file!=bare_loose_.end()) return loose(file->second);
    const auto group=base.compare(0,3,"sys")==0 ? "sys" : base.substr(0,2);
    for (const auto& pack:packs_) if (pack.group==group) {
        if (auto found=packed(pack,base);found.file) return found;
    }
    for (const auto& pack:packs_) if (pack.group!=group) {
        if (auto found=packed(pack,base);found.file) return found;
    }
    return {};
}

std::string GameAssets::pc_reference(const render::Texture& texture) const {
    if (texture.name.size()>nnn::archive::max_name_bytes) return {};
    const auto resource=find_graphic(texture.name,true);
    if (!resource.file || resource.identity!=texture.asset_id) return {};
    auto name=texture.name;
    std::replace(name.begin(),name.end(),'/','\\');
    return name;
}

bool GameAssets::has_image(const std::string& name) const { return find_graphic(name).file!=nullptr; }

std::shared_ptr<const render::Texture> GameAssets::image(const std::string& name,bool pc_only) {
    if (name.empty()) return {};
    const auto graphic=find_graphic(name,pc_only);
    if (!graphic.file) throw std::runtime_error("Graphic absent from loose files and packs: "+name);
    const auto cached=cache_.find(graphic.identity);
    if (cached!=cache_.end()) {
        ++stats_.hits;
        recent_.splice(recent_.begin(),recent_,cached->second);
        return cached->second->second;
    }
    std::vector<nnn::Byte> bytes;
    if (graphic.pack) {
        auto read=nnn::archive::read_member(graphic.file->string(),graphic.pack->index,*graphic.member);
        if (!read.status) throw std::runtime_error(read.status.detail);
        bytes=std::move(read.bytes); ++stats_.packed_reads;
    } else {
        if (fs::file_size(*graphic.file)>nnn::image::max_input_bytes) throw std::runtime_error("Loose graphic exceeds image limit");
        auto read=nnn::read_script_file(graphic.file->string());
        if (!read.status) throw std::runtime_error(read.status.detail);
        bytes=std::move(read.bytes); ++stats_.loose_reads;
    }
    auto texture=decode(bytes);
    texture->name=graphic.name;
    texture->source=graphic.file->filename().u8string();
    texture->asset_id=graphic.identity;
    ++stats_.decodes;
    const auto size=texture->pixels.rgba.size();
    if (size<=limit_) {
        while (!recent_.empty() && stats_.cached_bytes>limit_-size) {
            const auto& victim=recent_.back();
            stats_.cached_bytes-=victim.second->pixels.rgba.size();
            cache_.erase(victim.first); recent_.pop_back(); ++stats_.evictions;
        }
        recent_.emplace_front(graphic.identity,texture);
        cache_.emplace(graphic.identity,recent_.begin()); stats_.cached_bytes+=size;
    }
    return texture;
}
} // namespace emberfall::scene
