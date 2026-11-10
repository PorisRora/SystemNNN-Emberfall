#include "emberfall/scene.hpp"
#include "emberfall/game_session.hpp"
#include "nnn/player_behavior.hpp"
#include "nnn/snapshot.hpp"
#include "fixtures/generated_cases.hpp"
#include "../src/chart_node_save.hpp"
#include "../src/game_assets.hpp"
#include "../src/pc_save.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <chrono>
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
using Byte=std::uint8_t;
using Word=std::uint32_t;
using Bytes=std::vector<Byte>;
namespace scene=emberfall::scene;
constexpr Word control=0x66660002U,system_function=0x66660007U,command=0x66660006U;
constexpr Word draw=0x22220004U,load=0x33330015U,preload=0x3333001fU,effect=0x33330013U,rect=0x33330016U;
constexpr Word clear_all=0x33330012U,clear_layer=0x33330014U;
void require(bool condition,const std::string& text){if(!condition)throw std::runtime_error(text);}
const auto fixtures=std::filesystem::path(EMBERFALL_SCENE_FIXTURE_DIR);
Bytes read_file(const std::filesystem::path& path){std::ifstream input(path,std::ios::binary);require(static_cast<bool>(input),"missing fixture "+path.string());return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};}
Bytes asset(const char* name){return read_file(fixtures/"assets"/(std::string(name)+".dwq"));}
Word bits(std::int32_t value){return static_cast<Word>(value);}
struct Code{
    std::vector<Word> words,registry;
    Word pc()const{return 32U+static_cast<Word>(words.size());}
    Word add(std::initializer_list<Word> values){Word start=pc();words.insert(words.end(),values);return start;}
    Word number(Word code,const std::vector<Word>& values){Word start=pc();words.insert(words.end(),{4U+static_cast<Word>(values.size()),code==draw||code==0x22220005U?command:system_function,code,static_cast<Word>(values.size())});words.insert(words.end(),values.begin(),values.end());return start;}
    Word image(Word layer,Word index=1,Word code=load){return add({5,system_function,code,layer,index});}
    Word story(Word index=0){return add({5,system_function,0x33330051U,1,index});}
    Word voice(Word index=4){return number(nnn::audio::voice,{index,0});}
    Word user(Word id,const std::vector<int>& values={}){
        const Word start=pc();
        words.insert(words.end(),{4U+static_cast<Word>(values.size()),0x66660003U,id,static_cast<Word>(values.size())});
        for (int value:values) words.push_back(0x90000000U|(static_cast<Word>(value)&0xffffU));
        return start;
    }
    Word print(Word serial,Word index=0){return add({6,command,0x22220001U,index,serial,0});}
    Word end(){return add({3,control,0x55560005U});}
    Word present(Word value=1){return number(draw,{value});}
    Word place(Word layer,std::int32_t x,std::int32_t y,Word width,Word height){return number(rect,{layer,bits(x),bits(y),width,height,bits(x),bits(y),width,height});}
};
struct Model{
    Code code;
    std::vector<std::string> strings{"story","red","green","alpha","aa_voice"};
    std::vector<Word> command_adjust;
    bool message=false;
    std::string message_text="M";
};
Bytes script(const Model& model){
    std::vector<Word> words(32,0);words[0]=32;words[1]=0x66660001U;words[2]=0x55550001U;
    words[12]=0x48545053U;words[13]=0x45444145U;words[14]=0x20203052U;words[15]=0x00202020U;
    words.insert(words.end(),model.code.words.begin(),model.code.words.end());
    const auto table=[&](Word kind,Word field,const std::vector<Word>& payload,bool record=false){words[field]=static_cast<Word>(words.size())+(record?0U:4U);words.insert(words.end(),{4U+static_cast<Word>(payload.size()),0x66660001U,0x55550002U,kind});words.insert(words.end(),payload.begin(),payload.end());};
    table(0x44440003U,9,{});table(0x44440003U,16,{},true);table(0x44440003U,17,{},true);
    if(model.message){
        words[4]=1;
        std::vector<Word> message(1+(model.message_text.size()+4)/4,0);
        message[0]=static_cast<Word>(words.size())+5U;
        for (std::size_t i=0;i<model.message_text.size();++i)
            message[1+i/4]|=static_cast<Word>(static_cast<unsigned char>(model.message_text[i]))<<(8U*(i%4));
        table(0x44440002U,5,message);
    }
    else table(0x44440002U,5,{});
    std::vector<Word> strings(model.strings.size(),0);const Word begin=static_cast<Word>(words.size())+4U;
    for(std::size_t i=0;i<model.strings.size();++i){
        strings[i]=begin+static_cast<Word>(strings.size());Bytes bytes;
        for(char value:model.strings[i])bytes.push_back(static_cast<Byte>(static_cast<unsigned char>(value)));
        bytes.push_back(0);while(bytes.size()%4U)bytes.push_back(0);
        for(std::size_t b=0;b<bytes.size();b+=4U)strings.push_back(Word{bytes[b]}|(Word{bytes[b+1U]}<<8U)|(Word{bytes[b+2U]}<<16U)|(Word{bytes[b+3U]}<<24U));
    }
    words[6]=static_cast<Word>(model.strings.size());table(0x44440001U,7,strings);table(0x44440004U,18,{});table(0x44440005U,20,model.code.registry);
    table(0x44440006U,21,{});table(0x44440007U,22,model.command_adjust);table(0x44440008U,23,{});
    Bytes bytes;for(Word word:words){word^=UINT32_MAX;for(unsigned shift=0;shift<32;shift+=8)bytes.push_back(static_cast<Byte>((word>>shift)&255U));}return bytes;
}
struct Resources{
    std::map<std::string,Bytes> files{{"red",asset("tiny_red")},{"green",asset("tiny_green")},{"alpha",asset("tiny_alpha")}};
    std::vector<std::string> keys;
    bool read(const std::string& key,Bytes& output){keys.push_back(key);auto found=files.find(key);if(found==files.end())return false;output=found->second;return true;}
    scene::Reader reader(){return [this](const std::string& key,Bytes& output){return read(key,output);};}
};
scene::FrameResult run(const Model& model,Resources& resources,Word width=1,Word height=1){return scene::first_frame(script(model),resources.reader(),width,height);}
void success(const scene::FrameResult& result,Word width,Word height,const Bytes& rgb){require(result.error.empty(),"scene failure: "+result.error);require(result.surface.width==width&&result.surface.height==height&&result.surface.rgb==rgb,"scene RGB/dimensions mismatch");require(result.exec_calls==1,"first-frame path must use one Exec");}
void failure(const scene::FrameResult& result){require(!result.error.empty(),"failure was acknowledged as success");require(result.surface.width==0&&result.surface.height==0&&result.surface.rgb.empty(),"failure exposed partial/stale pixels");}
Model single(Word index=1){Model m;m.code.image(4,index);m.code.number(effect,{4,0});m.code.present();m.code.end();return m;}
void le32(Bytes& bytes,std::size_t offset,Word value){for(unsigned n=0;n<4;++n)bytes[offset+n]=static_cast<Byte>(value>>(8U*n));}
void be32(Bytes& bytes,Word value){for(int shift=24;shift>=0;shift-=8)bytes.push_back(static_cast<Byte>(value>>static_cast<unsigned>(shift)));}
Word crc(const Bytes& bytes){Word value=UINT32_MAX;for(Byte byte:bytes){value^=byte;for(int bit=0;bit<8;++bit)value=(value>>1U)^(0xedb88320U&(0U-(value&1U)));}return value^UINT32_MAX;}
Bytes metadata_heavy(){
    Bytes bytes=asset("tiny_red"),chunks;
    for(int i=0;i<2;++i){Bytes body{'t','E','X','t','k',0};body.resize(4U+3U*1024U*1024U,'x');be32(chunks,static_cast<Word>(body.size()-4U));chunks.insert(chunks.end(),body.begin(),body.end());be32(chunks,crc(body));}
    // PNG starts at64; insert after its fixed33-byte signature/IHDR sequence.
    bytes.insert(bytes.begin()+97,chunks.begin(),chunks.end());le32(bytes,32,static_cast<Word>(bytes.size()-64U));return bytes;
}
// A few bytes of authored configuration, without graphics or game resources.
// Each instance owns a newly created directory and removes only that directory.
struct BehaviorConfig {
    std::filesystem::path root;
    BehaviorConfig(){
        const auto base=std::filesystem::canonical(std::filesystem::temp_directory_path());
        const auto stamp=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for(unsigned attempt=0;attempt<128U;++attempt){
            const auto candidate=base/("systemnnn-player-behavior-"+stamp+"-"+std::to_string(attempt));
            if(std::filesystem::create_directory(candidate)){root=candidate;break;}
        }
        require(!root.empty(),"cannot create private behavior configuration");
        try{
            std::filesystem::create_directory(root/"init");
            const auto write=[&](const char* name,const char* text){
                std::ofstream output(root/"init"/name,std::ios::binary);
                output<<text;output.close();require(static_cast<bool>(output),"cannot write behavior configuration");
            };
            write("printmessage.xtx","customMessageSpeed 1\nmessageSpeed1 20\nmessageSpeed2 37\n"
                "customAutoMessageSpeed 1\nautoMessageSpeed1 113\nautoMessageSpeed2 137\n"
                "noVoiceLastWait 100\nvoiceLastWait 50\n");
            write("config.xtx","volumeNumber 8\nautoSpeedVolumeExistFlag 1\n");
        }catch(...){std::error_code ignored;std::filesystem::remove_all(root,ignored);throw;}
    }
    ~BehaviorConfig(){std::error_code ignored;std::filesystem::remove_all(root,ignored);}
    BehaviorConfig(const BehaviorConfig&)=delete;
    BehaviorConfig& operator=(const BehaviorConfig&)=delete;
};
struct TempGame {
    std::filesystem::path root;
    explicit TempGame(const Model& model) {
        const auto base=std::filesystem::canonical(std::filesystem::temp_directory_path());
        const auto stamp=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for(unsigned attempt=0;attempt<128U;++attempt){
            const auto candidate=base/("systemnnn-session-"+stamp+"-"+std::to_string(attempt));
            if(std::filesystem::create_directory(candidate)){root=candidate;break;}
        }
        require(!root.empty(),"cannot create private game");
        try { write("spt/sys.spt",script(model)); }
        catch(...) { std::error_code ignored;std::filesystem::remove_all(root,ignored);throw; }
    }
    void write(const std::string& name,const Bytes& bytes) {
        const auto path=root/name;std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path,std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        out.close();require(static_cast<bool>(out),"cannot write temporary game");
    }
    void text(const std::string& name,const std::string& value) { write(name,Bytes(value.begin(),value.end())); }
    ~TempGame(){std::error_code ignored;std::filesystem::remove_all(root,ignored);}
    TempGame(const TempGame&)=delete;
    TempGame& operator=(const TempGame&)=delete;
};
scene::SessionOptions test_options(){scene::SessionOptions options;options.width=1;options.height=1;options.audio.record_events=true;return options;}
Bytes pcm_vaw(unsigned channels,unsigned rate,unsigned bits,const Bytes& pcm) {
    Bytes b(108U+pcm.size(),0);
    const std::string header="PACKTYPE=0";std::copy(header.begin(),header.end(),b.begin()+48);
    const auto tag=[&](std::size_t at,const char* text){std::copy(text,text+4,b.begin()+static_cast<std::ptrdiff_t>(at));};
    const auto le16=[&](std::size_t at,unsigned value){b[at]=static_cast<Byte>(value);b[at+1]=static_cast<Byte>(value>>8U);};
    tag(64,"RIFF");tag(72,"WAVE");tag(76,"fmt ");le32(b,80,16);le16(84,1);le16(86,channels);
    le32(b,88,rate);le16(96,channels*bits/8U);le16(98,bits);tag(100,"data");le32(b,104,static_cast<Word>(pcm.size()));
    std::copy(pcm.begin(),pcm.end(),b.begin()+108);return b;
}
struct Suite{int passed=0,failed=0;void test(const std::string& name,const std::function<void()>& body){try{body();++passed;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& error){++failed;std::cerr<<"FAIL "<<name<<": "<<error.what()<<'\n';}}};
} // namespace

int main(){
    Suite suite;
    suite.test("script input waits at the return PC and commits the original name and replacement state",[]{
        Model model;model.message=true;model.message_text="#L/#F, #02/#S03";
        model.code.print(1);model.code.user(0);model.code.print(2);
        model.code.user(1,{2,0});model.code.user(1,{3,1});
        model.code.user(1,{99,2});model.code.user(1,{0,3});
        model.code.print(3);model.code.user(0);model.code.print(4);model.code.end();
        TempGame temp(model);
        temp.text("init/game.xtx","codeByte 1\nNameInputIsDefault 1\nKanjiInputIsDefault 1\n"
            "seiInitData Old\nmeiInitData Given\nuseDefaultOkikae 1\nJumpFlag 1\n"
            "compareResultVarName compared\nanswerStringNumber 1\nanswerString1 word\n");
        temp.text("init/nameInput.xtx","meiFlag 0\nmeiFixFlag 1\nmeiFixData Fixed\nnullNameOk 1\nseiMax 3\n"
            "statusVarName result\ntimeVarName elapsed\n");
        temp.text("init/kanjiInput.xtx","kakuteiTextMax1 4\n");
        temp.text("nya/softkey.xtx","kakuteiTextMax 5\n");
        temp.text("nya/okikae.xtx","2,BASE\n");
        temp.text("spt/command.xtx","NAMEINPUT,0\nKANJIINPUT,2\n");
        temp.text("spt/function.xtx","check,2\n");
        const std::string variables="result\nelapsed\ncompared\n";
        Bytes encoded(variables.begin(),variables.end());
        for (auto& byte:encoded) byte^=0xffU;
        temp.write("spt/var.fxf",encoded);
        auto options=test_options();
        options.runtime=std::make_shared<nnn::RuntimeContext>(temp.root);
        require(options.runtime->set_system_name("Stored","Global"),"initial system names");
        require(options.runtime->set_system_replacement(3,"SYS"),"initial system substitution");
        scene::GameSession game(temp.root,options);
        require(game.step_frame(0).dialogue.display_bytes=="Stored/Global, BASE/SYS","initial text expansion");
        game.capture_backlog_jump(1);
        const auto old_jump=game.backlog().back().jump_id;
        const auto old_native=game.snapshot(),old_pc=game.export_pc_save();
        scene::InputState advance;advance.advance=true;
        const auto name=game.step_frame(0,advance).modal;
        require(name && name->mode==nnn::RuntimeMode::NameInput && !name->return_mode &&
            name->text==std::vector<std::string>{"Stored","Fixed","Old","Given"} && name->byte_limits[0]==6 &&
            game.mode()==static_cast<int>(nnn::RuntimeMode::NameInput),"name request source config");
        const auto word=game.state().word;
        bool rejected=false;
        try { (void)game.snapshot(); } catch (const std::runtime_error&) { rejected=true; }
        require(rejected && game.step_frame(100,advance).word==word &&
            game.state().wait==scene::Wait::Modal,"pending input must not step or save");
        require(!game.complete_modal(name->request+1,true,2,{"","bad"}) &&
            !game.complete_modal(name->request,true,2,{"toolong","bad"}) &&
            game.complete_modal(name->request,true,10000,{"","ignored"}),
            "stale, overlong and blank name completion policy");
        require(game.variable(0)==1 && game.variable(1)==9999 && game.take_system_save_request() &&
            game.step_frame(0).dialogue.display_bytes=="Old/Fixed, BASE/SYS","fixed and empty-default name commit");
        auto pending=game.step_frame(0,advance).modal;
        require(pending && pending->mode==nnn::RuntimeMode::KanjiInput && pending->byte_limits==std::vector<int>{4} &&
            pending->text==std::vector<std::string>{""} && pending->arguments==std::vector<nnn::Value>{2,0},
            "local replacement modal args and capacity");
        require(!game.complete_modal(pending->request,false,0,{}) &&
            !game.complete_modal(pending->request,true,0,{""}) &&
            game.complete_modal(pending->request,true,0,{"word"}),"KANJI requires nonempty confirmation");
        Model menu;menu.code.add({6,0x66660004U,0,2,0x90000002U,0x90000001U}); // check(2,1)
        menu.code.present();menu.code.end();
        temp.write("spt/menu.spt",script(menu));
        auto preview_options=options;preview_options.runtime=game.runtime()->fork();
        auto preview=scene::GameSession::create_ui_preview(temp.root,preview_options);
        preview->reset_ui_background("menu",12,preview_options.runtime);
        require(preview->step_ui_background({},false).wait==scene::Wait::Draw && preview->variable(2)==1 &&
            game.variable(2)==0,"decorative CHECK sees local text and keeps its result private");
        for (const std::string input:{"outer","New","First"}) {
            pending=game.step_frame(0).modal;
            require(pending && pending->byte_limits==std::vector<int>{5} &&
                game.complete_modal(pending->request,true,0,{input}),"KANJI target completion");
        }
        const auto& after=game.step_frame(0).dialogue;
        require(after.display_bytes=="New/First, word/outer" && after.raw_bytes==model.message_text &&
            game.runtime()->system_replacement(3)=="outer","four KANJI targets and original bytes");
        const auto saved=game.snapshot();
        nnn::snapshot::Reader inspected(saved);
        scene::GameSession::inspect_snapshot(inspected,[](const scene::FrameState& state){
            return state.wait==scene::Wait::Dialogue && state.dialogue.display_bytes=="New/First, word/outer";
        });
        const nnn::snapshot::Bytes truncated(saved.begin(),saved.end()-1);
        nnn::snapshot::Reader invalid_inspection(truncated);
        bool inspection_rejected=false;
        try { scene::GameSession::inspect_snapshot(invalid_inspection); }
        catch (const std::runtime_error&) { inspection_rejected=true; }
        require(inspection_rejected,"resource-independent snapshot inspection must reject a truncated payload");
        pending=game.step_frame(0,advance).modal;
        require(pending && game.complete_modal(pending->request,false,27,{}) &&
            game.variable(0)==0 && game.variable(1)==27 &&
            game.step_frame(0).dialogue.display_bytes=="New/First, word/outer","NAME cancel must retain names and return status/time");
        require(game.restore_backlog_jump(old_jump,1) && game.runtime()->runtime_text().surname=="Stored" &&
            game.runtime()->runtime_text().given_name=="Global" && game.runtime()->system_replacement(3)=="outer",
            "Backlog restores both names but leaves the system replacement table live");
        game.restore_snapshot(saved);
        require(game.runtime()->runtime_text().surname=="New" && game.runtime()->runtime_text().given_name=="First" &&
            game.runtime()->system_replacement(3)=="outer","native snapshot name commit");
        rejected=false;
        try { game.restore_snapshot(old_native,[](const scene::FrameState&){return false;}); }
        catch (const std::runtime_error&) { rejected=true; }
        require(rejected && game.runtime()->runtime_text().surname=="New","failed restore changed global names");
        game.restore_pc_save(old_pc);
        require(game.runtime()->runtime_text().surname=="Stored" && game.runtime()->runtime_text().given_name=="Global" &&
            game.state().dialogue.display_bytes=="Stored/Global, BASE/outer","PC slot re-expands original bytes with live system table");
        const auto unavailable=game.step_frame(0,advance).modal;
        require(unavailable && game.abort_modal(unavailable->request,"missing art",31)==0 &&
            game.abort_modal(unavailable->request,"duplicate")==-1 && game.variable(0)==0 && game.variable(1)==31 &&
            game.runtime()->runtime_text().surname=="Stored","unavailable name UI follows cancellation without changing names");
    });
    suite.test("input byte validation accepts complete game pairs and honors command class gates",[]{
        Model model;model.message=true;model.message_text="#\x90\xa9";
        model.code.user(0);model.code.user(1,{0,2});model.code.print(1);model.code.end();
        TempGame temp(model);
        temp.text("init/game.xtx","NameInputIsDefault 1\nNameInputClassDisable 1\nKanjiInputIsDefault 1\n");
        temp.text("spt/command.xtx","nameinput\n0\nkanjiinput\n2\n");
        temp.text("init/kanjiInput.xtx","kakuteiTextMax3 1\n");
        scene::GameSession game(temp.root,test_options());
        const auto pending=game.step_frame(0).modal;
        require(pending && pending->mode==nnn::RuntimeMode::KanjiInput && pending->byte_limits==std::vector<int>{2},
            "class-disabled name command must pass, leaving the later kanji request");
        require(!game.complete_modal(pending->request,true,0,{"a"}) &&
            !game.complete_modal(pending->request,true,0,{"\x82"}) &&
            !game.complete_modal(pending->request,true,0,{std::string("\x82\0",2)}) &&
            game.complete_modal(pending->request,true,0,{"\x81\x01"}),
            "accept valid tunneled pairs without accepting incomplete or NUL data");
        const auto& line=game.step_frame(0).dialogue;
        require(line.display_bytes=="\x81\x01" && line.raw_bytes==model.message_text &&
            game.runtime()->runtime_text().surname=="\x81\x01","raw game bytes preserved");
    });
    suite.test("optional class creation and default command registration are independent",[]{
        struct Flags { const char* values; bool created,registered; };
        for (const auto& flags:{
            Flags{"",false,false},
            Flags{"SelectZukanIsDefault 1\n",true,true},
            Flags{"SelectZukanIsDefault 1\nSelectZukanClassDisable 1\n",false,false},
            Flags{"SelectZukanIsDefault 0\nSelectZukanClassDisable 0\n",true,false}}) {
            Model model;model.message=true;
            model.code.add({5,0x66660004U,0,1,0x90000001U}); // zukanon(1)
            model.code.user(0);model.code.print(1);model.code.end();
            TempGame temp(model);
            temp.text("init/game.xtx",flags.values);
            temp.text("init/selectZukan.xtx","appearVarName seen\nfilenameSeat seat\n");
            temp.text("spt/command.xtx","selectzukan,0\n");
            temp.text("spt/function.xtx","zukanon,1\n");
            const std::string variables="seen\n";
            Bytes encoded(variables.begin(),variables.end());
            for (auto& byte:encoded) byte^=0xffU;
            temp.write("spt/var.fxf",encoded);
            scene::GameSession game(temp.root,test_options());
            auto modal=game.step_frame(0).modal;
            require(modal.has_value()==flags.registered && game.variable(0)==(flags.created ? 1 : 0),
                "user command gate or class function dispatch differed");
            if (modal) {
                require(game.abort_modal(modal->request,"test") ==0 && game.step_frame(0).wait==scene::Wait::Dialogue,
                    "script modal cleanup");
            }
            require(game.open_modal(nnn::RuntimeMode::SelectZukan,nnn::RuntimeMode::Omake)==flags.created,
                "external panel must use class creation gate");
        }
    });
    suite.test("both place classes keep independent rules across a native save and decorative preview",[]{
        Model model;model.message=true;model.code.user(0);model.code.user(1);model.code.print(1);model.code.end();
        TempGame temp(model);
        temp.text("init/game.xtx","SelectPlaceIsDefault 1\nSelectPlace2IsDefault 1\n");
        temp.text("spt/command.xtx","SELECTPLACE,0\nSELECTPLACE2,0\n");
        const std::string variables="first\nsecond\nvisible\nenabled\nchara\nfirstHover\nsecondHover\n";
        Bytes encoded(variables.begin(),variables.end());
        for (auto& byte:encoded) byte^=0xffU;
        temp.write("spt/var.fxf",encoded);
        temp.text("spt/varinitdata.xtx","0\n0\n1\n1\n4\n0\n0\n");
        temp.text("init/selectPlace.xtx","selectedPlaceVarName first\nonButtonVarName firstHover\n");
        temp.text("init/selectPlace2.xtx","placeNumber 3\nselectedPlaceVarName second\nonButtonVarName secondHover\n"
            "placeAppearFlag 1\nplaceAppearVarName2 visible\nplaceAppearVarName3 visible\n"
            "selectFlag 1\nplaceSelectOkVarName3 enabled\ncharaFlag 1\nplaceCharaVarName3 chara\n");
        auto options=test_options();
        scene::GameSession game(temp.root,options);
        require(game.step_frame(0).wait==scene::Wait::Place && game.place_mode()==nnn::RuntimeMode::SelectPlace &&
            game.places()==std::vector<int>{2} && game.place_characters()==std::vector<int>{0},"first class setup");
        const auto first=game.snapshot();
        scene::InputState choose;choose.choice=1;
        require(game.step_frame(0,choose).wait==scene::Wait::Place && game.place_mode()==nnn::RuntimeMode::SelectPlace2 &&
            game.places()==std::vector<int>{0,1,2} && game.place_characters()==std::vector<int>{0,0,4} &&
            game.variable(0)==1 && game.variable(1)==0,"second class setup");
        const auto second=game.snapshot();
        auto preview=scene::GameSession::create_ui_preview(temp.root,options);
        preview->start_ui_background("sys",static_cast<int>(nnn::RuntimeMode::SelectPlace2));
        preview->set_ui_place_preview(3);
        require(preview->named_variable("secondHover")==3 && preview->named_variable("firstHover")==0,
            "mode 35 preview must use its own hover variable");
        require(game.step_frame(0,choose).wait==scene::Wait::Place,"hidden place accepted");
        choose.choice=2;
        require(game.step_frame(0,choose).wait==scene::Wait::Place,"disabled place accepted");
        choose.choice=3;
        require(game.step_frame(0,choose).wait==scene::Wait::Dialogue && game.place_mode()==nnn::RuntimeMode::Nothing &&
            game.variable(1)==3 && game.variable(5)==1 && game.variable(6)==3,"selection result and hover");
        options.runtime=game.runtime()->fork();
        scene::GameSession title(temp.root,options);
        require(title.named_variable("second")==0 && title.named_variable("second",true)==3,
            "external menu checks can read the last committed story locals");
        game.restore_snapshot(first);
        require(game.place_mode()==nnn::RuntimeMode::SelectPlace && game.places()==std::vector<int>{2},
            "first place class snapshot");
        game.restore_snapshot(second);
        require(game.place_mode()==nnn::RuntimeMode::SelectPlace2 && game.places()==std::vector<int>{0,1,2} &&
            game.step_frame(0,choose).wait==scene::Wait::Dialogue && game.variable(1)==3,"second place class snapshot");
        temp.text("spt/varinitdata.xtx","0\n77\n1\n0\n4\n0\n0\n");
        scene::GameSession empty(temp.root,test_options());
        choose.choice=1;
        require(empty.step_frame(0).wait==scene::Wait::Place && empty.step_frame(0,choose).wait==scene::Wait::Dialogue &&
            empty.variable(1)==77,"a place2 panel without legal choices must resume without a fabricated selection");
    });
    suite.test("hint and zukan modal actions stay in the native menu until the original return path",[]{
        Model model;model.message=true;
        model.code.user(0);
        model.code.add({5,0x66660004U,0,1,0x9000001fU}); // zukanon(31)
        model.code.user(1);model.code.user(2,{1});model.code.print(1);model.code.end();
        TempGame temp(model);
        temp.text("init/game.xtx","SelectHintIsDefault 1\nSelectZukanIsDefault 1\nPrintHintIsDefault 1\n");
        temp.text("spt/command.xtx","selecthint 0\nselectzukan,0\nprinthint,1\n");
        temp.text("spt/function.xtx","zukanon,1\n");
        const std::string variables="locked\nunlocked\nseenA\nseenB\nchosen\n";
        Bytes encoded(variables.begin(),variables.end());
        for (auto& byte:encoded) byte^=0xffU;
        temp.write("spt/var.fxf",encoded);
        temp.text("spt/varinitdata.xtx","0\n1\n2\n0\n9\n");
        temp.text("init/selectHint.xtx","groupNumber 3\ngroup1Flag locked\ngroup2Flag unlocked\n"
            "group2FileName visible_hint\ngroup3Flag unlocked\n");
        temp.text("init/printHint.xtx","hintAllNumber 3\npicNumX 1\npicNumY 1\nhint3Number 1\nhint3flag1 unlocked\n"
            "hint3filename1 missing_picture\n"
            "hint2Number 3\nhint2flag1 unlocked\nhint2filename1 missing_first\n"
            "hint2flag2 unlocked\nhint2filename2 visible_picture\nnopicFilename locked_picture\n");
        temp.write("sys/visible_picture.dwq",asset("tiny_red"));
        temp.write("sys/locked_picture.dwq",asset("tiny_green"));
        temp.text("init/selectZukan.xtx","zukanNumber 32\nzukanNumberX 8\nzukanNumberY 4\nfilenameSeat seat\n"
            "appearVarName seenA\nappearVarName2 seenB\nreturnVarName chosen\n");
        scene::GameSession game(temp.root,test_options());
        auto modal=game.step_frame(0).modal;
        require(modal && modal->mode==nnn::RuntimeMode::SelectHint && modal->rows.size()==1 &&
            modal->rows[0].id==2 && modal->rows[0].asset=="visible_hint","locked or unavailable hint groups must stay hidden");
        const auto start_id=modal->request;
        require(game.act_modal(start_id,1,1)==-1 && game.act_modal(start_id,1,3)==-1 &&
            game.act_modal(start_id,1,2)==1 &&
            game.act_modal(start_id,0,0)==-1,"only a current, unlocked group can open");
        modal=game.state().modal;
        require(modal && modal->mode==nnn::RuntimeMode::PrintHint && modal->group==2 && modal->pages==3 && modal->page==1 &&
            modal->rows[0].asset=="visible_picture" && game.act_modal(modal->request,2,3)==-1 &&
            game.act_modal(modal->request,2,2)==0,"starts on the first page with usable pictures");
        modal=game.step_frame(1000).modal;
        require(modal && modal->page==2 && modal->rows[0].asset=="locked_picture" && modal->rows[0].flags==0 &&
            game.act_modal(modal->request,0,0)==1,"print back should open select without stepping the story");
        modal=game.state().modal;
        require(modal && modal->mode==nnn::RuntimeMode::SelectHint && game.act_modal(modal->request,0,0)==2,
            "select hint back resumes the VM");
        modal=game.step_frame(0).modal;
        require(modal && modal->mode==nnn::RuntimeMode::SelectZukan && modal->rows.size()==2 &&
            modal->rows[0].id==2 && modal->rows[1].id==31 && game.variable(4)==0,"zukan bit blocks and reset");
        const auto grid_id=modal->request;
        require(game.act_modal(grid_id,1,1)==-1 && game.act_modal(grid_id,1,31)==0 && game.variable(4)==31 &&
            game.runtime()->preview_story()->vars[4]==31,"detail result is available to the background script");
        modal=game.state().modal;
        require(modal && modal->request!=grid_id && modal->selection==31 && modal->rows.empty() &&
            game.act_modal(grid_id,0,0)==-1 && game.act_modal(modal->request,0,0)==0 && game.variable(4)==0,
            "close detail and reset its result before redrawing the grid");
        modal=game.state().modal;
        require(modal && modal->selection==0 && game.act_modal(modal->request,0,0)==2,"grid back resumes the VM");
        modal=game.step_frame(0).modal;
        require(modal && modal->mode==nnn::RuntimeMode::PrintHint && modal->group==2 && modal->page==1,
            "direct PRINTHINT uses the last PC hint group and ignores its script argument");
        temp.text("init/printHint.xtx","hintAllNumber 3\nhint2Number 0\nhint3Number 0\n");
        scene::GameSession no_hints(temp.root,test_options());
        const auto fallback=no_hints.step_frame(0).modal;
        require(fallback && fallback->mode==nnn::RuntimeMode::SelectZukan,
            "no valid child groups must resume the story, without a second empty panel");
    });
    suite.test("ordinary VOICE marks one-based roles separately from voice clips",[]{
        Model model;model.message=true;
        model.code.voice();model.code.print(1);model.code.end();
        TempGame temp(model);
        temp.text("spt/charaname.xtx","0,header\naa,Character\n");
        temp.write("cdvaw/aa/aa_voice.vaw",pcm_vaw(1,44100,8,{188,198,208,218}));
        temp.text("init/game.xtx","mustSetVoiceFlag 0\n");
        auto options=test_options();
        scene::GameSession heard(temp.root,options);
        require(heard.step_frame(0).wait==scene::Wait::Dialogue && heard.runtime()->voice_role_heard(1) &&
            !heard.runtime()->voice_played(1) && heard.take_system_save_request(),"ordinary voice role progress");
        scene::GameSession skipped(temp.root,options);
        skipped.set_skip_active(true);
        require(skipped.step_frame(0).wait==scene::Wait::Dialogue && !skipped.runtime()->voice_role_heard(1),
            "skip suppresses role progress when mustSetVoiceFlag is off");
        temp.text("init/game.xtx","mustSetVoiceFlag 1\n");
        scene::GameSession required(temp.root,options);
        required.set_skip_active(true);
        require(required.step_frame(0).wait==scene::Wait::Dialogue && required.runtime()->voice_role_heard(1),
            "mustSetVoiceFlag marks roles even while skipping");
    });
    suite.test("term and voice menus use global flags, filter unavailable data and keep source return destinations",[]{
        Model model;model.message=true;
        model.code.number(0x3333006dU,{0});
        model.code.number(nnn::audio::set_voice_flag,{17});
        model.code.user(0);model.code.user(1);model.code.user(2);model.code.user(3);
        model.code.print(1);model.code.end();
        TempGame temp(model);
        temp.text("init/game.xtx","codeByte 1\nPrintTermIsDefault 1\nListenVoiceIsDefault 1\n"
            "PrintCalendarIsDefault 1\nCardSystemIsDefault 1\n");
        temp.text("spt/command.xtx","printterm,0\nlistenvoice,0\nprintcalendar,0\ncardsystem,0\n");
        temp.text("init/printTerm.xtx","termTypeNumber 2\ntermPerPage 1\n");
        temp.text("spt/termList.xtx","Zebra,1,0,zebra\nAlpha,1,0,alpha\nMissing,1,0,absent\nOther,2,0,other\n");
        temp.text("nya/term/zebra.xtx","Zebra description\n");
        temp.text("nya/term/alpha.xtx","Alpha description\n");
        temp.text("nya/term/other.xtx","Other description\n");
        temp.text("init/listenVoice.xtx","voiceCharaNumber 1\nvoicePerPage 1\n");
        temp.text("nya/charaVoiceList1.xtx","1,line,Unlocked\n0,raw,Default\n");
        Bytes flags(48,0);
        const std::string header="VOICEFLAGANDNAME",filename="line";
        std::copy(header.begin(),header.end(),flags.begin());
        std::copy(filename.begin(),filename.end(),flags.begin()+32);
        le32(flags,16,1);le32(flags,44,17);
        temp.write("spt/voiceflag.tbl",flags);
        temp.write("cdvaw/li/line.vaw",pcm_vaw(1,44100,8,{188,198,208,218}));
        temp.write("cdvaw/ra/raw.vaw",pcm_vaw(1,44100,8,{178,188,198,208}));
        auto options=test_options();
        scene::GameSession game(temp.root,options);
        auto modal=game.step_frame(0).modal;
        require(modal && modal->mode==nnn::RuntimeMode::PrintTerm && modal->pages==2 && modal->rows.size()==1 &&
            modal->rows[0].id==2 && modal->rows[0].label=="???" &&
            game.runtime()->term_unlocked(0) && game.runtime()->voice_played(17) &&
            game.take_system_save_request(),"system instructions and filtered title ordering");
        const int term=modal->request;
        require(game.act_modal(term,1,2)==0 && game.state().modal->text==std::vector<std::string>{"Alpha","description"} &&
            game.runtime()->term_read(1) && !game.runtime()->term_unlocked(1) && game.take_system_save_request(),
            "the source permits inspecting a locked term without unlocking it");
        require(game.act_modal(term,5,1)==0 && game.state().modal->group==1 && game.state().modal->text.empty() &&
            game.state().modal->rows[0].id==4 && game.act_modal(term,5,0)==0 &&
            game.act_modal(term,2,1)==0 && game.state().modal->rows[0].flags==(scene::ScriptModalRow::Enabled|scene::ScriptModalRow::New) &&
            game.act_modal(term,1,1)==0 &&
            game.state().modal->rows[0].flags==(scene::ScriptModalRow::Enabled|scene::ScriptModalRow::Selected),
            "tab/page reset and first read markers");
        require(game.act_modal(term,0,0)==2,"term back resumes its script caller");
        modal=game.step_frame(0).modal;
        require(modal && modal->mode==nnn::RuntimeMode::ListenVoice && modal->return_mode==nnn::RuntimeMode::Omake &&
            modal->rows[0].asset=="line" && game.act_modal(modal->request,1,1)==0 &&
            game.audio().state().voices[0].active,"voice flag and ch0 replay");
        require(game.abort_modal(modal->request,"unavailable background")==0 &&
            game.step_frame(0).wait==scene::Wait::Dialogue,"a failed script modal resumes, and unavailable classes do not stall");
        nnn::SystemEvent legacy;
        legacy.opcode=nnn::audio::set_voice_flag;legacy.parameters={23};
        require(game.audio().submit(legacy).empty() && !game.runtime()->voice_played(23),"legacy audio-only flag fixture");
        const auto snapshot=game.snapshot();
        temp.write("spt/voiceflag.tbl",Bytes{0});
        scene::GameSession degraded(temp.root,options);
        degraded.runtime()->set_voice_played(42);
        degraded.restore_snapshot(snapshot);
        require(degraded.runtime()->voice_played(23) && degraded.runtime()->voice_played(42),
            "older audio flags migrate without replacing global progress");
        require(degraded.open_modal(nnn::RuntimeMode::ListenVoice,nnn::RuntimeMode::Title,true),"invalid voice table can retain normal rows");
        modal=degraded.state().modal;
        temp.write("cdvaw/ra/raw.vaw",Bytes{0}); // The indexed source becomes undecodable before replay.
        require(modal && modal->pages==1 && modal->rows.size()==1 && modal->rows[0].id==2 &&
            degraded.act_modal(modal->request,1,2)==3 &&
            degraded.abort_modal(modal->request,"view unavailable")==static_cast<int>(nnn::RuntimeMode::Title) &&
            degraded.state().wait==scene::Wait::Dialogue,"last physical source failure restores the caller and story wait");
        temp.text("spt/termList.xtx","Missing,1,0,absent\n");
        temp.text("nya/charaVoiceList1.xtx","1,line,NeedsFlags\n");
        scene::GameSession unavailable(temp.root,options);
        require(unavailable.step_frame(0).wait==scene::Wait::Dialogue,"entirely invalid catalogues must not suspend the script");
    });
    suite.test("result acknowledgements and achievements retain PC progression and result hover voice is one-shot",[]{
        Model model;model.message=true;
        model.code.user(0);model.code.user(1);model.code.print(1);model.code.end();
        TempGame temp(model);
        temp.text("init/game.xtx","codeByte 1\nresultFlag 1\nPrintResultIsDefault 1\nPrintAchievementIsDefault 1\n");
        temp.text("spt/command.xtx","printresult,0\nprintachievement,0\n");
        temp.text("init/printResult.xtx","resultsMax 2\nresultsNumberX 1\nresultsNumberY 1\n"
            "dialogFlag 1\nvoiceFlag 1\nvoiceStartWait 2\nnextMode title\n");
        temp.text("nya/resultDesc.xtx","dummy,!,fixed,@,@,@,@,@\nOne,line,First,@,@,@,@,@\nTwo,!,Second,@,@,@,@,@\n");
        temp.text("init/printAchievement.xtx","achievementNumber 2\nachievementType1 endingrange\n"
            "achievementParamNumber1 2\nachievement1Param1 0\nachievement1Param2 1\n"
            "achievementType2 ending\nachievementParamNumber2 1\nachievement2Param1 2\n");
        temp.write("cdvaw/li/line.vaw",pcm_vaw(1,44100,8,{188,198,208,218}));
        auto options=test_options();
        options.runtime=std::make_shared<nnn::RuntimeContext>(temp.root);
        options.runtime->set_result_if_clear(1,1);options.runtime->set_result_if_clear(2,1);
        options.runtime->set_achievement(0,100);options.runtime->set_achievement(1,100);
        options.runtime->set_achievement(2,25);
        scene::GameSession game(temp.root,options);
        auto modal=game.step_frame(0).modal;
        require(modal && modal->mode==nnn::RuntimeMode::MyPrintResult && modal->selection==1 &&
            modal->return_mode==nnn::RuntimeMode::Title && modal->rows[0].flags==scene::ScriptModalRow::New,
            "first pending result and configured return mode");
        const int first=modal->request;
        require(game.act_modal(first,0,0)==-1 && game.act_modal(first,4,2)==-1 &&
            game.act_modal(first,4,1)==0 && game.runtime()->result_value(1)==2 &&
            game.state().modal->page==1 && game.state().modal->selection==2 &&
            game.act_modal(first,4,2)==-1,"ordered acknowledgements and stale request rejection");
        require(game.abort_modal(game.state().modal->request,"missing dialog art")==0 &&
            game.runtime()->result_value(2)==1,"failure preserves unacknowledged progress");
        require(game.open_modal(nnn::RuntimeMode::MyPrintResult,nnn::RuntimeMode::Omake),
            "an external visit restores the real pending result");
        require(game.act_modal(game.state().modal->request,4,2)==0,"confirm the remaining result");
        const int hover=game.state().modal->request;
        require(game.act_modal(hover,2,0)==0 && game.act_modal(hover,3,1)==0 &&
            game.state().modal->group==1 && game.state().modal->text==std::vector<std::string>{"First"},
            "result description tracks the currently hovered row");
        const auto plays=game.audio().stats().plays;
        game.advance_modal(hover,1);
        require(game.audio().stats().plays==plays,"voice played before source wait");
        game.advance_modal(hover,1);game.advance_modal(hover,20);
        require(game.audio().stats().plays==plays+1 && game.act_modal(hover,3,0)==0 &&
            game.state().modal->text.empty() && game.act_modal(hover,0,0)==2,"sustained hover plays exactly once");
        modal=game.step_frame(0).modal;
        require(modal && modal->mode==nnn::RuntimeMode::PrintAchievement && modal->return_mode==nnn::RuntimeMode::Omake &&
            modal->rows.size()==2 && modal->rows[0].flags==scene::ScriptModalRow::Selected &&
            modal->rows[0].detail=="100" && modal->rows[1].detail=="25","source achievement completion and percentages");
    });
    suite.test("heroine carousel and object attachments only write original choices and restore selection state",[]{
        Model model;model.message=true;
        model.code.user(0);model.code.print(1);model.code.user(1,{2});model.code.print(2);
        model.code.user(0);model.code.print(3);model.code.end();
        TempGame temp(model);
        temp.text("init/game.xtx","SelectHeroinIsDefault 1\nSelectObjectIsDefault 1\n"
            "PrintStatusClassDisable 0\nPrintPartyClassDisable 0\nexpModeCheckByVarNumber 4\n"
            "expModeCheckByVarMode1 38\nexpModeCheckByVarName1 faceB\n"
            "expModeCheckByVarMode2 38\nexpModeCheckByVarName2 sysAppearB\n"
            "expModeCheckByVarMode3 49\nexpModeCheckByVarName3 faceA\n"
            "expModeCheckByVarMode4 43\nexpModeCheckByVarName4 unresolved\n");
        temp.text("spt/command.xtx","selectheroin,0\nselectobject,1\n");
        std::string variables="faceA\nfaceB\nnext\nheroResult\ntype\nobjResult\naddResult\nhidden\nlocked\naddA\naddB\nhover\n";
        for (int id=12;id<200;++id) variables+="v"+std::to_string(id)+"\n";
        variables+="sysFaceA\nsysFaceB\nsysNext\nsysAppearA\nsysAppearB\n";
        Bytes encoded(variables.begin(),variables.end());
        for (auto& byte:encoded) byte^=0xffU;
        temp.write("spt/var.fxf",encoded);
        temp.text("spt/varinitdata.xtx","0\n2\n1\n9\n0\n7\n8\n0\n0\n1\n1\n0\n");
        temp.text("init/printStatus.xtx","set1_1VarName faceB\n");
        temp.text("init/printParty.xtx","oldVersion 1\nparty1Filename a\n");
        temp.text("init/selectHeroin.xtx","heroinPrintMax 2\nheroinVarName1 sysFaceA\nheroinVarName2 sysFaceB\n"
            "nextButtonFlag 1\nnextButtonVarName sysNext\nreturnVarName heroResult\n");
        temp.text("nya/heroin.xtx","a\nb\nc\nd\n");
        temp.write("ta/a.dwq",asset("tiny_red"));temp.write("ta/d.dwq",asset("tiny_green"));
        temp.text("init/selectObject.xtx","objectTypeVarName type\nselectObjectMax 2\nresultVarName objResult\n"
            "resultAddVarName addResult\nonButtonVarName hover\nautoPicCannotOk 1\n");
        temp.text("init/selectObjectType2.xtx","objectNumber 2\naddObjectNumber 2\nobjectSelectOkVarName1 locked\n"
            "objectAppearVarName1 sysAppearA\nobjectAppearVarName2 sysAppearB\naddVarName1 addA\naddVarName2 addB\n");
        auto options=test_options();
        options.runtime=std::make_shared<nnn::RuntimeContext>(temp.root);
        options.runtime->set_system_variable(0,1);options.runtime->set_system_variable(1,2);
        options.runtime->set_system_variable(2,1);options.runtime->set_system_variable(3,1);
        scene::GameSession game(temp.root,options);
        auto modal=game.step_frame(0).modal;
        require(modal && modal->mode==nnn::RuntimeMode::SelectHeroin && modal->group==3 && modal->selection==1 &&
            modal->rows.size()==2 && modal->rows[1].asset=="ta/d","original face/slot mapping and Next flag");
        const auto hero_request=modal->request;
        require(game.act_modal(hero_request,2,1)==0 && game.state().modal->selection==2 &&
            game.variable(3)==9 && game.modal_savable(),"rotation only changes selection until OK");
        using Mode=nnn::RuntimeMode;
        const auto hero_word=game.state().word;
        require(!game.menu_extension_enabled(Mode::PrintStatus) && game.menu_extension_enabled(Mode::PrintParty) &&
            game.menu_extension_enabled(Mode::PrintTerm) && !game.open_modal(Mode::PrintStatus,Mode::SelectHeroin),
            "a last bound extension gate uses live SYS while unresolved and index zero allow access");
        options.runtime->set_system_variable(4,1);
        require(game.menu_extension_enabled(Mode::PrintStatus) && !game.open_modal(Mode::PrintStatus,Mode::PrintMessage) &&
            game.open_modal(Mode::PrintStatus,Mode::SelectHeroin),"only the real modal caller can open a child");
        const auto child=game.state().modal->request;
        require(child!=hero_request && game.state().modal->return_mode==Mode::SelectHeroin && !game.modal_savable() &&
            !game.open_modal(Mode::PrintParty,Mode::PrintStatus) && game.act_modal(hero_request,4,2)==-1 &&
            game.step_frame(0).word==hero_word,"only the child owns input while the same VM remains suspended");
        require(game.act_modal(child,0,0)==2 && game.state().modal->request==hero_request &&
            game.state().modal->selection==2 && game.mode()==static_cast<int>(Mode::SelectHeroin) &&
            game.abort_modal(child,"stale child")==-1 && game.modal_savable(),
            "child Back restores the exact parent request, selection and save state");
        require(game.open_modal(Mode::PrintStatus,Mode::SelectHeroin) &&
            game.abort_modal(game.state().modal->request,"missing child art")==static_cast<int>(Mode::SelectHeroin) &&
            game.state().modal->request==hero_request && game.state().word==hero_word,
            "unavailable child returns to the parent without stepping the interpreter");
        options.runtime->set_system_variable(4,0);
        const auto pending_hero=game.snapshot();
        require(game.act_modal(hero_request,6,0)==0 && game.state().modal->selection==2,
            "menu return revalidates the carousel while retaining its selection");
        options.runtime->set_system_variable(1,0);
        game.restore_snapshot(pending_hero);
        require(game.state().modal && game.state().modal->selection==1 && game.variable(201)==0 && game.variable(3)==9,
            "live SYS can hide a saved heroine without invalidating the slot");
        options.runtime->set_system_variable(0,0);options.runtime->set_system_variable(2,0);
        game.restore_snapshot(pending_hero,[](const auto& saved){return saved.wait==scene::Wait::Modal;});
        require(game.state().wait==scene::Wait::Running && game.variable(3)==9,
            "no live heroine or Next consumes only the already-dispatched instruction");
        options.runtime->set_system_variable(0,1);options.runtime->set_system_variable(1,2);
        options.runtime->set_system_variable(2,1);
        game.restore_snapshot(pending_hero);
        modal=game.state().modal;
        require(modal && modal->request!=hero_request && modal->selection==2 && game.variable(3)==9 &&
            game.act_modal(hero_request,4,2)==-1 && game.act_modal(modal->request,4,1)==-1 &&
            game.act_modal(modal->request,4,2)==2 && game.variable(3)==2,
            "saved carousel waits at the same PC with a new request and does not commit until OK");
        const auto dialogue=game.step_frame(0);
        require(dialogue.wait==scene::Wait::Dialogue && game.open_modal(Mode::PrintParty,Mode::PrintMessage) &&
            game.state().modal->return_mode==Mode::PrintMessage && game.act_modal(game.state().modal->request,0,0)==2 &&
            game.state().wait==scene::Wait::Dialogue && game.state().word==dialogue.word &&
            game.state().dialogue.sequence==dialogue.dialogue.sequence,"a direct live extension returns to the same dialogue");
        scene::InputState advance;advance.advance=true;
        modal=game.step_frame(0,advance).modal;
        require(modal && modal->mode==nnn::RuntimeMode::SelectObject && modal->group==2 && modal->rows.size()==3 &&
            modal->rows[0].id==1 && modal->rows[0].flags==0 && modal->rows[1].id==-1 && modal->rows[1].flags==0 &&
            modal->rows[2].id==-2 && modal->rows[2].detail=="1" && game.variable(4)==2,
            "typed object list and last attachment mapping");
        const auto object_request=modal->request;
        require(game.act_modal(object_request,1,1)==-1 && game.act_modal(object_request,1,-1)==-1 &&
            game.act_modal(object_request,3,1)==0 && game.variable(11)==1 && game.variable(5)==7 && game.modal_savable(),
            "invalid choices and hover do not commit results");
        const auto pending_object=game.snapshot();
        require(game.act_modal(object_request,6,0)==0 && game.state().modal->selection==0 &&
            game.variable(11)==0 && game.variable(5)==7,"object menu return clears hover and rebuilds choices");
        auto corrupt=pending_object;
        nnn::snapshot::Reader prefix(corrupt);
        prefix.raw(9);
        std::uint32_t period=0,word=0,remaining=0;std::uint8_t wait=0;std::string script_name;
        prefix(period,wait,script_name,word,remaining);
        le32(corrupt,corrupt.size()-prefix.remaining(),static_cast<Word>(nnn::RuntimeMode::SelectHeroin));
        bool rejected=false;
        try { game.restore_snapshot(corrupt); } catch (const std::runtime_error&) { rejected=true; }
        require(rejected && game.variable(11)==0 && game.variable(5)==7,"mismatched saved modal cannot commit state");
        options.runtime->set_system_variable(3,0);options.runtime->set_system_variable(4,1);
        game.restore_snapshot(pending_object);
        require(game.state().modal && game.state().modal->selection==0 && game.variable(11)==0 &&
            game.state().modal->rows[0].id==2 && game.variable(203)==0 && game.variable(5)==7,
            "live SYS hides the saved object hover without overwriting current globals");
        options.runtime->set_system_variable(4,0);
        game.restore_snapshot(pending_object);
        require(game.state().wait==scene::Wait::Running && game.variable(5)==7 && game.variable(6)==8,
            "no live object safely resumes without writing either result");
        options.runtime->set_system_variable(3,1);
        game.restore_snapshot(pending_object);
        modal=game.state().modal;
        require(modal && modal->request!=object_request && modal->selection==1 &&
            modal->arguments==std::vector<nnn::Value>{2} && game.variable(11)==1 &&
            game.act_modal(modal->request,1,-2)==2 && game.variable(5)==1 && game.variable(6)==2,
            "attachment can choose an otherwise unselectable visible object");
        require(game.step_frame(0).wait==scene::Wait::Dialogue,"continue after object");
        const auto native=game.snapshot(),original=game.export_pc_save();
        auto old_native=native;
        le32(old_native,4,8);old_native.erase(old_native.begin()+8); // Exact v8 header and unchanged remaining fields.
        game.restore_snapshot(old_native);
        modal=game.step_frame(0,advance).modal;
        require(modal && modal->selection==1 && game.abort_modal(modal->request,"missing controls")==0 &&
            game.variable(3)==2,"v8 reader defaults to first portrait and fallback preserves result");
        game.restore_snapshot(native);
        modal=game.step_frame(0,advance).modal;
        require(modal && modal->selection==2 && game.act_modal(modal->request,4,0)==2 && game.variable(3)==1,
            "v9 restores the original portrait while explicit Next writes 1");
        game.restore_pc_save(original);
        modal=game.step_frame(0,advance).modal;
        require(modal && modal->selection==2,"PC STATUS restores last heroine");
    });
    suite.test("inventory inspection is read only and shop confirms both original variable writes together",[]{
        Model model;model.message=true;model.code.user(0);model.code.user(1);model.code.print(1);model.code.end();
        TempGame temp(model);
        temp.text("init/game.xtx","PrintItemIsDefault 1\nShopIsDefault 1\n");
        temp.text("spt/command.xtx","printitem,0\nshop,0\n");
        std::string variables="amount\nshown\nseen\n";
        for (int id=3;id<200;++id) variables+="v"+std::to_string(id)+"\n";
        variables+="points\nstock\n";
        Bytes encoded(variables.begin(),variables.end());
        for (auto& byte:encoded) byte^=0xffU;
        temp.write("spt/var.fxf",encoded);
        temp.text("spt/varinitdata.xtx","2\n1\n0\n");
        temp.text("init/printItem.xtx","itemBlockX 1\nitemBlockY 1\n");
        temp.text("nya/haveItem.xtx","%7,1,amount,owned,One,Base,!Two,Variant\n");
        temp.text("init/shop.xtx","angouFlag 0\npointVarName points\nitemBlockX 1\nitemBlockY 1\n"
            "appearVar1 seen\ncanSelectLimit 1\ncanSelectNoPoint 1\n");
        temp.text("nnndir/shopItem.txt","%1,1,3,1,Potion,potion,shown,stock,Description\n"
            "%2,1,99,0,Large,large,shown,stock,TooMuch\n");
        auto options=test_options();
        options.runtime=std::make_shared<nnn::RuntimeContext>(temp.root);
        options.runtime->set_system_variable(0,7);
        scene::GameSession game(temp.root,options);
        auto modal=game.step_frame(0).modal;
        require(modal && modal->mode==nnn::RuntimeMode::PrintItem && modal->rows[0].label=="Two" &&
            modal->rows[0].asset=="sys/owned_2" && game.act_modal(modal->request,1,1)==0 &&
            game.state().modal->text==std::vector<std::string>{"Variant"} && game.variable(0)==2,
            "quantity-specific detail must not change the inventory");
        require(game.act_modal(game.state().modal->request,0,0)==0 &&
            game.state().modal->selection==0 && game.act_modal(game.state().modal->request,0,0)==2,
            "item detail Back stays on the same page before leaving");
        modal=game.step_frame(0).modal;
        require(modal && modal->mode==nnn::RuntimeMode::Shop && modal->balance==7 && modal->pages==2 &&
            (modal->rows[0].flags&scene::ScriptModalRow::PurchaseEnabled)!=0,"shop points and affordability");
        require(game.act_modal(modal->request,1,1)==0 && game.variable(2)==1 &&
            game.variable(200)==7 && game.variable(201)==0 &&
            game.act_modal(game.state().modal->request,0,0)==0 && game.variable(200)==7,
            "inspection sets only the configured seen bit; Cancel does not purchase");
        require(game.act_modal(game.state().modal->request,1,1)==0,"choose after cancel");
        const int buy=game.state().modal->request;
        require(game.act_modal(buy,4,2)==-1 && game.act_modal(buy,4,1)==0 &&
            game.act_modal(buy,4,1)==-1 && game.variable(200)==4 && game.variable(201)==1 &&
            game.take_system_save_request() && game.state().modal->balance==4 &&
            (game.state().modal->rows[0].flags&scene::ScriptModalRow::SoldOut)!=0,
            "successful purchase atomically updates global point and inventory vars once");
        require(game.act_modal(game.state().modal->request,2,1)==0 &&
            game.act_modal(game.state().modal->request,1,2)==0 &&
            game.act_modal(game.state().modal->request,4,2)==0 && game.variable(200)==4 &&
            game.variable(201)==1,"unaffordable item can be inspected but never purchased");
    });
    suite.test("status preserves signed order and party commits only bound controls",[]{
        Model model;model.message=true;model.code.user(0);model.code.user(1);model.code.print(1);model.code.end();
        TempGame temp(model);
        temp.text("init/game.xtx","PrintStatusIsDefault 1\nPrintPartyIsDefault 1\n");
        temp.text("spt/command.xtx","printstatus,0\nprintparty,0\n");
        std::string names="placeA\nplaceB\nlayout\nparam\nextra\nvisible\nmember\nhp\netcData\ntextVar\n";
        for (int id=10;id<200;++id) names+="v"+std::to_string(id)+"\n";
        names+="globalGraph\n";
        Bytes encoded(names.begin(),names.end());
        for (auto& byte:encoded) byte^=0xffU;
        temp.write("spt/var.fxf",encoded);
        temp.text("spt/varinitdata.xtx","-1\n2\n0\n1\n-1\n-1\n1\n17\n23\n3\n");
        const std::string controls="setNumber 2\ncontrolVarName1 placeA\ncontrolVarName2 placeB\n"
            "setPlaceSetNumber 2\nsetPlaceSelectVarName layout\nuseRadio 1\n";
        temp.text("init/printStatus.xtx",controls+"effectTime 2\nset1_1VarName hp\nset2_1VarName globalGraph\n"
            "freePrintFlag 1\ntitleIsFreePrint 1\nprintoverrapIsFreePrint 2\n");
        temp.text("init/printParty.xtx",controls+"effectTime 99\nparamOffVarName1 param\netc1ControlVarName extra\n"
            "freePrintFlag 1\ntitleIsFreePrint 1\nprintmessageIsFreePrint -1\n");
        temp.text("init/faceStatusList.xtx","statusPrintVarName visible\nstatus1PlayerVarName member\n"
            "textNumber 2\ntext1Type 3\ntext1_1VarName textVar\ntext2Type 2\ntext2_1VarName textVar\n"
            "suujiNumber 1\ndata1_1VarName hp\n"
            "etcNumber 1\netc1VarName etcData\n");
        auto options=test_options();
        options.runtime=std::make_shared<nnn::RuntimeContext>(temp.root);
        require(options.runtime->set_system_replacement(3,"Native text"),"seed original replacement");
        options.runtime->set_system_variable(0,61);
        auto story=std::make_shared<nnn::PreviewStoryState>();
        story->vars[0]=-2;story->vars[1]=1;story->vars[2]=1;story->vars[5]=-1;
        story->vars[6]=1;story->vars[7]=99;story->vars[200]=1234;
        story->replacements={"","","","Story text"};
        options.runtime->set_preview_story(story);
        const auto detail=[](const scene::ScriptModal& modal,const std::string& key) {
            for (const auto& row:modal.rows) if (row.label==key) return row.detail;
            return std::string{};
        };
        scene::GameSession game(temp.root,options);
        const auto mode=game.mode();
        auto overlay=game.status_overlay(nnn::RuntimeMode::PrintStatus,nnn::RuntimeMode::Title,true);
        require(overlay && overlay->request==0 && overlay->selection==100 && overlay->group==1 &&
            detail(*overlay,"set1_1varname")=="99" && detail(*overlay,"set2_1varname")=="61" &&
            (overlay->rows[0].flags&scene::ScriptModalRow::Enabled)==0,
            "title free print reads committed story locals and live system variables");
        overlay=game.status_overlay(nnn::RuntimeMode::PrintParty,nnn::RuntimeMode::Title,true);
        require(overlay && detail(*overlay,"local3")=="Story text" && detail(*overlay,"system3")=="Native text" &&
            game.variable(7)==17 && game.mode()==mode && !game.state().modal &&
            !game.status_overlay(nnn::RuntimeMode::PrintParty,nnn::RuntimeMode::PrintMessage) &&
            game.status_overlay(nnn::RuntimeMode::PrintStatus,nnn::RuntimeMode::PrintOverlap),
            "free print is read only and uses the original per-mode names and positive flags");
        auto modal=game.step_frame(0).modal;
        require(modal && modal->mode==nnn::RuntimeMode::PrintStatus && modal->rows[0].detail=="-1" &&
            modal->selection==0 && detail(*modal,"set1_1varname")=="17" &&
            game.act_modal(modal->request,2,1)==-1 && game.variable(0)==-1,"entry does not mutate order and opening gates edits");
        game.advance_modal(modal->request,1);
        require(game.state().modal->selection==50 && game.act_modal(modal->request,2,1)==-1,"effect counts original frames");
        game.advance_modal(modal->request,1);
        require(game.act_modal(modal->request,2,1)==0 && game.variable(0)==-2 && game.variable(1)==1 &&
            game.act_modal(modal->request,1,1)==-1,"swap keeps each sign and rejects a stale press");
        require(game.act_modal(game.state().modal->request,5,1)==0 && game.variable(2)==1 &&
            game.state().modal->group==1 && game.act_modal(game.state().modal->request,0,0)==2,"layout writes zero-based and Back resumes");
        modal=game.step_frame(0).modal;
        require(modal && modal->mode==nnn::RuntimeMode::PrintParty && modal->rows[0].detail=="-2" &&
            detail(*modal,"statusprintvarname")=="-1" && detail(*modal,"data1_1varname")=="17" &&
            detail(*modal,"system3")=="Native text" && modal->selection==100,"modern data and nonzero draw gate");
        require(game.act_modal(modal->request,1,-1)==0 && game.variable(3)==0 && game.variable(4)==-1 &&
            game.act_modal(game.state().modal->request,1,-1001)==0 && game.variable(4)==2 &&
            game.variable(0)==-2,"party param and etc switches use one minus current without rewriting unrelated data");
        require(game.act_modal(game.state().modal->request,0,0)==2 && game.step_frame(0).wait==scene::Wait::Dialogue,
            "party leaves to the suspended script");

        TempGame old(model);
        old.text("init/game.xtx","PrintStatusIsDefault 1\nPrintPartyIsDefault 1\n");
        old.text("spt/command.xtx","printstatus,0\nprintparty,0\n");
        old.text("init/printStatus.xtx","setNumber 0\n");
        old.text("init/printParty.xtx","oldVersion 1\nprintNumber 2\nstatusNumber 1\n"
            "printByVar 1\nprint1Var unknown\nparty1_1Var unknown\nparty2Filename sys_face\n"
            "freePrintFlag 1\ntitleIsFreePrint 1\n");
        scene::GameSession legacy(old.root,test_options());
        require(!legacy.status_overlay(nnn::RuntimeMode::PrintParty,nnn::RuntimeMode::Title),
            "legacy party stays in its read-only modal");
        modal=legacy.step_frame(0).modal;
        require(modal && modal->mode==nnn::RuntimeMode::PrintParty && detail(*modal,"print1var")=="1" &&
            detail(*modal,"print2var")=="2" && detail(*modal,"party1_1var")=="-1" &&
            legacy.act_modal(modal->request,1,1)==-1 && legacy.act_modal(modal->request,0,0)==2,
            "empty graphs pass through and the old party view retains its read-only roster defaults");
    });
    suite.test("chart sentinel checkpoints, pending snapshots and validated branch rewinds",[]{
        Model model;model.message=true;
        model.command_adjust.push_back(model.code.add({5,0x66660003U,0,0,0}));
        model.code.add({5,0x66660004U,0,1,0x90000002U});
        model.code.add({6,0x66660004U,1,2,0x900003e8U,0x90000000U});
        model.code.print(1);
        model.command_adjust.push_back(model.code.add({5,0x66660003U,0,0,1}));
        model.code.print(2);
        model.code.end();
        TempGame temp(model);
        temp.text("init/game.xtx","SelectChartIsDefault 1\nclearAutoAfterLoad 1\nPrintStatusClassDisable 0\n");
        temp.text("init/printStatus.xtx","set1_1VarName now\n");
        temp.text("init/selectChart.xtx","chartListNumber 2\nbasicChartNumber 2\nautoNextOn 1\n"
            "slotSizeX 1\nslotSizeY 1\nslotAreaStartX 0\nslotVarName now\nworkVarName prior\n"
            "savePrintChartFlag 1\nworkVarName2 page\nnewMarkPrintFlag 1\nnewMarkVarName marks\n"
            "setRouteFunctionName reach\nuseSlotOnFunction 1\n");
        temp.text("chart/chart1.xtx","1800\n@1 1 20 30 1000 Root\n");
        temp.text("chart/chart2.xtx","1800\n@1000 1 20 40 1001 Start\n@1001 2 30 55 End\n");
        temp.text("spt/command.xtx","selectchart,0\n");
        temp.text("spt/function.xtx","reach,d,sloton,d,");
        const std::string vars="now\nprior\npage\nmarks\n";
        Bytes var_bytes(vars.begin(),vars.end());
        for (auto& byte:var_bytes) byte^=0xffU;
        temp.write("spt/var.fxf",var_bytes);
        temp.write("sys/ta_chart_button1_1.dwq",asset("tiny_red"));
        auto options=test_options();options.width=128;options.height=96;
        const auto storage=temp.root/"private";
        scene::GameSession game(temp.root,options);
        game.set_chart_storage(storage);
        auto modal=game.step_frame(0).modal;
        require(modal && modal->chart && modal->selection==1000 && modal->page==1 && game.variable(0)==1000 &&
            game.variable(1)==1000 && game.variable(2)==1 && game.modal_savable(),
            "initial sentinel checkpoints node 1 then follows its single cross-page edge");
        const nnn::chart::Graph graph(temp.root,128,96);
        const auto initial=read_file(storage/"chart1/slot1.dat");
        const auto moved=scene::parse_chart_node(read_file(storage/"chart2/slot1000.dat"),graph);
        require(scene::parse_chart_node(initial,graph).vars[1]==0 && moved.run.move_road[0]==1 && moved.vars[2]==1 &&
            game.runtime()->chart_progress()->seen[1]==1 && game.runtime()->chart_progress()->seen[1000]==1,
            "PC node checkpoints and global seen flags are separate");
        const auto snapshot=game.snapshot();
        const auto request=modal->request;
        require(snapshot[4]==10 && game.act_modal(request,7,60)==0 && game.state().modal->chart->scroll==60,
            "chart uses the versioned native save and authoritative scroll");
        require(game.open_modal(nnn::RuntimeMode::PrintStatus,nnn::RuntimeMode::SelectChart) &&
            game.act_modal(game.state().modal->request,0,0)==2 && game.state().modal->request==request &&
            game.state().modal->chart->scroll==60 && game.act_modal(request,7,59)==0,
            "a nested status view restores the chart visit and its native scroll state");
        game.restore_snapshot(snapshot);
        modal=game.state().modal;
        require(modal && modal->selection==1000 && modal->chart->scroll==0 && game.act_modal(request,7,0)==-1,
            "restored chart retains its page and replaces the request identity");
        const auto pointer=[](int x,int y){return x*65536+y;};
        const auto current=[&]{return game.state().modal->request;};
        require(game.act_modal(current(),2,0)==0 && game.act_modal(current(),3,pointer(20,30))==0 &&
            game.state().modal->chart->description==std::vector<std::string>{"Root"} &&
            game.act_modal(current(),1,pointer(20,30))==0 && game.state().modal->chart->pending==1 &&
            !game.modal_savable() && game.act_modal(current(),4,0)==0 && game.variable(0)==1000,
            "warning cancellation stays on the chart without altering the route");
        game.set_system_parameter(nnn::NNNPARAM_AUTOMODE,1);
        game.set_skip_active(true);
        require(game.act_modal(current(),2,1)==0 && game.act_modal(current(),1,pointer(30,55))==4 &&
            game.variable(0)==1000 && !game.modal_savable() && game.act_modal(current(),8,0)==2 &&
            game.variable(0)==1001 && game.variable(1)==1000 && game.variable(3)==2 &&
            game.get_system_parameter(nnn::NNNPARAM_AUTOMODE)==1 && game.skip_active() &&
            game.step_frame(0).wait==scene::Wait::Dialogue && game.runtime()->chart_progress()->seen[1001]==2,
            "forward exit resumes the live VM and the configured setroute alias records a multi-route flag");
        scene::InputState advance;advance.advance=true;
        modal=game.step_frame(0,advance).modal;
        require(modal && modal->selection==1001 && game.act_modal(current(),2,0)==0 &&
            game.act_modal(current(),1,pointer(20,23))==0,"select a seen earlier node from the second chart call");
        constexpr auto word=1000U/32U;
        constexpr auto bit=Word{1}<<(1000U%32U);
        const auto selection=game.runtime()->chart_selection();
        require(selection && selection->slots==graph.slot_capacity() && (selection->enabled[word]&bit)==0,
            "sloton changed the current process selection flags");
        auto fresh_options=options;
        fresh_options.runtime=game.runtime()->fork();
        scene::GameSession fresh(temp.root,fresh_options);
        fresh.set_chart_storage(temp.root/"fresh");
        require(fresh.step_frame(0).wait==scene::Wait::Modal,"new game can enter the chart");
        const auto fresh_run=scene::parse_chart_node(read_file(temp.root/"fresh/chart2/slot1000.dat"),graph).run;
        require((fresh_run.can_select[word]&bit)==0 && fresh_run.move_road[1000]==0 &&
            moved.run.move_road[0]==fresh_run.move_road[0],"new game keeps gate flags and resets only prior roads");
        // The intervening unread dialogue has already stopped read-only skip.
        game.set_skip_active(true);
        temp.text("private/chart1/slot1.dat","invalid checkpoint");
        require(game.act_modal(current(),4,1)==4 && game.variable(0)==1001 && game.act_modal(current(),8,0)==0 &&
            game.state().modal->chart->pending==-1 &&
            game.variable(0)==1001 && game.runtime()->chart_progress()->seen[1001]==2 &&
            game.runtime()->chart_selection()==selection && game.get_system_parameter(nnn::NNNPARAM_AUTOMODE)==1 &&
            game.skip_active(),
            "corrupt node never changes the route, interpreter, or global progress");
        temp.write("private/chart1/slot1.dat",initial);
        require(game.act_modal(current(),1,pointer(20,23))==0 && game.act_modal(current(),4,1)==4 &&
            game.act_modal(current(),8,0)==2 &&
            game.variable(0)==1 && game.variable(1)==0 && (game.runtime()->chart_selection()->enabled[word]&bit)!=0 &&
            game.get_system_parameter(nnn::NNNPARAM_AUTOMODE)==0 && !game.skip_active() &&
            game.step_frame(0).wait==scene::Wait::Dialogue &&
            game.runtime()->chart_progress()->seen[1001]==2,"validated revisit restores PC and locals while global progress survives");

        Model title_code;
        title_code.code.add({8,0x66660005U,0,0xa0000000U,3,0x800005dcU,0x90000063U,0xa000003dU});
        title_code.code.image(4);title_code.code.number(effect,{4,0});title_code.code.present();title_code.code.end();
        temp.write("spt/titlebg.spt",script(title_code));
        temp.write("dwq/red.dwq",asset("tiny_red"));
        auto title_options=options;
        title_options.runtime=std::make_shared<nnn::RuntimeContext>(temp.root);
        title_options.runtime->set_chart_progress(*game.runtime()->chart_progress());
        auto title_seed=std::make_shared<nnn::PreviewStoryState>();
        title_seed->vars[0]=1;title_seed->vars[1500]=77;
        title_options.runtime->set_preview_story(title_seed);
        scene::GameSession title(temp.root,title_options);
        title.set_chart_storage(storage);
        title.start_ui_background("titlebg",static_cast<int>(nnn::RuntimeMode::Title));
        title.step_ui_background({},false);
        const auto title_request=[&]{return title.state().modal->request;};
        temp.text("private/chart1/slot1.dat","invalid checkpoint");
        require(title.open_modal(nnn::RuntimeMode::SelectChart,nnn::RuntimeMode::Omake,true) &&
            title.act_modal(title_request(),2,1)==0 && title.act_modal(title_request(),1,pointer(20,40))==4 &&
            title.act_modal(title_request(),8,0)==0 && title.mode()==static_cast<int>(nnn::RuntimeMode::Title) &&
            title.variable(1500)==99 && title.layers()[4].texture && title.act_modal(title_request(),0,0)==2,
            "failed forward resume preserves the live title VM and returns to the external caller");
        temp.write("private/chart1/slot1.dat",initial);
        title.set_system_parameter(nnn::NNNPARAM_AUTOMODE,1);
        require(title.open_modal(nnn::RuntimeMode::SelectChart,nnn::RuntimeMode::Omake,true) &&
            title.act_modal(title_request(),1,pointer(20,40))==4 && title.act_modal(title_request(),8,0)==5 &&
            title.state().wait==scene::Wait::Running && !title.layers()[4].texture &&
            title.variable(0)==1000 && title.variable(1)==1 && title.variable(1500)==77 &&
            title.get_system_parameter(nnn::NNNPARAM_AUTOMODE)==1 && title.step_frame(0).wait==scene::Wait::Dialogue,
            "validated title forward promotes the current checkpoint and follows the selected successor");
    });
    suite.test("external chart preserves a timed choice until an adjacent branch commits",[]{
        Model model;model.message=true;
        model.code.add({11,command,0x22220006U,1,0,0,3,0,2,250,0});
        model.code.add({4,control,0x55560008U,1});
        model.code.add({8,0x66660005U,0,0xa0000000U,3,0x80000002U,0x9000006fU,0xa000003dU});
        model.code.add({4,control,0x55560008U,2});
        model.code.add({8,0x66660005U,0,0xa0000000U,3,0x80000002U,0x900000deU,0xa000003dU});
        model.code.print(77);model.code.end();
        TempGame temp(model);
        temp.text("init/game.xtx","SelectChartIsDefault 1\n");
        temp.text("init/selectChart.xtx","chartListNumber 1\nbasicChartNumber 1\nslotSizeX 1\nslotSizeY 1\n"
            "slotAreaStartX 0\nslotVarName now\nworkVarName prior\n");
        temp.text("chart/chart1.xtx","1800\n@1 1 20 30 2 Start\n@2 1 30 40 Next\n@3 1 45 42 Earlier\n");
        const std::string names="now\nprior\ncase_result\n";
        Bytes encoded(names.begin(),names.end());
        for(auto& byte:encoded)byte^=0xffU;
        temp.write("spt/var.fxf",encoded);
        temp.text("spt/varinitdata.xtx","1\n44\n63\n");
        temp.write("sys/ta_chart_button1_1.dwq",asset("tiny_red"));
        temp.text("private/chart1/slot3.dat","invalid checkpoint");
        auto options=test_options();options.width=128;options.height=96;
        scene::GameSession game(temp.root,options);
        game.set_chart_storage(temp.root/"private");
        const nnn::chart::Graph graph(temp.root,options.width,options.height);
        auto progress=graph.global_defaults();
        require(graph.mark_route(progress,3) && game.runtime()->set_chart_progress(std::move(progress)),
            "earlier node is unlocked");
        const auto open_chart=[&]{return game.open_modal(nnn::RuntimeMode::SelectChart,nnn::RuntimeMode::SelectMessage);};
        const auto act=[&](int action,int value=0){return game.act_modal(game.state().modal->request,action,value);};
        const auto pointer=[](int x,int y){return x*65536+y;};
        require(game.step_frame(0).wait==scene::Wait::Choice && game.step_frame(80).wait==scene::Wait::Choice,
            "reach a partially elapsed timed choice");
        const auto original=game.state();
        const auto backlog_size=game.backlog().size();
        const auto unchanged_choice=[&]{
            const auto& frame=game.step_frame(0);
            return frame.wait==scene::Wait::Choice && frame.selection && frame.selection->serial==3 &&
                frame.selection->options==original.selection->options && frame.selection->auto_select==2 &&
                frame.script==original.script && frame.word==original.word && frame.choice_remaining_ms==170 &&
                game.variable(0)==1 && game.variable(1)==44 && game.variable(2)==63 &&
                game.runtime()->selected_choices(3)==0 && game.backlog().size()==backlog_size;
        };
        require(open_chart() && game.step_frame(300).wait==scene::Wait::Modal && act(0)==2 && unchanged_choice(),
            "cancel returns to the exact choice without advancing its countdown");
        require(open_chart() && act(1,pointer(45,42))==0 && game.state().modal->chart->pending==3 &&
            act(4,1)==4 && act(8)==0 && game.state().modal->chart->pending==-1 && act(0)==2 && unchanged_choice(),
            "a corrupt revisit checkpoint preserves the choice, route variables and timer");
        require(open_chart() && act(1,pointer(30,40))==4 && act(8)==5 &&
            game.state().wait==scene::Wait::Running && game.variable(0)==2 && game.variable(1)==1,
            "a confirmed adjacent node commits the chart route");
        const auto& continued=game.step_frame(0);
        require(continued.wait==scene::Wait::Dialogue && continued.dialogue.message_id==77 &&
            !continued.selection && continued.choice_remaining_ms==0 && game.variable(2)==63 &&
            game.runtime()->selected_choices(3)==0 && game.stats().choices==0,
            "the next VM step skips both old CASE bodies and continues without recording an answer");
    });
    suite.test("menu PLACE2 saves as the active place and resumes after the suspended choice",[]{
        Model model;model.message=true;
        model.code.add({11,command,0x22220006U,1,0,0,3,0,2,250,0});
        model.code.add({4,control,0x55560008U,1});
        model.code.add({8,0x66660005U,0,0xa0000000U,3,0x80000003U,0x9000006fU,0xa000003dU});
        model.code.add({4,control,0x55560008U,2});
        model.code.add({8,0x66660005U,0,0xa0000000U,3,0x80000003U,0x900000deU,0xa000003dU});
        model.code.print(77);model.code.end();
        TempGame temp(model);
        temp.text("init/game.xtx","SelectPlace2IsDefault 1\n");
        temp.text("init/selectPlace2.xtx","placeNumber 2\nselectedPlaceVarName place\nselectFlag 1\n"
            "placeSelectOkVarName1 locked\nplaceSelectOkVarName2 available\n");
        const std::string names="place\nlocked\navailable\ncase_result\n";
        Bytes encoded(names.begin(),names.end());
        for(auto& byte:encoded)byte^=0xffU;
        temp.write("spt/var.fxf",encoded);
        temp.text("spt/varinitdata.xtx","9\n0\n1\n63\n");
        scene::GameSession game(temp.root,test_options());
        const auto mode=nnn::RuntimeMode::SelectPlace2;
        const auto caller=nnn::RuntimeMode::SelectMessage;
        const auto open_place=[&]{return game.open_modal(mode,caller);};
        require(game.step_frame(0).wait==scene::Wait::Choice && game.step_frame(80).wait==scene::Wait::Choice &&
            game.menu_extension_enabled(mode) && open_place() && game.place_mode()==mode &&
            game.places()==std::vector<int>{1,2} && game.state().modal->return_mode==caller,
            "PLACE2 can be opened from a choice without registering a script command");
        const auto request=game.state().modal->request;
        require(game.act_modal(request,1,1)==-1 && game.variable(0)==9 && game.modal_savable(),
            "a disabled place cannot commit");
        const auto saved=game.snapshot();
        scene::GameSession restored(temp.root,test_options());
        restored.restore_snapshot(saved);
        require(restored.state().wait==scene::Wait::Place && !restored.state().selection &&
            restored.state().choice_remaining_ms==0 && restored.place_mode()==mode &&
            restored.places()==std::vector<int>{1,2} && restored.variable(0)==9,
            "NS2 restores the active place screen with the selected-place result still unset");
        scene::InputState select;select.choice=2;
        const auto finished=[](scene::GameSession& session,const scene::FrameState& frame){
            return frame.wait==scene::Wait::Dialogue && frame.dialogue.message_id==77 &&
                session.variable(0)==2 && session.variable(3)==63 &&
                session.runtime()->selected_choices(3)==0 && session.stats().choices==0;
        };
        require(finished(restored,restored.step_frame(0,select)),"loaded place commits only the selected place");
        require(game.abort_modal(request,"missing place backdrop")==static_cast<int>(caller) &&
            game.step_frame(0).wait==scene::Wait::Choice && game.state().choice_remaining_ms==170 &&
            game.variable(0)==9 && game.variable(3)==63 && open_place(),
            "aborting a failed live visit restores the caller's choice and timer");
        require(game.act_modal(game.state().modal->request,1,2)==5 && finished(game,game.step_frame(0)),
            "a confirmed live place discards the old CASE without answering it");
    });
    suite.test("decorative visits retain their asset cache and isolate frame and runtime ownership",[]{
        Model menu;menu.code.number(nnn::audio::music,{1,1,100,0,0});menu.code.present(3);menu.code.end();
        TempGame temp(menu);
        temp.write("spt/menu.spt",script(menu));
        temp.write("dwq/red.dwq",asset("tiny_red"));
        const std::string names="var0\nvar1\nvar2\nvar3\nvar4\nscratch\n";
        Bytes encoded(names.begin(),names.end());
        for(auto& byte:encoded)byte^=0xffU;
        temp.write("spt/var.fxf",encoded);
        temp.text("spt/varinitdata.xtx","0\n0\n0\n0\n0\n17\n");
        auto options=test_options();options.entry="not-loaded.spt";
        options.runtime=std::make_shared<nnn::RuntimeContext>(temp.root);
        options.runtime->set_system_parameter(nnn::NNNPARAM_AUTOMODE,0);
        options.runtime->set_system_parameter(nnn::NNNPARAM_MESSAGESPEED,0);
        auto first=scene::GameSession::create_ui_preview(temp.root,options);
        const auto texture=first->ui_texture("red");
        auto second=scene::GameSession::create_ui_preview(temp.root,options,first.get());
        require(second->ui_texture("red")==texture && second->stats().decodes==1,"preview peer decoded the same texture again");

        first->reset_ui_background("menu",13,options.runtime);
        first->set_ui_saved_screen({1,1,{13,31,57}});
        require(first->step_ui_background({},false).wait==scene::Wait::Draw &&
            first->surface().rgb==Bytes({13,31,57}),"saved story screen was not used");
        require(first->stats().audio_events==0 && options.runtime->snapshot_gallery().empty(),
            "silent decoration submitted audio or unlocked a song");
        first->set_ui_variable("scratch",99);
        require(first->named_variable("scratch")==99 && first->text_delay_ms()==50,"old visit state");

        auto next=options.runtime->fork();
        next->set_system_parameter(nnn::NNNPARAM_MESSAGESPEED,1);
        next->set_system_variable(0,19);
        second->reset_ui_background("menu",12,next);
        second->step_ui_background({},false);
        require(first->surface().rgb==Bytes({13,31,57}) && second->surface().rgb==Bytes({0,0,0}),
            "warming a peer mutated the outgoing menu");

        temp.write("spt/menu.spt",Bytes{0}); // Same-root visits reuse the VM's validated program.
        first->reset_ui_background("menu",12,next);
        require(first->runtime()==next && first->mode()==12 && first->named_variable("scratch")==17 &&
            first->variable(200)==19 && first->text_delay_ms()==25,"reused visit retained old locals or parameters");
        first->step_ui_background({},false);
        require(first->surface().rgb==Bytes({0,0,0}),"reused visit retained the saved story screen");
        auto tentative=next->fork();
        const auto before=tentative->preview_state();
        second->reset_ui_background("menu",12,tentative);
        second->step_ui_background({},false);
        tentative->set_system_variable(0,93);
        require(!second->claim_ui_background(next,before) && next->system_variable(0)==19,
            "a speculative write leaked into live progress");
        tentative=next->fork();
        const auto stale=tentative->preview_state();
        second->reset_ui_background("menu",12,tentative);
        second->step_ui_background({},false);
        next->set_message_window(next->message_window()+1);
        require(!second->claim_ui_background(next,stale) && second->runtime()==tentative,
            "a changed auxiliary display setting accepted a stale frame");
        tentative=next->fork();
        second->reset_ui_background("menu",12,tentative);
        second->step_ui_background({},false);
        require(second->claim_ui_background(next,tentative->preview_state()) && second->runtime()==next,
            "an unchanged prepared frame did not rebind to the live parameters");
        first.reset();
        require(second->ui_texture("red")==texture,"closing the first peer discarded the shared cache");

        BehaviorConfig unrelated;
        bool rejected=false;
        try {second->reset_ui_background("menu",12,std::make_shared<nnn::RuntimeContext>(unrelated.root));}
        catch(const std::invalid_argument&) {rejected=true;}
        require(rejected && second->runtime()==next,"different game runtime was accepted");
    });
    suite.test("PC backlog uses the target layout and packs each row at the active voice stride",[]{
        for(const int layout:{0,1,2,4}){
            const int voices=std::max(1,layout);
            scene::pc::Save original;
            original.header.name="SAVEDATA";
            original.vars.resize(1024);
            original.thumbnail={1,1,{0x102030}};
            auto& log=original.log.emplace();
            log.layout=layout==0?scene::pc::LogLayout::Legacy:scene::pc::LogLayout::MultiVoice;
            log.voice_count=voices;
            for(int row=0;row<3;++row){
                auto& entry=log.entries.emplace_back();
                entry.text="line "+std::to_string(row);
                entry.color=0x112233U+static_cast<unsigned>(row);
                for(int voice=0;voice<voices;++voice)entry.voices[static_cast<std::size_t>(voice)]=
                    "voice"+std::to_string(row)+"_"+std::to_string(voice);
            }
            const auto restored=scene::pc::parse_save(scene::pc::write_save(original));
            require(restored.log && restored.log->layout==log.layout && restored.log->voice_count==voices &&
                    restored.log->entries.size()==3 && restored.log->tail==2,"PC LOG header or tail");
            for(std::size_t row=0;row<log.entries.size();++row){
                const auto& a=log.entries[row];const auto& b=restored.log->entries[row];
                require(a.text==b.text && a.color==b.color && a.voices==b.voices,"PC LOG row or voice stride");
            }
        }
    });
    suite.test("foreground END restarts control, EXIT ends at the title",[]{
        Model root;root.message=true;root.strings={"child"};root.code.print(1);
        root.code.add({5,control,0x5556000aU,0,0});root.code.print(2);root.code.end();
        Model child;child.code.end();
        TempGame temp(root);temp.write("spt/child.spt",script(child));
        scene::GameSession restarting(temp.root,test_options());
        require(restarting.step_frame(0).dialogue.message_id==1,"control opening");
        scene::InputState advance;advance.advance=true;
        const auto& restarted=restarting.step_frame(0,advance);
        require(restarted.wait==scene::Wait::Dialogue && restarted.end_reason==scene::EndReason::None &&
                restarted.dialogue.message_id==1 && restarted.dialogue.sequence==2,"child END did not restart control");

        Model exit;exit.message=true;exit.code.print(1);exit.code.add({3,control,0x5556000bU});exit.code.end();
        TempGame ending(exit);scene::GameSession finished(ending.root,test_options());
        require(finished.step_frame(0).wait==scene::Wait::Dialogue,"ending opening");
        finished.runtime()->set_system_variable(26,1);
        finished.runtime()->set_skip_active(true);
        const auto& last=finished.step_frame(0,advance);
        require(last.wait==scene::Wait::Ended && last.end_reason==scene::EndReason::Exit &&
                last.word==41 && finished.runtime()->mode()==static_cast<nnn::Value>(nnn::RuntimeMode::Title) &&
                finished.runtime()->system_variable(26)==1 && !finished.runtime()->skip_active(),"EXIT lost its reason or progress");
    });
    suite.test("PC chart checkpoints preserve status and exact padded RLE sections",[]{
        Model model;model.code.end();
        TempGame temp(model);
        temp.text("init/selectchart.xtx","chartBlockSize 319\n");
        temp.text("chart/chart1.xtx","1800\n@1 1 60 100 Node\n");
        const nnn::chart::Graph graph(temp.root,1280,720);

        scene::ChartNodeSave saved;
        saved.status.game_mode=static_cast<int>(nnn::RuntimeMode::SelectChart);
        saved.status.saved_at.year=2026;
        saved.status.vm[0].pc=87;
        saved.status.bgm_number=5;
        saved.status.guid[15]=0xf1;
        saved.status.log_messages[3]="Raw log";
        saved.vars[0]=INT32_MIN;
        saved.vars[200]=123;
        saved.vars[999]=-2;
        saved.run=graph.run_defaults();
        saved.run.move_road.back()=7;
        saved.run.can_select[1]=0x80000000U;

        constexpr std::size_t status_size=8112,legacy_status_size=7088;
        const Bytes rle{
            28,0,0,0, 0,0,0,0x80, 0,0,0,0,199,0,0,0, 123,0,0,0,
            0,0,0,0,0x1e,3,0,0, 0xfe,0xff,0xff,0xff,
            5,0,0,0, 0,255,0,64,7,
            20,0,0,0, 0xff,0xff,0xff,0xff,1,0,0,0, 0,0,0,0x80, 0xff,0xff,0xff,0xff,10,0,0,0};
        const auto bytes=scene::write_chart_node(saved,graph);
        require(bytes.size()==status_size+rle.size() && std::equal(rle.begin(),rle.end(),bytes.begin()+status_size),
            "checkpoint byte lengths, RLE markers or 255-byte split differ");
        const auto loaded=scene::parse_chart_node(bytes,graph);
        require(loaded.status.game_mode==saved.status.game_mode && loaded.status.saved_at.year==2026 &&
            loaded.status.vm[0].pc==87 && loaded.status.bgm_number==5 && loaded.status.guid[15]==0xf1 &&
            loaded.status.log_messages[3]=="Raw log" && loaded.vars==saved.vars &&
            loaded.run.move_road==saved.run.move_road && loaded.run.can_select==saved.run.can_select,
            "checkpoint lost status, variables or run flags");

        auto legacy=bytes;
        legacy.erase(legacy.begin()+legacy_status_size,legacy.begin()+status_size);
        le32(legacy,0,legacy_status_size);
        const auto older=scene::parse_chart_node(legacy,graph);
        require(older.status.log_messages[3].empty() && older.vars==saved.vars &&
            older.run.move_road==saved.run.move_road,"legacy checkpoint status width");

        const auto rejected=[&](const Bytes& invalid) {
            try { (void)scene::parse_chart_node(invalid,graph); }
            catch (const std::runtime_error&) { return true; }
            return false;
        };
        auto invalid=bytes;
        invalid[4]=1;
        require(rejected(invalid),"non-status block code accepted");
        invalid=bytes;le32(invalid,status_size,27);
        require(rejected(invalid),"non-word extent accepted");
        constexpr std::size_t road_data=status_size+4+28+4;
        invalid=bytes;invalid[road_data+1]=0;
        require(rejected(invalid),"zero-length road run accepted");
        invalid=bytes;invalid[road_data+1]=254;
        require(rejected(invalid),"incomplete decoded road section accepted");
        invalid=bytes;invalid[road_data+3]=65;
        require(rejected(invalid),"oversized decoded road section accepted");
        invalid=bytes;invalid.pop_back();
        require(rejected(invalid),"truncated selection mask accepted");
        invalid=bytes;invalid.push_back(0);
        require(rejected(invalid),"trailing bytes accepted");
    });
    suite.test("script message cursors survive saves without changing the configured cursor",[]{
        Model model;model.message=true;model.code.print(1);
        model.code.number(0x33330056U,{2});model.code.print(2);
        model.code.number(0x33330056U,{0});model.code.print(3);model.code.end();
        TempGame temp(model);
        Model background;background.code.number(0x33330056U,{3});background.code.present(10);background.code.end();
        temp.write("spt/cursor_back.spt",script(background));
        Model quiet;quiet.code.present(10);quiet.code.end();
        temp.write("spt/quiet_back.spt",script(quiet));
        scene::GameSession game(temp.root,test_options());
        require(game.step_frame(0).script_cursor==0,"fresh story cursor must start at zero");
        const auto legacy=game.snapshot();
        require(legacy[4]==9,"zero cursor unnecessarily upgraded the ordinary snapshot");
        const auto configured=game.runtime()->cursor_variants();
        scene::InputState advance;advance.advance=true;
        require(game.step_frame(0,advance).script_cursor==2 && game.runtime()->cursor_variants()==configured,
            "CURSORNUMBER changed the configured cursor or was not delivered in the frame");
        const auto saved=game.snapshot(),pc=game.export_pc_save();
        require(saved[4]==11 && scene::pc::parse_save(pc).status.cursor_number==2,"saved cursor state is missing");
        require(game.step_frame(0,advance).script_cursor==0,"CURSORNUMBER zero did not reset the script cursor");
        game.restore_snapshot(saved);
        require(game.state().script_cursor==2,"native snapshot lost the script cursor");
        game.restore_snapshot(legacy);
        require(game.state().script_cursor==0,"old snapshot inherited a later script cursor");
        auto pc_state=scene::pc::parse_save(pc,"SAVEDATA",scene::pc::first_extension_code);pc_state.status.cursor_number=1;
        game.restore_pc_save(scene::pc::write_save(pc_state));
        require(game.state().script_cursor==1 && scene::pc::parse_save(game.export_pc_save()).status.cursor_number==1,
            "PC slot cursor was not restored and exported");
        scene::GameSession fresh(temp.root,test_options());
        require(fresh.step_frame(0).script_cursor==0,"new story inherited a previous cursor");
        auto options=test_options();options.runtime=game.runtime();
        auto preview=scene::GameSession::create_ui_preview(temp.root,options);
        preview->start_ui_background("cursor_back",12);
        require(preview->step_ui_background({},false).script_cursor==3 && game.state().script_cursor==1,
            "decorative cursor escaped into the persistent story");
        preview->reset_ui_background("quiet_back",12,game.runtime());
        require(preview->step_ui_background({},false).script_cursor==0,"reused preview retained the previous script cursor");
    });
    suite.test("PC slot loading is atomic and both slot formats preserve global progress",[]{
        Model model;model.message=true;model.code.print(1);model.code.print(2);model.code.end();
        TempGame temp(model);
        std::string initial;
        for(unsigned i=0;i<=205;++i)initial+=(i==205?"3\n":"0\n");
        temp.text("spt/varinitdata.xtx",initial);
        scene::GameSession game(temp.root,test_options());
        require(game.step_frame(0).wait==scene::Wait::Dialogue,"first dialogue");
        const auto good=game.export_pc_save(),native=game.snapshot();
        game.runtime()->set_system_variable(5,17);
        scene::InputState advance;advance.advance=true;
        require(game.step_frame(0,advance).dialogue.message_id==2,"second dialogue");
        const auto prior=game.snapshot();
        auto damaged=good;le32(damaged,32,0x7fffffffU);
        bool rejected=false;
        try{game.restore_pc_save(damaged);}catch(const std::runtime_error&){rejected=true;}
        require(rejected && game.snapshot()==prior && game.variable(205)==17,"damaged PC block committed");
        game.restore_pc_save(good);
        require(game.state().wait==scene::Wait::Dialogue && game.state().dialogue.message_id==1 &&
                game.variable(205)==17,"PC slot overwrote global progress or lost its dialogue");
        game.restore_snapshot(native);
        require(game.variable(205)==17,"native slot overwrote global progress");
        scene::GameSession replay(temp.root,test_options());
        replay.seed_legacy_replay_variables();
        require(replay.variable(205)==3 && game.variable(205)==17,"legacy replay did not isolate its initial variables");
        auto current=scene::pc::parse_save(good,"SAVEDATA",scene::pc::first_extension_code);
        require(current.log && current.log->voice_count==1,"one-voice PC fixture");
        current.log->layout=scene::pc::LogLayout::MultiVoice;
        game.restore_pc_save(scene::pc::write_save(current));
        replay.restore_snapshot(game.snapshot());
        const auto exported=scene::pc::parse_save(replay.export_pc_save());
        require(exported.log && exported.log->layout==scene::pc::LogLayout::MultiVoice && exported.log->voice_count==1,
            "native snapshot forgot the imported single-voice layout");
    });
    suite.test("backlog jump validates the cursor and truncates future dialogue",[]{
        Model model;model.message=true;model.code.print(1);model.code.print(2);model.code.end();
        TempGame temp(model);temp.text("init/game.xtx","JumpFlag 1\n");
        scene::GameSession game(temp.root,test_options());
        scene::InputState first;first.mouse_x=137;first.mouse_y=89;
        require(game.step_frame(0,first).wait==scene::Wait::Dialogue,"first dialogue");
        game.capture_backlog_jump(5);
        const auto jump=game.backlog().back().jump_id;
        const auto checkpoint=game.backlog_jump(jump);
        require(jump!=0 && checkpoint.journal_cursor==5 && checkpoint.mouse_x==137 && checkpoint.mouse_y==89,
                "checkpoint cursor or pointer missing");
        scene::InputState advance;advance.advance=true;
        game.step_frame(0,advance);game.capture_backlog_jump(6);
        const auto future=game.backlog().back().jump_id;
        require(future!=0 && future!=jump && !game.restore_backlog_jump(jump,6) &&
                game.state().dialogue.message_id==2,"invalid cursor changed current dialogue");
        game.runtime()->set_system_variable(5,23);
        require(game.restore_backlog_jump(jump,5) && game.state().dialogue.message_id==1 &&
                game.backlog().size()==1 && game.backlog_jump(future).journal_cursor==0 &&
                game.variable(205)==23,"rewind retained future rows or rolled back global progress");
    });
    suite.test("timed selection records a hidden CASE without recoloring the visible option",[]{
        Model model;model.message=true;
        model.code.add({11,command,0x22220006U,1,0,0,3,0,2,50,0});
        model.code.add({4,control,0x55560008U,1});model.code.end();
        model.code.add({4,control,0x55560008U,2});model.code.print(2);model.code.end();
        TempGame temp(model);
        scene::GameSession game(temp.root,test_options());
        const auto& choice=game.step_frame(0);
        require(choice.wait==scene::Wait::Choice && choice.selection->options.size()==1 &&
                choice.choice_remaining_ms==50,"single visible timed option");
        require(game.step_frame(49).wait==scene::Wait::Choice,"timed option selected early");
        const auto& after=game.step_frame(1);
        require(after.wait==scene::Wait::Dialogue && after.choice_timed_out && after.dialogue.message_id==2 &&
                game.runtime()->selected_choices(3)==2,"hidden automatic CASE not selected and recorded");
        const auto visible=std::find_if(game.backlog().begin(),game.backlog().end(),[](const auto& row){return row.text=="story";});
        require(visible!=game.backlog().end() && visible->color==0xffffff,"timeout recolored the unchosen visible option");
    });
    for(const bool all:{false,true})suite.test(all?"skip-all skips both voices":"read-only skip stops before unread voice",[all]{
        Model m;m.message=true;
        for(Word serial:{1U,2U}){m.code.number(0x3333001bU,{serial});m.code.voice();m.code.print(serial);}
        m.code.end();TempGame temp(m);temp.text("spt/project.xtx","other\nsys\n");
        scene::GameSession game(temp.root,test_options());game.runtime()->mark_read(0x10001U);
        game.set_system_parameter(nnn::NNNPARAM_SKIPMODE,all?1:0);game.set_skip_active(true);
        require(game.step_frame(0).wait==scene::Wait::Dialogue,"first dialogue");
        require(game.state().dialogue.message_id==0x10001U&&game.skip_active(),"first identity or skip");
        auto log=game.audio().stats().log;require(log.size()==1&&log[0].action=="voice.skipped","first voice not skipped");
        scene::InputState input;input.advance=true;require(game.step_frame(0,input).wait==scene::Wait::Dialogue,"second dialogue");
        log=game.audio().stats().log;require(log.size()==2,"second voice missing");
        require((log[1].action=="voice.skipped")==all&&game.skip_active()==all,"second voice or skip gate");
        if(!all)require(!log[1].error.empty(),"missing voice did not reach audio");
    });
    for(const bool sentinel:{false,true})suite.test(sentinel?"STARTKOMA sentinel retains effect skip":"unread STARTKOMA gates only effects",[sentinel]{
        Model m;m.code.number(0x3333001cU,{sentinel?UINT32_MAX:42U});m.code.number(nnn::audio::music,{UINT32_MAX});
        m.code.voice();m.code.present();m.code.end();TempGame temp(m);scene::GameSession game(temp.root,test_options());
        game.set_system_parameter(nnn::NNNPARAM_SKIPMODE,0);game.set_skip_active(true);
        require(game.step_frame(0).wait==scene::Wait::Draw,"draw not reached");const auto log=game.audio().stats().log;
        require(log.size()==2&&log[0].action==(sentinel?"music.skipped":"music.stop")&&log[1].action=="voice.skipped","effect or voice gate");
        require(game.skip_active(),"STARTKOMA stopped skip");
    });
    suite.test("DRAW presentation follows PC message type and window overrides",[]{
        using DrawMessage=scene::DrawMessage;
        const struct Case {
            std::vector<Word> koma,message;
            bool retain;
            DrawMessage expected;
        } cases[]{
            {{2,0},{},false,DrawMessage::Window},
            {{2,0,0},{},true,DrawMessage::None},
            {{2,0,0},{2},true,DrawMessage::RetainedMessage},
            {{2,0,1},{},false,DrawMessage::Window},
            {{2,0,2},{2,1},true,DrawMessage::None},
            {{2,1,2},{},true,DrawMessage::LongBackground},
            {{2,2},{2,2},false,DrawMessage::LongBackground},
            {{2,3},{},false,DrawMessage::None},
            {{2,3},{2,2},true,DrawMessage::RetainedMessage},
        };
        for (const auto& scenario:cases) {
            Model m;m.message=true;m.message_text="Retained text";
            m.code.print(1);
            m.code.number(0x3333001cU,scenario.koma);
            if (!scenario.message.empty()) m.code.number(0x3333001bU,scenario.message);
            m.code.number(0x33330058U,{1});
            m.code.present(2);
            m.code.print(2);m.code.end();
            TempGame temp(m);
            if (scenario.retain) temp.text("init/printmessage.xtx","drawWindowWithMessageFlag 1\n");
            auto options=test_options();options.frame_ms=20;
            scene::GameSession game(temp.root,options);
            require(game.step_frame(0).wait==scene::Wait::Dialogue && game.draw_message()==DrawMessage::None,
                    "ordinary dialogue does not use the DRAW presentation");
            scene::InputState click;click.advance=true;
            const auto& waiting=game.step_frame(0,click);
            require(waiting.wait==scene::Wait::Draw && waiting.remaining_frames==2 &&
                    waiting.dialogue.display_bytes==m.message_text && game.draw_message()==scenario.expected,
                    "wrong DRAW presentation or lost preceding message");
            require(!game.request_advance(),"DRAW presentation did not inherit the click restriction");
            const auto snapshot=game.snapshot();
            require(snapshot[4]==(scenario.koma[1]==3?9:12),"DRAW message type snapshot format");
            scene::GameSession restored(temp.root,options);
            restored.restore_snapshot(snapshot);
            require(restored.state().wait==scene::Wait::Draw && restored.draw_message()==scenario.expected,
                    "restored DRAW changed its presentation");
            const auto& pending=restored.step_frame(39);
            require(pending.wait==scene::Wait::Draw && pending.remaining_frames==1,"DRAW wait ended early");
            const auto& next=restored.step_frame(1);
            require(next.wait==scene::Wait::Dialogue && next.dialogue.message_id==2 && next.dialogue.window_visible,
                    "DRAW did not finish on time or leaked its one-shot window mode");
        }
    });
    suite.test("DRAW applies the long window before the last screen layer",[]{
        Model m;
        m.code.number(effect,{0,54,0,100,100,100});
        m.code.number(0x3333001cU,{2,1,2});
        m.code.present(2);
        m.code.end();
        TempGame temp(m);
        temp.text("init/printmessage.xtx","LPrintBGMode 2\nlprintBGMultiR 33\nlprintBGMultiG -1\nlprintBGMultiB -1\n"
            "lprintBGDeltaR 5\nlprintBGDeltaG -9999\nlprintBGDeltaB -9999\n");
        scene::GameSession game(temp.root,test_options());
        require(game.step_frame(0).wait==scene::Wait::Draw,"long DRAW not reached");
        game.capture_screen_layer(true);
        auto frame=game.take_screen_layer();
        require(frame && !frame->captures_screen(),"ordinary DRAW needs a message background plane");
        std::vector<Byte> rgba(std::size_t{frame->width()}*frame->height()*4);
        frame->composite(rgba.data(),std::size_t{frame->width()}*4);
        require(rgba[0]==37 && rgba[1]==37 && rgba[2]==37,"wrong original fixed-point long-window colors");
        std::fill(rgba.begin(),rgba.end(),0);
        frame->composite(rgba.data(),std::size_t{frame->width()}*4,false);
        require(rgba[0]==100 && rgba[1]==100 && rgba[2]==100,"hidden window must leave the scene alone");
        require(game.surface().rgb[0]==100,"window decoration changed the native scene cache");
    });
    for (const Word transition:{draw,0x22220005U}) suite.test(transition==draw?"DRAW then APPEND":"OVERRAP then APPEND",[transition]{
        Model m;m.message=true;m.message_text="Old";
        m.code.add({6,command,0x22220002U,0,1,0});
        if (transition==draw) m.code.present(2);
        else m.code.number(transition,{0,2});
        m.code.add({6,command,0x22220003U,0,2,0});
        m.code.end();
        TempGame temp(m);
        scene::GameSession game(temp.root,test_options());
        require(game.step_frame(0).dialogue.layout_command==0x22220002U,"initial LPRINT mode");
        scene::InputState click;click.advance=true;
        const auto wait=transition==draw?scene::Wait::Draw:scene::Wait::Overlap;
        require(game.step_frame(0,click).wait==wait,"did not reach transition");
        const auto snap=game.snapshot();
        scene::GameSession restored(temp.root,test_options());
        restored.restore_snapshot(snap);
        if (transition!=draw) require(restored.step_frame(0,click).wait==scene::Wait::Running,"overlap endpoint");
        const auto& next=restored.step_frame(0,click);
        require(next.wait==scene::Wait::Dialogue && next.dialogue.layout_command==0x22220001U &&
                next.dialogue.window_visible && next.dialogue.display_bytes=="OldOld","APPEND must use the compact window after a transition");
        require(restored.append_prefix()=="Old","old text prefix must be available for the new layout");
        restored.set_text_length(6,3);
        require(restored.visible_text()==3 && restored.append_prefix().empty(),"initial visible text must use the projected prefix once");
    });
    suite.test("LPRINT and its APPEND keep option permission separate from the compact window",[]{
        Model m;m.message=true;
        m.code.add({6,command,0x22220002U,0,1,0});
        m.code.add({6,command,0x22220003U,0,2,0});
        m.code.number(0x3333005aU,{1});
        m.code.add({6,command,0x22220002U,0,3,0});
        m.code.end();TempGame temp(m);scene::GameSession game(temp.root,test_options());
        const auto& first=game.step_frame(0);
        require(first.wait==scene::Wait::Dialogue && first.dialogue.layout_command==0x22220002U &&
                !first.dialogue.window_visible && game.options_enabled(),"LPRINT wrongly tied options to the compact window");
        scene::InputState click;click.advance=true;
        const auto& appended=game.step_frame(0,click);
        require(appended.wait==scene::Wait::Dialogue && appended.dialogue.layout_command==0x22220002U &&
                !appended.dialogue.window_visible && game.options_enabled(),"APPEND did not keep LPRINT presentation");
        require(game.step_frame(0,click).wait==scene::Wait::Dialogue && !game.options_enabled(),"LPRINT ignored OPTIONOFF");
    });
    suite.test("themed configuration samples share the system sound buffer cursor",[]{
        Model model;model.code.end();
        TempGame temp(model);
        temp.text("init/game.xtx","useSystemVoice 1\n");
        temp.text("init/config.xtx","extSeFlag 1\nuseConfigSeVoice 1\nvoiceSameBufferFlag 1\n"
            "soundSeNumber 1\nvoiceSeNumber 2\nvoiceSeBunkatsu 0\nvoiceIsVoice 1\n");
        temp.text("nya/systemse.xtx","neutral,SE\n");
        temp.text("nya/systemvoice1.xtx","click,SE\n");
        temp.text("nya/configse.xtx","neutral,SE\nneutral,Voice\n");
        temp.text("nya/configsevoice1.xtx","themed_se,SE\nthemed_voice,Voice\n");
        for (const auto* name:{"click","themed_se","themed_voice"})
            temp.write(std::string("vaw/sys/")+name+".vaw",pcm_vaw(1,44100,16,{0,16,0,32}));
        nnn::audio::Settings options;options.record_events=true;
        nnn::audio::GameAudio audio(temp.root,options);
        audio.set_system_voice_selection(1);
        require(audio.play_config_feedback(2,40,100).empty() && audio.play_system_se(0).empty() &&
            audio.play_config_feedback(3,40,100).empty(),"configured samples can be decoded");
        const auto events=audio.stats().log;
        require(events.size()==3 && events[0].channel==1 && events[0].clips.at(0).name=="themed_se" &&
            events[1].channel==0 && events[1].clips.at(0).name=="click" &&
            events[2].channel==0 && events[2].clips.at(0).name=="themed_voice",
            "a same-buffer configuration voice must replace the most recent ordinary system sound");
    });
    for(const unsigned bits:{8U,16U})suite.test("PCM VAW playback "+std::to_string(bits),[bits]{
        Model m;m.code.end();TempGame temp(m);temp.text("spt/selist.xtx","sample\n");
        const Bytes samples=bits==8?Bytes{0,255,64,192,128}:Bytes{0,64,0,192,255,127,0,128,1};
        temp.write("vaw/se/sample.vaw",pcm_vaw(bits==8?1U:2U,44100,bits,samples));
        nnn::audio::Settings settings;settings.record_events=true;nnn::audio::GameAudio audio(temp.root,settings);
        nnn::SystemEvent event;event.opcode=nnn::audio::sound;event.parameters={0,0};
        require(audio.submit(event).empty(),"PCM submit failed");const auto stats=audio.stats();
        require(stats.errors==0&&stats.log.size()==1&&stats.log[0].clips.size()==1,"PCM log");
        const auto& clip=stats.log[0].clips[0];require(clip.rate==44100&&clip.channels==(bits==8?1U:2U)&&clip.frames==(bits==8?5U:2U),"PCM format/partial frame");
        std::int16_t output[8]{};audio.pull(output,4);require(std::any_of(std::begin(output),std::end(output),[](auto s){return s!=0;}),"silent PCM output");
    });
    for(int variant=0;variant<4;++variant)suite.test("PCM VAW rejects invalid variant "+std::to_string(variant),[variant]{
        Model m;m.code.end();TempGame temp(m);temp.text("spt/selist.xtx","sample\n");
        auto bytes=pcm_vaw(2,44100,16,{1,2,3,4});
        if(variant==0)bytes[57]='1';else if(variant==1)bytes[84]=3;else if(variant==2)bytes[96]=2;else le32(bytes,104,100);
        temp.write("vaw/se/sample.vaw",bytes);nnn::audio::Settings settings;settings.record_events=true;
        nnn::audio::GameAudio audio(temp.root,settings);nnn::SystemEvent event;event.opcode=nnn::audio::sound;event.parameters={0,0};
        const auto error=audio.submit(event);require(!error.empty()&&audio.stats().errors==1,"invalid PCM accepted");
        if(variant==0)require(error.find("Unsupported VAW PACKTYPE=1")!=std::string::npos,"pack type error");
        if(variant==3)require(error.find("Truncated VAW PCM data")!=std::string::npos,"truncated PCM error");
    });
    suite.test("DWQ routes select a pack and numeric patch level before caching",[]{
        Model m;m.code.end();TempGame temp(m);
        temp.text("init/game.xtx","dwqPackLevel 11\n");
        const auto pack=[&](const std::string& stem,const std::string& name,const char* picture) {
            Bytes gtb(12U,0);le32(gtb,0,1);
            gtb.insert(gtb.end(),name.begin(),name.end());gtb.push_back(0);
            temp.write(stem+".gtb",gtb);temp.write(stem+".gpk",asset(picture));
        };
        pack("dwq/sys0","ta_slot","tiny_red");
        pack("sys0","root_pack_slot","tiny_green");
        pack("dwq/ta2","ta_slot","tiny_red");
        pack("ta2","ta_slot","tiny_alpha");
        pack("ta10","ta_slot","tiny_green");
        pack("dwq/ta11","ta_slot","tiny_red");
        const std::string trail_backslash="\x81\x5c";
        pack("dwq/sc0",trail_backslash,"tiny_alpha");
        temp.write("dwq/ev/ta_slot.dwq",asset("tiny_red"));
        temp.write("sys/root_loose_slot.dwq",asset("tiny_green"));
        scene::GameAssets assets(temp.root,1024);
        const auto sys=assets.image("sys/ta_slot"),ta=assets.image("ta\\ta_slot");
        require(sys->source=="sys0.gpk" && ta->source=="ta10.gpk" && sys->pixels.rgba!=ta->pixels.rgba,
            "same basename aliased different packs or selected the wrong level");
        require(assets.image("SYS\\TA_SLOT.DWQ")==sys && assets.image(ta->asset_id)==ta,
            "physical resource did not reuse its cached texture");
        require(assets.image("sys/sc/"+trail_backslash)->source=="sc0.gpk", "CP932 trail split the member name");
        require(assets.image("ev/ta_slot")->asset_id=="@loose/dwq/ev/ta_slot", "missing exact loose fallback");
        const auto root_pack=assets.image("sys/root_pack_slot"),root_loose=assets.image("sys/root_loose_slot");
        require(root_pack->asset_id=="@root-pack/sys0/root_pack_slot" && assets.image(root_pack->asset_id)==root_pack &&
            root_loose->asset_id=="@loose/sys/root_loose_slot" && assets.pc_reference(*root_pack).empty() &&
            assets.pc_reference(*root_loose).empty(),"root assets were missing or reported as PC-compatible");
        bool missing=false;
        try {assets.image("bg/ta_slot");} catch (const std::runtime_error&) {missing=true;}
        require(missing,"explicit path fell back to an unrelated pack");
        const auto low=assets.image("@pack/ta2/ta_slot");
        require(low!=sys && low!=ta && assets.pc_reference(*ta).empty() &&
            assets.image(assets.pc_reference(*low),true)==low &&
            assets.image(assets.pc_reference(*sys),true)==sys,"physical or PC save identity changed");
        temp.text("init/game.xtx","dwqPackLevel 3\n");
        scene::GameAssets same_level(temp.root,1024);
        require(same_level.image("ta/ta_slot")->asset_id==low->asset_id,"dwq position lost a same-level tie");
        emberfall::render::Layers live(1,1),restored(1,1);
        live.bind(0,sys,false);live.bind(1,low,false);live.bind(2,ta,false);
        nnn::snapshot::Writer out;live.save_snapshot(out);
        nnn::snapshot::Reader in(out.bytes);
        restored.load_snapshot(in,[&](const std::string& name){return assets.image(name);});
        require(!in.remaining() && restored.slots()[0].texture==sys && restored.slots()[1].texture==low &&
            restored.slots()[2].texture==ta,
            "layer snapshot lost its physical resource identity");
    });
    suite.test("snapshot validates before replacing live scene and reuses its caches",[]{
        Model m;
        m.code.image(4,1);m.code.number(effect,{4,0});m.code.present();
        m.code.image(4,2);m.code.number(effect,{4,0});m.code.present();m.code.end();
        TempGame temp(m);
        temp.write("dwq/red.dwq",asset("tiny_red"));
        temp.write("dwq/green.dwq",asset("tiny_green"));
        scene::GameSession game(temp.root,test_options());
        require(game.step_frame(0).wait==scene::Wait::Draw,"first frame missing");
        const auto first=game.surface().rgb;
        const auto saved=game.snapshot();
        const auto owner=game.runtime();const auto* audio=&game.audio();
        scene::InputState advance;advance.advance=true;
        require(game.step_frame(20,advance).wait==scene::Wait::Draw,"second frame missing");
        const auto second=game.surface().rgb;
        const auto decodes=game.stats().decodes;
        require(first!=second,"fixture must change pixels");

        auto corrupt=saved;corrupt.push_back(0);
        bool rejected=false;
        try {game.restore_snapshot(corrupt);}
        catch (const std::exception&) {rejected=true;}
        require(rejected&&game.surface().rgb==second,"late validation modified the live frame");

        game.restore_snapshot(saved);
        require(game.surface().rgb==first&&game.stats().decodes==decodes,"compact snapshot missed cached layer");
        require(game.runtime()==owner && &game.audio()==audio,"restore replaced a live owner");
    });
    suite.test("backlog replay leaves script loops available to restart",[]{
        Model m;m.code.end();TempGame temp(m);
        temp.text("spt/selist.xtx","sample\n");
        temp.write("vaw/se/sample.vaw",pcm_vaw(1,44100,8,{148,158,168,178}));
        temp.write("cdvaw/lo/loop.vaw",pcm_vaw(1,44100,8,{152,162,172,182}));
        temp.write("cdvaw/li/line.vaw",pcm_vaw(1,44100,8,{188,198,208,218}));
        nnn::audio::GameAudio audio(temp.root);
        nnn::SystemEvent se,voice;
        se.opcode=nnn::audio::sound;se.parameters={0,0,0,1};
        voice.opcode=nnn::audio::voice;voice.text="loop";voice.parameters={0,0,0,1};
        require(audio.submit(se).empty() && audio.submit(voice).empty(),"start script loops");
        std::array<std::int16_t,2> opening{},output{};
        audio.pull(opening.data(),1);
        require(opening[0]!=0,"script loops silent");
        se.parameters={0,1};voice.parameters={0,1};
        require(audio.submit(se).empty() && audio.submit(voice).empty(),"start one-shot tracks");
        audio.suspend_script_audio();
        audio.pull(output.data(),1);
        require(output[0]==0 && output[1]==0,"script audio leaked into backlog");
        require(audio.replay_backlog_voice("line").empty(),"replay backlog voice");
        audio.pull(output.data(),1);
        require(output[0]!=0,"backlog replay silent");
        nnn::snapshot::Writer snapshot;
        audio.save_snapshot(snapshot);
        nnn::snapshot::Reader reader(snapshot.bytes);
        nnn::audio::GameAudio restored(temp.root);
        restored.load_snapshot(reader);
        const auto saved=restored.state();
        require(reader.remaining()==0 && saved.sounds[0].looping && saved.voices[0].looping &&
            saved.sounds[0].source_frame==1 && saved.voices[0].source_frame==1,"snapshot lost suspended loops");
        audio.restart_loop_script_audio();
        audio.pull(output.data(),1);
        require(output==opening,"loop mix did not restart or backlog audio leaked");
        const auto state=audio.state();
        require(state.sounds[0].active && state.voices[0].active &&
            !state.sounds[1].active && !state.voices[1].active,"only script loops should restart");

        nnn::RuntimeParameters params;
        params.set(nnn::NNNPARAM_CONTINUEVOICESWITCH,1);
        audio.apply_runtime_parameters(params);
        voice.parameters={0,2,0,1};
        require(audio.submit(voice).empty(),"start effect voice");
        voice.parameters={-1,0};
        require(audio.submit(voice).empty() && audio.state().voices[0].active,"continue main voice across VOICE -1");
        voice.parameters={-1,2};
        require(audio.submit(voice).empty() && !audio.state().voices[2].active,"continue setting must not retain effect voice");
        params.set(nnn::NNNPARAM_CONTINUEVOICESWITCH,0);
        audio.apply_runtime_parameters(params);
        voice.parameters={-1,0};
        require(audio.submit(voice).empty() && !audio.state().voices[0].active,"stop main voice with continue disabled");
    });
    for(const bool kill:{false,true})suite.test(kill?"restrictions kill skip and gate clicks":"restrictions suppress effective skip without killing it",[kill]{
        Model m;m.message=true;m.code.number(0x33330058U,{1});m.code.number(0x33330059U,{1});
        m.code.voice();m.code.present(10);m.code.print(1);m.code.number(0x33330058U,{UINT32_MAX});
        m.code.number(0x33330059U,{UINT32_MAX});m.code.present(10);m.code.end();TempGame temp(m);
        if(kill)temp.text("init/game.xtx","cannotSkipCheckKillShift 1\n");
        scene::GameSession game(temp.root,test_options());game.set_system_parameter(nnn::NNNPARAM_SKIPMODE,1);game.set_skip_active(true);
        require(game.step_frame(0).wait==scene::Wait::Draw,"region draw");auto r=game.script_restrictions();
        require(r.cannot_click==1&&r.cannot_skip==1&&r.auto_message==0&&r.option_off==0,"restriction values");
        require(game.skip_active()!=kill&&!game.skip_effective()&&!game.request_advance(),"restricted draw/skip");
        const auto log=game.audio().stats().log;require(log.size()==1&&log[0].action!="voice.skipped"&&!log[0].error.empty(),"restricted voice was skipped");
        require(game.step_frame(200).wait==scene::Wait::Dialogue,"region dialogue");game.set_text_length(5);
        require(!game.request_advance()&&game.visible_text()==0,"restricted text was completed");
        game.text_characters(10000);require(game.text_complete()&&game.request_advance(),"completed restricted text");
        scene::InputState input;input.advance=true;require(game.step_frame(0,input).wait==scene::Wait::Draw,"post-region draw");
        r=game.script_restrictions();require(r.cannot_click==0&&r.cannot_skip==0&&game.request_advance(),"restrictions not cleared");
        require(game.skip_effective()==!kill,"raw skip not restored");
    });
    suite.test("script AUTOMESSAGE gates input and controls timer",[]{
        Model m;m.message=true;m.code.number(0x33330057U,{1});m.code.print(1);
        m.code.number(0x33330057U,{UINT32_MAX});m.code.present();m.code.end();TempGame temp(m);
        temp.text("init/printmessage.xtx","autoMessageLastWait 100\nautoMessageWaitVoiceFlag 0\n");
        scene::GameSession game(temp.root,test_options());require(game.step_frame(0).wait==scene::Wait::Dialogue,"auto dialogue");
        require(game.script_restrictions().auto_message==1&&!game.options_enabled(),"automatic options");
        game.set_text_length(0);require(!game.request_advance(),"automatic click");
        require(!game.auto_ready(100,true)&&game.auto_ready(0,true),"automatic wait");
        scene::InputState input;input.advance=true;require(game.step_frame(0,input).wait==scene::Wait::Draw,"automatic exit");
        require(game.options_enabled()&&game.script_restrictions().auto_message==0,"automatic restriction not cleared");
    });
    suite.test("script auto timer waits for both elapsed text and voice boundaries",[]{
        BehaviorConfig config;std::ofstream out(config.root/"init/printmessage.xtx",std::ios::binary);
        out<<"autoMessageLastWait 100\nautoMessageWaitVoiceFlag 1\nautoMessageVoiceLastWait 50\nnoVoiceLastWait 100\n";out.close();
        nnn::RuntimeParameters parameters;nnn::PlayerBehavior behavior(parameters,config.root);
        behavior.begin_line(true,200);behavior.set_text_length(1);
        require(!behavior.auto_ready(100,false,true),"voice timer before text");
        behavior.text_characters(10000);require(!behavior.auto_ready(10,true,true),"last character delay");
        require(!behavior.auto_ready(100,true,true),"wait before threshold");
        require(!behavior.auto_ready(39,true,true),"voice before boundary");
        require(behavior.auto_ready(1,true,true),"voice boundary");
        parameters.set(nnn::NNNPARAM_AUTOMODE,1);behavior.begin_line(false,0);behavior.set_text_length(0);
        require(!behavior.auto_ready(99,true)&&behavior.auto_ready(1,true),"normal auto timing changed");
    });
    suite.test("message CPS is invariant to frame partition and retains fractional carry",[]{
        BehaviorConfig config;nnn::RuntimeParameters parameters;parameters.set(nnn::NNNPARAM_MESSAGESPEED,0);
        nnn::PlayerBehavior whole(parameters,config.root),split(parameters,config.root);
        whole.begin_line(false,0);split.begin_line(false,0);whole.set_text_length(1000);split.set_text_length(1000);
        require(whole.characters_per_second()==20U,"authored normal CPS was not loaded");
        std::uint32_t partitioned=0;
        for(const std::uint32_t dt:{7U,13U,17U,31U,101U,223U,307U,300U})partitioned+=split.text_characters(dt);
        require(whole.text_characters(999)==19U&&partitioned==19U&&whole.visible_text()==split.visible_text(),"frame boundaries lost fractional characters");
        require(whole.text_characters(1)==1U&&split.text_characters(1)==1U&&split.visible_text()==20U,"999ms carry was discarded");
        split.text_characters(49);split.begin_line(false,0);split.set_text_length(1000);
        require(split.text_characters(49)==0U&&split.text_characters(1)==1U,"new line retained old fractional carry");
    });
    suite.test("first advance reveals native text and second advance leaves the line",[]{
        BehaviorConfig config;nnn::RuntimeParameters parameters;parameters.set(nnn::NNNPARAM_MESSAGESPEED,0);
        nnn::PlayerBehavior behavior(parameters,config.root);behavior.begin_line(false,0);
        require(!behavior.request_advance(),"unknown text length advanced");behavior.set_text_length(9);
        require(behavior.text_characters(100)==2U&&behavior.visible_text()==2U&&!behavior.text_complete(),"partial reveal setup");
        require(!behavior.request_advance()&&behavior.visible_text()==9U&&behavior.text_complete(),"first advance skipped partially revealed text");
        require(behavior.request_advance()&&behavior.visible_text()==9U,"second advance failed after native completion");
    });
    suite.test("Auto starts spending wait on the frame after the last character",[]{
        BehaviorConfig config;nnn::RuntimeParameters parameters;
        parameters.set(nnn::NNNPARAM_AUTOMODE,1);parameters.set(nnn::NNNPARAM_AUTOSPEEDSLIDER,0);
        nnn::PlayerBehavior behavior(parameters,config.root);behavior.begin_line(false,0);behavior.set_text_length(1);
        require(behavior.auto_remaining_ms()==100&&!behavior.auto_ready(999,true),"frontend completion overrode unfinished native text");
        require(behavior.text_characters(9)==1U&&behavior.text_complete(),"last-character frame setup");
        require(!behavior.auto_ready(9,true)&&behavior.auto_remaining_ms()==100,"last-character frame consumed Auto wait");
        require(behavior.text_characters(40)==0U&&!behavior.auto_ready(40,false)&&behavior.auto_remaining_ms()==60,"first completed-at-entry frame timing");
        require(behavior.text_characters(59)==0U&&!behavior.auto_ready(59,false)&&behavior.auto_remaining_ms()==1,"Auto completed early");
        require(behavior.text_characters(1)==0U&&behavior.auto_ready(1,false)&&behavior.auto_remaining_ms()==0,"Auto did not complete at the exact source wait");
    });
    suite.test("Classic uses the Auto slider for wait and message speed for reveal",[]{
        BehaviorConfig config;
        {
            std::ofstream out(config.root/"init/config.xtx",std::ios::binary);
            out<<"volumeNumber 8\nsoundBarFileName old_bar\nsoundVolumePrintX 360\n";
        }
        {
            std::ofstream out(config.root/"init/printmessage.xtx",std::ios::binary);
            out<<"customMessageSpeed 1\nmessageSpeed1 20\nmessageSpeed2 37\n"
                "noVoiceLastWait1 2000\nnoVoiceLastWait2 1000\nvoiceLastWait1 500\nvoiceLastWait2 200\n";
        }
        nnn::RuntimeParameters parameters;
        parameters.set(nnn::NNNPARAM_AUTOMODE,1);
        parameters.set(nnn::NNNPARAM_MESSAGESPEED,0);
        parameters.set(nnn::NNNPARAM_AUTOSPEEDSLIDER,1);
        const nnn::RuntimeContext runtime(config.root);
        nnn::PlayerBehavior legacy(parameters,config.root,runtime.legacy_volume_defaults());
        nnn::PlayerBehavior modern(parameters,config.root);
        legacy.begin_line(false,0);modern.begin_line(false,0);
        require(legacy.characters_per_second()==20U&&modern.characters_per_second()==20U,"Classic Auto slider changed the text reveal speed");
        require(legacy.auto_remaining_ms()==1000&&modern.auto_remaining_ms()==2000,"unvoiced Auto used the wrong wait index");
        legacy.begin_line(true,250);modern.begin_line(true,250);
        require(legacy.auto_remaining_ms()==450&&modern.auto_remaining_ms()==750,"voiced Auto used the wrong wait index");
    });
    suite.test("script message speed uses custom Auto table while Auto is off",[]{
        BehaviorConfig config;nnn::RuntimeParameters parameters;parameters.set(nnn::NNNPARAM_MESSAGESPEED,0);
        nnn::PlayerBehavior behavior(parameters,config.root);behavior.begin_line(false,0,2);behavior.set_text_length(1000);
        require(parameters.get(nnn::NNNPARAM_AUTOMODE)==0&&behavior.characters_per_second()==137U,"script override selected the normal table");
        require(behavior.text_characters(1000)==137U&&behavior.visible_text()==137U,"custom Auto override did not drive reveal");
        behavior.begin_line(false,0,0);behavior.set_text_length(1000);
        require(behavior.characters_per_second()==20U&&behavior.text_characters(1000)==20U,"script override leaked into the next ordinary line");
        require(parameters.get(nnn::NNNPARAM_MESSAGESPEED)==0&&parameters.get(nnn::NNNPARAM_AUTOMODE)==0,"script speed mutated global settings");
    });
    suite.test("shared read identities preserve script and message halves",[]{
        nnn::RuntimeContext context(fixtures);
        require(!context.is_read(0x00010007U),"new identity was read");
        require(context.mark_read(0x00010007U)&&context.mark_read(0x00010007U),"mark/idempotence failed");
        require(context.is_read(0x00010007U)&&!context.is_read(0x00020007U),"script identity collided");
        require(context.is_read(0x1234ffffU)&&context.mark_read(0x1234ffffU),"source untracked serial sentinel");
        require(context.snapshot_read()==std::vector<nnn::Word>{0x00010007U},"read snapshot contains duplicates/sentinel");
    });
    suite.test("read restore is bounded and rejects transactionally",[]{
        nnn::RuntimeContext context(fixtures);
        require(context.restore_read({7U,7U,0x10002U,0xffffU}),"bounded restore failed");
        const auto before=context.snapshot_read();
        require(before==std::vector<nnn::Word>{7U,0x10002U},"restore normalization");
        require(!context.restore_read(std::vector<nnn::Word>(nnn::RuntimeContext::max_read_messages+1U,1U)),"oversized restore accepted");
        require(context.snapshot_read()==before,"failed restore changed committed read state");
    });
    suite.test("fork isolates candidate reads parameters and transient modes",[]{
        nnn::RuntimeContext context(fixtures);context.mark_read(10U);context.set_skip_active(true);
        context.set_mode(nnn::RuntimeMode::Title);context.set_return_mode(nnn::RuntimeMode::PrintMessage);
        const auto candidate=context.fork();
        require(candidate->is_read(10U)&&candidate->skip_active()&&candidate->mode()==4&&candidate->return_mode()==10,"fork omitted owner state");
        candidate->mark_read(11U);candidate->set_skip_active(false);candidate->set_mode(nnn::RuntimeMode::Config);
        candidate->set_system_parameter(nnn::NNNPARAM_AUTOMODE,1);
        require(!context.is_read(11U)&&context.skip_active()&&context.mode()==4,"candidate polluted committed context");
        require(context.get_system_parameter(nnn::NNNPARAM_AUTOMODE)==0,"candidate parameter mutation escaped");
    });
    suite.test("normal skip stops on unread dialogue without marking it",[]{
        nnn::RuntimeContext context(fixtures);context.mark_read(12U);context.set_skip_active(true);
        require(context.check_dialogue_skip(12U),"read dialogue stopped normal skip");
        require(!context.check_dialogue_skip(13U)&&!context.skip_active()&&!context.is_read(13U),"unread dialogue skip rule");
        context.set_system_parameter(nnn::NNNPARAM_SKIPMODE,nnn::NNNSKIPMODE_ALL);context.set_skip_active(true);
        require(context.check_dialogue_skip(13U)&&!context.is_read(13U),"all-skip marked or rejected unread line");
    });
    suite.test("choice confirmation uses source210211 polarity",[]{
        nnn::RuntimeContext context(fixtures);
        for(const nnn::Value auto_off:{0,1})for(const nnn::Value skip_off:{0,1}){
            context.update_system_parameters({{nnn::NNNPARAM_AUTOMODE,1},{nnn::NNNPARAM_AUTOCONTINUESWITCH,auto_off},{nnn::NNNPARAM_AUTOSKIPSWITCH,skip_off}});
            context.set_skip_active(true);context.complete_choice();
            require(context.get_system_parameter(nnn::NNNPARAM_AUTOMODE)==(auto_off?0:1),"choice Auto polarity");
            require(context.skip_active()==(skip_off==0),"choice Skip polarity");
        }
    });
    suite.test("platform skip is an optional command not a tick default",[]{
        emberfall::scene::InputState input;
        require(!input.skip.has_value(),"empty tick overwrites Core skip state");
        input.skip=false;require(input.skip.has_value()&&!*input.skip,"explicit off command lost");
        input.skip=true;require(input.skip.has_value()&&*input.skip,"explicit on command lost");
    });
    suite.test("public source-backed limits",[]{require(scene::first_layer==4&&scene::second_layer==5&&scene::nop_effect==0,"foreground/NOP contract");require(scene::max_dimension==2048&&scene::max_pixels==4194304&&scene::max_script_bytes==262144&&scene::max_name_bytes==255,"input limits");require(scene::max_resource_reads==8&&scene::max_resource_bytes==8388608&&scene::max_total_resource_bytes==16777216&&scene::max_decoded_pixels==8388608&&scene::max_callbacks==256&&scene::max_script_work==100000,"session limits");});
    suite.test("game assets preserve source mask presence",[]{
        Model m;m.code.image(4,1);m.code.image(5,2);m.code.present();m.code.end();TempGame temp(m);
        temp.text("spt/project.xtx","other\nsys\n");
        temp.write("dwq/red.dwq",read_file(fixtures/"../../../../core/tests/png/fixtures/rgb_1x1_red.dwq"));
        temp.write("dwq/green.dwq",asset("tiny_green"));
        scene::GameSession game(temp.root,test_options());
        require(game.step_frame(0).wait==scene::Wait::Draw,"draw wait");
        require(game.layers()[4].texture&&!game.layers()[4].texture->has_mask,"RGB mask flag");
        require(game.layers()[5].texture&&game.layers()[5].texture->has_mask,"RGBA mask flag");
    });
    suite.test("script and resource determine red first frame",[]{Resources resources;const Model m=single();const auto result=run(m,resources);success(result,1,1,{255,0,0});require(result.layers_drawn==1&&result.resource_reads==1&&resources.keys==std::vector<std::string>{"red"},"red diagnostics");});
    suite.test("changing script resource key changes actual pixels",[]{Resources resources;success(run(single(2),resources),1,1,{0,255,0});require(resources.keys==std::vector<std::string>{"green"},"SPT did not control reader key");});
    suite.test("changing reader bytes for same key changes actual pixels",[]{Resources resources;resources.files["red"]=asset("tiny_green");success(run(single(),resources),1,1,{0,255,0});});
    suite.test("LOAD binding enables default NOP without SETEFFECT",[]{Model m;m.code.image(4);m.code.present();m.code.end();Resources resources;success(run(m,resources),1,1,{255,0,0});});
    suite.test("layer order is ascending, independent of LOAD and effect order",[]{Model m;m.code.image(5,2);m.code.image(4,1);m.code.number(effect,{5,0});m.code.number(effect,{4,0});m.code.present();m.code.end();Resources resources;auto result=run(m,resources);success(result,1,1,{0,255,0});require(result.layers_drawn==2&&resources.keys==std::vector<std::string>{"green","red"},"load order or layer count");});
    suite.test("Source32 arithmetic retained at overlapping fractional alpha",[]{Model m;m.code.image(4,1);m.code.image(5,3);m.code.present();m.code.end();Resources resources;success(run(m,resources),1,1,{227,50,0});});
    suite.test("partial extents copy top-left and are not right-bottom corners",[]{Model m;m.code.image(4);m.code.place(4,1,1,1,1);m.code.present();m.code.end();Resources resources;resources.files["red"]=asset("lower");success(run(m,resources,3,3),3,3,{0,0,0,0,0,0,0,0,0,0,0,0,101,51,201,0,0,0,0,0,0,0,0,0,0,0,0});});
    suite.test("negative destination clips full source without scaling",[]{Model m;m.code.image(4);m.code.place(4,-1,-1,3,2);m.code.present();m.code.end();Resources resources;resources.files["red"]=asset("lower");success(run(m,resources,2,2),2,2,{11,151,61,11,151,61,0,0,0,0,0,0});});
    for(const Word extent:{0U,1U})suite.test("zero area extents accepted "+std::to_string(extent),[extent]{Model m;m.code.image(4);m.code.place(4,0,0,extent,0);m.code.present();m.code.end();Resources resources;success(run(m,resources),1,1,{0,0,0});});
    for(const std::int32_t x:{INT32_MIN,INT32_MAX})suite.test("extreme signed placement clips safely "+std::to_string(x),[x]{Model m;m.code.image(4);m.code.place(4,x,0,1,1);m.code.present();m.code.end();Resources resources;success(run(m,resources),1,1,{0,0,0});});
    suite.test("CLEARALLEFFECT produces fresh black frame",[]{Model m;m.code.image(4);m.code.image(5,2);m.code.number(clear_all,{});m.code.present();m.code.end();Resources resources;auto result=run(m,resources);success(result,1,1,{0,0,0});require(result.layers_drawn==0&&result.resource_reads==2,"clear refunded/unbound incorrectly");});
    suite.test("CLEAREFFECTLAYER only removes selected foreground",[]{Model m;m.code.image(4);m.code.image(5,2);m.code.number(clear_layer,{5});m.code.present();m.code.end();Resources resources;success(run(m,resources),1,1,{255,0,0});});
    suite.test("CLEAREFFECTLAYER single image leaves black",[]{Model m;m.code.image(4);m.code.number(clear_layer,{4});m.code.present();m.code.end();Resources resources;auto result=run(m,resources);success(result,1,1,{0,0,0});require(result.layers_drawn==0,"cleared layer still presented");});
    suite.test("LOAD after clear resets rectangles and rereads",[]{Model m;m.code.image(4);m.code.place(4,1,0,1,1);m.code.number(clear_layer,{4});m.code.image(4,2);m.code.present();m.code.end();Resources resources;auto result=run(m,resources);success(result,1,1,{0,255,0});require(result.resource_reads==2,"clear/load did not reread");});
    suite.test("repeated LOAD always rereads and replaces image",[]{Model m;m.code.image(4);m.code.image(4);m.code.present();m.code.end();Resources resources;int calls=0;auto reader=[&](const std::string& key,Bytes& bytes){require(key=="red","key changed");bytes=asset(++calls==1?"tiny_red":"tiny_green");return true;};auto result=scene::first_frame(script(m),reader,1,1);success(result,1,1,{0,255,0});require(calls==2&&result.resource_reads==2&&result.decoded_pixels==2,"repeat charges/cache");});
    for(const Word value:{1U,UINT32_MAX,0x80000000U})suite.test("first nonzero signed DRAW retained "+std::to_string(value),[value]{Model m;m.code.image(4);Word pc=m.code.present(value);m.code.image(5,2);m.code.number(effect,{4,1});m.code.end();Resources resources;resources.files.erase("green");auto result=run(m,resources);success(result,1,1,{255,0,0});require(result.draw_pc==pc&&bits(result.draw_value)==value&&resources.keys.size()==1,"first-DRAW boundary/PC/value");});
    suite.test("STORYNAME raw metadata is copied and truncated at62 bytes",[]{Model m;m.strings[0]=std::string(61,'s')+std::string("\x82\xa0",2);m.code.story();m.code.image(4);m.code.present();m.code.end();Resources resources;auto result=run(m,resources);success(result,1,1,{255,0,0});require(result.story_name==m.strings[0].substr(0,62),"story bytes/codepoint altered");});
    for(const std::string& key:{std::string("raw\\\x82\xa0",6),std::string(255,'k')})suite.test("reader receives exact opaque key bytes length "+std::to_string(key.size()),[key]{Model m=single();m.strings[1]=key;Resources resources;resources.files[key]=asset("tiny_red");success(run(m,resources),1,1,{255,0,0});require(resources.keys==std::vector<std::string>{key},"reader key normalized/transcoded");});
    for(const std::string& key:{std::string(),std::string(256,'k')})suite.test("invalid resource key rejected before reader "+std::to_string(key.size()),[key]{Model m=single();m.strings[1]=key;Resources resources;failure(run(m,resources));require(resources.keys.empty(),"invalid key reached reader");});
    suite.test("no prior invocation state or pixels retained",[]{Resources resources;auto first=run(single(),resources);success(first,1,1,{255,0,0});Model blank;blank.code.present();blank.code.end();success(run(blank,resources),1,1,{0,0,0});require(first.surface.rgb==Bytes{255,0,0},"returned pixels borrowed from next session");});
    suite.test("missing resource fails with no pixels",[]{Resources resources;resources.files.clear();failure(run(single(),resources));require(resources.keys.size()==1,"missing read absent");});
    suite.test("reader false ignores supplied output bytes",[]{auto reader=[](const std::string&,Bytes& bytes){bytes=asset("tiny_red");return false;};failure(scene::first_frame(script(single()),reader,1,1));});
    for(const bool standard:{false,true})suite.test(std::string("reader exception contained ")+(standard?"standard":"nonstandard"),[standard]{auto reader=[standard](const std::string&,Bytes&)->bool{if(standard)throw std::runtime_error("reader failure");throw 7;};failure(scene::first_frame(script(single()),reader,1,1));});
    suite.test("empty Reader fails cleanly on LOAD",[]{failure(scene::first_frame(script(single()),scene::Reader{},1,1));});
    for(const int variant:{0,1,2})suite.test("invalid resource rejected "+std::to_string(variant),[variant]{Resources resources;if(variant==0)resources.files["red"].clear();else if(variant==1)resources.files["red"].back()^=1;else resources.files["red"]={1,2,3};failure(run(single(),resources));});
    suite.test("nonzero PNG cut margins explicitly unsupported",[]{Resources resources;auto bytes=asset("lower");le32(bytes,16,1);resources.files["red"]=bytes;failure(run(single(),resources));});
    suite.test("resource advertised dimension product checked before decode",[]{Resources resources;auto bytes=asset("tiny_red");le32(bytes,36,UINT32_MAX);le32(bytes,40,UINT32_MAX);resources.files["red"]=bytes;failure(run(single(),resources));});
    suite.test("resource per-read encoded size bounded",[]{Resources resources;resources.files["red"].resize(scene::max_resource_bytes+1U,0);failure(run(single(),resources));});
    suite.test("cumulative encoded bytes charged across repeated loads and clear",[]{Resources resources;resources.files["red"]=metadata_heavy();Model m;for(int i=0;i<3;++i){m.code.image(4);m.code.number(clear_all,{});}m.code.present();m.code.end();auto result=run(m,resources);failure(result);require(resources.keys.size()==3&&result.resource_bytes<=scene::max_total_resource_bytes,"encoded budget refunded or not cumulative");});
    suite.test("large valid resource preflight succeeds within pixel limit",[]{Resources resources;resources.files["red"]=asset("large_black_2048");success(run(single(),resources),1,1,{0,0,0});});
    suite.test("cumulative decoded pixels charged again after clear",[]{Resources resources;resources.files["red"]=asset("large_black_2048");Model m;for(int i=0;i<3;++i){m.code.image(4);m.code.number(clear_layer,{4});}m.code.present();m.code.end();auto result=run(m,resources);failure(result);require(resources.keys.size()==3&&result.decoded_pixels<=scene::max_decoded_pixels,"decoded pixel budget refunded");});
    suite.test("eight Reader attempts are allowed",[]{Model m;for(int i=0;i<8;++i){m.code.image(4);m.code.number(clear_all,{});}m.code.present();m.code.end();Resources resources;auto result=run(m,resources);success(result,1,1,{0,0,0});require(resources.keys.size()==8&&result.resource_reads==8,"read maximum");});
    suite.test("ninth Reader attempt rejected before call",[]{Model m;for(int i=0;i<9;++i){m.code.image(4);m.code.number(clear_all,{});}m.code.present();m.code.end();Resources resources;auto result=run(m,resources);failure(result);require(resources.keys.size()==8&&result.resource_reads<=8,"read budget reset/overrun");});
    suite.test("callback budget includes metadata and DRAW",[]{Model m;for(int i=0;i<256;++i)m.code.story();m.code.present();m.code.end();Resources resources;auto result=run(m,resources);failure(result);require(result.callbacks<=scene::max_callbacks,"callback budget overrun");});
    suite.test("255 metadata callbacks plus DRAW reaches exact budget",[]{Model m;for(int i=0;i<255;++i)m.code.story();m.code.present();m.code.end();Resources resources;auto result=run(m,resources);success(result,1,1,{0,0,0});require(result.callbacks==256,"callback limit off-by-one");});
    suite.test("VM work bounds internal control loop",[]{Model m;Word pc=m.code.add({4,control,0x55560020U,32});require(pc==32,"loop PC");m.code.registry={UINT32_MAX};m.code.end();Resources resources;auto result=run(m,resources);failure(result);require(result.script_work>0&&result.script_work<=scene::max_script_work&&resources.keys.empty(),"work loop unbounded");});
    suite.test("script END without DRAW has no frame",[]{Model m;m.code.end();Resources resources;failure(run(m,resources));});
    suite.test("bound image is not implicitly presented at END",[]{Model m;m.code.image(4);m.code.end();Resources resources;failure(run(m,resources));});
    suite.test("PRINT text callback explicitly fails first-frame host",[]{Model m;m.message=true;m.code.image(4);m.code.add({6,command,0x22220001U,0,1,0});m.code.present();m.code.end();Resources resources;failure(run(m,resources));});
    suite.test("DRAW0 explicitly rejected with no partial frame",[]{Model m;m.code.image(4);m.code.present(0);m.code.end();Resources resources;failure(run(m,resources));});
    for(const auto& dimensions:std::vector<std::pair<Word,Word>>{{0,1},{1,0},{2049,1},{1,2049},{UINT32_MAX,UINT32_MAX}})suite.test("invalid output dimensions "+std::to_string(dimensions.first)+"x"+std::to_string(dimensions.second),[dimensions]{Resources resources;failure(run(single(),resources,dimensions.first,dimensions.second));require(resources.keys.empty(),"invalid canvas reached reader");});
    suite.test("maximum2048 canvas is bounded and accepted",[]{Model m;m.code.present();m.code.end();Resources resources;auto result=run(m,resources,2048,2048);success(result,2048,2048,Bytes(2048U*2048U*3U,0));});
    suite.test("script byte maximum exact accepted without executing tail",[]{Model m;m.strings={"s"};m.code.present();for(int i=0;i<21818;++i)m.code.add({3,control,0x55560001U});m.code.end();auto bytes=script(m);require(bytes.size()==scene::max_script_bytes,"script boundary construction");Resources resources;success(scene::first_frame(bytes,resources.reader(),1,1),1,1,{0,0,0});});
    suite.test("script byte maximum plus1 rejected",[]{Resources resources;Bytes bytes(scene::max_script_bytes+1U,0);failure(scene::first_frame(bytes,resources.reader(),1,1));require(resources.keys.empty(),"oversized script read resources");});
    for(const Word layer:{0U,3U,6U,UINT32_MAX,0x7fffffffU})suite.test("excluded LOAD layer "+std::to_string(layer),[layer]{Model m;m.code.image(layer);m.code.present();m.code.end();Resources resources;failure(run(m,resources));require(resources.keys.empty(),"excluded layer read bytes");});
    for(const Word code:{effect,rect})suite.test("unbound layer event rejected "+std::to_string(code),[code]{Model m;if(code==effect)m.code.number(code,{4,0});else m.code.place(4,0,0,1,1);m.code.present();m.code.end();Resources resources;failure(run(m,resources));});
    struct BadEvent{const char* name;std::function<void(Code&)> add;};
    const std::vector<BadEvent> bad{
        {"PRELOAD",[](Code& c){c.image(4,1,preload);}},
        {"OVERRAP",[](Code& c){c.number(0x22220005U,{0,1});}},
        {"MAE1",[](Code& c){c.number(effect,{4,1});}},
        {"effect-minus1",[](Code& c){c.number(effect,{4,UINT32_MAX});}},
        {"NOP extra argument",[](Code& c){c.number(effect,{4,0,99});}},
        {"STARTKOMA",[](Code& c){c.number(0x3333001cU,{1});}},
        {"STARTMESSAGE",[](Code& c){c.number(0x3333001bU,{1});}},
        {"FILMNAME",[](Code& c){c.add({5,system_function,0x33330050U,1,0});}},
        {"user-command",[](Code& c){c.add({5,0x66660003U,1,0,0});}},
        {"variable callback",[](Code& c){c.add({4,0x66660005U,0,0x80000064U});}},
        {"unequal destination rectangles",[](Code& c){c.number(rect,{4,0,0,1,1,1,0,1,1});}},
        {"oversized extent",[](Code& c){c.place(4,0,0,2,1);}},
        {"negative extent",[](Code& c){c.place(4,0,0,UINT32_MAX,1);}},
        {"extent overflow",[](Code& c){c.place(4,0,0,0x80000000U,UINT32_MAX);}},
    };
    for(const auto& item:bad)suite.test(std::string("unsupported host contract: ")+item.name,[&]{Model m;m.code.image(4);item.add(m.code);m.code.present();m.code.end();Resources resources;failure(run(m,resources));});
    // Fixed source-derived full-frame oracles supplied independently by the
    // reference task; they are never calculated by the scene under test.
    for(const auto& item:dw011_fixture::cases())suite.test(std::string("fixed scene oracle: ")+item.name,[&]{
        Resources resources;resources.files.clear();for(const auto& entry:item.resources)resources.files[entry.key]=read_file(fixtures/entry.filename);
        const auto result=scene::first_frame(read_file(fixtures/item.filename),resources.reader(),item.width,item.height);
        success(result,item.width,item.height,item.rgb32);require(resources.keys==item.reader_keys,"fixed reader sequence");
        require(result.draw_pc==item.draw_pc&&result.draw_value==item.draw_value&&result.callbacks==item.callbacks&&result.layers_drawn==item.layers_drawn&&result.decoded_pixels==item.decoded_pixels,"fixed diagnostics");
    });
    suite.test("PC calculated variables use a session RNG and replayable local clock",[]{
        Model model;model.message=true;
        auto copy=[&](Word to,Word from){
            model.code.add({8,0x66660005U,0,0xa0000000U,3,0x80000000U|to,0x80000000U|from,0xa000003dU});
        };
        copy(107,5);copy(108,6);
        model.code.add({8,0x66660005U,0,0xa0000000U,3,0x80000005U,0x90000136U,0xa000003dU});
        model.code.add({8,0x66660005U,0,0xa0000000U,3,0x80000006U,0x900007e9U,0xa000003dU});
        copy(101,0);copy(102,1);copy(103,2);copy(104,3);copy(105,4);copy(106,7);
        model.code.print(1);copy(101,0);model.code.print(2);model.code.end();
        TempGame temp(model);
        const std::string names="rnd\nsystemyear\nsystemmillisecond\nweek\nholiday\ndate\nyear\ndirect3d\n";
        Bytes encrypted(names.begin(),names.end());
        for (auto& byte:encrypted) byte^=0xffU;
        temp.write("spt/var.fxf",encrypted);
        temp.text("spt/varinitdata.xtx","0\n0\n0\n0\n0\n310\n2025\n0\n");
        temp.text("init/game.xtx","useSaijistu 1\n");
        temp.text("nya/saijitsu.xtx","2025 3 10\n");
        auto options=test_options();options.random_seed=1;
        int clock_calls=0;
        options.local_clock=[&]{++clock_calls;return scene::ScriptClockSample{20260308,12345};};
        options.runtime=std::make_shared<nnn::RuntimeContext>(temp.root);
        scene::GameSession game(temp.root,options);
        require(game.step_frame(0).wait==scene::Wait::Dialogue,"computed script did not reach dialogue");
        require(game.variable(101)==41 && game.variable(102)==2026 && game.variable(103)==345 && clock_calls==1,
            "PC RNG or coalesced local time differed");
        require(game.variable(104)==1 && game.variable(105)==1 && game.variable(106)==2 &&
            game.variable(107)==101 && game.variable(108)==2000,"PC calendar or capability value differed");
        auto saved=game.snapshot();
        scene::GameSession decoration(temp.root,options);
        decoration.step_frame(0);
        scene::InputState advance;advance.advance=true;
        game.step_frame(0,advance);
        require(game.variable(101)==8467,"other session disturbed the RNG sequence");
        game.restore_snapshot(saved);game.step_frame(0,advance);
        require(game.variable(101)==8467,"snapshot lost the RNG state");
        scene::GameSession replay(temp.root,options);
        scene::InputState recorded;recorded.recorded_clock=true;
        recorded.clock_sample=scene::ScriptClockSample{20280301,76543};
        const auto previous_calls=clock_calls;
        replay.step_frame(0,recorded);
        require(replay.variable(102)==2028 && replay.variable(103)==543 && clock_calls==previous_calls,
            "journal replay used the current clock");
        scene::GameSession legacy(temp.root,options);
        scene::InputState old;old.legacy_calculations=true;
        legacy.step_frame(0,old);
        require(legacy.variable(101)==0 && legacy.variable(102)==0 && legacy.variable(107)==310 &&
            legacy.variable(108)==2025 && clock_calls==previous_calls,
            "old journal used new calculated variables");
    });
    std::cout<<"SCENE RESULT "<<suite.passed<<" passed, "<<suite.failed<<" failed\n";return suite.failed==0?0:1;
}
