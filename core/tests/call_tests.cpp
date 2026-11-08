#include "nnn/script.hpp"
#include "calls/fixtures/generated_cases.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using nnn::Byte;
using nnn::Word;
using nnn::Value;
using nnn::ErrorCode;
constexpr Word data = 0x66660001U;
constexpr Word control = 0x66660002U;
constexpr Word system_command = 0x66660006U;
constexpr Word system_function = 0x66660007U;
constexpr Word end_code = 0x55560005U;
constexpr Word return_code = 0x55560007U;
constexpr Word sub_code = 0x55560009U;
constexpr Word print_code = 0x22220001U;
constexpr Word story_code = 0x33330051U;
constexpr Word film_code = 0x33330050U;
constexpr Word prepare_code = 0x33330011U;
constexpr Word no_message_code = 0x33330017U;
constexpr Word start_koma_code = 0x3333001cU;
constexpr Word table_label = 0x44440003U;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void status_is(const nnn::Status& status, ErrorCode expected, const std::string& context) {
    require(status.code == expected, context + ": expected " + nnn::error_name(expected) +
            ", got " + nnn::error_name(status.code) + " (" + status.detail + ")");
}
std::string quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\') out << '\\' << character;
        else if (byte < 0x20U || byte >= 0x80U)
            out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte) << std::dec;
        else out << character;
    }
    out << '"';
    return out.str();
}
bool frame_equal(const nnn::CallFrame& a, const nnn::CallFrame& b) {
    return a.return_pc == b.return_pc && a.id == b.id;
}
std::vector<nnn::CallFrame> frames(const nnn::CExecScript& vm) {
    std::vector<nnn::CallFrame> result;
    for (std::size_t index = 0; index < vm.GetCallDepth(); ++index) {
        nnn::CallFrame frame{};
        require(vm.GetCallFrame(index, frame), "active frame cannot be inspected");
        result.push_back(frame);
    }
    return result;
}
void frames_are(const nnn::CExecScript& vm, const std::vector<nnn::CallFrame>& expected) {
    require(vm.GetCallDepth() == expected.size(), "call depth mismatch");
    const auto actual = frames(vm);
    for (std::size_t index = 0; index < actual.size(); ++index)
        require(frame_equal(actual[index], expected[index]), "call frame/ID mismatch at " + std::to_string(index));
}
struct State { Word pc; std::vector<nnn::CallFrame> stack; };
struct Callback final : nnn::CScriptCallBack {
    nnn::CExecScript* vm = nullptr;
    std::map<Word, Value> variables;
    std::vector<std::string> events;
    std::vector<State> states;
    std::vector<std::tuple<Word,Word,Word>> skipped;
    void UnhandledInstruction(Word pc,Word identify,Word code) override { skipped.emplace_back(pc,identify,code); }
    std::function<void(Word)> on_script;
    void observe() {
        require(vm != nullptr, "callback VM missing");
        states.push_back({vm->GetPC(), frames(*vm)});
    }
    void ReceiveScriptCommand(Word code, Value para1, const std::string& text, Value para3) override {
        observe();
        events.push_back("{\"kind\":\"script\",\"code\":" + std::to_string(code) +
            ",\"para1\":" + std::to_string(para1) + ",\"text_ascii\":" + quote(text) +
            ",\"para3\":" + std::to_string(para3) + '}');
        if (on_script) on_script(code);
    }
    void PreReceiveScriptData(Word, Value, const std::string&) override { throw std::runtime_error("unexpected pre-data callback"); }
    void ReceiveUserCommand0(Word code, const std::vector<Value>& parameters) override {
        observe();
        std::string event = "{\"kind\":\"user_command\",\"code\":" + std::to_string(code) + ",\"parameters\":[";
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index != 0) event += ',';
            event += std::to_string(parameters[index]);
        }
        events.push_back(event + "]}");
    }
    void ReceiveUserFunction0(Word, const std::vector<Value>&) override { throw std::runtime_error("unexpected user-function callback"); }
    Value GetVarData(Word index) override {
        observe(); const Value value = variables[index];
        events.push_back("{\"kind\":\"get_var\",\"index\":" + std::to_string(index) + ",\"value\":" + std::to_string(value) + '}');
        return value;
    }
    Value GetCalcuVarData(Word index) override {
        observe(); const Value value = variables[index];
        events.push_back("{\"kind\":\"get_calcu_var\",\"index\":" + std::to_string(index) + ",\"value\":" + std::to_string(value) + '}');
        return value;
    }
    void SetVarData(Word index, Value value) override {
        observe(); variables[index] = value;
        events.push_back("{\"kind\":\"set_var\",\"index\":" + std::to_string(index) + ",\"value\":" + std::to_string(value) + '}');
    }
};
struct Code {
    std::vector<Word> words;
    // Compiler registry slots are shared by SUB and GOTO. A GOTO's stored
    // word is an opaque hole, while a SUB's stored word is its source PC.
    std::vector<Word> sub_sites;
    Word pc() const { return 32U + static_cast<Word>(words.size()); }
    Word add(std::initializer_list<Word> values) {
        const Word start = pc(); words.insert(words.end(), values); return start;
    }
    Word story() { return add({5,system_function,story_code,1,0}); }
    Word film() { return add({5,system_function,film_code,1,0}); }
    Word sub(Word label) {
        const auto id = static_cast<Word>(sub_sites.size());
        const Word start = add({5,control,sub_code,label,id}); sub_sites.push_back(start); return start;
    }
    Word jump(Word destination=0, Word hole=0) {
        const Word start=add({4,control,0x55560020U,destination}); sub_sites.push_back(hole); return start;
    }
    void set_jump(Word start,Word destination) { words[start-32U+3U]=destination; }
    Word marker(Word code) { return add({3,control,code}); }
    Word system(Word code) { return add({4,system_function,code,0}); }
    Word print(Word serial = 0) { return add({6,system_command,print_code,0,serial,0}); }
    Word end() { return marker(end_code); }
    Word ret() { return marker(return_code); }
};
struct Model {
    Code code;
    bool profile = true;
    Word story_count = 0;
    std::vector<Word> labels;
    std::vector<Word> offsets;
    std::vector<Word> komas;
};
std::vector<Word> program_words(const Model& model) {
    std::vector<Word> words(32,0);
    words[0]=32; words[1]=data; words[2]=0x55550001U;
    words[12]=0x48545053U; words[13]=0x45444145U; words[14]=0x20203052U; words[15]=0x00202020U;
    words.insert(words.end(),model.code.words.begin(),model.code.words.end());
    const auto table = [&](Word kind, Word field, const std::vector<Word>& payload, bool record_pointer = false) {
        words[field]=static_cast<Word>(words.size()) + (record_pointer ? 0U : 4U);
        words.push_back(4U+static_cast<Word>(payload.size()));
        words.insert(words.end(),{data,0x55550002U,kind});
        words.insert(words.end(),payload.begin(),payload.end());
    };
    if (model.profile) {
        words[8]=static_cast<Word>(model.labels.size()); words[10]=model.story_count;
        table(table_label,9,model.labels);
        table(table_label,16,model.offsets,true);
        table(table_label,17,model.komas,true);
    }
    words[4]=1; words[6]=1;
    const Word message_payload = static_cast<Word>(words.size())+4U;
    table(0x44440002U,5,{message_payload+1U,0x4dU});
    const Word string_payload = static_cast<Word>(words.size())+4U;
    table(0x44440001U,7,{string_payload+1U,0x53U});
    table(0x44440004U,18,{});
    table(0x44440005U,20,model.code.sub_sites);
    table(0x44440006U,21,{}); table(0x44440007U,22,{}); table(0x44440008U,23,{});
    return words;
}
std::vector<Byte> encrypted(const std::vector<Word>& words) {
    std::vector<Byte> bytes;
    for (Word word : words) {
        word ^= UINT32_MAX;
        for (unsigned shift=0; shift<32; shift+=8)
            bytes.push_back(static_cast<Byte>((word >> shift) & 0xffU));
    }
    return bytes;
}
Model base() {
    Model m;
    const Word story=m.code.story(); m.code.sub(1); m.code.print(); m.code.end();
    const Word film1=m.code.film(); const Word prepare=m.code.system(prepare_code);
    const Word no_message=m.code.system(no_message_code); m.code.print(); m.code.ret();
    const Word film2=m.code.film(); m.code.end();
    m.labels={story,film1,film2}; m.story_count=1; m.offsets={0,2}; m.komas={prepare,no_message};
    return m;
}
Model nested() {
    Model m;
    const Word story=m.code.story(); m.code.sub(1); m.code.print(10); m.code.end();
    const Word a=m.code.film(); m.code.sub(2); m.code.print(11); m.code.ret();
    const Word b=m.code.film(); m.code.print(12); m.code.ret();
    m.labels={story,a,b}; m.story_count=1; m.offsets={0,0}; return m;
}
Model chain(std::size_t depth) {
    Model m;
    m.labels.push_back(m.code.story()); m.story_count=1;
    m.code.sub(1); m.code.end();
    for (std::size_t index=0; index<depth; ++index) {
        m.labels.push_back(m.code.film()); m.offsets.push_back(0);
        if (index+1U<depth) m.code.sub(static_cast<Word>(index)+2U);
        else m.code.print(99);
        m.code.ret();
    }
    return m;
}
Model goto_before(Word hole=UINT32_MAX) {
    Model m; m.labels={m.code.story()}; m.story_count=1;
    const Word jump=m.code.jump(0,hole); const Word call=m.code.sub(1); m.code.set_jump(jump,call);
    m.code.print(); m.code.end(); m.labels.push_back(m.code.film()); m.code.print(); m.code.ret(); m.offsets={0};
    return m;
}
void load(nnn::CExecScript& vm, const Model& m, const std::string& name="case") {
    status_is(vm.LoadBytes(encrypted(program_words(m)),name),ErrorCode::Ok,"call load");
}
nnn::ExecResult exec(nnn::CExecScript& vm, Word result, std::uint64_t budget=100000) {
    const auto actual=vm.Exec(budget); status_is(actual.status,ErrorCode::Ok,"call Exec");
    require(actual.return_code==result,"call return code mismatch"); return actual;
}
void reject(const std::vector<Word>& words, ErrorCode expected=ErrorCode::InvalidFormat) {
    Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm;
    status_is(vm.LoadBytes(encrypted(words)),expected,"rejected call profile");
    require(!vm.IsLoaded() && cb.events.empty() && vm.GetCallDepth()==0,"bad input installed or called host");
}
struct Suite {
    int passed=0; int failed=0;
    void test(const std::string& name,const std::function<void()>& body) {
        try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
};
} // namespace

int main() {
    Suite suite;
    suite.test("public call-stack contract",[] {
        require(nnn::max_call_depth==249U,"source permits exactly 249 active calls");
        require(static_cast<int>(ErrorCode::Busy)==10,"existing error ordinals changed");
        require(std::string(nnn::error_name(ErrorCode::CallStackUnderflow))=="CallStackUnderflow","underflow name");
        require(std::string(nnn::error_name(ErrorCode::CallStackOverflow))=="CallStackOverflow","overflow name");
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm;
        nnn::CallFrame sentinel{123,456};
        require(vm.GetCallDepth()==0 && !vm.GetCallFrame(0,sentinel),"empty inspector");
        require(sentinel.return_pc==123 && sentinel.id==456,"failed inspector changed output");
    });
    suite.test("legacy seven tables remain valid",[] {
        Model m; m.profile=false; m.code.end(); Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm;
        load(vm,m); exec(vm,end_code); frames_are(vm,{});
    });
    suite.test("empty three-label profile permits empty RETURN typed error",[] {
        Model m; m.code.ret(); Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,m);
        const auto result=vm.Exec(); status_is(result.status,ErrorCode::CallStackUnderflow,"empty RETURN");
        require(result.pc==32 && result.status.word==32 && cb.events.empty(),"underflow mutated PC/callbacks");
        frames_are(vm,{});
    });
    suite.test("label pointers retain mixed record and payload semantics",[] {
        const Model m=base(); const auto words=program_words(m);
        require(words[words[9]-1U]==table_label && words[words[16]+3U]==table_label && words[words[17]+3U]==table_label,"builder pointer contract");
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,m);
        const auto first=exec(vm,print_code); require(first.pc==70,"callee PRINT post-PC"); frames_are(vm,{{42,0}});
        exec(vm,print_code); frames_are(vm,{}); exec(vm,end_code);
        require(cb.events.size()==6,"source marker and film callbacks missing");
        require(cb.events[2]=="{\"kind\":\"script\",\"code\":858980369,\"para1\":0,\"text_ascii\":\"\",\"para3\":0}","PREPARE callback payload");
        require(cb.events[3]=="{\"kind\":\"script\",\"code\":858980375,\"para1\":0,\"text_ascii\":\"\",\"para3\":0}","NOMESSAGE callback payload");
    });
    suite.test("nested calls save original return PCs and callsite IDs",[] {
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; const Model m=nested(); load(vm,m);
        exec(vm,print_code); frames_are(vm,{{42,0},{61,1}});
        exec(vm,print_code); frames_are(vm,{{42,0}});
        exec(vm,print_code); frames_are(vm,{}); exec(vm,end_code);
        require(cb.states.size()==6,"nested callbacks");
        require(cb.states[2].pc==70 && cb.states[2].stack.size()==2,"FILM callback observes pushed stack/current PC");
    });
    suite.test("consecutive calls reuse popped depth without losing IDs",[] {
        Model m; m.labels.push_back(m.code.story()); m.code.sub(1); m.code.sub(1); m.code.end();
        m.labels.push_back(m.code.film()); m.code.print(); m.code.ret(); m.story_count=1; m.offsets={0};
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,m);
        exec(vm,print_code); frames_are(vm,{{42,0}});
        exec(vm,print_code); frames_are(vm,{{47,1}});
        exec(vm,end_code); frames_are(vm,{});
    });
    suite.test("GOTO before SUB consumes compiler ID slot",[] {
        const Model m=goto_before(); Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,m);
        exec(vm,print_code); frames_are(vm,{{46,1}});
        exec(vm,print_code); frames_are(vm,{}); exec(vm,end_code);
    });
    suite.test("GOTO between SUBs preserves nonconsecutive source IDs",[] {
        Model m; m.labels={m.code.story()}; m.story_count=1;
        m.code.sub(1); const Word jump=m.code.jump(0,UINT32_MAX); const Word second=m.code.sub(1); m.code.set_jump(jump,second);
        m.code.print();m.code.end();m.labels.push_back(m.code.film());m.code.print();m.code.ret();m.offsets={0};
        Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);
        exec(vm,print_code);frames_are(vm,{{42,0}});
        exec(vm,print_code);frames_are(vm,{{51,2}});
        exec(vm,print_code);frames_are(vm,{});exec(vm,end_code);
    });
    suite.test("GOTO after final SUB consumes trailing registry slot",[] {
        Model m; m.labels={m.code.story()};m.story_count=1;m.code.sub(1);
        const Word jump=m.code.jump(0,UINT32_MAX);const Word after=m.code.print();m.code.set_jump(jump,after);m.code.end();
        m.labels.push_back(m.code.film());m.code.print();m.code.ret();m.offsets={0};
        Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);
        exec(vm,print_code);frames_are(vm,{{42,0}});
        exec(vm,print_code);frames_are(vm,{});exec(vm,end_code);
    });
    suite.test("consecutive GOTO holes consume distinct compiler slots",[] {
        Model m;m.labels={m.code.story()};m.story_count=1;
        const Word a=m.code.jump(0,0);const Word b=m.code.jump(0,UINT32_MAX);const Word call=m.code.sub(1);
        m.code.set_jump(a,b);m.code.set_jump(b,call);
        const Word c=m.code.jump(0,43);const Word after=m.code.print();m.code.set_jump(c,after);m.code.end();
        m.labels.push_back(m.code.film());m.code.print();m.code.ret();m.offsets={0};
        Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);
        exec(vm,print_code);frames_are(vm,{{50,2}});
        exec(vm,print_code);frames_are(vm,{});exec(vm,end_code);
    });
    for(const Word hole:{0U,UINT32_MAX,38U,43U,32U,41U,0x80000000U})
        suite.test("opaque GOTO registry hole is not a PC "+std::to_string(hole),[hole] {
            const Model m=goto_before(hole);Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);
            exec(vm,print_code);frames_are(vm,{{46,1}});
            exec(vm,print_code);exec(vm,end_code);frames_are(vm,{});
        });
    suite.test("GOTO-only label profile permits opaque registry slot",[] {
        Model m;const Word jump=m.code.jump(0,UINT32_MAX);const Word stop=m.code.end();m.code.set_jump(jump,stop);
        Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);exec(vm,end_code);frames_are(vm,{});
    });
    suite.test("unreachable GOTO still consumes compiler registry slot",[] {
        Model m;m.labels={m.code.story()};m.story_count=1;const Word stop=m.code.end();m.code.jump(stop,UINT32_MAX);
        Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);exec(vm,end_code);frames_are(vm,{});
    });
    suite.test("249 active calls succeed and unwind in reverse order",[] {
        const Model m=chain(249); Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,m);
        exec(vm,print_code); require(vm.GetCallDepth()==249,"depth 249 rejected");
        for (std::size_t i=0; i<249; ++i) {
            nnn::CallFrame frame{}; require(vm.GetCallFrame(i,frame),"deep inspector");
            require(frame.return_pc==m.code.sub_sites[i]+5U && frame.id==i,"deep fixed frames");
        }
        nnn::CallFrame untouched{7,8}; require(!vm.GetCallFrame(249,untouched),"inactive depth exposed");
        require(untouched.return_pc==7 && untouched.id==8,"out-of-range inspector mutated output");
        exec(vm,end_code); frames_are(vm,{});
    });
    suite.test("250th call rejects before installing an active frame",[] {
        const Model m=chain(250); Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,m);
        const auto actual=vm.Exec(100000); status_is(actual.status,ErrorCode::CallStackOverflow,"250th call");
        require(actual.pc==m.code.sub_sites[249] && actual.status.word==actual.pc && vm.GetCallDepth()==249,"overflow state");
        nnn::CallFrame top{}; require(vm.GetCallFrame(248,top),"overflow retained top");
        require(top.return_pc==m.code.sub_sites[248]+5U && top.id==248,"overflow overwrote active frame");
    });
    suite.test("recursive SUB is bounded by stack capacity",[] {
        Model m; m.labels={m.code.story()}; m.story_count=1; const auto sub=m.code.sub(0); m.code.end();
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,m);
        const auto actual=vm.Exec(100000); status_is(actual.status,ErrorCode::CallStackOverflow,"recursive bound");
        require(actual.pc==sub && vm.GetCallDepth()==249,"recursive overflow depth/PC");
        for (const auto& frame:frames(vm)) require(frame.return_pc==sub+5U && frame.id==0,"recursive ID");
    });
    suite.test("recursive budget exhaustion retains observable active frames",[] {
        Model m; m.labels={m.code.story()}; m.story_count=1; m.code.sub(0); m.code.end();
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,m);
        const auto actual=vm.Exec(24); status_is(actual.status,ErrorCode::BudgetExhausted,"recursive budget");
        require(actual.work_used<=24 && vm.GetCallDepth()>0 && vm.GetCallDepth()<249,"budget did not bound recursion");
    });
    suite.test("END inside callee retains source call state",[] {
        Model m; m.labels={m.code.story()}; m.code.sub(1); m.code.end();
        m.labels.push_back(m.code.film()); const Word stop=m.code.end(); m.story_count=1; m.offsets={0};
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,m);
        require(exec(vm,end_code).pc==stop,"callee END PC"); frames_are(vm,{{42,0}});
    });
    suite.test("zero budget does not pop pending RETURN",[] {
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,base()); exec(vm,print_code);
        const Word pc=vm.GetPC(); const auto actual=vm.Exec(0);
        status_is(actual.status,ErrorCode::BudgetExhausted,"zero budget");
        require(actual.pc==pc && actual.work_used==0,"zero budget PC/work"); frames_are(vm,{{42,0}});
    });
    suite.test("insufficient RETURN transition budget does not pop frame",[] {
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,base()); exec(vm,print_code);
        const Word pc=vm.GetPC(); const auto actual=vm.Exec(1);
        status_is(actual.status,ErrorCode::BudgetExhausted,"RETURN transition budget");
        require(actual.pc==pc && actual.work_used==0,"RETURN transition changed PC/work"); frames_are(vm,{{42,0}});
    });
    suite.test("insufficient SUB transition budget does not push frame",[] {
        Model m; m.code.print(); const Word call=m.code.sub(0); m.code.end();
        m.labels={m.code.film()}; m.code.ret(); m.offsets={0};
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,m); exec(vm,print_code);
        const auto actual=vm.Exec(1); status_is(actual.status,ErrorCode::BudgetExhausted,"SUB transition budget");
        require(actual.pc==call && actual.work_used==0,"SUB transition changed PC/work"); frames_are(vm,{});
    });
    suite.test("successful LoadBytes clears call stack and starts at header",[] {
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; const auto m=base(); load(vm,m,"same"); exec(vm,print_code);
        load(vm,m,"same"); require(vm.GetPC()==0 && vm.Identity()=="same","same-identity load did not restart"); frames_are(vm,{});
    });
    suite.test("successful explicit LoadFile rereads and clears call stack",[] {
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,base()); exec(vm,print_code);
        const auto bytes=encrypted(program_words(base())); int reads=0;
        const nnn::Reader reader=[&](const std::string&) { ++reads; return nnn::ReadResult{{},bytes,bytes.size()}; };
        status_is(vm.LoadFile("reader",{},reader),ErrorCode::Ok,"file restart");
        status_is(vm.LoadFile("reader",{},reader),ErrorCode::Ok,"file reread");
        require(reads==2 && vm.GetPC()==0,"file load cache/restart"); frames_are(vm,{});
    });
    for (const int kind:{0,1,2,3}) suite.test("failed load preserves pending call state "+std::to_string(kind),[kind] {
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,base(),"kept"); exec(vm,print_code);
        const auto pc=vm.GetPC(); const auto count=vm.WordCount(); const auto events=cb.events;
        nnn::Status status;
        if (kind==0) status=vm.LoadBytes({0},"bad");
        else if (kind==1) {
            auto words=program_words(base()); words[9]=0; status=vm.LoadBytes(encrypted(words),"bad");
        } else if (kind==2) status=vm.LoadFile("bad",{},[](const std::string&) { return nnn::ReadResult{{ErrorCode::ReadFailed,nnn::no_word,"reader"},{},0}; });
        else status=vm.LoadFile("bad",{},[](const std::string&) { return nnn::ReadResult{{},{0,0,0,0},8}; });
        require(!static_cast<bool>(status),"bad load succeeded");
        require(vm.GetPC()==pc && vm.WordCount()==count && vm.Identity()=="kept" && cb.events==events,"failed load mutated program/PC/identity/host");
        frames_are(vm,{{42,0}});
    });
    for (const Word throw_on:{film_code,print_code}) suite.test("callback exception retains frame at current source PC "+std::to_string(throw_on),[throw_on] {
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,base());
        cb.on_script=[&](Word code) { if (code==throw_on) throw std::runtime_error("host failure"); };
        const auto actual=vm.Exec(); status_is(actual.status,ErrorCode::CallbackFailed,"callee host failure");
        require(actual.pc==(throw_on==film_code ? 51U : 64U),"exception PC not source callback PC"); frames_are(vm,{{42,0}});
    });
    suite.test("callback reentry is Busy and preserves active frame",[] {
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; const auto bytes=encrypted(program_words(base()));
        load(vm,base()); int reentries=0;
        cb.on_script=[&](Word code) {
            if (code!=film_code) return;
            ++reentries; frames_are(vm,{{42,0}}); const auto pc=vm.GetPC();
            status_is(vm.Exec().status,ErrorCode::Busy,"nested Exec");
            status_is(vm.LoadBytes(bytes,"reentered"),ErrorCode::Busy,"nested LoadBytes");
            int reads=0;
            status_is(vm.LoadFile("reentered",{},[&](const std::string&) { ++reads; return nnn::ReadResult{}; }),ErrorCode::Busy,"nested LoadFile");
            require(reads==0 && vm.GetPC()==pc && vm.Identity()=="case","reentry changed VM"); frames_are(vm,{{42,0}});
        };
        exec(vm,print_code); require(reentries==1,"reentry callback not reached"); frames_are(vm,{{42,0}});
    });
    suite.test("throwing reader preserves frame and releases operation guard",[] {
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,base(),"kept"); exec(vm,print_code);
        const Word pc=vm.GetPC();
        status_is(vm.LoadFile("throws",{},[](const std::string&) -> nnn::ReadResult { throw std::runtime_error("reader failure"); }),ErrorCode::ReadFailed,"reader exception");
        require(vm.GetPC()==pc && vm.Identity()=="kept","reader exception changed PC/identity"); frames_are(vm,{{42,0}});
        load(vm,base(),"new"); frames_are(vm,{});
    });

    struct Bad { const char* name; std::function<void(std::vector<Word>&)> mutate; ErrorCode error=ErrorCode::InvalidFormat; };
    const std::vector<Bad> bad {
        {"main label pointer names record",[](auto& w){w[9]-=4;}},
        {"film offset pointer names payload",[](auto& w){w[16]+=4;}},
        {"koma pointer names payload",[](auto& w){w[17]+=4;}},
        {"film pointer aliases main",[](auto& w){w[16]=w[9]-4;}},
        {"koma pointer aliases film",[](auto& w){w[17]=w[16];}},
        {"main label pointer UINT32_MAX",[](auto& w){w[9]=UINT32_MAX;}},
        {"film offset pointer UINT32_MAX",[](auto& w){w[16]=UINT32_MAX;}},
        {"koma pointer UINT32_MAX",[](auto& w){w[17]=UINT32_MAX;}},
        {"label count short",[](auto& w){--w[8];}},
        {"label count large",[](auto& w){++w[8];}},
        {"label count UINT32_MAX",[](auto& w){w[8]=UINT32_MAX;}},
        {"story count exceeds total",[](auto& w){w[10]=4;}},
        {"story count UINT32_MAX",[](auto& w){w[10]=UINT32_MAX;}},
        {"label target header",[](auto& w){w[w[9]]=0;}},
        {"label target mid-record",[](auto& w){w[w[9]]+=1;}},
        {"label target table",[](auto& w){w[w[9]+2U]=w[9]-4U;}},
        {"label target one past end",[](auto& w){w[w[9]+2U]=static_cast<Word>(w.size());}},
        {"label target UINT32_MAX",[](auto& w){w[w[9]+2U]=UINT32_MAX;}},
        {"duplicate labels",[](auto& w){w[w[9]+1U]=w[w[9]];}},
        {"descending labels",[](auto& w){std::swap(w[w[9]+1U],w[w[9]+2U]);}},
        {"story slot targets FILMNAME",[](auto& w){w[34]=film_code;} ,ErrorCode::Ok},
        {"film slot targets STORYNAME",[](auto& w){w[53]=story_code;} ,ErrorCode::Ok},
        {"label role is PRINT",[](auto& w){w[w[9]]=42;} ,ErrorCode::Ok},
        {"film offsets first nonzero",[](auto& w){w[w[16]+4U]=1;}},
        {"film offsets descending",[](auto& w){w[w[16]+4U]=2;w[w[16]+5U]=1;}},
        {"film offsets above koma count",[](auto& w){w[w[16]+5U]=3;}},
        {"film offsets UINT32_MAX",[](auto& w){w[w[16]+5U]=UINT32_MAX;}},
        {"koma target label role accepted within owning film",[](auto& w){w[w[17]+4U]=51;} ,ErrorCode::Ok},
        {"koma target story role rejected",[](auto& w){w[w[17]+4U]=32;}},
        {"koma target next film label role rejected",[](auto& w){w[w[17]+5U]=73;}},
        {"koma target table",[](auto& w){w[w[17]+5U]=w[9]-4U;}},
        {"koma target mid-record",[](auto& w){w[w[17]+4U]=57;}},
        {"koma target UINT32_MAX",[](auto& w){w[w[17]+5U]=UINT32_MAX;}},
        {"koma targets duplicate",[](auto& w){w[w[17]+5U]=w[w[17]+4U];}},
        {"koma targets descending",[](auto& w){std::swap(w[w[17]+4U],w[w[17]+5U]);}},
        {"koma unverified role PRINT",[](auto& w){w[w[17]+5U]=64;} ,ErrorCode::Ok},
        {"koma unverified role RETURN",[](auto& w){w[w[17]+5U]=70;} ,ErrorCode::Ok},
        {"SUB label out of range",[](auto& w){w[40]=3;}},
        {"SUB label UINT32_MAX",[](auto& w){w[40]=UINT32_MAX;}},
        {"SUB ID out of table",[](auto& w){w[41]=1;}},
        {"SUB ID UINT32_MAX",[](auto& w){w[41]=UINT32_MAX;}},
        {"SUB adjustment names next record",[](auto& w){w[w[20]]=42;}},
        {"SUB adjustment names mid-record",[](auto& w){w[w[20]]=38;}},
        {"SUB adjustment target UINT32_MAX",[](auto& w){w[w[20]]=UINT32_MAX;}},
        {"unreachable unknown control loads",[](auto& w){w[80]=0x5556ffffU;} ,ErrorCode::Ok},
        {"FILMNAME bad string index",[](auto& w){w[55]=1;}},
        {"FILMNAME bad para count",[](auto& w){w[54]=0;},ErrorCode::Ok},
        {"marker nonzero parameter count",[](auto& w){w[59]=1;}},
        {"NOMESSAGE nonzero parameter count",[](auto& w){w[63]=1;}},
        {"unreachable unknown numeric system function",[](auto& w){w[79]=system_function;w[80]=0x3333ffffU;}},
    };
    for (const auto& item:bad) suite.test(item.name,[&] {
        auto words=program_words(base()); item.mutate(words);
        if(item.error!=ErrorCode::Ok) reject(words,item.error);
        else {
            Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;
            status_is(vm.LoadBytes(encrypted(words)),ErrorCode::Ok,"role-independent load");
            const auto result=exec(vm,print_code);
            require(result.pc==70 && cb.states.back().pc==64 && cb.events.back().find("\"code\":572653569,")!=std::string::npos,
                    "role-independent execution");
            frames_are(vm,{{42,0}});
        }
    });
    for (const Word kind:{0x44440006U,0x44440007U,0x44440008U}) suite.test("nonempty other adjustment loads without changing execution "+std::to_string(kind),[kind] {
        auto words=program_words(base());
        const Word field=kind-0x44440006U+21U; const Word payload=words[field];
        ++words[payload-4U]; words.insert(words.begin()+static_cast<std::ptrdiff_t>(payload),37);
        for (Word f=field+1U;f<=23;++f) ++words[f];
        Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;status_is(vm.LoadBytes(encrypted(words)),ErrorCode::Ok,"other adjustment load");
        require(exec(vm,print_code).pc==70&&cb.states.back().pc==64,"other adjustment changed execution");
    });
    suite.test("film with no koma and trailing empty film are valid",[] {
        Model m=base(); m.offsets={0,0}; m.komas.clear(); Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,m);
    });
    suite.test("stories only and no film offsets are valid",[] {
        Model m; m.labels={m.code.story()}; m.story_count=1; m.code.end(); Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,m); exec(vm,end_code);
    });
    suite.test("empty label payload can numerically equal next record pointer",[] {
        Model m; m.code.end(); const auto words=program_words(m);
        require(words[9]==words[16] && words[17]==words[16]+4U,"empty table pointer fixture");
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; load(vm,m); exec(vm,end_code);
    });
    suite.test("fourth LABEL table is rejected",[] {
        auto words=program_words(base()); const auto position=words[9]-4U;
        words.insert(words.begin()+static_cast<std::ptrdiff_t>(position),{4,data,0x55550002U,table_label});
        reject(words);
    });
    suite.test("LABEL after base table section is rejected",[] {
        auto words=program_words(base()); const auto position=words[9]-4U;
        words.insert(words.begin()+static_cast<std::ptrdiff_t>(position),{4,data,0x55550002U,0x44440004U});
        reject(words);
    });
    suite.test("missing third LABEL table is invalid",[] {
        auto words=program_words(base()); const auto position=words[17]; const auto count=words[position];
        words.erase(words.begin()+static_cast<std::ptrdiff_t>(position),words.begin()+static_cast<std::ptrdiff_t>(position+count));
        reject(words,ErrorCode::InvalidFormat);
    });
    suite.test("no films cannot own koma targets",[] {
        Model m; m.labels={m.code.story()}; m.story_count=1; m.komas={m.code.system(prepare_code)}; m.code.end(); reject(program_words(m));
    });
    suite.test("film offset count must match derived film count",[] {
        Model m=base(); m.offsets={0}; reject(program_words(m));
    });
    suite.test("extra film offset rejected",[] {
        Model m=base(); m.offsets={0,2,2}; reject(program_words(m));
    });
    suite.test("wrong film owns otherwise valid target",[] {
        Model m=base(); m.offsets={0,1}; reject(program_words(m));
    });
    suite.test("SUB adjustment missing a source callsite",[] {
        Model m=base(); m.code.sub_sites.clear(); reject(program_words(m));
    });
    suite.test("SUB adjustment duplicate callsite",[] {
        Model m=nested(); m.code.sub_sites[1]=m.code.sub_sites[0]; reject(program_words(m));
    });
    suite.test("SUB adjustment descending callsites",[] {
        Model m=nested(); std::swap(m.code.sub_sites[0],m.code.sub_sites[1]); reject(program_words(m));
    });
    suite.test("SUB adjustment contains a non-SUB entry",[] {
        Model m=base(); m.code.sub_sites.push_back(48); reject(program_words(m));
    });
    suite.test("SUB ID cannot use compact SUB ordinal after GOTO",[] {
        auto words=program_words(goto_before());words[45]=0;reject(words);
    });
    suite.test("SUB cannot take a valid callsite PC from a GOTO hole",[] {
        auto words=program_words(goto_before(41));words[words[20]+1U]=0;reject(words);
    });
    suite.test("SUB callsite written into wrong shared slot is rejected",[] {
        auto words=program_words(goto_before());std::swap(words[words[20]],words[words[20]+1U]);reject(words);
    });
    suite.test("missing trailing GOTO registry slot is rejected",[] {
        Model m;m.labels={m.code.story()};m.story_count=1;m.code.sub(0);const Word stop=m.code.end();m.code.jump(stop,UINT32_MAX);
        m.code.sub_sites.pop_back();reject(program_words(m));
    });
    suite.test("extra shared registry slot is rejected",[] {
        Model m=goto_before();m.code.sub_sites.push_back(UINT32_MAX);reject(program_words(m));
    });
    suite.test("GOTO-only profile cannot omit its registry slot",[] {
        Model m;const Word jump=m.code.jump();const Word stop=m.code.end();m.code.set_jump(jump,stop);m.code.sub_sites.clear();reject(program_words(m));
    });
    for(const Word target:{0U,33U,UINT32_MAX}) suite.test("label-profile GOTO still rejects invalid target "+std::to_string(target),[target] {
        Model m;m.code.jump(target,UINT32_MAX);m.code.end();reject(program_words(m));
    });
    suite.test("label-profile GOTO still rejects data-table target",[] {
        Model m;m.code.jump(0,UINT32_MAX);m.code.end();auto words=program_words(m);words[35]=words[9]-4U;reject(words);
    });
    suite.test("label-profile GOTO still rejects one-past-end target",[] {
        Model m;m.code.jump(0,UINT32_MAX);m.code.end();auto words=program_words(m);words[35]=static_cast<Word>(words.size());reject(words);
    });
    suite.test("SUB has no executable return record",[] {
        Model m; m.labels={m.code.story()}; m.story_count=1; m.code.sub(0); reject(program_words(m));
    });
    for (const bool is_sub:{false,true}) suite.test(std::string("legacy profile rejects ")+(is_sub?"SUB":"RETURN"),[is_sub] {
        Model m; m.profile=false; if (is_sub) {m.code.add({5,control,sub_code,0,0});m.code.end();} else m.code.ret();
        if(is_sub) reject(program_words(m),ErrorCode::InvalidFormat);
        else {Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);const auto r=vm.Exec();status_is(r.status,ErrorCode::CallStackUnderflow,"RETURN without frame");require(r.pc==32&&cb.events.empty(),"legacy RETURN state");}
    });
    suite.test("legacy profile dispatches FILMNAME",[] {
        Model m; m.profile=false; m.code.film();m.code.end();Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);
        require(exec(vm,end_code).pc==37&&cb.events.size()==1&&cb.states[0].pc==32&&cb.events[0].find("\"code\":858980432,")!=std::string::npos,"legacy FILMNAME dispatch");
    });
    suite.test("STARTKOMA role permits unhandled instruction fallback",[] {
        Model m=base(); m.code.words[56U-32U+2U]=start_koma_code;
        Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);require(exec(vm,print_code).pc==70 &&
            cb.skipped==std::vector<std::tuple<Word,Word,Word>>{{56,system_function,start_koma_code}},"STARTKOMA fallback");
    });
    for (const Word opcode:{sub_code,return_code}) for (const Word length:{2U,4U,6U}) {
        suite.test("malformed call record length "+std::to_string(opcode)+"/"+std::to_string(length),[opcode,length] {
            Model m; const Word start=m.code.pc();
            m.code.add({length,control,opcode});
            for (Word count=3;count<length;++count) m.code.words.push_back(0);
            if (opcode==sub_code) m.code.sub_sites.push_back(start);
            m.code.end(); reject(program_words(m));
        });
    }

    // The fixture file and its complete callbacks/PC/frame expectations are
    // independently source-derived and captured from the pinned original VM.
    const auto fixture_dir=std::filesystem::path(NNN_CALL_FIXTURE_DIR);
    for (const auto& item:dw008_fixture::cases()) suite.test(std::string("fixed original call trace: ")+item.name,[&] {
        std::ifstream input(fixture_dir/item.filename,std::ios::binary); require(static_cast<bool>(input),"missing call fixture");
        const std::vector<Byte> bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        Callback cb; nnn::CExecScript vm(cb); cb.vm=&vm; status_is(vm.LoadBytes(bytes,item.name),ErrorCode::Ok,"fixture load");
        for (const auto& step:item.steps) {
            const auto actual=exec(vm,step.return_code); require(actual.pc==step.pc,"fixture post-PC mismatch");
            require(vm.GetCallDepth()==step.frames.size(),"fixture post-depth mismatch");
            for (std::size_t index=0;index<step.frames.size();++index) {
                nnn::CallFrame frame{}; require(vm.GetCallFrame(index,frame),"fixture frame missing");
                require(frame.return_pc==step.frames[index].return_pc && frame.id==step.frames[index].id,"fixture frame mismatch");
            }
        }
        require(cb.events==item.callback_json,"fixture complete callback trace mismatch");
        require(cb.states.size()==item.callback_states.size(),"fixture callback state count");
        for (std::size_t index=0;index<cb.states.size();++index) {
            const auto& expected=item.callback_states[index]; const auto& actual=cb.states[index];
            require(actual.pc==expected.pc && actual.stack.size()==expected.frames.size(),"fixture callback PC/depth mismatch");
            for (std::size_t frame=0;frame<actual.stack.size();++frame)
                require(actual.stack[frame].return_pc==expected.frames[frame].return_pc && actual.stack[frame].id==expected.frames[frame].id,"fixture callback active frame mismatch");
        }
        for (const auto& variable:item.variables) require(cb.variables[variable.first]==variable.second,"fixture final variable mismatch");
    });
    std::cout << "CALL RESULT " << suite.passed << " passed, " << suite.failed << " failed\n";
    return suite.failed==0 ? 0 : 1;
}
