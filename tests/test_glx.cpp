// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
// Integration tests for the GLX backend, through the public C API only. Needs an X server with GLX (a
// software renderer such as Mesa llvmpipe under Xvfb is fine). A raw GLX context on its own X connection
// plays the role of "another library in the same process" (JavaFX/GTK, AWT, LWJGL...) whose current context
// Tessera must not disturb. Exits 77 (ctest: skipped) when no display/GLX is available.
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

#include "platform/x11/x11_types.hpp"

using namespace tessera::x11;

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

#include "foreign_other_api.hpp"
#include "interop_common.hpp"
#include "robustness_common.hpp"

using GlGetString = const unsigned char* (*)(unsigned);
using GlGenTextures = void (*)(int, unsigned*);
using GlIsTexture = unsigned char (*)(unsigned);
using GlBindTexture = void (*)(unsigned, unsigned);

// ---------------------------------------------------------------- raw libX11 / libGL for the "other library"

static X11Fns g_x;
static GlxFns g_g;

template <class Fn> static bool bind(void* lib, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(dlsym(lib, name));
    return out != nullptr;
}

static bool loadRaw() {
    void* x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    void* gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!x11 || !gl)
        return false;
    return bind(x11, "XOpenDisplay", g_x.XOpenDisplay) && bind(x11, "XCloseDisplay", g_x.XCloseDisplay) &&
           bind(x11, "XInitThreads", g_x.XInitThreads) && bind(x11, "XDefaultScreen", g_x.XDefaultScreen) &&
           bind(x11, "XRootWindow", g_x.XRootWindow) && bind(x11, "XCreateColormap", g_x.XCreateColormap) &&
           bind(x11, "XFreeColormap", g_x.XFreeColormap) && bind(x11, "XCreateWindow", g_x.XCreateWindow) &&
           bind(x11, "XDestroyWindow", g_x.XDestroyWindow) && bind(x11, "XFree", g_x.XFree) &&
           bind(x11, "XSync", g_x.XSync) && bind(x11, "XSetErrorHandler", g_x.XSetErrorHandler) &&
           bind(gl, "glXChooseFBConfig", g_g.glXChooseFBConfig) &&
           bind(gl, "glXGetVisualFromFBConfig", g_g.glXGetVisualFromFBConfig) &&
           bind(gl, "glXCreateNewContext", g_g.glXCreateNewContext) &&
           bind(gl, "glXDestroyContext", g_g.glXDestroyContext) &&
           bind(gl, "glXMakeContextCurrent", g_g.glXMakeContextCurrent) &&
           bind(gl, "glXGetCurrentContext", g_g.glXGetCurrentContext) &&
           bind(gl, "glXGetCurrentDrawable", g_g.glXGetCurrentDrawable);
}

struct ForeignContext {
    Display* dpy = nullptr;
    Window win = 0;
    Colormap cmap = 0;
    GLXContext ctx = nullptr;

    bool create() {
        dpy = g_x.XOpenDisplay(nullptr);
        if (!dpy)
            return false;
        const int screen = g_x.XDefaultScreen(dpy);
        const int attribs[] = {kGlxXRenderable,
                               1,
                               kGlxDrawableType,
                               kGlxWindowBit,
                               kGlxRenderType,
                               kGlxRgbaBit,
                               kGlxDoubleBuffer,
                               1,
                               kGlxRedSize,
                               8,
                               kGlxGreenSize,
                               8,
                               kGlxBlueSize,
                               8,
                               0};
        int n = 0;
        GLXFBConfig* list = g_g.glXChooseFBConfig(dpy, screen, attribs, &n);
        if (!list || n == 0)
            return false;
        GLXFBConfig fb = list[0];
        g_x.XFree(list);
        XVisualInfo* vi = g_g.glXGetVisualFromFBConfig(dpy, fb);
        if (!vi)
            return false;
        const Window root = g_x.XRootWindow(dpy, screen);
        cmap = g_x.XCreateColormap(dpy, root, vi->visual, kAllocNone);
        XSetWindowAttributes a = {};
        a.colormap = cmap;
        a.override_redirect = 1;
        win = g_x.XCreateWindow(dpy, root, 0, 0, 1, 1, 0, vi->depth, kInputOutput, vi->visual,
                                kCWColormap | kCWOverrideRedirect, &a);
        g_x.XFree(vi);
        ctx = g_g.glXCreateNewContext(dpy, fb, kGlxRgbaType, nullptr, 1);
        return win != 0 && ctx != nullptr;
    }
    void makeCurrent() const {
        g_g.glXMakeContextCurrent(dpy, win, win, ctx);
    }
    bool isCurrent() const {
        return g_g.glXGetCurrentContext() == ctx && g_g.glXGetCurrentDrawable() == win;
    }
    void destroy() {
        if (!dpy)
            return;
        if (g_g.glXGetCurrentContext() == ctx)
            g_g.glXMakeContextCurrent(dpy, 0, 0, nullptr);
        if (ctx)
            g_g.glXDestroyContext(dpy, ctx);
        if (win)
            g_x.XDestroyWindow(dpy, win);
        if (cmap)
            g_x.XFreeColormap(dpy, cmap);
        g_x.XCloseDisplay(dpy);
        *this = ForeignContext{};
    }
};

static TesseraContextConfig baseConfig() {
    TesseraContextConfig c = tesseraDefaultContextConfig();
    c.api = TESSERA_API_OPENGL;
    c.flags = TESSERA_FLAG_DOUBLE_BUFFER;
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
                "d=%d s=%d samples=%d\n",
                info.vendor, info.renderer, info.version, info.api, info.versionMajor, info.versionMinor,
                info.profile, info.flags, info.redBits, info.greenBits, info.blueBits, info.alphaBits,
                info.depthBits, info.stencilBits, info.samples);
    CHECK(info.api == TESSERA_API_OPENGL);
    CHECK(info.profile == TESSERA_PROFILE_COMPATIBILITY);
    CHECK(info.versionMajor >= 1);
    CHECK(info.renderer[0] != '\0' && info.version[0] != '\0');
    CHECK(info.redBits >= 8 && info.greenBits >= 8 && info.blueBits >= 8);
    CHECK(info.flags & TESSERA_FLAG_DOUBLE_BUFFER);
    CHECK(info.surface == TESSERA_SURFACE_HIDDEN_WINDOW);

    CHECK(g_g.glXGetCurrentContext() == nullptr); // creating leaves nothing current
    CHECK(tesseraGetCurrentContext() == TESSERA_NULL_HANDLE);

    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_ERROR_INVALID_HANDLE);
}

static void testForeignContextUntouched() {
    ForeignContext foreign;
    CHECK(foreign.create());
    foreign.makeCurrent();
    CHECK(foreign.isCurrent());

    TesseraContextConfig cfg = baseConfig();
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    CHECK(foreign.isCurrent()); // successful create

    TesseraContextConfig core = baseConfig();
    core.profile = TESSERA_PROFILE_CORE;
    core.versionMajor = 3;
    core.versionMinor = 3;
    core.fallback = TESSERA_FALLBACK_STRICT;
    TesseraContext coreCtx = 0;
    const TesseraResult r = tesseraCreateContext(&core, &coreCtx);
    CHECK(r == TESSERA_SUCCESS || r == TESSERA_ERROR_NO_MATCHING_CONFIG);
    CHECK(foreign.isCurrent());

    TesseraContextConfig absurd = baseConfig();
    absurd.samples = 64;
    absurd.fallback = TESSERA_FALLBACK_STRICT;
    TesseraContext none = 0;
    EXPECT(tesseraCreateContext(&absurd, &none), TESSERA_ERROR_NO_MATCHING_CONFIG);
    CHECK(foreign.isCurrent()); // failing create

    EXPECT(tesseraSwapBuffers(ctx), TESSERA_ERROR_WRONG_THREAD); // not current: refused, foreign untouched
    CHECK(foreign.isCurrent());

    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    if (coreCtx)
        EXPECT(tesseraDestroyContext(coreCtx), TESSERA_SUCCESS);
    CHECK(foreign.isCurrent()); // destroy

    // Ours made current, then the other library takes the thread back: our release must not drop it.
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    CHECK(!foreign.isCurrent());
    foreign.makeCurrent();
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    CHECK(foreign.isCurrent());

    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS); // nothing of ours current: no-op
    CHECK(foreign.isCurrent());

    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    foreign.makeCurrent();
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS); // destroying a displaced context
    CHECK(foreign.isCurrent());

    foreign.destroy();
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
    CHECK(g_g.glXGetCurrentContext() != nullptr);

    auto glGetString = reinterpret_cast<GlGetString>(tesseraGetProcAddress("glGetString"));
    CHECK(glGetString != nullptr);
    if (glGetString)
        CHECK(std::strcmp(reinterpret_cast<const char*>(glGetString(0x1F01)), info.renderer) == 0);
    // Not a "gl*" name, so even GLVND's dynamic stub generation does not apply. (A bogus gl* name may legally
    // resolve to a no-op stub under GLX; consult the extension strings for availability.)
    CHECK(tesseraGetProcAddress("tesseraNotAFunction") == nullptr);
    CHECK(tesseraGetProcAddress(nullptr) == nullptr);
    if (info.versionMajor >= 2)
        CHECK(tesseraGetProcAddress("glGenBuffers") != nullptr);

    const TesseraResult si = tesseraSetSwapInterval(ctx, 0);
    CHECK(si == TESSERA_SUCCESS || si == TESSERA_ERROR_UNSUPPORTED);
    std::printf("  swap interval 0: %s\n", tesseraResultString(si));
    EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS);

    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    CHECK(g_g.glXGetCurrentContext() == nullptr);
    CHECK(tesseraGetCurrentContext() == TESSERA_NULL_HANDLE);
    EXPECT(tesseraSetSwapInterval(ctx, 1), TESSERA_ERROR_WRONG_THREAD);

    EXPECT(tesseraMakeCurrent(0xBAD), TESSERA_ERROR_INVALID_HANDLE);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    EXPECT(tesseraSwapBuffers(ctx), TESSERA_ERROR_INVALID_HANDLE);
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

    // A thread takes it and exits without releasing; the dead thread's claim must not strand the context.
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
    TesseraContextConfig bad = baseConfig();
    bad.fallback = TESSERA_FALLBACK_STRICT;
    bad.versionMajor = 9;
    bad.versionMinor = 9;
    TesseraContext none = 0;
    EXPECT(tesseraCreateContext(&bad, &none), TESSERA_ERROR_NO_MATCHING_CONFIG);
    CHECK(none == TESSERA_NULL_HANDLE);

    TesseraContextConfig msaa = baseConfig();
    msaa.samples = 64;
    msaa.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraContext ctx = 0;
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

    TesseraContextConfig es = baseConfig();
    es.api = TESSERA_API_OPENGL_ES;
    const TesseraResult er = tesseraCreateContext(&es, &ctx);
    CHECK(er == TESSERA_SUCCESS || er == TESSERA_ERROR_UNSUPPORTED || er == TESSERA_ERROR_NO_MATCHING_CONFIG);
    std::printf("  OpenGL ES request: %s\n", tesseraResultString(er));
    if (er == TESSERA_SUCCESS) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.api == TESSERA_API_OPENGL_ES);
        tesseraDestroyContext(ctx);
    }

    TesseraContextConfig pb = baseConfig();
    pb.surface = TESSERA_SURFACE_PBUFFER;
    const TesseraResult pr = tesseraCreateContext(&pb, &ctx);
    CHECK(pr == TESSERA_SUCCESS || pr == TESSERA_ERROR_UNSUPPORTED || pr == TESSERA_ERROR_NO_MATCHING_CONFIG);
    std::printf("  pbuffer request: %s\n", tesseraResultString(pr));
    if (pr == TESSERA_SUCCESS) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.surface == TESSERA_SURFACE_PBUFFER);
        EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
        EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
        tesseraDestroyContext(ctx);
    }

    TesseraContextConfig none_ = baseConfig();
    none_.surface = TESSERA_SURFACE_NONE;
    EXPECT(tesseraCreateContext(&none_, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.surface == TESSERA_SURFACE_NONE);
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

    EXPECT(tesseraDestroyContext(a), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(b), TESSERA_SUCCESS);
    if (isTexture)
        CHECK(isTexture(tex));
    EXPECT(tesseraDestroyContext(b), TESSERA_SUCCESS); // destroyed while current on this thread
    CHECK(g_g.glXGetCurrentContext() == nullptr);
    CHECK(tesseraGetCurrentContext() == TESSERA_NULL_HANDLE);
}

static void testConcurrentCreation() {
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 15; ++i) {
                TesseraContextConfig cfg = baseConfig();
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

static int g_handlerCalls = 0;
static int hostErrorHandler(Display*, XErrorEvent*) {
    ++g_handlerCalls;
    return 0;
}

static void testNoLeaksAndHandlerRestored() {
    // The host application's X error handler must be exactly as it was after we have used the process-wide
    // slot.
    g_x.XSetErrorHandler(&hostErrorHandler);

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

    // Reading the slot means writing it; put the host handler back after looking.
    XErrorHandler current = g_x.XSetErrorHandler(&hostErrorHandler);
    CHECK(current == &hostErrorHandler);
    CHECK(g_handlerCalls == 0);
    g_x.XSetErrorHandler(nullptr);
}

static void testTerminateAndReinit() {
    TesseraContextConfig cfg = baseConfig();
    TesseraContext leaked = 0;
    EXPECT(tesseraCreateContext(&cfg, &leaked), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(leaked), TESSERA_SUCCESS); // leaked while current on this thread

    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    CHECK(g_g.glXGetCurrentContext() == nullptr);
    EXPECT(tesseraDestroyContext(leaked), TESSERA_ERROR_INVALID_HANDLE);

    EXPECT(tesseraInit(), TESSERA_SUCCESS); // the private X connection is reopened
    TesseraContext again = 0;
    EXPECT(tesseraCreateContext(&cfg, &again), TESSERA_SUCCESS);
    if (again)
        EXPECT(tesseraDestroyContext(again), TESSERA_SUCCESS);
}

// A thread still has a Tessera context current when tesseraTerminate runs, and exits afterwards. Closing the
// library's X connection at terminate used to let Mesa unload its driver under that thread; its exit then
// called a destructor in unmapped code (SIGSEGV in __nptl_deallocate_tsd). Found by the Java shutdown test
// (java_shutdown, 15/15 failures on Mesa D3D12 before the fix, 0/15 after). This native version does NOT
// reproduce the crash without the fix, so it only guards the scenario's basic behaviour.
static void testTerminateWithBusyThread() {
    TesseraContextConfig cfg = baseConfig();
    std::atomic<bool> current{false}, release{false};
    std::thread busy([&] {
        TesseraContext c = 0;
        EXPECT(tesseraCreateContext(&cfg, &c), TESSERA_SUCCESS);
        if (c)
            EXPECT(tesseraMakeCurrent(c), TESSERA_SUCCESS);
        current = true;
        while (!release.load())
            std::this_thread::yield();
        // exits with the context still current: the library's exit hook and the driver's thread-exit
        // destructors both run
    });
    while (!current.load())
        std::this_thread::yield();
    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    release = true;
    busy.join(); // would crash the whole process before the fix (the library must not re-initialise first:
                 // that reloads the driver)
    EXPECT(tesseraInit(), TESSERA_SUCCESS);
    TesseraContext again = 0;
    EXPECT(tesseraCreateContext(&cfg, &again), TESSERA_SUCCESS);
    if (again)
        EXPECT(tesseraDestroyContext(again), TESSERA_SUCCESS);
}

// SURFACE_NONE: the context is made current without a drawable where the driver allows it (no window is
// created at all), and with a hidden window where it does not. Either way it must be fully usable for
// offscreen rendering.
static void testSurfaceNone() {
    TesseraContextConfig cfg = baseConfig();
    cfg.surface = TESSERA_SURFACE_NONE;
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    if (!ctx)
        return;
    TesseraContextInfo info = tesseraDefaultContextInfo();
    EXPECT(tesseraGetContextInfo(ctx, &info), TESSERA_SUCCESS);
    CHECK(info.surface == TESSERA_SURFACE_NONE);
    TesseraNativeHandles nh = tesseraDefaultNativeHandles();
    EXPECT(tesseraGetNativeHandles(ctx, &nh), TESSERA_SUCCESS);
    const bool drawableless = nh.handles[2] == 0 && nh.handles[4] == 0;
    const bool windowed = nh.handles[2] != 0 && nh.handles[2] == nh.handles[4];
    CHECK(drawableless || windowed); // never half of each
    std::printf("  SURFACE_NONE here: %s\n",
                drawableless ? "no drawable at all" : "hidden window (this driver needs one)");

    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    CHECK(g_g.glXGetCurrentContext() != nullptr);
    CHECK(g_g.glXGetCurrentDrawable() == nh.handles[2]);
    // Offscreen rendering works: clear a renderbuffer-backed framebuffer and read the pixel back.
    using GenFn = void (*)(int, unsigned*);
    using BindFn = void (*)(unsigned, unsigned);
    using StorageFn = void (*)(unsigned, unsigned, int, int);
    using AttachFn = void (*)(unsigned, unsigned, unsigned, unsigned);
    using ClearColorFn = void (*)(float, float, float, float);
    using ClearFn = void (*)(unsigned);
    using ReadFn = void (*)(int, int, int, int, unsigned, unsigned, void*);
    auto genFbo = reinterpret_cast<GenFn>(tesseraGetProcAddress("glGenFramebuffers"));
    auto genRbo = reinterpret_cast<GenFn>(tesseraGetProcAddress("glGenRenderbuffers"));
    auto bindFbo = reinterpret_cast<BindFn>(tesseraGetProcAddress("glBindFramebuffer"));
    auto bindRbo = reinterpret_cast<BindFn>(tesseraGetProcAddress("glBindRenderbuffer"));
    auto storage = reinterpret_cast<StorageFn>(tesseraGetProcAddress("glRenderbufferStorage"));
    auto attach = reinterpret_cast<AttachFn>(tesseraGetProcAddress("glFramebufferRenderbuffer"));
    auto clearColor = reinterpret_cast<ClearColorFn>(tesseraGetProcAddress("glClearColor"));
    auto clear = reinterpret_cast<ClearFn>(tesseraGetProcAddress("glClear"));
    auto readPixels = reinterpret_cast<ReadFn>(tesseraGetProcAddress("glReadPixels"));
    CHECK(genFbo && genRbo && bindFbo && bindRbo && storage && attach && clearColor && clear && readPixels);
    if (genFbo && genRbo && bindFbo && bindRbo && storage && attach && clearColor && clear && readPixels) {
        unsigned fbo = 0, rbo = 0;
        genFbo(1, &fbo);
        genRbo(1, &rbo);
        bindRbo(0x8D41, rbo);                // GL_RENDERBUFFER
        storage(0x8D41, 0x8058, 8, 8);       // GL_RGBA8
        bindFbo(0x8D40, fbo);                // GL_FRAMEBUFFER
        attach(0x8D40, 0x8CE0, 0x8D41, rbo); // GL_COLOR_ATTACHMENT0
        clearColor(0.f, 0.f, 1.f, 1.f);
        clear(0x4000);
        unsigned char px[4] = {};
        readPixels(0, 0, 1, 1, 0x1908, 0x1401, px);
        CHECK(px[0] == 0 && px[1] == 0 && px[2] == 255 && px[3] == 255);
    }
    EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS); // nothing to present is not an error
    const TesseraResult si = tesseraSetSwapInterval(ctx, 1);
    CHECK(drawableless ? si == TESSERA_ERROR_UNSUPPORTED
                       : (si == TESSERA_SUCCESS || si == TESSERA_ERROR_UNSUPPORTED));
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    CHECK(g_g.glXGetCurrentContext() == nullptr);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);

    // Sharing between a drawable-less and a normal context.
    TesseraContext normal = 0;
    TesseraContextConfig plain = baseConfig();
    plain.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    EXPECT(tesseraCreateContext(&plain, &normal), TESSERA_SUCCESS);
    cfg.shareWith = normal;
    TesseraContext child = 0;
    EXPECT(tesseraCreateContext(&cfg, &child), TESSERA_SUCCESS);
    if (child)
        EXPECT(tesseraDestroyContext(child), TESSERA_SUCCESS);
    if (normal)
        EXPECT(tesseraDestroyContext(normal), TESSERA_SUCCESS);
}

// The host's context belongs to the OTHER windowing API (see foreign_other_api.hpp): Tessera must neither
// fail nor disturb it.
static void testForeignEglContext() {
    foreign::EglCtx host;
    if (!host.create()) {
        std::printf("  (no Egl context available to play the other library: skipped)\n");
        return;
    }
    TesseraContextConfig cfg = baseConfig();
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    host.makeCurrent();
    CHECK(host.isCurrent());

    TesseraContext a = 0, b = 0;
    EXPECT(tesseraCreateContext(&cfg, &a), TESSERA_SUCCESS);
    CHECK(host.isCurrent()); // creating leaves the host's context current
    TesseraContextConfig pbuffer = cfg;
    pbuffer.surface = TESSERA_SURFACE_PBUFFER;
    EXPECT(tesseraCreateContext(&pbuffer, &b), TESSERA_SUCCESS);
    CHECK(host.isCurrent());
    TesseraContextInfo info = tesseraDefaultContextInfo();
    EXPECT(tesseraGetContextInfo(a, &info), TESSERA_SUCCESS);
    CHECK(host.isCurrent());

    // Making ours current replaces the host's (the documented contract), and must work instead of failing.
    EXPECT(tesseraMakeCurrent(a), TESSERA_SUCCESS);
    CHECK(!host.isCurrent());
    EXPECT(tesseraMakeCurrent(b), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    CHECK(host.nothingCurrent());

    // Destroying and probing while the host's context is current leaves it alone.
    host.makeCurrent();
    EXPECT(tesseraDestroyContext(a), TESSERA_SUCCESS);
    CHECK(host.isCurrent());
    TesseraProbeReport rep = tesseraDefaultProbeReport();
    EXPECT(tesseraProbe(&cfg, &rep), TESSERA_SUCCESS);
    CHECK(host.isCurrent());
    EXPECT(tesseraDestroyContext(b), TESSERA_SUCCESS);
    CHECK(host.isCurrent());
    host.destroy();
}

static robust::Hooks glxHooks() {
    robust::Hooks h;
    h.base = baseConfig();
    h.nothingCurrentNatively = [] { return g_g.glXGetCurrentContext() == nullptr; };
    return h;
}
static void testDeferredDestroy() {
    robust::deferredDestroy(glxHooks());
}
static void testResetStatus() {
    robust::resetStatus(glxHooks());
}
static void testDebugForwarding() {
    robust::debugForwarding(glxHooks());
}
static void testAttributeMatrix() {
    ForeignContext foreign;
    robust::Hooks h = glxHooks();
    if (foreign.create()) {
        h.armForeign = [&] { foreign.makeCurrent(); };
        h.foreignIntact = [&] { return foreign.isCurrent(); };
    }
    robust::reportMatrix(h);
    foreign.destroy();
}

// ---------------------------------------------------------------- Phase 8

static void testPrewarm() {
    interop::prewarm(glxHooks());
}
static void testTopUp() {
    interop::topUp(glxHooks());
}
static void testAdapterRequests() {
    interop::adapterRequestsRefused(glxHooks());
}
static void testProbe() {
    ForeignContext foreign; // a context from "another library" must survive every probe
    robust::Hooks h = glxHooks();
    if (foreign.create()) {
        h.armForeign = [&] { foreign.makeCurrent(); };
        h.foreignIntact = [&] { return foreign.isCurrent(); };
    }
    interop::probe(h);
    foreign.destroy();
}

static void testNativeHandles() {
    interop::HandleExpectations e;
    e.kind = TESSERA_NATIVE_GLX;
    e.contextIndex = 1;
    e.mustBeNonZero = {0, 1, 2, 3, 4}; // Display*, GLXContext, drawable, FBConfig, hidden window
    e.nativeCurrent = [] {
        return static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(g_g.glXGetCurrentContext()));
    };
    interop::nativeHandles(glxHooks(), e);
}

static void testWindowAttach() {
    ForeignContext
        owner; // supplies a real, mapped-or-not X window with a GLX-capable visual that is NOT ours
    if (!owner.create()) {
        std::printf("  (no GLX-capable window available, skipped)\n");
        return;
    }
    TesseraContextConfig cfg = baseConfig();
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContextForWindow(&cfg, static_cast<uint64_t>(owner.win), 0, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        EXPECT(tesseraGetContextInfo(ctx, &info), TESSERA_SUCCESS);
        CHECK(info.surface == TESSERA_SURFACE_WINDOW);
        CHECK(info.flags & TESSERA_FLAG_DOUBLE_BUFFER);
        TesseraNativeHandles nh = tesseraDefaultNativeHandles();
        EXPECT(tesseraGetNativeHandles(ctx, &nh), TESSERA_SUCCESS);
        CHECK(nh.handles[2] == static_cast<uint64_t>(owner.win) &&
              nh.handles[4] == static_cast<uint64_t>(owner.win));

        EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
        CHECK(g_g.glXGetCurrentDrawable() == owner.win);
        using GlClearColor = void (*)(float, float, float, float);
        using GlClear = void (*)(unsigned);
        using GlReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void*);
        auto clearColor = reinterpret_cast<GlClearColor>(tesseraGetProcAddress("glClearColor"));
        auto clear = reinterpret_cast<GlClear>(tesseraGetProcAddress("glClear"));
        auto readPixels = reinterpret_cast<GlReadPixels>(tesseraGetProcAddress("glReadPixels"));
        if (clearColor && clear && readPixels) {
            clearColor(0.f, 1.f, 0.f, 1.f);
            clear(0x4000);
            unsigned char px[4] = {};
            readPixels(0, 0, 1, 1, 0x1908, 0x1401, px);
            CHECK(px[0] == 0 && px[1] == 255 && px[2] == 0);
        }
        EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
        EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);

        // The window is the caller's: it must survive the context, and the caller's own context must still
        // work on it.
        owner.makeCurrent();
        CHECK(owner.isCurrent());
        g_g.glXMakeContextCurrent(owner.dpy, 0, 0, nullptr);
    }

    // A window that does not exist.
    TesseraContext none = 0;
    EXPECT(tesseraCreateContextForWindow(&cfg, 0x7fffffffu, 0, &none), TESSERA_ERROR_INVALID_ARGUMENT);
    CHECK(none == TESSERA_NULL_HANDLE);

    // A window destroyed while a context renders into it: an error, not the process being terminated by Xlib.
    ForeignContext doomed;
    if (doomed.create()) {
        TesseraContext c = 0;
        EXPECT(tesseraCreateContextForWindow(&cfg, static_cast<uint64_t>(doomed.win), 0, &c),
               TESSERA_SUCCESS);
        if (c) {
            EXPECT(tesseraMakeCurrent(c), TESSERA_SUCCESS);
            g_g.glXMakeContextCurrent(doomed.dpy, 0, 0,
                                      nullptr); // (this also unbinds ours: same thread, one current context)
            g_x.XDestroyWindow(doomed.dpy, doomed.win);
            g_x.XSync(doomed.dpy, 0);
            doomed.win = 0;
            const TesseraResult swap = tesseraSwapBuffers(c);
            CHECK(swap == TESSERA_ERROR_PLATFORM || swap == TESSERA_ERROR_WRONG_THREAD);
            tesseraMakeCurrent(TESSERA_NULL_HANDLE);
            EXPECT(tesseraDestroyContext(c), TESSERA_SUCCESS);
        }
        doomed.destroy();
    }

    // Argument handling and unsupported flags.
    EXPECT(tesseraCreateContextForWindow(&cfg, 0, 0, &none), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraCreateContextForWindow(&cfg, static_cast<uint64_t>(owner.win), 0x80, &none),
           TESSERA_ERROR_INVALID_ARGUMENT);
    owner.destroy();
}

int main() {
    if (!loadRaw()) {
        std::fprintf(stderr, "test_glx: libX11/libGL not available, skipping\n");
        return 77;
    }
    setenv("TESSERA_BACKEND", "glx",
           1); // with EGL registered too, a missing display would fall through to it
    // What GLFW, SDL, GTK and AWT all do before touching Xlib; the "other library" owns its connection.
    g_x.XInitThreads();

    if (tesseraInit() != TESSERA_SUCCESS) {
        std::fprintf(stderr, "tesseraInit failed: %s\n", tesseraGetLastErrorMessage());
        return 1;
    }
    {
        TesseraContextConfig cfg = baseConfig();
        TesseraContext probe = 0;
        const TesseraResult r = tesseraCreateContext(&cfg, &probe);
        if (r == TESSERA_ERROR_UNSUPPORTED) {
            std::fprintf(stderr, "test_glx: no usable X display / GLX here, skipping: %s\n",
                         tesseraGetLastErrorMessage());
            return 77;
        }
        if (r != TESSERA_SUCCESS) {
            std::fprintf(stderr, "test_glx: cannot create a basic context: %s (%s)\n", tesseraResultString(r),
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
        {"foreign context untouched", testForeignContextUntouched},
        {"make current / procs / swap", testMakeCurrentAndProcs},
        {"thread rules", testThreadRules},
        {"config negotiation", testConfigNegotiation},
        {"sharing", testSharing},
        {"concurrent creation", testConcurrentCreation},
        {"leak check / error handler restored", testNoLeaksAndHandlerRestored},
        {"deferred destruction", testDeferredDestroy},
        {"reset status / recreate", testResetStatus},
        {"debug message forwarding", testDebugForwarding},
        {"attribute matrix", testAttributeMatrix},
        {"pre-warmed contexts", testPrewarm},
        {"keeping the pool full", testTopUp},
        {"probe", testProbe},
        {"GPU requests are refused", testAdapterRequests},
        {"native handles", testNativeHandles},
        {"window attach", testWindowAttach},
        {"terminate / re-init", testTerminateAndReinit},
        {"terminate with a busy thread", testTerminateWithBusyThread},
        {"surface none", testSurfaceNone},
        {"host context from EGL", testForeignEglContext},
    };
    for (const Section& sec : sections) {
        std::puts(sec.name);
        sec.fn();
    }

    tesseraTerminate();
    if (g_failures == 0)
        std::puts("test_glx: OK");
    return g_failures == 0 ? 0 : 1;
}
