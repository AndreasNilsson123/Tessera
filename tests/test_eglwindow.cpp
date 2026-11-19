// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
// EGL window surfaces: Tessera rendering into a window it does not own, through EGL, on X11 and on Wayland.
// The window systems are driven by hand through dlopen (no X11/Wayland development packages are needed): this
// test makes the X window and the wl_surface itself and checks that Tessera renders into them, never destroys
// them, and tears down its own objects in the right order.
//
// Exits 77 (skipped) where there is no X server / no Wayland compositor / no EGL.
#include <tessera/tessera.h>

#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

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

namespace {

using GlClearColor = void (*)(float, float, float, float);
using GlClear = void (*)(unsigned);
using GlReadPixels = void (*)(int, int, int, int, unsigned, unsigned, void*);

TesseraContextConfig windowConfig() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    cfg.flags = TESSERA_FLAG_DOUBLE_BUFFER;
    return cfg;
}

/* Render a colour, read it back from the window's own back buffer, swap a few times (interval 0: a Wayland
 * swap with vsync waits for a frame callback a role-less surface never gets), resize, swap again. */
void renderAndPresent(TesseraContext ctx, bool resize) {
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    auto clearColor = reinterpret_cast<GlClearColor>(tesseraGetProcAddress("glClearColor"));
    auto clear = reinterpret_cast<GlClear>(tesseraGetProcAddress("glClear"));
    auto readPixels = reinterpret_cast<GlReadPixels>(tesseraGetProcAddress("glReadPixels"));
    CHECK(clearColor && clear && readPixels);
    TesseraContextInfo info = tesseraDefaultContextInfo();
    EXPECT(tesseraGetContextInfo(ctx, &info), TESSERA_SUCCESS);
    CHECK(info.surface == TESSERA_SURFACE_WINDOW);
    CHECK(info.flags & TESSERA_FLAG_DOUBLE_BUFFER);

    TesseraSwapControl sc = tesseraDefaultSwapControl();
    EXPECT(tesseraGetSwapControl(ctx, &sc), TESSERA_SUCCESS);
    CHECK(sc.caps & TESSERA_SWAP_CAP_CONTROL);
    CHECK(!(sc.caps & TESSERA_SWAP_CAP_ADAPTIVE)); // EGL has none
    EXPECT(tesseraSetSwapInterval(ctx, 0), TESSERA_SUCCESS);
    EXPECT(tesseraSetSwapInterval(ctx, -1), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraGetSwapControl(ctx, &sc), TESSERA_SUCCESS);
    CHECK(sc.requested == 0);

    if (clearColor && clear && readPixels) {
        clearColor(0.f, 1.f, 0.f, 1.f);
        clear(0x4000); // GL_COLOR_BUFFER_BIT
        unsigned char px[4] = {};
        readPixels(0, 0, 1, 1, 0x1908, 0x1401, px);
        CHECK(px[0] == 0 && px[1] == 255 && px[2] == 0);
    }
    for (int i = 0; i < 3; ++i)
        EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS);

    TesseraNativeHandles nh = tesseraDefaultNativeHandles();
    EXPECT(tesseraGetNativeHandles(ctx, &nh), TESSERA_SUCCESS);
    CHECK(nh.kind == TESSERA_NATIVE_EGL && nh.handles[0] != 0 &&
          nh.handles[2] != 0); // display and a real surface

    EXPECT(tesseraResizeWindow(ctx, 0, 10), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraResizeWindow(ctx, 10, -5), TESSERA_ERROR_INVALID_ARGUMENT);
    if (resize) {
        EXPECT(tesseraResizeWindow(ctx, 96, 80), TESSERA_SUCCESS);
        for (int i = 0; i < 2; ++i)
            EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS);
    }
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    EXPECT(tesseraResizeWindow(ctx, 64, 64), TESSERA_ERROR_WRONG_THREAD); // must be current
}

// ------------------------------------------------------------------------------------------- X11

int runX11() {
    void* x11 = dlopen("libX11.so.6", RTLD_NOW);
    if (!x11 || !getenv("DISPLAY"))
        return 77;
    using OpenFn = void* (*)(const char*);
    using RootFn = unsigned long (*)(void*);
    using SimpleFn = unsigned long (*)(void*, unsigned long, int, int, unsigned, unsigned, unsigned,
                                       unsigned long, unsigned long);
    using MapFn = int (*)(void*, unsigned long);
    using SyncFn = int (*)(void*, int);
    using DestroyFn = int (*)(void*, unsigned long);
    using CloseFn = int (*)(void*);
    auto open = reinterpret_cast<OpenFn>(dlsym(x11, "XOpenDisplay"));
    auto root = reinterpret_cast<RootFn>(dlsym(x11, "XDefaultRootWindow"));
    auto create = reinterpret_cast<SimpleFn>(dlsym(x11, "XCreateSimpleWindow"));
    auto map = reinterpret_cast<MapFn>(dlsym(x11, "XMapWindow"));
    auto sync = reinterpret_cast<SyncFn>(dlsym(x11, "XSync"));
    auto destroy = reinterpret_cast<DestroyFn>(dlsym(x11, "XDestroyWindow"));
    auto close = reinterpret_cast<CloseFn>(dlsym(x11, "XCloseDisplay"));
    if (!open || !root || !create || !map || !sync || !destroy || !close)
        return 77;
    void* dpy = open(nullptr);
    if (!dpy)
        return 77;

    const unsigned long window = create(dpy, root(dpy), 0, 0, 64, 64, 0, 0, 0);
    map(dpy, window);
    sync(dpy, 0);

    TesseraNativeWindow w = tesseraDefaultNativeWindow();
    w.kind = TESSERA_WINDOW_X11;
    w.window = window;
    TesseraContextConfig cfg = windowConfig();
    TesseraContext ctx = 0;
    const TesseraResult r = tesseraCreateContextForNativeWindow(&cfg, &w, 0, &ctx);
    if (r == TESSERA_ERROR_UNSUPPORTED) {
        std::printf("  x11: not available through EGL here: %s\n", tesseraGetLastErrorMessage());
        destroy(dpy, window);
        close(dpy);
        return 77;
    }
    EXPECT(r, TESSERA_SUCCESS);
    if (ctx) {
        renderAndPresent(ctx, false);
        EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
        sync(dpy, 0); // an X error on a destroyed window would abort the process here: the window must still
                      // be alive
        // Tessera never destroys what it borrowed: the same window can be attached again.
        ctx = 0;
        EXPECT(tesseraCreateContextForNativeWindow(&cfg, &w, 0, &ctx), TESSERA_SUCCESS);
        if (ctx)
            EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    }
    // A window id that is not a window: refused, never a crash.
    w.window = 0x7ffffff0ul;
    TesseraContext none = 0;
    const TesseraResult bad = tesseraCreateContextForNativeWindow(&cfg, &w, 0, &none);
    CHECK(bad != TESSERA_SUCCESS && none == TESSERA_NULL_HANDLE);
    destroy(dpy, window);
    close(dpy);
    std::puts("  x11: ok");
    return 0;
}

// ------------------------------------------------------------------------------------------ Wayland

struct WlGlobals {
    uint32_t compositorName = 0;
    uint32_t compositorVersion = 0;
};

void onGlobal(void* data, void*, uint32_t name, const char* iface, uint32_t version) {
    auto* g = static_cast<WlGlobals*>(data);
    if (iface && std::strcmp(iface, "wl_compositor") == 0) {
        g->compositorName = name;
        g->compositorVersion = version < 4 ? version : 4;
    }
}
void onGlobalRemove(void*, void*, uint32_t) {}

int runWayland() {
    const char* socket = getenv("WAYLAND_DISPLAY");
    void* lib = dlopen("libwayland-client.so.0", RTLD_NOW);
    if (!lib || (!socket && !getenv("XDG_RUNTIME_DIR")))
        return 77;
    using ConnectFn = void* (*)(const char*);
    using DisconnectFn = void (*)(void*);
    using RoundtripFn = int (*)(void*);
    using MarshalFn = void* (*)(void*, uint32_t, const void*, uint32_t, uint32_t, ...);
    using AddListenerFn = int (*)(void*, void (**)(void), void*);
    using VersionFn = uint32_t (*)(void*);
    using DestroyProxyFn = void (*)(void*);
    auto connect = reinterpret_cast<ConnectFn>(dlsym(lib, "wl_display_connect"));
    auto disconnect = reinterpret_cast<DisconnectFn>(dlsym(lib, "wl_display_disconnect"));
    auto roundtrip = reinterpret_cast<RoundtripFn>(dlsym(lib, "wl_display_roundtrip"));
    auto marshal = reinterpret_cast<MarshalFn>(dlsym(lib, "wl_proxy_marshal_flags"));
    auto addListener = reinterpret_cast<AddListenerFn>(dlsym(lib, "wl_proxy_add_listener"));
    auto version = reinterpret_cast<VersionFn>(dlsym(lib, "wl_proxy_get_version"));
    auto destroyProxy = reinterpret_cast<DestroyProxyFn>(dlsym(lib, "wl_proxy_destroy"));
    const void* registryIface = dlsym(lib, "wl_registry_interface");
    const void* compositorIface = dlsym(lib, "wl_compositor_interface");
    const void* surfaceIface = dlsym(lib, "wl_surface_interface");
    if (!connect || !disconnect || !roundtrip || !marshal || !addListener || !version || !destroyProxy ||
        !registryIface || !compositorIface || !surfaceIface)
        return 77;

    void* display = connect(nullptr);
    if (!display) {
        std::printf("  wayland: no compositor to connect to\n");
        return 77;
    }
    // wl_display.get_registry (opcode 1), then learn the compositor's global name.
    void* registry = marshal(display, 1, registryIface, version(display), 0, nullptr);
    WlGlobals globals;
    void (*listener[2])(void) = {reinterpret_cast<void (*)()>(onGlobal),
                                 reinterpret_cast<void (*)()>(onGlobalRemove)};
    addListener(registry, listener, &globals);
    roundtrip(display);
    if (globals.compositorName == 0) {
        std::printf("  wayland: the compositor has no wl_compositor\n");
        destroyProxy(registry);
        disconnect(display);
        return 77;
    }
    // wl_registry.bind (opcode 0): name, interface name, version, new_id.
    const char* compositorName = *static_cast<const char* const*>(compositorIface); // wl_interface::name
    void* compositor = marshal(registry, 0, compositorIface, globals.compositorVersion, 0,
                               globals.compositorName, compositorName, globals.compositorVersion, nullptr);
    // wl_compositor.create_surface (opcode 0).
    void* surface = marshal(compositor, 0, surfaceIface, version(compositor), 0, nullptr);
    roundtrip(display);

    TesseraNativeWindow w = tesseraDefaultNativeWindow();
    w.kind = TESSERA_WINDOW_WAYLAND;
    w.window = reinterpret_cast<std::uintptr_t>(surface);
    w.display = reinterpret_cast<std::uintptr_t>(display);
    w.width = 64;
    w.height = 64;
    TesseraContextConfig cfg = windowConfig();
    TesseraContext ctx = 0;
    const TesseraResult r = tesseraCreateContextForNativeWindow(&cfg, &w, 0, &ctx);
    int status = 0;
    if (r == TESSERA_ERROR_UNSUPPORTED) {
        std::printf("  wayland: not available through EGL here: %s\n", tesseraGetLastErrorMessage());
        status = 77;
    } else {
        EXPECT(r, TESSERA_SUCCESS);
        if (ctx) {
            renderAndPresent(ctx, true);
            EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
            // The wl_surface is ours: it must still be usable after the context is gone (a destroyed one
            // would be a protocol error at the next round trip).
            CHECK(roundtrip(display) >= 0);
            ctx = 0;
            EXPECT(tesseraCreateContextForNativeWindow(&cfg, &w, 0, &ctx), TESSERA_SUCCESS); // attach again
            if (ctx)
                EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
        }
        // Bad descriptions are refused up front.
        TesseraNativeWindow bad = w;
        TesseraContext none = 0;
        bad.display = 0;
        EXPECT(tesseraCreateContextForNativeWindow(&cfg, &bad, 0, &none), TESSERA_ERROR_INVALID_ARGUMENT);
        bad = w;
        bad.width = 0;
        EXPECT(tesseraCreateContextForNativeWindow(&cfg, &bad, 0, &none), TESSERA_ERROR_INVALID_ARGUMENT);
        bad = w;
        bad.kind = 99;
        EXPECT(tesseraCreateContextForNativeWindow(&cfg, &bad, 0, &none), TESSERA_ERROR_INVALID_ARGUMENT);
        bad = w;
        bad.kind = TESSERA_WINDOW_COCOA;
        EXPECT(tesseraCreateContextForNativeWindow(&cfg, &bad, 0, &none), TESSERA_ERROR_UNSUPPORTED);
        CHECK(none == TESSERA_NULL_HANDLE);
        std::puts("  wayland: ok");
    }
    marshal(surface, 0, nullptr, version(surface), 1 /*WL_MARSHAL_FLAG_DESTROY*/); // wl_surface.destroy
    destroyProxy(compositor);
    destroyProxy(registry);
    roundtrip(display);
    disconnect(display);
    return status;
}

} // namespace

int main(int argc, char** argv) {
    setenv("TESSERA_BACKEND", "egl", 1); // GLX would take an X11 window before EGL gets a look
    if (tesseraInit() != TESSERA_SUCCESS) {
        std::fprintf(stderr, "tesseraInit failed: %s\n", tesseraGetLastErrorMessage());
        return 1;
    }
    const bool onlyX11 = argc > 1 && std::strcmp(argv[1], "x11") == 0;
    const bool onlyWayland = argc > 1 && std::strcmp(argv[1], "wayland") == 0;
    int skipped = 0, ran = 0;
    if (!onlyWayland) {
        std::puts("X11 window through EGL");
        const int s = runX11();
        (s == 77 ? skipped : ran) += 1;
    }
    if (!onlyX11) {
        std::puts("Wayland surface through EGL");
        const int s = runWayland();
        (s == 77 ? skipped : ran) += 1;
    }
    tesseraTerminate();
    if (g_failures) {
        std::fprintf(stderr, "test_eglwindow: %d failure(s)\n", g_failures);
        return 1;
    }
    if (ran == 0) {
        std::puts(
            "test_eglwindow: nothing could run here (no X server / compositor / EGL platform): skipped");
        return 77;
    }
    std::printf("test_eglwindow: OK (%d ran, %d skipped)\n", ran, skipped);
    return 0;
}
