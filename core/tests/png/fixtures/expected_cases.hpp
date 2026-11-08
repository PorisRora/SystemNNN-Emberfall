// Self-authored fixed RGBA expectations.
#pragma once
#include <cstdint>
#include <vector>
namespace dw007_fixture {
struct Fixture { const char* name; const char* binary_filename; std::uint32_t width,height; std::vector<std::uint8_t> expected_rgba; std::uint32_t margin_top,margin_left,margin_right,margin_bottom; std::uint8_t source_color_type; };
inline const std::vector<Fixture> fixtures = {
    {"rgb_1x1_red", "rgb_1x1_red.dwq", 1, 1, {255, 0, 0, 255}, 0, 0, 0, 0, 2},
    {"rgba_1x1_transparent_nonzero_rgb", "rgba_1x1_transparent_nonzero_rgb.dwq", 1, 1, {9, 80, 170, 0}, 0, 0, 0, 0, 6},
    {"rgba_alpha_edges_3x2", "rgba_alpha_edges_3x2.dwq", 3, 2, {255, 1, 2, 0, 3, 254, 4, 1, 5, 6, 253, 127, 252, 7, 8, 128, 9, 251, 10, 254, 11, 12, 250, 255}, 0, 0, 0, 0, 6},
    {"rgb_non_square_corners_2x3", "rgb_non_square_corners_2x3.dwq", 2, 3, {255, 0, 0, 255, 0, 255, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255}, 0, 0, 0, 0, 2},
    {"rgba_canvas_cut_margins_4x3", "rgba_canvas_cut_margins_4x3.dwq", 4, 3, {9, 8, 7, 0, 1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0, 10, 11, 12, 0, 255, 0, 0, 128, 0, 255, 0, 255, 13, 14, 15, 0, 16, 17, 18, 0, 19, 20, 21, 0, 22, 23, 24, 0, 25, 26, 27, 0}, 1, 1, 1, 1, 6},
    {"rgba_fully_transparent_writer_margin_fallback", "rgba_fully_transparent_writer_margin_fallback.dwq", 3, 2, {1, 2, 3, 0, 40, 50, 60, 0, 70, 80, 90, 0, 100, 110, 120, 0, 130, 140, 150, 0, 160, 170, 180, 0}, 1, 2, 0, 0, 6},
    {"rgb_all_filters_0_to_4", "rgb_all_filters_0_to_4.dwq", 3, 5, {1, 2, 3, 255, 10, 20, 30, 255, 100, 110, 120, 255, 2, 3, 4, 255, 20, 30, 40, 255, 120, 130, 140, 255, 3, 4, 5, 255, 30, 40, 50, 255, 140, 150, 160, 255, 4, 5, 6, 255, 40, 50, 60, 255, 160, 170, 180, 255, 5, 6, 7, 255, 50, 60, 70, 255, 180, 190, 200, 255}, 0, 0, 0, 0, 2},
    {"rgba_all_filters_0_to_4", "rgba_all_filters_0_to_4.dwq", 3, 5, {1, 2, 3, 0, 10, 20, 30, 1, 100, 110, 120, 127, 2, 3, 4, 128, 20, 30, 40, 254, 120, 130, 140, 255, 3, 4, 5, 255, 30, 40, 50, 128, 140, 150, 160, 0, 4, 5, 6, 1, 40, 50, 60, 127, 160, 170, 180, 254, 5, 6, 7, 254, 50, 60, 70, 255, 180, 190, 200, 0}, 0, 0, 0, 0, 6},
    {"rgba_multiple_idat", "rgba_multiple_idat.dwq", 2, 2, {255, 0, 0, 0, 0, 255, 0, 128, 0, 0, 255, 254, 255, 255, 0, 255}, 0, 0, 0, 0, 6},
    {"rgb_metadata_no_color_transform", "rgb_metadata_no_color_transform.dwq", 2, 2, {12, 34, 56, 255, 210, 130, 70, 255, 1, 127, 254, 255, 255, 128, 0, 255}, 0, 0, 0, 0, 2},
    {"rgba_iccp_metadata_no_color_transform", "rgba_iccp_metadata_no_color_transform.dwq", 2, 2, {12, 34, 56, 0, 210, 130, 70, 127, 1, 127, 254, 128, 255, 128, 0, 255}, 0, 0, 0, 0, 6},
    {"rgb_post_idat_text", "rgb_post_idat_text.dwq", 2, 1, {90, 80, 70, 255, 60, 50, 40, 255}, 0, 0, 0, 0, 2},
};
} // namespace dw007_fixture
