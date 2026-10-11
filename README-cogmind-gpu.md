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

Environment: `COGGPU=0` disables the GPU screen (stock software path), `COGGPU_LOG=<file>` logs `PRESENT fps=...` lines,
`COGGPU_DUMP=<dir>` writes `<dir>/frame.bmp` when `dump.req` appears.

Build (32-bit Windows, mingw, SSE2 float math): `gpu/build.sh` in decog (pins this repo and SDL2 as submodules). The
overlay that feeds the cells to this shim is `hybrid_src/gpu_blit.cpp` in decog.
