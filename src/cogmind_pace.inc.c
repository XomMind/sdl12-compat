/*
 * cogmind_pace.inc.c -- frame pacing and latency instrumentation of the Cogmind GPU shim.
 * #included by cogmind_gpu.inc.c (so it shares the statics of SDL12_compat.c and of the GPU extension).
 *
 * Everything here is OFF by default; with none of the environment variables below set, the only cost is a few predictable
 * branches per SDL_Flip / SDL_PollEvent / present.
 *
 *   COGGPU_LOG=<file>    statistics, one block every ~2 s (see CogGPU_PaceEmit): PRESENT (present-to-present interval
 *                        median/p95/p99/max, count of intervals > 1.5x median), LOOP (period between consecutive event-drain
 *                        starts = game loop iteration), FLIPCALL (SDL_Flip entry to return), LATENCY (injected key), PACE.
 *   COGGPU_INJECT=<ms>   a helper thread pushes a synthetic SDL 1.2 key press/release pair (F15, sym 296: the game's input
 *                        handler has no binding for it) through the normal event queue every <ms> (+-25% pseudo-random, so
 *                        the phase against the frame loop is sampled uniformly), and the shim logs the latency from the push
 *                        to the end of the first present after the game polled the event. The delivered key counts as a
 *                        change of the screen, so the (otherwise skipped) present really happens, like after a real key.
 *   COGGPU_PACE=<hz>|vsync   precise frame pacer, see CogPace_Wait.
 *   COGGPU_PACE_MARGIN_US, COGGPU_PACE_SPIN_US, COGGPU_PACE_DELAY=0   pacer tuning (see below).
 *   COGGPU_SKIP=0        never skip the present of an unchanged GPU screen (default: skip, heartbeat every 250 ms).
 *
 * Times are milliseconds from QueryPerformanceCounter (CogGPU_Now).
 */

#define COGPACE_N 4096                 /* samples kept per series and 2 s window */

typedef struct CogSeries {
    double v[COGPACE_N];
    int n;
} CogSeries;

static struct CogPaceState {
    int init;
    /* configuration */
    int mode;                          /* 0 off, 1 fixed rate, 2 follow the display (vsync) */
    double hz;
    double marginMs, spinMs;
    int swallowDelay;                  /* make the game's limiter SDL_Delay a no-op while pacing */
    int skipPresents;                  /* skip the present of an unchanged GPU screen */
    int skipExplicit;
    double injectMs;
    SDL_bool clock;                    /* clock reads needed: log, pacer or injection */

    /* game loop */
    volatile int afterFlip;            /* SDL_Flip returned and the next SDL_PollEvent has not run yet */
    double tFlipEnter, tLastDrain;
    double tWaitEnd;                   /* the pacer released the current drain at this time */
    int workPending;
    double workEma;                    /* ms from release of the drain to the end of the following present */
    double gridOrigin, period;         /* slot k = gridOrigin + k * period: the times the pacer aims the present at */
    int gridValid;
    double lastSlot;
    double tPresent0, tCall0;          /* PresentScreen start; start of SDL_RenderPresent */
    double tPrevEnd;                   /* end of the previous PresentScreen */
    double tPrevRealEnd;               /* end of the previous real (not skipped) present */
    double tPrevReal;                  /* end of the previous real present (vsync period estimate) */
    double realIv[32]; int realIvN;
    /* injection */
    volatile int injPending, injDelivered;
    volatile double injT, injDeliverT;

    /* statistics of the current window */
    double winT0;
    int frames, real, missed, swallowed, waits;
    double presentMs, waitMs;
    CogSeries iv, riv, loop, flip, lat, toDrain, d2p;
} CogPace;

static void CogPace_PresentEnd(int real);

/* ------------------------------------------------------------------------------------------------------------- */
/* configuration                                                                                                  */
/* ------------------------------------------------------------------------------------------------------------- */

static void
CogPace_Init(void)
{
    char buf[64];
    const char *v;
    if (CogPace.init) {
        return;
    }
    CogPace.init = 1;
    CogGPU_InitEnv();

    CogPace.marginMs = 0.5;
    CogPace.spinMs = 2.0;
    CogPace.swallowDelay = 1;
    CogPace.skipPresents = 1;

    v = SDL12COMPAT_getenv_unsafe("COGGPU_PACE");
    if (v && *v) {
        SDL20_snprintf(buf, sizeof (buf), "%s", v);
        if (!SDL20_strcasecmp(buf, "vsync")) {
            CogPace.mode = 2;
            CogPace.marginMs = 1.0;
        } else {
            const double hz = SDL20_strtod(buf, NULL);
            if (hz >= 10.0 && hz <= 1000.0) {
                CogPace.mode = 1;
                CogPace.hz = hz;
                CogPace.period = 1000.0 / hz;
            }
        }
    }
    v = SDL12COMPAT_getenv_unsafe("COGGPU_PACE_MARGIN_US");
    if (v && *v) {
        CogPace.marginMs = SDL20_strtod(v, NULL) / 1000.0;
    }
    v = SDL12COMPAT_getenv_unsafe("COGGPU_PACE_SPIN_US");
    if (v && *v) {
        CogPace.spinMs = SDL20_strtod(v, NULL) / 1000.0;
    }
    v = SDL12COMPAT_getenv_unsafe("COGGPU_PACE_DELAY");
    if (v && (v[0] == '0')) {
        CogPace.swallowDelay = 0;
    }
    v = SDL12COMPAT_getenv_unsafe("COGGPU_SKIP");
    if (v && *v) {
        CogPace.skipExplicit = 1;
        CogPace.skipPresents = (v[0] != '0');
    }
    if ((CogPace.mode == 2) && !CogPace.skipExplicit) {
        CogPace.skipPresents = 0;   /* following the display needs a present (= a blocking call) every frame */
    }
    v = SDL12COMPAT_getenv_unsafe("COGGPU_INJECT");
    if (v && *v) {
        const double ms = SDL20_strtod(v, NULL);
        if (ms >= 20.0) {
            CogPace.injectMs = ms;
        }
    }
    CogPace.clock = (CogGPU.logPath[0] || CogPace.mode || (CogPace.injectMs > 0.0)) ? SDL_TRUE : SDL_FALSE;
}

/* SetVideoModeImpl: request a vsynced renderer when asked to follow the display */
static SDL_bool
CogGPU_WantVsync(void)
{
    CogPace_Init();
    return (CogPace.mode == 2) ? SDL_TRUE : SDL_FALSE;
}

static SDL_bool
CogPace_SkipAllowed(void)
{
    CogPace_Init();
    return CogPace.skipPresents ? SDL_TRUE : SDL_FALSE;
}

/* ------------------------------------------------------------------------------------------------------------- */
/* statistics helpers                                                                                             */
/* ------------------------------------------------------------------------------------------------------------- */

static void
CogSeries_Add(CogSeries *s, double x)
{
    if (s->n < COGPACE_N) {
        s->v[s->n++] = x;
    }
}

static void
CogSeries_Sort(CogSeries *s)   /* shell sort: -nostdlib, no qsort */
{
    int gap, i, j, n = s->n;
    for (gap = n / 2; gap > 0; gap /= 2) {
        for (i = gap; i < n; i++) {
            const double t = s->v[i];
            for (j = i; j >= gap && s->v[j - gap] > t; j -= gap) {
                s->v[j] = s->v[j - gap];
            }
            s->v[j] = t;
        }
    }
}

static double
CogSeries_Q(const CogSeries *s, double q)   /* sorted series */
{
    int i;
    if (!s->n) {
        return 0.0;
    }
    i = (int) (q * (double) (s->n - 1) + 0.5);
    return s->v[i];
}

/* ------------------------------------------------------------------------------------------------------------- */
/* precise waiting                                                                                                */
/* ------------------------------------------------------------------------------------------------------------- */

#ifdef _WIN32
typedef LONG (NTAPI *CogNtDelay_t)(BOOLEAN, PLARGE_INTEGER);
typedef LONG (NTAPI *CogNtSetRes_t)(ULONG, BOOLEAN, PULONG);
typedef HANDLE (WINAPI *CogCreateTimerEx_t)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

/*
 * Wait until CogGPU_Now() >= target (ms): sleep until `spinMs` before the target, spin on QueryPerformanceCounter for the rest.
 * The sleep primitive depends on the OS:
 *  - Wine (this project's Wine on an M1 Max, loaded machine; probe in scratch/gpu/probe/waitprobe.c): Sleep(1) overshoots by
 *    0.3-1.2 ms (median), NtDelayExecution is the most accurate sleep for short waits (median error 0.1 ms at 0.3 ms, tails to
 *    several ms), the "high resolution" waitable timer is no better than Sleep (median 1.1 ms), NtSetTimerResolution is accepted
 *    but changes nothing. -> NtDelayExecution.
 *  - real Windows (UNTESTED here): the default 15.6 ms timer tick would ruin any sleep, so ask for 0.5 ms resolution and use the
 *    high-resolution waitable timer (Windows 10 1803+) when it exists, else NtDelayExecution.
 */
static void
CogPace_WaitUntil(double target)
{
    static CogNtDelay_t ntDelay;
    static HANDLE hrTimer;
    static int inited;
    if (!inited) {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        inited = 1;
        if (nt) {
            CogNtSetRes_t setRes = (CogNtSetRes_t) GetProcAddress(nt, "NtSetTimerResolution");
            ntDelay = (CogNtDelay_t) GetProcAddress(nt, "NtDelayExecution");
            if (!GetProcAddress(nt, "wine_get_version")) {   /* real Windows */
                ULONG actual = 0;
                const HMODULE k32 = GetModuleHandleA("kernel32.dll");
                CogCreateTimerEx_t createEx = k32 ? (CogCreateTimerEx_t) GetProcAddress(k32, "CreateWaitableTimerExW") : NULL;
                if (setRes) {
                    setRes(5000, TRUE, &actual);
                }
                if (createEx) {   /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 2, TIMER_ALL_ACCESS = 0x1F0003 */
                    hrTimer = createEx(NULL, NULL, 0x2, 0x1F0003);
                }
            }
        }
        CogGPU_Log("COGGPU: pacer wait primitive: %s", hrTimer ? "high resolution waitable timer" : (ntDelay ? "NtDelayExecution" : "Sleep"));
    }
    for (;;) {
        const double rem = target - CogGPU_Now();
        if (rem <= 0.0) {
            return;
        }
        if (rem > CogPace.spinMs + 0.15) {
            const double sleepMs = rem - CogPace.spinMs;
            if (hrTimer) {
                LARGE_INTEGER li;
                li.QuadPart = -(LONGLONG) (sleepMs * 10000.0);
                SetWaitableTimer(hrTimer, &li, 0, NULL, NULL, FALSE);
                WaitForSingleObject(hrTimer, INFINITE);
            } else if (ntDelay) {
                LARGE_INTEGER li;
                li.QuadPart = -(LONGLONG) (sleepMs * 10000.0);   /* 100 ns units, relative */
                ntDelay(FALSE, &li);
            } else {
                Sleep((DWORD) sleepMs);
            }
        } else {
            __builtin_ia32_pause();
        }
    }
}
#else
static void CogPace_WaitUntil(double target) { (void) target; }
#endif

/* ------------------------------------------------------------------------------------------------------------- */
/* the pacer                                                                                                      */
/* ------------------------------------------------------------------------------------------------------------- */

/*
 * Called on the first SDL_PollEvent after SDL_Flip returned, i.e. at the start of the game's event drain (REX::run: tick, render,
 * flip, drain, ...). `t` = now. The wait happens HERE, before the events are fetched: input is sampled as late as possible before
 * the frame is built, the frame is then built and presented at the next slot, and the game's own limiter (SDL_Delay, swallowed)
 * no longer adds a second, millisecond-quantised delay between the input and the render.
 *
 * A slot is the time the present of a frame should end (COGGPU_PACE=<hz>) or the present call should start (=vsync, where the
 * present blocks until the display's next refresh and returns at the refresh: the slot grid is re-anchored at every present end and
 * its period is the lower quartile of the recent present intervals). Slots advance by exactly one period; a frame that misses its slot just
 * takes the next one (no catch-up burst). deadline = slot - work - margin, work = EMA of (end of the wait -> end of present).
 */
static void
CogPace_Wait(double t)
{
    const double need = CogPace.workEma + CogPace.marginMs;
    double k, slot, deadline;

    if (CogPace.period <= 0.0) {
        CogPace.tWaitEnd = t;
        return;
    }
    if (!CogPace.gridValid) {
        CogPace.gridOrigin = t + CogPace.period;
        CogPace.gridValid = 1;
        CogPace.lastSlot = CogPace.gridOrigin - CogPace.period;
    }
    k = (t + need - CogPace.gridOrigin) / CogPace.period;
    if (k < 0.0) {
        k = 0.0;
    } else {
        const double kc = (double) (long) k;
        k = (kc < k) ? kc + 1.0 : kc;   /* ceil */
    }
    slot = CogPace.gridOrigin + k * CogPace.period;
    if ((slot - CogPace.lastSlot) > 1.5 * CogPace.period) {
        CogPace.missed += (int) ((slot - CogPace.lastSlot) / CogPace.period + 0.5) - 1;
    }
    CogPace.lastSlot = slot;
    deadline = slot - need;
    if (deadline > t) {
        CogPace_WaitUntil(deadline);
    }
    CogPace.tWaitEnd = CogGPU_Now();
    CogPace.waits++;
    CogPace.waitMs += CogPace.tWaitEnd - t;
}

static void
CogGPU_PollEnter(void)
{
    double t;
    if (!CogPace.init) {
        CogPace_Init();
    }
    if (!CogPace.afterFlip) {
        return;
    }
    CogPace.afterFlip = 0;
    CogVR_BridgePoll();
    if (!CogPace.clock) {
        return;
    }
    t = CogGPU_Now();
    if (CogPace.tLastDrain > 0.0) {
        CogSeries_Add(&CogPace.loop, t - CogPace.tLastDrain);
    }
    CogPace.tLastDrain = t;
    if (CogPace.mode) {
        CogPace_Wait(t);
    } else {
        CogPace.tWaitEnd = t;
    }
    CogPace.workPending = 1;
}

/* the game polled an event: detect our synthetic key (delivered = it will be handled in this drain) */
static void CogPace_StartInject(void);

static void
CogGPU_PollLeave(const SDL12_Event *event12, int ret)
{
    if (CogPace.injPending && !CogPace.injDelivered && ret && event12 && (event12->type == SDL12_KEYDOWN) && (event12->key.keysym.sym == SDLK12_F15)) {
        CogPace.injDeliverT = CogGPU_Now();
        CogPace.injDelivered = 1;
        CogGPU.dirty = 1;   /* a key changes the screen: do not let the unchanged-screen skip hide the present */
    }
}

static void
CogGPU_FlipEnter(void)
{
    if (!CogPace.init) {
        CogPace_Init();
    }
    if (CogPace.clock) {
        CogPace.tFlipEnter = CogGPU_Now();
    }
    if (CogPace.injectMs > 0.0) {
        CogPace_StartInject();
    }
}

static void
CogGPU_FlipLeave(void)
{
    if (CogPace.clock) {
        CogSeries_Add(&CogPace.flip, CogGPU_Now() - CogPace.tFlipEnter);
    }
    CogPace.afterFlip = 1;
}

/* SDL_Delay hook: swallow the game's frame limiter sleep while pacing. Identified by its call site in the retail exe
   (limitFrame, 0x41af30: `call [SDL_Delay]` at 0x41b042, return address 0x41b048) through the IAT, so other callers keep sleeping. */
static SDL_bool
CogGPU_Delay(const void *retaddr, Uint32 ticks)
{
    const Uint8 *ra = (const Uint8 *) retaddr;
    const Uint8 *exe;
    (void) ticks;
    if (!CogPace.init) {
        CogPace_Init();
    }
    if (!CogPace.mode || !CogPace.swallowDelay || !ra) {
        return SDL_FALSE;
    }
#ifdef _WIN32
    exe = (const Uint8 *) GetModuleHandleA(NULL);
    if ((ra != exe + 0x1b048) || (ra[-6] != 0xFF) || (ra[-5] != 0x15)) {
        return SDL_FALSE;
    }
    {   /* `call dword ptr [iat]` must really go to our SDL_Delay */
        void **iat = *(void ***) (ra - 4);
        if (IsBadReadPtr(iat, sizeof (void *)) || (*iat != (void *) SDL_Delay)) {
            return SDL_FALSE;
        }
    }
    CogPace.swallowed++;
    return SDL_TRUE;
#else
    (void) exe;
    return SDL_FALSE;
#endif
}

/* ------------------------------------------------------------------------------------------------------------- */
/* injected input                                                                                                 */
/* ------------------------------------------------------------------------------------------------------------- */

#ifdef _WIN32
static DWORD WINAPI
CogPace_InjectThread(LPVOID arg)
{
    Uint32 rng = 0x2545F491u;
    (void) arg;
    for (;;) {
        double wait;
        SDL12_Event e;
        rng = rng * 1664525u + 1013904223u;
        wait = CogPace.injectMs * (0.75 + 0.5 * (double) ((rng >> 8) & 0xFFFF) / 65535.0);
        Sleep((DWORD) wait);
        if (CogPace.injPending || !EventQueueMutex) {
            continue;   /* previous key not resolved yet (game not presenting): skip this one */
        }
        SDL20_zero(e);
        e.type = SDL12_KEYDOWN;
        e.key.state = SDL_PRESSED;
        e.key.keysym.sym = SDLK12_F15;
        CogPace.injDelivered = 0;
        CogPace.injT = CogGPU_Now();
        CogPace.injPending = 1;
        PushEventIfNotFiltered(&e);
        e.type = SDL12_KEYUP;
        e.key.state = SDL_RELEASED;
        PushEventIfNotFiltered(&e);
    }
    return 0;
}

static void
CogPace_StartInject(void)
{
    static int started;
    if (!started && EventQueueMutex) {
        started = 1;
        CreateThread(NULL, 0, CogPace_InjectThread, NULL, 0, NULL);
        CogGPU_Log("COGGPU: injecting F15 key events every ~%.0f ms", CogPace.injectMs);
    }
}
#else
static void CogPace_StartInject(void) { }
#endif

/* ------------------------------------------------------------------------------------------------------------- */
/* per present                                                                                                    */
/* ------------------------------------------------------------------------------------------------------------- */

static void
CogPace_PresentStart(void)
{
    if (!CogPace.init) {
        CogPace_Init();
    }
    if (CogPace.clock) {
        CogPace.tPresent0 = CogGPU_Now();
    }
}

static void
CogPace_Emit(double now)
{
    const double secs = (now - CogPace.winT0) / 1000.0;
    double med, over = 0.0;
    int i;
    CogSeries_Sort(&CogPace.iv);
    med = CogSeries_Q(&CogPace.iv, 0.5);
    for (i = 0; i < CogPace.iv.n; i++) {
        if (CogPace.iv.v[i] > 1.5 * med) {
            over += 1.0;
        }
    }
    CogSeries_Sort(&CogPace.riv);
    CogGPU_Log("PRESENT frames=%d fps=%.1f avgFlipMs=%.3f gpuScreen=%d real=%d ivMed=%.2f ivP95=%.2f ivP99=%.2f ivMax=%.2f ivOver=%d rivMed=%.2f rivP95=%.2f rivMax=%.2f",
               CogPace.frames, CogPace.frames / secs, CogPace.presentMs / (CogPace.frames ? CogPace.frames : 1), CogGPU.target ? 1 : 0, CogPace.real,
               med, CogSeries_Q(&CogPace.iv, 0.95), CogSeries_Q(&CogPace.iv, 0.99), CogPace.iv.n ? CogPace.iv.v[CogPace.iv.n - 1] : 0.0, (int) over,
               CogSeries_Q(&CogPace.riv, 0.5), CogSeries_Q(&CogPace.riv, 0.95), CogPace.riv.n ? CogPace.riv.v[CogPace.riv.n - 1] : 0.0);
    if (CogGPU.winFlushes) {
        CogGPU_Log("FLUSH flushes=%d avgCells=%.1f avgFlushMs=%.3f maxFlushMs=%.3f", CogGPU.winFlushes,
                   (double) CogGPU.winCells / CogGPU.winFlushes, CogGPU.winFlushMs / CogGPU.winFlushes, CogGPU.winFlushMax);
    }
    if (CogGPU.winSkipped) {
        CogGPU_Log("SKIP presents=%d", CogGPU.winSkipped);
    }
    if (CogPace.loop.n) {
        CogSeries_Sort(&CogPace.loop);
        CogGPU_Log("LOOP n=%d med=%.2f p95=%.2f p99=%.2f max=%.2f", CogPace.loop.n, CogSeries_Q(&CogPace.loop, 0.5), CogSeries_Q(&CogPace.loop, 0.95),
                   CogSeries_Q(&CogPace.loop, 0.99), CogPace.loop.v[CogPace.loop.n - 1]);
    }
    if (CogPace.flip.n) {
        CogSeries_Sort(&CogPace.flip);
        CogGPU_Log("FLIPCALL n=%d med=%.3f p95=%.3f max=%.3f", CogPace.flip.n, CogSeries_Q(&CogPace.flip, 0.5), CogSeries_Q(&CogPace.flip, 0.95),
                   CogPace.flip.v[CogPace.flip.n - 1]);
    }
    if (CogPace.injectMs > 0.0) {
        CogSeries_Sort(&CogPace.lat);
        CogSeries_Sort(&CogPace.toDrain);
        CogSeries_Sort(&CogPace.d2p);
        CogGPU_Log("LATENCY n=%d med=%.2f p95=%.2f max=%.2f toDrainMed=%.2f toDrainP95=%.2f drainToPresentMed=%.2f drainToPresentP95=%.2f", CogPace.lat.n,
                   CogSeries_Q(&CogPace.lat, 0.5), CogSeries_Q(&CogPace.lat, 0.95), CogPace.lat.n ? CogPace.lat.v[CogPace.lat.n - 1] : 0.0,
                   CogSeries_Q(&CogPace.toDrain, 0.5), CogSeries_Q(&CogPace.toDrain, 0.95), CogSeries_Q(&CogPace.d2p, 0.5), CogSeries_Q(&CogPace.d2p, 0.95));
    }
    if (CogPace.mode) {
        CogGPU_Log("PACE mode=%s period=%.3f workEma=%.3f waitAvg=%.3f missedSlots=%d swallowedDelays=%d", (CogPace.mode == 2) ? "vsync" : "hz",
                   CogPace.period, CogPace.workEma, CogPace.waits ? CogPace.waitMs / CogPace.waits : 0.0, CogPace.missed, CogPace.swallowed);
    }
    CogPace.winT0 = now;
    CogPace.frames = CogPace.real = CogPace.missed = CogPace.swallowed = CogPace.waits = 0;
    CogPace.presentMs = CogPace.waitMs = 0.0;
    CogPace.iv.n = CogPace.riv.n = CogPace.loop.n = CogPace.flip.n = CogPace.lat.n = CogPace.toDrain.n = CogPace.d2p.n = 0;
    CogGPU.winFlushes = CogGPU.winCells = CogGPU.winSkipped = 0;
    CogGPU.winFlushMs = CogGPU.winFlushMax = 0.0;
}

/* End of PresentScreen. real = SDL_RenderPresent ran (0 = skipped: unchanged screen). */
static void
CogPace_PresentEnd(int real)
{
    double now;
    if (!CogPace.clock) {
        return;
    }
    now = CogGPU_Now();
    if (CogPace.winT0 == 0.0) {
        CogPace.winT0 = CogPace.tPresent0;
    }
    CogPace.frames++;
    CogPace.real += real ? 1 : 0;
    CogPace.presentMs += now - CogPace.tPresent0;
    if (CogPace.tPrevEnd > 0.0) {
        CogSeries_Add(&CogPace.iv, now - CogPace.tPrevEnd);
    }
    CogPace.tPrevEnd = now;
    if (real) {
        if (CogPace.tPrevRealEnd > 0.0) {
            CogSeries_Add(&CogPace.riv, now - CogPace.tPrevRealEnd);
        }
        CogPace.tPrevRealEnd = now;
    }

    if (CogPace.workPending) {   /* pacer work estimate: from the release of the drain to the end of the present (vsync: to its call) */
        const double end = (CogPace.mode == 2 && real) ? CogPace.tCall0 : now;
        const double w = end - CogPace.tWaitEnd;
        CogPace.workPending = 0;
        if (w > 0.0 && w < 200.0) {
            const double cap = (CogPace.period > 0.0) ? 1.5 * CogPace.period : 12.0;   /* one long frame must not wreck the estimate */
            const double w1 = (w > cap) ? cap : w;
            CogPace.workEma = (CogPace.workEma > 0.0) ? (0.85 * CogPace.workEma + 0.15 * w1) : w1;
        }
    }
    if ((CogPace.mode == 2) && real) {   /* follow the display: re-anchor the slot grid at the refresh this present returned at */
        double d;
        if (CogPace.period <= 0.0) {
            SDL_DisplayMode mode;
            CogPace.period = ((SDL20_GetCurrentDisplayMode(VideoDisplayIndex, &mode) == 0) && (mode.refresh_rate > 0)) ? 1000.0 / (double) mode.refresh_rate : 1000.0 / 60.0;
        }
        d = now - CogPace.tPrevReal;
        if ((CogPace.tPrevReal > 0.0) && (d > 0.5 * CogPace.period) && (d < 1.6 * CogPace.period)) {
            CogPace.realIv[CogPace.realIvN++ & 31] = d;
            if (CogPace.realIvN >= 8) {
                double a[32];
                const int n = CogPace.realIvN < 32 ? CogPace.realIvN : 32;
                int i, j;
                for (i = 0; i < n; i++) {   /* insertion sort of the last n intervals; the median is the period */
                    const double x = CogPace.realIv[i];
                    for (j = i; j > 0 && a[j - 1] > x; j--) {
                        a[j] = a[j - 1];
                    }
                    a[j] = x;
                }
                CogPace.period = a[n / 4];   /* lower quartile: load only ever makes an interval LONGER than the refresh period */
            }
        }
        CogPace.tPrevReal = now;
        CogPace.gridOrigin = now;
        CogPace.gridValid = 1;
        CogPace.lastSlot = now;
    }

    if (CogPace.injPending && CogPace.injDelivered) {
        CogSeries_Add(&CogPace.lat, now - CogPace.injT);
        CogSeries_Add(&CogPace.toDrain, CogPace.injDeliverT - CogPace.injT);
        CogSeries_Add(&CogPace.d2p, now - CogPace.injDeliverT);
        CogPace.injPending = 0;
    }
    if (CogGPU.logPath[0] && (now - CogPace.winT0 >= 2000.0)) {
        CogPace_Emit(now);
    }
}

/* in place of SDL_RenderPresent() in PresentScreen (renderer lock held) */
static void
CogGPU_RenderPresent(SDL_Renderer *renderer)
{
    if (CogPace.clock) {
        CogPace.tCall0 = CogGPU_Now();
    }
    SDL20_RenderPresent(renderer);
    CogPace_PresentEnd(1);
}
