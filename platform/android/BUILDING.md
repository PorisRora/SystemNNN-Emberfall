# 构建与 TYN 接入

以下命令均在仓库根目录执行。

## Android APK


在 Windows 的仓库根目录构建 Android APK。需要 Python 3.9 或以上、JDK 17、Android SDK Platform 36、Build Tools 36.0.0 和 `curl.exe`。WSL 中需要 CMake、GNU Make 和 Linux 版 Android NDK r28。按安装位置填写以下路径。

```powershell
$env:JAVA_HOME = 'C:\Java\jdk-17'
$env:ANDROID_SDK_ROOT = 'C:\Android\Sdk'
$env:SYSTEMNNN_NDK = '/opt/android-ndk-r28'
python platform/android/build_player.py
```

脚本默认使用 Ubuntu WSL 发行版，可通过 `SYSTEMNNN_WSL_DISTRO` 指定其他发行版。首次构建会下载 FFmpeg 源码并交叉编译。默认输出为 `build/tyranor/SystemNNN-Emberfall-0.6.6-arm64.apk`。加上 `--movie-codec-linkage static` 可在本地生成供提取单个 `.so` 使用的 `build/tyranor/SystemNNN-Emberfall-0.6.6-single-arm64.apk`。构建脚本使用本地调试密钥，覆盖安装要求新旧 APK 证书一致。

在 Linux 或 WSL 中构建主机工具和测试。

```sh
cmake -S platform/scene -B build/scene-host-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/scene-host-debug --parallel 4
(cd build/scene-host-debug && ctest --output-on-failure)
```

单独构建核心库与探针的命令和公开头文件见 [核心说明](../../core/README.md)。

## 从源码接入 TYN

维护者可按[接入说明](tyn/README.md)将宿主与运行库合入 TYN 的 `:engine`，再合并游戏识别、启动和存档入口。接入包保留原 Java 包名与 JNI 约定。更新后的独立 APK 打开原游戏目录时，会将本应用的旧私有存档复制到游戏的 `sav`，供有该目录读写权限的内置版使用；TYN 本身不能直接读取另一个应用的私有目录。

```powershell
python platform/android/package_tyn.py --layout split
python platform/android/package_tyn.py --layout single
```

先构建对应布局的 APK，再执行本地导出。输出包含宿主源码、配置合并片段、引擎构建源码及 FFmpeg 源码。
