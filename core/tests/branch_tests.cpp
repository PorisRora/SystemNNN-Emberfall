#include "nnn/script.hpp"
#include "branches/fixtures/generated_cases.hpp"

#include <algorithm>
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
constexpr Word var100 = 0x80000064U;
constexpr Word var101 = 0x80000065U;
constexpr Word var102 = 0x80000066U;
constexpr Word assign = 0xa000003dU;
constexpr Word equal = 0xa300003dU;
constexpr Word add = 0xa000002bU;
constexpr Word subtract = 0xa000002dU;
constexpr Word multiply = 0xa000002aU;
constexpr Word less = 0xa400003cU;
constexpr Word greater = 0xa400003eU;
constexpr Word less_equal = 0xa500003cU;
constexpr Word greater_equal = 0xa500003eU;
constexpr Word not_equal = 0xa5000021U;
constexpr Word end_code = 0x55560005U;

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

std::string variable_event(const char* kind, Word index, Value value) {
    return "{\"kind\":" + quote(kind) + ",\"index\":" + std::to_string(index) +
           ",\"value\":" + std::to_string(value) + '}';
}

struct Callback final : nnn::CScriptCallBack {
    std::map<Word, Value> variables;
    std::vector<std::string> events;
    std::vector<std::tuple<Word,Word,Word>> skipped;
    std::function<void(Word, Value)> on_set;
    std::function<void(Word)> on_get;
    void UnhandledInstruction(Word pc,Word identify,Word code) override { skipped.emplace_back(pc,identify,code); }

    void ReceiveScriptCommand(Word code, Value para1, const std::string& text, Value para3) override {
        events.push_back("{\"kind\":\"script\",\"code\":" + std::to_string(code) +
            ",\"para1\":" + std::to_string(para1) + ",\"text_ascii\":" + quote(text) +
            ",\"para3\":" + std::to_string(para3) + '}');
    }
    void PreReceiveScriptData(Word, Value, const std::string&) override { throw std::runtime_error("unsupported pre-data callback"); }
    void ReceiveUserCommand0(Word code, const std::vector<Value>& parameters) override {
        std::string event = "{\"kind\":\"user_command\",\"code\":" + std::to_string(code) + ",\"parameters\":[";
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index != 0) event += ',';
            event += std::to_string(parameters[index]);
        }
        events.push_back(event + "]}");
    }
    void ReceiveUserFunction0(Word, const std::vector<Value>&) override { throw std::runtime_error("unsupported user function"); }
    Value GetVarData(Word index) override {
        if (on_get) on_get(index);
        const Value value = variables[index];
        events.push_back(variable_event("get_var", index, value));
        return value;
    }
    Value GetCalcuVarData(Word index) override {
        if (on_get) on_get(index);
        const Value value = variables[index];
        events.push_back(variable_event("get_calcu_var", index, value));
        return value;
    }
    void SetVarData(Word index, Value value) override {
        variables[index] = value;
        events.push_back(variable_event("set_var", index, value));
        if (on_set) on_set(index, value);
    }
};

constexpr Word control = 0x66660002U;
constexpr Word if_code = 0x55560002U;
constexpr Word elsif_code = 0x55560003U;
constexpr Word else_code = 0x55560004U;
constexpr Word open_code = 0xa000007bU;
constexpr Word close_code = 0xa000007dU;

Word number(Value value) {
    require(value >= -32768 && value <= 32767, "test signed16 value out of range");
    return 0x90000000U | (static_cast<Word>(value) & 0xffffU);
}

struct Code {
    std::vector<Word> words;
    Word pc() const { return static_cast<Word>(words.size()) + 32U; }
    Word scalar(Word code, Word value) {
        const Word start = pc();
        words.insert(words.end(), {4, control, code, value});
        return start;
    }
    Word marker(Word code) {
        const Word start = pc(); words.insert(words.end(), {3, control, code}); return start;
    }
    Word open() {
        const Word start = pc(); words.insert(words.end(), {4, control, open_code, 0}); return start;
    }
    void close(Word start) {
        marker(close_code);
        words[start - 32U + 3U] = pc() - start;
    }
    Word jump(Word destination = 0) { return scalar(0x55560020U, destination); }
    void set_jump(Word start, Word destination) { words[start - 32U + 3U] = destination; }
    void assign_value(Word index, Value value) {
        words.insert(words.end(), {8, 0x66660005U, 0, 0xa0000000U, 3,
                                  0x80000000U | index, number(value), assign});
    }
    void end() { marker(end_code); }
};

std::vector<Byte> program(const std::vector<Word>& code) {
    std::vector<Word> words(32, 0);
    words[0] = 32; words[1] = 0x66660001U; words[2] = 0x55550001U;
    words[12] = 0x48545053U; words[13] = 0x45444145U;
    words[14] = 0x20203052U; words[15] = 0x00202020U;
    words.insert(words.end(), code.begin(), code.end());
    const auto table = [&](Word kind, Word field) {
        words[field] = static_cast<Word>(words.size()) + 4U;
        words.insert(words.end(), {4, 0x66660001U, 0x55550002U, kind});
    };
    table(0x44440002U, 5); table(0x44440001U, 7); table(0x44440004U, 18);
    table(0x44440005U, 20); table(0x44440006U, 21);
    table(0x44440007U, 22); table(0x44440008U, 23);
    std::vector<Byte> bytes;
    for (Word word : words) {
        word ^= UINT32_MAX;
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<Byte>((word >> shift) & 0xffU));
    }
    return bytes;
}

void run_end(nnn::CExecScript& vm, std::uint64_t budget = 100000) {
    const auto result = vm.Exec(budget);
    status_is(result.status, ErrorCode::Ok, "branch Exec");
    require(result.return_code == end_code, "branch did not reach END");
}

void reject(const std::vector<Word>& code, ErrorCode error = ErrorCode::InvalidFormat) {
    Callback callback; nnn::CExecScript vm(callback);
    status_is(vm.LoadBytes(program(code)), error, "bad branch load");
    require(!vm.IsLoaded() && callback.events.empty(), "bad structure installed or emitted callbacks");
    status_is(vm.Exec().status, ErrorCode::NotLoaded, "bad branch remains unloaded");
}

struct Suite {
    int passed = 0;
    int failed = 0;
    void test(const std::string& name, const std::function<void()>& body) {
        try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
};

} // namespace

int main() {
    Suite suite;
    const auto fixtures = std::filesystem::path(__FILE__).parent_path() / "branches/fixtures";
    for (const auto& item : dw004_fixture::cases()) {
        suite.test(std::string("fixed full branch trace: ") + item.name, [&] {
            std::ifstream input(fixtures / item.filename, std::ios::binary);
            require(static_cast<bool>(input), "missing branch fixture");
            const std::vector<Byte> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            Callback callback; nnn::CExecScript vm(callback);
            status_is(vm.LoadBytes(bytes, item.name), ErrorCode::Ok, "branch fixture load");
            for (const auto& expected : item.steps) {
                const auto actual = vm.Exec(100000);
                status_is(actual.status, ErrorCode::Ok, "branch fixture Exec");
                require(actual.return_code == expected.return_code && actual.pc == expected.pc, "branch fixture return/PC mismatch");
            }
            require(callback.events == item.callback_json, "branch fixture full callback mismatch");
            for (const auto& expected : item.variables)
                require(callback.variables[expected.first] == expected.second, "branch fixture variable mismatch");
        });
    }

    struct Bad { const char* name; std::function<std::vector<Word>()> code; ErrorCode error = ErrorCode::InvalidFormat; };
    const std::vector<Bad> bad {
        {"OPEN short length", [] { return std::vector<Word>{3,control,open_code,3,control,close_code,3,control,end_code}; }},
        {"OPEN long length", [] { return std::vector<Word>{5,control,open_code,8,0,3,control,close_code,3,control,end_code}; }},
        {"CLOSE long length", [] { return std::vector<Word>{4,control,open_code,8,4,control,close_code,0,3,control,end_code}; }},
        {"missing close", [] { Code c; c.open(); c.end(); return c.words; }},
        {"extra close", [] { Code c; c.marker(close_code); c.end(); return c.words; }},
        {"span zero", [] { Code c; const auto o=c.open(); c.close(o); c.words[o-32U+3U]=0; c.end(); return c.words; }},
        {"span overflow", [] { Code c; const auto o=c.open(); c.close(o); c.words[o-32U+3U]=UINT32_MAX; c.end(); return c.words; }},
        {"span points valid inner boundary", [] { Code c; const auto o=c.open(); c.marker(0x55560001U); c.close(o); c.words[o-32U+3U]=4; c.end(); return c.words; }},
        {"span points own close", [] { Code c; const auto o=c.open(); c.close(o); c.words[o-32U+3U]-=3; c.end(); return c.words; }},
        {"crossed nested spans", [] { Code c; const auto o=c.open(); const auto i=c.open(); c.close(i); const auto inner_after=c.pc(); c.close(o); c.words[o-32U+3U]=inner_after-o; c.end(); return c.words; }},
        {"nested close missing", [] { Code c; const auto o=c.open(); c.open(); c.close(o); c.end(); return c.words; }},
        {"orphan ELSE", [] { Code c; c.marker(else_code); const auto o=c.open(); c.close(o); c.end(); return c.words; }},
        {"orphan ELSIF", [] { Code c; c.scalar(elsif_code,number(1)); const auto o=c.open(); c.close(o); c.end(); return c.words; }},
        {"duplicate ELSE", [] { Code c; c.scalar(if_code,number(0)); auto o=c.open(); c.close(o); c.marker(else_code); o=c.open(); c.close(o); c.marker(else_code); o=c.open(); c.close(o); c.end(); return c.words; }},
        {"ELSIF after ELSE", [] { Code c; c.scalar(if_code,number(0)); auto o=c.open(); c.close(o); c.marker(else_code); o=c.open(); c.close(o); c.scalar(elsif_code,number(1)); o=c.open(); c.close(o); c.end(); return c.words; }},
        {"ELSE invalid length", [] { Code c; c.scalar(if_code,number(0)); auto o=c.open(); c.close(o); c.words.insert(c.words.end(),{4,control,else_code,0}); o=c.open(); c.close(o); c.end(); return c.words; }},
        {"ELSIF missing expression", [] { Code c; c.scalar(if_code,number(0)); auto o=c.open(); c.close(o); c.marker(elsif_code); o=c.open(); c.close(o); c.end(); return c.words; }},
        {"orphan nested branch", [] { Code c; const auto outer=c.open(); c.marker(else_code); const auto inner=c.open(); c.close(inner); c.close(outer); c.end(); return c.words; }},
        {"unreachable broken span", [] { Code c; c.end(); const auto o=c.open(); c.close(o); c.words[o-32U+3U]=1; c.end(); return c.words; }},
        {"unreachable orphan ELSIF", [] { Code c; c.end(); c.scalar(elsif_code,number(1)); const auto o=c.open(); c.close(o); c.end(); return c.words; }},
        {"unmatched branch scope at close", [] { Code c; c.scalar(if_code,number(1)); auto o=c.open(); c.close(o); o=c.open(); c.close(o); c.marker(else_code); o=c.open(); c.close(o); c.end(); return c.words; }},
    };
    for (const auto& item : bad) suite.test(item.name, [&] { reject(item.code(),item.error); });
    for (const bool first_braced:{false,true}) for (const bool second_elsif:{false,true}) {
        suite.test(first_braced ? std::string("braced IF with single ")+(second_elsif?"ELSIF":"ELSE") :
                                std::string("single IF mixed ")+(second_elsif?"ELSIF":"ELSE"),[=] {
            Code c; c.scalar(if_code,number(0));
            Word o=0; if (first_braced) o=c.open();
            c.assign_value(100,7); if (first_braced) c.close(o);
            if (second_elsif) c.scalar(elsif_code,number(1)); else c.marker(else_code);
            if (!first_braced) o=c.open();
            c.assign_value(100,9); if (!first_braced) c.close(o);
            c.end(); Callback cb; nnn::CExecScript vm(cb);
            status_is(vm.LoadBytes(program(c.words)),ErrorCode::Ok,"mixed branch load");
            run_end(vm);
            require(cb.events==std::vector<std::string>{variable_event("set_var",100,9),variable_event("get_var",100,9)},"mixed branch selected wrong body");
        });
    }
    suite.test("unknown control marker falls back and advances", [] {
        Callback cb; nnn::CExecScript vm(cb);
        status_is(vm.LoadBytes(program({3,control,0xa000007cU,3,control,end_code})),ErrorCode::Ok,"unknown control load");
        run_end(vm);
        require(cb.skipped==std::vector<std::tuple<Word,Word,Word>>{{32,control,0xa000007cU}} && vm.GetPC()==35,"unknown control fallback");
    });
    suite.test("unknown instruction inside unreachable balanced block loads without dispatch", [] {
        Code c; c.end(); const auto o=c.open(); c.marker(0x99999999U); c.close(o); c.end();
        Callback cb; nnn::CExecScript vm(cb);
        status_is(vm.LoadBytes(program(c.words)),ErrorCode::Ok,"unreachable unknown load");
        run_end(vm); require(cb.skipped.empty() && vm.GetPC()==32,"unreachable unknown executed");
    });
    suite.test("64 bare nesting levels accepted", [] {
        Code c; std::vector<Word> starts;
        for (unsigned i=0;i<64;++i) starts.push_back(c.open());
        c.marker(0x55560001U);
        for (auto i=starts.rbegin();i!=starts.rend();++i) c.close(*i);
        c.end(); Callback cb; nnn::CExecScript vm(cb);
        status_is(vm.LoadBytes(program(c.words)),ErrorCode::Ok,"64 nested load"); run_end(vm);
        require(cb.events.empty(),"bare blocks emitted callbacks");
    });
    suite.test("65 nesting levels rejected", [] {
        Code c; std::vector<Word> starts;
        for (unsigned i=0;i<65;++i) starts.push_back(c.open());
        for (auto i=starts.rbegin();i!=starts.rend();++i) c.close(*i);
        c.end(); reject(c.words);
    });
    suite.test("long false ELSIF chain exhausts one Exec budget", [] {
        Code c; c.scalar(if_code,number(0)); auto o=c.open(); c.close(o);
        for (unsigned i=0;i<1000;++i) { c.scalar(elsif_code,number(0)); o=c.open(); c.close(o); }
        c.end(); Callback cb; nnn::CExecScript vm(cb);
        status_is(vm.LoadBytes(program(c.words)),ErrorCode::Ok,"long chain load");
        const auto result=vm.Exec(37);
        status_is(result.status,ErrorCode::BudgetExhausted,"long chain internal budget");
        require(result.work_used<=37 && cb.events.empty(),"long chain bypassed budget");
    });
    suite.test("deep selected branches exhaust inside one Exec", [] {
        Code c; std::vector<Word> starts;
        for (unsigned i=0;i<64;++i) { c.scalar(if_code,number(1)); starts.push_back(c.open()); }
        c.marker(0x55560001U);
        for(auto i=starts.rbegin();i!=starts.rend();++i) c.close(*i);
        c.end(); Callback cb; nnn::CExecScript vm(cb);
        status_is(vm.LoadBytes(program(c.words)),ErrorCode::Ok,"deep branch load");
        const auto result=vm.Exec(29);
        status_is(result.status,ErrorCode::BudgetExhausted,"deep branch budget");
        require(result.work_used<=29 && cb.events.empty(),"deep branch bypassed budget");
    });
    suite.test("GOTO self loop inside block exhausts internally", [] {
        Code c; const auto o=c.open(); const auto jump=c.jump(); c.set_jump(jump,jump); c.close(o); c.end();
        Callback cb; nnn::CExecScript vm(cb); status_is(vm.LoadBytes(program(c.words)),ErrorCode::Ok,"block loop load");
        const auto result=vm.Exec(23); status_is(result.status,ErrorCode::BudgetExhausted,"block loop budget");
        require(result.pc==jump && result.work_used==21 && cb.events.empty(),"block loop PC/work mismatch");
    });
    suite.test("raw legacy IF skips inner IF header but enters OPEN", [] {
        Code c; c.scalar(if_code,number(0)); c.scalar(if_code,0x8000006aU);
        auto o=c.open(); c.assign_value(100,7); c.close(o);
        c.scalar(elsif_code,0x8000006bU); o=c.open(); c.assign_value(100,8); c.close(o);
        c.marker(else_code); o=c.open(); c.assign_value(100,9); c.close(o); c.end();
        Callback cb; nnn::CExecScript vm(cb); status_is(vm.LoadBytes(program(c.words)),ErrorCode::Ok,"raw nested header load");
        run_end(vm);
        require(cb.events==std::vector<std::string>{variable_event("set_var",100,7),variable_event("get_var",100,7)},"raw IF got AST semantics or evaluated skipped condition");
    });
    suite.test("ordinary GOTO entry to ELSIF never evaluates condition", [] {
        Code c; const auto jump=c.jump(); c.scalar(if_code,number(1)); auto o=c.open(); c.assign_value(100,1); c.close(o);
        const auto elsif=c.scalar(elsif_code,0x80000069U); o=c.open(); c.assign_value(100,2); c.close(o);
        c.marker(else_code); o=c.open(); c.assign_value(100,3); c.close(o); c.end(); c.set_jump(jump,elsif);
        Callback cb; nnn::CExecScript vm(cb); status_is(vm.LoadBytes(program(c.words)),ErrorCode::Ok,"goto elsif load"); run_end(vm);
        require(cb.events.empty(),"ordinary ELSIF read condition or ran a body");
    });
    suite.test("negative scalar condition selects ELSE", [] {
        Code c; c.scalar(if_code,0x80000069U); auto o=c.open(); c.assign_value(100,1); c.close(o);
        c.marker(else_code); o=c.open(); c.assign_value(100,3); c.close(o); c.end();
        Callback cb; cb.variables[105]=-1; nnn::CExecScript vm(cb);
        status_is(vm.LoadBytes(program(c.words)),ErrorCode::Ok,"negative condition load"); run_end(vm);
        require(cb.events==std::vector<std::string>{variable_event("get_calcu_var",105,-1),variable_event("set_var",100,3),variable_event("get_var",100,3)},"negative treated as truthy");
    });
    suite.test("bad unreachable block replacement preserves loaded image and PC", [] {
        const auto old=std::filesystem::path(__FILE__).parent_path()/"fixtures/flow.spt";
        std::ifstream input(old,std::ios::binary); const std::vector<Byte> bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        Callback cb; nnn::CExecScript vm(cb); status_is(vm.LoadBytes(bytes,"A"),ErrorCode::Ok,"transaction base load");
        const auto first=vm.Exec(100000); status_is(first.status,ErrorCode::Ok,"transaction base Exec");
        const auto pc=vm.GetPC(); const auto count=vm.WordCount(); const auto events=cb.events;
        Code c; c.end(); const auto o=c.open(); c.close(o); c.words[o-32U+3U]=0; c.end();
        status_is(vm.LoadBytes(program(c.words),"B"),ErrorCode::InvalidFormat,"bad block replacement");
        require(vm.Identity()=="A" && vm.GetPC()==pc && vm.WordCount()==count && cb.events==events,"bad block changed current state");
        for(Word index=0;index<count;++index) { Word value=0; require(vm.GetData(index,value),"old data lost");
            const Word expected=static_cast<Word>(bytes[index*4U])|(static_cast<Word>(bytes[index*4U+1U])<<8U)|(static_cast<Word>(bytes[index*4U+2U])<<16U)|(static_cast<Word>(bytes[index*4U+3U])<<24U);
            require(value==(expected^UINT32_MAX),"old image changed"); }
    });
    suite.test("conditional callback reentry remains Busy", [] {
        Code c; c.scalar(if_code,0x80000069U); auto o=c.open(); c.close(o); c.end();
        const auto bytes=program(c.words); Callback cb; cb.variables[105]=1; nnn::CExecScript vm(cb);
        cb.on_get=[&](Word) { status_is(vm.Exec().status,ErrorCode::Busy,"condition reentrant Exec"); status_is(vm.LoadBytes(bytes,"inner"),ErrorCode::Busy,"condition reentrant load"); };
        status_is(vm.LoadBytes(bytes,"outer"),ErrorCode::Ok,"condition guard load"); run_end(vm);
        require(vm.Identity()=="outer","condition callback replaced image");
    });
    suite.test("conditional callback exception clears operation guard", [] {
        Code c; c.scalar(if_code,0x80000069U); auto o=c.open(); c.close(o); c.end();
        const auto bytes=program(c.words); Callback cb; nnn::CExecScript vm(cb);
        cb.on_get=[](Word) { throw std::runtime_error("condition callback failure"); };
        status_is(vm.LoadBytes(bytes),ErrorCode::Ok,"throw condition load");
        status_is(vm.Exec().status,ErrorCode::CallbackFailed,"throw condition Exec");
        cb.on_get={}; status_is(vm.LoadBytes(bytes),ErrorCode::Ok,"guard reset load"); run_end(vm);
    });
    std::cout << "BRANCH RESULT " << suite.passed << " passed, " << suite.failed << " failed\n";
    return suite.failed==0 ? 0 : 1;
}
