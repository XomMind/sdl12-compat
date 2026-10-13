/*
 * cogmind_vrmusic.inc.c -- the in-engine music player (docs/VR_SUIT.md, Music). Included by cogmind_vr.inc.c.
 *
 *   COGVR_MUSIC=<playlist.m3u | folder>   Ogg Vorbis tracks; an .m3u lists paths (relative to it, '#' lines skipped),
 *                                         a folder plays its *.ogg in name order. The set starts at track 1 and stops
 *                                         after the last.
 *   COGVR_MUSIC_VOL=<0..1>                default 0.8
 *   COGVR_NOWPLAYING=<file>               "NOW ...\r\nNEXT ...\r\n" rewritten at every track change (OBS text source)
 *
 * The audio callback mixes the track into the game's own output just before the beat tap, so the beat tracker locks on the
 * music (the deck's tempo grid follows the set) and OBS captures one mixed game audio source. Threads: the render thread
 * scans the playlist and opens tracks (file read + stb_vorbis setup); the audio thread takes a ready track through an
 * atomic pointer, decodes, resamples linearly to the device rate and mixes; it closes the tracks it is done with.
 * Cogmind has sound effects but no music, so nothing of the game is replaced.
 */

/* stb_vorbis without a C runtime: its few libc calls go to SDL2 (resolved at start, like the renderer's float math) */
static double (SDLCALL *CogMusic_pow)(double, double);
static double (SDLCALL *CogMusic_exp)(double);
static double (SDLCALL *CogMusic_log)(double);
static double (SDLCALL *CogMusic_cos)(double);
static double (SDLCALL *CogMusic_sin)(double);
static double (SDLCALL *CogMusic_scalbn)(double, int);
static int (SDLCALL *CogMusic_memcmp)(const void *, const void *, size_t);
static void (SDLCALL *CogMusic_qsort)(void *, size_t, size_t, int (SDLCALL *)(const void *, const void *));

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <assert.h>
#include <malloc.h>
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define malloc SDL20_malloc
#define realloc SDL20_realloc
#define free SDL20_free
#define memcpy SDL20_memcpy
#define memset SDL20_memset
#define memcmp CogMusic_memcmp
#define qsort(b, n, s, c) CogMusic_qsort((b), (n), (s), (int (SDLCALL *)(const void *, const void *)) (c))
#define pow CogMusic_pow
#define exp CogMusic_exp
#define log CogMusic_log
#define cos CogMusic_cos
#define sin CogMusic_sin
#define floor SDL20_floor
#define ldexp CogMusic_scalbn
#define abs(x) (((x) < 0) ? -(x) : (x))
#undef alloca
#define alloca __builtin_alloca
#include "stb_vorbis.c"
#undef malloc
#undef realloc
#undef free
#undef memcpy
#undef memset
#undef memcmp
#undef qsort
#undef pow
#undef exp
#undef log
#undef cos
#undef sin
#undef floor
#undef ldexp
#undef abs
#undef alloca
/* stb_vorbis leaves these single-letter macros defined */
#undef L
#undef C
#undef R

/* gcc emits a call to memcpy for stb_vorbis's large struct copies; this DLL links no C runtime, so forward it to SDL2 */
void *memcpy(void *dst, const void *src, size_t n);
void *
memcpy(void *dst, const void *src, size_t n)
{
    return SDL20_memcpy(dst, src, n);
}

#define COGVR_MUSIC_FRAMES 1024         /* decoded stereo frames per refill */

typedef struct CogVRTrack {
    stb_vorbis *v;
    unsigned char *mem;                /* the whole .ogg (stb_vorbis reads from memory) */
    int idx, rate;
} CogVRTrack;

static struct {
    /* render thread */
    int n;                             /* playlist length (0: no player) */
    char **path, **title;              /* title: "Artist - Title" once the track was opened, else the file name */
    char nowFile[300];
    int shownCur, started;
    /* render thread -> audio thread */
    CogVRTrack *volatile pending;      /* the next track to play, opened */
    volatile LONG skip;                /* start `pending` now instead of at the end of the current track */
    volatile LONG paused;
    float volume;
    /* audio thread -> render thread */
    volatile LONG cur;                 /* playlist index playing, -1 none */
    volatile LONG played;              /* frames of the current track at its own rate */
    /* audio thread */
    CogVRTrack *play;
    float buf[COGVR_MUSIC_FRAMES * 2];
    int bufN, bufPos;
    float a[2], b[2];                  /* resampler: the frames around the read position */
    double t;
} CogVRMusic;

static int
CogVRMusic_LoadSyms(void)
{
    int okay = 1;
    CogMusic_pow = (double (SDLCALL *)(double, double)) LoadSDL20Symbol("SDL_pow", &okay);
    CogMusic_exp = (double (SDLCALL *)(double)) LoadSDL20Symbol("SDL_exp", &okay);
    CogMusic_log = (double (SDLCALL *)(double)) LoadSDL20Symbol("SDL_log", &okay);
    CogMusic_cos = (double (SDLCALL *)(double)) LoadSDL20Symbol("SDL_cos", &okay);
    CogMusic_sin = (double (SDLCALL *)(double)) LoadSDL20Symbol("SDL_sin", &okay);
    CogMusic_scalbn = (double (SDLCALL *)(double, int)) LoadSDL20Symbol("SDL_scalbn", &okay);
    CogMusic_memcmp = (int (SDLCALL *)(const void *, const void *, size_t)) LoadSDL20Symbol("SDL_memcmp", &okay);
    CogMusic_qsort = (void (SDLCALL *)(void *, size_t, size_t, int (SDLCALL *)(const void *, const void *))) LoadSDL20Symbol("SDL_qsort", &okay);
    return okay;
}

static char *
CogVRMusic_Strdup(const char *s, size_t n)
{
    char *d = (char *) SDL20_malloc(n + 1);
    if (d) {
        SDL20_memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

static void
CogVRMusic_Add(const char *path)
{
    const char *base = SDL20_strrchr(path, '\\'), *dot;
    char **p, **t;
    base = base ? base + 1 : path;
    dot = SDL20_strrchr(base, '.');
    p = (char **) SDL20_realloc(CogVRMusic.path, (size_t) (CogVRMusic.n + 1) * sizeof (char *));
    if (p) { CogVRMusic.path = p; }
    t = (char **) SDL20_realloc(CogVRMusic.title, (size_t) (CogVRMusic.n + 1) * sizeof (char *));
    if (t) { CogVRMusic.title = t; }
    if (!p || !t) {
        return;
    }
    CogVRMusic.path[CogVRMusic.n] = CogVRMusic_Strdup(path, SDL20_strlen(path));
    CogVRMusic.title[CogVRMusic.n] = CogVRMusic_Strdup(base, dot ? (size_t) (dot - base) : SDL20_strlen(base));
    if (CogVRMusic.path[CogVRMusic.n] && CogVRMusic.title[CogVRMusic.n]) {
        CogVRMusic.n++;
    }
}

static int
CogVRMusic_CmpName(const void *x, const void *y)
{
    return SDL20_strcmp(*(char *const *) x, *(char *const *) y);
}

/* render thread, at start */
static void
CogVRMusic_Init(void)
{
    const char *env = SDL12COMPAT_getenv_unsafe("COGVR_MUSIC");   /* shared static buffer: copy before the next getenv */
    char v[MAX_PATH];
    DWORD attr;
    CogVRMusic.cur = -1;
    CogVRMusic.shownCur = -2;
    CogVRMusic.volume = 0.8f;
    if (!env || !*env) {
        return;
    }
    SDL20_strlcpy(v, env, sizeof (v));
    if (!CogVRMusic_LoadSyms()) {
        CogVR_Log("COGVR music: SDL2 math functions missing: player off");
        return;
    }
    {
        const char *vol = SDL12COMPAT_getenv_unsafe("COGVR_MUSIC_VOL");
        if (vol && *vol) { CogVRMusic.volume = CogVR_Clamp((float) SDL20_strtod(vol, NULL), 0.0f, 1.0f); }
        vol = SDL12COMPAT_getenv_unsafe("COGVR_NOWPLAYING");
        if (vol && *vol) { SDL20_strlcpy(CogVRMusic.nowFile, vol, sizeof (CogVRMusic.nowFile)); }
    }
    attr = GetFileAttributesA(v);
    if ((attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        char pat[MAX_PATH], full[MAX_PATH];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        SDL20_snprintf(pat, sizeof (pat), "%s\\*.ogg", v);
        h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                SDL20_snprintf(full, sizeof (full), "%s\\%s", v, fd.cFileName);
                CogVRMusic_Add(full);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        if (CogVRMusic.n > 1) {   /* name order; titles move with their paths */
            int i, j;
            for (i = 1; i < CogVRMusic.n; i++) {
                for (j = i; (j > 0) && (CogVRMusic_CmpName(&CogVRMusic.path[j - 1], &CogVRMusic.path[j]) > 0); j--) {
                    char *tp = CogVRMusic.path[j]; char *tt = CogVRMusic.title[j];
                    CogVRMusic.path[j] = CogVRMusic.path[j - 1]; CogVRMusic.title[j] = CogVRMusic.title[j - 1];
                    CogVRMusic.path[j - 1] = tp; CogVRMusic.title[j - 1] = tt;
                }
            }
        }
    } else {
        HANDLE f = CreateFileA(v, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        char buf[16384], dir[MAX_PATH], *line, *slash;
        DWORD n = 0;
        if (f == INVALID_HANDLE_VALUE) {
            CogVR_Log("COGVR music: cannot open %s (error %lu): player off", v, (unsigned long) GetLastError());
            return;
        }
        if (!ReadFile(f, buf, sizeof (buf) - 1, &n, NULL)) { n = 0; }
        CloseHandle(f);
        buf[n] = 0;
        SDL20_strlcpy(dir, v, sizeof (dir));
        slash = SDL20_strrchr(dir, '\\');
        if (slash) { slash[1] = 0; } else { dir[0] = 0; }
        for (line = buf; line && *line; ) {
            char *next = SDL20_strchr(line, '\n'), *end, full[MAX_PATH];
            if (next) { *next++ = 0; }
            end = line + SDL20_strlen(line);
            while ((end > line) && ((end[-1] == '\r') || (end[-1] == ' '))) { *--end = 0; }
            if (*line && (*line != '#')) {
                const int absolute = (line[0] == '\\') || (line[0] && (line[1] == ':'));
                SDL20_snprintf(full, sizeof (full), "%s%s", absolute ? "" : dir, line);
                CogVRMusic_Add(full);
            }
            line = next;
        }
    }
    CogVR_Log("COGVR music: %d tracks from %s, volume %.2f", CogVRMusic.n, v, CogVRMusic.volume);
}

/* render thread: open playlist entry i (whole file in memory) and learn its title from the Vorbis comments */
static CogVRTrack *
CogVRMusic_Open(int i)
{
    CogVRTrack *tr;
    HANDLE f;
    DWORD size, n = 0;
    int err = 0;
    if ((i < 0) || (i >= CogVRMusic.n)) {
        return NULL;
    }
    f = CreateFileA(CogVRMusic.path[i], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        CogVR_Log("COGVR music: cannot open %s (error %lu)", CogVRMusic.path[i], (unsigned long) GetLastError());
        return NULL;
    }
    size = GetFileSize(f, NULL);
    tr = (CogVRTrack *) SDL20_calloc(1, sizeof (*tr));
    if (tr && (size > 0) && (size < 512u * 1024u * 1024u)) {
        tr->mem = (unsigned char *) SDL20_malloc(size);
    }
    if (tr && tr->mem && !ReadFile(f, tr->mem, size, &n, NULL)) {
        n = 0;
    }
    CloseHandle(f);
    if (tr && tr->mem && (n == size)) {
        tr->v = stb_vorbis_open_memory(tr->mem, (int) n, &err, NULL);
    }
    if (!tr || !tr->v) {
        CogVR_Log("COGVR music: %s is not a playable Ogg Vorbis file (stb_vorbis error %d)", CogVRMusic.path[i], err);
        if (tr) { SDL20_free(tr->mem); }
        SDL20_free(tr);
        return NULL;
    }
    tr->idx = i;
    tr->rate = (int) stb_vorbis_get_info(tr->v).sample_rate;
    {
        const stb_vorbis_comment c = stb_vorbis_get_comment(tr->v);
        const char *artist = NULL, *title = NULL;
        int k;
        for (k = 0; k < c.comment_list_length; k++) {
            if (!SDL20_strncasecmp(c.comment_list[k], "ARTIST=", 7)) { artist = c.comment_list[k] + 7; }
            if (!SDL20_strncasecmp(c.comment_list[k], "TITLE=", 6)) { title = c.comment_list[k] + 6; }
        }
        if (title && *title) {
            char text[160];
            char *dup;
            SDL20_snprintf(text, sizeof (text), "%s%s%s", artist ? artist : "", artist ? " - " : "", title);
            dup = CogVRMusic_Strdup(text, SDL20_strlen(text));
            if (dup) {
                SDL20_free(CogVRMusic.title[i]);
                CogVRMusic.title[i] = dup;
            }
        }
    }
    return tr;
}

static void
CogVRMusic_Close(CogVRTrack *tr)
{
    if (tr) {
        stb_vorbis_close(tr->v);
        SDL20_free(tr->mem);
        SDL20_free(tr);
    }
}

/* render thread: make `tr` the next track (replacing an unplayed one); skip = start it now */
static void
CogVRMusic_Queue(CogVRTrack *tr, int skip)
{
    CogVRMusic_Close((CogVRTrack *) InterlockedExchangePointer((PVOID volatile *) &CogVRMusic.pending, tr));
    if (skip && tr) {
        InterlockedExchange(&CogVRMusic.skip, 1);
    }
}

/* deck: 0 play/pause, +1 next, -1 previous (restart when more than 3 s in) */
static void
CogVRMusic_Command(int what)
{
    const int cur = (int) CogVRMusic.cur;
    if (!CogVRMusic.n) {
        return;
    }
    if (what == 0) {
        InterlockedExchange(&CogVRMusic.paused, !CogVRMusic.paused);
        CogVR_Log("COGVR music: %s", CogVRMusic.paused ? "paused" : "playing");
        return;
    }
    {
        const int restart = (what < 0) && (cur >= 0) && (CogVRMusic.played > 3 * 44100);
        const int i = (cur < 0) ? 0 : (restart ? cur : cur + what);
        if ((i < 0) || (i >= CogVRMusic.n)) {
            return;
        }
        CogVRMusic_Queue(CogVRMusic_Open(i), 1);
        InterlockedExchange(&CogVRMusic.paused, 0);
        CogVR_Log("COGVR music: %s -> track %d", (what > 0) ? "next" : (restart ? "restart" : "previous"), i + 1);
    }
}

static void
CogVRMusic_WriteNow(void)
{
    char text[400];
    const int cur = (int) CogVRMusic.cur;
    HANDLE f;
    DWORD wrote;
    int n;
    n = SDL20_snprintf(text, sizeof (text), "NOW  %s\r\nNEXT %s\r\n", (cur >= 0) ? CogVRMusic.title[cur] : "-",
                       ((cur + 1 >= 1) && (cur + 1 < CogVRMusic.n)) ? CogVRMusic.title[cur + 1] : "-");
    CogVR_Log("COGVR music: now %s", (cur >= 0) ? CogVRMusic.title[cur] : "-");
    if (!CogVRMusic.nowFile[0] || (n <= 0) || (n >= (int) sizeof (text))) {
        return;
    }
    f = CreateFileA(CogVRMusic.nowFile, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        CogVR_Log("COGVR music: cannot write %s (error %lu)", CogVRMusic.nowFile, (unsigned long) GetLastError());
        return;
    }
    WriteFile(f, text, (DWORD) n, &wrote, NULL);
    CloseHandle(f);
}

/* render thread, once per frame: start the set, keep the following track opened ahead (gapless), publish now/next */
static void
CogVRMusic_Update(void)
{
    const int cur = (int) CogVRMusic.cur;
    if (!CogVRMusic.n) {
        return;
    }
    if (!CogVRMusic.started) {
        CogVRMusic.started = 1;
        CogVRMusic_Queue(CogVRMusic_Open(0), 1);
    } else if ((cur >= 0) && !CogVRMusic.pending && !CogVRMusic.skip && (cur + 1 < CogVRMusic.n)) {
        CogVRMusic_Queue(CogVRMusic_Open(cur + 1), 0);
    }
    if (cur != CogVRMusic.shownCur) {
        CogVRMusic.shownCur = cur;
        CogVRMusic_WriteNow();
    }
}

/* audio thread: the next frame of the playing track into a->b, refilling; 0 at its end */
static int
CogVRMusic_Advance(void)
{
    CogVRTrack *tr = CogVRMusic.play;
    if (CogVRMusic.bufPos >= CogVRMusic.bufN) {
        CogVRMusic.bufN = stb_vorbis_get_samples_float_interleaved(tr->v, 2, CogVRMusic.buf, COGVR_MUSIC_FRAMES * 2);
        CogVRMusic.bufPos = 0;
        if (CogVRMusic.bufN <= 0) {
            return 0;
        }
    }
    CogVRMusic.a[0] = CogVRMusic.b[0];
    CogVRMusic.a[1] = CogVRMusic.b[1];
    CogVRMusic.b[0] = CogVRMusic.buf[CogVRMusic.bufPos * 2];
    CogVRMusic.b[1] = CogVRMusic.buf[CogVRMusic.bufPos * 2 + 1];
    CogVRMusic.bufPos++;
    return 1;
}

/* audio thread: make `tr` the playing track (closing the old one) */
static void
CogVRMusic_Start(CogVRTrack *tr)
{
    CogVRMusic_Close(CogVRMusic.play);
    CogVRMusic.play = tr;
    CogVRMusic.bufN = CogVRMusic.bufPos = 0;
    CogVRMusic.t = 0.0;
    CogVRMusic.b[0] = CogVRMusic.b[1] = 0.0f;
    InterlockedExchange(&CogVRMusic.played, 0);
    InterlockedExchange(&CogVRMusic.cur, tr ? tr->idx : -1);
    if (tr) {
        CogVRMusic_Advance();
        CogVRMusic_Advance();
    }
}

/* audio thread: mix the music into the device stream (S16 / S32 / F32, any channel count) */
static void
CogVR_MusicMix(Uint8 *stream, int len, const SDL_AudioSpec *spec)
{
    int bytes, ch, frames, i;
    double step;
    if (!CogVRMusic.n || !stream || !spec || (spec->freq <= 0) || !spec->channels) {
        return;
    }
    if (InterlockedExchange(&CogVRMusic.skip, 0)) {
        CogVRMusic_Start((CogVRTrack *) InterlockedExchangePointer((PVOID volatile *) &CogVRMusic.pending, NULL));
    }
    if (!CogVRMusic.play || CogVRMusic.paused) {
        return;
    }
    if (spec->format == AUDIO_S16SYS) {
        bytes = 2;
    } else if ((spec->format == AUDIO_F32SYS) || (spec->format == AUDIO_S32SYS)) {
        bytes = 4;
    } else {
        return;
    }
    ch = spec->channels;
    frames = len / (bytes * ch);
    step = (double) CogVRMusic.play->rate / (double) spec->freq;
    for (i = 0; (i < frames) && CogVRMusic.play; i++) {
        const float tt = (float) CogVRMusic.t;
        const float l = (CogVRMusic.a[0] + (CogVRMusic.b[0] - CogVRMusic.a[0]) * tt) * CogVRMusic.volume;
        const float r = (CogVRMusic.a[1] + (CogVRMusic.b[1] - CogVRMusic.a[1]) * tt) * CogVRMusic.volume;
        int c;
        for (c = 0; c < ((ch >= 2) ? 2 : 1); c++) {
            const float x = (ch >= 2) ? (c ? r : l) : 0.5f * (l + r);
            const int k = i * ch + c;
            if (bytes == 2) {
                const float s = (float) ((Sint16 *) stream)[k] + x * 32767.0f;
                ((Sint16 *) stream)[k] = (Sint16) ((s > 32767.0f) ? 32767.0f : ((s < -32768.0f) ? -32768.0f : s));
            } else if (spec->format == AUDIO_F32SYS) {
                ((float *) stream)[k] += x;
            } else {
                const double s = (double) ((Sint32 *) stream)[k] + (double) x * 2147483647.0;
                ((Sint32 *) stream)[k] = (Sint32) ((s > 2147483647.0) ? 2147483647.0 : ((s < -2147483648.0) ? -2147483648.0 : s));
            }
        }
        CogVRMusic.t += step;
        while (CogVRMusic.t >= 1.0) {
            CogVRMusic.t -= 1.0;
            if (!CogVRMusic_Advance()) {   /* end of the track: the next one, opened ahead, plays gaplessly */
                CogVRMusic_Start((CogVRTrack *) InterlockedExchangePointer((PVOID volatile *) &CogVRMusic.pending, NULL));
                break;
            }
        }
    }
    if (CogVRMusic.play) {
        InterlockedExchangeAdd(&CogVRMusic.played, (LONG) ((double) frames * step));
    }
}
