// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
// Runs the macOS (CGL) backend against tests/fake_cgl.cpp on any platform with dlopen. This checks the
// backend's own logic - attribute building, the negotiation ladder, strict vs best-effort, sharing rules,
// state restoration, release order and leaks. It does NOT check that Apple's real CGL accepts these
// attributes or behaves like the fake: that needs a Mac (the macOS CI job runs the same backend against the
// real framework, but has no such test yet).
#include <tessera/tessera.h>

#include <dlfcn.h>
#include <stdlib.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "core/runtime.hpp"
#include "platform/cgl/cgl_backend.hpp"
#include "platform/cgl/cgl_types.hpp"

using namespace tessera::cgl;

static int g_failures = 0;
#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                    \
            ++g_failures;                                                                                    \
        }                                                                                                    \
    } while (0)
#define EXPECT(call, want)                                                                                   \
    do {                                                                                                     \
        const TesseraResult got_ = (call);                                                                   \
        if (got_ != (want)) {                                                                                \
            std::fprintf(stderr, "%s:%d: %s -> %s, expected %s (%s)\n", __FILE__, __LINE__, #call,           \
                         tesseraResultString(got_), tesseraResultString(want),                               \
                         tesseraGetLastErrorMessage());                                                      \
            ++g_failures;                                                                                    \
        }                                                                                                    \
    } while (0)

using GlGenTextures = void (*)(int, unsigned*);
using GlIsTexture = unsigned char (*)(unsigned);

static CglFns g_cgl;
static int (*g_liveContexts)() = nullptr;
static int (*g_livePixelFormats)() = nullptr;
static int (*g_chooseCalls)() = nullptr;
static void (*g_setFailAt)(int) = nullptr;
static int (*g_fakeCalls)() = nullptr;

template <class Fn> static bool bind(void* lib, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(dlsym(lib, name));
    return out != nullptr;
}

static bool loadFake() {
    void* lib = dlopen(FAKE_CGL_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "cannot load %s: %s\n", FAKE_CGL_PATH, dlerror());
        return false;
    }
    return bind(lib, "CGLChoosePixelFormat", g_cgl.CGLChoosePixelFormat) &&
           bind(lib, "CGLReleasePixelFormat", g_cgl.CGLReleasePixelFormat) &&
           bind(lib, "CGLCreateContext", g_cgl.CGLCreateContext) &&
           bind(lib, "CGLDestroyContext", g_cgl.CGLDestroyContext) &&
           bind(lib, "CGLSetCurrentContext", g_cgl.CGLSetCurrentContext) &&
           bind(lib, "CGLGetCurrentContext", g_cgl.CGLGetCurrentContext) &&
           bind(lib, "fake_cgl_live_contexts", g_liveContexts) &&
           bind(lib, "fake_cgl_live_pixel_formats", g_livePixelFormats) &&
           bind(lib, "fake_cgl_choose_calls", g_chooseCalls) &&
           bind(lib, "fake_cgl_set_fail_at", g_setFailAt) && bind(lib, "fake_cgl_calls", g_fakeCalls);
}

static TesseraContextConfig baseConfig() {
    TesseraContextConfig c = tesseraDefaultContextConfig();
    c.api = TESSERA_API_OPENGL;
    return c;
}

static TesseraContextConfig coreConfig() {
    TesseraContextConfig c = baseConfig();
    c.profile = TESSERA_PROFILE_CORE;
    return c;
}

static void noLeaks(int line) {
    if (g_liveContexts() != 0 || g_livePixelFormats() != 0) {
        std::fprintf(stderr, "%s:%d: leak: %d context(s), %d pixel format(s) still alive\n", __FILE__, line,
                     g_liveContexts(), g_livePixelFormats());
        ++g_failures;
    }
}

// ---------------------------------------------------------------- tests

static void testLegacyDefault() {
    TesseraContextConfig cfg = baseConfig();
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    if (!ctx)
        return;
    TesseraContextInfo info = tesseraDefaultContextInfo();
    EXPECT(tesseraGetContextInfo(ctx, &info), TESSERA_SUCCESS);
    CHECK(info.api == TESSERA_API_OPENGL);
    CHECK(info.versionMajor == 2 && info.versionMinor == 1);
    CHECK(info.profile == TESSERA_PROFILE_COMPATIBILITY);
    CHECK(info.surface == TESSERA_SURFACE_NONE); // CGL contexts have no drawable
    CHECK(info.redBits == 8 && info.greenBits == 8 &&
          info.blueBits == 8); // 32-bit colour incl. 8 alpha -> 8 per channel
    CHECK(info.alphaBits == 8);
    CHECK(info.depthBits == 24);
    CHECK(std::strcmp(info.renderer, "Fake CGL Renderer") == 0);
    CHECK(g_livePixelFormats() ==
          0); // the format was released right after the context took its own reference
    CHECK(g_cgl.CGLGetCurrentContext() == nullptr); // creating leaves nothing current
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    noLeaks(__LINE__);
}

static void testCoreProfiles() {
    TesseraContext ctx = 0;
    TesseraContextConfig core = coreConfig();
    EXPECT(tesseraCreateContext(&core, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.profile == TESSERA_PROFILE_CORE);
        CHECK(info.versionMajor == 4 && info.versionMinor == 1); // the newest profile is tried first
        CHECK(info.flags & TESSERA_FLAG_FORWARD_COMPATIBLE); // core contexts on macOS are forward-compatible
        tesseraDestroyContext(ctx);
    }

    // Any request for 3.0+ is a core request on macOS, even with profile "any".
    TesseraContextConfig v33 = baseConfig();
    v33.versionMajor = 3;
    v33.versionMinor = 3;
    v33.fallback = TESSERA_FALLBACK_STRICT;
    EXPECT(tesseraCreateContext(&v33, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.profile == TESSERA_PROFILE_CORE && info.versionMajor == 4);
        tesseraDestroyContext(ctx);
    }

    // Hardware without 4.1: falls back to the 3.2 core profile for a request that allows it...
    setenv("FAKE_CGL_NO_GL4", "1", 1);
    EXPECT(tesseraCreateContext(&core, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.profile == TESSERA_PROFILE_CORE && info.versionMajor == 3 && info.versionMinor == 2);
        tesseraDestroyContext(ctx);
    }
    // ... and a strict request for 4.x then has no match.
    TesseraContextConfig v41 = coreConfig();
    v41.versionMajor = 4;
    v41.versionMinor = 1;
    v41.fallback = TESSERA_FALLBACK_STRICT;
    EXPECT(tesseraCreateContext(&v41, &ctx), TESSERA_ERROR_NO_MATCHING_CONFIG);
    unsetenv("FAKE_CGL_NO_GL4");
    noLeaks(__LINE__);
}

static void testRefusals() {
    TesseraContext ctx = 0;
    TesseraContextConfig es = baseConfig();
    es.api = TESSERA_API_OPENGL_ES;
    EXPECT(tesseraCreateContext(&es, &ctx),
           TESSERA_ERROR_UNSUPPORTED); // leaves the request to other backends

    auto noMatch = [&](TesseraContextConfig c, const char* why) {
        c.fallback = TESSERA_FALLBACK_STRICT;
        const TesseraResult r = tesseraCreateContext(&c, &ctx);
        if (r != TESSERA_ERROR_NO_MATCHING_CONFIG) {
            std::fprintf(stderr, "strict %s -> %s, expected no matching configuration\n", why,
                         tesseraResultString(r));
            ++g_failures;
        }
    };
    TesseraContextConfig compat33 = baseConfig();
    compat33.profile = TESSERA_PROFILE_COMPATIBILITY;
    compat33.versionMajor = 3;
    compat33.versionMinor = 2; // a compatibility profile must name 3.2+ to pass config validation at all
    noMatch(compat33, "compatibility profile 3.x");
    TesseraContextConfig v5 = coreConfig();
    v5.versionMajor = 5;
    noMatch(v5, "OpenGL 5");
    TesseraContextConfig v42 = coreConfig();
    v42.versionMajor = 4;
    v42.versionMinor = 2;
    noMatch(v42, "OpenGL 4.2");
    TesseraContextConfig v22 = baseConfig();
    v22.versionMajor = 2;
    v22.versionMinor = 2;
    noMatch(v22, "legacy 2.2");

    // Context flags macOS cannot provide: refused in strict mode, dropped (and reported absent) in
    // best-effort mode.
    const uint32_t unsupported[] = {TESSERA_FLAG_DEBUG, TESSERA_FLAG_ROBUST_ACCESS, TESSERA_FLAG_NO_ERROR,
                                    TESSERA_FLAG_RELEASE_FLUSH, TESSERA_FLAG_SRGB};
    for (uint32_t flag : unsupported) {
        TesseraContextConfig c = baseConfig();
        c.flags = flag;
        noMatch(c, "unsupported context flag");
        c.fallback = TESSERA_FALLBACK_BEST_EFFORT;
        EXPECT(tesseraCreateContext(&c, &ctx), TESSERA_SUCCESS);
        if (ctx) {
            TesseraContextInfo info = tesseraDefaultContextInfo();
            tesseraGetContextInfo(ctx, &info);
            CHECK(!(info.flags & flag)); // reported truthfully
            tesseraDestroyContext(ctx);
        }
    }
    // Forward-compatible is implied by core and impossible for legacy.
    TesseraContextConfig fwdLegacy = baseConfig();
    fwdLegacy.flags = TESSERA_FLAG_FORWARD_COMPATIBLE;
    noMatch(fwdLegacy, "forward-compatible legacy context");
    TesseraContextConfig fwdCore = coreConfig();
    fwdCore.flags = TESSERA_FLAG_FORWARD_COMPATIBLE;
    fwdCore.fallback = TESSERA_FALLBACK_STRICT;
    EXPECT(tesseraCreateContext(&fwdCore, &ctx), TESSERA_SUCCESS);
    if (ctx)
        tesseraDestroyContext(ctx);
    noLeaks(__LINE__);
}

static void testPixelFormatNegotiation() {
    TesseraContext ctx = 0;
    // Multisampling beyond what the "hardware" offers: strict says no, best effort halves until it fits.
    TesseraContextConfig msaa = baseConfig();
    msaa.samples = 64;
    msaa.fallback = TESSERA_FALLBACK_STRICT;
    EXPECT(tesseraCreateContext(&msaa, &ctx), TESSERA_ERROR_NO_MATCHING_CONFIG);
    msaa.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    EXPECT(tesseraCreateContext(&msaa, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.samples == 4); // 64 -> 32 -> 16 -> 8 -> 4 (the fake's limit)
        tesseraDestroyContext(ctx);
    }
    setenv("FAKE_CGL_MAX_SAMPLES", "0", 1);
    EXPECT(tesseraCreateContext(&msaa, &ctx), TESSERA_SUCCESS); // down to no multisampling at all
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.samples == 0);
        tesseraDestroyContext(ctx);
    }
    unsetenv("FAKE_CGL_MAX_SAMPLES");

    // A format that exists only on an offline (not display-driving) GPU is found by the second,
    // offline-allowing try.
    setenv("FAKE_CGL_NEEDS_OFFLINE", "1", 1);
    const int before = g_chooseCalls();
    EXPECT(tesseraCreateContext(&msaa, &ctx), TESSERA_SUCCESS);
    CHECK(g_chooseCalls() - before >= 2);
    if (ctx)
        tesseraDestroyContext(ctx);
    unsetenv("FAKE_CGL_NEEDS_OFFLINE");
    // ... but the cheap, display-GPU-only attempt wins when it works (no needless wake-up of a discrete GPU).
    const int before2 = g_chooseCalls();
    TesseraContextConfig plain = baseConfig();
    EXPECT(tesseraCreateContext(&plain, &ctx), TESSERA_SUCCESS);
    CHECK(g_chooseCalls() - before2 == 1);
    if (ctx)
        tesseraDestroyContext(ctx);

    // Float colour.
    TesseraContextConfig fl = baseConfig();
    fl.flags = TESSERA_FLAG_FLOAT_COLOR;
    fl.fallback = TESSERA_FALLBACK_STRICT;
    EXPECT(tesseraCreateContext(&fl, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.flags & TESSERA_FLAG_FLOAT_COLOR);
        CHECK(info.redBits == 16); // 64-bit colour incl. 16 alpha -> 16 per channel
        tesseraDestroyContext(ctx);
    }
    setenv("FAKE_CGL_NO_FLOAT", "1", 1);
    EXPECT(tesseraCreateContext(&fl, &ctx), TESSERA_ERROR_NO_MATCHING_CONFIG);
    unsetenv("FAKE_CGL_NO_FLOAT");

    // Depth/stencil and double-buffer requests are passed through and reported.
    TesseraContextConfig ds = baseConfig();
    ds.depthBits = 32;
    ds.stencilBits = 8;
    ds.flags = TESSERA_FLAG_DOUBLE_BUFFER;
    ds.fallback = TESSERA_FALLBACK_STRICT;
    EXPECT(tesseraCreateContext(&ds, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.depthBits == 32 && info.stencilBits == 8);
        CHECK(info.flags & TESSERA_FLAG_DOUBLE_BUFFER);
        tesseraDestroyContext(ctx);
    }
    noLeaks(__LINE__);
}

static void testMakeCurrentAndProcs() {
    TesseraContextConfig cfg = baseConfig();
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    if (!ctx)
        return;

    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    CHECK(tesseraGetCurrentContext() == ctx);
    CHECK(g_cgl.CGLGetCurrentContext() != nullptr);
    CHECK(tesseraGetProcAddress("glGetString") != nullptr);
    CHECK(tesseraGetProcAddress("glNoSuchFunction") == nullptr); // dlsym gives a real NULL here
    CHECK(tesseraGetProcAddress("CGLCreateContext") == nullptr); // not a GL function: never handed out
    CHECK(tesseraGetProcAddress(nullptr) == nullptr);
    EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS); // no drawable: nothing to present
    EXPECT(tesseraSetSwapInterval(ctx, 1), TESSERA_ERROR_UNSUPPORTED);

    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    CHECK(g_cgl.CGLGetCurrentContext() == nullptr);
    EXPECT(tesseraSwapBuffers(ctx), TESSERA_ERROR_WRONG_THREAD);

    // Destroying a context that is current on this thread releases it first.
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    CHECK(g_cgl.CGLGetCurrentContext() == nullptr);
    noLeaks(__LINE__);
}

static void testForeignContextUntouched() {
    // The "other library": a raw CGL context made current on this thread before and during Tessera's work.
    const int32_t attribs[] = {99, 0x1000, 0};
    CGLPixelFormatObj pf = nullptr;
    int32_t n = 0;
    CHECK(g_cgl.CGLChoosePixelFormat(attribs, &pf, &n) == 0 && pf);
    CGLContextObj foreign = nullptr;
    CHECK(g_cgl.CGLCreateContext(pf, nullptr, &foreign) == 0 && foreign);
    g_cgl.CGLReleasePixelFormat(pf);
    g_cgl.CGLSetCurrentContext(foreign);

    TesseraContextConfig cfg = baseConfig();
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    CHECK(g_cgl.CGLGetCurrentContext() == foreign); // successful create

    TesseraContextConfig bad = baseConfig();
    bad.samples = 64;
    bad.fallback = TESSERA_FALLBACK_STRICT;
    TesseraContext none = 0;
    EXPECT(tesseraCreateContext(&bad, &none), TESSERA_ERROR_NO_MATCHING_CONFIG);
    CHECK(g_cgl.CGLGetCurrentContext() == foreign); // failing create
    TesseraContextConfig refused = baseConfig();
    refused.flags = TESSERA_FLAG_DEBUG;
    refused.fallback = TESSERA_FALLBACK_STRICT;
    EXPECT(tesseraCreateContext(&refused, &none), TESSERA_ERROR_NO_MATCHING_CONFIG);
    CHECK(g_cgl.CGLGetCurrentContext() == foreign); // refused before any native call

    // Ours displaced by the other library, then released by us: must not drop theirs.
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    g_cgl.CGLSetCurrentContext(foreign);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    CHECK(g_cgl.CGLGetCurrentContext() == foreign);

    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    g_cgl.CGLSetCurrentContext(foreign);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    CHECK(g_cgl.CGLGetCurrentContext() == foreign);

    g_cgl.CGLSetCurrentContext(nullptr);
    g_cgl.CGLDestroyContext(foreign);
    noLeaks(__LINE__);
}

static void testSharing() {
    TesseraContextConfig legacy = baseConfig();
    TesseraContext a = 0, b = 0;
    EXPECT(tesseraCreateContext(&legacy, &a), TESSERA_SUCCESS);
    if (!a)
        return;
    legacy.shareWith = a;
    EXPECT(tesseraCreateContext(&legacy, &b), TESSERA_SUCCESS);
    if (!b) {
        tesseraDestroyContext(a);
        return;
    }

    EXPECT(tesseraMakeCurrent(a), TESSERA_SUCCESS);
    auto genTextures = reinterpret_cast<GlGenTextures>(tesseraGetProcAddress("glGenTextures"));
    auto isTexture = reinterpret_cast<GlIsTexture>(tesseraGetProcAddress("glIsTexture"));
    CHECK(genTextures && isTexture);
    unsigned tex = 0;
    if (genTextures && isTexture) {
        genTextures(1, &tex);
        CHECK(tex != 0 && isTexture(tex));
        EXPECT(tesseraMakeCurrent(b), TESSERA_SUCCESS); // switching our own contexts on one thread
        CHECK(tesseraGetCurrentContext() == b);
        CHECK(isTexture(tex)); // visible through the share group
    }
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);

    // A core child of a legacy parent (or the reverse) cannot share objects.
    TesseraContextConfig core = coreConfig();
    core.shareWith = a;
    TesseraContext cross = 0;
    EXPECT(tesseraCreateContext(&core, &cross), TESSERA_ERROR_NO_MATCHING_CONFIG);
    CHECK(std::strstr(tesseraGetLastErrorMessage(), "legacy") != nullptr);

    // Parent first: the child keeps working (it holds its own reference to the parent).
    EXPECT(tesseraDestroyContext(a), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(b), TESSERA_SUCCESS);
    if (isTexture)
        CHECK(isTexture(tex));
    EXPECT(tesseraDestroyContext(b), TESSERA_SUCCESS);
    noLeaks(__LINE__);

    // Core parent with a core child follows the parent's profile (and the fake accepts the pair).
    TesseraContextConfig cp = coreConfig();
    TesseraContext p = 0, c = 0;
    EXPECT(tesseraCreateContext(&cp, &p), TESSERA_SUCCESS);
    cp.shareWith = p;
    EXPECT(tesseraCreateContext(&cp, &c), TESSERA_SUCCESS);
    if (c)
        tesseraDestroyContext(c);
    if (p)
        tesseraDestroyContext(p);
    noLeaks(__LINE__);
}

static void testThreadsAndConcurrency() {
    TesseraContextConfig cfg = baseConfig();
    TesseraContext made = 0;
    std::thread([&] { EXPECT(tesseraCreateContext(&cfg, &made), TESSERA_SUCCESS); }).join();
    std::thread([&] {
        EXPECT(tesseraMakeCurrent(made), TESSERA_SUCCESS);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    }).join();
    std::thread([&] { EXPECT(tesseraDestroyContext(made), TESSERA_SUCCESS); }).join();

    // A thread that exits while its context is current: Tessera releases it on that thread.
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    std::thread([&] { EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS); }).join();
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 50; ++i) {
                TesseraContextConfig c = baseConfig();
                TesseraContext h = 0;
                if (tesseraCreateContext(&c, &h) != TESSERA_SUCCESS) {
                    ++failures;
                    continue;
                }
                if (tesseraMakeCurrent(h) != TESSERA_SUCCESS)
                    ++failures;
                if (tesseraDestroyContext(h) != TESSERA_SUCCESS)
                    ++failures;
            }
        });
    for (auto& t : threads)
        t.join();
    CHECK(failures == 0);
    noLeaks(__LINE__);
}

static void testTerminateDestroysLeaks() {
    TesseraContextConfig cfg = baseConfig();
    TesseraContext leaked = 0;
    EXPECT(tesseraCreateContext(&cfg, &leaked), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(leaked), TESSERA_SUCCESS); // leaked while current on this thread
    CHECK(g_liveContexts() == 1);
    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    CHECK(g_liveContexts() == 0);
    CHECK(g_cgl.CGLGetCurrentContext() == nullptr);
    EXPECT(tesseraInit(), TESSERA_SUCCESS);
    noLeaks(__LINE__);
}

// Fails every native call position of a create in turn. After each injected fault: no leaked contexts or
// pixel formats, the other library's context still current, a sensible error code, and no handle returned on
// failure.
static void testFaultInjection() {
    // The "other library": a raw legacy context current on this thread throughout.
    const int32_t rawAttribs[] = {99, 0x1000, 0};
    CGLPixelFormatObj pf = nullptr;
    int32_t n = 0;
    CHECK(g_cgl.CGLChoosePixelFormat(rawAttribs, &pf, &n) == 0 && pf);
    CGLContextObj foreign = nullptr;
    CHECK(g_cgl.CGLCreateContext(pf, nullptr, &foreign) == 0 && foreign);
    g_cgl.CGLReleasePixelFormat(pf);
    g_cgl.CGLSetCurrentContext(foreign);

    struct Scenario {
        const char* name;
        TesseraContextConfig cfg;
        bool withParent;
    };
    TesseraContextConfig legacy = baseConfig();
    TesseraContextConfig core = coreConfig();
    TesseraContextConfig msaa = baseConfig();
    msaa.samples =
        64; // many pixel-format attempts: lots of call positions, and graceful degradation when one fails
    msaa.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraContextConfig fl = baseConfig();
    fl.flags = TESSERA_FLAG_FLOAT_COLOR;
    TesseraContextConfig child = baseConfig();
    const Scenario scenarios[] = {{"legacy", legacy, false},
                                  {"core", core, false},
                                  {"msaa 64 best-effort", msaa, false},
                                  {"float colour", fl, false},
                                  {"legacy sharing with a live parent", child, true}};

    for (const Scenario& sc : scenarios) {
        TesseraContextConfig cfg = sc.cfg;
        TesseraContext parent = 0;
        if (sc.withParent) {
            TesseraContextConfig pc = baseConfig();
            EXPECT(tesseraCreateContext(&pc, &parent), TESSERA_SUCCESS);
            cfg.shareWith = parent;
        }
        const int baseline = g_liveContexts();

        // Dry run: how many native calls does a healthy create make?
        TesseraContext h = 0;
        g_setFailAt(0);
        EXPECT(tesseraCreateContext(&cfg, &h), TESSERA_SUCCESS);
        const int calls = g_fakeCalls();
        if (h)
            tesseraDestroyContext(h);

        int clean = 0, survived = 0;
        for (int at = 1; at <= calls; ++at) {
            g_cgl.CGLSetCurrentContext(foreign);
            h = 0;
            g_setFailAt(at);
            const TesseraResult r = tesseraCreateContext(&cfg, &h);
            g_setFailAt(0); // from here on the fake behaves

            if (r == TESSERA_SUCCESS) {
                ++survived; // the fault hit something the backend can do without (or retried around)
                CHECK(h != TESSERA_NULL_HANDLE);
                EXPECT(tesseraDestroyContext(h), TESSERA_SUCCESS);
            } else {
                ++clean;
                CHECK(h == TESSERA_NULL_HANDLE);
                CHECK(r == TESSERA_ERROR_OUT_OF_MEMORY || r == TESSERA_ERROR_PLATFORM ||
                      r == TESSERA_ERROR_NO_MATCHING_CONFIG);
                CHECK(tesseraGetLastErrorMessage()[0] != '\0');
            }
            if (g_liveContexts() != baseline || g_livePixelFormats() != 0) {
                std::fprintf(stderr,
                             "%s:%d: leak after a fault at native call %d/%d of '%s': %d context(s) "
                             "(expected %d), %d pixel format(s)\n",
                             __FILE__, __LINE__, at, calls, sc.name, g_liveContexts(), baseline,
                             g_livePixelFormats());
                ++g_failures;
            }
            if (g_cgl.CGLGetCurrentContext() != foreign) {
                std::fprintf(stderr,
                             "%s:%d: the other library's context was disturbed by a fault at native call "
                             "%d/%d of '%s'\n",
                             __FILE__, __LINE__, at, calls, sc.name);
                ++g_failures;
            }
        }
        std::printf("  %-34s %2d native calls: %2d faults failed cleanly, %2d survived\n", sc.name, calls,
                    clean, survived);
        CHECK(clean > 0); // at least the pixel-format and context-creation calls must be fatal
        if (parent)
            tesseraDestroyContext(parent);
    }

    // Faults while tearing down: the failing destroy/release must not crash or leave the runtime unusable.
    TesseraContextConfig cfg = baseConfig();
    TesseraContext h = 0;
    EXPECT(tesseraCreateContext(&cfg, &h), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(h), TESSERA_SUCCESS);
    g_setFailAt(1); // the first native call during teardown (the release of the current context) fails
    tesseraDestroyContext(h);
    g_setFailAt(0);
    g_cgl.CGLSetCurrentContext(nullptr);
    TesseraContext after = 0;
    EXPECT(tesseraCreateContext(&cfg, &after), TESSERA_SUCCESS); // still fully functional
    if (after)
        tesseraDestroyContext(after);

    g_cgl.CGLSetCurrentContext(nullptr);
    g_cgl.CGLDestroyContext(foreign);
    noLeaks(__LINE__);
}

// A missing library, and a library that exists but is not OpenGL.framework: clean UNSUPPORTED, never a crash.
static int runUnavailableLibrary(const char* path) {
    setenv("TESSERA_CGL_LIBRARY", path, 1);
    tessera::Runtime::disableBuiltinBackendsForTesting();
    if (tesseraInit() != TESSERA_SUCCESS)
        return 1;
    tessera::Runtime::instance().registerBackend(makeCglBackend());
    TesseraContextConfig cfg = baseConfig();
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_ERROR_UNSUPPORTED);
    CHECK(ctx == TESSERA_NULL_HANDLE);
    CHECK(tesseraGetProcAddress("glGetString") == nullptr);
    tesseraTerminate();
    if (g_failures == 0)
        std::printf("test_cgl_fake (%s): OK\n", path);
    return g_failures == 0 ? 0 : 1;
}

// One way to say "this GPU" (ABI 0.14.0): the renderer inventory, selection by id, and the report of who
// served the context.
static TesseraContextConfig adapterConfig(const char* id) {
    TesseraContextConfig c = baseConfig();
    std::snprintf(c.adapter, sizeof c.adapter, "%s", id);
    return c;
}

static void testAdapters() {
    int32_t count = 0;
    EXPECT(tesseraEnumerateAdapters(nullptr, 0, &count), TESSERA_SUCCESS);
    CHECK(count == 3);
    TesseraAdapterInfo list[4];
    for (auto& a : list)
        a = tesseraDefaultAdapterInfo();
    EXPECT(tesseraEnumerateAdapters(list, 4, &count), TESSERA_SUCCESS);
    CHECK(count == 3);
    CHECK(std::strcmp(list[0].id, "cgl:0x00024600") == 0 && std::strcmp(list[0].backend, "cgl") == 0);
    CHECK(list[0].flags == 0 && std::strstr(list[0].name, "1536 MB") != nullptr);
    CHECK(std::strcmp(list[1].id, "cgl:0x00022600") == 0 && std::strstr(list[1].name, "offline") != nullptr);
    CHECK(std::strcmp(list[2].id, "cgl:0x00020400") == 0 && (list[2].flags & TESSERA_ADAPTER_SOFTWARE));
    // a smaller capacity writes what fits and still reports the total
    TesseraAdapterInfo one[1] = {tesseraDefaultAdapterInfo()};
    EXPECT(tesseraEnumerateAdapters(one, 1, &count), TESSERA_SUCCESS);
    CHECK(count == 3 && std::strcmp(one[0].id, "cgl:0x00024600") == 0);

    // Who served a default context.
    TesseraContext ctx = 0;
    TesseraContextConfig cfg = baseConfig();
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    TesseraContextInfo info = tesseraDefaultContextInfo();
    if (ctx) {
        EXPECT(tesseraGetContextInfo(ctx, &info), TESSERA_SUCCESS);
        CHECK(std::strcmp(info.adapter, "cgl:0x00024600") == 0);
        EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    }

    // Selecting: the id asked for is the id reported, including the one that needs offline renderers allowed.
    for (const char* id : {"cgl:0x00024600", "cgl:0x00022600", "cgl:0x00020400"}) {
        TesseraContextConfig want = adapterConfig(id);
        TesseraContext c = 0;
        EXPECT(tesseraCreateContext(&want, &c), TESSERA_SUCCESS);
        if (!c)
            continue;
        info = tesseraDefaultContextInfo();
        EXPECT(tesseraGetContextInfo(c, &info), TESSERA_SUCCESS);
        CHECK(std::strcmp(info.adapter, id) == 0);
        if (std::strcmp(id, "cgl:0x00020400") == 0)
            CHECK(info.adapterClass & TESSERA_ADAPTER_SOFTWARE);
        EXPECT(tesseraDestroyContext(c), TESSERA_SUCCESS);
    }
    // An explicit choice is never replaced: no such renderer is a refusal, another backend's id is
    // UNSUPPORTED.
    TesseraContextConfig none = adapterConfig("cgl:0x00099999");
    TesseraContext c = 0;
    EXPECT(tesseraCreateContext(&none, &c), TESSERA_ERROR_NO_MATCHING_CONFIG);
    TesseraContextConfig foreign = adapterConfig("luid:00000000-00001234");
    EXPECT(tesseraCreateContext(&foreign, &c), TESSERA_ERROR_UNSUPPORTED);
    TesseraContextConfig garbage = adapterConfig("cgl:0xZZ");
    EXPECT(tesseraCreateContext(&garbage, &c), TESSERA_ERROR_UNSUPPORTED);
    TesseraContextConfig luidReq = baseConfig();
    luidReq.adapterLuid = 5; // a Windows request must not be served by whatever renderer CGL likes
    EXPECT(tesseraCreateContext(&luidReq, &c), TESSERA_ERROR_UNSUPPORTED);
    noLeaks(__LINE__);
}

// Sharing explained (ABI 0.15.0): a core context and a legacy one cannot share objects on macOS; the
// preflight says so first, and agrees with what the create then does.
static void testShareVerdicts() {
    TesseraContextConfig legacy = baseConfig(), core = coreConfig();
    TesseraContext lp = 0, cp = 0;
    EXPECT(tesseraCreateContext(&legacy, &lp), TESSERA_SUCCESS);
    EXPECT(tesseraCreateContext(&core, &cp), TESSERA_SUCCESS);
    if (!lp || !cp)
        return;
    struct Case {
        const TesseraContextConfig* want;
        TesseraContext parent;
        int32_t expected;
    };
    const Case cases[] = {{&legacy, lp, TESSERA_SHARE_OK},
                          {&core, cp, TESSERA_SHARE_OK},
                          {&core, lp, TESSERA_SHARE_PROFILE_MISMATCH},
                          {&legacy, cp, TESSERA_SHARE_PROFILE_MISMATCH}};
    for (const Case& c : cases) {
        int32_t why = -1;
        EXPECT(tesseraCanShare(c.want, c.parent, &why), TESSERA_SUCCESS);
        CHECK(why == c.expected);
        TesseraContextConfig share = *c.want;
        share.shareWith = c.parent;
        TesseraContext made = 0;
        const TesseraResult r = tesseraCreateContext(&share, &made);
        CHECK((why == TESSERA_SHARE_OK) ==
              (r == TESSERA_SUCCESS)); // the verdict is what the create then does
        if (made)
            EXPECT(tesseraDestroyContext(made), TESSERA_SUCCESS);
    }
    TesseraShareGroupInfo g = tesseraDefaultShareGroupInfo();
    EXPECT(tesseraGetShareGroup(lp, &g), TESSERA_SUCCESS);
    CHECK(g.members == 1 && g.alive == 1);
    EXPECT(tesseraDestroyContext(lp), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(cp), TESSERA_SUCCESS);
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--missing-library") == 0)
        return runUnavailableLibrary("/nonexistent/OpenGL");
    if (argc > 1 && std::strcmp(argv[1], "--wrong-library") == 0) {
#if defined(__APPLE__)
        return runUnavailableLibrary("/usr/lib/libSystem.B.dylib");
#else
        return runUnavailableLibrary("libm.so.6");
#endif
    }
    setenv("TESSERA_CGL_LIBRARY", FAKE_CGL_PATH, 1);
    if (!loadFake())
        return 77;

    // Only the backend under test: the real platform backends (GLX/EGL/WGL) must not answer for it.
    tessera::Runtime::disableBuiltinBackendsForTesting();
    if (tesseraInit() != TESSERA_SUCCESS) {
        std::fprintf(stderr, "tesseraInit failed: %s\n", tesseraGetLastErrorMessage());
        return 1;
    }
    tessera::Runtime::instance().registerBackend(makeCglBackend());

    struct Section {
        const char* name;
        void (*fn)();
    };
    const Section sections[] = {
        {"legacy default context", testLegacyDefault},
        {"core profiles", testCoreProfiles},
        {"refusals and dropped flags", testRefusals},
        {"pixel format negotiation", testPixelFormatNegotiation},
        {"make current / procs / swap", testMakeCurrentAndProcs},
        {"foreign context untouched", testForeignContextUntouched},
        {"adapters (inventory, selection, report)", testAdapters},
        {"sharing", testSharing},
        {"sharing verdicts", testShareVerdicts},
        {"threads and concurrency", testThreadsAndConcurrency},
        {"fault injection at every native call", testFaultInjection},
        {"terminate destroys leaked contexts", testTerminateDestroysLeaks},
    };
    for (const Section& sec : sections) {
        std::puts(sec.name);
        sec.fn();
    }

    tesseraTerminate();
    if (g_failures == 0)
        std::puts("test_cgl_fake: OK");
    return g_failures == 0 ? 0 : 1;
}
