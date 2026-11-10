# SystemNNN source and porting notes

SystemNNN-Emberfall is an Android compatibility implementation of tinyan's SystemNNN. The portable code uses the original engine as a reference for execution order, callback behavior and file layouts, with explicit little-endian decoding, checked bounds and owned state.

- The engine reference is `tinyan/SystemNNN` at commit `78a05053c78b3b80065a9a209c6685a7c60a2e83`. It provides the script interpreter, engine callbacks, configuration handling and resource readers. [Engine source](https://github.com/tinyan/SystemNNN/tree/78a05053c78b3b80065a9a209c6685a7c60a2e83)
- The script compiler reference is `tinyan/SystemNNN_Editor` at commit `e5c46148fa1dc429f3e0391da7c056d7baa82622`. `scriptCompiler.cpp` and `minicompiler.cpp` document how the executable records, branches and script adjustment tables are written. [Editor source](https://github.com/tinyan/SystemNNN_Editor/tree/e5c46148fa1dc429f3e0391da7c056d7baa82622)
- The asset writer reference is `tinyan/systemNNN_support` at commit `07aa58cec71cedeeda61344fb9b596a540cb3a53`. `pacNyan/packroutine.cpp` and `nerune/packroutine.cpp` show how DWQ assets and GTB/GPK archives are laid out. [Support tools source](https://github.com/tinyan/systemNNN_support/tree/07aa58cec71cedeeda61344fb9b596a540cb3a53)

## Source mapping

| Original files | Portable implementation |
| --- | --- |
| `nnnLib/execscript.cpp`, `nnnLib/execscript.h`, `nnnLib/scriptCallBack.h`, `nnnUtilLib/scriptDefine.h`, `nnnUtilLib/scriptcommand.h` | `src/loader.cpp`, `src/interpreter.cpp`, `src/program.hpp` and `include/nnn/script.hpp` implement SPT validation and execution. The VM preserves word offsets and passes owned command data to the host for the duration of the callback. |
| `nnnLib/gamecallback.cpp`, `nnnLib/backExecClass.cpp` | `src/scheduler.cpp` and the game session in `platform/scene` manage script transfers and independent background execution. |
| `nnnLib/namelist.cpp`, `nnnLib/gamecallback.cpp` | `src/function_registry.cpp` and `include/nnn/game_config.hpp` resolve per-game functions and configuration. |
| `nyanPictureLib/Picture.cpp`, `nyanPictureLib/pngLoader.cpp`, `pacNyan/packroutine.cpp`, `nerune/packroutine.cpp` | `src/image.cpp`, `src/jpeg.cpp`, `src/png.cpp`, `src/dwq.cpp` and `src/archive.cpp` implement image decoding and archive reads with bounded input sizes and offsets. |
| Audio command paths in `nnnLib/gamecallback.cpp` and the original music and voice controllers | `src/music.cpp`, `src/voice.cpp`, `src/audio_assets.cpp` and `src/game_audio.cpp` implement command handling, decoding and PCM output. |

The SPT inputs in `tests/fixtures/` include authored programs from the port's initial VM tests, with decoded word lists and expected traces. The historical comparisons used selected upstream method bodies with encoding and host adapters. The repository's CMake test targets use fixtures shipped with the source tree. [Core build and APIs](README.md)

The renderer generates a default mesh of 32 shards from a fixed seed. Full-screen wipes and the default layer shatter effect share this mesh.

For licensing and third-party component details, see the project notices. [Third-party notices](../THIRD_PARTY_NOTICES.md)
