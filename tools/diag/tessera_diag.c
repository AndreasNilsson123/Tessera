// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
/*
 * tessera-diag: reports what Tessera can do on this machine. It uses only the public C API (tesseraProbe), so
 * what it prints is exactly what an application linking Tessera would experience. Paste its output into a bug
 * report, or into docs/compat.md.
 *
 *   tessera-diag                 text report for every backend that exists on this platform
 *   tessera-diag --json          the same as JSON (for scripts and for attaching to issues)
 *   tessera-diag --backend egl   only this backend (wgl, glx, egl, cgl)
 *   tessera-diag --quick         a short list of cases instead of the full matrix
 *   tessera-diag --bench-swap [S]  frame pacing: vsync on/off (and adaptive) in a real window, S seconds each
 *                                (default 2); Windows and X11 only. Add --json for machine-readable output.
 *
 * Exit status: 0 if at least one backend could create a context, 1 if none could, 2 on a usage error.
 * Every case is a real create-inspect-destroy of a context in STRICT mode, so "refused" means the driver
 * cannot give exactly that, not that Tessera gave up.
 */
#define _CRT_SECURE_NO_WARNINGS 1
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200112L /* setenv / unsetenv under -std=c99 */
#endif
#include <tessera/tessera.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#include <time.h>
#endif

#if defined(_WIN32)
#define SETENV(name, value) _putenv_s((name), (value))
#define UNSETENV(name) _putenv_s((name), "")
#else
#define SETENV(name, value) setenv((name), (value), 1)
#define UNSETENV(name) unsetenv(name)
#endif

static int g_json = 0;
static int g_quick = 0;
static double g_benchSeconds = 0; /* > 0: --bench-swap */

/* ------------------------------------------------------------------ small output helpers */

static void jstr(const char* s) {
    putchar('"');
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c == '\n')
            fputs("\\n", stdout);
        else if (c < 0x20 || c >= 0x7f)
            printf("\\u%04x", c); /* driver strings are not guaranteed to be clean UTF-8 */
        else
            putchar(c);
    }
    putchar('"');
}

static const char* profileName(uint32_t p) {
    return p == TESSERA_PROFILE_CORE ? "core" : p == TESSERA_PROFILE_COMPATIBILITY ? "compatibility" : "any";
}
static const char* apiName(uint32_t a) {
    return a == TESSERA_API_OPENGL_ES ? "GLES" : "GL";
}
static const char* surfaceName(uint32_t s) {
    switch (s) {
    case TESSERA_SURFACE_HIDDEN_WINDOW:
        return "hidden-window";
    case TESSERA_SURFACE_PBUFFER:
        return "pbuffer";
    case TESSERA_SURFACE_NONE:
        return "none";
    case TESSERA_SURFACE_WINDOW:
        return "window";
    default:
        return "default";
    }
}

static const struct {
    uint32_t bit;
    const char* name;
} kFlagNames[] = {{TESSERA_FLAG_DEBUG, "debug"},
                  {TESSERA_FLAG_FORWARD_COMPATIBLE, "forward-compatible"},
                  {TESSERA_FLAG_ROBUST_ACCESS, "robust-access"},
                  {TESSERA_FLAG_RESET_NOTIFICATION, "reset-notification"},
                  {TESSERA_FLAG_NO_ERROR, "no-error"},
                  {TESSERA_FLAG_SRGB, "srgb"},
                  {TESSERA_FLAG_FLOAT_COLOR, "float-color"},
                  {TESSERA_FLAG_DOUBLE_BUFFER, "double-buffer"},
                  {TESSERA_FLAG_STEREO, "stereo"},
                  {TESSERA_FLAG_RELEASE_FLUSH, "release-flush"}};

static void flagsText(uint32_t flags, char* out, size_t cap) {
    size_t n = 0;
    out[0] = '\0';
    for (size_t i = 0; i < sizeof kFlagNames / sizeof *kFlagNames; ++i) {
        if (!(flags & kFlagNames[i].bit))
            continue;
        int w = snprintf(out + n, cap - n, "%s%s", n ? "," : "", kFlagNames[i].name);
        if (w < 0 || (size_t)w >= cap - n)
            return;
        n += (size_t)w;
    }
}

/* ------------------------------------------------------------------ the cases */

typedef struct {
    const char* label;
    TesseraContextConfig cfg;
    int quick; /* part of the --quick list */
} Case;

static Case g_cases[64];
static int g_caseCount = 0;

static void addCase(const char* label, uint32_t api, int major, int minor, uint32_t profile, uint32_t flags,
                    int samples, int depth, uint32_t surface, int quick) {
    Case* c = &g_cases[g_caseCount++];
    c->label = label;
    c->cfg = tesseraDefaultContextConfig();
    c->cfg.api = api;
    c->cfg.versionMajor = major;
    c->cfg.versionMinor = minor;
    c->cfg.profile = profile;
    c->cfg.flags = flags;
    c->cfg.samples = samples;
    c->cfg.depthBits = depth;
    c->cfg.surface = surface;
    c->cfg.fallback = TESSERA_FALLBACK_STRICT;
    c->quick = quick;
}

static void buildCases(void) {
    const uint32_t GL = TESSERA_API_OPENGL, ES = TESSERA_API_OPENGL_ES;
    const uint32_t ANY = TESSERA_PROFILE_ANY, CORE = TESSERA_PROFILE_CORE,
                   COMPAT = TESSERA_PROFILE_COMPATIBILITY;
    const uint32_t DEF = TESSERA_SURFACE_DEFAULT;
    const uint32_t DB =
        0; /* off-screen use: headless EGL cannot be double-buffered, so only one case asks for it */
    addCase("OpenGL, driver's choice", GL, 0, 0, ANY, DB, 0, 0, DEF, 1);
    addCase("OpenGL 2.1 (legacy)", GL, 2, 1, ANY, DB, 0, 0, DEF, 0);
    addCase("OpenGL 3.2 core", GL, 3, 2, CORE, DB, 0, 0, DEF, 0);
    addCase("OpenGL 3.3 core", GL, 3, 3, CORE, DB, 0, 24, DEF, 1);
    addCase("OpenGL 4.1 core", GL, 4, 1, CORE, DB, 0, 24, DEF, 1);
    addCase("OpenGL 4.3 core", GL, 4, 3, CORE, DB, 0, 24, DEF, 0);
    addCase("OpenGL 4.5 core", GL, 4, 5, CORE, DB, 0, 24, DEF, 0);
    addCase("OpenGL 4.6 core", GL, 4, 6, CORE, DB, 0, 24, DEF, 1);
    addCase("OpenGL 3.2 compatibility", GL, 3, 2, COMPAT, DB, 0, 24, DEF, 0);
    addCase("OpenGL 4.5 compatibility", GL, 4, 5, COMPAT, DB, 0, 24, DEF, 0);
    addCase("OpenGL ES 2.0", ES, 2, 0, ANY, DB, 0, 0, DEF, 1);
    addCase("OpenGL ES 3.0", ES, 3, 0, ANY, DB, 0, 0, DEF, 1);
    addCase("OpenGL ES 3.1", ES, 3, 1, ANY, DB, 0, 0, DEF, 0);
    addCase("OpenGL ES 3.2", ES, 3, 2, ANY, DB, 0, 0, DEF, 0);
    addCase("core 3.3 + debug", GL, 3, 3, CORE, DB | TESSERA_FLAG_DEBUG, 0, 24, DEF, 1);
    addCase("core 3.3 + forward-compatible", GL, 3, 3, CORE, DB | TESSERA_FLAG_FORWARD_COMPATIBLE, 0, 24, DEF,
            0);
    addCase("core 3.3 + robust access", GL, 3, 3, CORE, DB | TESSERA_FLAG_ROBUST_ACCESS, 0, 24, DEF, 0);
    addCase("core 3.3 + robust + reset notification", GL, 3, 3, CORE,
            DB | TESSERA_FLAG_ROBUST_ACCESS | TESSERA_FLAG_RESET_NOTIFICATION, 0, 24, DEF, 0);
    addCase("core 3.3 + no-error", GL, 3, 3, CORE, DB | TESSERA_FLAG_NO_ERROR, 0, 24, DEF, 0);
    addCase("core 3.3 + release-flush", GL, 3, 3, CORE, DB | TESSERA_FLAG_RELEASE_FLUSH, 0, 24, DEF, 0);
    addCase("core 3.3, 4x MSAA", GL, 3, 3, CORE, DB, 4, 24, DEF, 0);
    addCase("core 3.3, 8x MSAA", GL, 3, 3, CORE, DB, 8, 24, DEF, 0);
    addCase("core 3.3, sRGB framebuffer", GL, 3, 3, CORE, DB | TESSERA_FLAG_SRGB, 0, 24, DEF, 0);
    addCase("core 3.3, float colour buffer", GL, 3, 3, CORE, DB | TESSERA_FLAG_FLOAT_COLOR, 0, 24, DEF, 0);
    addCase("core 3.3, stereo", GL, 3, 3, CORE, DB | TESSERA_FLAG_STEREO, 0, 24, DEF, 0);
    addCase("core 3.3, double-buffered", GL, 3, 3, CORE, TESSERA_FLAG_DOUBLE_BUFFER, 0, 24, DEF, 0);
    addCase("core 3.3, pbuffer surface", GL, 3, 3, CORE, 0, 0, 24, TESSERA_SURFACE_PBUFFER, 1);
    addCase("core 3.3, no surface (FBO only)", GL, 3, 3, CORE, 0, 0, 0, TESSERA_SURFACE_NONE, 0);
}

/* ------------------------------------------------------------------ one backend */

typedef struct {
    int available;
    int created;
} BackendSummary;

static void printInfoText(const TesseraContextInfo* i) {
    char flags[160];
    flagsText(i->flags, flags, sizeof flags);
    printf("%s %d.%d %s", apiName(i->api), i->versionMajor, i->versionMinor, profileName(i->profile));
    printf(" | rgba %d%d%d%d d%d s%d x%d | %s%s%s", i->redBits, i->greenBits, i->blueBits, i->alphaBits,
           i->depthBits, i->stencilBits, i->samples, surfaceName(i->surface), flags[0] ? " | " : "", flags);
}

static void printInfoJson(const TesseraContextInfo* i) {
    char flags[160];
    flagsText(i->flags, flags, sizeof flags);
    printf("{\"api\":\"%s\",\"version\":\"%d.%d\",\"profile\":\"%s\",\"red\":%d,\"green\":%d,\"blue\":%d,"
           "\"alpha\":%d,\"depth\":%d,"
           "\"stencil\":%d,\"samples\":%d,\"surface\":\"%s\",\"flags\":\"%s\"}",
           apiName(i->api), i->versionMajor, i->versionMinor, profileName(i->profile), i->redBits,
           i->greenBits, i->blueBits, i->alphaBits, i->depthBits, i->stencilBits, i->samples,
           surfaceName(i->surface), flags);
}

/* What swap control the backend offers, read from a real context (best-effort: the baseline request). */
typedef struct {
    int known; /* a context could be created and made current */
    uint32_t caps;
} SwapReport;

static SwapReport probeSwapControl(void) {
    SwapReport rep = {0, 0};
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraContext ctx = TESSERA_NULL_HANDLE;
    if (tesseraCreateContext(&cfg, &ctx) != TESSERA_SUCCESS)
        return rep;
    if (tesseraMakeCurrent(ctx) == TESSERA_SUCCESS) {
        TesseraSwapControl sc = tesseraDefaultSwapControl();
        if (tesseraGetSwapControl(ctx, &sc) == TESSERA_SUCCESS) {
            rep.known = 1;
            rep.caps = sc.caps;
        }
        tesseraMakeCurrent(TESSERA_NULL_HANDLE);
    }
    tesseraDestroyContext(ctx);
    return rep;
}

static void swapCapsText(uint32_t caps, char* out, size_t cap) {
    snprintf(out, cap, "%s%s%s", caps & TESSERA_SWAP_CAP_CONTROL ? "control" : "none",
             caps & TESSERA_SWAP_CAP_ADAPTIVE ? ", adaptive (-1)" : "",
             caps & TESSERA_SWAP_CAP_READBACK ? ", read-back" : "");
}

static BackendSummary runBackend(const char* name, int* firstBackend) {
    BackendSummary sum = {0, 0};
    SETENV("TESSERA_BACKEND", name);
    tesseraTerminate();
    if (tesseraInit() != TESSERA_SUCCESS) {
        if (g_json) {
            printf("%s    {\"backend\":", *firstBackend ? "" : ",\n");
            jstr(name);
            printf(",\"available\":false,\"reason\":");
            jstr(tesseraGetLastErrorMessage());
            printf("}");
        } else {
            printf("[%s] not available: %s\n\n", name, tesseraGetLastErrorMessage());
        }
        *firstBackend = 0;
        return sum;
    }

    /* The baseline decides whether the backend exists at all on this machine. */
    TesseraContextConfig base = tesseraDefaultContextConfig();
    base.api = TESSERA_API_OPENGL;
    base.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraProbeReport baseline = tesseraDefaultProbeReport();
    TesseraResult r = tesseraProbe(&base, &baseline);
    if (r == TESSERA_ERROR_UNSUPPORTED || r == TESSERA_ERROR_NOT_INITIALIZED) {
        if (g_json) {
            printf("%s    {\"backend\":", *firstBackend ? "" : ",\n");
            jstr(name);
            printf(",\"available\":false,\"reason\":");
            jstr(baseline.message);
            printf("}");
        } else {
            printf("[%s] not available: %s\n\n", name, baseline.message);
        }
        *firstBackend = 0;
        return sum;
    }
    sum.available = 1;

    if (g_json) {
        printf("%s    {\"backend\":", *firstBackend ? "" : ",\n");
        jstr(name);
        printf(",\"available\":true,\"answeredBy\":");
        jstr(baseline.backend);
        if (r == TESSERA_SUCCESS) {
            printf(",\"vendor\":");
            jstr(baseline.info.vendor);
            printf(",\"renderer\":");
            jstr(baseline.info.renderer);
            printf(",\"version\":");
            jstr(baseline.info.version);
            const SwapReport sw = probeSwapControl();
            if (sw.known)
                printf(",\"swapControl\":{\"control\":%s,\"adaptive\":%s,\"readback\":%s}",
                       (sw.caps & TESSERA_SWAP_CAP_CONTROL) ? "true" : "false",
                       (sw.caps & TESSERA_SWAP_CAP_ADAPTIVE) ? "true" : "false",
                       (sw.caps & TESSERA_SWAP_CAP_READBACK) ? "true" : "false");
        } else {
            printf(",\"baselineError\":");
            jstr(baseline.message);
        }
        printf(",\n      \"cases\":[");
    } else {
        printf("[%s]%s\n", name, r == TESSERA_SUCCESS ? "" : " (the baseline request failed)");
        if (r == TESSERA_SUCCESS) {
            printf("  vendor   : %s\n  renderer : %s\n  version  : %s\n", baseline.info.vendor,
                   baseline.info.renderer, baseline.info.version);
        } else {
            printf("  %s: %s\n", tesseraResultString(r), baseline.message);
        }
        if (r == TESSERA_SUCCESS) {
            const SwapReport sw = probeSwapControl();
            char caps[96];
            swapCapsText(sw.caps, caps, sizeof caps);
            printf("  swap control (default window): %s\n", sw.known ? caps : "unknown");
        }
        printf("  strict requests (a context is really created and destroyed for each):\n");
    }
    *firstBackend = 0;

    int firstCase = 1, ran = 0;
    for (int k = 0; k < g_caseCount; ++k) {
        if (g_quick && !g_cases[k].quick)
            continue;
        ++ran;
        TesseraProbeReport rep = tesseraDefaultProbeReport();
        r = tesseraProbe(&g_cases[k].cfg, &rep);
        if (r == TESSERA_SUCCESS)
            ++sum.created;
        if (g_json) {
            printf("%s\n        {\"case\":", firstCase ? "" : ",");
            jstr(g_cases[k].label);
            printf(",\"result\":");
            jstr(tesseraResultString(r));
            if (r == TESSERA_SUCCESS) {
                printf(",\"info\":");
                printInfoJson(&rep.info);
            } else {
                printf(",\"message\":");
                jstr(rep.message);
            }
            printf("}");
        } else if (r == TESSERA_SUCCESS) {
            printf("  [ ok ] %-40s ", g_cases[k].label);
            printInfoText(&rep.info);
            printf("\n");
        } else {
            printf("  [ -- ] %-40s %s: %s\n", g_cases[k].label, tesseraResultString(r), rep.message);
        }
        firstCase = 0;
    }
    if (g_json)
        printf("\n      ]}");
    else
        printf("  %d of %d requests satisfied\n\n", sum.created, ran);
    return sum;
}

/* ------------------------------------------------------------------ frame pacing benchmark */

typedef struct {
    int frames, dropped;
    double meanMs, minMs, p99Ms, maxMs;
} BenchStats;

typedef struct {
    uint64_t nativeWindow;
#if defined(_WIN32)
    HWND hwnd;
#elif defined(__linux__)
    void* x11;
    void* display;
    unsigned long window;
    void (*xDestroy)(void*, unsigned long);
    int (*xClose)(void*);
    int (*xPending)(void*);
    void (*xSync)(void*, int);
#endif
} BenchWindow;

static double nowMs(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
#elif defined(__linux__)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return 1000.0 * (double)ts.tv_sec + (double)ts.tv_nsec / 1e6;
#else
    return 0.0;
#endif
}

/* 1 = window created. The window is the tool's own; Tessera only borrows it. */
static int benchWindowCreate(BenchWindow* w, int width, int height) {
    memset(w, 0, sizeof *w);
#if defined(_WIN32)
    w->hwnd = CreateWindowExA(0, "STATIC", "tessera-diag --bench-swap", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 60,
                              60, width, height, NULL, NULL, NULL, NULL);
    w->nativeWindow = (uint64_t)(uintptr_t)w->hwnd;
    return w->hwnd != NULL;
#elif defined(__linux__)
    typedef void* (*OpenFn)(const char*);
    typedef unsigned long (*RootFn)(void*);
    typedef unsigned long (*SimpleFn)(void*, unsigned long, int, int, unsigned, unsigned, unsigned,
                                      unsigned long, unsigned long);
    typedef int (*MapFn)(void*, unsigned long);
    w->x11 = dlopen("libX11.so.6", RTLD_NOW);
    if (!w->x11)
        return 0;
    OpenFn open = (OpenFn)dlsym(w->x11, "XOpenDisplay");
    RootFn root = (RootFn)dlsym(w->x11, "XDefaultRootWindow");
    SimpleFn create = (SimpleFn)dlsym(w->x11, "XCreateSimpleWindow");
    MapFn map = (MapFn)dlsym(w->x11, "XMapWindow");
    w->xDestroy = (void (*)(void*, unsigned long))dlsym(w->x11, "XDestroyWindow");
    w->xClose = (int (*)(void*))dlsym(w->x11, "XCloseDisplay");
    w->xSync = (void (*)(void*, int))dlsym(w->x11, "XSync");
    if (!open || !root || !create || !map || !w->xDestroy || !w->xClose || !w->xSync)
        return 0;
    w->display = open(NULL); /* $DISPLAY: the same server Tessera's GLX backend talks to */
    if (!w->display)
        return 0;
    w->window = create(w->display, root(w->display), 0, 0, (unsigned)width, (unsigned)height, 0, 0, 0);
    map(w->display, w->window);
    w->xSync(w->display, 0);
    w->nativeWindow = (uint64_t)w->window;
    return w->window != 0;
#else
    (void)width;
    (void)height;
    return 0;
#endif
}

static void benchWindowPump(BenchWindow* w) {
#if defined(_WIN32)
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    (void)w;
#else
    (void)w;
#endif
}

static void benchWindowDestroy(BenchWindow* w) {
#if defined(_WIN32)
    if (w->hwnd)
        DestroyWindow(w->hwnd);
#elif defined(__linux__)
    if (w->display && w->window)
        w->xDestroy(w->display, w->window);
    if (w->display)
        w->xClose(w->display);
    if (w->x11)
        dlclose(w->x11);
#endif
    (void)w;
}

static int cmpDouble(const void* a, const void* b) {
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

typedef void (*ClearColorFn)(float, float, float, float);
typedef void (*ClearFn)(unsigned);

/* Renders and swaps for `seconds`; returns the per-frame statistics. A frame that takes 1.5x the median
 * counts as dropped. */
static BenchStats benchRun(TesseraContext ctx, BenchWindow* w, double seconds) {
    BenchStats st;
    memset(&st, 0, sizeof st);
    ClearColorFn clearColor = (ClearColorFn)tesseraGetProcAddress("glClearColor");
    ClearFn clear = (ClearFn)tesseraGetProcAddress("glClear");
    if (!clearColor || !clear)
        return st;
    size_t cap = 1 << 16, n = 0;
    double* ms = (double*)malloc(cap * sizeof *ms);
    if (!ms)
        return st;
    double prev = nowMs();
    const double start = prev;
    for (int i = 0;; ++i) {
        benchWindowPump(w);
        clearColor((float)(i & 1), 0.f, 0.f, 1.f);
        clear(0x4000); /* GL_COLOR_BUFFER_BIT */
        if (tesseraSwapBuffers(ctx) != TESSERA_SUCCESS)
            break;
        const double t = nowMs();
        if (i >= 5) { /* warm-up */
            if (n == cap) {
                cap *= 2;
                double* bigger = (double*)realloc(ms, cap * sizeof *ms);
                if (!bigger)
                    break;
                ms = bigger;
            }
            ms[n++] = t - prev;
        }
        prev = t;
        if (t - start >= seconds * 1000.0)
            break;
    }
    if (n > 10) {
        double sum = 0;
        double* sorted = (double*)malloc(n * sizeof *sorted);
        if (sorted) {
            memcpy(sorted, ms, n * sizeof *ms);
            qsort(sorted, n, sizeof *sorted, cmpDouble);
            for (size_t i = 0; i < n; ++i)
                sum += ms[i];
            const double median = sorted[n / 2];
            for (size_t i = 0; i < n; ++i)
                if (ms[i] > 1.5 * median)
                    ++st.dropped;
            st.frames = (int)n;
            st.meanMs = sum / (double)n;
            st.minMs = sorted[0];
            st.p99Ms = sorted[(size_t)((double)(n - 1) * 0.99)];
            st.maxMs = sorted[n - 1];
            free(sorted);
        }
    }
    free(ms);
    return st;
}

/* 0 = ran, 1 = could not (no window, no context, no swap control). */
static int runBenchSwap(const char* backend) {
    if (backend)
        SETENV("TESSERA_BACKEND", backend);
    if (tesseraInit() != TESSERA_SUCCESS) {
        fprintf(stderr, "tessera-diag: init failed: %s\n", tesseraGetLastErrorMessage());
        return 1;
    }
    BenchWindow win;
    if (!benchWindowCreate(&win, 640, 360)) {
        fprintf(stderr, "tessera-diag: --bench-swap needs a window (Windows or X11 with $DISPLAY) and could "
                        "not make one\n");
        tesseraTerminate();
        return 1;
    }
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    cfg.flags = TESSERA_FLAG_DOUBLE_BUFFER; /* a single-buffered window has nothing to swap, so no pacing */
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraContext ctx = TESSERA_NULL_HANDLE;
    TesseraResult r =
        tesseraCreateContextForWindow(&cfg, win.nativeWindow, TESSERA_ATTACH_ALLOW_PIXEL_FORMAT, &ctx);
    if (r != TESSERA_SUCCESS || tesseraMakeCurrent(ctx) != TESSERA_SUCCESS) {
        fprintf(stderr, "tessera-diag: cannot render into the window: %s (%s)\n", tesseraResultString(r),
                tesseraGetLastErrorMessage());
        benchWindowDestroy(&win);
        tesseraTerminate();
        return 1;
    }
    TesseraSwapControl sc = tesseraDefaultSwapControl();
    tesseraGetSwapControl(ctx, &sc);
    TesseraContextInfo info = tesseraDefaultContextInfo();
    tesseraGetContextInfo(ctx, &info);
    char caps[96];
    swapCapsText(sc.caps, caps, sizeof caps);
    if (!(sc.caps & TESSERA_SWAP_CAP_CONTROL)) {
        fprintf(stderr, "tessera-diag: this driver exposes no swap control (%s); nothing to compare\n", caps);
        tesseraMakeCurrent(TESSERA_NULL_HANDLE);
        tesseraDestroyContext(ctx);
        benchWindowDestroy(&win);
        tesseraTerminate();
        return 1;
    }

    int intervals[3];
    int nInt = 0;
    intervals[nInt++] = 1;
    intervals[nInt++] = 0;
    if (sc.caps & TESSERA_SWAP_CAP_ADAPTIVE)
        intervals[nInt++] = -1;

    if (g_json) {
        printf("{\n  \"swapBenchmark\":{\"renderer\":");
        jstr(info.renderer);
        printf(",\"seconds\":%.2f,\"modes\":[", g_benchSeconds);
    } else {
        printf("Frame pacing in a 640x360 window, %.1f s per mode\n  renderer: %s\n  swap control: %s\n",
               g_benchSeconds, info.renderer, caps);
    }
    for (int k = 0; k < nInt; ++k) {
        const TesseraResult sr = tesseraSetSwapInterval(ctx, intervals[k]);
        TesseraSwapControl now = tesseraDefaultSwapControl();
        tesseraGetSwapControl(ctx, &now);
        const BenchStats st = sr == TESSERA_SUCCESS ? benchRun(ctx, &win, g_benchSeconds) : (BenchStats){0};
        if (g_json) {
            printf("%s\n    {\"interval\":%d,\"applied\":", k ? "," : "", intervals[k]);
            if (now.applied == TESSERA_SWAP_UNKNOWN)
                printf("null");
            else
                printf("%d", now.applied);
            printf(",\"frames\":%d,\"meanMs\":%.3f,\"fps\":%.1f,\"minMs\":%.3f,\"p99Ms\":%.3f,\"maxMs\":%.3f,"
                   "\"dropped\":%d}",
                   st.frames, st.meanMs, st.meanMs > 0 ? 1000.0 / st.meanMs : 0.0, st.minMs, st.p99Ms,
                   st.maxMs, st.dropped);
        } else if (sr != TESSERA_SUCCESS) {
            printf("  interval %2d: refused (%s)\n", intervals[k], tesseraResultString(sr));
        } else {
            printf("  interval %2d (driver reports %s%d): %5d frames, mean %.3f ms (%.1f fps), min %.3f, p99 "
                   "%.3f, max "
                   "%.3f, dropped %d\n",
                   intervals[k], now.applied == TESSERA_SWAP_UNKNOWN ? "?" : "",
                   now.applied == TESSERA_SWAP_UNKNOWN ? 0 : now.applied, st.frames, st.meanMs,
                   st.meanMs > 0 ? 1000.0 / st.meanMs : 0.0, st.minMs, st.p99Ms, st.maxMs, st.dropped);
        }
    }
    if (g_json)
        printf("\n  ]}\n}\n");
    tesseraMakeCurrent(TESSERA_NULL_HANDLE);
    tesseraDestroyContext(ctx);
    benchWindowDestroy(&win);
    tesseraTerminate();
    return 0;
}

/* ------------------------------------------------------------------ main */

static const char* platformName(void) {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "other";
#endif
}

static void usage(void) {
    fputs("usage: tessera-diag [--json] [--quick] [--backend wgl|glx|egl|cgl] [--bench-swap [SECONDS]] "
          "[--help]\n",
          stderr);
}

int main(int argc, char** argv) {
    const char* only = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--json"))
            g_json = 1;
        else if (!strcmp(argv[i], "--quick"))
            g_quick = 1;
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc)
            only = argv[++i];
        else if (!strcmp(argv[i], "--bench-swap")) {
            g_benchSeconds = 2.0;
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                g_benchSeconds = atof(argv[++i]);
                if (g_benchSeconds < 0.2 || g_benchSeconds > 60.0) {
                    usage();
                    return 2;
                }
            }
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }
    int count = 0;
    for (const char* p = only; p && *p; ++p)
        count++;
    if (only && (count == 0 || count > 16 || strchr(only, ','))) {
        usage();
        return 2;
    }

    if (g_benchSeconds > 0)
        return runBenchSwap(only);

    int32_t major = 0, minor = 0, patch = 0;
    tesseraGetVersion(&major, &minor, &patch);
    const char* userBackend = getenv("TESSERA_BACKEND");
    char savedBackend[128] = "";
    if (userBackend)
        snprintf(savedBackend, sizeof savedBackend, "%s", userBackend);

    buildCases();

    static const char* const kEnv[] = {"TESSERA_BACKEND",
                                       "TESSERA_EGL_PLATFORM",
                                       "TESSERA_EGL_DEVICE",
                                       "TESSERA_EGL_LIBRARY",
                                       "TESSERA_CGL_LIBRARY",
                                       "TESSERA_NO_XINITTHREADS",
                                       "DISPLAY",
                                       "WAYLAND_DISPLAY"};
    if (g_json) {
        printf("{\n  "
               "\"tessera\":\"%d.%d.%d\",\"abi\":%d,\"headerAbi\":%d,\"platform\":\"%s\",\"pointerBits\":%d,"
               "\n  \"environment\":{",
               major, minor, patch, tesseraGetAbiVersion(), TESSERA_ABI_VERSION, platformName(),
               (int)(sizeof(void*) * 8));
        int first = 1;
        for (size_t i = 0; i < sizeof kEnv / sizeof *kEnv; ++i) {
            const char* v = i == 0 ? (savedBackend[0] ? savedBackend : NULL) : getenv(kEnv[i]);
            if (!v)
                continue;
            printf("%s", first ? "" : ",");
            jstr(kEnv[i]);
            putchar(':');
            jstr(v);
            first = 0;
        }
        printf("},\n  \"backends\":[\n");
    } else {
        printf("Tessera %d.%d.%d (ABI %d, header ABI %d) on %s, %d-bit\n", major, minor, patch,
               tesseraGetAbiVersion(), TESSERA_ABI_VERSION, platformName(), (int)(sizeof(void*) * 8));
        for (size_t i = 0; i < sizeof kEnv / sizeof *kEnv; ++i) {
            const char* v = i == 0 ? (savedBackend[0] ? savedBackend : NULL) : getenv(kEnv[i]);
            if (v)
                printf("  %s=%s\n", kEnv[i], v);
        }
        printf("\n");
    }

    /* Candidate backends. A TESSERA_BACKEND already set by the user is respected as the single candidate. */
    const char* candidates[5];
    int nCand = 0;
    if (only)
        candidates[nCand++] = only;
    else if (savedBackend[0])
        candidates[nCand++] = savedBackend;
    else {
        candidates[nCand++] = "wgl";
        candidates[nCand++] = "glx";
        candidates[nCand++] = "egl";
        candidates[nCand++] = "cgl";
    }

    int first = 1, anyCreated = 0;
    for (int i = 0; i < nCand; ++i) {
        BackendSummary s = runBackend(candidates[i], &first);
        if (s.created > 0)
            anyCreated = 1;
    }
    if (g_json)
        printf("\n  ]\n}\n");
    else if (!anyCreated)
        printf("No backend could create a context on this machine.\n");

    tesseraTerminate();
    if (savedBackend[0])
        SETENV("TESSERA_BACKEND", savedBackend);
    else
        UNSETENV("TESSERA_BACKEND");
    return anyCreated ? 0 : 1;
}
