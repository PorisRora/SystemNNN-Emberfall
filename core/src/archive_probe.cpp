#include "nnn/archive.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using nnn::archive::Byte;

std::string quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\') out << '\\' << character;
        else if (byte < 0x20U || byte >= 0x80U) {
            out << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<unsigned int>(byte) << std::dec;
        } else out << character;
    }
    out << '"';
    return out.str();
}

template <typename Iterator>
std::string hex(Iterator begin, Iterator end) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto current = begin; current != end; ++current) {
        out << std::setw(2) << static_cast<unsigned int>(static_cast<unsigned char>(*current));
    }
    return out.str();
}

void emit_status(const nnn::archive::Status& status) {
    std::cout << "{\"code\":" << quote(nnn::archive::error_name(status.code)) << ",\"byte_offset\":";
    if (status.byte_offset == nnn::archive::no_byte) std::cout << "null";
    else std::cout << status.byte_offset;
    std::cout << ",\"detail\":" << quote(status.detail) << '}';
}

void emit_entry(const nnn::archive::Entry& entry) {
    std::cout << "{\"name\":" << quote(entry.name) << ",\"name_hex\":"
              << quote(hex(entry.name.begin(), entry.name.end())) << ",\"offset\":" << entry.offset
              << ",\"length\":" << entry.length << '}';
}

int nibble(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

bool decode_name(const std::string& encoded, std::string& decoded) {
    if (encoded.empty() || encoded.size() % 2U != 0U || encoded.size() / 2U > nnn::archive::max_name_bytes) return false;
    for (std::size_t index = 0; index < encoded.size(); index += 2U) {
        const int high = nibble(encoded[index]);
        const int low = nibble(encoded[index + 1U]);
        if (high < 0 || low < 0) return false;
        const auto byte = static_cast<unsigned char>(high * 16 + low);
        if (byte == 0U) return false;
        decoded.push_back(static_cast<char>(byte));
    }
    return true;
}

bool has_bytes(const std::vector<Byte>& bytes, std::size_t offset, const Byte* expected, std::size_t count) {
    if (offset > bytes.size() || count > bytes.size() - offset) return false;
    return std::equal(expected, expected + count, bytes.data() + offset);
}

void emit_summary(const nnn::archive::MemberResult& result) {
    std::cout << "{\"status\":";
    emit_status(result.status);
    std::cout << ",\"entry\":";
    emit_entry(result.entry);
    std::cout << ",\"byte_count\":" << result.bytes.size();
    if (result.status) {
        std::uint64_t digest = 14695981039346656037ULL;
        for (const Byte byte : result.bytes) {
            digest ^= byte;
            digest *= 1099511628211ULL;
        }
        std::ostringstream value;
        value << std::hex << std::setw(16) << std::setfill('0') << digest;
        const auto prefix = std::min<std::size_t>(80, result.bytes.size());
        constexpr std::array<Byte, 9> dwq_tag{'P', 'A', 'C', 'K', 'T', 'Y', 'P', 'E', '='};
        constexpr std::array<Byte, 8> png_signature{137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
        const bool dwq = result.bytes.size() >= 64U && has_bytes(result.bytes, 48U, dwq_tag.data(), dwq_tag.size());
        std::cout << ",\"fnv1a64\":" << quote(value.str()) << ",\"prefix_hex\":"
                  << quote(hex(result.bytes.begin(), result.bytes.begin() + static_cast<std::ptrdiff_t>(prefix)))
                  << ",\"dwq_signature\":" << (dwq ? "true" : "false")
                  << ",\"png_signature_at_64\":"
                  << (has_bytes(result.bytes, 64U, png_signature.data(), png_signature.size()) ? "true" : "false");
    }
    std::cout << '}';
}

void usage() {
    std::cerr << "Usage: nnn-archive-probe GTB GPK [--verify-members | --member-hex HEX]\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 && argc != 4 && argc != 5) { usage(); return 2; }
        const std::string table = argv[1];
        const std::string pack = argv[2];
        const bool verify_all = argc == 4 && std::string(argv[3]) == "--verify-members";
        const bool verify_one = argc == 5 && std::string(argv[3]) == "--member-hex";
        std::string name;
        if (table.empty() || pack.empty() || table.front() == '-' || pack.front() == '-' ||
            (argc != 3 && !verify_all && !verify_one) || (verify_one && !decode_name(argv[4], name))) {
            usage(); return 2;
        }
        const auto loaded = nnn::archive::load_index(table, pack);
        std::cout << "{\"loaded\":" << (loaded.status ? "true" : "false") << ",\"status\":";
        emit_status(loaded.status);
        std::cout << ",\"index_format\":" << quote(loaded.index.format() == nnn::archive::IndexFormat::Extended64 ? "Extended64" : "Classic32")
                  << ",\"gpk_size\":" << loaded.index.gpk_size() << ",\"entry_count\":" << loaded.index.entries().size()
                  << ",\"name_policy\":\"exact-original-bytes-case-sensitive\",\"entries\":[";
        bool first = true;
        for (const auto& entry : loaded.index.entries()) {
            if (!first) std::cout << ',';
            first = false;
            emit_entry(entry);
        }
        std::cout << "],\"member_reads_requested\":" << (verify_all || verify_one ? "true" : "false") << ",\"members\":[";
        bool all_ok = static_cast<bool>(loaded.status);
        first = true;
        if (loaded.status && verify_one) {
            const auto result = nnn::archive::read_member(pack, loaded.index, name);
            emit_summary(result);
            all_ok = static_cast<bool>(result.status);
        } else if (loaded.status && verify_all) {
            for (const auto& entry : loaded.index.entries()) {
                const auto result = nnn::archive::read_member(pack, loaded.index, entry.name);
                if (!first) std::cout << ',';
                first = false;
                emit_summary(result);
                if (!result.status) { all_ok = false; break; }
            }
        }
        std::cout << "],\"member_reads_ok\":";
        if (loaded.status && (verify_all || verify_one)) std::cout << (all_ok ? "true" : "false");
        else std::cout << "null";
        std::cout << "}\n";
        return all_ok ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Archive probe error: " << error.what() << '\n';
        return 2;
    }
}
