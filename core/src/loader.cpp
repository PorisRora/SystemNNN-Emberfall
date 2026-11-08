#include "program.hpp"
#include <algorithm>

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <utility>

namespace nnn {
namespace {

Status failure(ErrorCode code, std::string detail, WordIndex word = no_word) {
    return {code, word, std::move(detail)};
}

bool addressable_byte_count(std::uint64_t bytes) noexcept {
    return bytes / sizeof(Word) < no_word && bytes <= std::numeric_limits<std::size_t>::max() &&
           bytes <= static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max());
}

class OperationGuard {
public:
    explicit OperationGuard(bool& busy) : busy_(busy) { busy_ = true; }
    ~OperationGuard() { busy_ = false; }
private:
    bool& busy_;
};

struct FunctionBinding {
    bool present = false;
    std::size_t size = 0;
    Word stop_f5_id = no_word;
};

Status snapshot_registry(const FunctionRegistry* registry, FunctionBinding& binding) {
    if (!registry) return {};
    if (!registry->IsValid()) return failure(ErrorCode::InvalidFormat, "Function registry is invalid");
    binding = {true, registry->Size(), registry->StopF5Id()};
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
    case ErrorCode::UnsupportedInstruction: return "UnsupportedInstruction";
    case ErrorCode::UnsupportedExpression: return "UnsupportedExpression";
    case ErrorCode::NotLoaded: return "NotLoaded";
    case ErrorCode::BudgetExhausted: return "BudgetExhausted";
    case ErrorCode::CallbackFailed: return "CallbackFailed";
    case ErrorCode::Busy: return "Busy";
    case ErrorCode::CallStackUnderflow: return "CallStackUnderflow";
    case ErrorCode::CallStackOverflow: return "CallStackOverflow";
    case ErrorCode::HostRejected: return "HostRejected";
    }
    return "UnknownError";
}

ReadResult read_script_file(const std::string& filename) {
    ReadResult result;
    if (filename.find('\0') != std::string::npos) {
        result.status = failure(ErrorCode::ReadFailed, "NUL in file path");
        return result;
    }
    std::error_code error;
    const auto state = std::filesystem::status(filename, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && !std::filesystem::exists(state))) {
        result.status = failure(ErrorCode::NotFound, "Script file not found");
        return result;
    }
    if (error || !std::filesystem::is_regular_file(state)) {
        result.status = failure(ErrorCode::ReadFailed, "Script is not a readable regular file");
        return result;
    }
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    const auto end = file.tellg();
    if (!file || end < std::streampos(0)) {
        result.status = failure(ErrorCode::ReadFailed, "Cannot open or size script");
        return result;
    }
    const auto length = static_cast<std::uint64_t>(end);
    if (!addressable_byte_count(length)) {
        result.status = failure(ErrorCode::InvalidFormat, "Script exceeds addressable word offsets");
        return result;
    }
    result.declared_size = length;
    result.bytes.resize(static_cast<std::size_t>(length));
    file.seekg(0, std::ios::beg);
    if (!file) {
        result.status = failure(ErrorCode::ReadFailed, "Cannot seek script");
        return result;
    }
    if (length != 0) {
        file.read(reinterpret_cast<char*>(result.bytes.data()), static_cast<std::streamsize>(length));
        if (file.gcount() != static_cast<std::streamsize>(length) || file.bad()) {
            result.status = failure(ErrorCode::ReadFailed, "Short or failed script read");
            return result;
        }
    }
    if (file.peek() != std::char_traits<char>::eof() || file.bad()) {
        result.status = failure(ErrorCode::ReadFailed, "Script changed size while reading");
    }
    return result;
}

DecodedWords decode_words(const std::vector<Byte>& bytes, DecodeParameters parameters) {
    DecodedWords result;
    if (!addressable_byte_count(bytes.size()) || bytes.size() % 4U != 0) {
        result.status = failure(ErrorCode::InvalidFormat, "Script must contain complete bounded 32-bit words");
        return result;
    }
    result.words.reserve(bytes.size() / 4U);
    Word key = parameters.xor_key;
    Word increment = 0;
    for (std::size_t byte = 0; byte < bytes.size(); byte += 4U) {
        const Word encrypted = static_cast<Word>(bytes[byte]) |
            (static_cast<Word>(bytes[byte + 1U]) << 8U) |
            (static_cast<Word>(bytes[byte + 2U]) << 16U) |
            (static_cast<Word>(bytes[byte + 3U]) << 24U);
        result.words.push_back((encrypted ^ key) + increment);
        key += parameters.key_step;
        increment += parameters.increment_step;
    }
    return result;
}

CExecScript::CExecScript(CScriptCallBack& callback) : callback_(callback) {}
CExecScript::~CExecScript() = default;
bool CExecScript::IsLoaded() const noexcept { return static_cast<bool>(program_); }
WordIndex CExecScript::GetPC() const noexcept { return pc_; }
const std::string& CExecScript::Identity() const noexcept { return identity_; }
const Status& CExecScript::LastStatus() const noexcept { return last_status_; }
std::size_t CExecScript::WordCount() const noexcept { return program_ ? program_->words.size() : 0; }
bool CExecScript::GetData(WordIndex index, Word& value) const noexcept {
    if (!program_ || index >= program_->words.size()) return false;
    value = program_->words[index];
    return true;
}
std::size_t CExecScript::GetCallDepth() const noexcept { return call_depth_; }
bool CExecScript::GetCallFrame(std::size_t index, CallFrame& frame) const noexcept {
    if (index >= call_depth_) return false;
    frame = call_frames_[index];
    return true;
}
bool CExecScript::HasFunctionRegistry() const noexcept { return program_ && program_->has_function_registry; }
std::size_t CExecScript::FunctionRegistrySize() const noexcept { return program_ ? program_->function_registry_size : 0U; }
Word CExecScript::BoundStopF5Id() const noexcept { return program_ ? program_->stop_f5_id : no_word; }
bool CExecScript::HasForegroundControls() const noexcept {
    return program_ && program_->script_control_profile == ScriptControlProfile::Foreground;
}
bool CExecScript::GetScriptCallSlot(Word& out) const noexcept {
    if (!script_call_slot_valid_) return false;
    out = script_call_slot_;
    return true;
}
bool CExecScript::GetCommandCallSlot(Word& out) const noexcept {
    if (command_call_slot_==no_word) return false;
    out=command_call_slot_;
    return true;
}
SkipFlags CExecScript::GetSkipFlags() const noexcept { return callback_.GetSkipFlags(); }
void CExecScript::SetSkipFlags(SkipFlags flags) noexcept { callback_.SetSkipFlags(flags); }

Status CExecScript::install_bytes(const std::vector<Byte>& bytes, std::string identity, DecodeParameters parameters,
                                 bool has_registry, std::size_t registry_size, Word stop_f5_id,
                                 ScriptControlProfile profile) {
    auto decoded = decode_words(bytes, parameters);
    if (!decoded.status) return last_status_ = std::move(decoded.status);
    auto candidate = std::make_shared<detail::Program>();
    candidate->words = std::move(decoded.words);
    candidate->has_function_registry = has_registry;
    candidate->function_registry_size = registry_size;
    candidate->stop_f5_id = stop_f5_id;
    candidate->script_control_profile = profile;
    auto status = detail::validate_program(*candidate);
    if (!status) return last_status_ = std::move(status);
    auto hash=14695981039346656037ULL;
    const auto mix=[&](std::uint8_t b){hash=(hash^b)*1099511628211ULL;};
    for (const auto word:candidate->words) for (unsigned i=0;i<4;++i) mix(static_cast<std::uint8_t>(word>>(8U*i)));
    for (const auto b:candidate->strings.bytes) mix(static_cast<std::uint8_t>(b));
    for (const auto b:candidate->messages.bytes) mix(static_cast<std::uint8_t>(b));
    candidate->fingerprint=hash;
    // All operations after validation are non-throwing commits. No filename
    // cache exists, so an explicit same-name request always reads again.
    program_=std::move(candidate);
    identity_.swap(identity);
    reset_execution();
    return last_status_;
}

void CExecScript::reset_execution() noexcept {
    pc_ = 0;
    call_frames_.fill({});
    call_depth_ = 0;
    script_call_slot_ = 0;
    script_call_slot_valid_ = false;
    command_call_slot_ = no_word;
    selection_.reset();
    conditional_entry_ = no_word;
    last_status_ = {};
}

Status CExecScript::LoadBytes(const std::vector<Byte>& bytes, std::string identity, DecodeParameters parameters,
                             const FunctionRegistry* registry, ScriptControlProfile profile) {
    if (executing_) return last_status_ = failure(ErrorCode::Busy, "VM operation already active");
    OperationGuard operation(executing_);
    try {
        FunctionBinding binding;
        const auto snapshot = snapshot_registry(registry, binding);
        if (!snapshot) return last_status_ = snapshot;
        if (profile != ScriptControlProfile::Closed && profile != ScriptControlProfile::Foreground) {
            return last_status_ = failure(ErrorCode::InvalidFormat, "Invalid script control profile");
        }
        return install_bytes(bytes, std::move(identity), parameters, binding.present, binding.size, binding.stop_f5_id, profile);
    } catch (const std::exception& error) {
        return last_status_ = failure(ErrorCode::ReadFailed, error.what());
    } catch (...) {
        return last_status_ = failure(ErrorCode::ReadFailed, "Script preparation failed");
    }
}

Status CExecScript::LoadFile(const std::string& filename, DecodeParameters parameters, const Reader& reader,
                            const FunctionRegistry* registry, ScriptControlProfile profile) {
    if (executing_) return last_status_ = failure(ErrorCode::Busy, "VM operation already active");
    OperationGuard operation(executing_);
    try {
        FunctionBinding binding;
        const auto snapshot = snapshot_registry(registry, binding);
        if (!snapshot) return last_status_ = snapshot;
        if (profile != ScriptControlProfile::Closed && profile != ScriptControlProfile::Foreground) {
            return last_status_ = failure(ErrorCode::InvalidFormat, "Invalid script control profile");
        }
        auto input = reader ? reader(filename) : read_script_file(filename);
        if (!input.status) return last_status_ = std::move(input.status);
        if (input.declared_size != input.bytes.size()) {
            return last_status_ = failure(ErrorCode::ReadFailed, "Reader returned an incomplete script");
        }
        return install_bytes(input.bytes, filename, parameters, binding.present, binding.size, binding.stop_f5_id, profile);
    } catch (const std::exception& error) {
        return last_status_ = failure(ErrorCode::ReadFailed, error.what());
    } catch (...) {
        return last_status_ = failure(ErrorCode::ReadFailed, "Script reader failed");
    }
}

namespace detail {
namespace {
struct Range { WordIndex first = 0; WordIndex end = 0; };

Status read_text_table(const std::vector<Word>& words, Range range, Word count, TextTable& output) {
    if (count > range.end - range.first) return failure(ErrorCode::InvalidFormat, "Text pointer table exceeds record", range.first);
    const WordIndex payload = range.first + count;
    output.bytes.reserve(static_cast<std::size_t>(range.end - payload) * 4);
    for (WordIndex i = payload; i < range.end; ++i) {
        for (unsigned shift = 0; shift < 32; shift += 8) output.bytes.push_back(static_cast<char>((words[i] >> shift) & 255U));
    }
    // One backward scan validates even shared strings without repeatedly scanning long suffixes.
    std::size_t terminated = output.bytes.size();
    while (terminated && output.bytes[terminated - 1] != 0) --terminated;
    output.offsets.reserve(count);
    for (Word i = 0; i < count; ++i) {
        const WordIndex at = words[range.first + i];
        if (at < payload || at >= range.end) return failure(ErrorCode::InvalidFormat, "Text pointer outside table", range.first + i);
        const auto offset = static_cast<std::size_t>(at - payload) * 4;
        if (offset >= terminated) return failure(ErrorCode::InvalidFormat, "Unterminated text in table", at);
        output.offsets.push_back(offset);
    }
    return {};
}

// Validate expression structure without evaluating values or calling the host.
// System-command numeric payloads are deliberately excluded: those are raw words.
class ExpressionShape {
    const Program& program_;
    struct Entry { Word token; bool computed = false; };
    Status operand(Word token, WordIndex pc) const {
        const Word kind=token&0xffff0000U;
        if (kind==token_var || kind==token_number || (token>>28)==12 || (token>>28)==13) return {};
        if (kind==token_number32) {
            if ((token&0xffffU)<program_.constants.size()) return {};
            return failure(ErrorCode::InvalidFormat,"INT32 index outside table",pc);
        }
        return failure(ErrorCode::UnsupportedExpression,"Invalid expression operand",pc);
    }
    static bool writable(const Entry& entry) {
        return !entry.computed && (entry.token&0xffff0000U)==token_var && (entry.token&0xffffU)<32768;
    }
    static bool arithmetic(Word op) {
        return op=='+' || op=='-' || op=='*' || op=='/' || op=='%' || op=='&' || op=='|' || op=='^';
    }
public:
    explicit ExpressionShape(const Program& program):program_(program) {}
    Status read(WordIndex& cursor,WordIndex end) const {
        const auto& words=program_.words;
        if (cursor>=end) return failure(ErrorCode::InvalidFormat,"Missing expression",cursor);
        const Word first=words[cursor++];
        if (first!=token_expression) return operand(first,cursor-1);
        if (cursor>=end) return failure(ErrorCode::InvalidFormat,"Missing RPN length",cursor);
        const Word count=words[cursor++];
        if (!count || count>end-cursor) return failure(ErrorCode::InvalidFormat,"Invalid RPN extent",cursor-1);
        const WordIndex limit=cursor+count;
        std::vector<Entry> stack;
        stack.reserve(std::min<Word>(count,256U));
        while (cursor<limit) {
            const WordIndex pc=cursor++;
            const Word token=words[pc],kind=token&0xffff0000U,op=token&0xffffU;
            if ((token>>28)!=10) {
                const auto status=operand(token,pc);
                if (!status) return status;
                // The original evaluator's operand stack has 256 cells.
                if (stack.size()>=256U) return failure(ErrorCode::InvalidFormat,"RPN operand stack limit",pc);
                stack.push_back({token,false});continue;
            }
            if (kind==0xa0000000U && op=='!') {
                if (stack.empty()) return failure(ErrorCode::InvalidFormat,"Missing unary operand",pc);
                auto& entry=stack.back();
                if (!entry.computed && (entry.token&0xffff0000U)==token_var) {
                    if (!writable(entry)) return failure(ErrorCode::InvalidFormat,"Assignment requires a writable variable",pc);
                } else entry={0,true};
                continue;
            }
            bool assignment=false,known=false;
            if (kind==0xa0000000U) {assignment=op=='=';known=assignment || arithmetic(op);}
            else if (kind==0xa2000000U) {assignment=op!='=';known=op=='=' || op=='!' || arithmetic(op);}
            else if (kind==0xa3000000U) known=op=='=' || op=='&' || op=='|' || op=='^';
            else if (kind==0xa4000000U) known=op=='<' || op=='>';
            else if (kind==0xa5000000U) known=op=='<' || op=='>' || op=='=' || op=='!';
            if (!known) return failure(ErrorCode::UnsupportedExpression,"Unknown RPN operator",pc);
            if (stack.size()<2) return failure(ErrorCode::InvalidFormat,"Missing binary operand",pc);
            stack.pop_back();
            auto& left=stack.back();
            if (assignment) {
                if (!writable(left)) return failure(ErrorCode::InvalidFormat,"Assignment requires a writable variable",pc);
            } else left={0,true};
        }
        if (stack.size()!=1) return failure(ErrorCode::InvalidFormat,"RPN did not produce one result",cursor);
        return {};
    }
};
} // namespace

Status validate_program(Program& p) {
    const auto& w = p.words;
    if (w.size() < 32 || w[0] != 32 || w[1] != identify_data || w[2] != data_header)
        return failure(ErrorCode::InvalidFormat, "Invalid 32-word SPT header");
    constexpr std::array<Word,4> magic{0x48545053U,0x45444145U,0x20203052U,0x00202020U};
    if (!std::equal(magic.begin(), magic.end(), w.begin()+12)) return failure(ErrorCode::UnsupportedFormat, "Expected SPTHEADER0", 12);

    // The original VM reads text through the header's absolute pointer tables
    // (execscript.cpp GetMessagePtr/GetStringPtr); it does not use the enclosing
    // DATA record lengths. Translation tools can move the strings and update
    // those pointers while retaining the producer's old DATA lengths. Bound the
    // text regions by independently typed neighbouring tables, never by looking
    // for an arbitrary marker inside text. Executable record lengths stay strict.
    const auto raw_table = [&](WordIndex pc, Word kind) {
        return pc >= 32 && pc <= w.size()-4 && w[pc] >= 4 &&
               w[pc+1] == identify_data && w[pc+2] == data_table && w[pc+3] == kind;
    };
    const auto header_table = [&](WordIndex field, Word kind) -> WordIndex {
        if (w[field] < 4) return no_word;
        const WordIndex pc = w[field]-4;
        return raw_table(pc,kind) ? pc : no_word;
    };
    const WordIndex message_record = header_table(5,0x44440002U);
    const WordIndex string_record = header_table(7,0x44440001U);
    WordIndex constant_record = header_table(18,0x44440004U);
    if (constant_record != no_word && w[constant_record] > w.size()-constant_record)
        constant_record = no_word;
    bool relocated_empty_constants = false;
    if (constant_record == no_word) {
        const WordIndex sub_record = header_table(20,0x44440005U);
        // EndBuild writes STRING, INT32, SUB in this order. A stale optional
        // INT32 pointer is recoverable only for the exact empty table directly
        // before the independently addressed SUB table. Never infer a nonempty
        // constant pool: NUMBER32 accesses still require a validated pool entry.
        if (sub_record != no_word && sub_record >= 4 &&
            w[sub_record] <= w.size()-sub_record &&
            raw_table(sub_record-4,0x44440004U) && w[sub_record-4] == 4) {
            constant_record = sub_record-4;
            relocated_empty_constants = true;
        }
    }
    const bool header_text_layout = message_record != no_word && string_record != no_word &&
        constant_record != no_word && message_record+4 <= string_record && string_record+4 <= constant_record;
    if (!header_text_layout) relocated_empty_constants = false;
    p.record_index.assign(w.size(), no_word);
    for (WordIndex pc = 0; pc < w.size();) {
        Word len = w[pc];
        if (header_text_layout && pc == message_record) len = string_record-message_record;
        else if (header_text_layout && pc == string_record) len = constant_record-string_record;
        if (len < 3 || len > w.size() - pc) return failure(ErrorCode::InvalidFormat, "Record length exceeds script", pc);
        p.record_index[pc] = static_cast<WordIndex>(p.records.size());
        p.records.push_back({pc,len,w[pc+1],w[pc+2]});
        pc += len;
    }
    const auto table = [&](WordIndex pc, Word kind, Range& out) {
        if (pc >= w.size() || p.record_index[pc] == no_word) return false;
        const auto& r = p.records[p.record_index[pc]];
        if (r.identify != identify_data || r.code != data_table || r.length < 4 || w[pc+3] != kind) return false;
        out = {pc+4,pc+r.length};
        return true;
    };
    constexpr std::array<Word,7> kinds{0x44440002U,0x44440001U,0x44440004U,0x44440005U,0x44440006U,0x44440007U,0x44440008U};
    constexpr std::array<WordIndex,7> fields{5,7,18,20,21,22,23};
    std::array<Range,7> tables{};
    for (std::size_t i=0; i<tables.size(); ++i) {
        const WordIndex address = i == 2 && relocated_empty_constants ? constant_record+4 : w[fields[i]];
        if (address < 4 || !table(address-4,kinds[i],tables[i])) return failure(ErrorCode::InvalidFormat,"Invalid base table pointer",fields[i]);
    }
    auto status = read_text_table(w,tables[0],w[4],p.messages);
    if (!status) return status;
    status = read_text_table(w,tables[1],w[6],p.strings);
    if (!status) return status;
    if (relocated_empty_constants) {
        // Establish that the inferred empty pool immediately follows actual
        // string data, rather than treating an unreferenced tail as strings.
        if (p.strings.offsets.empty()) {
            if (!p.strings.bytes.empty())
                return failure(ErrorCode::InvalidFormat,"Unexpected data after empty string table",tables[1].first);
        } else {
            const auto last = *std::max_element(p.strings.offsets.begin(),p.strings.offsets.end());
            const auto terminator = std::find(p.strings.bytes.begin()+static_cast<std::ptrdiff_t>(last),p.strings.bytes.end(),0);
            const auto end = static_cast<std::size_t>(terminator-p.strings.bytes.begin())+1;
            if (p.strings.bytes.size()-end >= sizeof(Word))
                return failure(ErrorCode::InvalidFormat,"Unreferenced data after relocated string table",tables[1].first);
        }
    }
    for (WordIndex i=tables[2].first; i<tables[2].end; ++i) p.constants.push_back(value_from_word(w[i]));
    for (std::size_t i=0; i<p.adjustments.size(); ++i) {
        const Range range = tables[i+3];
        p.adjustments[i].assign(w.begin()+range.first,w.begin()+range.end);
    }
    if (w[9] || w[16] || w[17] || w[8] || w[10]) {
        Range labels, films, koma;
        if (w[9]<4 || !table(w[9]-4,table_label,labels) || !table(w[16],table_label,films) || !table(w[17],table_label,koma) ||
            w[8]!=labels.end-labels.first || w[10]>w[8] || films.end-films.first!=w[8]-w[10])
            return failure(ErrorCode::InvalidFormat,"Invalid label table pointers or counts",9);
        p.labels.assign(w.begin()+labels.first,w.begin()+labels.end);
        p.film_koma_offsets.assign(w.begin()+films.first,w.begin()+films.end);
        p.koma_targets.assign(w.begin()+koma.first,w.begin()+koma.end);
        const auto executable=[&](Word dest) {
            return dest<w.size() && p.record_index[dest]!=no_word &&
                   p.records[p.record_index[dest]].identify!=identify_data;
        };
        for (std::size_t i=0;i<p.labels.size();++i)
            if (!executable(p.labels[i]) || (i && p.labels[i-1]>=p.labels[i]))
                return failure(ErrorCode::InvalidFormat,"Invalid label target or order",labels.first+static_cast<WordIndex>(i));
        for (std::size_t i=0;i<p.film_koma_offsets.size();++i)
            if (p.film_koma_offsets[i]>p.koma_targets.size() || (!i && p.film_koma_offsets[i]!=0) ||
                (i && p.film_koma_offsets[i-1]>p.film_koma_offsets[i]))
                return failure(ErrorCode::InvalidFormat,"Invalid film koma offset",films.first+static_cast<WordIndex>(i));
        if (p.film_koma_offsets.empty() && !p.koma_targets.empty())
            return failure(ErrorCode::InvalidFormat,"Koma targets without film",koma.first);
        for (std::size_t i=0;i<p.koma_targets.size();++i)
            if (!executable(p.koma_targets[i]) || (i && p.koma_targets[i-1]>=p.koma_targets[i]))
                return failure(ErrorCode::InvalidFormat,"Invalid koma target or order",koma.first+static_cast<WordIndex>(i));
        for (std::size_t i=0;i<p.film_koma_offsets.size();++i) {
            const auto label=static_cast<std::size_t>(w[10])+i;
            const auto last=i+1<p.film_koma_offsets.size() ? p.film_koma_offsets[i+1] : p.koma_targets.size();
            for (std::size_t k=p.film_koma_offsets[i];k<last;++k)
                if (p.koma_targets[k]<p.labels[label] ||
                    (label+1<p.labels.size() && p.koma_targets[k]>=p.labels[label+1]))
                    return failure(ErrorCode::InvalidFormat,"Koma target outside film",koma.first+static_cast<WordIndex>(k));
        }
    }
    p.loop_back.assign(p.records.size(),no_word);
    const ExpressionShape expressions(p);
    std::vector<std::pair<WordIndex,Word>> blocks;
    std::vector<bool> branch_ends(p.records.size(),false);
    std::size_t sub_slot=0;
    for (std::size_t i=1; i<p.records.size(); ++i) {
        auto& r=p.records[i];
        const WordIndex pc=r.pc;
        const WordIndex record_end=pc+r.length;
        if (r.identify==identify_control) {
            Word required=0;
            switch (r.code) {
            case control_nop: case control_else: case control_end: case control_next: case control_return:
            case control_exit: case control_endfilm: case control_endkoma: case control_endif: case control_close:
                required=3; break;
            case control_open: case control_goto: case control_case: case control_scriptjump: required=4; break;
            case control_sub: case control_script: case control_subscript: required=5; break;
            default: break;
            }
            if (required && r.length!=required) return failure(ErrorCode::InvalidFormat,"Invalid control record length",pc);
            if (r.code==control_open) {
                const auto span=w[pc+3];
                if (span<4 || span>w.size()-pc || blocks.size()>=max_block_depth)
                    return failure(ErrorCode::InvalidFormat,"Invalid block span or depth",pc);
                blocks.emplace_back(pc,span);
            } else if (r.code==control_close) {
                if (blocks.empty() || blocks.back().first+blocks.back().second!=record_end)
                    return failure(ErrorCode::InvalidFormat,"Unmatched block close",pc);
                blocks.pop_back();
            }
            if (r.code==control_if || r.code==control_elsif || r.code==control_else || r.code==control_while || r.code==control_case) {
                if ((r.code==control_elsif || r.code==control_else) && !branch_ends[i])
                    return failure(ErrorCode::InvalidFormat,"Orphan branch",pc);
                if (i+1>=p.records.size() || p.records[i+1].identify==identify_data ||
                    (p.records[i+1].identify==identify_control &&
                     (p.records[i+1].code==control_close || p.records[i+1].code==control_else || p.records[i+1].code==control_elsif)))
                    return failure(ErrorCode::InvalidFormat,"Missing control body",pc);
                if (r.code==control_if || r.code==control_elsif) {
                    const auto& body=p.records[i+1];
                    const auto span=body.identify==identify_control && body.code==control_open && body.length==4 ? w[body.pc+3] : body.length;
                    if (span>w.size()-body.pc) return failure(ErrorCode::InvalidFormat,"Invalid branch body",body.pc);
                    const auto after=body.pc+span;
                    if (after<w.size() && p.record_index[after]!=no_word) branch_ends[p.record_index[after]]=true;
                }
            }
            if ((w[9] || w[16] || w[17] || w[8] || w[10]) && (r.code==control_sub || r.code==control_goto)) {
                if (sub_slot>=p.adjustments[0].size() ||
                    (r.code==control_sub && (w[pc+4]!=sub_slot || p.adjustments[0][sub_slot]!=pc)))
                    return failure(ErrorCode::InvalidFormat,"Invalid SUB adjustment registry",pc);
                ++sub_slot;
            }
        }
        if (r.identify==identify_calcu || (r.identify==identify_control &&
            (r.code==control_if || r.code==control_elsif || r.code==control_while))) {
            WordIndex cursor=pc+3;
            status=expressions.read(cursor,record_end);
            if (!status) return status;
            if (cursor!=record_end) return failure(ErrorCode::InvalidFormat,"Trailing expression data",pc);
        } else if (r.identify==identify_command || r.identify==identify_function) {
            if (r.length<4 || w[pc+3]>r.length-4) return failure(ErrorCode::InvalidFormat,"User arguments exceed record",pc);
            const Word count=w[pc+3];
            WordIndex cursor=pc+4;
            for (Word argument=0;argument<count;++argument) {
                status=expressions.read(cursor,record_end);
                if (!status) return status;
            }
            if (cursor!=record_end && !(r.identify==identify_command && record_end-cursor==1))
                return failure(ErrorCode::InvalidFormat,"Unexpected user record tail",pc);
            if (cursor<record_end) {
                const Word slot=w[cursor];
                if (slot<p.adjustments[2].size() && p.adjustments[2][slot]==pc) r.command_call_slot=slot;
            }
        } else if (r.identify==identify_control) {
            if (r.code==control_goto && (r.length<4 || w[pc+3]>=w.size() || p.record_index[w[pc+3]]==no_word ||
                p.records[p.record_index[w[pc+3]]].identify==identify_data))
                return failure(ErrorCode::InvalidFormat,"GOTO does not name a record",pc);
            if (r.code==control_sub && (r.length<5 || w[pc+3]>=p.labels.size()))
                return failure(ErrorCode::InvalidFormat,"Invalid SUB label",pc);
            if (r.code==control_sub && (i+1>=p.records.size() || p.records[i+1].identify==identify_data))
                return failure(ErrorCode::InvalidFormat,"SUB lacks executable return record",pc);
            if (r.code==control_script || r.code==control_subscript || r.code==control_scriptjump) {
                if (r.length<(r.code==control_scriptjump?4U:5U) || w[pc+3]>=p.strings.offsets.size())
                    return failure(ErrorCode::InvalidFormat,"Invalid script transfer fields",pc);
            }
        }
        if (r.identify==identify_systemcommand || r.identify==identify_systemfunction) {
            const bool function=r.identify==identify_systemfunction;
            const bool dialogue=!function && (r.code==command_print || r.code==command_lprint || r.code==command_append);
            if (dialogue) {
                if (r.length<5 || w[pc+3]>=p.messages.offsets.size()) return failure(ErrorCode::InvalidFormat,"Invalid dialogue fields",pc);
            } else if (function && has_string_argument(r.code)) {
                if (r.length<5 || (w[pc+4]!=UINT32_MAX && w[pc+4]>=p.strings.offsets.size()))
                    return failure(ErrorCode::InvalidFormat,"Invalid string argument",pc);
                if ((r.code==function_loaddwq || r.code==function_preloaddwq) &&
                    (r.length!=5 || w[pc+4]==UINT32_MAX))
                    return failure(ErrorCode::InvalidFormat,"Invalid graphic filename record",pc);
            } else if (!function && r.code==command_select) {
                if (r.length<5 || w[pc+3]>r.length-5) return failure(ErrorCode::InvalidFormat,"Invalid SELECT header",pc);
            } else {
                if (r.length<4 || w[pc+3]>r.length-4 || r.length-4-w[pc+3]>1)
                    return failure(ErrorCode::InvalidFormat,"Invalid system argument count",pc);
                const auto count=w[pc+3];
                if (count>max_numeric_event_parameters || (r.length-4!=count && !(function && r.code==function_voice)))
                    return failure(ErrorCode::InvalidFormat,"Invalid numeric event extent",pc);
                Word minimum=0;
                if (!function && r.code==command_draw) minimum=1;
                else if (!function && r.code==command_overrap) minimum=2;
                else if (function && r.code==function_seteffect) minimum=2;
                else if (function && r.code==function_seteffectrect) minimum=9;
                else if (function && r.code==function_cleareffectlayer) minimum=1;
                if (count<minimum) return failure(ErrorCode::InvalidFormat,"Too few numeric event parameters",pc);
                if (function && r.code==function_voice && w[pc+3]>0 && w[pc+4]!=UINT32_MAX && w[pc+4]>=p.strings.offsets.size())
                    return failure(ErrorCode::InvalidFormat,"Invalid voice string index",pc);
            }
        }
        if (r.identify!=identify_control || r.code!=control_while || i+1>=p.records.size()) continue;
        const auto& body=p.records[i+1];
        const bool braced=body.identify==identify_control && body.code==control_open;
        if (braced && body.length<4) continue;
        const Word span=braced ? w[body.pc+3] : body.length;
        if (span<body.length || span>w.size()-body.pc) continue;
        const WordIndex end=body.pc+span;
        const auto next=end==w.size() ? p.records.size() : static_cast<std::size_t>(p.record_index[end]);
        if (next<=i+1 || next>p.records.size()) continue;
        const auto back_jump=[&](const Record& x) {
            return x.identify==identify_control && x.code==control_goto && x.length>=4 && w[x.pc+3]==pc;
        };
        if (next<p.records.size() && back_jump(p.records[next])) continue;
        p.loop_back[next-1]=pc;
    }
    if (!blocks.empty()) return failure(ErrorCode::InvalidFormat,"Unclosed block",blocks.back().first);
    if ((w[9] || w[16] || w[17] || w[8] || w[10]) && sub_slot!=p.adjustments[0].size())
        return failure(ErrorCode::InvalidFormat,"Extra SUB adjustment entries",tables[3].first);
    return {};
}
} // namespace detail
} // namespace nnn
