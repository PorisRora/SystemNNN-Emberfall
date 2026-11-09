package com.tyranor.runtime.systemnnn;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;

public final class DialogueMarkupTest {
    private static void check(boolean value,String reason){if(!value)throw new AssertionError(reason);}

    private static String message(Path game,String script,int number)throws Exception{
        byte[] source=Files.readAllBytes(game.resolve("spt").resolve(script));
        for(int i=0;i<source.length;i++)source[i]^=(byte)255;
        ByteBuffer words=ByteBuffer.wrap(source).order(ByteOrder.LITTLE_ENDIAN);
        int pointerTable=words.getInt(5*4)*4;
        int offset=words.getInt(pointerTable+number*4)*4,end=offset;
        while(end<source.length&&source[end]!=0)end++;
        return GameTextDecoder.load(game.toFile()).decode(Arrays.copyOfRange(source,offset,end));
    }

    public static void main(String[] args)throws Exception{
        String marked="漢字#［２かんじ］語#強・］。";
        DialogueMarkup parsed=DialogueMarkup.parse(marked,false,true);
        check(parsed.text.equals("漢字語。")&&parsed.glyphCount==4,"annotation metadata is outside timed text");
        check(parsed.annotations.size()==2,"both commands are recognized");
        DialogueMarkup.Annotation ruby=parsed.annotations.get(0),emphasis=parsed.annotations.get(1);
        check(ruby.start==0&&ruby.end==2&&ruby.label.equals("かんじ")&&!ruby.emphasis,"ruby targets the preceding two glyphs");
        check(emphasis.start==2&&emphasis.end==3&&emphasis.label.equals("・")&&emphasis.emphasis,"emphasis uses the supplied glyph");
        check(parsed.slice(1,4).annotations.get(0).label.equals("じ"),"line wrapping preserves the second ruby portion");

        DialogueMarkup disabled=DialogueMarkup.parse(marked,false,false);
        check(disabled.text.equals(parsed.text)&&disabled.annotations.isEmpty(),"rubiFlag off still consumes control codes");
        DialogueMarkup single=DialogueMarkup.parse("AB#[2ab]CD#.^*]",true,true);
        check(single.text.equals("ABCD")&&single.annotations.get(1).label.equals("^*"),"one-byte grammar retains distinct emphasis marks");
        check(single.slice(3,4).annotations.get(0).label.equals("*"),"each mark stays attached to its own character");
        DialogueMarkup supplementary=DialogueMarkup.parse("𠀀字#［2よみ］",false,true);
        check(supplementary.glyphCount==2&&supplementary.annotations.get(0).end==3,"visible UTF-16 offsets retain supplementary characters");
        DialogueMarkup crossed=DialogueMarkup.parse("甲\n乙#［２かな］",false,true);
        check(crossed.text.equals("甲\n乙")&&crossed.annotations.isEmpty(),"annotation cannot attach across a script newline");
        String literal="A # link #［xbroken #強no\nend］";
        check(DialogueMarkup.parse(literal,false,true).text.equals(literal),"malformed or unrecognized commands remain literal");
        check(DialogueMarkup.parse("A#[1a]",false,true).text.equals("A#[1a]"),"one-byte markers are gated by game codeByte");

        Map<String,String> fields=new HashMap<>();
        fields.put("rubiflag","1");fields.put("rubipercent","35");fields.put("rubideltay","-4");fields.put("rubisukima","2");fields.put("rubicolorg","199");
        DialogueMarkup.Style style=new DialogueMarkup.Style(fields);
        check(style.percent==35&&style.offsetY==-4&&style.spacing==2&&style.color==0xffffc7ff,"game annotation style is parsed");
        if(args.length==2){
            DialogueMarkup dea=style.parse(message(Paths.get(args[0]),"DEA_test.spt",597));
            check(dea.annotations.stream().anyMatch(a->!a.emphasis&&a.label.equals("フィールド")),"real DEA full-width ruby");
            DialogueMarkup gokuin=style.parse(message(Paths.get(args[1]),"mutuki.spt",1758));
            check(gokuin.annotations.size()==2&&gokuin.annotations.stream().allMatch(a->a.emphasis&&a.label.equals("・")),"real Gokuin emphasis");
            check(!dea.text.contains("#［")&&!gokuin.text.contains("#強"),"real control codes never reach the displayed body");
        }
        System.out.println("PASS dialogue markup"+(args.length==2?" (including two real SPT messages)":""));
    }
}
