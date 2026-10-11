/*
 * cogmind_gpu.h -- GPU console rendering extension of the SDL 1.2 shim (SDG.dll = sdl12-compat + this).
 *
 * The retail game draws its console (a grid of cells: glyph, foreground, background) by software: for every cell that
 * changed since the previous frame, XConsole::blit() (0x42a5b0) SDL_FillRect()s the cell background, recolours the
 * glyph's pixels IN the font bitmap and SDL_UpperBlit()s them onto the 1.2 screen surface; SDL_Flip() then uploads the
 * whole surface. The overlay (hybrid_src/gpu_blit.cpp) replaces that loop: it makes the same per-cell decisions but,
 * instead of touching pixels, writes one COGGPU_Cell per changed cell into a buffer owned by this shim. The shim keeps
 * a persistent render-target texture (the "screen") and draws the cells into it on the GPU; SDL_Flip() presents it.
 *
 * All functions are __cdecl exports of the shim DLL. The overlay finds them with GetProcAddress; if any is missing or
 * COGGPU_Begin() returns NULL the overlay runs the untouched retail software path, so the exe also works with a plain
 * SDL 1.2 DLL.
 *
 * Pointers called "surface12" are the SDL 1.2 SDL_Surface* exactly as the game sees them (the shim's SDL12_Surface).
 */
#ifndef COGMIND_GPU_H
#define COGMIND_GPU_H

#include <stdint.h>

#define COGGPU_ABI 1

/* One changed console cell. 16 bytes, natural alignment (2). Colours are 8-bit RGB. */
typedef struct COGGPU_Cell {
    int16_t dx, dy;       /* destination top-left on the screen, pixels (console origin and screen-shake offset applied) */
    int16_t sx, sy;       /* top-left of the glyph inside the font bitmap, pixels; ignored when font < 0 */
    int16_t font;         /* slot from COGGPU_Font(); -1 = no glyph, draw the background only */
    uint8_t bgR, bgG, bgB;/* cell background: opaque fill of the cell rectangle */
    uint8_t fgR, fgG, fgB;/* glyph tint (see COGGPU_End) */
} COGGPU_Cell;

#ifdef __cplusplus
extern "C" {
#endif

/* Returns COGGPU_ABI. */
int COGGPU_Abi(void);

/*
 * Start a console pass on the 1.2 screen surface.
 *  - Returns NULL if GPU console rendering is unavailable (disabled with COGGPU=0, renderer lacks render targets or
 *    geometry, screen is not the live video surface, ...). The caller must then use the software path; nothing
 *    has been changed.
 *  - Otherwise returns a buffer with room for `capacity` cells, valid until COGGPU_End(). *fullRedraw is set to 1
 *    when the persistent screen was just (re)created or its contents were lost (device reset): the caller must emit
 *    EVERY cell of the console (treat the previous frame as absent), else to 0.
 * Creates the render target on first use. Not re-entrant.
 */
COGGPU_Cell *COGGPU_Begin(void *screenSurface12, int capacity, int *fullRedraw);

/*
 * Atlas slot for a font bitmap (an SDL 1.2 surface). Valid between COGGPU_Begin and COGGPU_End; the slot stays valid
 * until the surface is freed with SDL_FreeSurface (the shim drops the atlas then). Returns -1 on failure.
 * The atlas holds only the glyph SHAPE as an 8-bit coverage mask, taken from the surface the way the software blit
 * reads it:
 *   - surface has SDL_SRCALPHA and an alpha mask: coverage = per-pixel alpha;
 *   - else if it has SDL_SRCCOLORKEY: coverage = 255 where pixel (RGB bits) != colorkey, else 0;
 *   - else coverage = 255 everywhere.
 * RGB of the bitmap is ignored (the retail code overwrites it with the foreground colour). The shim samples the mask
 * once, lazily; the bitmap must not change shape afterwards.
 */
int COGGPU_Font(void *fontSurface12);

/*
 * Finish the pass: `count` cells (all `cellW` x `cellH` pixels) are in the buffer returned by COGGPU_Begin.
 * Drawing order is equivalent to the software path: for every cell its background rectangle is filled opaquely, then,
 * if font >= 0, the glyph rectangle (sx,sy,cellW,cellH) of that font's coverage mask is alpha-blended over it tinted
 * with the foreground colour:  dst = bg + (fg - bg) * coverage / 255.  Cells never overlap, so the shim may draw all
 * backgrounds first and all glyphs second. Cells outside the screen are clipped.
 * (Doc-only note: the shim records the cells and draws them lazily, before the next present, COGGPU_Readback or redirected
 * SDL_FillRect/SDL_UpperBlit/SDL_FreeSurface; passes are always drawn in call order. A pass with count 0 costs nothing.
 * Also, SDL_Flip/SDL_UpdateRect of a GPU screen that has not changed since the last present may skip the present; the window
 * keeps its last image and a real present is forced at least every 250 ms and after any window event.)
 */
void COGGPU_End(int count, int cellW, int cellH);

/*
 * Test / diagnostics hooks.
 * COGGPU_Readback: copies a rectangle of the persistent screen (not the window) into `dst` as 32-bit XRGB8888
 * (memory order B,G,R,X), `pitch` bytes per row. Returns 0 on success, -1 if there is no GPU screen.
 */
int COGGPU_Readback(void *dst, int pitch, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

/*
 * Behaviour of the ordinary 1.2 API while the GPU screen exists (after the first successful COGGPU_Begin until the
 * next SDL_SetVideoMode / SDL_Quit):
 *   SDL_FillRect(screen, rect, color)   -> filled on the GPU screen (rect NULL = whole screen).
 *   SDL_UpperBlit(src, srcrect, screen, dstrect) -> drawn on the GPU screen (colorkey / per-pixel alpha / opaque).
 *   SDL_UpperBlit(screen, srcrect, dst, dstrect) -> the requested region of the screen is first read back from the
 *                                       GPU into the CPU screen surface, then blitted normally (screenshots).
 *   SDL_Flip / SDL_UpdateRect(s) on the screen -> present the GPU screen (no CPU surface upload).
 * Environment: COGGPU=0 disables the GPU screen (software path, for A/B tests); COGGPU_LOG=<file> appends diagnostics;
 * COGGPU_DUMP=<dir>: when <dir>/dump.req exists at a present, write <dir>/frame.bmp (the GPU screen, 32-bit BMP)
 * and then <dir>/dump.ack, and delete dump.req (software mode dumps the CPU screen surface the same way).
 */
#endif
