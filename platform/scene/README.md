# SystemNNN game session

Use `emberfall::scene::GameSession` for a game directory. A session keeps the scheduler, script stack, script variables, graphics, transitions and pending input alive until it is destroyed. The public interface is in `include/emberfall/game_session.hpp`. The older `first_frame` entry points are retained for existing diagnostic callers.

Call `step_frame(elapsed_ms, input)`, then read `surface()` for the current top-left RGB8 frame. The surface is owned by the session. Copy its pixels before the next change if they must be retained. A dialogue stays pending until `input.advance`; a selection takes a one-based `input.choice`. Timed drawing and overlap use the game's `frameTime`. A completed overlap exposes its final frame before the next VM step. Dialogue carries the source bytes, speaker bytes, message and table IDs.

Read-only skip is checked at STARTMESSAGE with the same script/serial identity as PRINT, so it stops before the first unread line's VOICE and effects; an unread STARTKOMA re-enables effect commands while message skipping continues. AUTOMESSAGE, CANNOTCLICK and CANNOTSKIP are tracked from the script (`script_restrictions()`), with `cannotSkipCheckKillShift`/`autoMessageCheckKillShift` from `game.xtx`. Platforms ask `request_advance()` before a click (it also answers for Draw/Overlap waits), drive skip advances only while `skip_effective()`, and advance on `auto_ready()`, which also runs the script auto-message timer from `printmessage.xtx`. `step_frame` itself does not gate advances, so recorded input journals replay unchanged.

Build and run the desktop framebuffer capture from the repository root.

```sh
cmake -S platform/scene -B build/scene-host-debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=OFF
cmake --build build/scene-host-debug --parallel --target nnn-scene-player
build/scene-host-debug/nnn-scene-player "/path/to/DeadEndAegis [zh]" --entry sys.spt --limit 85 --verify --capture 17 build/game-frames/deadendaegis-zh.png
```

Each PNG contains the native layer framebuffer at the specified dialogue. The companion JSON records the script word, message, active layers, source packages, canvas extents and trimmed sprite bounds. Dialogue and choices are available to the presentation frontend. The desktop runner automatically clicks dialogue, consumes timed frames and chooses the requested option. Use `--choice 2` for the second branch, `--click-draw` to simulate a click during drawing, and `--list` to see the resource bindings at each dialogue.

Graphic lookup checks loose game files first, then the prefix-matched package and other available package indexes. An 8A sprite keeps its original canvas position when its transparent margins are dropped from the cache. A missing graphic or an unhandled effect is reported once per session. BUFFER retains a texture off-screen; MAE retains the preceding effect. SETCG records gallery registration and can resolve its graphic from the bound layer or the game's CG list.

For audio, set `SessionOptions::audio.sample_rate` to the device rate before creating the session. On the game thread, `step_frame` dispatches script events, reads the requested VPK range or loose file, and decodes it: VAW byte 57 selects PACKTYPE 2 (wrapped Ogg Vorbis) or PACKTYPE 0 (raw 8/16-bit PCM WAVE), as `CWaveData::LoadWaveRoutine` does; WGQ music is PACKTYPE 6 Ogg. On the audio thread, call `session.audio().pull(dst, frames)` with space for `2 * frames` signed 16-bit samples; silence, resampling, mixing and fades all advance on this device clock. Keep pulling while the VM waits for input. The pull path does not open files or decode Vorbis. The optional `take_audio_events()` queue mirrors already-applied requests; set `queue_audio_events=false` for a PCM-only frontend.

Music IDs are one-based; script SE and UI SE IDs are zero-based. Call `play_system_se(id)` for a menu cue from `nya/systemse.xtx`. Speech checks loose chapter and system voices before the matching VTB/VPK. Only the requested VPK member is read. The cache holds decoded clips (64 MiB by default); active tracks also retain their segments. All MML parts of the current score are ready before playback starts, so a device buffer can cross the intro/loop boundary without a gap. `set_character_slider(prefix, steps)` adjusts the corresponding role in `spt/charaname.xtx`.

This game's bytecode matches upstream `scriptDefine.h`: suffixes `66/67` set the next SE/voice fade, `68/69` set SE/voice volume, `6a` sets music volume and `6c` records a character voice flag. Music plays/stops and music volume changes carry their own fade time (tenths of a second); the host can also call `set_next_music_fade`.

To reproduce the opening excerpt and the trace (one row per VM request, with decoded file, rate, channels and frames for plays):

```sh
game="/path/to/DeadEndAegis [zh]"
build/scene-host-debug/nnn-scene-player "$game" --entry sys.spt --limit 85 --verify \
  --dialogue-ms 1500 --wait-voice --wav-start-ms 9500 --wav-ms 22000 \
  --wav build/game-audio/deadendaegis-zh.wav --audio-stats build/game-audio/deadendaegis-zh.json
cmake --build build/scene-host-debug --target nnn-audio-check
build/scene-host-debug/nnn-core/nnn-audio-check "$game"
```

`--wait-voice` lets foreground speech finish before the simulated reader clicks. For the real debug menu, use `--entry dea_debug.spt --limit 12 --click-draw --choice 1` or `--choice 2`. The separate audio check exercises all four voices, role gain, SE and system routing, fade endpoints at 48 kHz, and sample-exact transitions of bgm01, bgm10, bgm17 and the MML-free short.wgq at their native rate.
