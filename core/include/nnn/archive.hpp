#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace nnn::archive {

using Byte = std::uint8_t;
inline constexpr std::size_t no_byte = std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t max_gtb_bytes = 16U * 1024U * 1024U;
inline constexpr std::size_t max_entries = 65536U;
inline constexpr std::size_t max_name_bytes = 255U;
inline constexpr std::size_t max_member_bytes = 64U * 1024U * 1024U;
inline constexpr std::uint64_t max_classic_gpk_bytes = 0x7fffffffULL;
inline constexpr std::uint64_t max_gpk_bytes = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

enum class IndexFormat { Classic32, Extended64 };

enum class ErrorCode {
    Ok,
    NotFound,
    ReadFailed,
    UnsupportedFormat,
    InvalidFormat,
    LimitExceeded,
    MemberNotFound,
    ChangedFile,
};

struct Status {
    ErrorCode code = ErrorCode::Ok;
    std::size_t byte_offset = no_byte;
    std::string detail;
    explicit operator bool() const noexcept { return code == ErrorCode::Ok; }
};

const char* error_name(ErrorCode code) noexcept;

struct Entry {
    // Original non-NUL name bytes, used only as an opaque case-sensitive key.
    std::string name;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct IndexResult;

class Index {
public:
    Index() = default;
    const std::vector<Entry>& entries() const noexcept { return entries_; }
    std::uint64_t gpk_size() const noexcept { return gpk_size_; }
    IndexFormat format() const noexcept { return format_; }

private:
    std::vector<Entry> entries_;
    std::uint64_t gpk_size_ = 0;
    IndexFormat format_ = IndexFormat::Classic32;
    friend IndexResult parse_gtb(const std::vector<Byte>&, std::uint64_t);
};

struct IndexResult {
    Status status;
    Index index;
};

struct EntryResult {
    Status status;
    Entry entry;
};

struct MemberResult {
    Status status;
    Entry entry;
    std::vector<Byte> bytes;
};

// Parse the verified classic32 or explicitly marked extended64 GTB profile
// against a known GPK size. Extended64 offsets never pass through signed32.
// Index owns names and entries. Failed results contain a default empty Index.
IndexResult parse_gtb(const std::vector<Byte>& bytes, std::uint64_t known_gpk_size);

// Exact original-byte lookup, without case folding, basename or encoding rules.
EntryResult find_entry(const Index& index, const std::string& raw_name);

// Ordinary-file adapters. Names are never turned into filesystem paths.
// GPK is sized but never loaded wholesale by load_index.
IndexResult load_index(const std::string& gtb_filename, const std::string& gpk_filename);

// Read just the validated member interval, subject to max_member_bytes.
// Checks GPK length before/after the read and reports short reads/length changes.
// Length checks do not promise detection of same-size content replacement.
// Every failed result has an empty Entry and no partial member bytes.
MemberResult read_member(const std::string& gpk_filename, const Index& index, const std::string& raw_name);

} // namespace nnn::archive
