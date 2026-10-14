/*
 * cogmind_vrrec.inc.c -- snapshot recorder (COGVR_RECORD) and autopilot (COGVR_AUTOPILOT). Game thread, except
 * CogVRRec_MusicTitle (render thread). Included at the end of cogmind_vrbridge.inc.c (uses its key injection).
 *
 *   COGVR_RECORD=<file>        append every changed snapshot to a .cogrec file (format below)
 *   COGVR_AUTOPILOT=1          walk Cogmind out of the current map, then out of the next one ... until the first map
 *                              after Materials (COGVR_AUTOPILOT_DEPTH, default -10) is reached; COGVR_AUTOPILOT_MS = cadence
 *
 * Both see only what the player sees: the snapshot (cogmind_vr.h fairness invariant) and the text of the 2D screen as
 * the GPU console draws it (HUD "Loc: -11/???", "Core 250/250"). The autopilot injects only ordinary key presses
 * (numpad moves, '>' on an exit), exactly like the VR bridge; retail decides what they do.
 *
 * .cogrec, little-endian: "COGREC1\n" | u32 abi (COGVR_ABI) | u32 sizeof(COGVR_Header) | u32 sizeof(COGVR_Cell), then
 * records until EOF: u8 kind | u8[3] 0 | u32 t_ms since the start | i32 depth (HUD, 0 unknown) | COGVR_Header | u32 n | payload
 *   kind 0 KEY:   n = mapW*mapH cells (column-major x*mapH+y, as the snapshot buffer)
 *   kind 1 DELTA: n x (u32 index, COGVR_Cell): cells changed since the previous record (same dims and depth)
 *   kind 2 EVENT: header zeroed, n bytes of UTF-8 text ("depth -10 Materials", "autopilot: exit reached", "music: ...")
 * A record is written when gameTick or any cell changed; a KEY at least every COGVR_REC_KEYEVERY records.
 */

/* ------------------------------------------------------------------------------------------------------------- */
/* screen text: the root console as drawn (glyph index decoded from the font atlas cell)                          */
/* ------------------------------------------------------------------------------------------------------------- */

#define COGVR_SCR_COLS 320
#define COGVR_SCR_ROWS 160

static struct {
    Uint8 ch[COGVR_SCR_ROWS][COGVR_SCR_COLS];
    int dirty, any;
    double parsedAt;
    /* parsed HUD */
    int haveLoc, depth;                /* "Loc: -11/???" */
    char locName[40];
    int core, coreMax;                 /* "Core 250/250", -1 unknown */
    int turn;                          /* "Time: 1", -1 unknown */
    int loggedProbe;
    double firstAt;                    /* first screen cells seen */
} CogVRScr = { { { 0 } }, 0, 0, 0.0, 0, 0, { 0 }, -1, -1, -1, 0, 0.0 };

/* Glyph index -> character of Cogmind's text font (Beta 17.1 font sets, observed on the HUD): 0..32 = ' '..'@',
   33..42 = "[\]^_`{|}~", 64..89 = 'A'..'Z', 96..121 = 'a'..'z'; other slots hold the game's own symbols (1 here). */
static int
CogVRScr_GlyphChar(int g)
{
    static const char sym[] = "[\\]^_`{|}~";
    if ((g >= 0) && (g <= 32)) { return g + 32; }
    if ((g >= 33) && (g <= 42)) { return (unsigned char) sym[g - 33]; }
    if (((g >= 64) && (g <= 89)) || ((g >= 96) && (g <= 121))) { return g + 1; }
    return 1;
}

/* COGGPU_End: the cells of one console pass, already in screen pixels. A cell's glyph index is its slot in the font
   bitmap (rows of atlas.w / cellW glyphs). */
static void
CogVR_ScreenCells(const COGGPU_Cell *c, int n, int cw, int chh)
{
    int i;
    if ((cw <= 0) || (chh <= 0)) {
        return;
    }
    for (i = 0; i < n; i++, c++) {
        const int col = ((int) c->dx + cw / 2) / cw, row = ((int) c->dy + chh / 2) / chh;
        int code = ' ';
        if ((col < 0) || (row < 0) || (col >= COGVR_SCR_COLS) || (row >= COGVR_SCR_ROWS)) {
            continue;
        }
        if ((c->font >= 0) && (c->font < COGGPU_MAXATLAS) && (CogGPU.atlas[c->font].w >= cw)) {
            code = CogVRScr_GlyphChar(((int) c->sy / chh) * (CogGPU.atlas[c->font].w / cw) + (int) c->sx / cw);
        }
        CogVRScr.ch[row][col] = (Uint8) code;
    }
    CogVRScr.dirty = CogVRScr.any = 1;
}

static int
CogVRScr_Up(int c)
{
    return ((c >= 'a') && (c <= 'z')) ? (c - 32) : c;
}

/* column after the first `word` (upper case; the screen is matched case-insensitively) on row r, or -1 */
static int
CogVRScr_Find(int r, const char *word)
{
    int c, k;
    for (c = 0; c < COGVR_SCR_COLS; c++) {
        for (k = 0; word[k] && (c + k < COGVR_SCR_COLS) && (CogVRScr_Up(CogVRScr.ch[r][c + k]) == word[k]); k++) {
        }
        if (!word[k]) {
            return c + k;
        }
    }
    return -1;
}

static int
CogVRScr_Int(int r, int *c, int *out)
{
    int v = 0, neg = 0, d = 0;
    while ((*c < COGVR_SCR_COLS) && (CogVRScr.ch[r][*c] == ' ')) { (*c)++; }
    if ((*c < COGVR_SCR_COLS) && (CogVRScr.ch[r][*c] == '-')) { neg = 1; (*c)++; }
    while ((*c < COGVR_SCR_COLS) && (CogVRScr.ch[r][*c] >= '0') && (CogVRScr.ch[r][*c] <= '9')) {
        v = v * 10 + (CogVRScr.ch[r][*c] - '0'); (*c)++; d++;
    }
    if (!d) { return 0; }
    *out = neg ? -v : v;
    return 1;
}

static void
CogVRScr_Parse(double now)
{
    int r;
    if (!CogVRScr.dirty || (now - CogVRScr.parsedAt < 100.0)) {
        return;
    }
    CogVRScr.dirty = 0;
    CogVRScr.parsedAt = now;
    for (r = 0; r < COGVR_SCR_ROWS; r++) {
        int c = CogVRScr_Find(r, "LOC:"), v, w;
        if (c >= 0 && CogVRScr_Int(r, &c, &v) && (c < COGVR_SCR_COLS) && (CogVRScr.ch[r][c] == '/')) {
            int k = 0;
            c++;
            /* the name runs to the next double space (or the panel edge) */
            while ((c < COGVR_SCR_COLS) && (k < (int) sizeof (CogVRScr.locName) - 1) && (CogVRScr.ch[r][c] > ' ') && (CogVRScr.ch[r][c] < 127)) {
                CogVRScr.locName[k++] = (char) CogVRScr.ch[r][c++];
            }
            CogVRScr.locName[k] = 0;
            CogVRScr.depth = v;
            CogVRScr.haveLoc = 1;
        }
        c = CogVRScr_Find(r, "CORE ");
        if (c >= 0 && CogVRScr_Int(r, &c, &v) && (c < COGVR_SCR_COLS) && (CogVRScr.ch[r][c] == '/') && (++c, CogVRScr_Int(r, &c, &w))) {
            CogVRScr.core = v;
            CogVRScr.coreMax = w;
        }
        c = CogVRScr_Find(r, "TIME:");
        if (c >= 0 && CogVRScr_Int(r, &c, &v)) {
            CogVRScr.turn = v;
        }
    }
    if (CogVRScr.any && (CogVRScr.firstAt <= 0.0)) {
        CogVRScr.firstAt = now;
    }
    if (!CogVRScr.haveLoc && !CogVRScr.loggedProbe && (CogVRScr.firstAt > 0.0) && (now - CogVRScr.firstAt > 20000.0) && CogVR.logPath[0]) {
        /* 20 s in and still no HUD location: show what the decoder sees (the glyph layout assumption is wrong for this font) */
        char line[COGVR_SCR_COLS + 1];
        int nonblank = 0;
        for (r = 0; r < COGVR_SCR_ROWS && nonblank < 80; r++) {
            int c, last = -1;
            for (c = 0; c < COGVR_SCR_COLS; c++) {
                const int ch = CogVRScr.ch[r][c];
                line[c] = (char) (((ch >= 32) && (ch < 127)) ? ch : (ch ? '~' : ' '));
                if (line[c] != ' ') { last = c; }
            }
            if (last >= 0) {
                line[last + 1] = 0;
                CogVR_Log("COGVR screen %3d: %s", r, line);
                nonblank++;
            }
        }
        CogVRScr.loggedProbe = nonblank > 0;
    }
}

/* ------------------------------------------------------------------------------------------------------------- */
/* recorder                                                                                                       */
/* ------------------------------------------------------------------------------------------------------------- */

#define COGVR_REC_KEYEVERY 600
#define COGVR_REC_FLUSHMS  250.0

static struct {
    int init, on;
    HANDLE f;
    Uint8 *buf;
    size_t n, cap;
    double t0, lastFlush;
    COGVR_Cell *prev;
    int prevW, prevH, prevDepth, havePrev;
    Uint32 prevTick;
    unsigned records, keys, deltas, events, sinceKey;
    /* music, render thread -> game thread (spinlock) */
    volatile LONG musicLock;
    char musicTitle[200];
    int musicSeq, musicSeen;
    int bpmNoted;
    char lastLoc[64];
} CogVRRec;

static int
CogVRRec_Reserve(size_t more)
{
    if (CogVRRec.n + more > CogVRRec.cap) {
        size_t cap = CogVRRec.cap ? CogVRRec.cap : (1u << 20);
        Uint8 *b;
        while (cap < CogVRRec.n + more) { cap *= 2; }
        b = (Uint8 *) SDL20_realloc(CogVRRec.buf, cap);
        if (!b) { return 0; }
        CogVRRec.buf = b;
        CogVRRec.cap = cap;
    }
    return 1;
}

static void
CogVRRec_Flush(void)
{
    DWORD wrote;
    if (CogVRRec.on && CogVRRec.n) {
        if (!WriteFile(CogVRRec.f, CogVRRec.buf, (DWORD) CogVRRec.n, &wrote, NULL) || (wrote != (DWORD) CogVRRec.n)) {
            CogVR_Log("COGVR record: write failed (error %lu): recording stopped", (unsigned long) GetLastError());
            CloseHandle(CogVRRec.f);
            CogVRRec.on = 0;
        }
        FlushFileBuffers(CogVRRec.f);
    }
    CogVRRec.n = 0;
    CogVRRec.lastFlush = CogGPU_Now();
}

static void
CogVRRec_Put(const void *p, size_t len)
{
    SDL20_memcpy(CogVRRec.buf + CogVRRec.n, p, len);
    CogVRRec.n += len;
}

static void
CogVRRec_U32(Uint32 v)
{
    CogVRRec_Put(&v, 4);
}

static void
CogVRRec_Init(void)
{
    const char *path;
    CogVR_InitEnv();   /* COGVR_LOG; first: getenv_unsafe returns one static buffer */
    path = SDL12COMPAT_getenv_unsafe("COGVR_RECORD");
    CogVRRec.init = 1;
    CogVRRec.prevDepth = 0x7fffffff;
    if (!path || !*path) {
        return;
    }
    CogVRRec.f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (CogVRRec.f == INVALID_HANDLE_VALUE) {
        CogVR_Log("COGVR record: cannot create %s (error %lu)", path, (unsigned long) GetLastError());
        return;
    }
    if (!CogVRRec_Reserve(4096)) {
        CloseHandle(CogVRRec.f);
        return;
    }
    CogVRRec.on = 1;
    CogVRRec.t0 = CogGPU_Now();
    CogVRRec_Put("COGREC1\n", 8);
    CogVRRec_U32(COGVR_ABI);
    CogVRRec_U32((Uint32) sizeof (COGVR_Header));
    CogVRRec_U32((Uint32) sizeof (COGVR_Cell));
    CogVRRec_Flush();
    CogVR_Log("COGVR record: %s", path);
}

static int
CogVRRec_Wanted(void)
{
    if (!CogVRRec.init) { CogVRRec_Init(); }
    return CogVRRec.on;
}

static int
CogVRRec_Depth(void)
{
    return CogVRScr.haveLoc ? CogVRScr.depth : 0;
}

static void
CogVRRec_Head(int kind, const COGVR_Header *h, Uint32 n)
{
    static const COGVR_Header zero;
    Uint32 t = (Uint32) (CogGPU_Now() - CogVRRec.t0);
    Uint8 k[4] = { (Uint8) kind, 0, 0, 0 };
    Sint32 depth = CogVRRec_Depth();
    CogVRRec_Put(k, 4);
    CogVRRec_U32(t);
    CogVRRec_Put(&depth, 4);
    CogVRRec_Put(h ? h : &zero, sizeof (COGVR_Header));
    CogVRRec_U32(n);
    CogVRRec.records++;
}

/* EVENT record (and the log line); also usable with the recorder off, for the log */
static void
CogVRRec_Event(const char *fmt, ...)
{
    char text[300];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = SDL20_vsnprintf(text, sizeof (text), fmt, ap);
    va_end(ap);
    if (n < 0) { return; }
    if (n >= (int) sizeof (text)) { n = (int) sizeof (text) - 1; }
    CogVR_Log("COGVR event: %s", text);
    if (!CogVRRec.on || !CogVRRec_Reserve(16 + sizeof (COGVR_Header) + (size_t) n)) {
        return;
    }
    CogVRRec_Head(2, NULL, (Uint32) n);
    CogVRRec_Put(text, (size_t) n);
    CogVRRec.events++;
    CogVRRec_Flush();   /* events are rare and mark what matters: never lose one to a kill */
}

/* render thread (CogVRMusic_WriteNow): the title now playing */
static void
CogVRRec_MusicTitle(const char *title)
{
    while (InterlockedExchange(&CogVRRec.musicLock, 1)) { Sleep(0); }
    SDL20_snprintf(CogVRRec.musicTitle, sizeof (CogVRRec.musicTitle), "%s", title ? title : "-");
    CogVRRec.musicSeq++;
    InterlockedExchange(&CogVRRec.musicLock, 0);
}

/* game thread: music and HUD milestones as EVENT records */
static void
CogVRRec_Notes(void)
{
    char title[200];
    int seq;
    while (InterlockedExchange(&CogVRRec.musicLock, 1)) { Sleep(0); }
    seq = CogVRRec.musicSeq;
    SDL20_memcpy(title, CogVRRec.musicTitle, sizeof (title));
    InterlockedExchange(&CogVRRec.musicLock, 0);
    if (seq != CogVRRec.musicSeen) {
        CogVRRec.musicSeen = seq;
        CogVRRec.bpmNoted = 0;
        CogVRRec_Event("music: %s", title);
    }
    if (seq && (CogVRBeat.conf >= COGVR_BEAT_CONF) && (CogVRBeat.bpm > 0.0f)) {
        const int bpm = (int) (CogVRBeat.bpm + 0.5f);
        if ((bpm - CogVRRec.bpmNoted > 2) || (CogVRRec.bpmNoted - bpm > 2)) {
            CogVRRec.bpmNoted = bpm;
            CogVRRec_Event("music: %s %d bpm", title, bpm);
        }
    }
    if (CogVRScr.haveLoc) {
        char loc[64];
        SDL20_snprintf(loc, sizeof (loc), "%d %s", CogVRScr.depth, CogVRScr.locName);
        if (SDL20_strcmp(loc, CogVRRec.lastLoc)) {
            SDL20_strlcpy(CogVRRec.lastLoc, loc, sizeof (CogVRRec.lastLoc));
            CogVRRec_Event("depth %s", loc);
        }
    }
}

/* COGVR_End: one snapshot (every root blit while recording, not only the renderer's throttled ones) */
static void
CogVRRec_Frame(const COGVR_Header *h, const COGVR_Cell *cells)
{
    const size_t ncell = (size_t) h->mapW * (size_t) h->mapH;
    const int depth = CogVRRec_Depth();
    int key;
    if (!CogVRRec.on) {
        return;
    }
    key = !CogVRRec.havePrev || (h->mapW != CogVRRec.prevW) || (h->mapH != CogVRRec.prevH) || (depth != CogVRRec.prevDepth) ||
          (CogVRRec.sinceKey >= COGVR_REC_KEYEVERY);
    if (!CogVRRec_Reserve(16 + sizeof (COGVR_Header) + ncell * (4 + sizeof (COGVR_Cell)))) {
        return;
    }
    if (!key) {
        const size_t at = CogVRRec.n;
        Uint32 changed = 0;
        size_t i;
        CogVRRec_Head(1, h, 0);
        for (i = 0; i < ncell; i++) {
            if (SDL20_memcmp(&cells[i], &CogVRRec.prev[i], sizeof (COGVR_Cell))) {
                CogVRRec_U32((Uint32) i);
                CogVRRec_Put(&cells[i], sizeof (COGVR_Cell));
                changed++;
            }
        }
        if (!changed && (h->gameTick == CogVRRec.prevTick)) {
            CogVRRec.n = at;                 /* nothing new: no record */
            CogVRRec.records--;
            return;
        }
        if ((size_t) changed * (4 + sizeof (COGVR_Cell)) > ncell * sizeof (COGVR_Cell)) {
            CogVRRec.n = at;                 /* a key is smaller */
            CogVRRec.records--;
            key = 1;
        } else {
            SDL20_memcpy(CogVRRec.buf + at + 16 + sizeof (COGVR_Header) - 4, &changed, 4);
            CogVRRec.deltas++;
            CogVRRec.sinceKey++;
            for (i = 0; i < ncell; i++) { CogVRRec.prev[i] = cells[i]; }
        }
    }
    if (key) {
        if (!CogVRRec.prev || (CogVRRec.prevW * CogVRRec.prevH < (int) ncell)) {
            COGVR_Cell *p = (COGVR_Cell *) SDL20_realloc(CogVRRec.prev, ncell * sizeof (COGVR_Cell));
            if (!p) { return; }
            CogVRRec.prev = p;
        }
        CogVRRec_Head(0, h, (Uint32) ncell);
        CogVRRec_Put(cells, ncell * sizeof (COGVR_Cell));
        SDL20_memcpy(CogVRRec.prev, cells, ncell * sizeof (COGVR_Cell));
        CogVRRec.prevW = h->mapW;
        CogVRRec.prevH = h->mapH;
        CogVRRec.prevDepth = depth;
        CogVRRec.havePrev = 1;
        CogVRRec.keys++;
        CogVRRec.sinceKey = 0;
    }
    CogVRRec.prevTick = h->gameTick;
    if ((CogVRRec.n > (4u << 20)) || (CogGPU_Now() - CogVRRec.lastFlush >= COGVR_REC_FLUSHMS)) {
        CogVRRec_Flush();
    }
}

/* ------------------------------------------------------------------------------------------------------------- */
/* autopilot                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------- */

enum { COGVR_AP_UNKNOWN = 0, COGVR_AP_FLOOR, COGVR_AP_WALL, COGVR_AP_ROBOT, COGVR_AP_EXIT };

static struct {
    int init, on, done, started;
    int lastDepth;                     /* depth (HUD) the autopilot has to leave; the run ends one map after it */
    int W, H;
    Uint8 *cls;                        /* COGVR_AP_* per cell, from the latest snapshot */
    Uint16 *glyph;                     /* last glyph seen on the cell other than the player (what is under Cogmind) */
    Uint8 *visited, *fails;
    double *blockUntil;                /* temporary blocks after a failed move (robots, closed things) */
    int *dist, *queue;
    Uint8 learned[256];                /* glyph codes found impassable: 2 failed moves on different cells */
    Uint16 learnCell[256];
    int px, py, havePlayer;
    int cadenceMs;
    double t0, nextAct, sentAt, lastMoveAt, doneAt, settleUntil;
    int waiting, sentX, sentY, toX, toY, stairsTries, onExitX, onExitY;
    int tx, ty, targetExit;
    unsigned moves, fails_n;
    int known, mapSeq, lastSeq;        /* lastSeq: map sequence number on which depth lastDepth was first seen, 0 not yet */
    int exitSeenX, exitSeenY;
    int doneLogged;
    int prevCore;
    double histAt;
} CogVRAuto;

static void
CogVRAuto_Init(void)
{
    const char *v;
    CogVR_InitEnv();   /* first: getenv_unsafe returns one static buffer */
    v = SDL12COMPAT_getenv_unsafe("COGVR_AUTOPILOT");
    CogVRAuto.init = 1;
    CogVRAuto.on = v && (SDL20_atoi(v) != 0);
    v = SDL12COMPAT_getenv_unsafe("COGVR_AUTOPILOT_DEPTH");
    CogVRAuto.lastDepth = (v && *v) ? SDL20_atoi(v) : -10;
    v = SDL12COMPAT_getenv_unsafe("COGVR_AUTOPILOT_MS");
    CogVRAuto.cadenceMs = (v && *v) ? SDL20_atoi(v) : 150;
    if (CogVRAuto.cadenceMs < 40) { CogVRAuto.cadenceMs = 40; }
    CogVRAuto.prevCore = -1;
    CogVRAuto.tx = CogVRAuto.ty = -1;
    CogVRAuto.exitSeenX = -1;
    if (CogVRAuto.on) {
        CogVR_Log("COGVR autopilot: on, until the map after depth %d, a step every %d ms", CogVRAuto.lastDepth, CogVRAuto.cadenceMs);
    }
}

static int
CogVRAuto_Wanted(void)
{
    if (!CogVRAuto.init) { CogVRAuto_Init(); }
    return CogVRAuto.on;
}

/* EventFilter20to12: while the autopilot drives, real keyboard/mouse input of the game window is dropped, so a desktop
   keystroke that lands in the window (focus) cannot change the run; the autopilot's own keys go straight to the 1.2 queue */
static int
CogVR_RealInputBlocked(const SDL_Event *event20)
{
    switch (event20->type) {
    case SDL_KEYDOWN: case SDL_KEYUP: case SDL_TEXTINPUT: case SDL_TEXTEDITING:
    case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
        return CogVRAuto_Wanted() && !CogVRAuto.done;
    default:
        return 0;
    }
}

static void
CogVRAuto_Reset(int W, int H)
{
    const size_t n = (size_t) W * (size_t) H;
    if ((W != CogVRAuto.W) || (H != CogVRAuto.H) || !CogVRAuto.cls) {
        SDL20_free(CogVRAuto.cls); SDL20_free(CogVRAuto.glyph); SDL20_free(CogVRAuto.visited); SDL20_free(CogVRAuto.fails);
        SDL20_free(CogVRAuto.blockUntil); SDL20_free(CogVRAuto.dist); SDL20_free(CogVRAuto.queue);
        CogVRAuto.cls = (Uint8 *) SDL20_malloc(n);
        CogVRAuto.glyph = (Uint16 *) SDL20_malloc(n * 2);
        CogVRAuto.visited = (Uint8 *) SDL20_malloc(n);
        CogVRAuto.fails = (Uint8 *) SDL20_malloc(n);
        CogVRAuto.blockUntil = (double *) SDL20_malloc(n * sizeof (double));
        CogVRAuto.dist = (int *) SDL20_malloc(n * sizeof (int));
        CogVRAuto.queue = (int *) SDL20_malloc(n * sizeof (int));
        if (!CogVRAuto.cls || !CogVRAuto.glyph || !CogVRAuto.visited || !CogVRAuto.fails || !CogVRAuto.blockUntil || !CogVRAuto.dist || !CogVRAuto.queue) {
            CogVR_Log("COGVR autopilot: out of memory: off");
            CogVRAuto.on = 0;
            return;
        }
        CogVRAuto.W = W;
        CogVRAuto.H = H;
    }
    SDL20_memset(CogVRAuto.cls, 0, n);
    SDL20_memset(CogVRAuto.glyph, 0, n * 2);
    SDL20_memset(CogVRAuto.visited, 0, n);
    SDL20_memset(CogVRAuto.fails, 0, n);
    SDL20_memset(CogVRAuto.blockUntil, 0, n * sizeof (double));
    CogVRAuto.tx = CogVRAuto.ty = -1;
    CogVRAuto.exitSeenX = CogVRAuto.exitSeenY = -1;
    CogVRAuto.waiting = 0;
    CogVRAuto.stairsTries = 0;
    CogVRAuto.mapSeq++;
}

static int
CogVRAuto_Classify(int ch, int flags)
{
    if (!(flags & COGVR_KNOWN)) { return COGVR_AP_UNKNOWN; }
    if (ch == '>') { return COGVR_AP_EXIT; }
    if ((ch == '#') || ((ch < 256) && CogVRAuto.learned[ch])) { return COGVR_AP_WALL; }
    /* robots are letters; only a visible one is there now (remembered cells show terrain and items) */
    if ((flags & COGVR_VISIBLE) && (((ch >= 'A') && (ch <= 'Z')) || ((ch >= 'a') && (ch <= 'z')))) { return COGVR_AP_ROBOT; }
    return COGVR_AP_FLOOR;
}

/* COGVR_End: fold the snapshot into the planner's view */
static void
CogVRAuto_Frame(const COGVR_Header *h, const COGVR_Cell *cells)
{
    int i, n, known = 0;
    const int jump = CogVRAuto.havePlayer && (h->playerX >= 0) &&
                     ((h->playerX - CogVRAuto.px > 3) || (CogVRAuto.px - h->playerX > 3) || (h->playerY - CogVRAuto.py > 3) || (CogVRAuto.py - h->playerY > 3));
    if (!CogVRAuto.on) { return; }
    n = h->mapW * h->mapH;
    for (i = 0; i < n; i++) { known += (cells[i].flags & COGVR_KNOWN) != 0; }
    /* a new map: other dims, a teleport, or most of the known map gone */
    if ((h->mapW != CogVRAuto.W) || (h->mapH != CogVRAuto.H) || jump || (CogVRAuto.known > 400 && known * 3 < CogVRAuto.known)) {
        if (CogVRAuto.W) {
            CogVR_Log("COGVR autopilot: new map %dx%d (player %d,%d, known %d -> %d)", h->mapW, h->mapH, h->playerX, h->playerY, CogVRAuto.known, known);
        }
        CogVRAuto_Reset(h->mapW, h->mapH);
        if (!CogVRAuto.on) { return; }
        CogVRAuto.settleUntil = CogGPU_Now() + 1500.0;
    }
    CogVRAuto.known = known;
    for (i = 0; i < n; i++) {
        const COGVR_Cell *c = &cells[i];
        if (c->flags & COGVR_PLAYER) {
            if (!CogVRAuto.glyph[i]) { CogVRAuto.cls[i] = COGVR_AP_FLOOR; }
            continue;                        /* keep what was seen under Cogmind */
        }
        CogVRAuto.cls[i] = (Uint8) CogVRAuto_Classify(c->ch, c->flags);
        if (c->flags & COGVR_KNOWN) { CogVRAuto.glyph[i] = c->ch ? c->ch : ' '; }
    }
    CogVRAuto.havePlayer = h->playerX >= 0 && h->playerY >= 0 && h->playerX < h->mapW && h->playerY < h->mapH;
    if (CogVRAuto.havePlayer) {
        CogVRAuto.px = h->playerX;
        CogVRAuto.py = h->playerY;
        CogVRAuto.visited[h->playerX * h->mapH + h->playerY] = 1;
    }
}

static void
CogVRAuto_Histogram(void)
{
    int count[256], i, k, n = CogVRAuto.W * CogVRAuto.H;
    char line[400];
    int len = 0;
    SDL20_zero(count);
    for (i = 0; i < n; i++) {
        if (CogVRAuto.glyph[i] && CogVRAuto.glyph[i] < 256) { count[CogVRAuto.glyph[i]]++; }
    }
    for (k = 0; k < 24 && len < (int) sizeof (line) - 20; k++) {
        int best = -1;
        for (i = 0; i < 256; i++) { if (count[i] && (best < 0 || count[i] > count[best])) { best = i; } }
        if (best < 0) { break; }
        len += SDL20_snprintf(line + len, sizeof (line) - (size_t) len, " %d'%c'x%d", best, (best >= 32 && best < 127) ? best : '?', count[best]);
        count[best] = 0;
    }
    CogVR_Log("COGVR autopilot: glyphs%s", line);
}

static void
CogVRAuto_Fail(double now)
{
    const int i = CogVRAuto.toX * CogVRAuto.H + CogVRAuto.toY;
    const int g = CogVRAuto.glyph[i];
    CogVRAuto.fails_n++;
    if (CogVRAuto.fails[i] < 255) { CogVRAuto.fails[i]++; }
    CogVRAuto.blockUntil[i] = now + ((CogVRAuto.cls[i] == COGVR_AP_ROBOT) ? 3000.0 : 4000.0 * ((CogVRAuto.fails[i] < 5) ? CogVRAuto.fails[i] : 5));   /* never for good: a robot may have moved on */
    CogVR_Log("COGVR autopilot: move %d,%d -> %d,%d failed (glyph %d, cls %d, fail %d)", CogVRAuto.sentX, CogVRAuto.sentY,
              CogVRAuto.toX, CogVRAuto.toY, g, CogVRAuto.cls[i], CogVRAuto.fails[i]);
    /* a glyph that stops moves on two different cells is terrain we cannot walk (machines, closed walls) */
    if ((g > ' ') && (g < 256) && (g != '>') && (CogVRAuto.cls[i] == COGVR_AP_FLOOR) && !CogVRAuto.learned[g]) {
        if (CogVRAuto.learnCell[g] && (CogVRAuto.learnCell[g] != (Uint16) (i + 1))) {
            CogVRAuto.learned[g] = 1;
            CogVR_Log("COGVR autopilot: glyph %d '%c' is impassable", g, (g < 127) ? g : '?');
        } else {
            CogVRAuto.learnCell[g] = (Uint16) (i + 1);
        }
    }
}

/* BFS from the player over walkable known cells: the nearest exit, else the nearest unvisited cell next to the unknown.
   Visible robots are walls unless `bump` (boxed in by them: walk into one and let the game decide what happens).
   Returns the first step (dx,dy) through *sx,*sy, or 0 when there is nowhere to go. */
static int
CogVRAuto_Plan(double now, int bump, int *sx, int *sy)
{
    static const int ddx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, ddy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
    const int W = CogVRAuto.W, H = CogVRAuto.H, start = CogVRAuto.px * H + CogVRAuto.py;
    int head = 0, tail = 0, i, best = -1, bestExit = -1, d;
    for (i = 0; i < W * H; i++) { CogVRAuto.dist[i] = -1; }
    CogVRAuto.dist[start] = 0;
    CogVRAuto.queue[tail++] = start;
    while (head < tail) {
        const int cur = CogVRAuto.queue[head++], x = cur / H, y = cur % H;
        if (cur != start) {
            if (CogVRAuto.cls[cur] == COGVR_AP_EXIT) {
                if (bestExit < 0) { bestExit = cur; }
            } else if ((best < 0) && !CogVRAuto.visited[cur]) {
                for (d = 0; d < 8; d++) {
                    const int nx = x + ddx[d], ny = y + ddy[d];
                    if ((nx >= 0) && (ny >= 0) && (nx < W) && (ny < H) && (CogVRAuto.cls[nx * H + ny] == COGVR_AP_UNKNOWN)) {
                        best = cur;
                        break;
                    }
                }
            }
        }
        if (bestExit >= 0) { break; }
        for (d = 0; d < 8; d++) {
            const int nx = x + ddx[d], ny = y + ddy[d];
            int ni, c;
            if ((nx < 0) || (ny < 0) || (nx >= W) || (ny >= H)) { continue; }
            ni = nx * H + ny;
            c = CogVRAuto.cls[ni];
            if ((CogVRAuto.dist[ni] >= 0) || ((c != COGVR_AP_FLOOR) && (c != COGVR_AP_EXIT) && !(bump && (c == COGVR_AP_ROBOT))) || (CogVRAuto.blockUntil[ni] > now)) { continue; }
            CogVRAuto.dist[ni] = CogVRAuto.dist[cur] + 1;
            CogVRAuto.queue[tail++] = ni;
        }
    }
    if (bestExit >= 0) {
        best = bestExit;
        if (CogVRAuto.exitSeenX < 0) {
            CogVRAuto.exitSeenX = best / H; CogVRAuto.exitSeenY = best % H;
            CogVRRec_Event("autopilot: exit seen at %d,%d (%d steps)", CogVRAuto.exitSeenX, CogVRAuto.exitSeenY, CogVRAuto.dist[best]);
        }
    }
    CogVRAuto.targetExit = bestExit >= 0;
    if (best < 0) { return 0; }
    CogVRAuto.tx = best / H; CogVRAuto.ty = best % H;
    /* walk back to the first step */
    while (CogVRAuto.dist[best] > 1) {
        const int x = best / H, y = best % H;
        int found = -1;
        for (d = 0; d < 8 && found < 0; d++) {
            const int nx = x + ddx[d], ny = y + ddy[d];
            if ((nx >= 0) && (ny >= 0) && (nx < W) && (ny < H) && (CogVRAuto.dist[nx * H + ny] == CogVRAuto.dist[best] - 1)) {
                found = nx * H + ny;
            }
        }
        if (found < 0) { return 0; }
        best = found;
    }
    *sx = best / H - CogVRAuto.px;
    *sy = best % H - CogVRAuto.py;
    return 1;
}

/* PollEnter, before the game drains its queue: at most one key per call */
static void
CogVRAuto_Poll(void)
{
    static const SDL12Key moveKey[3][3] = { { SDLK12_KP7, SDLK12_KP8, SDLK12_KP9 }, { SDLK12_KP4, SDLK12_UNKNOWN, SDLK12_KP6 },
                                            { SDLK12_KP1, SDLK12_KP2, SDLK12_KP3 } };
    const double now = CogGPU_Now();
    int sx, sy, here;
    const int ap = CogVRAuto_Wanted(), rec = CogVRRec_Wanted();
    if (!ap && !rec) { return; }
    CogVRScr_Parse(now);
    if (CogVRRec.on) { CogVRRec_Notes(); }
    if (!CogVRAuto.on || !CogVRAuto.W || !CogVRAuto.havePlayer || !CogVRScr.haveLoc) { return; }
    if (!CogVRAuto.started) {
        CogVRAuto.started = 1;
        CogVRAuto.t0 = now;
        CogVRAuto.settleUntil = now + 2000.0;
        CogVRRec_Event("autopilot: start at %d,%d depth %d %s", CogVRAuto.px, CogVRAuto.py, CogVRScr.depth, CogVRScr.locName);
    }
    if ((CogVRScr.core >= 0) && (CogVRScr.core != CogVRAuto.prevCore)) {
        if ((CogVRAuto.prevCore >= 0) && (CogVRScr.core < CogVRAuto.prevCore)) {
            CogVRRec_Event("core %d/%d", CogVRScr.core, CogVRScr.coreMax);
        }
        CogVRAuto.prevCore = CogVRScr.core;
    }
    if (CogVRAuto.done) {
        if (!CogVRAuto.doneLogged && (now - CogVRAuto.doneAt > 4000.0)) {
            CogVRAuto.doneLogged = 1;
            CogVRRec_Flush();
            CogVR_Log("COGVR autopilot: done (%u moves, %u failed, %.0f s)", CogVRAuto.moves, CogVRAuto.fails_n, (now - CogVRAuto.t0) / 1000.0);
        }
        return;
    }
    if (now - CogVRAuto.histAt > 20000.0) {
        CogVRAuto.histAt = now;
        CogVRAuto_Histogram();
        CogVR_Log("COGVR autopilot: at %d,%d depth %d %s core %d turn %d moves %u target %d,%d%s", CogVRAuto.px, CogVRAuto.py, CogVRScr.depth,
                  CogVRScr.locName, CogVRScr.core, CogVRScr.turn, CogVRAuto.moves, CogVRAuto.tx, CogVRAuto.ty, CogVRAuto.targetExit ? " (exit)" : "");
    }
    if (now < CogVRAuto.settleUntil) { return; }
    /* past the settle time of a map, the HUD shows its depth: remember the map on which lastDepth was entered */
    if ((CogVRScr.depth == CogVRAuto.lastDepth) && !CogVRAuto.lastSeq) {
        CogVRAuto.lastSeq = CogVRAuto.mapSeq;
    }
    /* done one map past lastDepth: a higher depth, or another map at the same depth (a branch) */
    if ((CogVRScr.depth > CogVRAuto.lastDepth) || (CogVRAuto.lastSeq && (CogVRAuto.mapSeq != CogVRAuto.lastSeq))) {
        CogVRAuto.done = 1;
        CogVRAuto.doneAt = now;
        CogVRRec_Event("autopilot: exit reached, now depth %d %s (%u moves)", CogVRScr.depth, CogVRScr.locName, CogVRAuto.moves);
        return;
    }
    if (CogVRAuto.waiting) {
        if ((CogVRAuto.px != CogVRAuto.sentX) || (CogVRAuto.py != CogVRAuto.sentY)) {
            CogVRAuto.waiting = 0;
            CogVRAuto.moves++;
            CogVRAuto.lastMoveAt = now;
        } else if (now - CogVRAuto.sentAt > 900.0) {
            CogVRAuto.waiting = 0;
            CogVRAuto_Fail(now);
        } else {
            return;
        }
    }
    if (now < CogVRAuto.nextAct) { return; }
    here = CogVRAuto.px * CogVRAuto.H + CogVRAuto.py;
    if (CogVRAuto.glyph[here] == '>') {
        /* on an exit: take it (the deck's STAIR key, Beta 17.1 manual binding) */
        if (CogVRAuto.stairsTries++ == 0) {
            CogVRRec_Event("autopilot: on exit %d,%d depth %d, taking it", CogVRAuto.px, CogVRAuto.py, CogVRScr.depth);
        }
        CogVRBridge_KeyEx(SDLK12_PERIOD, KMOD12_LSHIFT, '>');
        CogVRAuto.nextAct = now + 1500.0;
        if (CogVRAuto.stairsTries > 4) {
            CogVRAuto.blockUntil[here] = now + 30000.0;   /* not an exit we can take: move on */
            CogVRAuto.stairsTries = 0;
        }
        return;
    }
    if (!CogVRAuto_Plan(now, 0, &sx, &sy) && !CogVRAuto_Plan(now, 1, &sx, &sy)) {
        int i, any = 0;
        for (i = 0; i < CogVRAuto.W * CogVRAuto.H; i++) {
            if (CogVRAuto.blockUntil[i] > now) { CogVRAuto.blockUntil[i] = 0; any = 1; }
        }
        if (!any) {
            /* everything reachable is explored: revisit (the view may have missed cells) */
            SDL20_memset(CogVRAuto.visited, 0, (size_t) CogVRAuto.W * (size_t) CogVRAuto.H);
            CogVR_Log("COGVR autopilot: nothing left to explore at %d,%d; waiting a turn", CogVRAuto.px, CogVRAuto.py);
            CogVRBridge_Key(SDLK12_PERIOD);
        }
        CogVRAuto.nextAct = now + 500.0;
        return;
    }
    CogVRAuto.sentX = CogVRAuto.px; CogVRAuto.sentY = CogVRAuto.py;
    CogVRAuto.toX = CogVRAuto.px + sx; CogVRAuto.toY = CogVRAuto.py + sy;
    CogVRAuto.waiting = 1;
    CogVRAuto.sentAt = now;
    CogVRAuto.nextAct = now + CogVRAuto.cadenceMs;
    CogVRBridge_Key(moveKey[sy + 1][sx + 1]);
}
