/*
 * cogmind_vrinteg.inc.c -- integrity sky (docs/VR_SUIT.md): the run's status from a local fault channel, heartbeat
 * watchdogs and internal checks. Included by cogmind_vr.inc.c after the per-frame box helpers.
 *
 * Render thread owns everything here except CogVRInteg.sky, which the game-thread bridge reads to gate input.
 * Reads no game memory: helpers (verifier, detectors, remotes) report over UDP 127.0.0.1:COGVR_INTEGRITY_PORT.
 *
 *   watch <source> <ms>     expect `beat <source>` at least every <ms>; a miss turns the sky red
 *   beat <source>           heartbeat
 *   red <source> <reason>   proof broken: red for the rest of the session, bridge cut, world greyed
 *   hold <source> <reason>  amber (bridge paused) until `clear <source>`
 *   clear <source>          clear that source's hold
 *   wiz <source> <reason>   green for the rest of the run (restored from COGVR_LEDGER at start)
 */

enum { COGVR_SKY_NORMAL = 0, COGVR_SKY_GREEN, COGVR_SKY_AMBER, COGVR_SKY_RED };
enum { COGVR_HOLD_PRESENT = 0, COGVR_HOLD_VR, COGVR_HOLD_COUNT };

#define COGVR_INTEG_SOURCES     16
#define COGVR_INTEG_TEXT        96
#define COGVR_INTEG_PORT        47917
#define COGVR_INTEG_PRESENT_MS  3000.0  /* no new snapshot for this long: the game stopped presenting */
#define COGVR_INTEG_GRACE_MS    10000.0 /* COGVR_INTEGRITY_REQUIRE: time for a required source's first beat */

typedef struct CogVRSockAddr { short family; unsigned short port; unsigned long addr; char zero[8]; } CogVRSockAddr;
typedef int (WINAPI *CogVRWSAStartup_t)(WORD, void *);
typedef UINT_PTR (WINAPI *CogVRSocket_t)(int, int, int);
typedef int (WINAPI *CogVRBind_t)(UINT_PTR, const CogVRSockAddr *, int);
typedef int (WINAPI *CogVRIoctl_t)(UINT_PTR, long, unsigned long *);
typedef int (WINAPI *CogVRRecvFrom_t)(UINT_PTR, char *, int, int, CogVRSockAddr *, int *);

static const char *const CogVRSkyName[] = { "normal", "green", "amber", "red" };
/* sky / fog colours (linear, before exposure): night, wizard green, hold amber, disconnected red */
static const float CogVRSkyColour[4][3] = {
    { 0.004f, 0.005f, 0.009f }, { 0.005f, 0.042f, 0.011f }, { 0.055f, 0.028f, 0.002f }, { 0.070f, 0.003f, 0.003f }
};

static struct {
    int open;                          /* the UDP channel is bound */
    UINT_PTR sock;
    CogVRRecvFrom_t recvFrom;
    char ledger[300];                  /* COGVR_LEDGER: append-only run ledger */
    int red, green;
    char redWhy[COGVR_INTEG_TEXT], greenWhy[COGVR_INTEG_TEXT];
    struct CogVRIntegSource {
        char name[24];
        int hold, watchMs;
        double lastBeat;
        char why[COGVR_INTEG_TEXT];
    } src[COGVR_INTEG_SOURCES];
    int nsrc, loggedFull;
    int internal[COGVR_HOLD_COUNT];
    double lastPresent;
    int shown;                         /* sky applied by the last CogVR_IntegPoll */
    char banner[COGVR_INTEG_TEXT + 48];
    volatile LONG sky;                 /* published for the bridge (game thread) */
} CogVRInteg;

/* Game-thread rehearsal activation is consumed here on the render thread. */
static volatile LONG CogVRRehearsalWizard;

static void CogVR_Haptic(int h);
static int CogMesh_JointWorld(const char *name, CogM4 *out);

static void
CogVR_IntegLedger(const char *kind, const char *source, const char *why)
{
    char line[300];
    SYSTEMTIME t;
    HANDLE h;
    DWORD wrote;
    int n;
    if (!CogVRInteg.ledger[0]) {
        return;
    }
    GetSystemTime(&t);
    n = SDL20_snprintf(line, sizeof (line), "%04u-%02u-%02uT%02u:%02u:%02uZ %s %s %s\r\n", (unsigned) t.wYear, (unsigned) t.wMonth,
                       (unsigned) t.wDay, (unsigned) t.wHour, (unsigned) t.wMinute, (unsigned) t.wSecond, kind, source, why);
    if ((n <= 0) || (n >= (int) sizeof (line))) {
        return;
    }
    h = CreateFileA(CogVRInteg.ledger, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        CogVR_Log("COGVR integrity: cannot append to ledger %s (error %lu)", CogVRInteg.ledger, (unsigned long) GetLastError());
        return;
    }
    WriteFile(h, line, (DWORD) n, &wrote, NULL);
    CloseHandle(h);
}

static struct CogVRIntegSource *
CogVR_IntegSource(const char *name)
{
    struct CogVRIntegSource *s;
    int i;
    for (i = 0; i < CogVRInteg.nsrc; i++) {
        if (!SDL20_strcmp(CogVRInteg.src[i].name, name)) {
            return &CogVRInteg.src[i];
        }
    }
    if (CogVRInteg.nsrc >= COGVR_INTEG_SOURCES) {
        if (!CogVRInteg.loggedFull) {
            CogVRInteg.loggedFull = 1;
            CogVR_Log("COGVR integrity: source table full (%d): ignoring new source %s", COGVR_INTEG_SOURCES, name);
        }
        return NULL;
    }
    s = &CogVRInteg.src[CogVRInteg.nsrc++];
    SDL20_zerop(s);
    SDL20_strlcpy(s->name, name, sizeof (s->name));
    return s;
}

static void
CogVR_IntegRed(const char *source, const char *why)
{
    if (CogVRInteg.red) {
        return;                        /* the first reason stands */
    }
    CogVRInteg.red = 1;
    SDL20_snprintf(CogVRInteg.redWhy, sizeof (CogVRInteg.redWhy), "%s: %s", source, why);
    CogVR_IntegLedger("red", source, why);
}

static void
CogVR_IntegGreen(const char *source, const char *why, int fromLedger)
{
    if (CogVRInteg.green) {
        return;
    }
    CogVRInteg.green = 1;
    SDL20_snprintf(CogVRInteg.greenWhy, sizeof (CogVRInteg.greenWhy), "%s: %s", source, why);
    if (!fromLedger) {
        CogVR_IntegLedger("wiz", source, why);
    }
}

/* an internal hold (renderer-side check) changed */
static void
CogVR_IntegInternal(int which, int hold)
{
    static const char *const names[COGVR_HOLD_COUNT] = { "internal:present", "internal:vr" };
    static const char *const whys[COGVR_HOLD_COUNT] = { "game stopped presenting frames", "headset not tracking" };
    if (hold == CogVRInteg.internal[which]) {
        return;
    }
    CogVRInteg.internal[which] = hold;
    CogVR_IntegLedger(hold ? "hold" : "clear", names[which], hold ? whys[which] : "-");
}

/* one channel line: printable ASCII only (the caller sanitised it) */
static void
CogVR_IntegLine(char *line)
{
    char *source, *why;
    struct CogVRIntegSource *s;
    while (*line == ' ') { line++; }
    source = SDL20_strchr(line, ' ');
    if (!*line || !source) {
        return;
    }
    *source++ = 0;
    while (*source == ' ') { source++; }
    why = SDL20_strchr(source, ' ');
    if (why) {
        *why++ = 0;
        while (*why == ' ') { why++; }
    } else {
        why = source + SDL20_strlen(source);
    }
    if (!*source || SDL20_strlen(source) >= sizeof (s->name)) {
        CogVR_Log("COGVR integrity: bad source in '%s'", line);
        return;
    }
    if (!*why) {
        why = "-";
    }
    if (!SDL20_strcmp(line, "red")) {
        CogVR_IntegRed(source, why);
    } else if (!SDL20_strcmp(line, "wiz")) {
        CogVR_IntegGreen(source, why, 0);
    } else if ((s = CogVR_IntegSource(source)) == NULL) {
        return;
    } else if (!SDL20_strcmp(line, "beat")) {
        s->lastBeat = CogGPU_Now();
    } else if (!SDL20_strcmp(line, "watch")) {
        const int ms = SDL20_atoi(why);
        if ((ms < 100) || (ms > 600000)) {
            CogVR_Log("COGVR integrity: watch %s %s: want 100..600000 ms", source, why);
            return;
        }
        s->watchMs = ms;
        s->lastBeat = CogGPU_Now();
        CogVR_Log("COGVR integrity: watching %s, heartbeat <= %d ms", source, ms);
    } else if (!SDL20_strcmp(line, "hold")) {
        if (!s->hold || SDL20_strcmp(s->why, why)) {
            CogVR_IntegLedger("hold", source, why);
        }
        s->hold = 1;
        SDL20_strlcpy(s->why, why, sizeof (s->why));
    } else if (!SDL20_strcmp(line, "clear")) {
        if (s->hold) {
            CogVR_IntegLedger("clear", source, "-");
        }
        s->hold = 0;
    } else {
        CogVR_Log("COGVR integrity: unknown verb '%s' from %s", line, source);
    }
}

/* COGVR_LEDGER: restore green from an earlier session of the same run (the launcher starts a new ledger per run) */
static void
CogVR_IntegReadLedger(void)
{
    HANDLE f = CreateFileA(CogVRInteg.ledger, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    char buf[16384], *line;
    DWORD n = 0;
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    if (!ReadFile(f, buf, sizeof (buf) - 1, &n, NULL)) {
        n = 0;
    }
    CloseHandle(f);
    buf[n] = 0;
    for (line = buf; line && *line; ) {
        char *next = SDL20_strchr(line, '\n');
        char *kind = SDL20_strchr(line, ' ');
        if (next) { *next++ = 0; }
        if (kind && !SDL20_strncmp(kind + 1, "wiz ", 4)) {
            char *source = kind + 5, *why = SDL20_strchr(source, ' ');
            char *cr = SDL20_strchr(source, '\r');
            if (cr) { *cr = 0; }
            if (why) { *why++ = 0; }
            CogVR_IntegGreen(source, why ? why : "-", 1);
            CogVR_Log("COGVR integrity: ledger %s: run is wizard since an earlier session (%s)", CogVRInteg.ledger, CogVRInteg.greenWhy);
            break;
        }
        line = next;
    }
}

/* render thread, before the first frame */
static void
CogVR_IntegInit(void)
{
    const char *v = SDL12COMPAT_getenv_unsafe("COGVR_INTEGRITY_PORT");
    const int port = (v && *v) ? SDL20_atoi(v) : COGVR_INTEG_PORT;
    const double now = CogGPU_Now();
    CogVRInteg.lastPresent = now;
    v = SDL12COMPAT_getenv_unsafe("COGVR_LEDGER");
    if (v && *v) {
        SDL20_strlcpy(CogVRInteg.ledger, v, sizeof (CogVRInteg.ledger));
        CogVR_IntegReadLedger();
        CogVR_IntegLedger("session", "shim", "start");
    }
    v = SDL12COMPAT_getenv_unsafe("COGVR_INTEGRITY_REQUIRE");   /* source:ms[,source:ms...] */
    while (v && *v) {
        char name[24];
        int len = 0;
        while (*v && (*v != ':') && (*v != ',') && (len < (int) sizeof (name) - 1)) { name[len++] = *v++; }
        name[len] = 0;
        if ((*v == ':') && len) {
            struct CogVRIntegSource *s = CogVR_IntegSource(name);
            const int ms = SDL20_atoi(++v);
            if (s && (ms >= 100) && (ms <= 600000)) {
                s->watchMs = ms;
                s->lastBeat = now + COGVR_INTEG_GRACE_MS;
                CogVR_Log("COGVR integrity: requiring %s, first beat within %.0f s, then <= %d ms", name, COGVR_INTEG_GRACE_MS / 1000.0, ms);
            } else {
                CogVR_Log("COGVR integrity: ignoring COGVR_INTEGRITY_REQUIRE entry %s (want source:100..600000)", name);
            }
        }
        while (*v && (*v != ',')) { v++; }
        if (*v == ',') { v++; }
    }
    if ((port <= 0) || (port > 65535)) {
        CogVR_Log("COGVR integrity: channel off (COGVR_INTEGRITY_PORT=%d)", port);
        return;
    }
    {
        HMODULE ws2 = LoadLibraryA("ws2_32.dll");
        CogVRWSAStartup_t startup = ws2 ? (CogVRWSAStartup_t) (void *) GetProcAddress(ws2, "WSAStartup") : NULL;
        CogVRSocket_t sock = ws2 ? (CogVRSocket_t) (void *) GetProcAddress(ws2, "socket") : NULL;
        CogVRBind_t bindf = ws2 ? (CogVRBind_t) (void *) GetProcAddress(ws2, "bind") : NULL;
        CogVRIoctl_t ioctl = ws2 ? (CogVRIoctl_t) (void *) GetProcAddress(ws2, "ioctlsocket") : NULL;
        char wsa[512];                 /* WSADATA (about 400 bytes on x86) */
        CogVRSockAddr a;
        unsigned long nonBlocking = 1;
        CogVRInteg.recvFrom = ws2 ? (CogVRRecvFrom_t) (void *) GetProcAddress(ws2, "recvfrom") : NULL;
        if (!startup || !sock || !bindf || !ioctl || !CogVRInteg.recvFrom || startup(0x0202, wsa)) {
            CogVR_Log("COGVR integrity: ws2_32 unavailable: channel off");
            return;
        }
        CogVRInteg.sock = sock(2 /* AF_INET */, 2 /* SOCK_DGRAM */, 17 /* IPPROTO_UDP */);
        if (CogVRInteg.sock == (UINT_PTR) ~(UINT_PTR) 0) {
            CogVR_Log("COGVR integrity: socket failed: channel off");
            return;
        }
        SDL20_zero(a);
        a.family = 2;
        a.port = (unsigned short) (((port & 0xff) << 8) | ((port >> 8) & 0xff));
        a.addr = 0x0100007fUL;         /* 127.0.0.1, network order */
        if (bindf(CogVRInteg.sock, &a, (int) sizeof (a)) || ioctl(CogVRInteg.sock, (long) 0x8004667EUL /* FIONBIO */, &nonBlocking)) {
            CogVR_Log("COGVR integrity: cannot bind 127.0.0.1:%d: channel off", port);
            return;
        }
        CogVRInteg.open = 1;
        CogVR_Log("COGVR integrity: channel on 127.0.0.1:%d (udp)", port);
    }
}

static void
CogVR_IntegApplySky(int sky, const char *why)
{
    const int wasRed = (CogVRInteg.shown == COGVR_SKY_RED);
    static const char *const label[4] = { "", "WIZ", "HOLD", "DISCONNECTED" };
    CogVR_Log("COGVR integrity: sky %s -> %s (%s)", CogVRSkyName[CogVRInteg.shown], CogVRSkyName[sky], why);
    CogVR_IntegLedger("sky", CogVRSkyName[sky], why);
    if (sky == COGVR_SKY_NORMAL) {
        CogVRInteg.banner[0] = 0;
    } else {
        SDL20_snprintf(CogVRInteg.banner, sizeof (CogVRInteg.banner), "%s %s", label[sky], why);
    }
    if (CogVR.hwnd) {
        char title[200];
        SDL20_snprintf(title, sizeof (title), "Cogmind VR%s%s", CogVRInteg.banner[0] ? " - " : "", CogVRInteg.banner);
        SetWindowTextA(CogVR.hwnd, title);
    }
    if (CogVR.mode == 2) {
        CogVR_Haptic(0);
        CogVR_Haptic(1);
    }
    if (wasRed != (sky == COGVR_SKY_RED)) {
        CogVR.sceneDirty = 1;          /* rebuild the map greyed (or not) */
    }
    CogVRInteg.shown = sky;
    InterlockedExchange(&CogVRInteg.sky, (LONG) sky);
}

/* render thread, once per frame: drain the channel, run the watchdogs, publish the sky */
static void
CogVR_IntegPoll(void)
{
    const double now = CogGPU_Now();
    const char *why = "-";
    int sky = COGVR_SKY_NORMAL, i;
    if (InterlockedCompareExchange(&CogVRRehearsalWizard, 0, 0)) {
        CogVR_IntegGreen("rehearsal", "OFFLINE ASSIST god-mode; AI enabled", 0);
    }
    if (CogVRInteg.open) {
        char buf[1024];
        int n;
        while ((n = CogVRInteg.recvFrom(CogVRInteg.sock, buf, (int) sizeof (buf) - 1, 0, NULL, NULL)) > 0) {
            char *line = buf, *p;
            buf[n] = 0;
            for (p = buf; *p; p++) {
                if (*p == '\n') { continue; }
                if ((*p < 32) || (*p > 126)) { *p = ' '; }
            }
            while (line && *line) {
                char *next = SDL20_strchr(line, '\n');
                if (next) { *next++ = 0; }
                if (SDL20_strlen(line) > COGVR_INTEG_TEXT) { line[COGVR_INTEG_TEXT] = 0; }
                CogVR_IntegLine(line);
                line = next;
            }
        }
    }
    for (i = 0; i < CogVRInteg.nsrc; i++) {
        const struct CogVRIntegSource *s = &CogVRInteg.src[i];
        if (s->watchMs && (now - s->lastBeat > (double) s->watchMs)) {
            CogVR_IntegRed(s->name, "heartbeat lost");
        }
    }
    CogVR_IntegInternal(COGVR_HOLD_PRESENT, now - CogVRInteg.lastPresent > COGVR_INTEG_PRESENT_MS);
    if (CogVRInteg.red) {
        sky = COGVR_SKY_RED;
        why = CogVRInteg.redWhy;
    } else {
        if (CogVRInteg.internal[COGVR_HOLD_VR]) {
            sky = COGVR_SKY_AMBER;
            why = "internal:vr: headset not tracking";
        } else if (CogVRInteg.internal[COGVR_HOLD_PRESENT]) {
            sky = COGVR_SKY_AMBER;
            why = "internal:present: game stopped presenting frames";
        }
        for (i = 0; (sky != COGVR_SKY_AMBER) && (i < CogVRInteg.nsrc); i++) {
            if (CogVRInteg.src[i].hold) {
                static char text[COGVR_INTEG_TEXT + 32];
                SDL20_snprintf(text, sizeof (text), "%s: %s", CogVRInteg.src[i].name, CogVRInteg.src[i].why);
                sky = COGVR_SKY_AMBER;
                why = text;
            }
        }
        if ((sky == COGVR_SKY_NORMAL) && CogVRInteg.green) {
            sky = COGVR_SKY_GREEN;
            why = CogVRInteg.greenWhy;
        }
    }
    if ((sky != CogVRInteg.shown) || ((sky != COGVR_SKY_NORMAL) && SDL20_strcmp(CogVRInteg.banner + SDL20_strlen(CogVRInteg.banner) - SDL20_strlen(why), why))) {
        CogVR_IntegApplySky(sky, why);
    }
}

/* map colours under a red sky: grey, dim, no glow (the world is read-only) */
static void
CogVR_IntegGrey(float *r, float *g, float *b, float *e)
{
    const float l = (0.30f * *r + 0.59f * *g + 0.11f * *b) * 0.55f + 0.03f;
    *r = *g = *b = l;
    if (e) {
        *e = 0.0f;
    }
}

/*
 * Text as glyph boxes (the 8x8 font) in the current batch's space: left edge at x, top at y, facing +z, `px` metres per
 * font pixel. Returns the width in metres.
 */
static float
CogVR_DynText(const char *s, float x, float y, float z, float px, Uint32 rgba)
{
    float cx = x;
    for (; *s; s++, cx += 8.0f * px) {
        const unsigned ch = (unsigned char) *s;
        int k;
        if (ch >= 128) {
            continue;
        }
        for (k = 0; k < CogVR.glyphN[ch]; k++) {
            const Uint8 *b = CogVR.glyphBox[ch][k];
            CogVR_DynBox(cx + (b[0] + b[1] + 1) * 0.5f * px, y - (b[2] + b[3] + 1) * 0.5f * px, z,
                         (b[1] - b[0] + 1) * 0.5f * px, (b[3] - b[2] + 1) * 0.5f * px, px * 0.4f, rgba);
        }
    }
    return cx - x;
}

/* headset/desktop view: the banner 0.7 m ahead of the eyes, below the line of sight (eyes-only batch) */
static void
CogVR_IntegBannerDyn(const CogM4 *head)
{
    static const float px = 0.0024f;
    Uint32 rgba;
    const float *c;
    if (!CogVRInteg.banner[0]) {
        return;
    }
    c = CogVRSkyColour[CogVRInteg.shown];
    rgba = CogVR_Pack(0.35f + c[0] * 4.0f, 0.35f + c[1] * 4.0f, 0.35f + c[2] * 4.0f, 1.0f);
    CogVR_DynBatch(head, 2);
    CogVR_DynText(CogVRInteg.banner, -(float) SDL20_strlen(CogVRInteg.banner) * 4.0f * px, -0.16f, -0.7f, px, rgba);
}

/* a joint frame without its scale: box sizes stay in metres */
static void
CogVR_IntegUnscale(CogM4 *m)
{
    int c, r;
    for (c = 0; c < 3; c++) {
        const float l = CogVR.sqrtf_(m->m[0][c] * m->m[0][c] + m->m[1][c] * m->m[1][c] + m->m[2][c] * m->m[2][c]);
        if (l > 1e-6f) {
            for (r = 0; r < 3; r++) { m->m[r][c] /= l; }
        }
    }
}

/*
 * Wizard: a witch hat on the avatar's head and a wand in its right hand (stream camera only). Sized by u, the distance
 * from the head joint to the halo above it (the head's height on this rig), so it fits any avatar scale.
 */
static void
CogVR_IntegAvatarDyn(const CogM4 *head)
{
    const Uint32 hat = CogVR_Pack(0.30f, 0.12f, 0.50f, 0.30f), band = CogVR_Pack(0.25f, 1.0f, 0.40f, 1.0f);
    const Uint32 wood = CogVR_Pack(0.45f, 0.28f, 0.15f, 0.10f), star = CogVR_Pack(1.0f, 0.95f, 0.55f, 1.0f);
    CogM4 at, halo;
    float u = 0.24f;
    int k;
    if (!CogVRInteg.green) {
        return;
    }
    if (CogMesh_JointWorld("head", &at)) {
        if (CogMesh_JointWorld("halo", &halo)) {
            const float dx = halo.m[0][3] - at.m[0][3], dy = halo.m[1][3] - at.m[1][3], dz = halo.m[2][3] - at.m[2][3];
            u = SDL_max(0.05f, CogVR.sqrtf_(dx * dx + dy * dy + dz * dz));
        }
        CogVR_IntegUnscale(&at);
    } else {
        at = *head;
    }
    CogVR_DynBatch(&at, 0);
    /* the Migu rig's crown is ~1.65 u above the head joint (halo mid-head); a chibi head wants a wide brim */
    CogVR_DynBox(0.0f, 1.60f * u, 0.0f, 1.30f * u, 0.06f * u, 1.30f * u, hat);                     /* brim */
    CogVR_DynBox(0.0f, 1.76f * u, 0.0f, 0.78f * u, 0.10f * u, 0.78f * u, band);                    /* band */
    for (k = 0; k < 5; k++) {                                                                     /* cone, tip bending back */
        const float w = (0.72f - 0.13f * (float) k) * u;
        CogVR_DynBox(0.0f, (2.00f + 0.33f * (float) k) * u, 0.03f * (float) (k * k) * u, w, 0.17f * u, w, hat);
    }
    if (CogMesh_JointWorld("rightHand", &at)) {
        CogVR_IntegUnscale(&at);
        CogVR_DynBatch(&at, 0);
        CogVR_DynBox(0.0f, 0.0f, -1.1f * u, 0.04f * u, 0.04f * u, 1.1f * u, wood);
        CogVR_DynBox(0.0f, 0.0f, -2.3f * u, 0.14f * u, 0.14f * u, 0.14f * u, star);
    }
}
