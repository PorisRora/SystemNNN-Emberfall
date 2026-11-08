package com.tyranor.runtime.systemnnn;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;

/** The production configuration reader runs without graphics/JNI calls. */
public final class GameConfigTest {
    private static int checks;
    private static void check(boolean b,String message){if(!b)throw new AssertionError(message);checks++;}
    private static void write(File file,String text,boolean encrypted)throws Exception{
        file.getParentFile().mkdirs();byte[] bytes=text.getBytes(StandardCharsets.ISO_8859_1);
        if(encrypted)for(int i=0;i<bytes.length;i++)bytes[i]^=(byte)255;
        Files.write(file.toPath(),bytes);
    }
    private static void remove(File file){if(file.isDirectory())for(File child:file.listFiles())remove(child);file.delete();}
    private static byte[] originalValue(File root,String field)throws Exception{
        File file=new File(root,"init2/config.fxf");boolean encrypted=file.isFile();
        if(!encrypted)file=new File(root,"init/config.xtx");
        byte[] input=Files.readAllBytes(file.toPath());if(encrypted)for(int i=0;i<input.length;i++)input[i]^=(byte)255;
        byte[] key=field.getBytes(StandardCharsets.US_ASCII);
        for(int start=0;start<input.length;){
            int end=start;while(end<input.length&&input[end]!=10&&input[end]!=13)end++;
            int first=start,last=end;while(first<last&&(input[first]==32||input[first]==9))first++;
            while(last>first&&(input[last-1]==32||input[last-1]==9))last--;
            if(last-first>key.length){
                boolean match=true;for(int i=0;i<key.length;i++)if(input[first+i]!=key[i])match=false;
                int value=first+key.length;
                if(match&&(input[value]==32||input[value]==9)){
                    while(value<last&&(input[value]==32||input[value]==9))value++;
                    return Arrays.copyOfRange(input,value,last);
                }
            }
            start=end+1;
        }
        throw new AssertionError("Missing original value "+field);
    }
    private static byte[] hex(String value){
        byte[] bytes=new byte[value.length()/2];
        for(int i=0;i<bytes.length;i++)bytes[i]=(byte)Integer.parseInt(value.substring(i*2,i*2+2),16);
        return bytes;
    }
    // Independently fixed original bytes and complete visible text. These do not
    // derive the expected text by invoking either decoder profile.
    private static final String[][] PREVIEWS={
        {"windowMessage1","81a18db68fe382cc825282c282cc835e834f82c594778c69835483938376838b82f082ab82e882a982a682e782ea82dc82b78142","■左上の３つのタグで背景サンプルをきりかえられます。"},
        {"nameWindowMessage","83818362835a815b835783458343839383688345","メッセ—ジウインドウ"},
        {"sampleText1","81a18cbb8ddd90dd92e882b382ea82c482a282e983818362835a815b835783588373815b836882cc835483938376838b82c582b78142","■现在设定されているメッセ—ジスピ—ドのサンプルです。"},
        {"sampleText2","814093c782dd82e282b782a283588373815b836882c992b290ae82b582c482ad82be82b382a28142","　读みやすいスピ—ドに调整してください。"},
        {"windowMessage1","81a1935f820c8db68fe38a709349825298a2880f88c489c288c88d5889fc94778c6982158eae","■点击左上角的３个图案可以更改背景样式"},
        {"nameWindowMessage","823782329e79","对话框"},
        {"sampleText1","81a18ddd811797a289c2851c832795b68e9a833e8ea691ac93788143","■在这里可测试文字显示速度，"},
        {"sampleText2","81408212850490ae939e95fb95d68ea98cc88f128b08934991ac93788142","　请调整到方便自己阅读的速度。"},
        {"windowMessage1","81a1ea7981148db68fe38a709349338cc29aa488c489c288c88d5889fc94778c699ee98eae","■點擊左上角的3個圖案可以更改背景樣式"},
        {"nameWindowMessage","9b9498629e79","對話框"},
        {"sampleText1","81a18ddd948797a189c291aa8e8e95b68e9ae8fb8ea691ac93788143","■在這裡可測試文字顯示速度，"},
        {"sampleText2","814090bf92b290ae939e95fb95d68ea98cc88524e6a4934991ac93788142","　請調整到方便自己閱讀的速度。"}
    };
    public static void main(String[] args)throws Exception{
        check(GameUiAssets.systemImageKey("TA_Load_Slot01.DWQ").equals("sys/ta_load_slot01")&&
            GameUiAssets.imageKey("TA\\TA_Load_Slot01.DWQ").equals("ta/ta_load_slot01"),"system and script sprites retain distinct routes");
        check(GameUiAssets.systemImageKey("\u0081\\A.DWQ").equals("sys/\u0081\\a"),"CP932 trail byte is not a path separator");
        byte[] japanese=GameResourcePaths.encodeFilename("ソ.DWQ".getBytes(StandardCharsets.UTF_8));
        check(Arrays.equals(japanese,hex("835c2e445751"))&&GameResourcePaths.decodeFilename(japanese).equals("ソ.DWQ"),
            "physical UTF-8 name round-trips the CP932 backslash trail");
        check(GameResourcePaths.encodeFilename("😀".getBytes(StandardCharsets.UTF_8))==null,
            "unrepresentable filenames do not produce question-mark aliases");
        File root=Files.createTempDirectory("systemnnn-config-").toFile();
        try{
            write(new File(root,"SpT/SyS.SpT"),"test",false);
            File script=GameResourcePaths.file(root,"spt","sys.spt");
            check(script!=null&&script.isFile(),"mixed-case script directory and filename");
            check(GameUiAssets.readConfig(root,"title").value("backScriptName","fallback").equals("fallback"),"missing title fallback");
            write(new File(root,"init2/title.fxf"),"backScriptName encrypted_title\r\nfontSize 24\r\nFONTSIZE 42\r\n@fontSize 99\r\nmenu1Button1PrintX 128\r\n",true);
            GameUiAssets.Config config=GameUiAssets.readConfig(root,"title");
            check(config.value("BACKSCRIPTNAME","").equals("encrypted_title"),"FXF XOR and case folding");
            check(config.number("fontSize",0)==24&&config.number("menu1Button1PrintX",0)==128,"FXF numbers and disabled lines");
            write(new File(root,"init/title.xtx"),"backScriptName plain_title\n",false);
            check(GameUiAssets.readConfig(root,"title").value("backScriptName","").equals("encrypted_title"),"PC init2 priority");
            write(new File(root,"init2/messageWindow.fxf"),"filenameWindow1 game_window\nnamePrintX 46\n",true);
            check(GameUiAssets.readConfig(root,"messageWindow").number("namePrintX",0)==46,"message FXF");
            write(new File(root,"init/extended.xtx"),"modePageMax 2\nexpRadioNumber 17\nexpCheckNumber 9\n"+
                "exp1Radio 3\nexp1RadioPrintPage 2\nexp1Radio1Button1PrintX 80\nexp1Radio1Button1SizeX 50\n"+
                "exp2RadioPrintPage 3\n",false);
            OriginalSettingsModel extended=new OriginalSettingsModel(GameUiAssets.readConfig(root,"extended"));
            OriginalSettingsModel.Extended radio=extended.extended.get(0);
            check(extended.original&&radio.page==2&&radio.buttons.length==3&&radio.buttons[2].x==180,
                "extended radio uses original page and previous option geometry");
            check(extended.extended.size()==24&&radio.parameter()==300&&radio.locked(1),"radio count, page bounds and mask");
            check(extended.extended.stream().filter(group->group.index==8).noneMatch(group->group.locked(-1))&&
                extended.extended.stream().anyMatch(group->group.parameter()==400&&group.locked(256)&&!group.locked(1)),
                "only the first eight groups of each kind are maskable");
            write(new File(root,"init/buttonInheritance.xtx"),"modePageMax 3\nmusicVolumePrintX 500\nmusicBarFileName bar\n"+
                "voiceCutButtonNumber 3\nvoiceAutoLineSetFlag 1\nvoiceAutoLinePrintX 10\nvoiceAutoLinePrintY 20\nvoiceAutoLineNextX 150\n"+
                "voice1Button1SizeX 80\nvoice1Button1SizeY 36\nvoice1Button1FileName first\nvoice1Button1CheckSizeX 60\n"+
                "voice1Button1TateYoko 1\nvoice1Button1PicSrcX2 84\nvoice1Button1PicSrcY2 12\nvoice1Button1AnimePattern2 4\n"+
                "voice1Button2SizeX 96\nvoice1Button2SizeY 44\nvoice1Button2FileName active\nvoice1Button2PicSrcY3 78\n"+
                "voice1Button2CheckDeltaX 4\nvoice1Button2CheckPicY 3\nvoice1Button2ClickTime 5\nvoice1Button2ClickSound 7\n"+
                "voice1Button2ClickVoice 6\nvoice1Button2PrintMode 0\nvoice3Button1PrintX 320\nvoice3Button2PrintY 42\n"+
                "tab1Button1SizeX 100\ntab1Button1SizeY 50\ntab1Button1PicSrcX2 15\ntab1Button1AnimePattern2 3\n"+
                "tab1Button2SizeX 120\ntab1Button2PrintX 50\ntab1Button2PrintY 37\n"+
                "systemVoice1Button1FileName theme\nsystemVoice1Button1SizeX 160\n",false);
            write(new File(root,"init/buttonGame.xtx"),"notSrcCopyFlagEnable 1\nuseSystemVoice 2\n",false);
            OriginalSettingsModel buttons=new OriginalSettingsModel(GameUiAssets.readConfig(root,"buttonInheritance"),
                GameUiAssets.readConfig(root,"buttonGame"),1280,720);
            OriginalSettingsModel.Button firstVoice=buttons.characters.get(0).button,thirdVoice=buttons.characters.get(2).button;
            check(firstVoice.off.width==80&&firstVoice.off.height==36&&firstVoice.off.hitWidth==60&&
                firstVoice.on.width==96&&firstVoice.on.height==44&&firstVoice.on.hitWidth==96,
                "each state has its own source and hit dimensions");
            ButtonArt.Setup voice=thirdVoice.off;
            check(voice.width==96&&voice.height==44&&voice.art.image.equals("active")&&voice.art.horizontal&&
                voice.art.states[1].x==84&&voice.art.states[1].y==12&&voice.art.states[1].frames==4&&voice.art.states[2].y==78,
                "third character inherits resolved second-state atlas and animation across omitted fields");
            check(voice.hitX==4&&voice.hitPicY==3&&!voice.pixelHit&&voice.hitWidth==96&&voice.hitHeight==44&&
                voice.clickTime==5&&voice.sound==6&&voice.voice==5,
                "third character inherits click and hit rules, with hit size defaulting to its final dimensions");
            check(thirdVoice.x==320&&thirdVoice.y==20&&thirdVoice.onX==320&&thirdVoice.onY==42,
                "explicit positions override the character grid and the second state inherits the first position");
            OriginalSettingsModel.Button tab=buttons.tabs.get(2);
            check(tab.x==290&&tab.y==37&&tab.off.width==120&&tab.off.art.states[1].x==120&&
                tab.off.art.states[1].frames==3,"radio position inherits previous ON and optional source reset retains animation");
            check(buttons.systemVoices.size()==3&&buttons.systemVoices.get(2).off.art.image.equals("theme")&&
                buttons.systemVoices.get(2).off.width==160,"game system voice count allows sparse atlas declarations");
            write(new File(root,"init2/config.fxf"),"musicVolumePrintX 100\nmusicBarFileName bar\nvolumeNumber 4\nvolumeSizeX1 320\nvolumeSizeY1 13\nfilenameBG first_bg\nfilenameBG3 third_bg\nmodePageMax 3\npageMode 1\nvoiceCutButtonNumber 2\nvoiceCutButtonCustomFlag 1\nvoiceOffNumberByButton1 2\nvoiceOffButton1chara1 7\nvoiceOffNumberByButton2 1\n",true);
            OriginalSettingsModel inherited=new OriginalSettingsModel(GameUiAssets.readConfig(root,"config"));
            check(inherited.pageMode==1&&inherited.sliders.get(3).width==320&&inherited.sliders.get(3).height==13,"source slider size inheritance and page mode");
            check(inherited.background(2).equals("first_bg")&&inherited.background(3).equals("third_bg"),"source page background inheritance");
            check(java.util.Arrays.equals(inherited.characters.get(0).ids,new int[]{7,8})&&inherited.characters.get(1).ids[0]==9,"source custom character ID defaults advance globally");
            OriginalSettingsModel small=new OriginalSettingsModel(GameUiAssets.readConfig(root,"config"),100,50);
            check(small.sliders.get(0).width==100&&small.sliders.get(0).height==13,"small canvas bounds");
            write(new File(root,"init2/config.fxf"),"musicVolumePrintX 0\nmusicBarFileName bar\nvolumeNumber 1\nvolumeSizeX1 1600\nvolumeSizeY1 900\n",true);
            OriginalSettingsModel large=new OriginalSettingsModel(GameUiAssets.readConfig(root,"config"),1920,1080);
            check(large.sliders.get(0).width==1600&&large.sliders.get(0).height==900,"large canvas bounds");
            write(new File(root,"init2/config.fxf"),"volumeNumber 8\nmodePageMax 2\nwindowPercentVolumePrintPage 2\nautoSpeedVolumePrintPage 2\n",true);
            GameUiAssets.Config declared=GameUiAssets.readConfig(root,"config");
            OriginalSettingsModel legacy=new OriginalSettingsModel(declared,null,800,600,true);
            check(new OriginalSettingsModel(declared).sliders.size()==6&&legacy.sliders.size()==8,
                "only legacy profile enables all declared sliders by default");
            for(int i:new int[]{0,6,7})check(legacy.sliders.get(i).button==null,"text and window sliders have no independent switch");
            check(legacy.sliders.get(6).page==2&&legacy.sliders.get(7).page==2,"legacy text-page positions use the config");
            write(new File(root,"init2/config.fxf"),"volumeNumber 8\nautoSpeedVolumeExistFlag 0\nwindowPercentVolumeExistFlag 1\n",true);
            GameUiAssets.Config flags=GameUiAssets.readConfig(root,"config");
            for(boolean old:new boolean[]{true,false}){
                OriginalSettingsModel explicit=new OriginalSettingsModel(flags,null,800,600,old);
                check(explicit.sliders.size()==7&&explicit.sliders.get(6).index==6,"explicit slider flags override the profile");
            }
            byte[] mapping=new byte[SjisTunnelDecoder.MAX_MAPPINGS*2];
            for(int i=0;i<SjisTunnelDecoder.MAX_MAPPINGS;i++){int cp=0x4e00+i;mapping[i*2]=(byte)cp;mapping[i*2+1]=(byte)(cp>>8);}
            Files.write(new File(root,"sjis_ext.bin").toPath(),mapping);
            Files.write(new File(root,"uif_config.json").toPath(),"{\"character_substitution\":{\"enable\":true,\"source_characters\":\"一\",\"target_characters\":\"壹\"}}".getBytes(StandardCharsets.UTF_8));
            StringBuilder controls=new StringBuilder();
            for(int low=1;low<32;low++)if(low!=9&&low!=10&&low!=13)controls.append("  tunnel").append(low).append("\t\u0081").append((char)low).append(" \t\r\n");
            controls.append("literal\t\u001fA\u0001\t \r\nnotASplit\u000bwrong\tignored\r\nnumber\t24\tcomment\r\nlast \u0081\u0002");
            write(new File(root,"init/controls.xtx"),controls.toString(),false);
            GameUiAssets.Config preserved=GameUiAssets.readConfig(root,"controls");GameTextDecoder expectedCodec=GameTextDecoder.load(root);
            for(int low=1;low<32;low++)if(low!=9&&low!=10&&low!=13){
                byte[] pair={(byte)0x81,(byte)low};String field="tunnel"+low;
                check(Arrays.equals(preserved.value(field,"").getBytes(StandardCharsets.ISO_8859_1),pair),"trailing tunnel byte preserved "+low);
                check(preserved.text(field,"").equals(expectedCodec.decodeConfiguration(pair)),"tunnel/UIF text decoding "+low);
            }
            check(preserved.text("tunnel1","").equals("壹"),"configuration text uses UIF after sjis_ext");
            check(preserved.value("literal","").equals("\u001fA\u0001"),"literal low bytes preserved at both ends");
            check(!preserved.has("notASplit"),"vertical tab is not a key/value delimiter");
            check(preserved.number("number",0)==24,"numeric ASCII tab delimiter");
            check(preserved.value("last","").equals("\u0081\u0002"),"tunnel byte at EOF without newline");
            Files.write(new File(root,"init2/printmessage.fxf").toPath(),new byte[1024*1024+1]);
            try{GameUiAssets.readConfig(root,"printmessage");throw new AssertionError("Oversize config accepted");}catch(IOException expected){checks++;}
        }finally{remove(root);}
        if(args.length>0){
            root=new File(args[0]);GameUiAssets.Config title=GameUiAssets.readConfig(root,"title");
            check(title.value("backScriptName","").equals("MNG_backscript"),"real MNG title");
            check(title.number("menu1Button1PrintX",0)==128&&title.number("menu1Button1SizeX",0)==247,"real MNG title geometry");
            check(GameUiAssets.readConfig(root,"printmessage").number("fontSize",0)==24,"real MNG font");
            check(GameUiAssets.readConfig(root,"printmessage").number("printX",0)==230,"real MNG body position");
            check(GameUiAssets.readConfig(root,"messageWindow").number("namePrintX",0)==46,"real MNG name position");
            OriginalSettingsModel model=new OriginalSettingsModel(GameUiAssets.readConfig(root,"config"));
            check(model.original&&model.pages==3&&model.tabs.size()==3,"real three-page settings");
            check(model.sliders.size()==10,"all original volume/speed/opacity sliders");
            for(OriginalSettingsModel.Slider s:model.sliders){
                check(s.width==400&&s.height==14,"indexed original slider dimensions");
                if(s.index==9)check(s.x==654&&s.y==161&&s.key.equals("scriptSoundVolume"),"script SE geometry and separate parameter");
                if(s.index==8)check(s.y==82&&s.key.equals("masterVolume"),"master volume geometry");
                if(s.index==2)check(s.y==201&&s.key.equals("soundVolume"),"system SE geometry");
            }
            check(model.characters.size()==7&&model.characterMin==-10&&model.characterMax==5,"character groups and range");
            check(java.util.Arrays.equals(model.characters.get(6).ids,new int[]{7,8,9,10}),"shared other-character group");
            OriginalSettingsModel.CharacterControl first=model.characters.get(0);
            check(first.x==371&&first.y==448&&first.width==120&&first.height==11,"relative character slider placement");
            check(model.checks.size()==5&&model.modes.size()==3&&model.systemVoices.size()==7,"all original checks/radios/themes");
            check(model.page("systemVoiceButtonPrintPage",1)==3,"theme page declaration");
            check(model.tabs.get(0).on.art.image.equals("ta_cnf_pgbt11"),"radio selected state is Button2");
            check(model.characters.get(0).button.off.art.image.equals("ta_cnf_vocha12"),"character state0 uses Button1 (muted)");
            check(GameUiAssets.readConfig(root,"config").imageNames().contains("sys/ta_cnf_sysvocha71"),"theme images are preloaded from the system route");
        }
        for(int i=1;i<args.length;i++){
            File game=new File(args[i]);
            GameUiAssets.Config title=GameUiAssets.readConfig(game,"title");
            check(title.value("backScriptName","").equals("DEA_backscript"),"existing XTX title");
            OriginalSettingsModel model=new OriginalSettingsModel(GameUiAssets.readConfig(game,"config"),
                GameUiAssets.readConfig(game,"game"),1280,720);
            check(model.sliders.stream().filter(slider->slider.button!=null).allMatch(slider->
                slider.button.off.width==76&&slider.button.off.height==40&&slider.button.on.width==76&&slider.button.on.height==40),
                "all DEA audio switches use the declared 76 by 40 cell");
            check(model.characters.size()==6&&model.characters.stream().allMatch(group->
                group.button.off.width==256&&group.button.on.width==256&&group.button.on.height==52&&
                group.button.on.art.states[1].y==52&&group.button.on.art.states[1].frames==4&&group.button.on.art.states[2].y==260),
                "all DEA character states use the inherited atlas geometry and animation");
            check(model.tabs.size()==3&&model.tabs.get(2).on.art.states[1].y==50&&
                model.tabs.get(2).on.art.states[1].frames==4,"DEA third tab inherits hover frames");
            check(model.systemVoices.size()==7&&model.systemVoices.get(0).off.width==192&&
                model.systemVoices.subList(1,7).stream().allMatch(button->button.off.width==272&&button.on.height==52),
                "DEA system voice sizes follow the previous resolved theme instead of the first theme");
        }
        for(String path:args){
            root=new File(path);GameUiAssets.Config config=GameUiAssets.readConfig(root,"config");GameTextDecoder codec=GameTextDecoder.load(root);
            GameUiAssets.Config cursor=GameUiAssets.readConfig(root,"messageCursor");
            int frames=cursor.number("animePattern1",0);
            check(frames>1&&frames==cursor.number("ANIMEPATTERN1",0),"real cursor frame count through case folding: "+root.getName());
            for(String field:new String[]{"windowMessage1","sampleText1","sampleText2"}){
                byte[] original=originalValue(root,field);
                check(Arrays.equals(config.value(field,"").getBytes(StandardCharsets.ISO_8859_1),original),"real complete value bytes: "+root.getName()+"/"+field);
                check(config.text(field,"missing").equals(codec.decodeConfiguration(original)),"real configuration decoder equality: "+root.getName()+"/"+field);
            }
            if(new File(root,"uif_config.json").isFile())check(config.text("sampleText1","").startsWith("■现在设定"),"real MNG FXF + UIF Chinese substitution");
        }
        if(args.length==3){
            for(int game=0;game<3;game++){
                root=new File(args[game]);GameUiAssets.Config config=GameUiAssets.readConfig(root,"config");GameTextDecoder codec=GameTextDecoder.load(root);
                for(int field=0;field<4;field++){
                    String[] fixed=PREVIEWS[game*4+field];byte[] original=originalValue(root,fixed[0]);
                    check(Arrays.equals(original,hex(fixed[1])),"fixed original preview bytes "+game+"/"+fixed[0]);
                    check(codec.decodeConfiguration(hex(fixed[1])).equals(fixed[2]),"fixed complete preview meaning "+game+"/"+fixed[0]);
                    check(config.text(fixed[0],"missing").equals(fixed[2]),"production complete preview meaning "+game+"/"+fixed[0]);
                }
                if(game>0){
                    String[] discriminating=PREVIEWS[game*4+(game==1?1:3)];
                    check(!codec.decode(hex(discriminating[1])).equals(discriminating[2]),"configuration cannot use canonical58 profile "+game);
                }
            }
        }
        System.out.println("PASS "+checks+" configuration checks");
    }
}
