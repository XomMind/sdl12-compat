# sdl12-compat + Cogmind GPU console (branch `cogmind-gpu`)

`release-1.2.78` of [sdl12-compat](https://github.com/libsdl-org/sdl12-compat) (SDL 1.2 API on SDL2, zlib licence) plus a
GPU console renderer for Cogmind Beta 17.1. Used by [XomMind/decog](https://github.com/XomMind/decog) (`gpu/`,
`docs/WINDOWS.md`).

What changed against upstream (`git diff release-1.2.78 cogmind-gpu`):

- `src/cogmind_gpu.h` - the contract (`COGGPU_Begin/Font/End/Readback/Abi`, 16-byte `COGGPU_Cell`).
- `src/cogmind_gpu.inc.c` - the extension, `#include`d at the end of `SDL12_compat.c` so it can use that file's statics: a
  persistent render-target texture is the 1.2 screen, glyph shapes live in per-font atlas textures, changed console cells
  are recorded and drawn as batched `SDL_RenderGeometryRaw` quads at present time, an unchanged screen is not re-presented
  (forced every 250 ms and after window events), `SDL_FillRect` / `SDL_UpperBlit` on the screen are redirected to the GPU
  screen, blits from the screen read it back first.
- `src/SDL12_compat.c` - 135 lines of hooks (`SetVideoMode`/`Quit` teardown, `SDL_FreeSurface` atlas invalidation,
  `SDL_FillRect`, `SDL_UpperBlit`, `PresentScreen`, `SDL_UpdateRects`, event watch) and `SDL20_LIBNAME` = `SDG2.dll` so the
  build can sit beside a stock `SDL2.dll`. It also fixes an upstream bug: the video texture was not recreated when
  `SDL_SetVideoMode` was called again with the same flags but a different size (the next `SDL_UpdateRect` then overran the
  smaller texture under D3D9).
- `src/SDL12_compat.c` `UseMsvcrtHeap()` - on Windows, SDL2's allocator is set to msvcrt.dll's `malloc/calloc/realloc/free`
  (when msvcrt is loaded and before SDL2 allocates anything), restoring SDL 1.2's heap contract: its mingw DLL used msvcrt's
  heap. Without it, every Cogmind sound load crossed heaps: SDL_mixer 1.2 decodes each `.ogg` into a msvcrt `malloc` block
  and passes it to `SDL_FreeWAV` (= SDL2's `free`), and Cogmind's pitch-shifted `Mix_Chunk`s come from msvcrt's `malloc`
  via `GetProcAddress`.

Runtime audio: decog's source-built `SDG2.dll` includes a WASAPI refill correction. It checks endpoint space before
waiting for an event and retains enough buffering for the requested mixing quantum, including across device recovery.
The shim does not force WinMM or enable legacy audio conversion; SDL2's normal backend selection and explicit
`SDL_AUDIODRIVER` overrides remain in effect. The older
[forced-WinMM patch](https://github.com/struffl/sdl12-compat/commit/b02f5d0ffb8b809472833bb2b2c3fb872711e8b0)
was a workaround, not the current repair. See decog's `docs/WINDOWS.md` for real Zion mixer timing evidence and limitations.

Environment: `COGGPU=0` disables the GPU screen (stock software path), `COGGPU_LOG=<file>` logs `PRESENT fps=...` lines,
`COGGPU_DUMP=<dir>` writes `<dir>/frame.bmp` when `dump.req` appears.

Pacing / latency instrumentation (`src/cogmind_pace.inc.c`, all off by default, details in the file header):

- `COGGPU_LOG=<file>`: every 2 s a block of lines: `PRESENT` (frames, fps, mean present cost, `ivMed/ivP95/ivP99/ivMax` =
  present-to-present interval from QPC, `ivOver` = intervals > 1.5x median, `riv*` = the same for real, non-skipped
  presents), `LOOP` (period between consecutive event-drain starts = game loop iteration), `FLIPCALL` (`SDL_Flip` entry to
  return), `LATENCY` (see below), `PACE`.
- `COGGPU_INJECT=<ms>`: a helper thread pushes a synthetic key press/release pair (SDL 1.2 `SDLK_F15`, ignored by the game) into
  the normal event queue every ~`<ms>` (+-25 % pseudo-random phase) and logs, per window, the latency from the push to the end
  of the first present after the game polled the event (`toDrain` = push to the poll that delivers it, `drainToPresent` = rest).
  The delivered key counts as a screen change, so the present is not skipped.
- `COGGPU_PACE=<hz>` or `vsync`: replaces the game's millisecond-quantised frame limiting by a slot schedule. The first
  `SDL_PollEvent` after `SDL_Flip` (start of the game's event drain) waits until `slot - work - margin` (`work` = EMA of
  release -> end of the present), `NtDelayExecution` for the bulk and a `QueryPerformanceCounter` spin for the last
  `COGGPU_PACE_SPIN_US` (default 2000 us); slots advance by exactly one period, a missed slot is skipped (no catch-up burst).
  `vsync` requests a vsynced renderer, re-anchors the slot grid at every present return and uses the lower quartile of the recent
  present intervals as the period. `COGGPU_PACE_MARGIN_US` (default 500, vsync 1000). While pacing, the game's own limiter sleep (`SDL_Delay`
  called from `FrameLimiter::limitFrame`, identified by its call site 0x41b048 in the retail exe through the import table) is
  swallowed; `COGGPU_PACE_DELAY=0` keeps it. `fpsCap=0` in the game's system.cfg removes the limiter altogether.
- `COGGPU_SKIP=0`: never skip the present of an unchanged GPU screen (default: skipped, forced every 250 ms; `vsync` pacing
  implies `COGGPU_SKIP=0` unless set explicitly).

3D / VR map view (`src/cogmind_vr.inc.c`, contract `src/cogmind_vr.h`, exports `COGVR_Abi/Begin/End`; off unless `COGVR` is set):

- The overlay copies one world snapshot per frame (`COGVR_Cell` per map cell, column-major, plus a header) into a triple buffer
  (SDL mutex) at most 30 times a second; `COGVR_Begin` returns NULL when nothing is wanted (throttled, renderer not running,
  map larger than 1024x1024). A render thread started on the first `COGVR_Begin` does everything else: it loads `d3d11.dll`,
  `dxgi.dll`, `d3dcompiler_47.dll` (HLSL compiled at run time, `vs_4_0`/`ps_4_0`, feature level 10_0 or better) and, for
  OpenVR, `openvr_api.dll` with `LoadLibrary`; any failure is logged once and turns the view off, the game is unaffected. The
  render thread never calls SDL video/renderer functions and never reads game memory.
- Scene: one unit per cell. Known cells get a floor tile (background colour when there is one) and their glyph extruded from the
  public-domain 8x8 font (`font8x8_basic.h`, merged boxes per glyph): terrain lies flat (`#` 1.0 tall, doors `+ ' / =` 0.45,
  floor dots flat, the rest 0.1); on LIVE cells (what the 2D view shows this frame) letters and `@` are robots and
  `) ( [ ] ! ? * % } { " $ & =` items: they stand upright (items smaller), hover, bob and turn (and lean) to the viewer in the
  vertex shader. The classification uses only the glyph and the KNOWN/VISIBLE/LIVE/BACK/PLAYER flags, i.e. nothing the 2D map
  does not show; remembered cells are always terrain. The game window is never touched: the view has its own window.
  Characters above 127 are small cubes. Cells not in view are drawn desaturated, dimmed and blue-tinted; the
  player has a pulsing halo ring and a beacon. One instanced cube draw for the map (budget 100000 boxes: floor dots of
  remembered cells, then all floor dots, then wall glyphs are simplified to stay inside it; logged), a second instance buffer
  for per-frame boxes (avatar, controllers). HDR scene, 4x MSAA when available, reversed-Z, exponential fog into a near-black
  void, bloom, ACES tone map into sRGB targets.
- `COGVR=desktop`: keyboard-only operation: WASD, Q/E down/up, Shift fast, arrows look, `+`/`-` scale,
  Home = recenter above the player at 60 degrees. Mouse look/wheel remain optional, not required.
  P or Tab = Examine (free fly) <-> Cogmind POV (camera at the player's cell, eye height 0.55 cells; look stays free). The
  avatar stands in for the headset user at the free-fly camera; C switches the window between the free-fly view and the
  director's stream camera, so both can be checked without a headset. Closing the window turns the view off.
- `COGVR=openvr`: SteamVR through the OpenVR C API (`VR_InitInternal2`, `FnTable:IVRSystem_023` / `IVRCompositor_029`,
  OpenVR SDK v2.12.14 = the newest one Proton 10's 32-bit vrclient knows), the device on `GetDXGIOutputInfo`'s adapter, per-eye
  targets at the recommended size, both eyes submitted as `TextureType_DirectX` + `ColorSpace_Auto` (sRGB textures).
  SteamVR Input actions use the packaged `cogvr/actions.json` and Index bindings; unavailable actions fall back to
  legacy controller state, discovering the joystick axis rather than assuming the trackpad is the stick.
  Grip toggles grab by default (`COGVR_STICKY_GRIP=0` for hold); one hand pulls the world, both yaw/scale it
  (1 cell = 0.5 mm .. 100 m). Left stick flies at 20 cells/s; click latches cruise at 100 cells/s, click again stops.
  Right stick smoothly turns and flies vertically (`COGVR_SNAPTURN=30` for snap). Menu tap toggles POV; hold 1 s resets.
  A/trigger and POV stick actions go through the legacy SDL event bridge, preserving retail validation.
  `COGVR_VIGNETTE=0..1` controls light movement/turn edge darkening (default 0.35), headset eyes only.
  The separate `Cogmind VR - stream camera` window is resizable, `COGVR_STREAM_SIZE=WxH` (default 1280x720).
  Real Index bindings and Proton/DXVK remain unverified.
- Rigged avatar: `COGVR_AVATAR=<file.glb>` loads a self-contained skinned GLB with embedded PNGs (<=64 joints).
  `src/cogmind_vrmesh.inc.c` uses pinned cgltf/stb_image, with SDL2 allocator/file helpers rather than a new CRT dependency.
  CPU skinning runs once per frame for all views: the project Wine shader compiler cannot dynamically index a bone cbuffer.
  Head follows HMD pitch/roll; controller hands use two-bone arm IK; legs float, hair chains use damped springs.
  Private artwork is never packaged. Original 2D geometry does not provide body depth or back surfaces: those are authored.
- Legacy input bridge: `src/cogmind_vrbridge.inc.c`, game thread before the SDL event drain. It injects keypad movement,
  pointer motion and clicks, wait, arm aiming and cancel. Aim motion precedes a click by two loop passes so retail can
  update its target. `COGVR_LOG_BRIDGE=1` logs commands; `COGVR_BINDINGS` overrides keys/repeat cadence.
- Director (stream camera), following the user's editing rules: within a shot the camera is locked off (fixed position and
  FOV 50-60, no sway, bob, shake or zoom); it only pans, critically damped, when the subject leaves the middle 50% of the
  frame, and settles again. Shots: chase/over-the-shoulder, wide establishing, low dramatic (on the avatar, or on Cogmind's
  area during action), top-down (avatar more than 3 m above the map, or a tiny world), two-shot of the avatar and the Cogmind
  player cell. Hard cuts only, at least 4 s apart, typically every 8-16 s, on the next 4-beat boundary of the music (without
  a confident tempo: the next onset, else after 3 s). Action from the snapshot only: motion energy = LIVE cells within 12 of
  the player whose glyph or colour changed between snapshots; a spike cuts to the low shot of Cogmind's area / two-shot,
  calm alternates wide / chase / two-shot. An occluded subject (wall heights ray-marched) forces a cut. Each cut is logged:
  `cut t=... <from> -> <to> reason=timer|action|height|lost|occlusion/beat|onset|timer|now bpm=... conf=... bar=...`.
- Beat tracking: `AudioCallbackWrapper` hands the final mixed device buffer to `CogVR_AudioTap` (read-only; the samples are
  never changed; nothing runs unless the 3D view is running): mono, ~150 Hz low-pass, 10 ms energy hops, positive flux of
  `log(1 + k e)`. The render thread estimates the tempo by autocorrelation over the last 8 s (80-160 BPM preferred, `conf` =
  the normalised peak, usable from 0.30), the beat phase by a comb and the downbeat from the accents (with hysteresis).
- `COGVR_LOG=<file>`: init results, adapter, eye size, avatar box count, level-of-detail changes, director cuts, every 2 s
  `fps`, snapshot version, box count, player cell, camera. `COGVR_DUMP=<dir>`: when `<dir>\dump.req` appears, writes
  `<dir>\vr.bmp` (the window's view in desktop mode, else the left eye) and in OpenVR mode `<dir>\stream.bmp`, then
  `dump.ack`, and deletes `dump.req`.
- Wine on macOS: wined3d's Vulkan backend offers no feature level 10 on MoltenVK; run with `WINE_D3D_CONFIG=renderer=gl`
  (feature level 10_1). Proton uses DXVK.

Build (32-bit Windows, mingw, SSE2 float math): `gpu/build.sh` in decog (pins this repo and SDL2 as submodules, and downloads
`font8x8_basic.h`, `openvr_capi.h`, `cgltf.h` and `stb_image.h` by sha256 into its work-tree include directory. The
overlay that feeds the cells to this shim is `hybrid_src/gpu_blit.cpp` in decog; the 3D view's is `hybrid_src/vr_tap.cpp`.
