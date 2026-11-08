# SystemNNN-Emberfall · Tyranor-Next 接入包

本包供 Tyranor-Next 维护者评估内置 SystemNNN 播放器。运行库从对应版本的已签名 Emberfall ARM64 APK 提取，播放器沿用原 Java 包名和 JNI 入口。运行需要 Android 8.0 及以上、ARM64，以及已解压到宿主可读路径的游戏目录。

## 包内文件

- `overlay/engine/` 放入 TYN 的 `:engine` 模块。两种接入包都含播放器 Java、`SystemNnnHost` 和许可资产。`split-so` 含 `libsystemnnn.so` 与五个 FFmpeg 共享库。`single-so` 将同一套 FFmpeg 功能静态链接到唯一的 `libsystemnnn.so`。宿主选用其中一套。
- `integration/` 包含 Manifest 片段、R8 规则、Gradle 与游戏库接入示例。按说明合并到宿主现有文件。[宿主接入示例](integration/host-hooks.md)
- `source/` 是 Emberfall 可公开的构建源码范围。包含核心、渲染、音频、JNI、独立播放器和构建脚本。
- `third_party/ffmpeg/` 提供 APK 对应的 FFmpeg 8.1.3 源码压缩包和构建脚本。[第三方许可与发布说明](source/THIRD_PARTY_NOTICES.md)

## 启动与存档

`SystemNnnHost.canOpen(File)` 以不区分 ASCII 大小写的 `spt/sys.spt` 作为目录识别标记。播放器启动后会校验脚本和资源。宿主先将游戏库目录解析成可访问的绝对文件系统路径，再用 `SystemNnnHost.createLaunchIntent(context, folder, title)` 构造同应用内的显式 Intent。Intent 沿用 `LAUNCH`、`FOLDER_PATH`、`GAME_TITLE` 和 `PROTOCOL_VERSION=1` 协议。标题文案仍由游戏自己的配置和贴图决定。

独立版和内置 TYN 都使用游戏目录下的 `sav`，兼容安装中已有的 `SAV` 等大小写。宿主可通过 `SystemNnnHost.saveDirectory(context, folder)` 获取该路径，播放器启动时会按需创建目录。Emberfall 的槽位、已读和鉴赏使用 `emberfall` 前缀，流程图节点放在 `emberfall.chart/`，原版的 `data1.sav`、`sysfile.*` 和 `chartN/` 仍由 PC 格式使用。移动整个游戏目录会带上这些 Emberfall 存档。宿主若提供备份入口，应筛选 Emberfall 数据，避免把原版存档或迁移暂存文件纳入。切换播放器前请先退出当前游戏。

首次用新版打开原游戏路径时，播放器会从当前应用的旧私有目录复制可迁移的存档及进度。旧档保留，`sav` 中已有的存档优先。旧独立 APK 与内置 TYN 各自只能访问自己的私有目录。要把旧独立版存档带到 TYN，先用新版独立 APK 在原路径打开一次，迁移完成后退出，再用 TYN 启动同一游戏目录。应用设置仍各自保存。保存面板长按「返回」继续提供 PC 存档导出。

## 重新导出与重建

在 Emberfall 的 Git 工作区保留与播放器 Manifest 版本对应的签名 APK 及 FFmpeg 源码包，使用独立播放器构建所用的 Android SDK 和 JDK 17。

```powershell
python platform/android/package_tyn.py
python platform/android/package_tyn.py --layout single
```

第一条命令从正式分离 APK 导出 `split-so`，第二条从 `*-single-arm64.apk` 导出 `single-so`。每套都在 `build/tyranor/` 生成同名的 `SystemNNN-Emberfall-<版本>-<布局>-so-tyn-integration` 目录及 ZIP。导出会验证 APK 的签名和版本、编译 Java 副本并检查 JNI 符号。

从随包源码重建时，将 `third_party/ffmpeg/ffmpeg-8.1.3-source.tar.gz` 复制到 `source/build/tyranor/`，再从 `source/` 运行 `python platform/android/build_player.py` 构建分离版。加上 `--movie-codec-linkage static` 构建单库版。[Android 构建说明](source/platform/android/README.md)

## 用修改过的 FFmpeg 重新链接单库

在接入包根目录用 Linux 或 WSL 运行以下命令。将 `SYSTEMNNN_NDK` 指向 NDK r28 的目录，并将 `ffmpeg_archive` 指向要使用的完整源码压缩包。保留源码归档的顶层目录结构。每次替换源码都使用新的构建目录。

```bash
ffmpeg_archive="$PWD/third_party/ffmpeg/ffmpeg-8.1.3-source.tar.gz"
relink_dir=$(mktemp -d)
bash third_party/ffmpeg/build_movie_codec.sh "$ffmpeg_archive" "$SYSTEMNNN_NDK" "$relink_dir/ffmpeg" static
cmake -S source/platform/android/player -B "$relink_dir/player" \
  -DCMAKE_TOOLCHAIN_FILE="$SYSTEMNNN_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release -DMOVIE_CODEC_ROOT="$relink_dir/ffmpeg" -DMOVIE_CODEC_LINKAGE=static
cmake --build "$relink_dir/player" --parallel 6 --target systemnnn
```

用 `$relink_dir/player/libsystemnnn.so` 替换单库接入目录中的库，再由宿主构建。FFmpeg 许可证随包附在 `overlay/engine/src/main/assets/licenses/systemnnn/ffmpeg-lgpl-2.1.txt`。原版 SystemNNN 的衍生内容授权仍需在公开发布前确认。[第三方声明](source/THIRD_PARTY_NOTICES.md)
