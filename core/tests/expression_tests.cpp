#include "nnn/script.hpp"
#include "expressions/fixtures/generated_cases.hpp"

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
    std::function<void(Word, Value)> on_set;
    std::function<void(Word)> on_get;

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

Word number(Value value) {
    require(value >= -32768 && value <= 32767, "test signed16 value is out of range");
    return 0x90000000U | (static_cast<Word>(value) & 0xffffU);
}

void expression_record(std::vector<Word>& code, const std::vector<Word>& tokens, bool condition = false) {
    code.insert(code.end(), {static_cast<Word>(tokens.size()) + 5U,
        condition ? 0x66660002U : 0x66660005U,
        condition ? 0x55560002U : 0U, 0xa0000000U, static_cast<Word>(tokens.size())});
    code.insert(code.end(), tokens.begin(), tokens.end());
}

std::vector<Byte> program(const std::vector<Word>& code, const std::vector<Word>& constants = {}) {
    // Test-only writer for the established profile. Arithmetic expected values
    // below are hand-fixed and are never calculated by this writer.
    std::vector<Word> words(32, 0);
    words[0] = 32; words[1] = 0x66660001U; words[2] = 0x55550001U;
    words[12] = 0x48545053U; words[13] = 0x45444145U;
    words[14] = 0x20203052U; words[15] = 0x00202020U;
    words.insert(words.end(), code.begin(), code.end());
    const auto table = [&](Word kind, Word field, const std::vector<Word>& payload) {
        words[field] = static_cast<Word>(words.size()) + 4U;
        words.insert(words.end(), {static_cast<Word>(payload.size()) + 4U, 0x66660001U, 0x55550002U, kind});
        words.insert(words.end(), payload.begin(), payload.end());
    };
    table(0x44440002U, 5, {});
    table(0x44440001U, 7, {});
    table(0x44440004U, 18, constants);
    table(0x44440005U, 20, {});
    table(0x44440006U, 21, {});
    table(0x44440007U, 22, {});
    table(0x44440008U, 23, {});
    std::vector<Byte> bytes;
    for (Word word : words) {
        word ^= UINT32_MAX;
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<Byte>((word >> shift) & 0xffU));
    }
    return bytes;
}

std::vector<Byte> expression_program(const std::vector<Word>& tokens, const std::vector<Word>& constants = {}) {
    std::vector<Word> code;
    expression_record(code, tokens);
    code.insert(code.end(), {3, 0x66660002U, end_code});
    return program(code, constants);
}

void run_end(nnn::CExecScript& vm) {
    const auto result = vm.Exec(100000);
    status_is(result.status, ErrorCode::Ok, "expression Exec");
    require(result.return_code == end_code, "expression did not reach END");
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
    const auto fixtures = std::filesystem::path(__FILE__).parent_path() / "expressions/fixtures";
    for (const auto& item : dw003_fixture::cases()) {
        suite.test(std::string("independent fixed trace: ") + item.name, [&] {
            std::ifstream input(fixtures / item.filename, std::ios::binary);
            require(static_cast<bool>(input), "missing expression fixture");
            const std::vector<Byte> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            Callback callback; nnn::CExecScript vm(callback);
            status_is(vm.LoadBytes(bytes, item.name), ErrorCode::Ok, "fixture load");
            for (const auto& expected : item.steps) {
                const auto actual = vm.Exec(100000);
                status_is(actual.status, ErrorCode::Ok, "fixture Exec");
                require(actual.return_code == expected.return_code && actual.pc == expected.pc, "fixture return/PC mismatch");
            }
            require(callback.events == item.callback_json, "fixture full callback trace mismatch");
            for (const auto& expected : item.variables)
                require(callback.variables[expected.first] == expected.second, "fixture final variable mismatch");
        });
    }

    struct Vector { const char* name; Word operation; Word left; Word right; Value expected; };
    // Independent hand-fixed two's-complement boundaries, not a generator
    // roundtrip and not the original host's -fwrapv behavior as an oracle.
    const std::vector<Vector> vectors {
        {"MAX+1 wraps MIN",add,0x7fffffffU,1U,INT32_MIN},
        {"MIN+minus1 wraps MAX",add,0x80000000U,0xffffffffU,INT32_MAX},
        {"minus1+1 is zero",add,0xffffffffU,1U,0},
        {"signed16 overflow stays full32",add,32767U,1U,32768},
        {"MIN-1 wraps MAX",subtract,0x80000000U,1U,INT32_MAX},
        {"MAX-minus1 wraps MIN",subtract,0x7fffffffU,0xffffffffU,INT32_MIN},
        {"zero-MIN wraps MIN",subtract,0U,0x80000000U,INT32_MIN},
        {"MIN times minus1 wraps MIN",multiply,0x80000000U,0xffffffffU,INT32_MIN},
        {"MAX times two wraps minus2",multiply,0x7fffffffU,2U,-2},
        {"65536 squared wraps zero",multiply,65536U,65536U,0},
        {"negative multiplication",multiply,static_cast<Word>(-200),300U,-60000},
        {"minus1 squared",multiply,0xffffffffU,0xffffffffU,1},
        {"signed less MIN MAX",less,0x80000000U,0x7fffffffU,1},
        {"signed less MAX MIN false",less,0x7fffffffU,0x80000000U,0},
        {"signed greater MAX MIN",greater,0x7fffffffU,0x80000000U,1},
        {"signed greater equal values false",greater,0xffffffffU,0xffffffffU,0},
        {"less equal equal values",less_equal,0x80000000U,0x80000000U,1},
        {"less equal reversed false",less_equal,1U,0xffffffffU,0},
        {"greater equal MAX MIN",greater_equal,0x7fffffffU,0x80000000U,1},
        {"greater equal reversed false",greater_equal,0x80000000U,0x7fffffffU,0},
        {"not equal extremes",not_equal,0x80000000U,0x7fffffffU,1},
        {"not equal equal false",not_equal,0x80000000U,0x80000000U,0},
        {"existing equality true",equal,0x7fffffffU,0x7fffffffU,1},
        {"existing equality false",equal,0xffffffffU,1U,0},
    };
    for (const auto& item : vectors) suite.test(std::string("fixed vector: ") + item.name, [&] {
        Callback callback; nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(expression_program({var100,0x91000000U,0x91000001U,item.operation,assign},
                  {item.left,item.right})),ErrorCode::Ok,"fixed vector load");
        run_end(vm);
        require(callback.variables[100] == item.expected, "fixed expected value mismatch");
        require(callback.events == std::vector<std::string>{variable_event("set_var",100,item.expected),
                variable_event("get_var",100,item.expected)}, "fixed vector write/read trace mismatch");
    });

    const std::vector<std::pair<Word, Value>> delayed_operations {
        {add,-4},{subtract,-10},{multiply,-21},{less,1},{greater,0},
        {less_equal,1},{greater_equal,0},{not_equal,1},{equal,0}
    };
    for (const auto& item : delayed_operations) suite.test("lazy left/right resolution " + std::to_string(item.first), [&] {
        Callback callback; callback.variables[100]=-7; callback.variables[101]=3;
        nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(expression_program({var102,var100,var101,item.first,assign})),ErrorCode::Ok,"lazy load");
        run_end(vm);
        require(callback.events == std::vector<std::string>{variable_event("get_var",100,-7),
            variable_event("get_var",101,3),variable_event("set_var",102,item.second),
            variable_event("get_var",102,item.second)},"lazy callback ordering changed");
    });
    suite.test("nested intermediate beyond signed16 and repeated reference",[] {
        Callback callback; callback.variables[100]=20000; callback.variables[101]=3;
        nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(expression_program({var102,var100,var100,add,var101,multiply,assign})),ErrorCode::Ok,"nested load");
        run_end(vm);
        require(callback.events == std::vector<std::string>{variable_event("get_var",100,20000),
            variable_event("get_var",100,20000),variable_event("get_var",101,3),
            variable_event("set_var",102,120000),variable_event("get_var",102,120000)},"nested callback order/value mismatch");
    });
    suite.test("delayed references observe earlier assignment callback effects",[] {
        Callback callback; callback.variables[100]=7;
        callback.on_set=[&](Word index,Value){if(index==101)callback.variables[100]=20;};
        nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(expression_program({var102,var100,var101,var100,assign,add,assign})),ErrorCode::Ok,"reference load");
        run_end(vm);
        require(callback.events==std::vector<std::string>{variable_event("get_var",100,7),
            variable_event("set_var",101,7),variable_event("get_var",100,20),variable_event("get_var",101,7),
            variable_event("set_var",102,27),variable_event("get_var",102,27)},"references were eagerly resolved or cached");
    });
    for (const auto& condition : std::vector<std::pair<std::vector<Word>,Value>>{
             {{number(1),number(2),add},9},{{number(1),number(2),subtract},0},
             {{number(-2),number(1),less},9},{{number(2),number(1),less},0},
             {{number(-2),number(-2),not_equal},0}}) {
        suite.test("computed result in IF " + std::to_string(condition.first.back()) + "/" + std::to_string(condition.second),[&] {
            std::vector<Word> code;
            expression_record(code,condition.first,true);
            expression_record(code,{var101,number(9),assign});
            code.insert(code.end(),{3,0x66660002U,end_code});
            Callback callback; nnn::CExecScript vm(callback);
            status_is(vm.LoadBytes(program(code)),ErrorCode::Ok,"IF load"); run_end(vm);
            require(callback.variables[101]==condition.second,"computed IF branch mismatch");
        });
    }
    for (Word token : {0xa000002fU,0xa0000025U,0xa0000021U,0xa200002bU,0xa200002dU,
                       0xa200002aU,0xa2000021U,0xa200003dU,0xa500003dU,0xa3000026U,0xa600003cU}) {
        suite.test("operator acceptance and expression shape " + std::to_string(token),[&] {
            Callback callback; nnn::CExecScript vm(callback);
            const bool valid=token==0xa000002fU || token==0xa0000025U || token==0xa200003dU ||
                             token==0xa500003dU || token==0xa3000026U;
            if (valid) {
                status_is(vm.LoadBytes(expression_program({var102,number(1),number(2),token,assign})),ErrorCode::Ok,"operator load");
                run_end(vm);
                const Value expected=token==0xa0000025U || token==0xa3000026U ? 1 : 0;
                require(callback.variables[102]==expected && callback.events==std::vector<std::string>{
                    variable_event("set_var",102,expected),variable_event("get_var",102,expected)},"operator result or callbacks");
            } else {
                const auto expected=token==0xa600003cU ? ErrorCode::UnsupportedExpression : ErrorCode::InvalidFormat;
                status_is(vm.LoadBytes(expression_program({number(1),number(2),token})),expected,"operator shape");
                require(!vm.IsLoaded()&&callback.events.empty(),"invalid operator shape reached callback");
            }
        });
    }
    for (const auto& tokens : std::vector<std::vector<Word>>{
             {number(1),add},{number(1),number(2),number(3),add},
             {number(1),number(2),add,number(7),assign},
             {number(1),number(2),less,number(7),assign},
             {var100,number(1),number(2),add,number(7),assign}}) {
        suite.test("invalid new stack/lvalue shape " + std::to_string(tokens.size()) + "/" + std::to_string(tokens.back()),[&] {
            Callback callback; nnn::CExecScript vm(callback);
            status_is(vm.LoadBytes(expression_program(tokens)),ErrorCode::InvalidFormat,"stack shape");
            require(callback.events.empty()&&!vm.IsLoaded(),"malformed expression caused effects");
        });
    }
    for (Word operation : {add,subtract,multiply,less,greater,less_equal,greater_equal,not_equal}) {
        suite.test("expression record work is budgeted before evaluation " + std::to_string(operation),[&] {
            Callback callback; callback.variables[100]=2; callback.variables[101]=3;
            nnn::CExecScript vm(callback);
            status_is(vm.LoadBytes(expression_program({var102,var100,var101,operation,assign})),ErrorCode::Ok,"budget load");
            const auto result=vm.Exec(6);
            status_is(result.status,ErrorCode::BudgetExhausted,"new operator budget");
            require(result.work_used==1 && callback.events.empty() && vm.GetPC()==32,"operator resolution bypassed budget");
        });
    }
    suite.test("unfunded expression performs no reads",[] {
        Callback callback; callback.variables[100]=2; callback.variables[101]=3;
        nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(expression_program({var102,var100,var101,add,assign})),ErrorCode::Ok,"partial budget load");
        const auto result=vm.Exec(7);
        status_is(result.status,ErrorCode::BudgetExhausted,"partial resolve budget");
        require(result.work_used==1 && result.pc==32 && callback.events.empty(),"unfunded expression produced effects");
        const auto funded=vm.Exec(10);
        status_is(funded.status,ErrorCode::BudgetExhausted,"funded expression boundary");
        require(funded.work_used==10 && funded.pc==42 && callback.events==std::vector<std::string>{
            variable_event("get_var",100,2),variable_event("get_var",101,3),variable_event("set_var",102,5),
            variable_event("get_var",102,5)},"funded expression callback sequence");
    });
    suite.test("unsupported expression replacement preserves valid new program",[] {
        Callback callback; nnn::CExecScript vm(callback);
        const auto good=expression_program({var100,number(4),number(5),add,assign});
        status_is(vm.LoadBytes(good,"A"),ErrorCode::Ok,"transaction A load"); run_end(vm);
        const auto old_pc=vm.GetPC(); const auto old_events=callback.events;
        status_is(vm.LoadBytes(expression_program({number(1),number(2),0xa600003cU}),"B"),ErrorCode::UnsupportedExpression,"transaction B load");
        require(vm.Identity()=="A"&&vm.GetPC()==old_pc&&callback.events==old_events,"failed replacement changed new program state");
        run_end(vm);
    });
    suite.test("new binary resolution callback failure clears guard",[] {
        Callback callback; callback.on_get=[](Word){throw std::runtime_error("controlled binary read failure");};
        nnn::CExecScript vm(callback);
        const auto bytes=expression_program({var102,var100,var101,multiply,assign});
        status_is(vm.LoadBytes(bytes),ErrorCode::Ok,"throwing binary load");
        status_is(vm.Exec(100).status,ErrorCode::CallbackFailed,"binary callback error");
        callback.on_get={};
        status_is(vm.LoadBytes(bytes,"retry"),ErrorCode::Ok,"guard recovery load"); run_end(vm);
    });
    suite.test("new binary callback reentry cannot replace program",[] {
        Callback callback; nnn::CExecScript vm(callback);
        const auto bytes=expression_program({var102,var100,var101,multiply,assign});
        callback.on_get=[&](Word){
            status_is(vm.Exec(100).status,ErrorCode::Busy,"binary reentrant Exec");
            status_is(vm.LoadBytes(bytes,"inner"),ErrorCode::Busy,"binary reentrant load");
        };
        status_is(vm.LoadBytes(bytes,"outer"),ErrorCode::Ok,"binary outer load"); run_end(vm);
        require(vm.Identity()=="outer","binary callback replaced program");
    });
    std::cout << "EXPRESSION RESULT " << suite.passed << " passed, " << suite.failed << " failed\n";
    return suite.failed==0 ? 0 : 1;
}
