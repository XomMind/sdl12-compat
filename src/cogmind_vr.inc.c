/*
 * cogmind_vr.inc.c -- in-process 3D / VR map view ("Luigi's Mansion mode") of the Cogmind shim (SDG.dll).
 * #included at the end of SDL12_compat.c, after cogmind_gpu.inc.c (shares its logging/file helpers). Contract: cogmind_vr.h.
 *
 * Game thread: COGVR_Begin/COGVR_End copy one snapshot of the world (one COGVR_Cell per map cell) into a triple buffer guarded
 * by an SDL mutex, at most COGVR_MAXRATE times per second. Nothing else happens on the game thread.
 * Render thread (started on the first COGVR_Begin): owns D3D11 (d3d11.dll / dxgi.dll / d3dcompiler_47.dll, loaded at run time),
 * the preview window and OpenVR (openvr_api.dll, loaded at run time, C function tables of openvr_capi.h). It never calls SDL
 * video/renderer functions (they belong to the game thread) and never touches game memory.
 *
 * Environment:
 *   COGVR=desktop        window with a free-fly camera (WASD, Q/E down/up, Shift fast, hold RMB + mouse look, wheel = world
 *                        scale, Home = recenter above the player, P / Tab = Examine <-> Cogmind POV, C = free-fly view <->
 *                        the director's stream camera, the avatar standing in for the headset user at the free-fly camera)
 *   COGVR=openvr         SteamVR headset: sticky grip grabs reality; two grips scale/turn; sticks fly/smooth turn;
 *                        menu tap = Examine/POV, hold = reset. Actions map back to legacy SDL input.
 *                        The separate stream window shows the director's camera and the rigged avatar.
 *   COGVR_AVATAR=<file>  self-contained skinned GLB of the headset user; else a built-in robot
 *   COGVR_STREAM_SIZE=WxH  stream camera render size (default 1280x720)
 *   COGVR_LOG=<file>     append diagnostics (init, adapter, eye size, avatar, director cuts; every 2 s fps / snapshot / boxes)
 *   COGVR_DUMP=<dir>     when <dir>\dump.req exists: write <dir>\vr.bmp (the window's view in desktop mode / left eye) and in
 *                        openvr mode <dir>\stream.bmp, then dump.ack, delete dump.req
 * The director (stream camera) cuts on the music: AudioCallbackWrapper passes the mixed output to CogVR_AudioTap (read only).
 *
 * Scene: world cell = 1 unit (x = map x, z = map y, y up). Every known cell gets a thin floor tile; its glyph (8x8 public-domain
 * font, per glyph precomputed merged boxes) is extruded: terrain lies flat (walls tall, doors medium, the rest low), robots /
 * items / the player stand upright, hover, bob and turn to the viewer (vertex shader). One instanced unit cube: one draw call
 * for the map (rebuilt per snapshot, level of detail chosen to fit COGVR_MAXINST), one per batch of per-frame boxes (avatar,
 * controllers; separate dynamic buffer). HDR scene, reversed-Z depth, MSAA 4x when available, bloom, tone map to sRGB targets.
 */

#ifdef _WIN32

#define COBJMACROS
#include <d3d11.h>
#include <d3dcompiler.h>

/* OpenVR C API (openvr_capi.h, BSD-3, pinned by gpu/build.sh). Only its types and the FnTable layouts are used; the DLL is
   loaded with LoadLibrary. The header is meant for C but typedefs `bool` (a keyword in C23) and redefines EXTERN_C. */
#pragma push_macro("EXTERN_C")
#undef EXTERN_C
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#define OPENVR_API_NODLL
#define bool CogVR_bool
#include "openvr_capi.h"
#undef bool
#pragma GCC diagnostic pop
#undef EXTERN_C
#pragma pop_macro("EXTERN_C")

/* dhepper/font8x8 font8x8_basic.h (public domain, pinned by gpu/build.sh): char font8x8_basic[128][8], row 0 = top, bit 0 = left */
#define font8x8_basic CogVR_font8x8
#include "font8x8_basic.h"
#undef font8x8_basic

#endif

#include "cogmind_vr.h"

#define COGVR_MAXDIM   1024
#define COGVR_MAXRATE  30.0             /* snapshots per second */
#define COGVR_MAXINST  100000           /* map boxes; more are dropped (logged) */
#define COGVR_MAXDYN   12288            /* per-frame boxes (avatar, controllers, deck, stand text), separate dynamic buffer */
#define COGVR_MAXBATCH 16
#define COGVR_SCALE_MIN 0.0005f        /* numerical bounds: giant tabletop to tiny fairy */
#define COGVR_SCALE_MAX 100.0f

typedef struct CogVRSlot {
    COGVR_Cell *cells;
    int cap;                           /* cells */
    COGVR_Header hdr;
    Uint32 version;
} CogVRSlot;

enum { COGVR_OFF = 0, COGVR_STARTING, COGVR_RUNNING, COGVR_FAILED, COGVR_STOPPED };

#ifdef _WIN32

typedef struct CogV3 { float x, y, z; } CogV3;
typedef struct CogM4 { float m[4][4]; } CogM4;   /* row-major, column vectors: p' = M p (HLSL: row_major + mul(M, p)) */

typedef struct CogVRInst {             /* 48 bytes, input slot 1 */
    float ax, ay, az, kind;            /* anchor (cell space); kind 0 static, 1 upright glyph, 2 halo */
    float lx, ly, lz, phase;           /* offset from the anchor (glyph plane for kind 1; radius/height for kind 2) */
    float hx, hy, hz;                  /* box half size */
    Uint32 rgba;                       /* sRGB colour, alpha = emissive */
} CogVRInst;

typedef struct CogVRFrameCB {
    CogM4 viewProj;                    /* metres -> clip (reversed Z) */
    CogM4 model;                       /* cell space (or controller space) -> metres */
    float camCell[4];                  /* viewer in cell space (billboards), w = time (s) */
    float eyeM[4];                     /* viewer in metres (fog), w = fog density per metre */
    float fog[4];                      /* fog colour (linear), w = emissive gain */
    float light[4];                    /* direction to the light, w = ambient */
} CogVRFrameCB;

/* One rendered view: the scene in HDR (MSAA, resolved), a quarter-size bloom ping-pong pair, and the final tone-mapped sRGB
   texture(s) (two for the eyes: the HDR/bloom targets are shared, the eyes are rendered one after the other). */
typedef struct CogVRView {
    int w, h, samples, bw, bh;
    ID3D11Texture2D *msaa;             /* HDR scene, MSAA; NULL when samples == 1 (then the scene is drawn into hdr) */
    ID3D11RenderTargetView *msaaRTV;
    ID3D11Texture2D *hdr;
    ID3D11RenderTargetView *hdrRTV;
    ID3D11ShaderResourceView *hdrSRV;
    ID3D11Texture2D *depth;
    ID3D11DepthStencilView *dsv;
    ID3D11Texture2D *bloom[2];
    ID3D11RenderTargetView *bloomRTV[2];
    ID3D11ShaderResourceView *bloomSRV[2];
    ID3D11Texture2D *res[2];           /* final (eye) textures, R8G8B8A8_UNORM_SRGB */
    ID3D11RenderTargetView *resRTV[2];
    ID3D11ShaderResourceView *resSRV[2];
    int nres;
} CogVRView;

#define COGVR_HDR_FORMAT DXGI_FORMAT_R16G16B16A16_FLOAT

/*
 * Camera module. A camera produces a shot (cell space) from its state, the target (cell space position + velocity) and a
 * scene query; the caller turns the shot into view/projection matrices for its world transform.
 *   COGVR_CAM_FREE      desktop free-fly: pos/yaw/pitch are driven by the window's input (CogVR_DeskInput)
 *   COGVR_CAM_DIRECTOR  the stream camera's auto-director (see "director" below): locked-off shots, gentle pans only when the
 *                       subject leaves the central deadzone, hard cuts on the music's 4-beat boundaries, shot choice from
 *                       the snapshot's motion energy near the Cogmind player
 */
enum { COGVR_CAM_FREE = 0, COGVR_CAM_DIRECTOR };
enum { COGVR_SHOT_CHASE = 0, COGVR_SHOT_WIDE, COGVR_SHOT_LOW, COGVR_SHOT_TOP, COGVR_SHOT_TWO };

typedef struct CogVRShot {
    CogV3 eye, fwd, right, up;         /* cell space, orthonormal basis */
    float fovY;                        /* radians */
} CogVRShot;

typedef float (*CogVRWallQuery)(int x, int y);   /* extrusion height of the terrain at a world cell (0 = nothing / unknown) */

typedef struct CogVRCam {
    int kind;
    CogV3 pos;                         /* eye, cell space (DIRECTOR: fixed for the whole shot) */
    float yaw, pitch;                  /* FREE: look direction; yaw 0 = north (-z) */
    /* DIRECTOR inputs, set by the caller before each update */
    float heading;                     /* the avatar's facing (yaw, 0 = north) */
    CogV3 subject2;                    /* the Cogmind player cell (cell space), valid when hasSubject2 */
    int hasSubject2;
    float aspect;                      /* of the stream view */
    /* DIRECTOR state */
    int init, shot, shotN, calmN, lowOnPlayer, panning;
    float fov, shotLen, occT, offT, startDist;
    CogV3 aim, aimV;                   /* look-at point (cell space) and its spring velocity */
    double shotStart, lastCut;         /* ms (CogGPU_Now) */
    int pend, pendShot, pendLow;       /* a requested cut waiting for its beat / onset */
    const char *pendReason;
    double pendSince, pendAt;          /* pendAt < 0: no confident tempo, wait for an onset */
    Uint32 seed;
} CogVRCam;

typedef struct CogVRBatch {            /* a run of boxes drawn with its own model matrix (box space -> metres) */
    CogM4 model;
    int first, count;
    int inEyes;                        /* 0: stream camera only (the avatar); 1: everywhere; 2: headset/desktop view only */
} CogVRBatch;

typedef HRESULT (WINAPI *CogCreateDXGIFactory1_t)(REFIID, void **);
typedef intptr_t (*CogVRInit2_t)(EVRInitError *, EVRApplicationType, const char *);
typedef intptr_t (*CogVRGetGeneric_t)(const char *, EVRInitError *);
typedef void (*CogVRShutdown_t)(void);
typedef const char *(*CogVRErrDesc_t)(EVRInitError);

static const GUID CogVR_IID_IDXGIFactory1 = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const GUID CogVR_IID_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };
static const GUID CogVR_IID_IDXGIDevice = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };
static const GUID CogVR_IID_IDXGIAdapter1 = { 0x29038f61, 0x3839, 0x4626, { 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05 } };

#define COGVR_REL(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

#endif /* _WIN32 */

static struct CogVRState {
    /* game thread */
    int envInit;
    int mode;                          /* 0 off, 1 desktop, 2 openvr */
    char logPath[300];
    char dumpDir[300];
    char avatarPath[300];              /* COGVR_AVATAR */
    int streamW, streamH;              /* COGVR_STREAM_SIZE, default 1280x720 */
    int snapDeg;                       /* COGVR_SNAPTURN=<deg>: snap turn; 0 = smooth turn */
    float startScale;                  /* COGVR_SCALE=<metres per cell>: headset start scale; 0 = tabletop */
    int companion;                     /* COGVR_COMPANION=1: in Examine the left stick moves Cogmind (head-relative), not you */
    int stickyGrip, stickyRaw[2], stickyState[2], cruise, boostWas;
    float cruiseX, cruiseY, cruiseV, vignetteStrength, vignette, postEye;
    int started;
    int inPass, passW, passH;
    double nextSlot;
    Uint32 version;
    SDL_mutex *mutex;
    /* shared (mutex) */
    CogVRSlot slot[3];
    int wIdx, rIdx, dIdx;              /* write (game), ready (latest published), draw (renderer) */
    int fresh;                         /* ready holds a snapshot the renderer has not taken */
    volatile int rstate;

#ifdef _WIN32
    /* render thread only */
    float (SDLCALL *sinf_)(float);
    float (SDLCALL *cosf_)(float);
    float (SDLCALL *tanf_)(float);
    float (SDLCALL *sqrtf_)(float);
    float (SDLCALL *atan2f_)(float, float);
    float (SDLCALL *logf_)(float);     /* also used by the audio tap (resolved before the renderer runs) */
    HMODULE hD3D, hDXGI, hComp, hVR;
    IDXGIFactory1 *factory;
    IDXGIAdapter1 *adapter;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11VertexShader *vs, *vsBlit;
    ID3D11PixelShader *ps, *psBlit, *psBright, *psBlur, *psComp;
    ID3D11InputLayout *layout;
    ID3D11Buffer *cubeVB, *cubeIB, *instVB, *dynVB, *cb, *cbPost;
    ID3D11RasterizerState *rsScene, *rsBlit;
    ID3D11DepthStencilState *dsScene, *dsBlit;
    ID3D11SamplerState *sampler;
    int msaa;
    CogVRView view;                    /* desktop view or the two eyes */
    CogVRView streamView;              /* the director's stream camera (openvr: always; desktop: key C) */
    ID3D11Texture2D *staging;
    int stagingW, stagingH;

    HWND hwnd;
    IDXGISwapChain *swap;
    ID3D11RenderTargetView *backRTV;
    int winW, winH;
    volatile int resized, quit;

    /* scene */
    CogVRInst *inst;
    int ninst, overflow, loggedOverflow;
    int countOnly;                     /* CogVR_Push only counts (level-of-detail selection) */
    int lod;                           /* level of detail of the built scene */
    CogVRInst dyn[COGVR_MAXDYN];       /* per-frame boxes */
    int ndyn;
    CogVRBatch batch[COGVR_MAXBATCH];
    int nbatch;
    int sceneDirty;
    Uint32 drawnVersion;
    COGVR_Header hdr;                  /* header of the drawn snapshot */
    Uint8 glyphN[128];
    Uint8 glyphBox[128][32][4];        /* c0, c1, r0, r1 */
    Uint8 glyphBB[128][4];             /* min col, max col, min row, max row */

    /* cameras; world -> metres: p_m = Ry(wYaw) (p * wScale) + wT (desktop: wYaw = 0, wT = 0) */
    CogVRCam desk, stream;
    float wYaw, wScale;
    CogV3 wT;
    int pov;
    float povSavedScale;
    int autoCentered;
    int rmb, mouseX, mouseY;
    int mouseDX, mouseDY, wheel, keyHome, keyPov, keyStream;
    Uint8 keys[256];                   /* window key state from WM_KEYDOWN/UP (works unfocused for posted messages too) */
    int deskStream;                    /* desktop: the window shows the director's stream camera (key C) */
    int streamFrame;
    float streamDt;                    /* time since the last stream camera frame */

    CogV3 avVel;                       /* smoothed velocity relative to the world, tracking space (m/s) */
    CogV3 avPrevCell;
    int avPrevValid;

    /* motion energy of the snapshots near the Cogmind player (director's action detector) */
    Uint32 *prevKey;
    int prevW, prevH;
    float energyFast, energySlow;
    double energyT;

    /* OpenVR */
    struct VR_IVRSystem_FnTable *vrSys;
    struct VR_IVRCompositor_FnTable *vrComp;
    CogVRShutdown_t vrShutdown;
    CogM4 eyeProj[2];
    uint32_t eyeW, eyeH;
    TrackedDevicePose_t poses[64];
    struct CogVRHand {
        TrackedDeviceIndex_t idx;
        int axis;                      /* legacy axis of the thumbstick */
        int valid, grip;
        uint64_t prevPressed;
        CogV3 pos, fwd;
        CogM4 pose;
        VRControllerState_t st;
    } hand[2];
    int grabMode;                      /* 0 none, 1 one hand (grabHand), 2 both */
    int grabHand;
    CogV3 grab0[2];
    float g0Yaw, g0Scale;
    CogV3 g0T;
    int snapArmed;
    int povPX, povPY;
    CogV3 seat;                        /* the floating chair under the player (tracking space), placed at recenter */
    float seatYaw;
    /* SteamVR Input (action manifest next to the exe); NULL: legacy GetControllerState */
    struct VR_IVRInput_FnTable *vrIn;
    VRActionSetHandle_t inSet;
    VRActionHandle_t aGrab, aMenu, aFly, aTurn, aBoost, aTrigger, aA, aSqueeze, aPose[2], aHaptic[2];
    VRInputValueHandle_t inSrc[2];
    struct CogVRPad {                  /* this frame's controls, from either API */
        int grab[2], menu, boost;
        float squeeze[2];              /* grip force 0..1 (SteamVR actions; legacy: grip button = 1) */
        float flyX, flyY, turnX, turnY;
    } pad;
    int padPrevGrab[2];
    double menuT0;
    int menuWas, menuFired;
    int clampState;                    /* -1 at the smallest scale, 1 at the largest */
    int turning, flying, flyBoost;
    double turnT0, flyT0;
    float turnAcc, flyAcc;

    /* stats */
    double t0, statT0;
    int statFrames;
    double dumpPoll;
#endif
} CogVR;

static void
CogVR_Log(const char *fmt, ...)
{
#ifdef _WIN32
    char buf[600];
    va_list ap;
    HANDLE h;
    DWORD wrote;
    int n;

    if (!CogVR.logPath[0]) {
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
    h = CreateFileA(CogVR.logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, buf, (DWORD) n, &wrote, NULL);
        CloseHandle(h);
    }
#else
    (void) fmt;
#endif
}

static void
CogVR_InitEnv(void)
{
    const char *v;
    if (CogVR.envInit) {
        return;
    }
    CogVR.envInit = 1;
    v = SDL12COMPAT_getenv_unsafe("COGVR_LOG");
    if (v && *v) {
        SDL20_snprintf(CogVR.logPath, sizeof (CogVR.logPath), "%s", v);
    }
    v = SDL12COMPAT_getenv_unsafe("COGVR_DUMP");
    if (v && *v) {
        SDL20_snprintf(CogVR.dumpDir, sizeof (CogVR.dumpDir), "%s", v);
    }
    v = SDL12COMPAT_getenv_unsafe("COGVR_AVATAR");
    if (v && *v) {
        SDL20_snprintf(CogVR.avatarPath, sizeof (CogVR.avatarPath), "%s", v);
    }
    CogVR.streamW = 1280;
    CogVR.streamH = 720;
    v = SDL12COMPAT_getenv_unsafe("COGVR_STREAM_SIZE");
    if (v && *v) {   /* WxH */
        int w = 0, h = 0;
        while ((*v >= '0') && (*v <= '9')) {
            w = w * 10 + (*v++ - '0');
        }
        if ((*v == 'x') || (*v == 'X')) {
            v++;
            while ((*v >= '0') && (*v <= '9')) {
                h = h * 10 + (*v++ - '0');
            }
        }
        if ((w >= 160) && (h >= 90) && (w <= 7680) && (h <= 4320)) {
            CogVR.streamW = w;
            CogVR.streamH = h;
        } else {
            CogVR_Log("COGVR: ignoring COGVR_STREAM_SIZE (want WxH, 160x90 .. 7680x4320)");
        }
    }
    v = SDL12COMPAT_getenv_unsafe("COGVR_SNAPTURN");
    if (v && *v) {
        CogVR.snapDeg = SDL20_atoi(v);
        if ((CogVR.snapDeg < 0) || (CogVR.snapDeg > 180)) {
            CogVR.snapDeg = 0;
        }
    }
    v = SDL12COMPAT_getenv_unsafe("COGVR_STICKY_GRIP");
    CogVR.stickyGrip = v ? (SDL20_atoi(v) != 0) : 1;
    v = SDL12COMPAT_getenv_unsafe("COGVR_VIGNETTE");
    CogVR.vignetteStrength = v ? (float) SDL20_strtod(v, NULL) : 0.35f;
    if (CogVR.vignetteStrength < 0.0f) { CogVR.vignetteStrength = 0.0f; }
    if (CogVR.vignetteStrength > 1.0f) { CogVR.vignetteStrength = 1.0f; }
    v = SDL12COMPAT_getenv_unsafe("COGVR_SCALE");
    if (v && *v) {
        const float s = (float) SDL20_strtod(v, NULL);
        if ((s >= COGVR_SCALE_MIN) && (s <= COGVR_SCALE_MAX)) {
            CogVR.startScale = s;
        } else {
            CogVR_Log("COGVR: ignoring COGVR_SCALE=%s (want %g .. %g metres per cell)", v, COGVR_SCALE_MIN, COGVR_SCALE_MAX);
        }
    }
    v = SDL12COMPAT_getenv_unsafe("COGVR_COMPANION");
    CogVR.companion = v && (SDL20_atoi(v) != 0);
    v = SDL12COMPAT_getenv_unsafe("COGVR");
    if (v && !SDL20_strcasecmp(v, "desktop")) {
        CogVR.mode = 1;
    } else if (v && !SDL20_strcasecmp(v, "openvr")) {
        CogVR.mode = 2;
    } else if (v && *v) {
        CogVR_Log("COGVR: unknown COGVR=%s (want desktop or openvr): off", v);
    }
}

#ifdef _WIN32

/* ------------------------------------------------------------------------------------------------------------- */
/* math                                                                                                           */
/* ------------------------------------------------------------------------------------------------------------- */

#define COGVR_PI 3.14159265f

static void CogVR_PublishInput(CogV3 origin, CogV3 forward, float yaw, int desktop);
static void CogVR_SaveHeader(const COGVR_Header *header);
static void CogVR_FlushHaptics(void);
static void CogMesh_Draw(const CogM4 *vp, int eyePass);
static void CogVR_ActionUpdate(void);   /* director: snapshot motion energy */
static void CogVR_LoadAvatar(void);
static void CogVR_StickArrowDyn(float headYaw, float sx, float sy, int drives);

static CogV3 CogV3_Make(float x, float y, float z) { CogV3 r; r.x = x; r.y = y; r.z = z; return r; }
static CogV3 CogV3_Add(CogV3 a, CogV3 b) { return CogV3_Make(a.x + b.x, a.y + b.y, a.z + b.z); }
static CogV3 CogV3_Sub(CogV3 a, CogV3 b) { return CogV3_Make(a.x - b.x, a.y - b.y, a.z - b.z); }
static CogV3 CogV3_Scale(CogV3 a, float s) { return CogV3_Make(a.x * s, a.y * s, a.z * s); }
static float CogV3_Dot(CogV3 a, CogV3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float CogV3_Len(CogV3 a) { return CogVR.sqrtf_(CogV3_Dot(a, a)); }
static float CogVR_Clamp(float v, float lo, float hi) { return (v < lo) ? lo : ((v > hi) ? hi : v); }

/* rotation by `a` about +Y: (0,0,-1) turns to the left (-x) for a > 0 */
static CogV3
CogV3_RotY(CogV3 p, float a)
{
    const float c = CogVR.cosf_(a), s = CogVR.sinf_(a);
    return CogV3_Make(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);
}

static void
CogM4_Identity(CogM4 *m)
{
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            m->m[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
}

static void
CogM4_Mul(CogM4 *out, const CogM4 *a, const CogM4 *b)
{
    CogM4 r;
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j] + a->m[i][3] * b->m[3][j];
        }
    }
    *out = r;
}

static void
CogM4_From34(CogM4 *out, const HmdMatrix34_t *h)
{
    int i, j;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            out->m[i][j] = h->m[i][j];
        }
    }
    out->m[3][0] = out->m[3][1] = out->m[3][2] = 0.0f;
    out->m[3][3] = 1.0f;
}

static void
CogM4_RigidInverse(CogM4 *out, const CogM4 *a)
{
    int i, j;
    CogM4 r;
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            r.m[i][j] = a->m[j][i];
        }
        r.m[i][3] = -(a->m[0][i] * a->m[0][3] + a->m[1][i] * a->m[1][3] + a->m[2][i] * a->m[2][3]);
    }
    r.m[3][0] = r.m[3][1] = r.m[3][2] = 0.0f;
    r.m[3][3] = 1.0f;
    *out = r;
}

/* view matrix from a camera basis (right, up, forward) at `eye` */
static void
CogM4_View(CogM4 *out, CogV3 r, CogV3 u, CogV3 f, CogV3 eye)
{
    out->m[0][0] = r.x;  out->m[0][1] = r.y;  out->m[0][2] = r.z;  out->m[0][3] = -CogV3_Dot(r, eye);
    out->m[1][0] = u.x;  out->m[1][1] = u.y;  out->m[1][2] = u.z;  out->m[1][3] = -CogV3_Dot(u, eye);
    out->m[2][0] = -f.x; out->m[2][1] = -f.y; out->m[2][2] = -f.z; out->m[2][3] = CogV3_Dot(f, eye);
    out->m[3][0] = 0.0f; out->m[3][1] = 0.0f; out->m[3][2] = 0.0f; out->m[3][3] = 1.0f;
}

/* right-handed perspective, reversed Z with an infinite far plane: z_ndc = near / w */
static void
CogM4_Persp(CogM4 *out, float fovY, float aspect, float zn)
{
    const float f = 1.0f / CogVR.tanf_(fovY * 0.5f);
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            out->m[i][j] = 0.0f;
        }
    }
    out->m[0][0] = f / aspect;
    out->m[1][1] = f;
    out->m[2][3] = zn;
    out->m[3][2] = -1.0f;
}

/* world (cells) -> metres: Ry(yaw) * scale, then translate */
static void
CogM4_World(CogM4 *out, float yaw, float scale, CogV3 t)
{
    const float c = CogVR.cosf_(yaw) * scale, s = CogVR.sinf_(yaw) * scale;
    CogM4_Identity(out);
    out->m[0][0] = c;  out->m[0][2] = s;  out->m[0][3] = t.x;
    out->m[1][1] = scale;                 out->m[1][3] = t.y;
    out->m[2][0] = -s; out->m[2][2] = c;  out->m[2][3] = t.z;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* font: merged boxes per glyph                                                                                   */
/* ------------------------------------------------------------------------------------------------------------- */

static void
CogVR_InitFont(void)
{
    int ch, r, c;
    for (ch = 0; ch < 128; ch++) {
        int n = 0, minc = 8, maxc = -1, minr = 8, maxr = -1;
        for (r = 0; r < 8; r++) {
            const unsigned bits = (Uint8) CogVR_font8x8[ch][r];
            c = 0;
            while (c < 8) {
                if (bits & (1u << c)) {
                    const int c0 = c;
                    int k, merged = 0;
                    while ((c < 8) && (bits & (1u << c))) {
                        c++;
                    }
                    /* merge with the identical run of the previous row */
                    for (k = 0; k < n; k++) {
                        Uint8 *b = CogVR.glyphBox[ch][k];
                        if ((b[0] == c0) && (b[1] == c - 1) && (b[3] == r - 1)) {
                            b[3] = (Uint8) r;
                            merged = 1;
                            break;
                        }
                    }
                    if (!merged && (n < 32)) {
                        Uint8 *b = CogVR.glyphBox[ch][n++];
                        b[0] = (Uint8) c0; b[1] = (Uint8) (c - 1); b[2] = (Uint8) r; b[3] = (Uint8) r;
                    }
                    if (c0 < minc) { minc = c0; }
                    if (c - 1 > maxc) { maxc = c - 1; }
                    if (r < minr) { minr = r; }
                    if (r > maxr) { maxr = r; }
                } else {
                    c++;
                }
            }
        }
        CogVR.glyphN[ch] = (Uint8) n;
        CogVR.glyphBB[ch][0] = (Uint8) ((n > 0) ? minc : 0);
        CogVR.glyphBB[ch][1] = (Uint8) ((n > 0) ? maxc : 7);
        CogVR.glyphBB[ch][2] = (Uint8) ((n > 0) ? minr : 0);
        CogVR.glyphBB[ch][3] = (Uint8) ((n > 0) ? maxr : 7);
    }
}

/* ------------------------------------------------------------------------------------------------------------- */
/* scene                                                                                                          */
/* ------------------------------------------------------------------------------------------------------------- */

#define COGVR_TILE_GAP   0.05f
#define COGVR_FLAT_PIX   0.11f          /* cell units per font pixel, terrain */
#define COGVR_UP_PIX     0.12f          /* upright robot glyphs */
#define COGVR_ITEM_PIX   0.085f         /* upright item glyphs (smaller) */
#define COGVR_HOVER      0.35f
#define COGVR_EYE        0.55f          /* Cogmind POV eye height */

static Uint32
CogVR_Pack(float r, float g, float b, float e)
{
    const Uint32 R = (Uint32) (CogVR_Clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
    const Uint32 G = (Uint32) (CogVR_Clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
    const Uint32 B = (Uint32) (CogVR_Clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
    const Uint32 E = (Uint32) (CogVR_Clamp(e, 0.0f, 1.0f) * 255.0f + 0.5f);
    return R | (G << 8) | (B << 16) | (E << 24);
}

static void
CogVR_Fill(CogVRInst *p, float ax, float ay, float az, float kind, float lx, float ly, float lz, float phase, float hx, float hy, float hz, Uint32 rgba)
{
    p->ax = ax; p->ay = ay; p->az = az; p->kind = kind;
    p->lx = lx; p->ly = ly; p->lz = lz; p->phase = phase;
    p->hx = hx; p->hy = hy; p->hz = hz;
    p->rgba = rgba;
}

/* a map box (cell space) */
static void
CogVR_Push(float ax, float ay, float az, float kind, float lx, float ly, float lz, float phase, float hx, float hy, float hz, Uint32 rgba)
{
    if (CogVR.countOnly) {
        CogVR.ninst++;
        return;
    }
    if (CogVR.ninst >= COGVR_MAXINST) {
        CogVR.overflow++;
        return;
    }
    CogVR_Fill(&CogVR.inst[CogVR.ninst++], ax, ay, az, kind, lx, ly, lz, phase, hx, hy, hz, rgba);
}

/*
 * Per-frame boxes, independent of the map instance buffer: CogVR_DynBegin(), then for each group CogVR_DynBatch(model, inEyes)
 * followed by CogVR_DynBox() calls in that group's box space; CogVR_DynUpload() once before drawing (CogVR_DrawDyn).
 */
static void
CogVR_DynBegin(void)
{
    CogVR.ndyn = 0;
    CogVR.nbatch = 0;
}

static void
CogVR_DynBatch(const CogM4 *model, int inEyes)
{
    CogVRBatch *b;
    if (CogVR.nbatch >= COGVR_MAXBATCH) {
        return;
    }
    b = &CogVR.batch[CogVR.nbatch++];
    b->model = *model;
    b->first = CogVR.ndyn;
    b->count = 0;
    b->inEyes = inEyes;
}


static void
CogVR_DynBox(float cx, float cy, float cz, float hx, float hy, float hz, Uint32 rgba)
{
    if (!CogVR.nbatch || (CogVR.ndyn >= COGVR_MAXDYN)) {
        return;
    }
    CogVR_Fill(&CogVR.dyn[CogVR.ndyn++], 0.0f, 0.0f, 0.0f, 0.0f, cx, cy, cz, 0.0f, hx, hy, hz, rgba);
    CogVR.batch[CogVR.nbatch - 1].count++;
}

static void
CogVR_DynUpload(void)
{
    D3D11_MAPPED_SUBRESOURCE map;
    if (CogVR.ndyn && SUCCEEDED(ID3D11DeviceContext_Map(CogVR.ctx, (ID3D11Resource *) CogVR.dynVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        SDL20_memcpy(map.pData, CogVR.dyn, (size_t) CogVR.ndyn * sizeof (CogVRInst));
        ID3D11DeviceContext_Unmap(CogVR.ctx, (ID3D11Resource *) CogVR.dynVB, 0);
    }
}

#include "cogmind_vrinteg.inc.c"

/* terrain extrusion height by character */
static float
CogVR_TerrainHeight(unsigned ch)
{
    switch (ch) {
        case '#':
            return 1.0f;
        case '+': case '\'': case '/': case '=':
            return 0.45f;
        case '.': case ',': case ':': case ';': case '`':
            return 0.04f;               /* floor dots stay flat */
        default:
            return 0.1f;
    }
}

/*
 * What a cell's glyph stands for, from what the 2D map shows only (ch + flags; the contract carries no entity/item knowledge):
 * only LIVE cells (the 2D view's cell this frame) and the player's cell hold robots/items; remembered records are terrain.
 * Letters and '@' are robots, item-ish symbols items; everything else terrain (height: CogVR_TerrainHeight).
 */
enum { COGVR_GK_TERRAIN = 0, COGVR_GK_ROBOT, COGVR_GK_ITEM };

static int
CogVR_GlyphKind(unsigned ch, unsigned flags)
{
    if (flags & COGVR_PLAYER) {
        return COGVR_GK_ROBOT;
    }
    if (!(flags & COGVR_LIVE)) {
        return COGVR_GK_TERRAIN;
    }
    if (((ch >= 'a') && (ch <= 'z')) || ((ch >= 'A') && (ch <= 'Z')) || (ch == '@')) {
        return COGVR_GK_ROBOT;
    }
    switch (ch) {
        case ')': case '(': case '[': case ']': case '!': case '?': case '*': case '%':
        case '}': case '{': case '"': case '$': case '&': case '=':
            return COGVR_GK_ITEM;
        default:
            return COGVR_GK_TERRAIN;
    }
}

/* scene query (camera module): terrain height at a world cell of the drawn snapshot, 0 when unknown or outside */
static float
CogVR_WallHeight(int x, int y)
{
    const CogVRSlot *sl = &CogVR.slot[CogVR.dIdx];
    const COGVR_Cell *c;
    if (!sl->cells || (x < 0) || (y < 0) || (x >= sl->hdr.mapW) || (y >= sl->hdr.mapH)) {
        return 0.0f;
    }
    c = &sl->cells[(size_t) x * (size_t) sl->hdr.mapH + (size_t) y];
    if (!(c->flags & COGVR_KNOWN) || (c->ch == ' ') || (c->ch == 0)) {
        return 0.0f;
    }
    if (CogVR_GlyphKind(c->ch, c->flags) != COGVR_GK_TERRAIN) {
        return 0.0f;   /* upright glyphs hover; nothing solid */
    }
    return (c->ch < 128) ? CogVR_TerrainHeight(c->ch) : 0.26f;
}

/* memory style: desaturated, dimmed, slightly blue (like the 2D map's remembered cells) */
static void
CogVR_MemoryStyle(float *r, float *g, float *b)
{
    const float l = 0.30f * *r + 0.59f * *g + 0.11f * *b;
    *r = (l + (*r - l) * 0.25f) * 0.42f + 0.010f;
    *g = (l + (*g - l) * 0.25f) * 0.42f + 0.025f;
    *b = (l + (*b - l) * 0.25f) * 0.42f + 0.070f;
}

/*
 * Emit the map boxes at level of detail `lod` (CogVR_BuildScene picks the most detailed level within COGVR_MAXINST):
 *   0 everything; 1 no floor dots in remembered cells; 2 no floor dots at all; 3 walls as one solid block per cell.
 */
static void
CogVR_EmitMap(int lod)
{
    const CogVRSlot *sl = &CogVR.slot[CogVR.dIdx];
    const int W = sl->hdr.mapW, H = sl->hdr.mapH;
    const COGVR_Header *hd = &sl->hdr;
    int x, y;

    for (x = 0; x < W; x++) {
        const COGVR_Cell *col = sl->cells + (size_t) x * (size_t) H;
        for (y = 0; y < H; y++) {
            const COGVR_Cell *c = &col[y];
            const int vis = (c->flags & COGVR_VISIBLE) != 0;
            const int player = (c->flags & COGVR_PLAYER) != 0;
            const int kind = CogVR_GlyphKind(c->ch, c->flags);
            const int upright = (kind != COGVR_GK_TERRAIN);
            const float upPix = (kind == COGVR_GK_ITEM) ? COGVR_ITEM_PIX : COGVR_UP_PIX;
            const unsigned ch = c->ch;
            const float cx = (float) x + 0.5f, cz = (float) y + 0.5f;
            float fr = c->fgR / 255.0f, fg = c->fgG / 255.0f, fb = c->fgB / 255.0f, fe;
            float tr, tg, tb;
            int tall;

            if (!(c->flags & COGVR_KNOWN)) {
                continue;
            }
            /* glyph colour */
            if (!vis) {
                CogVR_MemoryStyle(&fr, &fg, &fb);
                fe = 0.04f;
            } else if (player) {
                fr = fr + (1.0f - fr) * 0.35f; fg = fg + (1.0f - fg) * 0.35f; fb = fb + (1.0f - fb) * 0.35f;
                fe = 1.0f;
            } else {
                /* robots/items glow; terrain only a little (walls barely, so the glow marks what matters) */
                fe = upright ? ((kind == COGVR_GK_ITEM) ? 0.6f : 0.8f)
                             : ((ch < 128) ? ((CogVR_TerrainHeight(ch) >= 1.0f) ? 0.06f : ((CogVR_TerrainHeight(ch) < 0.05f) ? 0.12f : 0.35f)) : 0.3f);
            }
            /* floor tile: background colour when there is one, else very dark */
            if ((c->flags & COGVR_BACK) && (c->bgR | c->bgG | c->bgB)) {
                tr = c->bgR / 255.0f; tg = c->bgG / 255.0f; tb = c->bgB / 255.0f;
                if (!vis) {
                    CogVR_MemoryStyle(&tr, &tg, &tb);
                }
                tr = 0.05f + tr * 0.9f; tg = 0.055f + tg * 0.9f; tb = 0.065f + tb * 0.9f;
            } else if (vis) {
                tr = 0.17f; tg = 0.185f; tb = 0.21f;
            } else {
                tr = 0.09f; tg = 0.10f; tb = 0.14f;
            }
            if (CogVRInteg.red) {      /* red sky: the world is read-only */
                CogVR_IntegGrey(&fr, &fg, &fb, &fe);
                CogVR_IntegGrey(&tr, &tg, &tb, NULL);
            }
            tall = !upright && (ch < 128) && (CogVR_TerrainHeight(ch) >= 1.0f);
            if (!tall) {
                CogVR_Push(cx, 0.0f, cz, 0.0f, 0.0f, -0.03f, 0.0f, 0.0f, 0.5f - COGVR_TILE_GAP, 0.03f, 0.5f - COGVR_TILE_GAP,
                           CogVR_Pack(tr, tg, tb, vis ? 0.06f : 0.0f));
            }
            if ((ch == ' ') || (ch == 0) || (player && CogVR.pov)) {   /* POV: the camera is inside the player's glyph */
                continue;
            }
            if (!upright && (ch < 128) && (CogVR_TerrainHeight(ch) < 0.05f) && ((lod >= 2) || ((lod >= 1) && !vis))) {
                continue;               /* floor dot dropped by the LOD */
            }
            if (tall && (lod >= 3)) {
                CogVR_Push(cx, 0.0f, cz, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.5f - COGVR_TILE_GAP, 0.5f, 0.5f - COGVR_TILE_GAP, CogVR_Pack(fr, fg, fb, fe));
                continue;
            }
            if ((ch >= 128) || !CogVR.glyphN[ch]) {   /* no glyph data: a small cube */
                const float hcube = upright ? 0.16f : 0.13f;
                const float yc = upright ? COGVR_HOVER + 0.3f : hcube;
                CogVR_Push(cx, 0.0f, cz, upright ? 1.0f : 0.0f, 0.0f, yc, 0.0f, (float) ((x * 7 + y * 13) & 63) * 0.37f,
                           hcube, hcube, hcube, CogVR_Pack(fr, fg, fb, fe));
            } else if (upright) {
                const Uint8 *bb = CogVR.glyphBB[ch];
                const float gcx = (bb[0] + bb[1] + 1) * 0.5f;
                const float phase = (float) ((x * 7 + y * 13) & 63) * 0.37f;
                const Uint32 rgba = CogVR_Pack(fr, fg, fb, fe);
                int k;
                for (k = 0; k < CogVR.glyphN[ch]; k++) {
                    const Uint8 *b = CogVR.glyphBox[ch][k];
                    const float bot = (float) (bb[3] - b[3]) * upPix, top = (float) (bb[3] - b[2] + 1) * upPix;
                    CogVR_Push(cx, 0.0f, cz, 1.0f, ((b[0] + b[1] + 1) * 0.5f - gcx) * upPix, COGVR_HOVER + (bot + top) * 0.5f, 0.0f, phase,
                               (b[1] - b[0] + 1) * 0.5f * upPix, (top - bot) * 0.5f, upPix * 0.6f, rgba);
                }
            } else {
                const Uint8 *bb = CogVR.glyphBB[ch];
                const float gcx = (bb[0] + bb[1] + 1) * 0.5f, gcy = (bb[2] + bb[3] + 1) * 0.5f;
                const float h = CogVR_TerrainHeight(ch);
                const Uint32 rgba = CogVR_Pack(fr, fg, fb, fe);
                int k;
                for (k = 0; k < CogVR.glyphN[ch]; k++) {
                    const Uint8 *b = CogVR.glyphBox[ch][k];
                    CogVR_Push(cx, 0.0f, cz, 0.0f, ((b[0] + b[1] + 1) * 0.5f - gcx) * COGVR_FLAT_PIX, h * 0.5f, ((b[2] + b[3] + 1) * 0.5f - gcy) * COGVR_FLAT_PIX, 0.0f,
                               (b[1] - b[0] + 1) * 0.5f * COGVR_FLAT_PIX, h * 0.5f, (b[3] - b[2] + 1) * 0.5f * COGVR_FLAT_PIX, rgba);
                }
            }
        }
    }

    /* the player: a rotating, pulsing halo ring and a beacon column */
    if (!CogVR.pov && (hd->playerX >= 0) && (hd->playerX < W) && (hd->playerY >= 0) && (hd->playerY < H)) {
        const COGVR_Cell *pc = &sl->cells[(size_t) hd->playerX * (size_t) H + (size_t) hd->playerY];
        const float px = (float) hd->playerX + 0.5f, pz = (float) hd->playerY + 0.5f;
        const float r = 0.35f + 0.65f * (pc->fgR / 255.0f), g = 0.35f + 0.65f * (pc->fgG / 255.0f), b = 0.35f + 0.65f * (pc->fgB / 255.0f);
        const Uint32 rgba = CogVR_Pack(r, g, b, 1.0f);
        int k;
        for (k = 0; k < 20; k++) {
            CogVR_Push(px, 0.0f, pz, 2.0f, 0.68f, 0.03f, 0.0f, (float) k * (2.0f * COGVR_PI / 20.0f), 0.03f, 0.025f, 0.08f, rgba);
        }
        CogVR_Push(px, 0.0f, pz, 2.0f, 0.0f, 3.6f, 0.0f, 0.0f, 0.022f, 2.4f, 0.022f, CogVR_Pack(r, g, b, 0.7f));
    }
}

static void
CogVR_BuildScene(void)
{
    const COGVR_Header *hd = &CogVR.slot[CogVR.dIdx].hdr;
    int lod;

    for (lod = 0; lod < 3; lod++) {     /* counting passes: the most detailed level that fits */
        CogVR.countOnly = 1;
        CogVR.ninst = 0;
        CogVR_EmitMap(lod);
        if (CogVR.ninst <= COGVR_MAXINST) {
            break;
        }
    }
    CogVR.countOnly = 0;
    CogVR.ninst = 0;
    CogVR.overflow = 0;
    CogVR_EmitMap(lod);
    if (lod != CogVR.lod) {
        CogVR_Log("COGVR: scene level of detail %d (%d boxes, budget %d)", lod, CogVR.ninst, COGVR_MAXINST);
        CogVR.lod = lod;
    }
    if (CogVR.overflow && !CogVR.loggedOverflow) {
        CogVR.loggedOverflow = 1;
        CogVR_Log("COGVR: scene exceeds %d boxes even at the lowest detail (%d dropped): cap reached", COGVR_MAXINST, CogVR.overflow);
    }
    {
        D3D11_MAPPED_SUBRESOURCE map;
        if (SUCCEEDED(ID3D11DeviceContext_Map(CogVR.ctx, (ID3D11Resource *) CogVR.instVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
            SDL20_memcpy(map.pData, CogVR.inst, (size_t) CogVR.ninst * sizeof (CogVRInst));
            ID3D11DeviceContext_Unmap(CogVR.ctx, (ID3D11Resource *) CogVR.instVB, 0);
        }
    }
    CogVR.hdr = *hd;
    CogVR.sceneDirty = 0;
}

/* take the newest published snapshot (if any) and rebuild the instance buffer */
static void
CogVR_TakeSnapshot(void)
{
    int got = 0;
    SDL20_LockMutex(CogVR.mutex);
    if (CogVR.fresh) {
        const int t = CogVR.dIdx;
        CogVR.dIdx = CogVR.rIdx;
        CogVR.rIdx = t;
        CogVR.fresh = 0;
        got = 1;
    }
    SDL20_UnlockMutex(CogVR.mutex);
    if (got) {
        CogVRInteg.lastPresent = CogGPU_Now();
    }
    if ((got || CogVR.sceneDirty) && CogVR.slot[CogVR.dIdx].cells) {
        CogVR_BuildScene();
        CogVR.drawnVersion = CogVR.slot[CogVR.dIdx].version;
        if (got) {
            CogVR_ActionUpdate();
        }
    }
}

/* ------------------------------------------------------------------------------------------------------------- */
/* D3D11                                                                                                          */
/* ------------------------------------------------------------------------------------------------------------- */

#define COGVR_STR2(x) #x
#define COGVR_STR(x) COGVR_STR2(x)
static const char CogVR_HLSL[] =
"static const float HOVER = " COGVR_STR(COGVR_HOVER) ";\n"
"cbuffer Frame : register(b0) {\n"
"    row_major float4x4 viewProj;\n"
"    row_major float4x4 model;\n"
"    float4 camCell;\n"
"    float4 eyeM;\n"
"    float4 fogColor;\n"
"    float4 lightDir;\n"
"};\n"
"struct VSIn {\n"
"    float3 pos : POSITION; float3 nrm : NORMAL;\n"
"    float4 anchor : TEXCOORD0; float4 local : TEXCOORD1; float3 hs : TEXCOORD2; float4 color : COLOR0;\n"
"};\n"
"struct VSOut {\n"
"    float4 pos : SV_Position; float3 nrm : NORMAL; float3 wpos : TEXCOORD0;\n"
"    float4 color : COLOR0; float3 q : TEXCOORD1; float3 hs : TEXCOORD2; float edgeW : TEXCOORD3;\n"
"};\n"
"VSOut vs_main(VSIn i) {\n"
"    VSOut o;\n"
"    float t = camCell.w;\n"
"    float3 q = i.pos * i.hs;\n"
"    float3 n = i.nrm;\n"
"    float3 p;\n"
"    float emis = i.color.a;\n"
"    if (i.anchor.w > 1.5) {\n"
"        float pulse = 0.5 + 0.5 * sin(t * 3.0);\n"
"        float a = i.local.w + t * 0.9;\n"
"        float c = cos(a); float s = sin(a);\n"
"        float r = i.local.x * (1.0 + 0.15 * pulse);\n"
"        float3 lq = float3(q.x * c + q.z * s, q.y, -q.x * s + q.z * c);\n"
"        n = float3(n.x * c + n.z * s, n.y, -n.x * s + n.z * c);\n"
"        p = i.anchor.xyz + float3(c * r, i.local.y, -s * r) + lq;\n"
"        emis = emis * (0.45 + 0.9 * pulse);\n"
"    } else if (i.anchor.w > 0.5) {\n"
"        float2 d = camCell.xz - i.anchor.xz;\n"
"        float l = length(d);\n"
"        float2 f = (l > 0.0001) ? d / l : float2(0.0, 1.0);\n"
"        float3 fw = float3(f.x, 0.0, f.y);\n"
"        float3 rt = float3(f.y, 0.0, -f.x);\n"
"        float hgt = camCell.y - HOVER;\n"
"        float tilt = clamp(0.85 * hgt / max(sqrt(hgt * hgt + l * l), 0.0001), 0.0, 0.8);\n"
"        float3 upv = float3(0.0, cos(tilt), 0.0) - fw * sin(tilt);\n"
"        float3 nv = float3(0.0, sin(tilt), 0.0) + fw * cos(tilt);\n"
"        float3 lp = i.local.xyz + q;\n"
"        float bob = 0.045 * sin(t * 2.1 + i.local.w);\n"
"        p = i.anchor.xyz + rt * lp.x + float3(0.0, HOVER + bob, 0.0) + upv * (lp.y - HOVER) + nv * lp.z;\n"
"        n = rt * n.x + upv * n.y + nv * n.z;\n"
"    } else {\n"
"        p = i.anchor.xyz + i.local.xyz + q;\n"
"    }\n"
"    float4 wp = mul(model, float4(p, 1.0));\n"
"    o.pos = mul(viewProj, wp);\n"
"    o.wpos = wp.xyz;\n"
"    o.nrm = mul((float3x3) model, n);\n"
"    o.color = float4(pow(abs(i.color.rgb), 2.2), emis);\n"
"    o.q = q;\n"
"    o.hs = i.hs;\n"
"    o.edgeW = 0.016;\n"
"    return o;\n"
"}\n"
"float4 ps_main(VSOut i) : SV_Target {\n"
"    float3 N = normalize(i.nrm);\n"
"    float ndl = saturate(dot(N, lightDir.xyz));\n"
"    float hemi = 0.6 + 0.4 * N.y;\n"
"    float3 e = i.hs - abs(i.q);\n"
"    float mn = min(e.x, min(e.y, e.z));\n"
"    float mx = max(e.x, max(e.y, e.z));\n"
"    float mid = e.x + e.y + e.z - mn - mx;\n"
"    float edge = (i.edgeW > 0.0) ? 0.55 + 0.45 * smoothstep(0.0, i.edgeW, mid) : 1.0;\n"
"    float3 base = i.color.rgb;\n"
"    float3 lit = base * (lightDir.w * hemi + 0.85 * ndl) * edge + base * (i.color.a * fogColor.w) * (0.4 + 0.6 * edge);\n"
"    float dist = length(i.wpos - eyeM.xyz);\n"
"    float fog = exp(-dist * eyeM.w);\n"
"    return float4(lerp(fogColor.rgb, lit, fog), 1.0);\n"
"}\n"
"Texture2D blitTex : register(t0);\n"
"Texture2D blitTex2 : register(t1);\n"
"SamplerState blitSmp : register(s0);\n"
"cbuffer Post : register(b1) {\n"
"    float4 postTexel;\n"
"    float4 postParam;\n"
"};\n"
"struct BOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"BOut vs_blit(uint id : SV_VertexID) {\n"
"    BOut o;\n"
"    float2 uv = float2((id == 1) ? 2.0 : 0.0, (id == 2) ? 2.0 : 0.0);\n"
"    o.uv = uv;\n"
"    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);\n"
"    return o;\n"
"}\n"
"float4 ps_blit(BOut i) : SV_Target {\n"
"    float3 c = blitTex.Sample(blitSmp, i.uv).rgb;\n"
"    return float4(pow(abs(c), 1.0 / 2.2), 1.0);\n"
"}\n"
"float4 ps_bright(BOut i) : SV_Target {\n"
"    float2 t = postTexel.xy;\n"
"    float3 c = blitTex.Sample(blitSmp, i.uv + float2(-t.x, -t.y)).rgb + blitTex.Sample(blitSmp, i.uv + float2(t.x, -t.y)).rgb\n"
"             + blitTex.Sample(blitSmp, i.uv + float2(-t.x, t.y)).rgb + blitTex.Sample(blitSmp, i.uv + float2(t.x, t.y)).rgb;\n"
"    float l;\n"
"    c = c * 0.25;\n"
"    l = max(c.r, max(c.g, c.b));\n"
"    return float4(c * saturate((l - postParam.x) / max(l, 0.0001)), 1.0);\n"
"}\n"
"float4 ps_blur(BOut i) : SV_Target {\n"
"    float2 d = postTexel.zw;\n"
"    float3 c = blitTex.Sample(blitSmp, i.uv).rgb * 0.2270;\n"
"    c = c + (blitTex.Sample(blitSmp, i.uv + d).rgb + blitTex.Sample(blitSmp, i.uv - d).rgb) * 0.1945;\n"
"    c = c + (blitTex.Sample(blitSmp, i.uv + d * 2.0).rgb + blitTex.Sample(blitSmp, i.uv - d * 2.0).rgb) * 0.1216;\n"
"    c = c + (blitTex.Sample(blitSmp, i.uv + d * 3.0).rgb + blitTex.Sample(blitSmp, i.uv - d * 3.0).rgb) * 0.0540;\n"
"    c = c + (blitTex.Sample(blitSmp, i.uv + d * 4.0).rgb + blitTex.Sample(blitSmp, i.uv - d * 4.0).rgb) * 0.0162;\n"
"    return float4(c, 1.0);\n"
"}\n"
"float4 ps_comp(BOut i) : SV_Target {\n"
"    float3 c = (blitTex.Sample(blitSmp, i.uv).rgb + blitTex2.Sample(blitSmp, i.uv).rgb * postParam.y) * postParam.z;\n"
"    c = (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14);\n"
"    c *= 1.0 - postParam.w * smoothstep(0.50, 0.96, length((i.uv - 0.5) * 2.0));\n"
"    return float4(saturate(c), 1.0);\n"
"}\n";

static ID3DBlob *
CogVR_Compile(pD3DCompile compile, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    const HRESULT hr = compile(CogVR_HLSL, sizeof (CogVR_HLSL) - 1, "cogvr.hlsl", NULL, NULL, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) || !code) {
        CogVR_Log("COGVR: shader %s (%s) failed: hr 0x%08lx %s", entry, target, (unsigned long) hr,
                  err ? (const char *) ID3D10Blob_GetBufferPointer(err) : "");
        COGVR_REL(code);
    }
    COGVR_REL(err);
    return code;
}

static void
CogVR_FreeView(CogVRView *v)
{
    int i;
    COGVR_REL(v->msaaRTV);
    COGVR_REL(v->msaa);
    COGVR_REL(v->hdrSRV);
    COGVR_REL(v->hdrRTV);
    COGVR_REL(v->hdr);
    COGVR_REL(v->dsv);
    COGVR_REL(v->depth);
    for (i = 0; i < 2; i++) {
        COGVR_REL(v->bloomSRV[i]);
        COGVR_REL(v->bloomRTV[i]);
        COGVR_REL(v->bloom[i]);
        COGVR_REL(v->resSRV[i]);
        COGVR_REL(v->resRTV[i]);
        COGVR_REL(v->res[i]);
    }
    v->w = v->h = 0;
}

/* a single-sample colour texture with RTV + SRV */
static HRESULT
CogVR_MakeTarget(int w, int h, DXGI_FORMAT fmt, ID3D11Texture2D **tex, ID3D11RenderTargetView **rtv, ID3D11ShaderResourceView **srv)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;
    SDL20_zero(td);
    td.Width = (UINT) w;
    td.Height = (UINT) h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = ID3D11Device_CreateTexture2D(CogVR.dev, &td, NULL, tex);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateRenderTargetView(CogVR.dev, (ID3D11Resource *) *tex, NULL, rtv);
    }
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateShaderResourceView(CogVR.dev, (ID3D11Resource *) *tex, NULL, srv);
    }
    return hr;
}

static int
CogVR_MakeView(CogVRView *v, int w, int h, int nres)
{
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;
    int i;

    CogVR_FreeView(v);
    v->w = w;
    v->h = h;
    v->bw = SDL_max(w / 4, 1);
    v->bh = SDL_max(h / 4, 1);
    v->nres = nres;
    v->samples = CogVR.msaa;
    hr = CogVR_MakeTarget(w, h, COGVR_HDR_FORMAT, &v->hdr, &v->hdrRTV, &v->hdrSRV);
    for (i = 0; SUCCEEDED(hr) && (i < 2); i++) {
        hr = CogVR_MakeTarget(v->bw, v->bh, COGVR_HDR_FORMAT, &v->bloom[i], &v->bloomRTV[i], &v->bloomSRV[i]);
    }
    for (i = 0; SUCCEEDED(hr) && (i < nres); i++) {
        hr = CogVR_MakeTarget(w, h, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, &v->res[i], &v->resRTV[i], &v->resSRV[i]);
    }
    if (FAILED(hr)) {
        CogVR_Log("COGVR: view targets %dx%d failed: hr 0x%08lx", w, h, (unsigned long) hr);
        return 0;
    }
    SDL20_zero(td);
    td.Width = (UINT) w;
    td.Height = (UINT) h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    if (v->samples > 1) {
        td.Format = COGVR_HDR_FORMAT;
        td.SampleDesc.Count = (UINT) v->samples;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = ID3D11Device_CreateTexture2D(CogVR.dev, &td, NULL, &v->msaa);
        if (SUCCEEDED(hr)) {
            hr = ID3D11Device_CreateRenderTargetView(CogVR.dev, (ID3D11Resource *) v->msaa, NULL, &v->msaaRTV);
        }
        if (FAILED(hr)) {
            CogVR_Log("COGVR: MSAA target %dx%dx%d failed: hr 0x%08lx", w, h, v->samples, (unsigned long) hr);
            return 0;
        }
    }
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = (UINT) v->samples;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = ID3D11Device_CreateTexture2D(CogVR.dev, &td, NULL, &v->depth);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateDepthStencilView(CogVR.dev, (ID3D11Resource *) v->depth, NULL, &v->dsv);
    }
    if (FAILED(hr)) {
        CogVR_Log("COGVR: depth buffer %dx%d failed: hr 0x%08lx", w, h, (unsigned long) hr);
        return 0;
    }
    return 1;
}

static HRESULT
CogVR_MakePS(pD3DCompile compile, const char *entry, ID3D11PixelShader **ps)
{
    ID3DBlob *b = CogVR_Compile(compile, entry, "ps_4_0");
    HRESULT hr = E_FAIL;
    if (b) {
        hr = ID3D11Device_CreatePixelShader(CogVR.dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, ps);
        COGVR_REL(b);
    }
    return hr;
}

static int
CogVR_InitPipeline(void)
{
    /* unit cube: 6 faces x 4 vertices (position, normal), counter-clockwise seen from outside */
    float verts[24 * 6];
    Uint16 idx[36];
    int f, k, nv = 0, ni = 0;
    pD3DCompile compile;
    ID3DBlob *vsb, *psb, *vbb, *pbb;
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_DEPTH_STENCIL_DESC dd;
    D3D11_SAMPLER_DESC smp;
    HRESULT hr;
    UINT q1 = 0, q2 = 0;
    static const D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 44, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };

    for (f = 0; f < 6; f++) {
        const int axis = f >> 1;
        const float sgn = (f & 1) ? -1.0f : 1.0f;
        float n[3] = { 0, 0, 0 }, u[3] = { 0, 0, 0 }, v[3] = { 0, 0, 0 };
        static const float cu[4] = { -1, 1, 1, -1 }, cv[4] = { -1, -1, 1, 1 };
        n[axis] = sgn;
        if (sgn > 0) {   /* cross(u, v) = n */
            u[(axis + 1) % 3] = 1.0f;
            v[(axis + 2) % 3] = 1.0f;
        } else {
            u[(axis + 2) % 3] = 1.0f;
            v[(axis + 1) % 3] = 1.0f;
        }
        for (k = 0; k < 4; k++) {
            float *o = &verts[nv * 6];
            int a;
            for (a = 0; a < 3; a++) {
                o[a] = n[a] + cu[k] * u[a] + cv[k] * v[a];
                o[3 + a] = n[a];
            }
            nv++;
        }
        idx[ni++] = (Uint16) (f * 4 + 0); idx[ni++] = (Uint16) (f * 4 + 1); idx[ni++] = (Uint16) (f * 4 + 2);
        idx[ni++] = (Uint16) (f * 4 + 0); idx[ni++] = (Uint16) (f * 4 + 2); idx[ni++] = (Uint16) (f * 4 + 3);
    }

    compile = (pD3DCompile) (void *) GetProcAddress(CogVR.hComp, "D3DCompile");
    if (!compile) {
        CogVR_Log("COGVR: D3DCompile missing in d3dcompiler_47.dll");
        return 0;
    }
    vsb = CogVR_Compile(compile, "vs_main", "vs_4_0");
    psb = CogVR_Compile(compile, "ps_main", "ps_4_0");
    vbb = CogVR_Compile(compile, "vs_blit", "vs_4_0");
    pbb = CogVR_Compile(compile, "ps_blit", "ps_4_0");
    hr = (vsb && psb && vbb && pbb) ? S_OK : E_FAIL;
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateVertexShader(CogVR.dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &CogVR.vs);
    }
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreatePixelShader(CogVR.dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &CogVR.ps);
    }
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateVertexShader(CogVR.dev, ID3D10Blob_GetBufferPointer(vbb), ID3D10Blob_GetBufferSize(vbb), NULL, &CogVR.vsBlit);
    }
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreatePixelShader(CogVR.dev, ID3D10Blob_GetBufferPointer(pbb), ID3D10Blob_GetBufferSize(pbb), NULL, &CogVR.psBlit);
    }
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateInputLayout(CogVR.dev, il, (UINT) SDL_arraysize(il), ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &CogVR.layout);
    }
    if (SUCCEEDED(hr)) {
        hr = CogVR_MakePS(compile, "ps_bright", &CogVR.psBright);
    }
    if (SUCCEEDED(hr)) {
        hr = CogVR_MakePS(compile, "ps_blur", &CogVR.psBlur);
    }
    if (SUCCEEDED(hr)) {
        hr = CogVR_MakePS(compile, "ps_comp", &CogVR.psComp);
    }
    COGVR_REL(vsb);
    COGVR_REL(psb);
    COGVR_REL(vbb);
    COGVR_REL(pbb);
    if (FAILED(hr)) {
        CogVR_Log("COGVR: shader objects failed: hr 0x%08lx", (unsigned long) hr);
        return 0;
    }

    SDL20_zero(bd);
    SDL20_zero(sd);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = sizeof (verts);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    sd.pSysMem = verts;
    hr = ID3D11Device_CreateBuffer(CogVR.dev, &bd, &sd, &CogVR.cubeVB);
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = sizeof (idx);
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sd.pSysMem = idx;
        hr = ID3D11Device_CreateBuffer(CogVR.dev, &bd, &sd, &CogVR.cubeIB);
    }
    if (SUCCEEDED(hr)) {
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = (UINT) (COGVR_MAXINST * sizeof (CogVRInst));
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        hr = ID3D11Device_CreateBuffer(CogVR.dev, &bd, NULL, &CogVR.instVB);
    }
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = (UINT) (COGVR_MAXDYN * sizeof (CogVRInst));
        hr = ID3D11Device_CreateBuffer(CogVR.dev, &bd, NULL, &CogVR.dynVB);
    }
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = sizeof (CogVRFrameCB);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = ID3D11Device_CreateBuffer(CogVR.dev, &bd, NULL, &CogVR.cb);
    }
    if (SUCCEEDED(hr)) {
        bd.ByteWidth = 32;
        hr = ID3D11Device_CreateBuffer(CogVR.dev, &bd, NULL, &CogVR.cbPost);
    }
    if (FAILED(hr)) {
        CogVR_Log("COGVR: buffers failed: hr 0x%08lx", (unsigned long) hr);
        return 0;
    }

    SDL20_zero(rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = TRUE;
    rd.DepthClipEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(CogVR.dev, &rd, &CogVR.rsScene);
    if (SUCCEEDED(hr)) {
        rd.CullMode = D3D11_CULL_NONE;
        rd.MultisampleEnable = FALSE;
        hr = ID3D11Device_CreateRasterizerState(CogVR.dev, &rd, &CogVR.rsBlit);
    }
    SDL20_zero(dd);
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_GREATER;   /* reversed Z */
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateDepthStencilState(CogVR.dev, &dd, &CogVR.dsScene);
    }
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateDepthStencilState(CogVR.dev, &dd, &CogVR.dsBlit);
    }
    SDL20_zero(smp);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    smp.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateSamplerState(CogVR.dev, &smp, &CogVR.sampler);
    }
    if (FAILED(hr)) {
        CogVR_Log("COGVR: pipeline states failed: hr 0x%08lx", (unsigned long) hr);
        return 0;
    }

    CogVR.msaa = 1;
    if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(CogVR.dev, COGVR_HDR_FORMAT, 4, &q1)) &&
        SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(CogVR.dev, DXGI_FORMAT_D32_FLOAT, 4, &q2)) && q1 && q2) {
        CogVR.msaa = 4;
    }
    CogVR_Log("COGVR: pipeline ready, MSAA %dx", CogVR.msaa);
    return 1;
}

static int
CogVR_InitDevice(int adapterIndex)
{
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    CogCreateDXGIFactory1_t createFactory;
    PFN_D3D11_CREATE_DEVICE createDevice;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_10_0;
    DXGI_ADAPTER_DESC1 ad;
    char name[128];
    HRESULT hr;
    int i;

    CogVR.hD3D = LoadLibraryA("d3d11.dll");
    CogVR.hDXGI = LoadLibraryA("dxgi.dll");
    CogVR.hComp = LoadLibraryA("d3dcompiler_47.dll");
    if (!CogVR.hD3D || !CogVR.hDXGI || !CogVR.hComp) {
        CogVR_Log("COGVR: cannot load %s", !CogVR.hD3D ? "d3d11.dll" : (!CogVR.hDXGI ? "dxgi.dll" : "d3dcompiler_47.dll"));
        return 0;
    }
    createFactory = (CogCreateDXGIFactory1_t) (void *) GetProcAddress(CogVR.hDXGI, "CreateDXGIFactory1");
    createDevice = (PFN_D3D11_CREATE_DEVICE) (void *) GetProcAddress(CogVR.hD3D, "D3D11CreateDevice");
    if (!createFactory || !createDevice) {
        CogVR_Log("COGVR: CreateDXGIFactory1 / D3D11CreateDevice missing");
        return 0;
    }
    hr = createFactory(&CogVR_IID_IDXGIFactory1, (void **) &CogVR.factory);
    if (FAILED(hr)) {
        CogVR_Log("COGVR: CreateDXGIFactory1 failed: hr 0x%08lx", (unsigned long) hr);
        return 0;
    }
    if (FAILED(IDXGIFactory1_EnumAdapters1(CogVR.factory, (UINT) ((adapterIndex >= 0) ? adapterIndex : 0), &CogVR.adapter)) &&
        FAILED(IDXGIFactory1_EnumAdapters1(CogVR.factory, 0, &CogVR.adapter))) {
        CogVR_Log("COGVR: no DXGI adapter");
        return 0;
    }
    hr = createDevice((IDXGIAdapter *) CogVR.adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, levels, (UINT) SDL_arraysize(levels),
                      D3D11_SDK_VERSION, &CogVR.dev, &got, &CogVR.ctx);
    if (FAILED(hr)) {
        /* some runtimes (this project's macOS Wine) only create devices for the default adapter: use that one, and its factory for
           the swap chain */
        IDXGIDevice *dxdev = NULL;
        IDXGIAdapter *ad0 = NULL;
        const HRESULT hr0 = hr;
        COGVR_REL(CogVR.adapter);
        COGVR_REL(CogVR.factory);
        hr = createDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, levels, (UINT) SDL_arraysize(levels), D3D11_SDK_VERSION, &CogVR.dev, &got, &CogVR.ctx);
        if (SUCCEEDED(hr)) {
            hr = ID3D11Device_QueryInterface(CogVR.dev, &CogVR_IID_IDXGIDevice, (void **) &dxdev);
        }
        if (SUCCEEDED(hr)) {
            hr = IDXGIDevice_GetAdapter(dxdev, &ad0);
        }
        if (SUCCEEDED(hr)) {
            hr = IDXGIAdapter_QueryInterface(ad0, &CogVR_IID_IDXGIAdapter1, (void **) &CogVR.adapter);
        }
        if (SUCCEEDED(hr)) {
            hr = IDXGIAdapter_GetParent(ad0, &CogVR_IID_IDXGIFactory1, (void **) &CogVR.factory);
        }
        COGVR_REL(ad0);
        COGVR_REL(dxdev);
        if (FAILED(hr)) {
            const HMODULE nt = GetModuleHandleA("ntdll.dll");
            CogVR_Log("COGVR: D3D11CreateDevice failed: hr 0x%08lx on the adapter, 0x%08lx on the default adapter%s", (unsigned long) hr0, (unsigned long) hr,
                      (nt && GetProcAddress(nt, "wine_get_version")) ?
                      " (Wine: wined3d's Vulkan backend lacks feature level 10 on MoltenVK; WINE_D3D_CONFIG=renderer=gl or DXVK)" : "");
            return 0;
        }
        CogVR_Log("COGVR: D3D11CreateDevice on DXGI adapter %d failed (hr 0x%08lx): using the default adapter", adapterIndex, (unsigned long) hr0);
    }
    name[0] = 0;
    if (SUCCEEDED(IDXGIAdapter1_GetDesc1(CogVR.adapter, &ad))) {
        for (i = 0; (i < (int) sizeof (name) - 1) && ad.Description[i]; i++) {
            name[i] = (ad.Description[i] < 128) ? (char) ad.Description[i] : '?';
        }
        name[i] = 0;
    }
    CogVR_Log("COGVR: D3D11 device on adapter %d '%s', feature level %x", adapterIndex, name, (unsigned) got);
    if (!CogVR_InitPipeline()) {
        return 0;
    }
    CogVR_LoadAvatar();
    return 1;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* preview window                                                                                                 */
/* ------------------------------------------------------------------------------------------------------------- */

static LRESULT CALLBACK
CogVR_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_CLOSE:
            if (CogVR.mode == 1) {
                CogVR.quit = 1;         /* closing the preview turns the desktop renderer off */
            } else {
                ShowWindow(hwnd, SW_MINIMIZE);
            }
            return 0;
        case WM_SIZE:
            CogVR.resized = 1;
            return 0;
        case WM_RBUTTONDOWN:
            CogVR.rmb = 1;
            CogVR.mouseX = (short) LOWORD(lp);
            CogVR.mouseY = (short) HIWORD(lp);
            SetCapture(hwnd);
            return 0;
        case WM_RBUTTONUP:
            CogVR.rmb = 0;
            ReleaseCapture();
            return 0;
        case WM_MOUSEMOVE:
            if (CogVR.rmb) {
                const int mx = (short) LOWORD(lp), my = (short) HIWORD(lp);
                CogVR.mouseDX += mx - CogVR.mouseX;
                CogVR.mouseDY += my - CogVR.mouseY;
                CogVR.mouseX = mx;
                CogVR.mouseY = my;
            }
            return 0;
        case WM_MOUSEWHEEL:
            CogVR.wheel += (short) HIWORD(wp);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            CogVR.keys[wp & 0xFF] = 1;
            if (!(lp & (1 << 30))) {    /* not auto-repeat */
                if (wp == VK_HOME) {
                    CogVR.keyHome = 1;
                } else if ((wp == 'P') || (wp == VK_TAB)) {
                    CogVR.keyPov = 1;   /* Examine (free fly) <-> Cogmind POV */
                } else if (wp == 'C') {
                    CogVR.keyStream = 1;
                }
            }
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            CogVR.keys[wp & 0xFF] = 0;
            return 0;
        case WM_KILLFOCUS:
            SDL20_memset(CogVR.keys, 0, sizeof (CogVR.keys));
            return 0;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int
CogVR_ClientSize(int *w, int *h)
{
    RECT rc;
    if (!GetClientRect(CogVR.hwnd, &rc)) {
        return 0;
    }
    *w = rc.right - rc.left;
    *h = rc.bottom - rc.top;
    return 1;
}

static int
CogVR_MakeBackRTV(void)
{
    ID3D11Texture2D *bb = NULL;
    HRESULT hr = IDXGISwapChain_GetBuffer(CogVR.swap, 0, &CogVR_IID_ID3D11Texture2D, (void **) &bb);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateRenderTargetView(CogVR.dev, (ID3D11Resource *) bb, NULL, &CogVR.backRTV);
    }
    COGVR_REL(bb);
    if (FAILED(hr)) {
        CogVR_Log("COGVR: back buffer view failed: hr 0x%08lx", (unsigned long) hr);
        return 0;
    }
    return 1;
}

static int
CogVR_InitWindow(void)
{
    WNDCLASSA wc;
    DXGI_SWAP_CHAIN_DESC sc;
    HINSTANCE inst = GetModuleHandleA(NULL);
    RECT rc;
    HRESULT hr;

    SDL20_zero(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = CogVR_WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR) IDC_ARROW);
    wc.lpszClassName = "CogVRPreview";
    RegisterClassA(&wc);
    rc.left = 0; rc.top = 0; rc.right = CogVR.streamW; rc.bottom = CogVR.streamH;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    /* always a window of its own: the game's SDL window is never touched */
    CogVR.hwnd = CreateWindowExA(0, "CogVRPreview", (CogVR.mode == 2) ? "Cogmind VR - stream camera" : "Cogmind VR - desktop (C: stream camera)",
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                 NULL, NULL, inst, NULL);
    if (!CogVR.hwnd) {
        CogVR_Log("COGVR: CreateWindowEx failed: %lu", (unsigned long) GetLastError());
        return 0;
    }
    ShowWindow(CogVR.hwnd, SW_SHOWNOACTIVATE);
    if (!CogVR_ClientSize(&CogVR.winW, &CogVR.winH) || (CogVR.winW <= 0) || (CogVR.winH <= 0)) {
        CogVR.winW = 1280;
        CogVR.winH = 720;
    }
    SDL20_zero(sc);
    sc.BufferDesc.Width = (UINT) CogVR.winW;
    sc.BufferDesc.Height = (UINT) CogVR.winH;
    sc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 1;
    sc.OutputWindow = CogVR.hwnd;
    sc.Windowed = TRUE;
    sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    hr = IDXGIFactory1_CreateSwapChain(CogVR.factory, (IUnknown *) CogVR.dev, &sc, &CogVR.swap);
    if (FAILED(hr)) {
        CogVR_Log("COGVR: CreateSwapChain failed: hr 0x%08lx", (unsigned long) hr);
        return 0;
    }
    IDXGIFactory1_MakeWindowAssociation(CogVR.factory, CogVR.hwnd, DXGI_MWA_NO_ALT_ENTER);
    return CogVR_MakeBackRTV();
}

static void
CogVR_Pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

/* returns 0 when the window has no area (minimised) */
static int
CogVR_HandleResize(void)
{
    int w, h;
    if (!CogVR.resized) {
        return (CogVR.winW > 0) && (CogVR.winH > 0);
    }
    CogVR.resized = 0;
    if (!CogVR_ClientSize(&w, &h) || (w <= 0) || (h <= 0)) {
        CogVR.winW = CogVR.winH = 0;
        return 0;
    }
    if ((w != CogVR.winW) || (h != CogVR.winH) || !CogVR.backRTV) {
        ID3D11DeviceContext_OMSetRenderTargets(CogVR.ctx, 0, NULL, NULL);
        COGVR_REL(CogVR.backRTV);
        if (FAILED(IDXGISwapChain_ResizeBuffers(CogVR.swap, 0, (UINT) w, (UINT) h, DXGI_FORMAT_UNKNOWN, 0)) || !CogVR_MakeBackRTV()) {
            CogVR.winW = CogVR.winH = 0;
            return 0;
        }
        CogVR.winW = w;
        CogVR.winH = h;
    }
    return 1;
}

/* draw texture `srv` (srcW x srcH) into the window, aspect preserved */
static void
CogVR_BlitToWindow(ID3D11ShaderResourceView *srv, int srcW, int srcH)
{
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    D3D11_VIEWPORT vp;
    float sw = (float) CogVR.winW, sh = (float) CogVR.winW * (float) srcH / (float) srcW;
    ID3D11ShaderResourceView *none = NULL;
    if (sh > (float) CogVR.winH) {
        sh = (float) CogVR.winH;
        sw = (float) CogVR.winH * (float) srcW / (float) srcH;
    }
    ID3D11DeviceContext_OMSetRenderTargets(CogVR.ctx, 1, &CogVR.backRTV, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(CogVR.ctx, CogVR.backRTV, black);
    vp.TopLeftX = ((float) CogVR.winW - sw) * 0.5f;
    vp.TopLeftY = ((float) CogVR.winH - sh) * 0.5f;
    vp.Width = sw;
    vp.Height = sh;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(CogVR.ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(CogVR.ctx, CogVR.rsBlit);
    ID3D11DeviceContext_OMSetDepthStencilState(CogVR.ctx, CogVR.dsBlit, 0);
    ID3D11DeviceContext_IASetInputLayout(CogVR.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(CogVR.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(CogVR.ctx, CogVR.vsBlit, NULL, 0);
    ID3D11DeviceContext_PSSetShader(CogVR.ctx, CogVR.psBlit, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(CogVR.ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(CogVR.ctx, 0, 1, &CogVR.sampler);
    ID3D11DeviceContext_Draw(CogVR.ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(CogVR.ctx, 0, 1, &none);
}

/* ------------------------------------------------------------------------------------------------------------- */
/* rendering                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------- */

#define COGVR_EMISSIVE 3.0f             /* emissive gain (HDR) */
#define COGVR_BLOOM_THRESHOLD 0.9f
#define COGVR_BLOOM_STRENGTH 0.9f
#define COGVR_EXPOSURE 1.1f

static float
CogVR_Time(void)
{
    return (float) ((CogGPU_Now() - CogVR.t0) / 1000.0);
}

static void
CogVR_SetCB(const CogM4 *viewProj, const CogM4 *model, CogV3 camCell, CogV3 eyeM, float fogDensity)
{
    D3D11_MAPPED_SUBRESOURCE map;
    CogVRFrameCB *cb;
    if (FAILED(ID3D11DeviceContext_Map(CogVR.ctx, (ID3D11Resource *) CogVR.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        return;
    }
    cb = (CogVRFrameCB *) map.pData;
    cb->viewProj = *viewProj;
    cb->model = *model;
    cb->camCell[0] = camCell.x; cb->camCell[1] = camCell.y; cb->camCell[2] = camCell.z; cb->camCell[3] = CogVR_Time();
    cb->eyeM[0] = eyeM.x; cb->eyeM[1] = eyeM.y; cb->eyeM[2] = eyeM.z; cb->eyeM[3] = fogDensity;
    cb->fog[0] = CogVRSkyColour[CogVRInteg.shown][0]; cb->fog[1] = CogVRSkyColour[CogVRInteg.shown][1];
    cb->fog[2] = CogVRSkyColour[CogVRInteg.shown][2]; cb->fog[3] = COGVR_EMISSIVE;
    cb->light[0] = 0.36f; cb->light[1] = 0.86f; cb->light[2] = 0.36f; cb->light[3] = 0.32f;
    ID3D11DeviceContext_Unmap(CogVR.ctx, (ID3D11Resource *) CogVR.cb, 0);
}

/* begin a scene pass into view `v` (HDR target; CogVR_EndPass produces the final texture) */
static void
CogVR_BeginPass(CogVRView *v)
{
    const float *sky = CogVRSkyColour[CogVRInteg.shown];
    const float clear[4] = { sky[0], sky[1], sky[2], 1.0f };
    ID3D11RenderTargetView *rtv = (v->samples > 1) ? v->msaaRTV : v->hdrRTV;
    D3D11_VIEWPORT vp;
    const UINT strides[2] = { 24, sizeof (CogVRInst) }, offsets[2] = { 0, 0 };
    ID3D11Buffer *vbs[2];
    vbs[0] = CogVR.cubeVB;
    vbs[1] = CogVR.instVB;
    ID3D11DeviceContext_OMSetRenderTargets(CogVR.ctx, 1, &rtv, v->dsv);
    ID3D11DeviceContext_ClearRenderTargetView(CogVR.ctx, rtv, clear);
    ID3D11DeviceContext_ClearDepthStencilView(CogVR.ctx, v->dsv, D3D11_CLEAR_DEPTH, 0.0f, 0);
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float) v->w;
    vp.Height = (float) v->h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(CogVR.ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(CogVR.ctx, CogVR.rsScene);
    ID3D11DeviceContext_OMSetDepthStencilState(CogVR.ctx, CogVR.dsScene, 0);
    ID3D11DeviceContext_IASetInputLayout(CogVR.ctx, CogVR.layout);
    ID3D11DeviceContext_IASetVertexBuffers(CogVR.ctx, 0, 2, vbs, strides, offsets);
    ID3D11DeviceContext_IASetIndexBuffer(CogVR.ctx, CogVR.cubeIB, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(CogVR.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(CogVR.ctx, CogVR.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(CogVR.ctx, CogVR.ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(CogVR.ctx, 0, 1, &CogVR.cb);
    ID3D11DeviceContext_PSSetConstantBuffers(CogVR.ctx, 0, 1, &CogVR.cb);
}

static void
CogVR_DrawMap(void)
{
    const UINT stride = sizeof (CogVRInst), offset = 0;
    if (CogVR.ninst > 0) {
        ID3D11DeviceContext_IASetVertexBuffers(CogVR.ctx, 1, 1, &CogVR.instVB, &stride, &offset);
        ID3D11DeviceContext_DrawIndexedInstanced(CogVR.ctx, 36, (UINT) CogVR.ninst, 0, 0, 0);
    }
}

/* the per-frame boxes; `eyePass`: skip the batches that are not drawn in the headset (CogVRBatch.inEyes). Each batch binds the
   buffer at its byte offset: wined3d's GL backend ignores StartInstanceLocation without ARB_base_instance (macOS). */
static void
CogVR_DrawDyn(const CogM4 *viewProj, CogV3 eyeM, float fogDensity, int eyePass)
{
    const UINT stride = sizeof (CogVRInst);
    int i;
    for (i = 0; i < CogVR.nbatch; i++) {
        const CogVRBatch *b = &CogVR.batch[i];
        const UINT offset = (UINT) b->first * stride;
        if (!b->count || (eyePass && !b->inEyes) || (!eyePass && (b->inEyes == 2))) {
            continue;
        }
        ID3D11DeviceContext_IASetVertexBuffers(CogVR.ctx, 1, 1, &CogVR.dynVB, &stride, &offset);
        CogVR_SetCB(viewProj, &b->model, CogV3_Make(0.0f, 0.0f, 0.0f), eyeM, fogDensity);
        ID3D11DeviceContext_DrawIndexedInstanced(CogVR.ctx, 36, (UINT) b->count, 0, 0, 0);
    }
    CogMesh_Draw(viewProj, eyePass);
}

/* one fullscreen post-processing pass: `ps` reads src (t0) and src2 (t1), writes dst (dw x dh) */
static void
CogVR_PostPass(ID3D11RenderTargetView *dst, int dw, int dh, ID3D11PixelShader *ps, ID3D11ShaderResourceView *src,
               ID3D11ShaderResourceView *src2, float tx, float ty, float dx, float dy)
{
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11ShaderResourceView *srvs[2], *none[2] = { NULL, NULL };
    srvs[0] = src;
    srvs[1] = src2;
    if (SUCCEEDED(ID3D11DeviceContext_Map(CogVR.ctx, (ID3D11Resource *) CogVR.cbPost, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        float *p = (float *) map.pData;
        p[0] = tx; p[1] = ty; p[2] = dx; p[3] = dy;
        p[4] = COGVR_BLOOM_THRESHOLD; p[5] = COGVR_BLOOM_STRENGTH; p[6] = COGVR_EXPOSURE; p[7] = CogVR.postEye * CogVR.vignette;
        ID3D11DeviceContext_Unmap(CogVR.ctx, (ID3D11Resource *) CogVR.cbPost, 0);
    }
    ID3D11DeviceContext_OMSetRenderTargets(CogVR.ctx, 1, &dst, NULL);
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = (float) dw;
    vp.Height = (float) dh;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(CogVR.ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(CogVR.ctx, CogVR.rsBlit);
    ID3D11DeviceContext_OMSetDepthStencilState(CogVR.ctx, CogVR.dsBlit, 0);
    ID3D11DeviceContext_IASetInputLayout(CogVR.ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(CogVR.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(CogVR.ctx, CogVR.vsBlit, NULL, 0);
    ID3D11DeviceContext_PSSetShader(CogVR.ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(CogVR.ctx, 1, 1, &CogVR.cbPost);
    ID3D11DeviceContext_PSSetShaderResources(CogVR.ctx, 0, 2, srvs);
    ID3D11DeviceContext_PSSetSamplers(CogVR.ctx, 0, 1, &CogVR.sampler);
    ID3D11DeviceContext_Draw(CogVR.ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(CogVR.ctx, 0, 2, none);
}

/* finish a scene pass: resolve the HDR scene, bloom (bright pass at 1/4 size, two separable blur rounds), tone map into res[res] */
static void
CogVR_EndPass(CogVRView *v, int res)
{
    const float bx = 1.0f / (float) v->bw, by = 1.0f / (float) v->bh;
    if (v->samples > 1) {
        ID3D11DeviceContext_ResolveSubresource(CogVR.ctx, (ID3D11Resource *) v->hdr, 0, (ID3D11Resource *) v->msaa, 0, COGVR_HDR_FORMAT);
    }
    ID3D11DeviceContext_OMSetRenderTargets(CogVR.ctx, 0, NULL, NULL);
    CogVR_PostPass(v->bloomRTV[0], v->bw, v->bh, CogVR.psBright, v->hdrSRV, NULL, 1.0f / (float) v->w, 1.0f / (float) v->h, 0.0f, 0.0f);
    CogVR_PostPass(v->bloomRTV[1], v->bw, v->bh, CogVR.psBlur, v->bloomSRV[0], NULL, 0.0f, 0.0f, bx, 0.0f);
    CogVR_PostPass(v->bloomRTV[0], v->bw, v->bh, CogVR.psBlur, v->bloomSRV[1], NULL, 0.0f, 0.0f, 0.0f, by);
    CogVR_PostPass(v->bloomRTV[1], v->bw, v->bh, CogVR.psBlur, v->bloomSRV[0], NULL, 0.0f, 0.0f, 2.0f * bx, 0.0f);
    CogVR_PostPass(v->bloomRTV[0], v->bw, v->bh, CogVR.psBlur, v->bloomSRV[1], NULL, 0.0f, 0.0f, 0.0f, 2.0f * by);
    CogVR_PostPass(v->resRTV[res], v->w, v->h, CogVR.psComp, v->hdrSRV, v->bloomSRV[0], 0.0f, 0.0f, 0.0f, 0.0f);
    ID3D11DeviceContext_OMSetRenderTargets(CogVR.ctx, 0, NULL, NULL);
}

/* ------------------------------------------------------------------------------------------------------------- */
/* dump, statistics                                                                                               */
/* ------------------------------------------------------------------------------------------------------------- */

static int
CogVR_DumpTex(ID3D11Texture2D *tex, int w, int h, const char *name)
{
    char bmp[400];
    Uint8 hdr[54];
    Uint8 *buf;
    D3D11_MAPPED_SUBRESOURCE map;
    Uint32 imgsize, filesize;
    int ok = 0, x, y;

    SDL20_snprintf(bmp, sizeof (bmp), "%s\\%s", CogVR.dumpDir, name);
    if (!CogVR.staging || (CogVR.stagingW != w) || (CogVR.stagingH != h)) {
        D3D11_TEXTURE2D_DESC td;
        COGVR_REL(CogVR.staging);
        SDL20_zero(td);
        td.Width = (UINT) w;
        td.Height = (UINT) h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(CogVR.dev, &td, NULL, &CogVR.staging))) {
            CogVR.staging = NULL;
        }
        CogVR.stagingW = w;
        CogVR.stagingH = h;
    }
    buf = (Uint8 *) SDL20_malloc((size_t) w * (size_t) h * 4);
    if (buf && CogVR.staging) {
        ID3D11DeviceContext_CopyResource(CogVR.ctx, (ID3D11Resource *) CogVR.staging, (ID3D11Resource *) tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(CogVR.ctx, (ID3D11Resource *) CogVR.staging, 0, D3D11_MAP_READ, 0, &map))) {
            for (y = 0; y < h; y++) {
                const Uint8 *s = (const Uint8 *) map.pData + (size_t) y * map.RowPitch;
                Uint8 *d = buf + (size_t) y * (size_t) w * 4;
                for (x = 0; x < w; x++) {   /* RGBA -> BGRA */
                    d[4 * x + 0] = s[4 * x + 2];
                    d[4 * x + 1] = s[4 * x + 1];
                    d[4 * x + 2] = s[4 * x + 0];
                    d[4 * x + 3] = 0xFF;
                }
            }
            ID3D11DeviceContext_Unmap(CogVR.ctx, (ID3D11Resource *) CogVR.staging, 0);
            ok = 1;
        }
    }
    if (ok) {
        imgsize = (Uint32) w * (Uint32) h * 4;
        filesize = 54 + imgsize;
        SDL20_memset(hdr, 0, sizeof (hdr));
        hdr[0] = 'B'; hdr[1] = 'M';
        hdr[2] = (Uint8) filesize; hdr[3] = (Uint8) (filesize >> 8); hdr[4] = (Uint8) (filesize >> 16); hdr[5] = (Uint8) (filesize >> 24);
        hdr[10] = 54;
        hdr[14] = 40;
        hdr[18] = (Uint8) w; hdr[19] = (Uint8) (w >> 8); hdr[20] = (Uint8) (w >> 16); hdr[21] = (Uint8) (w >> 24);
        { const Sint32 nh = -h;
          hdr[22] = (Uint8) nh; hdr[23] = (Uint8) (nh >> 8); hdr[24] = (Uint8) (nh >> 16); hdr[25] = (Uint8) (nh >> 24); }
        hdr[26] = 1;
        hdr[28] = 32;
        hdr[34] = (Uint8) imgsize; hdr[35] = (Uint8) (imgsize >> 8); hdr[36] = (Uint8) (imgsize >> 16); hdr[37] = (Uint8) (imgsize >> 24);
        ok = CogGPU_WriteFile(bmp, hdr, 54, buf, (int) imgsize);
    }
    SDL20_free(buf);
    CogVR_Log("COGVR: dump %s %dx%d (%s)", bmp, w, h, ok ? "ok" : "FAILED");
    return ok;
}

/* COGVR_DUMP: on <dir>\dump.req write vr.bmp (desktop view / left eye) and, when given, stream.bmp, then dump.ack */
static void
CogVR_Dump(ID3D11Texture2D *tex, int w, int h, ID3D11Texture2D *tex2, int w2, int h2)
{
    char req[400], ack[400];
    int ok;
    const double now = CogGPU_Now();

    if (!CogVR.dumpDir[0] || (now - CogVR.dumpPoll < 200.0)) {
        return;
    }
    CogVR.dumpPoll = now;
    SDL20_snprintf(req, sizeof (req), "%s\\dump.req", CogVR.dumpDir);
    if (!CogGPU_FileExists(req)) {
        return;
    }
    SDL20_snprintf(ack, sizeof (ack), "%s\\dump.ack", CogVR.dumpDir);
    ok = CogVR_DumpTex(tex, w, h, "vr.bmp");
    if (ok && tex2) {
        ok = CogVR_DumpTex(tex2, w2, h2, "stream.bmp");
    }
    CogGPU_DeleteFile(req);
    if (ok) {
        CogGPU_WriteFile(ack, "ok", 2, NULL, 0);
    }
}

static void
CogVR_Stats(CogV3 cam)
{
    const double now = CogGPU_Now();
    CogVR.statFrames++;
    if (now - CogVR.statT0 >= 2000.0) {
        CogVR_Log("COGVR: fps=%.1f snapshot=%u inst=%d dropped=%d player=%d,%d cam=%.1f,%.1f,%.1f scale=%.3f pov=%d",
                  CogVR.statFrames * 1000.0 / (now - CogVR.statT0), (unsigned) CogVR.drawnVersion, CogVR.ninst,
                  CogVR.overflow, (int) CogVR.hdr.playerX, (int) CogVR.hdr.playerY, cam.x, cam.y, cam.z, CogVR.wScale, CogVR.pov);
        CogVR.statT0 = now;
        CogVR.statFrames = 0;
    }
}

/* focus cell: the player, else the centre of the 2D view, else of the map */
static void
CogVR_FocusCell(float *fx, float *fz)
{
    const COGVR_Header *h = &CogVR.hdr;
    if ((h->playerX >= 0) && (h->playerY >= 0)) {
        *fx = (float) h->playerX + 0.5f;
        *fz = (float) h->playerY + 0.5f;
    } else if ((h->viewW > 0) && (h->viewH > 0)) {
        *fx = (float) h->viewX + (float) h->viewW * 0.5f;
        *fz = (float) h->viewY + (float) h->viewH * 0.5f;
    } else {
        *fx = (float) h->mapW * 0.5f;
        *fz = (float) h->mapH * 0.5f;
    }
}

/* ------------------------------------------------------------------------------------------------------------- */
/* camera module                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------- */

#define COGVR_DESK_DIST  26.0f          /* desktop recenter: cells from the player along the view ray */
#define COGVR_DESK_FOG   0.045f         /* per metre */
#define COGVR_DESK_SCALE 0.1f           /* desktop metres per cell at start */

static CogV3 CogV3_Cross(CogV3 a, CogV3 b) { return CogV3_Make(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }

static CogV3
CogV3_Norm(CogV3 a, CogV3 fallback)
{
    const float l = CogV3_Len(a);
    return (l > 1e-6f) ? CogV3_Scale(a, 1.0f / l) : fallback;
}

static CogV3 CogV3_Lerp(CogV3 a, CogV3 b, float t) { return CogV3_Add(a, CogV3_Scale(CogV3_Sub(b, a), t)); }

/* cell space <-> metres (tracking space in VR) */
static CogV3
CogVR_CellToTrack(CogV3 p)
{
    return CogV3_Add(CogV3_RotY(CogV3_Scale(p, CogVR.wScale), CogVR.wYaw), CogVR.wT);
}

static CogV3
CogVR_TrackToCell(CogV3 p)
{
    return CogV3_Scale(CogV3_RotY(CogV3_Sub(p, CogVR.wT), -CogVR.wYaw), 1.0f / CogVR.wScale);
}

/* free camera basis from yaw/pitch (yaw 0 looks north = -z) */
static void
CogVRCam_FreeBasis(const CogVRCam *c, CogV3 *f, CogV3 *r, CogV3 *u)
{
    const float cp = CogVR.cosf_(c->pitch), sp = CogVR.sinf_(c->pitch);
    const float cy = CogVR.cosf_(c->yaw), sy = CogVR.sinf_(c->yaw);
    *f = CogV3_Make(-sy * cp, sp, -cy * cp);
    *r = CogV3_Make(cy, 0.0f, -sy);
    *u = CogV3_Make(sy * sp, cp, cy * sp);   /* cross(r, f) */
}

/* does the segment a -> b (cell space) pass below the top of some cell's terrain? */
static int
CogVRCam_Blocked(CogV3 a, CogV3 b, CogVRWallQuery wall)
{
    const int n = 40;
    int i;
    for (i = 1; i < n; i++) {
        const CogV3 p = CogV3_Lerp(a, b, (float) i / (float) n);
        if (p.y < wall((int) SDL20_floor(p.x), (int) SDL20_floor(p.z))) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* beat tracker: read-only tap of the mixed game audio                                                            */
/* ------------------------------------------------------------------------------------------------------------- */

/*
 * The audio thread (CogVR_AudioTap, called by AudioCallbackWrapper with the final device buffer, never modifying it) reduces
 * the output to an onset envelope: mono, one-pole low-pass at ~150 Hz (kick/bass), energy per 10 ms hop, positive flux of
 * the log energy. The render thread (CogVR_BeatUpdate) estimates the tempo by autocorrelation of the last ~8 s of envelope
 * (80..160 BPM preferred), the beat phase with a comb over the same window and the bar phase (which beat of 4 is the
 * downbeat) from the accent pattern, with hysteresis. The director cuts on 4-beat boundaries of that grid.
 */
#define COGVR_ENV_N      1024           /* envelope hops kept (10.24 s) */
#define COGVR_HOP_HZ     100
#define COGVR_BEAT_WIN   800            /* hops analysed (8 s) */
#define COGVR_BEAT_CONF  0.30f          /* minimum autocorrelation peak for a usable tempo */

static struct CogVRTapState {
    /* audio thread */
    int freq, hop, accN;
    float lp, lpK, acc, prevLog;
    /* written by the audio thread, read by the render thread */
    float env[COGVR_ENV_N];
    volatile Uint32 n;                 /* hops written */
    volatile double anchorMs, anchorHop;   /* hop index anchorHop is heard at CogGPU_Now() == anchorMs */
} CogVRTap;

static struct CogVRBeatState {
    double nextUpdate;
    float bpm, conf;
    double periodMs, downbeatMs;       /* a past downbeat, wall time (ms) */
    Uint32 seenN;
    float onMean, onDev;
    double lastOnsetMs;
    double msPerHop;                   /* measured wall-clock length of a hop (the device clock may run fast or slow) */
    double refMs, refHop;              /* anchor pair of the rate measurement */
} CogVRBeat;

static void
CogVR_AudioTap(const Uint8 *stream, int len, const SDL_AudioSpec *spec)
{
    struct CogVRTapState *a = &CogVRTap;
    int frames, ch, bytes, i, c;
    if ((CogVR.rstate != COGVR_RUNNING) || !stream || !spec || (spec->freq <= 0) || !spec->channels) {
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
    if (a->freq != spec->freq) {
        a->freq = spec->freq;
        a->hop = SDL_max(spec->freq / COGVR_HOP_HZ, 1);
        a->lpK = 2.0f * COGVR_PI * 150.0f / (float) spec->freq;
        a->acc = 0.0f;
        a->accN = 0;
    }
    for (i = 0; i < frames; i++) {
        float x = 0.0f;
        for (c = 0; c < ch; c++) {
            const int k = i * ch + c;
            if (bytes == 2) {
                x += (float) ((const Sint16 *) stream)[k] * (1.0f / 32768.0f);
            } else if (spec->format == AUDIO_F32SYS) {
                x += ((const float *) stream)[k];
            } else {
                x += (float) ((const Sint32 *) stream)[k] * (1.0f / 2147483648.0f);
            }
        }
        x /= (float) ch;
        a->lp += a->lpK * (x - a->lp);
        a->acc += a->lp * a->lp;
        if (++a->accN >= a->hop) {
            /* log(1 + k e): silence -> 0, so a loud (accented) hit gives more flux than a soft one */
            const float e = CogVR.logf_(1.0f + 1000.0f * a->acc / (float) a->accN);
            const float flux = e - a->prevLog;
            a->prevLog = e;
            a->env[a->n % COGVR_ENV_N] = (flux > 0.0f) ? flux : 0.0f;
            a->n = a->n + 1;
            a->acc = 0.0f;
            a->accN = 0;
        }
    }
    /* this buffer is heard after the one now queued (~ one buffer of latency) */
    a->anchorHop = (double) a->n + (double) a->accN / (double) a->hop;
    a->anchorMs = CogGPU_Now() + 1000.0 * (double) frames / (double) spec->freq;
}

static double
CogVR_HopMs(double hop)
{
    const double rate = (CogVRBeat.msPerHop > 0.0) ? CogVRBeat.msPerHop : 1000.0 / COGVR_HOP_HZ;
    return CogVRTap.anchorMs + (hop - CogVRTap.anchorHop) * rate;
}

static double CogVR_Ceil(double x) { return -SDL20_floor(-x); }

/* comb sum of x[] at i = start - k * lag * every (k >= 0), with the neighbours */
static float
CogVR_Comb(const float *x, int m, float start, float lag, int every)
{
    float s = 0.0f, i;
    for (i = start; i >= 1.0f; i -= lag * (float) every) {
        const int k = (int) (i + 0.5f);
        if (k < m - 1) {
            s += x[k] + 0.5f * (x[k - 1] + x[k + 1]);
        }
    }
    return s;
}

static void
CogVR_BeatUpdate(double nowMs)
{
    struct CogVRBeatState *b = &CogVRBeat;
    static float x[COGVR_BEAT_WIN], r[128];
    const Uint32 n = CogVRTap.n;
    int m, i, L, best = 0, j, bestJ = 0;
    float mean = 0.0f, e0 = 0.0f, lf, phaseBest = -1.0f, bestPhase = 0.0f, lastBeatHop, sj[4];
    double lastBeatMs;

    /* onsets (for cuts without a confident tempo): envelope above an adaptive threshold */
    if (n - b->seenN > COGVR_ENV_N) {
        b->seenN = n - COGVR_ENV_N;
    }
    for (; b->seenN != n; b->seenN++) {
        const float v = CogVRTap.env[b->seenN % COGVR_ENV_N];
        const double t = CogVR_HopMs((double) b->seenN);
        if ((v > b->onMean + 3.0f * b->onDev + 0.3f) && (t - b->lastOnsetMs > 150.0)) {
            b->lastOnsetMs = t;
        }
        b->onMean += (v - b->onMean) * 0.01f;
        b->onDev += (((v > b->onMean) ? v - b->onMean : b->onMean - v) - b->onDev) * 0.01f;
    }
    if (nowMs < b->nextUpdate) {
        return;
    }
    b->nextUpdate = nowMs + 250.0;
    {
        /* wall-clock rate of the audio clock, over >= 4 s, smoothed */
        const double am = CogVRTap.anchorMs, ah = CogVRTap.anchorHop;
        if ((b->refMs <= 0.0) || (ah < b->refHop)) {
            b->refMs = am;
            b->refHop = ah;
        } else if (am - b->refMs >= 4000.0) {
            const double r = (am - b->refMs) / (ah - b->refHop);
            if ((r > 5.0) && (r < 20.0)) {
                b->msPerHop = (b->msPerHop > 0.0) ? b->msPerHop + (r - b->msPerHop) * 0.3 : r;
            }
            b->refMs = am;
            b->refHop = ah;
        }
    }
    m = (n < COGVR_BEAT_WIN) ? (int) n : COGVR_BEAT_WIN;
    if (m < 400) {
        b->conf = 0.0f;
        return;
    }
    for (i = 0; i < m; i++) {
        x[i] = CogVRTap.env[(n - (Uint32) m + (Uint32) i) % COGVR_ENV_N];
        mean += x[i];
    }
    mean /= (float) m;
    for (i = 0; i < m; i++) {
        x[i] -= mean;
        e0 += x[i] * x[i];
    }
    if (e0 < 1e-6f) {
        b->conf = 0.0f;
        return;
    }
    /* autocorrelation, lags 30..110 hops (200..55 BPM), unbiased */
    for (L = 30; L <= 110; L++) {
        float s = 0.0f;
        for (i = 0; i + L < m; i++) {
            s += x[i] * x[i + L];
        }
        r[L] = s / e0 * (float) m / (float) (m - L);
    }
    for (L = 38; L <= 75; L++) {        /* 158..80 BPM */
        if (!best || (r[L] > r[best])) {
            best = L;
        }
    }
    if ((best >= 60) && (r[best / 2] > 0.8f * r[best])) {           /* prefer the faster octave when it is as strong */
        best /= 2;
    }
    lf = (float) best;
    {
        const float d = r[best - 1] - 2.0f * r[best] + r[best + 1];
        if (d < 0.0f) {
            lf += 0.5f * (r[best - 1] - r[best + 1]) / d;
        }
    }
    b->conf = r[best];
    if (b->conf < COGVR_BEAT_CONF) {
        return;
    }
    b->periodMs = (double) lf * ((b->msPerHop > 0.0) ? b->msPerHop : 1000.0 / COGVR_HOP_HZ);
    b->bpm = (float) (60000.0 / b->periodMs);
    /* beat phase: the most recent beat (comb over the window) */
    for (i = 0; i < best; i++) {
        const float s = CogVR_Comb(x, m, (float) (m - 2 - i), lf, 1);
        if (s > phaseBest) {
            phaseBest = s;
            bestPhase = (float) i;
        }
    }
    lastBeatHop = (float) (m - 2) - bestPhase;
    lastBeatMs = CogVR_HopMs((double) (n - (Uint32) m) + (double) lastBeatHop);
    /* bar phase: the accented beat of 4 */
    for (j = 0; j < 4; j++) {
        sj[j] = CogVR_Comb(x, m, lastBeatHop - (float) j * lf, lf, 4);
        if (sj[j] > sj[bestJ]) {
            bestJ = j;
        }
    }
    {
        /* hysteresis: keep the previous bar alignment unless the new one is clearly stronger */
        int keep = -1;
        if (b->downbeatMs > 0.0) {
            const double beats = (lastBeatMs - b->downbeatMs) / b->periodMs;
            const double k = SDL20_floor(beats + 0.5);
            if ((beats - k < 0.25) && (beats - k > -0.25)) {
                keep = (int) (((long) k % 4 + 4) % 4);   /* beats since the old downbeat, mod 4 */
            }
        }
        if ((keep >= 0) && (sj[keep] >= 0.85f * sj[bestJ])) {
            bestJ = keep;
        }
    }
    b->downbeatMs = lastBeatMs - (double) bestJ * b->periodMs;
}

/* the first beat at or after `t` (ms) that is a multiple of `every` beats from the downbeat; < 0 without a confident tempo */
static double
CogVR_NextBeat(double t, int every)
{
    const struct CogVRBeatState *b = &CogVRBeat;
    double k;
    if ((b->conf < COGVR_BEAT_CONF) || (b->periodMs <= 0.0) || (b->downbeatMs <= 0.0)) {
        return -1.0;
    }
    k = CogVR_Ceil((t - b->downbeatMs) / (b->periodMs * (double) every));
    return b->downbeatMs + k * b->periodMs * (double) every;
}

/* beat position inside the bar (0..4) at `t`, for the cut log; -1 without tempo */
static float
CogVR_BarPhase(double t)
{
    const struct CogVRBeatState *b = &CogVRBeat;
    double beats;
    if ((b->conf < COGVR_BEAT_CONF) || (b->periodMs <= 0.0) || (b->downbeatMs <= 0.0)) {
        return -1.0f;
    }
    beats = (t - b->downbeatMs) / b->periodMs;
    beats -= 4.0 * SDL20_floor(beats / 4.0);
    return (float) beats;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* action: motion energy of the snapshots near the Cogmind player (fair data only)                                */
/* ------------------------------------------------------------------------------------------------------------- */

#define COGVR_ACTION_RADIUS 12

/* per new snapshot: number of LIVE cells within COGVR_ACTION_RADIUS of the player whose glyph or colour changed since the
   previous snapshot (both LIVE); fast (0.6 s) and slow (10 s) averages */
static void
CogVR_ActionUpdate(void)
{
    const CogVRSlot *sl = &CogVR.slot[CogVR.dIdx];
    const int W = sl->hdr.mapW, H = sl->hdr.mapH, px = sl->hdr.playerX, py = sl->hdr.playerY;
    const double now = CogGPU_Now();
    int x, y, count = 0;
    float dt;

    if (!sl->cells || (W <= 0) || (H <= 0)) {
        return;
    }
    if (!CogVR.prevKey || (CogVR.prevW != W) || (CogVR.prevH != H)) {
        SDL20_free(CogVR.prevKey);
        CogVR.prevKey = (Uint32 *) SDL20_calloc((size_t) W * (size_t) H, sizeof (Uint32));
        CogVR.prevW = CogVR.prevKey ? W : 0;
        CogVR.prevH = CogVR.prevKey ? H : 0;
        CogVR.energyT = now;
        if (!CogVR.prevKey) {
            return;
        }
    }
    for (x = 0; x < W; x++) {
        for (y = 0; y < H; y++) {
            const size_t i = (size_t) x * (size_t) H + (size_t) y;
            const COGVR_Cell *c = &sl->cells[i];
            const Uint32 key = (c->flags & COGVR_LIVE) ?
                (0x80000000u | (((Uint32) c->ch * 0x9E3779B1u) ^ ((Uint32) c->fgR | ((Uint32) c->fgG << 8) | ((Uint32) c->fgB << 16)))) : 0u;
            const int dx = x - px, dy = y - py;
            if (key && CogVR.prevKey[i] && (key != CogVR.prevKey[i]) && (px >= 0) &&
                (dx <= COGVR_ACTION_RADIUS) && (dx >= -COGVR_ACTION_RADIUS) && (dy <= COGVR_ACTION_RADIUS) && (dy >= -COGVR_ACTION_RADIUS)) {
                count++;
            }
            CogVR.prevKey[i] = key;
        }
    }
    dt = (float) ((now - CogVR.energyT) / 1000.0);
    CogVR.energyT = now;
    CogVR.energyFast += ((float) count - CogVR.energyFast) * CogVR_Clamp(dt / 0.6f, 0.0f, 1.0f);
    CogVR.energySlow += ((float) count - CogVR.energySlow) * CogVR_Clamp(dt / 10.0f, 0.0f, 1.0f);
}

static int
CogVR_ActionSpike(void)
{
    return (CogVR.energyFast > 2.0f) && (CogVR.energyFast > 2.5f * CogVR.energySlow + 1.0f);
}

/* ------------------------------------------------------------------------------------------------------------- */
/* director                                                                                                       */
/* ------------------------------------------------------------------------------------------------------------- */

/*
 * Editing rules (from the user's own Cogmind edits): within a shot the camera is locked off -- fixed position, fixed FOV, no
 * sway/bob/shake/zoom. It only pans (critically damped, gentle) when the subject leaves the middle 50% of the frame, then
 * settles still. Shot changes are hard cuts, at least 4 s apart (typically 8-16 s), placed on the next 4-beat boundary of the
 * music (no confident tempo: the next onset, else the timer). Shot choice: calm -> wide establishing / follow the avatar /
 * two-shot; action spike near the Cogmind player (motion energy of the snapshots) -> low shot of Cogmind's area / two-shot;
 * the avatar high above the map or a tiny world -> top-down. Occlusion of the subject (> 0.25 s, ray-marched through the
 * scene's wall heights) forces a cut once 4 s have passed since the last one: on the next beat if it is within 0.5 s, else at
 * once. Every cut is logged.
 */
static const char *const CogVR_ShotName[] = { "chase", "wide", "low", "top", "two-shot" };

#define COGVR_CUT_MIN_MS   4000.0
#define COGVR_PAN_OMEGA    1.6f         /* critically damped pan spring (rad/s) */

static float
CogVRDir_Rand(CogVRCam *c)
{
    c->seed = c->seed * 1664525u + 1013904223u;
    return (float) (c->seed >> 8) / 16777216.0f;
}

static CogV3
CogVRDir_Player(const CogVRCam *c)
{
    return CogV3_Make(c->subject2.x, 0.45f, c->subject2.z);
}

/* what the camera keeps in frame during a shot */
static CogV3
CogVRDir_Subject(const CogVRCam *c, int shot, int lowOnPlayer, CogV3 T, float mpc)
{
    if ((shot == COGVR_SHOT_TWO) && c->hasSubject2) {
        return CogV3_Scale(CogV3_Add(T, CogVRDir_Player(c)), 0.5f);
    }
    if ((shot == COGVR_SHOT_LOW) && lowOnPlayer && c->hasSubject2) {
        return CogVRDir_Player(c);
    }
    return CogV3_Sub(T, CogV3_Make(0.0f, 0.5f / mpc, 0.0f));   /* the avatar's torso */
}

/* camera position, initial aim and FOV of a shot, from the subjects' state at the cut */
static void
CogVRDir_Pose(CogVRCam *c, int shot, int lowOnPlayer, CogV3 T, float mpc, CogV3 *pos, CogV3 *aim, float *fov)
{
    const float m = 1.0f / mpc;        /* cells per metre */
    const CogV3 up = CogV3_Make(0.0f, 1.0f, 0.0f);
    const CogV3 fwd = CogV3_Make(-CogVR.sinf_(c->heading), 0.0f, -CogVR.cosf_(c->heading));
    const CogV3 right = CogV3_Make(CogVR.cosf_(c->heading), 0.0f, -CogVR.sinf_(c->heading));
    const int has2 = c->hasSubject2;
    const CogV3 P = CogVRDir_Player(c);
    switch (shot) {
        case COGVR_SHOT_WIDE: {        /* high establishing view over both subjects */
            CogV3 C = has2 ? CogV3_Scale(CogV3_Add(T, P), 0.5f) : T;
            const float a = 0.7f + 2.3f * (float) c->shotN, e = 0.85f;
            C.y *= 0.4f;
            *pos = CogV3_Add(C, CogV3_Scale(CogV3_Make(CogVR.sinf_(a) * CogVR.cosf_(e), CogVR.sinf_(e), CogVR.cosf_(a) * CogVR.cosf_(e)), 8.0f * m));
            *aim = C;
            *fov = 50.0f;
            break;
        }
        case COGVR_SHOT_LOW: {         /* low dramatic angle looking slightly up at the subject */
            const int onP = lowOnPlayer && has2;
            const CogV3 Q = onP ? P : CogV3_Sub(T, CogV3_Scale(up, 0.9f * m));
            const float d = onP ? SDL_max(7.0f, 1.2f * m) : 2.6f * m, a = 2.1f + 1.7f * (float) c->shotN;
            *pos = CogV3_Add(Q, CogV3_Make(CogVR.sinf_(a) * d, 0.06f * d, CogVR.cosf_(a) * d));
            *aim = CogV3_Add(Q, CogV3_Scale(up, 0.12f * d));
            *fov = 60.0f;
            break;
        }
        case COGVR_SHOT_TOP:
            *pos = CogV3_Add(T, CogV3_Make(0.0f, 5.0f * m, 0.9f * m));
            *aim = CogV3_Sub(T, CogV3_Scale(up, 1.0f * m));
            *fov = 50.0f;
            break;
        case COGVR_SHOT_TWO:
            if (has2) {                /* both the avatar and the Cogmind player in frame */
                const CogV3 M = CogV3_Scale(CogV3_Add(T, P), 0.5f), v = CogV3_Sub(T, P);
                const CogV3 nrm = CogV3_Norm(CogV3_Make(-v.z, 0.0f, v.x), CogV3_Make(1.0f, 0.0f, 0.0f));
                const float d = CogV3_Len(v) * 0.5f / CogVR.tanf_(27.5f * COGVR_PI / 180.0f) * 1.4f + 1.2f * m;
                *pos = CogV3_Add(M, CogV3_Add(CogV3_Scale(nrm, d * CogVR.cosf_(0.45f)), CogV3_Scale(up, d * CogVR.sinf_(0.45f))));
                *aim = M;
                *fov = 55.0f;
                break;
            }
            /* no player cell: the chase shot */
            /* fallthrough */
        default:                       /* chase: behind and right of the avatar, following the explorer */
            *pos = CogV3_Add(T, CogV3_Add(CogV3_Scale(fwd, -3.0f * m), CogV3_Add(CogV3_Scale(right, 1.0f * m), CogV3_Scale(up, 0.5f * m))));
            *aim = CogV3_Add(T, CogV3_Sub(CogV3_Scale(fwd, 1.0f * m), CogV3_Scale(up, 0.5f * m)));
            *fov = 55.0f;
            break;
    }
    if (pos->y < 0.3f) {
        pos->y = 0.3f;
    }
}

/* the next shot type; sets *low for LOW shots on the Cogmind player */
static int
CogVRDir_Choose(CogVRCam *c, int wantTop, int spike, int *low)
{
    static const int calmSeq[8] = { COGVR_SHOT_WIDE, COGVR_SHOT_CHASE, COGVR_SHOT_TWO, COGVR_SHOT_CHASE,
                                    COGVR_SHOT_LOW, COGVR_SHOT_WIDE, COGVR_SHOT_CHASE, COGVR_SHOT_TWO };
    int k;
    *low = 0;
    if (wantTop) {
        return COGVR_SHOT_TOP;
    }
    if (spike) {
        if (c->hasSubject2 && (c->shot == COGVR_SHOT_LOW)) {
            return COGVR_SHOT_TWO;
        }
        *low = 1;
        return COGVR_SHOT_LOW;
    }
    for (k = 0; k < 8; k++) {
        const int s = calmSeq[c->calmN++ % 8];
        if ((s != c->shot) && ((s != COGVR_SHOT_TWO) || c->hasSubject2)) {
            return s;
        }
    }
    return COGVR_SHOT_WIDE;
}

static void
CogVRDir_Cut(CogVRCam *c, int shot, int low, CogV3 T, float mpc, CogVRWallQuery wall, const char *reason, double nowMs)
{
    static const int fallback[3] = { COGVR_SHOT_WIDE, COGVR_SHOT_TOP, COGVR_SHOT_CHASE };
    const int from = c->init ? c->shot : -1;
    CogV3 pos, aim;
    float fov;
    int k, s = shot, l = low;
    /* the chosen shot, else the first unoccluded fallback */
    for (k = -1; k < 3; k++) {
        if (k >= 0) {
            s = fallback[k];
            l = 0;
            if (s == from) {
                continue;
            }
        }
        CogVRDir_Pose(c, s, l, T, mpc, &pos, &aim, &fov);
        if (!CogVRCam_Blocked(pos, CogVRDir_Subject(c, s, l, T, mpc), wall)) {
            break;
        }
    }
    if (k == 3) {                      /* everything occluded: take the chosen shot anyway */
        s = shot;
        l = low;
        CogVRDir_Pose(c, s, l, T, mpc, &pos, &aim, &fov);
    }
    c->shot = s;
    c->lowOnPlayer = l;
    c->pos = pos;
    c->aim = aim;
    c->aimV = CogV3_Make(0.0f, 0.0f, 0.0f);
    c->fov = fov * COGVR_PI / 180.0f;
    c->panning = 0;
    c->occT = 0.0f;
    c->offT = 0.0f;
    c->pend = 0;
    c->shotStart = nowMs;
    c->lastCut = nowMs;
    c->shotLen = 8.0f + 8.0f * CogVRDir_Rand(c);
    c->startDist = SDL_max(CogV3_Len(CogV3_Sub(CogVRDir_Subject(c, s, l, T, mpc), pos)), 1.0f);
    c->shotN++;
    CogVR_Log("COGVR: cut t=%.2f %s -> %s%s reason=%s bpm=%.1f conf=%.2f bar=%.2f energy=%.1f/%.1f",
              (nowMs - CogVR.t0) / 1000.0, (from >= 0) ? CogVR_ShotName[from] : "-", CogVR_ShotName[s],
              (s == COGVR_SHOT_LOW && l) ? "(cogmind)" : "", reason, CogVRBeat.bpm, CogVRBeat.conf,
              CogVR_BarPhase(nowMs), CogVR.energyFast, CogVR.energySlow);
}

/* ask for a cut; it happens on the next 4-beat boundary at least COGVR_CUT_MIN_MS after the last cut (or the next onset) */
static void
CogVRDir_Request(CogVRCam *c, const char *reason, int wantTop, int spike, double nowMs)
{
    c->pend = 1;
    c->pendReason = reason;
    c->pendSince = nowMs;
    c->pendShot = CogVRDir_Choose(c, wantTop, spike, &c->pendLow);
    c->pendAt = CogVR_NextBeat(SDL_max(nowMs, c->lastCut + COGVR_CUT_MIN_MS), 4);
}

static void
CogVRDir_Update(CogVRCam *c, float dt, CogV3 T, float mpc, CogVRWallQuery wall, CogVRShot *out)
{
    const double nowMs = CogGPU_Now();
    const CogV3 worldUp = CogV3_Make(0.0f, 1.0f, 0.0f);
    const int wantTop = (T.y * mpc > 3.0f) || (mpc < 0.012f);
    const int spike = CogVR_ActionSpike();
    float tanY, tanX, z, sx, sy;
    CogV3 subj, f, r, u, d;
    char reason[48];
    int off;

    if (!c->init) {
        int low;
        const int s = CogVRDir_Choose(c, wantTop, spike, &low);
        c->seed = 0x2545F491u;
        c->lastCut = nowMs - COGVR_CUT_MIN_MS;
        CogVRDir_Cut(c, s, low, T, mpc, wall, "start", nowMs);
        c->init = 1;
    }
    subj = CogVRDir_Subject(c, c->shot, c->lowOnPlayer, T, mpc);

    /* where is the subject in the current frame? */
    tanY = CogVR.tanf_(c->fov * 0.5f);
    tanX = tanY * ((c->aspect > 0.1f) ? c->aspect : 16.0f / 9.0f);
    f = CogV3_Norm(CogV3_Sub(c->aim, c->pos), CogV3_Make(0.0f, -1.0f, 0.0f));
    r = CogV3_Norm(CogV3_Cross(f, worldUp), CogV3_Make(1.0f, 0.0f, 0.0f));
    u = CogV3_Cross(r, f);
    d = CogV3_Sub(subj, c->pos);
    z = CogV3_Dot(d, f);
    sx = (z > 0.01f) ? CogV3_Dot(d, r) / (z * tanX) : 9.0f;
    sy = (z > 0.01f) ? CogV3_Dot(d, u) / (z * tanY) : 9.0f;
    off = (sx > 1.0f) || (sx < -1.0f) || (sy > 1.0f) || (sy < -1.0f);

    /* pan only when the subject leaves the middle 50% of the frame; stop when it is centred again */
    if ((sx > 0.5f) || (sx < -0.5f) || (sy > 0.5f) || (sy < -0.5f)) {
        c->panning = 1;
    } else if (c->panning && (sx < 0.12f) && (sx > -0.12f) && (sy < 0.12f) && (sy > -0.12f)) {
        c->panning = 0;
    }
    {
        /* critically damped spring on the look-at point (goal = the subject while panning, else where it is: comes to rest) */
        const CogV3 goal = c->panning ? subj : c->aim;
        float left = dt;
        while (left > 0.0f) {
            const float h = SDL_min(left, 0.02f);
            const CogV3 acc = CogV3_Sub(CogV3_Scale(CogV3_Sub(goal, c->aim), COGVR_PAN_OMEGA * COGVR_PAN_OMEGA), CogV3_Scale(c->aimV, 2.0f * COGVR_PAN_OMEGA));
            c->aimV = CogV3_Add(c->aimV, CogV3_Scale(acc, h));
            c->aim = CogV3_Add(c->aim, CogV3_Scale(c->aimV, h));
            left -= h;
        }
    }

    /* cut requests */
    c->occT = CogVRCam_Blocked(c->pos, subj, wall) ? c->occT + dt : 0.0f;
    c->offT = off ? c->offT + dt : 0.0f;
    if ((c->occT > 0.25f) && (nowMs - c->lastCut >= COGVR_CUT_MIN_MS)) {
        if (!c->pend || (SDL20_strcmp(c->pendReason, "occlusion") != 0)) {
            const double nb = CogVR_NextBeat(nowMs, 1);
            int low;
            const int s = CogVRDir_Choose(c, wantTop, 0, &low);
            if ((nb >= 0.0) && (nb - nowMs <= 500.0)) {
                c->pend = 1;
                c->pendReason = "occlusion";
                c->pendSince = nowMs;
                c->pendShot = s;
                c->pendLow = low;
                c->pendAt = nb;
            } else {
                CogVRDir_Cut(c, s, low, T, mpc, wall, "occlusion/now", nowMs);
            }
        }
    } else if (!c->pend && (nowMs - c->lastCut >= COGVR_CUT_MIN_MS)) {
        if (nowMs - c->shotStart >= (double) c->shotLen * 1000.0) {
            CogVRDir_Request(c, "timer", wantTop, spike, nowMs);
        } else if (spike && (c->shot != COGVR_SHOT_LOW) && (c->shot != COGVR_SHOT_TWO)) {
            CogVRDir_Request(c, "action", wantTop, spike, nowMs);
        } else if (wantTop != (c->shot == COGVR_SHOT_TOP)) {
            CogVRDir_Request(c, "height", wantTop, spike, nowMs);
        } else if ((c->offT > 1.5f) || (CogV3_Len(d) > 3.5f * c->startDist)) {
            CogVRDir_Request(c, "lost", wantTop, spike, nowMs);
        }
    }
    /* execute a pending cut on its beat, else on the next onset, else after 3 s */
    if (c->pend) {
        if (c->pendAt >= 0.0) {
            if (nowMs >= c->pendAt) {
                SDL20_snprintf(reason, sizeof (reason), "%s/beat", c->pendReason);
                CogVRDir_Cut(c, c->pendShot, c->pendLow, T, mpc, wall, reason, nowMs);
            }
        } else if (nowMs - c->lastCut >= COGVR_CUT_MIN_MS) {
            if (CogVRBeat.lastOnsetMs > c->pendSince) {
                SDL20_snprintf(reason, sizeof (reason), "%s/onset", c->pendReason);
                CogVRDir_Cut(c, c->pendShot, c->pendLow, T, mpc, wall, reason, nowMs);
            } else if (nowMs - c->pendSince > 3000.0) {
                SDL20_snprintf(reason, sizeof (reason), "%s/timer", c->pendReason);
                CogVRDir_Cut(c, c->pendShot, c->pendLow, T, mpc, wall, reason, nowMs);
            }
        }
    }

    f = CogV3_Norm(CogV3_Sub(c->aim, c->pos), CogV3_Make(0.0f, -1.0f, 0.0f));
    r = CogV3_Norm(CogV3_Cross(f, worldUp), CogV3_Make(1.0f, 0.0f, 0.0f));
    out->eye = c->pos;
    out->fwd = f;
    out->right = r;
    out->up = CogV3_Cross(r, f);
    out->fovY = c->fov;
}

/*
 * One camera step. target/targetVel: the followed point (cell space, cells/s); metresPerCell: current world scale (shot
 * distances are in metres); wall: scene query. FREE cameras ignore the target (their pos/yaw/pitch come from input). The
 * director's shots are locked off, so it does not use targetVel.
 */
static void
CogVRCam_Update(CogVRCam *c, float dt, CogV3 target, CogV3 targetVel, float metresPerCell, CogVRWallQuery wall, CogVRShot *out)
{
    (void) targetVel;
    if (c->kind == COGVR_CAM_DIRECTOR) {
        CogVRDir_Update(c, dt, target, metresPerCell, wall, out);
        return;
    }
    (void) dt;
    (void) target;
    (void) metresPerCell;
    (void) wall;
    CogVRCam_FreeBasis(c, &out->fwd, &out->right, &out->up);
    out->eye = c->pos;
    out->fovY = 60.0f * COGVR_PI / 180.0f;
}

/* shot (cell space) -> view-projection in metres for the current world transform; *eyeM = the eye in metres */
static void
CogVR_ShotMatrix(const CogVRShot *s, float aspect, CogM4 *viewProj, CogV3 *eyeM)
{
    CogM4 view, proj;
    *eyeM = CogVR_CellToTrack(s->eye);
    CogM4_View(&view, CogV3_RotY(s->right, CogVR.wYaw), CogV3_RotY(s->up, CogVR.wYaw), CogV3_RotY(s->fwd, CogVR.wYaw), *eyeM);
    CogM4_Persp(&proj, s->fovY, aspect, 0.005f);
    CogM4_Mul(viewProj, &proj, &view);
}

/* ------------------------------------------------------------------------------------------------------------- */
/* avatar: the headset user's skinned GLB, else a built-in robot */
/* ------------------------------------------------------------------------------------------------------------- */


#include "cogmind_vrmesh.inc.c"

static void
CogVR_LoadAvatar(void)
{
    if (CogVR.avatarPath[0]) {
        CogMesh_Load(CogVR.avatarPath);
    }
}

/* rotation by `a` about the unit axis k (Rodrigues), into the 3x3 part of *m */
static void
CogM4_AxisAngle(CogM4 *m, CogV3 k, float a)
{
    const float c = CogVR.cosf_(a), s = CogVR.sinf_(a), t = 1.0f - c;
    CogM4_Identity(m);
    m->m[0][0] = c + t * k.x * k.x;       m->m[0][1] = t * k.x * k.y - s * k.z; m->m[0][2] = t * k.x * k.z + s * k.y;
    m->m[1][0] = t * k.y * k.x + s * k.z; m->m[1][1] = c + t * k.y * k.y;       m->m[1][2] = t * k.y * k.z - s * k.x;
    m->m[2][0] = t * k.z * k.x - s * k.y; m->m[2][1] = t * k.z * k.y + s * k.x; m->m[2][2] = c + t * k.z * k.z;
}

/*
 * The avatar for the stream view at `head` (tracking-space pose of the eyes, metres) as per-frame batches, hidden in the
 * headset's own views: upright, turned with the head's heading, leaning gently into its motion relative to the world, a
 * subtle bob while flying. `headCell`: the head in cell space (motion relative to the world).
 */
static void
CogVR_AvatarDyn(const CogM4 *head, CogV3 headCell, float dt)
{
    const CogV3 hp = CogV3_Make(head->m[0][3], head->m[1][3], head->m[2][3]);
    const CogV3 f = CogV3_Norm(CogV3_Make(-head->m[0][2], 0.0f, -head->m[2][2]), CogV3_Make(0.0f, 0.0f, -1.0f));
    CogM4 yawM, leanM, body;
    float speed, cellSpeed, lean = 0.0f, bob;

    if (CogVR.avPrevValid && (dt > 0.0f)) {
        const CogV3 vc = CogV3_Scale(CogV3_Sub(headCell, CogVR.avPrevCell), 1.0f / dt);
        const CogV3 vt = CogV3_RotY(CogV3_Scale(vc, CogVR.wScale), CogVR.wYaw);
        CogVR.avVel = CogV3_Add(CogVR.avVel, CogV3_Scale(CogV3_Sub(vt, CogVR.avVel), CogVR_Clamp(dt * 3.0f, 0.0f, 1.0f)));
    }
    CogVR.avPrevCell = headCell;
    CogVR.avPrevValid = 1;
    speed = CogVR.sqrtf_(CogVR.avVel.x * CogVR.avVel.x + CogVR.avVel.z * CogVR.avVel.z);
    cellSpeed = speed / CogVR.wScale;
    /* yaw: x right = cross(f, up), y up, z back = -f */
    CogM4_Identity(&yawM);
    yawM.m[0][0] = -f.z; yawM.m[2][0] = f.x;
    yawM.m[0][2] = -f.x; yawM.m[2][2] = -f.z;
    if (speed > 1e-4f) {
        const CogV3 dir = CogV3_Scale(CogV3_Make(CogVR.avVel.x, 0.0f, CogVR.avVel.z), 1.0f / speed);
        lean = 0.26f * cellSpeed / (cellSpeed + 4.0f);
        CogM4_AxisAngle(&leanM, CogV3_Make(dir.z, 0.0f, -dir.x), lean);   /* axis cross(up, dir): the top tips towards the motion */
        CogM4_Mul(&body, &leanM, &yawM);
    } else {
        body = yawM;
    }
    bob = 0.012f * CogVR.sinf_(CogVR_Time() * 2.4f) * CogVR_Clamp(cellSpeed / 4.0f, 0.0f, 1.0f);
    body.m[0][3] = hp.x;
    body.m[1][3] = hp.y + bob;
    body.m[2][3] = hp.z;
    if (CogMesh.vb) {
        CogMesh_Pose(head, &body, dt);
        CogVR_IntegAvatarDyn(head);
        return;
    }
    {
        const Uint32 shell = CogVR_Pack(0.30f, 0.33f, 0.40f, 0.06f), glow = CogVR_Pack(0.30f, 0.95f, 1.0f, 1.0f);
        /* built-in robot: head (headset pose: shell, visor, antenna) and a body on the leaning frame */
        CogVR_DynBatch(head, 0);
        CogVR_DynBox(0.0f, 0.0f, 0.03f, 0.095f, 0.11f, 0.10f, shell);
        CogVR_DynBox(0.0f, 0.005f, -0.072f, 0.08f, 0.03f, 0.012f, glow);
        CogVR_DynBox(0.0f, 0.14f, 0.05f, 0.006f, 0.035f, 0.006f, shell);
        CogVR_DynBox(0.0f, 0.185f, 0.05f, 0.014f, 0.014f, 0.014f, glow);
        CogVR_DynBatch(&body, 0);
        CogVR_DynBox(0.0f, -0.17f, 0.04f, 0.035f, 0.05f, 0.035f, shell);
        CogVR_DynBox(0.0f, -0.42f, 0.05f, 0.16f, 0.2f, 0.085f, shell);
        CogVR_DynBox(0.0f, -0.36f, -0.04f, 0.05f, 0.05f, 0.01f, glow);
        CogVR_IntegAvatarDyn(head);
    }
}

/* ------------------------------------------------------------------------------------------------------------- */
/* desktop (COGVR=desktop)                                                                                        */
/* ------------------------------------------------------------------------------------------------------------- */

static void
CogVR_DeskRecenter(void)
{
    CogVRCam *c = &CogVR.desk;
    float fx, fz;
    CogV3 f, r, u;
    CogVR_FocusCell(&fx, &fz);
    if (CogVR.pov) {
        CogVR.pov = 0;
        CogVR.wScale = CogVR.povSavedScale;
        CogVR.sceneDirty = 1;
    }
    c->yaw = 0.0f;
    c->pitch = -60.0f * COGVR_PI / 180.0f;
    CogVRCam_FreeBasis(c, &f, &r, &u);
    c->pos = CogV3_Sub(CogV3_Make(fx, 0.0f, fz), CogV3_Scale(f, COGVR_DESK_DIST));
}

static int
CogVR_KeyDown(int vk)
{
    return CogVR.keys[vk & 0xFF] != 0;
}

/* window input -> the free camera and the world scale */
static void
CogVR_DeskInput(float dt)
{
    CogVRCam *c = &CogVR.desk;
    CogV3 f, r, u;
    if (!CogVR.autoCentered && CogVR.slot[CogVR.dIdx].cells) {
        CogVR.autoCentered = 1;
        CogVR_DeskRecenter();
    }
    if (CogVR.keyHome) {
        CogVR.keyHome = 0;
        CogVR_DeskRecenter();
    }
    if (CogVR.keyPov) {
        CogVR.keyPov = 0;
        CogVR.pov = !CogVR.pov;
        if (CogVR.pov) {
            CogVR.povSavedScale = CogVR.wScale;
            CogVR.wScale = 1.0f;
            c->pitch = -10.0f * COGVR_PI / 180.0f;
        } else {
            CogVR.wScale = CogVR.povSavedScale;
            CogVRCam_FreeBasis(c, &f, &r, &u);
            c->pos = CogV3_Sub(c->pos, CogV3_Scale(f, COGVR_DESK_DIST * 0.5f));
        }
        CogVR.sceneDirty = 1;
    }
    if (CogVR.mouseDX || CogVR.mouseDY) {
        c->yaw -= (float) CogVR.mouseDX * 0.004f;
        c->pitch = CogVR_Clamp(c->pitch - (float) CogVR.mouseDY * 0.004f, -1.55f, 1.55f);
        CogVR.mouseDX = CogVR.mouseDY = 0;
    }
    c->yaw += (float) (CogVR_KeyDown(VK_LEFT) - CogVR_KeyDown(VK_RIGHT)) * 2.1f * dt;
    c->pitch = CogVR_Clamp(c->pitch + (float) (CogVR_KeyDown(VK_UP) - CogVR_KeyDown(VK_DOWN)) * 1.5f * dt, -1.55f, 1.55f);
    if (!CogVR.pov) {
        CogVR.wheel += (int) ((CogVR_KeyDown(VK_OEM_PLUS) - CogVR_KeyDown(VK_OEM_MINUS)) * 12000.0f * dt);
    }
    CogVRCam_FreeBasis(c, &f, &r, &u);
    if (CogVR.wheel && !CogVR.pov) {
        /* world scale about the ground point under the screen centre (zooms towards it) */
        const int notches = CogVR.wheel / 120;
        float factor = 1.0f, ns;
        CogV3 pivot;
        int i;
        for (i = 0; (i < 40) && (i < ((notches < 0) ? -notches : notches)); i++) {
            factor *= (notches > 0) ? 1.15f : (1.0f / 1.15f);
        }
        ns = CogVR_Clamp(CogVR.wScale * factor, COGVR_SCALE_MIN, COGVR_SCALE_MAX);
        factor = ns / CogVR.wScale;
        if ((f.y < -0.05f) && (c->pos.y > 0.0f)) {
            pivot = CogV3_Add(c->pos, CogV3_Scale(f, c->pos.y / -f.y));
        } else {
            pivot = CogV3_Add(c->pos, CogV3_Scale(f, 20.0f));
        }
        c->pos = CogV3_Add(pivot, CogV3_Scale(CogV3_Sub(c->pos, pivot), 1.0f / factor));
        CogVR.wScale = ns;
        CogVR.wheel -= notches * 120;
    }
    if (CogVR.pov) {
        float fx, fz;
        CogVR_FocusCell(&fx, &fz);
        c->pos = CogV3_Make(fx, COGVR_EYE, fz);
        CogVR.wheel = 0;
    } else {
        /* 2.5 m/s, Shift x4, in cells */
        const float speed = 2.5f * (CogVR_KeyDown(VK_SHIFT) ? 4.0f : 1.0f) / CogVR.wScale * dt;
        CogV3 mv = CogV3_Make(0.0f, 0.0f, 0.0f);
        if (CogVR_KeyDown('W')) { mv = CogV3_Add(mv, f); }
        if (CogVR_KeyDown('S')) { mv = CogV3_Sub(mv, f); }
        if (CogVR_KeyDown('D')) { mv = CogV3_Add(mv, r); }
        if (CogVR_KeyDown('A')) { mv = CogV3_Sub(mv, r); }
        if (CogVR_KeyDown('E')) { mv.y += 1.0f; }
        if (CogVR_KeyDown('Q')) { mv.y -= 1.0f; }
        c->pos = CogV3_Add(c->pos, CogV3_Scale(mv, speed));
    }
}

/* the director's stream camera: map + per-frame boxes (the avatar) into streamView, shown in the window */
static void
CogVR_StreamRender(CogV3 headCell, float heading, float dt, const CogM4 *model, float fog)
{
    CogVRView *sv = &CogVR.streamView;
    CogVRCam *c = &CogVR.stream;
    CogVRShot shot;
    CogM4 vp;
    CogV3 eyeM;
    if (!sv->w && !CogVR_MakeView(sv, CogVR.streamW, CogVR.streamH, 1)) {
        return;
    }
    c->heading = heading;
    c->aspect = (float) sv->w / (float) sv->h;
    c->hasSubject2 = (CogVR.hdr.playerX >= 0) && (CogVR.hdr.playerY >= 0);
    c->subject2 = CogV3_Make((float) CogVR.hdr.playerX + 0.5f, 0.0f, (float) CogVR.hdr.playerY + 0.5f);
    CogVRCam_Update(c, dt, headCell, CogV3_Make(0.0f, 0.0f, 0.0f), CogVR.wScale, CogVR_WallHeight, &shot);
    CogVR_ShotMatrix(&shot, c->aspect, &vp, &eyeM);
    CogVR_BeginPass(sv);
    CogVR_SetCB(&vp, model, shot.eye, eyeM, fog);
    CogVR_DrawMap();
    CogVR_DrawDyn(&vp, eyeM, fog, 0);
    CogVR_EndPass(sv, 0);
    CogVR_BlitToWindow(sv->resSRV[0], sv->w, sv->h);
}

static void
CogVR_DesktopFrame(float dt)
{
    CogVRShot shot;
    CogM4 vp, model, head;
    CogV3 eyeM, f, r, u, hp;
    CogVRView *v = &CogVR.view;

    CogVR_DeskInput(dt);
    if (CogVR.keyStream) {
        CogVR.keyStream = 0;
        CogVR.deskStream = !CogVR.deskStream;
        CogVR_Log("COGVR: desktop window shows the %s", CogVR.deskStream ? "director's stream camera" : "free-fly camera");
    }
    CogM4_World(&model, CogVR.wYaw, CogVR.wScale, CogVR.wT);
    /* the avatar stands in for the headset user at the free-fly camera */
    CogVRCam_FreeBasis(&CogVR.desk, &f, &r, &u);
    hp = CogVR_CellToTrack(CogVR.desk.pos);
    CogM4_Identity(&head);
    head.m[0][0] = r.x; head.m[1][0] = r.y; head.m[2][0] = r.z;
    head.m[0][1] = u.x; head.m[1][1] = u.y; head.m[2][1] = u.z;
    head.m[0][2] = -f.x; head.m[1][2] = -f.y; head.m[2][2] = -f.z;
    head.m[0][3] = hp.x; head.m[1][3] = hp.y; head.m[2][3] = hp.z;
    CogVR_PublishInput(CogVR.desk.pos, f, -CogVR.desk.yaw, 1);
    CogVR_DynBegin();
    CogVR_AvatarDyn(&head, CogVR.desk.pos, dt);
    CogVR_IntegBannerDyn(&head);
    CogVR_StickArrowDyn(-CogVR.desk.yaw, (float) (CogVR_KeyDown('D') - CogVR_KeyDown('A')),
                        (float) (CogVR_KeyDown('W') - CogVR_KeyDown('S')), CogVR.pov);
    CogVR_DynUpload();

    if (CogVR.deskStream) {
        CogVR_StreamRender(CogVR.desk.pos, CogVR.desk.yaw, dt, &model, COGVR_DESK_FOG);
        IDXGISwapChain_Present(CogVR.swap, 1, 0);
        CogVR_Dump(CogVR.streamView.res[0], CogVR.streamView.w, CogVR.streamView.h, NULL, 0, 0);
        CogVR_Stats(CogVR.stream.pos);
        return;
    }
    if ((v->w != CogVR.winW) || (v->h != CogVR.winH)) {
        if (!CogVR_MakeView(v, CogVR.winW, CogVR.winH, 1)) {
            CogVR.quit = 1;
            return;
        }
    }
    CogVRCam_Update(&CogVR.desk, dt, CogVR.desk.pos, CogV3_Make(0.0f, 0.0f, 0.0f), CogVR.wScale, CogVR_WallHeight, &shot);
    CogVR_ShotMatrix(&shot, (float) v->w / (float) v->h, &vp, &eyeM);
    CogVR_BeginPass(v);
    CogVR_SetCB(&vp, &model, shot.eye, eyeM, COGVR_DESK_FOG);
    CogVR_DrawMap();
    CogVR_DrawDyn(&vp, eyeM, COGVR_DESK_FOG, 1);   /* the camera is the "headset": the avatar is not drawn here */
    CogVR_EndPass(v, 0);
    CogVR_BlitToWindow(v->resSRV[0], v->w, v->h);
    IDXGISwapChain_Present(CogVR.swap, 1, 0);
    CogVR_Dump(v->res[0], v->w, v->h, NULL, 0, 0);
    CogVR_Stats(shot.eye);
}

/* ------------------------------------------------------------------------------------------------------------- */
/* OpenVR                                                                                                         */
/* ------------------------------------------------------------------------------------------------------------- */

#define COGVR_VR_NEAR  0.02f
#define COGVR_VR_FAR   500.0f
#define COGVR_VR_FOG   0.05f
#define COGVR_VR_SCALE 0.025f            /* default tabletop: 1 cell = 2.5 cm */
#define COGVR_VR_POVSCALE 2.0f
#define COGVR_BTN(id) (1ull << (id))

/* VR_InitInternal2 + function tables; returns the DXGI adapter index the HMD is on, or -2 on failure */
static int
CogVR_InitOpenVR(void)
{
    CogVRInit2_t init2;
    CogVRGetGeneric_t getGeneric;
    CogVRErrDesc_t errDesc;
    EVRInitError err = EVRInitError_VRInitError_None;
    char iface[128];
    int32_t adapter = -1;

    CogVR.hVR = LoadLibraryA("openvr_api.dll");
    if (!CogVR.hVR) {
        CogVR_Log("COGVR: openvr_api.dll not found (error %lu): OpenVR off", (unsigned long) GetLastError());
        return -2;
    }
    init2 = (CogVRInit2_t) (void *) GetProcAddress(CogVR.hVR, "VR_InitInternal2");
    getGeneric = (CogVRGetGeneric_t) (void *) GetProcAddress(CogVR.hVR, "VR_GetGenericInterface");
    errDesc = (CogVRErrDesc_t) (void *) GetProcAddress(CogVR.hVR, "VR_GetVRInitErrorAsEnglishDescription");
    CogVR.vrShutdown = (CogVRShutdown_t) (void *) GetProcAddress(CogVR.hVR, "VR_ShutdownInternal");
    if (!init2 || !getGeneric || !CogVR.vrShutdown) {
        CogVR_Log("COGVR: openvr_api.dll lacks VR_InitInternal2 / VR_GetGenericInterface / VR_ShutdownInternal");
        return -2;
    }
    init2(&err, EVRApplicationType_VRApplication_Scene, NULL);
    if (err != EVRInitError_VRInitError_None) {
        CogVR_Log("COGVR: VR_InitInternal2 failed: %d %s", (int) err, errDesc ? errDesc(err) : "");
        CogVR.vrShutdown = NULL;
        return -2;
    }
    SDL20_snprintf(iface, sizeof (iface), "FnTable:%s", IVRSystem_Version);
    CogVR.vrSys = (struct VR_IVRSystem_FnTable *) getGeneric(iface, &err);
    if (!CogVR.vrSys || (err != EVRInitError_VRInitError_None)) {
        CogVR_Log("COGVR: %s unavailable: %d %s", iface, (int) err, errDesc ? errDesc(err) : "");
        CogVR.vrSys = NULL;
        return -2;
    }
    SDL20_snprintf(iface, sizeof (iface), "FnTable:%s", IVRCompositor_Version);
    CogVR.vrComp = (struct VR_IVRCompositor_FnTable *) getGeneric(iface, &err);
    if (!CogVR.vrComp || (err != EVRInitError_VRInitError_None)) {
        CogVR_Log("COGVR: %s unavailable: %d %s", iface, (int) err, errDesc ? errDesc(err) : "");
        CogVR.vrComp = NULL;
        return -2;
    }
    CogVR.vrSys->GetDXGIOutputInfo(&adapter);
    CogVR.vrSys->GetRecommendedRenderTargetSize(&CogVR.eyeW, &CogVR.eyeH);
    if ((CogVR.eyeW == 0) || (CogVR.eyeH == 0) || (CogVR.eyeW > 8192) || (CogVR.eyeH > 8192)) {
        CogVR_Log("COGVR: bad recommended eye size %ux%u", (unsigned) CogVR.eyeW, (unsigned) CogVR.eyeH);
        return -2;
    }
    CogVR.vrComp->SetTrackingSpace(ETrackingUniverseOrigin_TrackingUniverseStanding);
    CogVR_Log("COGVR: OpenVR up (%s, %s), eye %ux%u, DXGI adapter %d", IVRSystem_Version, IVRCompositor_Version,
              (unsigned) CogVR.eyeW, (unsigned) CogVR.eyeH, (int) adapter);
    {
        char path[MAX_PATH], *slash;
        int okay = 1, i;
        const struct { const char *name; VRActionHandle_t *handle; } actions[] = {
            {"/actions/cogmind/in/grab_move", &CogVR.aGrab}, {"/actions/cogmind/in/menu", &CogVR.aMenu},
            {"/actions/cogmind/in/fly", &CogVR.aFly}, {"/actions/cogmind/in/turn", &CogVR.aTurn},
            {"/actions/cogmind/in/boost", &CogVR.aBoost}, {"/actions/cogmind/in/trigger", &CogVR.aTrigger},
            {"/actions/cogmind/in/a_button", &CogVR.aA}, {"/actions/cogmind/in/squeeze", &CogVR.aSqueeze},
            {"/actions/cogmind/in/pose_left", &CogVR.aPose[0]}, {"/actions/cogmind/in/pose_right", &CogVR.aPose[1]},
            {"/actions/cogmind/out/haptic_left", &CogVR.aHaptic[0]}, {"/actions/cogmind/out/haptic_right", &CogVR.aHaptic[1]}
        };
        SDL20_snprintf(iface, sizeof(iface), "FnTable:%s", IVRInput_Version);
        CogVR.vrIn = (struct VR_IVRInput_FnTable *) getGeneric(iface, &err);
        path[0] = 0;
        if (!GetModuleFileNameA(NULL, path, sizeof(path)) || SDL20_strlen(path) >= sizeof(path)-1) { okay = 0; }
        slash = SDL20_strrchr(path, '\\');
        if (!slash || slash-path+SDL20_strlen("\\cogvr\\actions.json") >= sizeof(path)) { okay = 0; }
        if (okay) { SDL20_strlcpy(slash, "\\cogvr\\actions.json", sizeof(path)-(slash-path)); }
        if (!CogVR.vrIn || err != EVRInitError_VRInitError_None) { okay = 0; }
        if (okay && CogVR.vrIn->SetActionManifestPath(path) != EVRInputError_VRInputError_None) { okay = 0; }
        if (okay && CogVR.vrIn->GetActionSetHandle("/actions/cogmind", &CogVR.inSet) != EVRInputError_VRInputError_None) { okay = 0; }
        if (okay) for (i=0; i<SDL_arraysize(actions); i++) {
            if (CogVR.vrIn->GetActionHandle((char *)actions[i].name, actions[i].handle) != EVRInputError_VRInputError_None) { okay = 0; }
        }
        if (okay && (CogVR.vrIn->GetInputSourceHandle("/user/hand/left", &CogVR.inSrc[0]) != EVRInputError_VRInputError_None ||
                     CogVR.vrIn->GetInputSourceHandle("/user/hand/right", &CogVR.inSrc[1]) != EVRInputError_VRInputError_None)) { okay = 0; }
        if (!okay) { CogVR.vrIn = NULL; }
        CogVR_Log("COGVR: input = %s", okay ? "SteamVR actions (rebindable Index profile)" : "legacy controllers");
    }
    return (int) adapter;
}

static void
CogVR_VRProjections(void)
{
    int e;
    for (e = 0; e < 2; e++) {
        const HmdMatrix44_t p = CogVR.vrSys->GetProjectionMatrix((EVREye) e, COGVR_VR_NEAR, COGVR_VR_FAR);
        int i, j;
        for (i = 0; i < 4; i++) {
            for (j = 0; j < 4; j++) {
                CogVR.eyeProj[e].m[i][j] = p.m[i][j];
            }
        }
        /* reversed Z: z' = w - z */
        for (j = 0; j < 4; j++) {
            CogVR.eyeProj[e].m[2][j] = p.m[3][j] - p.m[2][j];
        }
    }
}

static CogV3
CogVR_PosOf(const CogM4 *m)
{
    return CogV3_Make(m->m[0][3], m->m[1][3], m->m[2][3]);
}

/*
 * Tabletop scales: the focus cell 0.45 m in front of and 0.55 m below the eyes. Human scales (>= 0.25 m per cell, e.g.
 * COGVR_SCALE=1.7): Cogmind stands beside you, 1 m ahead and 0.6 m to the right, its glyph at your eye level (lying in bed
 * or standing alike). Map north points away from the viewer.
 */
static void
CogVR_VRRecenter(const CogM4 *hmd)
{
    float fx, fz;
    const CogV3 h = CogVR_PosOf(hmd);
    CogV3 f = CogV3_Make(-hmd->m[0][2], 0.0f, -hmd->m[2][2]);
    const float l = CogV3_Len(f);
    CogV3 target;
    if (CogVR.pov) {
        CogVR.pov = 0;
        CogVR.wScale = CogVR.povSavedScale;
        CogVR.sceneDirty = 1;
    }
    f = (l > 0.001f) ? CogV3_Scale(f, 1.0f / l) : CogV3_Make(0.0f, 0.0f, -1.0f);
    CogVR_FocusCell(&fx, &fz);
    CogVR.wYaw = CogVR.atan2f_(-f.x, -f.z);
    if (CogVR.wScale >= 0.25f) {
        const CogV3 right = CogV3_Make(-f.z, 0.0f, f.x);
        const float glyphMid = (COGVR_HOVER + 4.0f * COGVR_UP_PIX) * CogVR.wScale;
        target = CogV3_Add(h, CogV3_Add(CogV3_Add(CogV3_Scale(f, 1.0f), CogV3_Scale(right, 0.6f)), CogV3_Make(0.0f, -0.1f - glyphMid, 0.0f)));
    } else {
        target = CogV3_Add(h, CogV3_Add(CogV3_Scale(f, 0.45f), CogV3_Make(0.0f, -0.55f, 0.0f)));
    }
    CogVR.wT = CogV3_Sub(target, CogV3_RotY(CogV3_Scale(CogV3_Make(fx, 0.0f, fz), CogVR.wScale), CogVR.wYaw));
    CogVR.grabMode = 0;
    CogVR.cruise = CogVR.flying = 0;
    CogVR.stickyState[0] = CogVR.stickyState[1] = 0;
    CogVR.seat = CogV3_Add(h, CogV3_Make(0.0f, -0.75f, 0.0f));   /* seated eye height above the chair */
    CogVR.seatYaw = CogVR.atan2f_(-f.x, -f.z);
}

/* POV: the player's eye point sits at the headset (re-placed when the player moves) */
static void
CogVR_VRPlacePov(const CogM4 *hmd)
{
    float fx, fz;
    CogVR_FocusCell(&fx, &fz);
    CogVR.wT = CogV3_Sub(CogVR_PosOf(hmd), CogV3_RotY(CogV3_Scale(CogV3_Make(fx, COGVR_EYE, fz), CogVR.wScale), CogVR.wYaw));
    CogVR.povPX = CogVR.hdr.playerX;
    CogVR.povPY = CogVR.hdr.playerY;
}

static void
CogVR_VRGrabStart(void)
{
    CogVR.grab0[0] = CogVR.hand[0].pos;
    CogVR.grab0[1] = CogVR.hand[1].pos;
    CogVR.g0Yaw = CogVR.wYaw;
    CogVR.g0Scale = CogVR.wScale;
    CogVR.g0T = CogVR.wT;
}

static void
CogVR_Haptic(int h)
{
    if (CogVR.vrIn) {
        CogVR.vrIn->TriggerHapticVibrationAction(CogVR.aHaptic[h], 0.0f, 0.02f, 160.0f, 0.6f, k_ulInvalidInputValueHandle);
    } else if (CogVR.hand[h].idx < 64) {
        CogVR.vrSys->TriggerHapticPulse(CogVR.hand[h].idx, 0, 1500);
    }
}

/* stick deadzone 0.15, rescaled to 0..1 */
static float
CogVR_Dead(float v)
{
    if (v > 0.15f) {
        return (v - 0.15f) / 0.85f;
    }
    if (v < -0.15f) {
        return (v + 0.15f) / 0.85f;
    }
    return 0.0f;
}

#include "cogmind_vrmusic.inc.c"
#include "cogmind_vrdeck.inc.c"

/* this frame's controller poses and actions: SteamVR Input (IVRInput, the action manifest) or the legacy controller state */
static void
CogVR_VRReadPad(void)
{
    struct CogVRPad *p = &CogVR.pad;
    int h;
    SDL20_zero(*p);
    if (CogVR.vrIn) {
        VRActiveActionSet_t set;
        InputDigitalActionData_t dd;
        InputAnalogActionData_t ad;
        SDL20_zero(set);
        set.ulActionSet = CogVR.inSet;
        CogVR.vrIn->UpdateActionState(&set, sizeof (set), 1);
        for (h = 0; h < 2; h++) {
            struct CogVRHand *hd = &CogVR.hand[h];
            InputPoseActionData_t pd;
            SDL20_zero(pd);
            hd->valid = (CogVR.vrIn->GetPoseActionDataForNextFrame(CogVR.aPose[h], ETrackingUniverseOrigin_TrackingUniverseStanding, &pd, sizeof (pd),
                                                                   k_ulInvalidInputValueHandle) == EVRInputError_VRInputError_None) &&
                        pd.bActive && pd.pose.bPoseIsValid;
            if (hd->valid) {
                CogM4_From34(&hd->pose, &pd.pose.mDeviceToAbsoluteTracking);
            }
            SDL20_zero(dd);
            if ((CogVR.vrIn->GetDigitalActionData(CogVR.aGrab, &dd, sizeof (dd), CogVR.inSrc[h]) == EVRInputError_VRInputError_None) && dd.bActive) {
                p->grab[h] = dd.bState != 0;
            }
            hd->st.ulButtonPressed = 0;
            SDL20_zero(ad);
            if (CogVR.vrIn->GetAnalogActionData(CogVR.aTrigger, &ad, sizeof(ad), CogVR.inSrc[h]) == EVRInputError_VRInputError_None && ad.bActive && ad.x > 0.75f) {
                hd->st.ulButtonPressed |= COGVR_BTN(EVRButtonId_k_EButton_SteamVR_Trigger);
            }
            SDL20_zero(dd);
            if (CogVR.vrIn->GetDigitalActionData(CogVR.aA, &dd, sizeof(dd), CogVR.inSrc[h]) == EVRInputError_VRInputError_None && dd.bActive && dd.bState) {
                hd->st.ulButtonPressed |= COGVR_BTN(EVRButtonId_k_EButton_A);
            }
            SDL20_zero(ad);
            if (CogVR.vrIn->GetAnalogActionData(CogVR.aSqueeze, &ad, sizeof(ad), CogVR.inSrc[h]) == EVRInputError_VRInputError_None && ad.bActive) {
                p->squeeze[h] = ad.x;
            }
        }
        SDL20_zero(dd);
        if ((CogVR.vrIn->GetDigitalActionData(CogVR.aMenu, &dd, sizeof (dd), k_ulInvalidInputValueHandle) == EVRInputError_VRInputError_None) && dd.bActive) {
            p->menu = dd.bState != 0;
        }
        SDL20_zero(dd);
        if ((CogVR.vrIn->GetDigitalActionData(CogVR.aBoost, &dd, sizeof (dd), k_ulInvalidInputValueHandle) == EVRInputError_VRInputError_None) && dd.bActive) {
            p->boost = dd.bState != 0;
        }
        SDL20_zero(ad);
        if ((CogVR.vrIn->GetAnalogActionData(CogVR.aFly, &ad, sizeof (ad), k_ulInvalidInputValueHandle) == EVRInputError_VRInputError_None) && ad.bActive) {
            p->flyX = ad.x;
            p->flyY = ad.y;
        }
        SDL20_zero(ad);
        if ((CogVR.vrIn->GetAnalogActionData(CogVR.aTurn, &ad, sizeof (ad), k_ulInvalidInputValueHandle) == EVRInputError_VRInputError_None) && ad.bActive) {
            p->turnX = ad.x;
            p->turnY = ad.y;
        }
    } else {
        for (h = 0; h < 2; h++) {
            struct CogVRHand *hd = &CogVR.hand[h];
            const TrackedDeviceIndex_t idx = CogVR.vrSys->GetTrackedDeviceIndexForControllerRole(
                h ? ETrackedControllerRole_TrackedControllerRole_RightHand : ETrackedControllerRole_TrackedControllerRole_LeftHand);
            if ((idx >= 64) || !CogVR.poses[idx].bPoseIsValid || !CogVR.vrSys->GetControllerState(idx, &hd->st, sizeof (hd->st))) {
                hd->valid = 0;
                hd->idx = k_unTrackedDeviceIndexInvalid;
                continue;
            }
            if (hd->idx != idx) {   /* new device: which legacy axis is the joystick (Index has a trackpad too), else a trackpad */
                int a;
                hd->idx = idx;
                hd->axis = -1;
                for (a = 0; a < 5; a++) {
                    ETrackedPropertyError perr = ETrackedPropertyError_TrackedProp_Success;
                    const int32_t type = CogVR.vrSys->GetInt32TrackedDeviceProperty(idx, (ETrackedDeviceProperty) (ETrackedDeviceProperty_Prop_Axis0Type_Int32 + a), &perr);
                    if ((perr == ETrackedPropertyError_TrackedProp_Success) && (type == EVRControllerAxisType_k_eControllerAxis_Joystick)) {
                        hd->axis = a;
                        break;
                    }
                    if ((perr == ETrackedPropertyError_TrackedProp_Success) && (type == EVRControllerAxisType_k_eControllerAxis_TrackPad) && (hd->axis < 0)) {
                        hd->axis = a;
                    }
                }
                if (hd->axis < 0) {
                    hd->axis = 0;
                }
                CogVR_Log("COGVR: %s controller = device %u, legacy stick axis %d", h ? "right" : "left", (unsigned) idx, hd->axis);
            }
            hd->valid = 1;
            CogM4_From34(&hd->pose, &CogVR.poses[idx].mDeviceToAbsoluteTracking);
            p->grab[h] = (hd->st.ulButtonPressed & COGVR_BTN(EVRButtonId_k_EButton_Grip)) != 0;
            p->squeeze[h] = p->grab[h] ? 1.0f : 0.0f;
            p->menu |= (hd->st.ulButtonPressed & COGVR_BTN(EVRButtonId_k_EButton_ApplicationMenu)) != 0;
            if (h == 0) {
                p->flyX = hd->st.rAxis[hd->axis].x;
                p->flyY = hd->st.rAxis[hd->axis].y;
                p->boost = (hd->st.ulButtonPressed & COGVR_BTN(EVRButtonId_k_EButton_Axis0 + hd->axis)) != 0;
            } else {
                p->turnX = hd->st.rAxis[hd->axis].x;
                p->turnY = hd->st.rAxis[hd->axis].y;
            }
        }
    }
    for (h = 0; h < 2; h++) {
        struct CogVRHand *hd = &CogVR.hand[h];
        if (hd->valid) {
            hd->pos = CogVR_PosOf(&hd->pose);
            hd->fwd = CogV3_Make(-hd->pose.m[0][2], -hd->pose.m[1][2], -hd->pose.m[2][2]);
        } else {
            p->grab[h] = 0;
        }
        {
            /* contextual grip: the deck takes a press at its controls; a sticky world grab is released by the next press */
            const int raw = p->grab[h];
            const int press = raw && !CogVR.stickyRaw[h], release = !raw && CogVR.stickyRaw[h];
            const int deck = CogVRDeck_Grip(h, press && !CogVR.stickyState[h], release);
            const int open = !(CogVRDeck.throttle && (h == 1));   /* throttle: the right squeeze in open air is tempo */
            CogVR.stickyRaw[h] = raw;
            if (CogVR.stickyGrip) {
                if (press && !deck && (open || CogVR.stickyState[h])) { CogVR.stickyState[h] = !CogVR.stickyState[h]; }
                if (!hd->valid) { CogVR.stickyState[h] = 0; }
                p->grab[h] = CogVR.stickyState[h];
            } else {
                p->grab[h] = raw && !deck && open;
            }
        }
        hd->grip = p->grab[h];
    }
}

/* world scale with the limits; a haptic tick and a log line when a limit is reached */
static float
CogVR_LimitScale(float s)
{
    const int st = (s <= COGVR_SCALE_MIN) ? -1 : ((s >= COGVR_SCALE_MAX) ? 1 : 0);
    if (st && (st != CogVR.clampState)) {
        CogVR_Log("COGVR: scale limit: 1 cell = %g m (%s)", (st < 0) ? COGVR_SCALE_MIN : COGVR_SCALE_MAX, (st < 0) ? "giant" : "tiny fairy");
        if (CogVR.mode == 2) {
            CogVR_Haptic(0);
            CogVR_Haptic(1);
        }
    }
    CogVR.clampState = st;
    return CogVR_Clamp(s, COGVR_SCALE_MIN, COGVR_SCALE_MAX);
}

static void
CogVR_VRTogglePov(const CogM4 *hmd)
{
    CogVR.pov = !CogVR.pov;
    CogVR.cruise = CogVR.flying = 0;
    CogVR.boostWas = CogVR.pad.boost;
    if (CogVR.pov) {
        CogVR.povSavedScale = CogVR.wScale;
        CogVR.wScale = COGVR_VR_POVSCALE;
        CogVR_VRPlacePov(hmd);
    } else {
        CogVR.wScale = CogVR.povSavedScale;
        CogVR_VRRecenter(hmd);
    }
    CogVR.sceneDirty = 1;
    CogVR.grabMode = 0;
}

/*
 * Tabletop Simulator style: grip = grab reality and pull yourself around (1:1), both grips = turn + scale about the hands'
 * midpoint (1 cell = COGVR_SCALE_MIN .. COGVR_SCALE_MAX m), left stick = fly along the left controller (cells/s, so speed is
 * proportional to the scale; boost = stick click), right stick x = smooth turn (COGVR_SNAPTURN=<deg>: snap turn), right
 * stick y = up/down, menu tap = Examine <-> Cogmind POV, menu hold >= 1 s = reset. Trigger and A are not used.
 */
static void
CogVR_VRInput(const CogM4 *hmd, float dt)
{
    const struct CogVRPad *p = &CogVR.pad;
    const struct CogVRHand *L = &CogVR.hand[0], *R = &CogVR.hand[1];
    const CogV3 hpos = CogVR_PosOf(hmd);
    const double now = CogGPU_Now();
    int gm, h;
    float tx, fy, fx, vy;

    CogVR_VRReadPad();
    CogVRDeck_Update(p);

    /* menu: tap = POV toggle, hold = reset */
    if (p->menu && !CogVR.menuWas) {
        CogVR.menuT0 = now;
        CogVR.menuFired = 0;
    }
    if (p->menu && !CogVR.menuFired && (now - CogVR.menuT0 >= 1000.0)) {
        CogVR.menuFired = 1;
        if (CogVR.pov) {
            CogVR.pov = 0;
            CogVR.sceneDirty = 1;
        }
        CogVR.wScale = CogVR.startScale ? CogVR.startScale : COGVR_VR_SCALE;
        CogVR_VRRecenter(hmd);
        CogVR_Haptic(0);
        CogVR_Haptic(1);
        CogVR_Log("COGVR: menu hold: reset (1 cell = %g m above Cogmind)", CogVR.wScale);
    }
    if (!p->menu && CogVR.menuWas && !CogVR.menuFired) {
        CogVR_VRTogglePov(hmd);
        CogVR_Log("COGVR: menu tap: %s", CogVR.pov ? "Cogmind POV" : "Examine");
    }
    CogVR.menuWas = p->menu;

    /* right stick: x = turn about the head (smooth 120 deg/s, or snap), y = up/down */
    tx = CogVR_Dead(p->turnX);
    if (CogVR.snapDeg > 0) {
        if (CogVR.snapArmed && ((p->turnX > 0.7f) || (p->turnX < -0.7f))) {
            const float d = (p->turnX > 0.0f ? 1.0f : -1.0f) * (float) CogVR.snapDeg * COGVR_PI / 180.0f;
            CogVR.wYaw += d;
            CogVR.wT = CogV3_Add(hpos, CogV3_RotY(CogV3_Sub(CogVR.wT, hpos), d));
            CogVR.snapArmed = 0;
            CogVR.grabMode = 0;
            CogVR_Log("COGVR: snap turn %+d deg", (p->turnX > 0.0f) ? CogVR.snapDeg : -CogVR.snapDeg);
        } else if ((p->turnX < 0.3f) && (p->turnX > -0.3f)) {
            CogVR.snapArmed = 1;
        }
    } else if (tx != 0.0f) {
        const float d = tx * 120.0f * COGVR_PI / 180.0f * dt;
        CogVR.wYaw += d;
        CogVR.wT = CogV3_Add(hpos, CogV3_RotY(CogV3_Sub(CogVR.wT, hpos), d));
        CogVR.grabMode = 0;   /* re-anchor a running grab */
        if (!CogVR.turning) {
            CogVR.turning = 1;
            CogVR.turnT0 = now;
            CogVR.turnAcc = 0.0f;
        }
        CogVR.turnAcc += d;
    }
    if (CogVR.turning && (tx == 0.0f)) {
        CogVR.turning = 0;
        CogVR_Log("COGVR: smooth turn %.1f deg in %.2f s", CogVR.turnAcc * 180.0f / COGVR_PI, (now - CogVR.turnT0) / 1000.0);
    }

    if (CogVR.pov) {
        if ((CogVR.hdr.playerX != CogVR.povPX) || (CogVR.hdr.playerY != CogVR.povPY)) {
            CogVR_VRPlacePov(hmd);
        }
        CogVR.grabMode = 0;
        return;
    }

    /* grips: one = drag 1:1, both = scale + turn about the hands' midpoint */
    gm = (p->grab[0] && p->grab[1]) ? 2 : ((p->grab[0] || p->grab[1]) ? 1 : 0);
    for (h = 0; h < 2; h++) {
        if (p->grab[h] != CogVR.padPrevGrab[h]) {
            CogVR_Haptic(h);
            CogVR.padPrevGrab[h] = p->grab[h];
        }
    }
    if ((gm != CogVR.grabMode) || ((gm == 1) && (CogVR.grabHand != (p->grab[1] ? 1 : 0)))) {
        if (CogVR.grabMode == 2) {
            CogVR_Log("COGVR: two-hand grab end: 1 cell = %g -> %g m, turned %.1f deg", CogVR.g0Scale, CogVR.wScale,
                      (CogVR.wYaw - CogVR.g0Yaw) * 180.0f / COGVR_PI);
        } else if (CogVR.grabMode == 1) {
            const CogV3 d = CogV3_Sub(CogVR.wT, CogVR.g0T);
            CogVR_Log("COGVR: drag end (%s hand): world moved %.3f m", CogVR.grabHand ? "right" : "left", CogV3_Len(d));
        }
        CogVR.grabMode = gm;
        CogVR.grabHand = p->grab[1] ? 1 : 0;
        CogVR_VRGrabStart();
    }
    if (gm == 1) {
        const int k = CogVR.grabHand;
        CogVR.wT = CogV3_Add(CogVR.g0T, CogV3_Sub(CogVR.hand[k].pos, CogVR.grab0[k]));
    } else if (gm == 2) {
        const CogV3 m0 = CogV3_Scale(CogV3_Add(CogVR.grab0[0], CogVR.grab0[1]), 0.5f);
        const CogV3 m1 = CogV3_Scale(CogV3_Add(L->pos, R->pos), 0.5f);
        const CogV3 v0 = CogV3_Sub(CogVR.grab0[1], CogVR.grab0[0]), v1 = CogV3_Sub(R->pos, L->pos);
        const float d0 = CogV3_Len(v0), d1 = CogV3_Len(v1);
        const float a0 = CogVR.atan2f_(-v0.z, v0.x), a1 = CogVR.atan2f_(-v1.z, v1.x);
        /* the world point under the start midpoint follows the midpoint */
        const CogV3 w = CogV3_Scale(CogV3_RotY(CogV3_Sub(m0, CogVR.g0T), -CogVR.g0Yaw), 1.0f / CogVR.g0Scale);
        if (d0 > 0.01f) {
            CogVR.wScale = CogVR_LimitScale(CogVR.g0Scale * d1 / d0);
        }
        CogVR.wYaw = CogVR.g0Yaw + (a1 - a0);
        CogVR.wT = CogV3_Sub(m1, CogV3_RotY(CogV3_Scale(w, CogVR.wScale), CogVR.wYaw));
    }

    /* left stick: fly along the left controller (10 cells/s, boost x5); right stick y: up/down */
    fx = CogVR_Dead(p->flyX);
    fy = CogVR_Dead(p->flyY);
    if (CogVR.companion) {             /* the left stick drives Cogmind (bridge); arms and the right stick move you */
        fx = fy = 0.0f;
    }
    vy = CogVR_Dead(p->turnY);
    if (p->boost && !CogVR.boostWas) {
        CogVR.cruise = !CogVR.cruise && ((fx != 0.0f) || (fy != 0.0f) || (vy != 0.0f));
        CogVR.cruiseX = fx; CogVR.cruiseY = fy; CogVR.cruiseV = vy;
        CogVR_Log("COGVR: cruise %s", CogVR.cruise ? "on (click again to stop)" : "off");
    }
    CogVR.boostWas = p->boost;
    if (CogVR.cruise) { fx = CogVR.cruiseX; fy = CogVR.cruiseY; vy = CogVR.cruiseV; }
    if ((gm == 0) && ((fx != 0.0f) || (fy != 0.0f) || (vy != 0.0f))) {
        const float speed = 20.0f * CogVR.wScale * (CogVR.cruise ? 5.0f : 1.0f) * dt;
        CogV3 mv = CogV3_Make(0.0f, vy, 0.0f);
        if (L->valid) {
            const CogV3 right = CogV3_Make(L->pose.m[0][0], L->pose.m[1][0], L->pose.m[2][0]);
            mv = CogV3_Add(mv, CogV3_Add(CogV3_Scale(L->fwd, fy), CogV3_Scale(right, fx)));
        }
        CogVR.wT = CogV3_Sub(CogVR.wT, CogV3_Scale(mv, speed));
        if (!CogVR.flying) {
            CogVR.flying = 1;
            CogVR.flyT0 = now;
            CogVR.flyAcc = 0.0f;
            CogVR.flyBoost = 0;
        }
        CogVR.flyAcc += CogV3_Len(mv) * speed / CogVR.wScale;
        CogVR.flyBoost |= p->boost;
    } else if (CogVR.flying) {
        CogVR.flying = 0;
        CogVR_Log("COGVR: fly %.1f cells in %.2f s%s", CogVR.flyAcc, (now - CogVR.flyT0) / 1000.0, CogVR.flyBoost ? " (boost)" : "");
    }
}

/* the direction the bridge's head-relative rule maps the stick to now: 0 = map north, clockwise in 45 degree steps */
static int
CogVR_StickDir(float headYaw, float sx, float sy)
{
    const float a = ((sx * sx + sy * sy > 0.25f) ? CogVR.atan2f_(sx, sy) : 0.0f) + headYaw;
    const int k = (int) (a * 4.0f / COGVR_PI + 64.5f) - 64;   /* floor(x + 0.5) for |x| < 64 */
    return ((k % 8) + 8) % 8;
}

/*
 * The arrow around Cogmind: where the left stick moves him from where you stand (bright while deflected, dim = "stick
 * forward"). Walk around him and it snaps to the cardinal you face. Headset/desktop view only.
 */
static void
CogVR_StickArrowDyn(float headYaw, float sx, float sy, int drives)
{
    const COGVR_Header *hd = &CogVR.hdr;
    const int live = drives && (sx * sx + sy * sy > 0.25f);
    const Uint32 rgba = live ? CogVR_Pack(1.0f, 0.85f, 0.25f, 1.0f) : CogVR_Pack(0.30f, 0.80f, 1.0f, drives ? 0.5f : 0.15f);
    CogM4 m;
    CogV3 p;
    if (!CogVR.slot[CogVR.dIdx].cells || (hd->playerX < 0) || (hd->playerY < 0) || (hd->playerX >= hd->mapW) || (hd->playerY >= hd->mapH)) {
        return;
    }
    p = CogV3_RotY(CogV3_Make(((float) hd->playerX + 0.5f) * CogVR.wScale, 0.0f, ((float) hd->playerY + 0.5f) * CogVR.wScale), CogVR.wYaw);
    CogM4_World(&m, CogVR.wYaw - (float) CogVR_StickDir(headYaw, sx, sy) * (COGVR_PI / 4.0f), CogVR.wScale, CogV3_Add(CogVR.wT, p));
    CogVR_DynBatch(&m, 2);
    /* cell units, outside the halo ring (radius 0.68) so the upright glyph does not cover it from above */
    CogVR_DynBox(0.0f, 0.05f, -1.15f, 0.07f, 0.025f, 0.35f, rgba);   /* shaft */
    CogVR_DynBox(0.0f, 0.05f, -1.55f, 0.18f, 0.025f, 0.06f, rgba);   /* head */
    CogVR_DynBox(0.0f, 0.05f, -1.67f, 0.09f, 0.025f, 0.06f, rgba);   /* tip */
}

/*
 * Grounding: a chair floating under you (tracking space, fixed while the world moves) and a compass ring around it with
 * the map's N/E/S/W, which turn when you turn or grab the world. Headset only.
 */
static void
CogVR_SeatDyn(void)
{
    static const char *const cardinal[4] = { "N", "E", "S", "W" };
    const Uint32 frame = CogVR_Pack(0.16f, 0.18f, 0.24f, 0.05f), glow = CogVR_Pack(0.30f, 0.95f, 1.0f, 0.8f);
    CogM4 m;
    int k;
    CogM4_World(&m, CogVR.seatYaw, 1.0f, CogVR.seat);
    CogVR_DynBatch(&m, 2);
    CogVR_DynBox(0.0f, 0.0f, 0.0f, 0.25f, 0.035f, 0.25f, frame);                 /* seat */
    CogVR_DynBox(0.0f, -0.04f, 0.0f, 0.26f, 0.006f, 0.26f, glow);               /* underglow */
    CogVR_DynBox(0.0f, 0.36f, 0.24f, 0.25f, 0.34f, 0.035f, frame);             /* backrest */
    CogVR_DynBox(-0.28f, 0.16f, 0.0f, 0.03f, 0.03f, 0.22f, frame);             /* armrests */
    CogVR_DynBox(0.28f, 0.16f, 0.0f, 0.03f, 0.03f, 0.22f, frame);
    CogVR_DynBox(0.0f, -0.35f, 0.0f, 0.035f, 0.31f, 0.035f, frame);            /* column */
    for (k = 0; k < 4; k++) {
        /* map direction k (N, E, S, W) in tracking space; the letter faces the chair */
        const CogV3 d = CogV3_RotY(CogV3_Make((k == 1) ? 1.0f : ((k == 3) ? -1.0f : 0.0f), 0.0f, (k == 0) ? -1.0f : ((k == 2) ? 1.0f : 0.0f)), CogVR.wYaw);
        const CogV3 at = CogV3_Add(CogVR.seat, CogV3_Make(d.x * 0.9f, -0.02f, d.z * 0.9f));
        CogM4_World(&m, CogVR.atan2f_(-d.x, -d.z), 1.0f, at);
        CogVR_DynBatch(&m, 2);
        CogVR_DynText(cardinal[k], -0.048f, 0.10f, 0.0f, 0.012f, (k == 0) ? CogVR_Pack(1.0f, 0.35f, 0.30f, 1.0f) : glow);
        CogVR_DynBox(0.0f, -0.02f, 0.0f, 0.02f, 0.004f, 0.06f, glow);             /* tick on the ring */
    }
}

/* per-frame boxes: the headset user's avatar (stream camera only) and the controllers (everywhere) */
static void
CogVR_VRBuildDyn(const CogM4 *hmd, CogV3 headCell, float dt)
{
    int h;
    CogVR_DynBegin();
    CogVR_AvatarDyn(hmd, headCell, dt);
    CogVR_IntegBannerDyn(hmd);
    {
        const CogV3 hf = CogV3_RotY(CogV3_Make(-hmd->m[0][2], 0.0f, -hmd->m[2][2]), -CogVR.wYaw);
        CogVR_StickArrowDyn(CogVR.atan2f_(hf.x, -hf.z), CogVR.pad.flyX, CogVR.pad.flyY, CogVR.pov || CogVR.companion);
    }
    CogVR_SeatDyn();
    CogVRDeck_Dyn();
    /* controllers, controller space: body and pointer ray */
    for (h = 0; h < 2; h++) {
        if (CogVR.hand[h].valid) {
            CogVR_DynBatch(&CogVR.hand[h].pose, 1);
            CogVR_DynBox(0.0f, -0.01f, 0.05f, 0.018f, 0.014f, 0.06f, CogVR_Pack(0.35f, 0.38f, 0.45f, 0.15f));
            if (CogVR.pov) {
                CogVR_DynBox(0.0f, 0.025f, -0.075f, 0.03f, 0.03f, 0.11f, CogVR_Pack(0.3f, 0.25f, 0.4f, 0.1f));
                CogVR_DynBox(0.0f, 0.025f, -0.20f, 0.014f, 0.014f, 0.035f, CogVR_Pack(0.5f, 0.25f, 0.8f, 0.6f));
                CogVR_DynBox(0.0f, -0.06f, -0.03f, 0.023f, 0.045f, 0.025f, CogVR_Pack(0.2f, 0.2f, 0.25f, 0.1f));
            }
            CogVR_DynBox(0.0f, 0.0f, -1.0f, 0.0015f, 0.0015f, 1.0f, CogVR_Pack(0.30f, 0.95f, 1.0f, 1.0f));
        }
    }
    CogVR_DynUpload();
}

static void
CogVR_VRFrame(float dt)
{
    CogM4 hmd, eyeToHead, eyePose, view, vp, model;
    CogV3 headCell, headPos;
    struct VREvent_t ev;
    int e;
    const EVRCompositorError werr = CogVR.vrComp->WaitGetPoses(CogVR.poses, 64, NULL, 0);

    while (CogVR.vrSys->PollNextEvent(&ev, sizeof (ev))) {
        if (ev.eventType == EVREventType_VREvent_Quit) {
            CogVR_Log("COGVR: SteamVR quit: VR renderer off");
            CogVR.vrSys->AcknowledgeQuit_Exiting();
            CogVR.quit = 1;
            return;
        }
    }
    /* DoNotHaveFocus: another scene app (SteamVR Home) has focus. Keep rendering and submitting -- submitted frames are how
       an app takes focus. The compositor may leave the poses empty then: read them from IVRSystem instead. */
    if ((werr == EVRCompositorError_VRCompositorError_DoNotHaveFocus) && !CogVR.poses[k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
        CogVR.vrSys->GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin_TrackingUniverseStanding, 0.011f, CogVR.poses, 64);
    }
    {
        static int lastErr = -1, lastValid = -1;
        const int valid = CogVR.poses[k_unTrackedDeviceIndex_Hmd].bPoseIsValid != 0;
        if (((int) werr != lastErr) || (valid != lastValid)) {
            CogVR_Log("COGVR: WaitGetPoses error %d, headset pose %s", (int) werr, valid ? "valid" : "invalid");
            lastErr = (int) werr;
            lastValid = valid;
        }
    }
    if (((werr != EVRCompositorError_VRCompositorError_None) && (werr != EVRCompositorError_VRCompositorError_DoNotHaveFocus)) ||
        !CogVR.poses[k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
        CogVR_IntegInternal(COGVR_HOLD_VR, 1);
        Sleep(5);
        return;
    }
    CogVR_IntegInternal(COGVR_HOLD_VR, 0);
    CogM4_From34(&hmd, &CogVR.poses[k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking);
    headPos = CogVR_PosOf(&hmd);
    if (!CogVR.autoCentered && CogVR.slot[CogVR.dIdx].cells) {
        CogVR.autoCentered = 1;
        CogVR_VRRecenter(&hmd);
    }
    CogVR_VRInput(&hmd, dt);
    {
        const float demand = (CogVR.flying || CogVR.grabMode) ? 1.0f : 0.0f;
        const float goal = CogVR.vignetteStrength * SDL_max(demand, CogVR.turning ? 0.65f : 0.0f);
        CogVR.vignette += (goal - CogVR.vignette) * CogVR_Clamp(dt * 5.0f, 0.0f, 1.0f);
    }
    CogM4_World(&model, CogVR.wYaw, CogVR.wScale, CogVR.wT);
    headCell = CogVR_TrackToCell(headPos);
    CogVR_VRBuildDyn(&hmd, headCell, dt);
    {
        const CogV3 rayOrigin = CogVR.hand[1].valid ? CogVR_TrackToCell(CogVR.hand[1].pos) : headCell;
        const CogV3 trackF = CogVR.hand[1].valid ? CogVR.hand[1].fwd : CogV3_Make(-hmd.m[0][2], -hmd.m[1][2], -hmd.m[2][2]);
        const CogV3 f = CogV3_RotY(trackF, -CogVR.wYaw);
        const CogV3 hf = CogV3_RotY(CogV3_Make(-hmd.m[0][2], 0.0f, -hmd.m[2][2]), -CogVR.wYaw);
        CogVR_PublishInput(rayOrigin, f, CogVR.atan2f_(hf.x, -hf.z), 0);
        CogVR_FlushHaptics();
    }

    for (e = 0; e < 2; e++) {
        const HmdMatrix34_t eh = CogVR.vrSys->GetEyeToHeadTransform((EVREye) e);
        Texture_t tex;
        VRTextureBounds_t bounds;
        CogM4_From34(&eyeToHead, &eh);
        CogM4_Mul(&eyePose, &hmd, &eyeToHead);
        CogM4_RigidInverse(&view, &eyePose);
        CogM4_Mul(&vp, &CogVR.eyeProj[e], &view);
        CogVR_BeginPass(&CogVR.view);
        CogVR_SetCB(&vp, &model, headCell, headPos, COGVR_VR_FOG);
        CogVR_DrawMap();
        CogVR_DrawDyn(&vp, headPos, COGVR_VR_FOG, 1);
        CogVR.postEye = 1.0f;
        CogVR_EndPass(&CogVR.view, e);
        CogVR.postEye = 0.0f;
        tex.handle = CogVR.view.res[e];
        tex.eType = ETextureType_TextureType_DirectX;
        tex.eColorSpace = EColorSpace_ColorSpace_Auto;
        bounds.uMin = 0.0f; bounds.vMin = 0.0f; bounds.uMax = 1.0f; bounds.vMax = 1.0f;
        CogVR.vrComp->Submit((EVREye) e, &tex, &bounds, EVRSubmitFlags_Submit_Default);
    }
    CogVR.vrComp->PostPresentHandoff();

    /* the window: the director's stream camera with the avatar, every other headset frame */
    CogVR.streamDt += dt;
    CogVR.streamFrame ^= 1;
    if (CogVR.streamFrame && CogVR.hwnd && !IsIconic(CogVR.hwnd) && CogVR_HandleResize()) {
        const CogV3 fc = CogV3_RotY(CogV3_Make(-hmd.m[0][2], 0.0f, -hmd.m[2][2]), -CogVR.wYaw);
        CogVR_StreamRender(headCell, CogVR.atan2f_(-fc.x, -fc.z), CogVR.streamDt, &model, COGVR_VR_FOG);
        CogVR.streamDt = 0.0f;
        IDXGISwapChain_Present(CogVR.swap, 0, 0);
    }
    CogVR_Dump(CogVR.view.res[0], CogVR.view.w, CogVR.view.h, CogVR.streamView.res[0], CogVR.streamView.w, CogVR.streamView.h);
    CogVR_Stats(headCell);
}

/* ------------------------------------------------------------------------------------------------------------- */
/* render thread                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------- */

static void
CogVR_Free(void)
{
    if (CogVR.ctx) {
        ID3D11DeviceContext_ClearState(CogVR.ctx);
    }
    CogMesh_Free();
    if (CogVR.vrShutdown) {
        CogVR.vrShutdown();
        CogVR.vrShutdown = NULL;
    }
    CogVR.vrSys = NULL;
    CogVR.vrComp = NULL;
    CogVR_FreeView(&CogVR.view);
    CogVR_FreeView(&CogVR.streamView);
    COGVR_REL(CogVR.staging);
    COGVR_REL(CogVR.backRTV);
    COGVR_REL(CogVR.swap);
    COGVR_REL(CogVR.sampler);
    COGVR_REL(CogVR.dsBlit);
    COGVR_REL(CogVR.dsScene);
    COGVR_REL(CogVR.rsBlit);
    COGVR_REL(CogVR.rsScene);
    COGVR_REL(CogVR.cbPost);
    COGVR_REL(CogVR.cb);
    COGVR_REL(CogVR.dynVB);
    COGVR_REL(CogVR.instVB);
    COGVR_REL(CogVR.cubeIB);
    COGVR_REL(CogVR.cubeVB);
    COGVR_REL(CogVR.layout);
    COGVR_REL(CogVR.psComp);
    COGVR_REL(CogVR.psBlur);
    COGVR_REL(CogVR.psBright);
    COGVR_REL(CogVR.psBlit);
    COGVR_REL(CogVR.vsBlit);
    COGVR_REL(CogVR.ps);
    COGVR_REL(CogVR.vs);
    COGVR_REL(CogVR.ctx);
    COGVR_REL(CogVR.dev);
    COGVR_REL(CogVR.adapter);
    COGVR_REL(CogVR.factory);
    if (CogVR.hwnd) {
        DestroyWindow(CogVR.hwnd);
        CogVR.hwnd = NULL;
    }
    if (CogVR.inst) {
        SDL20_free(CogVR.inst);
        CogVR.inst = NULL;
    }
    if (CogVR.prevKey) {
        SDL20_free(CogVR.prevKey);
        CogVR.prevKey = NULL;
    }
}

static int
CogVR_RenderInit(void)
{
    int adapter = -1;
    CogVR.inst = (CogVRInst *) SDL20_malloc(COGVR_MAXINST * sizeof (CogVRInst));
    if (!CogVR.inst) {
        CogVR_Log("COGVR: out of memory");
        return 0;
    }
    CogVR_InitFont();
    if (CogVR.mode == 2) {
        adapter = CogVR_InitOpenVR();
        if (adapter == -2) {
            return 0;
        }
    }
    if (!CogVR_InitDevice(adapter) || !CogVR_InitWindow()) {
        return 0;
    }
    if (CogVR.mode == 2) {
        CogVR_VRProjections();
        if (!CogVR_MakeView(&CogVR.view, (int) CogVR.eyeW, (int) CogVR.eyeH, 2)) {
            return 0;
        }
        CogVR.wScale = CogVR.startScale ? CogVR.startScale : COGVR_VR_SCALE;
        CogVR.wYaw = 0.0f;
        CogVR.wT = CogV3_Make(0.0f, 0.0f, 0.0f);
        CogVR.hand[0].idx = CogVR.hand[1].idx = k_unTrackedDeviceIndexInvalid;
        CogVR.snapArmed = 1;
    } else {
        CogVR.wScale = COGVR_DESK_SCALE;
        CogVR.desk.kind = COGVR_CAM_FREE;
        CogVR.desk.pos = CogV3_Make(40.0f, 20.0f, 40.0f);
        CogVR.desk.pitch = -60.0f * COGVR_PI / 180.0f;
    }
    CogVR.povSavedScale = CogVR.wScale;
    CogVR.stream.kind = COGVR_CAM_DIRECTOR;
    return 1;
}

static DWORD WINAPI
CogVR_Thread(LPVOID unused)
{
    double last;
    (void) unused;
    CogVR.t0 = CogGPU_Now();
    CogVR.statT0 = CogVR.t0;
    if (!CogVR_RenderInit()) {
        CogVR_Log("COGVR: renderer init failed: 3D view off (the game is not affected)");
        CogVR_Free();
        CogVR.rstate = COGVR_FAILED;
        return 0;
    }
    CogVR_Log("COGVR: renderer running (%s)", (CogVR.mode == 2) ? "openvr" : "desktop");
    CogVR_IntegInit();
    CogVRDeck_Init();
    CogVRMusic_Init();
    CogVR.rstate = COGVR_RUNNING;
    last = CogGPU_Now();
    while (!CogVR.quit) {
        double now;
        float dt;
        CogVR_Pump();
        if (CogVR.quit) {
            break;
        }
        CogVR_TakeSnapshot();
        CogVR_IntegPoll();
        if (CogVR.mode == 1) {
            CogVRDeck_DeskKeys();
        }
        CogVRDeck_TickUpdate();
        CogVRMusic_Update();
        CogVR_BeatUpdate(CogGPU_Now());
        now = CogGPU_Now();
        dt = (float) ((now - last) / 1000.0);
        if (dt > 0.1f) {
            dt = 0.1f;
        }
        last = now;
        if (CogVR.mode == 2) {
            CogVR_VRFrame(dt);
        } else {
            if (!CogVR_HandleResize()) {
                Sleep(30);
                continue;
            }
            CogVR_DesktopFrame(dt);
            /* the swap chain may not block on every system: cap the preview at ~144 fps */
            now = CogGPU_Now() - last;
            if (now < 6.9) {
                Sleep((DWORD) (6.9 - now));
            }
        }
    }
    CogVR.rstate = COGVR_STOPPED;
    CogVR_Log("COGVR: renderer stopped");
    CogVR_Free();
    return 0;
}

static int
CogVR_LoadMath(void)
{
    int okay = 1;
    CogVR.sinf_ = (float (SDLCALL *)(float)) LoadSDL20Symbol("SDL_sinf", &okay);
    CogVR.cosf_ = (float (SDLCALL *)(float)) LoadSDL20Symbol("SDL_cosf", &okay);
    CogVR.tanf_ = (float (SDLCALL *)(float)) LoadSDL20Symbol("SDL_tanf", &okay);
    CogVR.sqrtf_ = (float (SDLCALL *)(float)) LoadSDL20Symbol("SDL_sqrtf", &okay);
    CogVR.atan2f_ = (float (SDLCALL *)(float, float)) LoadSDL20Symbol("SDL_atan2f", &okay);
    CogVR.logf_ = (float (SDLCALL *)(float)) LoadSDL20Symbol("SDL_logf", &okay);
    return okay;
}

static void
CogVR_Start(void)
{
    HANDLE th;
    CogVR.started = 1;
    CogVR.rstate = COGVR_FAILED;
    if (!CogVR_LoadMath()) {
        CogVR_Log("COGVR: SDL2 math functions missing: off");
        return;
    }
    CogVR.mutex = SDL20_CreateMutex();
    if (!CogVR.mutex) {
        CogVR_Log("COGVR: SDL_CreateMutex failed: off");
        return;
    }
    CogVR.wIdx = 0;
    CogVR.rIdx = 1;
    CogVR.dIdx = 2;
    CogVR.rstate = COGVR_STARTING;
    th = CreateThread(NULL, 0, CogVR_Thread, NULL, 0, NULL);
    if (!th) {
        CogVR.rstate = COGVR_FAILED;
        CogVR_Log("COGVR: CreateThread failed: off");
        return;
    }
    CloseHandle(th);
    CogVR_Log("COGVR: mode %s, render thread started", (CogVR.mode == 2) ? "openvr" : "desktop");
}

#else
static void CogVR_AudioTap(const Uint8 *stream, int len, const SDL_AudioSpec *spec) { (void) stream; (void) len; (void) spec; }
static void CogVR_MusicMix(Uint8 *stream, int len, const SDL_AudioSpec *spec) { (void) stream; (void) len; (void) spec; }
#endif /* _WIN32 */

/* ------------------------------------------------------------------------------------------------------------- */
/* exports (game thread)                                                                                          */
/* ------------------------------------------------------------------------------------------------------------- */

DECLSPEC12 int SDLCALL
COGVR_Abi(void)
{
    return COGVR_ABI;
}

DECLSPEC12 COGVR_Cell * SDLCALL
COGVR_Begin(int mapW, int mapH)
{
#ifdef _WIN32
    CogVRSlot *s;
    double now;
    const double period = 1000.0 / COGVR_MAXRATE;
    size_t need;

    CogVR.inPass = 0;   /* a Begin without End is abandoned */
    CogVR_InitEnv();
    if (!CogVR.mode || (mapW <= 0) || (mapH <= 0) || (mapW > COGVR_MAXDIM) || (mapH > COGVR_MAXDIM)) {
        return NULL;
    }
    if (!CogVR.started) {
        CogVR_Start();
    }
    if (CogVR.rstate != COGVR_RUNNING) {
        return NULL;
    }
    /* throttle: slots every 1/COGVR_MAXRATE s, the first frame at or after a slot takes it, no catch-up */
    now = CogGPU_Now();
    if (now < CogVR.nextSlot) {
        return NULL;
    }
    CogVR.nextSlot += period;
    if (CogVR.nextSlot < now) {
        CogVR.nextSlot = now + period;
    }
    s = &CogVR.slot[CogVR.wIdx];
    need = (size_t) mapW * (size_t) mapH;
    if ((size_t) s->cap < need) {
        SDL20_free(s->cells);
        s->cells = (COGVR_Cell *) SDL20_malloc(need * sizeof (COGVR_Cell));
        s->cap = s->cells ? (int) need : 0;
        if (!s->cells) {
            return NULL;
        }
    }
    CogVR.inPass = 1;
    CogVR.passW = mapW;
    CogVR.passH = mapH;
    return s->cells;
#else
    (void) mapW;
    (void) mapH;
    return NULL;
#endif
}

DECLSPEC12 void SDLCALL
COGVR_End(const COGVR_Header *header)
{
#ifdef _WIN32
    int t;
    if (!CogVR.inPass) {
        return;
    }
    CogVR.inPass = 0;
    if (!header || (header->mapW != CogVR.passW) || (header->mapH != CogVR.passH)) {
        return;
    }
    CogVR.slot[CogVR.wIdx].hdr = *header;
    CogVR.slot[CogVR.wIdx].version = ++CogVR.version;
    SDL20_LockMutex(CogVR.mutex);
    t = CogVR.rIdx;
    CogVR.rIdx = CogVR.wIdx;
    CogVR.wIdx = t;
    CogVR.fresh = 1;
    SDL20_UnlockMutex(CogVR.mutex);
    CogVR_SaveHeader(header);
#else
    (void) header;
#endif
}
