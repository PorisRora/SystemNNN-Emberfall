#include "nnn/script.hpp"

#include <utility>

namespace nnn {
namespace {

bool whitespace(Byte byte) noexcept {
    return byte == static_cast<Byte>(' ') || byte == static_cast<Byte>('\t')
        || byte == static_cast<Byte>('\r') || byte == static_cast<Byte>('\n');
}

bool stop_f5_name(const std::string& name) noexcept {
    constexpr char target[] = "stopf5";
    if (name.size() != sizeof(target) - 1U) return false;
    for (std::size_t index = 0; index < name.size(); ++index) {
        auto byte = static_cast<unsigned char>(name[index]);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) byte += 'a' - 'A';
        if (byte != static_cast<unsigned char>(target[index])) return false;
    }
    return true;
}

FunctionRegistryResult failure(ErrorCode code, const char* detail) {
    return {{code, no_word, detail}, {}};
}

} // namespace

FunctionRegistry& FunctionRegistry::operator=(const FunctionRegistry& other) {
    if (this != &other) {
        FunctionRegistry copy(other);
        *this = std::move(copy);
    }
    return *this;
}
FunctionRegistry::FunctionRegistry(FunctionRegistry&& other) noexcept
    : names_(std::move(other.names_)), stop_f5_id_(other.stop_f5_id_), valid_(other.valid_) {
    other.names_.clear();
    other.stop_f5_id_ = no_word;
    other.valid_ = false;
}
FunctionRegistry& FunctionRegistry::operator=(FunctionRegistry&& other) noexcept {
    if (this != &other) {
        names_ = std::move(other.names_);
        stop_f5_id_ = other.stop_f5_id_;
        valid_ = other.valid_;
        other.names_.clear();
        other.stop_f5_id_ = no_word;
        other.valid_ = false;
    }
    return *this;
}
bool FunctionRegistry::IsValid() const noexcept { return valid_; }
std::size_t FunctionRegistry::Size() const noexcept { return names_.size(); }
bool FunctionRegistry::GetName(std::size_t id, std::string& out) const {
    if (!valid_ || id >= names_.size()) return false;
    out = names_[id];
    return true;
}
Word FunctionRegistry::StopF5Id() const noexcept { return valid_ ? stop_f5_id_ : no_word; }

FunctionRegistryResult parse_function_registry(const std::vector<Byte>& bytes) {
    try {
        if (bytes.size() > max_function_config_bytes) return failure(ErrorCode::UnsupportedFormat, "Function configuration exceeds byte limit");
        for (const auto byte : bytes) {
            if (byte == 0U) return failure(ErrorCode::InvalidFormat, "NUL in function configuration");
        }
        FunctionRegistry candidate;
        std::size_t cursor = 0;
        std::size_t tokens = 0;
        while (cursor < bytes.size()) {
            // Exact bounded CNameList order: one comma, then whitespace.
            // A comma encountered after whitespace yields an empty token.
            if (bytes[cursor] == static_cast<Byte>(',')) ++cursor;
            while (cursor < bytes.size() && whitespace(bytes[cursor])) ++cursor;
            if (cursor == bytes.size()) break;
            const auto begin = cursor;
            while (cursor < bytes.size() && !whitespace(bytes[cursor]) && bytes[cursor] != static_cast<Byte>(',')) ++cursor;
            const auto length = cursor - begin;
            if (length > max_function_token_bytes || tokens >= max_function_registry_entries * 2U) {
                return failure(ErrorCode::UnsupportedFormat, "Function configuration exceeds token limits");
            }
            if (tokens % 2U == 0U) {
                candidate.names_.emplace_back(reinterpret_cast<const char*>(bytes.data() + begin), length);
                if (candidate.stop_f5_id_ == no_word && stop_f5_name(candidate.names_.back())) {
                    candidate.stop_f5_id_ = static_cast<Word>(candidate.names_.size() - 1U);
                }
            }
            ++tokens;
        }
        if (tokens % 2U != 0U) return failure(ErrorCode::InvalidFormat, "Function configuration has an unpaired token");
        candidate.valid_ = true;
        FunctionRegistryResult result;
        result.registry = std::move(candidate);
        return result;
    } catch (...) {
        return failure(ErrorCode::ReadFailed, "Function configuration preparation failed");
    }
}

} // namespace nnn
