package com.tyranor.runtime.systemnnn;

import java.io.Closeable;
import java.io.File;
import java.io.IOException;
import java.nio.channels.FileChannel;
import java.nio.channels.FileLock;
import java.nio.channels.OverlappingFileLockException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;

/** Emberfall files live beside the original PC saves under their own names. */
final class GameSavePaths {
    static final String PREFIX="emberfall", READ=".read", GALLERY=".gallery", CHART=".chart", THUMB=".thumb";
    private static final Set<Path> OPEN=ConcurrentHashMap.newKeySet();
    private final File game, directory;

    private GameSavePaths(File game,File directory) {
        this.game=game;
        this.directory=directory;
    }

    static GameSavePaths forGame(File root) throws IOException {
        File game=root.getCanonicalFile();
        File directory=GameResourcePaths.directory(game,"sav");
        if(directory==null)directory=new File(game,"sav");
        if(!game.isDirectory() || Files.isSymbolicLink(directory.toPath()) ||
            directory.exists() && !directory.isDirectory())
            throw new IOException("游戏存档目录无效 · "+directory);
        return new GameSavePaths(game,directory);
    }

    /** AtomicFile.finishWrite logs rename errors instead of reporting them to the caller. */
    static void verifyCommitted(File file) throws IOException {
        String path=file.getPath();
        if(!file.isFile()||new File(path+".new").exists()||new File(path+".bak").exists())
            throw new IOException("Save commit did not finish: "+file.getName());
    }

    File directory(){return directory;}
    File file(String suffix){return new File(directory,PREFIX+suffix);}

    Access open(File legacyFiles) throws IOException {
        return open(legacyFiles,game);
    }

    Access open(File legacyFiles,File legacySource) throws IOException {
        Path dir=directory.toPath();
        Files.createDirectories(dir);
        if(!Files.isDirectory(dir,LinkOption.NOFOLLOW_LINKS))throw new IOException("游戏存档目录无效 · "+directory);
        Path lockFile=file(".lock").toPath();
        // Reserve the path before opening a second channel. Closing it could release this JVM's first lock.
        if(!OPEN.add(lockFile))throw inUse();
        FileChannel channel=null;
        try {
            channel=FileChannel.open(lockFile,StandardOpenOption.CREATE,StandardOpenOption.WRITE,LinkOption.NOFOLLOW_LINKS);
            FileLock lock;
            try{lock=channel.tryLock();}
            catch(OverlappingFileLockException e){lock=null;}
            if(lock==null)throw inUse();
            new LegacySaveMigration(dir).migrate(legacySource,legacyFiles);
            return new Access(lockFile,lock);
        } catch(IOException | RuntimeException e) {
            try{if(channel!=null)channel.close();}catch(IOException closeFailure){e.addSuppressed(closeFailure);}
            finally{OPEN.remove(lockFile);}
            throw e;
        }
    }

    private static IOException inUse(){return new IOException("这款游戏的存档正在使用，请退出另一个播放器后重试");}

    static final class Access implements Closeable {
        private final Path path;
        private final FileLock lock;
        private boolean closed;
        private Access(Path path,FileLock lock){this.path=path;this.lock=lock;}
        @Override public synchronized void close() throws IOException {
            if(closed)return;
            closed=true;
            try{lock.channel().close();}
            finally{OPEN.remove(path);}
        }
    }
}
