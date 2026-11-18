#include "wgl_backend.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/boundary.hpp"
#include "core/library.hpp"
#include "core/log.hpp"
#include "platform/common/arb_context.hpp"
#include "platform/common/gl_debug.hpp"
#include "platform/common/gl_query.hpp"
#include "wgl_api.hpp"
#include "window_host.hpp"

namespace tessera::win32 {
namespace {

// ---- WGL pixel-format constants (kept local; we do not depend on wglext.h) -------------------------
constexpr int kWglDrawToWindow = 0x2001;
constexpr int kWglSupportOpenGL = 0x2010;
constexpr int kWglDoubleBuffer = 0x2011;
constexpr int kWglStereo = 0x2012;
constexpr int kWglPixelType = 0x2013;
constexpr int kWglRedBits = 0x2015;
constexpr int kWglGreenBits = 0x2017;
constexpr int kWglBlueBits = 0x2019;
constexpr int kWglAlphaBits = 0x201B;
constexpr int kWglDepthBits = 0x2022;
constexpr int kWglStencilBits = 0x2023;
constexpr int kWglTypeRgba = 0x202B;
constexpr int kWglTypeRgbaFloat = 0x21A0;
constexpr int kWglDrawToPbuffer = 0x202D;
constexpr int kWglSampleBuffers = 0x2041;
constexpr int kWglSamples = 0x2042;
constexpr int kWglFramebufferSrgb = 0x20A9;

using PfnCreateContextAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using PfnChoosePixelFormat = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using PfnGetPixelFormatAttribiv = BOOL(WINAPI*)(HDC, int, int, UINT, const int*, int*);
using PfnGetExtensionsString = const char*(WINAPI*)(HDC);
using PfnSwapInterval = BOOL(WINAPI*)(int);
using PfnCreatePbuffer = HANDLE(WINAPI*)(HDC, int, int, int, const int*);
using PfnGetPbufferDC = HDC(WINAPI*)(HANDLE);
using PfnReleasePbufferDC = int(WINAPI*)(HANDLE, HDC);
using PfnDestroyPbuffer = BOOL(WINAPI*)(HANDLE);

struct ExtFns {
    PfnCreateContextAttribs createContextAttribs = nullptr;
    PfnChoosePixelFormat choosePixelFormat = nullptr;
    PfnGetPixelFormatAttribiv getPixelFormatAttribiv = nullptr;
    PfnGetExtensionsString getExtensionsString = nullptr;
    PfnSwapInterval swapInterval = nullptr;
    PfnCreatePbuffer createPbuffer = nullptr;
    PfnGetPbufferDC getPbufferDC = nullptr;
    PfnReleasePbufferDC releasePbufferDC = nullptr;
    PfnDestroyPbuffer destroyPbuffer = nullptr;
};

// ---- state shared by the backend and all of its contexts ------------------------------------------

class WglShared {
  public:
    const WglApi* api = nullptr;
    WindowHost host;
    ExtFns fn;

    bool has(const char* extension) const {
        return std::find(exts_.begin(), exts_.end(), extension) != exts_.end();
    }

    /* Creates a throwaway legacy context to reach the ARB entry points and read the extension list.
     * Idempotent. The caller must hold a ForeignContextGuard: this changes the thread's current context. */
    bool initExtensions(std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done_)
            return true;

        WindowHost::Window window;
        if (!host.createWindow(window, error))
            return false;

        HGLRC rc = nullptr;
        bool ok = false;
        do {
            PIXELFORMATDESCRIPTOR pfd = {};
            pfd.nSize = sizeof pfd;
            pfd.nVersion = 1;
            pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            pfd.iPixelType = PFD_TYPE_RGBA;
            pfd.cColorBits = 32;
            pfd.cDepthBits = 24;
            pfd.iLayerType = PFD_MAIN_PLANE;
            const int pf = ChoosePixelFormat(window.dc, &pfd);
            if (!pf || !SetPixelFormat(window.dc, pf, &pfd)) {
                error = describeWin32Error("no usable pixel format for the bootstrap window", GetLastError());
                break;
            }
            rc = api->createContext(window.dc);
            if (!rc) {
                error = describeWin32Error("wglCreateContext failed (is a graphics driver installed?)",
                                           GetLastError());
                break;
            }
            if (!api->makeCurrent(window.dc, rc)) {
                error = describeWin32Error("wglMakeCurrent failed on the bootstrap context", GetLastError());
                break;
            }
            resolve();
            const char* list = nullptr;
            if (fn.getExtensionsString)
                list = fn.getExtensionsString(window.dc);
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
            ok = true;
        } while (false);

        if (rc) {
            if (api->getCurrentContext() == rc)
                api->makeCurrent(nullptr, nullptr);
            api->deleteContext(rc);
        }
        host.destroyWindow(window);
        if (ok) {
            done_ = true;
            logf(TESSERA_LOG_DEBUG, "WGL: %zu extensions, ARB_create_context=%d ARB_pixel_format=%d",
                 exts_.size(), fn.createContextAttribs != nullptr, fn.choosePixelFormat != nullptr);
        }
        return ok;
    }

  private:
    template <class Fn> void get(const char* name, Fn& out) {
        PROC p = api->getProcAddress(name);
        out = isPlausibleProcAddress(reinterpret_cast<void*>(p)) ? reinterpret_cast<Fn>(p) : nullptr;
    }
    void resolve() {
        get("wglCreateContextAttribsARB", fn.createContextAttribs);
        get("wglChoosePixelFormatARB", fn.choosePixelFormat);
        get("wglGetPixelFormatAttribivARB", fn.getPixelFormatAttribiv);
        get("wglGetExtensionsStringARB", fn.getExtensionsString);
        if (!fn.getExtensionsString)
            get("wglGetExtensionsStringEXT", fn.getExtensionsString);
        get("wglSwapIntervalEXT", fn.swapInterval);
        get("wglCreatePbufferARB", fn.createPbuffer);
        get("wglGetPbufferDCARB", fn.getPbufferDC);
        get("wglReleasePbufferDCARB", fn.releasePbufferDC);
        get("wglDestroyPbufferARB", fn.destroyPbuffer);
    }

    std::mutex mutex_;
    bool done_ = false;
    std::vector<std::string> exts_;
};

// ---- a native context ---------------------------------------------------------------------------

class WglContext final : public ContextObject {
  public:
    explicit WglContext(std::shared_ptr<WglShared> shared) : shared_(std::move(shared)) {}
    ~WglContext() override {
        destroy();
    }

    TesseraResult describe(TesseraContextInfo& out) const noexcept override {
        out = info;
        return TESSERA_SUCCESS;
    }

    TesseraResult nativeHandles(TesseraNativeHandles& out) const noexcept override {
        out.kind = TESSERA_NATIVE_WGL;
        out.handles[0] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(rc));
        out.handles[1] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(dc));
        out.handles[2] = static_cast<uint64_t>(
            reinterpret_cast<std::uintptr_t>(foreignWindow ? foreignWindow : window.hwnd));
        out.handles[3] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(pbuffer));
        out.handles[4] = static_cast<uint64_t>(pixelFormat);
        return TESSERA_SUCCESS;
    }

    TesseraResult bindCurrent() noexcept override {
        if (foreignWindow && !IsWindow(foreignWindow))
            return fail(TESSERA_ERROR_PLATFORM, "the window this context renders into no longer exists");
        if (!shared_->api->makeCurrent(dc, rc))
            return fail(TESSERA_ERROR_PLATFORM, describeWin32Error("wglMakeCurrent failed", GetLastError()));
        return TESSERA_SUCCESS;
    }

    void unbindCurrent() noexcept override {
        if (rc && shared_->api->getCurrentContext() == rc)
            shared_->api->makeCurrent(nullptr, nullptr);
    }

    TesseraResult swapBuffers() noexcept override {
        if (pbuffer)
            return TESSERA_SUCCESS; // nothing to present
        if (foreignWindow && !IsWindow(foreignWindow))
            return fail(TESSERA_ERROR_PLATFORM, "the window this context renders into no longer exists");
        if (!SwapBuffers(dc))
            return fail(TESSERA_ERROR_PLATFORM, describeWin32Error("SwapBuffers failed", GetLastError()));
        return TESSERA_SUCCESS;
    }

    TesseraResult setSwapInterval(int32_t interval) noexcept override {
        if (!shared_->fn.swapInterval || !shared_->has("WGL_EXT_swap_control"))
            return fail(TESSERA_ERROR_UNSUPPORTED, "WGL_EXT_swap_control is not available");
        if (interval < 0 && !shared_->has("WGL_EXT_swap_control_tear"))
            return fail(TESSERA_ERROR_INVALID_ARGUMENT,
                        "adaptive vsync (WGL_EXT_swap_control_tear) is not available");
        if (pbuffer)
            return fail(TESSERA_ERROR_UNSUPPORTED, "swap interval does not apply to pbuffers");
        if (!shared_->fn.swapInterval(interval))
            return fail(TESSERA_ERROR_PLATFORM,
                        describeWin32Error("wglSwapIntervalEXT failed", GetLastError()));
        return TESSERA_SUCCESS;
    }

    TesseraResult queryResetStatus(int32_t& out) noexcept override {
        return queryResetStatusFrom(resetFn, info.flags, out);
    }

    GlResetStatusFn resetFn = nullptr;
    std::shared_ptr<WglShared> shared_;
    WindowHost::Window window;
    HWND foreignWindow =
        nullptr; // a caller's window we render into (borrowed: never destroyed, only its DC is released)
    HANDLE pbuffer = nullptr;
    HDC dc = nullptr; // window DC, or pbuffer DC
    int pixelFormat = 0;
    HGLRC rc = nullptr;
    std::shared_ptr<WglContext> parent; // keeps a share parent's native context alive as long as we are
    TesseraContextInfo info = {};

  private:
    void destroy() noexcept {
        const WglApi* api = shared_->api;
        if (rc) {
            if (api->getCurrentContext() == rc)
                api->makeCurrent(nullptr, nullptr);
            api->deleteContext(rc);
            rc = nullptr;
        }
        if (pbuffer) {
            if (dc && shared_->fn.releasePbufferDC)
                shared_->fn.releasePbufferDC(pbuffer, dc);
            if (shared_->fn.destroyPbuffer)
                shared_->fn.destroyPbuffer(pbuffer);
            pbuffer = nullptr;
        }
        shared_->host.destroyWindow(window);
        if (foreignWindow && dc)
            ReleaseDC(foreignWindow, dc); // the window itself is not ours to destroy
        dc = nullptr;
    }
};

// ---- pixel format negotiation ---------------------------------------------------------------------

std::vector<int> pixelFormatAttribs(const TesseraContextConfig& c, const FormatAttempt& a, bool pbuffer) {
    std::vector<int> v;
    auto add = [&v](int key, int value) {
        v.push_back(key);
        v.push_back(value);
    };
    add(pbuffer ? kWglDrawToPbuffer : kWglDrawToWindow, 1);
    add(kWglSupportOpenGL, 1);
    const bool isFloat = (c.flags & TESSERA_FLAG_FLOAT_COLOR) != 0;
    add(kWglPixelType, isFloat ? kWglTypeRgbaFloat : kWglTypeRgba);
    if (c.flags & TESSERA_FLAG_DOUBLE_BUFFER)
        add(kWglDoubleBuffer, 1);
    if (c.flags & TESSERA_FLAG_STEREO)
        add(kWglStereo, 1);

    int r = c.redBits, g = c.greenBits, b = c.blueBits;
    if (r == 0 && g == 0 && b == 0)
        r = g = b = isFloat ? 16 : 8; // sane default for "don't care"
    add(kWglRedBits, r);
    add(kWglGreenBits, g);
    add(kWglBlueBits, b);
    if (c.alphaBits > 0)
        add(kWglAlphaBits, c.alphaBits);
    if (!a.relaxDepth) {
        if (c.depthBits > 0)
            add(kWglDepthBits, c.depthBits);
        if (c.stencilBits > 0)
            add(kWglStencilBits, c.stencilBits);
    }
    if (a.samples > 0) {
        add(kWglSampleBuffers, 1);
        add(kWglSamples, a.samples);
    }
    if (a.srgb)
        add(kWglFramebufferSrgb, 1);
    v.push_back(0);
    return v;
}

bool setPixelFormat(HDC dc, int pf) {
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    DescribePixelFormat(dc, pf, sizeof pfd, &pfd); // may fail for ARB-only formats; pfd stays a valid stub
    return SetPixelFormat(dc, pf, &pfd) != FALSE;
}

/* Legacy GDI path, used when WGL_ARB_pixel_format is unavailable or (best effort) found nothing. */
int chooseLegacyFormat(HDC dc, const TesseraContextConfig& c) {
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    if (c.flags & TESSERA_FLAG_DOUBLE_BUFFER)
        pfd.dwFlags |= PFD_DOUBLEBUFFER;
    if (c.flags & TESSERA_FLAG_STEREO)
        pfd.dwFlags |= PFD_STEREO;
    pfd.iPixelType = PFD_TYPE_RGBA;
    const int rgb = c.redBits + c.greenBits + c.blueBits;
    pfd.cColorBits = static_cast<BYTE>(rgb > 0 ? rgb : 24);
    pfd.cAlphaBits = static_cast<BYTE>(c.alphaBits);
    pfd.cDepthBits = static_cast<BYTE>(c.depthBits);
    pfd.cStencilBits = static_cast<BYTE>(c.stencilBits);
    pfd.iLayerType = PFD_MAIN_PLANE;
    return ChoosePixelFormat(dc, &pfd);
}

/* Picks and (for windows) applies a pixel format. On success `pfOut` is set. */
TesseraResult negotiatePixelFormat(const WglShared& sh, HDC dc, const TesseraContextConfig& c, bool pbuffer,
                                   int& pfOut) {
    const bool strict = c.fallback == TESSERA_FALLBACK_STRICT;
    const bool haveArb = sh.fn.choosePixelFormat != nullptr && sh.has("WGL_ARB_pixel_format");

    // Features that need extensions: strict mode refuses, best effort drops them.
    FormatRequest request;
    if (TesseraResult r = gateFormatFeatures(
            c, "WGL_", [&sh](const char* e) { return sh.has(e); }, request);
        r != TESSERA_SUCCESS)
        return r;
    const int samples = request.samples;
    const bool srgb = request.srgb;
    const bool wantFloat = (c.flags & TESSERA_FLAG_FLOAT_COLOR) != 0;

    if (haveArb) {
        const std::vector<FormatAttempt> attempts = formatAttempts(samples, srgb, strict);
        for (const FormatAttempt& a : attempts) {
            const std::vector<int> attribs = pixelFormatAttribs(c, a, pbuffer);
            int pf = 0;
            UINT count = 0;
            if (sh.fn.choosePixelFormat(dc, attribs.data(), nullptr, 1, &pf, &count) && count > 0 && pf > 0) {
                if (pbuffer || setPixelFormat(dc, pf)) {
                    pfOut = pf;
                    if (a.samples != samples || a.srgb != srgb || a.relaxDepth)
                        logf(TESSERA_LOG_INFO,
                             "WGL: pixel format relaxed (samples %d->%d, srgb %d, depth relaxed %d)",
                             c.samples, a.samples, a.srgb, a.relaxDepth);
                    return TESSERA_SUCCESS;
                }
            }
        }
        if (strict || pbuffer)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "no pixel format matches the requested configuration");
    } else {
        if (pbuffer)
            return fail(TESSERA_ERROR_UNSUPPORTED, "pbuffers need WGL_ARB_pixel_format");
        if (strict && (c.samples > 0 || srgb || wantFloat))
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "WGL_ARB_pixel_format is required for this configuration");
    }

    const int pf = chooseLegacyFormat(dc, c);
    if (pf > 0 && setPixelFormat(dc, pf)) {
        pfOut = pf;
        return TESSERA_SUCCESS;
    }
    return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "no pixel format matches the requested configuration");
}

// ---- native context creation ------------------------------------------------------------------------

TesseraResult createNativeContext(WglShared& sh, HDC dc, const TesseraContextConfig& c, HGLRC shareRc,
                                  HGLRC& rcOut) {
    const WglApi& api = *sh.api;

    ArbContextPlan plan;
    const TesseraResult planned = planArbContext(
        c, "WGL_", sh.fn.createContextAttribs != nullptr, [&sh](const char* e) { return sh.has(e); }, plan);
    if (planned != TESSERA_SUCCESS)
        return planned;

    if (plan.legacy) {
        HGLRC rc = api.createContext(dc);
        if (!rc)
            return fail(TESSERA_ERROR_PLATFORM,
                        describeWin32Error("wglCreateContext failed", GetLastError()));
        if (shareRc && !api.shareLists(shareRc, rc)) {
            const DWORD e = GetLastError();
            api.deleteContext(rc);
            return fail(TESSERA_ERROR_PLATFORM, describeWin32Error("wglShareLists failed", e));
        }
        rcOut = rc;
        return TESSERA_SUCCESS;
    }

    DWORD lastError = 0;
    for (const auto& version : plan.versions) {
        const std::vector<int> attribs = plan.attribs(version);
        SetLastError(0);
        HGLRC rc = sh.fn.createContextAttribs(dc, shareRc, attribs.data());
        if (rc) {
            rcOut = rc;
            return TESSERA_SUCCESS;
        }
        lastError = GetLastError();
    }

    // The driver rejected the request. The ARB spec names ERROR_INVALID_VERSION_ARB /
    // ERROR_INVALID_PROFILE_ARB, but drivers also answer a combination they dislike with plain Win32 codes
    // (NVIDIA: ERROR_INVALID_DATA), and put various high bits on all of them. Out-of-memory is the only
    // failure that is not about the request itself.
    const DWORD code = lastError & 0xFFFFu;
    if (code == 0x2095 || code == 0x2096 || code == 0x000D /*ERROR_INVALID_DATA*/ ||
        code == 0x0057 /*ERROR_INVALID_PARAMETER*/)
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                    "the driver does not support the requested OpenGL version/profile/flags");
    if (code == 0x0008 /*ERROR_NOT_ENOUGH_MEMORY*/ || code == 0x000E /*ERROR_OUTOFMEMORY*/)
        return fail(TESSERA_ERROR_OUT_OF_MEMORY,
                    describeWin32Error("wglCreateContextAttribsARB failed", lastError));
    return fail(TESSERA_ERROR_PLATFORM, describeWin32Error("wglCreateContextAttribsARB failed", lastError));
}

// ---- reporting what we actually got -------------------------------------------------------------

/* Requires the context to be current on the calling thread. */
bool queryInfo(const WglShared& sh, HDC dc, int pf, const TesseraContextConfig& c, bool pbuffer,
               TesseraContextInfo& info) {
    const WglApi& api = *sh.api;
    GlQueryFns gl;
    gl.getString = reinterpret_cast<decltype(gl.getString)>(api.lib.symbol("glGetString"));
    gl.getIntegerv = reinterpret_cast<decltype(gl.getIntegerv)>(api.lib.symbol("glGetIntegerv"));
    gl.getError = reinterpret_cast<decltype(gl.getError)>(api.lib.symbol("glGetError"));
    if (!queryContextInfo(gl, info))
        return false;

    uint32_t flags = info.flags;

    // Pixel format: prefer the ARB query (also valid for ARB-only formats), fall back to GDI.
    bool queried = false;
    if (sh.fn.getPixelFormatAttribiv) {
        const int keys[] = {kWglRedBits,     kWglGreenBits,    kWglBlueBits, kWglAlphaBits, kWglDepthBits,
                            kWglStencilBits, kWglDoubleBuffer, kWglStereo,   kWglSamples,   kWglPixelType};
        int values[sizeof keys / sizeof keys[0]] = {};
        if (sh.fn.getPixelFormatAttribiv(dc, pf, 0, static_cast<UINT>(sizeof keys / sizeof keys[0]), keys,
                                         values)) {
            info.redBits = values[0];
            info.greenBits = values[1];
            info.blueBits = values[2];
            info.alphaBits = values[3];
            info.depthBits = values[4];
            info.stencilBits = values[5];
            if (values[6])
                flags |= TESSERA_FLAG_DOUBLE_BUFFER;
            if (values[7])
                flags |= TESSERA_FLAG_STEREO;
            info.samples = values[8];
            if (values[9] == kWglTypeRgbaFloat)
                flags |= TESSERA_FLAG_FLOAT_COLOR;
            queried = true;
            const int srgbKey = kWglFramebufferSrgb;
            int srgb = 0;
            if ((sh.has("WGL_ARB_framebuffer_sRGB") || sh.has("WGL_EXT_framebuffer_sRGB")) &&
                sh.fn.getPixelFormatAttribiv(dc, pf, 0, 1, &srgbKey, &srgb) && srgb)
                flags |= TESSERA_FLAG_SRGB;
        }
    }
    if (!queried) {
        PIXELFORMATDESCRIPTOR pfd = {};
        if (DescribePixelFormat(dc, pf, sizeof pfd, &pfd)) {
            info.redBits = pfd.cRedBits;
            info.greenBits = pfd.cGreenBits;
            info.blueBits = pfd.cBlueBits;
            info.alphaBits = pfd.cAlphaBits;
            info.depthBits = pfd.cDepthBits;
            info.stencilBits = pfd.cStencilBits;
            if (pfd.dwFlags & PFD_DOUBLEBUFFER)
                flags |= TESSERA_FLAG_DOUBLE_BUFFER;
            if (pfd.dwFlags & PFD_STEREO)
                flags |= TESSERA_FLAG_STEREO;
        }
    }
    info.flags = flags;
    info.surface =
        pbuffer ? TESSERA_SURFACE_PBUFFER
                : (c.surface == TESSERA_SURFACE_NONE ? TESSERA_SURFACE_NONE : TESSERA_SURFACE_HIDDEN_WINDOW);
    return true;
}

// ---- the backend --------------------------------------------------------------------------------

class WglBackend final : public Backend {
  public:
    const char* name() const noexcept override {
        return "wgl";
    }

    TesseraResult createContext(const TesseraContextConfig& c,
                                const std::shared_ptr<ContextObject>& shareWith,
                                std::shared_ptr<ContextObject>& out) override {
        std::string error;
        std::shared_ptr<WglShared> sh = getShared(error);
        if (!sh)
            return fail(TESSERA_ERROR_UNSUPPORTED, error);

        std::shared_ptr<WglContext> parent;
        if (shareWith) {
            parent = std::dynamic_pointer_cast<WglContext>(shareWith);
            if (!parent)
                return fail(TESSERA_ERROR_INVALID_ARGUMENT,
                            "cannot share objects with a context from another backend");
        }

        // Everything below may change this thread's current context; put the caller's back on every path.
        ForeignContextGuard guard;

        if (!sh->initExtensions(error))
            return fail(TESSERA_ERROR_PLATFORM, error);

        const bool wantPbuffer = c.surface == TESSERA_SURFACE_PBUFFER;
        if (wantPbuffer && !(sh->fn.createPbuffer && sh->fn.getPbufferDC && sh->fn.releasePbufferDC &&
                             sh->fn.destroyPbuffer && sh->has("WGL_ARB_pbuffer")))
            return fail(TESSERA_ERROR_UNSUPPORTED, "WGL_ARB_pbuffer is not available");

        auto ctx = std::make_shared<WglContext>(sh); // RAII: any failure below cleans up through ~WglContext
        if (!sh->host.createWindow(ctx->window, error))
            return fail(TESSERA_ERROR_PLATFORM, error);
        ctx->dc = ctx->window.dc;

        if (TesseraResult r = negotiatePixelFormat(*sh, ctx->dc, c, wantPbuffer, ctx->pixelFormat);
            r != TESSERA_SUCCESS)
            return r;

        if (wantPbuffer) {
            ctx->pbuffer = sh->fn.createPbuffer(ctx->window.dc, ctx->pixelFormat, 1, 1, nullptr);
            if (!ctx->pbuffer)
                return fail(TESSERA_ERROR_PLATFORM,
                            describeWin32Error("wglCreatePbufferARB failed", GetLastError()));
            ctx->dc = sh->fn.getPbufferDC(ctx->pbuffer);
            if (!ctx->dc)
                return fail(TESSERA_ERROR_PLATFORM,
                            describeWin32Error("wglGetPbufferDCARB failed", GetLastError()));
        }

        if (TesseraResult r = createNativeContext(*sh, ctx->dc, c, parent ? parent->rc : nullptr, ctx->rc);
            r != TESSERA_SUCCESS)
            return r;
        ctx->parent = std::move(parent);

        return finishContext(*sh, ctx, c, wantPbuffer, false, out);
    }

    /*
     * Renders into a caller's window. Ownership rules (see tessera.h): the window is borrowed; an existing
     * pixel format is adopted and checked against the request; a missing one is set only if the caller
     * allowed it, because a window's pixel format can be set exactly once and a window that is not ours must
     * not be changed behind its owner's back.
     */
    TesseraResult createContextForWindow(const TesseraContextConfig& c, uint64_t nativeWindow, uint32_t flags,
                                         const std::shared_ptr<ContextObject>& shareWith,
                                         std::shared_ptr<ContextObject>& out) override {
        std::string error;
        std::shared_ptr<WglShared> sh = getShared(error);
        if (!sh)
            return fail(TESSERA_ERROR_UNSUPPORTED, error);

        std::shared_ptr<WglContext> parent;
        if (shareWith) {
            parent = std::dynamic_pointer_cast<WglContext>(shareWith);
            if (!parent)
                return fail(TESSERA_ERROR_INVALID_ARGUMENT,
                            "cannot share objects with a context from another backend");
        }
        HWND hwnd = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(nativeWindow));
        if (!IsWindow(hwnd))
            return fail(TESSERA_ERROR_INVALID_ARGUMENT, "nativeWindow is not a window handle");

        ForeignContextGuard guard;
        if (!sh->initExtensions(error))
            return fail(TESSERA_ERROR_PLATFORM, error);

        auto ctx = std::make_shared<WglContext>(sh); // RAII: failure paths release the DC through ~WglContext
        ctx->foreignWindow = hwnd;
        ctx->dc = GetDC(hwnd);
        if (!ctx->dc) {
            ctx->foreignWindow = nullptr; // no DC to release
            return fail(TESSERA_ERROR_PLATFORM,
                        describeWin32Error("GetDC failed on the window", GetLastError()));
        }

        const int existing = GetPixelFormat(ctx->dc);
        if (existing != 0) {
            ctx->pixelFormat = existing; // adopted as is; strict mode verifies it against the request below
        } else {
            if (!(flags & TESSERA_ATTACH_ALLOW_PIXEL_FORMAT))
                return fail(TESSERA_ERROR_INVALID_ARGUMENT,
                            "the window has no pixel format and TESSERA_ATTACH_ALLOW_PIXEL_FORMAT was not "
                            "given: Tessera does not modify windows it does not own");
            if (TesseraResult r = negotiatePixelFormat(*sh, ctx->dc, c, false, ctx->pixelFormat);
                r != TESSERA_SUCCESS)
                return r;
        }

        if (TesseraResult r = createNativeContext(*sh, ctx->dc, c, parent ? parent->rc : nullptr, ctx->rc);
            r != TESSERA_SUCCESS)
            return r;
        ctx->parent = std::move(parent);
        return finishContext(*sh, ctx, c, false, true, out);
    }

    void* getProcAddress(const char* name) noexcept override {
        const WglApi* api = wglApi();
        if (!api || !name)
            return nullptr;
        void* p =
            reinterpret_cast<void*>(api->getProcAddress(name)); // only resolves while a context is current
        if (isPlausibleProcAddress(p))
            return p;
        return api->lib.symbol(name); // OpenGL 1.1 functions are exported by opengl32.dll itself
    }

  private:
    /* Makes the new context current once to describe it, verifies strict requests, and binds the per-context
     * extras. */
    static TesseraResult finishContext(WglShared& sh, const std::shared_ptr<WglContext>& ctx,
                                       const TesseraContextConfig& c, bool pbuffer, bool window,
                                       std::shared_ptr<ContextObject>& out) {
        if (!sh.api->makeCurrent(ctx->dc, ctx->rc))
            return fail(TESSERA_ERROR_PLATFORM,
                        describeWin32Error("wglMakeCurrent failed on the new context", GetLastError()));
        const bool ok = queryInfo(sh, ctx->dc, ctx->pixelFormat, c, pbuffer, ctx->info);
        // The caller's ForeignContextGuard restores their context; it must not be left to us.
        if (!ok)
            return fail(TESSERA_ERROR_PLATFORM, "could not query the new context (glGetString failed)");
        if (window)
            ctx->info.surface = TESSERA_SURFACE_WINDOW;

        // Strict mode promises exactly what was asked for: verify the parts the driver may have loosened.
        if (c.fallback == TESSERA_FALLBACK_STRICT && !contextSatisfies(c, ctx->info))
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "the created context does not satisfy the strict request");

        // Reset polling and debug-message forwarding are bound to the context that is current right now.
        const WglApi* api = sh.api;
        const auto glProc = [api](const char* n) -> void* {
            void* p = reinterpret_cast<void*>(api->getProcAddress(n));
            return isPlausibleProcAddress(p) ? p : api->lib.symbol(n);
        };
        ctx->resetFn = resolveResetStatus(glProc);
        if (ctx->info.flags & TESSERA_FLAG_DEBUG)
            installGlDebugForwarder(glProc);

        out = ctx;
        return TESSERA_SUCCESS;
    }

    std::shared_ptr<WglShared> getShared(std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shared_)
            return shared_;
        const WglApi* api = wglApi();
        if (!api) {
            error = "opengl32.dll is not available on this system";
            return nullptr;
        }
        auto s = std::make_shared<WglShared>();
        s->api = api;
        if (!s->host.start(error))
            return nullptr;
        shared_ = s;
        return shared_;
    }

    std::mutex mutex_;
    std::shared_ptr<WglShared> shared_;
};

} // namespace

std::shared_ptr<Backend> makeWglBackend() {
    return std::make_shared<WglBackend>();
}

} // namespace tessera::win32
