#pragma once

#include "nnn/script.hpp"
#include "nnn/snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace nnn {

enum class ScriptMode : std::uint8_t { Control = 0, Main = 1, Sub = 2, SubSub = 3 };
inline constexpr std::size_t script_mode_count = 4;
inline constexpr std::size_t max_scheduler_transitions = 1024;

struct SchedulerResult {
    Status status;
    Word return_code = 0;
    ScriptMode executed_mode = ScriptMode::Control;
    ScriptMode active_mode = ScriptMode::Control;
    WordIndex pc = 0; // Last executed VM, which can differ from active_mode after NEXT.
    std::uint64_t work_used = 0;
    std::size_t transitions = 0;
};
struct PcScriptSlot {
    std::string path;
    WordIndex pc=0;
    std::vector<Word> stack;
    Word script_call=no_word,command_call=no_word;
};
enum class PcScriptWait { Dialogue, Selection, Command, Raw };

// Foreground owner only. Host outlives the scheduler. Single-threaded and
// non-reentrant. All slots use the exact same host, not callback proxies.
class ScriptScheduler final {
public:
    explicit ScriptScheduler(CScriptCallBack& callback, Reader reader = {},
                             const FunctionRegistry* registry = nullptr);
    ~ScriptScheduler();
    ScriptScheduler(const ScriptScheduler&) = delete;
    ScriptScheduler& operator=(const ScriptScheduler&) = delete;
    ScriptScheduler(ScriptScheduler&&) = delete;
    ScriptScheduler& operator=(ScriptScheduler&&) = delete;

    // Install a new control (mode0) launch. Success resets all four slots;
    // failure preserves every installed VM and mode. No script is executed.
    Status LoadFile(const std::string& filename, DecodeParameters parameters = {});
    Status LoadBytes(const std::vector<Byte>& bytes, std::string identity = "control",
                     DecodeParameters parameters = {});
    // Decorative scripts restart the validated program without reopening it.
    bool RestartRoot(const std::string& identity) noexcept;
    // One VM yield and at most one owner push/pop; parent is not run after pop.
    SchedulerResult Step(std::uint64_t work_budget = 10000);
    Status Choose(Word one_based_option,bool timeout=false);
    // Resume at the post-SELECT PC without choosing a CASE.
    bool DiscardPendingSelection() noexcept;
    // Scene replay seeks a film in the validated label table of the root script.
    Status SeekFilm(Word zero_based_film);
    // Continue SCRIPT/SUBSCRIPT/NEXT until an ordinary external yield or error,
    // sharing one work budget and a bounded number of owner transitions.
    SchedulerResult Exec(std::uint64_t work_budget = 10000, std::size_t transition_budget = 64);

    bool IsLoaded() const noexcept;
    ScriptMode ActiveMode() const noexcept;
    const CExecScript* GetVM(ScriptMode mode) const noexcept;
    const Status& LastStatus() const noexcept;
    void save_snapshot(snapshot::Writer& out,bool include_command_slots=false,bool save_pending_selection=true) const;
    void load_snapshot(snapshot::Reader& in,const ScriptScheduler* cached=nullptr,bool has_command_slots=false);
    struct SnapshotPosition {
        std::string path;
        WordIndex pc;
        bool selected;
    };
    // Script contents and addresses are checked against game assets on restore.
    static SnapshotPosition inspect_snapshot(snapshot::Reader& in,bool has_command_slots);
    bool pc_adjustment_ids() const noexcept;
    // Read-only, including at the PC boundary within a user callback.
    Status capture_pc_positions(std::array<PcScriptSlot,script_mode_count>& positions,
                                bool adjustment_ids,PcScriptWait wait) const;
    // Restores the legacy address stack or resolves the PC save's adjustment
    // table IDs against freshly validated script programs. Raw keeps the
    // active PC, while parent SCRIPT calls still use the selected stack format.
    Status restore_pc_positions(ScriptMode active,const std::array<PcScriptSlot,script_mode_count>& positions,
                                bool adjustment_ids,PcScriptWait wait,Word wait_id,bool relocate);

private:
    std::unique_ptr<CExecScript> make_vm(ScriptMode mode);
    Status prepare_child(ScriptMode caller, Word code, const std::string& target);
    SchedulerResult step(std::uint64_t work_budget);
    Status load(const std::vector<Byte>* bytes, std::string identity, DecodeParameters parameters);
    SchedulerResult reject(ErrorCode code, const char* detail) const;

    CScriptCallBack& callback_;
    Reader reader_;
    FunctionRegistry registry_;
    bool has_registry_ = false;
    std::array<std::unique_ptr<CExecScript>, script_mode_count> slots_;
    std::unique_ptr<CExecScript> pending_child_;
    ScriptMode mode_ = ScriptMode::Control;
    DecodeParameters parameters_{};
    Status last_status_;
    bool busy_ = false;
};

} // namespace nnn
