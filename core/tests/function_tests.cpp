#include "nnn/script.hpp"
#include "functions/fixtures/generated_cases.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using nnn::Byte;using nnn::Word;using nnn::Value;using nnn::ErrorCode;
constexpr Word control=0x66660002U,function_id=0x66660004U,system_function=0x66660007U,system_command=0x66660006U;
constexpr Word end_code=0x55560005U,print_code=0x22220001U;
void require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
void status_is(const nnn::Status& actual,ErrorCode expected,const std::string& context){require(actual.code==expected,context+": expected "+nnn::error_name(expected)+", got "+nnn::error_name(actual.code)+" ("+actual.detail+")");}
std::vector<Byte> bytes(const std::string& text){std::vector<Byte> result;for(char c:text)result.push_back(static_cast<Byte>(static_cast<unsigned char>(c)));return result;}
std::vector<Byte> read_file(const std::filesystem::path& path){std::ifstream input(path,std::ios::binary);require(static_cast<bool>(input),"missing fixture "+path.string());return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};}
nnn::SkipFlags flags(Word mask){return {(mask&1U)!=0,(mask&2U)!=0,(mask&4U)!=0,(mask&8U)!=0,(mask&16U)!=0};}
Word mask(nnn::SkipFlags value){return (value.skip_f4_mode?1U:0U)|(value.skip_f5_mode?2U:0U)|(value.skip_next_command?4U:0U)|(value.skip_effect_command?8U:0U)|(value.skip_to_film_end?16U:0U);}
std::string hex(const std::string& text){constexpr char digits[]="0123456789abcdef";std::string out;for(char c:text){const auto b=static_cast<unsigned char>(c);out+=digits[b>>4U];out+=digits[b&15U];}return out;}
std::vector<nnn::CallFrame> frames(const nnn::CExecScript& vm){std::vector<nnn::CallFrame> result;for(std::size_t i=0;i<vm.GetCallDepth();++i){nnn::CallFrame frame{};require(vm.GetCallFrame(i,frame),"missing active frame");result.push_back(frame);}return result;}
struct State{Word pc;std::vector<nnn::CallFrame> stack;Word flags;};
struct Callback final:nnn::CScriptCallBack{
    nnn::CExecScript* vm=nullptr;
    std::vector<std::string> events;
    std::vector<State> states;
    std::vector<std::tuple<Word,Word,Word>> skipped;
    std::size_t variable_reads=0;
    void UnhandledInstruction(Word pc,Word identify,Word code)override{skipped.emplace_back(pc,identify,code);}
    std::function<void(Word)> on_function;
    void observe(){require(vm!=nullptr,"missing callback VM");states.push_back({vm->GetPC(),frames(*vm),mask(GetSkipFlags())});}
    void ReceiveScriptCommand(Word code,Value p1,const std::string& text,Value p3)override{observe();events.push_back("{\"kind\":\"script\",\"code\":"+std::to_string(code)+",\"para1\":"+std::to_string(p1)+",\"text_hex\":\""+hex(text)+"\",\"para3\":"+std::to_string(p3)+'}');}
    void PreReceiveScriptData(Word,Value,const std::string&)override{throw std::runtime_error("unexpected predata");}
    void ReceiveUserCommand0(Word,const std::vector<Value>&)override{throw std::runtime_error("unexpected user command");}
    void ReceiveUserFunction0(Word code,const std::vector<Value>& parameters)override{
        observe();std::string event="{\"kind\":\"user_function\",\"code\":"+std::to_string(code)+",\"parameters\":[";
        for(std::size_t i=0;i<parameters.size();++i){if(i)event+=',';event+=std::to_string(parameters[i]);}
        events.push_back(event+"]}");
        if(on_function)on_function(code);
    }
    Value GetVarData(Word)override{throw std::runtime_error("unexpected GetVarData");}
    Value GetCalcuVarData(Word index)override{require(index==100,"unexpected calculator variable");++variable_reads;return 7;}
    void SetVarData(Word,Value)override{throw std::runtime_error("unexpected SetVarData");}
    // No test-owned SkipFlags or clearing override: exercise production storage.
};
struct Code{
    std::vector<Word> words,sub_sites;
    Word pc()const{return 32U+static_cast<Word>(words.size());}
    Word add(std::initializer_list<Word> values){const Word start=pc();words.insert(words.end(),values);return start;}
    Word function(Word id,const std::vector<Word>& values={}){const Word start=pc();words.insert(words.end(),{4U+static_cast<Word>(values.size()),function_id,id,static_cast<Word>(values.size())});words.insert(words.end(),values.begin(),values.end());return start;}
    Word story(){return add({5,system_function,0x33330051U,1,0});}
    Word print(){return add({6,system_command,print_code,0,0,0});}
    Word end(){return add({3,control,end_code});}
    Word sub(Word label){const Word start=add({5,control,0x55560009U,label,static_cast<Word>(sub_sites.size())});sub_sites.push_back(start);return start;}
};
struct Model{Code code;bool labels=true;std::vector<Word> targets;};
std::vector<Word> program_words(const Model& model){
    std::vector<Word> words(32,0);words[0]=32;words[1]=0x66660001U;words[2]=0x55550001U;words[12]=0x48545053U;words[13]=0x45444145U;words[14]=0x20203052U;words[15]=0x00202020U;
    words.insert(words.end(),model.code.words.begin(),model.code.words.end());
    const auto table=[&](Word kind,Word field,const std::vector<Word>& payload,bool record=false){words[field]=static_cast<Word>(words.size())+(record?0U:4U);words.insert(words.end(),{4U+static_cast<Word>(payload.size()),0x66660001U,0x55550002U,kind});words.insert(words.end(),payload.begin(),payload.end());};
    if(model.labels){words[8]=static_cast<Word>(model.targets.size());words[10]=words[8];table(0x44440003U,9,model.targets);table(0x44440003U,16,{},true);table(0x44440003U,17,{},true);}
    words[4]=1;Word begin=static_cast<Word>(words.size())+4U;table(0x44440002U,5,{begin+1U,0x4dU});words[6]=1;begin=static_cast<Word>(words.size())+4U;table(0x44440001U,7,{begin+1U,0x53U});
    table(0x44440004U,18,{});table(0x44440005U,20,model.code.sub_sites);table(0x44440006U,21,{});table(0x44440007U,22,{});table(0x44440008U,23,{});return words;
}
std::vector<Byte> script(const Model& model){std::vector<Byte> result;for(Word word:program_words(model)){word^=UINT32_MAX;for(unsigned shift=0;shift<32;shift+=8)result.push_back(static_cast<Byte>(word>>shift));}return result;}
nnn::FunctionRegistry registry(const std::string& text){auto parsed=nnn::parse_function_registry(bytes(text));status_is(parsed.status,ErrorCode::Ok,"registry parse");require(parsed.registry.IsValid(),"successful registry invalid");return parsed.registry;}
const std::string third="first,d,second,d,stopf5,d,";
Model simple(Word id=2){Model m;m.code.function(id);m.code.print();m.code.end();return m;}
void load(nnn::CExecScript& vm,const Model& model,const nnn::FunctionRegistry* config=nullptr,const std::string& identity="functions"){status_is(vm.LoadBytes(script(model),identity,{},config),ErrorCode::Ok,"function load");}
nnn::ExecResult exec(nnn::CExecScript& vm,Word code,std::uint64_t budget=100000){auto result=vm.Exec(budget);status_is(result.status,ErrorCode::Ok,"function Exec");require(result.return_code==code,"function return code");return result;}
void reject(const Model& model,const nnn::FunctionRegistry* config,ErrorCode error=ErrorCode::UnsupportedInstruction){Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;cb.SetSkipFlags(flags(31));status_is(vm.LoadBytes(script(model),"bad",{},config),error,"rejected function");require(!vm.IsLoaded()&&cb.events.empty()&&mask(cb.GetSkipFlags())==31,"bad program installed/dispatched/cleared flags");}
void check_simple(Word id,const nnn::FunctionRegistry* config,bool fallback,bool clears){
    Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;cb.SetSkipFlags(flags(31));load(vm,simple(id),config);
    require(exec(vm,print_code).pc==42 && mask(cb.GetSkipFlags())==(clears?0U:31U),"function PC or flags");
    if(fallback) require(cb.events.size()==1 && cb.skipped==std::vector<std::tuple<Word,Word,Word>>{{32,function_id,id}},"missing function fallback");
    else require(cb.events.size()==2 && cb.events[0]=="{\"kind\":\"user_function\",\"code\":"+std::to_string(id)+",\"parameters\":[]}" && cb.states[0].pc==32 && cb.skipped.empty(),"function callback");
}
struct Suite{int passed=0,failed=0;void test(const std::string& name,const std::function<void()>& body){try{body();++passed;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& error){++failed;std::cerr<<"FAIL "<<name<<": "<<error.what()<<'\n';}}};
} // namespace

int main(){
    Suite suite;
    suite.test("public bounds and old error ordinals",[]{require(nnn::max_function_config_bytes==65536&&nnn::max_function_registry_entries==256&&nnn::max_function_token_bytes==127,"registry bounds");require(static_cast<int>(ErrorCode::HostRejected)==13&&static_cast<int>(ErrorCode::Busy)==10,"old error ordinals changed");});
    suite.test("default invalid registry inspectors leave output unchanged",[]{nnn::FunctionRegistry config;std::string out="kept";require(!config.IsValid()&&config.Size()==0&&config.StopF5Id()==nnn::no_word&&!config.GetName(0,out)&&out=="kept","invalid registry inspector");require(!config.GetName(static_cast<std::size_t>(-1),out)&&out=="kept","huge registry index");});
    suite.test("real base host state and VM forwarding share ownership",[]{Callback cb;nnn::CExecScript a(cb),b(cb);cb.vm=&a;require(mask(cb.GetSkipFlags())==0&&mask(a.GetSkipFlags())==0,"initial flags");a.SetSkipFlags(flags(31));require(mask(cb.GetSkipFlags())==31&&mask(b.GetSkipFlags())==31,"VM did not forward host state");cb.CScriptCallBack::SetSkipFlags(flags(7));require(mask(a.GetSkipFlags())==7&&mask(b.GetSkipFlags())==7,"base setter storage mismatch");auto copy=a.GetSkipFlags();copy.skip_f4_mode=false;require(mask(copy)==6&&mask(cb.GetSkipFlags())==7,"Get returned aliased state");cb.CScriptCallBack::ClearSkipFlags();require(mask(a.GetSkipFlags())==0&&mask(b.GetSkipFlags())==0,"production clear did not update host");});
    suite.test("different hosts own independent skip storage",[]{Callback a,b;a.SetSkipFlags(flags(31));b.SetSkipFlags(flags(3));a.ClearSkipFlags();require(mask(a.GetSkipFlags())==0&&mask(b.GetSkipFlags())==3,"flags were global/static");});
    struct Parse{const char* name;std::string input;std::vector<std::string> names;Word stop;};
    const std::vector<Parse> parsed_cases{
        {"empty","",{},nnn::no_word},{"whitespace"," \t\r\n",{},nnn::no_word},{"trailing comma only",",",{},nnn::no_word},
        {"ID0","stopf5,d,",{"stopf5"},0},{"ID2",third,{"first","second","stopf5"},2},
        {"ASCII casefold","other,d,StOpF5,d,",{"other","StOpF5"},1},
        {"whitespace token stream","other d\r\nstopf5\td",{"other","stopf5"},1},
        {"leading and final comma",",stopf5,d,",{"stopf5"},0},
        {"empty description","stopf5,,other,d",{"stopf5","other"},0},
        {"empty name retains slot",",,d,stopf5,x",{"","stopf5"},1},
        {"duplicate first wins","stopf5,a,STOPF5,b,",{"stopf5","STOPF5"},0},
        {"spaces before commas produce empty tokens","stopf5 , desc ,",{"stopf5","desc"},0},
        {"quotes are literal","\"stopf5\",d,",{"\"stopf5\""},nnn::no_word},
        {"comment punctuation is literal","//,note,stopf5,d,",{"//","stopf5"},1},
        {"raw opaque high-byte name",std::string("\x82\xa0",2)+",d,stopf5,x",{std::string("\x82\xa0",2),"stopf5"},1},
    };
    for(const auto& item:parsed_cases)suite.test(std::string("source token grammar: ")+item.name,[&]{auto config=registry(item.input);require(config.Size()==item.names.size()&&config.StopF5Id()==item.stop,"registry names/first binding mismatch");for(std::size_t i=0;i<item.names.size();++i){std::string name;require(config.GetName(i,name)&&name==item.names[i],"registry token bytes mismatch");name="mutated";std::string again;require(config.GetName(i,again)&&again==item.names[i],"name inspector borrowed storage");}std::string out="sentinel";require(!config.GetName(item.names.size(),out)&&out=="sentinel","out-of-range name changed output");});
    suite.test("registry owns bytes and independent copies",[]{auto input=bytes(third);auto parsed=nnn::parse_function_registry(input);status_is(parsed.status,ErrorCode::Ok,"owned parse");auto copied=parsed.registry;input.assign(input.size(),0);parsed.registry=nnn::FunctionRegistry{};std::string name;require(copied.IsValid()&&copied.StopF5Id()==2&&copied.GetName(2,name)&&name=="stopf5","registry kept borrowed configuration");});
    suite.test("maximum127-byte opaque token accepted",[]{auto config=registry(std::string(127,'n')+",d,");std::string out;require(config.GetName(0,out)&&out.size()==127,"token maximum");});
    suite.test("maximum256 registry pairs and last stop ID255",[]{std::string text;for(int i=0;i<255;++i)text+="n,d,";text+="stopf5,d,";auto config=registry(text);require(config.Size()==256&&config.StopF5Id()==255,"entry bound/last slot");});
    suite.test("maximum64KiB whitespace input accepted",[]{auto config=registry(std::string(65536,' '));require(config.Size()==0&&config.StopF5Id()==nnn::no_word,"byte maximum");});
    struct BadConfig{const char* name;std::vector<Byte> input;ErrorCode error;};
    std::vector<BadConfig> bad_config{{"odd single",bytes("stopf5"),ErrorCode::InvalidFormat},{"odd three",bytes("a,d,stopf5"),ErrorCode::InvalidFormat},{"NUL",{Byte{'a'},0,Byte{','},Byte{'d'}},ErrorCode::InvalidFormat},{"oversized token",bytes(std::string(128,'x')+",d,"),ErrorCode::UnsupportedFormat},{"oversized bytes",std::vector<Byte>(65537,' '),ErrorCode::UnsupportedFormat}};
    std::string too_many;for(int i=0;i<257;++i)too_many+="n,d,";bad_config.push_back({"too many tokens",bytes(too_many),ErrorCode::UnsupportedFormat});
    for(const auto& item:bad_config)suite.test(std::string("invalid bounded config: ")+item.name,[&]{auto result=nnn::parse_function_registry(item.input);status_is(result.status,item.error,"bad config");require(!result.registry.IsValid()&&result.registry.Size()==0&&result.registry.StopF5Id()==nnn::no_word,"failed parse leaked partial registry");});
    for(Word initial=0;initial<32;++initial)suite.test("all five production flags clear: "+std::to_string(initial),[initial]{
        auto config=registry(third);Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;cb.CScriptCallBack::SetSkipFlags(flags(initial));load(vm,simple(),&config);
        const auto result=exec(vm,print_code);require(result.pc==42&&cb.states.size()==2&&cb.states[0].pc==32&&cb.states[0].flags==initial&&cb.states[1].pc==36&&cb.states[1].flags==0,"entry/action/step ordering");require(mask(cb.CScriptCallBack::GetSkipFlags())==0&&mask(vm.GetSkipFlags())==0,"production storage was not cleared");require(cb.events[0]=="{\"kind\":\"user_function\",\"code\":2,\"parameters\":[]}","function callback shape");exec(vm,end_code);
    });
    for(const bool labels:{false,true})suite.test(std::string("opt-in works with ")+(labels?"labels":"legacy7"),[labels]{auto config=registry("stopf5,d,");Model m=simple(0);m.labels=labels;Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;vm.SetSkipFlags(flags(31));load(vm,m,&config);exec(vm,print_code);require(vm.HasFunctionRegistry()&&vm.FunctionRegistrySize()==1&&vm.BoundStopF5Id()==0&&mask(cb.GetSkipFlags())==0,"opt-in profile or binding");});
    suite.test("reordered registry changes callable ID",[]{auto a=registry(third);auto b=registry("stopf5,d,other,d,");check_simple(2,&b,true,false);check_simple(0,&a,false,false);check_simple(0,&b,false,true);});
    suite.test("same numeric ID with different name is not stopf5",[]{auto config=registry("first,d,second,d,other,d,");check_simple(2,&config,false,false);});
    suite.test("duplicate stop name only enables first ID",[]{auto config=registry("stopf5,d,STOPF5,d,");check_simple(1,&config,false,false);});
    suite.test("no registry retains old FUNCTION rejection",[]{check_simple(2,nullptr,false,false);});
    suite.test("registry without stopf5 rejects all functions",[]{auto config=registry("other,d,");check_simple(0,&config,false,false);});
    suite.test("empty valid registry has explicit installed binding",[]{auto config=registry("");Model m;m.code.end();Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m,&config);require(vm.HasFunctionRegistry()&&vm.FunctionRegistrySize()==0&&vm.BoundStopF5Id()==nnn::no_word,"empty registry disappeared");exec(vm,end_code);check_simple(0,&config,true,false);});
    suite.test("default invalid registry rejected before Reader",[]{nnn::FunctionRegistry config;Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;int reads=0;status_is(vm.LoadFile("bad",{},[&](const std::string&){++reads;return nnn::ReadResult{};},&config),ErrorCode::InvalidFormat,"invalid registry load");require(reads==0&&!vm.IsLoaded(),"invalid registry invoked reader");});
    suite.test("successful load preserves host flags and resets frames",[]{auto config=registry(third);Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;vm.SetSkipFlags(flags(31));load(vm,simple(),&config);require(mask(cb.GetSkipFlags())==31&&vm.GetPC()==0&&vm.GetCallDepth()==0,"load changed host flags");});
    suite.test("unregistered successful load does not reuse binding",[]{auto config=registry(third);Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,simple(),&config);vm.SetSkipFlags(flags(31));Model next;next.code.end();load(vm,next);require(!vm.HasFunctionRegistry()&&vm.FunctionRegistrySize()==0&&vm.BoundStopF5Id()==nnn::no_word&&mask(cb.GetSkipFlags())==31,"old binding/flags reused/reset");});
    suite.test("program owns binding after registry reassignment/destruction",[]{Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;{auto config=registry(third);load(vm,simple(),&config);config=registry("stopf5,d,");}require(vm.BoundStopF5Id()==2&&vm.FunctionRegistrySize()==3,"binding borrowed registry");exec(vm,print_code);});
    suite.test("LoadFile captures binding before Reader side effects",[]{auto config=registry(third);const auto input=script(simple());Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;int reads=0;auto reader=[&](const std::string&){++reads;config=registry("stopf5,d,");cb.SetSkipFlags(flags(7));return nnn::ReadResult{{},input,input.size()};};status_is(vm.LoadFile("reader",{},reader,&config),ErrorCode::Ok,"entry registry snapshot");require(reads==1&&config.StopF5Id()==0&&vm.BoundStopF5Id()==2&&vm.FunctionRegistrySize()==3&&mask(vm.GetSkipFlags())==7,"registry read too late or host side effect reset");exec(vm,print_code);require(cb.states[0].flags==7,"Reader flags not observed");});
    for(int kind=0;kind<6;++kind)suite.test("failed load preserves program frames binding flags "+std::to_string(kind),[kind]{
        auto old=registry(third);auto next=registry("stopf5,d,");Model m;m.targets.push_back(m.code.story());m.code.sub(1);m.code.end();m.targets.push_back(m.code.story());m.code.print();m.code.function(2);m.code.print();m.code.add({3,control,0x55560007U});
        Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;vm.SetSkipFlags(flags(31));load(vm,m,&old,"kept");exec(vm,print_code);const Word pc=vm.GetPC();const auto count=vm.WordCount(),events=cb.events.size();require(vm.GetCallDepth()==1,"test needs pending frame");
        nnn::Status result;if(kind==0)result=vm.LoadBytes({0},"bad",{},&next);else if(kind==1){Model bad=simple(99);bad.code.words[3]=1;result=vm.LoadBytes(script(bad),"bad",{},&next);}
        else if(kind==2)result=vm.LoadFile("bad",{},[](const std::string&){return nnn::ReadResult{{ErrorCode::ReadFailed,nnn::no_word,"reader"},{},0};},&next);
        else if(kind==3)result=vm.LoadFile("bad",{},[](const std::string&){return nnn::ReadResult{{},{0,0,0,0},8};},&next);
        else if(kind==4){nnn::FunctionRegistry invalid;result=vm.LoadBytes(script(simple()),"bad",{},&invalid);}
        else result=vm.LoadFile("bad",{},[](const std::string&)->nnn::ReadResult{throw std::runtime_error("reader");},&next);
        require(!static_cast<bool>(result)&&vm.GetPC()==pc&&vm.Identity()=="kept"&&vm.WordCount()==count&&cb.events.size()==events,"failed load changed program/PC/identity/events");
        const auto saved=frames(vm);require(saved.size()==1&&saved[0].return_pc==42&&saved[0].id==0&&vm.HasFunctionRegistry()&&vm.FunctionRegistrySize()==3&&vm.BoundStopF5Id()==2&&mask(cb.GetSkipFlags())==31,"failed load changed frames/binding/flags");
    });
    suite.test("Reader side effects are not rolled back on failure",[]{auto config=registry(third);Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,simple(),&config);cb.SetSkipFlags(flags(31));status_is(vm.LoadFile("bad",{},[&](const std::string&){cb.SetSkipFlags(flags(5));return nnn::ReadResult{{ErrorCode::ReadFailed,nnn::no_word,"reader"},{},0};},&config),ErrorCode::ReadFailed,"reader failure");require(mask(cb.GetSkipFlags())==5&&vm.BoundStopF5Id()==2,"Reader side effect rolled back/binding changed");});
    suite.test("successive stopf5 is idempotent within one Exec",[]{auto config=registry(third);Model m;m.code.function(2);m.code.function(2);m.code.print();m.code.end();Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;cb.SetSkipFlags(flags(31));load(vm,m,&config);exec(vm,print_code);require(cb.states.size()==3&&cb.states[0].flags==31&&cb.states[1].flags==0&&cb.states[2].flags==0&&mask(cb.GetSkipFlags())==0,"repeat function mutation/order");});
    for(const bool side_effect:{false,true})suite.test(std::string("throwing function leaves current PC and no automatic clear ")+(side_effect?"host-mutated":"original"),[side_effect]{auto config=registry(third);Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;cb.SetSkipFlags(flags(31));load(vm,simple(),&config);cb.on_function=[&](Word){if(side_effect)cb.SetSkipFlags(flags(9));throw std::runtime_error("host");};auto result=vm.Exec();status_is(result.status,ErrorCode::CallbackFailed,"throwing function");require(result.pc==32&&cb.events.size()==1&&mask(cb.GetSkipFlags())==(side_effect?9U:31U),"exception advanced or cleared/rolled back flags");});
    suite.test("nonstandard function exception is contained",[]{auto config=registry(third);Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;cb.SetSkipFlags(flags(31));load(vm,simple(),&config);cb.on_function=[](Word){throw 7;};auto result=vm.Exec();status_is(result.status,ErrorCode::CallbackFailed,"nonstandard exception");require(result.pc==32&&mask(cb.GetSkipFlags())==31,"nonstandard exception cleared");});
    suite.test("function reentry Busy preserves pre-action state",[]{auto config=registry(third);Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;cb.SetSkipFlags(flags(31));load(vm,simple(),&config,"kept");int calls=0;cb.on_function=[&](Word){++calls;require(vm.GetPC()==32&&mask(vm.GetSkipFlags())==31,"callback not pre-action");status_is(vm.Exec().status,ErrorCode::Busy,"nested Exec");status_is(vm.LoadBytes(script(simple()),"new",{},&config),ErrorCode::Busy,"nested LoadBytes");int reads=0;status_is(vm.LoadFile("new",{},[&](const std::string&){++reads;return nnn::ReadResult{};},&config),ErrorCode::Busy,"nested LoadFile");require(reads==0&&vm.Identity()=="kept"&&vm.BoundStopF5Id()==2&&mask(cb.GetSkipFlags())==31,"Busy changed state");};exec(vm,print_code);require(calls==1&&mask(cb.GetSkipFlags())==0,"outer stopf5 did not finish");});
    for(std::uint64_t budget=0;budget<=5;++budget)suite.test("function budget paid before callback "+std::to_string(budget),[budget]{auto config=registry(third);Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;cb.SetSkipFlags(flags(31));load(vm,simple(),&config);auto result=vm.Exec(budget);status_is(result.status,ErrorCode::BudgetExhausted,"function budget");require(result.work_used==(budget==0?0U:budget<5?1U:5U),"budget charge");if(budget<5)require(cb.events.empty()&&mask(cb.GetSkipFlags())==31&&result.pc==(budget==0?0U:32U),"unfunded callback/clear");else require(cb.events.size()==1&&result.pc==36&&mask(cb.GetSkipFlags())==0,"funded callback lost when next instruction ran out");});
    for(const Word argc:{1U,UINT32_MAX})suite.test("function argc exceeding record is invalid "+std::to_string(argc),[argc]{auto config=registry(third);Model m;m.code.add({4,function_id,2,argc});m.code.end();reject(m,&config,ErrorCode::InvalidFormat);});
    for(const Word length:{3U,5U,6U})suite.test("zero-argc exact function length "+std::to_string(length),[length]{auto config=registry(third);Model m;m.code.add({length,function_id,2});for(Word i=3;i<length;++i)m.code.words.push_back(0);m.code.end();reject(m,&config,ErrorCode::InvalidFormat);});
    suite.test("valid nonzero argument is evaluated and dispatched",[]{auto config=registry(third);Model m;m.code.function(2,{0x80000064U});m.code.end();Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m,&config);require(exec(vm,end_code).pc==37&&cb.variable_reads==1&&cb.events==std::vector<std::string>{"{\"kind\":\"user_function\",\"code\":2,\"parameters\":[7]}"},"argument evaluation/dispatch");});
    suite.test("UINT32_MAX function ID falls back without indexing",[]{auto config=registry(third);check_simple(UINT32_MAX,&config,true,false);});
    suite.test("unknown unreachable function loads without dispatch",[]{auto config=registry(third);Model m;m.code.end();m.code.function(0);m.code.end();Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m,&config);require(exec(vm,end_code).pc==32&&cb.events.empty()&&cb.skipped.empty(),"unreachable function executed");});
    suite.test("function may fall through to end",[]{auto config=registry(third);Model m;m.code.function(2);Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m,&config);require(exec(vm,end_code).pc==vm.WordCount()&&cb.events==std::vector<std::string>{"{\"kind\":\"user_function\",\"code\":2,\"parameters\":[]}"},"function fallthrough");});

    // Fixed original-method callback/PC/flag expectations are provided by the
    // independent reference task. They do not calculate outcomes via this VM.
    for(const auto& item:dw012_fixture::cases())suite.test(std::string("fixed original function trace: ")+item.name,[&]{
        const auto config_bytes=read_file(std::filesystem::path(NNN_FUNCTION_FIXTURE_DIR)/item.registry_filename);auto parsed=nnn::parse_function_registry(config_bytes);status_is(parsed.status,ErrorCode::Ok,"fixture registry");const Word expected_stop=item.stop_id<0?nnn::no_word:static_cast<Word>(item.stop_id);require(parsed.registry.StopF5Id()==expected_stop,"fixture binding");
        const auto input=read_file(std::filesystem::path(NNN_FUNCTION_FIXTURE_DIR)/item.script_filename);
        for(const auto& run:item.runs){Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;cb.SetSkipFlags(flags(run.initial_mask));status_is(vm.LoadBytes(input,item.name,{},&parsed.registry),ErrorCode::Ok,"fixture load");
            for(const auto& step:run.steps){const auto actual=exec(vm,step.return_code);require(actual.pc==step.pc&&mask(cb.GetSkipFlags())==step.skip_flags_mask,"fixture return/PC/flags");const auto active=frames(vm);require(active.size()==step.frames.size(),"fixture depth");for(std::size_t i=0;i<active.size();++i)require(active[i].return_pc==step.frames[i].return_pc&&active[i].id==step.frames[i].id,"fixture frames");}
            auto expected_events=run.callback_json;auto expected_states=run.callback_states;
            const bool fallback=std::string(item.name)=="missing" || std::string(item.name)=="quoted_name_is_literal";
            if(fallback){expected_events.erase(expected_events.begin()+1);expected_states.erase(expected_states.begin()+1);require(cb.skipped==std::vector<std::tuple<Word,Word,Word>>{{37,function_id,2}},"fixture fallback");}
            else require(cb.skipped.empty(),"unexpected fixture fallback");
            require(cb.events==expected_events&&cb.states.size()==expected_states.size(),"fixture complete callbacks/states");for(std::size_t i=0;i<cb.states.size();++i){const auto& actual=cb.states[i];const auto& expected=expected_states[i];require(actual.pc==expected.pc&&actual.flags==expected.skip_flags_mask&&actual.stack.size()==expected.frames.size(),"fixture callback PC/flags/depth");for(std::size_t f=0;f<actual.stack.size();++f)require(actual.stack[f].return_pc==expected.frames[f].return_pc&&actual.stack[f].id==expected.frames[f].id,"fixture callback frame");}require(mask(cb.GetSkipFlags())==run.final_mask,"fixture final flags");
        }
    });
    std::cout<<"FUNCTION RESULT "<<suite.passed<<" passed, "<<suite.failed<<" failed\n";return suite.failed==0?0:1;
}
