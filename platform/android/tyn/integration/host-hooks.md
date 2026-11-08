# Tyranor-Next 宿主合并示例

示例对应 Tyranor-Next `main` 的 `228cc343`。把 `overlay/engine/` 中的文件添加到现有 `engine/`。同路径文件或同名动态库有冲突时，先核对现有文件的来源和 ABI。[Tyranor-Next 源码](https://github.com/Weiss-UltimateSavior/Tyranor-Next/tree/228cc343b58af8cc3d9f46316641477f1178ac77)

## Manifest 与 Gradle

1. 将片段中的 `<activity>` 合并到 `engine/src/main/AndroidManifest.xml` 的 `<application>`。播放器由 TYN 内部显式启动，在 `:systemnnn` 进程运行。宿主需要可读的绝对游戏路径和可写的 `sav` 目录，SAF 目录须能映射到真实路径。Android 8–10 的公共存储目录需要读写权限，Android 10 还需要启用旧版外部存储模式。请核对宿主清单里的 `WRITE_EXTERNAL_STORAGE`，可合并片段中的权限声明。Android 11 及以上沿用宿主的所有文件访问授权。[Manifest 片段](AndroidManifest.systemnnn.xml)
2. 将 [R8 规则](consumer-systemnnn.pro) 放到 `engine/consumer-systemnnn.pro`。在现有 Groovy `engine/build.gradle` 的 `android.defaultConfig` 中追加该规则文件，保留既有规则。

   ```groovy
   consumerProguardFiles "consumer-rules.pro", "consumer-systemnnn.pro"
   ```

3. 保持 TYN 现有的 `jniLibs` 打包和 `:app` 对 `:engine` 的依赖。选用的库放在 `engine/src/main/jniLibs/arm64-v8a/`，入口统一为 `System.loadLibrary("systemnnn")`。分离版还由 ELF 依赖加载五个 FFmpeg 库。单库版已将 FFmpeg 链入 `libsystemnnn.so`，切换时应移出上一套 Emberfall 库。许可资产走 Android library 默认的 `engine/src/main/assets/`，与 app 的资产合并。宿主最终构建时检查对应布局的库和 `assets/licenses/systemnnn/`。

## 注册引擎与识别目录

在 `app/src/main/java/com/tyranor/next/core/engine/EngineType.kt` 的枚举中加入这一项，并为宿主针对 `EngineType` 的穷举分支补上相应的显示和设置入口。[EngineType.kt](https://github.com/Weiss-UltimateSavior/Tyranor-Next/blob/228cc343b58af8cc3d9f46316641477f1178ac77/app/src/main/java/com/tyranor/next/core/engine/EngineType.kt)

```kotlin
SYSTEMNNN("SystemNNN", "NNN"),
```

在 `core/game/scan/EngineScanner.kt` 共用的泛型 `detectEngine<T>()` 内加入目录特征。以下位置在通用 `if (hasIndex)` 网页回退前。更早判定的明确引擎类型保持原优先级，识别成功的 SystemNNN 目录走内置引擎路径。真实路径和 SAF 扫描都调用这个检测入口。[EngineScanner.kt](https://github.com/Weiss-UltimateSavior/Tyranor-Next/blob/228cc343b58af8cc3d9f46316641477f1178ac77/app/src/main/java/com/tyranor/next/core/game/scan/EngineScanner.kt)

```kotlin
val hasSystemNnnEntry = children.any { directory ->
    isDirectory(directory) && nameOf(directory).equals("spt", ignoreCase = true) &&
        childrenOf(directory).any { file ->
            !isDirectory(file) && nameOf(file).equals("sys.spt", ignoreCase = true)
        }
}
if (hasSystemNnnEntry) {
    return Detection(EngineType.SYSTEMNNN, 96, LAUNCH_TARGET_GAME_DIR)
}
```

## 启动

在 `core/game/launch/EngineLauncher.kt` 导入 `com.tyranor.runtime.systemnnn.SystemNnnHost`，将 `SYSTEMNNN` 加入 `supportedEngines`，并在 `buildIntent()` 的 `when (engine)` 中加入以下分支。沿用已有的目录解析、文件访问检查、`FLAG_ACTIVITY_NEW_TASK` 和启动结果处理。[EngineLauncher.kt](https://github.com/Weiss-UltimateSavior/Tyranor-Next/blob/228cc343b58af8cc3d9f46316641477f1178ac77/app/src/main/java/com/tyranor/next/core/game/launch/EngineLauncher.kt)

```kotlin
EngineType.SYSTEMNNN -> SystemNnnHost.createLaunchIntent(context, File(path), game.title)
```

已绑定外置模块的游戏条目仍沿用宿主现有的绑定策略。内置条目由游戏库重扫或宿主按需迁移。接入后的宿主启动和存档管理需在 TYN 构建中验收。
