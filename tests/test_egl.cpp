// Integration tests for the EGL backend, through the public C API only (the backend is forced with
// TESSERA_BACKEND=egl). Needs an EGL implementation with a headless platform (Mesa surfaceless, or a vendor
// EGL with EGL_EXT_platform_device). Raw EGL contexts created here play the role of "other libraries in the
// same process" whose per-thread EGL state Tessera must leave exactly as it found it. Exits 77 (ctest:
// skipped) when EGL is not available.
#include <tessera/tessera.h>

#include <dirent.h>
#include <dlfcn.h>
#include <stdlib.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "platform/egl/egl_types.hpp"

using namespace tessera::egl;

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

#include "interop_common.hpp"
#include "robustness_common.hpp"

using GlGetString = const unsigned char* (*)(unsigned);
using GlGenTextures = void (*)(int, unsigned*);
using GlIsTexture = unsigned char (*)(unsigned);
using GlBindTexture = void (*)(unsigned, unsigned);

// ---------------------------------------------------------------- raw libEGL for the "other library"

static EglFns g_e;

template <class Fn> static bool bind(void* lib, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(dlsym(lib, name));
    return out != nullptr;
}

static bool loadRaw() {
    void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return false;
    const bool ok =
        bind(lib, "eglQueryString", g_e.eglQueryString) && bind(lib, "eglGetError", g_e.eglGetError) &&
        bind(lib, "eglGetDisplay", g_e.eglGetDisplay) && bind(lib, "eglInitialize", g_e.eglInitialize) &&
        bind(lib, "eglBindAPI", g_e.eglBindAPI) && bind(lib, "eglQueryAPI", g_e.eglQueryAPI) &&
        bind(lib, "eglChooseConfig", g_e.eglChooseConfig) &&
        bind(lib, "eglCreateContext", g_e.eglCreateContext) &&
        bind(lib, "eglDestroyContext", g_e.eglDestroyContext) &&
        bind(lib, "eglMakeCurrent", g_e.eglMakeCurrent) &&
        bind(lib, "eglGetCurrentContext", g_e.eglGetCurrentContext) &&
        bind(lib, "eglGetCurrentSurface", g_e.eglGetCurrentSurface) &&
        bind(lib, "eglCreatePbufferSurface", g_e.eglCreatePbufferSurface) &&
        bind(lib, "eglDestroySurface", g_e.eglDestroySurface) &&
        bind(lib, "eglGetProcAddress", g_e.eglGetProcAddress);
    if (!ok)
        return false;
    g_e.eglGetPlatformDisplayEXT = reinterpret_cast<decltype(g_e.eglGetPlatformDisplayEXT)>(
        g_e.eglGetProcAddress("eglGetPlatformDisplayEXT"));
    return true;
}

struct ForeignContext {
    EGLDisplay dpy = nullptr;
    EGLContext ctx = nullptr;
    EGLSurface surf = nullptr;
    EGLenum api = 0;
    bool onSharedSurfaceless = false; // the very same EGLDisplay Tessera will use

    bool create(EGLenum apiEnum, bool sharedSurfaceless) {
        api = apiEnum;
        onSharedSurfaceless = sharedSurfaceless;
        if (sharedSurfaceless) {
            if (!g_e.eglGetPlatformDisplayEXT)
                return false;
            dpy = g_e.eglGetPlatformDisplayEXT(kPlatformSurfaceless, nullptr, nullptr);
        } else {
            dpy = g_e.eglGetDisplay(nullptr); // what a toolkit would use
        }
        EGLint major = 0, minor = 0;
        if (!dpy || !g_e.eglInitialize(dpy, &major, &minor))
            return false;
        if (!g_e.eglBindAPI(api))
            return false;
        const EGLint attribs[] = {kSurfaceType,
                                  kPbufferBit,
                                  kRenderableType,
                                  api == kOpenGlEsApi ? kOpenGlEs2Bit : kOpenGlBit,
                                  kRedSize,
                                  8,
                                  kGreenSize,
                                  8,
                                  kBlueSize,
                                  8,
                                  kNone};
        EGLConfig cfg = nullptr;
        EGLint n = 0;
        if (!g_e.eglChooseConfig(dpy, attribs, &cfg, 1, &n) || n == 0)
            return false;
        const EGLint ctxAttribs[] = {kContextMajorVersion, 2, kNone};
        ctx = g_e.eglCreateContext(dpy, cfg, nullptr, api == kOpenGlEsApi ? ctxAttribs : ctxAttribs + 2);
        const EGLint pb[] = {kWidth, 1, kHeight, 1, kNone};
        surf = g_e.eglCreatePbufferSurface(dpy, cfg, pb);
        return ctx != nullptr && surf != nullptr;
    }
    void makeCurrent() const {
        g_e.eglBindAPI(api);
        g_e.eglMakeCurrent(dpy, surf, surf, ctx);
    }
    // Looks without disturbing: restores the bound API it had to change to look.
    bool isCurrent() const {
        const EGLenum before = g_e.eglQueryAPI();
        g_e.eglBindAPI(api);
        const bool r = g_e.eglGetCurrentContext() == ctx && g_e.eglGetCurrentSurface(kDraw) == surf;
        g_e.eglBindAPI(before);
        return r;
    }
    bool nothingCurrent(EGLenum forApi) const {
        const EGLenum before = g_e.eglQueryAPI();
        g_e.eglBindAPI(forApi);
        const bool r = g_e.eglGetCurrentContext() == nullptr;
        g_e.eglBindAPI(before);
        return r;
    }
    void destroy() {
        if (!dpy)
            return;
        g_e.eglBindAPI(api);
        if (g_e.eglGetCurrentContext() == ctx)
            g_e.eglMakeCurrent(dpy, nullptr, nullptr, nullptr);
        if (ctx)
            g_e.eglDestroyContext(dpy, ctx);
        if (surf)
            g_e.eglDestroySurface(dpy, surf);
        *this = ForeignContext{};
        // Never eglTerminate here either: the display is shared with whoever else asked for it.
    }
};

static TesseraContextConfig baseConfig() {
    TesseraContextConfig c = tesseraDefaultContextConfig();
    c.api = TESSERA_API_OPENGL;
    return c; // no DOUBLE_BUFFER: EGL pbuffer/surfaceless drawables are single-buffered
}

static TesseraContextConfig esConfig(int major = 0) {
    TesseraContextConfig c = baseConfig();
    c.api = TESSERA_API_OPENGL_ES;
    c.versionMajor = major;
    return c;
}

static int openFdCount() {
    int n = 0;
    if (DIR* d = opendir("/proc/self/fd")) {
        while (readdir(d))
            ++n;
        closedir(d);
    }
    return n;
}

// ---------------------------------------------------------------- tests

static void testBasicCreateAndInfo() {
    TesseraContextConfig cfg = baseConfig();
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    if (!ctx)
        return;

    TesseraContextInfo info = tesseraDefaultContextInfo();
    EXPECT(tesseraGetContextInfo(ctx, &info), TESSERA_SUCCESS);
    std::printf("  default context: %s | %s | GL %s\n    api=%u %d.%d profile=%u flags=0x%x rgba=%d%d%d%d "
                "d=%d s=%d samples=%d surface=%u\n",
                info.vendor, info.renderer, info.version, info.api, info.versionMajor, info.versionMinor,
                info.profile, info.flags, info.redBits, info.greenBits, info.blueBits, info.alphaBits,
                info.depthBits, info.stencilBits, info.samples, info.surface);
    CHECK(info.api == TESSERA_API_OPENGL);
    CHECK(info.profile == TESSERA_PROFILE_COMPATIBILITY);
    CHECK(info.versionMajor >= 1);
    CHECK(info.renderer[0] != '\0' && info.version[0] != '\0');
    CHECK(info.redBits >= 8 && info.greenBits >= 8 && info.blueBits >= 8);
    CHECK(!(info.flags & TESSERA_FLAG_DOUBLE_BUFFER));
    CHECK(info.surface == TESSERA_SURFACE_PBUFFER); // the default drawable on EGL is a pbuffer, not a window

    CHECK(tesseraGetCurrentContext() == TESSERA_NULL_HANDLE);

    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_ERROR_INVALID_HANDLE);
}

// Runs the guest-behaviour checks against one foreign library setup.
static void foreignUntouched(EGLenum foreignApi, bool sharedDisplay, const char* label) {
    std::printf("  foreign: %s\n", label);
    ForeignContext foreign;
    if (!foreign.create(foreignApi, sharedDisplay)) {
        std::printf("    (cannot set up this foreign context here, skipped)\n");
        foreign.destroy();
        return;
    }
    foreign.makeCurrent();
    CHECK(foreign.isCurrent());
    CHECK(g_e.eglQueryAPI() == foreignApi);

    auto intact = [&](int line) {
        if (!foreign.isCurrent() || g_e.eglQueryAPI() != foreignApi) {
            std::fprintf(stderr, "%s:%d: foreign EGL state disturbed (current=%d api=0x%x)\n", __FILE__, line,
                         foreign.isCurrent(), g_e.eglQueryAPI());
            ++g_failures;
        }
    };

    TesseraContextConfig gl = baseConfig();
    TesseraContext glCtx = 0;
    EXPECT(tesseraCreateContext(&gl, &glCtx), TESSERA_SUCCESS);
    intact(__LINE__);

    TesseraContextConfig es = esConfig();
    TesseraContext esCtx = 0;
    EXPECT(tesseraCreateContext(&es, &esCtx), TESSERA_SUCCESS);
    intact(__LINE__);

    TesseraContextConfig absurd = baseConfig();
    absurd.samples = 64;
    absurd.fallback = TESSERA_FALLBACK_STRICT;
    TesseraContext none = 0;
    EXPECT(tesseraCreateContext(&absurd, &none), TESSERA_ERROR_NO_MATCHING_CONFIG);
    intact(__LINE__); // failing create

    // Making our context current replaces the thread's context for OUR api only; the foreign one for the
    // other api (and, with the same api, its binding) must survive our release.
    EXPECT(tesseraMakeCurrent(glCtx), TESSERA_SUCCESS);
    EXPECT(tesseraSwapBuffers(glCtx), TESSERA_SUCCESS);
    CHECK(g_e.eglQueryAPI() == foreignApi); // our make-current did not leave our API bound
    // Whether the other API's context survives while ours is current is up to the EGL implementation: the
    // spec says one current context per API, Mesa keeps one per thread. Either is fine; what Tessera owes is
    // restoring the API binding.
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    CHECK(g_e.eglQueryAPI() == foreignApi);

    // Our context displaced by the foreign library, then released by us: must not drop the foreign one.
    EXPECT(tesseraMakeCurrent(glCtx), TESSERA_SUCCESS);
    foreign.makeCurrent();
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    intact(__LINE__);

    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS); // nothing of ours current: no-op
    intact(__LINE__);

    // Both of our contexts, one per API, on this thread; switching releases the previous one.
    EXPECT(tesseraMakeCurrent(glCtx), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(esCtx), TESSERA_SUCCESS);
    CHECK(tesseraGetCurrentContext() == esCtx);
    EXPECT(tesseraMakeCurrent(glCtx), TESSERA_SUCCESS); // the earlier one is free again: nothing is stuck
    EXPECT(tesseraMakeCurrent(esCtx), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    foreign.makeCurrent();

    EXPECT(tesseraDestroyContext(glCtx), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(esCtx), TESSERA_SUCCESS);
    intact(__LINE__);

    foreign.destroy();
}

static void testForeignContextUntouched() {
    foreignUntouched(kOpenGlEsApi, false, "GLES context on the toolkit's default display");
    foreignUntouched(kOpenGlApi, false, "desktop GL context on the toolkit's default display");
    foreignUntouched(kOpenGlEsApi, true, "GLES context on the SAME surfaceless EGLDisplay Tessera uses");
}

static void testMakeCurrentAndProcs() {
    TesseraContextConfig cfg = baseConfig();
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    if (!ctx)
        return;
    TesseraContextInfo info = tesseraDefaultContextInfo();
    tesseraGetContextInfo(ctx, &info);

    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    CHECK(tesseraGetCurrentContext() == ctx);

    auto glGetString = reinterpret_cast<GlGetString>(tesseraGetProcAddress("glGetString"));
    CHECK(glGetString != nullptr);
    if (glGetString)
        CHECK(std::strcmp(reinterpret_cast<const char*>(glGetString(0x1F01)), info.renderer) == 0);
    CHECK(tesseraGetProcAddress("tesseraNotAFunction") == nullptr);
    CHECK(tesseraGetProcAddress(nullptr) == nullptr);
    if (info.versionMajor >= 2)
        CHECK(tesseraGetProcAddress("glGenBuffers") != nullptr);

    EXPECT(tesseraSetSwapInterval(ctx, 1), TESSERA_ERROR_UNSUPPORTED); // pbuffer: nothing to swap
    EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS);                  // harmless no-op

    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    CHECK(tesseraGetCurrentContext() == TESSERA_NULL_HANDLE);
    EXPECT(tesseraSwapBuffers(ctx), TESSERA_ERROR_WRONG_THREAD);

    EXPECT(tesseraMakeCurrent(0xBAD), TESSERA_ERROR_INVALID_HANDLE);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
}

static void testThreadRules() {
    TesseraContextConfig cfg = baseConfig();
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    if (!ctx)
        return;

    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    std::thread([&] {
        EXPECT(tesseraMakeCurrent(ctx), TESSERA_ERROR_WRONG_THREAD);
        EXPECT(tesseraSetSwapInterval(ctx, 1), TESSERA_ERROR_WRONG_THREAD);
        EXPECT(tesseraSwapBuffers(ctx), TESSERA_ERROR_WRONG_THREAD);
    }).join();
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);

    // A thread takes it and exits without releasing; Tessera must release it on that thread's exit, otherwise
    // EGL keeps the context busy (EGL_BAD_ACCESS) and nobody else can use it.
    std::thread([&] { EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS); }).join();
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);

    std::thread([&] { EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS); }).join();

    TesseraContext made = 0;
    std::thread([&] { EXPECT(tesseraCreateContext(&cfg, &made), TESSERA_SUCCESS); }).join();
    std::thread([&] {
        EXPECT(tesseraMakeCurrent(made), TESSERA_SUCCESS);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    }).join();
    std::thread([&] { EXPECT(tesseraDestroyContext(made), TESSERA_SUCCESS); }).join();
}

static void testConfigNegotiation() {
    TesseraContext ctx = 0;

    // Double buffering cannot exist on pbuffer/surfaceless drawables: strict says so, best effort drops it.
    TesseraContextConfig dbl = baseConfig();
    dbl.flags |= TESSERA_FLAG_DOUBLE_BUFFER;
    dbl.fallback = TESSERA_FALLBACK_STRICT;
    EXPECT(tesseraCreateContext(&dbl, &ctx), TESSERA_ERROR_NO_MATCHING_CONFIG);
    CHECK(std::strstr(tesseraGetLastErrorMessage(), "single-buffered") != nullptr);
    dbl.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    EXPECT(tesseraCreateContext(&dbl, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(!(info.flags & TESSERA_FLAG_DOUBLE_BUFFER)); // reported truthfully
        tesseraDestroyContext(ctx);
    }

    TesseraContext none = 0;
    TesseraContextConfig bad = baseConfig();
    bad.fallback = TESSERA_FALLBACK_STRICT;
    bad.versionMajor = 9;
    bad.versionMinor = 9;
    EXPECT(tesseraCreateContext(&bad, &none), TESSERA_ERROR_NO_MATCHING_CONFIG);
    CHECK(none == TESSERA_NULL_HANDLE);

    TesseraContextConfig msaa = baseConfig();
    msaa.samples = 64;
    msaa.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    EXPECT(tesseraCreateContext(&msaa, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.samples < 64);
        std::printf("  best-effort msaa 64 -> %d samples\n", info.samples);
        tesseraDestroyContext(ctx);
    }

    TesseraContextConfig core = baseConfig();
    core.profile = TESSERA_PROFILE_CORE;
    core.versionMajor = 3;
    core.versionMinor = 3;
    core.flags |= TESSERA_FLAG_DEBUG | TESSERA_FLAG_FORWARD_COMPATIBLE;
    core.depthBits = 24;
    core.stencilBits = 8;
    core.fallback = TESSERA_FALLBACK_STRICT;
    const TesseraResult r = tesseraCreateContext(&core, &ctx);
    CHECK(r == TESSERA_SUCCESS || r == TESSERA_ERROR_NO_MATCHING_CONFIG);
    if (r == TESSERA_SUCCESS) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        std::printf("  core request -> GL %d.%d profile=%u flags=0x%x\n", info.versionMajor,
                    info.versionMinor, info.profile, info.flags);
        CHECK(info.profile == TESSERA_PROFILE_CORE);
        CHECK(info.versionMajor > 3 || (info.versionMajor == 3 && info.versionMinor >= 3));
        CHECK(info.flags & TESSERA_FLAG_DEBUG);
        CHECK(info.flags & TESSERA_FLAG_FORWARD_COMPATIBLE);
        CHECK(info.depthBits >= 24 && info.stencilBits >= 8);
        tesseraDestroyContext(ctx);
    } else {
        std::printf("  core 3.3 strict: no match on this driver (%s)\n", tesseraGetLastErrorMessage());
    }

    TesseraContextConfig ladder = baseConfig();
    ladder.profile = TESSERA_PROFILE_CORE;
    if (tesseraCreateContext(&ladder, &ctx) == TESSERA_SUCCESS) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        std::printf("  core ladder -> GL %d.%d\n", info.versionMajor, info.versionMinor);
        CHECK(info.versionMajor >= 3);
        tesseraDestroyContext(ctx);
    }

    // OpenGL ES, default version and ES 3.
    TesseraContextConfig es = esConfig();
    EXPECT(tesseraCreateContext(&es, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        std::printf("  ES default -> %s\n", info.version);
        CHECK(info.api == TESSERA_API_OPENGL_ES);
        CHECK(info.versionMajor >= 2);
        CHECK(info.profile == TESSERA_PROFILE_ANY);
        tesseraDestroyContext(ctx);
    }
    TesseraContextConfig es3 = esConfig(3);
    es3.fallback = TESSERA_FALLBACK_STRICT;
    const TesseraResult e3 = tesseraCreateContext(&es3, &ctx);
    CHECK(e3 == TESSERA_SUCCESS || e3 == TESSERA_ERROR_NO_MATCHING_CONFIG);
    std::printf("  ES 3 strict: %s\n", tesseraResultString(e3));
    if (e3 == TESSERA_SUCCESS) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.api == TESSERA_API_OPENGL_ES && info.versionMajor >= 3);
        tesseraDestroyContext(ctx);
    }

    // Surfaces.
    TesseraContextConfig sl = baseConfig();
    sl.surface = TESSERA_SURFACE_NONE;
    EXPECT(tesseraCreateContext(&sl, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.surface == TESSERA_SURFACE_NONE); // a genuinely drawable-less context
        EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
        tesseraDestroyContext(ctx);
    }
    TesseraContextConfig pb = baseConfig();
    pb.surface = TESSERA_SURFACE_PBUFFER;
    EXPECT(tesseraCreateContext(&pb, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.surface == TESSERA_SURFACE_PBUFFER);
        tesseraDestroyContext(ctx);
    }

    // Optional capabilities: either granted and reported, or refused with a specific code (never silently
    // wrong).
    TesseraContextConfig srgb = baseConfig();
    srgb.flags |= TESSERA_FLAG_SRGB;
    srgb.fallback = TESSERA_FALLBACK_STRICT;
    TesseraResult sr = tesseraCreateContext(&srgb, &ctx);
    CHECK(sr == TESSERA_SUCCESS || sr == TESSERA_ERROR_NO_MATCHING_CONFIG);
    std::printf("  sRGB strict: %s\n", tesseraResultString(sr));
    if (sr == TESSERA_SUCCESS) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.flags & TESSERA_FLAG_SRGB);
        tesseraDestroyContext(ctx);
    }
    TesseraContextConfig fl = baseConfig();
    fl.flags |= TESSERA_FLAG_FLOAT_COLOR;
    sr = tesseraCreateContext(&fl, &ctx);
    CHECK(sr == TESSERA_SUCCESS || sr == TESSERA_ERROR_NO_MATCHING_CONFIG);
    std::printf("  float color: %s\n", tesseraResultString(sr));
    if (sr == TESSERA_SUCCESS) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.flags & TESSERA_FLAG_FLOAT_COLOR);
        tesseraDestroyContext(ctx);
    }
    TesseraContextConfig robust = baseConfig();
    robust.flags |= TESSERA_FLAG_ROBUST_ACCESS | TESSERA_FLAG_RESET_NOTIFICATION;
    robust.fallback = TESSERA_FALLBACK_STRICT;
    sr = tesseraCreateContext(&robust, &ctx);
    CHECK(sr == TESSERA_SUCCESS || sr == TESSERA_ERROR_NO_MATCHING_CONFIG);
    std::printf("  robust access + reset notification strict: %s\n", tesseraResultString(sr));
    if (sr == TESSERA_SUCCESS) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.flags & TESSERA_FLAG_ROBUST_ACCESS);
        tesseraDestroyContext(ctx);
    }
}

static void testSharing() {
    TesseraContextConfig cfg = baseConfig();
    TesseraContext a = 0, b = 0;
    EXPECT(tesseraCreateContext(&cfg, &a), TESSERA_SUCCESS);
    if (!a)
        return;
    cfg.shareWith = a;
    EXPECT(tesseraCreateContext(&cfg, &b), TESSERA_SUCCESS);
    if (!b) {
        tesseraDestroyContext(a);
        return;
    }

    EXPECT(tesseraMakeCurrent(a), TESSERA_SUCCESS);
    auto genTextures = reinterpret_cast<GlGenTextures>(tesseraGetProcAddress("glGenTextures"));
    auto bindTexture = reinterpret_cast<GlBindTexture>(tesseraGetProcAddress("glBindTexture"));
    auto isTexture = reinterpret_cast<GlIsTexture>(tesseraGetProcAddress("glIsTexture"));
    CHECK(genTextures && bindTexture && isTexture);
    unsigned tex = 0;
    if (genTextures && bindTexture && isTexture) {
        genTextures(1, &tex);
        bindTexture(0x0DE1, tex);
        CHECK(tex != 0 && isTexture(tex));
        EXPECT(tesseraMakeCurrent(b), TESSERA_SUCCESS);
        CHECK(tesseraGetCurrentContext() == b);
        CHECK(isTexture(tex)); // visible through the share group
    }
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);

    // Desktop GL and GLES contexts cannot share objects.
    TesseraContextConfig es = esConfig();
    es.shareWith = a;
    TesseraContext cross = 0;
    EXPECT(tesseraCreateContext(&es, &cross), TESSERA_ERROR_INVALID_ARGUMENT);

    EXPECT(tesseraDestroyContext(a), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(b), TESSERA_SUCCESS);
    if (isTexture)
        CHECK(isTexture(tex));
    EXPECT(tesseraDestroyContext(b), TESSERA_SUCCESS); // destroyed while current on this thread
    CHECK(tesseraGetCurrentContext() == TESSERA_NULL_HANDLE);
}

static void testConcurrentCreation() {
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < 15; ++i) {
                TesseraContextConfig cfg = (t % 2) ? esConfig() : baseConfig();
                TesseraContext ctx = 0;
                if (tesseraCreateContext(&cfg, &ctx) != TESSERA_SUCCESS) {
                    ++failures;
                    continue;
                }
                if (tesseraMakeCurrent(ctx) != TESSERA_SUCCESS)
                    ++failures;
                if (tesseraSwapBuffers(ctx) != TESSERA_SUCCESS)
                    ++failures;
                if (tesseraDestroyContext(ctx) != TESSERA_SUCCESS)
                    ++failures;
            }
        });
    for (auto& t : threads)
        t.join();
    CHECK(failures == 0);
}

static void testNoLeaks() {
    TesseraContextConfig cfg = baseConfig();
    auto cycle = [&](int n) {
        for (int i = 0; i < n; ++i) {
            TesseraContext ctx = 0;
            if (tesseraCreateContext(&cfg, &ctx) != TESSERA_SUCCESS) {
                ++g_failures;
                return;
            }
            tesseraMakeCurrent(ctx);
            tesseraDestroyContext(ctx);
        }
    };
    cycle(10); // warm up lazily created process-wide resources
    const int fds0 = openFdCount();
    cycle(200);
    const int fds1 = openFdCount();
    std::printf("  open fds %d -> %d after 200 create/destroy cycles\n", fds0, fds1);
    CHECK(fds1 <= fds0 + 1);
}

static void testTerminateKeepsSharedDisplayAlive() {
    // A library that uses the same surfaceless EGLDisplay as Tessera must be unaffected by tesseraTerminate:
    // EGL displays are shared and not reference counted, so Tessera must never eglTerminate them.
    ForeignContext foreign;
    const bool haveShared = foreign.create(kOpenGlEsApi, true);
    if (haveShared)
        foreign.makeCurrent();

    TesseraContextConfig cfg = baseConfig();
    TesseraContext leaked = 0;
    EXPECT(tesseraCreateContext(&cfg, &leaked), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(leaked), TESSERA_SUCCESS); // leaked while current on this thread

    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(leaked), TESSERA_ERROR_INVALID_HANDLE);
    if (haveShared) {
        foreign.makeCurrent(); // the shared display must still be initialised and usable after our teardown
        CHECK(foreign.isCurrent());
    } else {
        std::printf("  (no shared surfaceless display available, shared-display survival not checked)\n");
    }

    EXPECT(tesseraInit(), TESSERA_SUCCESS);
    TesseraContext again = 0;
    EXPECT(tesseraCreateContext(&cfg, &again), TESSERA_SUCCESS);
    if (again)
        EXPECT(tesseraDestroyContext(again), TESSERA_SUCCESS);
    if (haveShared) {
        foreign.makeCurrent();
        CHECK(foreign.isCurrent());
    }
    foreign.destroy();
}

static void testDeviceSelection() {
    // An explicit GPU choice is honoured or refused, never silently replaced by a different GPU.
    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    const char* oldPlatform = getenv("TESSERA_EGL_PLATFORM");
    const char* oldDevice = getenv("TESSERA_EGL_DEVICE");
    const std::string savedPlatform = oldPlatform ? oldPlatform : "";
    const std::string savedDevice = oldDevice ? oldDevice : "";

    const auto tryWith = [&](const char* platform, const char* device, TesseraResult want,
                             const char* messagePart) {
        EXPECT(tesseraInit(), TESSERA_SUCCESS);
        if (platform)
            setenv("TESSERA_EGL_PLATFORM", platform, 1);
        else
            unsetenv("TESSERA_EGL_PLATFORM");
        if (device)
            setenv("TESSERA_EGL_DEVICE", device, 1);
        else
            unsetenv("TESSERA_EGL_DEVICE");
        EXPECT(tesseraTerminate(),
               TESSERA_SUCCESS); // the backend re-reads the environment when it is rebuilt
        EXPECT(tesseraInit(), TESSERA_SUCCESS);
        TesseraContextConfig cfg = baseConfig();
        TesseraContext ctx = 0;
        const TesseraResult r = tesseraCreateContext(&cfg, &ctx);
        if (want == TESSERA_SUCCESS && r == TESSERA_ERROR_UNSUPPORTED) {
            std::printf("  (%s / %s: not available here: %s)\n", platform ? platform : "-",
                        device ? device : "-", tesseraGetLastErrorMessage());
        } else {
            EXPECT(r, want);
            if (messagePart)
                CHECK(std::strstr(tesseraGetLastErrorMessage(), messagePart) != nullptr);
        }
        if (ctx)
            EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
        EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    };

    // Things that match nothing are refused with a message that names the setting.
    tryWith("device", "99", TESSERA_ERROR_UNSUPPORTED, "TESSERA_EGL_DEVICE=99");
    tryWith("device", "/dev/dri/renderD999", TESSERA_ERROR_UNSUPPORTED,
            "TESSERA_EGL_DEVICE=/dev/dri/renderD999");
    tryWith("device", "0000:ff:1f.7", TESSERA_ERROR_UNSUPPORTED, "TESSERA_EGL_DEVICE=0000:ff:1f.7");
    // A choice combined with a platform that cannot honour it.
    tryWith("surfaceless", "0", TESSERA_ERROR_UNSUPPORTED, "needs TESSERA_EGL_PLATFORM=device");
    // Index 0 works wherever the device platform exists at all.
    tryWith("device", "0", TESSERA_SUCCESS, nullptr);
    // With no platform forced, a choice implies the device platform instead of being ignored by surfaceless.
    tryWith(nullptr, "0", TESSERA_SUCCESS, nullptr);

    if (oldPlatform)
        setenv("TESSERA_EGL_PLATFORM", savedPlatform.c_str(), 1);
    else
        unsetenv("TESSERA_EGL_PLATFORM");
    if (oldDevice)
        setenv("TESSERA_EGL_DEVICE", savedDevice.c_str(), 1);
    else
        unsetenv("TESSERA_EGL_DEVICE");
    EXPECT(tesseraInit(), TESSERA_SUCCESS);
}

static void testHeadlessFallthrough() {
    // The default backend order (GLX first, then EGL) on a machine with no X display: GLX reports UNSUPPORTED
    // and the request is served by EGL's headless platform instead of failing.
    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    const char* oldBackend = getenv("TESSERA_BACKEND");
    const char* oldDisplay = getenv("DISPLAY");
    const std::string savedBackend = oldBackend ? oldBackend : "";
    const std::string savedDisplay = oldDisplay ? oldDisplay : "";
    setenv("TESSERA_BACKEND", "glx,egl", 1);
    unsetenv("DISPLAY");

    EXPECT(tesseraInit(), TESSERA_SUCCESS);
    TesseraContextConfig cfg = baseConfig();
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.surface == TESSERA_SURFACE_PBUFFER); // GLX would have given a hidden window
        EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    }

    // A request GLX could never serve either way, but with a restricted list nothing should serve it.
    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    setenv("TESSERA_BACKEND", "no-such-backend", 1);
    EXPECT(tesseraInit(), TESSERA_SUCCESS);
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_ERROR_UNSUPPORTED);
    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);

    if (oldBackend)
        setenv("TESSERA_BACKEND", savedBackend.c_str(), 1);
    else
        unsetenv("TESSERA_BACKEND");
    if (oldDisplay)
        setenv("DISPLAY", savedDisplay.c_str(), 1);
    EXPECT(tesseraInit(), TESSERA_SUCCESS);
}

static robust::Hooks eglHooks() {
    robust::Hooks h;
    h.base = baseConfig();
    h.nothingCurrentNatively = [] { return ForeignContext{}.nothingCurrent(kOpenGlApi); };
    return h;
}
static void testDeferredDestroy() {
    robust::deferredDestroy(eglHooks());
}
static void testResetStatus() {
    robust::resetStatus(eglHooks());
}
static void testDebugForwarding() {
    robust::debugForwarding(eglHooks());
}
static void testAttributeMatrix() {
    ForeignContext foreign;
    robust::Hooks h = eglHooks();
    if (foreign.create(kOpenGlEsApi, false)) {
        h.armForeign = [&] { foreign.makeCurrent(); };
        h.foreignIntact = [&] { return foreign.isCurrent(); };
    }
    robust::reportMatrix(h);
    foreign.destroy();
}

// ---------------------------------------------------------------- Phase 8

static void testPrewarm() {
    interop::prewarm(eglHooks());
}
static void testAdapterRequests() {
    interop::adapterRequestsRefused(eglHooks());
}
static void testProbe() {
    ForeignContext foreign; // a context from "another library" must survive every probe
    robust::Hooks h = eglHooks();
    if (foreign.create(kOpenGlEsApi, false)) {
        h.armForeign = [&] { foreign.makeCurrent(); };
        h.foreignIntact = [&] { return foreign.isCurrent(); };
    }
    interop::probe(h);
    foreign.destroy();
}

static void testNativeHandles() {
    interop::HandleExpectations e;
    e.kind = TESSERA_NATIVE_EGL;
    e.contextIndex = 1;
    e.mustBeNonZero = {0, 1, 3,
                       4}; // EGLDisplay, EGLContext, EGLConfig, API (the surface is 0 when surfaceless)
    e.nativeCurrent =
        {}; // eglGetCurrentContext is per bound API; the handle identity checks below are enough
    interop::nativeHandles(eglHooks(), e);

    // The EGLDisplay is the process's shared one, usable by other libraries (e.g. for
    // EGL_EXT_image_dma_buf_import).
    TesseraContextConfig cfg = baseConfig();
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraNativeHandles nh = tesseraDefaultNativeHandles();
        EXPECT(tesseraGetNativeHandles(ctx, &nh), TESSERA_SUCCESS);
        EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
        using EglGetCurrentContext = void* (*)();
        using EglGetCurrentDisplay = void* (*)();
        auto cur = reinterpret_cast<EglGetCurrentContext>(tesseraGetProcAddress("eglGetCurrentContext"));
        auto dpy = reinterpret_cast<EglGetCurrentDisplay>(tesseraGetProcAddress("eglGetCurrentDisplay"));
        if (cur && dpy) {
            CHECK(static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(cur())) == nh.handles[1]);
            CHECK(static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(dpy())) == nh.handles[0]);
        }
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
        EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    }
    // Window attach is not offered on EGL.
    TesseraContext none = 0;
    EXPECT(tesseraCreateContextForWindow(&cfg, 0x1234, 0, &none), TESSERA_ERROR_UNSUPPORTED);
}

int main() {
    if (!loadRaw()) {
        std::fprintf(stderr, "test_egl: libEGL not available, skipping\n");
        return 77;
    }
    setenv("TESSERA_BACKEND", "egl", 1); // the GLX backend would otherwise serve these requests

    if (tesseraInit() != TESSERA_SUCCESS) {
        std::fprintf(stderr, "tesseraInit failed: %s\n", tesseraGetLastErrorMessage());
        return 1;
    }
    {
        TesseraContextConfig cfg = baseConfig();
        TesseraContext probe = 0;
        const TesseraResult r = tesseraCreateContext(&cfg, &probe);
        if (r == TESSERA_ERROR_UNSUPPORTED) {
            std::fprintf(stderr, "test_egl: no usable EGL display here, skipping: %s\n",
                         tesseraGetLastErrorMessage());
            return 77;
        }
        if (r != TESSERA_SUCCESS) {
            std::fprintf(stderr, "test_egl: cannot create a basic context: %s (%s)\n", tesseraResultString(r),
                         tesseraGetLastErrorMessage());
            return 1;
        }
        tesseraDestroyContext(probe);
    }

    struct Section {
        const char* name;
        void (*fn)();
    };
    const Section sections[] = {
        {"basic create/info", testBasicCreateAndInfo},
        {"foreign EGL state untouched", testForeignContextUntouched},
        {"make current / procs / swap", testMakeCurrentAndProcs},
        {"thread rules", testThreadRules},
        {"config negotiation", testConfigNegotiation},
        {"sharing", testSharing},
        {"concurrent creation", testConcurrentCreation},
        {"leak check", testNoLeaks},
        {"deferred destruction", testDeferredDestroy},
        {"reset status / recreate", testResetStatus},
        {"debug message forwarding", testDebugForwarding},
        {"attribute matrix", testAttributeMatrix},
        {"pre-warmed contexts", testPrewarm},
        {"probe", testProbe},
        {"GPU requests are refused", testAdapterRequests},
        {"native handles", testNativeHandles},
        {"terminate leaves the shared display alive", testTerminateKeepsSharedDisplayAlive},
        {"device selection", testDeviceSelection},
        {"headless fallthrough (no DISPLAY, default order)", testHeadlessFallthrough},
    };
    for (const Section& sec : sections) {
        std::puts(sec.name);
        sec.fn();
    }

    tesseraTerminate();
    if (g_failures == 0)
        std::puts("test_egl: OK");
    return g_failures == 0 ? 0 : 1;
}
