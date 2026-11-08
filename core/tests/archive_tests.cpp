#include <nnn/archive.hpp>

#include "archives/fixtures/expected_cases.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using nnn::archive::Byte;
using nnn::archive::Entry;
using nnn::archive::ErrorCode;
using nnn::archive::Index;
using Bytes = std::vector<Byte>;

void require(bool condition, const std::string& detail) {
    if (!condition) throw std::runtime_error(detail);
}

struct Tests {
    int passed = 0;
    int failed = 0;
    void run(const std::string& name, const std::function<void()>& body) {
        try {
            body();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
};

void put32(Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        bytes.at(offset + shift / 8U) = static_cast<Byte>((value >> shift) & 255U);
    }
}

void put64(Bytes& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        bytes.at(offset + shift / 8U) = static_cast<Byte>((value >> shift) & 255U);
    }
}

Bytes table(const std::vector<std::uint32_t>& names, const std::vector<std::uint32_t>& offsets,
            const Bytes& pool) {
    require(names.size() == offsets.size(), "test table arrays have equal length");
    Bytes result(4U + names.size() * 8U, Byte{0});
    put32(result, 0, static_cast<std::uint32_t>(names.size()));
    for (std::size_t index = 0; index < names.size(); ++index) {
        put32(result, 4U + index * 4U, names[index]);
        put32(result, 4U + names.size() * 4U + index * 4U, offsets[index]);
    }
    result.insert(result.end(), pool.begin(), pool.end());
    return result;
}

// Independent fixed table: physical name pool is z,a,middle, while index order
// is a,middle,z. Payload boundaries/bytes are separately fixed constants.
Bytes three() {
    return table({2U, 4U, 0U}, {0U, 2U, 5U}, {'z', 0, 'a', 0, 'm', 'i', 'd', 'd', 'l', 'e', 0});
}

const Bytes three_payload{1, 2, 0, 255, 127, 66, 0, 165};

Bytes single(const std::string& name = "a") {
    Bytes pool(name.begin(), name.end());
    pool.push_back(0);
    return table({0U}, {0U}, pool);
}

Bytes wide(const std::vector<std::uint32_t>& names, const std::vector<std::uint32_t>& ignored_offsets,
           const Bytes& pool, const std::vector<std::uint64_t>& active_offsets) {
    require(names.size() == active_offsets.size(), "wide test arrays have equal length");
    auto result = table(names, ignored_offsets, pool);
    while ((result.size() & 7U) != 0U) result.push_back(0);
    const auto start = result.size();
    result.resize(start + active_offsets.size() * 8U);
    for (std::size_t index = 0; index < active_offsets.size(); ++index) put64(result, start + index * 8U, active_offsets[index]);
    result.insert(result.end(), 8U, Byte{0});
    result.insert(result.end(), {'o', 'v', 'e', 'r', '2', 'G', '!', 0});
    return result;
}

Bytes wide_single(const std::string& name = "a", std::uint32_t ignored = 0U) {
    Bytes pool(name.begin(), name.end());
    pool.push_back(0);
    return wide({0U}, {ignored}, pool, {0U});
}

void entry_is(const Entry& entry, const std::string& name, std::uint64_t offset, std::uint64_t length) {
    require(entry.name == name && entry.offset == offset && entry.length == length, "exact owned entry metadata");
}

void index_failure(const nnn::archive::IndexResult& result, ErrorCode code) {
    require(!static_cast<bool>(result.status) && result.status.code == code, "typed index failure");
    require(result.index.entries().empty() && result.index.gpk_size() == 0U, "failed parse returns no partial index");
}

void entry_failure(const nnn::archive::EntryResult& result, ErrorCode code) {
    require(!static_cast<bool>(result.status) && result.status.code == code, "typed entry failure");
    require(result.entry.name.empty() && result.entry.offset == 0U && result.entry.length == 0U, "failed lookup returns no partial entry");
}

void member_failure(const nnn::archive::MemberResult& result, ErrorCode code) {
    require(!static_cast<bool>(result.status) && result.status.code == code, "typed member failure");
    require(result.entry.name.empty() && result.entry.offset == 0U && result.entry.length == 0U && result.bytes.empty(),
            "failed read returns neither partial entry nor bytes");
}

Bytes read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "open self-authored fixture");
    return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void write(const std::filesystem::path& path, const Bytes& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(file), "create own test data file");
    if (!bytes.empty()) file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    require(static_cast<bool>(file), "write complete own test data file");
}

std::filesystem::path work_file(const char* name) {
    const std::filesystem::path root(NNN_ARCHIVE_TEST_WORK_DIR);
    std::filesystem::create_directories(root);
    return root / name;
}

void erase(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

Index valid_three() {
    auto result = nnn::archive::parse_gtb(three(), 8U);
    require(static_cast<bool>(result.status), "fixed three-entry table parses");
    return result.index;
}

} // namespace

int main() {
    Tests tests;
    for (const auto& fixture : dw006_fixture::fixtures) {
        tests.run(std::string("fixed fixture ") + fixture.name, [&] {
            const auto directory = std::filesystem::path(NNN_ARCHIVE_FIXTURE_DIR);
            const auto parsed = nnn::archive::parse_gtb(read(directory / fixture.gtb_filename), fixture.gpk_size);
            require(static_cast<bool>(parsed.status), "fixed fixture parse succeeds");
            require(parsed.index.gpk_size() == fixture.gpk_size && parsed.index.entries().size() == fixture.entries.size(), "fixture dimensions");
            require(parsed.index.format() == (fixture.extended64 ? nnn::archive::IndexFormat::Extended64 : nnn::archive::IndexFormat::Classic32), "explicit fixture index format");
            Index loaded_index = parsed.index;
            if (!fixture.virtual_gpk) {
                const auto loaded = nnn::archive::load_index((directory / fixture.gtb_filename).string(), (directory / fixture.gpk_filename).string());
                require(static_cast<bool>(loaded.status), "fixed fixture file adapter succeeds");
                loaded_index = loaded.index;
            }
            for (std::size_t index = 0; index < fixture.entries.size(); ++index) {
                const auto& expected = fixture.entries[index];
                entry_is(parsed.index.entries()[index], expected.name, expected.offset, expected.length);
                const auto found = nnn::archive::find_entry(parsed.index, expected.name);
                require(static_cast<bool>(found.status), "fixture exact lookup succeeds");
                entry_is(found.entry, expected.name, expected.offset, expected.length);
                if (!fixture.virtual_gpk) {
                    const auto member = nnn::archive::read_member((directory / fixture.gpk_filename).string(), loaded_index, expected.name);
                    require(static_cast<bool>(member.status), "fixture member read succeeds");
                    entry_is(member.entry, expected.name, expected.offset, expected.length);
                    require(member.bytes == expected.bytes, "fixture exact manually fixed member bytes");
                }
            }
        });
    }

    tests.run("first middle and last metadata use fixed offsets and EOF", [] {
        const auto index = valid_three();
        entry_is(index.entries().at(0), "a", 0U, 2U);
        entry_is(index.entries().at(1), "middle", 2U, 3U);
        entry_is(index.entries().at(2), "z", 5U, 3U);
    });
    tests.run("single last member length derives from known GPK EOF", [] {
        const auto result = nnn::archive::parse_gtb(single("solo"), 7U);
        require(static_cast<bool>(result.status), "single table succeeds");
        entry_is(result.index.entries().at(0), "solo", 0U, 7U);
    });
    tests.run("name offsets may be nonmonotonic with exact interval coverage", [] {
        const auto result = nnn::archive::parse_gtb(three(), 8U);
        require(static_cast<bool>(result.status), "physical name order differs from lookup order");
        entry_is(result.index.entries().at(2), "z", 5U, 3U);
    });
    tests.run("empty table is exactly four bytes with empty GPK", [] {
        const auto result = nnn::archive::parse_gtb(Bytes{0, 0, 0, 0}, 0U);
        require(static_cast<bool>(result.status) && result.index.entries().empty() && result.index.gpk_size() == 0U, "valid empty archive");
        index_failure(nnn::archive::parse_gtb(Bytes{0, 0, 0, 0}, 1U), ErrorCode::InvalidFormat);
        index_failure(nnn::archive::parse_gtb(Bytes{0, 0, 0, 0, 0}, 0U), ErrorCode::InvalidFormat);
    });
    tests.run("default empty index has member-not-found without touching disk", [] {
        const Index index;
        entry_failure(nnn::archive::find_entry(index, "a"), ErrorCode::MemberNotFound);
        member_failure(nnn::archive::read_member(work_file("default-never-created.gpk").string(), index, "a"), ErrorCode::MemberNotFound);
    });
    tests.run("index and entry result own name bytes independent of input", [] {
        auto input = three();
        auto parsed = nnn::archive::parse_gtb(input, 8U);
        require(static_cast<bool>(parsed.status), "parse ownership seed");
        std::fill(input.begin(), input.end(), Byte{255});
        Index copied = parsed.index;
        auto found = nnn::archive::find_entry(copied, "middle");
        require(static_cast<bool>(found.status), "copied index retained name");
        found.entry.name.assign("changed result");
        entry_is(nnn::archive::find_entry(copied, "middle").entry, "middle", 2U, 3U);
    });
    tests.run("lookup is exact and does not fold case or basename", [] {
        const auto index = valid_three();
        entry_failure(nnn::archive::find_entry(index, "A"), ErrorCode::MemberNotFound);
        entry_failure(nnn::archive::find_entry(index, "dir/a"), ErrorCode::MemberNotFound);
        entry_failure(nnn::archive::find_entry(index, "a.dwq"), ErrorCode::MemberNotFound);
        entry_failure(nnn::archive::find_entry(index, "middl"), ErrorCode::MemberNotFound);
        entry_failure(nnn::archive::find_entry(index, "middle-more"), ErrorCode::MemberNotFound);
    });
    tests.run("query errors distinguish empty NUL and overlong raw key", [] {
        const auto index = valid_three();
        entry_failure(nnn::archive::find_entry(index, ""), ErrorCode::MemberNotFound);
        entry_failure(nnn::archive::find_entry(index, std::string("a\0hidden", 8)), ErrorCode::InvalidFormat);
        entry_failure(nnn::archive::find_entry(index, std::string(nnn::archive::max_name_bytes + 1U, 'a')), ErrorCode::LimitExceeded);
    });
    tests.run("opaque high bytes and path-like names remain exact keys", [] {
        const std::string high("\x81\x40", 2);
        auto raw = table({0U, 2U}, {0U, 1U}, {'a', 0, 0x81, 0x40, 0});
        const auto index = nnn::archive::parse_gtb(raw, 3U);
        require(static_cast<bool>(index.status), "unsigned raw-byte order");
        entry_is(nnn::archive::find_entry(index.index, high).entry, high, 1U, 2U);
        const auto opaque = nnn::archive::parse_gtb(single("../logical/key"), 1U);
        require(static_cast<bool>(opaque.status), "slashes accepted as opaque key bytes");
        entry_is(nnn::archive::find_entry(opaque.index, "../logical/key").entry, "../logical/key", 0U, 1U);
    });
    tests.run("every truncated prefix of a fixed table fails without partial index", [] {
        const auto input = three();
        for (std::size_t size = 0; size < input.size(); ++size) {
            index_failure(nnn::archive::parse_gtb(Bytes(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(size)), 8U), ErrorCode::InvalidFormat);
        }
    });
    tests.run("GTB byte limit precedes untrusted table allocation", [] {
        index_failure(nnn::archive::parse_gtb(Bytes(nnn::archive::max_gtb_bytes + 1U, 0), 0U), ErrorCode::LimitExceeded);
    });
    tests.run("count limit and top-bit family are distinct", [] {
        Bytes input(4U, 0);
        put32(input, 0, static_cast<std::uint32_t>(nnn::archive::max_entries + 1U));
        index_failure(nnn::archive::parse_gtb(input, 1U), ErrorCode::LimitExceeded);
        put32(input, 0, 0x80000000U);
        index_failure(nnn::archive::parse_gtb(input, 1U), ErrorCode::UnsupportedFormat);
    });
    tests.run("declared count cannot escape short offset arrays", [] {
        Bytes input(4U, 0);
        put32(input, 0, 65536U);
        index_failure(nnn::archive::parse_gtb(input, 1U), ErrorCode::InvalidFormat);
        put32(input, 0, 2U);
        input.resize(15U);
        index_failure(nnn::archive::parse_gtb(input, 1U), ErrorCode::InvalidFormat);
    });
    tests.run("name-offset top bit is not silently masked", [] {
        auto input = single();
        put32(input, 4U, 0x80000000U);
        index_failure(nnn::archive::parse_gtb(input, 1U), ErrorCode::UnsupportedFormat);
    });
    tests.run("classic data-offset top bit is explicitly unsupported", [] {
        auto input = single();
        put32(input, 8U, 0x80000000U);
        index_failure(nnn::archive::parse_gtb(input, 1U), ErrorCode::UnsupportedFormat);
    });
    tests.run("name offsets must point inside the name area", [] {
        for (const std::uint32_t offset : {2U, 100U, 0x7fffffffU}) {
            auto input = single();
            put32(input, 4U, offset);
            index_failure(nnn::archive::parse_gtb(input, 1U), ErrorCode::InvalidFormat);
        }
    });
    tests.run("NUL strings must be nonempty and terminate inside the area", [] {
        index_failure(nnn::archive::parse_gtb(table({0U}, {0U}, {0}), 1U), ErrorCode::InvalidFormat);
        index_failure(nnn::archive::parse_gtb(table({0U}, {0U}, {'a'}), 1U), ErrorCode::InvalidFormat);
    });
    tests.run("suffix or interior name offsets cannot alias a larger string", [] {
        index_failure(nnn::archive::parse_gtb(table({1U}, {0U}, {'b', 'a', 0}), 1U), ErrorCode::InvalidFormat);
        index_failure(nnn::archive::parse_gtb(table({1U, 0U}, {0U, 1U}, {'b', 'a', 0}), 2U), ErrorCode::InvalidFormat);
    });
    tests.run("duplicate name references and duplicate names are both invalid", [] {
        index_failure(nnn::archive::parse_gtb(table({0U, 0U}, {0U, 1U}, {'a', 0}), 2U), ErrorCode::InvalidFormat);
        index_failure(nnn::archive::parse_gtb(table({0U, 2U}, {0U, 1U}, {'a', 0, 'a', 0}), 2U), ErrorCode::InvalidFormat);
    });
    tests.run("lookup table names must increase in raw strcmp order", [] {
        index_failure(nnn::archive::parse_gtb(table({2U, 0U}, {0U, 1U}, {'a', 0, 'b', 0}), 2U), ErrorCode::InvalidFormat);
        index_failure(nnn::archive::parse_gtb(table({0U, 3U}, {0U, 1U}, {0x81, 0x40, 0, 'z', 0}), 2U), ErrorCode::InvalidFormat);
    });
    tests.run("name length 255 is accepted and 256 is bounded", [] {
        const auto accepted = nnn::archive::parse_gtb(single(std::string(255U, 'a')), 1U);
        require(static_cast<bool>(accepted.status), "maximum name succeeds");
        entry_is(accepted.index.entries().at(0), std::string(255U, 'a'), 0U, 1U);
        index_failure(nnn::archive::parse_gtb(single(std::string(256U, 'a')), 1U), ErrorCode::LimitExceeded);
    });
    tests.run("classic name pool has no unreferenced gap padding or tail", [] {
        index_failure(nnn::archive::parse_gtb(table({1U}, {0U}, {0, 'a', 0}), 1U), ErrorCode::InvalidFormat);
        index_failure(nnn::archive::parse_gtb(table({0U, 3U}, {0U, 1U}, {'a', 0, 0, 'b', 0}), 2U), ErrorCode::InvalidFormat);
        auto trailing = single();
        trailing.push_back(0);
        index_failure(nnn::archive::parse_gtb(trailing, 1U), ErrorCode::InvalidFormat);
    });
    tests.run("classic data begins at zero and every member is nonempty", [] {
        auto first = single();
        put32(first, 8U, 1U);
        index_failure(nnn::archive::parse_gtb(first, 2U), ErrorCode::InvalidFormat);
        index_failure(nnn::archive::parse_gtb(table({0U, 2U}, {0U, 0U}, {'a', 0, 'b', 0}), 2U), ErrorCode::InvalidFormat);
        index_failure(nnn::archive::parse_gtb(single(), 0U), ErrorCode::InvalidFormat);
    });
    tests.run("data offsets strictly increase and remain below EOF", [] {
        index_failure(nnn::archive::parse_gtb(table({0U, 2U, 4U}, {0U, 3U, 2U}, {'a', 0, 'b', 0, 'c', 0}), 5U), ErrorCode::InvalidFormat);
        index_failure(nnn::archive::parse_gtb(table({0U, 2U}, {0U, 2U}, {'a', 0, 'b', 0}), 2U), ErrorCode::InvalidFormat);
        index_failure(nnn::archive::parse_gtb(table({0U, 2U}, {0U, 3U}, {'a', 0, 'b', 0}), 2U), ErrorCode::InvalidFormat);
    });
    tests.run("large metadata member is indexed without allocating its bytes", [] {
        const auto result = nnn::archive::parse_gtb(single(), static_cast<std::uint64_t>(nnn::archive::max_member_bytes) + 1U);
        require(static_cast<bool>(result.status), "metadata may exceed per-read limit");
        entry_is(result.index.entries().at(0), "a", 0U, static_cast<std::uint64_t>(nnn::archive::max_member_bytes) + 1U);
    });
    tests.run("classic GPK domain ends at signed32 maximum", [] {
        const auto maximum = nnn::archive::parse_gtb(single(), nnn::archive::max_classic_gpk_bytes);
        require(static_cast<bool>(maximum.status), "maximum classic metadata size");
        entry_is(maximum.index.entries().at(0), "a", 0U, 0x7fffffffULL);
        index_failure(nnn::archive::parse_gtb(single(), 0x80000000ULL), ErrorCode::UnsupportedFormat);
    });
    tests.run("recognized wide table overrides every legacy offset bit", [] {
        const auto input = wide({0U, 2U}, {0xffffffffU, 0x80000001U}, {'a', 0, 'b', 0}, {0U, 3U});
        const auto parsed = nnn::archive::parse_gtb(input, 7U);
        require(static_cast<bool>(parsed.status) && parsed.index.format() == nnn::archive::IndexFormat::Extended64, "recognized wide format");
        entry_is(parsed.index.entries().at(0), "a", 0U, 3U);
        entry_is(parsed.index.entries().at(1), "b", 3U, 4U);
    });
    tests.run("wide virtual offsets above 2GiB remain full-width metadata", [] {
        const auto input = wide({0U, 6U}, {0x12345678U, 0xffffffffU}, {'l', 'a', 'r', 'g', 'e', 0, 't', 'a', 'i', 'l', 0}, {0U, 0x80000005ULL});
        const auto parsed = nnn::archive::parse_gtb(input, 0x80000010ULL);
        require(static_cast<bool>(parsed.status), "wide metadata above 2GiB parses without payload allocation");
        entry_is(parsed.index.entries().at(0), "large", 0U, 0x80000005ULL);
        entry_is(nnn::archive::find_entry(parsed.index, "tail").entry, "tail", 0x80000005ULL, 11U);
    });
    tests.run("wide signed64 maximum is bounded without overflow", [] {
        const auto maximum = nnn::archive::max_gpk_bytes;
        const auto input = wide({0U, 2U}, {0U, 0U}, {'a', 0, 'b', 0}, {0U, maximum - 1U});
        const auto parsed = nnn::archive::parse_gtb(input, maximum);
        require(static_cast<bool>(parsed.status), "signed64 maximum metadata succeeds");
        entry_is(parsed.index.entries().at(1), "b", maximum - 1U, 1U);
        index_failure(nnn::archive::parse_gtb(input, maximum + 1U), ErrorCode::LimitExceeded);
        auto too_wide = input;
        put64(too_wide, too_wide.size() - 24U, maximum + 1U);
        index_failure(nnn::archive::parse_gtb(too_wide, maximum), ErrorCode::LimitExceeded);
        index_failure(nnn::archive::parse_gtb(input, ~std::uint64_t{0}), ErrorCode::LimitExceeded);
    });
    tests.run("wide names and zero alignment padding cover all modulo8 cases", [] {
        for (std::size_t length = 1U; length <= 8U; ++length) {
            const std::string name(length, 'q');
            const auto input = wide_single(name, 0xfedcba98U);
            const auto parsed = nnn::archive::parse_gtb(input, 1U);
            require(static_cast<bool>(parsed.status), "all canonical 0..7 padding counts accepted");
            entry_is(parsed.index.entries().at(0), name, 0U, 1U);
        }
    });
    tests.run("wide empty archive is exactly aligned count and recognized footer", [] {
        const auto input = wide({}, {}, {}, {});
        require(input.size() == 24U, "fixed wide empty encoding is 24 bytes");
        const auto parsed = nnn::archive::parse_gtb(input, 0U);
        require(static_cast<bool>(parsed.status) && parsed.index.entries().empty() && parsed.index.format() == nnn::archive::IndexFormat::Extended64, "wide empty success");
        index_failure(nnn::archive::parse_gtb(input, 1U), ErrorCode::InvalidFormat);
        auto bad = input; bad[4] = 1U;
        index_failure(nnn::archive::parse_gtb(bad, 0U), ErrorCode::InvalidFormat);
    });
    tests.run("wide u64 table cannot overlap fixed tables or lose alignment", [] {
        auto overlap = wide({}, {}, {}, {});
        put32(overlap, 0U, 2U);
        index_failure(nnn::archive::parse_gtb(overlap, 2U), ErrorCode::InvalidFormat);
        auto unaligned = wide_single();
        unaligned.insert(unaligned.begin() + 16, Byte{0});
        index_failure(nnn::archive::parse_gtb(unaligned, 1U), ErrorCode::InvalidFormat);
    });
    tests.run("wide padding is zero and exactly the required alignment", [] {
        auto bad = wide_single();
        bad[14] = 1U;
        index_failure(nnn::archive::parse_gtb(bad, 1U), ErrorCode::InvalidFormat);
        auto overpadded = wide_single();
        overpadded.insert(overpadded.begin() + 16, 8U, Byte{0});
        index_failure(nnn::archive::parse_gtb(overpadded, 1U), ErrorCode::InvalidFormat);
    });
    tests.run("wide names retain exact unique complete interval rules", [] {
        index_failure(nnn::archive::parse_gtb(wide({0U, 0U}, {0U, 0U}, {'a', 0}, {0U, 1U}), 2U), ErrorCode::InvalidFormat);
        index_failure(nnn::archive::parse_gtb(wide({0U}, {0U}, {'a', 0, 'b', 0}, {0U}), 1U), ErrorCode::InvalidFormat);
        index_failure(nnn::archive::parse_gtb(wide({1U}, {0U}, {'b', 'a', 0}, {0U}), 1U), ErrorCode::InvalidFormat);
    });
    tests.run("wide active offsets are strict bounded nonempty intervals", [] {
        for (const auto& offsets : std::vector<std::vector<std::uint64_t>>{{1U, 2U}, {0U, 0U}, {0U, 3U}, {0U, 4U}}) {
            index_failure(nnn::archive::parse_gtb(wide({0U, 2U}, {0U, 0U}, {'a', 0, 'b', 0}, offsets), 3U), ErrorCode::InvalidFormat);
        }
        index_failure(nnn::archive::parse_gtb(wide({0U, 2U, 4U}, {0U, 0U, 0U}, {'a', 0, 'b', 0, 'c', 0}, {0U, 3U, 2U}), 5U), ErrorCode::InvalidFormat);
    });
    tests.run("wide name-offset top bit remains unsupported", [] {
        auto input = wide_single();
        put32(input, 4U, 0x80000000U);
        index_failure(nnn::archive::parse_gtb(input, 1U), ErrorCode::UnsupportedFormat);
    });
    tests.run("partial altered or fake extension footer is not guessed", [] {
        const auto input = wide_single();
        for (std::size_t removed = 1U; removed <= 16U; ++removed) {
            index_failure(nnn::archive::parse_gtb(Bytes(input.begin(), input.end() - static_cast<std::ptrdiff_t>(removed)), 1U), ErrorCode::InvalidFormat);
        }
        auto bad_magic = input; bad_magic.back() = 1U;
        index_failure(nnn::archive::parse_gtb(bad_magic, 1U), ErrorCode::InvalidFormat);
        auto nonzero_prefix = input; nonzero_prefix[nonzero_prefix.size() - 16U] = 1U;
        index_failure(nnn::archive::parse_gtb(nonzero_prefix, 1U), ErrorCode::InvalidFormat);
        auto extra_tail = input; extra_tail.push_back(0U);
        index_failure(nnn::archive::parse_gtb(extra_tail, 1U), ErrorCode::InvalidFormat);
    });
    tests.run("member read returns exact first middle and last bytes only", [] {
        const auto path = work_file("fixed-read.gpk");
        write(path, three_payload);
        const auto index = valid_three();
        for (const auto& expected : std::vector<std::pair<std::string, Bytes>>{{"a", {1, 2}}, {"middle", {0, 255, 127}}, {"z", {66, 0, 165}}}) {
            const auto result = nnn::archive::read_member(path.string(), index, expected.first);
            require(static_cast<bool>(result.status) && result.bytes == expected.second, "exact bounded member bytes");
        }
        erase(path);
    });
    tests.run("file adapters reread same-size contents and results own bytes", [] {
        const auto gtb = work_file("fresh.gtb");
        const auto gpk = work_file("fresh.gpk");
        write(gtb, single("key")); write(gpk, {1, 2, 3});
        const auto index = nnn::archive::load_index(gtb.string(), gpk.string());
        require(static_cast<bool>(index.status), "load fixed index");
        const auto old = nnn::archive::read_member(gpk.string(), index.index, "key");
        require(static_cast<bool>(old.status) && old.bytes == Bytes({1, 2, 3}), "first contents");
        write(gpk, {9, 8, 7});
        const auto fresh = nnn::archive::read_member(gpk.string(), index.index, "key");
        require(static_cast<bool>(fresh.status) && fresh.bytes == Bytes({9, 8, 7}), "fresh same-size contents");
        require(old.bytes == Bytes({1, 2, 3}), "retained first result owns bytes");
        write(gtb, single("new"));
        const auto fresh_index = nnn::archive::load_index(gtb.string(), gpk.string());
        require(static_cast<bool>(fresh_index.status), "GTB reread");
        entry_failure(nnn::archive::find_entry(fresh_index.index, "key"), ErrorCode::MemberNotFound);
        require(static_cast<bool>(nnn::archive::find_entry(index.index, "key").status), "old index owns old name");
        erase(gtb); erase(gpk);
    });
    tests.run("member length changes are ChangedFile without partial bytes", [] {
        const auto path = work_file("changed.gpk");
        const auto parsed = nnn::archive::parse_gtb(single(), 3U);
        require(static_cast<bool>(parsed.status), "changed-file seed parses");
        for (const Bytes& contents : {Bytes{1, 2}, Bytes{1, 2, 3, 4}}) {
            write(path, contents);
            member_failure(nnn::archive::read_member(path.string(), parsed.index, "a"), ErrorCode::ChangedFile);
        }
        erase(path);
    });
    tests.run("file missing directory and missing member are distinct", [] {
        const auto missing_gtb = work_file("missing.gtb");
        const auto missing_gpk = work_file("missing.gpk");
        erase(missing_gtb); erase(missing_gpk);
        const auto gtb = work_file("known.gtb");
        const auto gpk = work_file("known.gpk");
        write(gtb, single()); write(gpk, {1, 2, 3});
        index_failure(nnn::archive::load_index(missing_gtb.string(), gpk.string()), ErrorCode::NotFound);
        index_failure(nnn::archive::load_index(gtb.string(), missing_gpk.string()), ErrorCode::NotFound);
        index_failure(nnn::archive::load_index(std::filesystem::path(NNN_ARCHIVE_TEST_WORK_DIR).string(), gpk.string()), ErrorCode::ReadFailed);
        index_failure(nnn::archive::load_index(gtb.string(), std::filesystem::path(NNN_ARCHIVE_TEST_WORK_DIR).string()), ErrorCode::ReadFailed);
        const auto parsed = nnn::archive::parse_gtb(single(), 3U);
        member_failure(nnn::archive::read_member(missing_gpk.string(), parsed.index, "a"), ErrorCode::NotFound);
        member_failure(nnn::archive::read_member(gpk.string(), parsed.index, "absent"), ErrorCode::MemberNotFound);
        member_failure(nnn::archive::read_member(std::filesystem::path(NNN_ARCHIVE_TEST_WORK_DIR).string(), parsed.index, "a"), ErrorCode::ReadFailed);
        erase(gtb); erase(gpk);
    });
    tests.run("bad GTB file never produces a usable partial index", [] {
        const auto gtb = work_file("bad.gtb");
        const auto gpk = work_file("bad-pair.gpk");
        write(gtb, {1, 2, 3}); write(gpk, {4});
        index_failure(nnn::archive::load_index(gtb.string(), gpk.string()), ErrorCode::InvalidFormat);
        erase(gtb); erase(gpk);
    });
    tests.run("GTB ordinary-file limit is checked before allocating table bytes", [] {
        const auto gtb = work_file("gtb-over-limit.gtb");
        const auto gpk = work_file("gtb-over-limit.gpk");
        {
            std::ofstream file(gtb, std::ios::binary | std::ios::trunc);
            require(static_cast<bool>(file), "create GTB size-limit file");
            file.seekp(static_cast<std::streamoff>(nnn::archive::max_gtb_bytes));
            file.put('\0');
            require(static_cast<bool>(file), "write sparse GTB size-limit file");
        }
        write(gpk, {});
        index_failure(nnn::archive::load_index(gtb.string(), gpk.string()), ErrorCode::LimitExceeded);
        erase(gtb); erase(gpk);
    });
    tests.run("oversized member read rejects before returning any payload", [] {
        const auto path = work_file("member-over-limit.gpk");
        const auto size = static_cast<std::uint64_t>(nnn::archive::max_member_bytes) + 1U;
        const auto parsed = nnn::archive::parse_gtb(single(), size);
        require(static_cast<bool>(parsed.status), "large member metadata parses");
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            require(static_cast<bool>(file), "create member size-limit file");
            file.seekp(static_cast<std::streamoff>(size - 1U)); file.put('\0');
            require(static_cast<bool>(file), "write sparse member size-limit file");
        }
        member_failure(nnn::archive::read_member(path.string(), parsed.index, "a"), ErrorCode::LimitExceeded);
        erase(path);
    });
    tests.run("path-like member name is never resolved as a filesystem path", [] {
        const auto path = work_file("opaque-key.gpk");
        write(path, {42});
        const auto parsed = nnn::archive::parse_gtb(single("../../outside"), 1U);
        require(static_cast<bool>(parsed.status), "opaque path-like key parses");
        const auto result = nnn::archive::read_member(path.string(), parsed.index, "../../outside");
        require(static_cast<bool>(result.status) && result.bytes == Bytes({42}), "member is read from supplied GPK only");
        erase(path);
    });
    tests.run("status bool and every public error code have stable names", [] {
        require(static_cast<bool>(nnn::archive::Status{}), "default status is Ok");
        for (const auto& item : std::vector<std::pair<ErrorCode, std::string>>{
                 {ErrorCode::Ok, "Ok"}, {ErrorCode::NotFound, "NotFound"}, {ErrorCode::ReadFailed, "ReadFailed"},
                 {ErrorCode::UnsupportedFormat, "UnsupportedFormat"}, {ErrorCode::InvalidFormat, "InvalidFormat"},
                 {ErrorCode::LimitExceeded, "LimitExceeded"}, {ErrorCode::MemberNotFound, "MemberNotFound"}, {ErrorCode::ChangedFile, "ChangedFile"}}) {
            require(nnn::archive::error_name(item.first) == item.second, "public error name");
            nnn::archive::Status status;
            status.code = item.first;
            require(static_cast<bool>(status) == (item.first == ErrorCode::Ok), "status truth is only Ok");
        }
    });

    std::cout << "ARCHIVE RESULT " << tests.passed << " passed, " << tests.failed << " failed\n";
    return tests.failed == 0 ? 0 : 1;
}
