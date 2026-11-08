package com.tyranor.runtime.systemnnn;

import android.content.Context;
import android.content.Intent;
import java.io.File;
import java.io.IOException;

/** Host entry point for the built-in Tyranor-Next engine. */
public final class SystemNnnHost {
    private static final String ACTION_LAUNCH = "com.tyranor.runtime.action.LAUNCH";
    private static final String EXTRA = "com.tyranor.runtime.extra.";

    private SystemNnnHost() {}

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
