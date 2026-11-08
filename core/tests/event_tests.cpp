#include "nnn/script.hpp"
#include "events/fixtures/generated_cases.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
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
constexpr Word data=0x66660001U,control=0x66660002U,system_command=0x66660006U,system_function=0x66660007U;
constexpr Word end_code=0x55560005U,print_code=0x22220001U,draw_code=0x22220004U,overrap_code=0x22220005U;
constexpr Word story_code=0x33330051U,film_code=0x33330050U,load_code=0x33330015U,preload_code=0x3333001fU;
constexpr Word effect_code=0x33330013U,rect_code=0x33330016U,clear_layer_code=0x33330014U,clear_all_code=0x33330012U;
constexpr Word koma_code=0x3333001cU,message_code=0x3333001bU;

void require(bool condition,const std::string& text) {if(!condition)throw std::runtime_error(text);}
void status_is(const nnn::Status& actual,ErrorCode expected,const std::string& context) {
    require(actual.code==expected,context+": expected "+nnn::error_name(expected)+", got "+nnn::error_name(actual.code)+" ("+actual.detail+")");
}
std::string hex(const std::string& bytes) {
    constexpr char digits[]="0123456789abcdef";std::string result;
    for(const char value:bytes){const auto byte=static_cast<unsigned char>(value);result+=digits[byte>>4U];result+=digits[byte&15U];}
    return result;
}
struct State{Word pc;std::vector<nnn::CallFrame> frames;};
std::vector<nnn::CallFrame> active_frames(const nnn::CExecScript& vm) {
    std::vector<nnn::CallFrame> result;
    for(std::size_t i=0;i<vm.GetCallDepth();++i){nnn::CallFrame frame{};require(vm.GetCallFrame(i,frame),"missing active frame");result.push_back(frame);}
    return result;
}
struct Callback:nnn::CScriptCallBack {
    nnn::CExecScript* vm=nullptr;
    std::map<Word,Value> variables;
    std::vector<std::string> events;
    std::vector<State> states;
    std::vector<std::string> retained_text;
    std::vector<std::tuple<Word,Word,Word>> skipped;
    void UnhandledInstruction(Word pc,Word identify,Word code)override{skipped.emplace_back(pc,identify,code);}
    std::function<void(Word)> on_text;
    void observe(){require(vm!=nullptr,"callback VM missing");states.push_back({vm->GetPC(),active_frames(*vm)});}
    void ReceiveScriptCommand(Word code,Value p1,const std::string& text,Value p3)override{
        observe();retained_text.push_back(text);
        events.push_back("{\"kind\":\"script\",\"code\":"+std::to_string(code)+",\"para1\":"+std::to_string(p1)+
            ",\"text_hex\":\""+hex(text)+"\",\"para3\":"+std::to_string(p3)+'}');
        if(on_text)on_text(code);
    }
    void PreReceiveScriptData(Word,Value,const std::string&)override{throw std::runtime_error("unexpected predata");}
    void ReceiveUserCommand0(Word code,const std::vector<Value>& values)override{
        observe();std::string event="{\"kind\":\"user_command\",\"code\":"+std::to_string(code)+",\"parameters\":[";
        for(std::size_t i=0;i<values.size();++i){if(i)event+=',';event+=std::to_string(values[i]);}events.push_back(event+"]}");
    }
    void ReceiveUserFunction0(Word,const std::vector<Value>&)override{throw std::runtime_error("unexpected user function");}
    Value variable(const char* kind,Word index){observe();const Value value=variables[index];events.push_back("{\"kind\":\""+std::string(kind)+"\",\"index\":"+std::to_string(index)+",\"value\":"+std::to_string(value)+'}');return value;}
    Value GetVarData(Word index)override{return variable("get_var",index);}
    Value GetCalcuVarData(Word index)override{return variable("get_calcu_var",index);}
    void SetVarData(Word index,Value value)override{observe();variables[index]=value;events.push_back("{\"kind\":\"set_var\",\"index\":"+std::to_string(index)+",\"value\":"+std::to_string(value)+'}');}
    // Deliberately inherits the additive numeric method's rejecting default.
};
struct NumericCallback final:Callback {
    bool accept=true;
    std::vector<std::vector<Value>> retained_parameters;
    std::function<void(Word,const std::vector<Value>&)> on_numeric;
    bool ReceiveNumericScriptCommand(Word code,Value p1,const std::vector<Value>& values,Value p3)override{
        observe();retained_parameters.push_back(values);
        std::string event="{\"kind\":\"numeric_script\",\"code\":"+std::to_string(code)+",\"para1\":"+std::to_string(p1)+",\"parameters\":[";
        for(std::size_t i=0;i<values.size();++i){if(i)event+=',';event+=std::to_string(values[i]);}
        events.push_back(event+"],\"para3\":"+std::to_string(p3)+'}');
        if(on_numeric)on_numeric(code,values);
        return accept;
    }
};
struct Code{
    std::vector<Word> words;
    Word pc()const{return 32U+static_cast<Word>(words.size());}
    Word add(std::initializer_list<Word> values){const Word start=pc();words.insert(words.end(),values);return start;}
    Word end(){return add({3,control,end_code});}
    Word story(Word index=0){return add({5,system_function,story_code,1,index});}
    Word print(Word index=0){return add({6,system_command,print_code,index,0,0});}
    Word filename(Word code,Word layer=0,Word index=0){return add({5,system_function,code,layer,index});}
    Word numeric(Word code,const std::vector<Word>& values){
        const Word start=pc();words.insert(words.end(),{4U+static_cast<Word>(values.size()),code==draw_code||code==overrap_code?system_command:system_function,code,static_cast<Word>(values.size())});
        words.insert(words.end(),values.begin(),values.end());return start;
    }
};
struct Model{
    Code code;
    bool labels=true;
    Byte padding=0;
    std::vector<std::string> messages{"M"},strings{"S"};
    std::vector<Word> main_labels,film_offsets,koma_targets;
    Word story_count=0;
};
std::vector<Word> program_words(const Model& model){
    std::vector<Word> words(32,0);words[0]=32;words[1]=data;words[2]=0x55550001U;
    words[12]=0x48545053U;words[13]=0x45444145U;words[14]=0x20203052U;words[15]=0x00202020U;
    words.insert(words.end(),model.code.words.begin(),model.code.words.end());
    const auto table=[&](Word kind,Word field,const std::vector<Word>& payload,bool record=false){words[field]=static_cast<Word>(words.size())+(record?0U:4U);words.insert(words.end(),{4U+static_cast<Word>(payload.size()),data,0x55550002U,kind});words.insert(words.end(),payload.begin(),payload.end());};
    if(model.labels){words[8]=static_cast<Word>(model.main_labels.size());words[10]=model.story_count;table(0x44440003U,9,model.main_labels);table(0x44440003U,16,model.film_offsets,true);table(0x44440003U,17,model.koma_targets,true);}
    const auto text_table=[&](Word kind,Word field,const std::vector<std::string>& entries){
        const Word begin=static_cast<Word>(words.size())+4U;std::vector<Word> payload(entries.size(),0);
        for(std::size_t i=0;i<entries.size();++i){
            payload[i]=begin+static_cast<Word>(payload.size());std::vector<Byte> bytes;
            for(const char ch:entries[i])bytes.push_back(static_cast<Byte>(static_cast<unsigned char>(ch)));
            bytes.push_back(0);while(bytes.size()%4U)bytes.push_back(model.padding);
            for(std::size_t b=0;b<bytes.size();b+=4U)payload.push_back(static_cast<Word>(bytes[b])|(static_cast<Word>(bytes[b+1U])<<8U)|(static_cast<Word>(bytes[b+2U])<<16U)|(static_cast<Word>(bytes[b+3U])<<24U));
        }
        table(kind,field,payload);
    };
    words[4]=static_cast<Word>(model.messages.size());words[6]=static_cast<Word>(model.strings.size());
    text_table(0x44440002U,5,model.messages);text_table(0x44440001U,7,model.strings);
    table(0x44440004U,18,{});table(0x44440005U,20,{});table(0x44440006U,21,{});table(0x44440007U,22,{});table(0x44440008U,23,{});return words;
}
std::vector<Byte> encrypted(const std::vector<Word>& words){std::vector<Byte> bytes;for(Word word:words){word^=UINT32_MAX;for(unsigned shift=0;shift<32;shift+=8)bytes.push_back(static_cast<Byte>((word>>shift)&255U));}return bytes;}
void load(nnn::CExecScript& vm,const Model& model,const std::string& identity="events"){status_is(vm.LoadBytes(encrypted(program_words(model)),identity),ErrorCode::Ok,"event load");}
nnn::ExecResult exec(nnn::CExecScript& vm,Word code,std::uint64_t budget=100000){const auto actual=vm.Exec(budget);status_is(actual.status,ErrorCode::Ok,"event Exec");require(actual.return_code==code,"event return code mismatch");return actual;}
void reject(const std::vector<Word>& words,ErrorCode error=ErrorCode::InvalidFormat){Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;status_is(vm.LoadBytes(encrypted(words)),error,"event rejected load");require(!vm.IsLoaded()&&cb.events.empty(),"bad event installed/called host");}
Model event_model(Word code,const std::vector<Word>& values){Model m;m.code.numeric(code,values);m.code.end();return m;}
struct Suite{int passed=0,failed=0;void test(const std::string& name,const std::function<void()>& body){try{body();++passed;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& error){++failed;std::cerr<<"FAIL "<<name<<": "<<error.what()<<'\n';}}};
} // namespace

int main(){
    Suite suite;
    suite.test("additive API and old error ordinal contract",[]{
        require(nnn::max_numeric_event_parameters==258U,"numeric bound");
        require(static_cast<int>(ErrorCode::Busy)==10&&static_cast<int>(ErrorCode::CallStackUnderflow)==11&&static_cast<int>(ErrorCode::CallStackOverflow)==12&&static_cast<int>(ErrorCode::HostRejected)==13,"error ordinal changed");
        require(std::string(nnn::error_name(ErrorCode::HostRejected))=="HostRejected","host rejection name");
    });
    for(const std::string& text:{std::string(),std::string("A"),std::string("AB"),std::string("ABC"),std::string("ABCD"),std::string("\x82\xa0",2)})
        for(const bool message:{false,true})suite.test("labels preserve bytes and ignore only alignment padding "+hex(text)+(message?" message":" string"),[text,message]{
            Model m;m.padding=0xff;m.messages={text};m.strings={text};if(message)m.code.print();else m.code.story();m.code.end();
            Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);exec(vm,message?print_code:end_code);
            require(cb.retained_text==std::vector<std::string>{text},"padding entered callback or text transcoded");
        });
    suite.test("nonzero pads between entries and in final entry",[]{
        Model m;m.padding=0x81;m.strings={"","A","AB","ABC",std::string("\xfe\x80",2)};
        for(Word i=0;i<5;++i)m.code.story(i);
        m.code.end();Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);exec(vm,end_code);
        require(cb.retained_text==m.strings,"text entries/pads crossed");
    });
    for(const bool labels:{false,true})suite.test(std::string("canonical zero padding stays valid ")+(labels?"labels":"legacy"),[labels]{
        Model m;m.labels=labels;m.code.story();m.code.print();m.code.end();Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);exec(vm,print_code);exec(vm,end_code);
    });
    for(const Byte pad:{Byte{1},Byte{0x80},Byte{0xff}})suite.test("legacy accepts nonzero alignment pad "+std::to_string(pad),[pad]{
        Model m;m.labels=false;m.padding=pad;m.code.story();m.code.end();Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;
        load(vm,m);exec(vm,end_code);require(cb.retained_text==std::vector<std::string>{"S"},"padding changed string");
    });
    struct TextBad{const char* name;std::function<void(std::vector<Word>&)> mutate;};
    const std::vector<TextBad> text_bad{
        {"missing last NUL",[](auto& w){w[w[w[7]+1U]]=0xffffffffU;}},
        {"first pointer before payload",[](auto& w){w[w[7]]=w[7];}},
        {"first pointer outside table",[](auto& w){w[w[7]]=w[18]-4U;}},
        {"first pointer UINT32_MAX",[](auto& w){w[w[7]]=UINT32_MAX;}},
        {"second pointer overlaps first",[](auto& w){w[w[7]+1U]=w[w[7]];}},
        {"second pointer skips aligned word",[](auto& w){++w[w[7]+1U];}},
        {"second pointer out of table",[](auto& w){w[w[7]+1U]=static_cast<Word>(w.size());}},
        {"first string can consume next entry",[](auto& w){w[w[w[7]]]=0xffffffffU;}},
        {"impossible text count UINT32_MAX",[](auto& w){w[6]=UINT32_MAX;}},
    };
    for(const auto& item:text_bad)suite.test(item.name,[&]{
        Model m;m.padding=0xee;m.strings={"A","B"};m.code.end();auto words=program_words(m);item.mutate(words);
        const std::string name=item.name;
        if(name=="second pointer overlaps first" || name=="first string can consume next entry") {
            Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;status_is(vm.LoadBytes(encrypted(words)),ErrorCode::Ok,"pointer text load");
            std::string first,second;require(vm.GetString(0,first)&&vm.GetString(1,second),"pointer text access");
            require(first==(name=="second pointer overlaps first" ? "A" : std::string(4,static_cast<char>(0xff))+"B") &&
                    second==(name=="second pointer overlaps first" ? "A" : "B"),"pointer text value");
        } else reject(words);
    });
    suite.test("extra whole padding word is accepted",[]{
        Model m;m.padding=0xee;m.code.end();auto words=program_words(m);const Word end=words[18]-4U;
        ++words[words[7]-4U];words.insert(words.begin()+static_cast<std::ptrdiff_t>(end),0xffffffffU);
        for(const Word field:{18U,20U,21U,22U,23U})++words[field];
        Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;status_is(vm.LoadBytes(encrypted(words)),ErrorCode::Ok,"padded text load");
        std::string text;require(vm.GetString(0,text)&&text=="S","whole padding changed string");
    });
    suite.test("message NUL remains mandatory with opaque alignment padding",[]{
        Model m;m.padding=0xff;m.code.end();auto words=program_words(m);words[words[words[5]]]=UINT32_MAX;reject(words);
    });
    suite.test("message entries may overlap with opaque padding",[]{
        Model m;m.padding=0xff;m.messages={"A","B"};m.code.end();auto words=program_words(m);words[words[5]+1U]=words[words[5]];
        Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;status_is(vm.LoadBytes(encrypted(words)),ErrorCode::Ok,"overlap load");
        std::string a,b;require(vm.GetMessage(0,a)&&vm.GetMessage(1,b)&&a=="A"&&b=="A","overlap changed messages");
    });

    for(const Word code:{load_code,preload_code})for(const Word layer:{0U,1U,UINT32_MAX,0x80000000U,0x7fffffffU})
        suite.test("filename uses unchanged raw text callback "+std::to_string(code)+"/"+std::to_string(layer),[code,layer]{
            Model m;m.padding=0x91;m.strings={std::string("x\\\x82\xa0.dwq",8)};m.code.filename(code,layer);m.code.end();
            Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);exec(vm,end_code);
            require(cb.retained_text==m.strings&&cb.states[0].pc==32,"filename bytes/current PC changed");
            const Value expected=layer<=0x7fffffffU?static_cast<Value>(layer):static_cast<Value>(-1-static_cast<Value>(UINT32_MAX-layer));
            require(cb.events[0].find("\"para1\":"+std::to_string(expected)+",")!=std::string::npos,"filename signed layer changed");
        });
    struct Shape{Word code;std::vector<std::size_t> accepted;std::vector<std::size_t> rejected;};
    const std::vector<Shape> shapes{
        {effect_code,{2,3,257,258},{0,1,259}}, {rect_code,{9},{0,8,10}}, {clear_layer_code,{1},{0,2}},
        {clear_all_code,{0},{1}}, {draw_code,{1},{0,2}}, {overrap_code,{2},{0,1,3}},
        {koma_code,{1,2,3},{4}}, {message_code,{1,2},{0,3}},
    };
    for(const auto& shape:shapes){
        for(const std::size_t count:shape.accepted)suite.test("bounded numeric shape "+std::to_string(shape.code)+"/"+std::to_string(count),[&,count]{
            std::vector<Word> values(count,0);if(count>0)values[0]=0x80000000U;if(count>1)values[1]=UINT32_MAX;if(count>2)values[2]=0x7fffffffU;
            const Model m=event_model(shape.code,values);NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);
            const bool yields=shape.code==draw_code||shape.code==overrap_code;exec(vm,yields?shape.code:end_code);if(yields)exec(vm,end_code);
            require(cb.retained_parameters.size()==1&&cb.retained_parameters[0].size()==count&&cb.states[0].pc==32,"numeric vector/PC mismatch");
            if(count>0)require(cb.retained_parameters[0][0]==INT32_MIN,"signed minimum lost");
            if(count>1)require(cb.retained_parameters[0][1]==-1,"signed minus one lost");
            if(count>2)require(cb.retained_parameters[0][2]==INT32_MAX,"signed maximum lost");
            require(cb.events[0].find("\"para1\":"+std::to_string(count)+",")!=std::string::npos&&cb.events[0].find("\"para3\":0}")!=std::string::npos,"numeric count/para3 mismatch");
        });
        for(const std::size_t count:shape.rejected)suite.test("numeric count uses structural minimums "+std::to_string(shape.code)+"/"+std::to_string(count),[&,count]{
            const bool invalid=count>258 || (shape.code==effect_code && count<2) || (shape.code==rect_code && count<9) ||
                (shape.code==clear_layer_code && count<1) || (shape.code==draw_code && count<1) || (shape.code==overrap_code && count<2);
            const Model m=event_model(shape.code,std::vector<Word>(count,0));
            if(invalid) reject(program_words(m));
            else {NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);const auto result=vm.Exec();
                status_is(result.status,ErrorCode::Ok,"numeric load and dispatch");
                require(cb.retained_parameters==std::vector<std::vector<Value>>{std::vector<Value>(count,0)} &&
                        cb.states[0].pc==32 && result.pc==32U+4U+count,"numeric count/dispatch/PC");}
        });
    }
    suite.test("STARTKOMA zero parameters dispatch",[]{NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,event_model(koma_code,{}));const auto r=exec(vm,end_code);require(r.pc==36 && cb.retained_parameters==std::vector<std::vector<Value>>{{}} && cb.states[0].pc==32,"zero STARTKOMA dispatch");});
    suite.test("STARTKOMA numeric record is a valid film koma metadata target",[]{
        Model m;m.main_labels={m.code.add({5,system_function,film_code,1,0})};m.film_offsets={0};m.koma_targets={m.code.numeric(koma_code,{7,1,0})};m.code.end();
        NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);exec(vm,end_code);require(cb.events.size()==2&&cb.states[1].pc==37,"film STARTKOMA event missing");
    });
    for(const Word duration:{0U,1U,UINT32_MAX,0x80000000U})suite.test("DRAW source duration yield "+std::to_string(duration),[duration]{
        NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,event_model(draw_code,{duration}));
        const auto result=exec(vm,duration==0?end_code:draw_code);require(result.pc==37&&cb.states[0].pc==32,"DRAW advance/callback PC");
        if(duration!=0)exec(vm,end_code);
    });
    for(const Word duration:{0U,1U,UINT32_MAX})suite.test("OVERRAP always yields "+std::to_string(duration),[duration]{
        NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,event_model(overrap_code,{0,duration}));
        require(exec(vm,overrap_code).pc==38&&cb.states[0].pc==32,"OVERRAP advance/current PC");exec(vm,end_code);
    });
    for(const auto& shape:shapes)suite.test("legacy host numeric default explicitly rejects "+std::to_string(shape.code),[&]{
        Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,event_model(shape.code,std::vector<Word>(shape.accepted.front(),0)));
        const auto result=vm.Exec();status_is(result.status,ErrorCode::Ok,"default numeric host");
        const Word ident=shape.code==draw_code||shape.code==overrap_code?system_command:system_function;
        require(result.pc==32U+4U+shape.accepted.front()&&cb.events.empty() &&
                cb.skipped==std::vector<std::tuple<Word,Word,Word>>{{32,ident,shape.code}},"default fallback/PC");
    });
    suite.test("host rejection leaves command unadvanced and prior callbacks visible",[]{
        Model m;m.code.story();m.code.numeric(effect_code,{1,2});m.code.end();NumericCallback cb;cb.accept=false;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);
        const auto result=vm.Exec();status_is(result.status,ErrorCode::Ok,"host decline");
        require(result.pc==43&&cb.events.size()==2&&cb.states[1].pc==37 &&
                cb.skipped==std::vector<std::tuple<Word,Word,Word>>{{37,system_function,effect_code}},"decline fallback or PC");
    });
    for(const bool standard:{false,true})suite.test(std::string("numeric exception does not advance ")+(standard?"standard":"nonstandard"),[standard]{
        NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,event_model(effect_code,{1,2}));cb.on_numeric=[standard](Word,const auto&){if(standard)throw std::runtime_error("host");throw 7;};
        const auto result=vm.Exec();status_is(result.status,ErrorCode::CallbackFailed,"numeric exception");require(result.pc==32,"exception advanced event");
    });
    suite.test("numeric callback Busy reentry preserves current PC",[]{
        const Model m=event_model(draw_code,{1});const auto bytes=encrypted(program_words(m));NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m,"kept");int calls=0;
        cb.on_numeric=[&](Word,const auto&){++calls;require(vm.GetPC()==32,"callback PC");status_is(vm.Exec().status,ErrorCode::Busy,"nested Exec");status_is(vm.LoadBytes(bytes,"new"),ErrorCode::Busy,"nested bytes");
            int reads=0;status_is(vm.LoadFile("new",{},[&](const std::string&){++reads;return nnn::ReadResult{};}),ErrorCode::Busy,"nested file");require(reads==0&&vm.GetPC()==32&&vm.Identity()=="kept","reentry mutated VM");};
        exec(vm,draw_code);require(calls==1,"numeric callback missing");
    });
    suite.test("numeric vector copy remains owned after successful replacement",[]{
        NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,event_model(effect_code,{0x80000000U,UINT32_MAX,0x7fffffffU}));exec(vm,end_code);
        Model next;next.code.end();load(vm,next,"replaced");exec(vm,end_code);require(cb.retained_parameters==std::vector<std::vector<Value>>{{INT32_MIN,-1,INT32_MAX}},"retained vector invalidated");
    });
    suite.test("raw filename copy survives successful replacement",[]{
        Model m;m.strings={std::string("\x82\xa0",2)};m.code.filename(load_code);m.code.end();Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);exec(vm,end_code);
        Model next;next.code.end();load(vm,next);require(cb.retained_text==m.strings,"retained text invalidated");
    });
    suite.test("large numeric budget is paid before any dispatch",[]{
        NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,event_model(effect_code,std::vector<Word>(258,0)));
        const auto result=vm.Exec(3);status_is(result.status,ErrorCode::BudgetExhausted,"numeric preflight budget");require(result.work_used<=3&&result.pc==32&&cb.events.empty(),"unfunded numeric event dispatched/advanced");
    });
    for(const std::uint64_t budget:{4U,5U,6U})suite.test("DRAW exact prepaid event budget "+std::to_string(budget),[budget]{
        NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,event_model(draw_code,{1}));const auto result=vm.Exec(budget);
        if(budget<6){status_is(result.status,ErrorCode::BudgetExhausted,"DRAW underfunded");require(result.work_used==1&&result.pc==32&&cb.events.empty(),"DRAW partial event funding");}
        else{status_is(result.status,ErrorCode::Ok,"DRAW funded");require(result.work_used==6&&result.return_code==draw_code&&result.pc==37&&cb.events.size()==1,"DRAW funded event");}
    });
    suite.test("maximum vector is fully funded before callback and next instruction",[]{
        NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,event_model(effect_code,std::vector<Word>(258,0)));
        const auto result=vm.Exec(263);status_is(result.status,ErrorCode::BudgetExhausted,"post-event budget stop");
        require(result.work_used==263&&result.pc==294&&cb.events.size()==1&&cb.retained_parameters[0].size()==258,"fully prepaid event was partially copied or rolled back");
    });
    suite.test("filename bytes budget is paid before dispatch",[]{
        Model m;m.strings={std::string(1000,'x')};m.code.filename(load_code);m.code.end();Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);
        const auto result=vm.Exec(3);status_is(result.status,ErrorCode::BudgetExhausted,"filename budget");require(result.work_used<=3&&result.pc==32&&cb.events.empty(),"unfunded text dispatch");
    });
    for(const auto& shape:shapes)suite.test("legacy seven-table instruction scope unchanged "+std::to_string(shape.code),[&]{Model m=event_model(shape.code,std::vector<Word>(shape.accepted.front(),0));m.labels=false;NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);const auto r=vm.Exec();status_is(r.status,ErrorCode::Ok,"legacy numeric");require(r.pc==32U+4U+shape.accepted.front()&&cb.retained_parameters==std::vector<std::vector<Value>>{std::vector<Value>(shape.accepted.front(),0)},"legacy numeric dispatch");});
    for(const Word code:{load_code,preload_code})suite.test("legacy filename scope unchanged "+std::to_string(code),[code]{Model m;m.labels=false;m.code.filename(code);m.code.end();Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);const auto r=exec(vm,end_code);require(r.pc==37&&cb.retained_text==std::vector<std::string>{"S"}&&cb.states[0].pc==32,"legacy filename dispatch");});
    for(const Word declared:{0U,1U,3U,UINT32_MAX})suite.test("numeric payload count must exactly match record "+std::to_string(declared),[declared]{auto words=program_words(event_model(effect_code,{1,2}));words[35]=declared;reject(words);});
    for(const Word code:{load_code,preload_code})for(const Word index:{1U,UINT32_MAX})suite.test("filename index remains bounded "+std::to_string(code)+"/"+std::to_string(index),[code,index]{Model m;m.code.filename(code,0,index);m.code.end();reject(program_words(m));});
    for(const Word code:{load_code,preload_code})for(const Word length:{4U,6U})suite.test("filename exact record length "+std::to_string(code)+"/"+std::to_string(length),[code,length]{
        Model m;m.code.add({length,system_function,code,0});if(length>=5)m.code.words.push_back(0);if(length>=6)m.code.words.push_back(0);m.code.end();reject(program_words(m));
    });
    suite.test("numeric command requires its count word",[]{Model m;m.code.add({3,system_function,clear_all_code});m.code.end();reject(program_words(m));});
    suite.test("failed event load preserves existing program and PC",[]{
        NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,event_model(draw_code,{1}),"kept");exec(vm,draw_code);const auto count=vm.WordCount();
        const auto invalid=encrypted(program_words(event_model(rect_code,{1})));status_is(vm.LoadBytes(invalid,"bad"),ErrorCode::InvalidFormat,"failed new event load");
        require(vm.GetPC()==37&&vm.Identity()=="kept"&&vm.WordCount()==count&&cb.events.size()==1,"failed event load mutated installed state");
    });
    suite.test("unreachable malformed numeric record still rejected",[]{Model m;m.code.end();m.code.numeric(effect_code,{1});m.code.end();reject(program_words(m));});
    suite.test("unreachable unknown opcode loads without dispatch",[]{Model m;m.code.end();m.code.numeric(0x3333ffffU,{});m.code.end();Callback cb;nnn::CExecScript vm(cb);cb.vm=&vm;load(vm,m);require(exec(vm,end_code).pc==32&&cb.skipped.empty(),"unreachable opcode dispatched");});

    for(const auto& item:dw009_fixture::cases())suite.test(std::string("fixed original event trace: ")+item.name,[&]{
        std::ifstream input(std::filesystem::path(NNN_EVENT_FIXTURE_DIR)/item.filename,std::ios::binary);require(static_cast<bool>(input),"missing event fixture");
        const std::vector<Byte> bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};NumericCallback cb;nnn::CExecScript vm(cb);cb.vm=&vm;status_is(vm.LoadBytes(bytes,item.name),ErrorCode::Ok,"fixed event load");
        for(const auto& step:item.steps){const auto actual=exec(vm,step.return_code);require(actual.pc==step.pc,"fixed event post-PC");const auto frames=active_frames(vm);require(frames.size()==step.frames.size(),"fixed event post-depth");for(std::size_t i=0;i<frames.size();++i)require(frames[i].return_pc==step.frames[i].return_pc&&frames[i].id==step.frames[i].id,"fixed event post-frame");}
        require(cb.events==item.callback_json,"fixed event complete callbacks");require(cb.states.size()==item.callback_states.size(),"fixed event callback state count");
        for(std::size_t i=0;i<cb.states.size();++i){const auto& actual=cb.states[i];const auto& expected=item.callback_states[i];require(actual.pc==expected.pc&&actual.frames.size()==expected.frames.size(),"fixed event callback PC/depth");for(std::size_t f=0;f<actual.frames.size();++f)require(actual.frames[f].return_pc==expected.frames[f].return_pc&&actual.frames[f].id==expected.frames[f].id,"fixed event callback frame");}
        for(const auto& pair:item.variables)require(cb.variables[pair.first]==pair.second,"fixed event variables");
    });
    std::cout<<"EVENT RESULT "<<suite.passed<<" passed, "<<suite.failed<<" failed\n";return suite.failed==0?0:1;
}
