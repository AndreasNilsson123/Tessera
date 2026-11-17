#include "glx_backend.hpp"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "glx_api.hpp"
#include "platform/common/arb_context.hpp"
#include "platform/common/gl_query.hpp"
#include "core/boundary.hpp"
#include "core/log.hpp"

namespace tessera::x11 {
namespace {

using PfnCreateContextAttribs = GLXContext (*)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
using PfnSwapIntervalExt = void (*)(Display*, GLXDrawable, int);
using PfnSwapIntervalMesa = int (*)(unsigned);
using PfnSwapIntervalSgi = int (*)(int);

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

    ~GlxShared() {
        if (dpy) api->xlib.XCloseDisplay(dpy);
    }

    bool has(const char* extension) const {
        return std::find(exts_.begin(), exts_.end(), extension) != exts_.end();
    }

    bool open(std::string& error) {
        // Xlib is only safe to use from several threads after XInitThreads(); contexts here are used from
        // whichever threads the application chooses. Every toolkit that creates GL contexts (GLFW, SDL, GTK,
        // AWT) makes this same call, and it is idempotent. TESSERA_NO_XINITTHREADS=1 opts out for hosts that
        // guarantee it themselves.
        static std::once_flag once;
        std::call_once(once, [this] {
            if (!std::getenv("TESSERA_NO_XINITTHREADS")) {
                const int ok = api->xlib.XInitThreads();
                logf(TESSERA_LOG_DEBUG, "XInitThreads() -> %d", ok);
            }
        });

        dpy = api->xlib.XOpenDisplay(nullptr);
        if (!dpy) {
            const char* name = std::getenv("DISPLAY");
            error = std::string("cannot open the X display (DISPLAY=") + (name ? name : "<unset>") + ")";
            return false;
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
                while (*e && *e != ' ') ++e;
                if (e > p) exts_.emplace_back(p, static_cast<std::size_t>(e - p));
                p = *e ? e + 1 : e;
            }
        }
        createContextAttribs = reinterpret_cast<PfnCreateContextAttribs>(proc("glXCreateContextAttribsARB"));
        swapIntervalExt = reinterpret_cast<PfnSwapIntervalExt>(proc("glXSwapIntervalEXT"));
        swapIntervalMesa = reinterpret_cast<PfnSwapIntervalMesa>(proc("glXSwapIntervalMESA"));
        swapIntervalSgi = reinterpret_cast<PfnSwapIntervalSgi>(proc("glXSwapIntervalSGI"));
        logf(TESSERA_LOG_DEBUG, "GLX %d.%d, %zu extensions, ARB_create_context=%d", major, minor, exts_.size(),
             createContextAttribs != nullptr);
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
    ~GlxContext() override { destroy(); }

    TesseraResult describe(TesseraContextInfo& out) const noexcept override {
        out = info;
        return TESSERA_SUCCESS;
    }

    TesseraResult bindCurrent() noexcept override {
        const LinuxApi& api = *shared_->api;
        XErrorScope scope(api, shared_->dpy);
        const Bool ok = api.glx.glXMakeContextCurrent(shared_->dpy, drawable, drawable, ctx);
        const int error = scope.sync();
        if (!ok || error != 0)
            return fail(TESSERA_ERROR_PLATFORM, "glXMakeContextCurrent failed (X error " + std::to_string(error) + ")");
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

    TesseraResult swapBuffers() noexcept override {
        if (pbuffer) return TESSERA_SUCCESS; // nothing to present
        shared_->api->glx.glXSwapBuffers(shared_->dpy, drawable);
        return TESSERA_SUCCESS;
    }

    TesseraResult setSwapInterval(int32_t interval) noexcept override {
        if (pbuffer) return fail(TESSERA_ERROR_UNSUPPORTED, "swap interval does not apply to pbuffers");
        if (interval < 0 && !shared_->has("GLX_EXT_swap_control_tear"))
            return fail(TESSERA_ERROR_INVALID_ARGUMENT, "adaptive vsync (GLX_EXT_swap_control_tear) is not available");
        const LinuxApi& api = *shared_->api;
        XErrorScope scope(api, shared_->dpy);
        if (shared_->swapIntervalExt && shared_->has("GLX_EXT_swap_control")) {
            shared_->swapIntervalExt(shared_->dpy, drawable, interval);
            if (scope.sync() != 0) return fail(TESSERA_ERROR_PLATFORM, "glXSwapIntervalEXT raised an X error");
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

    std::shared_ptr<GlxShared> shared_;
    GLXFBConfig fbconfig = nullptr;
    Window window = 0;
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
            if (api.glx.glXGetCurrentContext() == ctx) api.glx.glXMakeContextCurrent(dpy, 0, 0, nullptr);
            api.glx.glXDestroyContext(dpy, ctx);
            ctx = nullptr;
        }
        if (pbuffer) api.glx.glXDestroyPbuffer(dpy, pbuffer);
        if (window) api.xlib.XDestroyWindow(dpy, window);
        if (colormap) api.xlib.XFreeColormap(dpy, colormap);
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
    if (!isFloat && !pbuffer) add(kGlxXVisualType, kGlxTrueColor);
    if (c.flags & TESSERA_FLAG_DOUBLE_BUFFER) add(kGlxDoubleBuffer, 1);
    if (c.flags & TESSERA_FLAG_STEREO) add(kGlxStereo, 1);

    int r = c.redBits, g = c.greenBits, b = c.blueBits;
    if (r == 0 && g == 0 && b == 0) r = g = b = isFloat ? 16 : 8; // sane default for "don't care"
    add(kGlxRedSize, r);
    add(kGlxGreenSize, g);
    add(kGlxBlueSize, b);
    if (c.alphaBits > 0) add(kGlxAlphaSize, c.alphaBits);
    if (!a.relaxDepth) {
        if (c.depthBits > 0) add(kGlxDepthSize, c.depthBits);
        if (c.stencilBits > 0) add(kGlxStencilSize, c.stencilBits);
    }
    if (a.samples > 0) {
        add(kGlxSampleBuffers, 1);
        add(kGlxSamples, a.samples);
    }
    if (a.srgb) add(kGlxFramebufferSrgb, 1);
    v.push_back(0);
    return v;
}

int attrib(const GlxShared& sh, GLXFBConfig fb, int key) {
    int value = 0;
    sh.api->glx.glXGetFBConfigAttrib(sh.dpy, fb, key, &value);
    return value;
}

TesseraResult chooseFbConfig(const GlxShared& sh, const TesseraContextConfig& c, bool pbuffer, GLXFBConfig& out) {
    const bool strict = c.fallback == TESSERA_FALLBACK_STRICT;
    FormatRequest request;
    if (TesseraResult r = gateFormatFeatures(c, "GLX_", [&sh](const char* e) { return sh.has(e); }, request);
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
                logf(TESSERA_LOG_INFO, "GLX: framebuffer config relaxed (samples %d->%d, srgb %d, depth relaxed %d)",
                     c.samples, a.samples, a.srgb, a.relaxDepth);
            return TESSERA_SUCCESS;
        }
        if (list) sh.api->xlib.XFree(list);
    }
    return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "no framebuffer configuration matches the requested configuration");
}

// ---- reporting what we actually got -------------------------------------------------------------

/* Requires the context to be current on the calling thread. */
bool queryInfo(const GlxShared& sh, GLXFBConfig fb, const TesseraContextConfig& c, bool pbuffer,
               TesseraContextInfo& info) {
    GlQueryFns gl;
    gl.getString = reinterpret_cast<decltype(gl.getString)>(sh.proc("glGetString"));
    gl.getIntegerv = reinterpret_cast<decltype(gl.getIntegerv)>(sh.proc("glGetIntegerv"));
    gl.getError = reinterpret_cast<decltype(gl.getError)>(sh.proc("glGetError"));
    if (!queryContextInfo(gl, c, info)) return false;

    uint32_t flags = info.flags;
    info.redBits = attrib(sh, fb, kGlxRedSize);
    info.greenBits = attrib(sh, fb, kGlxGreenSize);
    info.blueBits = attrib(sh, fb, kGlxBlueSize);
    info.alphaBits = attrib(sh, fb, kGlxAlphaSize);
    info.depthBits = attrib(sh, fb, kGlxDepthSize);
    info.stencilBits = attrib(sh, fb, kGlxStencilSize);
    info.samples = attrib(sh, fb, kGlxSamples);
    if (attrib(sh, fb, kGlxDoubleBuffer)) flags |= TESSERA_FLAG_DOUBLE_BUFFER;
    if (attrib(sh, fb, kGlxStereo)) flags |= TESSERA_FLAG_STEREO;
    if ((sh.has("GLX_ARB_framebuffer_sRGB") || sh.has("GLX_EXT_framebuffer_sRGB")) && attrib(sh, fb, kGlxFramebufferSrgb))
        flags |= TESSERA_FLAG_SRGB;
    if (attrib(sh, fb, kGlxRenderType) & kGlxRgbaFloatBit) flags |= TESSERA_FLAG_FLOAT_COLOR;
    info.flags = flags;
    info.surface = pbuffer ? TESSERA_SURFACE_PBUFFER
                           : (c.surface == TESSERA_SURFACE_NONE ? TESSERA_SURFACE_NONE : TESSERA_SURFACE_HIDDEN_WINDOW);
    return true;
}

// ---- the backend --------------------------------------------------------------------------------

class GlxBackend final : public Backend {
  public:
    const char* name() const noexcept override { return "glx"; }

    TesseraResult createContext(const TesseraContextConfig& c, const std::shared_ptr<ContextObject>& shareWith,
                                std::shared_ptr<ContextObject>& out) override {
        std::string error;
        std::shared_ptr<GlxShared> sh = getShared(error);
        if (!sh) return fail(TESSERA_ERROR_UNSUPPORTED, error);
        const LinuxApi& api = *sh->api;

        std::shared_ptr<GlxContext> parent;
        if (shareWith) {
            parent = std::dynamic_pointer_cast<GlxContext>(shareWith);
            if (!parent) return fail(TESSERA_ERROR_INVALID_ARGUMENT, "cannot share objects with a context from another backend");
        }

        // Everything below may change this thread's current context; put the caller's back on every path.
        ForeignContextGuard guard;

        const bool wantPbuffer = c.surface == TESSERA_SURFACE_PBUFFER;
        auto ctx = std::make_shared<GlxContext>(sh); // RAII: any failure below cleans up through ~GlxContext

        if (TesseraResult r = chooseFbConfig(*sh, c, wantPbuffer, ctx->fbconfig); r != TESSERA_SUCCESS) return r;

        // The drawable: a pbuffer, or a hidden window that is never mapped (no window manager involvement).
        {
            XErrorScope scope(api, sh->dpy);
            if (wantPbuffer) {
                const int attribs[] = {kGlxPbufferWidth, 1, kGlxPbufferHeight, 1, kGlxPreservedContents, 0, 0};
                ctx->pbuffer = api.glx.glXCreatePbuffer(sh->dpy, ctx->fbconfig, attribs);
                ctx->drawable = ctx->pbuffer;
            } else {
                XVisualInfo* vi = api.glx.glXGetVisualFromFBConfig(sh->dpy, ctx->fbconfig);
                if (!vi) return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "the framebuffer configuration has no X visual");
                const Window root = api.xlib.XRootWindow(sh->dpy, sh->screen);
                ctx->colormap = api.xlib.XCreateColormap(sh->dpy, root, vi->visual, kAllocNone);
                XSetWindowAttributes attrs = {};
                attrs.border_pixel = 0;
                attrs.override_redirect = 1;
                attrs.event_mask = 0;
                attrs.colormap = ctx->colormap;
                ctx->window = api.xlib.XCreateWindow(sh->dpy, root, 0, 0, 1, 1, 0, vi->depth, kInputOutput, vi->visual,
                                                     kCWBorderPixel | kCWOverrideRedirect | kCWEventMask | kCWColormap, &attrs);
                api.xlib.XFree(vi);
                ctx->drawable = ctx->window;
            }
            const int e = scope.sync();
            if (e != 0 || !ctx->drawable)
                return fail(TESSERA_ERROR_PLATFORM, "could not create the drawable (X error " + std::to_string(e) + ")");
        }

        if (TesseraResult r = createNativeContext(*sh, *ctx, c, parent ? parent->ctx : nullptr); r != TESSERA_SUCCESS)
            return r;
        ctx->parent = std::move(parent);

        if (TesseraResult r = ctx->bindCurrent(); r != TESSERA_SUCCESS) return r;
        const bool ok = queryInfo(*sh, ctx->fbconfig, c, wantPbuffer, ctx->info);
        // `guard` restores the caller's context; it must not be left to us.
        if (!ok) return fail(TESSERA_ERROR_PLATFORM, "could not query the new context (glGetString failed)");

        // Strict mode promises exactly what was asked for: verify the parts the driver may have loosened.
        if (c.fallback == TESSERA_FALLBACK_STRICT && !contextSatisfies(c, ctx->info))
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "the created context does not satisfy the strict request");

        out = std::move(ctx);
        return TESSERA_SUCCESS;
    }

    void* getProcAddress(const char* name) noexcept override {
        const LinuxApi* api = linuxApi();
        if (!api || !name) return nullptr;
        // GLVND hands out a dispatch stub for any name it is asked about; every GL/GLX function starts with
        // "gl", so refuse the rest. (A made-up "gl*" name can still resolve to a no-op stub - that is the GLX
        // contract; check the extension strings, not the pointer, to learn what is supported.)
        if (name[0] != 'g' || name[1] != 'l') return nullptr;
        return api->glx.glXGetProcAddress(reinterpret_cast<const unsigned char*>(name));
    }

  private:
    static TesseraResult createNativeContext(GlxShared& sh, GlxContext& ctx, const TesseraContextConfig& c,
                                             GLXContext shareCtx) {
        const LinuxApi& api = *sh.api;

        ArbContextPlan plan;
        const TesseraResult planned = planArbContext(
            c, "GLX_", sh.createContextAttribs != nullptr, [&sh](const char* e) { return sh.has(e); }, plan);
        if (planned != TESSERA_SUCCESS) return planned;

        if (plan.legacy) {
            XErrorScope scope(api, sh.dpy);
            ctx.ctx = api.glx.glXCreateNewContext(sh.dpy, ctx.fbconfig, kGlxRgbaType, shareCtx, 1);
            const int e = scope.sync();
            if (!ctx.ctx || e != 0) {
                ctx.ctx = nullptr;
                return fail(TESSERA_ERROR_PLATFORM, "glXCreateNewContext failed (X error " + std::to_string(e) + ")");
            }
            return TESSERA_SUCCESS;
        }

        int lastError = 0;
        for (const auto& version : plan.versions) {
            const std::vector<int> attribs = plan.attribs(version);
            XErrorScope scope(api, sh.dpy);
            GLXContext created = sh.createContextAttribs(sh.dpy, ctx.fbconfig, shareCtx, 1, attribs.data());
            lastError = scope.sync();
            if (created && lastError == 0) {
                ctx.ctx = created;
                return TESSERA_SUCCESS;
            }
            // A rejected request may still have produced an object alongside the error.
            if (created) api.glx.glXDestroyContext(sh.dpy, created);
        }
        // Unsupported version/profile surfaces as BadMatch / BadValue / GLXBadFBConfig / GLXBadProfileARB.
        if (lastError == kBadAlloc) return fail(TESSERA_ERROR_OUT_OF_MEMORY, "the X server is out of memory");
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "the driver does not support the requested OpenGL version/profile");
    }

    std::shared_ptr<GlxShared> getShared(std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shared_) return shared_;
        const LinuxApi* api = linuxApi();
        if (!api) {
            error = "libX11 and libGL are not available on this system";
            return nullptr;
        }
        auto s = std::make_shared<GlxShared>();
        s->api = api;
        if (!s->open(error)) return nullptr;
        shared_ = s;
        return shared_;
    }

    std::mutex mutex_;
    std::shared_ptr<GlxShared> shared_;
};

} // namespace

std::shared_ptr<Backend> makeGlxBackend() { return std::make_shared<GlxBackend>(); }

} // namespace tessera::x11
