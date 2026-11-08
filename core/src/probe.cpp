#include "nnn/script.hpp"
#include "nnn/scheduler.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string quote(const std::string& value) {
    std::ostringstream output;
    output << '"';
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\') output << '\\' << character;
        else if (byte < 0x20U || byte >= 0x80U) {
            output << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(byte) << std::dec;
        } else output << character;
    }
    output << '"';
    return output.str();
}

struct Callback final : nnn::CScriptCallBack {
    std::map<nnn::Word, nnn::Value> variables;
    std::vector<std::string> events;

    void ReceiveScriptCommand(nnn::Word code, nnn::Value para1, const std::string& text, nnn::Value para3) override {
        std::ostringstream event;
        event << "{\"kind\":\"script\",\"code\":" << code << ",\"para1\":" << para1
              << ",\"text_ascii\":" << quote(text) << ",\"para3\":" << para3 << '}';
        events.push_back(event.str());
    }
    void PreReceiveScriptData(nnn::Word code, nnn::Value kind, const std::string& text) override {
        events.push_back("{\"kind\":\"pre_data\",\"code\":"+std::to_string(code)+",\"para1\":"+
                         std::to_string(kind)+",\"text_ascii\":"+quote(text)+"}");
    }
    bool ReceiveNumericScriptCommand(nnn::Word code, nnn::Value, const std::vector<nnn::Value>& values, nnn::Value tail) override {
        std::ostringstream event;
        event << "{\"kind\":\"numeric_script\",\"code\":" << code << ",\"parameters\":[";
        for (std::size_t i=0; i<values.size(); ++i) { if (i) event << ','; event << values[i]; }
        event << "],\"para3\":" << tail << '}';
        events.push_back(event.str());
        return true;
    }
    void ReceiveUserCommand0(nnn::Word code, const std::vector<nnn::Value>& parameters) override {
        std::ostringstream event;
        event << "{\"kind\":\"user_command\",\"code\":" << code << ",\"parameters\":[";
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index != 0) event << ',';
            event << parameters[index];
        }
        event << "]}";
        events.push_back(event.str());
    }
    void ReceiveUserFunction0(nnn::Word code, const std::vector<nnn::Value>& values) override {
        events.push_back("{\"kind\":\"user_function\",\"code\":"+std::to_string(code)+",\"count\":"+std::to_string(values.size())+"}");
    }
    nnn::Value GetVarData(nnn::Word index) override {
        const auto value = variables[index];
        record("get_var", index, value);
        return value;
    }
    nnn::Value GetCalcuVarData(nnn::Word index) override {
        const auto value = variables[index];
        record("get_calcu_var", index, value);
        return value;
    }
    void SetVarData(nnn::Word index, nnn::Value value) override {
        variables[index] = value;
        record("set_var", index, value);
    }
    void record(const char* kind, nnn::Word index, nnn::Value value) {
        std::ostringstream event;
        event << "{\"kind\":" << quote(kind) << ",\"index\":" << index << ",\"value\":" << value << '}';
        events.push_back(event.str());
    }
};

std::uint64_t number(const std::string& value) {
    if (value.empty() || value.front() == '-') throw std::invalid_argument("Expected an unsigned number");
    std::size_t used = 0;
    const auto parsed = std::stoull(value, &used, 0);
    if (used != value.size()) throw std::invalid_argument("Invalid numeric argument");
    return parsed;
}

void emit_status(const nnn::Status& status) {
    std::cout << "{\"code\":" << quote(nnn::error_name(status.code))
              << ",\"word\":" << status.word << ",\"detail\":" << quote(status.detail) << '}';
}

// Headless game acceptance. Text and media requests are counted without exporting game assets.
struct GameHost final : nnn::CScriptCallBack {
    std::array<nnn::Value,32768> variables{};
    std::size_t lines=0, voices=0, draws=0, choices=0, transfers=0;
    std::map<std::pair<nnn::Word,nnn::Word>,std::size_t> skipped;
    void ReceiveScriptCommand(nnn::Word op,nnn::Value,const std::string&,nnn::Value) override {
        if (op>=0x22220001U && op<=0x22220003U) ++lines;
    }
    void PreReceiveScriptData(nnn::Word,nnn::Value,const std::string&) override {}
    bool ReceiveNumericScriptCommand(nnn::Word op,nnn::Value,const std::vector<nnn::Value>&,nnn::Value) override {
        if (op==0x33330008U) ++voices;
        if (op==0x22220004U || op==0x22220005U) ++draws;
        // Selection is handled by the driver. Rendering and audio use the VM's real no-op path.
        return op==0x22220006U;
    }
    void UnhandledInstruction(nnn::WordIndex,nnn::Word identify,nnn::Word op) override { ++skipped[{identify,op}]; }
    void ReceiveUserFunction0(nnn::Word,const std::vector<nnn::Value>&) override {}
    void ReceiveUserCommand0(nnn::Word,const std::vector<nnn::Value>&) override {}
    nnn::Value GetVarData(nnn::Word id) override { return variables.at(id); }
    nnn::Value GetCalcuVarData(nnn::Word id) override {
        return (id&0x8000U) ? static_cast<nnn::Value>(id&0x7fffU) : variables.at(id);
    }
    void SetVarData(nnn::Word id,nnn::Value value) override { variables.at(id)=value; }
};

std::string folded(std::string value) {
    for (char& c : value) if (c>='A' && c<='Z') c=static_cast<char>(c-'A'+'a');
    return value;
}

int check_directory(const std::filesystem::path& game, nnn::Word preferred,
                    const std::map<nnn::Word,nnn::Value>& initial) {
    namespace fs=std::filesystem;
    const auto spt=fs::is_directory(game/"spt") ? game/"spt" : game;
    const auto config=nnn::read_script_file((spt/"function.xtx").string());
    if (!config.status) throw std::runtime_error(config.status.detail);
    const auto functions=nnn::parse_function_registry(config.bytes);
    if (!functions.status) throw std::runtime_error(functions.status.detail);
    std::map<std::string,fs::path> files;
    for (const auto& file : fs::directory_iterator(spt)) {
        if (file.is_regular_file() && folded(file.path().extension().string())==".spt")
            files.emplace("spt/"+folded(file.path().filename().string()),file.path());
    }
    if (files.empty()) throw std::runtime_error("No .spt files in game directory");
    const auto reader=[&](const std::string& name) -> nnn::ReadResult {
        const auto file=files.find(folded(name));
        if (file==files.end()) return {{nnn::ErrorCode::NotFound,nnn::no_word,"Missing child script: "+name},{},0};
        return nnn::read_script_file(file->second.string());
    };
    std::size_t loaded=0, terminal=0, bounded=0, errors=0, lines=0, voices=0, draws=0, choices=0, transfers=0;
    std::map<std::pair<nnn::Word,nnn::Word>,std::size_t> skipped;
    for (const auto& file : files) {
        GameHost host;
        for (const auto& v : initial) host.variables.at(v.first)=v.second;
        nnn::ScriptScheduler vm(host,reader,&functions.registry);
        auto status=vm.LoadFile(file.first);
        const char* stop="error";
        if (status) {
            ++loaded;
            stop="step-limit";
            for (unsigned step=0; step<100000U; ++step) {
                const auto result=vm.Step(250000);
                status=result.status;
                if (!status) {
                    stop=status.code==nnn::ErrorCode::BudgetExhausted ? "work-limit" : "error";
                    break;
                }
                host.transfers+=result.transitions;
                if (result.return_code==0x22220006U) {
                    const auto* pending=vm.GetVM(vm.ActiveMode())->PendingSelection();
                    if (!pending || pending->options.empty()) throw std::runtime_error("Empty pending selection");
                    const auto choice=std::min<nnn::Word>(preferred,static_cast<nnn::Word>(pending->options.size()));
                    status=vm.Choose(choice);
                    if (!status) { stop="error"; break; }
                    ++host.choices;
                } else if (result.return_code==0x55560005U || result.return_code==0x5556000bU ||
                           (result.return_code==0x55560006U && !result.transitions)) {
                    stop=result.return_code==0x55560005U ? "END" : result.return_code==0x5556000bU ? "EXIT" : "NEXT";
                    ++terminal;
                    break;
                }
            }
        }
        if (std::string(stop)=="error") ++errors;
        if (std::string(stop)=="work-limit" || std::string(stop)=="step-limit") ++bounded;
        std::cout<<file.second.filename().string()<<"  "<<stop<<"  lines="<<host.lines<<"  choices="<<host.choices
                 <<"  transfers="<<host.transfers;
        if (!status) std::cout<<"  "<<nnn::error_name(status.code)<<" at "<<status.word<<" "<<status.detail;
        std::cout<<'\n';
        lines+=host.lines; voices+=host.voices; draws+=host.draws; choices+=host.choices; transfers+=host.transfers;
        for (const auto& item : host.skipped) skipped[item.first]+=item.second;
    }
    for (const auto& item : skipped) {
        const auto identify=item.first.first;
        if (identify!=0x66660006U && identify!=0x66660007U) ++errors;
        std::cout<<"No-op 0x"<<std::hex<<identify<<"/0x"<<item.first.second<<std::dec<<"  "<<item.second<<'\n';
    }
    std::cout<<"Loaded "<<loaded<<'/'<<files.size()<<", terminal "<<terminal<<", bounded "<<bounded<<", errors "<<errors
             <<", dialogue "<<lines<<", voice "<<voices<<", draw/overlap "<<draws<<", choices "<<choices
             <<", file transfers "<<transfers<<'\n';
    return loaded==files.size() && errors==0 && bounded==0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc>=3 && std::string(argv[1])=="--game-dir") {
            nnn::Word choice=1;
            std::map<nnn::Word,nnn::Value> initial;
            for (int i=3; i<argc; ++i) {
                const std::string option=argv[i];
                if (option=="--choice" && i+1<argc) {
                    const auto value=number(argv[++i]);
                    if (!value || value>UINT32_MAX) throw std::invalid_argument("Invalid one-based choice");
                    choice=static_cast<nnn::Word>(value);
                } else if (option=="--set" && i+2<argc) {
                    const auto id=number(argv[++i]);
                    const std::string raw=argv[++i];
                    std::size_t consumed=0;
                    const auto value=std::stoll(raw,&consumed,0);
                    if (id>=32768 || consumed!=raw.size() || value<INT32_MIN || value>INT32_MAX) throw std::invalid_argument("Invalid initial variable");
                    initial[static_cast<nnn::Word>(id)]=static_cast<nnn::Value>(value);
                } else throw std::invalid_argument("Usage: --game-dir ROOT [--choice N] [--set INDEX VALUE]");
            }
            return check_directory(argv[2],choice,initial);
        }
        nnn::DecodeParameters decode;
        std::uint64_t budget = 10000;
        std::uint64_t max_calls = 64;
        std::string filename;
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--xor" || argument == "--key-step" || argument == "--increment-step" ||
                argument == "--budget" || argument == "--max-calls") {
                if (++index >= argc) throw std::invalid_argument("Missing option value");
                const auto value = number(argv[index]);
                if (argument == "--budget") budget = value;
                else if (argument == "--max-calls") {
                    if (value == 0 || value > 10000) throw std::invalid_argument("max-calls must be 1..10000");
                    max_calls = value;
                } else {
                    if (value > UINT32_MAX) throw std::out_of_range("Decoder values must fit uint32");
                    if (argument == "--xor") decode.xor_key = static_cast<nnn::Word>(value);
                    else if (argument == "--key-step") decode.key_step = static_cast<nnn::Word>(value);
                    else decode.increment_step = static_cast<nnn::Word>(value);
                }
            } else if (!argument.empty() && argument.front() != '-' && filename.empty()) filename = argument;
            else throw std::invalid_argument("Unknown option or extra path");
        }
        if (filename.empty()) {
            std::cerr << "Usage: nnn-script-probe [--xor N --key-step N --increment-step N --budget N --max-calls N] FILE\n";
            return 2;
        }
        Callback callback;
        nnn::CExecScript vm(callback);
        auto status = vm.LoadFile(filename, decode);
        std::cout << "{\"loaded\":" << (status ? "true" : "false") << ",\"load_status\":";
        emit_status(status);
        if (!status) {
            std::cout << ",\"callback_events\":[],\"end_reached\":false}\n";
            return 1;
        }
        bool ended = false;
        std::cout << ",\"exec_calls\":[";
        for (std::uint64_t call = 0; call < max_calls; ++call) {
            const auto result = vm.Exec(budget);
            if (call != 0) std::cout << ',';
            std::cout << "{\"return\":" << result.return_code << ",\"pc_after\":" << result.pc
                      << ",\"work_used\":" << result.work_used << ",\"status\":";
            emit_status(result.status);
            std::cout << '}';
            status = result.status;
            if (!status) break;
            if (result.return_code == 0x55560005U) { ended = true; break; }
        }
        std::cout << "],\"callback_events\":[";
        for (std::size_t index = 0; index < callback.events.size(); ++index) {
            if (index != 0) std::cout << ',';
            std::cout << callback.events[index];
        }
        std::cout << "],\"final_variables\":{";
        bool first = true;
        for (const auto& entry : callback.variables) {
            if (!first) std::cout << ',';
            first = false;
            std::cout << quote(std::to_string(entry.first)) << ':' << entry.second;
        }
        std::cout << "},\"end_reached\":" << (ended ? "true" : "false") << "}\n";
        return status && ended ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Probe error: " << error.what() << '\n';
        return 2;
    }
}
