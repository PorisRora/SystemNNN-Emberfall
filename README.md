<p align="center">
  <img src="assets/emberfall-logo.png" alt="SystemNNN-Emberfall" width="640">
</p>

# SystemNNN-Emberfall

面向 Android 的 SystemNNN 引擎移植，让手机运行使用 SystemNNN 制作的视觉小说。支持独立 APK，也可作为 Tyranor-Next 的内置引擎。

**移植作者：[PorisRora](https://github.com/PorisRora)（Rooboko）**

- C++ 内核负责剧情脚本、图层、特效和音频。
- 按游戏自身配置呈现标题、对话、菜单、存读档和鉴赏界面。
- 支持触屏操作、自动播放、已读快进、历史回看。剧情正文可选 60%、85%、100% 和 115%，默认 85%。
- 存档保存在游戏目录的 `sav` 中，支持读取及导出 PC 剧情存档。

## 下载与使用

从 [Releases](https://github.com/PorisRora/SystemNNN-Emberfall/releases/latest) 下载 APK。需要 **Android 8.0+、ARM64** 设备。

安装后选择包含 `spt/sys.spt` 的已解压游戏目录即可。游戏资源需自行提供，兼容程度因游戏版本而异。

[播放器说明](platform/android/README.md) · [构建说明](platform/android/BUILDING.md) · [TYN 接入说明](platform/android/tyn/README.md)

## 来源

原版 SystemNNN 由 tinyan 开发。Emberfall 自有代码采用 GPL-2.0-only，随包第三方组件按各自许可分发。 [原版 SystemNNN](https://github.com/tinyan/SystemNNN) · [GPL-2.0 许可证](LICENSE) · [第三方声明](THIRD_PARTY_NOTICES.md)
