#include "nnn/png.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

constexpr std::size_t explicit_output_pixel_limit = 4096U;

std::string quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\') out << '\\' << character;
        else if (byte < 0x20U || byte >= 0x80U) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte) << std::dec;
        else out << character;
    }
    out << '"';
    return out.str();
}

void emit_status(const nnn::png::Status& status) {
    std::cout << "{\"code\":" << quote(nnn::png::error_name(status.code)) << ",\"byte_offset\":";
    if (status.byte_offset == nnn::png::no_byte) std::cout << "null"; else std::cout << status.byte_offset;
    std::cout << ",\"detail\":" << quote(status.detail) << '}';
}

nnn::png::Status export_image(const std::string& input, const std::string& output,
                               const nnn::png::Image& image, bool pam) {
    using namespace nnn::png;
    if (image.rgba.size() / 4U > explicit_output_pixel_limit) return {ErrorCode::LimitExceeded, no_byte, "Explicit pixel/export mode is limited to 4096-pixel synthetic samples"};
    namespace fs = std::filesystem;
    std::error_code error;
    const auto source = fs::weakly_canonical(fs::path(input), error);
    if (error) return {ErrorCode::ReadFailed, no_byte, "Cannot resolve input path"};
    const auto destination = fs::weakly_canonical(fs::path(output), error);
    if (error) return {ErrorCode::ReadFailed, no_byte, "Cannot resolve export path"};
    if (source == destination) return {ErrorCode::ReadFailed, no_byte, "Export aliases input; refusing overwrite"};
    const bool equivalent = fs::equivalent(source, destination, error);
    if (!error && equivalent) return {ErrorCode::ReadFailed, no_byte, "Export aliases input; refusing overwrite"};
    std::ofstream file(destination, std::ios::binary | std::ios::trunc);
    if (!file) return {ErrorCode::ReadFailed, no_byte, "Cannot open explicit image output"};
    if (pam) {
        file << "P7\nWIDTH " << image.width << "\nHEIGHT " << image.height << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
        file.write(reinterpret_cast<const char*>(image.rgba.data()), static_cast<std::streamsize>(image.rgba.size()));
    } else {
        file << "P6\n" << image.width << ' ' << image.height << "\n255\n";
        for (std::size_t offset = 0; offset < image.rgba.size(); offset += 4U) {
            // Preserve RGB values, including nonzero RGB at alpha0; no compositing.
            file.write(reinterpret_cast<const char*>(image.rgba.data() + offset), 3);
        }
    }
    file.close();
    if (!file) return {ErrorCode::ReadFailed, no_byte, "Explicit image output write failed"};
    return {};
}

void summary(const nnn::png::Image& image, bool pixels) {
    std::array<std::uint64_t, 4> sums{};
    std::array<std::uint64_t, 256> alpha{};
    std::uint64_t transparent_nonzero = 0;
    for (std::size_t index = 0; index < image.rgba.size(); index += 4U) {
        for (std::size_t channel = 0; channel < 4U; ++channel) sums[channel] += image.rgba[index + channel];
        ++alpha[image.rgba[index + 3U]];
        if (image.rgba[index + 3U] == 0U && (image.rgba[index] != 0U || image.rgba[index + 1U] != 0U || image.rgba[index + 2U] != 0U)) ++transparent_nonzero;
    }
    std::ostringstream crc;
    crc << std::hex << std::setw(8) << std::setfill('0') << static_cast<std::uint32_t>(crc32(0L, image.rgba.data(), static_cast<uInt>(image.rgba.size())));
    std::cout << ",\"width\":" << image.width << ",\"height\":" << image.height << ",\"source_color_type\":" << static_cast<unsigned>(image.source_color_type)
              << ",\"output_format\":\"RGBA8\",\"origin\":\"top-left\",\"premultiplied\":false,\"margins\":{\"top\":" << image.margins.top
              << ",\"left\":" << image.margins.left << ",\"right\":" << image.margins.right << ",\"bottom\":" << image.margins.bottom << '}'
              << ",\"rgba_crc32\":" << quote(crc.str()) << ",\"pixel_summary\":{\"pixel_count\":" << image.rgba.size() / 4U << ",\"byte_count\":" << image.rgba.size()
              << ",\"channel_sums\":[" << sums[0] << ',' << sums[1] << ',' << sums[2] << ',' << sums[3] << "],\"transparent_nonzero_rgb\":" << transparent_nonzero
              << ",\"alpha_histogram\":[";
    for (std::size_t index = 0; index < alpha.size(); ++index) { if (index != 0U) std::cout << ','; std::cout << alpha[index]; }
    std::cout << "]},\"pixels_included\":" << (pixels ? "true" : "false");
    if (pixels) {
        std::cout << ",\"rgba\":[";
        for (std::size_t index = 0; index < image.rgba.size(); ++index) { if (index != 0U) std::cout << ','; std::cout << static_cast<unsigned>(image.rgba[index]); }
        std::cout << ']';
    }
}

void usage() { std::cerr << "Usage: nnn-png-probe INPUT [--pixels] [--pam OUTPUT | --ppm OUTPUT]\n"; }

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) { usage(); return 2; }
        const std::string input = argv[1];
        if (input.empty() || input.front() == '-') { usage(); return 2; }
        bool pixels_requested = false;
        bool pam = false;
        std::string output;
        for (int i = 2; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--pixels" && !pixels_requested) pixels_requested = true;
            else if ((option == "--pam" || option == "--ppm") && output.empty() && i + 1 < argc) {
                pam = option == "--pam";
                output = argv[++i];
                if (output.empty()) { usage(); return 2; }
            } else { usage(); return 2; }
        }
        const auto result = nnn::png::load_dwq8a(input);
        nnn::png::Status option_status;
        if (result.status && pixels_requested && result.image.rgba.size() / 4U > explicit_output_pixel_limit) {
            option_status = {nnn::png::ErrorCode::LimitExceeded, nnn::png::no_byte, "Explicit pixel mode is limited to 4096-pixel synthetic samples"};
        }
        if (result.status && option_status && !output.empty()) option_status = export_image(input, output, result.image, pam);
        std::cout << "{\"loaded\":" << (result.status ? "true" : "false") << ",\"status\":";
        emit_status(result.status);
        summary(result.image, result.status && option_status && pixels_requested);
        std::cout << ",\"export_requested\":" << (!output.empty() ? "true" : "false")
                  << ",\"exported\":" << (result.status && option_status && !output.empty() ? "true" : "false") << ",\"option_status\":";
        if (result.status && (pixels_requested || !output.empty())) emit_status(option_status); else std::cout << "null";
        std::cout << "}\n";
        return result.status && option_status ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "PNG probe error: " << error.what() << '\n';
        return 2;
    }
}
