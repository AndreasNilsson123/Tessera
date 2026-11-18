/*
 * tessera-diag: reports what Tessera can do on this machine. It uses only the public C API (tesseraProbe), so what it prints is
 * exactly what an application linking Tessera would experience. Paste its output into a bug report, or into docs/compat.md.
 *
 *   tessera-diag                 text report for every backend that exists on this platform
 *   tessera-diag --json          the same as JSON (for scripts and for attaching to issues)
 *   tessera-diag --backend egl   only this backend (wgl, glx, egl, cgl)
 *   tessera-diag --quick         a short list of cases instead of the full matrix
 *
 * Exit status: 0 if at least one backend could create a context, 1 if none could, 2 on a usage error.
 * Every case is a real create-inspect-destroy of a context in STRICT mode, so "refused" means the driver cannot give exactly
 * that, not that Tessera gave up.
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
#define SETENV(name, value) _putenv_s((name), (value))
#define UNSETENV(name) _putenv_s((name), "")
#else
#define SETENV(name, value) setenv((name), (value), 1)
#define UNSETENV(name) unsetenv(name)
#endif

static int g_json = 0;
static int g_quick = 0;

/* ------------------------------------------------------------------ small output helpers */

static void jstr(const char* s) {
    putchar('"');
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c < 0x20 || c >= 0x7f) printf("\\u%04x", c); /* driver strings are not guaranteed to be clean UTF-8 */
        else putchar(c);
    }
    putchar('"');
}

static const char* profileName(uint32_t p) {
    return p == TESSERA_PROFILE_CORE ? "core" : p == TESSERA_PROFILE_COMPATIBILITY ? "compatibility" : "any";
}
static const char* apiName(uint32_t a) { return a == TESSERA_API_OPENGL_ES ? "GLES" : "GL"; }
static const char* surfaceName(uint32_t s) {
    switch (s) {
        case TESSERA_SURFACE_HIDDEN_WINDOW: return "hidden-window";
        case TESSERA_SURFACE_PBUFFER: return "pbuffer";
        case TESSERA_SURFACE_NONE: return "none";
        case TESSERA_SURFACE_WINDOW: return "window";
        default: return "default";
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
        if (!(flags & kFlagNames[i].bit)) continue;
        int w = snprintf(out + n, cap - n, "%s%s", n ? "," : "", kFlagNames[i].name);
        if (w < 0 || (size_t)w >= cap - n) return;
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

static void addCase(const char* label, uint32_t api, int major, int minor, uint32_t profile, uint32_t flags, int samples, int depth,
                    uint32_t surface, int quick) {
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
    const uint32_t ANY = TESSERA_PROFILE_ANY, CORE = TESSERA_PROFILE_CORE, COMPAT = TESSERA_PROFILE_COMPATIBILITY;
    const uint32_t DEF = TESSERA_SURFACE_DEFAULT;
    const uint32_t DB = 0; /* off-screen use: headless EGL cannot be double-buffered, so only one case asks for it */
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
    addCase("core 3.3 + forward-compatible", GL, 3, 3, CORE, DB | TESSERA_FLAG_FORWARD_COMPATIBLE, 0, 24, DEF, 0);
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
    printf(" | rgba %d%d%d%d d%d s%d x%d | %s%s%s", i->redBits, i->greenBits, i->blueBits, i->alphaBits, i->depthBits, i->stencilBits,
           i->samples, surfaceName(i->surface), flags[0] ? " | " : "", flags);
}

static void printInfoJson(const TesseraContextInfo* i) {
    char flags[160];
    flagsText(i->flags, flags, sizeof flags);
    printf("{\"api\":\"%s\",\"version\":\"%d.%d\",\"profile\":\"%s\",\"red\":%d,\"green\":%d,\"blue\":%d,\"alpha\":%d,\"depth\":%d,"
           "\"stencil\":%d,\"samples\":%d,\"surface\":\"%s\",\"flags\":\"%s\"}",
           apiName(i->api), i->versionMajor, i->versionMinor, profileName(i->profile), i->redBits, i->greenBits, i->blueBits,
           i->alphaBits, i->depthBits, i->stencilBits, i->samples, surfaceName(i->surface), flags);
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
        } else {
            printf(",\"baselineError\":");
            jstr(baseline.message);
        }
        printf(",\n      \"cases\":[");
    } else {
        printf("[%s]%s\n", name, r == TESSERA_SUCCESS ? "" : " (the baseline request failed)");
        if (r == TESSERA_SUCCESS) {
            printf("  vendor   : %s\n  renderer : %s\n  version  : %s\n", baseline.info.vendor, baseline.info.renderer,
                   baseline.info.version);
        } else {
            printf("  %s: %s\n", tesseraResultString(r), baseline.message);
        }
        printf("  strict requests (a context is really created and destroyed for each):\n");
    }
    *firstBackend = 0;

    int firstCase = 1, ran = 0;
    for (int k = 0; k < g_caseCount; ++k) {
        if (g_quick && !g_cases[k].quick) continue;
        ++ran;
        TesseraProbeReport rep = tesseraDefaultProbeReport();
        r = tesseraProbe(&g_cases[k].cfg, &rep);
        if (r == TESSERA_SUCCESS) ++sum.created;
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
    if (g_json) printf("\n      ]}");
    else printf("  %d of %d requests satisfied\n\n", sum.created, ran);
    return sum;
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
    fputs("usage: tessera-diag [--json] [--quick] [--backend wgl|glx|egl|cgl] [--help]\n", stderr);
}

int main(int argc, char** argv) {
    const char* only = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--json")) g_json = 1;
        else if (!strcmp(argv[i], "--quick")) g_quick = 1;
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }
    int count = 0;
    for (const char* p = only; p && *p; ++p) count++;
    if (only && (count == 0 || count > 16 || strchr(only, ','))) {
        usage();
        return 2;
    }

    int32_t major = 0, minor = 0, patch = 0;
    tesseraGetVersion(&major, &minor, &patch);
    const char* userBackend = getenv("TESSERA_BACKEND");
    char savedBackend[128] = "";
    if (userBackend) snprintf(savedBackend, sizeof savedBackend, "%s", userBackend);

    buildCases();

    static const char* const kEnv[] = {"TESSERA_BACKEND", "TESSERA_EGL_PLATFORM", "TESSERA_EGL_DEVICE", "TESSERA_EGL_LIBRARY",
                                       "TESSERA_CGL_LIBRARY", "TESSERA_NO_XINITTHREADS", "DISPLAY", "WAYLAND_DISPLAY"};
    if (g_json) {
        printf("{\n  \"tessera\":\"%d.%d.%d\",\"abi\":%d,\"headerAbi\":%d,\"platform\":\"%s\",\"pointerBits\":%d,\n  \"environment\":{", major,
               minor, patch, tesseraGetAbiVersion(), TESSERA_ABI_VERSION, platformName(), (int)(sizeof(void*) * 8));
        int first = 1;
        for (size_t i = 0; i < sizeof kEnv / sizeof *kEnv; ++i) {
            const char* v = i == 0 ? (savedBackend[0] ? savedBackend : NULL) : getenv(kEnv[i]);
            if (!v) continue;
            printf("%s", first ? "" : ",");
            jstr(kEnv[i]);
            putchar(':');
            jstr(v);
            first = 0;
        }
        printf("},\n  \"backends\":[\n");
    } else {
        printf("Tessera %d.%d.%d (ABI %d, header ABI %d) on %s, %d-bit\n", major, minor, patch, tesseraGetAbiVersion(),
               TESSERA_ABI_VERSION, platformName(), (int)(sizeof(void*) * 8));
        for (size_t i = 0; i < sizeof kEnv / sizeof *kEnv; ++i) {
            const char* v = i == 0 ? (savedBackend[0] ? savedBackend : NULL) : getenv(kEnv[i]);
            if (v) printf("  %s=%s\n", kEnv[i], v);
        }
        printf("\n");
    }

    /* Candidate backends. A TESSERA_BACKEND already set by the user is respected as the single candidate. */
    const char* candidates[5];
    int nCand = 0;
    if (only) candidates[nCand++] = only;
    else if (savedBackend[0]) candidates[nCand++] = savedBackend;
    else {
        candidates[nCand++] = "wgl";
        candidates[nCand++] = "glx";
        candidates[nCand++] = "egl";
        candidates[nCand++] = "cgl";
    }

    int first = 1, anyCreated = 0;
    for (int i = 0; i < nCand; ++i) {
        BackendSummary s = runBackend(candidates[i], &first);
        if (s.created > 0) anyCreated = 1;
    }
    if (g_json) printf("\n  ]\n}\n");
    else if (!anyCreated) printf("No backend could create a context on this machine.\n");

    tesseraTerminate();
    if (savedBackend[0]) SETENV("TESSERA_BACKEND", savedBackend);
    else UNSETENV("TESSERA_BACKEND");
    return anyCreated ? 0 : 1;
}
