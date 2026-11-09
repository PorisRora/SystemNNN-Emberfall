#include "nnn/script.hpp"
#include "nnn/chart_graph.hpp"
#include "nnn/inventory_catalog.hpp"
#include "nnn/optional_catalog.hpp"
#include "nnn/progress_views.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#ifndef NNN_FIXTURE_DIR
#error NNN_FIXTURE_DIR must point to the checked-in fixtures
#endif
#ifndef NNN_TEST_WORK_DIR
#error NNN_TEST_WORK_DIR must point inside this test build directory
#endif

int public_header_contract();

namespace {
using nnn::Byte;
using nnn::Word;
using nnn::Value;
using nnn::ErrorCode;
constexpr Word data = 0x66660001;
constexpr Word control = 0x66660002;
constexpr Word command = 0x66660003;
constexpr Word calcu = 0x66660005;
constexpr Word end_code = 0x55560005;
constexpr Word goto_code = 0x55560020;
constexpr Word print_code = 0x22220001;
constexpr Word user_return = 0x11110001;
constexpr Word story_code = 0x33330051;
constexpr Word expression = 0xa0000000;
constexpr Word assignment = 0xa000003d;
constexpr Word equal = 0xa300003d;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void status_is(const nnn::Status& status, ErrorCode expected, const std::string& context) {
    require(status.code == expected,
            context + ": expected " + nnn::error_name(expected) + ", got "
            + nnn::error_name(status.code) + " (" + status.detail + ")");
}

std::vector<Byte> read_bytes(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    require(static_cast<bool>(stream), "cannot open test fixture: " + file.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::vector<Word> fixture_words(const std::string& name) {
    // These are the byte-for-byte DW-001 expected word arrays, not output from
    // the decoder under test. Only JSON's unsigned numeric-array form is used.
    std::ifstream input(std::filesystem::path(NNN_FIXTURE_DIR) / (name + ".decoded_words.json"));
    require(static_cast<bool>(input), "cannot open fixed word expectation");
    char punctuation = 0;
    input >> punctuation;
    require(punctuation == '[', "invalid expected-word array");
    std::vector<Word> words;
    input >> std::ws;
    while (input.peek() != ']') {
        std::uint64_t value = 0;
        input >> value;
        require(input.good() && value <= UINT32_MAX, "invalid fixed expected word");
        words.push_back(static_cast<Word>(value));
        input >> std::ws;
        if (input.peek() == ',') { input.get(); input >> std::ws; }
        else require(input.peek() == ']', "invalid expected-word separator");
    }
    return words;
}

std::vector<Byte> little_endian(const std::vector<Word>& words) {
    std::vector<Byte> bytes;
    bytes.reserve(words.size() * 4);
    for (Word word : words)
        for (unsigned shift = 0; shift != 32; shift += 8)
            bytes.push_back(static_cast<Byte>((word >> shift) & 0xff));
    return bytes;
}

std::vector<Byte> default_encrypt(std::vector<Word> words) {
    // Test-only mutation input: default encryption is bytewise complement.
    // Positive decode tests below use fixed ciphertext/expected constants.
    for (Word& word : words) word ^= UINT32_MAX;
    return little_endian(words);
}

std::vector<Word> minimal_with_code(const std::vector<Word>& code) {
    const auto original = fixture_words("minimal_end");
    std::vector<Word> result(original.begin(), original.begin() + 32);
    result.insert(result.end(), code.begin(), code.end());
    result.insert(result.end(), original.begin() + 35, original.end());
    const Word delta = static_cast<Word>(code.size() - 3);
    for (std::size_t field : {5U, 7U, 18U, 20U, 21U, 22U, 23U}) result[field] += delta;
    return result;
}

std::vector<Word> translated_text_tables(bool nonempty_constants = false) {
    const auto original = fixture_words("minimal_end");
    std::vector<Word> words(original.begin(),original.begin()+32);
    words.insert(words.end(),{6,0x66660006,print_code,0,0,0,3,control,end_code});
    // Actual boundaries: MESSAGE41..47, STRING47..53, then INT32/SUB.
    // The retained text lengths describe the older, larger translation source.
    words[4]=1; words[5]=45; words[6]=1; words[7]=51;
    words.insert(words.end(),{12,data,0x55550002,0x44440002,46,0x4d});
    words.insert(words.end(),{8,data,0x55550002,0x44440001,52,0x53});
    words[18]=200; // The old optional constant pointer was not relocated.
    words.insert(words.end(),{nonempty_constants?5U:4U,data,0x55550002,0x44440004});
    if(nonempty_constants)words.push_back(50000);
    for(const auto& table:std::vector<std::pair<Word,Word>>{{20,0x44440005},{21,0x44440006},{22,0x44440007},{23,0x44440008}}) {
        words[table.first]=static_cast<Word>(words.size()+4);
        words.insert(words.end(),{4,data,0x55550002,table.second});
    }
    return words;
}

struct Event {
    std::string kind;
    Word code = 0;
    Word index = 0;
    Value value = 0;
    Value para1 = 0;
    Value para3 = 0;
    std::string text;
    std::vector<Value> parameters;

    bool operator==(const Event& other) const {
        return std::tie(kind, code, index, value, para1, para3, text, parameters)
            == std::tie(other.kind, other.code, other.index, other.value,
                        other.para1, other.para3, other.text, other.parameters);
    }
};

Event variable_event(std::string kind, Word index, Value value) {
    Event event;
    event.kind = std::move(kind); event.index = index; event.value = value;
    return event;
}

Event script_event(Word code, Value para1, std::string text) {
    Event event;
    event.kind = "script"; event.code = code; event.para1 = para1; event.text = std::move(text);
    return event;
}

struct Callback final : nnn::CScriptCallBack {
    std::map<Word, Value> variables;
    std::vector<Event> events;
    std::vector<std::tuple<Word,Word,Word>> skipped;
    std::function<void()> on_set;
    bool throw_on_set = false;

    void ReceiveScriptCommand(Word code, Value para1, const std::string& text, Value para3) override {
        Event event = script_event(code, para1, text);
        event.para3 = para3;
        events.push_back(std::move(event));
    }
    void PreReceiveScriptData(Word, Value, const std::string&) override {
        throw std::runtime_error("unsupported pre-data callback escaped validation");
    }
    void ReceiveUserCommand0(Word code, const std::vector<Value>& parameters) override {
        Event event; event.kind = "user_command"; event.code = code; event.parameters = parameters;
        events.push_back(std::move(event));
    }
    void ReceiveUserFunction0(Word, const std::vector<Value>&) override {
        throw std::runtime_error("unsupported user-function callback escaped validation");
    }
    Value GetVarData(Word index) override {
        const Value value = variables[index];
        events.push_back(variable_event("get_var", index, value));
        return value;
    }
    void UnhandledInstruction(Word pc,Word identify,Word opcode) override { skipped.emplace_back(pc,identify,opcode); }
    Value GetCalcuVarData(Word index) override {
        const Value value = variables[index];
        events.push_back(variable_event("get_calcu_var", index, value));
        return value;
    }
    void SetVarData(Word index, Value value) override {
        if (throw_on_set) throw std::runtime_error("controlled callback failure");
        if (on_set) on_set();
        variables[index] = value;
        events.push_back(variable_event("set_var", index, value));
    }
};

std::vector<Event> golden_events() {
    std::vector<Event> events {
        variable_event("set_var",100,7), variable_event("get_var",100,7),
        variable_event("get_var",100,7), variable_event("get_var",100,7),
        script_event(print_code,1001,"DW001 HELLO"),
        script_event(story_code,1,"DW001-SYNTHETIC"),
        variable_event("set_var",102,50000), variable_event("get_var",102,50000),
        variable_event("get_calcu_var",100,7),
    };
    Event user; user.kind="user_command"; user.code=0x1234; user.parameters={7,3};
    events.push_back(std::move(user));
    events.push_back(script_event(print_code,1002,"DW001 DONE"));
    return events;
}

std::vector<Word> vm_words(const nnn::CExecScript& vm) {
    std::vector<Word> result;
    for (std::size_t i = 0; i < vm.WordCount(); ++i) {
        Word word = 0;
        require(vm.GetData(static_cast<Word>(i),word), "GetData failed inside program");
        result.push_back(word);
    }
    return result;
}

void run_flow(nnn::CExecScript& vm, Callback& callback) {
    const std::vector<std::pair<Word,Word>> expected {{print_code,70},{user_return,102},{print_code,108},{end_code,108}};
    for (const auto& item : expected) {
        const auto result = vm.Exec(100000);
        status_is(result.status, ErrorCode::Ok, "flow Exec");
        require(result.return_code == item.first && result.pc == item.second
                && vm.GetPC() == item.second, "flow return/PC differs from DW-001");
    }
    require(callback.events == golden_events(), "flow callback sequence differs from DW-001");
    require(callback.variables[100] == 7 && callback.variables[101] == 0
            && callback.variables[102] == 50000, "flow final variables differ from DW-001");
}

struct Suite {
    int passed = 0;
    int failed = 0;
    void test(const std::string& name, const std::function<void()>& body) {
        try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
};

void malformed_case(Suite& suite, const std::string& name,
                    const std::function<void(std::vector<Word>&)>& mutate,
                    ErrorCode expected = ErrorCode::InvalidFormat) {
    suite.test(name,[&] {
        auto words = fixture_words("flow"); mutate(words);
        Callback callback; nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(default_encrypt(words), name),expected,name);
        require(!vm.IsLoaded() && vm.WordCount()==0 && vm.GetPC()==0, "failed initial load installed state");
        require(callback.events.empty(), "load failure emitted a callback");
        status_is(vm.Exec().status,ErrorCode::NotLoaded,"Exec after failed initial load");
        require(callback.events.empty(), "Exec after failed initial load emitted a callback");
    });
}

void write_file(const std::filesystem::path& path, const std::vector<Byte>& bytes) {
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output),"cannot write owned test file");
}

} // namespace

int main() {
    Suite suite;
    suite.test("public header is self-contained",[] { require(public_header_contract()==0,"header contract"); });
    suite.test("fixed little-endian and wrapping decode vectors",[] {
        const auto cipher = little_endian({0,UINT32_MAX,0x80000000,0x01234567});
        const auto decoded = nnn::decode_words(cipher,{0xfffffffe,3,0x80000001});
        status_is(decoded.status,ErrorCode::Ok,"fixed wrap vector");
        // Hand-calculated; key progression FFFFFFFE,1,4,7 and increments
        // 0,80000001,2,80000003. No generator/encoder computes this expectation.
        require(decoded.words==std::vector<Word>({0xfffffffe,0x7fffffff,0x80000006,0x81234563}),"32-bit wrap vector");
        const auto advancing = nnn::decode_words(little_endian({0,1,2,3}),{0x10,1,0});
        status_is(advancing.status,ErrorCode::Ok,"advancing XOR");
        require(advancing.words==std::vector<Word>({16,16,16,16}),"incrementing key regression");
        const std::vector<Byte> bytes {0x78,0x56,0x34,0x12};
        const auto endian=nnn::decode_words(bytes,{0,0,0});
        status_is(endian.status,ErrorCode::Ok,"little-endian vector");
        require(endian.words==std::vector<Word>({0x12345678}),"little-endian word assembly");
    });
    for (const std::string name : {"minimal_end","flow","flow_evolving_key"}) {
        suite.test("unchanged DW-001 fixture: "+name,[name] {
            Callback callback; nnn::CExecScript vm(callback);
            const nnn::DecodeParameters parameters = name=="flow_evolving_key"
                ? nnn::DecodeParameters{0x12345678,0x01010101,3} : nnn::DecodeParameters{};
            const auto path=std::filesystem::path(NNN_FIXTURE_DIR)/(name+".spt");
            status_is(vm.LoadFile(path.string(),parameters),ErrorCode::Ok,"fixture LoadFile");
            require(vm_words(vm)==fixture_words(name),"loaded plaintext differs from fixed word expectation");
            require(callback.events.empty(),"loading emitted callback");
            if(name!="minimal_end") run_flow(vm,callback);
            else {
                for(int repeat=0;repeat!=2;++repeat) {
                    const auto result=vm.Exec(100);
                    status_is(result.status,ErrorCode::Ok,"minimal END");
                    require(result.return_code==end_code && result.pc==32,"END PC/return changed");
                }
                require(callback.events.empty(),"minimal END callback");
            }
            Word out=0;
            require(!vm.GetData(static_cast<Word>(vm.WordCount()),out),"GetData accepted end index");
        });
    }
    suite.test("VM owns program and host copies callback text",[] {
        auto input=read_bytes(std::filesystem::path(NNN_FIXTURE_DIR)/"flow.spt");
        Callback callback; nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(input,"owned-input"),ErrorCode::Ok,"owned-input load");
        std::fill(input.begin(),input.end(),0);
        run_flow(vm,callback);
        const auto copied=callback.events;
        status_is(vm.LoadBytes(read_bytes(std::filesystem::path(NNN_FIXTURE_DIR)/"minimal_end.spt"),"next"),ErrorCode::Ok,"replacement");
        require(callback.events==copied && callback.events[4].text=="DW001 HELLO", "host-owned text changed across load");
    });
    suite.test("signed INT32 table boundary remains explicit",[] {
        auto words=fixture_words("flow"); words[136]=0x80000000;
        Callback callback; nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(default_encrypt(words)),ErrorCode::Ok,"INT32_MIN table load");
        for(int call=0;call!=4;++call) status_is(vm.Exec(100000).status,ErrorCode::Ok,"INT32_MIN Exec");
        require(callback.variables[102]==std::numeric_limits<Value>::min(),"INT32_MIN bit pattern changed");
    });

    const auto scratch = std::filesystem::path(NNN_TEST_WORK_DIR)
        / std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(scratch);
    suite.test("game text keeps original defaults, raw substitutions and atomic system state",[&] {
        const auto root=scratch/"text";
        for (const auto* folder:{"init","nya","sav"}) std::filesystem::create_directories(root/folder);
        const std::string config="codeByte 1\nuseDefaultOkikae 1\nseiInitData First\nmeiInitData Cleared\n"
            "meiClearFlag 1\ndefaultNameSei Default\nresetNameByStartTitle 1\n";
        const std::string replacements="0,alpha\n7,local\nS7,shadowed\n";
        write_file(root/"init/game.xtx",{config.begin(),config.end()});
        write_file(root/"nya/okikae.xtx",{replacements.begin(),replacements.end()});
        nnn::RuntimeContext initial(root);
        require(initial.runtime_text().surname=="First" && initial.runtime_text().given_name.empty() &&
            initial.initial_names()[1]=="Cleared" && initial.game_default_names()[0]=="Default" &&
            initial.reset_name_on_title(),"original name defaults");
        const auto& local=initial.local_replacement_defaults();
        require(local[0]=="alpha" && local[7]=="local" && local[12]=="replace12" &&
            initial.system_replacement(7).empty(),"local defaults and empty SYSTEMFILE slot precedence");

        std::vector<Byte> archive;
        const auto add_record=[&](Word size,Word code,std::string_view label) {
            const auto at=archive.size();
            archive.resize(at+size);
            const auto header=little_endian({size,code});
            std::copy(header.begin(),header.end(),archive.begin()+at);
            std::copy(label.begin(),label.end(),archive.begin()+at+16);
            return at;
        };
        add_record(64,0,"FILE INFO      ");
        archive[32]=14;
        const auto system=add_record(2048,1,"SYSTEM DATA    ");
        const std::string stored_surname="Persisted";
        std::copy(stored_surname.begin(),stored_surname.end(),archive.begin()+system+224);
        add_record(32,5,"");
        add_record(544,6,"SYSTEMVAR      ");
        for (Word code=7;code<=9;++code) add_record(32,code,"");
        const auto role=add_record(1056,10,"VOICEFLAG      ");
        archive[role+32+4*6]=1;
        add_record(32,11,"");
        add_record(32,12,"");
        const auto okikae=add_record(6432,13,"OKIKAE         ");
        const std::string tunneled="\x81\x01#F";
        std::copy(tunneled.begin(),tunneled.end(),archive.begin()+okikae+32+7*64);
        add_record(32,14,"");
        const auto term=add_record(4128,15,"TERM           ");
        const auto voice=add_record(65568,16,"CHARAVOICEFLAG ");
        archive[term+4124]=3;
        archive[term+4127]=0x80;
        archive[voice+65567]=0x80;
        write_file(root/"sav/sysfile.org",archive);

        nnn::RuntimeContext runtime(root);
        require(!runtime.routing().separate_script_se,"validated classic .org uses shared SE controls");
        require(runtime.legacy_volume_defaults(),"validated classic .org selects the original slider defaults");
        const auto text=runtime.runtime_text();
        require(runtime.loaded_original_defaults() && text.surname==stored_surname && text.given_name.empty() &&
            text.system_replacements[7]==tunneled,"SYSTEMDATA offsets or OKIKAE raw bytes");
        require(runtime.optional_progress_view()->terms.back()==0x80000003U && runtime.term_unlocked(1023) &&
            runtime.term_read(1023) && runtime.voice_played(524287),"TERM and CHARAVOICEFLAG SYS offsets");
        require(runtime.voice_role_heard(6) && !runtime.voice_role_heard(5) && !runtime.voice_played(6),
            "VOICEFLAG role data stays separate from CHARAVOICEFLAG");
        require(!runtime.set_system_name("Rejected",std::string(16,'x')) &&
            !runtime.set_system_replacement(7,std::string("bad\0",4)) && runtime.runtime_text()==text,
            "invalid text changed live system state");
        auto fork=runtime.fork();
        require(fork->set_system_name("",std::nullopt) && fork->set_system_replacement(7,"") &&
            !(fork->preview_state()==runtime.preview_state()) && runtime.runtime_text()==text,"text fork isolation");
        const auto blank=fork->runtime_text();
        require(fork->restore_runtime_text(blank,true) && fork->runtime_text().surname=="First" &&
            fork->restore_runtime_text(blank) && fork->runtime_text().surname.empty(),"startup and exact restore differ");

        const auto local_at=[&](std::size_t index) -> const std::string& {return local[index];};
        const auto system_at=[&](std::size_t index) {return runtime.system_replacement(index);};
        require(nnn::expand_text_replacements("#L/#F/#0/#S7/#S/#U",true,text.surname,text.given_name,
            local_at,system_at)==stored_surname+"//alpha/"+tunneled+"//#U","single-byte command grammar");
        require(nnn::expand_text_replacements("#\x90\xa9/#\x96\xbc/#\x82\x4f\x82\x4f\x82\x50\x82\x51/#\x83\x56\x82\x56",
            false,text.surname,text.given_name,local_at,system_at)==stored_surname+"//replace12/"+tunneled,
            "wide command grammar and bounded leading zeros");
    });
    suite.test("legacy SE controls require a known archive or explicit legacy layout",[&] {
        const auto root=scratch/"sound-routing";
        std::filesystem::create_directories(root/"init");
        const auto unspecified=nnn::load_initial_runtime_parameters(root);
        require(unspecified.routing.separate_script_se&&!unspecified.legacy_volume_defaults,
            "missing defaults alone must not select the legacy controls");
        const std::string legacy="volumeNumber 8\nsoundBarFileName sound_bar\nsoundVolumePrintX 360\n";
        write_file(root/"init/config.xtx",{legacy.begin(),legacy.end()});
        const auto old=nnn::load_initial_runtime_parameters(root);
        require(!old.routing.separate_script_se&&old.legacy_volume_defaults,"explicit old sound layout");
        const std::string split="scriptseVolumeUseFlag 1\n";
        write_file(root/"init/game.xtx",{split.begin(),split.end()});
        const auto modern=nnn::load_initial_runtime_parameters(root);
        require(modern.routing.separate_script_se&&!modern.legacy_volume_defaults,"explicit modern split wins");
    });
    suite.test("optional catalogues use original term rows and grouped voice flags",[&] {
        const auto root=scratch/"catalogue";
        for (const auto* folder:{"init","nya/term/nested","spt"}) std::filesystem::create_directories(root/folder);
        const auto write_text=[&](const std::string& name,const std::string& data) {
            write_file(root/name,{data.begin(),data.end()});
        };
        write_text("init/printterm.xtx","termTypeNumber 2\ntermTypeData1_1 0\ntermTypeData2_1 7\n"
            "TERMPERPAGE 3\ntermPerPage 9\n");
        write_text("spt/termlist.xtx","zulu,7,1,stem\r\nAlpha,9,2,nested\\Description\r\nbeta,7,0,missing\r\n");
        write_text("nya/term/nested/description.xtx","first,,\x81\x09" "second\r\n");
        write_text("init/listenvoice.xtx","voiceCharaNumber 2\nvoicePerPage 4\nunknownVoice ?????\n");
        write_text("nya/charavoicelist2.xtx","1,SAMPLE01,Heard\n0,sample02,Always\n2,sample02,VariableIndex\n"
            "isOpen,sample02,VariableName\nmissingVar,sample02,Unknown\n1,absent,MissingIndex\n");
        std::vector<Byte> flags(32+16*3);
        const std::string label="VOICEFLAGANDNAME";
        std::copy(label.begin(),label.end(),flags.begin());
        flags[16]=3;
        const auto voice_record=[&](std::size_t index,const std::string& name,Byte delta,Word flag) {
            auto entry=flags.begin()+32+16*index;
            std::copy(name.begin(),name.end(),entry);
            entry[10]=delta;
            const auto encoded=little_endian({flag});
            std::copy(encoded.begin(),encoded.end(),entry+12);
        };
        voice_record(0,"sample01",0,31);
        voice_record(1,"sample01",1,32);
        voice_record(2,"sample02",0,524287);
        write_file(root/"spt/voiceflag.tbl",flags);

        nnn::RuntimeContext runtime(root);
        auto story=std::make_shared<nnn::PreviewStoryState>();
        story->vars[100]=7;
        runtime.set_preview_story(story);
        auto fork=runtime.fork();
        require(fork->preview_story()==story && fork->preview_state()==runtime.preview_state(),
            "fork did not keep decorative story seed");
        fork->set_preview_story({});
        require(fork->set_term_unlocked(0) && fork->set_voice_played(32) && fork->mark_voice_role_heard(31) &&
            !fork->set_voice_played(524288) && !fork->set_term_read(-1) &&
             !fork->mark_voice_role_heard(0) && !fork->mark_voice_role_heard(256) &&
             !runtime.term_unlocked(0) && !runtime.voice_played(32) && !runtime.voice_role_heard(31) &&
            !(fork->preview_state()==runtime.preview_state()) && runtime.preview_story()==story,"global progress fork isolation");
        runtime.restore_optional_progress(fork->optional_progress());
        require(runtime.term_unlocked(0) && runtime.voice_played(32) && runtime.voice_role_heard(31) &&
             !runtime.voice_played(31) && !runtime.voice_role_heard(32),"raw progress restore");

        const nnn::TermCatalog terms(root);
        const auto rows=terms.entries(0,runtime);
        require(terms.configured() && terms.category_count()==2 && terms.per_page()==3 && rows.size()==3 &&
            rows[0].id==1 && rows[0].type==9 && !rows[0].unlocked && !rows[0].is_new &&
            rows[1].id==2 && rows[2].id==0 && rows[2].sort==1 && rows[2].is_new,
            "term order, duplicate config or locked rows");
        require(terms.entries(1,runtime).size()==2 && runtime.set_term_read(0) &&
            !terms.entry(0,runtime)->is_new && terms.text(1)==std::vector<std::string>({"first","","\x81\x09" "second"}),
            "term type filter, new mark or raw description");

        const nnn::VoiceCatalog voices(root);
        const auto resolve=[](std::string_view name) -> Value {return name=="isOpen" ? 3 : -1;};
        const auto read=[](Value index) -> Value {return index==3 ? 1 : 0;};
        const auto list=voices.entries(1,runtime,resolve,read);
        require(voices.configured() && voices.character_count()==2 && voices.per_page()==4 && voices.unknown_title(false)=="?????" &&
            list.size()==6 && list[0].character==1 && list[0].id==0 && list[0].control=="1" &&
            list[0].played && list[0].available && list[0].filename=="SAMPLE01" && list[0].title=="Heard" &&
            list[1].available && !list[1].played && !list[2].available && list[3].available && list[4].available &&
            !voices.entry(0,0,runtime),"voice variants, raw replay filename, control and tab bounds");
        require(runtime.set_voice_played(31) && voices.missing_flag_ranges()==1 &&
            !voices.entry(1,5,runtime)->available,"missing voice range inherited a different clip's unlock");
        flags[32+16+10]=2;
        write_file(root/"spt/voiceflag.tbl",flags);
        bool invalid=false;
        try { (void)nnn::VoiceFlagTable(root); } catch (const std::runtime_error&) { invalid=true; }
        require(invalid,"out-of-bounds variant group accepted");
    });
    suite.test("achievement percentages and result visits follow source progress rules",[&] {
        const auto root=scratch/"progress-views";
        for (const auto* folder:{"init","nya/nested"}) std::filesystem::create_directories(root/folder);
        const auto write_text=[&](const std::string& name,const std::string& data) {
            write_file(root/name,{data.begin(),data.end()});
        };
        write_text("init/game.xtx","resultFlag 1\n");
        write_text("init/printachievement.xtx",
            "achievementNumber 3\nachievementType1 ENDING\nachievementParamNumber1 3\n"
            "achievement1Param1 0\nachievement1Param2 1\nachievement1Param3 900\n"
            "achievementType2 endingrange\nachievementParamNumber2 2\nachievement2Param1 -2147483648\n"
            "achievement2Param2 2147483647\nachievementType3 2\nachievementParamNumber3 2\n"
            "achievement3Param1 4\nachievement3Param2 2\n");
        write_text("init/printresult.xtx",
            "resultsMax 3\nresultsNumberX 2\nresultsNumberY 1\nnextMode TiTlE\n"
            "voiceFlag 1\nvoiceStartWait 2\nscrollStartWait 3\nfilenameResultsTag plate\n"
            "filenameDialogTag notify\nresultDescName nya\\nested\\result-list.xtx\n");
        write_text("nya/nested/result-list.xtx",
            "prefix,!,Previous,@skip,Line3,Line4,@,@\nfirst,voice01,Part1,@skip,Part2,@,@,@\n"
            "second,!noVoice,Middle,@,@,@,@,@\nthird,voice03,Last,@,@,@,@,@\n");

        nnn::RuntimeContext runtime(root);
        nnn::AchievementCatalog achievements(root);
        runtime.set_achievement(0,1);
        const auto first=achievements.entries(runtime);
        require(achievements.configured() && first.size()==3 && first[0].id==0 && first[0].percent==1 &&
            !first[0].completed && first[1].percent==1 && first[2].percent==0,
            "achievement average, positive minimum or invalid range");
        runtime.set_achievement(0,300);
        require(achievements.entries(runtime)[0].completed,"achievement completion threshold");
        runtime.set_achievement(-1,0);
        const auto reset=nnn::AchievementCatalog(root).entries(*runtime.fork());
        require(reset[0].completed && reset[0].percent==100 && reset[1].percent==0,
            "completed cache or partial progress did not follow source");

        const nnn::ResultCatalog results(root);
        require(results.configured() && results.next_mode()==nnn::RuntimeMode::Title && results.page_count()==2 &&
            results.per_page()==2 && results.plate_name(1)=="plate2" && results.dialog_name(2)=="notify03" &&
            results.plate_name(-1).empty() && results.dialog_name(3).empty(),"result setup, mode or asset numbering");
        require(runtime.set_result_if_clear(0,1) && runtime.set_result_if_clear(1,1) &&
            runtime.set_result_if_clear(3,1) && runtime.set_result_if_clear(4,1),"result state setup");
        auto visit=results.begin(runtime);
        require(visit.page==0 && visit.pending_ack==0 && results.entries(visit).size()==2 &&
            !results.entry(0,visit)->visible && !results.entry(0,visit)->hoverable &&
            !results.entry(1,visit) && !results.set_page(visit,1) && !results.acknowledge(visit,2,runtime),
            "result popup eligibility or ordering");
        require(results.acknowledge(visit,0,runtime) && runtime.result_value(1)==2 &&
            visit.pending_ack==2 && visit.page==1 && results.entry(0,visit)->visible &&
            results.acknowledge(visit,2,runtime) && visit.pending_ack==-1 && runtime.result_value(3)==2,
            "result acknowledgement sequence and page");
        const auto detail=results.entry(0,visit);
        require(detail && detail->label=="first" && detail->voice=="voice01" &&
            detail->lines==std::vector<std::string>({"Previous","","Line3","Line4"}) &&
            detail->scrolling_text=="Part1Part2","result description row offsets and sentinels");
        require(!results.hover(visit,0) && results.set_page(visit,0) && results.hover(visit,0) &&
            results.tick_hover(visit).empty() && results.tick_hover(visit)=="voice01" &&
            results.tick_hover(visit).empty() && visit.scroll_ticks==3,"result hover timer");
        require(runtime.set_result_if_clear(2,1) && !results.entry(1,visit) &&
            results.begin(runtime).pending_ack==1,"result visit must capture progress once");

        write_text("init/printresult.xtx","resultsMax 3\ndialogFlag 0\nnextMode 77\nresultDescName ..\\outside.xtx\n");
        const nnn::ResultCatalog no_dialog(root);
        auto quiet=no_dialog.begin(runtime);
        const auto missing=no_dialog.entry(1,quiet);
        require(quiet.pending_ack==-1 && missing && missing->visible && missing->voice.empty() &&
            missing->scrolling_text.empty() && static_cast<Value>(no_dialog.next_mode())==77 &&
            !no_dialog.acknowledge(quiet,1,runtime) && runtime.result_value(2)==1,
            "dialog-disabled or missing description fallback");
        const nnn::ResultCatalog unconfigured(root/"missing");
        require(!unconfigured.configured() && unconfigured.page_count()==0 &&
            unconfigured.entries(unconfigured.begin(runtime)).empty(),"unconfigured results invented rows");
    });
    suite.test("chart graph keeps source slots, routes, hit order and save boundaries",[&] {
        namespace chart=nnn::chart;
        const auto root=scratch/"chart";
        std::filesystem::create_directories(root/"init");
        std::filesystem::create_directories(root/"chart");
        const auto write_text=[&](const std::string& file,const std::string& text,bool encrypted=false) {
            std::vector<Byte> bytes(text.begin(),text.end());
            if (encrypted) for (auto& byte:bytes) byte^=0xffU;
            write_file(root/file,bytes);
        };
        write_text("init/selectchart.xtx","chartListNumber 2\nbasicChartNumber 1\nchartAngouFlag 1\n"
            "tagNameChartSlot GraphSlot\ntagNameBack GraphBack\ntagNameCenter GraphCenter\n"
            "slotAreaStartX 30\nslotAreaStartY 5\nslotSizeX 100\nslotSizeY 40\nbackPicSizeY2 1800\n"
            "chartOpenVar chart_open\nslotVarName now\nworkVarName prior\n"
            "autoNextOn 1\nuseSlotOnFunction 1\nuseSlotOpenFunction 1\nsetRouteFunctionName reach\n");
        write_text("chart/chart1.xtx","invalid plaintext should never be selected");
        write_text("chart/chart1.fxf","1600\n@1 1 200 400 3 4 One\n@3 2 250 430 Three\n"
            "@4 1 250 430 $locked Four !Preview4 !Unused\n@85 1 300 1000 1000 Later\n",true);
        write_text("chart/chart2.fxf","2200\n@1000 1 180 300 Next\n",true);
        const chart::Graph graph(root,1280,720);
        require(graph.configured() && graph.pages().size()==2 && graph.page(0)->extent==1860 &&
            graph.page(1)->back_tiles.size()==3 && graph.page(1)->back_tiles[2]=="sys/graphback2_3" &&
            graph.page(1)->center_asset=="sys/graphcenter2" && graph.setup().commands.front().name=="reach" &&
            graph.node(1000)->previous==std::vector<Value>{85} && !graph.node(2),"chart files, sources and predecessor mapping");
        const auto atlas=graph.atlas(85,200,80);
        require(atlas && graph.first_atlas()=="sys/graphslot1_1" && atlas->asset=="sys/graphslot1_22" &&
            atlas->source.x==100 && atlas->source.y==0 && atlas->source.w==100 &&
            graph.background_y(0,graph.scroll_limit(0))==-720 && graph.center_y(0,graph.scroll_limit(0),50)==-40,
            "chart atlas slicing and tile parallax");

        auto progress=graph.global_defaults();
        auto run=graph.run_defaults();
        Value opened=0,current=3;
        const auto read=[&](std::string_view key) -> std::optional<Value> {
            if (key=="chart_open") return opened;
            if (key=="now") return current;
            if (key=="prior") return 4;
            if (key=="locked" || key=="answer") return 0;
            return {};
        };
        require(graph.valid(progress) && graph.valid(run) && progress.seen.size()==2000 &&
            run.move_road.size()==2000 && run.can_select.size()==64 &&
            graph.apply_function(chart::Function::SetRoute,{2},progress,run,read).progress_changed &&
            graph.seen(progress,3) && !graph.complete(progress,3),"chart route flag layout and command");
        const auto visit=graph.begin_visit(1);
        const auto hit=graph.hit(visit,{280,395},progress,run,read);
        require(visit.scroll_y==40 && hit.candidate==4 && hit.access==chart::Access::Locked && hit.description==3 &&
            graph.description(0,4,progress)==std::vector<std::string_view>{"Preview4"} &&
            graph.branch(progress,run,1,3,read) && !graph.branch(progress,run,1,3,read)->restore_node &&
            !graph.branch(progress,run,1,4,read),"chart candidate, description and confirmed selection gates");
        require(graph.mark_route(progress,85) && graph.branch(progress,run,1,85,read)->confirm &&
            graph.branch(progress,run,1,85,read)->restore_node && !graph.page_available(1,progress,run,85,read),
            "chart revisits and unopened page");
        const auto unlock=graph.apply_function(chart::Function::OpenChart,{2},progress,run,read);
        require(unlock.save_system && unlock.write && unlock.write->name=="chart_open" && unlock.write->value==1,
            "chart open command writes configured bit and requests save");
        opened=unlock.write->value;
        require(graph.page_available(1,progress,run,85,read) &&
            graph.apply_function(chart::Function::SetRoute,{1},progress,run,read).progress_changed &&
            graph.apply_function(chart::Function::CheckCompSlot,{},progress,run,read).write->value==1 &&
            graph.apply_function(chart::Function::GetChartPercent,{0},progress,run,read).write->value==60,
            "chart activation, completion and basic-chart percent");

        const auto first=graph.enter_plan(0,99);
        const auto automatic=graph.enter_plan(85,1);
        const auto opened_slot=graph.apply_function(chart::Function::SlotOpen,{85},progress,run,read).open_slot;
        require(first.size()==1 && first[0].slot==1 && first[0].work_slot==0 &&
            automatic.size()==2 && automatic[1].slot==1000 && automatic[1].display_page==1 &&
            automatic[1].road && automatic[1].road->from==85 && automatic[1].road->edge==0 &&
            opened_slot && opened_slot->checkpoint.slot==3 && opened_slot->checkpoint.mark_seen==85 &&
            opened_slot->checkpoint.work_slot==85 && opened_slot->restore_work==4 &&
            graph.exit_plan(85,1)->mode==nnn::RuntimeMode::Title && graph.legacy_slot_path(1000)=="chart2/slot1000.dat",
            "ordered checkpoints and exact slotopen destination");

        require(graph.record_edge(run,{1,0}) && graph.apply_function(chart::Function::SlotOn,{3,0},progress,run,read).run_changed,
            "chart local road and selectability writes");
        progress.traversed[1]=1;
        require(graph.edge_style({85,0,40},{1,0},-1,progress,run,read)==chart::EdgeStyle::CurrentPath,
            "current road priority");
        graph.new_game(run);
        require(graph.edge_style({85,0,40},{1,0},-1,progress,run,read)==chart::EdgeStyle::Traversed &&
            graph.access(progress,run,1,3,read)==chart::Access::Locked,"new game resets roads while preserving selection bits");
        std::array<Value,1100> live{};
        live.fill(-1);
        const auto saved=chart::capture_variables([](std::size_t i){return static_cast<Value>(i+1U);});
        chart::restore_variables(saved,[&](std::size_t i,Value value){live[i]=value;});
        require(live[199]==200 && live[200]==-1 && live[299]==-1 && live[300]==301 &&
            live[999]==1000 && live[1000]==-1,"chart-only variable merge");
        write_text("chart/chart1.fxf","1600\n@1 1 200 400 1000 One\n",true);
        const chart::Graph sentinel(root,1280,720);
        const auto opening=sentinel.enter_plan(0,99);
        auto sentinel_run=sentinel.run_defaults();
        require(!sentinel.node(0) && opening.size()==2 && opening[0].slot==1 &&
            opening[0].work_slot==0 && opening[1].slot==1000 && opening[1].display_page==1 &&
            opening[1].road && opening[1].road->from==0 &&
            sentinel.record_edge(sentinel_run,*opening[1].road) && sentinel_run.move_road[0]==1 &&
            sentinel.enter_plan(3,99).empty() && !sentinel.edge_index(3,1000),
            "chart first-slot sentinel follows a unique cross-page road");
        write_text("chart/chart2.fxf","2200\n@99 1 180 300 invalid\n",true);
        bool invalid=false;
        try { (void)chart::Graph(root,1280,720); } catch (const std::runtime_error&) { invalid=true; }
        require(invalid && !chart::Graph(root/"missing",1280,720).configured(),"chart malformed or missing input fallback");
    });
    suite.test("inventory and shop preserve filtered rows and stage checked variable writes",[&] {
        namespace inventory=nnn::inventory;
        const auto root=scratch/"inventory";
        for (const auto* folder:{"init","nya","nnndir"}) std::filesystem::create_directories(root/folder);
        const auto write_text=[&](const std::string& file,const std::string& text,bool encrypted=false) {
            std::vector<Byte> bytes(text.begin(),text.end());
            if (encrypted) for (auto& byte:bytes) byte^=0xffU;
            write_file(root/file,bytes);
        };
        write_text("init/printitem.xtx","itemTypeNumber 2\nitemBlockX 2\nitemBlockY 1\n"
            "itemPrintX 70\nitemPrintY 60\nitemPrintNextX 95\nsuffix _owned\n");
        std::string item_text="%20 1 a IconA Base @ BaseLine";
        for (int variant=2;variant<=15;++variant)
            item_text+=" !Variant"+std::to_string(variant)+" Text"+std::to_string(variant);
        item_text+="\n%10 2 b IconB Category2\n%30 1 b IconB Unowned\n%40 1 a IconC Fallback\n";
        write_text("nya/haveitem.xtx",item_text);
        const inventory::ItemCatalog items(root);
        Value a=15,points=5,owned=3,free_count=0,available=1,seen_bits=0,more_bits=0;
        const auto read=[&](std::string_view key) -> std::optional<inventory::Variable> {
            if (key=="a") return inventory::Variable{1,a};
            if (key=="b") return inventory::Variable{2,-1};
            if (key=="money") return inventory::Variable{3,points};
            if (key=="owned") return inventory::Variable{4,owned};
            if (key=="available") return inventory::Variable{5,available};
            if (key=="seen_flags") return inventory::Variable{6,seen_bits};
            if (key=="more_flags") return inventory::Variable{7,more_bits};
            if (key=="free_owned") return inventory::Variable{8,free_count};
            if (key=="@") return inventory::Variable{9,1};
            return {};
        };
        const auto first=items.page(0,0,read);
        const auto second=items.page(0,1,read);
        require(items.configured() && items.category_count()==2 && first.page_count==2 && first.rows.size()==2 &&
            first.rows[0].serial==20 && first.rows[0].variant==15 && first.rows[0].name=="Variant15" &&
            first.rows[0].description==std::vector<std::string_view>{"Text15"} &&
            first.rows[0].asset=="sys/icona_owned" && first.rows[0].cell.x==70 && first.rows[0].cell.y==60 &&
            first.rows[1].id==2 && first.rows[1].availability==-1 && !first.rows[1].detail_selectable &&
            first.rows[1].asset=="sys/ta_noitem" && first.rows[1].cell.x==165 &&
            second.rows[0].position==2 && second.rows[0].variant==1 && second.rows[0].name=="Fallback" &&
            items.page(1,0,read).rows[0].serial==10,"item filtered order, variants or row geometry");
        a=16;
        require(items.page(0,0,read).rows[0].description==std::vector<std::string_view>({"","BaseLine"}),
            "undefined variant falls back to base and keeps blank lines");

        write_text("init/shop.xtx","itemTypeNumber 2\nitemBlockX 2\nitemBlockY 1\n"
            "pointVarName money\nappearVar1 seen_flags\nappearVar2 more_flags\ncanSelectLimit 1\ncanSelectNoPoint 1\n");
        write_text("nnndir/shopitem.txt","%99 1 2 1 Plain icon available owned");
        std::string shop_text=
            "%70 2 1 0 Other other available owned\n%40 1 7 3 Wanted wanted available owned First @ Third\n"
            "%41 1 7abc 0 Malformed bad available owned\n%42 1 -5 0 Negative negative available owned\n"
            "%43 1 0 0 Free free available free_owned\n%44 1 1 1 Hidden hidden @ owned\n"
            "%45 1 5 0 Alias alias available money\n%46 1 1 0 Unresolved none available @\n";
        for (int i=0;i<26;++i) shop_text+="%"+std::to_string(100+i)+" 1 1 0 More more available owned Text\n";
        shop_text+="%200 1 -5 0 AliasCredit alias available money\n"
            "%201 1 -0 0 ZeroCredit zero available free_owned\n"
            "%202 1 -2147483649 0 InvalidPrice bad available owned\n";
        write_text("nya/shopitem.fxf",shop_text,true);
        const inventory::ShopCatalog shop(root);
        const auto visit=shop.begin(read);
        const auto shelf=shop.page(0,0,visit,read);
        require(shop.configured() && shop.category_count()==2 && shelf.balance==5 &&
            shelf.rows[0].id==1 && shelf.rows[0].position==0 && shelf.rows[0].price==7 &&
            shelf.rows[0].sold_out && shelf.rows[0].detail_selectable && !shelf.rows[0].purchase_enabled &&
            shelf.rows[0].is_new && shelf.rows[0].description==std::vector<std::string_view>({"First","","Third"}) &&
            !shelf.rows[1].price && shop.entry(3)->price==-5 && !shop.plan_purchase(0,1,read) &&
            !shop.plan_purchase(0,2,read) && !shop.entry(36)->price && !shop.plan_purchase(0,36,read),
            "shop appearance, detail exceptions or strict price");

        const auto seen=shop.plan_seen(0,1,read);
        const auto high=shop.plan_seen(0,32,read);
        const auto next=shop.plan_seen(0,33,read);
        require(seen && seen->id==6 && seen->value==1 && seen_bits==0 &&
            high && high->id==6 && high->value==std::numeric_limits<Value>::min() &&
            next && next->id==7 && next->value==1 && !shop.plan_seen(0,5,read),"shop seen bits use filtered position");
        seen_bits=seen->value;
        require(shop.page(1,0,visit,read).rows[0].is_new &&
            !shop.page(1,0,shop.begin(read),read).rows[0].is_new,"shop New display uses visit source-index cache");

        points=8;owned=2;
        const auto purchase=shop.plan_purchase(0,1,read);
        const auto alias=shop.plan_purchase(0,6,read);
        const auto credit=shop.plan_purchase(0,3,read);
        const auto alias_credit=shop.plan_purchase(0,34,read);
        require(purchase && purchase->item_id==1 && purchase->writes.size()==2 &&
            purchase->writes[0].id==4 && purchase->writes[0].expected==2 && purchase->writes[0].value==3 &&
            purchase->writes[1].id==3 && purchase->writes[1].expected==8 && purchase->writes[1].value==1 &&
            alias && alias->writes.size()==1 && alias->writes[0].value==4 &&
            credit && credit->writes.size()==2 && credit->writes[0].value==3 && credit->writes[1].value==13 &&
            alias_credit && alias_credit->writes.size()==1 && alias_credit->writes[0].value==14 &&
            !shop.plan_purchase(1,1,read) && !shop.plan_purchase(0,5,read) && !shop.plan_purchase(0,7,read),
            "shop purchase plans validate category and both writable variables");
        available=0;
        require(!shop.plan_purchase(0,1,read),"shop confirm rechecks live appearance");
        available=1;
        free_count=std::numeric_limits<Value>::max();
        require(!shop.plan_purchase(0,4,read),"shop quantity cannot overflow");
        free_count=0;
        require(shop.plan_purchase(0,4,read)->writes[1].value==points && shop.entry(35)->price==0 &&
            shop.plan_purchase(0,35,read)->writes[1].value==points,"both signed and unsigned zero prices remain valid");
        points=-6;
        require(!shop.plan_purchase(0,3,read),"negative balance still has to meet signed price");
        points=std::numeric_limits<Value>::max()-5;
        require(shop.plan_purchase(0,3,read)->writes[1].value==std::numeric_limits<Value>::max() &&
            !shop.plan_purchase(0,34,read),"alias net credit cannot overflow");
        ++points;
        require(!shop.plan_purchase(0,3,read),"separate currency credit cannot overflow");

        write_text("init/shop.xtx","angouFlag 0\npointVarName money\n");
        const inventory::ShopCatalog plain(root);
        const auto plain_shelf=plain.page(0,0,plain.begin(read),read);
        require(plain_shelf.rows.size()==1 && plain_shelf.rows[0].serial==99 &&
            !plain_shelf.rows[0].detail_selectable && !plain.plan_seen(0,0,read) &&
            !inventory::ShopCatalog(root/"missing").configured(),"shop source-selected plaintext and empty fallback");
    });
    suite.test("missing twice then create same file",[&] {
        const auto path=scratch/"late.spt";
        Callback callback; nnn::CExecScript vm(callback);
        status_is(vm.LoadFile(path.string()),ErrorCode::NotFound,"first missing load");
        status_is(vm.LoadFile(path.string()),ErrorCode::NotFound,"second missing load");
        require(!vm.IsLoaded() && vm.Identity().empty(),"missing load committed identity");
        write_file(path,read_bytes(std::filesystem::path(NNN_FIXTURE_DIR)/"minimal_end.spt"));
        status_is(vm.LoadFile(path.string()),ErrorCode::Ok,"load after create");
        require(vm.Exec(100).return_code==end_code,"created same-name file did not run");
    });
    suite.test("explicit same-name load rereads changed contents",[&] {
        const auto path=scratch/"changing.spt";
        Callback callback; nnn::CExecScript vm(callback);
        write_file(path,read_bytes(std::filesystem::path(NNN_FIXTURE_DIR)/"minimal_end.spt"));
        status_is(vm.LoadFile(path.string()),ErrorCode::Ok,"first same-name load");
        require(vm.WordCount()==63,"initial program size");
        write_file(path,read_bytes(std::filesystem::path(NNN_FIXTURE_DIR)/"flow.spt"));
        status_is(vm.LoadFile(path.string()),ErrorCode::Ok,"changed same-name load");
        require(vm.WordCount()==153 && vm.GetPC()==0,"changed file was cached");
        run_flow(vm,callback);
    });
    suite.test("A success and B failure preserve A program and PC",[&] {
        Callback callback; nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(read_bytes(std::filesystem::path(NNN_FIXTURE_DIR)/"flow.spt"),"program-A"),ErrorCode::Ok,"A load");
        status_is(vm.Exec(100000).status,ErrorCode::Ok,"advance A");
        require(vm.GetPC()==70,"A did not reach known yield point");
        const auto before=vm_words(vm); const auto events=callback.events;
        const auto path=scratch/"missing-B.spt";
        for(int attempt=0;attempt!=2;++attempt) {
            status_is(vm.LoadFile(path.string()),ErrorCode::NotFound,"B missing");
            require(vm.Identity()=="program-A" && vm.GetPC()==70 && vm_words(vm)==before,"B failure mutated A");
        }
        status_is(vm.LoadBytes({1,2,3},"bad-B"),ErrorCode::InvalidFormat,"B malformed");
        require(vm.Identity()=="program-A" && vm.GetPC()==70 && vm_words(vm)==before,"malformed B mutated A");
        require(callback.events==events,"failed replacement emitted callback");
        const auto resumed=vm.Exec(100000);
        status_is(resumed.status,ErrorCode::Ok,"resume validated A explicitly");
        require(resumed.return_code==user_return && resumed.pc==102,"preserved A did not resume");
    });
    suite.test("injectable short-read and read-error results are distinct",[] {
        const auto valid=read_bytes(std::filesystem::path(NNN_FIXTURE_DIR)/"minimal_end.spt");
        Callback callback; nnn::CExecScript vm(callback);
        nnn::Reader short_reader=[&](const std::string&) { return nnn::ReadResult{{},valid,valid.size()+4}; };
        status_is(vm.LoadFile("short",{},short_reader),ErrorCode::ReadFailed,"controlled actual short read");
        nnn::Reader excess_reader=[&](const std::string&) { return nnn::ReadResult{{},valid,valid.size()-4}; };
        status_is(vm.LoadFile("excess",{},excess_reader),ErrorCode::ReadFailed,"declared length smaller than returned bytes");
        nnn::Reader failed=[](const std::string&) { return nnn::ReadResult{{ErrorCode::ReadFailed,nnn::no_word,"controlled"},{},0}; };
        status_is(vm.LoadFile("io-error",{},failed),ErrorCode::ReadFailed,"reader failure");
        nnn::Reader throwing=[](const std::string&)->nnn::ReadResult { throw std::runtime_error("reader threw"); };
        status_is(vm.LoadFile("io-throw",{},throwing),ErrorCode::ReadFailed,"reader exception");
        require(!vm.IsLoaded() && callback.events.empty(),"read failure installed program or emitted callbacks");
    });

    suite.test("empty, non-word, truncated and oversized byte streams",[] {
        Callback callback; nnn::CExecScript vm(callback);
        for(const auto& input : std::vector<std::vector<Byte>>{{},{0},{0,0,0},{0,0,0,0}})
            status_is(vm.LoadBytes(input),ErrorCode::InvalidFormat,"short/non-word bytes");
        auto header=read_bytes(std::filesystem::path(NNN_FIXTURE_DIR)/"flow.spt"); header.resize(31*4);
        status_is(vm.LoadBytes(header),ErrorCode::InvalidFormat,"truncated 32-word header");
        const std::vector<Byte> huge(nnn::max_script_bytes+4,0);
        status_is(vm.LoadBytes(huge),ErrorCode::InvalidFormat,"over input cap");
        require(!vm.IsLoaded() && callback.events.empty(),"invalid byte stream emitted callback");
    });
    malformed_case(suite,"wrong SPTHEADER0 magic",[](auto& w){w[12]^=1;},ErrorCode::UnsupportedFormat);
    malformed_case(suite,"invalid fixed header width",[](auto& w){w[0]=31;});
    malformed_case(suite,"zero record length",[](auto& w){w[32]=0;});
    malformed_case(suite,"record shorter than instruction header",[](auto& w){w[32]=2;});
    malformed_case(suite,"record length wraps beyond input",[](auto& w){w[32]=UINT32_MAX;});
    malformed_case(suite,"truncated record",[](auto& w){w.resize(36);});
    malformed_case(suite,"header table pointer outside input",[](auto& w){w[5]=UINT32_MAX;});
    malformed_case(suite,"table count exceeds its record",[](auto& w){w[4]=1000;});
    malformed_case(suite,"table identity mismatches header pointer",[](auto& w){w[5]=w[7];});
    malformed_case(suite,"string pointer outside data",[](auto& w){w[115]=UINT32_MAX;});
    malformed_case(suite,"nonempty string pointer equals buffer end",[](auto& w){w[115]=static_cast<Word>(w.size());});
    malformed_case(suite,"string pointer points into pointer table",[](auto& w){w[115]=115;});
    malformed_case(suite,"string has no terminator in its table",[](auto& w){for(std::size_t i=117;i<123;++i)w[i]=0x41414141;});
    malformed_case(suite,"GOTO points into instruction payload",[](auto& w){w[73]=33;});
    malformed_case(suite,"GOTO points outside input",[](auto& w){w[73]=UINT32_MAX;});
    malformed_case(suite,"GOTO points at buffer end",[](auto& w){w[73]=static_cast<Word>(w.size());});
    malformed_case(suite,"message index exceeds message table",[](auto& w){w[67]=2;});
    malformed_case(suite,"string index exceeds string table",[](auto& w){w[86]=1;});
    malformed_case(suite,"INT32 reference exceeds table",[](auto& w){w[93]=0x91000001;});
    malformed_case(suite,"expression count exceeds record",[](auto& w){w[36]=256;});
    malformed_case(suite,"expression stack underflow",[](auto& w){w[37]=assignment;});
    malformed_case(suite,"assignment destination is not variable",[](auto& w){w[37]=0x90000064;});
    malformed_case(suite,"user parameter count exceeds record",[](auto& w){w[98]=256;});
    malformed_case(suite,"user parameter count exceeds limit",[](auto& w){w[98]=257;});
    malformed_case(suite,"unsupported expression token",[](auto& w){w[38]=0x92000000;},ErrorCode::UnsupportedExpression);
    malformed_case(suite,"RPN variable-number literal cannot be assigned",[](auto& w){w[37]=0x80008000;});
    malformed_case(suite,"truncated user expression is rejected",[](auto& w){w[99]=expression;});

    suite.test("STORYNAME follows its fixed scalar callback format",[] {
        auto words=fixture_words("flow");words[85]=0;
        Callback callback;nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(default_encrypt(words)),ErrorCode::Ok,"STORYNAME fixed shape load");
        run_flow(vm,callback);
    });
    suite.test("unknown bounded instruction falls back only when executed",[] {
        Callback callback;nnn::CExecScript vm(callback);
        auto words=minimal_with_code({3,0x6666eeee,0x12345678,3,control,end_code});
        status_is(vm.LoadBytes(default_encrypt(words)),ErrorCode::Ok,"unknown bounded load");
        require(callback.skipped.empty(),"loader called runtime fallback");
        status_is(vm.Exec(1000).status,ErrorCode::Ok,"unknown bounded Exec");
        require(callback.skipped==std::vector<std::tuple<Word,Word,Word>>{{32,0x6666eeee,0x12345678}},"fallback identity/PC missing");
        callback.skipped.clear();
        words=minimal_with_code({4,control,goto_code,39,3,0x6666eeee,0x12345678,3,control,end_code});
        status_is(vm.LoadBytes(default_encrypt(words)),ErrorCode::Ok,"unreachable unknown load");
        status_is(vm.Exec(1000).status,ErrorCode::Ok,"unreachable unknown Exec");
        require(callback.skipped.empty(),"unreachable instruction was dispatched");
    });
    suite.test("implemented RETURN reports stack underflow during execution",[] {
        Callback callback;nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(default_encrypt(minimal_with_code({3,control,0x55560007,3,control,end_code}))),ErrorCode::Ok,"RETURN load");
        status_is(vm.Exec(100).status,ErrorCode::CallStackUnderflow,"RETURN without SUB");
        require(callback.events.empty(),"underflow invoked host");
    });
    suite.test("supported division and special-variable assignment retain source semantics",[] {
        Callback callback;nnn::CExecScript vm(callback);
        const std::vector<Word> code={10,calcu,0,expression,5,0x80000063,0x90000007,0x90000002,0xa000002f,assignment,3,control,end_code};
        status_is(vm.LoadBytes(default_encrypt(minimal_with_code(code))),ErrorCode::Ok,"division and low-variable load");
        status_is(vm.Exec(1000).status,ErrorCode::Ok,"division and low-variable Exec");
        require(callback.variables[99]==3,"division or special-variable assignment mismatch");
    });
    suite.test("direct INT32 and well-formed RPN user arguments are supported",[] {
        Callback callback;nnn::CExecScript vm(callback);
        auto words=fixture_words("flow");words[99]=0x91000000;
        status_is(vm.LoadBytes(default_encrypt(words)),ErrorCode::Ok,"direct INT32 user load");
        for(int i=0;i<4;++i) {
            const auto result=vm.Exec(10000);status_is(result.status,ErrorCode::Ok,"direct INT32 user Exec");
            if(result.return_code==user_return)break;
        }
        require(!callback.events.empty()&&callback.events.back().kind=="user_command"&&
                callback.events.back().parameters==std::vector<Value>{50000,3},"direct INT32 user value mismatch");
        callback.events.clear();
        const std::vector<Word> code={10,command,0x1234,1,expression,3,0x90000002,0x90000003,0xa000002b,0,3,control,end_code};
        status_is(vm.LoadBytes(default_encrypt(minimal_with_code(code))),ErrorCode::Ok,"RPN user argument load");
        status_is(vm.Exec(1000).status,ErrorCode::Ok,"RPN user argument Exec");
        require(callback.events.size()==1&&callback.events[0].parameters==std::vector<Value>{5},"RPN user argument mismatch");
    });

    suite.test("translated metadata lengths use bounded typed header anchors",[] {
        auto words=translated_text_tables();
        Callback callback;nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(default_encrypt(words)),ErrorCode::Ok,"translated text load");
        Word raw=0;
        require(vm.GetData(41,raw)&&raw==12,"compatibility rewrote original MESSAGE length");
        require(vm.GetData(47,raw)&&raw==8,"compatibility rewrote original STRING length");
        require(vm.GetData(18,raw)&&raw==200,"compatibility rewrote raw header");
        const auto printed=vm.Exec(1000);
        status_is(printed.status,ErrorCode::Ok,"translated PRINT");
        require(printed.return_code==print_code&&printed.pc==38,"translated print PC");
        require(callback.events==std::vector<Event>{script_event(print_code,0,"M")},"translated text changed");
        require(vm.Exec(1000).return_code==end_code,"translated END not reached");
    });
    suite.test("translated tables reject invalid pointers and unterminated data",[] {
        const auto reject=[](std::vector<Word> words,const std::string& reason){
            Callback callback;nnn::CExecScript vm(callback);
            status_is(vm.LoadBytes(default_encrypt(words)),ErrorCode::InvalidFormat,reason);
            require(!vm.IsLoaded()&&callback.events.empty(),"bad translated load committed state");
        };
        auto words=translated_text_tables();words[45]=52;reject(words,"message pointer crosses typed table boundary");
        words=translated_text_tables();words[46]=0x41414141;reject(words,"message terminator absent");
        words=translated_text_tables();words[51]=53;reject(words,"string pointer reaches constant table");
        words=translated_text_tables();words[52]=0x41414141;reject(words,"string terminator absent");
        words=translated_text_tables();words[7]=50;reject(words,"string anchor is not typed header");
        words=translated_text_tables();words[56]=0x44440005;reject(words,"inferred constant table kind differs");
        words=translated_text_tables();words[32]=UINT32_MAX;reject(words,"executable record remains bounded");
        reject(translated_text_tables(true),"nonempty constant pool is never inferred");
    });

    suite.test("GOTO self-loop exhausts one internal execution budget",[] {
        const auto words=minimal_with_code({4,control,goto_code,32,3,control,end_code});
        Callback callback; nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(default_encrypt(words)),ErrorCode::Ok,"self-loop load");
        const auto result=vm.Exec(23);
        status_is(result.status,ErrorCode::BudgetExhausted,"self-loop budget");
        require(result.work_used>0 && result.work_used<=23 && result.pc==32,"self-loop budget accounting");
        require(callback.events.empty(),"self-loop emitted callback");
    });
    suite.test("zero budget performs no callback or VM advancement",[] {
        Callback callback; nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(read_bytes(std::filesystem::path(NNN_FIXTURE_DIR)/"flow.spt")),ErrorCode::Ok,"zero-budget load");
        const auto result=vm.Exec(0);
        status_is(result.status,ErrorCode::BudgetExhausted,"zero budget");
        require(result.pc==0 && result.work_used==0 && callback.events.empty(),"zero budget side effects");
    });
    suite.test("RPN token work consumes the single Exec budget",[] {
        std::vector<Word> code {260,calcu,0,expression,255,0x90000000};
        for(int pair=0;pair!=127;++pair) {code.push_back(0x90000000);code.push_back(equal);}
        code.insert(code.end(),{3,control,end_code});
        Callback callback; nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(default_encrypt(minimal_with_code(code))),ErrorCode::Ok,"long expression load");
        const auto result=vm.Exec(32);
        status_is(result.status,ErrorCode::BudgetExhausted,"inner expression budget");
        require(result.work_used<=32 && callback.events.empty(),"expression bypassed budget");
    });
    suite.test("parameter loop work consumes the single Exec budget",[] {
        std::vector<Word> code {261,command,0x1234,256};
        for(Word index=0;index!=256;++index) code.push_back(0x80000000|index);
        code.push_back(0); code.insert(code.end(),{3,control,end_code});
        Callback callback; nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(default_encrypt(minimal_with_code(code))),ErrorCode::Ok,"long parameter load");
        const auto result=vm.Exec(32);
        status_is(result.status,ErrorCode::BudgetExhausted,"inner parameter budget");
        require(callback.events.size()<256 && std::none_of(callback.events.begin(),callback.events.end(),[](const auto& e){return e.kind=="user_command";}),"parameter loop bypassed budget");
    });
    suite.test("direct scalar variable and signed16 parameter boundaries",[] {
        const auto words=minimal_with_code({9,command,0x1234,4,
            0x80000007,0x90008000,0x9000ffff,0x90007fff,0,
            5,command,0x4567,0,0,3,control,end_code});
        Callback callback; callback.variables[7]=42; nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(default_encrypt(words)),ErrorCode::Ok,"scalar parameter boundaries load");
        const auto first=vm.Exec(1000);
        status_is(first.status,ErrorCode::Ok,"scalar parameter boundaries Exec");
        require(first.return_code==user_return && first.pc==41,"scalar user command return/PC");
        Event expected; expected.kind="user_command"; expected.code=0x1234;
        expected.parameters={42,-32768,-1,32767};
        require(callback.events==std::vector<Event>({variable_event("get_calcu_var",7,42),expected}),
                "scalar VAR must use GetCalcuVarData; signed16 boundaries must sign-extend");
        const auto empty=vm.Exec(1000);
        status_is(empty.status,ErrorCode::Ok,"zero-parameter command");
        require(empty.return_code==user_return && empty.pc==46,"zero-parameter return/PC");
        require(callback.events.back().kind=="user_command" && callback.events.back().code==0x4567
                && callback.events.back().parameters.empty(),"zero parameter callback");
        const auto finished=vm.Exec(1000);
        status_is(finished.status,ErrorCode::Ok,"boundary program END");
        require(finished.return_code==end_code && finished.pc==46,"boundary program END PC");
    });
    suite.test("RPN length and stack overflow are bounded",[] {
        std::vector<Word> code {262,calcu,0,expression,257};
        code.insert(code.end(),257,0x90000000); code.insert(code.end(),{3,control,end_code});
        Callback callback; nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(default_encrypt(minimal_with_code(code))),ErrorCode::InvalidFormat,"overlong expression");
        require(callback.events.empty(),"invalid expression emitted callback");
    });
    suite.test("callback exceptions are typed and execution guard clears",[] {
        Callback callback; callback.throw_on_set=true; nnn::CExecScript vm(callback);
        status_is(vm.LoadBytes(read_bytes(std::filesystem::path(NNN_FIXTURE_DIR)/"flow.spt")),ErrorCode::Ok,"throwing callback load");
        status_is(vm.Exec(100000).status,ErrorCode::CallbackFailed,"callback exception");
        callback.throw_on_set=false;
        status_is(vm.LoadBytes(read_bytes(std::filesystem::path(NNN_FIXTURE_DIR)/"minimal_end.spt")),ErrorCode::Ok,"load after callback failure");
        status_is(vm.Exec(100).status,ErrorCode::Ok,"Exec after callback failure");
    });
    suite.test("callback reentrant Exec and load are rejected",[] {
        Callback callback; nnn::CExecScript vm(callback);
        const auto valid=read_bytes(std::filesystem::path(NNN_FIXTURE_DIR)/"minimal_end.spt");
        callback.on_set=[&] {
            status_is(vm.Exec(100).status,ErrorCode::Busy,"reentrant Exec");
            status_is(vm.LoadBytes(valid,"reentrant"),ErrorCode::Busy,"reentrant LoadBytes");
        };
        status_is(vm.LoadBytes(read_bytes(std::filesystem::path(NNN_FIXTURE_DIR)/"flow.spt"),"outer"),ErrorCode::Ok,"outer load");
        run_flow(vm,callback);
        require(vm.Identity()=="outer","reentrant load changed identity");
    });

    std::cout << "RESULT " << suite.passed << " passed, " << suite.failed << " failed\n";
    return suite.failed==0 ? 0 : 1;
}
