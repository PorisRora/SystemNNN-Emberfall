// Synthetic fixed archive expectations, separate from encoded GTB table constants.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace dw006_fixture {
struct ExpectedEntry { std::string name; std::uint64_t offset, length; std::vector<std::uint8_t> bytes; };
struct Fixture { const char* name; const char* gtb_filename; const char* gpk_filename; std::uint64_t gpk_size; std::vector<ExpectedEntry> entries; bool virtual_gpk = false; bool extended64 = false; };
inline const std::vector<Fixture> fixtures = {
    {"three_unsorted_name_pool", "three_unsorted_name_pool.gtb", "three_unsorted_name_pool.gpk", 10, {
        {std::string("\x61\x6c\x70\x68\x61", 5), 0, 3, {65, 0, 66}},
        {std::string("\x62\x65\x74\x61", 4), 3, 5, {10, 20, 30, 40, 50}},
        {std::string("\x67\x61\x6d\x6d\x61", 5), 8, 2, {255, 0}},
    }, false, false},
    {"single_member", "single_member.gtb", "single_member.gpk", 4, {
        {std::string("\x73\x6f\x6c\x6f", 4), 0, 4, {9, 8, 7, 6}},
    }, false, false},
    {"empty_archive", "empty_archive.gtb", "empty_archive.gpk", 0, {
    }, false, false},
    {"raw_byte_names", "raw_byte_names.gtb", "raw_byte_names.gpk", 6, {
        {std::string("\x61", 1), 0, 1, {17}},
        {std::string("\x7a", 1), 1, 2, {34, 35}},
        {std::string("\x81\x40", 2), 3, 3, {68, 69, 70}},
    }, false, false},
    {"wide_three_members", "wide_three_members.gtb", "wide_three_members.gpk", 10, {
        {std::string("\x61\x6c\x70\x68\x61", 5), 0, 3, {65, 0, 66}},
        {std::string("\x62\x65\x74\x61", 4), 3, 5, {10, 20, 30, 40, 50}},
        {std::string("\x67\x61\x6d\x6d\x61", 5), 8, 2, {255, 0}},
    }, false, true},
    {"wide_empty_archive", "wide_empty_archive.gtb", "wide_empty_archive.gpk", 0, {
    }, false, true},
    {"wide_virtual_over2g", "wide_virtual_over2g.gtb", "", 2147483664, {
        {std::string("\x6c\x61\x72\x67\x65", 5), 0, 2147483653, {}},
        {std::string("\x74\x61\x69\x6c", 4), 2147483653, 11, {}},
    }, true, true},
};
} // namespace dw006_fixture
