"""Build the TYN runtime APK from this checkout with the installed Windows SDK and WSL NDK."""
import os
import argparse
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path
from xml.etree import ElementTree

ANDROID = Path(__file__).resolve().parent
ROOT = ANDROID.parents[1]
PLAYER = ANDROID / "player"
OUTPUT = ROOT / "build/tyranor"
PACKAGE = "com.tyranor.runtime.systemnnn"
MOVIE_CODEC_VERSION = "8.1.3"
MOVIE_CODEC_LIBS = ("avformat", "avcodec", "swscale", "swresample", "avutil")
FFMPEG_SOURCE = f"ffmpeg-{MOVIE_CODEC_VERSION}-source.tar.gz"

def apk_filename(version, linkage):
    suffix = "-single" if linkage == "static" else ""
    return f"SystemNNN-Emberfall-{version}{suffix}-arm64.apk"

def run(args, cwd=None):
    subprocess.run([str(arg) for arg in args], cwd=cwd, check=True)

def linux(path):
    return Path("/mnt") / path.drive[0].lower() / path.as_posix()[3:]

def prepare_movie_codec(wsl, ndk, linkage):
    archive = OUTPUT / FFMPEG_SOURCE
    if not archive.is_file():
        partial = archive.with_suffix(".partial")
        url = f"https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n{MOVIE_CODEC_VERSION}"
        run(["curl.exe", "-fL", "--retry", "2", "--connect-timeout", "15", "--max-time", "180",
             "-o", partial, url])
        partial.replace(archive)
    suffix = "-static" if linkage == "static" else ""
    codec = OUTPUT / "movie-codec-arm64" / (MOVIE_CODEC_VERSION + suffix)
    prefix = linux(codec).as_posix()
    run(wsl + ["bash", linux(ANDROID / "build_movie_codec.sh").as_posix(),
               linux(archive).as_posix(), ndk, prefix, linkage])
    return codec

def main():
    manifest = PLAYER / "AndroidManifest.xml"
    version = ElementTree.parse(manifest).getroot().attrib["{http://schemas.android.com/apk/res/android}versionName"]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--movie-codec-linkage", choices=("shared", "static"), default="shared",
                        help="link FFmpeg as separate shared libraries or embed it in libsystemnnn.so")
    parser.add_argument("--output", type=Path, help="override the versioned APK output path")
    args = parser.parse_args()
    linkage = args.movie_codec_linkage
    local = Path(os.environ["LOCALAPPDATA"]) / "CodexTools/SystemNNN-M"
    sdk = Path(os.getenv("ANDROID_SDK_ROOT") or local / "android-sdk")
    java_home = Path(os.getenv("JAVA_HOME") or Path.home() / ".gradle/jdks/eclipse_adoptium-17-amd64-windows.2")
    bt = sdk / "build-tools/36.0.0"
    jar = sdk / "platforms/android-36/android.jar"
    java = java_home / "bin/java.exe"
    native = OUTPUT / "native-arm64"
    OUTPUT.mkdir(parents=True, exist_ok=True)
    wsl = ["wsl", "-d", os.getenv("SYSTEMNNN_WSL_DISTRO", "Ubuntu"), "--"]
    ndk = os.getenv("SYSTEMNNN_NDK")
    if not ndk:
        distro_home = subprocess.check_output(wsl + ["printenv", "HOME"], encoding="utf-8").strip()
        ndk = distro_home + "/.cache/systemnnn-m/toolchains/android-ndk-r28"
    codec = prepare_movie_codec(wsl, ndk, linkage)
    run(wsl + ["cmake", "-S", linux(PLAYER).as_posix(), "-B", linux(native).as_posix(),
               "-DCMAKE_TOOLCHAIN_FILE=" + ndk + "/build/cmake/android.toolchain.cmake", "-DANDROID_ABI=arm64-v8a",
               "-DANDROID_PLATFORM=android-26", "-DANDROID_STL=c++_static", "-DCMAKE_BUILD_TYPE=Release",
               "-DMOVIE_CODEC_ROOT=" + linux(codec).as_posix(), "-DMOVIE_CODEC_LINKAGE=" + linkage])
    run(wsl + ["cmake", "--build", linux(native).as_posix(), "--parallel", "6", "--target", "systemnnn"])
    local.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="player-", dir=local) as tmp:
        stage = Path(tmp)
        packed_lib = native / "libsystemnnn-package.so"
        run(wsl + [ndk + "/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-objcopy", "--strip-unneeded",
                   linux(native / "libsystemnnn.so").as_posix(), linux(packed_lib).as_posix()])
        shutil.copytree(PLAYER / "res", stage / "res")
        shutil.copyfile(manifest, stage / "AndroidManifest.xml")
        licenses = stage / "assets/licenses"
        licenses.mkdir(parents=True)
        for source, name in ((ROOT / "core/third_party/zlib/LICENSE", "zlib.txt"),
                             (ROOT / "core/third_party/libpng/LICENSE", "libpng.txt"),
                             (codec / "source/COPYING.LGPLv2.1", "ffmpeg-lgpl-2.1.txt"),
                             (ANDROID / "app/assets/licenses/VNTranslationTools-MIT.txt", "VNTranslationTools-MIT.txt")):
            shutil.copyfile(source, licenses / name)
        linked = "statically linked into libsystemnnn.so" if linkage == "static" else "dynamically linked"
        relinking = ("Static relinking: https://github.com/PorisRora/SystemNNN-Emberfall/blob/main/platform/android/tyn/README.md\n"
                     if linkage == "static" else "")
        (licenses / "ffmpeg-source.txt").write_text(
            f"FFmpeg {MOVIE_CODEC_VERSION} (LGPL 2.1 or later) is {linked}.\n"
            f"Unmodified source: https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n{MOVIE_CODEC_VERSION}\n"
            "Build options: platform/android/build_movie_codec.sh (also packaged as ffmpeg-build.sh).\n"
            "Emberfall build instructions: https://github.com/PorisRora/SystemNNN-Emberfall/blob/main/platform/android/BUILDING.md\n"
            + relinking, encoding="utf-8")
        shutil.copyfile(ANDROID / "build_movie_codec.sh", licenses / "ffmpeg-build.sh")
        for source in ("stb_vorbis.c", "stb_image.h"):
            decoder = ROOT / "core/third_party/stb" / source
            terms = decoder.read_text(encoding="utf-8")
            (licenses / (decoder.stem + ".txt")).write_text(terms[terms.rindex("ALTERNATIVE A"):].removesuffix("*/\n"), encoding="utf-8")
        shutil.copytree(PLAYER / "java", stage / "java")
        shutil.copyfile(ANDROID / "app/java/com/tyranor/runtime/systemnnn/SjisTunnelDecoder.java", stage / "java/SjisTunnelDecoder.java")
        run([bt / "aapt2.exe", "compile", "--dir", "res", "-o", "resources.zip"], stage)
        run([bt / "aapt2.exe", "link", "-I", jar, "--manifest", "AndroidManifest.xml", "--auto-add-overlay", "-R", "resources.zip",
             "-A", "assets", "-o", "resources.apk"], stage)
        classes = stage / "classes"
        classes.mkdir()
        run([java_home / "bin/javac.exe", "-encoding", "UTF-8", "--release", "8", "-classpath", jar,
             "-d", classes, *sorted((stage / "java").rglob("*.java"))], stage)
        run([java, "-cp", bt / "lib/d8.jar", "com.android.tools.r8.D8", "--release", "--min-api", "26",
             "--lib", jar, "--output", "classes.zip", *sorted(classes.rglob("*.class"))], stage)
        with zipfile.ZipFile(stage / "resources.apk") as resources, zipfile.ZipFile(stage / "player.apk", "w") as apk, zipfile.ZipFile(stage / "classes.zip") as dex:
            for item in resources.infolist():
                apk.writestr(item.filename, resources.read(item), item.compress_type)
            apk.writestr("classes.dex", dex.read("classes.dex"), zipfile.ZIP_DEFLATED)
            apk.write(packed_lib, "lib/arm64-v8a/libsystemnnn.so", zipfile.ZIP_STORED)
            if linkage == "shared":
                for component in MOVIE_CODEC_LIBS:
                    lib = f"lib{component}.so"
                    apk.write(codec / "lib" / lib, "lib/arm64-v8a/" + lib, zipfile.ZIP_STORED)
        run([bt / "zipalign.exe", "-P", "16", "-f", "4", "player.apk", "aligned.apk"], stage)
        key = local / "debug-signing/emberfall-debug.p12"
        if not key.is_file():
            key.parent.mkdir(parents=True, exist_ok=True)
            run([java_home / "bin/keytool.exe", "-genkeypair", "-keystore", key, "-alias", "emberfall-debug",
                 "-storepass", "android", "-keypass", "android", "-dname", "CN=Emberfall Local Debug",
                 "-keyalg", "RSA", "-keysize", "2048", "-validity", "3650", "-noprompt"])
        signer = [java, "-jar", bt / "lib/apksigner.jar"]
        run(signer + ["sign", "--ks", key, "--ks-key-alias", "emberfall-debug", "--ks-pass", "pass:android",
                      "--key-pass", "pass:android", "--v1-signing-enabled", "false", "--out", "signed.apk", "aligned.apk"], stage)
        run(signer + ["verify", "--verbose", "signed.apk"], stage)
        run([bt / "zipalign.exe", "-c", "-P", "16", "4", "signed.apk"], stage)
        final = (args.output or OUTPUT / apk_filename(version, linkage)).resolve()
        final.parent.mkdir(parents=True, exist_ok=True)
        pending = final.with_suffix(".tmp")
        shutil.copyfile(stage / "signed.apk", pending)
        pending.replace(final)
        print(final)

if __name__ == "__main__":
    main()
