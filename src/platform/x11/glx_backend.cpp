// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "glx_backend.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/boundary.hpp"
#include "core/log.hpp"
#include "glx_api.hpp"
#include "platform/common/arb_context.hpp"
#include "platform/common/context_priority.hpp"
#include "platform/common/create_errors.hpp"
#include "platform/common/cross_api.hpp"
#include "platform/common/gl_debug.hpp"
#include "platform/common/gl_query.hpp"
#include "platform/common/swap_control.hpp"
#include "platform/common/version_negotiation.hpp"

namespace tessera::x11 {
namespace {

using PfnCreateContextAttribs = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
using PfnSwapIntervalExt = void (*)(Display*, GLXDrawable, int);
using PfnSwapIntervalMesa = int (*)(unsigned);
using PfnSwapIntervalSgi = int (*)(int);
using PfnGetSwapIntervalMesa = int (*)(void);
using PfnQueryDrawable = void (*)(Display*, GLXDrawable, int, unsigned int*);
using PfnQueryContext = int (*)(Display*, GLXContext, int, int*);
constexpr int kGlxSwapIntervalExt = 0x20F1;
constexpr int kGlxMaxSwapIntervalExt = 0x20F2;
constexpr int kGlxLateSwapsTearExt = 0x20F3;

// ---- state shared by the backend and all of its contexts ------------------------------------------

/*
 * One private X connection for every context of this backend (never the host application's own), plus the
 * resolved extension entry points. A single connection is required because GLX share groups only work between
 * contexts that live on the same connection. Xlib is made thread-safe once, before the connection is opened.
 */
class GlxShared {
  public:
    const LinuxApi* api = nullptr;
    Display* dpy = nullptr;
    int screen = 0;
    int errorBase = 0;
    PfnCreateContextAttribs createContextAttribs = nullptr;
    PfnSwapIntervalExt swapIntervalExt = nullptr;
    PfnSwapIntervalMesa swapIntervalMesa = nullptr;
    PfnSwapIntervalSgi swapIntervalSgi = nullptr;
    PfnGetSwapIntervalMesa getSwapIntervalMesa = nullptr;
    PfnQueryDrawable queryDrawable = nullptr;
    PfnQueryContext queryContext = nullptr;

    /*
     * A connection that opened successfully is never closed (it lives in the process-wide `keptConnections()`
     * list below and is reused by the next backend instance for the same DISPLAY). Closing the last
     * connection makes Mesa dlclose its DRI driver, and a thread that still has a context current from that
     * driver (or merely used it) runs the driver's pthread-key destructors at its own exit, in code that is
     * no longer mapped: SIGSEGV in __nptl_deallocate_tsd. Found by the Java shutdown test (a context current
     * on a parked thread when tesseraTerminate ran). The EGL backend makes the same choice for the same
     * reason (never eglTerminate, ADR 0006). Only a connection that failed to open is closed.
     */
    bool keepOpen = false;
    std::string displayName; // $DISPLAY at the time of opening: a different value means a different server
    ~GlxShared() {
        if (dpy && !keepOpen)
            api->xlib.XCloseDisplay(dpy);
    }

    bool has(const char* extension) const {
        return std::find(exts_.begin(), exts_.end(), extension) != exts_.end();
    }

    bool open(std::string& error, const std::string& serverName = std::string()) {
        // Xlib is only safe to use from several threads after XInitThreads(); contexts here are used from
        // whichever threads the application chooses. Every toolkit that creates GL contexts (GLFW, SDL, GTK,
        // AWT) makes this same call, and it is idempotent. TESSERA_NO_XINITTHREADS=1 opts out for hosts that
        // guarantee it themselves.
        //
        // Whether the host got there first is visible in libX11's own state (_Xglobal_lock is set by
        // XInitThreads), so Tessera reports it instead of guessing: a host that has NOT initialised Xlib
        // threading and already has displays open is the one case where a late call is a hazard, and the log
        // line says who did what.
        static std::once_flag once;
        std::call_once(once, [this] {
            const int before = xlibThreadsInitialized(api->x11);
            if (std::getenv("TESSERA_NO_XINITTHREADS")) {
                logf(TESSERA_LOG_INFO,
                     "XInitThreads: not called (TESSERA_NO_XINITTHREADS is set); Xlib threading was %s",
                     before > 0    ? "already initialised by the host"
                     : before == 0 ? "NOT initialised"
                                   : "of unknown state");
            } else if (before > 0) {
                logf(TESSERA_LOG_INFO,
                     "XInitThreads: the host had already initialised Xlib threading; nothing to do");
            } else {
                const int ok = api->xlib.XInitThreads();
                logf(TESSERA_LOG_INFO, "XInitThreads: %s, so Tessera called it (-> %d)",
                     before == 0 ? "the host had not initialised Xlib threading"
                                 : "the state could not be read",
                     ok);
            }
        });

        if (!serverName.empty()) { // the host named its server (a window's Display*): ours is a connection to
                                   // that one
            displayName = serverName;
            dpy = api->xlib.XOpenDisplay(serverName.c_str());
            if (!dpy) {
                error = "cannot open the X display \"" + serverName + "\" the window belongs to";
                return false;
            }
        } else {
            const char* name = std::getenv("DISPLAY");
            displayName = name ? name : "";
            dpy = api->xlib.XOpenDisplay(nullptr);
            if (!dpy) {
                error = std::string("cannot open the X display (DISPLAY=") + (name ? name : "<unset>") + ")";
                return false;
            }
        }
        screen = api->xlib.XDefaultScreen(dpy);

        int eventBase = 0;
        if (!api->glx.glXQueryExtension(dpy, &errorBase, &eventBase)) {
            error = "the X server does not support the GLX extension";
            return false;
        }
        int major = 0, minor = 0;
        if (!api->glx.glXQueryVersion(dpy, &major, &minor) || major < 1 || (major == 1 && minor < 3)) {
            error = "GLX 1.3 or newer is required";
            return false;
        }

        const char* list = api->glx.glXQueryExtensionsString(dpy, screen);
        if (list) {
            const char* p = list;
            while (*p) {
                const char* e = p;
                while (*e && *e != ' ')
                    ++e;
                if (e > p)
                    exts_.emplace_back(p, static_cast<std::size_t>(e - p));
                p = *e ? e + 1 : e;
            }
        }
        createContextAttribs = reinterpret_cast<PfnCreateContextAttribs>(proc("glXCreateContextAttribsARB"));
        swapIntervalExt = reinterpret_cast<PfnSwapIntervalExt>(proc("glXSwapIntervalEXT"));
        swapIntervalMesa = reinterpret_cast<PfnSwapIntervalMesa>(proc("glXSwapIntervalMESA"));
        swapIntervalSgi = reinterpret_cast<PfnSwapIntervalSgi>(proc("glXSwapIntervalSGI"));
        getSwapIntervalMesa = reinterpret_cast<PfnGetSwapIntervalMesa>(proc("glXGetSwapIntervalMESA"));
        queryDrawable = reinterpret_cast<PfnQueryDrawable>(proc("glXQueryDrawable"));
        queryContext = reinterpret_cast<PfnQueryContext>(proc("glXQueryContext"));
        logf(TESSERA_LOG_DEBUG, "GLX %d.%d, %zu extensions, ARB_create_context=%d", major, minor,
             exts_.size(), createContextAttribs != nullptr);
        keepOpen = true;
        return true;
    }

    void* proc(const char* name) const {
        return api->glx.glXGetProcAddress(reinterpret_cast<const unsigned char*>(name));
    }

  private:
    std::vector<std::string> exts_;
};

// ---- a native context ---------------------------------------------------------------------------

class GlxContext final : public ContextObject {
  public:
    explicit GlxContext(std::shared_ptr<GlxShared> shared) : shared_(std::move(shared)) {}
    ~GlxContext() override {
        destroy();
    }

    TesseraResult describe(TesseraContextInfo& out) const noexcept override {
        out = info;
        return TESSERA_SUCCESS;
    }

    TesseraResult bindCurrent() noexcept override {
        const LinuxApi& api = *shared_->api;
        crossapi::releaseOtherEglCurrent(); // a host's EGL context on this thread must not make ours fail
                                            // (Mesa: one per thread)
        XErrorScope scope(api, shared_->dpy);
        const Bool ok = api.glx.glXMakeContextCurrent(shared_->dpy, drawable, drawable, ctx);
        const int error = scope.sync();
        if (!ok || error != 0)
            return fail(TESSERA_ERROR_PLATFORM,
                        "glXMakeContextCurrent failed (X error " + std::to_string(error) + ")");
        return TESSERA_SUCCESS;
    }

    void unbindCurrent() noexcept override {
        const LinuxApi& api = *shared_->api;
        if (ctx && api.glx.glXGetCurrentContext() == ctx) {
            XErrorScope scope(api, shared_->dpy);
            api.glx.glXMakeContextCurrent(shared_->dpy, 0, 0, nullptr);
            scope.sync();
        }
    }

    TesseraResult nativeHandles(TesseraNativeHandles& out) const noexcept override {
        out.kind = TESSERA_NATIVE_GLX;
        out.handles[0] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(shared_->dpy));
        out.handles[1] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(ctx));
        out.handles[2] = static_cast<uint64_t>(drawable);
        out.handles[3] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(fbconfig));
        out.handles[4] = static_cast<uint64_t>(window ? window : foreignWindow);
        out.handles[5] = static_cast<uint64_t>(shared_->screen);
        return TESSERA_SUCCESS;
    }

    TesseraResult swapBuffers() noexcept override {
        if (pbuffer || drawable == 0)
            return TESSERA_SUCCESS; // nothing to present
        // A caller's window can vanish under us; the resulting X error must not reach Xlib's default (exit)
        // handler.
        XErrorScope scope(*shared_->api, shared_->dpy);
        shared_->api->glx.glXSwapBuffers(shared_->dpy, drawable);
        const int e = scope.sync();
        if (e != 0)
            return fail(TESSERA_ERROR_PLATFORM,
                        "glXSwapBuffers raised an X error " + std::to_string(e) + " (is the window gone?)");
        return TESSERA_SUCCESS;
    }

    TesseraResult setSwapInterval(int32_t interval) noexcept override {
        if (pbuffer)
            return fail(TESSERA_ERROR_UNSUPPORTED, "swap interval does not apply to pbuffers");
        if (drawable == 0)
            return fail(TESSERA_ERROR_UNSUPPORTED,
                        "swap interval does not apply to a context without a drawable");
        if (interval < 0 && !shared_->has("GLX_EXT_swap_control_tear"))
            return fail(TESSERA_ERROR_INVALID_ARGUMENT,
                        "adaptive vsync (GLX_EXT_swap_control_tear) is not available");
        const LinuxApi& api = *shared_->api;
        XErrorScope scope(api, shared_->dpy);
        if (shared_->swapIntervalExt && shared_->has("GLX_EXT_swap_control")) {
            shared_->swapIntervalExt(shared_->dpy, drawable, interval);
            if (scope.sync() != 0)
                return fail(TESSERA_ERROR_PLATFORM, "glXSwapIntervalEXT raised an X error");
            return TESSERA_SUCCESS;
        }
        if (interval >= 0 && shared_->swapIntervalMesa && shared_->has("GLX_MESA_swap_control"))
            return shared_->swapIntervalMesa(static_cast<unsigned>(interval)) == 0
                       ? TESSERA_SUCCESS
                       : fail(TESSERA_ERROR_PLATFORM, "glXSwapIntervalMESA failed");
        if (interval >= 0 && shared_->swapIntervalSgi && shared_->has("GLX_SGI_swap_control"))
            return shared_->swapIntervalSgi(interval) == 0
                       ? TESSERA_SUCCESS
                       : fail(TESSERA_ERROR_PLATFORM, "glXSwapIntervalSGI failed");
        return fail(TESSERA_ERROR_UNSUPPORTED, "no GLX swap-control extension is available");
    }

    // A caller's window is resized by its owner and the drawable follows by itself; our own hidden window has
    // no one to tell.
    TesseraResult resizeWindow(int32_t, int32_t) noexcept override {
        return foreignWindow
                   ? TESSERA_SUCCESS
                   : fail(TESSERA_ERROR_UNSUPPORTED, "this context has no window of a caller's to resize");
    }

    GlxSwapInputs swapInputs() const {
        GlxSwapInputs in;
        in.hasExt = shared_->has("GLX_EXT_swap_control");
        in.hasMesa = shared_->has("GLX_MESA_swap_control");
        in.hasSgi = shared_->has("GLX_SGI_swap_control");
        in.extFn = shared_->swapIntervalExt != nullptr;
        in.mesaFn = shared_->swapIntervalMesa != nullptr;
        in.sgiFn = shared_->swapIntervalSgi != nullptr;
        in.hasTear = shared_->has("GLX_EXT_swap_control_tear");
        in.queryDrawableFn = shared_->queryDrawable != nullptr;
        in.getMesaFn = shared_->getSwapIntervalMesa != nullptr;
        return in;
    }

    TesseraResult getSwapControl(TesseraSwapControl& out) noexcept override {
        const GlxSwapInputs in = swapInputs();
        out.caps = glxSwapCaps(in, !pbuffer && drawable != 0);
        if (!(out.caps & TESSERA_SWAP_CAP_CONTROL))
            return TESSERA_SUCCESS;
        if (chooseGlxSwapApi(in) == GlxSwapApi::Mesa) {
            if (out.caps & TESSERA_SWAP_CAP_READBACK)
                out.applied = shared_->getSwapIntervalMesa();
            return TESSERA_SUCCESS;
        }
        if (chooseGlxSwapApi(in) == GlxSwapApi::Ext && (out.caps & TESSERA_SWAP_CAP_READBACK)) {
            const LinuxApi& api = *shared_->api;
            XErrorScope scope(api, shared_->dpy);
            unsigned int interval = 0, maxInterval = 0, tear = 0;
            shared_->queryDrawable(shared_->dpy, drawable, kGlxSwapIntervalExt, &interval);
            shared_->queryDrawable(shared_->dpy, drawable, kGlxMaxSwapIntervalExt, &maxInterval);
            if (in.hasTear)
                shared_->queryDrawable(shared_->dpy, drawable, kGlxLateSwapsTearExt, &tear);
            if (scope.sync() != 0) {
                out.caps &= ~static_cast<uint32_t>(TESSERA_SWAP_CAP_READBACK); // the server refused the query
                return TESSERA_SUCCESS;
            }
            out.applied = glxAppliedInterval(static_cast<int32_t>(interval), tear != 0);
            out.maxInterval = static_cast<int32_t>(maxInterval);
        }
        return TESSERA_SUCCESS;
    }

    TesseraResult queryResetStatus(int32_t& out) noexcept override {
        return queryResetStatusFrom(resetFn, info.flags, out);
    }

    GlResetStatusFn resetFn = nullptr;
    std::shared_ptr<GlxShared> shared_;
    GLXFBConfig fbconfig = nullptr;
    Window window = 0;        // the hidden window we created and own
    Window foreignWindow = 0; // a caller's window we render into (borrowed: never destroyed)
    Colormap colormap = 0;
    GLXPbuffer pbuffer = 0;
    GLXDrawable drawable = 0; // the window or the pbuffer
    GLXContext ctx = nullptr;
    std::shared_ptr<GlxContext> parent; // keeps a share parent's native context alive as long as we are
    TesseraContextInfo info = {};

  private:
    void destroy() noexcept {
        const LinuxApi& api = *shared_->api;
        Display* dpy = shared_->dpy;
        XErrorScope scope(api, dpy); // teardown must never reach Xlib's terminate-the-process default handler
        if (ctx) {
            if (api.glx.glXGetCurrentContext() == ctx)
                api.glx.glXMakeContextCurrent(dpy, 0, 0, nullptr);
            api.glx.glXDestroyContext(dpy, ctx);
            ctx = nullptr;
        }
        if (pbuffer)
            api.glx.glXDestroyPbuffer(dpy, pbuffer);
        if (window)
            api.xlib.XDestroyWindow(dpy, window);
        if (colormap)
            api.xlib.XFreeColormap(dpy, colormap);
        pbuffer = 0;
        window = 0;
        colormap = 0;
        scope.sync();
    }
};

// ---- framebuffer configuration --------------------------------------------------------------------

std::vector<int> fbconfigAttribs(const TesseraContextConfig& c, const FormatAttempt& a, bool pbuffer) {
    std::vector<int> v;
    auto add = [&v](int key, int value) {
        v.push_back(key);
        v.push_back(value);
    };
    const bool isFloat = (c.flags & TESSERA_FLAG_FLOAT_COLOR) != 0;
    add(kGlxXRenderable, 1);
    add(kGlxDrawableType, pbuffer ? kGlxPbufferBit : kGlxWindowBit);
    add(kGlxRenderType, isFloat ? kGlxRgbaFloatBit : kGlxRgbaBit);
    if (!isFloat && !pbuffer)
        add(kGlxXVisualType, kGlxTrueColor);
    if (c.flags & TESSERA_FLAG_DOUBLE_BUFFER)
        add(kGlxDoubleBuffer, 1);
    if (c.flags & TESSERA_FLAG_STEREO)
        add(kGlxStereo, 1);

    int r = c.redBits, g = c.greenBits, b = c.blueBits;
    if (r == 0 && g == 0 && b == 0)
        r = g = b = isFloat ? 16 : 8; // sane default for "don't care"
    add(kGlxRedSize, r);
    add(kGlxGreenSize, g);
    add(kGlxBlueSize, b);
    if (c.alphaBits > 0)
        add(kGlxAlphaSize, c.alphaBits);
    if (!a.relaxDepth) {
        if (c.depthBits > 0)
            add(kGlxDepthSize, c.depthBits);
        if (c.stencilBits > 0)
            add(kGlxStencilSize, c.stencilBits);
    }
    if (a.samples > 0) {
        add(kGlxSampleBuffers, 1);
        add(kGlxSamples, a.samples);
    }
    if (a.srgb)
        add(kGlxFramebufferSrgb, 1);
    v.push_back(0);
    return v;
}

int attrib(const GlxShared& sh, GLXFBConfig fb, int key) {
    int value = 0;
    sh.api->glx.glXGetFBConfigAttrib(sh.dpy, fb, key, &value);
    return value;
}

TesseraResult chooseFbConfig(const GlxShared& sh, const TesseraContextConfig& c, bool pbuffer,
                             GLXFBConfig& out) {
    const bool strict = c.fallback == TESSERA_FALLBACK_STRICT;
    FormatRequest request;
    if (TesseraResult r = gateFormatFeatures(
            c, "GLX_", [&sh](const char* e) { return sh.has(e); }, request);
        r != TESSERA_SUCCESS)
        return r;

    // GLX has no ARB name for multisample/sRGB/float on every server; the gate above uses the standard ones.
    for (const FormatAttempt& a : formatAttempts(request.samples, request.srgb, strict)) {
        const std::vector<int> attribs = fbconfigAttribs(c, a, pbuffer);
        int count = 0;
        GLXFBConfig* list = sh.api->glx.glXChooseFBConfig(sh.dpy, sh.screen, attribs.data(), &count);
        if (list && count > 0) {
            out = list[0]; // the server sorts best-first
            sh.api->xlib.XFree(list);
            if (a.samples != request.samples || a.srgb != request.srgb || a.relaxDepth)
                logf(TESSERA_LOG_INFO,
                     "GLX: framebuffer config relaxed (samples %d->%d, srgb %d, depth relaxed %d)", c.samples,
                     a.samples, a.srgb, a.relaxDepth);
            return TESSERA_SUCCESS;
        }
        if (list)
            sh.api->xlib.XFree(list);
    }
    return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                "no framebuffer configuration matches the requested configuration");
}

// ---- reporting what we actually got -------------------------------------------------------------

/* Requires the context to be current on the calling thread. */
bool queryInfo(const GlxShared& sh, GLXFBConfig fb, const TesseraContextConfig& c, bool pbuffer,
               TesseraContextInfo& info) {
    GlQueryFns gl;
    gl.getString = reinterpret_cast<decltype(gl.getString)>(sh.proc("glGetString"));
    gl.getIntegerv = reinterpret_cast<decltype(gl.getIntegerv)>(sh.proc("glGetIntegerv"));
    gl.getError = reinterpret_cast<decltype(gl.getError)>(sh.proc("glGetError"));
    if (!queryContextInfo(gl, info))
        return false;

    uint32_t flags = info.flags;
    info.redBits = attrib(sh, fb, kGlxRedSize);
    info.greenBits = attrib(sh, fb, kGlxGreenSize);
    info.blueBits = attrib(sh, fb, kGlxBlueSize);
    info.alphaBits = attrib(sh, fb, kGlxAlphaSize);
    info.depthBits = attrib(sh, fb, kGlxDepthSize);
    info.stencilBits = attrib(sh, fb, kGlxStencilSize);
    info.samples = attrib(sh, fb, kGlxSamples);
    if (attrib(sh, fb, kGlxDoubleBuffer))
        flags |= TESSERA_FLAG_DOUBLE_BUFFER;
    if (attrib(sh, fb, kGlxStereo))
        flags |= TESSERA_FLAG_STEREO;
    if ((sh.has("GLX_ARB_framebuffer_sRGB") || sh.has("GLX_EXT_framebuffer_sRGB")) &&
        attrib(sh, fb, kGlxFramebufferSrgb))
        flags |= TESSERA_FLAG_SRGB;
    if (attrib(sh, fb, kGlxRenderType) & kGlxRgbaFloatBit)
        flags |= TESSERA_FLAG_FLOAT_COLOR;
    info.flags = flags;
    info.surface =
        pbuffer ? TESSERA_SURFACE_PBUFFER
                : (c.surface == TESSERA_SURFACE_NONE ? TESSERA_SURFACE_NONE : TESSERA_SURFACE_HIDDEN_WINDOW);
    return true;
}

// ---- the backend --------------------------------------------------------------------------------

class GlxBackend final : public Backend {
  public:
    const char* name() const noexcept override {
        return "glx";
    }

    TesseraResult createContext(const TesseraContextConfig& c,
                                const std::shared_ptr<ContextObject>& shareWith,
                                std::shared_ptr<ContextObject>& out) override {
        std::string error;
        std::shared_ptr<GlxShared> sh = getShared(error);
        if (!sh)
            return fail(TESSERA_ERROR_UNSUPPORTED, error);
        const LinuxApi& api = *sh->api;

        std::shared_ptr<GlxContext> parent;
        if (shareWith) {
            parent = std::dynamic_pointer_cast<GlxContext>(shareWith);
            if (!parent)
                return fail(TESSERA_ERROR_INVALID_ARGUMENT,
                            "cannot share objects with a context from another backend");
        }

        // Everything below may change this thread's current context; put the caller's back on every path.
        crossapi::GlxGuard guard;

        const bool wantPbuffer = c.surface == TESSERA_SURFACE_PBUFFER;
        const bool wantNone = c.surface == TESSERA_SURFACE_NONE;
        auto ctx = std::make_shared<GlxContext>(sh); // RAII: any failure below cleans up through ~GlxContext

        if (TesseraResult r = chooseFbConfig(*sh, c, wantPbuffer, ctx->fbconfig); r != TESSERA_SUCCESS)
            return r;

        // The drawable: a pbuffer, or a hidden window that is never mapped (no window manager involvement),
        // or none at all (SURFACE_NONE: the context is made current without a drawable, which GLX allows for
        // modern contexts; if the driver refuses, a hidden window is created after all, see below).
        if (wantPbuffer) {
            XErrorScope scope(api, sh->dpy);
            const int attribs[] = {kGlxPbufferWidth, 1, kGlxPbufferHeight, 1, kGlxPreservedContents, 0, 0};
            ctx->pbuffer = api.glx.glXCreatePbuffer(sh->dpy, ctx->fbconfig, attribs);
            ctx->drawable = ctx->pbuffer;
            const int e = scope.sync();
            if (e != 0 || !ctx->drawable)
                return fail(TESSERA_ERROR_PLATFORM,
                            "could not create the drawable (X error " + std::to_string(e) + ")");
        } else if (!wantNone) {
            if (TesseraResult r = createHiddenWindow(*sh, *ctx); r != TESSERA_SUCCESS)
                return r;
        }

        if (TesseraResult r = createNativeContext(*sh, *ctx, c, parent ? parent->ctx : nullptr);
            r != TESSERA_SUCCESS)
            return r;
        ctx->parent = std::move(parent);

        TesseraResult bound = ctx->bindCurrent();
        if (bound != TESSERA_SUCCESS && wantNone && ctx->drawable == 0) {
            // This driver cannot make a context current without a drawable: give it a hidden window after
            // all.
            logf(TESSERA_LOG_DEBUG,
                 "GLX: no drawable-less make-current here, using a hidden window for SURFACE_NONE");
            if (TesseraResult r = createHiddenWindow(*sh, *ctx); r != TESSERA_SUCCESS)
                return r;
            bound = ctx->bindCurrent();
        }
        if (bound != TESSERA_SUCCESS)
            return bound;
        const bool ok = queryInfo(*sh, ctx->fbconfig, c, wantPbuffer, ctx->info);
        // `guard` restores the caller's context; it must not be left to us.
        if (!ok)
            return fail(TESSERA_ERROR_PLATFORM, "could not query the new context (glGetString failed)");
        return finishContext(*sh, ctx, c, false, out);
    }

    /*
     * Renders into a caller's X window (an XID on $DISPLAY's server). The window is borrowed: we only look up
     * its visual, pick the framebuffer configuration that belongs to it, and never destroy or modify the
     * window. A window created with a visual that has no GLX framebuffer configuration cannot be used (GLX
     * cannot change a window's visual afterwards).
     */
    uint32_t nativeWindowKind() const noexcept override {
        return TESSERA_WINDOW_X11;
    }

    TesseraResult createContextForWindow(const TesseraContextConfig& c, uint64_t nativeWindow,
                                         uint32_t /*flags*/, const std::shared_ptr<ContextObject>& shareWith,
                                         std::shared_ptr<ContextObject>& out) override {
        return attachToWindow(std::string(), c, nativeWindow, shareWith, out);
    }

    /* The described form. For X11, `display` (optional) is the host's own Display*: the window lives on ITS
     * server, which may not be $DISPLAY. Tessera never uses that pointer for anything but reading the name
     * the host opened it with (XDisplayString): Xlib connections are not thread-safe unless the host called
     * XInitThreads, so Tessera opens its own connection to the same server (and screen's server-wide GLX) and
     * the host's one stays the host's. */
    TesseraResult createContextForNativeWindow(const TesseraContextConfig& c,
                                               const TesseraNativeWindow& window, uint32_t flags,
                                               const std::shared_ptr<ContextObject>& shareWith,
                                               std::shared_ptr<ContextObject>& out) override {
        if (window.kind != TESSERA_WINDOW_X11)
            return fail(TESSERA_ERROR_UNSUPPORTED, "this backend cannot attach to that kind of window");
        if (window.display == 0)
            return createContextForWindow(c, window.window, flags, shareWith, out);
        const LinuxApi* api = linuxApi();
        if (!api)
            return fail(TESSERA_ERROR_UNSUPPORTED, "libX11 and libGL are not available on this system");
        const char* name =
            api->xlib.XDisplayString(reinterpret_cast<Display*>(static_cast<std::uintptr_t>(window.display)));
        if (!name || !*name)
            return fail(TESSERA_ERROR_INVALID_ARGUMENT, "the display is not an open Xlib Display*");
        return attachToWindow(name, c, window.window, shareWith, out);
    }

    TesseraResult attachToWindow(const std::string& serverName, const TesseraContextConfig& c,
                                 uint64_t nativeWindow, const std::shared_ptr<ContextObject>& shareWith,
                                 std::shared_ptr<ContextObject>& out) {
        std::string error;
        std::shared_ptr<GlxShared> sh = getShared(error, serverName);
        if (!sh)
            return fail(TESSERA_ERROR_UNSUPPORTED, error);
        const LinuxApi& api = *sh->api;
#if !defined(__LP64__)
        return fail(TESSERA_ERROR_UNSUPPORTED, "window attach is implemented for LP64 X11 only");
#endif
        std::shared_ptr<GlxContext> parent;
        if (shareWith) {
            parent = std::dynamic_pointer_cast<GlxContext>(shareWith);
            if (!parent)
                return fail(TESSERA_ERROR_INVALID_ARGUMENT,
                            "cannot share objects with a context from another backend");
        }
        crossapi::GlxGuard guard;
        auto ctx = std::make_shared<GlxContext>(sh);
        ctx->foreignWindow = static_cast<Window>(nativeWindow);
        ctx->drawable = static_cast<GLXDrawable>(nativeWindow);

        // Which visual does the window have? (Also validates that it exists: BadWindow.)
        unsigned long visualId = 0;
        {
            XErrorScope scope(api, sh->dpy);
            alignas(8) unsigned char attrs[kWindowAttributesSize] = {};
            const int status = api.xlib.XGetWindowAttributes(sh->dpy, ctx->foreignWindow, attrs);
            const int e = scope.sync();
            if (status == 0 || e != 0) {
                ctx->foreignWindow = 0; // nothing of ours to clean up
                return fail(TESSERA_ERROR_INVALID_ARGUMENT,
                            "nativeWindow is not a window on the X server of $DISPLAY");
            }
            Visual* visual = nullptr;
            std::memcpy(&visual, attrs + kWindowAttributesVisualOffset, sizeof visual);
            if (visual)
                visualId = api.xlib.XVisualIDFromVisual(visual);
        }
        if (visualId == 0)
            return fail(TESSERA_ERROR_PLATFORM, "could not determine the visual of the window");

        {
            const int attribs[] = {kGlxVisualId, static_cast<int>(visualId), 0};
            int count = 0;
            GLXFBConfig* list = api.glx.glXChooseFBConfig(sh->dpy, sh->screen, attribs, &count);
            if (!list || count <= 0) {
                if (list)
                    api.xlib.XFree(list);
                return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                            "the window's visual has no GLX framebuffer configuration");
            }
            // A visual can map to several framebuffer configurations (e.g. single- and double-buffered
            // variants, which are both valid for the same window): take the first one that agrees with the
            // request on buffering, else the first.
            const bool wantDouble = (c.flags & TESSERA_FLAG_DOUBLE_BUFFER) != 0;
            ctx->fbconfig = list[0];
            for (int i = 0; i < count; ++i) {
                if ((attrib(*sh, list[i], kGlxDoubleBuffer) != 0) == wantDouble) {
                    ctx->fbconfig = list[i];
                    break;
                }
            }
            api.xlib.XFree(list);
        }

        if (TesseraResult r = createNativeContext(*sh, *ctx, c, parent ? parent->ctx : nullptr);
            r != TESSERA_SUCCESS)
            return r;
        ctx->parent = std::move(parent);
        if (TesseraResult r = ctx->bindCurrent(); r != TESSERA_SUCCESS)
            return r;
        if (!queryInfo(*sh, ctx->fbconfig, c, false, ctx->info))
            return fail(TESSERA_ERROR_PLATFORM, "could not query the new context (glGetString failed)");
        ctx->info.surface = TESSERA_SURFACE_WINDOW;
        return finishContext(*sh, ctx, c, true, out);
    }

    void* getProcAddress(const char* name) noexcept override {
        const LinuxApi* api = linuxApi();
        if (!api || !name)
            return nullptr;
        // GLVND hands out a dispatch stub for any name it is asked about; every GL/GLX function starts with
        // "gl", so refuse the rest. (A made-up "gl*" name can still resolve to a no-op stub - that is the GLX
        // contract; check the extension strings, not the pointer, to learn what is supported.)
        if (name[0] != 'g' || name[1] != 'l')
            return nullptr;
        return api->glx.glXGetProcAddress(reinterpret_cast<const unsigned char*>(name));
    }

  private:
    /* A 1x1 override-redirect window that is never mapped, with our own colormap, matching the context's
     * framebuffer configuration; becomes the context's drawable. */
    static TesseraResult createHiddenWindow(GlxShared& sh, GlxContext& ctx) {
        const LinuxApi& api = *sh.api;
        XErrorScope scope(api, sh.dpy);
        XVisualInfo* vi = api.glx.glXGetVisualFromFBConfig(sh.dpy, ctx.fbconfig);
        if (!vi)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "the framebuffer configuration has no X visual");
        const Window root = api.xlib.XRootWindow(sh.dpy, sh.screen);
        ctx.colormap = api.xlib.XCreateColormap(sh.dpy, root, vi->visual, kAllocNone);
        XSetWindowAttributes attrs = {};
        attrs.border_pixel = 0;
        attrs.override_redirect = 1;
        attrs.event_mask = 0;
        attrs.colormap = ctx.colormap;
        ctx.window =
            api.xlib.XCreateWindow(sh.dpy, root, 0, 0, 1, 1, 0, vi->depth, kInputOutput, vi->visual,
                                   kCWBorderPixel | kCWOverrideRedirect | kCWEventMask | kCWColormap, &attrs);
        api.xlib.XFree(vi);
        ctx.drawable = ctx.window;
        const int e = scope.sync();
        if (e != 0 || !ctx.drawable)
            return fail(TESSERA_ERROR_PLATFORM,
                        "could not create the drawable (X error " + std::to_string(e) + ")");
        return TESSERA_SUCCESS;
    }

    /* The context is current and described: verify strict requests and bind the per-context extras. */
    static TesseraResult finishContext(GlxShared& sh, const std::shared_ptr<GlxContext>& ctx,
                                       const TesseraContextConfig& c, bool /*window*/,
                                       std::shared_ptr<ContextObject>& out) {
        // Never negotiable, whatever the fallback policy: API, minimum version, profile.
        if (const char* why = hardRequirementViolation(c, ctx->info))
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, why);

        // What priority did the driver grant? (Only asked where the extension exists; the answer is the
        // driver's.)
        if (sh.queryContext && sh.has("GLX_EXT_context_priority")) {
            XErrorScope scope(*sh.api, sh.dpy);
            int level = 0;
            if (sh.queryContext(sh.dpy, ctx->ctx, kNativeContextPriorityLevel, &level) == 0 &&
                scope.sync() == 0)
                ctx->info.priority = priorityFromNative(level);
        }

        // Strict mode promises exactly what was asked for: verify the parts the driver may have loosened.
        if (c.fallback == TESSERA_FALLBACK_STRICT && !contextSatisfies(c, ctx->info))
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "the created context does not satisfy the strict request");

        // Reset polling and debug-message forwarding are bound to the context that is current right now.
        const auto glProc = [&sh](const char* n) -> void* { return sh.proc(n); };
        ctx->resetFn = resolveResetStatus(glProc);
        if (ctx->info.flags & TESSERA_FLAG_DEBUG)
            installGlDebugForwarder(glProc);

        out = ctx;
        return TESSERA_SUCCESS;
    }

    static TesseraResult createNativeContext(GlxShared& sh, GlxContext& ctx, const TesseraContextConfig& c,
                                             GLXContext shareCtx) {
        const LinuxApi& api = *sh.api;

        ArbContextPlan plan;
        const TesseraResult planned = planArbContext(
            c, "GLX_", sh.createContextAttribs != nullptr, [&sh](const char* e) { return sh.has(e); }, plan);
        if (planned != TESSERA_SUCCESS)
            return planned;

        if (plan.legacy) {
            XErrorScope scope(api, sh.dpy);
            ctx.ctx = api.glx.glXCreateNewContext(sh.dpy, ctx.fbconfig, kGlxRgbaType, shareCtx, 1);
            const int e = scope.sync();
            if (!ctx.ctx || e != 0) {
                ctx.ctx = nullptr;
                return fail(TESSERA_ERROR_PLATFORM,
                            "glXCreateNewContext failed (X error " + std::to_string(e) + ")");
            }
            return TESSERA_SUCCESS;
        }

        int lastError = 0;
        const bool made =
            negotiateVersion(versionMemoryKey("glx", reinterpret_cast<std::uintptr_t>(sh.dpy), c),
                             plan.versions, [&](const GlVersion& version) {
                                 const std::vector<int> attribs = plan.attribs(version);
                                 XErrorScope scope(api, sh.dpy);
                                 GLXContext created = sh.createContextAttribs(sh.dpy, ctx.fbconfig, shareCtx,
                                                                              1, attribs.data());
                                 lastError = scope.sync();
                                 if (created && lastError == 0) {
                                     ctx.ctx = created;
                                     return true;
                                 }
                                 // A rejected request may still have produced an object alongside the error.
                                 if (created)
                                     api.glx.glXDestroyContext(sh.dpy, created);
                                 return false;
                             });
        if (made)
            return TESSERA_SUCCESS;
        // Unsupported version/profile surfaces as BadMatch / BadValue / GLXBadFBConfig / GLXBadProfileARB.
        if (classifyGlxCreateError(lastError) == TESSERA_ERROR_OUT_OF_MEMORY)
            return fail(TESSERA_ERROR_OUT_OF_MEMORY, "the X server is out of memory");
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                    "the driver does not support the requested OpenGL version/profile");
    }

    /* `serverName` (a window's server, from a host Display*): "" = $DISPLAY, the usual connection. */
    std::shared_ptr<GlxShared> getShared(std::string& error, const std::string& serverName = std::string()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (serverName.empty() && shared_)
            return shared_;
        if (!serverName.empty()) {
            auto it = namedShared_.find(serverName);
            if (it != namedShared_.end())
                return it->second;
        }
        const LinuxApi* api = linuxApi();
        if (!api) {
            error = "libX11 and libGL are not available on this system";
            return nullptr;
        }
        // Reuse the connection a previous backend instance opened for the same display (see
        // GlxShared::keepOpen).
        static std::mutex keptMutex;
        static std::vector<std::shared_ptr<GlxShared>>* const kept =
            new std::vector<std::shared_ptr<GlxShared>>(); // never freed
        std::lock_guard<std::mutex> keptLock(keptMutex);
        const char* display = std::getenv("DISPLAY");
        const std::string wanted = !serverName.empty() ? serverName : (display ? display : "");
        std::shared_ptr<GlxShared> s;
        for (const auto& k : *kept)
            if (k->displayName == wanted && k->api == api)
                s = k;
        if (!s) {
            s = std::make_shared<GlxShared>();
            s->api = api;
            if (!s->open(error, serverName))
                return nullptr;
            kept->push_back(s);
        }
        if (!serverName.empty()) {
            namedShared_[serverName] = s;
            return s;
        }
        shared_ = s;
        return shared_;
    }

    std::mutex mutex_;
    std::shared_ptr<GlxShared> shared_;
    std::map<std::string, std::shared_ptr<GlxShared>>
        namedShared_; // by X server name (a host window's Display*)
};

} // namespace

std::shared_ptr<Backend> makeGlxBackend() {
    return std::make_shared<GlxBackend>();
}

} // namespace tessera::x11
