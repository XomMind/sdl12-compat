/*
 * cogmind_vr.h -- in-process VR / 3D map view of the SDL 1.2 shim (SDG.dll).
 *
 * The overlay (hybrid_src/vr_tap.cpp, inside the retail exe) reads the world state the 2D map is drawn from (cell grid,
 * known/visible grids, remembered glyphs, the CMap view buffer, the player position) and copies one snapshot per frame
 * into a buffer owned by this shim. A shim thread renders that snapshot in 3D: to a Valve Index / any SteamVR headset
 * through OpenVR (COGVR=openvr) or to a desktop preview window (COGVR=desktop). The game thread never waits for the
 * renderer; the renderer never touches game memory.
 *
 * Fairness invariant: the snapshot carries only what the player can already see in the original 2D render -- glyphs of
 * the 2D map view this frame, remembered records, and the known/visible state (the 2D view can scroll over all of it).
 * No extra game state (hidden entities, item presence, terrain truth) is exported, so the 3D view cannot reveal anything the
 * 2D game does not. The renderer styles cells from ch and the flags below only.
 *
 * All functions are __cdecl exports of the shim DLL. The overlay resolves them with GetProcAddress; when they are absent
 * or COGVR_Abi() differs the overlay does nothing.
 */
#ifndef COGMIND_VR_H
#define COGMIND_VR_H

#include <stdint.h>

#define COGVR_ABI 2

/* COGVR_Cell.flags */
#define COGVR_KNOWN   0x01  /* the player knows this cell (BS+0x674); otherwise the cell is not drawn and the rest is zero */
#define COGVR_VISIBLE 0x02  /* in view this turn (BS+0x69c != 0) */
#define COGVR_LIVE    0x04  /* ch/fg/bg are the 2D map view's cell this frame (visible and inside the view); else the remembered
                               record (BS+0x7c4), undimmed: the renderer applies its own memory style to !VISIBLE cells */
#define COGVR_BACK    0x20  /* bg is meaningful (record back-valid, or LIVE) */
#define COGVR_PLAYER  0x40  /* the player's cell (footprint[0]) */

/* One world cell, 12 bytes. Grid is column-major like the game's Array2D: index = x * mapH + y. */
typedef struct COGVR_Cell {
    uint16_t ch;            /* character code as stored by the game (' ' = nothing) */
    uint8_t flags;
    uint8_t reserved0;
    uint8_t fgR, fgG, fgB;
    uint8_t bgR, bgG, bgB;
    uint8_t reserved1[2];
} COGVR_Cell;

typedef struct COGVR_Header {
    int32_t mapW, mapH;         /* must equal the arguments of the matching COGVR_Begin */
    int32_t playerX, playerY;   /* world cell; -1 when there is no player entity */
    int32_t viewX, viewY;       /* world cell shown at the top-left of the 2D map view (-scroll) */
    int32_t viewW, viewH;       /* 2D map view size in map cells */
    uint32_t gameTick;          /* retail tick counter (0xcaed20), for animation sync only */
    /* ABI 2: pixel geometry of the 2D map view on the 1.2 screen surface, for the VR input bridge (cell -> mouse position).
       Pixel of view cell (vx,vy) top-left = (mapPxX + vx*cellPxW, mapPxY + vy*cellPxH); REX origin included, screen shake not.
       cellPxW/H <= 0 when unknown: the bridge then injects no mouse events. */
    int32_t mapPxX, mapPxY;
    int32_t cellPxW, cellPxH;
} COGVR_Header;

#ifdef __cplusplus
extern "C" {
#endif

/* Returns COGVR_ABI. */
int COGVR_Abi(void);

/*
 * Start a snapshot of a mapW x mapH world. Returns a buffer of mapW*mapH cells, valid until COGVR_End, or NULL when no
 * snapshot is wanted right now (COGVR unset, renderer not running, throttled, size out of range): the caller then does
 * nothing more for this frame. Called on the game thread. Not re-entrant.
 */
COGVR_Cell *COGVR_Begin(int mapW, int mapH);

/* Publish the buffer filled since COGVR_Begin. `header` is copied. */
void COGVR_End(const COGVR_Header *header);

#ifdef __cplusplus
}
#endif

#endif
