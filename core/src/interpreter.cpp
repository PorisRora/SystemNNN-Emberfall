// SystemNNN SPT foreground VM. Word offsets, callback order and file-transfer yields follow execscript.cpp.
#include "program.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <utility>

namespace nnn {
namespace {
using namespace detail;

struct Failure { Status status; };
[[noreturn]] void invalid(WordIndex pc, const char* detail) {
    throw Failure{{ErrorCode::InvalidFormat, pc, detail}};
}

const Record& at(const Program& program, WordIndex pc) {
    if (pc >= program.record_index.size() || program.record_index[pc] == no_word)
        invalid(pc, "PC does not name a record");
    return program.records[program.record_index[pc]];
}

WordIndex after_body(const Program& program, WordIndex pc) {
    const auto& r = at(program, pc);
    if (r.identify != identify_control || r.code != control_open) return pc + r.length;
    if (r.length < 4) invalid(pc, "Missing block extent");
    const Word span = program.words[pc+3];
    if (span < r.length || span > program.words.size()-pc) invalid(pc, "Block exceeds script");
    const WordIndex next = pc + span;
    if (next < program.words.size()) at(program,next);
    return next;
}

Value arithmetic(Word op, Value a, Value b, WordIndex pc) {
    const Word left = static_cast<Word>(a), right = static_cast<Word>(b);
    switch (op) {
    case '+': return value_from_word(left + right);
    case '-': return value_from_word(left - right);
    case '*': return value_from_word(left * right);
    case '/': return b ? value_from_word(static_cast<Word>(static_cast<std::int64_t>(a) / b)) : 0;
    case '%': return b ? static_cast<Value>(static_cast<std::int64_t>(a) % b) : 0;
    case '&': return value_from_word(left & right);
    case '|': return value_from_word(left | right);
    case '^': return value_from_word(left ^ right);
    default: invalid(pc,"Unknown arithmetic operator");
    }
}

struct Operand {
    Word token = 0;
    Value value = 0;
    bool computed = false;
};

class Evaluator {
    const Program& program_;
    CScriptCallBack& host_;
    Value operand(const Operand& v, WordIndex pc, bool single = false) const {
        if (v.computed) return v.value;
        const Word tag = v.token & 0xffff0000U, id = v.token & 0xffffU;
        if (tag == token_var) {
            if (!single && (id & 0x8000U)) return static_cast<Value>(id & 0x7fffU);
            return single || id < 100 ? host_.GetCalcuVarData(id) : host_.GetVarData(id);
        }
        if (tag == token_number) return short_value(v.token);
        if (tag == token_number32) {
            if (id >= program_.constants.size()) invalid(pc,"INT32 index outside table");
            return program_.constants[id];
        }
        if ((tag >> 28) == 12 || (tag >> 28) == 13) return value_from_word(v.token);
        invalid(pc,"Invalid expression operand");
    }
    void assign(const Operand& destination, Value value, WordIndex pc) {
        if (destination.computed || (destination.token & 0xffff0000U) != token_var)
            invalid(pc,"Assignment requires a variable");
        const Word id = destination.token & 0xffffU;
        if (id < 100) host_.SetCalcuVarData(id,value);
        else if (id < 32768) host_.SetVarData(id,value);
        else invalid(pc,"Assignment to a variable-number literal");
    }
public:
    Evaluator(const Program& p, CScriptCallBack& host) : program_(p), host_(host) {}
    Value read(WordIndex& cursor, WordIndex end) {
        const auto& words = program_.words;
        if (cursor >= end) invalid(cursor,"Missing expression");
        const Word first = words[cursor++];
        if (first != token_expression) return operand({first},cursor-1,true);
        if (cursor >= end) invalid(cursor,"Missing RPN length");
        const Word count = words[cursor++];
        if (!count || count > end - cursor) invalid(cursor-1,"Invalid RPN extent");
        const WordIndex limit = cursor + count;
        std::vector<Operand> stack;
        stack.reserve(std::min<Word>(count,256));
        while (cursor < limit) {
            const WordIndex pc = cursor++;
            const Word token = words[pc], kind = token & 0xffff0000U, op = token & 0xffffU;
            if ((token >> 28) != 10) {
                stack.push_back({token});
                continue;
            }
            if (kind == 0xa0000000U && op == '!') {
                if (stack.empty()) invalid(pc,"Missing unary operand");
                auto& v = stack.back();
                const Value result = !operand(v,pc);
                if (!v.computed && (v.token & 0xffff0000U) == token_var) assign(v,result,pc);
                else v = {0,result,true};
                continue;
            }
            if (stack.size()<2) invalid(pc,"Missing binary operand");
            const Operand right = stack.back();
            stack.pop_back();
            auto& left = stack.back();
            if (kind == 0xa0000000U && op == '=') {
                assign(left,operand(right,pc),pc);
                continue;
            }
            const Value a = operand(left,pc), b = operand(right,pc);
            Value value = 0;
            switch (kind) {
            case 0xa0000000U: value = arithmetic(op,a,b,pc); break;
            case 0xa2000000U:
                if (op == '=') value = (a == b);
                else {
                    assign(left,op=='!' ? !b : arithmetic(op,a,b,pc),pc);
                    continue;
                }
                break;
            case 0xa3000000U:
                switch (op) {
                case '=': value = a == b; break;
                case '&': value = a && b; break;
                case '|': value = a || b; break;
                case '^': value = (a != 0) != (b != 0); break;
                default: invalid(pc,"Unknown logical operator");
                }
                break;
            case 0xa4000000U:
                if (op=='<') value = a < b;
                else if (op=='>') value = a > b;
                else invalid(pc,"Unknown comparison");
                break;
            case 0xa5000000U:
                switch (op) {
                case '<': value = a <= b; break;
                case '>': value = a >= b; break;
                case '=': value = a == b; break;
                case '!': value = a != b; break;
                default: invalid(pc,"Unknown comparison");
                }
                break;
            default: invalid(pc,"Unknown RPN operator class");
            }
            left = {0,value,true};
        }
        if (stack.size()!=1) invalid(cursor,"RPN did not produce one result");
        return operand(stack.back(),cursor-1);
    }
};

struct ExecutionGuard {
    bool& flag;
    explicit ExecutionGuard(bool& value) : flag(value) { flag=true; }
    ~ExecutionGuard() { flag=false; }
};

std::vector<Value> numbers(const Program& p, WordIndex first, Word count) {
    std::vector<Value> values;
    values.reserve(count);
    for (Word i=0; i<count; ++i) values.push_back(value_from_word(p.words[first+i]));
    return values;
}

std::unique_ptr<ScriptSelection> selection(const Program& p, const Record& r, std::vector<Value>& args) {
    const auto& w = p.words;
    const WordIndex pc = r.pc, end = pc+r.length;
    const Word options = w[pc+3];
    // Older writers end with just the adjustment ID. Newer ones append prompt count and timing fields.
    const Word prompts = r.length==options+5 ? 0 : w[end-1];
    if (!options || prompts>r.length-5-options) invalid(pc,"Invalid selection size");
    const Word total = options+prompts;
    auto out = std::make_unique<ScriptSelection>();
    args = {static_cast<Value>(options),static_cast<Value>(prompts)};
    for (Word i=0; i<total; ++i) {
        const Word index = w[pc+4+i];
        std::string message;
        if (!p.strings.get(index,message)) invalid(pc,"Selection string index outside table");
        if (i<prompts) {
            // The compiler prefixes additional prompt lines with a marker byte.
            if (!message.empty()) message.erase(0,1);
            out->messages.push_back(std::move(message));
        } else out->options.push_back(std::move(message));
        args.push_back(value_from_word(index));
    }
    if (r.length>total+5) {
        out->time_limit=value_from_word(w[end-2]);
        out->auto_select=value_from_word(w[end-3]);
    }
    if (r.length>total+6) out->special=value_from_word(w[end-4]);
    if (r.length>total+7) out->serial=value_from_word(w[end-5]);
    args.insert(args.end(),{out->time_limit,out->auto_select,out->special,out->serial});
    return out;
}
// Return the VM yield for this record. The host can consume or decline each event independently.
Word system_event(const Program& p, const Record& r, CScriptCallBack& host,
                  std::unique_ptr<ScriptSelection>& pending) {
    const WordIndex pc = r.pc;
    const auto& words = p.words;
    const bool function = r.identify == identify_systemfunction;
    SystemEvent event;
    event.identify = r.identify;
    event.opcode = r.code;
    Word yield = 0;
    std::unique_ptr<ScriptSelection> choices;
    if (!function && (r.code == command_print || r.code == command_lprint || r.code == command_append)) {
        event.payload = SystemPayload::Dialogue;
        event.parameters = numbers(p, pc + 3, r.length - 3);
        event.primary = event.parameters[1];
        if (event.parameters.size() > 2) event.trailing = event.parameters[2];
        if (!p.messages.get(words[pc+3], event.text.emplace())) invalid(pc, "Missing dialogue");
        yield = command_print;
    } else if (!function && r.code == command_select) {
        event.payload = SystemPayload::Selection;
        choices = selection(p, r, event.parameters);
        event.primary = static_cast<Value>(event.parameters.size());
        yield = command_select;
    } else if (function && has_string_argument(r.code)) {
        event.payload = SystemPayload::Named;
        event.primary = (r.code == function_storyname || r.code == function_filmname) ? 1 : value_from_word(words[pc+3]);
        event.parameters = numbers(p, pc+4, r.length-4);
        if (words[pc+4] != UINT32_MAX && !p.strings.get(words[pc+4], event.text.emplace()))
            invalid(pc, "Missing script string");
    } else {
        const Word count = words[pc+3];
        event.primary = static_cast<Value>(count);
        event.parameters = numbers(p, pc+4, count);
        if (r.length-4 > count) event.trailing = value_from_word(words[pc+4+count]);
        if (function && r.code == function_voice) {
            if (count && words[pc+4] != UINT32_MAX && !p.strings.get(words[pc+4], event.text.emplace()))
                invalid(pc, "Missing voice name");
            host.PreReceiveOptionalScriptData(r.code, 1, event.text ? &*event.text : nullptr);
        }
        // Adapt the original scalar callback signature while retaining every physical argument above.
        if (function && (r.code == function_filmtypetime || r.code == function_prepareoverrap || r.code == function_nomessagewindow)) {
            event.payload = SystemPayload::Scalar;
            event.primary = event.parameters.empty() ? 0 : event.parameters.front();
        }
        if (!function && (r.code == command_overrap ||
            (r.code == command_draw && !event.parameters.empty() && event.parameters.front() != 0))) yield = r.code;
    }
    if (!host.ReceiveSystemEvent(event)) host.UnhandledInstruction(pc, r.identify, r.code);
    if (choices) pending = std::move(choices);
    return yield;
}
} // namespace

bool CScriptCallBack::ReceiveSystemEvent(const SystemEvent& event) {
    if (event.payload == SystemPayload::Numeric || event.payload == SystemPayload::Selection)
        return ReceiveNumericScriptCommand(event.opcode, event.primary, event.parameters, event.trailing.value_or(0));
    static const std::string empty;
    ReceiveScriptCommand(event.opcode, event.primary, event.text ? *event.text : empty, event.trailing.value_or(0));
    return true;
}

bool CScriptCallBack::ReceiveUserEvent(Word identify, Word id, const std::vector<Value>& parameters) {
    if (identify == detail::identify_command) ReceiveUserCommand0(id,parameters);
    else ReceiveUserFunction0(id,parameters);
    return true;
}

void CScriptCallBack::UnhandledInstruction(WordIndex pc, Word identify, Word opcode) {
    std::fprintf(stderr,"SystemNNN: skipped unhandled instruction %08x/%08x at word %u\n",
                 static_cast<unsigned>(identify),static_cast<unsigned>(opcode),static_cast<unsigned>(pc));
}

bool CExecScript::GetString(Word index, std::string& out) const { return program_ && program_->strings.get(index,out); }
bool CExecScript::GetMessage(Word index, std::string& out) const { return program_ && program_->messages.get(index,out); }
const ScriptSelection* CExecScript::PendingSelection() const noexcept { return selection_.get(); }

bool CExecScript::discard_selection() noexcept {
    if (executing_ || !selection_) return false;
    selection_.reset();
    return true;
}

bool CExecScript::rebuild_selection() {
    if (!program_) return false;
    const auto& records=program_->records;
    auto at_pc=std::lower_bound(records.begin(),records.end(),pc_,[](const detail::Record& r,WordIndex pc){return r.pc<pc;});
    if (at_pc==records.begin()) return false;
    const auto& previous=*--at_pc;
    if (previous.pc+previous.length!=pc_ || previous.identify!=detail::identify_systemcommand || previous.code!=detail::command_select)
        return false;
    try {
        std::vector<Value> args;
        selection_=selection(*program_,previous,args);
        return true;
    } catch (const Failure&) { return false; }
}

Status CExecScript::Choose(Word option,bool timeout) {
    if (executing_) return {ErrorCode::Busy,pc_,"Cannot select during a callback"};
    if (!program_ || !selection_ || (!timeout && (option==0 || option>selection_->options.size())))
        return {ErrorCode::InvalidFormat,pc_,"No such pending option"};
    const auto& p = *program_;
    try {
        WordIndex cursor = pc_;
        const auto& first = at(p,cursor);
        if (first.identify==identify_control && first.code==control_open) cursor += first.length;
        while (cursor < p.words.size()) {
            const auto& r = at(p,cursor);
            if (r.identify!=identify_control || r.code!=control_case) break;
            if (r.length<4) invalid(cursor,"Missing CASE number");
            if ((p.words[cursor+3] & 0xffffU)==option) {
                pc_=cursor+r.length;
                selection_.reset();
                return last_status_={};
            }
            cursor=after_body(p,cursor+r.length);
        }
        if (timeout) {
            // The PC's GetCase returns the end of the CASE list when the
            // script's automatic option has no matching branch.
            pc_=cursor; selection_.reset();
            return last_status_={};
        }
        return last_status_={ErrorCode::InvalidFormat,pc_,"Selection has no matching CASE"};
    } catch (const Failure& e) { return last_status_=e.status; }
}

ExecResult CExecScript::Exec(std::uint64_t work_budget) {
    using namespace detail;
    std::uint64_t used=0;
    const auto finish = [&](Status s,Word code=0) {
        last_status_=std::move(s);
        return ExecResult{last_status_,code,pc_,used};
    };
    if (executing_) return finish({ErrorCode::Busy,pc_,"Reentrant execution"});
    if (!program_) return finish({ErrorCode::NotLoaded,no_word,"No script loaded"});
    if (selection_) return finish({},command_select);
    ExecutionGuard guard(executing_);
    auto& p=*program_;
    const auto& w=p.words;
    Evaluator eval(p,callback_);
    try {
        while (pc_<w.size()) {
            const auto& r=at(p,pc_);
            const WordIndex end=pc_+r.length;
            // Charge before invoking the host, so yielding for budget never repeats a partial instruction.
            const std::uint64_t cost=r.identify==identify_data ? 1U : r.length;
            if (cost>work_budget-used) return finish({ErrorCode::BudgetExhausted,pc_,"Execution work budget exhausted"});
            used+=cost;
            const WordIndex loop=p.loop_back[p.record_index[pc_]];
            const WordIndex next=loop==no_word ? end : loop;
            if (r.identify==identify_data) { pc_=end; continue; }
            if (r.identify==identify_control) {
                switch (r.code) {
                case control_end: case control_next: return finish({},r.code);
                case control_exit: case control_endkoma: case control_endfilm:
                    pc_=next; return finish({},r.code);
                case control_nop: case control_open: case control_close: case control_endif:
                    pc_=next; continue;
                case control_if: case control_elsif: case control_while: {
                    if (r.code==control_elsif && pc_!=conditional_entry_) {
                        pc_=after_body(p,end);
                        continue;
                    }
                    WordIndex cursor=pc_+3;
                    const Value test=eval.read(cursor,end);
                    if (cursor!=end) invalid(pc_,"Trailing condition expression");
                    conditional_entry_=no_word;
                    if (test>0) { pc_=end; continue; }
                    pc_=after_body(p,end);
                    if (r.code!=control_while && pc_<w.size()) {
                        const auto& sibling=at(p,pc_);
                        if (sibling.identify==identify_control && sibling.code==control_else) pc_+=sibling.length;
                        else if (sibling.identify==identify_control && sibling.code==control_elsif) conditional_entry_=pc_;
                    }
                    continue;
                }
                case control_else: case control_case: pc_=after_body(p,end); continue;
                case control_goto:
                    if (r.length<4) invalid(pc_,"Missing GOTO target");
                    at(p,w[pc_+3]); pc_=w[pc_+3]; continue;
                case control_sub: {
                    if (r.length<5 || w[pc_+3]>=p.labels.size()) invalid(pc_,"Invalid SUB label");
                    if (call_depth_>=call_frames_.size()) return finish({ErrorCode::CallStackOverflow,pc_,"SUB stack limit"});
                    call_frames_[call_depth_++]={next,w[pc_+4]};
                    pc_=p.labels[w[pc_+3]]; continue;
                }
                case control_return:
                    if (!call_depth_) return finish({ErrorCode::CallStackUnderflow,pc_,"RETURN without SUB"});
                    pc_=call_frames_[--call_depth_].return_pc; continue;
                case control_script: case control_subscript: case control_scriptjump: {
                    if (r.length<(r.code==control_scriptjump ? 4U : 5U)) invalid(pc_,"Incomplete script transfer");
                    std::string target;
                    if (!p.strings.get(w[pc_+3],target)) invalid(pc_,"Script transfer name outside table");
                    if (prepare_script_) {
                        auto status=prepare_script_(r.code,target);
                        if (!status) return finish(std::move(status));
                    }
                    callback_.ReceiveScriptCommand(r.code,0,target,0);
                    if (r.code!=control_scriptjump) { script_call_slot_=w[pc_+4]; script_call_slot_valid_=true; }
                    pc_=next; return finish({},r.code);
                }
                default: callback_.UnhandledInstruction(pc_,r.identify,r.code); pc_=next; continue;
                }
            }
            if (r.identify==identify_calcu) {
                WordIndex cursor=pc_+3;
                eval.read(cursor,end);
                if (cursor!=end) invalid(pc_,"Trailing calculation data");
            } else if (r.identify==identify_function || r.identify==identify_command) {
                if (r.length<4) invalid(pc_,"Missing user argument count");
                const Word count=w[pc_+3];
                if (count>r.length-4) invalid(pc_,"User arguments exceed record");
                WordIndex cursor=pc_+4;
                std::vector<Value> args;
                args.reserve(count);
                for (Word i=0; i<count; ++i) args.push_back(eval.read(cursor,end));
                const bool command=r.identify==identify_command;
                if (cursor!=end && !(command && end-cursor==1)) invalid(pc_,"Unexpected user record tail");
                if (command) {
                    command_call_slot_=r.command_call_slot;
                    pc_=next;
                    if (!callback_.ReceiveUserEvent(r.identify,r.code,args))
                        callback_.UnhandledInstruction(r.pc,r.identify,r.code);
                    return finish({},user_command_return);
                }
                if ((p.has_function_registry && r.code >= p.function_registry_size) ||
                    !callback_.ReceiveUserEvent(r.identify,r.code,args))
                    callback_.UnhandledInstruction(pc_,r.identify,r.code);
                if (p.has_function_registry && r.code==p.stop_f5_id) callback_.ClearSkipFlags();
            } else if (r.identify==identify_systemfunction || r.identify==identify_systemcommand) {
                const Word yield = system_event(p, r, callback_, selection_);
                if (yield) {
                    pc_ = yield == command_select ? end : next;
                    return finish({}, yield);
                }
            } else callback_.UnhandledInstruction(pc_,r.identify,r.code);
            pc_=next;
        }
        return finish({},control_end);
    } catch (const Failure& e) { return finish(e.status); }
    catch (const std::exception& e) { return finish({ErrorCode::CallbackFailed,pc_,e.what()}); }
    catch (...) { return finish({ErrorCode::CallbackFailed,pc_,"Script host threw"}); }
}
} // namespace nnn
