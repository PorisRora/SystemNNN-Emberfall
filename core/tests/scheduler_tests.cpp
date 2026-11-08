#include "nnn/scheduler.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using nnn::Byte; using nnn::Word; using nnn::Value; using nnn::ErrorCode;
using nnn::ScriptMode; using nnn::ScriptScheduler; using nnn::ScriptControlProfile;
constexpr Word data=0x66660001U, control=0x66660002U, next_code=0x55560006U;
constexpr Word script_code=0x5556000aU, subscript_code=0x55560016U;
constexpr Word print_code=0x22220001U, end_code=0x55560005U;
ScriptMode mode(std::size_t index){return static_cast<ScriptMode>(index);}
std::size_t index(ScriptMode value){return static_cast<std::size_t>(value);}
void require(bool condition,const std::string& detail){if(!condition)throw std::runtime_error(detail);}
void status_is(const nnn::Status& status,ErrorCode expected,const std::string& detail){require(status.code==expected,detail+": "+nnn::error_name(status.code)+" "+status.detail);}
nnn::SkipFlags flags(Word value){return {(value&1U)!=0,(value&2U)!=0,(value&4U)!=0,(value&8U)!=0,(value&16U)!=0};}
Word mask(nnn::SkipFlags value){return(value.skip_f4_mode?1U:0U)|(value.skip_f5_mode?2U:0U)|(value.skip_next_command?4U:0U)|(value.skip_effect_command?8U:0U)|(value.skip_to_film_end?16U:0U);}
std::vector<Byte> raw(const std::string& text){std::vector<Byte> result;for(char c:text)result.push_back(static_cast<Byte>(static_cast<unsigned char>(c)));return result;}
std::vector<Byte> file(const std::string& name){const auto path=std::filesystem::path(NNN_SCHEDULER_FIXTURE_DIR)/name;std::ifstream input(path,std::ios::binary);require(static_cast<bool>(input),"missing fixture "+path.string());return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};}
nnn::FunctionRegistry registry(){auto result=nnn::parse_function_registry(raw("one,d,two,d,stopf5,d,"));status_is(result.status,ErrorCode::Ok,"registry");return std::move(result.registry);}
struct Code {
    std::vector<Word> words,sub_slots,script_sites;
    Word pc()const{return 32U+static_cast<Word>(words.size());}
    Word add(std::initializer_list<Word> values){const Word at=pc();words.insert(words.end(),values);return at;}
    Word marker(Word code){return add({3,control,code});}
    Word call(Word code=script_code,Word string_index=0,Word slot=0){const Word at=add({5,control,code,string_index,slot});script_sites.push_back(at);return at;}
    Word print(){return add({6,0x66660006U,print_code,0,0,0});}
    Word sub(Word label){const Word at=add({5,control,0x55560009U,label,static_cast<Word>(sub_slots.size())});sub_slots.push_back(at);return at;}
    Word story(Word string_index=0){return add({5,0x66660007U,0x33330051U,1,string_index});}
    Word jump(Word destination){const Word at=add({4,control,0x55560020U,destination});sub_slots.push_back(UINT32_MAX);return at;}
    void function(){add({4,0x66660004U,2,0});}
    void set(Word variable,Word number){add({8,0x66660005U,0,0xa0000000U,3,0x80000000U|variable,0x90000000U|number,0xa000003dU});}
};
struct Model {Code code;bool labels=true;std::vector<std::string> strings{"child"};std::vector<Word> targets,label_adjust,command_adjust;};
std::vector<Word> words(const Model& model){
    std::vector<Word> out(32,0);out[0]=32;out[1]=data;out[2]=0x55550001U;
    out[12]=0x48545053U;out[13]=0x45444145U;out[14]=0x20203052U;out[15]=0x00202020U;
    out.insert(out.end(),model.code.words.begin(),model.code.words.end());
    const auto table=[&](Word kind,Word field,const std::vector<Word>& payload,bool record=false){out[field]=static_cast<Word>(out.size())+(record?0U:4U);out.insert(out.end(),{4U+static_cast<Word>(payload.size()),data,0x55550002U,kind});out.insert(out.end(),payload.begin(),payload.end());};
    if(model.labels){out[8]=out[10]=static_cast<Word>(model.targets.size());table(0x44440003U,9,model.targets);table(0x44440003U,16,{},true);table(0x44440003U,17,{},true);}
    const auto text=[&](Word kind,Word field,Word countfield,const std::vector<std::string>& values){
        out[countfield]=static_cast<Word>(values.size());const Word begin=static_cast<Word>(out.size())+4U;std::vector<Word> payload(values.size(),0);
        for(std::size_t i=0;i<values.size();++i){payload[i]=begin+static_cast<Word>(payload.size());auto bytes=raw(values[i]);bytes.push_back(0);while(bytes.size()%4U)bytes.push_back(0);for(std::size_t b=0;b<bytes.size();b+=4U)payload.push_back(static_cast<Word>(bytes[b])|(static_cast<Word>(bytes[b+1U])<<8U)|(static_cast<Word>(bytes[b+2U])<<16U)|(static_cast<Word>(bytes[b+3U])<<24U));}table(kind,field,payload);
    };
    text(0x44440002U,5,4,{"M"});text(0x44440001U,7,6,model.strings);
    table(0x44440004U,18,{});table(0x44440005U,20,model.code.sub_slots);table(0x44440006U,21,model.label_adjust);
    table(0x44440007U,22,model.command_adjust);table(0x44440008U,23,model.code.script_sites);return out;
}
std::vector<Byte> encode(const std::vector<Word>& source,nnn::DecodeParameters parameters={}){std::vector<Byte> result;Word key=parameters.xor_key,increment=0;for(Word value:source){value=(value-increment)^key;for(unsigned shift=0;shift<32;shift+=8)result.push_back(static_cast<Byte>(value>>shift));key+=parameters.key_step;increment+=parameters.increment_step;}return result;}
std::vector<Byte> script(const Model& model,nnn::DecodeParameters parameters={}){return encode(words(model),parameters);}
Model caller(Word code=script_code,const std::string& target="child"){Model m;m.strings={target};m.code.call(code);m.code.print();m.code.marker(end_code);return m;}
Model leaf(Word code=next_code){Model m;m.code.marker(code);return m;}
Model repeated(){Model m;m.code.call();m.code.jump(32);return m;}
std::vector<std::pair<Word,Word>> frames(const nnn::CExecScript& vm){std::vector<std::pair<Word,Word>> result;for(std::size_t i=0;i<vm.GetCallDepth();++i){nnn::CallFrame value;require(vm.GetCallFrame(i,value),"missing frame");result.emplace_back(value.return_pc,value.id);}return result;}
struct Event {Word code;Value p1;std::string text;Value p3;std::size_t mode;Word pc;Word flags;};
struct Host final:nnn::CScriptCallBack {
    ScriptScheduler* owner=nullptr;nnn::CExecScript* bare=nullptr;
    std::map<Word,Value> variables;std::vector<Event> events;std::size_t functions=0,numerics=0;
    std::function<void(Word)> on_script,on_command;std::function<void()> on_function;
    void ReceiveScriptCommand(Word code,Value p1,const std::string& text,Value p3)override{
        const std::size_t active=owner?index(owner->ActiveMode()):0;const auto* vm=owner?owner->GetVM(mode(active)):bare;
        events.push_back({code,p1,text,p3,active,vm?vm->GetPC():0,mask(GetSkipFlags())});if(on_script)on_script(code);
    }
    void PreReceiveScriptData(Word,Value,const std::string&)override{throw std::runtime_error("unexpected predata");}
    void ReceiveUserCommand0(Word code,const std::vector<Value>& args)override{
        require(on_command && args.empty(),"unexpected user command");
        on_command(code);
    }
    void ReceiveUserFunction0(Word code,const std::vector<Value>& values)override{require(code==2&&values.empty(),"wrong function binding");++functions;if(on_function)on_function();}
    Value GetVarData(Word key)override{return variables[key];}
    Value GetCalcuVarData(Word key)override{return variables[key];}
    void SetVarData(Word key,Value value)override{variables[key]=value;}
    bool ReceiveNumericScriptCommand(Word,Value,const std::vector<Value>&,Value)override{++numerics;return true;}
};
struct Environment {
    Host host;std::map<std::string,std::vector<Byte>> files;std::vector<std::string> requests;
    std::function<void(const std::string&)> on_read;
    std::function<nnn::ReadResult(const std::string&)> override_read;
    std::unique_ptr<ScriptScheduler> owner;
    explicit Environment(const nnn::FunctionRegistry* config=nullptr){owner=std::make_unique<ScriptScheduler>(host,[this](const std::string& name){requests.push_back(name);if(on_read)on_read(name);if(override_read)return override_read(name);const auto found=files.find(name);if(found==files.end())return nnn::ReadResult{{ErrorCode::NotFound,nnn::no_word,"missing authored file"},{},0};return nnn::ReadResult{{},found->second,found->second.size()};},config);host.owner=owner.get();}
    void load(const Model& model,const std::string& identity="control",nnn::DecodeParameters parameters={}){status_is(owner->LoadBytes(script(model,parameters),identity,parameters),ErrorCode::Ok,"owner load");}
    const nnn::CExecScript& vm(std::size_t value)const{const auto* result=owner->GetVM(mode(value));require(result!=nullptr,"valid slot absent");return *result;}
    nnn::SchedulerResult step(std::uint64_t work=100000){auto result=owner->Step(work);status_is(result.status,ErrorCode::Ok,"Step");require(result.transitions<=1,"Step changed more than one mode");return result;}
};
struct VmState {std::string identity;Word pc;std::vector<Word> words;std::vector<std::pair<Word,Word>> stack;bool slot_valid;Word slot;bool foreground;bool has_registry;std::size_t registry_size;Word stop;
    bool operator==(const VmState& other)const{return identity==other.identity&&pc==other.pc&&words==other.words&&stack==other.stack&&slot_valid==other.slot_valid&&slot==other.slot&&foreground==other.foreground&&has_registry==other.has_registry&&registry_size==other.registry_size&&stop==other.stop;}};
std::vector<VmState> snapshot(const Environment& env){std::vector<VmState> result;for(std::size_t m=0;m<4;++m){const auto& vm=env.vm(m);VmState state{vm.Identity(),vm.GetPC(),{},frames(vm),false,0,vm.HasForegroundControls(),vm.HasFunctionRegistry(),vm.FunctionRegistrySize(),vm.BoundStopF5Id()};Word slot=123456;state.slot_valid=vm.GetScriptCallSlot(slot);state.slot=slot;for(std::size_t i=0;i<vm.WordCount();++i){Word value;require(vm.GetData(static_cast<Word>(i),value),"word inspector");state.words.push_back(value);}result.push_back(std::move(state));}return result;}
void chain(Environment& env){env.files["spt/main.spt"]=file("main.spt");env.files["spt/_sub.spt"]=file("_sub.spt");env.files["spt/__sub.spt"]=file("__sub.spt");status_is(env.owner->LoadBytes(file("control.spt"),"control"),ErrorCode::Ok,"chain load");}
void depth_three(Environment& env){chain(env);for(std::size_t m=0;m<3;++m){const auto result=env.step();require(index(result.executed_mode)==m&&index(result.active_mode)==m+1U,"chain push");}}
void busy_checks(Environment& env){const auto before=snapshot(env);const auto active=env.owner->ActiveMode();const auto reads=env.requests.size();status_is(env.owner->Step().status,ErrorCode::Busy,"reentrant Step");status_is(env.owner->Exec().status,ErrorCode::Busy,"reentrant Exec");status_is(env.owner->LoadBytes(script(leaf()),"nested"),ErrorCode::Busy,"reentrant bytes");status_is(env.owner->LoadFile("nested"),ErrorCode::Busy,"reentrant file");require(snapshot(env)==before&&env.owner->ActiveMode()==active&&env.requests.size()==reads,"Busy mutated owner or invoked reader");}
void foreground_reject(const Model& model,ErrorCode expected){Host host;nnn::CExecScript vm(host);host.bare=&vm;status_is(vm.LoadBytes(script(model),"bad",{},nullptr,ScriptControlProfile::Foreground),expected,"foreground shape rejection");require(!vm.IsLoaded()&&host.events.empty(),"bad foreground program installed/dispatched");}
struct Suite {int passed=0,failed=0;void test(const std::string& name,const std::function<void()>& body){try{body();++passed;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& error){++failed;std::cerr<<"FAIL "<<name<<": "<<error.what()<<'\n';}}};
} // namespace

int main(){Suite suite;
    suite.test("PC chart captures independent command adjustment and raw active PCs",[]{
        constexpr Word user_command=0x66660003U,user_return=0x11110001U;
        Environment e;
        Model root;
        const auto root_command=root.code.add({5,user_command,70,0,0});
        root.code.call();root.code.marker(end_code);
        root.command_adjust={root_command};
        Model child;
        const auto child_command=child.code.add({5,user_command,71,0,1});
        child.code.set(3,10);
        const auto function=child.code.pc();
        child.code.function();
        child.code.print();
        const auto unadjustable=child.code.add({4,user_command,72,0});
        child.code.marker(next_code);
        child.command_adjust={unadjustable,child_command};
        e.files["spt/sys.spt"]=script(root);
        e.files["spt/child.spt"]=script(child);
        status_is(e.owner->LoadFile("spt/sys.spt"),ErrorCode::Ok,"chart root load");
        require(e.owner->pc_adjustment_ids(),"chart PC adjustment header");

        e.host.on_command=[&](Word code) {
            Word slot=99;
            const auto& vm=*e.owner->GetVM(e.owner->ActiveMode());
            if (code==70)
                require(vm.GetCommandCallSlot(slot) && slot==0 && vm.GetPC()==root_command+5,"root callback slot/PC");
            else if (code==71)
                require(vm.GetCommandCallSlot(slot) && slot==1 && vm.GetPC()==child_command+5,"child callback slot/PC");
            else
                require(code==72 && !vm.GetCommandCallSlot(slot) && slot==99,"missing tail retained or fabricated an adjustment");
        };
        std::array<nnn::PcScriptSlot,nnn::script_mode_count> positions;
        e.host.on_function=[&] {
            status_is(e.owner->capture_pc_positions(positions,true,nnn::PcScriptWait::Command),ErrorCode::Ok,"slotopen capture");
            require(positions[0].script_call==0 && positions[0].command_call==0 &&
                positions[1].pc==function && positions[1].command_call==1,"function lost independent PC slots");
        };
        require(e.step().return_code==user_return && e.step().active_mode==mode(1) &&
            e.step().return_code==user_return && e.step().return_code==print_code,"chart command sequence");

        nnn::snapshot::Writer old,current;
        e.owner->save_snapshot(old);
        e.owner->save_snapshot(current,true);
        nnn::snapshot::Reader older(old.bytes);
        e.owner->load_snapshot(older);
        Word slot=99;
        require(older.remaining()==0 && !e.vm(1).GetCommandCallSlot(slot) && slot==99,"old snapshot invented a slot");
        nnn::snapshot::Reader modern(current.bytes);
        e.owner->load_snapshot(modern,nullptr,true);
        require(modern.remaining()==0 && e.vm(0).GetCommandCallSlot(slot) && slot==0 &&
            e.vm(1).GetCommandCallSlot(slot) && slot==1,"native snapshot lost executed command slots");

        auto invalid=positions;
        invalid[1].command_call=0;
        const auto prior=e.vm(1).GetPC();
        status_is(e.owner->restore_pc_positions(mode(1),invalid,true,nnn::PcScriptWait::Command,0,false),
            ErrorCode::InvalidFormat,"table target without command tail");
        require(e.vm(1).GetPC()==prior && e.vm(1).GetCommandCallSlot(slot) && slot==1,"bad chart replaced live VM");
        status_is(e.owner->restore_pc_positions(mode(1),positions,true,nnn::PcScriptWait::Command,0,false),
            ErrorCode::Ok,"adjusted chart resume");
        require(e.vm(1).GetPC()==child_command+5 && e.vm(0).GetPC()==positions[0].pc,"chart resume relocated wrong PC");
        status_is(e.owner->restore_pc_positions(mode(1),positions,true,nnn::PcScriptWait::Raw,0,false),
            ErrorCode::Ok,"raw TITLE resume");
        require(e.vm(1).GetPC()==function,"TITLE unexpectedly adjusted command");
        status_is(e.owner->restore_pc_positions(mode(1),positions,false,nnn::PcScriptWait::Command,0,false),
            ErrorCode::Ok,"legacy raw chart resume");
        require(e.vm(1).GetPC()==function,"legacy chart unexpectedly adjusted command");
        invalid=positions;invalid[1].pc=function+1;
        status_is(e.owner->restore_pc_positions(mode(1),invalid,true,nnn::PcScriptWait::Raw,0,false),
            ErrorCode::InvalidFormat,"non-boundary raw PC");
        require(e.vm(1).GetPC()==function && e.step().return_code==print_code && e.step().return_code==user_return,
            "invalid raw restore changed resumed code");
        const auto saved_pc=positions[1].pc;
        status_is(e.owner->capture_pc_positions(positions,true,nnn::PcScriptWait::Command),ErrorCode::InvalidFormat,
            "unadjustable chart capture");
        require(positions[1].pc==saved_pc && positions[1].command_call==1,"failed capture mutated caller state");
    });
    suite.test("PC SCRIPT and SUB adjustment slots restore both return points",[]{
        Environment e;
        Model child;
        child.code.sub(0);child.code.marker(next_code);
        child.targets={child.code.print()};
        const Word resume=child.code.marker(0x55560007U);
        e.files["spt/sys.spt"]=script(caller());
        e.files["spt/child.spt"]=script(child);
        status_is(e.owner->LoadFile("spt/sys.spt"),ErrorCode::Ok,"root load");
        std::array<nnn::PcScriptSlot,nnn::script_mode_count> saved{};
        saved[0].path="spt/sys.spt";saved[0].pc=37;saved[0].script_call=0;
        saved[1].path="spt/child.spt";saved[1].pc=resume;saved[1].stack={0};
        auto damaged=saved;damaged[1].stack[0]=999;
        const auto before=snapshot(e);
        status_is(e.owner->restore_pc_positions(mode(1),damaged,true,nnn::PcScriptWait::Dialogue,0,false),
            ErrorCode::InvalidFormat,"bad saved SUB ID");
        require(snapshot(e)==before&&e.owner->ActiveMode()==mode(0),"invalid PC frame replaced live VM");
        status_is(e.owner->restore_pc_positions(mode(1),saved,true,nnn::PcScriptWait::Dialogue,0,false),
            ErrorCode::Ok,"saved SCRIPT/SUB");
        require(e.vm(1).GetCallDepth()==1&&e.vm(1).GetPC()==resume&&e.vm(0).GetPC()==37,"restored stack or PC");
        const auto result=e.owner->Exec();
        status_is(result.status,ErrorCode::Ok,"resumed execution");
        require(result.return_code==print_code&&result.transitions==1&&result.active_mode==mode(0)&&
            e.vm(1).GetCallDepth()==0&&e.vm(0).GetPC()==43,"SUB RETURN and SCRIPT NEXT did not resume the caller");
    });
    suite.test("API bounds, valid empty slots and closed initial state",[]{Environment e;require(nnn::script_mode_count==4&&nnn::max_scheduler_transitions==1024&&static_cast<int>(ErrorCode::HostRejected)==13,"old bounds/error ordinal");require(!e.owner->IsLoaded()&&e.owner->ActiveMode()==mode(0)&&e.owner->GetVM(static_cast<ScriptMode>(255))==nullptr,"initial owner state");for(std::size_t m=0;m<4;++m){Word out=123;require(!e.vm(m).IsLoaded()&&!e.vm(m).GetScriptCallSlot(out)&&out==123,"unloaded slot/failed out");}status_is(e.owner->Step().status,ErrorCode::NotLoaded,"empty Step");status_is(e.owner->Exec().status,ErrorCode::NotLoaded,"empty Exec");});
    suite.test("fixed8-step source-shaped chain one layer per NEXT",[]{
        Environment e;chain(e);
        struct Expected {Word code;std::size_t executed,active;Word pc;std::array<Word,4> pcs;std::array<std::size_t,4> depths;std::size_t events;};
        const std::array<Expected,8> expected{{
            {script_code,0,1,56,{56,0,0,0},{1,0,0,0},2},
            {subscript_code,1,2,50,{56,50,0,0},{1,1,0,0},4},
            {subscript_code,2,3,50,{56,50,50,0},{1,1,1,0},6},
            {next_code,3,2,45,{56,50,50,45},{1,1,1,1},7},
            {next_code,2,1,37,{56,50,37,45},{1,1,0,1},7},
            {next_code,1,0,37,{56,37,37,45},{1,0,0,1},7},
            {print_code,0,0,43,{43,37,37,45},{0,0,0,1},8},
            {end_code,0,0,43,{43,37,37,45},{0,0,0,1},8}}};
        for(std::size_t step=0;step<expected.size();++step){const auto result=e.step();const auto& wanted=expected[step];require(result.return_code==wanted.code&&index(result.executed_mode)==wanted.executed&&index(result.active_mode)==wanted.active&&result.pc==wanted.pc,"fixed result/PC/mode");require(result.transitions==(step<6?1U:0U),"fixed transition count");require(e.host.events.size()==wanted.events,"fixed callback count");for(std::size_t m=0;m<4;++m){require(e.vm(m).GetPC()==wanted.pcs[m]&&e.vm(m).GetCallDepth()==wanted.depths[m],"fixed slot PC/depth");if(wanted.depths[m])require(frames(e.vm(m))==std::vector<std::pair<Word,Word>>{{37,0}},"SUB return frame lost");Word slot=999;const bool valid=e.vm(m).GetScriptCallSlot(slot);require(valid==(m<3&&step>=m)&&slot==(valid?0U:999U),"independent slot253");}}
        const std::vector<std::string> reads{"spt/main.spt","spt/_sub.spt","spt/__sub.spt"};require(e.requests==reads,"raw child Reader paths");
        const std::array<std::size_t,8> modes{0,0,1,1,2,2,3,0};const std::array<Word,8> pcs{46,51,40,45,40,45,40,37};const std::array<std::string,8> texts{"Control","main","Main","_sub","Sub","__sub","Deep","M"};
        for(std::size_t i=0;i<8;++i){const auto& event=e.host.events[i];require(event.mode==modes[i]&&event.pc==pcs[i]&&event.text==texts[i]&&event.p3==0,"fixed callback current mode/PC/raw text");if(event.code==script_code||event.code==subscript_code)require(event.p1==0,"source para1 must be zero");}
    });
    suite.test("Exec drains six transitions until parent PRINT without resetting SUB",[]{Environment e;chain(e);const auto r=e.owner->Exec(100000,64);status_is(r.status,ErrorCode::Ok,"drain");require(r.return_code==print_code&&r.transitions==6&&r.executed_mode==mode(0)&&r.active_mode==mode(0)&&r.pc==43&&e.vm(3).GetPC()==45&&e.vm(3).GetCallDepth()==1,"drain result");});
    suite.test("transition limit after push reports last executed parent",[]{Environment e;e.files["spt/child.spt"]=script(leaf());e.load(caller());const auto r=e.owner->Exec(100000,1);status_is(r.status,ErrorCode::BudgetExhausted,"push transition limit");require(r.transitions==1&&r.executed_mode==mode(0)&&r.active_mode==mode(1)&&r.pc==37&&e.vm(1).GetPC()==0,"push limit metadata");});
    suite.test("transition limit after NEXT reports child PC and untouched parent",[]{Environment e;depth_three(e);const auto r=e.owner->Exec(100000,1);status_is(r.status,ErrorCode::BudgetExhausted,"pop transition limit");require(r.transitions==1&&r.executed_mode==mode(3)&&r.active_mode==mode(2)&&r.pc==45&&e.vm(2).GetPC()==50&&e.vm(2).GetCallDepth()==1,"pop limit metadata/parent frame");});
    suite.test("budget truncation permits continuation at current active VM",[]{Environment e;chain(e);for(std::size_t i=0;i<6;++i){const auto r=e.owner->Exec(100000,1);status_is(r.status,ErrorCode::BudgetExhausted,"continued transition");require(r.transitions==1,"continuation skipped/merged transitions");}const auto r=e.owner->Exec();status_is(r.status,ErrorCode::Ok,"continue PRINT");require(r.return_code==print_code&&e.requests.size()==3,"transition replayed");});
    for(const std::size_t limit:{0U,1025U})suite.test("invalid transition budget before VM "+std::to_string(limit),[limit]{Environment e;e.load(caller());const auto before=snapshot(e);const auto r=e.owner->Exec(100000,limit);status_is(r.status,limit==0?ErrorCode::BudgetExhausted:ErrorCode::InvalidFormat,"invalid limit");require(r.work_used==0&&r.transitions==0&&snapshot(e)==before&&e.requests.empty(),"invalid limit entered VM");});
    for(const auto code:{script_code,subscript_code})for(const bool labels:{false,true})suite.test("both table profiles admit bound call "+std::to_string(code)+(labels?" labels":" legacy"),[code,labels]{Environment e;e.files["spt/child.spt"]=script(leaf());auto m=caller(code);m.labels=labels;e.load(m);const auto r=e.step();require(r.return_code==code&&r.active_mode==mode(1)&&e.vm(0).HasForegroundControls(),"call/profile");});
    for(const auto code:{next_code,script_code,subscript_code})suite.test("bare default Closed rejects new control "+std::to_string(code),[code]{Host host;nnn::CExecScript vm(host);host.bare=&vm;const auto m=code==next_code?leaf():caller(code);status_is(vm.LoadBytes(script(m)),ErrorCode::Ok,"Closed load");const auto r=vm.Exec();status_is(r.status,ErrorCode::Ok,"Closed dispatch");require(r.return_code==code&&r.pc==(code==next_code?32U:37U)&&host.events.size()==(code==next_code?0U:1U),"Closed dispatch/PC");});
    suite.test("explicit naked foreground NEXT yields same PC repeatedly",[]{Host h;nnn::CExecScript vm(h);h.bare=&vm;status_is(vm.LoadBytes(script(leaf()),"next",{},nullptr,ScriptControlProfile::Foreground),ErrorCode::Ok,"naked next load");for(int i=0;i<2;++i){const auto r=vm.Exec(100);status_is(r.status,ErrorCode::Ok,"naked next");require(r.return_code==next_code&&r.pc==32&&h.events.empty(),"naked NEXT advanced");}});
    suite.test("explicit naked foreground call lacks owner and cannot dispatch",[]{Host h;nnn::CExecScript vm(h);h.bare=&vm;status_is(vm.LoadBytes(script(caller()),"call",{},nullptr,ScriptControlProfile::Foreground),ErrorCode::Ok,"naked call load");const auto r=vm.Exec(100);status_is(r.status,ErrorCode::Ok,"naked call");Word slot=19;require(r.return_code==script_code&&r.pc==37&&h.events.size()==1&&h.events[0].text=="child"&&h.events[0].pc==32&&vm.GetScriptCallSlot(slot)&&slot==0,"naked call dispatch");});
    for(const bool use_exec:{false,true})suite.test(std::string("root NEXT bounded rejection ")+(use_exec?"Exec":"Step"),[use_exec]{Environment e;e.load(leaf());for(int n=0;n<2;++n){const auto r=use_exec?e.owner->Exec():e.owner->Step();status_is(r.status,ErrorCode::Ok,"root NEXT");require(r.return_code==next_code&&r.active_mode==mode(0)&&r.pc==32&&r.transitions==0&&e.requests.empty(),"root NEXT progressed");}});
    suite.test("ordinary child END does not pop or restart root",[]{Environment e;e.files["spt/child.spt"]=script(leaf(end_code));e.load(caller());e.step();const auto r=e.owner->Exec();status_is(r.status,ErrorCode::Ok,"child END");require(r.return_code==end_code&&r.active_mode==mode(1)&&r.transitions==0&&e.vm(0).GetPC()==37,"END misused as NEXT");});
    suite.test("ordinary numeric DRAW yield is returned instead of drained",[]{Environment e;Model child;child.code.add({5,0x66660006U,0x22220004U,1,1});child.code.marker(next_code);e.files["spt/child.spt"]=script(child);e.load(caller());const auto r=e.owner->Exec(100000);status_is(r.status,ErrorCode::Ok,"DRAW drain");require(e.host.numerics==1&&r.active_mode==mode(1)&&r.transitions==1&&r.return_code==0x22220004U,"DRAW not external yield");});
    suite.test("owner copies registry and shares true host flags and variables",[]{auto config=registry();Environment e(&config);config=nnn::FunctionRegistry{};Model child;child.code.set(100,23);child.code.function();child.code.marker(next_code);e.files["spt/child.spt"]=script(child);e.host.SetSkipFlags(flags(31));e.host.variables[100]=7;e.host.on_function=[&]{require(mask(e.host.GetSkipFlags())==31&&e.host.variables[100]==23,"child not sharing host state");};e.load(caller());const auto r=e.owner->Exec();status_is(r.status,ErrorCode::Ok,"shared drain");require(r.return_code==print_code&&e.host.variables[100]==23&&e.host.functions==1&&mask(e.host.GetSkipFlags())==0,"production flag action not shared");for(std::size_t m=0;m<4;++m){require(mask(e.vm(m).GetSkipFlags())==0,"slot proxy flags diverged");if(e.vm(m).IsLoaded())require(e.vm(m).HasFunctionRegistry()&&e.vm(m).BoundStopF5Id()==2&&e.vm(m).FunctionRegistrySize()==3,"registry not inherited");}});
    suite.test("invalid owned registry refuses root before Reader",[]{nnn::FunctionRegistry config;Environment e(&config);e.files["root"]=script(leaf());status_is(e.owner->LoadFile("root"),ErrorCode::InvalidFormat,"invalid scheduler registry");require(e.requests.empty()&&!e.owner->IsLoaded(),"invalid registry reached reader");});
    suite.test("unregistered owner retains function rejection",[]{Environment e;Model root;root.code.function();root.code.marker(end_code);e.load(root);const auto r=e.step();require(r.return_code==end_code&&r.pc==36&&e.host.functions==1,"unregistered function dispatch");});
    suite.test("same child name is read anew and replacement resets child frames",[]{Environment e;Model child;child.code.sub(0);child.code.marker(next_code);child.targets={child.code.story()};child.code.marker(next_code);e.files["spt/child.spt"]=script(child);e.load(repeated());e.step();e.step();require(e.vm(1).GetCallDepth()==1,"first child SUB missing");const auto old_words=e.vm(1).WordCount();e.files["spt/child.spt"]=script(leaf());e.step();require(e.requests==std::vector<std::string>{"spt/child.spt","spt/child.spt"}&&e.vm(1).GetPC()==0&&e.vm(1).GetCallDepth()==0&&e.vm(1).WordCount()!=old_words,"same-name cached/reused child");});
    suite.test("custom decoding is propagated to freshly loaded child",[]{Environment e;const nnn::DecodeParameters decode{0x12345678U,0x10203U,0x303U};e.files["spt/child.spt"]=script(leaf(),decode);e.load(caller(),"encoded",decode);const auto r=e.owner->Exec();status_is(r.status,ErrorCode::Ok,"decode child");require(r.return_code==print_code&&r.transitions==2,"child not decoded with launch parameters");});
    suite.test("root LoadFile keeps exact caller key and performs no execution",[]{Environment e;e.files["Dir/Control.X"]=script(caller());status_is(e.owner->LoadFile("Dir/Control.X"),ErrorCode::Ok,"root file load");require(e.requests==std::vector<std::string>{"Dir/Control.X"}&&e.vm(0).Identity()=="Dir/Control.X"&&e.vm(0).GetPC()==0&&e.host.events.empty(),"root load normalized/ran script");});
    suite.test("mode3 call rejected before child Reader and ordinary callback",[]{Environment e;chain(e);e.files["spt/__sub.spt"]=script(caller());for(int i=0;i<3;++i)e.step();const auto reads=e.requests.size();const auto events=e.host.events.size();const auto r=e.owner->Step();status_is(r.status,ErrorCode::HostRejected,"mode3 overflow");Word slot=27;require(r.active_mode==mode(3)&&r.pc==32&&r.transitions==0&&e.requests.size()==reads&&e.host.events.size()==events&&!e.vm(3).GetScriptCallSlot(slot)&&slot==27,"deep call had side effects");});
    for(int kind=0;kind<5;++kind)suite.test("child preparation failure preserves parent and old child "+std::to_string(kind),[kind]{Environment e;e.files["spt/child.spt"]=script(leaf());e.load(repeated());e.step();e.step();const auto child=snapshot(e)[1];e.override_read=[kind](const std::string&)->nnn::ReadResult{if(kind==0)return {{ErrorCode::NotFound,nnn::no_word,"missing"},{},0};if(kind==1)return {{},{0,0,0,0},8};if(kind==2)return {{},{0,0,0,0},4};if(kind==3)throw std::runtime_error("reader exception");throw 3;};const auto events=e.host.events.size();const auto r=e.owner->Step();status_is(r.status,kind==0?ErrorCode::NotFound:kind==2?ErrorCode::InvalidFormat:ErrorCode::ReadFailed,"child failed preparation");require(e.owner->ActiveMode()==mode(0)&&e.vm(0).GetPC()==32&&snapshot(e)[1]==child&&e.host.events.size()==events,"failed child replaced/advanced/dispatched");Word slot=1;require(e.vm(0).GetScriptCallSlot(slot)&&slot==0,"previous call slot lost after failed new call");});
    for(int kind=0;kind<4;++kind)suite.test("failed root launch preserves all four slots "+std::to_string(kind),[kind]{Environment e;depth_three(e);const auto before=snapshot(e);if(kind==0)status_is(e.owner->LoadBytes({0,0,0,0},"bad"),ErrorCode::InvalidFormat,"bad root bytes");else{e.override_read=[kind](const std::string&)->nnn::ReadResult{if(kind==1)return {{ErrorCode::NotFound,nnn::no_word,"missing"},{},0};if(kind==2)return {{},{0,0,0,0},8};throw std::runtime_error("root reader");};const auto result=e.owner->LoadFile("badroot");status_is(result,kind==1?ErrorCode::NotFound:ErrorCode::ReadFailed,"failed root file");}require(snapshot(e)==before&&e.owner->ActiveMode()==mode(3),"failed root changed four-slot state");});
    suite.test("successful root relaunch resets every slot but keeps host flags",[]{Environment e;depth_three(e);e.host.SetSkipFlags(flags(29));e.load(leaf(end_code),"fresh");require(e.owner->ActiveMode()==mode(0)&&e.vm(0).GetPC()==0&&e.vm(0).Identity()=="fresh"&&mask(e.host.GetSkipFlags())==29,"root reset/flags");for(std::size_t m=0;m<4;++m){Word slot=999;require(e.vm(m).GetCallDepth()==0&&!e.vm(m).GetScriptCallSlot(slot)&&slot==999,"root reset call state");if(m)require(!e.vm(m).IsLoaded(),"old child remains installed");}});
    for(const bool side_effect:{false,true})suite.test(std::string("throwing host rejects prepared candidate ")+(side_effect?"explicit side effect":"unchanged flags"),[side_effect]{Environment e;e.files["spt/child.spt"]=script(leaf());e.load(caller());e.host.SetSkipFlags(flags(31));const auto before=snapshot(e);e.host.on_script=[&](Word){if(side_effect)e.host.SetSkipFlags(flags(7));throw std::runtime_error("host script");};const auto r=e.owner->Step();status_is(r.status,ErrorCode::CallbackFailed,"host transfer exception");require(e.owner->ActiveMode()==mode(0)&&e.vm(0).GetPC()==32&&!e.vm(1).IsLoaded()&&snapshot(e)[1]==before[1]&&mask(e.host.GetSkipFlags())==(side_effect?7U:31U),"throw committed candidate or rolled back host");Word slot=5;require(!e.vm(0).GetScriptCallSlot(slot)&&slot==5,"throw committed slot253");});
    suite.test("Reader mutation survives failed child load",[]{Environment e;e.load(caller());e.on_read=[&](const std::string&){e.host.SetSkipFlags(flags(11));e.host.variables[100]=91;};const auto r=e.owner->Step();status_is(r.status,ErrorCode::NotFound,"reader side effect");require(e.vm(0).GetPC()==32&&mask(e.host.GetSkipFlags())==11&&e.host.variables[100]==91,"Reader side effect rolled back");});
    for(int where=0;where<3;++where)suite.test("owner reentry Busy during reader or callback "+std::to_string(where),[where]{Environment e;e.files["root"]=script(caller());e.files["spt/child.spt"]=script(leaf());int invoked=0;if(where<2)e.on_read=[&](const std::string& name){if((where==0&&name=="root")||(where==1&&name=="spt/child.spt")){++invoked;busy_checks(e);}};else e.host.on_script=[&](Word code){if(code==script_code){++invoked;require(e.owner->ActiveMode()==mode(0)&&e.vm(0).GetPC()==32&&!e.vm(1).IsLoaded(),"host callback not precommit parent");busy_checks(e);}};status_is(e.owner->LoadFile("root"),ErrorCode::Ok,"outer root");e.step();require(invoked==1&&e.owner->ActiveMode()==mode(1),"Busy outer operation failed");});
    for(std::uint64_t budget=0;budget<=9;++budget)suite.test("SCRIPT work paid before Reader and host "+std::to_string(budget),[budget]{Environment e;e.files["spt/child.spt"]=script(leaf());e.load(caller());const auto r=e.owner->Step(budget);if(budget<6){status_is(r.status,ErrorCode::BudgetExhausted,"call budget");require(r.work_used==(budget==0?0U:1U)&&e.requests.empty()&&e.host.events.empty()&&e.owner->ActiveMode()==mode(0)&&e.vm(0).GetPC()==(budget==0?0U:32U),"budget entered owner or host");Word slot=7;require(!e.vm(0).GetScriptCallSlot(slot)&&slot==7,"budget committed slot");}else{status_is(r.status,ErrorCode::Ok,"exact call budget");require(r.work_used==6&&r.active_mode==mode(1)&&e.vm(0).GetPC()==37,"exact work boundary");}});
    for(const std::uint64_t budget:{9U,10U,11U})suite.test("shared Exec budget across push and pop "+std::to_string(budget),[budget]{Environment e;e.files["spt/child.spt"]=script(leaf());e.load(repeated());const auto r=e.owner->Exec(budget,64);status_is(r.status,ErrorCode::BudgetExhausted,"shared work");require(r.work_used==(budget==9?7U:10U)&&r.transitions==(budget==9?1U:2U),"per-VM budget reset");if(budget==9)require(r.executed_mode==mode(1)&&r.pc==32&&r.active_mode==mode(1),"child partial work");if(budget==10)require(r.executed_mode==mode(1)&&r.pc==32&&r.active_mode==mode(0)&&e.vm(0).GetPC()==37,"work NEXT metadata");if(budget==11)require(r.executed_mode==mode(0)&&r.pc==37&&r.active_mode==mode(0),"parent partial GOTO");});
    suite.test("finite transition bound stops repeated parent-child loop",[]{Environment e;e.files["spt/child.spt"]=script(leaf());e.load(repeated());const auto r=e.owner->Exec(1000000,1024);status_is(r.status,ErrorCode::BudgetExhausted,"bounded loop");require(r.transitions==1024&&r.active_mode==mode(0)&&e.requests.size()==512,"loop not bounded one push/pop each");});
    const std::vector<std::string> bad_names{"",".","..","a/b","a\\b","a:b",std::string("a\x01",2),std::string("a\x7f",2),std::string(128,'x')};
    for(std::size_t n=0;n<bad_names.size();++n)suite.test("target basename rejection before Reader "+std::to_string(n),[&,n]{Environment e;e.load(caller(script_code,bad_names[n]));const auto r=e.owner->Step();status_is(r.status,ErrorCode::HostRejected,"bad basename");require(e.requests.empty()&&e.host.events.empty()&&e.vm(0).GetPC()==32&&!e.vm(1).IsLoaded(),"bad target side effect");});
    for(const std::string& name:std::vector<std::string>{"MiXeD",std::string(127,'x'),std::string("A\x80\xff",3),"a..b"})suite.test("target preserves allowed raw bytes "+std::to_string(name.size()),[name]{Environment e;const std::string key="spt/"+name+".spt";e.files[key]=script(leaf());e.load(caller(script_code,name));e.step();require(e.requests==std::vector<std::string>{key}&&e.host.events.size()==1&&e.host.events[0].text==name,"target transformed");});
    for(const Word length:{4U,5U,6U})suite.test("NEXT exact record length "+std::to_string(length),[length]{Model m;m.code.add({length,control,next_code});for(Word i=3;i<length;++i)m.code.words.push_back(0);m.code.marker(end_code);foreground_reject(m,ErrorCode::InvalidFormat);});
    for(const Word length:{3U,4U,6U})suite.test("SCRIPT exact record length "+std::to_string(length),[length]{Model m;const Word at=m.code.pc();m.code.add({length,control,script_code});for(Word i=3;i<length;++i)m.code.words.push_back(0);m.code.script_sites={at};m.code.marker(end_code);foreground_reject(m,ErrorCode::InvalidFormat);});
    for(const Word slot:{1U,UINT32_MAX})suite.test("slot0-only explicit narrow profile "+std::to_string(slot),[slot]{auto m=caller();m.code.words[4]=slot;Host h;nnn::CExecScript vm(h);h.bare=&vm;status_is(vm.LoadBytes(script(m),"slot",{},nullptr,ScriptControlProfile::Foreground),ErrorCode::Ok,"slot load");const auto r=vm.Exec();Word actual=0;require(r.status.code==ErrorCode::Ok&&r.return_code==script_code&&r.pc==37&&vm.GetScriptCallSlot(actual)&&actual==slot&&h.events.size()==1&&h.events[0].text=="child","slot dispatch");});
    for(int bad=0;bad<4;++bad)suite.test("SCRIPT registry exact mapping "+std::to_string(bad),[bad]{auto m=caller();if(bad==0)m.code.script_sites.clear();if(bad==1)m.code.script_sites={33};if(bad==2)m.code.script_sites={32,32};if(bad==3){m=leaf();m.code.script_sites={32};}Host h;nnn::CExecScript vm(h);h.bare=&vm;status_is(vm.LoadBytes(script(m),"adjust",{},nullptr,ScriptControlProfile::Foreground),ErrorCode::Ok,"adjust load");const auto r=vm.Exec();require(r.status.code==ErrorCode::Ok&&r.return_code==(bad==3?next_code:script_code)&&r.pc==(bad==3?32U:37U)&&h.events.size()==(bad==3?0U:1U),"adjust dispatch");});
    suite.test("multiple callsites load and first dispatches",[]{auto m=caller();m.code.call(subscript_code);m.code.marker(end_code);Host h;nnn::CExecScript vm(h);h.bare=&vm;status_is(vm.LoadBytes(script(m)),ErrorCode::Ok,"multiple calls");const auto r=vm.Exec();require(r.status.code==ErrorCode::Ok&&r.return_code==script_code&&r.pc==37&&h.events.size()==1,"first call dispatch");});
    suite.test("SCRIPT raw STR index bounded",[]{auto m=caller();m.code.words[3]=UINT32_MAX;foreground_reject(m,ErrorCode::InvalidFormat);});
    suite.test("SCRIPT requires executable fallthrough",[]{Model m;m.code.call();Host h;nnn::CExecScript vm(h);h.bare=&vm;status_is(vm.LoadBytes(script(m)),ErrorCode::Ok,"call fallthrough");const auto r=vm.Exec();require(r.status.code==ErrorCode::Ok&&r.return_code==script_code&&r.pc==37&&h.events.size()==1,"fallthrough call dispatch");});
    for(const Word code:{0x55560015U,0x5556000bU})suite.test("invalid cross-script/exit control lengths are rejected "+std::to_string(code),[code]{Model m;m.code.add({5,control,code,0,0});m.code.marker(end_code);foreground_reject(m,ErrorCode::InvalidFormat);});
    suite.test("unreachable unknown instruction after NEXT loads without dispatch",[]{auto m=leaf();m.code.add({3,0xdeadbeefU,0});Host h;nnn::CExecScript vm(h);h.bare=&vm;status_is(vm.LoadBytes(script(m)),ErrorCode::Ok,"unreachable load");const auto r=vm.Exec();require(r.status.code==ErrorCode::Ok&&r.return_code==next_code&&r.pc==32&&h.events.empty(),"unreachable instruction ran");});
    suite.test("unknown profile rejected before Reader preserves old program",[]{Host host;nnn::CExecScript vm(host);status_is(vm.LoadBytes(script(leaf(end_code)),"kept"),ErrorCode::Ok,"old bare");int reads=0;status_is(vm.LoadFile("new",{},[&](const std::string&){++reads;return nnn::ReadResult{};},nullptr,static_cast<ScriptControlProfile>(99)),ErrorCode::InvalidFormat,"unknown profile");require(reads==0&&vm.Identity()=="kept"&&!vm.HasForegroundControls(),"bad profile Reader/state");});
    suite.test("failed bare load preserves foreground, SUB stack and slot253",[]{Host host;nnn::CExecScript vm(host);status_is(vm.LoadBytes(file("__sub.spt"),"kept",{},nullptr,ScriptControlProfile::Foreground),ErrorCode::Ok,"foreground SUB");auto r=vm.Exec();status_is(r.status,ErrorCode::Ok,"foreground yield");const auto stack=frames(vm);status_is(vm.LoadBytes({0,0,0,0},"bad"),ErrorCode::InvalidFormat,"failed reload");Word out=91;require(vm.Identity()=="kept"&&vm.GetPC()==45&&frames(vm)==stack&&vm.HasForegroundControls()&&!vm.GetScriptCallSlot(out)&&out==91,"failed reload lost prior state");});
    suite.test("nonstandard host exception discards candidate",[]{Environment e;e.files["spt/child.spt"]=script(leaf());e.load(caller());e.host.on_script=[](Word){throw 7;};const auto r=e.owner->Step();status_is(r.status,ErrorCode::CallbackFailed,"nonstandard host exception");require(e.owner->ActiveMode()==mode(0)&&e.vm(0).GetPC()==32&&!e.vm(1).IsLoaded(),"nonstandard exception committed");});
    suite.test("bare foreground preflight only reads root and never follows target",[]{Host host;nnn::CExecScript vm(host);int reads=0;status_is(vm.LoadFile("one/root",{},[&](const std::string& name){require(name=="one/root","preflight followed child");++reads;const auto bytes=script(caller());return nnn::ReadResult{{},bytes,bytes.size()};},nullptr,ScriptControlProfile::Foreground),ErrorCode::Ok,"preflight");require(reads==1&&vm.GetPC()==0&&host.events.empty(),"preflight executed");});
    for(const bool command:{false,true})suite.test(command?"COMMAND adjustment permits dispatch":"LABEL adjustment permits dispatch",[command]{auto m=caller();if(command)m.command_adjust={32};else m.label_adjust={32};Host h;nnn::CExecScript vm(h);h.bare=&vm;status_is(vm.LoadBytes(script(m)),ErrorCode::Ok,"adjustment load");const auto r=vm.Exec();require(r.status.code==ErrorCode::Ok&&r.return_code==script_code&&r.pc==37&&h.events.size()==1&&h.events[0].text=="child","adjustment dispatch");});
    suite.test("profile and registry are captured before root Reader",[]{Host host;nnn::CExecScript vm(host);auto config=registry();ScriptControlProfile profile=ScriptControlProfile::Foreground;status_is(vm.LoadFile("captured",{},[&](const std::string&){profile=ScriptControlProfile::Closed;config=nnn::FunctionRegistry{};const auto bytes=script(caller());return nnn::ReadResult{{},bytes,bytes.size()};},&config,profile),ErrorCode::Ok,"captured profile");require(vm.HasForegroundControls()&&vm.HasFunctionRegistry()&&vm.BoundStopF5Id()==2&&vm.FunctionRegistrySize()==3,"Reader changed entry profile/registry");});
    suite.test("oversized child rejected before callback or commit",[]{Environment e;e.files["spt/child.spt"]=std::vector<Byte>(nnn::max_script_bytes+4U,0);e.load(caller());const auto r=e.owner->Step();status_is(r.status,ErrorCode::InvalidFormat,"large child");require(e.vm(0).GetPC()==32&&!e.vm(1).IsLoaded()&&e.host.events.empty(),"large child committed");});
    suite.test("successful Closed reload removes opt-in and resets slot validity",[]{Host host;nnn::CExecScript vm(host);status_is(vm.LoadBytes(script(leaf()),"fg",{},nullptr,ScriptControlProfile::Foreground),ErrorCode::Ok,"foreground");status_is(vm.LoadBytes(script(leaf(end_code)),"closed"),ErrorCode::Ok,"Closed replacement");Word slot=123;require(!vm.HasForegroundControls()&&vm.GetPC()==0&&!vm.GetScriptCallSlot(slot)&&slot==123,"reload retained profile/callslot");});
    for(const std::uint64_t budget:{0U,1U,2U,3U,4U})suite.test("root NEXT exact work boundary "+std::to_string(budget),[budget]{Environment e;e.load(leaf());const auto r=e.owner->Step(budget);status_is(r.status,budget<4?ErrorCode::BudgetExhausted:ErrorCode::Ok,"NEXT work");require(r.work_used==(budget==0?0U:budget<4?1U:4U)&&e.owner->ActiveMode()==mode(0)&&r.transitions==0&&e.vm(0).GetPC()==(budget==0?0U:32U)&&e.host.events.empty(),"NEXT budget advanced");if(budget==4)require(r.return_code==next_code,"NEXT yield");});
    suite.test("default root file Reader does not authorize implicit child file IO",[]{Host host;ScriptScheduler owner(host);host.owner=&owner;const auto path=std::filesystem::absolute(std::filesystem::path(NNN_SCHEDULER_FIXTURE_DIR)/"control.spt");status_is(owner.LoadFile(path.string()),ErrorCode::Ok,"default root file load");require(host.events.empty()&&owner.GetVM(mode(0))->GetPC()==0,"root load executed");for(int retry=0;retry<2;++retry){const auto r=owner.Step(10000);status_is(r.status,ErrorCode::HostRejected,"missing explicit child Reader");const auto* parent=owner.GetVM(mode(0));Word slot=77;require(r.executed_mode==mode(0)&&r.active_mode==mode(0)&&r.transitions==0&&parent->GetPC()==51&&frames(*parent)==std::vector<std::pair<Word,Word>>{{37,0}}&&!parent->GetScriptCallSlot(slot)&&slot==77&&!owner.GetVM(mode(1))->IsLoaded(),"missing Reader changed parent/child/slot");require(host.events.size()==1&&host.events[0].code==0x33330051U&&host.events[0].pc==46,"missing Reader dispatched SCRIPT or replayed STORY");}});
    std::cout<<"SCHEDULER RESULT "<<suite.passed<<" passed, "<<suite.failed<<" failed\n";return suite.failed==0?0:1;
}
