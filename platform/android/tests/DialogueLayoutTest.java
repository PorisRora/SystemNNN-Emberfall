package com.tyranor.runtime.systemnnn;

import java.util.HashMap;
import java.util.Collections;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

public final class DialogueLayoutTest {
    private static int checks;
    private static void check(boolean value,String message){checks++;if(!value)throw new AssertionError(message);}
    private static void put(Map<String,String> m,String key,Object value){m.put(key.toLowerCase(Locale.ROOT),value.toString());}
    private static void text(DialogueLayout.Text t,int font,int x,int y,int nx,int ny,int space){
        check(t.font==font&&t.x==x&&t.y==y&&t.nextX==nx&&t.nextY==ny&&t.space==space,"text layout");
    }
    private static Map<String,String> read(Path path)throws Exception{
        Map<String,String> fields=new HashMap<>();
        for(String line:Files.readAllLines(path,StandardCharsets.ISO_8859_1)){
            String[] pair=line.trim().split("\\s+",2);if(pair.length==2)put(fields,pair[0],pair[1]);
        }
        return fields;
    }
    public static void main(String[] args)throws Exception{
        for(int s:new int[]{1,3})for(int c:new int[]{1,2}){
            DialogueLayout.Counts n=new DialogueLayout.Counts(c,s,2);
            int j=s-1,i=c-1;
            String suffix=s>1&&c>1?s+"_"+c:Integer.toString(s<=1?c:s);
            check(DialogueLayout.key("filenameWindow",n,j,i,0).equals("filenameWindow"+suffix),"base key");
            check(DialogueLayout.key("filenameNameWindow",n,j,i,1).equals("filenameNameWindow"+suffix+"_Font1"),"font name key");
            check(DialogueLayout.index(n,j,i,1)==s*c+j*c+i,"index");
            check(DialogueLayout.index(n,999,-2,999)==s*c+(s-1)*c,"clamped index");
            Map<String,String> m=new HashMap<>();put(m,"filenameWindow","base");
            put(m,DialogueLayout.key("filenameWindow",n,j,i,1),"variant");
            String[] names=DialogueLayout.names(m,"filenameWindow","ta_meswin",n);
            check(names[DialogueLayout.index(n,j,i,1)].equals("variant"),"variant lookup");
            check(names[DialogueLayout.index(n,j,i,0)].equals("base"),"fallback uses base not unfonted variant");
            check(DialogueLayout.names(new HashMap<>(),"filenameNameWindow","ta_namewin",n)[0].equals("ta_namewin"),"source default");
        }
        Map<String,String> m=new HashMap<>();
        put(m,"fontSizeTypeNumber",4);put(m,"fontSize",30);put(m,"printX",180);put(m,"printY",580);
        put(m,"nextX",0);put(m,"nextY",30);put(m,"space",0);
        int[][] values={{30,150,170,0,42,0},{30,180,580,0,30,0},{30,230,300,0,68,0}};
        String[] keys={"fontSize","printX","printY","nextX","nextY","space"};
        for(int i=0;i<values.length;i++)for(int k=0;k<keys.length;k++)put(m,keys[k]+"ByFontSizeType"+(i+1),values[i][k]);
        DialogueLayout.Text[] layouts=DialogueLayout.layouts(m);check(layouts.length==5,"type count");
        text(layouts[0],30,180,580,0,30,0);text(layouts[1],30,150,170,0,42,0);
        text(layouts[2],30,180,580,0,30,0);text(layouts[3],30,230,300,0,68,0);
        text(layouts[4],30,180,580,0,30,0);text(DialogueLayout.layout(layouts,-1),30,180,580,0,30,0);
        check(DialogueLayout.layout(layouts,99)==layouts[4],"font clamp");
        text(DialogueLayout.layouts(new HashMap<>())[0],24,100,100,0,30,1);
        Map<String,String> names=new HashMap<>();
        put(names,"nameX",80);put(names,"nameY",535);put(names,"nameFontSize",30);put(names,"nameLengthMax",6);
        put(names,"nameSpace",4);put(names,"nameXByFontSizeType1",140);
        put(names,"nameYByFontSizeType1",250);put(names,"nameSpaceByFontSizeType1",7);
        DialogueLayout.Name[] nameLayouts=DialogueLayout.nameLayouts(names,layouts,false);
        check(nameLayouts[0].y==535&&nameLayouts[0].space==1&&nameLayouts[0].printX("ABCD")==111,"base font slot spacing");
        check(nameLayouts[1].y==250&&nameLayouts[1].space==7&&nameLayouts[1].printX("ABCD")==177,"font-specific name coordinates");
        check(nameLayouts[2].space==4&&nameLayouts[2].printX("ABCDEF!") == 80,"long name starts at its anchor");
        put(names,"nameCenterFlag",2);
        check(DialogueLayout.nameLayouts(names,new DialogueLayout.Text[]{layouts[0]},true)[0].printX("ABCD")==118,
            "one-byte name uses half-width cells and right alignment");
        put(names,"nameCenterFlag",0);
        check(DialogueLayout.nameLayouts(names,layouts,false)[0].printX("A")==80,"left-aligned name");
        Set<String> known=Collections.singleton("Known");
        String dialogue="Known\nText";
        DialogueLayout.Message named=DialogueLayout.split(dialogue,true,true,true,known);
        check(named.name.equals("Known")&&named.body.equals("Text")&&named.nameWindow,"known name gets its own text and image");
        DialogueLayout.Message noName=DialogueLayout.split(dialogue,true,false,true,known);
        check(noName.name.isEmpty()&&noName.body.equals(dialogue)&&noName.nameWindow,"nameFlag preserves the first row independently of the image");
        DialogueLayout.Message plain=DialogueLayout.split("Unknown\nText",true,true,true,known);
        check(plain.name.isEmpty()&&plain.body.equals("Unknown\nText")&&!plain.nameWindow,"automatic names require the palette entry");
        DialogueLayout.Message forced=DialogueLayout.split("Unknown\nText",true,true,false,known);
        check(forced.name.equals("Unknown")&&forced.body.equals("Text")&&!forced.nameWindow,"forced name has text without requiring an image");
        DialogueLayout.Message longText=DialogueLayout.split(dialogue,false,true,true,known);
        check(longText.name.isEmpty()&&longText.body.equals(dialogue)&&!longText.nameWindow,"LPRINT keeps the first row");
        check(DialogueLayout.usesNameTable(names),"name color table is enabled by default");
        put(names,"nameColorChangeFlag",0);
        check(!DialogueLayout.usesNameTable(names),"both disabled flags use default name color");
        put(names,"autoNameWindowFlag",1);
        check(DialogueLayout.usesNameTable(names),"automatic name windows still need the name table");
        put(m,"LPrintX",110);put(m,"LPrintY",220);put(m,"LNextX",-13);put(m,"LNextY",36);
        DialogueLayout.Text lprint=DialogueLayout.longLayouts(m,layouts)[1];
        text(lprint,30,110,220,-13,36,0);
        check(DialogueLayout.lineX(lprint,0,1280)==110&&DialogueLayout.lineX(lprint,2,1280)==84&&
            DialogueLayout.lineX(lprint,Integer.MAX_VALUE,1280)==0,"logical rows use LNextX with clamped coordinates");
        put(m,"fontSizeByFontSizeType1",100);check(DialogueLayout.layouts(m)[1].font==72,"upstream font maximum");
        put(m,"windowNumberConfig",1000);put(m,"windowNumberScript",1000);put(m,"windowNumberFontSizeType",1000);
        check(DialogueLayout.counts(m).size()<=DialogueLayout.MAX_VARIANTS,"bounded window table");
        put(m,"fontSizeTypeNumber",1000);check(DialogueLayout.layouts(m).length==DialogueLayout.MAX_LAYOUTS,"bounded layout table");
        if(args.length>0){
            Path root=Paths.get(args[0]);Map<String,String> real=read(root.resolve("init/printmessage.xtx"));
            DialogueLayout.Text[] actual=DialogueLayout.layouts(real);
            check(actual.length==5,"DEA font count");text(actual[0],30,180,580,0,30,0);
            text(actual[1],30,150,170,0,42,0);text(actual[2],30,180,580,0,30,0);
            text(actual[3],30,230,300,0,68,0);text(actual[4],30,180,580,0,30,0);
            Map<String,String> windows=read(root.resolve("init/messageWindow.xtx"));DialogueLayout.Counts n=DialogueLayout.counts(windows);
            check(n.config==1&&n.script==4&&n.font==1,"DEA window counts");
            String[] realNames=DialogueLayout.names(windows,"filenameWindow","ta_meswin",n);
            check(realNames[0].equals("ta_meswin01")&&realNames[1].equals("ta_meswin02")&&
                realNames[2].equals("ta_meswin02")&&realNames[3].equals("ta_meswin02"),"DEA window names");
            String[] realNameWindows=DialogueLayout.names(windows,"filenameNameWindow","ta_namewin",n);
            check(realNameWindows[0].equals("ta_meswin_name01")&&realNameWindows[1].equals("ta_meswin_name02"),"DEA name windows");
        }
        System.out.println("PASS "+checks+" dialogue layout checks");
    }
}
