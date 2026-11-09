package com.tyranor.runtime.systemnnn;

import android.content.Context;
import android.content.Intent;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.util.Map;
import java.util.Set;

/** Host entry point for the built-in Tyranor-Next engine. */
public final class SystemNnnHost {
    private static final String ACTION_LAUNCH = "com.tyranor.runtime.action.LAUNCH";
    private static final String EXTRA = "com.tyranor.runtime.extra.";

    private SystemNnnHost() {}

    /** Checks the staged snapshot without promoting any legacy backup files. */
    public static boolean canReplaceSaveSnapshot(Map<String,File> incoming,Set<String> existing) throws IOException {
        boolean hasStory=false;
        for(Map.Entry<String,File> entry:incoming.entrySet()){
            String name=entry.getKey();
            Integer slot=storySaveSlot(name);
            // AtomicFile reads the legacy backup first when both files exist.
            if(slot==null||incoming.containsKey(name+".bak"))continue;
            File file=entry.getValue();
            try(InputStream in=Files.newInputStream(file.toPath(),LinkOption.NOFOLLOW_LINKS)){
                if(GameSave.readFrom(in,file.lastModified(),slot,false).metadata.slot!=slot)return false;
            }
            hasStory=true;
        }
        String progress=GameSavePaths.PREFIX+GameSavePaths.READ;
        File primary=incoming.get(progress),backup=incoming.get(progress+".bak");
        boolean hasProgress=primary!=null||backup!=null;
        if(hasProgress&&!GameProgress.hasReadableBase(primary,backup))return false;
        if(hasStory)return true;
        String chart=GameSavePaths.PREFIX+GameSavePaths.CHART+"/";
        return hasProgress&&existing.stream().noneMatch(name->storySaveSlot(name)!=null||name.startsWith(chart));
    }

    private static Integer storySaveSlot(String name){
        String prefix=GameSavePaths.PREFIX;
        return name.startsWith(prefix)?SaveSlots.committedSlot(name.substring(prefix.length())):null;
    }

    /** Checks for a readable spt/sys.spt marker. The player validates the script when opened. */
    public static boolean canOpen(File root) {
        try {
            return hasMarker(canonicalRoot(root));
        } catch (IllegalArgumentException | IOException | SecurityException e) {
            return false;
        }
    }

    /** Builds an explicit in-app intent using an absolute filesystem path. */
    public static Intent createLaunchIntent(Context context, File root, String title) throws IOException {
        File game = canonicalRoot(root);
        if (!hasMarker(game)) {
            throw new IOException("Cannot read spt/sys.spt in " + game);
        }
        return new Intent(context, PlayerActivity.class)
            .setAction(ACTION_LAUNCH)
            .putExtra(EXTRA + "FOLDER_PATH", game.getPath())
            .putExtra(EXTRA + "GAME_TITLE", title)
            .putExtra(EXTRA + "PROTOCOL_VERSION", 1);
    }

    /** Returns the shared sav directory without creating it. */
    public static File saveDirectory(Context context, File root) throws IOException {
        return GameSavePaths.forGame(canonicalRoot(root)).directory();
    }

    private static File canonicalRoot(File root) throws IOException {
        if (root == null || !root.isAbsolute()) {
            throw new IllegalArgumentException("SystemNNN requires an absolute filesystem path");
        }
        return root.getCanonicalFile();
    }

    private static boolean hasMarker(File game) {
        File marker = GameResourcePaths.file(game, "spt", "sys.spt");
        return marker != null && marker.canRead();
    }
}
