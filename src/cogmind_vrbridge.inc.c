/* VR controls -> ordinary SDL 1.2 events. Game thread only. No retail memory access.
 * Included after cogmind_vr.inc.c; PollEnter calls once per frame before draining events.
 */
#ifdef _WIN32
#include "cogmind_vr_internal.h"

static int CogVRBridge_Floor(float x)
{
    const int i = (int) x;
    return i - (x < (float) i);
}

static CogVRBridgeInput CogVRInput;
static double CogVRInputTime;
static COGVR_Header CogVRLastHeader;
static int CogVRHaveHeader;
static volatile LONG CogVRHapticPending;

static void
CogVR_SaveHeader(const COGVR_Header *header)
{
    SDL20_LockMutex(CogVR.mutex);
    CogVRLastHeader = *header;
    CogVRHaveHeader = 1;
    SDL20_UnlockMutex(CogVR.mutex);
}

static void
CogVR_FlushHaptics(void)
{
    const LONG bits = InterlockedExchange(&CogVRHapticPending, 0);
    if (bits & 1) { CogVR_Haptic(0); }
    if (bits & 2) { CogVR_Haptic(1); }
}

static void
CogVR_GetBridgeInput(CogVRBridgeInput *out)
{
    SDL20_zero(*out);
    if (!CogVR.mutex) { return; }
    SDL20_LockMutex(CogVR.mutex);
    *out = CogVRInput;
    if ((CogVR.rstate != COGVR_RUNNING) || (CogGPU_Now() - CogVRInputTime > 200.0)) { out->valid = 0; }
    if (CogVRInteg.sky >= COGVR_SKY_AMBER) { out->valid = 0; }   /* amber pauses, red cuts (docs/VR_SUIT.md) */
    SDL20_UnlockMutex(CogVR.mutex);
}

static int
CogVR_GetLastHeader(COGVR_Header *out)
{
    int have;
    if (!CogVR.mutex) { return 0; }
    SDL20_LockMutex(CogVR.mutex);
    have = CogVRHaveHeader;
    *out = CogVRLastHeader;
    SDL20_UnlockMutex(CogVR.mutex);
    return have;
}

static void
CogVR_RequestHaptic(int hand)
{
    if (hand >= 0 && hand < 2) { InterlockedOr(&CogVRHapticPending, 1L << hand); }
}

/* Render thread; intersects only the already-known presentation, never the true map. */
static void
CogVR_PublishInput(CogV3 origin, CogV3 forward, float yaw, int desktop)
{
    CogVRBridgeInput in;
    const CogVRSlot *sl = &CogVR.slot[CogVR.dIdx];
    float t;
    SDL20_zero(in);
    in.valid = 1;
    in.povMode = CogVR.pov;
    in.companion = CogVR.companion && !desktop;
    in.headYawRad = yaw;
    if (desktop) {
        in.leftStick[0] = (float) (CogVR_KeyDown('D') - CogVR_KeyDown('A'));
        in.leftStick[1] = (float) (CogVR_KeyDown('W') - CogVR_KeyDown('S'));
        in.leftStickClick = CogVR_KeyDown(VK_SHIFT);
        in.rightTriggerClick = CogVR_KeyDown(VK_SPACE);
        in.leftTriggerClick = CogVR_KeyDown(VK_BACK);
        in.leftA = CogVR_KeyDown('F');
        in.rightA = CogVR_KeyDown(VK_RETURN);
    } else {
        int h;
        in.leftStick[0] = CogVR.pad.flyX; in.leftStick[1] = CogVR.pad.flyY;
        in.rightStick[0] = CogVR.pad.turnX; in.rightStick[1] = CogVR.pad.turnY;
        in.leftStickClick = CogVR.pad.boost;
        for (h = 0; h < 2; h++) {
            const struct CogVRHand *hand = &CogVR.hand[h];
            const int trigger = hand->valid && !CogVRDeck.claim[h] && (hand->st.ulButtonPressed & COGVR_BTN(EVRButtonId_k_EButton_SteamVR_Trigger)) != 0;
            const int a = hand->valid && (hand->st.ulButtonPressed & COGVR_BTN(EVRButtonId_k_EButton_A)) != 0;
            if (h) { in.rightTriggerClick = trigger; in.rightA = a; }
            else { in.leftTriggerClick = trigger; in.leftA = a; }
        }
    }
    if (sl->cells) {
        for (t = 0.0f; t < 512.0f; t += 0.10f) {
            const CogV3 p = CogV3_Add(origin, CogV3_Scale(forward, t));
            const int x = CogVRBridge_Floor(p.x), y = CogVRBridge_Floor(p.z);
            if (x < 0 || y < 0 || x >= sl->hdr.mapW || y >= sl->hdr.mapH) { continue; }
            if (!(sl->cells[x * sl->hdr.mapH + y].flags & COGVR_KNOWN)) { continue; }
            if (p.y >= 0.0f && p.y <= SDL_max(0.06f, CogVR_WallHeight(x, y))) {
                const COGVR_Cell *c = &sl->cells[x * sl->hdr.mapH + y];
                const int gk = CogVR_GlyphKind(c->ch, c->flags);
                in.laserValid = 1; in.laserCellX = x; in.laserCellY = y;
                in.laserKind = (c->flags & COGVR_PLAYER) ? COGVR_LK_PLAYER
                             : (!(c->flags & COGVR_LIVE) ? COGVR_LK_TERRAIN
                             : ((gk == COGVR_GK_ROBOT) ? COGVR_LK_ROBOT : ((gk == COGVR_GK_ITEM) ? COGVR_LK_ITEM : COGVR_LK_TERRAIN)));
                break;
            }
        }
    }
    SDL20_LockMutex(CogVR.mutex);
    in.seq = CogVRInput.seq + 1;
    CogVRInput = in;
    CogVRInputTime = CogGPU_Now();
    SDL20_UnlockMutex(CogVR.mutex);
}

/* deck presses and auto-input ticks: render thread -> game thread (CogVR.mutex) */
#define COGVR_KEYQ 16
static struct { int sym[COGVR_KEYQ], mod[COGVR_KEYQ], uni[COGVR_KEYQ]; int n, ticks; } CogVRKeyQ;

static void
CogVR_QueueKey(int sym, int mod, int uni)
{
    SDL20_LockMutex(CogVR.mutex);
    if (CogVRKeyQ.n < COGVR_KEYQ) {
        CogVRKeyQ.sym[CogVRKeyQ.n] = sym; CogVRKeyQ.mod[CogVRKeyQ.n] = mod; CogVRKeyQ.uni[CogVRKeyQ.n] = uni;
        CogVRKeyQ.n++;
    }
    SDL20_UnlockMutex(CogVR.mutex);
}

static void
CogVR_QueueTick(void)
{
    SDL20_LockMutex(CogVR.mutex);
    CogVRKeyQ.ticks++;
    SDL20_UnlockMutex(CogVR.mutex);
}

static struct {
    int initialized, log, direction, latch, latchedDir, initialMs, repeatMs;
    int pendingClick, clickX, clickY;
    unsigned frame, clickFrame, autoFrame;
    double nextMove;
    SDL12Key leftA, rightA, cancel;
    CogVRBridgeInput previous;
} CogVRBridge;

static SDL12Key
CogVRBridge_KeyName(const char *name)
{
    if (!SDL20_strcmp(name, "period")) { return SDLK12_PERIOD; }
    if (!SDL20_strcmp(name, "escape")) { return SDLK12_ESCAPE; }
    if (!SDL20_strcmp(name, "return")) { return SDLK12_RETURN; }
    if (!SDL20_strcmp(name, "space")) { return SDLK12_SPACE; }
    if (name[0] && !name[1]) { return (SDL12Key) (unsigned char) name[0]; }
    return SDLK12_UNKNOWN;
}

static void
CogVRBridge_Init(void)
{
    const char *path = SDL12COMPAT_getenv_unsafe("COGVR_BINDINGS");
    const char *log = SDL12COMPAT_getenv_unsafe("COGVR_LOG_BRIDGE");
    CogVRBridge.initialized = 1;
    CogVRBridge.log = log && SDL20_atoi(log);
    CogVRBridge.direction = -1;
    CogVRBridge.initialMs = 180; CogVRBridge.repeatMs = 90;
    CogVRBridge.leftA = SDLK12_f; CogVRBridge.rightA = SDLK12_PERIOD; CogVRBridge.cancel = SDLK12_ESCAPE;
    if (path && *path) {
        HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        char buf[4096], *line;
        DWORD n;
        if (f == INVALID_HANDLE_VALUE) { CogVR_Log("COGVR bridge: cannot open bindings %s", path); return; }
        if (!ReadFile(f, buf, sizeof(buf)-1, &n, NULL)) { CloseHandle(f); return; }
        CloseHandle(f); buf[n] = 0; line = buf;
        while (line && *line) {
            char *next = SDL20_strchr(line, '\n');
            char *eq, *end;
            SDL12Key key;
            if (next) { *next++ = 0; }
            while (*line == ' ' || *line == '\t') { line++; }
            eq = SDL20_strchr(line, '=');
            if (!eq || *line == '#') { line = next; continue; }
            *eq++ = 0; end = line + SDL20_strlen(line);
            while (end > line && (end[-1] == ' ' || end[-1] == '\t')) { *--end = 0; }
            while (*eq == ' ' || *eq == '\t') { eq++; }
            end = eq + SDL20_strlen(eq);
            while (end > eq && (end[-1] == ' ' || end[-1] == '\r')) { *--end = 0; }
            if (!SDL20_strcmp(line, "move_repeat_ms")) {
                char *comma = SDL20_strchr(eq, ',');
                const int a = SDL20_atoi(eq), b = comma ? SDL20_atoi(comma+1) : 0;
                if (a >= 40 && b >= 40 && a <= 2000 && b <= 2000) { CogVRBridge.initialMs = a; CogVRBridge.repeatMs = b; }
            } else if ((key = CogVRBridge_KeyName(eq)) != SDLK12_UNKNOWN) {
                if (!SDL20_strcmp(line,"left_a")) { CogVRBridge.leftA = key; }
                else if (!SDL20_strcmp(line,"right_a")) { CogVRBridge.rightA = key; }
                else if (!SDL20_strcmp(line,"left_trigger")) { CogVRBridge.cancel = key; }
                else { CogVR_Log("COGVR bridge: unknown action %s",line); }
            } else { CogVR_Log("COGVR bridge: unknown key %s",eq); }
            line = next;
        }
    }
}

/* one key press (down + up) as a keyboard would send it; mod/unicode for shifted symbols such as ':' and '>' */
static void
CogVRBridge_KeyEx(SDL12Key sym, int mod, int uni)
{
    SDL12_Event ev;
    SDL20_zero(ev);
    ev.type = SDL12_KEYDOWN; ev.key.state = SDL_PRESSED;
    ev.key.keysym.sym = sym;
    ev.key.keysym.mod = (unsigned int) mod;
    ev.key.keysym.unicode = (Uint16) uni;
    PushEventIfNotFiltered(&ev);
    ev.type = SDL12_KEYUP; ev.key.state = SDL_RELEASED;
    ev.key.keysym.unicode = 0;
    PushEventIfNotFiltered(&ev);
    if (CogVRBridge.log) { CogVR_Log("COGVR bridge: key=%d mod=%d", (int) sym, mod); }
}

static void
CogVRBridge_Key(SDL12Key sym)
{
    CogVRBridge_KeyEx(sym, 0, 0);
}

static void
CogVRBridge_Click(const COGVR_Header *h, int x, int y, int press)
{
    SDL12_Event ev;
    int px, py;
    const int vx = x - h->viewX, vy = y - h->viewY;
    if (vx < 0 || vy < 0 || vx >= h->viewW || vy >= h->viewH || h->cellPxW <= 0 || h->cellPxH <= 0) {
        CogVR_RequestHaptic(1); return;
    }
    px = h->mapPxX + vx * h->cellPxW + h->cellPxW / 2;
    py = h->mapPxY + vy * h->cellPxH + h->cellPxH / 2;
    if (!VideoSurface12 || px < 0 || py < 0 || px >= VideoSurface12->w || py >= VideoSurface12->h || px > 65535 || py > 65535) { return; }
    SDL20_zero(ev); ev.type = SDL12_MOUSEMOTION;
    ev.motion.x = (Uint16) px; ev.motion.y = (Uint16) py;
    MousePosition.x = px; MousePosition.y = py;
    PushEventIfNotFiltered(&ev);
    if (!press) {
        if (CogVRBridge.log) { CogVR_Log("COGVR bridge: aim cell=%d,%d frame=%u",x,y,CogVRBridge.frame); }
        return;
    }
    ev.type = SDL12_MOUSEBUTTONDOWN; ev.button.button = 1; ev.button.state = SDL_PRESSED;
    ev.button.x = (Uint16) px; ev.button.y = (Uint16) py;
    PushEventIfNotFiltered(&ev);
    ev.type = SDL12_MOUSEBUTTONUP; ev.button.state = SDL_RELEASED;
    PushEventIfNotFiltered(&ev);
    if (CogVRBridge.log) { CogVR_Log("COGVR bridge: click cell=%d,%d pixel=%d,%d frame=%u",x,y,px,py,CogVRBridge.frame); }
}

static void CogVRAuto_Poll(void);   /* cogmind_vrrec.inc.c */

static void
CogVR_BridgePoll(void)
{
    static const SDL12Key moves[8] = { SDLK12_KP8, SDLK12_KP9, SDLK12_KP6, SDLK12_KP3, SDLK12_KP2, SDLK12_KP1, SDLK12_KP4, SDLK12_KP7 };
    CogVRBridgeInput in;
    COGVR_Header hdr;
    double now;
    int dir = -1;
    CogVRAuto_Poll();   /* autopilot + recorder notes: independent of the 3D view's input */
    if (!CogVRBridge.initialized) { CogVRBridge_Init(); }
    CogVR_GetBridgeInput(&in);
    if (!in.valid || !CogVR_GetLastHeader(&hdr)) {
        CogVRBridge.latch = CogVRBridge.pendingClick = 0;
        CogVRBridge.direction = -1;
        SDL20_zero(CogVRBridge.previous);
        SDL20_LockMutex(CogVR.mutex);
        CogVRKeyQ.n = CogVRKeyQ.ticks = 0;   /* nothing queued during a hold or a cut fires later */
        SDL20_UnlockMutex(CogVR.mutex);
        return;
    }
    now = CogGPU_Now(); CogVRBridge.frame++;
    if ((in.povMode || in.companion) && in.leftStick[0]*in.leftStick[0]+in.leftStick[1]*in.leftStick[1] > 0.25f) {
        const float a = CogVR.atan2f_(in.leftStick[0],in.leftStick[1]) + in.headYawRad;
        dir = (CogVRBridge_Floor(a * 4.0f / COGVR_PI + 0.5f) % 8 + 8) % 8;
    }
    if (in.leftStickClick && !CogVRBridge.previous.leftStickClick) {
        CogVRBridge.latch = !CogVRBridge.latch && dir >= 0;
        CogVRBridge.latchedDir = dir;
    }
    if (!in.povMode && !in.companion) { CogVRBridge.latch = 0; }
    if (CogVRBridge.latch && dir < 0) { dir = CogVRBridge.latchedDir; }
    if (dir >= 0 && (dir != CogVRBridge.direction || now >= CogVRBridge.nextMove)) {
        CogVRBridge_Key(moves[dir]);
        CogVRBridge.nextMove = now + (dir != CogVRBridge.direction ? CogVRBridge.initialMs : CogVRBridge.repeatMs);
    }
    CogVRBridge.direction = dir;
    if (in.leftTriggerClick && !CogVRBridge.previous.leftTriggerClick) {
        CogVRBridge_Key(CogVRBridge.cancel); CogVRBridge.latch = CogVRBridge.pendingClick = 0;
    }
    if (in.leftA && !CogVRBridge.previous.leftA) { CogVRBridge_Key(CogVRBridge.leftA); }
    if (in.rightA && !CogVRBridge.previous.rightA) { CogVRBridge_Key(CogVRBridge.rightA); }
    if (in.rightTriggerClick && !CogVRBridge.previous.rightTriggerClick && in.laserValid) {
        CogVRBridge.pendingClick = 1; CogVRBridge.clickX = in.laserCellX; CogVRBridge.clickY = in.laserCellY;
        CogVRBridge_Click(&hdr,CogVRBridge.clickX,CogVRBridge.clickY,0);
        CogVRBridge.clickFrame = CogVRBridge.frame;
    }
    if (CogVRBridge.pendingClick && CogVRBridge.frame >= CogVRBridge.clickFrame + 2) {
        CogVRBridge_Click(&hdr,CogVRBridge.clickX,CogVRBridge.clickY,1);
        CogVRBridge.clickFrame = CogVRBridge.frame; CogVRBridge.pendingClick = 0;
    }
    {
        int q[COGVR_KEYQ][3], n, ticks, i;
        SDL20_LockMutex(CogVR.mutex);
        n = CogVRKeyQ.n; ticks = CogVRKeyQ.ticks;
        for (i = 0; i < n; i++) { q[i][0] = CogVRKeyQ.sym[i]; q[i][1] = CogVRKeyQ.mod[i]; q[i][2] = CogVRKeyQ.uni[i]; }
        CogVRKeyQ.n = CogVRKeyQ.ticks = 0;
        SDL20_UnlockMutex(CogVR.mutex);
        for (i = 0; i < n; i++) { CogVRBridge_KeyEx((SDL12Key) q[i][0], q[i][1], q[i][2]); }
        /* auto-input: one act per tick on the laser cell, at most one every 3 game loop passes so nothing piles up */
        if (ticks && CogVRBridge.log && (!in.laserValid || (in.laserKind == COGVR_LK_PLAYER))) {
            CogVR_Log("COGVR bridge: tick, nothing to act on (laser %s)", in.laserValid ? "on Cogmind" : "on no known cell");
        }
        if (ticks && in.laserValid && !CogVRBridge.pendingClick && (CogVRBridge.frame >= CogVRBridge.autoFrame + 3)) {
            static const int dirOf[3][3] = { { 7, 0, 1 }, { 6, -1, 2 }, { 5, 4, 3 } };
            CogVRBridge.autoFrame = CogVRBridge.frame;
            if (in.laserKind == COGVR_LK_ROBOT) {
                CogVRBridge.pendingClick = 1; CogVRBridge.clickX = in.laserCellX; CogVRBridge.clickY = in.laserCellY;
                CogVRBridge_Click(&hdr, CogVRBridge.clickX, CogVRBridge.clickY, 0);
                CogVRBridge.clickFrame = CogVRBridge.frame;
                if (CogVRBridge.log) { CogVR_Log("COGVR bridge: tick click robot at %d,%d", in.laserCellX, in.laserCellY); }
            } else if (((in.laserKind == COGVR_LK_TERRAIN) || (in.laserKind == COGVR_LK_ITEM)) && (hdr.playerX >= 0)) {
                const int dx = (in.laserCellX > hdr.playerX) - (in.laserCellX < hdr.playerX);
                const int dy = (in.laserCellY > hdr.playerY) - (in.laserCellY < hdr.playerY);
                const int d = dirOf[dy + 1][dx + 1];
                if (d >= 0) {
                    CogVRBridge_Key(moves[d]);
                    if (CogVRBridge.log) { CogVR_Log("COGVR bridge: tick step %d toward %d,%d from %d,%d", d, in.laserCellX, in.laserCellY, hdr.playerX, hdr.playerY); }
                }
            }
        }
    }
    CogVRBridge.previous = in;
}

#include "cogmind_vrrec.inc.c"
#else
static void CogVR_BridgePoll(void) { }
static void CogVR_ScreenCells(const COGGPU_Cell *c, int n, int cw, int chh) { (void) c; (void) n; (void) cw; (void) chh; }
static int CogVR_RealInputBlocked(const SDL_Event *event20) { (void) event20; return 0; }
#endif
