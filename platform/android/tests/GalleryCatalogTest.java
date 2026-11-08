package com.tyranor.runtime.systemnnn;

import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.List;

/** The original CG token grammar and logical sequence require no bitmap or JNI. */
public final class GalleryCatalogTest {
    private static int checks;
    private static void check(boolean value,String message){
        checks++;
        if(!value)throw new AssertionError(message);
    }
    public static void main(String[] args)throws Exception{
        List<String> fields=GalleryCatalog.cgTokens("#42-4/cg_anim,*@!#46,3/[<2:90_5|3:75_8]cg_diff\r\nother");
        check(fields.equals(Arrays.asList("#42-4/cg_anim","*@!#46,3/[<2:90_5|3:75_8]cg_diff","other")),
            "ordinary delimiters and an unambiguous animation comma");
        GalleryCatalog.CgVariant cg=GalleryCatalog.cgVariant(fields.get(1),1);
        check(cg.name.equals("cg_diff")&&cg.gore&&cg.difference&&cg.start==46&&cg.frames==3&&cg.unlockIndex==1,
            "animation metadata belongs to the original difference row");
        check(cg.unlocked(new int[]{0x10000001},0)&&!cg.unlocked(new int[]{0x10000000},0),
            "three animation frames use one original unlock key");
        check(cg.x.direction==0&&cg.x.tiles==2&&cg.x.time==90&&cg.x.wait==5&&
            cg.y.direction==1&&cg.y.tiles==3&&cg.y.time==75&&cg.y.wait==8,
            "directional tile counts, scroll duration and delay");
        check(cg.animationPicture(false,"_gore",2).equals("bg\\bganime48")&&
            cg.animationPicture(true,"_gore",2).equals("bg\\bganime_gore48"),
            "animation sprite name follows original gore suffix placement");
        check(cg.picture(true,"_gore",1).equals("cg/cg_diff_gore_2"),"bare original CG family and tile suffix");
        GalleryCatalog.CgVariant explicit=GalleryCatalog.cgVariant("ev\\folder\\ev018.DWQ",2);
        check(explicit.picture(false,"",1).equals("ev/folder/ev018_2"),"explicit CG directory is preserved");
        String trailBackslash=new String(new byte[]{(byte)0x83,0x5c},StandardCharsets.ISO_8859_1);
        GalleryCatalog.CgVariant cp932=GalleryCatalog.cgVariant("ev"+trailBackslash,3);
        check(cp932.picture(false,"",0).equals("ev/ev"+trailBackslash),"CP932 trail is not an explicit path separator");
        check(cg.frameAt(1,2,60)==0&&cg.frameAt(2,2,60)==1&&cg.frameAt(4,2,60)==2&&cg.frameAt(6,2,60)==0,
            "engine frame hold and looping");
        check(cg.frameAt(2,2,2)==2&&cg.frameAt(4,2,2)==2&&cg.frameAt(0,2,1)==2,
            "last allocation receives the last decoded animation frame");
        GalleryCatalog.CgVariant invalid=GalleryCatalog.cgVariant("#2147483647-2/picture",9);
        check(invalid.frames==0&&invalid.frameAt(0,1,60)==-1&&invalid.name.equals("picture"),
            "invalid frame ranges retain a static picture");
        String sjis=new String("@[↑]ev008d".getBytes(Charset.forName("windows-31j")),StandardCharsets.ISO_8859_1);
        GalleryCatalog.CgVariant marked=GalleryCatalog.cgVariant(sjis,15);
        check(marked.difference&&marked.y.direction==0&&marked.y.tiles==1&&marked.name.equals("ev008d"),
            "original CP932 arrow metadata");
        check(marked.picture(false,"",0).equals("ev/ev008d"),"original event CG family");
        for(String path:args){
            List<String> entries=GalleryCatalog.cgTokens(new String(Files.readAllBytes(Paths.get(path)),StandardCharsets.ISO_8859_1));
            int scrolls=0;
            for(int i=0;i<entries.size();i++){
                GalleryCatalog.CgVariant entry=GalleryCatalog.cgVariant(entries.get(i),i);
                check(entry.unlockIndex==i&&entry.frames==0&&!entry.name.isEmpty(),"installed gallery row "+i);
                if(entry.x.direction>=0||entry.y.direction>=0)scrolls++;
            }
            check(entries.size()==526&&scrolls==33,"original DeadEndAegis list and scroll marker count");
        }
        System.out.println("PASS "+checks+" gallery catalog checks");
    }
}
