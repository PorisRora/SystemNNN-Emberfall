#include "nnn/scheduler.hpp"
#include "filmtype/fixtures/generated_cases.hpp"
#include "format_event_probe.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using nnn::Byte;using nnn::Word;using nnn::Value;using nnn::ErrorCode;using nnn::ScriptControlProfile;
constexpr Word data=0x66660001U,control=0x66660002U,system_function=0x66660007U;
constexpr Word filmtype=0x33330052U,print_code=0x22220001U,end_code=0x55560005U;
void require(bool value,const std::string& text){if(!value)throw std::runtime_error(text);}
void status_is(const nnn::Status& status,ErrorCode expected,const std::string& text){require(status.code==expected,text+": "+nnn::error_name(status.code)+" "+status.detail);}
nnn::SkipFlags flags(Word value){return {(value&1U)!=0,(value&2U)!=0,(value&4U)!=0,(value&8U)!=0,(value&16U)!=0};}
Word mask(nnn::SkipFlags value){return(value.skip_f4_mode?1U:0U)|(value.skip_f5_mode?2U:0U)|(value.skip_next_command?4U:0U)|(value.skip_effect_command?8U:0U)|(value.skip_to_film_end?16U:0U);}
std::vector<Byte> bytes(const std::string& text){std::vector<Byte> out;for(char c:text)out.push_back(static_cast<Byte>(static_cast<unsigned char>(c)));return out;}
std::string hex(const std::string& text){constexpr char digits[]="0123456789abcdef";std::string out;for(char c:text){const auto b=static_cast<unsigned char>(c);out+=digits[b>>4U];out+=digits[b&15U];}return out;}
std::vector<Byte> read_file(const std::string& name){const auto path=std::filesystem::path(NNN_FILMTYPE_FIXTURE_DIR)/name;std::ifstream input(path,std::ios::binary);require(static_cast<bool>(input),"fixture missing "+path.string());return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};}
std::vector<std::pair<Word,Word>> frames(const nnn::CExecScript& vm){std::vector<std::pair<Word,Word>> result;for(std::size_t i=0;i<vm.GetCallDepth();++i){nnn::CallFrame frame;require(vm.GetCallFrame(i,frame),"missing active frame");result.emplace_back(frame.return_pc,frame.id);}return result;}
struct State {Word pc;std::vector<std::pair<Word,Word>> stack;Word flags;std::size_t mode;};
struct Event {Word code;Value p1;std::string text;Value p3;};
struct Host final:nnn::CScriptCallBack {
    nnn::CExecScript* vm=nullptr;nnn::ScriptScheduler* owner=nullptr;
    std::vector<Event> events;std::vector<State> states;std::map<Word,Value> variables;
    std::function<void(Word)> on_script;std::size_t variable_reads=0,functions=0,numeric=0;
    void ReceiveScriptCommand(Word code,Value p1,const std::string& text,Value p3)override{
        const std::size_t active=owner?static_cast<std::size_t>(owner->ActiveMode()):0;
        const auto* current=owner?owner->GetVM(owner->ActiveMode()):vm;require(current!=nullptr,"callback VM absent");
        events.push_back({code,p1,text,p3});states.push_back({current->GetPC(),frames(*current),mask(GetSkipFlags()),active});
        if(on_script)on_script(code);
    }
    void PreReceiveScriptData(Word,Value,const std::string&)override{throw std::runtime_error("unexpected predata");}
    void ReceiveUserCommand0(Word,const std::vector<Value>&)override{throw std::runtime_error("unexpected command");}
    void ReceiveUserFunction0(Word,const std::vector<Value>&)override{++functions;throw std::runtime_error("raw value became function");}
    Value GetVarData(Word)override{++variable_reads;throw std::runtime_error("raw value read GetVarData");}
    Value GetCalcuVarData(Word)override{++variable_reads;throw std::runtime_error("raw value read GetCalcuVarData");}
    void SetVarData(Word,Value)override{throw std::runtime_error("raw value wrote variable");}
    bool ReceiveNumericScriptCommand(Word,Value,const std::vector<Value>&,Value)override{++numeric;throw std::runtime_error("FILMTYPETIME used numeric interface");}
    std::string json()const{std::string out="[";for(std::size_t i=0;i<events.size();++i){if(i)out+=',';const auto& e=events[i];out+="{\"kind\":\"script\",\"code\":"+std::to_string(e.code)+",\"para1\":"+std::to_string(e.p1)+",\"text_hex\":\""+hex(e.text)+"\",\"para3\":"+std::to_string(e.p3)+'}';}return out+"]";}
};
struct Code {
    std::vector<Word> words,sub_sites,script_sites;
    Word pc()const{return 32U+static_cast<Word>(words.size());}
    Word add(std::initializer_list<Word> values){const Word at=pc();words.insert(words.end(),values);return at;}
    Word film(Word value){return add({5,system_function,filmtype,1,value});}
    Word print(){return add({6,0x66660006U,print_code,0,0,0});}
    Word end(){return add({3,control,end_code});}
    Word sub(Word label){const Word at=add({5,control,0x55560009U,label,static_cast<Word>(sub_sites.size())});sub_sites.push_back(at);return at;}
    Word story(){return add({5,system_function,0x33330051U,1,0});}
    Word call(){const Word at=add({5,control,0x5556000aU,0,0});script_sites.push_back(at);return at;}
};
struct Model {Code code;bool labels=true;std::vector<std::string> messages{"M"},strings;std::vector<Word> targets;};
std::vector<Word> words(const Model& model){
    std::vector<Word> out(32,0);out[0]=32;out[1]=data;out[2]=0x55550001U;
    out[12]=0x48545053U;out[13]=0x45444145U;out[14]=0x20203052U;out[15]=0x00202020U;
    out.insert(out.end(),model.code.words.begin(),model.code.words.end());
    const auto table=[&](Word kind,Word field,const std::vector<Word>& payload,bool record=false){out[field]=static_cast<Word>(out.size())+(record?0U:4U);out.insert(out.end(),{4U+static_cast<Word>(payload.size()),data,0x55550002U,kind});out.insert(out.end(),payload.begin(),payload.end());};
    if(model.labels){out[8]=out[10]=static_cast<Word>(model.targets.size());table(0x44440003U,9,model.targets);table(0x44440003U,16,{},true);table(0x44440003U,17,{},true);}
    const auto text=[&](Word kind,Word field,Word countfield,const std::vector<std::string>& entries){
        out[countfield]=static_cast<Word>(entries.size());const Word begin=static_cast<Word>(out.size())+4U;std::vector<Word> payload(entries.size(),0);
        for(std::size_t i=0;i<entries.size();++i){payload[i]=begin+static_cast<Word>(payload.size());auto raw=bytes(entries[i]);raw.push_back(0);while(raw.size()%4U)raw.push_back(0);for(std::size_t b=0;b<raw.size();b+=4U)payload.push_back(static_cast<Word>(raw[b])|(static_cast<Word>(raw[b+1U])<<8U)|(static_cast<Word>(raw[b+2U])<<16U)|(static_cast<Word>(raw[b+3U])<<24U));}table(kind,field,payload);
    };
    text(0x44440002U,5,4,model.messages);text(0x44440001U,7,6,model.strings);
    table(0x44440004U,18,{});table(0x44440005U,20,model.code.sub_sites);table(0x44440006U,21,{});table(0x44440007U,22,{});table(0x44440008U,23,model.code.script_sites);return out;
}
std::vector<Byte> encode(const std::vector<Word>& decoded){std::vector<Byte> result;for(Word value:decoded){value^=UINT32_MAX;for(unsigned shift=0;shift<32;shift+=8)result.push_back(static_cast<Byte>(value>>shift));}return result;}
std::vector<Byte> script(const Model& model){return encode(words(model));}
void load(nnn::CExecScript& vm,const Model& model,ScriptControlProfile profile=ScriptControlProfile::Closed,const nnn::FunctionRegistry* config=nullptr){status_is(vm.LoadBytes(script(model),"filmtype",{},config,profile),ErrorCode::Ok,"film load");}
nnn::ExecResult exec(nnn::CExecScript& vm,Word expected,std::uint64_t work=100000){const auto r=vm.Exec(work);status_is(r.status,ErrorCode::Ok,"film Exec");require(r.return_code==expected,"unexpected yield code");return r;}
void reject(const Model& model,ErrorCode expected=ErrorCode::InvalidFormat,ScriptControlProfile profile=ScriptControlProfile::Closed){Host h;nnn::CExecScript vm(h);h.vm=&vm;h.SetSkipFlags(flags(31));status_is(vm.LoadBytes(script(model),"bad",{},nullptr,profile),expected,"rejected shape");require(!vm.IsLoaded()&&h.events.empty()&&mask(h.GetSkipFlags())==31,"bad input changed/called host");}
struct Suite {int passed=0,failed=0;void test(const std::string& name,const std::function<void()>& body){try{body();++passed;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& error){++failed;std::cerr<<"FAIL "<<name<<": "<<error.what()<<'\n';}}};
} // namespace

int main(){Suite suite;
    const std::vector<std::pair<Word,Value>> values{{0,0},{1,1},{UINT32_MAX,-1},{0x7fffffffU,2147483647},{0x80000000U,-2147483647-1},
        {0x80000001U,-2147483647},{0x7fffU,32767},{0x8000U,32768},{0x12345678U,305419896},
        {0x80000007U,-2147483641},{0x90000007U,-1879048185},{0x10000000U,268435456},{0xa500003dU,-1526726595}};
    for(const bool labels:{false,true})for(const auto profile:{ScriptControlProfile::Closed,ScriptControlProfile::Foreground})for(const auto& pair:values)
    suite.test("raw signed word across table/control profiles "+std::to_string(pair.first)+(labels?" labels":" legacy")+(profile==ScriptControlProfile::Closed?" Closed":" Foreground"),[=]{
        Model m;m.labels=labels;m.messages.clear();m.code.film(pair.first);m.code.end();Host h;nnn::CExecScript vm(h);h.vm=&vm;h.SetSkipFlags(flags(31));load(vm,m,profile);const auto r=exec(vm,end_code);
        require(r.pc==37&&h.events.size()==1&&h.events[0].code==filmtype&&h.events[0].p1==pair.second&&h.events[0].text.empty()&&h.events[0].p3==0,"raw dispatch fields/sameExec END");
        require(h.states[0].pc==32&&h.states[0].stack.empty()&&h.states[0].flags==31&&mask(h.GetSkipFlags())==31&&h.variable_reads==0&&h.numeric==0&&h.functions==0,"raw operand was evaluated or state changed");
    });
    suite.test("film continues into PRINT and empty text is an owned host copy",[]{Model m;m.code.film(UINT32_MAX);m.code.print();m.code.end();Host h;nnn::CExecScript vm(h);h.vm=&vm;load(vm,m);const auto r=exec(vm,print_code);require(r.pc==43&&h.events.size()==2&&h.states[0].pc==32&&h.states[1].pc==37&&h.events[1].text=="M","film yielded or stepped callback early");load(vm,m);require(h.events[0].text.empty()&&h.events[0].p1==-1,"host copy invalidated by load");});
    suite.test("consecutive films dispatch once each in source order",[]{Model m;m.code.film(0);m.code.film(0x80000000U);m.code.film(UINT32_MAX);m.code.print();m.code.end();Host h;nnn::CExecScript vm(h);h.vm=&vm;load(vm,m);exec(vm,print_code);require(h.events.size()==4&&h.events[0].p1==0&&h.events[1].p1==(-2147483647-1)&&h.events[2].p1==-1&&h.states[0].pc==32&&h.states[1].pc==37&&h.states[2].pc==42&&h.states[3].pc==47,"film order/current PC");});
    suite.test("film inside SUB preserves frame until normal RETURN",[]{Model m;m.strings={"S"};m.code.sub(0);m.code.print();m.code.end();m.targets={m.code.story()};m.code.film(0x80000007U);m.code.add({3,control,0x55560007U});Host h;nnn::CExecScript vm(h);h.vm=&vm;load(vm,m);exec(vm,print_code);require(h.events.size()==3&&h.events[1].code==filmtype&&h.states[1].pc==51&&h.states[1].stack==std::vector<std::pair<Word,Word>>{{37,0}}&&h.states[2].pc==37&&h.states[2].stack.empty()&&vm.GetCallDepth()==0,"film damaged SUB frame");});
    for(std::uint64_t budget=0;budget<=9;++budget)suite.test("dispatch cost before callback "+std::to_string(budget),[budget]{Model m;m.code.film(1);m.code.end();Host h;nnn::CExecScript vm(h);h.vm=&vm;h.SetSkipFlags(flags(31));load(vm,m);const auto r=vm.Exec(budget);status_is(r.status,budget<9?ErrorCode::BudgetExhausted:ErrorCode::Ok,"film budget");require(h.events.size()==(budget<6?0U:1U)&&vm.GetPC()==(budget==0?0U:budget<6?32U:37U)&&r.work_used==(budget==0?0U:budget<6?1U:budget<9?6U:9U)&&mask(h.GetSkipFlags())==31,"budget entered callback or advanced wrong PC");if(budget==9)require(r.return_code==end_code,"exact END cost");});
    suite.test("completed event survives later budget failure and is not replayed",[]{Model m;m.code.film(1);m.code.film(UINT32_MAX);m.code.end();Host h;nnn::CExecScript vm(h);h.vm=&vm;load(vm,m);const auto r=vm.Exec(6);status_is(r.status,ErrorCode::BudgetExhausted,"first event boundary");require(r.work_used==6&&vm.GetPC()==37&&h.events.size()==1,"first event not committed");exec(vm,end_code,8);require(h.events.size()==2&&h.events[0].p1==1&&h.events[1].p1==-1&&vm.GetPC()==42,"completed film replayed");});
    suite.test("film can complete before PRINT's text budget fails",[]{Model m;m.code.film(7);m.code.print();m.code.end();Host h;nnn::CExecScript vm(h);h.vm=&vm;load(vm,m);const auto r=vm.Exec(6);status_is(r.status,ErrorCode::BudgetExhausted,"later PRINT work");require(r.work_used==6&&vm.GetPC()==37&&h.events.size()==1&&h.events[0].code==filmtype,"prior film rolled back or PRINT entered");exec(vm,print_code);require(h.events.size()==2&&h.events[1].code==print_code,"film repeated before PRINT");});
    for(const bool mutate:{false,true})suite.test(std::string("throwing film preserves PC and ordinary host side effects ")+(mutate?"mutated":"original"),[mutate]{Model m;m.code.film(7);m.code.print();m.code.end();Host h;nnn::CExecScript vm(h);h.vm=&vm;h.SetSkipFlags(flags(31));load(vm,m);h.on_script=[&](Word code){if(code==filmtype){if(mutate){h.variables[100]=99;h.SetSkipFlags(flags(9));}throw std::runtime_error("film host");}};const auto r=vm.Exec();status_is(r.status,ErrorCode::CallbackFailed,"film exception");require(vm.GetPC()==32&&h.events.size()==1&&mask(h.GetSkipFlags())==(mutate?9U:31U),"throw advanced/cleared/rolled back host");if(mutate)require(h.variables[100]==99,"explicit host mutation lost");});
    suite.test("earlier film side effects survive later host failure",[]{Model m;m.code.film(1);m.code.film(2);m.code.print();m.code.end();Host h;nnn::CExecScript vm(h);h.vm=&vm;load(vm,m);h.on_script=[&](Word code){if(code==filmtype){h.variables[100]=static_cast<Value>(h.events.size());h.SetSkipFlags(flags(5));if(h.events.size()==2)throw std::runtime_error("second film");}};status_is(vm.Exec().status,ErrorCode::CallbackFailed,"second event exception");require(vm.GetPC()==37&&h.events.size()==2&&h.variables[100]==2&&mask(h.GetSkipFlags())==5,"earlier effects rolled back/next records ran");});
    suite.test("nonstandard callback exception is contained",[]{Model m;m.code.film(1);m.code.end();Host h;nnn::CExecScript vm(h);h.vm=&vm;load(vm,m);h.on_script=[](Word){throw 4;};status_is(vm.Exec().status,ErrorCode::CallbackFailed,"nonstandard throw");require(vm.GetPC()==32&&h.events.size()==1,"nonstandard throw advanced");});
    suite.test("film callback Busy reentry leaves outer current PC and mode",[]{Model m;m.code.film(1);m.code.print();m.code.end();Host h;nnn::CExecScript vm(h);h.vm=&vm;load(vm,m,ScriptControlProfile::Foreground);h.on_script=[&](Word code){if(code!=filmtype)return;require(vm.GetPC()==32,"film callback not current PC");status_is(vm.Exec().status,ErrorCode::Busy,"nested Exec");status_is(vm.LoadBytes(script(m),"nested"),ErrorCode::Busy,"nested bytes");int reads=0;status_is(vm.LoadFile("nested",{},[&](const std::string&){++reads;return nnn::ReadResult{};}),ErrorCode::Busy,"nested file");require(reads==0&&vm.GetPC()==32&&vm.HasForegroundControls(),"Busy changed state");};exec(vm,print_code);status_is(vm.LastStatus(),ErrorCode::Ok,"outer status");});
    suite.test("film raw zero does not invoke registered stopf5 or clear flags",[]{auto result=nnn::parse_function_registry(bytes("stopf5,d,"));status_is(result.status,ErrorCode::Ok,"registry");Model m;m.code.film(0);m.code.end();Host h;nnn::CExecScript vm(h);h.vm=&vm;h.SetSkipFlags(flags(31));load(vm,m,ScriptControlProfile::Closed,&result.registry);exec(vm,end_code);require(mask(h.GetSkipFlags())==31&&h.functions==0&&vm.HasFunctionRegistry()&&vm.BoundStopF5Id()==0,"film confused with registry function");});
    suite.test("film callback throw and failed reload preserve SUB and binding",[]{auto config=nnn::parse_function_registry(bytes("stopf5,d,"));Model m;m.strings={"S"};m.code.sub(0);m.code.print();m.code.end();m.targets={m.code.story()};m.code.film(1);m.code.add({3,control,0x55560007U});Host h;nnn::CExecScript vm(h);h.vm=&vm;load(vm,m,ScriptControlProfile::Foreground,&config.registry);h.on_script=[](Word code){if(code==filmtype)throw std::runtime_error("nested film");};status_is(vm.Exec().status,ErrorCode::CallbackFailed,"nested throw");const auto before=frames(vm);status_is(vm.LoadBytes({0,0,0,0},"bad"),ErrorCode::InvalidFormat,"failed reload");require(vm.GetPC()==51&&frames(vm)==before&&before==std::vector<std::pair<Word,Word>>{{37,0}}&&vm.Identity()=="filmtype"&&vm.HasForegroundControls()&&vm.HasFunctionRegistry()&&vm.BoundStopF5Id()==0,"failed reload/film altered original state");});
    for(const bool labels:{false,true})for(const auto profile:{ScriptControlProfile::Closed,ScriptControlProfile::Foreground}){
        for(const Word count:{0U,2U,UINT32_MAX})suite.test("invalid count "+std::to_string(count)+(labels?" labels":" legacy")+(profile==ScriptControlProfile::Closed?" Closed":" Foreground"),[=]{Model m;m.labels=labels;m.code.add({5,system_function,filmtype,count,1});m.code.end();reject(m,ErrorCode::InvalidFormat,profile);});
        for(const Word length:{3U,4U,6U})suite.test("exact length "+std::to_string(length)+(labels?" labels":" legacy")+(profile==ScriptControlProfile::Closed?" Closed":" Foreground"),[=]{Model m;m.labels=labels;m.code.add({length,system_function,filmtype});for(Word i=3;i<length;++i)m.code.words.push_back(i==3?1U:0U);m.code.end();reject(m,ErrorCode::InvalidFormat,profile);});
        suite.test(std::string("requires executable next ")+(labels?"labels":"legacy")+(profile==ScriptControlProfile::Closed?" Closed":" Foreground"),[=]{Model m;m.labels=labels;m.code.film(0);Host h;nnn::CExecScript vm(h);h.vm=&vm;load(vm,m,profile);const auto r=exec(vm,end_code);require(r.pc==vm.WordCount()&&h.events.size()==1&&h.states[0].pc==32&&h.events[0].code==filmtype&&h.events[0].p1==0,"film fallthrough");});
    }
    for(const Word kind:{0x66660006U,0xdeadbeefU})suite.test("film code follows identify format "+std::to_string(kind),[kind]{Model m;m.code.add({5,kind,filmtype,1,0});m.code.end();if(kind==0xdeadbeefU)format_expect_unknown(script(m),ScriptControlProfile::Closed,kind,filmtype,5);else format_expect_numeric(script(m),ScriptControlProfile::Closed,kind,filmtype,{0});});
    for(const Word code:{0x33330053U,0x3333ffffU})suite.test("other system function dispatches by format "+std::to_string(code),[code]{Model m;m.code.add({5,system_function,code,1,0});m.code.end();format_expect_numeric(script(m),ScriptControlProfile::Closed,system_function,code,{0});});
    suite.test("unreachable malformed film still rejected",[]{Model m;m.code.end();m.code.add({5,system_function,filmtype,2,0});m.code.end();reject(m);});
    suite.test("film followed by unverified IF equality stays rejected",[]{Model m;m.code.film(0);m.code.add({4,control,0x55560002U,0xa500003dU});m.code.add({4,control,0xa000007bU,10});m.code.add({3,control,0x55560001U});m.code.add({3,control,0xa000007dU});m.code.end();reject(m,ErrorCode::UnsupportedExpression);});
    suite.test("film does not widen RPN SCRIPT_FUNCTION token",[]{Model m;m.code.film(0);m.code.add({6,0x66660005U,0,0xa0000000U,1,0x10000000U});m.code.end();reject(m,ErrorCode::UnsupportedExpression);});
    for(const bool child_labels:{false,true})suite.test(std::string("production owner child film shares flags and current mode ")+(child_labels?"labels":"legacy"),[child_labels]{
        Model root;root.strings={"child"};root.code.call();root.code.film(2);root.code.print();root.code.end();Model child;child.labels=child_labels;child.code.film(0x80000000U);child.code.add({3,control,0x55560006U});
        for(Word initial=0;initial<32;++initial){Host h;std::vector<std::string> requests;nnn::ScriptScheduler owner(h,[&](const std::string& key){requests.push_back(key);require(key=="spt/child.spt","wrong child key");const auto input=script(child);return nnn::ReadResult{{},input,input.size()};});h.owner=&owner;h.SetSkipFlags(flags(initial));h.variables[100]=17;status_is(owner.LoadBytes(script(root),"control"),ErrorCode::Ok,"owner root");const auto push=owner.Step();status_is(push.status,ErrorCode::Ok,"owner push");require(push.transitions==1&&owner.ActiveMode()==nnn::ScriptMode::Main,"not in child");const auto pop=owner.Step();status_is(pop.status,ErrorCode::Ok,"child film and NEXT");require(pop.return_code==0x55560006U&&pop.executed_mode==nnn::ScriptMode::Main&&pop.active_mode==nnn::ScriptMode::Control&&pop.pc==37&&pop.transitions==1&&owner.GetVM(nnn::ScriptMode::Control)->GetPC()==37,"film yielded/advanced parent during pop");const auto result=owner.Exec();status_is(result.status,ErrorCode::Ok,"parent film/PRINT");require(result.return_code==print_code&&result.pc==48&&h.events.size()==4&&h.events[1].code==filmtype&&h.events[1].p1==(-2147483647-1)&&h.states[1].mode==1&&h.states[1].pc==32&&h.events[2].code==filmtype&&h.states[2].mode==0&&h.states[2].pc==37,"owner film callback routing/PC");Word slot=99;require(owner.GetVM(nnn::ScriptMode::Control)->GetScriptCallSlot(slot)&&slot==0&&mask(h.GetSkipFlags())==initial&&h.variables[100]==17&&h.variable_reads==0&&h.numeric==0,"film changed slot/shared state");for(const auto& state:h.states)require(state.flags==initial,"film cleared host flags");require(requests==std::vector<std::string>{"spt/child.spt"},"film performed resource IO");}
    });
    for(const auto& item:dw014_fixture::cases())for(const auto profile:{ScriptControlProfile::Closed,ScriptControlProfile::Foreground})suite.test("fixed original trace "+item.name+(profile==ScriptControlProfile::Closed?" Closed":" Foreground"),[&,profile]{Host h;nnn::CExecScript vm(h);h.vm=&vm;status_is(vm.LoadBytes(read_file(item.filename),item.name,{},nullptr,profile),ErrorCode::Ok,"fixed load");for(const auto& step:item.steps){const auto r=exec(vm,step.return_code);require(r.pc==step.pc&&vm.GetCallDepth()==step.frames.size(),"fixed return/PC/frames");}require(h.json()==item.callback_json&&h.states.size()==item.callback_states.size()&&h.events.size()==item.raw_values.size()+1U,"fixed complete callbacks");for(std::size_t i=0;i<h.states.size();++i){const auto& actual=h.states[i];const auto& wanted=item.callback_states[i];require(actual.pc==wanted.pc&&actual.stack.size()==wanted.frames.size(),"fixed callback PC/frame count");for(std::size_t f=0;f<actual.stack.size();++f)require(actual.stack[f].first==wanted.frames[f].return_pc&&actual.stack[f].second==wanted.frames[f].id,"fixed callback frame");}require(h.variable_reads==0&&h.numeric==0,"fixed raw values evaluated");});
    std::cout<<"FILMTYPE RESULT "<<suite.passed<<" passed, "<<suite.failed<<" failed\n";return suite.failed==0?0:1;
}
