#include "egl_backend.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/boundary.hpp"
#include "core/library.hpp"
#include "core/log.hpp"
#include "egl_api.hpp"
#include "platform/common/arb_context.hpp"
#include "platform/common/gl_debug.hpp"
#include "platform/common/gl_query.hpp"

namespace tessera::egl {
namespace {

std::string eglErrorText(const char* what, EGLint code) {
    const char* name = "unknown error";
    switch (code) {
    case 0x3000: name = "EGL_SUCCESS"; break;
    case 0x3001: name = "EGL_NOT_INITIALIZED"; break;
    case 0x3002: name = "EGL_BAD_ACCESS"; break;
    case 0x3003: name = "EGL_BAD_ALLOC"; break;
    case 0x3004: name = "EGL_BAD_ATTRIBUTE"; break;
    case 0x3005: name = "EGL_BAD_CONFIG"; break;
    case 0x3006: name = "EGL_BAD_CONTEXT"; break;
    case 0x3007: name = "EGL_BAD_CURRENT_SURFACE"; break;
    case 0x3008: name = "EGL_BAD_DISPLAY"; break;
    case 0x3009: name = "EGL_BAD_MATCH"; break;
    case 0x300A: name = "EGL_BAD_NATIVE_PIXMAP"; break;
    case 0x300B: name = "EGL_BAD_NATIVE_WINDOW"; break;
    case 0x300C: name = "EGL_BAD_PARAMETER"; break;
    case 0x300D: name = "EGL_BAD_SURFACE"; break;
    case 0x300E: name = "EGL_CONTEXT_LOST"; break;
    default: break;
    }
    char tail[40];
    std::snprintf(tail, sizeof tail, " (0x%x)", static_cast<unsigned>(code));
    return std::string(what) + ": " + name + tail;
}

std::vector<std::string> splitWords(const char* list) {
    std::vector<std::string> out;
    if (!list) return out;
    for (const char* p = list; *p;) {
        const char* e = p;
        while (*e && *e != ' ') ++e;
        if (e > p) out.emplace_back(p, static_cast<std::size_t>(e - p));
        p = *e ? e + 1 : e;
    }
    return out;
}

bool contains(const std::vector<std::string>& v, const char* word) {
    return std::find(v.begin(), v.end(), word) != v.end();
}

// ---- the display ----------------------------------------------------------------------------------

/*
 * One EGLDisplay for every context of this backend.
 *
 * EGL gives out the SAME EGLDisplay handle to anyone who asks for the same platform/native display, and
 * eglInitialize/eglTerminate are not reference counted. So Tessera must never call eglTerminate: that would tear
 * down the host's (JavaFX/GTK, ANGLE, another library's) contexts if they share the display. The displays used
 * here (surfaceless, a device, or the default display) are therefore initialised and then left initialised for
 * the life of the process; only contexts and surfaces that Tessera created are destroyed.
 */
// ---- choosing a GPU -------------------------------------------------------------------------------

/*
 * TESSERA_EGL_DEVICE names the GPU for the `device` platform. Accepted forms:
 *   <n>                       index in eglQueryDevicesEXT order (unstable across drivers/boots)
 *   /dev/dri/renderD128       a DRM render node (matched by EGL_EXT_device_drm_render_node)
 *   0000:01:00.0              a PCI address, resolved through /dev/dri/by-path/pci-<address>-render
 * An explicit choice is never silently replaced by another GPU: if nothing matches, opening the display fails.
 */
constexpr EGLint kDrmRenderNodeFile = 0x3377; // EGL_DRM_RENDER_NODE_FILE_EXT

std::string canonicalPath(const std::string& p) {
#if defined(__unix__) || defined(__APPLE__)
    char buf[4096];
    if (::realpath(p.c_str(), buf)) return buf;
#endif
    return p;
}

bool looksLikePciAddress(const std::string& s) {
    // domain:bus:device.function, e.g. 0000:01:00.0
    return s.size() >= 12 && s[4] == ':' && s[7] == ':' && s[10] == '.';
}

/* The device in `devices` that `selector` names, or -1. */
int findDevice(const EglFns& f, void* const* devices, EGLint count, const std::string& selector) {
    if (!selector.empty() && selector.find_first_not_of("0123456789") == std::string::npos) {
        if (selector.size() > 4) return -1; // far beyond any device count; also keeps atoi away from signed overflow
        const int i = std::atoi(selector.c_str());
        return i < count ? i : -1;
    }
    std::string node = selector;
    if (looksLikePciAddress(selector)) {
        std::string lower = selector;
        for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        node = "/dev/dri/by-path/pci-" + lower + "-render";
    }
    if (!f.eglQueryDeviceStringEXT) return -1;
    const std::string wanted = canonicalPath(node);
    for (EGLint i = 0; i < count; ++i) {
        const char* file = f.eglQueryDeviceStringEXT(devices[i], kDrmRenderNodeFile);
        if (file && canonicalPath(file) == wanted) return i;
    }
    return -1;
}

class EglShared {
  public:
    const EglApi* api = nullptr;
    EGLDisplay dpy = nullptr;
    int major = 0, minor = 0;
    std::string platform;
    std::vector<std::string> exts;
    std::string clientApis;

    bool atLeast(int maj, int min) const { return major > maj || (major == maj && minor >= min); }
    /* Extension that EGL 1.5 folded into the core spec counts as present on 1.5. */
    bool has(const char* ext, bool coreIn15 = false) const { return (coreIn15 && atLeast(1, 5)) || contains(exts, ext); }
    bool supportsApi(EGLenum a) const {
        const std::vector<std::string> apis = splitWords(clientApis.c_str());
        return contains(apis, a == kOpenGlApi ? "OpenGL" : "OpenGL_ES");
    }

    bool open(std::string& error) {
        const EglFns& f = api->fn;
        const char* clientExtList = f.eglQueryString(nullptr, kExtensions); // null on EGL < 1.5 without the extension
        const std::vector<std::string> client = splitWords(clientExtList);
        const bool platformBase = contains(client, "EGL_EXT_platform_base") && f.eglGetPlatformDisplayEXT != nullptr;

        const char* wanted = std::getenv("TESSERA_EGL_PLATFORM"); // surfaceless | device | default
        const std::string want = wanted ? wanted : "";
        std::string tried;

        // An explicit GPU choice only makes sense on the device platform; the others pick for themselves.
        const char* selectorEnv = std::getenv("TESSERA_EGL_DEVICE");
        const std::string selector = selectorEnv ? selectorEnv : "";
        if (!selector.empty() && want != "device" && !want.empty()) {
            error = "TESSERA_EGL_DEVICE needs TESSERA_EGL_PLATFORM=device (or unset)";
            return false;
        }

        auto attempt = [&](const char* name, EGLDisplay d) {
            if (!d) return false;
            EGLint maj = 0, min = 0;
            if (!f.eglInitialize(d, &maj, &min)) {
                tried += std::string(" ") + name + "(init failed)";
                return false;
            }
            if (maj < 1 || (maj == 1 && min < 4)) {
                tried += std::string(" ") + name + "(EGL < 1.4)";
                return false;
            }
            const char* apis = f.eglQueryString(d, kClientApis);
            if (!apis || !*apis) {
                tried += std::string(" ") + name + "(no client APIs)";
                return false;
            }
            dpy = d;
            major = maj;
            minor = min;
            platform = name;
            clientApis = apis;
            const char* e = f.eglQueryString(d, kExtensions);
            exts = splitWords(e);
            return true;
        };

        if (selector.empty() && (want.empty() || want == "surfaceless") && platformBase && contains(client, "EGL_MESA_platform_surfaceless") &&
            attempt("surfaceless", f.eglGetPlatformDisplayEXT(kPlatformSurfaceless, nullptr, nullptr)))
            return finish();

        if ((want.empty() || want == "device") && platformBase && contains(client, "EGL_EXT_platform_device") &&
            f.eglQueryDevicesEXT != nullptr) {
            void* devices[16] = {};
            EGLint count = 0;
            if (f.eglQueryDevicesEXT(16, devices, &count) && count > 0) {
                if (!selector.empty()) {
                    const int i = findDevice(f, devices, count, selector);
                    if (i < 0) {
                        error = "TESSERA_EGL_DEVICE=" + selector + " matches none of the " + std::to_string(count) +
                                " EGL device(s) (render node / PCI matching needs EGL_EXT_device_drm_render_node)";
                        return false;
                    }
                    if (attempt("device", f.eglGetPlatformDisplayEXT(kPlatformDevice, devices[i], nullptr))) return finish();
                    error = "the EGL device selected by TESSERA_EGL_DEVICE=" + selector + " could not be opened (tried:" + tried + ")";
                    return false;
                }
                for (EGLint i = 0; i < count; ++i) {
                    if (attempt("device", f.eglGetPlatformDisplayEXT(kPlatformDevice, devices[i], nullptr))) return finish();
                }
            } else if (!selector.empty()) {
                error = "TESSERA_EGL_DEVICE is set but no EGL devices are available";
                return false;
            }
        } else if (!selector.empty()) {
            error = "TESSERA_EGL_DEVICE is set but EGL_EXT_platform_device is not available";
            return false;
        }

        if (want.empty() || want == "default") {
            if (attempt("default", f.eglGetDisplay(nullptr))) return finish();
        }

        error = "no usable EGL display (tried:" + (tried.empty() ? std::string(" nothing available") : tried) + ")";
        return false;
    }

  private:
    bool finish() const {
        logf(TESSERA_LOG_DEBUG, "EGL %d.%d via %s platform, client APIs: %s, %zu extensions", major, minor,
             platform.c_str(), clientApis.c_str(), exts.size());
        return true;
    }
};

// ---- a native context ---------------------------------------------------------------------------

class EglContext final : public ContextObject {
  public:
    explicit EglContext(std::shared_ptr<EglShared> shared) : shared_(std::move(shared)) {}
    ~EglContext() override { destroy(); }

    TesseraResult describe(TesseraContextInfo& out) const noexcept override {
        out = info;
        return TESSERA_SUCCESS;
    }

    TesseraResult bindCurrent() noexcept override {
        const EglFns& f = shared_->api->fn;
        // eglMakeCurrent needs no bound API, but leaving ours bound would change what the host's own
        // eglGetCurrentContext() calls report, so the previous binding is put back.
        ScopedBindApi bind(*shared_->api, apiEnum);
        if (!f.eglMakeCurrent(shared_->dpy, surface, surface, ctx)) {
            const EGLint err = f.eglGetError();
            return fail(err == kContextLost ? TESSERA_ERROR_CONTEXT_LOST : TESSERA_ERROR_PLATFORM,
                        eglErrorText("eglMakeCurrent failed", err));
        }
        return TESSERA_SUCCESS;
    }

    void unbindCurrent() noexcept override {
        const EglFns& f = shared_->api->fn;
        if (!ctx) return;
        ScopedBindApi bind(*shared_->api, apiEnum);
        if (f.eglGetCurrentContext() == ctx) f.eglMakeCurrent(shared_->dpy, nullptr, nullptr, nullptr);
    }

    // Pbuffers and surfaceless contexts have nothing to present.
    TesseraResult swapBuffers() noexcept override { return TESSERA_SUCCESS; }

    TesseraResult nativeHandles(TesseraNativeHandles& out) const noexcept override {
        out.kind = TESSERA_NATIVE_EGL;
        out.handles[0] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(shared_->dpy));
        out.handles[1] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(ctx));
        out.handles[2] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(surface));
        out.handles[3] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(config));
        out.handles[4] = static_cast<uint64_t>(apiEnum);
        return TESSERA_SUCCESS;
    }

    TesseraResult setSwapInterval(int32_t) noexcept override {
        return fail(TESSERA_ERROR_UNSUPPORTED, "swap interval does not apply to EGL pbuffer/surfaceless drawables");
    }

    TesseraResult queryResetStatus(int32_t& out) noexcept override {
        return queryResetStatusFrom(resetFn, info.flags, out);
    }

    GlResetStatusFn resetFn = nullptr;
    std::shared_ptr<EglShared> shared_;
    EGLenum apiEnum = kOpenGlApi;
    EGLConfig config = nullptr;
    EGLSurface surface = nullptr; // null = surfaceless
    EGLContext ctx = nullptr;
    std::shared_ptr<EglContext> parent; // keeps a share parent's native context alive as long as we are
    TesseraContextInfo info = {};

  private:
    void destroy() noexcept {
        const EglFns& f = shared_->api->fn;
        ScopedBindApi bind(*shared_->api, apiEnum);
        if (ctx) {
            if (f.eglGetCurrentContext() == ctx) f.eglMakeCurrent(shared_->dpy, nullptr, nullptr, nullptr);
            f.eglDestroyContext(shared_->dpy, ctx);
            ctx = nullptr;
        }
        if (surface) {
            f.eglDestroySurface(shared_->dpy, surface);
            surface = nullptr;
        }
        // Deliberately no eglTerminate: the display may be shared with other users in this process.
    }
};

// ---- configuration --------------------------------------------------------------------------------

std::vector<EGLint> configAttribs(const TesseraContextConfig& c, const FormatAttempt& a, bool floatColor) {
    std::vector<EGLint> v;
    auto add = [&v](EGLint key, EGLint value) {
        v.push_back(key);
        v.push_back(value);
    };
    add(kSurfaceType, kPbufferBit);
    EGLint renderable = kOpenGlBit;
    if (c.api == TESSERA_API_OPENGL_ES) renderable = c.versionMajor >= 3 ? kOpenGlEs3Bit : kOpenGlEs2Bit;
    add(kRenderableType, renderable);
    int r = c.redBits, g = c.greenBits, b = c.blueBits;
    if (r == 0 && g == 0 && b == 0) r = g = b = floatColor ? 16 : 8; // sane default for "don't care"
    add(kRedSize, r);
    add(kGreenSize, g);
    add(kBlueSize, b);
    if (c.alphaBits > 0) add(kAlphaSize, c.alphaBits);
    if (!a.relaxDepth) {
        if (c.depthBits > 0) add(kDepthSize, c.depthBits);
        if (c.stencilBits > 0) add(kStencilSize, c.stencilBits);
    }
    if (a.samples > 0) {
        add(kSampleBuffers, 1);
        add(kSamples, a.samples);
    }
    if (floatColor) add(kColorComponentType, kColorComponentFloat);
    v.push_back(kNone);
    return v;
}

/* The pieces of a request that depend on extensions, resolved once per attempt to create a context. */
struct EglPlan {
    bool srgb = false;
    bool floatColor = false;
    bool surfaceless = false;
    bool minorVersionAttrib = false; // EGL 1.4 without KHR_create_context only knows the major version
    int samples = 0;
    std::vector<std::pair<int, int>> versions;
    EGLint profile = 0;
    std::vector<EGLint> flagAttribs; // key,value pairs independent of the version
};

TesseraResult planEgl(const EglShared& sh, const TesseraContextConfig& c, EglPlan& plan) {
    const bool strict = c.fallback == TESSERA_FALLBACK_STRICT;
    plan = EglPlan{};

    // EGL pbuffers and surfaceless contexts are single-buffered: there is no swap chain to double.
    if (c.flags & TESSERA_FLAG_DOUBLE_BUFFER) {
        if (strict)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "EGL pbuffer/surfaceless drawables are single-buffered: drop TESSERA_FLAG_DOUBLE_BUFFER or use best-effort");
        logf(TESSERA_LOG_INFO, "EGL: double buffering is not available on pbuffer/surfaceless drawables, request dropped");
    }
    if (c.flags & TESSERA_FLAG_STEREO) {
        if (strict) return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "stereo is not available through EGL pbuffers");
        logf(TESSERA_LOG_INFO, "EGL: stereo is not available, request dropped");
    }

    // Soft, extension-dependent features: strict refuses, best effort drops with a log line.
    auto require = [&](bool ok, const char* what, bool& flag) -> TesseraResult {
        if (!flag) return TESSERA_SUCCESS;
        if (ok) return TESSERA_SUCCESS;
        if (strict) return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, std::string("required EGL capability missing: ") + what);
        logf(TESSERA_LOG_INFO, "EGL: %s missing, request dropped", what);
        flag = false;
        return TESSERA_SUCCESS;
    };
    plan.srgb = (c.flags & TESSERA_FLAG_SRGB) != 0;
    if (TesseraResult r = require(sh.has("EGL_KHR_gl_colorspace", true), "EGL_KHR_gl_colorspace (sRGB)", plan.srgb); r != TESSERA_SUCCESS)
        return r;
    plan.floatColor = (c.flags & TESSERA_FLAG_FLOAT_COLOR) != 0;
    if (plan.floatColor && !sh.has("EGL_EXT_pixel_format_float"))
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "float color buffers are not available (EGL_EXT_pixel_format_float)");
    plan.samples = c.samples;

    plan.surfaceless = c.surface == TESSERA_SURFACE_NONE && sh.has("EGL_KHR_surfaceless_context");

    // Context attributes (EGL 1.5 core, or EGL_KHR_create_context).
    const bool createContextExt = sh.has("EGL_KHR_create_context", true);
    plan.minorVersionAttrib = createContextExt;
    const bool needsAttribs = c.versionMajor != 0 || c.profile != TESSERA_PROFILE_ANY || (c.flags & ~(TESSERA_FLAG_DOUBLE_BUFFER | TESSERA_FLAG_SRGB | TESSERA_FLAG_FLOAT_COLOR | TESSERA_FLAG_STEREO)) != 0;
    if (needsAttribs && !createContextExt)
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "the driver lacks EGL_KHR_create_context (needed for this request)");

    if (c.api == TESSERA_API_OPENGL && c.profile != TESSERA_PROFILE_ANY)
        plan.profile = c.profile == TESSERA_PROFILE_CORE ? kProfileCore : kProfileCompat;

    if (c.flags & TESSERA_FLAG_DEBUG) plan.flagAttribs.insert(plan.flagAttribs.end(), {kContextDebug, 1});
    if ((c.flags & TESSERA_FLAG_FORWARD_COMPATIBLE) && c.api == TESSERA_API_OPENGL)
        plan.flagAttribs.insert(plan.flagAttribs.end(), {kContextForwardCompatible, 1});
    if (c.flags & TESSERA_FLAG_ROBUST_ACCESS) {
        if (sh.has("EGL_EXT_create_context_robustness", true)) {
            plan.flagAttribs.insert(plan.flagAttribs.end(), {kContextRobustAccess, 1});
            if (c.flags & TESSERA_FLAG_RESET_NOTIFICATION)
                plan.flagAttribs.insert(plan.flagAttribs.end(), {kContextResetStrategy, kLoseContextOnReset});
        } else if (strict) {
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "required extension missing: EGL_EXT_create_context_robustness");
        } else {
            logf(TESSERA_LOG_INFO, "EGL: robustness unavailable, request dropped");
        }
    }
    if (c.flags & TESSERA_FLAG_NO_ERROR) {
        if (sh.has("EGL_KHR_create_context_no_error"))
            plan.flagAttribs.insert(plan.flagAttribs.end(), {kContextNoError, 1});
        else if (strict)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "required extension missing: EGL_KHR_create_context_no_error");
        else
            logf(TESSERA_LOG_INFO, "EGL: no-error contexts unavailable, request dropped");
    }
    if (c.flags & TESSERA_FLAG_RELEASE_FLUSH) {
        if (sh.has("EGL_KHR_context_flush_control", true))
            plan.flagAttribs.insert(plan.flagAttribs.end(), {kContextReleaseBehavior, kContextReleaseFlush});
        else if (strict)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "required extension missing: EGL_KHR_context_flush_control");
        else
            logf(TESSERA_LOG_INFO, "EGL: flush control unavailable, request dropped");
    }

    plan.versions = contextVersionLadder(c);
    return TESSERA_SUCCESS;
}

TesseraResult chooseConfig(const EglShared& sh, const TesseraContextConfig& c, const EglPlan& plan, EGLConfig& out) {
    const EglFns& f = sh.api->fn;
    const bool strict = c.fallback == TESSERA_FALLBACK_STRICT;
    for (const FormatAttempt& a : formatAttempts(plan.samples, false, strict)) {
        const std::vector<EGLint> attribs = configAttribs(c, a, plan.floatColor);
        EGLConfig configs[32] = {};
        EGLint count = 0;
        if (f.eglChooseConfig(sh.dpy, attribs.data(), configs, 32, &count) && count > 0) {
            out = configs[0]; // EGL sorts best-first
            if (a.samples != plan.samples || a.relaxDepth)
                logf(TESSERA_LOG_INFO, "EGL: config relaxed (samples %d->%d, depth relaxed %d)", c.samples, a.samples,
                     a.relaxDepth);
            return TESSERA_SUCCESS;
        }
    }
    return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "no EGL config matches the requested configuration");
}

TesseraResult createNativeContext(EglShared& sh, EglContext& ctx, const TesseraContextConfig& c, const EglPlan& plan,
                                  EGLContext shareCtx) {
    const EglFns& f = sh.api->fn;
    EGLint lastError = 0;
    for (const auto& version : plan.versions) {
        std::vector<EGLint> attribs;
        if (version.first != 0) {
            attribs.insert(attribs.end(), {kContextMajorVersion, version.first});
            if (plan.minorVersionAttrib) attribs.insert(attribs.end(), {kContextMinorVersion, version.second});
        }
        if (plan.profile) attribs.insert(attribs.end(), {kContextProfileMask, plan.profile});
        attribs.insert(attribs.end(), plan.flagAttribs.begin(), plan.flagAttribs.end());
        attribs.push_back(kNone);

        EGLContext created = f.eglCreateContext(sh.dpy, ctx.config, shareCtx, attribs.data());
        if (created) {
            ctx.ctx = created;
            return TESSERA_SUCCESS;
        }
        lastError = f.eglGetError();
    }
    (void)c;
    if (lastError == kBadAlloc) return fail(TESSERA_ERROR_OUT_OF_MEMORY, eglErrorText("eglCreateContext failed", lastError));
    // BAD_MATCH / BAD_ATTRIBUTE / BAD_CONFIG: the driver cannot provide this version/profile on this config.
    return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, eglErrorText("the driver cannot create the requested context", lastError));
}

// ---- reporting what we actually got -------------------------------------------------------------

/* Requires the context to be current on the calling thread. */
bool queryInfo(const EglShared& sh, const EglContext& ctx, bool wantSrgb,
               TesseraContextInfo& info) {
    const EglFns& f = sh.api->fn;
    GlQueryFns gl;
    gl.getString = reinterpret_cast<decltype(gl.getString)>(f.eglGetProcAddress("glGetString"));
    gl.getIntegerv = reinterpret_cast<decltype(gl.getIntegerv)>(f.eglGetProcAddress("glGetIntegerv"));
    gl.getError = reinterpret_cast<decltype(gl.getError)>(f.eglGetProcAddress("glGetError"));
    if (!queryContextInfo(gl, info)) return false;

    auto attr = [&](EGLint key) {
        EGLint value = 0;
        f.eglGetConfigAttrib(sh.dpy, ctx.config, key, &value);
        return static_cast<int>(value);
    };
    uint32_t flags = info.flags;
    info.redBits = attr(kRedSize);
    info.greenBits = attr(kGreenSize);
    info.blueBits = attr(kBlueSize);
    info.alphaBits = attr(kAlphaSize);
    info.depthBits = attr(kDepthSize);
    info.stencilBits = attr(kStencilSize);
    info.samples = attr(kSamples);
    if (sh.has("EGL_EXT_pixel_format_float") && attr(kColorComponentType) == kColorComponentFloat)
        flags |= TESSERA_FLAG_FLOAT_COLOR;
    if (wantSrgb && ctx.surface) {
        EGLint cs = 0;
        if (f.eglQuerySurface(sh.dpy, ctx.surface, kGlColorspace, &cs) && cs == kGlColorspaceSrgb) flags |= TESSERA_FLAG_SRGB;
    }
    info.flags = flags;
    info.surface = ctx.surface ? TESSERA_SURFACE_PBUFFER : TESSERA_SURFACE_NONE;
    return true;
}

// ---- the backend --------------------------------------------------------------------------------

class EglBackend final : public Backend {
  public:
    const char* name() const noexcept override { return "egl"; }

    TesseraResult createContext(const TesseraContextConfig& c, const std::shared_ptr<ContextObject>& shareWith,
                                std::shared_ptr<ContextObject>& out) override {
        std::string error;
        std::shared_ptr<EglShared> sh = getShared(error);
        if (!sh) return fail(TESSERA_ERROR_UNSUPPORTED, error);
        const EglFns& f = sh->api->fn;

        const EGLenum apiEnum = c.api == TESSERA_API_OPENGL_ES ? kOpenGlEsApi : kOpenGlApi;
        if (!sh->supportsApi(apiEnum))
            return fail(TESSERA_ERROR_UNSUPPORTED, std::string("this EGL implementation has no ") +
                                                       (c.api == TESSERA_API_OPENGL_ES ? "OpenGL ES" : "desktop OpenGL") + " support");

        std::shared_ptr<EglContext> parent;
        if (shareWith) {
            parent = std::dynamic_pointer_cast<EglContext>(shareWith);
            if (!parent) return fail(TESSERA_ERROR_INVALID_ARGUMENT, "cannot share objects with a context from another backend");
            if (parent->apiEnum != apiEnum)
                return fail(TESSERA_ERROR_INVALID_ARGUMENT, "cannot share objects between OpenGL and OpenGL ES contexts");
        }

        // EGL state is per thread and per API; put the caller's all back on every path.
        ForeignContextGuard guard;

        EglPlan plan;
        if (TesseraResult r = planEgl(*sh, c, plan); r != TESSERA_SUCCESS) return r;

        auto ctx = std::make_shared<EglContext>(sh); // RAII: any failure below cleans up through ~EglContext
        ctx->apiEnum = apiEnum;
        if (!f.eglBindAPI(apiEnum)) return fail(TESSERA_ERROR_UNSUPPORTED, eglErrorText("eglBindAPI failed", f.eglGetError()));

        if (TesseraResult r = chooseConfig(*sh, c, plan, ctx->config); r != TESSERA_SUCCESS) return r;

        if (!plan.surfaceless) {
            auto makePbuffer = [&](bool srgb) {
                std::vector<EGLint> attribs = {kWidth, 1, kHeight, 1};
                if (srgb) attribs.insert(attribs.end(), {kGlColorspace, kGlColorspaceSrgb});
                attribs.push_back(kNone);
                return f.eglCreatePbufferSurface(sh->dpy, ctx->config, attribs.data());
            };
            ctx->surface = makePbuffer(plan.srgb);
            if (!ctx->surface && plan.srgb) {
                // The chosen config cannot back an sRGB surface (EGL has no config attribute to ask for one up front).
                const EGLint err = f.eglGetError();
                if (c.fallback == TESSERA_FALLBACK_STRICT)
                    return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, eglErrorText("no EGL config can back an sRGB surface", err));
                logf(TESSERA_LOG_INFO, "EGL: sRGB surface unavailable (%s), request dropped", eglErrorText("eglCreatePbufferSurface", err).c_str());
                plan.srgb = false;
                ctx->surface = makePbuffer(false);
            }
            if (!ctx->surface)
                return fail(TESSERA_ERROR_PLATFORM, eglErrorText("eglCreatePbufferSurface failed", f.eglGetError()));
        }

        if (TesseraResult r = createNativeContext(*sh, *ctx, c, plan, parent ? parent->ctx : nullptr); r != TESSERA_SUCCESS)
            return r;
        ctx->parent = std::move(parent);

        if (!f.eglMakeCurrent(sh->dpy, ctx->surface, ctx->surface, ctx->ctx))
            return fail(TESSERA_ERROR_PLATFORM, eglErrorText("eglMakeCurrent failed on the new context", f.eglGetError()));
        const bool ok = queryInfo(*sh, *ctx, plan.srgb, ctx->info);
        // `guard` releases the context again and restores the caller's state; it must not be left to us.
        if (!ok) return fail(TESSERA_ERROR_PLATFORM, "could not query the new context (glGetString failed)");

        // Strict mode promises exactly what was asked for: verify the parts the driver may have loosened.
        if (c.fallback == TESSERA_FALLBACK_STRICT && !contextSatisfies(c, ctx->info))
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "the created context does not satisfy the strict request");

        // Reset polling and debug-message forwarding are bound to the context that is current right now.
        const auto glProc = [&](const char* n) -> void* { return sh->api->fn.eglGetProcAddress(n); };
        ctx->resetFn = resolveResetStatus(glProc);
        if (ctx->info.flags & TESSERA_FLAG_DEBUG) installGlDebugForwarder(glProc);

        out = std::move(ctx);
        return TESSERA_SUCCESS;
    }

    void* getProcAddress(const char* name) noexcept override {
        const EglApi* api = eglApi();
        if (!api || !name) return nullptr;
        // GLVND hands out a dispatch stub for any name; every GL function starts with "gl".
        if (name[0] != 'g' || name[1] != 'l') return nullptr;
        return api->fn.eglGetProcAddress(name);
    }

  private:
    std::shared_ptr<EglShared> getShared(std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shared_) return shared_;
        const EglApi* api = eglApi();
        if (!api) {
            error = "libEGL is not available on this system";
            return nullptr;
        }
        auto s = std::make_shared<EglShared>();
        s->api = api;
        if (!s->open(error)) return nullptr;
        shared_ = s;
        return shared_;
    }

    std::mutex mutex_;
    std::shared_ptr<EglShared> shared_;
};

} // namespace

std::shared_ptr<Backend> makeEglBackend() { return std::make_shared<EglBackend>(); }

} // namespace tessera::egl
