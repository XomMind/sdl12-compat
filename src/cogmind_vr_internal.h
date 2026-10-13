/*
 * cogmind_vr_internal.h -- interface between the 3D/VR view (cogmind_vr.inc.c, render thread) and the VR input bridge
 * (cogmind_vrbridge.inc.c, game thread). Both are #included into SDL12_compat.c, so these are internal (static) functions of
 * one translation unit, not DLL exports. Keep this stable: the bridge is written against it.
 */
#ifndef COGMIND_VR_INTERNAL_H
#define COGMIND_VR_INTERNAL_H

#include <stdint.h>
#include "cogmind_vr.h"

/* The controls as of the render thread's latest input update (headset or the desktop preview's keys). */
enum { COGVR_LK_NONE = 0, COGVR_LK_TERRAIN, COGVR_LK_ROBOT, COGVR_LK_ITEM, COGVR_LK_PLAYER };

typedef struct CogVRBridgeInput {
    int valid;                         /* 0: no 3D view running / no input yet (everything else is zero) */
    int povMode;                       /* 1: Cogmind POV, 0: Examine (free fly) */
    int companion;                     /* COGVR_COMPANION in the headset: the left stick moves Cogmind in Examine too */
    float headYawRad;                  /* world yaw of the head: 0 = map north (-y), + = clockwise toward +x */
    float leftStick[2], rightStick[2]; /* x right, y up/forward, -1..1, no deadzone applied */
    int leftStickClick, rightStickClick;
    float leftTrigger, rightTrigger;   /* 0..1 */
    int leftTriggerClick, rightTriggerClick;
    int leftA, rightA;
    int laserValid;                    /* the right controller's laser (desktop: the screen-centre crosshair) hits a known cell */
    int laserCellX, laserCellY;        /* world cell under the laser (ray vs floor tiles and glyph boxes) */
    int laserKind;                     /* what the 2D map shows there: COGVR_LK_* (from the glyph only, like the renderer) */
    uint32_t seq;                      /* increments once per render-thread input update */
} CogVRBridgeInput;

/* Copy of the header of the last snapshot published with COGVR_End; returns 1, or 0 when none yet. Game thread (the thread that
   calls COGVR_End). */
static int CogVR_GetLastHeader(COGVR_Header *out);

/* Ask the render thread for a short haptic tick on hand 0 (left) / 1 (right); any thread, never blocks. No-op without a headset. */
static void CogVR_RequestHaptic(int hand);

/* Thread-safe copy of the latest input (any thread; never blocks on the renderer for longer than a struct copy). */
static void CogVR_GetBridgeInput(CogVRBridgeInput *out);

#endif
