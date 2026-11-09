"""Export the built-in Tyranor-Next integration from the signed ARM64 player APK."""

import argparse
import os
import re
import shutil
import subprocess
import tempfile
import zipfile
from pathlib import Path
from xml.etree import ElementTree

from build_player import (
    ANDROID, FFMPEG_SOURCE, MOVIE_CODEC_LIBS, OUTPUT, PACKAGE, PLAYER, ROOT, apk_filename, run,
)

HOST = ANDROID / "tyn"
PACKAGE_PATH = Path(*PACKAGE.split("."))
ABI = "arm64-v8a"
PLAYER_LIBRARY = "libsystemnnn.so"
PUBLIC_ROOTS = (".gitattributes", ".gitignore", "README.md", "THIRD_PARTY_NOTICES.md", "LICENSE*",
                "assets", "core", "platform")


def verify_apk(apk, application_id, version, code, build_tools, java):
    executable_suffix = ".exe" if os.name == "nt" else ""
    run([java, "-jar", build_tools / "lib/apksigner.jar", "verify", "--verbose", apk])
    badging = subprocess.check_output(
        [build_tools / f"aapt2{executable_suffix}", "dump", "badging", apk], encoding="utf-8"
    )
    package = f"package: name='{application_id}' versionCode='{code}' versionName='{version}'"
    if not any(line.startswith(package) for line in badging.splitlines()):
        raise RuntimeError(f"APK manifest does not match the player source: {apk}")


def export_apk(apk, engine, linkage):
    native_path = f"lib/{ABI}/"
    license_path = "assets/licenses/"
    native_dir = engine / "src/main/jniLibs" / ABI
    license_dir = engine / "src/main/assets/licenses/systemnnn"
    libraries = [PLAYER_LIBRARY]
    if linkage == "shared":
        libraries.extend(f"lib{name}.so" for name in MOVIE_CODEC_LIBS)
    native_dir.mkdir(parents=True)
    license_dir.mkdir(parents=True)
    with zipfile.ZipFile(apk) as packaged:
        names = [entry.filename for entry in packaged.infolist() if not entry.is_dir()]
        native_entries = [name for name in names if name.startswith("lib/")]
        if sorted(native_entries) != sorted(native_path + name for name in libraries):
            raise RuntimeError(f"Unexpected APK libraries for {linkage} FFmpeg linkage: {native_entries}")
        for name in libraries:
            library = packaged.read(native_path + name)
            if library[:6] != b"\x7fELF\x02\x01" or int.from_bytes(library[18:20], "little") != 183:
                raise RuntimeError(f"Expected a little-endian AArch64 ELF: {name}")
            (native_dir / name).write_bytes(library)
        recipe = (ANDROID / "build_movie_codec.sh").read_bytes()
        if packaged.read(license_path + "ffmpeg-build.sh") != recipe:
            raise RuntimeError("Current FFmpeg build script differs from the signed APK")
        for name in sorted(names):
            if name.startswith(license_path):
                filename = name.removeprefix(license_path)
                if Path(filename).name != filename or (license_dir / filename).exists():
                    raise RuntimeError(f"Unexpected APK license asset: {name}")
                (license_dir / filename).write_bytes(packaged.read(name))
        for filename in ("ffmpeg-lgpl-2.1.txt", "ffmpeg-source.txt", "libpng.txt", "zlib.txt",
                         "VNTranslationTools-MIT.txt", "stb_image.txt", "stb_vorbis.txt"):
            if not (license_dir / filename).is_file():
                raise RuntimeError(f"Missing APK license asset: {filename}")
    return native_dir / PLAYER_LIBRARY


def export_player(engine):
    java_dir = engine / "src/main/java" / PACKAGE_PATH
    java_dir.mkdir(parents=True)
    for source in sorted((PLAYER / "java").glob("*.java")):
        shutil.copyfile(source, java_dir / source.name)
    shutil.copyfile(ANDROID / "app/java" / PACKAGE_PATH / "SjisTunnelDecoder.java",
                    java_dir / "SjisTunnelDecoder.java")
    shutil.copyfile(HOST / "SystemNnnHost.java", java_dir / "SystemNnnHost.java")
    return java_dir


def export_public_source(directory):
    listed = subprocess.check_output(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *PUBLIC_ROOTS],
        cwd=ROOT,
    )
    # A previously tracked file can still be ignored by the current public tree rules.
    ignored = subprocess.run(["git", "check-ignore", "-z", "--no-index", "--stdin"], cwd=ROOT,
                             input=listed, stdout=subprocess.PIPE)
    if ignored.returncode not in (0, 1):
        raise RuntimeError("Cannot determine which files belong in the public source tree")
    paths = set(listed.split(b"\0")) - set(ignored.stdout.split(b"\0"))
    for raw in sorted(paths):
        path = Path(os.fsdecode(raw))
        source = ROOT / path
        if source.is_symlink() or not source.resolve().is_relative_to(ROOT) or not source.is_file():
            raise RuntimeError(f"Expected a public source file inside the checkout: {path}")
        destination = directory / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)


def check_java(java_dir, javac, android_jar, library, scratch):
    classes = scratch / "classes"
    headers = scratch / "jni-headers"
    classes.mkdir()
    headers.mkdir()
    run([javac, "-encoding", "UTF-8", "--release", "8", "-classpath", android_jar,
         "-d", classes, "-h", headers, *sorted(java_dir.glob("*.java"))])
    symbols = [symbol for header in headers.glob("*.h")
               for symbol in re.findall(rb"JNICALL (Java_[A-Za-z0-9_]+)", header.read_bytes())]
    if not symbols:
        raise RuntimeError("No JNI declarations found in the exported player")
    binary = library.read_bytes()
    for symbol in symbols:
        if symbol + b"\0" not in binary:
            raise RuntimeError(f"APK library is missing JNI symbol {symbol.decode('ascii')}")


def write_archive(directory, output):
    files = {path.relative_to(directory.parent).as_posix(): path
             for path in directory.rglob("*") if path.is_file()}
    entries = sorted(files)
    with zipfile.ZipFile(output, "w") as archive:
        for name in entries:
            info = zipfile.ZipInfo(name)
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_STORED if name.endswith(".tar.gz") else zipfile.ZIP_DEFLATED
            archive.writestr(info, files[name].read_bytes())
    with zipfile.ZipFile(output) as archive:
        if archive.namelist() != entries or archive.testzip() is not None:
            raise RuntimeError("The generated integration ZIP did not pass verification")


def publish(directory, archive, output):
    final_dir = output / directory.name
    final_zip = output / archive.name
    for target in (final_dir, final_zip):
        if target.is_symlink() or target.resolve() != target:
            raise RuntimeError(f"Cannot replace a redirected output: {target}")
    if final_dir.exists() and not final_dir.is_dir():
        raise RuntimeError(f"Expected an output directory: {final_dir}")
    if final_zip.exists() and not final_zip.is_file():
        raise RuntimeError(f"Expected a ZIP file: {final_zip}")
    previous_dir = directory.parent / "previous"
    if final_dir.exists():
        final_dir.replace(previous_dir)
    try:
        directory.replace(final_dir)
        archive.replace(final_zip)
    except OSError:
        if final_dir.exists():
            final_dir.replace(directory)
        if previous_dir.exists():
            previous_dir.replace(final_dir)
        raise
    print(final_dir)
    print(final_zip)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layout", choices=("split", "single"), default="split",
                        help="export the matching APK's split or single-library layout")
    args = parser.parse_args()
    linkage = "static" if args.layout == "single" else "shared"
    namespace = "{http://schemas.android.com/apk/res/android}"
    manifest = ElementTree.parse(PLAYER / "AndroidManifest.xml").getroot()
    version = manifest.attrib[namespace + "versionName"]
    code = manifest.attrib[namespace + "versionCode"]
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)+(?:-[A-Za-z0-9.-]+)?", version):
        raise RuntimeError(f"Unexpected player version: {version}")
    name = f"SystemNNN-Emberfall-{version}-{args.layout}-so-tyn-integration"
    output = OUTPUT.resolve()
    if not output.is_relative_to(ROOT) or not output.is_dir():
        raise RuntimeError(f"Expected the existing build output directory inside the checkout: {OUTPUT}")
    apk = output / apk_filename(version, linkage)
    ffmpeg = output / FFMPEG_SOURCE
    if not apk.is_file() or not ffmpeg.is_file():
        raise RuntimeError(f"Expected {apk.name} and {ffmpeg.name} in {output}")
    local = Path(os.getenv("LOCALAPPDATA", Path.home())) / "CodexTools/SystemNNN-M"
    sdk = Path(os.getenv("ANDROID_SDK_ROOT") or local / "android-sdk")
    java_home = Path(os.getenv("JAVA_HOME") or Path.home() / ".gradle/jdks/eclipse_adoptium-17-amd64-windows.2")
    suffix = ".exe" if os.name == "nt" else ""
    verify_apk(apk, manifest.attrib["package"], version, code,
               sdk / "build-tools/36.0.0", java_home / "bin" / f"java{suffix}")
    with tempfile.TemporaryDirectory(prefix="package-tyn-", dir=output) as temp:
        scratch = Path(temp)
        directory = scratch / name
        engine = directory / "overlay/engine"
        library = export_apk(apk, engine, linkage)
        java_dir = export_player(engine)
        shutil.copyfile(HOST / "README.md", directory / "README.md")
        shutil.copytree(HOST / "integration", directory / "integration")
        codec_dir = directory / "third_party/ffmpeg"
        codec_dir.mkdir(parents=True)
        shutil.copyfile(ffmpeg, codec_dir / ffmpeg.name)
        shutil.copyfile(ANDROID / "build_movie_codec.sh", codec_dir / "build_movie_codec.sh")
        export_public_source(directory / "source")
        check_java(java_dir, java_home / "bin" / f"javac{suffix}", sdk / "platforms/android-36/android.jar",
                   library, scratch)
        archive = scratch / f"{directory.name}.zip"
        write_archive(directory, archive)
        publish(directory, archive, output)


if __name__ == "__main__":
    main()
