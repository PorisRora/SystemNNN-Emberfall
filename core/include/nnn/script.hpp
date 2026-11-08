#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nnn {

using Byte = std::uint8_t;
using Word = std::uint32_t;
using Value = std::int32_t;
using WordIndex = std::uint32_t;
inline constexpr WordIndex no_word = UINT32_MAX;
inline constexpr std::size_t max_script_bytes = 4U * 1024U * 1024U; // Diagnostic bridge input limit.
inline constexpr std::size_t max_call_depth = 249;
inline constexpr std::size_t max_numeric_event_parameters = 258;
inline constexpr std::size_t max_function_config_bytes = 64U * 1024U;
inline constexpr std::size_t max_function_registry_entries = 256;
inline constexpr std::size_t max_function_token_bytes = 127;

enum class ErrorCode {
    Ok,
    NotFound,
    ReadFailed,
    UnsupportedFormat,
    InvalidFormat,
    UnsupportedInstruction,
    UnsupportedExpression,
    NotLoaded,
    BudgetExhausted,
    CallbackFailed,
    Busy,
    CallStackUnderflow,
    CallStackOverflow,
    HostRejected,
};

struct Status {
    ErrorCode code = ErrorCode::Ok;
    WordIndex word = no_word;
    std::string detail;
    explicit operator bool() const noexcept { return code == ErrorCode::Ok; }
};

const char* error_name(ErrorCode code) noexcept;

struct DecodeParameters {
    Word xor_key = UINT32_MAX;
    Word key_step = 0;
    Word increment_step = 0;
};

// Both entry points load the same format. A foreground scheduler owns file transfers.
enum class ScriptControlProfile { Closed, Foreground };

struct ReadResult {
    Status status;
    std::vector<Byte> bytes;
    std::uint64_t declared_size = 0;
};

// Success must return exactly declared_size bytes. A short successful reader is
// converted to ReadFailed by LoadFile before any VM state is changed.
using Reader = std::function<ReadResult(const std::string&)>;
ReadResult read_script_file(const std::string& filename);

struct DecodedWords {
    Status status;
    std::vector<Word> words;
};

// Pure little-endian word decoding with modulo-2^32 key and value increments.
// This checks byte alignment/size only; LoadBytes additionally validates SPT.
DecodedWords decode_words(const std::vector<Byte>& bytes, DecodeParameters parameters = {});

struct FunctionRegistryResult;

class FunctionRegistry {
public:
    FunctionRegistry() = default;
    FunctionRegistry(const FunctionRegistry&) = default;
    FunctionRegistry& operator=(const FunctionRegistry& other);
    FunctionRegistry(FunctionRegistry&& other) noexcept;
    FunctionRegistry& operator=(FunctionRegistry&& other) noexcept;
    bool IsValid() const noexcept;
    std::size_t Size() const noexcept;
    // Returns an owned name copy; failure leaves out unchanged.
    bool GetName(std::size_t id, std::string& out) const;
    Word StopF5Id() const noexcept;

private:
    friend FunctionRegistryResult parse_function_registry(const std::vector<Byte>& bytes);
    std::vector<std::string> names_;
    Word stop_f5_id_ = no_word;
    bool valid_ = false;
};

struct FunctionRegistryResult {
    Status status;
    FunctionRegistry registry;
};

FunctionRegistryResult parse_function_registry(const std::vector<Byte>& bytes);

struct SkipFlags {
    bool skip_f4_mode = false;
    bool skip_f5_mode = false;
    bool skip_next_command = false;
    bool skip_effect_command = false;
    bool skip_to_film_end = false;
};

enum class SystemPayload { Numeric, Named, Dialogue, Scalar, Selection };

struct SystemEvent {
    Word identify = 0, opcode = 0;
    SystemPayload payload = SystemPayload::Numeric;
    Value primary = 0;
    std::vector<Value> parameters;
    std::optional<Value> trailing;
    std::optional<std::string> text; // Raw game bytes. A missing name (-1) is distinct from an empty string.
};

class CScriptCallBack {
public:
    virtual ~CScriptCallBack() = default;
    // text and parameters are owned by the executing call and borrowed only
    // during the callback. Hosts retain data by copying it. No raw VM pointers
    // are exposed; a subsequent successful load cannot invalidate a host copy.
    virtual void ReceiveScriptCommand(Word command, Value para1, const std::string& text, Value para3) = 0;
    virtual void PreReceiveScriptData(Word command, Value para1, const std::string& text) = 0;
    virtual void ReceiveUserCommand0(Word command, const std::vector<Value>& parameters) = 0;
    virtual void ReceiveUserFunction0(Word command, const std::vector<Value>& parameters) = 0;
    virtual bool ReceiveUserEvent(Word identify, Word id, const std::vector<Value>& parameters);
    virtual Value GetVarData(Word index) = 0;
    virtual Value GetCalcuVarData(Word index) = 0;
    virtual void SetVarData(Word index, Value value) = 0;
    virtual void SetCalcuVarData(Word index, Value value) { SetVarData(index, value); }
    // A null name means the script supplied -1. An empty, present filename stays distinguishable.
    virtual void PreReceiveOptionalScriptData(Word command, Value kind, const std::string* name) {
        PreReceiveScriptData(command, kind, name ? *name : std::string{});
    }
    virtual void UnhandledInstruction(WordIndex pc, Word identify, Word opcode);
    // All system commands/functions pass through this entry. The default adapts existing callbacks.
    // A false return requests the logged runtime fallback. Yield and PC behavior still belong to the VM.
    virtual bool ReceiveSystemEvent(const SystemEvent& event);
    // Payloads are borrowed only during the callback.
    virtual bool ReceiveNumericScriptCommand(Word, Value, const std::vector<Value>&, Value) { return false; }

    SkipFlags GetSkipFlags() const noexcept { return skip_flags_; }
    void SetSkipFlags(SkipFlags flags) noexcept { skip_flags_ = flags; }
    void ClearSkipFlags() noexcept { skip_flags_ = {}; }

private:
    SkipFlags skip_flags_{};
};

struct ExecResult {
    Status status;
    Word return_code = 0;
    WordIndex pc = 0;
    std::uint64_t work_used = 0;
};

struct CallFrame {
    WordIndex return_pc = 0;
    Word id = 0;
};
struct SavedDialogue {
    WordIndex word=0;
    Word command=0,id=0,table_index=0;
    std::string text;
};
struct SavedUserCommand {
    Word command=0,argument_count=0;
};

struct ScriptSelection {
    std::vector<std::string> options, messages; // Source bytes in display order.
    Value time_limit = 0, auto_select = 0, special = 0, serial = 0;
};

namespace detail { struct Program; }
class ScriptScheduler;

// Single-threaded, non-reentrant VM. The callback must outlive this object.
// Failed loads preserve the prior program, identity, PC and call frames. Every explicit
// LoadFile reads anew. Hosts must stop that launch when a load returns failure.
class CExecScript {
public:
    explicit CExecScript(CScriptCallBack& callback);
    ~CExecScript();
    CExecScript(const CExecScript&) = delete;
    CExecScript& operator=(const CExecScript&) = delete;
    CExecScript(CExecScript&&) = delete;
    CExecScript& operator=(CExecScript&&) = delete;

    Status LoadFile(const std::string& filename, DecodeParameters parameters = {}, const Reader& reader = {},
                    const FunctionRegistry* registry = nullptr,
                    ScriptControlProfile profile = ScriptControlProfile::Closed);
    Status LoadBytes(const std::vector<Byte>& bytes, std::string identity = "memory", DecodeParameters parameters = {},
                     const FunctionRegistry* registry = nullptr,
                     ScriptControlProfile profile = ScriptControlProfile::Closed);
    ExecResult Exec(std::uint64_t work_budget = 10000);
    const ScriptSelection* PendingSelection() const noexcept;
    Status Choose(Word one_based_option,bool timeout=false);
    bool GetString(Word index, std::string& out) const;
    bool GetMessage(Word index, std::string& out) const;
    std::optional<SavedDialogue> saved_dialogue() const;
    std::optional<SavedUserCommand> saved_user_command() const noexcept;
    std::optional<Word> saved_selection_id() const;

    bool IsLoaded() const noexcept;
    WordIndex GetPC() const noexcept;
    const std::string& Identity() const noexcept;
    const Status& LastStatus() const noexcept;
    std::size_t WordCount() const noexcept;
    bool GetData(WordIndex index, Word& value) const noexcept;
    std::size_t GetCallDepth() const noexcept;
    // Bottom-based active-frame index. Failure leaves frame unchanged.
    bool GetCallFrame(std::size_t index, CallFrame& frame) const noexcept;
    bool HasFunctionRegistry() const noexcept;
    std::size_t FunctionRegistrySize() const noexcept;
    Word BoundStopF5Id() const noexcept;
    bool HasForegroundControls() const noexcept;
    // The independent source slot253; failure leaves out unchanged.
    bool GetScriptCallSlot(Word& out) const noexcept;
    // The last executed USER COMMAND's validated adjustment slot (PC stack[254]).
    bool GetCommandCallSlot(Word& out) const noexcept;
    SkipFlags GetSkipFlags() const noexcept;
    void SetSkipFlags(SkipFlags flags) noexcept;

private:
    friend class ScriptScheduler;
    void reset_execution() noexcept;
    bool rebuild_selection();
    bool discard_selection() noexcept;
    Status install_bytes(const std::vector<Byte>& bytes, std::string identity, DecodeParameters parameters,
                         bool has_registry, std::size_t registry_size, Word stop_f5_id,
                         ScriptControlProfile profile);
    CScriptCallBack& callback_;
    std::shared_ptr<const detail::Program> program_;
    WordIndex pc_ = 0;
    std::array<CallFrame, max_call_depth> call_frames_{};
    std::size_t call_depth_ = 0;
    Word script_call_slot_ = 0;
    bool script_call_slot_valid_ = false;
    Word command_call_slot_ = no_word;
    std::unique_ptr<ScriptSelection> selection_;
    WordIndex conditional_entry_ = no_word;
    // A scheduler prepares a candidate without committing its mode/child VM.
    // Load commits preserve this owner hook; bare VMs never install one.
    std::function<Status(Word, const std::string&)> prepare_script_;
    std::string identity_;
    Status last_status_;
    bool executing_ = false;
};

} // namespace nnn
