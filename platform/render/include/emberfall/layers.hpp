#pragma once

#include "emberfall/render.hpp"
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace nnn::snapshot { class Writer; class Reader; }

namespace emberfall::render {

namespace effects { struct State; }

// Pixel storage holds only the visible DWQ region. Script rectangles use the original canvas.
struct Texture {
    std::string name, source;
    int canvas_width = 0, canvas_height = 0;
    Rect content{};
    RgbaImage pixels;
    bool has_mask = true;
    // A stable resource key can distinguish the same basename in different packs.
    static constexpr std::size_t max_asset_id_bytes = 1024;
    std::string asset_id;
};

void paint_texture(Surface& out,const Texture& texture,Rect source,Point origin,int opacity=100);

struct Layer {
    std::shared_ptr<const Texture> texture;
    Rect source{}, from{}, to{};
    std::vector<std::int32_t> parameters;
    std::shared_ptr<effects::State> effect_state;
    std::uint32_t count = 0;
    int effect = 0;
    bool enabled = false;
    bool opaque = false;
};
struct FragmentMesh {
    std::vector<std::array<int,2>> vertices;
    std::vector<std::vector<int>> faces;
};

struct ChartLayerTarget {
    Byte* pixels=nullptr;
    std::size_t stride=0,size=0;
};
struct ChartLayerSource {
    const Byte* pixels=nullptr;
    std::size_t stride=0,size=0;
    bool opaque=false;
};
struct ChartLayerScratch {
    Surface frame;
};

// A detached frame for a host that draws its message UI between the scene and
// the last script layer. The host supplies a premultiplied RGBA message plane.
class ScreenLayerFrame {
public:
    unsigned width() const noexcept { return scene_.width; }
    unsigned height() const noexcept { return scene_.height; }
    bool captures_screen() const noexcept;
    const Surface& composed() const noexcept { return output_; }
    Surface capture_without_ui() const;
    void composite(Byte* pixels,std::size_t stride,bool show_backdrop=true);

private:
    friend class Layers;
    Surface scene_,decorated_scene_,saved_,output_;
    std::array<Layer,32> slots_;
    std::vector<std::pair<std::string,std::shared_ptr<const Texture>>> animation_;
    bool animation_off_=false,speaking_=false,draw_last_=true;
};

class Layers {
public:
    using TextureReader=std::function<std::shared_ptr<const Texture>(const std::string&)>;
    using OverlapDecoration=std::function<void(Surface&)>;
    Layers(int width, int height);
    void clear_all();
    void clear(unsigned slot);
    void bind(unsigned slot, std::shared_ptr<const Texture> texture, bool preload);
    void rectangle(unsigned slot, Rect from, Rect to);
    // Unknown effects still draw their texture and return false for reporting.
    bool effect(unsigned slot, int number, std::vector<std::int32_t> parameters,const FragmentMesh* mesh=nullptr);
    bool restore_layer(unsigned slot,Layer layer,const FragmentMesh* mesh=nullptr);
    void advance(std::uint32_t frames,Point cursor={-1,-1});
    void prepare_overlap();
    bool start_overlap(std::uint32_t frames,int type=0,const OverlapDecoration& decorate={});
    void finish_overlap();
    void resume_overlap_scene();
    bool has_overlap_decoration() const noexcept { return !overlap_display_.rgb.empty(); }
    bool overlapping() const noexcept;
    std::uint32_t overlap_remaining() const noexcept { return overlap_time_-overlap_count_; }
    int overlap_type() const noexcept { return overlap_type_; }
    unsigned width() const noexcept { return output_.width; }
    unsigned height() const noexcept { return output_.height; }
    const Surface& surface();
    bool screen_layer_active() const noexcept;
    void capture_screen_layer(bool enable,OverlapDecoration backdrop={});
    std::unique_ptr<ScreenLayerFrame> take_screen_layer() noexcept { return std::move(screen_layer_); }
    void commit_screen_capture(Surface frame);
    // SELECTCHART interleaves three script layer groups with its own art.
    // Active targets are premultiplied RGBA8 at the supplied raster size,
    // or at logical size when omitted. Empty groups leave targets untouched.
    unsigned chart_layer_mask(unsigned center) const noexcept;
    unsigned draw_chart_layers(unsigned center,const std::array<ChartLayerTarget,3>& targets,ChartLayerScratch& scratch,
                               unsigned raster_width=0,unsigned raster_height=0);
    bool chart_needs_composite(unsigned center) const noexcept;
    // Read opaque tiles, then insert the two premultiplied panes between
    // script ranges. A missing center or content pane is transparent.
    bool draw_chart_composite(unsigned center,ChartLayerSource base,ChartLayerSource center_plane,
                              ChartLayerSource content,ChartLayerTarget target,ChartLayerScratch& scratch,
                              unsigned raster_width=0,unsigned raster_height=0);
    std::uint64_t composition_revision() const noexcept { return revision_; }
    void saved_screen(Surface frame,bool background=false);
    bool needs_redraw() const noexcept { return dirty_ || output_changed_; }
    void hide_layers(std::uint32_t mask) noexcept { hidden_=mask; dirty_=true; }
    void animation_textures(TextureReader reader) { animation_textures_=std::move(reader); ++revision_; }
    void set_animation_off(bool off) noexcept { animation_off_=off; invalidate(); }
    void set_speaking(bool value) noexcept;
    std::uint32_t hidden_layers() const noexcept { return hidden_; }
    const std::array<Layer,32>& slots() const noexcept { return slots_; }
    unsigned duration(unsigned slot) const;
    void save_snapshot(nnn::snapshot::Writer& out,bool decorated_overlap=false) const;
    void load_snapshot(nnn::snapshot::Reader& in,const std::function<std::shared_ptr<const Texture>(const std::string&)>& texture,
                       bool compact_output=true,bool typed_overlap=true,bool opacity_field=true,bool once_state=true,
                       bool particle_state=true,bool decorated_overlap=false);
    static unsigned inspect_snapshot(nnn::snapshot::Reader& in,bool compact_output,bool typed_overlap,
                                     bool opacity_field,bool once_state,bool particle_state,bool decorated_overlap);

private:
    Layers()=default;
    std::array<Layer,32> slots_;
    TextureReader animation_textures_;
    Surface output_, previous_, target_, saved_, overlap_display_;
    std::uint32_t overlap_time_ = 0, overlap_count_ = 0;
    int overlap_type_=0;
    std::uint32_t hidden_ = 0;
    std::uint64_t revision_=0;
    bool dirty_ = true;
    bool saved_background_ = false;
    bool animation_off_ = false, speaking_ = false;
    bool capture_screen_layer_ = false;
    OverlapDecoration screen_backdrop_;
    std::unique_ptr<ScreenLayerFrame> screen_layer_;
    // A completed overlap already owns its endpoint pixels, but the platform
    // must still present them even when no layer needs to be recomposed.
    bool output_changed_ = false;
    void invalidate() noexcept { dirty_=true; ++revision_; }
    void compose(Surface& out,const OverlapDecoration& decorate={});
};
} // namespace emberfall::render
