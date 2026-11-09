package com.tyranor.runtime.systemnnn;

import java.io.*;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;

// Pure JVM codec/replay checks. Android AtomicFile itself is exercised by the player.
public final class GameSaveTest {
    private static int checks;
    private static final String SCRIPT="spt/dea_00_00.spt";
    private static final int[] POSITION={NativeGame.DIALOGUE,123,5,800,600,16,0};
    private static byte[][] content() { return new byte[][]{SCRIPT.getBytes(StandardCharsets.UTF_8),new byte[]{(byte)0x81,(byte)0xa0,65},new byte[]{66}}; }
    private static void check(boolean b,String detail) { if (!b) throw new AssertionError(detail); checks++; }
    interface Action { void run() throws Exception; }
    private static void invalid(Action action,String why) throws Exception {
        try { action.run(); throw new AssertionError("Accepted "+why); }
        catch (IOException expected) { checks++; }
    }
    private static GameSave.Loaded read(byte[] bytes) throws IOException {
        return GameSave.readFrom(new ByteArrayInputStream(bytes),99,0,true);
    }
    private static byte[] write(GameSave save,GameSave.Metadata metadata,byte[] snapshot) throws IOException {
        Path output=Files.createTempFile("systemnnn-save-codec-", ".sav");
        try {
            try(FileOutputStream file=new FileOutputStream(output.toFile())){
                GameSave.writePayload(file,new GameSave.Loaded(save,metadata,content(),snapshot));
            }
            return Files.readAllBytes(output);
        }finally{Files.deleteIfExists(output);}
    }
    private static byte[] legacy(int repeats,int ms,int booleanByte,int choice) throws IOException {
        return legacy(repeats,ms,booleanByte,choice,SCRIPT);
    }
    private static byte[] legacy(int repeats,int ms,int booleanByte,int choice,String script) throws IOException {
        ByteArrayOutputStream bytes=new ByteArrayOutputStream(); DataOutputStream out=new DataOutputStream(bytes);
        out.writeInt(GameSave.MAGIC_NS1); out.writeInt(1); out.writeInt(repeats); out.writeInt(ms); out.writeInt(12); out.writeInt(34);
        out.writeByte(booleanByte); out.writeInt(choice); out.writeUTF(script); out.writeInt(POSITION[0]); out.writeInt(POSITION[1]); out.writeInt(POSITION[2]);
        return bytes.toByteArray();
    }
    private static final class Engine implements GameSave.Replay {
        boolean skip=true;
        int ticks,sets,changedField=-1,keptBacklog;
        final List<Integer> systemSeeds=new ArrayList<>();
        final List<Integer> randomSeeds=new ArrayList<>(), resultSeeds=new ArrayList<>();
        final List<Integer> dates=new ArrayList<>(), clockMilliseconds=new ArrayList<>();
        int[] galleryKeys;
        byte[][] result=GameSaveTest.content();
        final List<Boolean> skipping=new ArrayList<>();
        final List<Integer> elapsed=new ArrayList<>();
        public void setSkip(boolean value) { skip=value; sets++; }
        public void tick(int ms,int x,int y,boolean advance,int choice,int[] state) {
            ticks++; skipping.add(skip); elapsed.add(ms); System.arraycopy(POSITION,0,state,0,POSITION.length);
            if (changedField>=0) state[changedField]++;
        }
        public byte[][] content() { return result; }
        public void trimBacklog(int keep) { keptBacklog=keep; }
        public void systemVariables(int[] values) { systemSeeds.add(values[5]); }
        public void scriptProgress(int[] values) { resultSeeds.add(values[7]); }
        public void gallery(int[] keys) { galleryKeys=keys; }
        public void randomState(int state) { randomSeeds.add(state); }
        public void clock(int date,int milliseconds) { dates.add(date);clockMilliseconds.add(milliseconds); }
    }
    private static void choiceAppearanceReplay() throws Exception {
        GameSave history=new GameSave();
        history.append(0,12,34,false,0);
        for(int i=0;i<3;i++)history.append(20,12,34,false,0);
        int[] position={NativeGame.CHOICE,123,5,800,600,20,980};
        GameSave.Metadata metadata=GameSave.metadata(0,1,SCRIPT,position,"","");
        GameSave.Loaded loaded=read(write(history,metadata,null));
        class ChoiceEngine implements GameSave.Replay {
            boolean entered;
            int appearing=2,remaining=1000;
            public void setSkip(boolean skip) {}
            public byte[][] content(){return GameSaveTest.content();}
            public void tick(int ms,int x,int y,boolean advance,int choice,int[] state){
                // The native selection first finishes its appearance, then starts counting down.
                if(!entered)entered=true;
                else if(appearing>0)appearing=Math.max(0,appearing-ms/20);
                else remaining=Math.max(0,remaining-ms);
                System.arraycopy(position,0,state,0,position.length);
                state[6]=remaining;
            }
        }
        ChoiceEngine engine=new ChoiceEngine();
        GameSave.replay(loaded,engine,new int[7]);
        check(engine.appearing==0&&engine.remaining==980,"choice replay preserves the appearance/countdown boundary");
    }
    private static void configuredSlots(String[] games) throws Exception {
        SaveSlots.Layout arrows=new SaveSlots.Layout(2,5,10,-1,true,1);
        check(arrows.pages==10&&arrows.perPage==10&&arrows.pageMode==1&&arrows.manualCount==99,"authored 10-page arrow grid");
        check(arrows.slotAt(0)==SaveSlots.QUICK&&arrows.slotAt(1)==0&&arrows.slotAt(99)==98,"quick slot leaves original NS2 numbering");
        check(arrows.openingPage(19)==2&&arrows.openingPage(130)==9&&!arrows.shows(130),"last selection opens its page and clamps after shrink");
        SaveSlots.Layout tabs=new SaveSlots.Layout(4,3,10,2,true,4);
        check(tabs.pageMode==2&&tabs.slotAt(3)==SaveSlots.QUICK&&tabs.slotAt(4)==3&&tabs.manualCount==119,
            "authored tabs and non-leading quick slot");
        SaveSlots.Layout single=new SaveSlots.Layout(2,2,1,-1,false,1);
        check(single.pageMode==0&&single.shows(3)&&!single.shows(4)&&!single.shows(SaveSlots.AUTO),"single page and supplemental saves");
        for(String name:games){
            File game=new File(name);
            GameTextDecoder decoder=GameTextDecoder.load(game);
            GameUiAssets.Config engine=GameUiAssets.readConfig(game,"game",decoder);
            GameUiAssets.Config data=GameUiAssets.readConfig(game,"dataFile",decoder);
            for(boolean saving:new boolean[]{false,true}){
                GameUiAssets.Config config=GameUiAssets.readConfig(game,saving?"save":"load",decoder);
                SaveSlots.Layout layout=new SaveSlots.Layout(config,engine,data,saving);
                int count=game.getName().contains("獄淫")||game.getName().equals("劣等")?120:100;
                check(layout.pages==10&&layout.pageMode==1&&layout.perPage*layout.pages==count,
                    "real "+game.getName()+" "+(saving?"save":"load")+" pages");
                check(layout.slotAt(count==120?3:0)==SaveSlots.QUICK&&layout.slotAt(count-1)==count-2,
                    "real "+game.getName()+" quick position and last slot");
            }
        }
    }
    private static void highSlotsAndLastSelection() throws Exception {
        Path game=Files.createTempDirectory("nnn-save-slots-");
        GameSavePaths paths=GameSavePaths.forGame(game.toFile());
        Files.createDirectory(paths.directory().toPath());
        SaveSlots slots=new SaveSlots(paths);
        int high=118;
        Path highFile=slots.file(high).toPath(),pointer=paths.file(SaveSlots.LAST_SELECTION).toPath();
        Path pointerBackup=paths.file(SaveSlots.LAST_SELECTION+".bak").toPath();
        Path slotUi=paths.file(SaveSlots.SLOT_UI).toPath();
        try {
            GameSave history=new GameSave();
            history.append(0,0,0,false,0);
            GameSave.Metadata metadata=GameSave.metadata(high,1,SCRIPT,POSITION,"","");
            byte[] encoded=write(history,metadata,null);
            check(GameSave.readFrom(new ByteArrayInputStream(encoded),99,high,true).metadata.slot==high,"high slot keeps NS2 format");
            Files.write(highFile,encoded);
            check(SaveSlots.slotForFile(highFile.toFile())==high&&
                SaveSlots.committedSlot(".slot118.sav.bak")==high&&SaveSlots.slotForSuffix(".slot0118.sav")==null,
                "canonical high slot names, including committed backup");
            slots.preload(4);
            check(slots.entries().length==7&&slots.entry(high).exists,"out-of-grid save stays listed after shrinking layout");
            slots.saved(metadata,null);
            slots.recordWritten(metadata,9,1);
            slots.remember(high);
            GameSave.Metadata auto=GameSave.metadata(SaveSlots.AUTO,2,SCRIPT,POSITION,"","");
            slots.saved(auto,null);
            slots.recordWritten(auto,99,1);
            check(slots.lastSelectedSlot()==high,"autosave does not replace explicit last selection");
            check(slots.entry(high).lastSaved&&slots.entry(high).artwork==9&&slots.entry(SaveSlots.AUTO).artwork==null,
                "only manually committed slots acquire display metadata");

            GameSave.Metadata quick=GameSave.metadata(SaveSlots.QUICK,3,SCRIPT,POSITION,"","");
            slots.saved(quick,null);
            slots.recordWritten(quick,15,0);
            slots.remember(SaveSlots.QUICK);
            check(slots.lastSelectedSlot()==SaveSlots.QUICK,"quick save is a selectable last save");
            check(slots.entry(high).lastSaved&&!slots.entry(SaveSlots.QUICK).lastSaved,
                "quickSetLastFlag zero retains the last save mark");
            slots.recordWritten(quick,15,1);
            slots.remember(high);
            check(slots.entry(SaveSlots.QUICK).lastSaved&&slots.entry(SaveSlots.QUICK).artwork==15&&!slots.entry(high).lastSaved,
                "quick save replaces the mark and later loads leave it alone");
            slots.recordWritten(quick,15,-1);
            check(!slots.entry(SaveSlots.QUICK).lastSaved&&!slots.entry(high).lastSaved,
                "quickSetLastFlag minus one clears the last save mark");
            slots.recordWritten(metadata,9,1);
            SaveSlots reopened=new SaveSlots(paths);
            reopened.preload(4);
            // The Android AtomicFile stub on the JVM cannot read a live save. Register the validated NS2 payload.
            reopened.saved(metadata,null);
            check(reopened.lastSelectedSlot()==high,"last selection survives reopen");
            check(reopened.entry(high).lastSaved&&reopened.entry(high).artwork==9,"slot presentation survives reopen");
            reopened.saved(GameSave.metadata(high,4,SCRIPT,POSITION,"",""),null);
            check(!reopened.entry(high).lastSaved&&reopened.entry(high).artwork==null,
                "stale presentation cannot attach to an overwritten slot");
            Files.copy(pointer,pointerBackup);
            slots.remember(SaveSlots.QUICK);
            reopened.preload(4);
            reopened.saved(quick,null);
            reopened.saved(metadata,null);
            check(reopened.lastSelectedSlot()==SaveSlots.QUICK,"valid primary selection takes precedence over backup");
            Files.delete(pointer);
            reopened.preload(4);
            reopened.saved(metadata,null);
            check(reopened.lastSelectedSlot()==high,"backup selection survives a missing primary");
            Files.write(pointer,new byte[]{0,0,0,0});
            reopened.preload(4);
            reopened.saved(metadata,null);
            check(reopened.lastSelectedSlot()==high,"backup selection survives an invalid primary");
            Files.write(pointerBackup,new byte[]{0,0,0,0});
            Files.write(slotUi,new byte[]{0,0,0,0});
            reopened.preload(4);
            reopened.saved(metadata,null);
            check(reopened.lastSelectedSlot()==null,"invalid selection cannot block or enable title LastLoad");
            check(reopened.entry(high).exists&&!reopened.entry(high).lastSaved&&reopened.entry(high).artwork==null,
                "corrupt presentation index leaves the underlying slot readable");
        }finally{
            Files.deleteIfExists(highFile);
            Files.deleteIfExists(pointer);
            Files.deleteIfExists(pointerBackup);
            Files.deleteIfExists(paths.file(SaveSlots.LAST_SELECTION+".new").toPath());
            Files.deleteIfExists(slotUi);
            Files.deleteIfExists(paths.file(SaveSlots.SLOT_UI+".new").toPath());
            Files.delete(paths.directory().toPath());
            Files.delete(game);
        }
    }
    public static void main(String[] games) throws Exception {
        choiceAppearanceReplay();
        GameSave history=new GameSave();
        history.append(0,12,34,false,0);
        history.append(16,12,34,false,0,true); history.append(16,12,34,false,0,true);
        history.append(0,12,34,true,0,false);
        check(history.runs.size()==3 && history.ticks==4 && history.runs.get(1).repeats==2,"skip-sensitive RLE");
        StringBuilder longBody=new StringBuilder(); for (int i=0;i<45;i++) longBody.append("😀");
        GameSave.Metadata metadata=GameSave.metadata(7,123456,SCRIPT,POSITION,"  爱丽丝\n ",longBody.toString());
        check(metadata.summary.codePointCount(0,metadata.summary.length())==40 && metadata.summary.endsWith("…"),"40-codepoint summary");
        check(metadata.speaker.equals("爱丽丝"),"one-line speaker");
        GameSave.checkSlot(new File("progress.slot07.sav"),metadata);
        invalid(() -> GameSave.checkSlot(new File("progress.slot08.sav"),metadata),"renamed slot metadata");
        ByteArrayOutputStream output=new ByteArrayOutputStream(); history.writeTo(output,metadata,content()); byte[] encoded=output.toByteArray();
        GameSave.Loaded loaded=read(encoded);
        check(loaded.metadata.slot==7 && loaded.metadata.savedAt==123456 && loaded.metadata.line==5 && !loaded.metadata.legacy,"metadata roundtrip");
        check(loaded.metadata.summary.equals(metadata.summary) && Arrays.deepEquals(loaded.content,content()),"same-file text snapshot");
        check(loaded.save.ticks==4 && loaded.save.runs.get(1).skip && !loaded.save.runs.get(2).skip,"skip roundtrip");
        GameSave.Loaded inspected=GameSave.readFrom(new ByteArrayInputStream(encoded),0,0,false);
        check(inspected.save==null && inspected.content==null && inspected.metadata.slot==7,"metadata scan without replay allocations");
        ByteArrayInputStream headerBytes=new ByteArrayInputStream(encoded);
        DataInputStream header=new DataInputStream(headerBytes);
        header.readInt();header.readInt();header.readLong();header.readUTF();
        header.readInt();header.readInt();header.readInt();header.readUTF();header.readUTF();
        byte[] metadataOnly=Arrays.copyOf(encoded,encoded.length-headerBytes.available());
        check(GameSave.readMetadata(new ByteArrayInputStream(metadataOnly),0,7).line==5,"slot listing reads only metadata");
        Engine engine=new Engine(); GameSave.replay(loaded,engine,new int[7]);
        check(engine.ticks==4 && engine.sets==4 && engine.skipping.equals(Arrays.asList(false,true,true,false)),"setSkip before every replay tick");
        check(engine.elapsed.equals(Arrays.asList(0,16,16,0)),"skip ticks never batched");
        for (int field=0;field<3;field++) { final Engine wrong=new Engine(); wrong.changedField=field; invalid(() -> GameSave.replay(loaded,wrong,new int[7]),"endpoint coordinate "+field); }
        for (int field=0;field<3;field++) { final Engine wrong=new Engine(); wrong.result[field][0]^=1; invalid(() -> GameSave.replay(loaded,wrong,new int[7]),"script/dialogue/choice bytes "+field); }
        GameSave.Loaded old=read(legacy(3,16,0,0));
        check(old.metadata.legacy && old.metadata.savedAt==99 && !old.save.runs.get(0).skip && old.save.ticks==3,"NS1 compatibility");
        check(GameSave.readMetadata(new ByteArrayInputStream(legacy(3,16,0,0)),99,0).legacy,"legacy listing skips input records");
        Engine oldEngine=new Engine(); int[] parked=POSITION.clone(); GameSave.replay(old,oldEngine,parked);
        check(oldEngine.sets==1 && oldEngine.skipping.equals(Arrays.asList(false)) && oldEngine.elapsed.equals(Arrays.asList(48)),"legacy non-skip replay batching");
        String japanese="spt/\u30bd_Sub.spt";
        byte[][] japaneseContent=content();
        japaneseContent[0]=japanese.getBytes(Charset.forName("windows-31j"));
        check(japaneseContent[0][5]==0x5c && GameResourcePaths.displayScript(japaneseContent[0]).equals(japanese),
            "CP932 script trail byte is part of its character");
        String oldSummary=new String(japaneseContent[0],StandardCharsets.UTF_8);
        for(String summary:Arrays.asList(japanese,oldSummary)){
            ByteArrayOutputStream out=new ByteArrayOutputStream();
            history.writeTo(out,GameSave.metadata(7,123456,summary,POSITION,"",""),japaneseContent);
            GameSave.Loaded named=read(out.toByteArray());
            Engine same=new Engine();same.result=japaneseContent;
            GameSave.replay(named,same,new int[7]);
            check(named.metadata.script.equals(summary)&&Arrays.deepEquals(named.content,japaneseContent),
                "new and legacy NS2 summaries preserve raw CP932 identity");
            Engine different=new Engine();different.result=japaneseContent.clone();
            different.result[0]=japaneseContent[0].clone();different.result[0][4]=(byte)0x84;
            invalid(()->GameSave.replay(named,different,new int[7]),"different CP932 identity with same old UTF-8 replacement");
        }
        Engine legacyNamed=new Engine();legacyNamed.result=japaneseContent;
        GameSave.replay(read(legacy(1,16,0,0,oldSummary)),legacyNamed,new int[7]);
        check(legacyNamed.ticks==1,"NS1 CP932 script replays with its old summary");
        old.save.append(16,12,34,false,0,true); check(old.save.runs.size()==2,"old save continues with explicit skip run");
        for (int length=0;length<encoded.length;length++) { final byte[] cut=Arrays.copyOf(encoded,length); invalid(() -> read(cut),"truncated NS2"); }
        byte[] changed=encoded.clone(); changed[changed.length/2]^=1; invalid(() -> read(changed),"payload corruption");
        byte[] footer=encoded.clone(); footer[footer.length-1]^=1; invalid(() -> read(footer),"checksum corruption");
        invalid(() -> read(Arrays.copyOf(encoded,encoded.length+1)),"trailing byte");
        invalid(() -> read(legacy(0,16,0,0)),"zero repeats");
        invalid(() -> read(legacy(24000001,16,0,0)),"tick overflow");
        invalid(() -> read(legacy(1,-1,0,0)),"negative time");
        invalid(() -> read(legacy(1,1001,0,0)),"large time");
        invalid(() -> read(legacy(1,16,2,0)),"noncanonical bool");
        invalid(() -> read(legacy(1,16,0,-1)),"negative choice");
        GameSave timeout=new GameSave();
        timeout.append(20,0,0,false,GameSave.TIMED_OUT);
        ByteArrayOutputStream timeoutBytes=new ByteArrayOutputStream();
        timeout.writeTo(timeoutBytes,metadata,content());
        check(read(timeoutBytes.toByteArray()).save.runs.get(0).choice==GameSave.TIMED_OUT,"timed choice journal roundtrip");
        GameSave rewound=history.rewind(2,1,12,34);
        check(rewound.cursor()==3 && rewound.lastInput().repeats==1 && history.ticks==4,"jump splits RLE and stages its trim");
        ByteArrayOutputStream jumpBytes=new ByteArrayOutputStream();
        rewound.writeTo(jumpBytes,metadata,content());
        Engine jumped=new Engine();
        GameSave.replay(read(jumpBytes.toByteArray()),jumped,new int[7]);
        check(jumped.elapsed.equals(Arrays.asList(0,16)) && jumped.keptBacklog==1,"trim is not VM input and future ticks were discarded");
        invalid(()->history.rewind(5,0,12,34),"jump past journal end");
        GameSave systemHistory=new GameSave();
        int[] vars=new int[NativeGame.SYSTEM_VARIABLES];vars[5]=7;
        systemHistory.recordSystemVariables(vars);
        systemHistory.append(0,12,34,false,0);
        vars[5]=9;systemHistory.recordSystemVariables(vars);
        systemHistory.append(16,12,34,false,0);
        ByteArrayOutputStream systemBytes=new ByteArrayOutputStream();
        systemHistory.writeTo(systemBytes,metadata,content());
        GameSave.Loaded systemSave=read(systemBytes.toByteArray());
        Engine resumed=new Engine();
        GameSave.replay(systemSave,resumed,new int[7]);
        check(systemSave.save.hasSystemBaseline()&&!old.save.hasSystemBaseline()&&
            resumed.systemSeeds.equals(Arrays.asList(7,9))&&resumed.elapsed.equals(Arrays.asList(0,16)),
            "system variables replay as a baseline and a resume boundary");
        GameSave calculations=new GameSave();
        int[] results=new int[NativeGame.SCRIPT_PROGRESS];results[7]=3;
        int[] unlocks={0x10000001,0x1000001f,0x20010001};
        calculations.recordReplayBaseline(vars,results,unlocks,-42);
        calculations.appendWithClock(0,12,34,false,0,false,20260308,12345);
        int checkpoint=calculations.cursor();
        calculations.append(16,12,34,false,0);calculations.append(16,12,34,false,0);
        calculations.recordMovieContinuationClock(20260309,23456);
        ByteArrayOutputStream calculationsBytes=new ByteArrayOutputStream();
        calculations.writeTo(calculationsBytes,metadata,content());
        Engine calculated=new Engine();
        GameSave.replay(read(calculationsBytes.toByteArray()),calculated,new int[7]);
        check(calculated.dates.equals(Arrays.asList(20260308,-1,20260309))&&
            calculated.clockMilliseconds.equals(Arrays.asList(12345,0,23456)),"clock samples survive a movie continuation in a repeated run");
        GameSave reset=calculations.rewind(checkpoint,1,12,34);
        results[7]=5;reset.recordReplayBaseline(vars,results,unlocks,77);
        reset.append(16,12,34,false,0);
        ByteArrayOutputStream resetBytes=new ByteArrayOutputStream();reset.writeTo(resetBytes,metadata,content());
        Engine resetReplay=new Engine();GameSave.replay(read(resetBytes.toByteArray()),resetReplay,new int[7]);
        check(resetReplay.dates.equals(Arrays.asList(20260308,-1))&&resetReplay.randomSeeds.equals(Arrays.asList(-42,77))&&
            resetReplay.resultSeeds.equals(Arrays.asList(3,5))&&Arrays.equals(resetReplay.galleryKeys,unlocks),
            "rewind removes future clock samples and applies the next baseline");
        byte[] snapshot={1,4,8,15,16,23,42};
        byte[] damaged=write(history,metadata,snapshot);damaged[damaged.length-1]^=1;
        GameSave.Loaded damagedFull=read(damaged);
        Engine recovered=new Engine();GameSave.replay(damagedFull,recovered,new int[7]);
        check(damagedFull.nativeSnapshot==null&&recovered.ticks==4,"a complete journal recovers a damaged native snapshot");

        GameSave longRun=new GameSave();
        for(int i=0;i<200000;i++)longRun.append(20+(i&1),640,360,false,0);
        int oldCursor=longRun.cursor();
        check(longRun.writable()&&!longRun.requiresSnapshot()&&oldCursor==200000,"complete journal at the run limit");
        check(longRun.rewind(oldCursor,1,640,360).requiresSnapshot(),"Backlog trim at the limit remains saveable");
        for(int i=0;i<200001;i++)longRun.append(20+(i&1),641,361,false,0);
        check(longRun.writable()&&longRun.requiresSnapshot()&&longRun.runs.size()==2&&longRun.cursor()>oldCursor,
            "repeated legal jitter switches to a bounded native-only journal");
        GameSave oldPoint=longRun.rewind(oldCursor,1,100,110);
        GameSave newPoint=longRun.rewind(longRun.cursor(),0,200,210);
        check(oldPoint.requiresSnapshot()&&oldPoint.lastInput().x==100&&oldPoint.lastInput().y==110&&
            newPoint.requiresSnapshot()&&newPoint.lastInput().x==200&&newPoint.lastInput().y==210,
            "native Backlog preserves both old and new checkpoint pointers across compaction");
        invalid(()->history.rewind(longRun.cursor(),0,12,34),"snapshot-only cursor used as a full journal offset");
        GameSave tickLimit=read(legacy(23999999,16,0,0)).save;
        tickLimit.appendWithClock(16,30,40,false,0,false,20260310,12345);
        check(tickLimit.requiresSnapshot()&&tickLimit.lastInput().x==30,"clock sample cannot leave an orphan at the tick limit");

        byte[] compact=write(longRun,metadata,snapshot);
        GameSave.Loaded compactSlot=read(compact);
        check(compact.length<1024&&Arrays.equals(compactSlot.nativeSnapshot,snapshot)&&compactSlot.save.requiresSnapshot()&&
            compactSlot.save.lastInput().x==641&&compactSlot.save.lastInput().y==361,"long run saves its native snapshot and pointer");
        byte[] multiBlockSnapshot=new byte[32768];
        new java.util.Random(7).nextBytes(multiBlockSnapshot);
        check(Arrays.equals(read(write(longRun,metadata,multiBlockSnapshot)).nativeSnapshot,multiBlockSnapshot),
            "reads a snapshot across compressed buffer boundaries");
        invalid(()->write(longRun,metadata,null),"snapshot-only slot without a native snapshot");
        compact[compact.length-1]^=1;
        GameSave.Loaded damagedCompact=read(compact);
        Engine noReplay=new Engine();
        invalid(()->GameSave.replay(damagedCompact,noReplay,new int[7]),"snapshot-only slot with a damaged native snapshot");
        check(damagedCompact.nativeSnapshot==null&&noReplay.ticks==0,"a partial journal must never run from the beginning");

        GameSave imported=GameSave.nativeBaseline();
        // Earlier PC-imported saves retained their post-import input suffix.
        imported.runs.add(new GameSave.Run(3,16,12,34,false,0));imported.ticks+=3;
        GameSave importedSlot=read(write(imported,metadata,snapshot)).save;
        check(importedSlot.requiresSnapshot()&&importedSlot.lastInput().x==12&&importedSlot.lastInput().y==34,
            "old PC-imported suffix keeps its pointer");
        for(int i=0;i<200001;i++)importedSlot.append(20+(i&1),13,35,false,0);
        check(importedSlot.writable()&&importedSlot.runs.size()==2&&importedSlot.lastInput().x==13,
            "PC baseline stays saveable past the same run limit");
        GameSave.Loaded reexported=read(write(importedSlot,metadata,snapshot));
        check(reexported.save.requiresSnapshot()&&reexported.save.runs.size()==2&&
            reexported.save.lastInput().x==13&&reexported.save.lastInput().y==35&&Arrays.equals(reexported.nativeSnapshot,snapshot),
            "old native-baseline suffix compacts, appends and saves again");
        invalid(() -> GameSave.metadata(SaveSlots.MAX_MANUAL_SLOTS,1,SCRIPT,POSITION,"",""),"slot outside manual range");
        invalid(() -> GameSave.metadata(0,-1,SCRIPT,POSITION,"",""),"negative timestamp");
        invalid(() -> GameSave.metadata(0,1,SCRIPT,POSITION,"x\0y",""),"NUL in speaker");
        SaveSlots slots=new SaveSlots(GameSavePaths.forGame(new File(".")));
        check(slots.file(0).getName().equals("emberfall.sav") && slots.file(SaveSlots.AUTO).getName().equals("emberfall.resume"),"portable filename identity");
        Set<String> names=new HashSet<>(); SaveSlots.Entry[] entries=slots.entries(); check(entries.length==22,"20+auto+quick");
        for (SaveSlots.Entry e:entries) { check(names.add(slots.file(e.slot).getName()),"unique slot file"); check(!e.exists && !e.corrupt,"empty slot"); check(SaveSlots.slotForFile(slots.file(e.slot))==e.slot,"slot name roundtrip"); }
        check(entries[0].label().equals("存档 01") && entries[19].label().equals("存档 20") && entries[20].label().equals("自动存档") && entries[21].label().equals("快速存档"),"slot labels");
        configuredSlots(games);
        highSlotsAndLastSelection();
        System.out.println("GameSave checks passed: "+checks);
    }
}
