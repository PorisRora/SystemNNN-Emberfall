# EF-S007 voice channels 0 and 1 in the first-frame host

VOLUMEONLY_VOICE(1, -999) uses the current name, permission, work and fade state of channel 1 and sends gain and stop to that channel. Both channels share the base setting and the one-shot next fade. The existing single-name getter serves channel 0; callers binding channel 1 supply current_name_for_channel. A caller-owned controller continues to tick across frames.

VOLUMEONLY_VOICE(0, -999) uses the live scene skip flags, current voice settings, name and channel permission. The overload accepting VoiceSession shares its controller across parent and child scripts and with the host clock. The caller advances fades with Tick(elapsed_ms) and the same output. The default preview starts with an unbound channel. Source anchors are gamecallback.cpp lines 9564-9621 and scriptVoiceControl.cpp lines 254-272 and 487-547.

The MUSIC(-1, 0) numeric callback invokes the session music controller synchronously with the live VM skip flags. The first_frame overload accepts the existing music state and immediate-stop output. Silent preview callers use the original overload with both output slots absent. Source anchors are gamecallback.cpp lines 10668-10735 and 14928-14933, and musicControl.cpp lines 113-130.

FILMTYPETIME (0x33330052) charges one host callback. It accepts a signed value with empty text and a trailing zero. The host continues to the next instruction with the current layers. This follows the pinned gamecallback.cpp lines 11992-11995 in the DW-014 core contract.

An explicit child-script reader selects the four-VM foreground scheduler. SCRIPT
and SUBSCRIPT can load a child, and NEXT resumes its parent's saved PC. The image
layers, game function registry, and skip flags belong to the shared host. Folder
scenes allow at most 8 child reads, 256 KiB per script and 1 MiB of child bytes
in total. Execution shares a 100000-unit work budget and a 32-transition cap.
The older overloads keep their single-script contract. Background loop and save
restoration are outside this profile.

The initial host state is cleared, with a black opaque RGB canvas. The only
foreground layers are **4 and 5**, drawn in ascending order regardless of LOAD
order. Source `CheckBGLayer` classifies layers below4 and layer30 as background;
their distinct policy is excluded. The only effect is **NOP=0**.

Public entry: `<emberfall/scene.hpp>`, target `emberfall-scene`, namespace
`emberfall::scene`; `first_frame(script, Reader, width, height)` returns owned
RGB pixels with empty `error` on success. Any failure has nonempty `error` and
a default zero-sized, empty surface. Dependencies are frozen DW-013 script/PNG
cores and R001 renderer. Default blending is Source32Mmx, not Android SRC_OVER.

| Executed host event | Exact supported shape and state |
|---|---|
| Registered stopf5 | The optional per-game FunctionRegistry binds its name to its ID. An exact zero-argument call counts toward the callback budget and DW-012 clears the five host skip flags before execution continues. |
| STORYNAME | Existing text callback, para1=1/para3=0; preserve source metadata behavior by copying at most62 raw name bytes to diagnostics; no text rendering |
| CLEARALLEFFECT | Numeric para1=0, no values, para3=0; disable/unbind both owned layers and reset NOP/rect state; resource/work counters never reset |
| LOADDWQ | Existing text callback, para1 is signed layer4 or5, para3=0; nonempty raw key at most255 bytes; fresh Reader and decode every time. Successful binding enables layer, resets full-image source/destination rectangles and retains NOP |
| CLEAREFFECTLAYER | Numeric para1=1, values `[layer]`, para3=0; disable/unbind/reset that layer. Its next LOAD rereads and resets full-image rectangles |
| SETEFFECT | Numeric para1=2, exactly `[layer,0]`, para3=0; only a bound layer is accepted. No extra effect parameters are discarded |
| SETEFFECTRECT | Numeric para1=9, `[layer,x,y,width,height,x,y,width,height]`, para3=0; only bound layers. The two dstFrom/dstTo rectangles must be equal. Width/height are direct **extents**, not right/bottom corners; each is0..image dimension. Source origin stays(0,0), so smaller extents copy the image's top-left region, with no scaling. Signed32 positions clip through R001 |
| DRAW | Numeric para1=1, exactly one signed32 **nonzero** value, para3=0. Callback then VM advances and yields DRAW; compose/return that first frame. Value is retained, but scheduling/timing is outside this API |

Zero DRAW is explicitly rejected: its original immediate-print/non-yield path
is outside this first-yield API. PRELOAD, OVERRAP, MAE=1, FILMNAME, message/window
callbacks, PRINT, user commands, other user functions, variable callbacks and every other
host event fail explicitly. They are not acknowledged as placeholders. Internal
validated VM control flow may run, within the work budget. The core validates its
whole supported program on load; this host executes only until its first DRAW.
No later resource/event is executed after the returned first frame.

Each resource must be canonical DWQ8A accepted by the existing PNG core, with
zero cut margins; pixel channels are unmodified straight RGBA. This slice does
not apply original filename routing, patch levels, loose fallback, charset
conversion, PRELOAD retention, picture cuts, animation or effect interpolation.
The injected Reader supplies bytes from the caller's bounded self-authored pack.

Limits: script256KiB; output and resource width/height each1..2048; each at most
4Mi pixels; at most8 Reader attempts,8MiB returned per attempt,16MiB accepted
for decoding in total,8Mi cumulative decoded pixels (including repeated loads);256 callbacks;
one Exec with100000 work. Every repeated LOAD is charged again, even for the same
key; clear never refunds budgets. Resource advertised dimensions and cumulative
pixel budget are checked before decode; PNG core still validates full framing.
No image cache or cross-call session state is retained. Reader exceptions and
false results fail the frame. Reader itself must cap its I/O/allocation; this
callback signature cannot preempt a blocking or misbehaving external reader.
Reader borrows its key/output references only during that call and must not
retain them. An over-budget returned vector is rejected before PNG decode.

Diagnostics: callbacks counts admitted callback entries; resource_reads counts
Reader invocations. resource_bytes counts quota-admitted returned bytes (also
when Reader returns false); decoded_pixels counts successful decodes. layers_drawn
counts successful enabled/bound blit submissions, including zero-area or fully
clipped submissions, rather than layers that changed a pixel. Failure diagnostics
may describe the processed prefix, but the returned surface is always empty.

Source anchors: `allEffect.cpp:471-477,544-565,590-620,1003-1026,1235-1274,
1325-1375`; `commoneffect.cpp:108-111`; `effectNop.cpp:24-43`;
`gamecallback.cpp:11351-11358,11361-11543,11581-11604`.
This is a deliberately small source-backed subset, not complete game support.
