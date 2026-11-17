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
#include "window_host.hpp"
#include "wgl_api.hpp"

namespace tessera::win32 {
namespace {

// ---- WGL / GL constants (kept local; we do not depend on wglext.h) ---------------------------------
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

constexpr int kCtxMajor = 0x2091;
constexpr int kCtxMinor = 0x2092;
constexpr int kCtxFlags = 0x2094;
constexpr int kCtxProfileMask = 0x9126;
constexpr int kCtxNoError = 0x31B3;
constexpr int kCtxResetStrategy = 0x8256;
constexpr int kCtxLoseOnReset = 0x8252;
constexpr int kCtxReleaseBehavior = 0x2097;
constexpr int kCtxReleaseFlush = 0x2098;

constexpr int kProfileCore = 0x1;
constexpr int kProfileCompat = 0x2;
constexpr int kProfileEs2 = 0x4;
constexpr int kFlagDebug = 0x1;
constexpr int kFlagForward = 0x2;
constexpr int kFlagRobust = 0x4;

constexpr unsigned kGlVersion = 0x1F02, kGlVendor = 0x1F00, kGlRenderer = 0x1F01;
constexpr unsigned kGlContextFlags = 0x821E, kGlProfileMask = 0x9126, kGlResetStrategy = 0x8256;

using PfnCreateContextAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using PfnChoosePixelFormat = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using PfnGetPixelFormatAttribiv = BOOL(WINAPI*)(HDC, int, int, UINT, const int*, int*);
using PfnGetExtensionsString = const char*(WINAPI*)(HDC);
using PfnSwapInterval = BOOL(WINAPI*)(int);
using PfnCreatePbuffer = HANDLE(WINAPI*)(HDC, int, int, int, const int*);
using PfnGetPbufferDC = HDC(WINAPI*)(HANDLE);
using PfnReleasePbufferDC = int(WINAPI*)(HANDLE, HDC);
using PfnDestroyPbuffer = BOOL(WINAPI*)(HANDLE);

using PfnGlGetString = const unsigned char*(WINAPI*)(unsigned);
using PfnGlGetIntegerv = void(WINAPI*)(unsigned, int*);
using PfnGlGetError = unsigned(WINAPI*)();

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

inline void copyString(char* dst, std::size_t cap, const char* src) noexcept {
    std::size_t i = 0;
    if (src)
        for (; i + 1 < cap && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

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
        if (done_) return true;

        WindowHost::Window window;
        if (!host.createWindow(window, error)) return false;

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
                error = describeWin32Error("wglCreateContext failed (is a graphics driver installed?)", GetLastError());
                break;
            }
            if (!api->makeCurrent(window.dc, rc)) {
                error = describeWin32Error("wglMakeCurrent failed on the bootstrap context", GetLastError());
                break;
            }
            resolve();
            const char* list = nullptr;
            if (fn.getExtensionsString) list = fn.getExtensionsString(window.dc);
            if (list) {
                const char* p = list;
                while (*p) {
                    const char* e = p;
                    while (*e && *e != ' ') ++e;
                    if (e > p) exts_.emplace_back(p, static_cast<std::size_t>(e - p));
                    p = *e ? e + 1 : e;
                }
            }
            ok = true;
        } while (false);

        if (rc) {
            if (api->getCurrentContext() == rc) api->makeCurrent(nullptr, nullptr);
            api->deleteContext(rc);
        }
        host.destroyWindow(window);
        if (ok) {
            done_ = true;
            logf(TESSERA_LOG_DEBUG, "WGL: %zu extensions, ARB_create_context=%d ARB_pixel_format=%d", exts_.size(),
                 fn.createContextAttribs != nullptr, fn.choosePixelFormat != nullptr);
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
        if (!fn.getExtensionsString) get("wglGetExtensionsStringEXT", fn.getExtensionsString);
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
    ~WglContext() override { destroy(); }

    TesseraResult describe(TesseraContextInfo& out) const noexcept override {
        out = info;
        return TESSERA_SUCCESS;
    }

    TesseraResult bindCurrent() noexcept override {
        if (!shared_->api->makeCurrent(dc, rc))
            return fail(TESSERA_ERROR_PLATFORM, describeWin32Error("wglMakeCurrent failed", GetLastError()));
        return TESSERA_SUCCESS;
    }

    void unbindCurrent() noexcept override {
        if (rc && shared_->api->getCurrentContext() == rc) shared_->api->makeCurrent(nullptr, nullptr);
    }

    TesseraResult swapBuffers() noexcept override {
        if (pbuffer) return TESSERA_SUCCESS; // nothing to present
        if (!SwapBuffers(dc))
            return fail(TESSERA_ERROR_PLATFORM, describeWin32Error("SwapBuffers failed", GetLastError()));
        return TESSERA_SUCCESS;
    }

    TesseraResult setSwapInterval(int32_t interval) noexcept override {
        if (!shared_->fn.swapInterval || !shared_->has("WGL_EXT_swap_control"))
            return fail(TESSERA_ERROR_UNSUPPORTED, "WGL_EXT_swap_control is not available");
        if (interval < 0 && !shared_->has("WGL_EXT_swap_control_tear"))
            return fail(TESSERA_ERROR_INVALID_ARGUMENT, "adaptive vsync (WGL_EXT_swap_control_tear) is not available");
        if (pbuffer) return fail(TESSERA_ERROR_UNSUPPORTED, "swap interval does not apply to pbuffers");
        if (!shared_->fn.swapInterval(interval))
            return fail(TESSERA_ERROR_PLATFORM, describeWin32Error("wglSwapIntervalEXT failed", GetLastError()));
        return TESSERA_SUCCESS;
    }

    std::shared_ptr<WglShared> shared_;
    WindowHost::Window window;
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
            if (api->getCurrentContext() == rc) api->makeCurrent(nullptr, nullptr);
            api->deleteContext(rc);
            rc = nullptr;
        }
        if (pbuffer) {
            if (dc && shared_->fn.releasePbufferDC) shared_->fn.releasePbufferDC(pbuffer, dc);
            if (shared_->fn.destroyPbuffer) shared_->fn.destroyPbuffer(pbuffer);
            pbuffer = nullptr;
        }
        shared_->host.destroyWindow(window);
        dc = nullptr;
    }
};

// ---- pixel format negotiation ---------------------------------------------------------------------

struct Attempt {
    int samples;
    bool srgb;
    bool relaxDepth;
};

std::vector<int> pixelFormatAttribs(const TesseraContextConfig& c, const Attempt& a, bool pbuffer) {
    std::vector<int> v;
    auto add = [&v](int key, int value) {
        v.push_back(key);
        v.push_back(value);
    };
    add(pbuffer ? kWglDrawToPbuffer : kWglDrawToWindow, 1);
    add(kWglSupportOpenGL, 1);
    const bool isFloat = (c.flags & TESSERA_FLAG_FLOAT_COLOR) != 0;
    add(kWglPixelType, isFloat ? kWglTypeRgbaFloat : kWglTypeRgba);
    if (c.flags & TESSERA_FLAG_DOUBLE_BUFFER) add(kWglDoubleBuffer, 1);
    if (c.flags & TESSERA_FLAG_STEREO) add(kWglStereo, 1);

    int r = c.redBits, g = c.greenBits, b = c.blueBits;
    if (r == 0 && g == 0 && b == 0) r = g = b = isFloat ? 16 : 8; // sane default for "don't care"
    add(kWglRedBits, r);
    add(kWglGreenBits, g);
    add(kWglBlueBits, b);
    if (c.alphaBits > 0) add(kWglAlphaBits, c.alphaBits);
    if (!a.relaxDepth) {
        if (c.depthBits > 0) add(kWglDepthBits, c.depthBits);
        if (c.stencilBits > 0) add(kWglStencilBits, c.stencilBits);
    }
    if (a.samples > 0) {
        add(kWglSampleBuffers, 1);
        add(kWglSamples, a.samples);
    }
    if (a.srgb) add(kWglFramebufferSrgb, 1);
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
    if (c.flags & TESSERA_FLAG_DOUBLE_BUFFER) pfd.dwFlags |= PFD_DOUBLEBUFFER;
    if (c.flags & TESSERA_FLAG_STEREO) pfd.dwFlags |= PFD_STEREO;
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
    int samples = c.samples;
    bool srgb = (c.flags & TESSERA_FLAG_SRGB) != 0;
    const bool wantFloat = (c.flags & TESSERA_FLAG_FLOAT_COLOR) != 0;
    const bool haveMs = sh.has("WGL_ARB_multisample");
    const bool haveSrgb = sh.has("WGL_ARB_framebuffer_sRGB") || sh.has("WGL_EXT_framebuffer_sRGB");
    const bool haveFloat = sh.has("WGL_ARB_pixel_format_float");
    if (samples > 0 && !haveMs) {
        if (strict) return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "multisampling (WGL_ARB_multisample) is not available");
        samples = 0;
    }
    if (srgb && !haveSrgb) {
        if (strict) return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "sRGB framebuffers are not available");
        srgb = false;
    }
    if (wantFloat && !haveFloat) return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "float color buffers are not available");

    if (haveArb) {
        std::vector<Attempt> attempts;
        for (int s = samples;; s = s > 1 ? s / 2 : 0) {
            attempts.push_back({s, srgb, false});
            if (!strict) {
                if (srgb) attempts.push_back({s, false, false});
                attempts.push_back({s, false, true});
            }
            if (strict || s == 0) break;
        }
        for (const Attempt& a : attempts) {
            const std::vector<int> attribs = pixelFormatAttribs(c, a, pbuffer);
            int pf = 0;
            UINT count = 0;
            if (sh.fn.choosePixelFormat(dc, attribs.data(), nullptr, 1, &pf, &count) && count > 0 && pf > 0) {
                if (pbuffer || setPixelFormat(dc, pf)) {
                    pfOut = pf;
                    if (a.samples != samples || a.srgb != srgb || a.relaxDepth)
                        logf(TESSERA_LOG_INFO, "WGL: pixel format relaxed (samples %d->%d, srgb %d, depth relaxed %d)",
                             c.samples, a.samples, a.srgb, a.relaxDepth);
                    return TESSERA_SUCCESS;
                }
            }
        }
        if (strict || pbuffer)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "no pixel format matches the requested configuration");
    } else {
        if (pbuffer) return fail(TESSERA_ERROR_UNSUPPORTED, "pbuffers need WGL_ARB_pixel_format");
        if (strict && (c.samples > 0 || srgb || wantFloat))
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "WGL_ARB_pixel_format is required for this configuration");
    }

    const int pf = chooseLegacyFormat(dc, c);
    if (pf > 0 && setPixelFormat(dc, pf)) {
        pfOut = pf;
        return TESSERA_SUCCESS;
    }
    return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "no pixel format matches the requested configuration");
}

// ---- context attribute negotiation ------------------------------------------------------------------

/* True when plain wglCreateContext gives exactly what was asked for (no version/profile/flags). */
bool legacyContextSuffices(const TesseraContextConfig& c) {
    constexpr uint32_t kContextFlags = TESSERA_FLAG_DEBUG | TESSERA_FLAG_FORWARD_COMPATIBLE | TESSERA_FLAG_ROBUST_ACCESS |
                                       TESSERA_FLAG_RESET_NOTIFICATION | TESSERA_FLAG_NO_ERROR |
                                       TESSERA_FLAG_RELEASE_FLUSH;
    return c.api == TESSERA_API_OPENGL && c.profile == TESSERA_PROFILE_ANY && c.versionMajor <= 2 &&
           (c.flags & kContextFlags) == 0;
}

std::vector<std::pair<int, int>> versionLadder(const TesseraContextConfig& c) {
    if (c.api == TESSERA_API_OPENGL_ES) {
        if (c.versionMajor == 0) return {{2, 0}};
        return {{c.versionMajor, c.versionMinor}};
    }
    if (c.versionMajor != 0) return {{c.versionMajor, c.versionMinor}};
    if (c.profile != TESSERA_PROFILE_ANY)
        return {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}, {3, 2}};
    return {{0, 0}}; // no version attributes: whatever the driver considers its default
}

/* Appends `key,value` unless the extension is missing: strict fails, best effort logs and drops it. */
bool addIfSupported(const WglShared& sh, const char* extension, bool strict, std::vector<int>& attribs, int key, int value,
                    std::string& error) {
    if (sh.has(extension)) {
        attribs.push_back(key);
        attribs.push_back(value);
        return true;
    }
    if (strict) {
        error = std::string("required extension missing: ") + extension;
        return false;
    }
    logf(TESSERA_LOG_INFO, "WGL: %s missing, request dropped", extension);
    return true;
}

TesseraResult createNativeContext(WglShared& sh, HDC dc, const TesseraContextConfig& c, HGLRC shareRc, HGLRC& rcOut) {
    const WglApi& api = *sh.api;
    const bool strict = c.fallback == TESSERA_FALLBACK_STRICT;

    if (legacyContextSuffices(c)) {
        HGLRC rc = api.createContext(dc);
        if (!rc) return fail(TESSERA_ERROR_PLATFORM, describeWin32Error("wglCreateContext failed", GetLastError()));
        if (shareRc && !api.shareLists(shareRc, rc)) {
            const DWORD e = GetLastError();
            api.deleteContext(rc);
            return fail(TESSERA_ERROR_PLATFORM, describeWin32Error("wglShareLists failed", e));
        }
        rcOut = rc;
        return TESSERA_SUCCESS;
    }

    if (!sh.fn.createContextAttribs || !sh.has("WGL_ARB_create_context"))
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "the driver lacks WGL_ARB_create_context (needed for this request)");

    int profileBit = 0;
    if (c.api == TESSERA_API_OPENGL_ES) {
        if (!sh.has("WGL_EXT_create_context_es2_profile") && !sh.has("WGL_EXT_create_context_es_profile"))
            return fail(TESSERA_ERROR_UNSUPPORTED, "OpenGL ES contexts are not available through WGL on this driver");
        profileBit = kProfileEs2;
    } else if (c.profile != TESSERA_PROFILE_ANY) {
        if (!sh.has("WGL_ARB_create_context_profile"))
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "the driver lacks WGL_ARB_create_context_profile");
        profileBit = c.profile == TESSERA_PROFILE_CORE ? kProfileCore : kProfileCompat;
    }

    std::string error;
    int ctxFlags = 0;
    std::vector<int> common;
    if (c.flags & TESSERA_FLAG_DEBUG) ctxFlags |= kFlagDebug;
    if (c.flags & TESSERA_FLAG_FORWARD_COMPATIBLE) ctxFlags |= kFlagForward;
    if (c.flags & TESSERA_FLAG_ROBUST_ACCESS) {
        if (sh.has("WGL_ARB_create_context_robustness")) {
            ctxFlags |= kFlagRobust;
            if (c.flags & TESSERA_FLAG_RESET_NOTIFICATION) {
                common.push_back(kCtxResetStrategy);
                common.push_back(kCtxLoseOnReset);
            }
        } else if (strict) {
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "required extension missing: WGL_ARB_create_context_robustness");
        } else {
            logf(TESSERA_LOG_INFO, "WGL: robustness unavailable, request dropped");
        }
    }
    if ((c.flags & TESSERA_FLAG_NO_ERROR) &&
        !addIfSupported(sh, "WGL_ARB_create_context_no_error", strict, common, kCtxNoError, 1, error))
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, error);
    if ((c.flags & TESSERA_FLAG_RELEASE_FLUSH) &&
        !addIfSupported(sh, "WGL_ARB_context_flush_control", strict, common, kCtxReleaseBehavior, kCtxReleaseFlush, error))
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, error);

    DWORD lastError = 0;
    for (const auto& version : versionLadder(c)) {
        std::vector<int> attribs;
        if (version.first != 0) {
            attribs.push_back(kCtxMajor);
            attribs.push_back(version.first);
            attribs.push_back(kCtxMinor);
            attribs.push_back(version.second);
        }
        if (profileBit) {
            attribs.push_back(kCtxProfileMask);
            attribs.push_back(profileBit);
        }
        if (ctxFlags) {
            attribs.push_back(kCtxFlags);
            attribs.push_back(ctxFlags);
        }
        attribs.insert(attribs.end(), common.begin(), common.end());
        attribs.push_back(0);

        SetLastError(0);
        HGLRC rc = sh.fn.createContextAttribs(dc, shareRc, attribs.data());
        if (rc) {
            rcOut = rc;
            return TESSERA_SUCCESS;
        }
        lastError = GetLastError();
    }

    // ERROR_INVALID_VERSION_ARB / ERROR_INVALID_PROFILE_ARB (drivers report them with various high bits set).
    const DWORD code = lastError & 0xFFFFu;
    if (code == 0x2095 || code == 0x2096)
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "the driver does not support the requested OpenGL version/profile");
    return fail(TESSERA_ERROR_PLATFORM, describeWin32Error("wglCreateContextAttribsARB failed", lastError));
}

// ---- reporting what we actually got -------------------------------------------------------------

bool parseVersion(const char* text, bool& isEs, int& major, int& minor) {
    if (!text) return false;
    isEs = std::strncmp(text, "OpenGL ES", 9) == 0;
    const char* p = text;
    if (isEs) {
        p += 9;
        if (std::strncmp(p, "-CM", 3) == 0 || std::strncmp(p, "-CL", 3) == 0) p += 3;
        while (*p == ' ') ++p;
    }
    // "<major>.<minor>..." - parsed by hand (locale-independent, no CRT scanf).
    auto number = [&p](int& out) {
        if (*p < '0' || *p > '9') return false;
        int v = 0;
        for (; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (*p - '0');
        out = v;
        return true;
    };
    if (!number(major) || *p != '.') return false;
    ++p;
    return number(minor);
}

/* Requires the context to be current on the calling thread. */
bool queryInfo(const WglShared& sh, HDC dc, int pf, const TesseraContextConfig& c, bool pbuffer,
               TesseraContextInfo& info) {
    const WglApi& api = *sh.api;
    PfnGlGetString glGetString = nullptr;
    PfnGlGetIntegerv glGetIntegerv = nullptr;
    PfnGlGetError glGetError = nullptr;
    glGetString = reinterpret_cast<PfnGlGetString>(api.lib.symbol("glGetString"));
    glGetIntegerv = reinterpret_cast<PfnGlGetIntegerv>(api.lib.symbol("glGetIntegerv"));
    glGetError = reinterpret_cast<PfnGlGetError>(api.lib.symbol("glGetError"));
    if (!glGetString || !glGetIntegerv || !glGetError) return false;

    std::memset(&info, 0, sizeof info);
    info.structSize = sizeof info;

    const char* version = reinterpret_cast<const char*>(glGetString(kGlVersion));
    copyString(info.version, sizeof info.version, version);
    copyString(info.vendor, sizeof info.vendor, reinterpret_cast<const char*>(glGetString(kGlVendor)));
    copyString(info.renderer, sizeof info.renderer, reinterpret_cast<const char*>(glGetString(kGlRenderer)));

    bool isEs = false;
    int major = 0, minor = 0;
    if (!parseVersion(version, isEs, major, minor)) return false;
    info.api = isEs ? TESSERA_API_OPENGL_ES : TESSERA_API_OPENGL;
    info.versionMajor = major;
    info.versionMinor = minor;

    while (glGetError() != 0) {} // drain anything pending from before us
    uint32_t flags = 0;
    if (isEs) {
        info.profile = TESSERA_PROFILE_ANY;
    } else if (major > 3 || (major == 3 && minor >= 2)) {
        int mask = 0;
        glGetIntegerv(kGlProfileMask, &mask);
        // A context created without a profile mask reports 0 on some drivers; it is a compatibility context.
        info.profile = (mask & 1) ? TESSERA_PROFILE_CORE : TESSERA_PROFILE_COMPATIBILITY;
    } else {
        info.profile = TESSERA_PROFILE_COMPATIBILITY;
    }
    if (major >= 3) {
        int f = 0;
        glGetIntegerv(kGlContextFlags, &f);
        if (f & 0x1) flags |= TESSERA_FLAG_FORWARD_COMPATIBLE;
        if (f & 0x2) flags |= TESSERA_FLAG_DEBUG;
        if (f & 0x4) {
            flags |= TESSERA_FLAG_ROBUST_ACCESS;
            int strategy = 0;
            glGetIntegerv(kGlResetStrategy, &strategy);
            if (strategy == kCtxLoseOnReset) flags |= TESSERA_FLAG_RESET_NOTIFICATION;
        }
        if (f & 0x8) flags |= TESSERA_FLAG_NO_ERROR;
        while (glGetError() != 0) {}
    }
    if (c.flags & TESSERA_FLAG_RELEASE_FLUSH) flags |= TESSERA_FLAG_RELEASE_FLUSH;

    // Pixel format: prefer the ARB query (also valid for ARB-only formats), fall back to GDI.
    bool queried = false;
    if (sh.fn.getPixelFormatAttribiv) {
        const int keys[] = {kWglRedBits,   kWglGreenBits, kWglBlueBits,  kWglAlphaBits, kWglDepthBits,
                            kWglStencilBits, kWglDoubleBuffer, kWglStereo, kWglSamples,   kWglPixelType};
        int values[sizeof keys / sizeof keys[0]] = {};
        if (sh.fn.getPixelFormatAttribiv(dc, pf, 0, static_cast<UINT>(sizeof keys / sizeof keys[0]), keys, values)) {
            info.redBits = values[0];
            info.greenBits = values[1];
            info.blueBits = values[2];
            info.alphaBits = values[3];
            info.depthBits = values[4];
            info.stencilBits = values[5];
            if (values[6]) flags |= TESSERA_FLAG_DOUBLE_BUFFER;
            if (values[7]) flags |= TESSERA_FLAG_STEREO;
            info.samples = values[8];
            if (values[9] == kWglTypeRgbaFloat) flags |= TESSERA_FLAG_FLOAT_COLOR;
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
            if (pfd.dwFlags & PFD_DOUBLEBUFFER) flags |= TESSERA_FLAG_DOUBLE_BUFFER;
            if (pfd.dwFlags & PFD_STEREO) flags |= TESSERA_FLAG_STEREO;
        }
    }
    info.flags = flags;
    info.surface = pbuffer ? TESSERA_SURFACE_PBUFFER
                           : (c.surface == TESSERA_SURFACE_NONE ? TESSERA_SURFACE_NONE : TESSERA_SURFACE_HIDDEN_WINDOW);
    return true;
}

// ---- the backend --------------------------------------------------------------------------------

class WglBackend final : public Backend {
  public:
    const char* name() const noexcept override { return "wgl"; }

    TesseraResult createContext(const TesseraContextConfig& c, const std::shared_ptr<ContextObject>& shareWith,
                                std::shared_ptr<ContextObject>& out) override {
        std::string error;
        std::shared_ptr<WglShared> sh = getShared(error);
        if (!sh) return fail(TESSERA_ERROR_UNSUPPORTED, error);

        std::shared_ptr<WglContext> parent;
        if (shareWith) {
            parent = std::dynamic_pointer_cast<WglContext>(shareWith);
            if (!parent) return fail(TESSERA_ERROR_INVALID_ARGUMENT, "cannot share objects with a context from another backend");
        }

        // Everything below may change this thread's current context; put the caller's back on every path.
        ForeignContextGuard guard;

        if (!sh->initExtensions(error)) return fail(TESSERA_ERROR_PLATFORM, error);

        const bool wantPbuffer = c.surface == TESSERA_SURFACE_PBUFFER;
        if (wantPbuffer && !(sh->fn.createPbuffer && sh->fn.getPbufferDC && sh->fn.releasePbufferDC &&
                             sh->fn.destroyPbuffer && sh->has("WGL_ARB_pbuffer")))
            return fail(TESSERA_ERROR_UNSUPPORTED, "WGL_ARB_pbuffer is not available");

        auto ctx = std::make_shared<WglContext>(sh); // RAII: any failure below cleans up through ~WglContext
        if (!sh->host.createWindow(ctx->window, error)) return fail(TESSERA_ERROR_PLATFORM, error);
        ctx->dc = ctx->window.dc;

        if (TesseraResult r = negotiatePixelFormat(*sh, ctx->dc, c, wantPbuffer, ctx->pixelFormat); r != TESSERA_SUCCESS)
            return r;

        if (wantPbuffer) {
            ctx->pbuffer = sh->fn.createPbuffer(ctx->window.dc, ctx->pixelFormat, 1, 1, nullptr);
            if (!ctx->pbuffer)
                return fail(TESSERA_ERROR_PLATFORM, describeWin32Error("wglCreatePbufferARB failed", GetLastError()));
            ctx->dc = sh->fn.getPbufferDC(ctx->pbuffer);
            if (!ctx->dc) return fail(TESSERA_ERROR_PLATFORM, describeWin32Error("wglGetPbufferDCARB failed", GetLastError()));
        }

        if (TesseraResult r = createNativeContext(*sh, ctx->dc, c, parent ? parent->rc : nullptr, ctx->rc);
            r != TESSERA_SUCCESS)
            return r;
        ctx->parent = std::move(parent);

        if (!sh->api->makeCurrent(ctx->dc, ctx->rc))
            return fail(TESSERA_ERROR_PLATFORM, describeWin32Error("wglMakeCurrent failed on the new context", GetLastError()));
        const bool ok = queryInfo(*sh, ctx->dc, ctx->pixelFormat, c, wantPbuffer, ctx->info);
        // `guard` restores the caller's context; it must not be left to us.
        if (!ok) return fail(TESSERA_ERROR_PLATFORM, "could not query the new context (glGetString failed)");

        // Strict mode promises exactly what was asked for: verify the parts the driver may have loosened.
        if (c.fallback == TESSERA_FALLBACK_STRICT) {
            const TesseraContextInfo& i = ctx->info;
            const bool versionOk = c.versionMajor == 0 || i.versionMajor > c.versionMajor ||
                                   (i.versionMajor == c.versionMajor && i.versionMinor >= c.versionMinor);
            const bool apiOk = i.api == c.api;
            const bool profileOk = c.profile == TESSERA_PROFILE_ANY || i.profile == c.profile;
            const bool samplesOk = i.samples >= c.samples;
            const uint32_t missing = c.flags & ~i.flags &
                                     (TESSERA_FLAG_DEBUG | TESSERA_FLAG_FORWARD_COMPATIBLE | TESSERA_FLAG_ROBUST_ACCESS |
                                      TESSERA_FLAG_NO_ERROR | TESSERA_FLAG_SRGB | TESSERA_FLAG_FLOAT_COLOR |
                                      TESSERA_FLAG_DOUBLE_BUFFER | TESSERA_FLAG_STEREO);
            if (!versionOk || !apiOk || !profileOk || !samplesOk || missing)
                return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "the created context does not satisfy the strict request");
        }

        out = std::move(ctx);
        return TESSERA_SUCCESS;
    }

    void* getProcAddress(const char* name) noexcept override {
        const WglApi* api = wglApi();
        if (!api || !name) return nullptr;
        void* p = reinterpret_cast<void*>(api->getProcAddress(name)); // only resolves while a context is current
        if (isPlausibleProcAddress(p)) return p;
        return api->lib.symbol(name); // OpenGL 1.1 functions are exported by opengl32.dll itself
    }

  private:
    std::shared_ptr<WglShared> getShared(std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shared_) return shared_;
        const WglApi* api = wglApi();
        if (!api) {
            error = "opengl32.dll is not available on this system";
            return nullptr;
        }
        auto s = std::make_shared<WglShared>();
        s->api = api;
        if (!s->host.start(error)) return nullptr;
        shared_ = s;
        return shared_;
    }

    std::mutex mutex_;
    std::shared_ptr<WglShared> shared_;
};

} // namespace

std::shared_ptr<Backend> makeWglBackend() { return std::make_shared<WglBackend>(); }

} // namespace tessera::win32
