/*
 * cogmind_vrdeck.inc.c -- the deck (docs/VR_SUIT.md, Deck): physical controls on a tilted panel in front of the floating
 * chair, like a DJ desk. Render thread. Included by cogmind_vr.inc.c before CogVR_VRInput.
 *
 * Grip is contextual (Tabletop Simulator style): squeeze at a deck control and the deck takes it -- buttons are picked up and
 * carried (drop one on the near edge to unplug it into the tray), the tempo fader knob is moved; squeeze in open air and the
 * hand grabs the world as before. Trigger at a button presses it. While a hand is at the deck its trigger never reaches the
 * game as a pointer click.
 *
 * Tempo: the fader (and with COGVR_GRIP=throttle the right squeeze in open air) sets how often the auto-input fires, locked
 * to the music's beat grid when the beat tracker is confident. Each tick acts on the cell under the right-hand laser through
 * the bridge (cogmind_vrbridge.inc.c): a robot is clicked (retail decides: aim, fire, attack), another known cell gets one
 * step toward it. Tempo starts at zero every session.
 */
enum { COGVR_DK_KEY = 0, COGVR_DK_STOP, COGVR_DK_LAYOUT, COGVR_DK_TEMPO, COGVR_DK_MANUAL, COGVR_DK_PAGE, COGVR_DK_MUSIC };

#define COGVR_DECK_BW    0.040f         /* button half width (m) */
#define COGVR_DECK_BH    0.032f         /* button half depth along the panel */
#define COGVR_DECK_FLEN  0.24f          /* tempo fader travel */
#define COGVR_DECK_TILT  0.52f          /* panel tilt from horizontal (30 degrees), far edge up */
#define COGVR_DECK_TRAYV (-0.135f)      /* the tray row (unplugged controls) at the near edge */
#define COGVR_DECK_UNPLUG (-0.095f)     /* a control dropped nearer than this is unplugged */

typedef struct CogVRDeckItem {
    const char *id;                    /* stable name, the layout file key */
    const char *label;                 /* <= 5 characters, on the control (everyone sees it) */
    const char *note;                  /* 1-3 words floating above it: headset only, never on the scene cameras */
    int kind;
    int sym, mod, uni;                 /* COGVR_DK_KEY: the SDL 1.2 key event (Beta 17.1 manual bindings) */
    float u, v;                        /* centre on the panel (m): u right, v away from you */
    int hidden;                        /* unplugged: shown in the tray while laying out */
    double flash;                      /* last press (ms) */
} CogVRDeckItem;

static CogVRDeckItem CogVRDeckItems[] = {
    /* far row: know */
    { "status", "STAT", "status", COGVR_DK_KEY, SDLK12_s, 0, 0, -0.285f, 0.16f },
    { "range", "RANGE", "volley range", COGVR_DK_KEY, SDLK12_v, 0, 0, -0.190f, 0.16f },
    { "zoom", "ZOOM", "map zoom", COGVR_DK_KEY, SDLK12_z, 0, 0, -0.095f, 0.16f },
    { "log", "LOG", "messages", COGVR_DK_KEY, SDLK12_F4, 0, 0, 0.000f, 0.16f },
    { "allies", "ALLY", "ally list", COGVR_DK_KEY, SDLK12_F5, 0, 0, 0.095f, 0.16f },
    { "intel", "INTEL", "intel", COGVR_DK_KEY, SDLK12_F6, 0, 0, 0.190f, 0.16f },
    { "orders", "ORDER", "ally orders", COGVR_DK_KEY, SDLK12_o, 0, 0, 0.285f, 0.16f },
    /* middle row: act */
    { "get", "GET", "pick up 100", COGVR_DK_KEY, SDLK12_g, 0, 0, -0.285f, 0.06f },
    { "attach", "ATTCH", "get+attach 150", COGVR_DK_KEY, SDLK12_a, 0, 0, -0.190f, 0.06f },
    { "parts", "PARTS", "manage parts", COGVR_DK_KEY, SDLK12_p, 0, 0, -0.095f, 0.06f },
    { "inventory", "INV", "inventory", COGVR_DK_KEY, SDLK12_i, 0, 0, 0.000f, 0.06f },
    { "sort", "SORT", "sort parts", COGVR_DK_KEY, SDLK12_SEMICOLON, KMOD12_LSHIFT, ':', 0.095f, 0.06f },
    { "stairs", "STAIR", "use > here", COGVR_DK_KEY, SDLK12_PERIOD, KMOD12_LSHIFT, '>', 0.190f, 0.06f },
    { "look", "LOOK", "examine", COGVR_DK_KEY, SDLK12_x, 0, 0, 0.285f, 0.06f },
    /* near row: fight */
    { "wait", "WAIT", "wait 100", COGVR_DK_KEY, SDLK12_PERIOD, 0, 0, -0.285f, -0.04f },
    { "fire", "FIRE", "aim mode", COGVR_DK_KEY, SDLK12_f, 0, 0, -0.190f, -0.04f },
    { "escape", "ESC", "back out", COGVR_DK_KEY, SDLK12_ESCAPE, 0, 0, -0.095f, -0.04f },
    { "stop", "STOP", "auto off", COGVR_DK_STOP, 0, 0, 0, 0.000f, -0.04f },
    { "layout", "EDIT", "move panel", COGVR_DK_LAYOUT, 0, 0, 0, 0.285f, -0.04f },
    /* the tempo fader: knob travel along v, centred here */
    { "tempo", "TEMPO", "auto speed", COGVR_DK_TEMPO, 0, 0, 0, 0.400f, 0.06f },
    /* the music stand (manual): left edge */
    { "manual", "MAN", "manual stand", COGVR_DK_MANUAL, 0, 0, 0, -0.400f, 0.16f },
    { "pagenext", "PG+", "next page", COGVR_DK_PAGE, 1, 0, 0, -0.400f, 0.06f },
    { "pageprev", "PG-", "prev page", COGVR_DK_PAGE, -1, 0, 0, -0.400f, -0.04f },
    /* the music player (COGVR_MUSIC): right edge */
    { "play", "PLAY", "play/pause", COGVR_DK_MUSIC, 0, 0, 0, 0.500f, 0.16f },
    { "next", "NEXT", "next track", COGVR_DK_MUSIC, 1, 0, 0, 0.500f, 0.06f },
    { "prev", "PREV", "restart/prev", COGVR_DK_MUSIC, -1, 0, 0, 0.500f, -0.04f },
};
#define COGVR_DECK_N ((int) SDL_arraysize(CogVRDeckItems))
#define COGVR_DECK_PANEL (-2)          /* hover/held: the panel itself (layout mode) */

static struct {
    CogV3 off;                         /* panel origin offset from the default, seat frame (m) */
    float tempo;                       /* fader 0..1 */
    float squeeze;                     /* COGVR_GRIP=throttle: right squeeze in open air, 0..1 */
    int throttle;                      /* COGVR_GRIP=throttle */
    int layout;                        /* EDIT: the panel and the fader can be carried, the tray is shown */
    int hover[2], held[2];             /* item index, COGVR_DECK_PANEL, or -1 */
    float grabU[2], grabV[2], itemU[2], itemV[2], grabTempo[2];
    CogV3 grabTip[2], grabOff[2];
    int trigWas[2], claim[2], tempoStep;
    double nextTick, tickFlash;
    char path[300];                    /* COGVR_DECK: layout file */
    int logged;
    /* the music stand: the harness card, then manual.txt from the game folder (headset only) */
    int standOpen, page, pages, nlines;
    char *manual;                      /* manual.txt, wrapped: lines of at most COGVR_STAND_COLS characters */
    const char **line;
} CogVRDeck;

#define COGVR_STAND_COLS  64
#define COGVR_STAND_ROWS  22

static const char *const CogVRStandCard[] = {
    "COGMIND VR -- HARNESS (page 1, then the game manual)",
    "",
    "GRIP at a control ...... take it (grip again: drop)",
    "GRIP in open air ....... grab the world",
    "BOTH GRIPS ............. scale + turn the world",
    "TRIGGER at a button .... press it",
    "TRIGGER on the map ..... click the cell (path, use, attack)",
    "LEFT STICK ............. move Cogmind (POV / companion)",
    "RIGHT STICK ............ turn, fly up/down",
    "B tap / hold ........... POV <-> fly / re-place me",
    "LEFT A / RIGHT A ....... fire mode / wait",
    "",
    "TEMPO fader ............ auto-act on the beat:",
    "   point at a cell ..... one step toward it per tick",
    "   point at a robot .... click it (aim, then fire)",
    "STOP ................... auto off",
    "EDIT ................... carry the panel and the fader",
    "near edge (tray) ....... drop a control there to unplug it",
    "",
    "SKY: amber = hold (input paused), red = cut, green = wizard",
    "MAN: this stand    PG+ / PG-: pages    notes: only you see them",
};
#define COGVR_STAND_CARD ((int) SDL_arraysize(CogVRStandCard))

static void CogVR_QueueKey(int sym, int mod, int uni);
static void CogVR_QueueTick(void);
static void CogVRDeck_LoadManual(void);
static void CogVRDeck_StandDyn(void);

static void
CogVRDeck_Frame(CogM4 *m)
{
    const float c = CogVR.cosf_(COGVR_DECK_TILT), s = CogVR.sinf_(COGVR_DECK_TILT);
    const CogV3 x = CogV3_RotY(CogV3_Make(1.0f, 0.0f, 0.0f), CogVR.seatYaw);
    const CogV3 v = CogV3_RotY(CogV3_Make(0.0f, s, -c), CogVR.seatYaw);
    const CogV3 n = CogV3_RotY(CogV3_Make(0.0f, c, s), CogVR.seatYaw);
    const CogV3 o = CogV3_Add(CogVR.seat, CogV3_RotY(CogV3_Add(CogV3_Make(0.0f, 0.30f, -0.36f), CogVRDeck.off), CogVR.seatYaw));
    CogM4_Identity(m);
    m->m[0][0] = x.x; m->m[1][0] = x.y; m->m[2][0] = x.z;
    m->m[0][1] = v.x; m->m[1][1] = v.y; m->m[2][1] = v.z;
    m->m[0][2] = n.x; m->m[1][2] = n.y; m->m[2][2] = n.z;
    m->m[0][3] = o.x; m->m[1][3] = o.y; m->m[2][3] = o.z;
}

/* hand h's controller tip in panel coordinates: x = u, y = v, z = height above the panel */
static CogV3
CogVRDeck_Tip(int h)
{
    const struct CogVRHand *hd = &CogVR.hand[h];
    const CogV3 tip = CogV3_Add(hd->pos, CogV3_Scale(hd->fwd, 0.06f));
    CogM4 m;
    CogV3 d;
    CogVRDeck_Frame(&m);
    d = CogV3_Make(tip.x - m.m[0][3], tip.y - m.m[1][3], tip.z - m.m[2][3]);
    return CogV3_Make(d.x * m.m[0][0] + d.y * m.m[1][0] + d.z * m.m[2][0], d.x * m.m[0][1] + d.y * m.m[1][1] + d.z * m.m[2][1],
                      d.x * m.m[0][2] + d.y * m.m[1][2] + d.z * m.m[2][2]);
}

/* where item i is drawn and hit now; 0 when it is not on the panel */
static int
CogVRDeck_Pos(int i, float *u, float *v)
{
    const CogVRDeckItem *it = &CogVRDeckItems[i];
    if (!it->hidden) {
        *u = it->u;
        *v = it->v;
        return 1;
    }
    if (CogVRDeck.layout || (CogVRDeck.held[0] >= 0) || (CogVRDeck.held[1] >= 0)) {
        int k, slot = 0;
        for (k = 0; k < i; k++) { slot += CogVRDeckItems[k].hidden; }
        *u = -0.30f + 0.09f * (float) slot;
        *v = COGVR_DECK_TRAYV;
        return 1;
    }
    return 0;
}

static void
CogVRDeck_Hover(int h)
{
    const CogV3 t = CogVRDeck_Tip(h);
    float best = 1e9f;
    int i;
    CogVRDeck.hover[h] = -1;
    if (!CogVR.hand[h].valid || (t.z < -0.03f) || (t.z > 0.08f)) {
        return;
    }
    for (i = 0; i < COGVR_DECK_N; i++) {
        const CogVRDeckItem *it = &CogVRDeckItems[i];
        const float hw = (it->kind == COGVR_DK_TEMPO) ? 0.030f : COGVR_DECK_BW;
        const float hd = (it->kind == COGVR_DK_TEMPO) ? COGVR_DECK_FLEN * 0.5f + 0.02f : COGVR_DECK_BH;
        float u, v, du, dv;
        if (!CogVRDeck_Pos(i, &u, &v)) {
            continue;
        }
        du = t.x - u;
        dv = t.y - v;
        if ((du > -hw - 0.01f) && (du < hw + 0.01f) && (dv > -hd - 0.01f) && (dv < hd + 0.01f) && (du * du + dv * dv < best)) {
            best = du * du + dv * dv;
            CogVRDeck.hover[h] = i;
        }
    }
    if ((CogVRDeck.hover[h] < 0) && CogVRDeck.layout && (t.x > -0.48f) && (t.x < 0.48f) && (t.y > -0.17f) && (t.y < 0.22f)) {
        CogVRDeck.hover[h] = COGVR_DECK_PANEL;
    }
}

static void
CogVRDeck_Save(void)
{
    char buf[2048];
    int n, i;
    HANDLE f;
    DWORD wrote;
    if (!CogVRDeck.path[0]) {
        return;
    }
    n = SDL20_snprintf(buf, sizeof (buf), "panel %.4f %.4f %.4f\r\n", CogVRDeck.off.x, CogVRDeck.off.y, CogVRDeck.off.z);
    for (i = 0; (i < COGVR_DECK_N) && (n > 0) && (n < (int) sizeof (buf)); i++) {
        const CogVRDeckItem *it = &CogVRDeckItems[i];
        n += SDL20_snprintf(buf + n, sizeof (buf) - (size_t) n, "item %s %.4f %.4f %d\r\n", it->id, it->u, it->v, it->hidden);
    }
    if ((n <= 0) || (n >= (int) sizeof (buf))) {
        return;
    }
    f = CreateFileA(CogVRDeck.path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        CogVR_Log("COGVR deck: cannot write layout %s (error %lu)", CogVRDeck.path, (unsigned long) GetLastError());
        return;
    }
    WriteFile(f, buf, (DWORD) n, &wrote, NULL);
    CloseHandle(f);
}

static void
CogVRDeck_Init(void)
{
    const char *v = SDL12COMPAT_getenv_unsafe("COGVR_GRIP");
    CogVRDeck.hover[0] = CogVRDeck.hover[1] = CogVRDeck.held[0] = CogVRDeck.held[1] = -1;
    CogVRDeck.throttle = v && !SDL20_strcasecmp(v, "throttle");
    if (v && *v && !CogVRDeck.throttle && SDL20_strcasecmp(v, "grab")) {
        CogVR_Log("COGVR deck: unknown COGVR_GRIP=%s (want grab or throttle): grab", v);
    }
    v = SDL12COMPAT_getenv_unsafe("COGVR_DECK");
    if (v && *v) {
        HANDLE f;
        char buf[2048], *line;
        DWORD n = 0;
        SDL20_strlcpy(CogVRDeck.path, v, sizeof (CogVRDeck.path));
        f = CreateFileA(CogVRDeck.path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            if (!ReadFile(f, buf, sizeof (buf) - 1, &n, NULL)) { n = 0; }
            CloseHandle(f);
        }
        buf[n] = 0;
        for (line = buf; line && *line; ) {
            char *next = SDL20_strchr(line, '\n'), *p;
            if (next) { *next++ = 0; }
            if (!SDL20_strncmp(line, "panel ", 6)) {
                CogVRDeck.off.x = (float) SDL20_strtod(line + 6, &p);
                CogVRDeck.off.y = (float) SDL20_strtod(p, &p);
                CogVRDeck.off.z = (float) SDL20_strtod(p, &p);
            } else if (!SDL20_strncmp(line, "item ", 5)) {
                char *id = line + 5, *sp = SDL20_strchr(id, ' ');
                int i;
                if (sp) {
                    *sp = 0;
                    for (i = 0; i < COGVR_DECK_N; i++) {
                        if (!SDL20_strcmp(CogVRDeckItems[i].id, id)) {
                            CogVRDeckItems[i].u = CogVR_Clamp((float) SDL20_strtod(sp + 1, &p), -0.6f, 0.6f);
                            CogVRDeckItems[i].v = CogVR_Clamp((float) SDL20_strtod(p, &p), -0.2f, 0.4f);
                            CogVRDeckItems[i].hidden = SDL20_atoi(p) != 0;
                        }
                    }
                }
            }
            line = next;
        }
        CogVR_Log("COGVR deck: layout %s (%s)", CogVRDeck.path, n ? "loaded" : "new");
    }
    CogVR_Log("COGVR deck: %d controls, grip %s", COGVR_DECK_N, CogVRDeck.throttle ? "throttle (right squeeze in open air = tempo)" : "grab");
    CogVRDeck_LoadManual();
}

/* append a manual line start (growing the table); 0 when out of memory */
static int
CogVRDeck_PushLine(char *s, int *cap)
{
    if (CogVRDeck.nlines >= *cap) {
        const int want = *cap ? *cap * 2 : 1024;
        const char **grown = (const char **) SDL20_realloc((void *) CogVRDeck.line, (size_t) want * sizeof (char *));
        if (!grown) {
            return 0;
        }
        CogVRDeck.line = grown;
        *cap = want;
    }
    CogVRDeck.line[CogVRDeck.nlines++] = s;
    return 1;
}

/* manual.txt next to the game exe, wrapped to the stand's width; page 1 of the stand is the harness card */
static void
CogVRDeck_LoadManual(void)
{
    char path[MAX_PATH], *slash, *src = NULL, *out;
    HANDLE f;
    DWORD size = 0, n = 0;
    int cap = 0, i, col;
    CogVRDeck.pages = 1;
    if (!GetModuleFileNameA(NULL, path, sizeof (path)) || !(slash = SDL20_strrchr(path, '\\')) ||
        (size_t) (slash - path) + sizeof ("\\manual.txt") > sizeof (path)) {
        return;
    }
    SDL20_strlcpy(slash, "\\manual.txt", sizeof (path) - (size_t) (slash - path));
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        CogVR_Log("COGVR deck: no manual.txt next to the game: the stand shows the harness card only");
        return;
    }
    size = GetFileSize(f, NULL);
    if ((size > 0) && (size < 4u * 1024u * 1024u)) {
        src = (char *) SDL20_malloc(size + 1);
    }
    if (src && !ReadFile(f, src, size, &n, NULL)) {
        n = 0;
    }
    CloseHandle(f);
    if (!src || !n) {
        SDL20_free(src);
        return;
    }
    /* wrapping adds at most one terminator per COGVR_STAND_COLS characters, plus one per source line */
    out = (char *) SDL20_malloc((size_t) n * 2 + 2);
    if (!out) {
        SDL20_free(src);
        return;
    }
    CogVRDeck.manual = out;
    for (i = 0, col = 0; i < (int) n; i++) {
        const char c = src[i];
        if (c == '\r') {
            continue;
        }
        if (col == 0) {
            if (!CogVRDeck_PushLine(out, &cap)) { break; }
        }
        if (c == '\n') {
            *out++ = 0;
            col = 0;
            continue;
        }
        *out++ = ((c < 32) || (c > 126)) ? ' ' : c;
        if (++col == COGVR_STAND_COLS) {   /* wrap at the last space of the second half, else hard */
            char *start = (char *) CogVRDeck.line[CogVRDeck.nlines - 1], *sp = out - 1;
            while ((sp > start + COGVR_STAND_COLS / 2) && (*sp != ' ')) { sp--; }
            if ((*sp == ' ') && (sp > start + COGVR_STAND_COLS / 2)) {
                *sp = 0;
                if (!CogVRDeck_PushLine(sp + 1, &cap)) { break; }
                col = (int) (out - (sp + 1));
            } else {
                *out++ = 0;
                col = 0;
            }
        }
    }
    *out = 0;
    SDL20_free(src);
    CogVRDeck.pages = 1 + (CogVRDeck.nlines + COGVR_STAND_ROWS - 1) / COGVR_STAND_ROWS;
    CogVR_Log("COGVR deck: manual.txt %lu bytes, %d lines, %d stand pages", (unsigned long) n, CogVRDeck.nlines, CogVRDeck.pages);
}

static void
CogVRDeck_Drop(int h)
{
    const int i = CogVRDeck.held[h];
    CogVRDeck.held[h] = -1;
    if (i >= 0 && CogVRDeckItems[i].kind != COGVR_DK_TEMPO) {
        CogVRDeckItem *it = &CogVRDeckItems[i];
        it->hidden = it->v < COGVR_DECK_UNPLUG;
        CogVR_Log("COGVR deck: %s %s at %.3f,%.3f", it->hidden ? "unplugged" : "placed", it->id, it->u, it->v);
        CogVRDeck_Save();
    } else if ((i >= 0) && CogVRDeck.layout) {
        CogVRDeck_Save();
    } else if (i == COGVR_DECK_PANEL) {
        CogVR_Log("COGVR deck: panel moved to offset %.3f,%.3f,%.3f", CogVRDeck.off.x, CogVRDeck.off.y, CogVRDeck.off.z);
        CogVRDeck_Save();
    } else if (i >= 0) {
        CogVR_Log("COGVR deck: tempo %.2f", CogVRDeck.tempo);
    }
}

/*
 * Grip on hand h this frame (press/release edges). Returns 1 while the deck has the hand (that grip does not grab the world).
 * Sticky grip: a press takes, the next press drops; otherwise holding keeps it.
 */
static int
CogVRDeck_Grip(int h, int press, int release)
{
    int i;
    if (!CogVR.hand[h].valid) {
        if (CogVRDeck.held[h] != -1) { CogVRDeck_Drop(h); }
        return 0;
    }
    CogVRDeck_Hover(h);
    if (CogVRDeck.held[h] != -1) {
        if ((CogVR.stickyGrip && press) || (!CogVR.stickyGrip && release)) {
            CogVRDeck_Drop(h);
            CogVR_Haptic(h);
        }
        return 1;
    }
    i = CogVRDeck.hover[h];
    if (!press || (i == -1)) {
        return 0;
    }
    {
        const CogV3 t = CogVRDeck_Tip(h);
        CogVRDeck.held[h] = i;
        CogVRDeck.grabU[h] = t.x;
        CogVRDeck.grabV[h] = t.y;
        CogVRDeck.grabTip[h] = CogVR.hand[h].pos;
        CogVRDeck.grabOff[h] = CogVRDeck.off;
        CogVRDeck.grabTempo[h] = CogVRDeck.tempo;
        if (i >= 0) {
            CogVRDeck.itemU[h] = CogVRDeckItems[i].u;
            CogVRDeck.itemV[h] = CogVRDeckItems[i].v;
            if (CogVRDeckItems[i].hidden) {   /* taken from the tray: carried from where it is shown */
                CogVRDeck_Pos(i, &CogVRDeck.itemU[h], &CogVRDeck.itemV[h]);
            }
        }
        CogVR_Haptic(h);
        CogVR_Log("COGVR deck: %s hand takes %s", h ? "right" : "left", (i >= 0) ? CogVRDeckItems[i].id : "the panel");
    }
    return 1;
}

static void
CogVRDeck_Press(int h, int i)
{
    CogVRDeckItem *it = &CogVRDeckItems[i];
    it->flash = CogGPU_Now();
    CogVR_Haptic(h);
    switch (it->kind) {
        case COGVR_DK_KEY:
            CogVR_QueueKey(it->sym, it->mod, it->uni);
            CogVR_Log("COGVR deck: press %s", it->id);
            break;
        case COGVR_DK_STOP:
            CogVRDeck.tempo = 0.0f;
            CogVR_Log("COGVR deck: stop (tempo 0)");
            break;
        case COGVR_DK_LAYOUT:
            CogVRDeck.layout = !CogVRDeck.layout;
            CogVR_Log("COGVR deck: edit %s", CogVRDeck.layout ? "on (grip carries the panel and the fader too)" : "off");
            break;
        case COGVR_DK_MANUAL:
            CogVRDeck.standOpen = !CogVRDeck.standOpen;
            CogVR_Log("COGVR deck: manual stand %s (page %d/%d)", CogVRDeck.standOpen ? "open" : "closed", CogVRDeck.page + 1, CogVRDeck.pages);
            break;
        case COGVR_DK_MUSIC:
            CogVRMusic_Command(it->sym);
            break;
        case COGVR_DK_PAGE:
            CogVRDeck.standOpen = 1;
            CogVRDeck.page = SDL_max(0, SDL_min(CogVRDeck.pages - 1, CogVRDeck.page + it->sym));
            CogVR_Log("COGVR deck: manual page %d/%d", CogVRDeck.page + 1, CogVRDeck.pages);
            break;
        default: {
            const CogV3 t = CogVRDeck_Tip(h);   /* trigger on the fader: jump the knob there */
            CogVRDeck.tempo = CogVR_Clamp((t.y - (it->v - COGVR_DECK_FLEN * 0.5f)) / COGVR_DECK_FLEN, 0.0f, 1.0f);
            CogVR_Log("COGVR deck: tempo %.2f", CogVRDeck.tempo);
            break;
        }
    }
}

/* per frame, after the pads are read: carried controls follow the hands, triggers press, the squeeze throttles */
static void
CogVRDeck_Update(const struct CogVRPad *p)
{
    int h;
    for (h = 0; h < 2; h++) {
        const struct CogVRHand *hd = &CogVR.hand[h];
        const int trig = hd->valid && ((hd->st.ulButtonPressed & COGVR_BTN(EVRButtonId_k_EButton_SteamVR_Trigger)) != 0);
        const int i = CogVRDeck.held[h];
        if (hd->valid && (i != -1)) {
            const CogV3 t = CogVRDeck_Tip(h);
            if (i == COGVR_DECK_PANEL) {
                CogVRDeck.off = CogV3_Add(CogVRDeck.grabOff[h], CogV3_RotY(CogV3_Sub(hd->pos, CogVRDeck.grabTip[h]), -CogVR.seatYaw));
            } else if ((CogVRDeckItems[i].kind == COGVR_DK_TEMPO) && !CogVRDeck.layout) {
                const float before = CogVRDeck.tempo;
                CogVRDeck.tempo = CogVR_Clamp(CogVRDeck.grabTempo[h] + (t.y - CogVRDeck.grabV[h]) / COGVR_DECK_FLEN, 0.0f, 1.0f);
                if ((int) (before * 4.0f) != (int) (CogVRDeck.tempo * 4.0f)) {
                    CogVR_Haptic(h);   /* a detent every quarter of the travel */
                }
            } else {
                CogVRDeckItems[i].u = CogVR_Clamp(CogVRDeck.itemU[h] + t.x - CogVRDeck.grabU[h], -0.6f, 0.6f);
                CogVRDeckItems[i].v = CogVR_Clamp(CogVRDeck.itemV[h] + t.y - CogVRDeck.grabV[h], -0.2f, 0.4f);
                CogVRDeckItems[i].hidden = 0;
            }
        } else if (i == -1) {
            CogVRDeck_Hover(h);
        }
        CogVRDeck.claim[h] = (CogVRDeck.hover[h] != -1) || (i != -1);
        if (trig && !CogVRDeck.trigWas[h] && (CogVRDeck.hover[h] >= 0) && (i == -1)) {
            CogVRDeck_Press(h, CogVRDeck.hover[h]);
        }
        CogVRDeck.trigWas[h] = trig;
    }
    if (CogVRDeck.throttle) {
        const float s = (CogVRDeck.claim[1] || !CogVR.hand[1].valid) ? 0.0f : p->squeeze[1];
        CogVRDeck.squeeze = CogVR_Clamp((s - 0.1f) / 0.9f, 0.0f, 1.0f);
    }
}

/* desktop preview: [ ] tempo by a quarter; M play/pause, . next track, , restart/previous */
static void
CogVRDeck_DeskKeys(void)
{
    static int was[5];
    const int down[5] = { CogVR_KeyDown(VK_OEM_4), CogVR_KeyDown(VK_OEM_6), CogVR_KeyDown('M'), CogVR_KeyDown(VK_OEM_PERIOD), CogVR_KeyDown(VK_OEM_COMMA) };
    int k;
    if ((down[0] && !was[0]) || (down[1] && !was[1])) {
        CogVRDeck.tempo = CogVR_Clamp(CogVRDeck.tempo + (down[1] && !was[1] ? 0.25f : -0.25f), 0.0f, 1.0f);
        CogVR_Log("COGVR deck: tempo %.2f", CogVRDeck.tempo);
    }
    if (down[2] && !was[2]) { CogVRMusic_Command(0); }
    if (down[3] && !was[3]) { CogVRMusic_Command(1); }
    if (down[4] && !was[4]) { CogVRMusic_Command(-1); }
    for (k = 0; k < 5; k++) { was[k] = down[k]; }
}

/* beats per tick for an effective tempo t (0..1): one bar, half a bar, a beat, half a beat */
static float
CogVRDeck_Beats(float t)
{
    return (t < 0.25f) ? 4.0f : ((t < 0.5f) ? 2.0f : ((t < 0.75f) ? 1.0f : 0.5f));
}

/* render thread, once per frame: fire the auto-input ticks on the beat grid (or a plain clock without a confident beat) */
static void
CogVRDeck_TickUpdate(void)
{
    const double now = CogGPU_Now();
    const float t = SDL_max(CogVRDeck.tempo, CogVRDeck.squeeze);
    const struct CogVRBeatState *b = &CogVRBeat;
    const int onBeat = (b->conf >= COGVR_BEAT_CONF) && (b->periodMs > 200.0) && (b->periodMs < 2000.0);
    const double interval = onBeat ? b->periodMs * CogVRDeck_Beats(t) : 1400.0 - 1150.0 * (double) t;
    if (t <= 0.02f) {
        CogVRDeck.nextTick = 0.0;
        return;
    }
    if ((CogVRDeck.nextTick <= 0.0) || (now - CogVRDeck.nextTick > interval)) {   /* (re)align to the grid */
        const double base = onBeat ? b->downbeatMs : now;
        double k = SDL20_ceil((now - base) / interval);
        if (k < 0.0) { k = 0.0; }
        CogVRDeck.nextTick = base + k * interval;
    }
    if (now >= CogVRDeck.nextTick) {
        CogVR_QueueTick();
        CogVRDeck.tickFlash = now;
        CogVRDeck.nextTick += interval;
        if (!CogVRDeck.logged) {
            CogVRDeck.logged = 1;
            CogVR_Log("COGVR deck: first tick, every %.0f ms (%s)", interval, onBeat ? "beat grid" : "clock");
        }
    }
}

static float
CogVRDeck_Label(const char *s, float u, float v, float z, float px, Uint32 rgba)
{
    return CogVR_DynText(s, u - (float) SDL20_strlen(s) * 4.0f * px, v + 4.0f * px, z, px, rgba);
}

/* the panel, in every view (on stream it reads as the avatar's DJ desk) */
static void
CogVRDeck_Dyn(void)
{
    const double now = CogGPU_Now();
    const struct CogVRBeatState *b = &CogVRBeat;
    const int onBeat = (b->conf >= COGVR_BEAT_CONF) && (b->periodMs > 200.0);
    const float t = SDL_max(CogVRDeck.tempo, CogVRDeck.squeeze);
    char text[32];
    CogM4 m;
    int i;
    CogVRDeck_Frame(&m);
    CogVR_DynBatch(&m, 1);
    CogVR_DynBox(0.045f, 0.07f, -0.008f, 0.515f, 0.24f, 0.006f, CogVR_Pack(0.06f, 0.065f, 0.08f, 0.02f));         /* panel */
    if (CogVRDeck.layout || (CogVRDeck.held[0] >= 0) || (CogVRDeck.held[1] >= 0)) {
        CogVR_DynBox(0.0f, COGVR_DECK_TRAYV, -0.010f, 0.40f, 0.035f, 0.004f, CogVR_Pack(0.20f, 0.08f, 0.08f, 0.10f));  /* tray */
    }
    for (i = 0; i < COGVR_DECK_N; i++) {
        const CogVRDeckItem *it = &CogVRDeckItems[i];
        const int hot = (CogVRDeck.hover[0] == i) || (CogVRDeck.hover[1] == i) || (CogVRDeck.held[0] == i) || (CogVRDeck.held[1] == i);
        const float fl = (now - it->flash < 160.0) ? 1.0f : 0.0f;
        float u, v, lum;
        if (!CogVRDeck_Pos(i, &u, &v)) {
            continue;
        }
        lum = 0.18f + (hot ? 0.22f : 0.0f) + fl * 0.6f;
        if (it->kind == COGVR_DK_TEMPO) {
            const float kv = v - COGVR_DECK_FLEN * 0.5f + CogVRDeck.tempo * COGVR_DECK_FLEN;
            const float sv = v - COGVR_DECK_FLEN * 0.5f + CogVRDeck.squeeze * COGVR_DECK_FLEN;
            CogVR_DynBox(u, v, 0.0f, 0.006f, COGVR_DECK_FLEN * 0.5f, 0.004f, CogVR_Pack(0.25f, 0.27f, 0.32f, 0.1f));    /* rail */
            CogVR_DynBox(u, kv, 0.012f, 0.026f, 0.012f, 0.012f, CogVR_Pack(lum + 0.2f, lum + 0.15f, 0.25f, hot ? 0.8f : 0.3f));
            if (CogVRDeck.throttle && (CogVRDeck.squeeze > 0.0f)) {
                CogVR_DynBox(u + 0.034f, sv, 0.004f, 0.006f, 0.006f, 0.004f, CogVR_Pack(1.0f, 0.5f, 0.2f, 1.0f));
            }
            CogVRDeck_Label(it->label, u, v - COGVR_DECK_FLEN * 0.5f - 0.022f, 0.004f, 0.0021f, CogVR_Pack(0.8f, 0.85f, 0.9f, 0.6f));
            continue;
        }
        CogVR_DynBox(u, v, 0.004f, COGVR_DECK_BW, COGVR_DECK_BH, 0.008f,
                     (it->kind == COGVR_DK_STOP) ? CogVR_Pack(lum + 0.35f, lum * 0.4f, lum * 0.4f, 0.2f + fl)
                                                 : CogVR_Pack(lum, lum + 0.03f, lum + 0.08f, hot ? 0.3f + fl : 0.05f + fl));
        CogVRDeck_Label(it->label, u, v, 0.013f, 0.0021f, CogVR_Pack(0.85f, 0.95f, 1.0f, 0.8f));
    }
    /* status strip: beat light, tick light, tempo */
    if (onBeat) {
        const double rel = now - b->downbeatMs;
        const double ph = rel - SDL20_floor(rel / b->periodMs) * b->periodMs;
        const float on = (ph >= 0.0 && ph < 90.0) ? 1.0f : 0.15f;
        SDL20_snprintf(text, sizeof (text), "BPM %d", (int) (b->bpm + 0.5f));
        CogVR_DynBox(-0.43f, 0.25f, 0.004f, 0.010f, 0.010f, 0.006f, CogVR_Pack(on, on * 0.3f, on * 0.9f, on));
    } else {
        SDL20_snprintf(text, sizeof (text), "BPM --");
    }
    CogVRDeck_Label(text, -0.33f, 0.25f, 0.004f, 0.0021f, CogVR_Pack(0.7f, 0.75f, 0.85f, 0.5f));
    if (t > 0.02f) {
        static const char *const names[4] = { "BAR", "1/2 BAR", "BEAT", "1/2 BEAT" };
        const float b4 = CogVRDeck_Beats(t);
        SDL20_snprintf(text, sizeof (text), "AUTO %s", onBeat ? names[(b4 > 3.0f) ? 0 : ((b4 > 1.5f) ? 1 : ((b4 > 0.75f) ? 2 : 3))] : "ON");
    } else {
        SDL20_snprintf(text, sizeof (text), "AUTO OFF");
    }
    CogVRDeck_Label(text, 0.12f, 0.25f, 0.004f, 0.0021f, (t > 0.02f) ? CogVR_Pack(1.0f, 0.8f, 0.3f, 0.8f) : CogVR_Pack(0.5f, 0.55f, 0.6f, 0.3f));
    {
        const float on = (now - CogVRDeck.tickFlash < 120.0) ? 1.0f : 0.1f;
        CogVR_DynBox(0.43f, 0.25f, 0.004f, 0.010f, 0.010f, 0.006f, CogVR_Pack(on, on * 0.8f, on * 0.2f, on));
    }
    if (CogVRMusic.n) {   /* now / next, public: it is the show's ticker */
        const int cur = (int) CogVRMusic.cur;
        SDL20_snprintf(text, sizeof (text), "%s %.28s", CogVRMusic.paused ? "||" : "NOW", (cur >= 0) ? CogVRMusic.title[cur] : "-");
        CogVR_DynText(text, -0.44f, 0.30f, 0.004f, 0.0016f, CogVR_Pack(0.95f, 0.75f, 1.0f, 0.9f));
        SDL20_snprintf(text, sizeof (text), "NEXT %.24s", ((cur + 1 >= 1) && (cur + 1 < CogVRMusic.n)) ? CogVRMusic.title[cur + 1] : "-");
        CogVR_DynText(text, 0.08f, 0.30f, 0.004f, 0.0016f, CogVR_Pack(0.6f, 0.55f, 0.75f, 0.5f));
    }
    /* the notes: headset only (batch 2), never on the scene cameras */
    CogVR_DynBatch(&m, 2);
    for (i = 0; i < COGVR_DECK_N; i++) {
        const CogVRDeckItem *it = &CogVRDeckItems[i];
        const int hot = (CogVRDeck.hover[0] == i) || (CogVRDeck.hover[1] == i);
        float u, v;
        if (!CogVRDeck_Pos(i, &u, &v)) {
            continue;
        }
        if (it->kind == COGVR_DK_TEMPO) {
            v += COGVR_DECK_FLEN * 0.5f + 0.01f;
        } else {
            v += COGVR_DECK_BH + 0.010f;
        }
        if (hot) {   /* the hovered note: one line, larger, lifted toward you */
            CogVRDeck_Label(it->note, u, v + 0.006f, 0.05f, 0.0019f, CogVR_Pack(1.0f, 0.95f, 0.6f, 1.0f));
        } else {     /* the rest: small, at most two lines so neighbours never touch */
            const Uint32 ink = CogVR_Pack(0.45f, 0.75f, 0.85f, 0.35f);
            const char *sp = SDL20_strchr(it->note, ' ');
            if ((SDL20_strlen(it->note) > 9) && sp) {
                char first[16];
                SDL20_strlcpy(first, it->note, SDL_min(sizeof (first), (size_t) (sp - it->note) + 1));
                CogVRDeck_Label(first, u, v + 0.0115f, 0.03f, 0.0011f, ink);
                CogVRDeck_Label(sp + 1, u, v, 0.03f, 0.0011f, ink);
            } else {
                CogVRDeck_Label(it->note, u, v, 0.03f, 0.0011f, ink);
            }
        }
    }
    CogVRDeck_StandDyn();
}

/* the key a control sends, as text for the stand */
static void
CogVRDeck_KeyText(const CogVRDeckItem *it, char *out, size_t n)
{
    if (it->kind != COGVR_DK_KEY) {
        SDL20_strlcpy(out, "deck", n);
    } else if (it->uni) {
        SDL20_snprintf(out, n, "'%c'", it->uni);
    } else if ((it->sym >= SDLK12_F1) && (it->sym <= SDLK12_F12)) {
        SDL20_snprintf(out, n, "F%d", it->sym - SDLK12_F1 + 1);
    } else if (it->sym == SDLK12_ESCAPE) {
        SDL20_strlcpy(out, "Esc", n);
    } else {
        SDL20_snprintf(out, n, "'%c'", it->sym);
    }
}

/* the music stand above the panel: what the hovered control does, then the page. Headset only. */
static void
CogVRDeck_StandDyn(void)
{
    static const float px = 0.00135f;
    const float lh = 8.0f * px * 1.4f, w = (float) COGVR_STAND_COLS * 8.0f * px;
    const Uint32 ink = CogVR_Pack(0.85f, 0.88f, 0.80f, 0.55f), dim = CogVR_Pack(0.5f, 0.55f, 0.6f, 0.3f);
    const int hv = (CogVRDeck.hover[1] >= 0) ? CogVRDeck.hover[1] : CogVRDeck.hover[0];
    char text[128], key[16];
    CogM4 m;
    float y = 0.0f;
    int r;
    if (!CogVRDeck.standOpen) {
        return;
    }
    CogM4_World(&m, CogVR.seatYaw, 1.0f, CogV3_Add(CogVR.seat, CogV3_RotY(CogV3_Add(CogV3_Make(0.0f, 0.74f, -0.62f), CogVRDeck.off), CogVR.seatYaw)));
    CogVR_DynBatch(&m, 2);
    CogVR_DynBox(0.0f, -lh * (COGVR_STAND_ROWS + 3) * 0.5f + lh, -0.006f, w * 0.5f + 0.02f, lh * (COGVR_STAND_ROWS + 3) * 0.5f + 0.015f, 0.003f,
                 CogVR_Pack(0.03f, 0.035f, 0.045f, 0.0f));
    if (hv >= 0) {
        const CogVRDeckItem *it = &CogVRDeckItems[hv];
        CogVRDeck_KeyText(it, key, sizeof (key));
        SDL20_snprintf(text, sizeof (text), "%s  %s  %s", it->label, key, it->note);
        CogVR_DynText(text, -w * 0.5f, y, 0.0f, px, CogVR_Pack(1.0f, 0.9f, 0.5f, 1.0f));
    }
    y -= lh * 1.6f;
    for (r = 0; r < COGVR_STAND_ROWS; r++, y -= lh) {
        const char *s = NULL;
        if (CogVRDeck.page == 0) {
            s = (r < COGVR_STAND_CARD) ? CogVRStandCard[r] : NULL;
        } else {
            const int k = (CogVRDeck.page - 1) * COGVR_STAND_ROWS + r;
            s = (k < CogVRDeck.nlines) ? CogVRDeck.line[k] : NULL;
        }
        if (s && *s) {
            CogVR_DynText(s, -w * 0.5f, y, 0.0f, px, ink);
        }
    }
    SDL20_snprintf(text, sizeof (text), "page %d/%d", CogVRDeck.page + 1, CogVRDeck.pages);
    CogVR_DynText(text, w * 0.5f - (float) SDL20_strlen(text) * 8.0f * px, y - lh * 0.4f, 0.0f, px, dim);
}
