#include "nnn/archive.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <utility>

namespace nnn::archive {
namespace {

Status status(ErrorCode code, std::size_t offset, const char* detail) {
    return {code, offset, detail};
}

IndexResult index_failure(ErrorCode code, std::size_t offset, const char* detail) {
    return {status(code, offset, detail), {}};
}

MemberResult member_failure(ErrorCode code, std::size_t offset, const char* detail) {
    return {status(code, offset, detail), {}, {}};
}

std::uint32_t u32(const std::vector<Byte>& bytes, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t u64(const std::vector<Byte>& bytes, std::size_t offset) noexcept {
    return static_cast<std::uint64_t>(u32(bytes, offset)) |
           (static_cast<std::uint64_t>(u32(bytes, offset + 4U)) << 32U);
}

bool checked_add(std::size_t left, std::size_t right, std::size_t& value) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) return false;
    value = left + right;
    return true;
}

bool checked_multiply(std::size_t left, std::size_t right, std::size_t& value) noexcept {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) return false;
    value = left * right;
    return true;
}

int compare_bytes(const std::string& left, const std::string& right) noexcept {
    const auto common = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < common; ++index) {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        if (a != b) return a < b ? -1 : 1;
    }
    if (left.size() == right.size()) return 0;
    return left.size() < right.size() ? -1 : 1;
}

bool extended_tail(const std::vector<Byte>& bytes) noexcept {
    constexpr std::array<Byte, 16> marker{0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                                          'o', 'v', 'e', 'r', '2', 'G', '!', 0U};
    return bytes.size() >= marker.size() &&
           std::equal(marker.begin(), marker.end(), bytes.end() - static_cast<std::ptrdiff_t>(marker.size()));
}

struct NameSpan {
    std::size_t relative;
    std::size_t length;
};

Status open_file(const std::string& filename, std::ifstream& file, std::uint64_t& size) {
    namespace fs = std::filesystem;
    std::error_code error;
    const fs::path path(filename);
    const auto state = fs::status(path, error);
    if (error) {
        const bool missing = error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory;
        return status(missing ? ErrorCode::NotFound : ErrorCode::ReadFailed, no_byte, "Cannot inspect archive file");
    }
    if (!fs::exists(state)) return status(ErrorCode::NotFound, no_byte, "Archive file was not found");
    if (!fs::is_regular_file(state)) return status(ErrorCode::ReadFailed, no_byte, "Archive input must be an ordinary file");
    file.open(path, std::ios::binary | std::ios::ate);
    if (!file) return status(ErrorCode::ReadFailed, no_byte, "Cannot open archive file");
    const auto end = file.tellg();
    if (end < std::ifstream::pos_type(0)) return status(ErrorCode::ReadFailed, no_byte, "Cannot determine archive file length");
    size = static_cast<std::uint64_t>(static_cast<std::streamoff>(end));
    return {};
}

Status check_length(std::ifstream& file, std::uint64_t expected) {
    file.clear();
    file.seekg(0, std::ios::end);
    if (!file) return status(ErrorCode::ReadFailed, no_byte, "Cannot recheck archive file length");
    const auto end = file.tellg();
    if (end < std::ifstream::pos_type(0)) return status(ErrorCode::ReadFailed, no_byte, "Cannot recheck archive file length");
    if (static_cast<std::uint64_t>(static_cast<std::streamoff>(end)) != expected) {
        return status(ErrorCode::ChangedFile, no_byte, "Archive file length changed");
    }
    return {};
}

} // namespace

const char* error_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::ReadFailed: return "ReadFailed";
    case ErrorCode::UnsupportedFormat: return "UnsupportedFormat";
    case ErrorCode::InvalidFormat: return "InvalidFormat";
    case ErrorCode::LimitExceeded: return "LimitExceeded";
    case ErrorCode::MemberNotFound: return "MemberNotFound";
    case ErrorCode::ChangedFile: return "ChangedFile";
    }
    return "Unknown";
}

IndexResult parse_gtb(const std::vector<Byte>& bytes, std::uint64_t known_gpk_size) {
    try {
        if (bytes.size() > max_gtb_bytes) return index_failure(ErrorCode::LimitExceeded, no_byte, "GTB exceeds byte limit");
        if (known_gpk_size > max_gpk_bytes) return index_failure(ErrorCode::LimitExceeded, no_byte, "GPK size exceeds supported seek domain");
        if (bytes.size() < 4U) return index_failure(ErrorCode::InvalidFormat, bytes.size(), "Truncated GTB count");
        const auto raw_count = u32(bytes, 0);
        if ((raw_count & 0x80000000U) != 0U) return index_failure(ErrorCode::UnsupportedFormat, 0, "High-bit GTB count is unsupported");
        if (raw_count > max_entries) return index_failure(ErrorCode::LimitExceeded, 0, "GTB entry count exceeds limit");
        const auto count = static_cast<std::size_t>(raw_count);
        std::size_t table_bytes = 0;
        std::size_t names_begin = 0;
        if (!checked_multiply(count, 8U, table_bytes) || !checked_add(4U, table_bytes, names_begin)) {
            return index_failure(ErrorCode::LimitExceeded, 0, "GTB fixed table arithmetic overflow");
        }
        if (names_begin > bytes.size()) return index_failure(ErrorCode::InvalidFormat, bytes.size(), "Truncated GTB fixed tables");
        const bool wide = extended_tail(bytes);
        std::size_t names_end = bytes.size();
        if (wide) {
            std::size_t trailer_bytes = 0;
            if (!checked_add(table_bytes, 16U, trailer_bytes)) return index_failure(ErrorCode::LimitExceeded, 0, "GTB extension arithmetic overflow");
            if (trailer_bytes > bytes.size()) return index_failure(ErrorCode::InvalidFormat, bytes.size(), "Truncated extended offset table");
            names_end = bytes.size() - trailer_bytes;
            if (names_end < names_begin || names_end % 8U != 0U) {
                return index_failure(ErrorCode::InvalidFormat, names_end, "Extended table crosses names or is not 8-byte aligned");
            }
        } else if (known_gpk_size > max_classic_gpk_bytes) {
            return index_failure(ErrorCode::UnsupportedFormat, no_byte, "Classic32 GPK exceeds signed-31-bit offset domain");
        }
        if (count == 0U && known_gpk_size != 0U) {
            return index_failure(ErrorCode::InvalidFormat, 0, "Empty GTB cannot describe a nonempty GPK");
        }
        std::size_t minimum_names = 0;
        if (!checked_multiply(count, 2U, minimum_names)) return index_failure(ErrorCode::LimitExceeded, 0, "GTB name count arithmetic overflow");
        if (minimum_names > names_end - names_begin) {
            return index_failure(ErrorCode::InvalidFormat, names_begin, "Name pool cannot contain all nonempty names");
        }
        std::vector<NameSpan> physical_names;
        physical_names.reserve(count);
        std::size_t cursor = names_begin;
        for (std::size_t index = 0; index < count; ++index) {
            if (cursor >= names_end) return index_failure(ErrorCode::InvalidFormat, cursor, "Missing GTB name");
            const auto scan_limit = std::min(names_end - cursor, max_name_bytes + 1U);
            std::size_t length = 0;
            while (length < scan_limit && bytes[cursor + length] != 0U) ++length;
            if (length == 0U) return index_failure(ErrorCode::InvalidFormat, cursor, "GTB names must be nonempty");
            if (length == scan_limit) {
                if (scan_limit == max_name_bytes + 1U) return index_failure(ErrorCode::LimitExceeded, cursor, "GTB name exceeds byte limit");
                return index_failure(ErrorCode::InvalidFormat, cursor, "Unterminated GTB name");
            }
            physical_names.push_back({cursor - names_begin, length});
            cursor += length + 1U;
        }
        if (wide) {
            const auto padding = (8U - cursor % 8U) % 8U;
            if (names_end - cursor != padding ||
                !std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                             bytes.begin() + static_cast<std::ptrdiff_t>(names_end), [](Byte byte) { return byte == 0U; })) {
                return index_failure(ErrorCode::InvalidFormat, cursor, "Extended name pool has unexplained bytes or nonzero padding");
            }
        } else if (cursor != names_end) {
            return index_failure(ErrorCode::InvalidFormat, cursor, "Classic name pool has trailing or unreferenced bytes");
        }

        Index index;
        index.gpk_size_ = known_gpk_size;
        index.format_ = wide ? IndexFormat::Extended64 : IndexFormat::Classic32;
        index.entries_.reserve(count);
        std::vector<bool> used(count, false);
        for (std::size_t position = 0; position < count; ++position) {
            const auto name_field = 4U + position * 4U;
            const auto name_offset = u32(bytes, name_field);
            if ((name_offset & 0x80000000U) != 0U) return index_failure(ErrorCode::UnsupportedFormat, name_field, "High-bit name offset is unsupported");
            const auto found = std::lower_bound(physical_names.begin(), physical_names.end(), static_cast<std::size_t>(name_offset),
                [](const NameSpan& span, std::size_t offset) { return span.relative < offset; });
            if (found == physical_names.end() || found->relative != name_offset) {
                return index_failure(ErrorCode::InvalidFormat, name_field, "Name offset does not point to a complete string start");
            }
            const auto physical = static_cast<std::size_t>(found - physical_names.begin());
            if (used[physical]) return index_failure(ErrorCode::InvalidFormat, name_field, "Name string is referenced more than once");
            used[physical] = true;
            std::string name(reinterpret_cast<const char*>(bytes.data() + names_begin + found->relative), found->length);
            if (!index.entries_.empty() && compare_bytes(index.entries_.back().name, name) >= 0) {
                return index_failure(ErrorCode::InvalidFormat, name_field, "Names must be strictly byte-sorted and unique");
            }
            const auto data_field = 4U + count * 4U + position * 4U;
            std::uint64_t offset = 0;
            if (wide) {
                // The source reader replaces the legacy word with this value.
                // Legacy words, including bit31, are deliberately not consulted.
                offset = u64(bytes, names_end + position * 8U);
                if (offset > max_gpk_bytes) return index_failure(ErrorCode::LimitExceeded, names_end + position * 8U, "Extended offset exceeds seek domain");
            } else {
                const auto value = u32(bytes, data_field);
                if ((value & 0x80000000U) != 0U) return index_failure(ErrorCode::UnsupportedFormat, data_field, "Classic high-bit data offset is unsupported");
                offset = value;
            }
            if ((position == 0U && offset != 0U) || offset >= known_gpk_size ||
                (!index.entries_.empty() && offset <= index.entries_.back().offset)) {
                return index_failure(ErrorCode::InvalidFormat, wide ? names_end + position * 8U : data_field,
                                     "Member offsets must start at zero, increase and remain below GPK EOF");
            }
            if (!index.entries_.empty()) index.entries_.back().length = offset - index.entries_.back().offset;
            index.entries_.push_back({std::move(name), offset, 0});
        }
        if (!index.entries_.empty()) index.entries_.back().length = known_gpk_size - index.entries_.back().offset;
        return {{}, std::move(index)};
    } catch (const std::bad_alloc&) {
        return index_failure(ErrorCode::LimitExceeded, no_byte, "Cannot allocate bounded GTB index");
    } catch (const std::length_error&) {
        return index_failure(ErrorCode::LimitExceeded, no_byte, "GTB storage exceeds container limit");
    }
}

EntryResult find_entry(const Index& index, const std::string& raw_name) {
    try {
        if (raw_name.size() > max_name_bytes) return {status(ErrorCode::LimitExceeded, no_byte, "Query name exceeds byte limit"), {}};
        if (raw_name.find('\0') != std::string::npos) return {status(ErrorCode::InvalidFormat, no_byte, "Query name contains NUL"), {}};
        if (raw_name.empty()) return {status(ErrorCode::MemberNotFound, no_byte, "Archive member was not found"), {}};
        const auto& entries = index.entries();
        const auto found = std::lower_bound(entries.begin(), entries.end(), raw_name,
            [](const Entry& entry, const std::string& query) { return compare_bytes(entry.name, query) < 0; });
        if (found == entries.end() || compare_bytes(found->name, raw_name) != 0) {
            return {status(ErrorCode::MemberNotFound, no_byte, "Archive member was not found"), {}};
        }
        return {{}, *found};
    } catch (const std::bad_alloc&) {
        return {status(ErrorCode::LimitExceeded, no_byte, "Cannot copy bounded archive member metadata"), {}};
    } catch (const std::length_error&) {
        return {status(ErrorCode::LimitExceeded, no_byte, "Archive name exceeds container limit"), {}};
    }
}

IndexResult load_index(const std::string& gtb_filename, const std::string& gpk_filename) {
    try {
        std::ifstream table;
        std::uint64_t table_size = 0;
        auto checked = open_file(gtb_filename, table, table_size);
        if (!checked) return {std::move(checked), {}};
        if (table_size > max_gtb_bytes) return index_failure(ErrorCode::LimitExceeded, no_byte, "GTB file exceeds byte limit");
        std::ifstream pack;
        std::uint64_t pack_size = 0;
        checked = open_file(gpk_filename, pack, pack_size);
        if (!checked) return {std::move(checked), {}};
        if (pack_size > max_gpk_bytes) return index_failure(ErrorCode::LimitExceeded, no_byte, "GPK exceeds supported seek domain");
        std::vector<Byte> bytes(static_cast<std::size_t>(table_size));
        table.seekg(0, std::ios::beg);
        if (!table) return index_failure(ErrorCode::ReadFailed, no_byte, "Cannot seek GTB file");
        if (!bytes.empty()) {
            const auto expected = static_cast<std::streamsize>(bytes.size());
            table.read(reinterpret_cast<char*>(bytes.data()), expected);
            if (table.bad()) return index_failure(ErrorCode::ReadFailed, no_byte, "GTB read failed");
            if (table.gcount() != expected) {
                checked = check_length(table, table_size);
                if (!checked) return {std::move(checked), {}};
                return index_failure(ErrorCode::ReadFailed, no_byte, "GTB read was incomplete without a detected length change");
            }
        }
        checked = check_length(table, table_size);
        if (!checked) return {std::move(checked), {}};
        checked = check_length(pack, pack_size);
        if (!checked) return {std::move(checked), {}};
        return parse_gtb(bytes, pack_size);
    } catch (const std::bad_alloc&) {
        return index_failure(ErrorCode::LimitExceeded, no_byte, "Cannot allocate bounded GTB input");
    } catch (const std::length_error&) {
        return index_failure(ErrorCode::LimitExceeded, no_byte, "GTB input exceeds container limit");
    } catch (const std::filesystem::filesystem_error&) {
        return index_failure(ErrorCode::ReadFailed, no_byte, "Cannot inspect archive paths");
    }
}

MemberResult read_member(const std::string& gpk_filename, const Index& index, const std::string& raw_name) {
    try {
        auto found = find_entry(index, raw_name);
        if (!found.status) return {std::move(found.status), {}, {}};
        const auto& entry = found.entry;
        if (entry.length > max_member_bytes) return member_failure(ErrorCode::LimitExceeded, no_byte, "Member exceeds read byte limit");
        if (entry.offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            return member_failure(ErrorCode::LimitExceeded, no_byte, "Member offset exceeds host seek domain");
        }
        std::ifstream file;
        std::uint64_t size = 0;
        auto checked = open_file(gpk_filename, file, size);
        if (!checked) return {std::move(checked), {}, {}};
        if (size != index.gpk_size()) return member_failure(ErrorCode::ChangedFile, no_byte, "GPK length differs from validated index");
        if (entry.offset > size || entry.length > size - entry.offset) {
            return member_failure(ErrorCode::InvalidFormat, no_byte, "Member range exceeds GPK EOF");
        }
        std::vector<Byte> bytes(static_cast<std::size_t>(entry.length));
        file.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
        if (!file) return member_failure(ErrorCode::ReadFailed, no_byte, "Cannot seek archive member");
        if (!bytes.empty()) {
            const auto expected = static_cast<std::streamsize>(bytes.size());
            file.read(reinterpret_cast<char*>(bytes.data()), expected);
            if (file.bad()) return member_failure(ErrorCode::ReadFailed, no_byte, "Archive member read failed");
            if (file.gcount() != expected) {
                checked = check_length(file, size);
                if (!checked) return {std::move(checked), {}, {}};
                return member_failure(ErrorCode::ReadFailed, no_byte, "Member read was incomplete without a detected length change");
            }
        }
        checked = check_length(file, size);
        if (!checked) return {std::move(checked), {}, {}};
        return {{}, std::move(found.entry), std::move(bytes)};
    } catch (const std::bad_alloc&) {
        return member_failure(ErrorCode::LimitExceeded, no_byte, "Cannot allocate bounded member storage");
    } catch (const std::length_error&) {
        return member_failure(ErrorCode::LimitExceeded, no_byte, "Member exceeds container limit");
    } catch (const std::filesystem::filesystem_error&) {
        return member_failure(ErrorCode::ReadFailed, no_byte, "Cannot inspect archive member path");
    }
}

} // namespace nnn::archive
