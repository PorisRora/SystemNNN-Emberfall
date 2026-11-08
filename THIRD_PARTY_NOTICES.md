# 第三方声明

## SystemNNN

原版引擎由 tinyan 开发。Emberfall 的脚本兼容行为和 UI 参数参考了原版，核心来源说明见 [移植记录](core/PORTING.md)。上游仓库未列明再分发许可证。公开发布前需确认原版衍生内容的授权，并由维护者确定 Emberfall 自有代码的许可证。 [SystemNNN 上游仓库](https://github.com/tinyan/SystemNNN) · [GitHub 许可说明](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository)

## 随构建分发的组件

| 组件 | 许可与位置 |
| --- | --- |
| zlib | [zlib 许可证](core/third_party/zlib/LICENSE) |
| libpng | [libpng 许可证](core/third_party/libpng/LICENSE) |
| stb_vorbis | 构建包附带 MIT 许可文本。 [stb_vorbis 源码及许可](core/third_party/stb/stb_vorbis.c) |
| stb_image | 仅编译 JPEG 解码。MIT。 [stb_image 源码及许可](core/third_party/stb/stb_image.h) |
| VNTranslationTools 字符隧道 | MIT。 [VNTranslationTools 许可](platform/android/app/assets/licenses/VNTranslationTools-MIT.txt) |
| FFmpeg 8.1.3 | LGPL 2.1 或更高版本。默认 APK 和分离库接入包动态链接五个 FFmpeg 共享库。单库接入包将 FFmpeg 静态链接到 `libsystemnnn.so`。 [FFmpeg 源码](https://github.com/FFmpeg/FFmpeg/tree/n8.1.3) · [构建选项](platform/android/build_movie_codec.sh) |

APK 内附 FFmpeg 许可、对应源码地址和构建脚本。可从 FFmpeg 官方仓库下载构建所用的未修改源码，按仓库内的构建说明复现。单库接入说明提供使用修改版 FFmpeg 重新链接的步骤。 [FFmpeg n8.1.3 源码下载](https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n8.1.3) · [Android 构建说明](platform/android/BUILDING.md) · [单库重链接说明](platform/android/tyn/README.md#用修改过的-ffmpeg-重新链接单库) · [FFmpeg 分发清单](https://ffmpeg.org/legal.html)
