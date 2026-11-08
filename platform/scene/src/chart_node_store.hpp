#pragma once

#include "chart_node_save.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#ifdef __unix__
#include <fcntl.h>
#include <unistd.h>
#endif

namespace emberfall::scene {

// Relative chart paths come only from validated source node IDs. The original
// installation is a read-only fallback when a private Android node is absent.
class ChartNodeStore {
public:
    explicit ChartNodeStore(std::filesystem::path original_save_root)
        : original_(std::move(original_save_root)) {}

    void private_root(std::filesystem::path path) { private_=std::move(path); }

    std::optional<ChartNodeSave> read(nnn::Value slot,const nnn::chart::Graph& graph) const {
        const auto relative=checked_path(slot,graph);
        auto file=private_.empty() ? std::filesystem::path{} : private_/relative;
        if (file.empty() || !std::filesystem::exists(file)) {
            if (original_.empty()) return {};
            const auto directory=nnn::resource::find_directory(original_,relative.parent_path().string());
            if (directory.empty()) return {};
            file=nnn::game_config::find_file(directory,relative.filename().string());
        }
        if (file.empty()) return {};
        return parse_chart_node(read_exact(file,max_node_bytes),graph);
    }

    void write(nnn::Value slot,const ChartNodeSave& state,const nnn::chart::Graph& graph) const {
        if (private_.empty()) throw std::runtime_error("Chart node storage has no private directory");
        const auto file=private_/checked_path(slot,graph);
        const auto bytes=write_chart_node(state,graph);
        std::filesystem::create_directories(file.parent_path());
        const auto serial=sequence_.fetch_add(1,std::memory_order_relaxed);
        const auto clock=std::chrono::steady_clock::now().time_since_epoch().count();
        auto pending=file;
        pending+=".new-"+std::to_string(clock)+"-"+std::to_string(serial);
        try {
            std::ofstream out(pending,std::ios::binary|std::ios::trunc);
            if (!out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))
                throw std::runtime_error("Cannot write the chart node");
            out.close();
            if (!out) throw std::runtime_error("Cannot close the chart node");
            sync_file(pending);
            std::filesystem::rename(pending,file);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(pending,ignored);
            throw;
        }
    }

    nnn::chart::Progress legacy_progress(const nnn::chart::Graph& graph) const {
        auto result=graph.global_defaults();
        read_flags("mita.dat",result.seen);
        read_flags("tsuka.dat",result.traversed);
        return result;
    }

private:
    static std::filesystem::path checked_path(nnn::Value slot,const nnn::chart::Graph& graph) {
        auto name=graph.legacy_slot_path(slot);
        if (name.empty()) throw std::runtime_error("Unknown chart node");
        return name;
    }
    static nnn::snapshot::Bytes read_exact(const std::filesystem::path& file,std::size_t limit) {
        const auto size=std::filesystem::file_size(file);
        if (size>limit) throw std::runtime_error("Chart data exceeds its storage limit");
        nnn::snapshot::Bytes bytes(static_cast<std::size_t>(size));
        std::ifstream in(file,std::ios::binary);
        if (!in || (size!=0 && !in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size))) ||
            in.peek()!=std::char_traits<char>::eof() || in.bad()) throw std::runtime_error("Cannot read complete chart data");
        return bytes;
    }
    void read_flags(const char* name,std::vector<nnn::Byte>& target) const {
        if (original_.empty()) return;
        const auto file=nnn::game_config::find_file(original_,name);
        if (file.empty()) return;
        auto bytes=read_exact(file,target.size());
        if (bytes.size()!=target.size()) throw std::runtime_error("Invalid legacy chart flag count");
        target=std::move(bytes);
    }
    static void sync_file(const std::filesystem::path& file) {
#ifdef __unix__
        const int fd=::open(file.c_str(),O_RDONLY|O_CLOEXEC);
        if (fd<0) throw std::runtime_error("Cannot reopen chart node for sync");
        const int flushed=::fsync(fd);
        const int closed=::close(fd);
        if (flushed!=0 || closed!=0) throw std::runtime_error("Cannot flush chart node data");
#else
        (void)file;
#endif
    }

    static constexpr std::size_t max_node_bytes=2U*1024U*1024U;
    inline static std::atomic<std::uint64_t> sequence_{0};
    std::filesystem::path original_,private_;
};

} // namespace emberfall::scene
