# SystemNNN-Emberfall core

This directory provides the portable C and C++ libraries used by the game session and Android player. They cover SPT execution, resource lookup and decoding, and game audio. The implementation derives behavior and file formats from tinyan's SystemNNN. [Source and porting notes](PORTING.md)

## Build from a checkout

From the repository root on Linux or WSL, use CMake 3.16 or later with C11 and C++17 compilers. The tree includes zlib, libpng and the host test fixtures.

```sh
cmake -S platform/scene -B build/scene-host-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/scene-host-debug --parallel 4
ctest --test-dir build/scene-host-debug --output-on-failure
```

`nnn-script-probe` reports load status, execution positions, work and callbacks as JSON. This example runs a repository fixture.

```sh
build/scene-host-debug/nnn-core/nnn-script-probe core/tests/fixtures/flow.spt
```

The host scene and renderer build, along with the Android APK build, are documented at the repository root. [Project build instructions](../README.md#构建)

## Public APIs

Add `core/` with CMake `add_subdirectory` and link the targets you use. Public headers are under `include/nnn/`.

| Target | Headers and entry points |
| --- | --- |
| `nnn-script-core` | `nnn/script.hpp` defines `CExecScript` and `CScriptCallBack`. `nnn/scheduler.hpp` defines `ScriptScheduler` for foreground script calls and returns. |
| `nnn-archive-core` | `nnn/archive.hpp` provides `load_index` and `read_member` for GTB indexes and GPK members. |
| `nnn-image-core` | `nnn/image.hpp` provides `decode_dwq` for uncompressed type 0 bitmaps and `decode_jpeg` for JPEG payloads. |
| `nnn-png-core` | `nnn/png.hpp` provides `decode_dwq_png` for PNG images in DWQ envelopes. |
| `nnn-dwq-core` | `nnn/dwq.hpp` provides `nnn::dwq::decode` for supported bitmap, JPEG and PNG DWQ variants. It returns owned RGBA8 pixels and margin metadata. |
| `nnn-game-audio` | `nnn/game_audio.hpp` provides `GameAudio` for asset decoding, mixing and playback control. `pull` writes interleaved signed 16-bit stereo PCM. |

`CExecScript` borrows a callback that must outlive the VM. `LoadFile` reads a script through the default file reader or a supplied `Reader`. `LoadBytes` accepts a buffer. Both validate a candidate before replacing the installed program. Check the returned `Status` before executing, even when a failed load leaves an earlier program available.

`Exec(work_budget)` executes on the calling thread and returns status, a legacy return code, a word-index PC and work used. Use `PendingSelection()` and `Choose()` for a pending choice. `ScriptScheduler` manages script transfers across foreground slots with its own transition budget. A callback may copy text or parameters it needs to keep, and a failed execution can leave earlier host effects in place.

The loader checks record and table bounds, executable targets and command payloads. The interpreter dispatches validated records by format. The host can log unsupported minor commands through `UnhandledInstruction` as execution advances. Text is passed as source bytes for the frontend to decode.

For a game directory, `emberfall::scene::GameSession` combines the scheduler, layer renderer and audio. `nnn::RuntimeContext` owns shared system parameters and read state. [Game session API](../platform/scene/include/emberfall/game_session.hpp) · [Runtime context API](include/nnn/runtime_context.hpp)
