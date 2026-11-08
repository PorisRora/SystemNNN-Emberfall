package com.tyranor.runtime.systemnnn;

import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.*;
import java.nio.file.Files;
import java.util.regex.*;

/** Pure JVM checks for the optional UIF table and unchanged sjis_ext decoding. */
public final class GameTextDecoderTest {
    private static int checks;
    private static void check(boolean b,String detail){if(!b)throw new AssertionError(detail);checks++;}
    private interface Action{void run()throws Exception;}
    private static void invalid(Action action)throws Exception{
        try{action.run();throw new AssertionError("Expected rejection");}catch(IOException expected){checks++;}
    }
    private static byte[] utf8(String value){return value.getBytes(StandardCharsets.UTF_8);}
    private static byte[] sjis(String value)throws Exception{
        ByteBuffer b=Charset.forName("windows-31j").newEncoder().onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT).encode(java.nio.CharBuffer.wrap(value));
        byte[] result=new byte[b.remaining()];b.get(result);return result;
    }
    private static byte[] config(String source,String target){return utf8("{\"character_substitution\":{\"enable\":true,\"source_characters\":\""+source+"\",\"target_characters\":\""+target+"\"}}");}
    private static byte[] hex(String value){
        byte[] bytes=new byte[value.length()/2];
        for(int i=0;i<bytes.length;i++)bytes[i]=(byte)Integer.parseInt(value.substring(i*2,i*2+2),16);
        return bytes;
    }
    private static byte[] story(File root,int message)throws Exception{
        byte[] data=Files.readAllBytes(new File(root,"spt/dea_00_00.spt").toPath());
        for(int i=0;i<data.length;i++)data[i]^=(byte)255;
        ByteBuffer words=ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        int count=words.getInt(4*4),table=words.getInt(5*4);
        if(message<0||message>=count||table<0||(long)(table+message)*4+4>data.length)throw new AssertionError("SPT string table bounds");
        int start=words.getInt((table+message)*4)*4,end=start;
        if(start<0||start>=data.length)throw new AssertionError("SPT string bounds");
        while(end<data.length&&data[end]!=0)end++;
        if(end==data.length)throw new AssertionError("Unterminated SPT string");
        return java.util.Arrays.copyOfRange(data,start,end);
    }
    private static final String[][] STORIES={
        {"1","14","9241814389e4812d8f59812689e489ef98f48117812e9aeb811f81480d0a","但，我为什么我会做这种梦呢？\r\n"},
        {"1","50","8cf590620d0a816799df8104814388f6812d974c97b990569349822e97bf821e822f8143821e822f822293be8d5890b6822197b98143924195730d0a926d93b9905e813a939e8136812e92f69378811f814281680d0a","光臣\r\n“听说，因为有了新的资料视频，视频变得更生动了，但不\r\n知道真实到哪种程度呢。”\r\n"},
        {"2","100","94fc9454979c0d0a8175812d89c691b0934990b688d38d449c419fa081058163816381760d0a","美乃梨\r\n「你家族的生意好廣泛啊……」\r\n"},
        {"2","109","88ea8cc290a590ab8a699aa28a8a93499744939990b6814392418bf48ea298498f6fe76a97a6934991d4937881438ea78a8ee3600d0a8ab4e65397e292578142812f8a4f88ea8cc290a591fce8cf945c97cd9861937797cd814390ab8a698cc394c293498cc7995a81420d0a96b396408d8793be98d281420d0a","一個是性格圓滑的優等生，但偶爾露出輕率的態度，而且總\r\n感覺冷淡。另外一個是只靠能力和努力，性格古板的孤兒。\r\n無法合得來。\r\n"}
    };
    public static void main(String[] args)throws Exception{
        byte[] empty=new byte[0];
        GameTextDecoder plain=new GameTextDecoder(empty,null);
        check(plain.decode(sjis("通常の文字\\~あ")).equals("通常の文字\\~あ"),"ordinary CP932");
        check(java.util.Arrays.equals(plain.encodeInput("Ａあ半",6),sjis("Ａあ半")),"exact CP932 input bytes");
        check(GameTextDecoder.fullWidthName("a\\\"' `ｶﾞ").equals("ａ￥”’　‘カ゛"),"PC NAMEINPUT full-width key expansion");
        invalid(()->plain.encodeInput("あい",3));
        invalid(()->plain.encodeInput("A\n",3));
        invalid(()->plain.encodeInput("\u00a5",2));
        invalid(()->plain.encodeInput("\ud83d\ude00",4));
        GameTextDecoder onePass=new GameTextDecoder(empty,config("遠丝","丝X"));
        check(onePass.decode(sjis("遠A")).equals("丝A"),"one pass, no recursive substitution");
        check(onePass.decodeConfiguration(sjis("遠A")).equals("丝A"),"configuration shares one-pass UIF substitution");
        check(java.util.Arrays.equals(onePass.encodeInput("丝X",3),sjis("遠X")),"UIF input uses a reachable source or an unchanged character");
        GameTextDecoder ambiguous=new GameTextDecoder(empty,config("AB","中中"));
        invalid(()->ambiguous.encodeInput("中",2));
        GameTextDecoder supplementary=new GameTextDecoder(empty,config("A","\\ud83d\\ude00"));
        check(supplementary.decode(new byte[]{65}).equals("\ud83d\ude00"),"JSON escapes and code points");
        check(java.util.Arrays.equals(supplementary.encodeInput("\ud83d\ude00",1),new byte[]{65}),"invert an explicit supplementary UIF glyph");
        GameTextDecoder disabled=new GameTextDecoder(empty,utf8("{\"character_substitution\":{\"enable\":false,\"source_characters\":123},\"other\":[true,null,-1.2e+3]}"));
        check(disabled.decode(new byte[]{65}).equals("A"),"disabled rules are not applied");
        GameTextDecoder tunneled=new GameTextDecoder(new byte[]{(byte)0x80,0x7b},config("简","中"));
        check(tunneled.decode(new byte[]{(byte)0x81,1}).equals("中"),"substitution follows tunnel decoding");
        check(tunneled.decodeConfiguration(new byte[]{(byte)0x81,1}).equals("中"),"configuration substitutes after its tunnel decoding");
        check(java.util.Arrays.equals(tunneled.encodeInput("中",2),hex("8101")),"encode canonical tunnel before considering the display glyph");
        byte[] tunnelInput=new byte[2*59];
        for(int i=0;i<59;i++){int point=0x3400+i;tunnelInput[i*2]=(byte)point;tunnelInput[i*2+1]=(byte)(point>>8);}
        GameTextDecoder rows=new GameTextDecoder(tunnelInput,null);
        check(java.util.Arrays.equals(rows.encodeInput("\u3408\u3439\u343a",6),hex("810b813f8201")),
            "tunnel input skips reserved trails and carries at 58 entries");
        byte[] legacyMap=new byte[2*SjisTunnelDecoder.MAX_LEGACY_CONFIGURATION_MAPPINGS];
        for(int i=0;i<legacyMap.length/2;i++){int point=0x4e00+i;legacyMap[i*2]=(byte)point;legacyMap[i*2+1]=(byte)(point>>8);}
        SjisTunnelDecoder legacy=SjisTunnelDecoder.legacyConfiguration(legacyMap);
        check(legacy.decode(hex("812c")).equals("\u4e27"),"legacy configuration comma maps to fixed index39");
        check(legacy.decode(hex("8237")).equals("\u4e6d"),"legacy configuration second lead fixed index109");
        check(legacy.decode(hex("fb3f")).equals(Character.toString((char)(0x4e00+3480))),"legacy configuration maximum pair3480");
        invalid(()->legacy.decode(hex("fc01")));
        for(int low:new int[]{0,9,10,13,32})invalid(()->legacy.decode(new byte[]{(byte)0x81,(byte)low}));
        invalid(()->SjisTunnelDecoder.legacyConfiguration(new byte[(SjisTunnelDecoder.MAX_LEGACY_CONFIGURATION_MAPPINGS+1)*2]));
        invalid(()->new SjisTunnelDecoder(legacyMap));
        byte[] canonicalMap=java.util.Arrays.copyOf(legacyMap,SjisTunnelDecoder.MAX_MAPPINGS*2);
        SjisTunnelDecoder canonical=new SjisTunnelDecoder(canonicalMap);
        invalid(()->canonical.decode(hex("812c")));
        check(canonical.decode(hex("8237")).equals("\u4e6b"),"canonical58 constructor retains fixed index107");
        invalid(()->new GameTextDecoder(empty,config("AA","BC")));
        invalid(()->new GameTextDecoder(empty,config("A","BC")));
        invalid(()->new GameTextDecoder(empty,config("A","\\ud800")));
        invalid(()->new GameTextDecoder(empty,config("A","\\u0000")));
        invalid(()->new GameTextDecoder(empty,utf8("{\"character_substitution\":{\"enable\":1}}")));
        invalid(()->new GameTextDecoder(empty,utf8("{\"character_substitution\":{\"enable\":true,\"source_characters\":1,\"target_characters\":\"A\"}}")));
        invalid(()->new GameTextDecoder(empty,utf8("{\"x\":0,\"x\":1}")));
        invalid(()->new GameTextDecoder(empty,utf8("{\"x\":01}")));
        invalid(()->new GameTextDecoder(empty,utf8("{}garbage")));
        invalid(()->new GameTextDecoder(empty,new byte[]{(byte)0xff}));
        invalid(()->new GameTextDecoder(empty,new byte[1024*1024+1]));
        invalid(()->plain.decode(new byte[]{(byte)0x81}));
        invalid(()->plain.decode(new byte[SjisTunnelDecoder.MAX_INPUT_BYTES+1]));
        StringBuilder deep=new StringBuilder();for(int i=0;i<34;i++)deep.append('[');deep.append('0');for(int i=0;i<34;i++)deep.append(']');
        invalid(()->new GameTextDecoder(empty,utf8(deep.toString())));
        File temp=Files.createTempDirectory("systemnnn-text-").toFile();
        try{
            check(GameTextDecoder.load(temp).decode(new byte[]{65}).equals("A"),"missing files preserve fallback");
            Files.write(new File(temp,"uif_config.json").toPath(),config("A","B"));
            check(GameTextDecoder.load(temp).decode(new byte[]{65}).equals("B"),"file loader");
            invalid(()->GameTextDecoder.readBounded(new File(temp,"uif_config.json"),1));
        }finally{for(File f:temp.listFiles())f.delete();temp.delete();}
        if(args.length>0){
            File root=new File(args[0]);String json=new String(Files.readAllBytes(new File(root,"uif_config.json").toPath()),StandardCharsets.UTF_8);
            Matcher a=Pattern.compile("\"source_characters\"\\s*:\\s*\"([^\"]*)\"").matcher(json);
            Matcher b=Pattern.compile("\"target_characters\"\\s*:\\s*\"([^\"]*)\"").matcher(json);
            check(a.find()&&b.find(),"real explicit rule strings");
            String source=a.group(1),target=b.group(1);check(source.length()==1038&&target.length()==1038,"real 1038 entries");
            GameTextDecoder real=GameTextDecoder.load(root);
            for(int i=0;i<source.length();i++){
                byte[] raw=sjis(source.substring(i,i+1));String shown=target.substring(i,i+1);
                check(real.decode(raw).equals(shown)&&java.util.Arrays.equals(real.encodeInput(shown,2),raw),"real UIF mapping roundtrip "+i);
            }
        }
        for(int i=1;i<args.length;i++){
            File root=new File(args[i]);byte[] mapping=Files.readAllBytes(new File(root,"sjis_ext.bin").toPath());
            SjisTunnelDecoder old=new SjisTunnelDecoder(mapping);GameTextDecoder game=GameTextDecoder.load(root);
            for(byte[] input:new byte[][]{sjis("あいうABC"),new byte[]{(byte)0x81,1},new byte[]{(byte)0x81,2},new byte[]{(byte)0x80,(byte)0xa0,(byte)0xfd}})
                check(game.decode(input).equals(old.decode(input)),"existing game decode unchanged");
        }
        if(args.length==3)for(String[] fixed:STORIES){
            File game=new File(args[Integer.parseInt(fixed[0])]);GameTextDecoder codec=GameTextDecoder.load(game);
            byte[] original=story(game,Integer.parseInt(fixed[1]));
            check(java.util.Arrays.equals(original,hex(fixed[2])),"fixed original SPT bytes "+fixed[0]+"/"+fixed[1]);
            check(codec.decode(hex(fixed[2])).equals(fixed[3]),"fixed complete SPT meaning with canonical58 "+fixed[0]+"/"+fixed[1]);
            check(!codec.decodeConfiguration(hex(fixed[2])).equals(fixed[3]),"SPT must not use configuration59 "+fixed[0]+"/"+fixed[1]);
        }
        System.out.println("PASS "+checks+" text decoding checks");
    }
}
