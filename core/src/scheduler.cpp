#include "nnn/scheduler.hpp"
#include "nnn/resource_path.hpp"
#include "program.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace nnn {
namespace {
constexpr Word next_code = 0x55560006U;
constexpr Word script_code = 0x5556000aU;
constexpr Word subscript_code = 0x55560016U;
constexpr Word scriptjump_code = 0x55560015U;

std::size_t index_of(ScriptMode mode) noexcept { return static_cast<std::size_t>(mode); }

class OwnerGuard {
public:
    explicit OwnerGuard(bool& busy) : busy_(busy) { busy_ = true; }
    ~OwnerGuard() { busy_ = false; }
    OwnerGuard(const OwnerGuard&) = delete;
    OwnerGuard& operator=(const OwnerGuard&) = delete;
private:
    bool& busy_;
};

bool valid_target(const std::string& target) noexcept {
    return target.size()<=127U && resource::valid_component(target);
}
bool valid_pc_path(const std::string& path) {
    return path.size()>=9 && path.compare(0,4,"spt/")==0 && path.compare(path.size()-4,4,".spt")==0 &&
        valid_target(path.substr(4,path.size()-8));
}
bool instruction_boundary(const detail::Program& p,WordIndex pc) {
    return pc==p.words.size() || (pc<p.record_index.size() && p.record_index[pc]!=no_word);
}
const detail::Record* instruction(const detail::Program& p,WordIndex pc) {
    return pc<p.record_index.size() && p.record_index[pc]!=no_word ? &p.records[p.record_index[pc]] : nullptr;
}
const detail::Record* before_instruction(const detail::Program& p,WordIndex pc) {
    auto it=std::lower_bound(p.records.begin(),p.records.end(),pc,[](const detail::Record& r,WordIndex at){return r.pc<at;});
    return it!=p.records.begin() && (it-1)->pc+(it-1)->length==pc ? &*(it-1) : nullptr;
}
const detail::Record* adjustment(const detail::Program& p,std::size_t table,Word index) {
    return index<p.adjustments[table].size() ? instruction(p,p.adjustments[table][index]) : nullptr;
}
const detail::Record* command_adjustment(const detail::Program& p,Word index) {
    const auto* record=adjustment(p,2,index);
    return record && record->command_call_slot==index ? record : nullptr;
}
const detail::Record* script_call(const detail::Program& p,const PcScriptSlot& saved,bool adjustment_ids) {
    const auto* record=adjustment_ids ? adjustment(p,3,saved.script_call) : before_instruction(p,saved.pc);
    return record && record->identify==detail::identify_control &&
        (record->code==detail::control_script || record->code==detail::control_subscript) ? record : nullptr;
}
const detail::Record* sub_call(const detail::Program& p,Word value,bool adjustment_ids) {
    const auto* record=adjustment_ids ? adjustment(p,0,value) : before_instruction(p,value);
    return record && record->identify==detail::identify_control && record->code==detail::control_sub ? record : nullptr;
}
bool is_dialogue(const detail::Record& r) {
    return r.identify==detail::identify_systemcommand &&
        (r.code==detail::command_print || r.code==detail::command_lprint || r.code==detail::command_append);
}

struct SavedScriptSlot {
    struct Call { Word return_pc=0,id=0; };
    std::string path;
    std::uint64_t fingerprint=0;
    Word pc=0,conditional=no_word,script_call_slot=0,command_call_slot=no_word;
    bool loaded=false,script_call_valid=false,selected=false;
    std::vector<Call> calls;
};

struct SavedScheduler {
    std::uint8_t active=0;
    DecodeParameters parameters;
    std::array<SavedScriptSlot,script_mode_count> slots;
};

SavedScheduler read_scheduler_snapshot(snapshot::Reader& in,bool has_command_slots) {
    SavedScheduler saved;
    in(saved.active,saved.parameters.xor_key,saved.parameters.key_step,saved.parameters.increment_step);
    if (saved.active>=saved.slots.size()) snapshot::invalid("Invalid active script slot");
    for (std::size_t i=0;i<saved.slots.size();++i) {
        auto& slot=saved.slots[i];
        in(slot.loaded);
        if (i<=saved.active && !slot.loaded) snapshot::invalid("Missing active script slot");
        if (!slot.loaded) continue;
        in(slot.path,slot.fingerprint);
        if (!valid_pc_path(slot.path)) snapshot::invalid("Invalid script snapshot path");
        in(slot.pc,slot.conditional,slot.script_call_slot,slot.script_call_valid,slot.selected);
        if (slot.pc==no_word) snapshot::invalid("Invalid script snapshot position");
        if (slot.selected && i!=saved.active) snapshot::invalid("Invalid snapshot selection");
        if (has_command_slots) in(slot.command_call_slot);
        in.records(slot.calls,max_call_depth,[](auto& reader,auto& call){
            reader(call.return_pc,call.id);
            if (call.return_pc==no_word) snapshot::invalid("Invalid snapshot return position");
        });
    }
    return saved;
}
} // namespace

ScriptScheduler::ScriptScheduler(CScriptCallBack& callback, Reader reader, const FunctionRegistry* registry)
    : callback_(callback), reader_(std::move(reader)),
      registry_(registry ? *registry : FunctionRegistry{}), has_registry_(registry != nullptr) {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        slots_[index] = make_vm(static_cast<ScriptMode>(index));
    }
}

ScriptScheduler::~ScriptScheduler() = default;

std::unique_ptr<CExecScript> ScriptScheduler::make_vm(ScriptMode mode) {
    auto vm = std::make_unique<CExecScript>(callback_);
    vm->prepare_script_ = [this, mode](Word code, const std::string& target) {
        return prepare_child(mode, code, target);
    };
    return vm;
}

bool ScriptScheduler::IsLoaded() const noexcept { return slots_[0]->IsLoaded(); }
ScriptMode ScriptScheduler::ActiveMode() const noexcept { return mode_; }
const CExecScript* ScriptScheduler::GetVM(ScriptMode mode) const noexcept {
    const auto index = index_of(mode);
    return index < slots_.size() ? slots_[index].get() : nullptr;
}
const Status& ScriptScheduler::LastStatus() const noexcept { return last_status_; }

void ScriptScheduler::save_snapshot(snapshot::Writer& out,bool include_command_slots,bool save_pending_selection) const {
    if (busy_ || !IsLoaded()) snapshot::invalid("Scheduler snapshot requires a script boundary");
    out(static_cast<std::uint8_t>(mode_),parameters_.xor_key,parameters_.key_step,parameters_.increment_step);
    for (const auto& vm:slots_) {
        const bool loaded=vm->IsLoaded();out(loaded);
        if (!loaded) continue;
        out(vm->identity_,vm->program_->fingerprint,vm->pc_,vm->conditional_entry_,
            vm->script_call_slot_,vm->script_call_slot_valid_,
            vm->selection_!=nullptr && (save_pending_selection || vm.get()!=slots_[index_of(mode_)].get()));
        if (include_command_slots) out(vm->command_call_slot_);
        out.size(vm->call_depth_,max_call_depth);
        for (std::size_t i=0;i<vm->call_depth_;++i) out(vm->call_frames_[i].return_pc,vm->call_frames_[i].id);
    }
}

ScriptScheduler::SnapshotPosition ScriptScheduler::inspect_snapshot(snapshot::Reader& in,bool has_command_slots) {
    auto saved=read_scheduler_snapshot(in,has_command_slots);
    auto& active=saved.slots[saved.active];
    return {std::move(active.path),active.pc,active.selected};
}

void ScriptScheduler::load_snapshot(snapshot::Reader& in,const ScriptScheduler* cached,bool has_command_slots) {
    if (busy_ || !reader_) snapshot::invalid("Scheduler cannot load a snapshot now");
    const auto saved=read_scheduler_snapshot(in,has_command_slots);
    const auto& parameters=saved.parameters;
    const bool reuse=cached && cached->parameters_.xor_key==parameters.xor_key &&
        cached->parameters_.key_step==parameters.key_step && cached->parameters_.increment_step==parameters.increment_step &&
        cached->has_registry_==has_registry_ && cached->registry_.Size()==registry_.Size() &&
        cached->registry_.StopF5Id()==registry_.StopF5Id();
    std::array<std::unique_ptr<CExecScript>,script_mode_count> fresh;
    const auto* registry=has_registry_?&registry_:nullptr;
    for (std::size_t i=0;i<fresh.size();++i) {
        auto vm=make_vm(static_cast<ScriptMode>(i));
        const auto& slot=saved.slots[i];
        if (slot.loaded) {
            const auto& path=slot.path;
            const auto fingerprint=slot.fingerprint;
            if (reuse) for (const auto& previous:cached->slots_) {
                if (previous->program_ && previous->identity_==path && previous->program_->fingerprint==fingerprint) {
                    vm->program_=previous->program_;vm->identity_=path;break;
                }
            }
            if (!vm->IsLoaded()) {
                const auto status=vm->LoadFile(path,parameters,reader_,registry,ScriptControlProfile::Foreground);
                if (!status || vm->program_->fingerprint!=fingerprint) snapshot::invalid("Snapshot script has changed");
            }
            const auto& program=*vm->program_;
            vm->pc_=slot.pc;
            vm->conditional_entry_=slot.conditional;
            vm->script_call_slot_=slot.script_call_slot;
            vm->script_call_slot_valid_=slot.script_call_valid;
            vm->command_call_slot_=slot.command_call_slot;
            if (vm->command_call_slot_!=no_word && !command_adjustment(program,vm->command_call_slot_))
                snapshot::invalid("Invalid snapshot user command adjustment");
            if (!instruction_boundary(program,vm->pc_) ||
                (vm->conditional_entry_!=no_word && !instruction_boundary(program,vm->conditional_entry_)))
                snapshot::invalid("Invalid script snapshot position");
            vm->call_depth_=slot.calls.size();
            for (std::size_t j=0;j<vm->call_depth_;++j) {
                const auto& call=slot.calls[j];
                if (!instruction_boundary(program,call.return_pc)) snapshot::invalid("Invalid snapshot return position");
                vm->call_frames_[j]={call.return_pc,call.id};
            }
            if (slot.selected && !vm->rebuild_selection()) snapshot::invalid("Invalid snapshot selection");
        }
        fresh[i]=std::move(vm);
    }
    slots_.swap(fresh);pending_child_.reset();mode_=static_cast<ScriptMode>(saved.active);parameters_=parameters;last_status_={};
}

bool ScriptScheduler::pc_adjustment_ids() const noexcept {
    Word header_size=0;
    return slots_[0]->GetData(0,header_size) && header_size>=32;
}

Status ScriptScheduler::capture_pc_positions(std::array<PcScriptSlot,script_mode_count>& positions,
                                             bool adjustment_ids,PcScriptWait wait) const {
    if (!IsLoaded()) return {ErrorCode::NotLoaded,no_word,"Cannot capture an unloaded PC script"};
    const auto count=index_of(mode_)+1;
    std::array<PcScriptSlot,script_mode_count> captured;
    for (std::size_t i=0;i<count;++i) {
        const auto& vm=*slots_[i];
        if (!vm.program_ || !valid_pc_path(vm.identity_) || !instruction_boundary(*vm.program_,vm.pc_))
            return {ErrorCode::InvalidFormat,vm.pc_,"Invalid live PC save script position"};
        const auto& program=*vm.program_;
        auto& position=captured[i];
        position.path=vm.identity_;
        position.pc=vm.pc_;
        position.command_call=vm.command_call_slot_;
        vm.GetScriptCallSlot(position.script_call);
        if (i+1<count) {
            const auto* call=script_call(program,position,adjustment_ids);
            if (!call || call->pc+call->length!=vm.pc_)
                return {ErrorCode::InvalidFormat,vm.pc_,"Missing live PC save SCRIPT call slot"};
        } else if (wait==PcScriptWait::Command && adjustment_ids && !command_adjustment(program,position.command_call)) {
            return {ErrorCode::InvalidFormat,vm.pc_,"Missing live PC save command adjustment"};
        }
        position.stack.reserve(vm.call_depth_);
        for (std::size_t j=0;j<vm.call_depth_;++j) {
            const auto& frame=vm.call_frames_[j];
            const auto value=adjustment_ids ? frame.id : frame.return_pc;
            const auto* call=sub_call(program,value,adjustment_ids);
            if (!call || call->pc+call->length!=frame.return_pc)
                return {ErrorCode::InvalidFormat,vm.pc_,"Missing live PC save SUB call frame"};
            position.stack.push_back(value);
        }
    }
    positions=std::move(captured);
    return {};
}

std::optional<SavedDialogue> CExecScript::saved_dialogue() const {
    if (!program_) return {};
    const auto* record=before_instruction(*program_,pc_);
    if (!record || !is_dialogue(*record)) return {};
    const auto& words=program_->words;
    SavedDialogue result{record->pc,record->code,words[record->pc+4],words[record->pc+3],{}};
    return GetMessage(result.table_index,result.text) ? std::optional<SavedDialogue>(std::move(result)) : std::nullopt;
}
std::optional<SavedUserCommand> CExecScript::saved_user_command() const noexcept {
    if (!program_) return {};
    const auto* record=before_instruction(*program_,pc_);
    if (!record || record->identify!=detail::identify_command || record->length<4) return {};
    return SavedUserCommand{record->code,program_->words[record->pc+3]};
}
std::optional<Word> CExecScript::saved_selection_id() const {
    if (!program_ || !selection_) return {};
    const auto* previous=before_instruction(*program_,pc_);
    if (!previous || previous->code!=detail::command_select) return {};
    const auto& table=program_->adjustments[1];
    const auto position=std::find(table.begin(),table.end(),previous->pc);
    return position==table.end() ? std::nullopt : std::optional<Word>{static_cast<Word>(position-table.begin())};
}

Status ScriptScheduler::restore_pc_positions(ScriptMode active,const std::array<PcScriptSlot,script_mode_count>& positions,
                                             bool adjustment_ids,PcScriptWait wait,Word wait_id,bool relocate) {
    const auto invalid=[](WordIndex pc,const char* why){return Status{ErrorCode::InvalidFormat,pc,why};};
    if (busy_ || !reader_) return {ErrorCode::Busy,no_word,"Scheduler cannot import a PC slot now"};
    const auto count=index_of(active)+1;
    if (count>slots_.size()) return invalid(no_word,"Invalid saved active script slot");
    std::array<std::unique_ptr<CExecScript>,script_mode_count> fresh;
    const auto* registry=has_registry_?&registry_:nullptr;
    for (std::size_t i=0;i<fresh.size();++i) {
        auto& vm=fresh[i];
        vm=make_vm(static_cast<ScriptMode>(i));
        if (i>=count) continue;
        const auto& saved=positions[i];
        const auto& path=saved.path;
        if (!valid_pc_path(path)) return invalid(saved.pc,"Invalid PC save script path");
        auto loaded=vm->LoadFile(path,parameters_,reader_,registry,ScriptControlProfile::Foreground);
        if (!loaded) return loaded;
        const auto& program=*vm->program_;
        const auto* command=command_adjustment(program,saved.command_call);
        if (command) vm->command_call_slot_=saved.command_call;
        vm->pc_=saved.pc;
        if (i+1<count) {
            const auto* transfer=script_call(program,saved,adjustment_ids);
            if (!transfer) return invalid(saved.pc,"Invalid PC save SCRIPT call slot");
            vm->pc_=transfer->pc+transfer->length;
            vm->script_call_slot_=program.words[transfer->pc+4];
            vm->script_call_slot_valid_=true;
        } else {
            if (wait==PcScriptWait::Command && adjustment_ids) {
                if (!command) return invalid(saved.pc,"Invalid PC save command adjustment");
                vm->pc_=command->pc+command->length;
            } else if (relocate && wait==PcScriptWait::Dialogue) {
                const auto found=std::find_if(program.records.begin(),program.records.end(),[&](const detail::Record& r){
                    return is_dialogue(r) && program.words[r.pc+4]==wait_id;
                });
                if (found==program.records.end()) return invalid(saved.pc,"PC save message ID no longer exists");
                vm->pc_=found->pc+found->length;
            } else if (relocate && wait==PcScriptWait::Selection) {
                const auto* selection=adjustment(program,1,wait_id);
                if (!selection || selection->code!=detail::command_select)
                    return invalid(saved.pc,"PC save selection ID no longer exists");
                vm->pc_=selection->pc+selection->length;
            }
            if (wait==PcScriptWait::Dialogue) {
                const auto dialogue=vm->saved_dialogue();
                if (!dialogue || dialogue->id!=wait_id) return invalid(vm->pc_,"PC save does not match the script's message");
            } else if (wait==PcScriptWait::Selection) {
                const auto* selection=adjustment(program,1,wait_id);
                if (!selection || selection->pc+selection->length!=vm->pc_ || !vm->rebuild_selection())
                    return invalid(vm->pc_,"PC save does not match the script's selection");
            }
        }
        if (!instruction_boundary(program,vm->pc_)) return invalid(vm->pc_,"Invalid PC save script position");
        if (saved.stack.size()>max_call_depth) return invalid(vm->pc_,"PC save SUB stack exceeds capacity");
        for (const auto value:saved.stack) {
            const auto* call=sub_call(program,value,adjustment_ids);
            if (!call) return invalid(vm->pc_,"Invalid PC save SUB call frame");
            vm->call_frames_[vm->call_depth_++]={call->pc+call->length,program.words[call->pc+4]};
        }
    }
    slots_.swap(fresh);pending_child_.reset();mode_=active;last_status_={};
    return {};
}

SchedulerResult ScriptScheduler::reject(ErrorCode code, const char* detail) const {
    const auto* vm = GetVM(mode_);
    const WordIndex pc = vm ? vm->GetPC() : 0;
    return {{code, pc, detail}, 0, mode_, mode_, pc, 0, 0};
}

Status ScriptScheduler::LoadFile(const std::string& filename, DecodeParameters parameters) {
    return load(nullptr, filename, parameters);
}

Status ScriptScheduler::LoadBytes(const std::vector<Byte>& bytes, std::string identity, DecodeParameters parameters) {
    return load(&bytes, std::move(identity), parameters);
}

bool ScriptScheduler::RestartRoot(const std::string& identity) noexcept {
    auto& root=*slots_[0];
    if (busy_ || !root.IsLoaded() || root.Identity()!=identity) return false;
    for (std::size_t i=1;i<slots_.size();++i) {
        slots_[i]->program_.reset();
        slots_[i]->identity_.clear();
        slots_[i]->reset_execution();
    }
    root.reset_execution();
    pending_child_.reset();
    mode_=ScriptMode::Control;
    last_status_={};
    return true;
}

Status ScriptScheduler::load(const std::vector<Byte>* bytes, std::string identity, DecodeParameters parameters) {
    if (busy_) return last_status_ = {ErrorCode::Busy, no_word, "Scheduler operation already active"};
    OwnerGuard guard(busy_);
    try {
        // All allocation and validation happens before the no-throw commit.
        std::array<std::unique_ptr<CExecScript>, script_mode_count> fresh;
        for (std::size_t index = 0; index < fresh.size(); ++index) {
            fresh[index] = make_vm(static_cast<ScriptMode>(index));
        }
        const auto* registry = has_registry_ ? &registry_ : nullptr;
        auto status = bytes
            ? fresh[0]->LoadBytes(*bytes, std::move(identity), parameters, registry, ScriptControlProfile::Foreground)
            : fresh[0]->LoadFile(identity, parameters, reader_, registry, ScriptControlProfile::Foreground);
        if (!status) return last_status_ = std::move(status);
        slots_.swap(fresh);
        pending_child_.reset();
        parameters_ = parameters;
        mode_ = ScriptMode::Control;
        return last_status_ = {};
    } catch (const std::exception& error) {
        return last_status_ = {ErrorCode::ReadFailed, no_word, error.what()};
    } catch (...) {
        return last_status_ = {ErrorCode::ReadFailed, no_word, "Scheduler launch preparation failed"};
    }
}

Status ScriptScheduler::prepare_child(ScriptMode caller, Word code, const std::string& target) {
    const auto index = index_of(caller);
    const WordIndex pc = slots_[index]->GetPC();
    if (!busy_ || caller != mode_ || (code != script_code && code != subscript_code && code != scriptjump_code)) {
        return {ErrorCode::HostRejected, pc, "Script transfer has no active foreground owner"};
    }
    const auto target_mode = code == scriptjump_code ? index : index + 1U;
    if (target_mode >= slots_.size()) {
        return {ErrorCode::HostRejected, pc, "Script call exceeds foreground mode3"};
    }
    if (!reader_) {
        return {ErrorCode::HostRejected, pc, "Script call requires an explicit child Reader"};
    }
    if (!valid_target(target)) return {ErrorCode::HostRejected, pc, "Script target is outside the supported basename profile"};
    auto candidate = make_vm(static_cast<ScriptMode>(target_mode));
    const auto* registry = has_registry_ ? &registry_ : nullptr;
    auto status = candidate->LoadFile("spt/" + target + ".spt", parameters_, reader_, registry,
                                      ScriptControlProfile::Foreground);
    if (!status) return status;
    pending_child_ = std::move(candidate);
    return {};
}

SchedulerResult ScriptScheduler::step(std::uint64_t work_budget) {
    pending_child_.reset();
    const auto executed = mode_;
    const auto index = index_of(executed);
    auto result = slots_[index]->Exec(work_budget);
    SchedulerResult output{std::move(result.status), result.return_code, executed, mode_, result.pc, result.work_used, 0};
    if (!output.status) {
        pending_child_.reset();
        return output;
    }
    if (output.return_code == script_code || output.return_code == subscript_code || output.return_code == scriptjump_code) {
        const auto target_mode = output.return_code == scriptjump_code ? index : index + 1U;
        if (!pending_child_ || target_mode >= slots_.size()) {
            pending_child_.reset();
            output.status = {ErrorCode::HostRejected, output.pc, "Script transfer was not prepared by its owner"};
            return output;
        }
        slots_[target_mode].swap(pending_child_);
        pending_child_.reset();
        mode_ = static_cast<ScriptMode>(target_mode);
        output.transitions = 1;
    } else if (output.return_code == next_code) {
        if (index == 0U) {
            return output; // A caller running a single chapter receives its terminal NEXT.
        } else if (!slots_[index - 1U]->IsLoaded()) {
            output.status = {ErrorCode::HostRejected, output.pc, "NEXT parent is not loaded"};
        } else {
            mode_ = static_cast<ScriptMode>(index - 1U);
            output.transitions = 1;
        }
    }
    output.active_mode = mode_;
    return output;
}

Status ScriptScheduler::Choose(Word one_based_option,bool timeout) {
    if (busy_) return {ErrorCode::Busy, no_word, "Scheduler operation already active"};
    return last_status_ = slots_[index_of(mode_)]->Choose(one_based_option,timeout);
}

bool ScriptScheduler::DiscardPendingSelection() noexcept {
    return !busy_ && slots_[index_of(mode_)]->discard_selection();
}

Status ScriptScheduler::SeekFilm(Word zero_based_film) {
    if (busy_) return {ErrorCode::Busy,no_word,"Scheduler operation already active"};
    auto* root=slots_[0].get();
    if (!root || !root->IsLoaded()) return {ErrorCode::NotLoaded,no_word,"Scene replay script is not loaded"};
    if (mode_!=ScriptMode::Control || root->call_depth_ || root->selection_)
        return {ErrorCode::HostRejected,root->pc_,"Film seek requires a root script boundary"};
    const auto& program=*root->program_;
    if (zero_based_film>=program.film_koma_offsets.size())
        return {ErrorCode::InvalidFormat,root->pc_,"Film is outside the script label table"};
    root->pc_=program.labels[program.words[10]+zero_based_film];
    root->conditional_entry_=no_word;
    return {};
}

SchedulerResult ScriptScheduler::Step(std::uint64_t work_budget) {
    if (busy_) {
        auto result = reject(ErrorCode::Busy, "Scheduler operation already active");
        last_status_ = result.status;
        return result;
    }
    OwnerGuard guard(busy_);
    try {
        auto result = step(work_budget);
        last_status_ = result.status;
        return result;
    } catch (const std::exception& error) {
        pending_child_.reset();
        auto result = reject(ErrorCode::CallbackFailed, error.what());
        last_status_ = result.status;
        return result;
    } catch (...) {
        pending_child_.reset();
        auto result = reject(ErrorCode::CallbackFailed, "Scheduler execution failed");
        last_status_ = result.status;
        return result;
    }
}

SchedulerResult ScriptScheduler::Exec(std::uint64_t work_budget, std::size_t transition_budget) {
    if (busy_) {
        auto result = reject(ErrorCode::Busy, "Scheduler operation already active");
        last_status_ = result.status;
        return result;
    }
    if (transition_budget == 0U || transition_budget > max_scheduler_transitions) {
        auto result = reject(transition_budget == 0U ? ErrorCode::BudgetExhausted : ErrorCode::InvalidFormat,
                             "Scheduler transition budget must be in 1..1024");
        last_status_ = result.status;
        return result;
    }
    OwnerGuard guard(busy_);
    std::uint64_t used = 0;
    std::size_t transitions = 0;
    SchedulerResult last_step;
    bool have_step = false;
    try {
        for (;;) {
            if (used >= work_budget || transitions >= transition_budget) {
                auto result = have_step ? std::move(last_step)
                                        : reject(ErrorCode::BudgetExhausted, "Scheduler work or transition budget exhausted");
                result.status = {ErrorCode::BudgetExhausted, result.pc, "Scheduler work or transition budget exhausted"};
                result.work_used = used;
                result.transitions = transitions;
                last_status_ = result.status;
                return result;
            }
            auto result = step(work_budget - used);
            const bool transferred = result.transitions != 0;
            used += result.work_used;
            transitions += result.transitions;
            result.work_used = used;
            result.transitions = transitions;
            if (!result.status || !transferred) {
                last_status_ = result.status;
                return result;
            }
            last_step = std::move(result);
            have_step = true;
        }
    } catch (const std::exception& error) {
        pending_child_.reset();
        auto result = reject(ErrorCode::CallbackFailed, error.what());
        result.work_used = used;
        result.transitions = transitions;
        last_status_ = result.status;
        return result;
    } catch (...) {
        pending_child_.reset();
        auto result = reject(ErrorCode::CallbackFailed, "Scheduler execution failed");
        result.work_used = used;
        result.transitions = transitions;
        last_status_ = result.status;
        return result;
    }
}

} // namespace nnn
