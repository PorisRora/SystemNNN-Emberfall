#include "emberfall/scene.hpp"
#include "nnn/png.hpp"
#include "nnn/script.hpp"
#include "nnn/scheduler.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace emberfall::scene {
namespace {

constexpr nnn::Word filmtypetime = 0x33330052U;
constexpr nnn::Word music_command = 0x33330007U;
constexpr nnn::Word voice_volume_command = 0x33330069U;
constexpr nnn::Word storyname = 0x33330051U;
constexpr nnn::Word loaddwq = 0x33330015U;
constexpr nnn::Word clearall = 0x33330012U;
constexpr nnn::Word clearlayer = 0x33330014U;
constexpr nnn::Word seteffect = 0x33330013U;
constexpr nnn::Word setrect = 0x33330016U;
constexpr nnn::Word draw = 0x22220004U;
constexpr nnn::Word end = 0x55560005U;

struct HostStop {};

struct Layer {
    bool enabled = false;
    bool bound = false;
    nnn::Word effect = nop_effect;
    render::RgbaImage image;
    render::Point position;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

std::uint32_t le32(const std::vector<std::uint8_t>& bytes, std::size_t offset) noexcept {
    return std::uint32_t{bytes[offset]} | (std::uint32_t{bytes[offset + 1U]} << 8U)
        | (std::uint32_t{bytes[offset + 2U]} << 16U) | (std::uint32_t{bytes[offset + 3U]} << 24U);
}

class Host final : public nnn::CScriptCallBack {
public:
    Host(const Reader& read, FrameResult& result, nnn::MusicStopController& music,
         const nnn::MusicStopOutput& stop_output, VoiceSession& voice)
        : read_(read), result_(result), music_(music), stop_output_(stop_output), voice_(voice) {
        for (nnn::Word channel = 0; channel < voice_inputs_.size(); ++channel) {
            auto& inputs = voice_inputs_[channel];
            inputs.skip_flags = [this] { return GetSkipFlags(); };
            if (voice.live.base_voice_volume) inputs.base_voice_volume = std::ref(voice.live.base_voice_volume);
            if (voice.live.character_adjust) inputs.character_adjust = std::ref(voice.live.character_adjust);
            if (voice.current_name_for_channel) {
                inputs.current_voice_name = [this,channel] { return voice_.current_name_for_channel(channel); };
            } else if (channel == 0 && voice.live.current_voice_name) {
                inputs.current_voice_name = std::ref(voice.live.current_voice_name);
            }
        }
    }
    void attach(nnn::CExecScript& vm) noexcept { vm_ = &vm; }
    void attach(nnn::ScriptScheduler& owner) noexcept { owner_ = &owner; }
    const nnn::CExecScript* active_vm() const noexcept {
        return owner_ ? owner_->GetVM(owner_->ActiveMode()) : vm_;
    }
    const std::string& failure() const noexcept { return failure_; }
    bool has_draw() const noexcept { return draw_seen_; }

    bool compose(std::uint32_t width, std::uint32_t height) {
        render::Surface output{width, height,
            std::vector<render::Byte>(static_cast<std::size_t>(std::uint64_t{width} * height * 3U), 0)};
        for (const auto& layer : layers_) {
            if (!layer.enabled || !layer.bound) continue;
            if (layer.effect != nop_effect) return reject("Unsupported retained layer effect");
            const auto status = render::blit(layer.image, output, {0, 0, layer.width, layer.height},
                                             layer.position, render::BlendProfile::Source32Mmx);
            if (status != render::Status::Ok) return reject("Layer composition failed");
            ++result_.layers_drawn;
        }
        result_.surface = std::move(output);
        return true;
    }

    void ReceiveScriptCommand(nnn::Word command, nnn::Value primary, const std::string& text,
                              nnn::Value trailing) override {
        if (!callback()) throw HostStop{};
        if (owner_ && (command == 0x5556000aU || command == 0x55560016U)) {
            if (primary != 0 || trailing != 0 || text.empty()) stop("Invalid child script request");
            return; // The scheduler owns child loading, switching and resumption.
        }
        if (command == filmtypetime) {
            if (!text.empty() || trailing != 0) stop("Invalid FILMTYPETIME fields");
            // The original scene host acknowledges every signed value without changing the scene.
            return;
        }
        if (command == storyname) {
            if (primary != 1 || trailing != 0) stop("Invalid STORYNAME fields");
            result_.story_name.assign(text.data(), std::min<std::size_t>(text.size(), 62U));
            return;
        }
        if (command == loaddwq) {
            if (trailing != 0 || !load(primary, text)) {
                if (failure_.empty()) reject("Invalid LOADDWQ fields");
                throw HostStop{};
            }
            return;
        }
        stop("Unsupported text host event");
    }

    bool ReceiveNumericScriptCommand(nnn::Word command, nnn::Value primary,
                                     const std::vector<nnn::Value>& values, nnn::Value trailing) override {
        if (!callback()) return false;
        if (trailing != 0) return reject("Invalid numeric trailing field");
        if (command == voice_volume_command) {
            if (primary != 2 || values.size() != 2U || (values[0] != 0 && values[0] != 1) || values[1] != -999)
                return reject("Unsupported voice volume request");
            const auto channel = static_cast<nnn::Word>(values[0]);
            const auto status = voice_.controller.ApplyMinus999(channel, voice_inputs_[channel], voice_.outputs);
            if (!status) return reject(status.detail.c_str());
            return true;
        }
        if (command == music_command) {
            if (primary != 2 || values.size() != 2U || values[0] != -1 || values[1] != 0)
                return reject("Unsupported MUSIC request");
            const auto status = music_.StopNow(GetSkipFlags(), stop_output_);
            if (!status) return reject(status.detail.c_str());
            return true;
        }
        if (command == clearall) {
            if (primary != 0 || !values.empty()) return reject("Invalid CLEARALLEFFECT fields");
            for (auto& layer : layers_) layer = Layer{};
            return true;
        }
        if (command == clearlayer) {
            if (primary != 1 || values.size() != 1U) return reject("Invalid CLEAREFFECTLAYER fields");
            std::size_t index = 0;
            if (!layer_index(values[0], index)) return false;
            layers_[index] = Layer{};
            return true;
        }
        if (command == seteffect) {
            if (primary != 2 || values.size() != 2U || values[1] != static_cast<nnn::Value>(nop_effect)) {
                return reject("Only exact NOP effect is supported");
            }
            std::size_t index = 0;
            if (!layer_index(values[0], index)) return false;
            auto& layer = layers_[index];
            if (!layer.bound) return reject("NOP requires a bound layer");
            layer.effect = nop_effect;
            layer.enabled = true;
            return true;
        }
        if (command == setrect) {
            if (primary != 9 || values.size() != 9U) return reject("Invalid SETEFFECTRECT fields");
            std::size_t index = 0;
            if (!layer_index(values[0], index)) return false;
            auto& layer = layers_[index];
            if (!layer.bound) return reject("Rectangle requires a bound layer");
            for (std::size_t field = 1; field <= 4U; ++field) {
                if (values[field] != values[field + 4U]) return reject("Rectangle transition is unsupported");
            }
            if (values[3] < 0 || values[4] < 0
                || static_cast<std::uint32_t>(values[3]) > layer.image.width
                || static_cast<std::uint32_t>(values[4]) > layer.image.height) {
                return reject("Rectangle extents exceed the loaded image");
            }
            // Source PrintLayers uses dst.right/bottom as extents, not corners.
            layer.position = {values[1], values[2]};
            layer.width = values[3];
            layer.height = values[4];
            return true;
        }
        if (command == draw) {
            if (primary != 1 || values.size() != 1U || values[0] == 0) {
                return reject("Only nonzero DRAW presentation is supported");
            }
            const auto* vm = active_vm();
            if (!vm || draw_seen_) return reject("Invalid presentation state");
            result_.draw_pc = vm->GetPC();
            if (owner_) result_.draw_mode = static_cast<std::uint32_t>(owner_->ActiveMode());
            result_.draw_value = values[0];
            draw_seen_ = true;
            return true;
        }
        return reject("Unsupported numeric host event");
    }

    void PreReceiveScriptData(nnn::Word, nnn::Value, const std::string&) override {
        unsupported_callback();
    }
    void ReceiveUserCommand0(nnn::Word, const std::vector<nnn::Value>&) override {
        unsupported_callback();
    }
    void ReceiveUserFunction0(nnn::Word code, const std::vector<nnn::Value>& values) override {
        if (!callback()) throw HostStop{};
        const auto* vm = active_vm();
        if (!vm || code == nnn::no_word || code != vm->BoundStopF5Id() || !values.empty())
            stop("Unsupported user function");
        // DW-012 applies the five host skip flags after this callback returns.
    }
    nnn::Value GetVarData(nnn::Word) override { unsupported_callback(); }
    nnn::Value GetCalcuVarData(nnn::Word) override { unsupported_callback(); }
    void SetVarData(nnn::Word, nnn::Value) override { unsupported_callback(); }

private:
    bool reject(const char* message) {
        if (failure_.empty()) failure_ = message;
        return false;
    }
    [[noreturn]] void stop(const char* message) {
        reject(message);
        throw HostStop{};
    }
    bool callback() {
        if (result_.callbacks >= max_callbacks) return reject("Scene callback limit reached");
        ++result_.callbacks;
        return true;
    }
    [[noreturn]] void unsupported_callback() {
        if (!callback()) throw HostStop{};
        stop("Unsupported script callback");
    }
    bool layer_index(nnn::Value layer, std::size_t& index) {
        if (layer == static_cast<nnn::Value>(first_layer)) { index = 0; return true; }
        if (layer == static_cast<nnn::Value>(second_layer)) { index = 1; return true; }
        return reject("Only foreground layers4 and5 are supported");
    }
    bool load(nnn::Value number, const std::string& name) {
        std::size_t index = 0;
        if (!layer_index(number, index)) return false;
        if (name.empty() || name.size() > max_name_bytes || name.find('\0') != std::string::npos) {
            return reject("Invalid raw resource key");
        }
        if (!read_) return reject("Resource Reader is missing");
        if (result_.resource_reads >= max_resource_reads) return reject("Resource read limit reached");
        ++result_.resource_reads;
        std::vector<std::uint8_t> bytes;
        bool read_ok = false;
        try { read_ok = read_(name, bytes); }
        catch (...) { return reject("Resource Reader threw an exception"); }
        const auto size = static_cast<std::uint64_t>(bytes.size());
        if (size > max_resource_bytes || size > max_total_resource_bytes - result_.resource_bytes) {
            return reject("Resource byte limit reached");
        }
        result_.resource_bytes += size;
        if (!read_ok) return reject("Resource Reader failed");
        if (bytes.size() < 64U) return reject("Resource DWQ header is truncated");
        const auto width = le32(bytes, 36U), height = le32(bytes, 40U);
        if (width == 0U || height == 0U || width > max_dimension || height > max_dimension) {
            return reject("Resource dimension limit reached");
        }
        const std::uint64_t pixels = std::uint64_t{width} * height;
        if (pixels > max_pixels || pixels > max_decoded_pixels - result_.decoded_pixels) {
            return reject("Decoded pixel limit reached");
        }
        if (le32(bytes, 16U) != 0U || le32(bytes, 20U) != 0U || le32(bytes, 24U) != 0U || le32(bytes, 28U) != 0U) {
            return reject("Resource cut margins are unsupported");
        }
        auto decoded = nnn::png::decode_dwq8a(bytes);
        if (!decoded.status) return reject("Resource PNG decode failed");
        if (decoded.image.width != width || decoded.image.height != height
            || decoded.image.rgba.size() != static_cast<std::size_t>(pixels * 4U)) {
            return reject("Resource decoded extent is inconsistent");
        }
        Layer next;
        next.enabled = true;
        next.bound = true;
        next.effect = layers_[index].effect;
        next.image = {width, height, std::move(decoded.image.rgba)};
        next.width = static_cast<std::int32_t>(width);
        next.height = static_cast<std::int32_t>(height);
        result_.decoded_pixels += pixels;
        layers_[index] = std::move(next);
        return true;
    }

    const Reader& read_;
    FrameResult& result_;
    nnn::MusicStopController& music_;
    const nnn::MusicStopOutput& stop_output_;
    VoiceSession& voice_;
    std::array<nnn::VoiceLiveInputs, 2> voice_inputs_;
    nnn::CExecScript* vm_ = nullptr;
    nnn::ScriptScheduler* owner_ = nullptr;
    std::array<Layer, 2> layers_{};
    std::string failure_;
    bool draw_seen_ = false;
};

} // namespace

FrameResult first_frame(const std::vector<std::uint8_t>& script, const Reader& read,
                        std::uint32_t width, std::uint32_t height) {
    return first_frame(script, read, width, height, nullptr);
}

FrameResult first_frame(const std::vector<std::uint8_t>& script, const Reader& read,
                        std::uint32_t width, std::uint32_t height, const nnn::FunctionRegistry* functions) {
    return first_frame(script, read, width, height, functions, {});
}

FrameResult first_frame(const std::vector<std::uint8_t>& script, const Reader& read,
                        std::uint32_t width, std::uint32_t height, const nnn::FunctionRegistry* functions,
                        const Reader& read_child) {
    nnn::MusicStopController silent;
    return first_frame(script, read, width, height, functions, read_child, silent, {});
}

FrameResult first_frame(const std::vector<std::uint8_t>& script, const Reader& read,
                        std::uint32_t width, std::uint32_t height, const nnn::FunctionRegistry* functions,
                        const Reader& read_child, nnn::MusicStopController& music,
                        const nnn::MusicStopOutput& stop_output, nnn::SkipFlags initial_skip) {
    VoiceSession unbound_voice;
    return first_frame(script, read, width, height, functions, read_child, music, stop_output, initial_skip, unbound_voice);
}

FrameResult first_frame(const std::vector<std::uint8_t>& script, const Reader& read,
                        std::uint32_t width, std::uint32_t height, const nnn::FunctionRegistry* functions,
                        const Reader& read_child, nnn::MusicStopController& music,
                        const nnn::MusicStopOutput& stop_output, nnn::SkipFlags initial_skip, VoiceSession& voice) {
    FrameResult result;
    try {
        if (width == 0U || height == 0U || width > max_dimension || height > max_dimension
            || std::uint64_t{width} * height > max_pixels) {
            result.error = "Invalid output dimensions";
            return result;
        }
        if (script.size() > max_script_bytes) {
            result.error = "Scene script byte limit reached";
            return result;
        }
        Host host(read, result, music, stop_output, voice);
        host.SetSkipFlags(initial_skip);
        nnn::Status loaded;
        nnn::ExecResult execution;
        if (read_child) {
            nnn::Reader resolver = [&](const std::string& relative) -> nnn::ReadResult {
                if (result.child_script_reads >= 8U)
                    return {{nnn::ErrorCode::ReadFailed, nnn::no_word, "Scene child read limit"}, {}, 0};
                ++result.child_script_reads;
                std::vector<nnn::Byte> bytes;
                if (!read_child(relative, bytes))
                    return {{nnn::ErrorCode::NotFound, nnn::no_word, "Scene child script missing"}, {}, 0};
                const auto size = static_cast<std::uint64_t>(bytes.size());
                if (size > max_script_bytes || size > 1024U * 1024U - result.child_script_bytes)
                    return {{nnn::ErrorCode::ReadFailed, nnn::no_word, "Scene child byte limit"}, {}, 0};
                result.child_script_bytes += size;
                return {{}, std::move(bytes), size};
            };
            nnn::ScriptScheduler owner(host, resolver, functions);
            host.attach(owner);
            loaded = owner.LoadBytes(script, "scene-control");
            if (loaded) {
                const auto yielded = owner.Exec(max_script_work, 32);
                result.script_transitions = yielded.transitions;
                execution = {yielded.status, yielded.return_code, yielded.pc, yielded.work_used};
            }
        } else {
            nnn::CExecScript vm(host);
            host.attach(vm);
            loaded = vm.LoadBytes(script, "scene-first-frame", {}, functions);
            if (loaded) execution = vm.Exec(max_script_work);
        }
        if (!loaded) {
            result.error = std::string("Script load failed: ") + nnn::error_name(loaded.code);
            return result;
        }
        result.exec_calls = 1;
        result.script_work = execution.work_used;
        if (!host.failure().empty()) result.error = host.failure();
        else if (!execution.status) {
            result.error = std::string("Script execution failed: ") + nnn::error_name(execution.status.code);
            if (read_child && !execution.status.detail.empty()) result.error += " (" + execution.status.detail + ")";
        }
        else if (execution.return_code == end) result.error = "Script ended without a presentation point";
        else if (execution.return_code != draw || !host.has_draw() || result.draw_value == 0
                 || execution.pc != result.draw_pc + 5U) result.error = "Missing supported DRAW presentation yield";
        else if (!host.compose(width, height)) result.error = host.failure();
    } catch (...) {
        result.surface = {};
        result.error = "Scene preparation failed with an exception";
    }
    if (!result.error.empty()) result.surface = {};
    return result;
}

} // namespace emberfall::scene
