/*
 * cogmind_gpu.inc.c -- Cogmind GPU console extension for sdl12-compat (SDG.dll).
 *
 * #included near the end of src/SDL12_compat.c (the hooks are this branch's diff vs release-1.2.78), so it shares that
 * file's statics: VideoSurface12, VideoRenderer20, VideoTexture20, LockVideoRenderer(), Rect12to20(), ... The contract
 * it implements is src/cogmind_gpu.h; read that first.
 *
 * Design in one paragraph: a persistent SDL_TEXTUREACCESS_TARGET texture ("the GPU screen", ARGB8888, size of the 1.2
 * video surface) replaces the CPU surface -> VideoTexture20 upload path. COGGPU_End() draws the changed console cells
 * into it with two batched SDL_RenderGeometryRaw passes (opaque background quads, then glyph quads sampling a
 * per-font coverage atlas, vertex colour = foreground). COGGPU_End() only RECORDS the cells; they are drawn when something needs
 * the pixels (present, readback, FillRect/UpperBlit on the screen, ...), so a frame costs one render-target round trip.
 * PresentScreen() draws the target instead of VideoTexture20.
 * The few other 1.2 calls that touch the screen surface (SDL_FillRect, SDL_UpperBlit) are redirected to the GPU screen
 * while it exists. All of this code only runs after the first successful COGGPU_Begin(); until then, and with
 * COGGPU=0, the library behaves exactly like stock sdl12-compat.
 *
 * Only SDL2 functions that sdl12-compat's own symbol table (SDL20_syms.h) lacks are resolved here, through the same
 * loader (LoadSDL20Symbol), so SDL20_syms.h stays unpatched.
 *
 * Byte order: little endian (Windows) is assumed for the pixel readers.
 */

#include "cogmind_gpu.h"

/* The DLL is linked -nostdlib: keep GCC from turning our fill/copy loops into memset()/memcpy() calls. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("no-tree-loop-distribute-patterns")
#endif

#define COGGPU_MAXATLAS 64
#define COGGPU_MAXQUADS 16384          /* quads per RenderGeometryRaw call: 4 * 16384 vertices still fit Uint16 indices */
#define COGGPU_UV_BIAS  0.01f          /* texels; see CogGPU_DrawGlyphs */

typedef struct CogGPUVertex {
    float x, y;
    SDL_Color c;
    float u, v;
} CogGPUVertex;

typedef struct CogGPUAtlas {
    SDL12_Surface *surf;               /* key: the 1.2 font surface */
    SDL_Texture *tex;
    Uint32 *argb;                      /* CPU copy (white, alpha = coverage) to re-upload after a device reset */
    Uint32 *sat;                       /* summed-area table of "coverage != 0", (w+1)*(h+1): O(1) test for blank glyph rectangles */
    int w, h;
    int dirty;
} CogGPUAtlas;

/* SDL2 entry points that SDL20_syms.h does not have. */
typedef struct CogGPUSyms {
    int (SDLCALL *SetRenderTarget)(SDL_Renderer *, SDL_Texture *);
    int (SDLCALL *RenderGeometryRaw)(SDL_Renderer *, SDL_Texture *, const float *, int, const SDL_Color *, int,
                                     const float *, int, int, const void *, int, int);
    int (SDLCALL *RenderReadPixels)(SDL_Renderer *, const SDL_Rect *, Uint32, void *, int);
    int (SDLCALL *SetTextureBlendMode)(SDL_Texture *, SDL_BlendMode);
    int (SDLCALL *SetTextureScaleMode)(SDL_Texture *, SDL_ScaleMode);
    int (SDLCALL *SetTextureAlphaMod)(SDL_Texture *, Uint8);
    int (SDLCALL *SetRenderDrawBlendMode)(SDL_Renderer *, SDL_BlendMode);
    int (SDLCALL *RenderFillRect)(SDL_Renderer *, const SDL_Rect *);
} CogGPUSyms;

static struct CogGPUState {
    int envInit;
    char logPath[300];
    char dumpDir[300];

    int symsTried, symsOK;
    CogGPUSyms f;

    SDL_Texture *target;               /* the GPU screen; non-NULL <=> "GPU mode" */
    int tw, th;
    int failed;                        /* creation failed for this video mode: stop retrying every frame */
    volatile int needFull;             /* next COGGPU_Begin reports fullRedraw */
    volatile int deviceReset;          /* atlases must be re-uploaded */
    int inPass;
    int passCap;                       /* capacity of the buffer handed out by the current COGGPU_Begin */

    COGGPU_Cell *cells;                /* pool: [0,pend) = recorded cells not drawn yet, then the buffer handed out by COGGPU_Begin */
    int cap;
    int pend;                          /* cells recorded by COGGPU_End and not drawn yet */
    struct CogGPUBatch { int start, count, cw, ch; } *batches;   /* one per COGGPU_End; drawn in order */
    int nbatches, batchCap;
    int maxBatch;                      /* largest batch: vertex buffer sizing */
    CogGPUVertex *verts;
    int vertsCap;                      /* in vertices */
    int *order;
    int orderCap;
    Uint16 *indices;                   /* shared pattern, COGGPU_MAXQUADS quads */

    CogGPUAtlas atlas[COGGPU_MAXATLAS];
    int fcnt[COGGPU_MAXATLAS + 1];

    int dirty;                         /* the GPU screen changed (or may need repainting) since the last present */
    Uint32 dumpPoll;                   /* GetTicks of the last dump.req poll */
    /* diagnostics, only touched when COGGPU_LOG is set */
    int endCalls;
    double tGeom;                      /* ms inside RenderGeometryRaw during the current flush */
    double tPresent0;                  /* PresentScreen start */
    double winT0;                      /* start of the current 2 s window */
    int winPresents; double winPresentMs;
    int winFlushes, winCells; double winFlushMs, winFlushMax;
    int winSkipped;
} CogGPU;

static SDL_bool CogGPU_MakeAtlasTexture(SDL_Renderer *renderer, CogGPUAtlas *a);
static void CogGPU_PresentDone(void);
static int CogGPU_UploadAtlas(CogGPUAtlas *a);

/* ------------------------------------------------------------------------------------------------------------- */
/* logging, timing, files                                                                                         */
/* ------------------------------------------------------------------------------------------------------------- */

static void
CogGPU_InitEnv(void)
{
    const char *v;
    if (CogGPU.envInit) {
        return;
    }
    CogGPU.envInit = 1;
    v = SDL12COMPAT_getenv_unsafe("COGGPU_LOG");
    if (v && *v) {
        SDL20_snprintf(CogGPU.logPath, sizeof (CogGPU.logPath), "%s", v);
    }
    v = SDL12COMPAT_getenv_unsafe("COGGPU_DUMP");
    if (v && *v) {
        SDL20_snprintf(CogGPU.dumpDir, sizeof (CogGPU.dumpDir), "%s", v);
    }
}

static SDL_bool
CogGPU_Enabled(void)
{
    const char *v = SDL12COMPAT_getenv_unsafe("COGGPU");
    return (v && (v[0] == '0')) ? SDL_FALSE : SDL_TRUE;
}

#ifdef _WIN32
static double
CogGPU_Now(void)   /* milliseconds */
{
    static double perMs;
    LARGE_INTEGER t;
    if (perMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        perMs = (double) f.QuadPart / 1000.0;
    }
    QueryPerformanceCounter(&t);
    return (double) t.QuadPart / perMs;
}

static void
CogGPU_Log(const char *fmt, ...)
{
    char buf[600];
    va_list ap;
    HANDLE h;
    DWORD wrote;
    int n;

    CogGPU_InitEnv();
    if (!CogGPU.logPath[0]) {
        return;
    }
    va_start(ap, fmt);
    n = SDL20_vsnprintf(buf, sizeof (buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int) sizeof (buf) - 3) {
        n = (int) sizeof (buf) - 3;
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    h = CreateFileA(CogGPU.logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, buf, (DWORD) n, &wrote, NULL);
        CloseHandle(h);
    }
}

static int
CogGPU_FileExists(const char *path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void
CogGPU_DeleteFile(const char *path)
{
    DeleteFileA(path);
}

static int
CogGPU_WriteFile(const char *path, const void *a, int an, const void *b, int bn)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD wrote = 0;
    int ok = 1;
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (an > 0) {
        ok = WriteFile(h, a, (DWORD) an, &wrote, NULL) && (wrote == (DWORD) an);
    }
    if (ok && bn > 0) {
        ok = WriteFile(h, b, (DWORD) bn, &wrote, NULL) && (wrote == (DWORD) bn);
    }
    CloseHandle(h);
    return ok;
}
#else  /* diagnostics are only implemented for the Windows build */
static double CogGPU_Now(void) { return 0.0; }
static void CogGPU_Log(const char *fmt, ...) { (void) fmt; }
static int CogGPU_FileExists(const char *path) { (void) path; return 0; }
static void CogGPU_DeleteFile(const char *path) { (void) path; }
static int CogGPU_WriteFile(const char *path, const void *a, int an, const void *b, int bn) { (void) path; (void) a; (void) an; (void) b; (void) bn; return 0; }
#endif

/* ------------------------------------------------------------------------------------------------------------- */
/* pixel helpers                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------- */

static Uint32
CogGPU_ReadPixel(const Uint8 *row, int bpp, int x)
{
    switch (bpp) {
        case 1: return row[x];
        case 2: { const Uint8 *p = row + 2 * x; return (Uint32) p[0] | ((Uint32) p[1] << 8); }
        case 3: { const Uint8 *p = row + 3 * x; return (Uint32) p[0] | ((Uint32) p[1] << 8) | ((Uint32) p[2] << 16); }
        default: { const Uint8 *p = row + 4 * x; return (Uint32) p[0] | ((Uint32) p[1] << 8) | ((Uint32) p[2] << 16) | ((Uint32) p[3] << 24); }
    }
}

/* component value v (already shifted down) with `loss` dropped bits -> 0..255 */
static Uint8
CogGPU_Expand(Uint32 v, int loss)
{
    const Uint32 maxv = (0xFFu >> loss);
    if (loss == 0) {
        return (Uint8) v;
    }
    return (Uint8) ((v * 255u + maxv / 2u) / maxv);
}

/* Convert a rectangle of a 1.2 surface to XRGB8888 (B,G,R,X in memory). Used by the dump and for nothing else. */
static void
CogGPU_SurfaceToXRGB(const SDL12_Surface *s, Uint8 *dst, int dpitch, int x0, int y0, int w, int h)
{
    const SDL12_PixelFormat *f = s->format;
    const int bpp = f->BytesPerPixel;
    const Uint8 *base = (const Uint8 *) s->pixels;
    const int spitch = s->surface20 ? s->surface20->pitch : s->pitch;
    int x, y;
    for (y = 0; y < h; y++) {
        const Uint8 *row = base + (size_t) (y0 + y) * (size_t) spitch + (size_t) x0 * (size_t) bpp;
        Uint8 *d = dst + (size_t) y * (size_t) dpitch;
        for (x = 0; x < w; x++) {
            const Uint32 p = CogGPU_ReadPixel(row, bpp, x);
            Uint8 r, g, b;
            if (f->palette && (p < (Uint32) f->palette->ncolors)) {
                r = f->palette->colors[p].r; g = f->palette->colors[p].g; b = f->palette->colors[p].b;
            } else if (f->palette) {
                r = g = b = 0;
            } else {
                r = CogGPU_Expand((p & f->Rmask) >> f->Rshift, f->Rloss);
                g = CogGPU_Expand((p & f->Gmask) >> f->Gshift, f->Gloss);
                b = CogGPU_Expand((p & f->Bmask) >> f->Bshift, f->Bloss);
            }
            d[4 * x + 0] = b; d[4 * x + 1] = g; d[4 * x + 2] = r; d[4 * x + 3] = 0xFF;
        }
    }
}

/* ------------------------------------------------------------------------------------------------------------- */
/* symbols                                                                                                        */
/* ------------------------------------------------------------------------------------------------------------- */

static SDL_bool
CogGPU_LoadSyms(void)
{
    int okay = 1;
    if (CogGPU.symsTried) {
        return CogGPU.symsOK ? SDL_TRUE : SDL_FALSE;
    }
    CogGPU.symsTried = 1;
    CogGPU.f.SetRenderTarget = (int (SDLCALL *)(SDL_Renderer *, SDL_Texture *)) LoadSDL20Symbol("SDL_SetRenderTarget", &okay);
    CogGPU.f.RenderGeometryRaw = (int (SDLCALL *)(SDL_Renderer *, SDL_Texture *, const float *, int, const SDL_Color *, int, const float *, int, int, const void *, int, int)) LoadSDL20Symbol("SDL_RenderGeometryRaw", &okay);
    CogGPU.f.RenderReadPixels = (int (SDLCALL *)(SDL_Renderer *, const SDL_Rect *, Uint32, void *, int)) LoadSDL20Symbol("SDL_RenderReadPixels", &okay);
    CogGPU.f.SetTextureBlendMode = (int (SDLCALL *)(SDL_Texture *, SDL_BlendMode)) LoadSDL20Symbol("SDL_SetTextureBlendMode", &okay);
    CogGPU.f.SetTextureScaleMode = (int (SDLCALL *)(SDL_Texture *, SDL_ScaleMode)) LoadSDL20Symbol("SDL_SetTextureScaleMode", &okay);
    CogGPU.f.SetTextureAlphaMod = (int (SDLCALL *)(SDL_Texture *, Uint8)) LoadSDL20Symbol("SDL_SetTextureAlphaMod", &okay);
    CogGPU.f.SetRenderDrawBlendMode = (int (SDLCALL *)(SDL_Renderer *, SDL_BlendMode)) LoadSDL20Symbol("SDL_SetRenderDrawBlendMode", &okay);
    CogGPU.f.RenderFillRect = (int (SDLCALL *)(SDL_Renderer *, const SDL_Rect *)) LoadSDL20Symbol("SDL_RenderFillRect", &okay);
    CogGPU.symsOK = okay;
    if (!okay) {
        CogGPU_Log("COGGPU: SDL2 is too old / lacks render-target or geometry API (%s)", loaderror);
    }
    return okay ? SDL_TRUE : SDL_FALSE;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* lifetime                                                                                                       */
/* ------------------------------------------------------------------------------------------------------------- */

/* Lock the renderer; on NULL return nothing is locked. */
static SDL_Renderer *
CogGPU_Lock(void)
{
    SDL_Renderer *r;
    if (!VideoRendererLock) {
        return NULL;
    }
    r = LockVideoRenderer();
    if (!r) {
        UnlockVideoRenderer();
    }
    return r;
}

/* Destroy the GPU screen (not the atlases: they belong to the renderer and stay valid while it lives). */
static void
CogGPU_DestroyTarget(void)
{
    if (CogGPU.target) {
        if (CogGPU_Lock()) {
            SDL20_DestroyTexture(CogGPU.target);
            UnlockVideoRenderer();
        }
        CogGPU.target = NULL;
    }
    CogGPU.tw = CogGPU.th = 0;
    CogGPU.inPass = 0;
    CogGPU.pend = CogGPU.nbatches = CogGPU.maxBatch = 0;   /* recorded cells belonged to the dead screen */
}

static void
CogGPU_DestroyAtlas(CogGPUAtlas *a, SDL_bool destroyTexture)
{
    if (a->tex && destroyTexture) {
        SDL20_DestroyTexture(a->tex);
    }
    SDL20_free(a->argb);
    SDL20_free(a->sat);
    SDL20_memset(a, 0, sizeof (*a));
}

/* Called by EndVidModeCreate() before the renderer is destroyed: drop everything that belongs to it. The atlases keep their
   CPU data and slot (a slot stays valid until its surface is freed); the textures are rebuilt by the next COGGPU_Begin. */
static void
CogGPU_Teardown(void)
{
    int i;
    SDL_Renderer *r = CogGPU_Lock();
    for (i = 0; i < COGGPU_MAXATLAS; i++) {
        if (CogGPU.atlas[i].tex) {
            if (r) {
                SDL20_DestroyTexture(CogGPU.atlas[i].tex);
            }
            CogGPU.atlas[i].tex = NULL;
        }
    }
    if (r) {
        UnlockVideoRenderer();
    }
    CogGPU_DestroyTarget();
    SDL20_free(CogGPU.cells);   CogGPU.cells = NULL;   CogGPU.cap = 0;
    SDL20_free(CogGPU.batches); CogGPU.batches = NULL; CogGPU.batchCap = 0;
    SDL20_free(CogGPU.verts);   CogGPU.verts = NULL;   CogGPU.vertsCap = 0;
    SDL20_free(CogGPU.order);   CogGPU.order = NULL;   CogGPU.orderCap = 0;
    SDL20_free(CogGPU.indices); CogGPU.indices = NULL;
    CogGPU.failed = 0;
    CogGPU.symsTried = 0;       /* SDL2 may be unloaded and loaded again */
    CogGPU.needFull = 0;
    CogGPU.deviceReset = 0;
}

/* Called by SetVideoModeImpl() (new size known): every SDL_SetVideoMode invalidates the GPU screen (the next COGGPU_Begin
   recreates it). It also fixes a stock sdl12-compat bug: when SDL_SetVideoMode resizes a window that survives (same flags),
   VideoTexture20 and its size were kept, so the next SDL_UpdateRect locked a rect of the new, larger surface in the old, smaller
   streaming texture and the blit (or memcpy) ran off the end of the locked memory (access violation inside SDL2). A texture of
   the wrong size is destroyed here; SetVideoModeImpl() then creates a new one. */
static void
CogGPU_NewVideoMode(int width, int height)
{
    CogGPU_DestroyTarget();
    CogGPU.failed = 0;
    if (VideoTexture20) {
        int w = 0, h = 0, okay = 1;
        int (SDLCALL *query)(SDL_Texture *, Uint32 *, int *, int *, int *) =
            (int (SDLCALL *)(SDL_Texture *, Uint32 *, int *, int *, int *)) LoadSDL20Symbol("SDL_QueryTexture", &okay);
        if (!okay || (query(VideoTexture20, NULL, NULL, &w, &h) < 0) || (w != width) || (h != height)) {
            SDL20_DestroyTexture(VideoTexture20);
            VideoTexture20 = NULL;
        }
    }
}

/* SDL_UpdateRects hook: SDL_TRUE = do not upload the CPU surface. Always when the screen lives on the GPU. As a last line of
   defence the CPU texture must match the surface: an upload through a mismatching streaming texture can fault inside SDL2. */
static SDL_bool
CogGPU_SkipUpload(void)
{
    if (CogGPU.target) {
        return SDL_TRUE;
    }
    if (VideoTexture20 && VideoSurface12) {
        int w = 0, h = 0, okay = 1;
        int (SDLCALL *query)(SDL_Texture *, Uint32 *, int *, int *, int *) =
            (int (SDLCALL *)(SDL_Texture *, Uint32 *, int *, int *, int *)) LoadSDL20Symbol("SDL_QueryTexture", &okay);
        if (okay && (query(VideoTexture20, NULL, NULL, &w, &h) == 0) && ((w != VideoSurface12->w) || (h != VideoSurface12->h))) {
            CogGPU_Log("COGGPU: CPU texture %dx%d does not match the %dx%d screen surface: upload skipped", w, h, VideoSurface12->w, VideoSurface12->h);
            return SDL_TRUE;
        }
    }
    return SDL_FALSE;
}

/* SDL_FreeSurface hook: a freed font surface must never be found again through a recycled pointer. */
static void CogGPU_Flush(void);

static void
CogGPU_ForgetSurface(SDL12_Surface *s)
{
    int i;
    for (i = 0; i < COGGPU_MAXATLAS; i++) {
        if (CogGPU.atlas[i].surf == s) {
            CogGPU_Flush();   /* recorded cells may still reference this atlas */
            if (CogGPU_Lock()) {
                CogGPU_DestroyAtlas(&CogGPU.atlas[i], SDL_TRUE);
                UnlockVideoRenderer();
            } else {
                CogGPU_DestroyAtlas(&CogGPU.atlas[i], SDL_FALSE);
            }
        }
    }
}

/* Event watch hook (EventFilter20to12 sees every SDL2 event, including the render ones). */
static void
CogGPU_NoteEvent(const SDL_Event *e)
{
    if (e->type == SDL_WINDOWEVENT) {   /* expose, resize, restore, ...: the next SDL_Flip must really present */
        CogGPU.dirty = 1;
    } else if (e->type == SDL_RENDER_TARGETS_RESET) {
        CogGPU.needFull = 1;
        CogGPU.dirty = 1;
        CogGPU_Log("COGGPU: SDL_RENDER_TARGETS_RESET");
    } else if (e->type == SDL_RENDER_DEVICE_RESET) {
        CogGPU.needFull = 1;
        CogGPU.dirty = 1;
        CogGPU.deviceReset = 1;
        CogGPU_Log("COGGPU: SDL_RENDER_DEVICE_RESET");
    }
}

static SDL_bool
CogGPU_Active(void)
{
    return CogGPU.target ? SDL_TRUE : SDL_FALSE;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* geometry helpers                                                                                               */
/* ------------------------------------------------------------------------------------------------------------- */

static int
CogGPU_EnsureBuffers(int quads)
{
    if (!CogGPU.indices) {
        int i;
        CogGPU.indices = (Uint16 *) SDL20_malloc(sizeof (Uint16) * 6 * COGGPU_MAXQUADS);
        if (!CogGPU.indices) {
            return 0;
        }
        /* vertices TL, TR, BR, BL; triangles (0,1,2) (0,2,3): they share the TL-BR diagonal, which is the layout SDL's
           software renderer recognises as an axis-aligned rectangle (see SDL_SW_RenderGeometryRaw) */
        for (i = 0; i < COGGPU_MAXQUADS; i++) {
            Uint16 *p = CogGPU.indices + 6 * i;
            const Uint16 b = (Uint16) (4 * i);
            p[0] = b; p[1] = (Uint16) (b + 1); p[2] = (Uint16) (b + 2);
            p[3] = b; p[4] = (Uint16) (b + 2); p[5] = (Uint16) (b + 3);
        }
    }
    if (4 * quads > CogGPU.vertsCap) {
        CogGPUVertex *nv = (CogGPUVertex *) SDL20_realloc(CogGPU.verts, sizeof (CogGPUVertex) * 4 * (size_t) quads);
        if (!nv) {
            return 0;
        }
        CogGPU.verts = nv;
        CogGPU.vertsCap = 4 * quads;
    }
    if (quads > CogGPU.orderCap) {
        int *no = (int *) SDL20_realloc(CogGPU.order, sizeof (int) * (size_t) quads);
        if (!no) {
            return 0;
        }
        CogGPU.order = no;
        CogGPU.orderCap = quads;
    }
    return 1;
}

static int
CogGPU_DrawQuads(SDL_Renderer *renderer, SDL_Texture *tex, const CogGPUVertex *v, int nquads)
{
    const double t0 = CogGPU.logPath[0] ? CogGPU_Now() : 0.0;
    int rc;
    if (nquads <= 0) {
        return 0;
    }
    rc = CogGPU.f.RenderGeometryRaw(renderer, tex,
                                    &v[0].x, (int) sizeof (CogGPUVertex),
                                    &v[0].c, (int) sizeof (CogGPUVertex),
                                    &v[0].u, (int) sizeof (CogGPUVertex),
                                    4 * nquads, CogGPU.indices, 6 * nquads, (int) sizeof (Uint16));
    if (CogGPU.logPath[0]) {
        CogGPU.tGeom += CogGPU_Now() - t0;
    }
    return rc;
}

static void
CogGPU_SetQuad(CogGPUVertex *v, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
               Uint8 r, Uint8 g, Uint8 b)
{
    int i;
    v[0].x = x0; v[0].y = y0; v[0].u = u0; v[0].v = v0;   /* TL */
    v[1].x = x1; v[1].y = y0; v[1].u = u1; v[1].v = v0;   /* TR */
    v[2].x = x1; v[2].y = y1; v[2].u = u1; v[2].v = v1;   /* BR */
    v[3].x = x0; v[3].y = y1; v[3].u = u0; v[3].v = v1;   /* BL */
    for (i = 0; i < 4; i++) {
        v[i].c.r = r; v[i].c.g = g; v[i].c.b = b; v[i].c.a = 255;
    }
}

/* ------------------------------------------------------------------------------------------------------------- */
/* the GPU screen                                                                                                 */
/* ------------------------------------------------------------------------------------------------------------- */

/* Copy the CPU screen surface into the (currently set) render target, so the GPU screen starts equal to the CPU one. */
static void
CogGPU_UploadCPUScreen(SDL_Renderer *renderer, int w, int h)
{
    SDL_Surface *tmp;
    SDL_Texture *tex;
    SDL_BlendMode oldblend = SDL_BLENDMODE_NONE;
    const char *old_scale_quality;
    int rc;

    if (!VideoSurface12 || !VideoSurface12->pixels || !VideoSurface12->surface20) {
        return;
    }
    tmp = SDL20_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!tmp) {
        return;
    }
    SDL20_GetSurfaceBlendMode(VideoSurface12->surface20, &oldblend);
    SDL20_SetSurfaceBlendMode(VideoSurface12->surface20, SDL_BLENDMODE_NONE);
    rc = SDL20_UpperBlit(VideoSurface12->surface20, NULL, tmp, NULL);
    SDL20_SetSurfaceBlendMode(VideoSurface12->surface20, oldblend);
    if (rc == 0) {
        old_scale_quality = SDL20_GetHint(SDL_HINT_RENDER_SCALE_QUALITY);
        SDL20_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
        tex = SDL20_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
        SDL20_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, old_scale_quality);
        if (tex) {
            if (SDL20_UpdateTexture(tex, NULL, tmp->pixels, tmp->pitch) == 0) {
                SDL20_RenderCopy(renderer, tex, NULL, NULL);
            }
            SDL20_DestroyTexture(tex);
        }
    }
    SDL20_FreeSurface(tmp);
}

/* Create the target and verify that geometry + readback behave (1:1 pixels). Renderer lock held. */
static SDL_bool
CogGPU_CreateTarget(SDL_Renderer *renderer)
{
    SDL_RendererInfo rinfo;
    const char *old_scale_quality;
    SDL_Texture *tex;
    CogGPUVertex q[4];
    Uint32 px[9];
    SDL_Rect rc;
    const int w = VideoSurface12->w, h = VideoSurface12->h;
    int i, ok = 1;

    if (SDL20_GetRendererInfo(renderer, &rinfo) < 0) {
        CogGPU_Log("COGGPU: GetRendererInfo failed: %s", SDL20_GetError());
        return SDL_FALSE;
    }
    if (!(rinfo.flags & SDL_RENDERER_TARGETTEXTURE)) {
        CogGPU_Log("COGGPU: renderer '%s' has no render-target support", rinfo.name);
        return SDL_FALSE;
    }
    if ((rinfo.max_texture_width && (w > rinfo.max_texture_width)) || (rinfo.max_texture_height && (h > rinfo.max_texture_height))) {
        CogGPU_Log("COGGPU: %dx%d exceeds the maximum texture size of '%s'", w, h, rinfo.name);
        return SDL_FALSE;
    }
    if (!CogGPU_EnsureBuffers(1)) {
        return SDL_FALSE;
    }

    old_scale_quality = SDL20_GetHint(SDL_HINT_RENDER_SCALE_QUALITY);
    SDL20_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, WantScaleMethodNearest ? "0" : "1");  /* as for VideoTexture20 */
    tex = SDL20_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
    SDL20_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, old_scale_quality);
    if (!tex) {
        CogGPU_Log("COGGPU: creating the %dx%d target texture failed: %s", w, h, SDL20_GetError());
        return SDL_FALSE;
    }

    if (CogGPU.f.SetRenderTarget(renderer, tex) < 0) {
        CogGPU_Log("COGGPU: SDL_SetRenderTarget failed: %s", SDL20_GetError());
        SDL20_DestroyTexture(tex);
        return SDL_FALSE;
    }
    /* clear black (draw colour is kept black by sdl12-compat), then probe: a 2x2 red quad at the origin must cover
       exactly pixels (0..1, 0..1) of the target, whatever the window / logical size is. */
    SDL20_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL20_RenderClear(renderer);
    CogGPU_SetQuad(q, 0.0f, 0.0f, 2.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 255, 0, 0);
    if (CogGPU_DrawQuads(renderer, NULL, q, 1) < 0) {
        CogGPU_Log("COGGPU: SDL_RenderGeometryRaw failed: %s", SDL20_GetError());
        ok = 0;
    } else {
        rc.x = 0; rc.y = 0; rc.w = 3; rc.h = 3;
        for (i = 0; i < 9; i++) {
            px[i] = 0xDEADBEEFu;
        }
        if (CogGPU.f.RenderReadPixels(renderer, &rc, SDL_PIXELFORMAT_ARGB8888, px, 3 * 4) < 0) {
            CogGPU_Log("COGGPU: SDL_RenderReadPixels failed: %s", SDL20_GetError());
            ok = 0;
        } else {
            const int want_red[9] = { 1, 1, 0, 1, 1, 0, 0, 0, 0 };
            for (i = 0; i < 9; i++) {
                const Uint32 rgb = px[i] & 0x00FFFFFFu;
                if (rgb != (want_red[i] ? 0x00FF0000u : 0x00000000u)) {
                    ok = 0;
                }
            }
            if (!ok) {
                CogGPU_Log("COGGPU: geometry probe mismatch %08x %08x %08x / %08x %08x %08x / %08x %08x %08x",
                           px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7], px[8]);
            }
        }
    }
    if (ok) {  /* wipe the probe, then start from whatever the app already drew on the CPU surface (black unless it did) */
        SDL20_RenderClear(renderer);
        CogGPU_UploadCPUScreen(renderer, w, h);
    }
    CogGPU.f.SetRenderTarget(renderer, NULL);
    if (!ok) {
        SDL20_DestroyTexture(tex);
        return SDL_FALSE;
    }

    CogGPU.target = tex;
    CogGPU.tw = w;
    CogGPU.th = h;
    CogGPU.needFull = 1;
    CogGPU.dirty = 1;
    CogGPU_Log("COGGPU: GPU screen %dx%d created on renderer '%s' (flags 0x%x, scale %s)", w, h, rinfo.name, (unsigned) rinfo.flags, WantScaleMethodNearest ? "nearest" : "linear");
    return SDL_TRUE;
}

DECLSPEC12 int SDLCALL
COGGPU_Abi(void)
{
    return COGGPU_ABI;
}

DECLSPEC12 COGGPU_Cell * SDLCALL
COGGPU_Begin(void *screenSurface12, int capacity, int *fullRedraw)
{
    SDL_Renderer *renderer = NULL;
    int i, needRenderer;

    if (fullRedraw) {
        *fullRedraw = 0;
    }
    CogGPU_InitEnv();
    if (!CogGPU_Enabled()) {
        if (CogGPU.target) {      /* switched off at run time: fall back to the CPU screen */
            CogGPU_DestroyTarget();
        }
        return NULL;
    }
    if (!screenSurface12 || !VideoSurface12 || (screenSurface12 != (void *) VideoSurface12) || !VideoSurface12->surface20 ||
        (VideoSurface12->flags & SDL12_OPENGL) || !VideoWindow20 || !VideoRenderer20 || !VideoTexture20 || !VideoRendererLock) {
        return NULL;
    }
    if ((VideoSurface12->w <= 0) || (VideoSurface12->h <= 0)) {
        return NULL;
    }
    if (CogGPU.failed || !CogGPU_LoadSyms()) {
        return NULL;
    }
    if (capacity < 1) {
        capacity = 1;
    }

    /* The renderer lock is only taken when GPU objects have to be (re)created: UnlockVideoRenderer() resets the GL context,
       which is not free, and the common call (screen and atlases exist) touches no renderer state. */
    needRenderer = (!CogGPU.target) || (CogGPU.tw != VideoSurface12->w) || (CogGPU.th != VideoSurface12->h);
    for (i = 0; !needRenderer && (i < COGGPU_MAXATLAS); i++) {
        needRenderer = (CogGPU.atlas[i].surf && !CogGPU.atlas[i].tex);
    }
    if (needRenderer) {
        renderer = LockVideoRenderer();
        if (!renderer) {
            UnlockVideoRenderer();
            return NULL;
        }
        if (CogGPU.target && ((CogGPU.tw != VideoSurface12->w) || (CogGPU.th != VideoSurface12->h))) {
            CogGPU_DestroyTarget();   /* size changed behind our back */
        }
        if (!CogGPU.target && !CogGPU_CreateTarget(renderer)) {
            CogGPU.failed = 1;
            UnlockVideoRenderer();
            return NULL;
        }
        /* textures lost with a previous renderer (SetVideoMode recreated the window): rebuild them from the CPU copies */
        for (i = 0; i < COGGPU_MAXATLAS; i++) {
            if (CogGPU.atlas[i].surf && !CogGPU.atlas[i].tex) {
                CogGPU_MakeAtlasTexture(renderer, &CogGPU.atlas[i]);
            }
        }
        UnlockVideoRenderer();
    }
    if (CogGPU.pend + capacity > CogGPU.cap) {   /* the recorded cells (and this buffer) live in one pool */
        const int want = CogGPU.pend + capacity;
        COGGPU_Cell *nc = (COGGPU_Cell *) SDL20_realloc(CogGPU.cells, sizeof (COGGPU_Cell) * (size_t) want);
        if (!nc) {
            return NULL;
        }
        CogGPU.cells = nc;
        CogGPU.cap = want;
    }
    if (fullRedraw) {
        *fullRedraw = CogGPU.needFull ? 1 : 0;
    }
    CogGPU.needFull = 0;
    CogGPU.inPass = 1;
    CogGPU.passCap = capacity;
    return CogGPU.cells + CogGPU.pend;
}

/* ---- font atlases ------------------------------------------------------------------------------------------- */

/* Coverage mask of a 1.2 surface exactly as the software blit would read it (see cogmind_gpu.h). */
static Uint32 *
CogGPU_BuildMask(SDL12_Surface *s)
{
    const SDL12_PixelFormat *f = s->format;
    const int bpp = f->BytesPerPixel;
    const Uint8 *base = (const Uint8 *) (s->surface20 && s->surface20->pixels ? s->surface20->pixels : s->pixels);
    const int pitch = (s->surface20 && s->surface20->pixels) ? s->surface20->pitch : s->pitch;
    const int w = s->w, h = s->h;
    const SDL_bool useAlpha = ((s->flags & SDL12_SRCALPHA) && f->Amask) ? SDL_TRUE : SDL_FALSE;
    const SDL_bool useKey = (!useAlpha && (s->flags & SDL12_SRCCOLORKEY)) ? SDL_TRUE : SDL_FALSE;
    const Uint32 rgbmask = f->palette ? 0xFFFFFFFFu : (f->Rmask | f->Gmask | f->Bmask);
    Uint32 *out = (Uint32 *) SDL20_malloc((size_t) w * (size_t) h * sizeof (Uint32));
    int x, y;
    if (!out) {
        return NULL;
    }
    for (y = 0; y < h; y++) {
        const Uint8 *row = base + (size_t) y * (size_t) pitch;
        Uint32 *o = out + (size_t) y * (size_t) w;
        for (x = 0; x < w; x++) {
            Uint8 cov = 255;
            if (useAlpha) {
                const Uint32 p = CogGPU_ReadPixel(row, bpp, x);
                cov = CogGPU_Expand((p & f->Amask) >> f->Ashift, f->Aloss);
            } else if (useKey) {
                const Uint32 p = CogGPU_ReadPixel(row, bpp, x);
                cov = ((p & rgbmask) == (f->colorkey & rgbmask)) ? 0 : 255;
            }
            o[x] = ((Uint32) cov << 24) | 0x00FFFFFFu;
        }
    }
    return out;
}

/* (Re)create the GPU texture of an atlas entry from its CPU data. Renderer lock held. */
static SDL_bool
CogGPU_MakeAtlasTexture(SDL_Renderer *renderer, CogGPUAtlas *a)
{
    SDL_RendererInfo rinfo;
    const char *old_scale_quality;
    if ((SDL20_GetRendererInfo(renderer, &rinfo) < 0) ||
        (rinfo.max_texture_width && (a->w > rinfo.max_texture_width)) || (rinfo.max_texture_height && (a->h > rinfo.max_texture_height))) {
        CogGPU_Log("COGGPU: font %dx%d does not fit a texture", a->w, a->h);
        return SDL_FALSE;
    }
    old_scale_quality = SDL20_GetHint(SDL_HINT_RENDER_SCALE_QUALITY);
    SDL20_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");   /* nearest, whatever the window scaling quality is */
    a->tex = SDL20_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, a->w, a->h);
    SDL20_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, old_scale_quality);
    if (!a->tex || !CogGPU_UploadAtlas(a)) {
        CogGPU_Log("COGGPU: atlas %dx%d failed: %s", a->w, a->h, SDL20_GetError());
        if (a->tex) {
            SDL20_DestroyTexture(a->tex);
            a->tex = NULL;
        }
        return SDL_FALSE;
    }
    CogGPU.f.SetTextureScaleMode(a->tex, SDL_ScaleModeNearest);
    CogGPU.f.SetTextureBlendMode(a->tex, SDL_BLENDMODE_BLEND);
    return SDL_TRUE;
}

static int
CogGPU_UploadAtlas(CogGPUAtlas *a)
{
    if (SDL20_UpdateTexture(a->tex, NULL, a->argb, a->w * 4) < 0) {
        CogGPU_Log("COGGPU: uploading an atlas failed: %s", SDL20_GetError());
        return 0;
    }
    a->dirty = 0;
    return 1;
}

DECLSPEC12 int SDLCALL
COGGPU_Font(void *fontSurface12)
{
    SDL12_Surface *s = (SDL12_Surface *) fontSurface12;
    SDL_Renderer *renderer;
    CogGPUAtlas *a = NULL;
    Uint32 *mask;
    int i, slot = -1;

    if (!s || !CogGPU.target || !s->format || !s->w || !s->h) {
        return -1;
    }
    if (!(s->pixels || (s->surface20 && s->surface20->pixels))) {
        return -1;
    }
    for (i = 0; i < COGGPU_MAXATLAS; i++) {
        if (CogGPU.atlas[i].surf == s) {
            SDL_bool ok = SDL_TRUE;
            if (!CogGPU.atlas[i].tex) {   /* known surface whose texture died with a previous renderer */
                renderer = CogGPU_Lock();
                ok = renderer ? CogGPU_MakeAtlasTexture(renderer, &CogGPU.atlas[i]) : SDL_FALSE;
                if (renderer) {
                    UnlockVideoRenderer();
                }
            }
            return ok ? i : -1;
        } else if (!CogGPU.atlas[i].surf && (slot < 0)) {
            slot = i;
        }
    }
    if (slot < 0) {
        CogGPU_Log("COGGPU: out of atlas slots");
        return -1;
    }

    mask = CogGPU_BuildMask(s);
    if (!mask) {
        return -1;
    }
    a = &CogGPU.atlas[slot];
    SDL20_memset(a, 0, sizeof (*a));
    a->surf = s;
    a->argb = mask;
    a->w = s->w;
    a->h = s->h;
    a->sat = (Uint32 *) SDL20_malloc((size_t) (a->w + 1) * (size_t) (a->h + 1) * sizeof (Uint32));
    if (!a->sat) {
        CogGPU_DestroyAtlas(a, SDL_FALSE);
        return -1;
    }
    {
        const int stride = a->w + 1;
        int x, y;
        for (x = 0; x <= a->w; x++) {
            a->sat[x] = 0;
        }
        for (y = 1; y <= a->h; y++) {
            Uint32 rowsum = 0;
            a->sat[(size_t) y * stride] = 0;
            for (x = 1; x <= a->w; x++) {
                rowsum += (mask[(size_t) (y - 1) * a->w + (x - 1)] >> 24) ? 1u : 0u;
                a->sat[(size_t) y * stride + x] = a->sat[(size_t) (y - 1) * stride + x] + rowsum;
            }
        }
    }

    renderer = CogGPU_Lock();
    if (!renderer || !CogGPU_MakeAtlasTexture(renderer, a)) {
        if (renderer) {
            UnlockVideoRenderer();
        }
        CogGPU_DestroyAtlas(a, SDL_FALSE);
        return -1;
    }
    UnlockVideoRenderer();
    CogGPU_Log("COGGPU: font slot %d = surface %p %dx%d (alpha=%d key=%d bpp=%d)", slot, (void *) s, s->w, s->h,
               (s->flags & SDL12_SRCALPHA) && s->format->Amask ? 1 : 0, (s->flags & SDL12_SRCCOLORKEY) ? 1 : 0, s->format->BitsPerPixel);
    return slot;
}

/* ---- drawing ------------------------------------------------------------------------------------------------ */

/* Background pass. Horizontally adjacent cells of the same colour that follow each other in the buffer (the usual
   row-major order of a console) become one quad: the result is identical, SDL has far fewer vertices to process. */
static int
CogGPU_DrawBackgrounds(SDL_Renderer *renderer, const COGGPU_Cell *cells, int count, int cw, int ch)
{
    const int W = CogGPU.tw, H = CogGPU.th;
    CogGPUVertex *v = CogGPU.verts;
    int i, n = 0, haveRun = 0;
    int rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
    Uint8 rr = 0, rg = 0, rb = 0;
    for (i = 0; i <= count; i++) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0, visible = 0;
        const COGGPU_Cell *c = NULL;
        if (i < count) {
            c = &cells[i];
            x0 = c->dx; y0 = c->dy; x1 = c->dx + cw; y1 = c->dy + ch;
            if (x0 < 0) { x0 = 0; }
            if (y0 < 0) { y0 = 0; }
            if (x1 > W) { x1 = W; }
            if (y1 > H) { y1 = H; }
            visible = (x0 < x1) && (y0 < y1);
            if (!visible) {
                continue;
            }
            if (haveRun && (x0 == rx1) && (y0 == ry0) && (y1 == ry1) && (c->bgR == rr) && (c->bgG == rg) && (c->bgB == rb)) {
                rx1 = x1;   /* extend the run */
                continue;
            }
        }
        if (haveRun) {      /* flush the finished run */
            CogGPU_SetQuad(&v[4 * n], (float) rx0, (float) ry0, (float) rx1, (float) ry1, 0.0f, 0.0f, 0.0f, 0.0f, rr, rg, rb);
            if (++n == COGGPU_MAXQUADS) {
                if (CogGPU_DrawQuads(renderer, NULL, v, n) < 0) {
                    return -1;
                }
                n = 0;
            }
            haveRun = 0;
        }
        if (visible) {
            rx0 = x0; ry0 = y0; rx1 = x1; ry1 = y1;
            rr = c->bgR; rg = c->bgG; rb = c->bgB;
            haveRun = 1;
        }
    }
    return CogGPU_DrawQuads(renderer, NULL, v, n);
}

/* Destination rectangle of the glyph quad of cell c: the cell rectangle shrunk where the glyph rectangle sticks out of the
   atlas (as a software blit clips its source rectangle), then clipped to the screen. 0 if nothing is left or if the
   covered atlas rectangle is entirely blank (coverage 0 everywhere draws nothing: skipping it is exact). */
static int
CogGPU_GlyphRect(const COGGPU_Cell *c, const CogGPUAtlas *a, int cw, int ch, int W, int H, int *x0, int *y0, int *x1, int *y1)
{
    int gx0 = c->dx + ((c->sx < 0) ? -c->sx : 0);
    int gy0 = c->dy + ((c->sy < 0) ? -c->sy : 0);
    int gx1 = c->dx + cw - ((c->sx + cw > a->w) ? (c->sx + cw - a->w) : 0);
    int gy1 = c->dy + ch - ((c->sy + ch > a->h) ? (c->sy + ch - a->h) : 0);
    if (gx0 < 0) { gx0 = 0; }
    if (gy0 < 0) { gy0 = 0; }
    if (gx1 > W) { gx1 = W; }
    if (gy1 > H) { gy1 = H; }
    *x0 = gx0; *y0 = gy0; *x1 = gx1; *y1 = gy1;
    if ((gx0 >= gx1) || (gy0 >= gy1)) {
        return 0;
    }
    {
        const int stride = a->w + 1;
        const int tx0 = c->sx + (gx0 - c->dx), tx1 = c->sx + (gx1 - c->dx);
        const int ty0 = c->sy + (gy0 - c->dy), ty1 = c->sy + (gy1 - c->dy);
        const Uint32 *S = a->sat;
        const Uint32 covered = S[(size_t) ty1 * stride + tx1] - S[(size_t) ty0 * stride + tx1] - S[(size_t) ty1 * stride + tx0] + S[(size_t) ty0 * stride + tx0];
        return covered != 0;
    }
}

/*
 * Glyph pass. Quads cover the (screen-clipped, atlas-clipped) cell rectangle at integer coordinates; the texture
 * coordinates map pixel (x,y) of the quad to atlas texel (sx + x - dx, sy + y - dy): u = (texel + COGGPU_UV_BIAS) / atlasW.
 * The bias keeps (int)(u * W) from truncating one texel too low in the software renderer (which reinterprets these
 * quads as SDL_RenderCopy with a source rect computed that way) and moves the sampling point by 0.01 texel for the GPU
 * renderers, which sample at texel centres (far away from texel borders), so it is invisible there. The far edge is
 * clamped to 1.0 because SDL rejects uv > 1.
 */
static int
CogGPU_DrawGlyphs(SDL_Renderer *renderer, const COGGPU_Cell *cells, int count, int cw, int ch)
{
    const int W = CogGPU.tw, H = CogGPU.th;
    CogGPUVertex *v = CogGPU.verts;
    int *order = CogGPU.order;
    int i, f, total = 0;

    SDL20_memset(CogGPU.fcnt, 0, sizeof (CogGPU.fcnt));
    /* pass A: count visible glyph cells per atlas */
    for (i = 0; i < count; i++) {
        const COGGPU_Cell *c = &cells[i];
        const CogGPUAtlas *a;
        int gx0, gy0, gx1, gy1;
        if ((c->font < 0) || (c->font >= COGGPU_MAXATLAS)) {
            continue;
        }
        a = &CogGPU.atlas[c->font];
        if (!a->tex) {
            continue;
        }
        if (!CogGPU_GlyphRect(c, a, cw, ch, W, H, &gx0, &gy0, &gx1, &gy1)) {
            continue;
        }
        CogGPU.fcnt[c->font + 1]++;
        total++;
    }
    if (!total) {
        return 0;
    }
    for (f = 0; f < COGGPU_MAXATLAS; f++) {   /* prefix sums: fcnt[f] = start of bucket f */
        CogGPU.fcnt[f + 1] += CogGPU.fcnt[f];
    }
    /* pass B: bucket the cell indices (fcnt[f] advances to the end of bucket f; start of f == end of f-1) */
    for (i = 0; i < count; i++) {
        const COGGPU_Cell *c = &cells[i];
        const CogGPUAtlas *a;
        int gx0, gy0, gx1, gy1;
        if ((c->font < 0) || (c->font >= COGGPU_MAXATLAS)) {
            continue;
        }
        a = &CogGPU.atlas[c->font];
        if (!a->tex) {
            continue;
        }
        if (!CogGPU_GlyphRect(c, a, cw, ch, W, H, &gx0, &gy0, &gx1, &gy1)) {
            continue;
        }
        order[CogGPU.fcnt[c->font]++] = i;
    }
    /* now bucket f spans [fcnt[f-1], fcnt[f]) with fcnt[-1] = 0 */
    {
        int start = 0;
        for (f = 0; f < COGGPU_MAXATLAS; f++) {
            const int end = CogGPU.fcnt[f];
            const CogGPUAtlas *a = &CogGPU.atlas[f];
            int k, n = 0;
            for (k = start; k < end; k++) {
                const COGGPU_Cell *c = &cells[order[k]];
                int gx0, gy0, gx1, gy1;
                float u0, v0, u1, v1;
                CogGPU_GlyphRect(c, a, cw, ch, W, H, &gx0, &gy0, &gx1, &gy1);
                u0 = ((float) (c->sx + (gx0 - c->dx)) + COGGPU_UV_BIAS) / (float) a->w;
                v0 = ((float) (c->sy + (gy0 - c->dy)) + COGGPU_UV_BIAS) / (float) a->h;
                u1 = ((float) (c->sx + (gx1 - c->dx)) + COGGPU_UV_BIAS) / (float) a->w;
                v1 = ((float) (c->sy + (gy1 - c->dy)) + COGGPU_UV_BIAS) / (float) a->h;
                if (u1 > 1.0f) { u1 = 1.0f; }
                if (v1 > 1.0f) { v1 = 1.0f; }
                CogGPU_SetQuad(&v[4 * n], (float) gx0, (float) gy0, (float) gx1, (float) gy1, u0, v0, u1, v1, c->fgR, c->fgG, c->fgB);
                if (++n == COGGPU_MAXQUADS) {
                    if (CogGPU_DrawQuads(renderer, a->tex, v, n) < 0) {
                        return -1;
                    }
                    n = 0;
                }
            }
            if (CogGPU_DrawQuads(renderer, a->tex, v, n) < 0) {
                return -1;
            }
            start = end;
        }
    }
    return 0;
}

/* Draw every recorded batch into the GPU screen: ONE render-target round trip for however many COGGPU_End() calls there were.
   Renderer lock held by the caller. */
static void
CogGPU_FlushLocked(SDL_Renderer *renderer)
{
    const SDL_bool logging = CogGPU.logPath[0] ? SDL_TRUE : SDL_FALSE;
    double t0 = 0.0, t1 = 0.0, t2 = 0.0, t3 = 0.0;
    int i, b, failed = 0, total = CogGPU.pend, nbatches = CogGPU.nbatches;

    if (!CogGPU.pend) {
        return;
    }
    if (!CogGPU.target || !CogGPU_EnsureBuffers(CogGPU.maxBatch)) {
        CogGPU.pend = CogGPU.nbatches = CogGPU.maxBatch = 0;
        CogGPU.needFull = 1;
        return;
    }
    if (CogGPU.deviceReset) {   /* textures were recreated by SDL: refill the atlases */
        CogGPU.deviceReset = 0;
        for (i = 0; i < COGGPU_MAXATLAS; i++) {
            if (CogGPU.atlas[i].tex) {
                CogGPU.atlas[i].dirty = 1;
            }
        }
    }
    for (i = 0; i < COGGPU_MAXATLAS; i++) {
        if (CogGPU.atlas[i].tex && CogGPU.atlas[i].dirty) {
            CogGPU_UploadAtlas(&CogGPU.atlas[i]);
        }
    }

    if (logging) {
        t0 = CogGPU_Now();
        CogGPU.tGeom = 0.0;
    }
    if (CogGPU.f.SetRenderTarget(renderer, CogGPU.target) < 0) {
        CogGPU_Log("COGGPU: SetRenderTarget failed: %s", SDL20_GetError());
        failed = 1;
    } else {
        if (logging) {
            t1 = CogGPU_Now();
        }
        CogGPU.f.SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        for (b = 0; b < CogGPU.nbatches; b++) {
            const struct CogGPUBatch *bt = &CogGPU.batches[b];
            const COGGPU_Cell *cells = CogGPU.cells + bt->start;
            if ((CogGPU_DrawBackgrounds(renderer, cells, bt->count, bt->cw, bt->ch) < 0) ||
                (CogGPU_DrawGlyphs(renderer, cells, bt->count, bt->cw, bt->ch) < 0)) {
                CogGPU_Log("COGGPU: geometry failed: %s", SDL20_GetError());
                failed = 1;
                break;
            }
        }
        if (logging) {
            t2 = CogGPU_Now();
        }
        CogGPU.f.SetRenderTarget(renderer, NULL);   /* submits the queued commands */
        if (logging) {
            t3 = CogGPU_Now();
        }
    }
    CogGPU.pend = CogGPU.nbatches = CogGPU.maxBatch = 0;
    if (failed) {
        CogGPU.needFull = 1;   /* we lost cells: make the caller repaint everything next time */
    }
    if (logging) {
        const double ms = t3 - t0;
        CogGPU.winFlushes++;
        CogGPU.winCells += total;
        CogGPU.winFlushMs += ms;
        if (ms > CogGPU.winFlushMax) {
            CogGPU.winFlushMax = ms;
        }
        if ((CogGPU.endCalls <= 8) || ((CogGPU.endCalls % 600) == 0) || ((total >= 2000) && (CogGPU.endCalls <= 400))) {
            CogGPU_Log("COGGPU: End #%d count=%d batches=%d %.3f ms (target in %.3f, geometry calls %.3f, vertex fill %.3f, target out/flush %.3f)%s",
                       CogGPU.endCalls, total, nbatches, ms, t1 - t0, CogGPU.tGeom, t2 - t1 - CogGPU.tGeom, t3 - t2, failed ? " FAILED" : "");
        }
    }
}

static void
CogGPU_Flush(void)
{
    SDL_Renderer *renderer;
    if (!CogGPU.pend) {
        return;
    }
    renderer = CogGPU_Lock();
    if (!renderer) {
        CogGPU.pend = CogGPU.nbatches = CogGPU.maxBatch = 0;
        return;
    }
    CogGPU_FlushLocked(renderer);
    UnlockVideoRenderer();
}

/* Only records: the cells stay in the pool (COGGPU_Begin handed out its tail) and are drawn by the next flush. */
DECLSPEC12 void SDLCALL
COGGPU_End(int count, int cellW, int cellH)
{
    if (!CogGPU.inPass) {
        return;
    }
    CogGPU.inPass = 0;
    if (!CogGPU.target || (count <= 0) || (cellW <= 0) || (cellH <= 0)) {
        return;   /* nothing to draw: costs nothing */
    }
    if (count > CogGPU.passCap) {
        count = CogGPU.passCap;
    }
    if (CogGPU.nbatches == CogGPU.batchCap) {
        const int nc = CogGPU.batchCap ? 2 * CogGPU.batchCap : 8;
        struct CogGPUBatch *nb = (struct CogGPUBatch *) SDL20_realloc(CogGPU.batches, sizeof (*nb) * (size_t) nc);
        if (!nb) {
            CogGPU.needFull = 1;   /* cells lost: make the caller repaint everything */
            return;
        }
        CogGPU.batches = nb;
        CogGPU.batchCap = nc;
    }
    CogGPU.batches[CogGPU.nbatches].start = CogGPU.pend;
    CogGPU.batches[CogGPU.nbatches].count = count;
    CogGPU.batches[CogGPU.nbatches].cw = cellW;
    CogGPU.batches[CogGPU.nbatches].ch = cellH;
    CogGPU.nbatches++;
    CogGPU.dirty = 1;
    CogGPU.pend += count;
    if (count > CogGPU.maxBatch) {
        CogGPU.maxBatch = count;
    }
    CogGPU.endCalls++;
}

DECLSPEC12 int SDLCALL
COGGPU_Readback(void *dst, int pitch, int x, int y, int w, int h)
{
    SDL_Renderer *renderer;
    SDL_Rect r, bounds, clipped;
    int rc;

    if (!CogGPU.target || !dst) {
        return -1;
    }
    if ((w <= 0) || (h <= 0)) {
        return 0;
    }
    r.x = x; r.y = y; r.w = w; r.h = h;
    bounds.x = 0; bounds.y = 0; bounds.w = CogGPU.tw; bounds.h = CogGPU.th;
    if (!SDL20_IntersectRect(&r, &bounds, &clipped)) {
        return 0;
    }
    renderer = LockVideoRenderer();
    if (!renderer || !CogGPU.target) {
        UnlockVideoRenderer();
        return -1;
    }
    CogGPU_FlushLocked(renderer);   /* recorded cells first */
    rc = -1;
    if (CogGPU.f.SetRenderTarget(renderer, CogGPU.target) == 0) {
        rc = CogGPU.f.RenderReadPixels(renderer, &clipped, SDL_PIXELFORMAT_ARGB8888,
                                       (Uint8 *) dst + (size_t) (clipped.y - y) * (size_t) pitch + (size_t) (clipped.x - x) * 4, pitch);
        CogGPU.f.SetRenderTarget(renderer, NULL);
    }
    UnlockVideoRenderer();
    return (rc < 0) ? -1 : 0;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* redirected 1.2 API                                                                                             */
/* ------------------------------------------------------------------------------------------------------------- */

/* SDL_FillRect(screen, ...) while the GPU screen exists. Same return / dstrect semantics as stock. */
static int
CogGPU_FillRect(SDL12_Surface *dst, SDL12_Rect *dstrect12, Uint32 color)
{
    SDL_Rect r, out, whole;
    SDL_Renderer *renderer;
    Uint8 cr, cg, cb;

    whole.x = whole.y = 0;
    whole.w = dst->w;
    whole.h = dst->h;
    if (dstrect12) {
        Rect12to20(dstrect12, &r);
    } else {
        r = whole;
    }
    SDL20_IntersectRect(&r, &dst->surface20->clip_rect, &out);   /* out is zero-sized when nothing is left */
    if (dstrect12) {
        Rect20to12(&out, dstrect12);   /* 1.2 stores the clip intersection in dstrect */
    }
    if ((out.w <= 0) || (out.h <= 0)) {
        return 0;
    }
    SDL_GetRGB(color, dst->format, &cr, &cg, &cb);

    renderer = LockVideoRenderer();
    if (!renderer || !CogGPU.target) {
        UnlockVideoRenderer();
        return SDL20_SetError("GPU screen is gone");
    }
    CogGPU_FlushLocked(renderer);   /* recorded cells first: the fill goes over them */
    if (CogGPU.f.SetRenderTarget(renderer, CogGPU.target) < 0) {
        UnlockVideoRenderer();
        return -1;
    }
    CogGPU.f.SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL20_SetRenderDrawColor(renderer, cr, cg, cb, 255);
    CogGPU.f.RenderFillRect(renderer, &out);
    SDL20_SetRenderDrawColor(renderer, 0, 0, 0, 255);   /* sdl12-compat keeps it black */
    CogGPU.f.SetRenderTarget(renderer, NULL);
    CogGPU.dirty = 1;
    UnlockVideoRenderer();
    return 0;
}

/* Copy the GPU screen's rectangle `rect` (already inside the screen) into the CPU screen surface. */
static int
CogGPU_ReadbackToCPU(const SDL_Rect *rect)
{
    SDL_Surface *tmp;
    SDL_Rect clipsave, dr;
    Uint8 *buf;
    int rc;
    const int pitch = rect->w * 4;

    buf = (Uint8 *) SDL20_malloc((size_t) pitch * (size_t) rect->h);
    if (!buf) {
        return SDL20_OutOfMemory();
    }
    if (COGGPU_Readback(buf, pitch, rect->x, rect->y, rect->w, rect->h) < 0) {
        SDL20_free(buf);
        return SDL20_SetError("GPU screen readback failed");
    }
    tmp = SDL20_CreateRGBSurfaceFrom(buf, rect->w, rect->h, 32, pitch, 0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0);
    if (!tmp) {
        SDL20_free(buf);
        return -1;
    }
    SDL20_GetClipRect(VideoSurface12->surface20, &clipsave);
    SDL20_SetClipRect(VideoSurface12->surface20, NULL);
    dr = *rect;
    rc = SDL20_UpperBlit(tmp, NULL, VideoSurface12->surface20, &dr);
    SDL20_SetClipRect(VideoSurface12->surface20, &clipsave);
    SDL20_FreeSurface(tmp);
    SDL20_free(buf);
    return rc;
}

/* SDL_UpperBlit(src, srcrect, screen, dstrect) while the GPU screen exists: upload the source rectangle to a
   temporary texture and draw it 1:1. Rare path (the console never goes through here). */
static int
CogGPU_BlitToScreen(SDL12_Surface *src12, SDL12_Rect *srcrect12, SDL12_Surface *dst12, SDL12_Rect *dstrect12)
{
    SDL_Rect sr, dr, clip;
    SDL_Surface *tmp;
    SDL_Texture *tex;
    SDL_Renderer *renderer;
    SDL_BlendMode oldblend = SDL_BLENDMODE_NONE;
    Uint8 oldalpha = 255;
    const char *old_scale_quality;
    SDL_bool perPixelAlpha, wholeAlpha, keyed;
    int rc = 0;

    /* source rectangle, clipped to the source surface */
    sr.x = sr.y = 0; sr.w = src12->w; sr.h = src12->h;
    if (srcrect12) {
        SDL_Rect want, whole = sr;
        Rect12to20(srcrect12, &want);
        if (!SDL20_IntersectRect(&want, &whole, &sr)) {
            sr.w = sr.h = 0;
        }
    }
    dr.x = dstrect12 ? dstrect12->x : 0;
    dr.y = dstrect12 ? dstrect12->y : 0;
    /* a source clipped on the left/top shifts the destination */
    if (srcrect12) {
        if (srcrect12->x < 0 && sr.w > 0) { dr.x += sr.x - srcrect12->x; }
        if (srcrect12->y < 0 && sr.h > 0) { dr.y += sr.y - srcrect12->y; }
    }
    /* destination clip rectangle */
    clip = dst12->surface20->clip_rect;
    if (dr.x < clip.x) { const int d = clip.x - dr.x; sr.x += d; sr.w -= d; dr.x = clip.x; }
    if (dr.y < clip.y) { const int d = clip.y - dr.y; sr.y += d; sr.h -= d; dr.y = clip.y; }
    if (dr.x + sr.w > clip.x + clip.w) { sr.w = clip.x + clip.w - dr.x; }
    if (dr.y + sr.h > clip.y + clip.h) { sr.h = clip.y + clip.h - dr.y; }
    if ((sr.w <= 0) || (sr.h <= 0)) {
        if (dstrect12) { dstrect12->w = dstrect12->h = 0; }
        return 0;
    }
    dr.w = sr.w;
    dr.h = sr.h;
    if (dstrect12) {
        Rect20to12(&dr, dstrect12);
    }

    perPixelAlpha = ((src12->flags & SDL12_SRCALPHA) && src12->format->Amask) ? SDL_TRUE : SDL_FALSE;
    wholeAlpha = ((src12->flags & SDL12_SRCALPHA) && !src12->format->Amask && (src12->format->alpha != 255)) ? SDL_TRUE : SDL_FALSE;
    keyed = ((src12->flags & SDL12_SRCCOLORKEY) && !src12->format->Amask) ? SDL_TRUE : SDL_FALSE;

    tmp = SDL20_CreateRGBSurfaceWithFormat(0, sr.w, sr.h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!tmp) {
        return -1;
    }
    SDL20_FillRect(tmp, NULL, 0);   /* transparent: colour-keyed pixels are skipped by the blit */
    /* copy raw pixels: no blending, no alpha modulation (the GPU does that when drawing the texture) */
    SDL20_GetSurfaceBlendMode(src12->surface20, &oldblend);
    SDL20_GetSurfaceAlphaMod(src12->surface20, &oldalpha);
    SDL20_SetSurfaceBlendMode(src12->surface20, SDL_BLENDMODE_NONE);
    SDL20_SetSurfaceAlphaMod(src12->surface20, 255);
    rc = SDL20_UpperBlit(src12->surface20, &sr, tmp, NULL);
    SDL20_SetSurfaceBlendMode(src12->surface20, oldblend);
    SDL20_SetSurfaceAlphaMod(src12->surface20, oldalpha);
    if (rc < 0) {
        SDL20_FreeSurface(tmp);
        return rc;
    }

    renderer = LockVideoRenderer();
    if (!renderer || !CogGPU.target) {
        UnlockVideoRenderer();
        SDL20_FreeSurface(tmp);
        return SDL20_SetError("GPU screen is gone");
    }
    CogGPU_FlushLocked(renderer);   /* recorded cells first: the blit goes over them */
    old_scale_quality = SDL20_GetHint(SDL_HINT_RENDER_SCALE_QUALITY);
    SDL20_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    tex = SDL20_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, sr.w, sr.h);
    SDL20_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, old_scale_quality);
    if (!tex) {
        UnlockVideoRenderer();
        SDL20_FreeSurface(tmp);
        return -1;
    }
    rc = SDL20_UpdateTexture(tex, NULL, tmp->pixels, tmp->pitch);
    if (rc == 0) {
        CogGPU.f.SetTextureBlendMode(tex, (perPixelAlpha || wholeAlpha || keyed) ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
        CogGPU.f.SetTextureAlphaMod(tex, wholeAlpha ? src12->format->alpha : (Uint8) 255);
        rc = CogGPU.f.SetRenderTarget(renderer, CogGPU.target);
        if (rc == 0) {
            rc = SDL20_RenderCopy(renderer, tex, NULL, &dr);
            CogGPU.f.SetRenderTarget(renderer, NULL);
        }
    }
    SDL20_DestroyTexture(tex);
    CogGPU.dirty = 1;
    UnlockVideoRenderer();
    SDL20_FreeSurface(tmp);
    return (rc < 0) ? -1 : 0;
}

/*
 * SDL_UpperBlit hook. Returns SDL_TRUE and sets *retval when the blit was done here; SDL_FALSE to let the stock code run
 * (after making the CPU screen surface current when the screen is the source).
 */
static SDL_bool
CogGPU_UpperBlit(SDL12_Surface *src12, SDL12_Rect *srcrect12, SDL12_Surface *dst12, SDL12_Rect *dstrect12, int *retval)
{
    if (src12 == VideoSurface12) {
        SDL_Rect want, whole, r;
        whole.x = whole.y = 0; whole.w = VideoSurface12->w; whole.h = VideoSurface12->h;
        if (srcrect12) {
            Rect12to20(srcrect12, &want);
        } else {
            want = whole;
        }
        if (SDL20_IntersectRect(&want, &whole, &r) && (r.w > 0) && (r.h > 0)) {
            if (CogGPU_ReadbackToCPU(&r) < 0) {
                *retval = -1;
                return SDL_TRUE;
            }
        }
    }
    if (dst12 == VideoSurface12) {
        *retval = CogGPU_BlitToScreen(src12, srcrect12, dst12, dstrect12);
        return SDL_TRUE;
    }
    return SDL_FALSE;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* present + dump                                                                                                 */
/* ------------------------------------------------------------------------------------------------------------- */

static void
CogGPU_MaybeDump(void)
{
    char req[400], bmp[400], ack[400];
    Uint8 hdr[54];
    Uint8 *buf;
    int w, h, pitch, ok = 0;
    Uint32 imgsize, filesize;

    if (!CogGPU.dumpDir[0] || !VideoSurface12 || !VideoSurface12->surface20 || (VideoSurface12->flags & SDL12_OPENGL)) {
        return;
    }
    SDL20_snprintf(req, sizeof (req), "%s\\dump.req", CogGPU.dumpDir);
    if (!CogGPU_FileExists(req)) {
        return;
    }
    SDL20_snprintf(bmp, sizeof (bmp), "%s\\frame.bmp", CogGPU.dumpDir);
    SDL20_snprintf(ack, sizeof (ack), "%s\\dump.ack", CogGPU.dumpDir);

    w = VideoSurface12->w;
    h = VideoSurface12->h;
    pitch = w * 4;
    buf = (Uint8 *) SDL20_malloc((size_t) pitch * (size_t) h);
    if (!buf) {
        return;
    }
    if (CogGPU.target) {
        ok = (COGGPU_Readback(buf, pitch, 0, 0, w, h) == 0);
    } else if (VideoSurface12->pixels) {
        CogGPU_SurfaceToXRGB(VideoSurface12, buf, pitch, 0, 0, w, h);
        ok = 1;
    }
    if (ok) {
        int i;
        for (i = 3; i < pitch * h; i += 4) {
            buf[i] = 0xFF;
        }
        imgsize = (Uint32) pitch * (Uint32) h;
        filesize = 54 + imgsize;
        SDL20_memset(hdr, 0, sizeof (hdr));
        hdr[0] = 'B'; hdr[1] = 'M';
        hdr[2] = (Uint8) filesize; hdr[3] = (Uint8) (filesize >> 8); hdr[4] = (Uint8) (filesize >> 16); hdr[5] = (Uint8) (filesize >> 24);
        hdr[10] = 54;
        hdr[14] = 40;                                   /* BITMAPINFOHEADER */
        hdr[18] = (Uint8) w; hdr[19] = (Uint8) (w >> 8); hdr[20] = (Uint8) (w >> 16); hdr[21] = (Uint8) (w >> 24);
        { const Sint32 nh = -h;                          /* negative height: top-down rows */
          hdr[22] = (Uint8) nh; hdr[23] = (Uint8) (nh >> 8); hdr[24] = (Uint8) (nh >> 16); hdr[25] = (Uint8) (nh >> 24); }
        hdr[26] = 1;
        hdr[28] = 32;
        hdr[34] = (Uint8) imgsize; hdr[35] = (Uint8) (imgsize >> 8); hdr[36] = (Uint8) (imgsize >> 16); hdr[37] = (Uint8) (imgsize >> 24);
        ok = CogGPU_WriteFile(bmp, hdr, 54, buf, (int) imgsize);
    }
    SDL20_free(buf);
    CogGPU_Log("COGGPU: dump %s (%s)", bmp, ok ? "ok" : "FAILED");
    CogGPU_DeleteFile(req);
    if (ok) {
        CogGPU_WriteFile(ack, "ok", 2, NULL, 0);
    }
}

/* PresentScreen hooks, in call order. Renderer lock held by the caller.
   1. before the window is cleared: starts the present timer and, in GPU mode,
      - returns SDL_TRUE (= skip this present entirely) when nothing changed since the last one: no cell was recorded, nothing
        else touched the screen, no window event arrived and no YUV overlay is queued. The window keeps showing the last frame, so
        an idle game costs nothing here. A real present is still forced at least every COGGPU_HEARTBEAT_MS.
      - otherwise draws the recorded console cells into the GPU screen (one render-target round trip) and returns SDL_FALSE. */
#define COGGPU_HEARTBEAT_MS 250
static SDL_bool
CogGPU_PresentBegin(SDL_Renderer *renderer)
{
    CogGPU_InitEnv();
    if (CogGPU.logPath[0]) {
        CogGPU.tPresent0 = CogGPU_Now();
    }
    if (!CogGPU.target) {
        return SDL_FALSE;
    }
    if (!CogGPU.dirty && !CogGPU.pend && !QueuedDisplayOverlays.next) {
        const Uint32 now = SDL20_GetTicks();
        SDL_bool force = SDL_TRUE;
        if (!SDL_TICKS_PASSED(now, VideoSurfaceLastPresentTicks + COGGPU_HEARTBEAT_MS)) {
            force = SDL_FALSE;
            if (CogGPU.dumpDir[0] && SDL_TICKS_PASSED(now, CogGPU.dumpPoll + 100)) {   /* a pending dump request needs a present */
                char req[400];
                CogGPU.dumpPoll = now;
                SDL20_snprintf(req, sizeof (req), "%s\\dump.req", CogGPU.dumpDir);
                force = CogGPU_FileExists(req) ? SDL_TRUE : SDL_FALSE;
            }
        }
        if (!force) {
            if (CogGPU.logPath[0]) {
                CogGPU.winSkipped++;
                CogGPU_PresentDone();
            }
            return SDL_TRUE;
        }
    }
    CogGPU_FlushLocked(renderer);
    CogGPU.dirty = 0;
    return SDL_FALSE;
}

/* 2. instead of copying VideoTexture20: handles the diagnostics dump and, in GPU mode, draws the GPU screen. SDL_FALSE = the stock
   code copies VideoTexture20. */
static SDL_bool
CogGPU_Present(SDL_Renderer *renderer)
{
    if (CogGPU.dumpDir[0]) {
        CogGPU_MaybeDump();
    }
    if (!CogGPU.target) {
        return SDL_FALSE;
    }
    SDL20_RenderCopy(renderer, CogGPU.target, NULL, NULL);
    return SDL_TRUE;
}

/* 3. in place of SDL_RenderPresent(): same call, plus the COGGPU_LOG statistics (both in GPU and in software mode, so the two
   can be compared): every ~2 s one PRESENT line (PresentScreen calls, fps, mean cost of PresentScreen in ms incl. drawing the
   recorded cells; skipped presents count as frames with ~0 ms), and in GPU mode one FLUSH line (cell drawing) and, if any,
   one SKIP line. Two clock reads per present when COGGPU_LOG is set, none otherwise. */
static void
CogGPU_PresentDone(void)
{
    const double now = CogGPU_Now();
    if (CogGPU.winT0 == 0.0) {
        CogGPU.winT0 = CogGPU.tPresent0;
    }
    CogGPU.winPresents++;
    CogGPU.winPresentMs += now - CogGPU.tPresent0;
    if (now - CogGPU.winT0 >= 2000.0) {
        const double secs = (now - CogGPU.winT0) / 1000.0;
        CogGPU_Log("PRESENT frames=%d fps=%.1f avgFlipMs=%.3f gpuScreen=%d", CogGPU.winPresents, CogGPU.winPresents / secs,
                   CogGPU.winPresentMs / CogGPU.winPresents, CogGPU.target ? 1 : 0);
        if (CogGPU.winFlushes) {
            CogGPU_Log("FLUSH flushes=%d avgCells=%.1f avgFlushMs=%.3f maxFlushMs=%.3f", CogGPU.winFlushes,
                       (double) CogGPU.winCells / CogGPU.winFlushes, CogGPU.winFlushMs / CogGPU.winFlushes, CogGPU.winFlushMax);
        }
        if (CogGPU.winSkipped) {
            CogGPU_Log("SKIP presents=%d", CogGPU.winSkipped);
        }
        CogGPU.winT0 = now;
        CogGPU.winPresents = CogGPU.winFlushes = CogGPU.winCells = CogGPU.winSkipped = 0;
        CogGPU.winPresentMs = CogGPU.winFlushMs = CogGPU.winFlushMax = 0.0;
    }
}

static void
CogGPU_RenderPresent(SDL_Renderer *renderer)
{
    SDL20_RenderPresent(renderer);
    if (CogGPU.logPath[0]) {
        CogGPU_PresentDone();
    }
}
