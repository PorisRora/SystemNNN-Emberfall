#include "nnn/image.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

std::string quote(const std::string& value) {
    std::ostringstream output;
    output << '"';
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\') output << '\\' << character;
        else if (byte < 0x20U || byte >= 0x80U) {
            output << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned int>(byte) << std::dec;
        } else output << character;
    }
    output << '"';
    return output.str();
}

void emit_status(const nnn::image::Status& status) {
    std::cout << "{\"code\":" << quote(nnn::image::error_name(status.code)) << ",\"byte_offset\":";
    if (status.byte_offset == nnn::image::no_byte) std::cout << "null";
    else std::cout << status.byte_offset;
    std::cout << ",\"detail\":" << quote(status.detail) << '}';
}

nnn::image::Status export_ppm(const std::string& input, const std::string& output,
                            const nnn::image::Image& image) {
    namespace fs = std::filesystem;
    std::error_code error;
    const auto input_path = fs::weakly_canonical(fs::path(input), error);
    if (error) return {nnn::image::ErrorCode::ReadFailed, nnn::image::no_byte, "Cannot resolve input path"};
    const auto output_path = fs::weakly_canonical(fs::path(output), error);
    if (error) return {nnn::image::ErrorCode::ReadFailed, nnn::image::no_byte, "Cannot resolve PPM output path"};
    if (input_path == output_path) {
        return {nnn::image::ErrorCode::ReadFailed, nnn::image::no_byte, "PPM output aliases input; refusing overwrite"};
    }
    const bool same_file = fs::equivalent(input_path, output_path, error);
    if (!error && same_file) {
        return {nnn::image::ErrorCode::ReadFailed, nnn::image::no_byte, "PPM output aliases input; refusing overwrite"};
    }
    std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
    if (!file) return {nnn::image::ErrorCode::ReadFailed, nnn::image::no_byte, "Cannot open PPM output"};
    file << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    file.write(reinterpret_cast<const char*>(image.rgb.data()), static_cast<std::streamsize>(image.rgb.size()));
    file.close();
    if (!file) return {nnn::image::ErrorCode::ReadFailed, nnn::image::no_byte, "PPM output write failed"};
    return {};
}

void emit_image(const nnn::image::Image& image) {
    std::cout << ",\"width\":" << image.width << ",\"height\":" << image.height
              << ",\"source_bit_depth\":" << image.source_bit_depth
              << ",\"output_format\":\"RGB8\",\"origin\":\"top-left\",\"rgb\":[";
    std::uint64_t sums[3] = {0, 0, 0};
    for (std::size_t index = 0; index < image.rgb.size(); ++index) {
        if (index != 0) std::cout << ',';
        std::cout << static_cast<unsigned int>(image.rgb[index]);
        sums[index % 3U] += image.rgb[index];
    }
    std::cout << "],\"pixel_summary\":{\"pixel_count\":" << image.rgb.size() / 3U
              << ",\"byte_count\":" << image.rgb.size() << ",\"channel_sums\":["
              << sums[0] << ',' << sums[1] << ',' << sums[2] << "]}";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2 && argc != 4) {
            std::cerr << "Usage: nnn-image-probe INPUT [--ppm OUTPUT]\n";
            return 2;
        }
        const std::string input = argv[1];
        if (input.empty() || input.front() == '-' || (argc == 4 && std::string(argv[2]) != "--ppm")) {
            std::cerr << "Usage: nnn-image-probe INPUT [--ppm OUTPUT]\n";
            return 2;
        }
        if (argc == 4 && std::string(argv[3]).empty()) {
            std::cerr << "PPM output path must not be empty\n";
            return 2;
        }
        const auto result = nnn::image::load_dwq(input);
        nnn::image::Status ppm_status;
        const bool ppm_requested = argc == 4;
        if (result.status && ppm_requested) ppm_status = export_ppm(input, argv[3], result.image);
        std::cout << "{\"loaded\":" << (result.status ? "true" : "false") << ",\"status\":";
        emit_status(result.status);
        emit_image(result.image);
        std::cout << ",\"ppm_requested\":" << (ppm_requested ? "true" : "false")
                  << ",\"ppm_exported\":" << (result.status && ppm_requested && ppm_status ? "true" : "false")
                  << ",\"ppm_status\":";
        if (result.status && ppm_requested) emit_status(ppm_status);
        else std::cout << "null";
        std::cout << "}\n";
        return result.status && (!ppm_requested || ppm_status) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Image probe error: " << error.what() << '\n';
        return 2;
    }
}
