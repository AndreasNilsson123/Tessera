// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "glx_api.hpp"

#include <atomic>
#include <mutex>

#include <tessera/tessera.h>

#include "core/log.hpp"

namespace tessera::x11 {
namespace {

template <class Fn> bool load(const DynamicLibrary& lib, const char* name, Fn& out) noexcept {
    out = reinterpret_cast<Fn>(lib.symbol(name));
    return out != nullptr;
}

const LinuxApi* loadApi() noexcept {
    try {
        auto* api = new LinuxApi(); // leaked on purpose, see header
        api->x11 = DynamicLibrary::open("libX11.so.6");
        api->gl = DynamicLibrary::open("libGL.so.1");
        if (!api->gl)
            api->gl = DynamicLibrary::open("libGLX.so.0");
        if (!api->x11 || !api->gl) {
            logf(TESSERA_LOG_INFO, "X11 backend unavailable: %s",
                 !api->x11 ? "libX11.so.6 not found" : "libGL.so.1 not found");
            delete api;
            return nullptr;
        }
        X11Fns& x = api->xlib;
        const bool xok = load(api->x11, "XOpenDisplay", x.XOpenDisplay) &&
                         load(api->x11, "XCloseDisplay", x.XCloseDisplay) &&
                         load(api->x11, "XInitThreads", x.XInitThreads) &&
                         load(api->x11, "XDefaultScreen", x.XDefaultScreen) &&
                         load(api->x11, "XRootWindow", x.XRootWindow) &&
                         load(api->x11, "XCreateColormap", x.XCreateColormap) &&
                         load(api->x11, "XFreeColormap", x.XFreeColormap) &&
                         load(api->x11, "XCreateWindow", x.XCreateWindow) &&
                         load(api->x11, "XDestroyWindow", x.XDestroyWindow) &&
                         load(api->x11, "XGetWindowAttributes", x.XGetWindowAttributes) &&
                         load(api->x11, "XVisualIDFromVisual", x.XVisualIDFromVisual) &&
                         load(api->x11, "XFree", x.XFree) && load(api->x11, "XSync", x.XSync) &&
                         load(api->x11, "XSetErrorHandler", x.XSetErrorHandler);
        GlxFns& g = api->glx;
        const bool gok = load(api->gl, "glXQueryExtension", g.glXQueryExtension) &&
                         load(api->gl, "glXQueryVersion", g.glXQueryVersion) &&
                         load(api->gl, "glXQueryExtensionsString", g.glXQueryExtensionsString) &&
                         load(api->gl, "glXChooseFBConfig", g.glXChooseFBConfig) &&
                         load(api->gl, "glXGetFBConfigAttrib", g.glXGetFBConfigAttrib) &&
                         load(api->gl, "glXGetVisualFromFBConfig", g.glXGetVisualFromFBConfig) &&
                         load(api->gl, "glXCreateNewContext", g.glXCreateNewContext) &&
                         load(api->gl, "glXDestroyContext", g.glXDestroyContext) &&
                         load(api->gl, "glXMakeContextCurrent", g.glXMakeContextCurrent) &&
                         load(api->gl, "glXGetCurrentContext", g.glXGetCurrentContext) &&
                         load(api->gl, "glXGetCurrentDisplay", g.glXGetCurrentDisplay) &&
                         load(api->gl, "glXGetCurrentDrawable", g.glXGetCurrentDrawable) &&
                         load(api->gl, "glXGetCurrentReadDrawable", g.glXGetCurrentReadDrawable) &&
                         load(api->gl, "glXSwapBuffers", g.glXSwapBuffers) &&
                         (load(api->gl, "glXGetProcAddressARB", g.glXGetProcAddress) ||
                          load(api->gl, "glXGetProcAddress", g.glXGetProcAddress)) &&
                         load(api->gl, "glXCreatePbuffer", g.glXCreatePbuffer) &&
                         load(api->gl, "glXDestroyPbuffer", g.glXDestroyPbuffer);
        if (!xok || !gok) {
            logf(TESSERA_LOG_ERROR, "X11 backend unavailable: libX11/libGL is missing required entry points");
            delete api;
            return nullptr;
        }
        return api;
    } catch (...) {
        return nullptr;
    }
}

// ---- process-global error handler state (see XErrorScope) -----------------------------------------
std::mutex g_scopeMutex;
std::atomic<Display*> g_scopeDisplay{nullptr};
std::atomic<int> g_scopeError{0};
std::atomic<XErrorHandler> g_scopePrevious{nullptr};
// The last handler seen that was not ours: the host's. The handler slot is process-global and other libraries
// (GTK, JavaFX) push and pop their own handlers without knowing about ours, so a late "restore" by one of
// them can leave scopedHandler installed after our scope ended. The next scope would then record
// scopedHandler as its "previous" and chain to itself forever (found by the JavaFX es2 coexistence test:
// stack overflow, SIGSEGV). So a previous handler that is ours is replaced by the real host handler
// remembered here.
std::atomic<XErrorHandler> g_hostHandler{nullptr};

int scopedHandler(Display* display, XErrorEvent* event) {
    if (display != nullptr && display == g_scopeDisplay.load()) {
        int expected = 0;
        g_scopeError.compare_exchange_strong(expected, static_cast<int>(event->error_code));
        return 0;
    }
    XErrorHandler previous = g_scopePrevious.load();
    if (previous == &scopedHandler)
        previous = g_hostHandler.load(); // never chain to ourselves
    if (previous == &scopedHandler)
        return 0;
    return previous ? previous(display, event) : 0;
}

std::mutex& scopeMutex() {
    return g_scopeMutex;
}

} // namespace

const LinuxApi* linuxApi() noexcept {
    static const LinuxApi* api = loadApi(); // thread-safe one-time init
    return api;
}

XErrorScope::XErrorScope(const LinuxApi& api, Display* display) : api_(api), display_(display) {
    scopeMutex().lock();
    g_scopeError.store(0);
    g_scopeDisplay.store(display);
    previous_ = api_.xlib.XSetErrorHandler(&scopedHandler);
    if (previous_ == &scopedHandler)
        previous_ = g_hostHandler.load(); // a stale install of ours: see g_hostHandler
    else
        g_hostHandler.store(previous_);
    g_scopePrevious.store(previous_);
}

XErrorScope::~XErrorScope() {
    if (display_)
        api_.xlib.XSync(display_, 0); // deliver any pending error while our handler is installed
    api_.xlib.XSetErrorHandler(previous_);
    g_scopeDisplay.store(nullptr);
    g_scopePrevious.store(nullptr);
    scopeMutex().unlock();
}

int XErrorScope::sync() {
    if (display_)
        api_.xlib.XSync(display_, 0);
    return g_scopeError.load();
}

GlxForeignOps::State GlxForeignOps::capture() noexcept {
    State s;
    if (const LinuxApi* api = linuxApi()) {
        s.ctx = api->glx.glXGetCurrentContext();
        s.dpy = api->glx.glXGetCurrentDisplay();
        s.draw = api->glx.glXGetCurrentDrawable();
        s.read = api->glx.glXGetCurrentReadDrawable();
    }
    return s;
}

void GlxForeignOps::restore(const State& s) noexcept {
    const LinuxApi* api = linuxApi();
    if (!api)
        return;
    const GlxFns& g = api->glx;
    if (g.glXGetCurrentContext() == s.ctx && g.glXGetCurrentDrawable() == s.draw &&
        g.glXGetCurrentReadDrawable() == s.read && (s.ctx == nullptr || g.glXGetCurrentDisplay() == s.dpy))
        return; // already as found

    // With nothing to restore, release on whichever display currently holds the context (ours).
    Display* dpy = s.ctx ? s.dpy : g.glXGetCurrentDisplay();
    if (!dpy)
        return;
    // The caller's display/drawable may be gone; a stray X error must not reach Xlib's default handler,
    // which would terminate the host process.
    XErrorScope scope(*api, dpy);
    const Bool ok = g.glXMakeContextCurrent(dpy, s.draw, s.read, s.ctx);
    const int error = scope.sync();
    if (!ok || error != 0) {
        logf(TESSERA_LOG_WARN,
             "could not restore the previous GLX context (X error %d); leaving the thread without one",
             error);
        // Never leave one of OUR contexts current on a thread that did not ask for it.
        if (Display* current = g.glXGetCurrentDisplay(); current && g.glXGetCurrentContext() != s.ctx)
            g.glXMakeContextCurrent(current, 0, 0, nullptr);
    }
}

} // namespace tessera::x11
