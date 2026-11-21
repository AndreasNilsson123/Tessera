// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "egl_backend.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/boundary.hpp"
#include "core/env.hpp"
#include "core/library.hpp"
#include "core/log.hpp"
#include "egl_api.hpp"
#include "platform/common/arb_context.hpp"
#include "platform/common/context_priority.hpp"
#if defined(_WIN32)
namespace tessera::crossapi { // no GLX on Windows: nothing of the other API to guard
struct EglGuard {
    EglGuard() noexcept {}
};
inline void releaseOtherGlxCurrent() noexcept {}
} // namespace tessera::crossapi
#else
#include "platform/common/cross_api.hpp"
#endif
#include "platform/common/create_errors.hpp"
#include "platform/common/device_select.hpp"
#include "platform/common/gl_debug.hpp"
#include "platform/common/gl_query.hpp"
#include "platform/common/software_renderer.hpp"
#include "platform/common/version_negotiation.hpp"

namespace tessera::egl {
namespace {

bool envSet(const char* name) {
    std::string ignore;
    return readEnv(name, ignore);
}

std::string eglErrorText(const char* what, EGLint code) {
    const char* name = "unknown error";
    switch (code) {
    case 0x3000:
        name = "EGL_SUCCESS";
        break;
    case 0x3001:
        name = "EGL_NOT_INITIALIZED";
        break;
    case 0x3002:
        name = "EGL_BAD_ACCESS";
        break;
    case 0x3003:
        name = "EGL_BAD_ALLOC";
        break;
    case 0x3004:
        name = "EGL_BAD_ATTRIBUTE";
        break;
    case 0x3005:
        name = "EGL_BAD_CONFIG";
        break;
    case 0x3006:
        name = "EGL_BAD_CONTEXT";
        break;
    case 0x3007:
        name = "EGL_BAD_CURRENT_SURFACE";
        break;
    case 0x3008:
        name = "EGL_BAD_DISPLAY";
        break;
    case 0x3009:
        name = "EGL_BAD_MATCH";
        break;
    case 0x300A:
        name = "EGL_BAD_NATIVE_PIXMAP";
        break;
    case 0x300B:
        name = "EGL_BAD_NATIVE_WINDOW";
        break;
    case 0x300C:
        name = "EGL_BAD_PARAMETER";
        break;
    case 0x300D:
        name = "EGL_BAD_SURFACE";
        break;
    case 0x300E:
        name = "EGL_CONTEXT_LOST";
        break;
    default:
        break;
    }
    char tail[40];
    std::snprintf(tail, sizeof tail, " (0x%x)", static_cast<unsigned>(code));
    return std::string(what) + ": " + name + tail;
}

std::vector<std::string> splitWords(const char* list) {
    std::vector<std::string> out;
    if (!list)
        return out;
    for (const char* p = list; *p;) {
        const char* e = p;
        while (*e && *e != ' ')
            ++e;
        if (e > p)
            out.emplace_back(p, static_cast<std::size_t>(e - p));
        p = *e ? e + 1 : e;
    }
    return out;
}

bool contains(const std::vector<std::string>& v, const char* word) {
    return std::find(v.begin(), v.end(), word) != v.end();
}

// ---- native window systems, loaded on demand ------------------------------------------------------

/* libwayland-egl: the bridge between a caller's wl_surface and an EGL window surface. Leaked on purpose (like
 * the other libraries: dlclose of a GL stack is hazardous). */
struct WaylandEgl {
    DynamicLibrary lib;
    void* (*create)(void* surface, int width, int height) = nullptr;
    void (*destroy)(void* window) = nullptr;
    void (*resize)(void* window, int width, int height, int dx, int dy) = nullptr;
};

const WaylandEgl* waylandEgl() noexcept {
    static const WaylandEgl* api = []() -> const WaylandEgl* {
        auto* w = new WaylandEgl();
        w->lib = DynamicLibrary::open("libwayland-egl.so.1");
        if (w->lib) {
            w->create = reinterpret_cast<decltype(w->create)>(w->lib.symbol("wl_egl_window_create"));
            w->destroy = reinterpret_cast<decltype(w->destroy)>(w->lib.symbol("wl_egl_window_destroy"));
            w->resize = reinterpret_cast<decltype(w->resize)>(w->lib.symbol("wl_egl_window_resize"));
        }
        if (!w->lib || !w->create || !w->destroy || !w->resize) {
            delete w;
            return nullptr;
        }
        return w;
    }();
    return api;
}

/* The one Xlib connection EGL's X11 platform uses, opened on first use and kept for the life of the process
 * (the server is $DISPLAY's, the same one a window of the caller's lives on). Xlib threading is made safe the
 * same way the GLX backend does (ADR 0005/0013): the host's own call is detected, and TESSERA_NO_XINITTHREADS
 * opts out. */
void* x11Display(std::string& error) {
    static std::mutex mutex;
    static void* display = nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (display)
        return display;
    static DynamicLibrary x11 = DynamicLibrary::open("libX11.so.6");
    if (!x11) {
        error = "libX11 is not available";
        return nullptr;
    }
    using OpenFn = void* (*)(const char*);
    using ThreadsFn = int (*)();
    auto open = reinterpret_cast<OpenFn>(x11.symbol("XOpenDisplay"));
    auto initThreads = reinterpret_cast<ThreadsFn>(x11.symbol("XInitThreads"));
    if (!open) {
        error = "libX11 has no XOpenDisplay";
        return nullptr;
    }
    void* const* lock_ = static_cast<void* const*>(x11.symbol("_Xglobal_lock"));
    const bool hostDidIt = lock_ && *lock_ != nullptr;
    if (initThreads && !hostDidIt && !envSet("TESSERA_NO_XINITTHREADS"))
        initThreads();
    display = open(nullptr);
    if (!display)
        error = "cannot open the X display for EGL";
    return display;
}

// ---- the display ----------------------------------------------------------------------------------

/*
 * One EGLDisplay for every context of this backend.
 *
 * EGL gives out the SAME EGLDisplay handle to anyone who asks for the same platform/native display, and
 * eglInitialize/eglTerminate are not reference counted. So Tessera must never call eglTerminate: that would
 * tear down the host's (JavaFX/GTK, ANGLE, another library's) contexts if they share the display. The
 * displays used here (surfaceless, a device, or the default display) are therefore initialised and then left
 * initialised for the life of the process; only contexts and surfaces that Tessera created are destroyed.
 */
// ---- choosing a GPU -------------------------------------------------------------------------------

/*
 * TESSERA_EGL_DEVICE names the GPU for the `device` platform. Accepted forms:
 *   <n>                       index in eglQueryDevicesEXT order (unstable across drivers/boots)
 *   /dev/dri/renderD128       a DRM render node (matched by EGL_EXT_device_drm_render_node)
 *   0000:01:00.0              a PCI address, resolved through /dev/dri/by-path/pci-<address>-render
 *   <uuid> or uuid:<uuid>     the device UUID (EGL_EXT_device_persistent_id): stable across boots and
 * re-plugging An explicit choice is never silently replaced by another GPU: if nothing matches, opening the
 * display fails.
 */
constexpr EGLint kDrmRenderNodeFile = 0x3377; // EGL_DRM_RENDER_NODE_FILE_EXT
constexpr EGLint kDeviceUuid = 0x335C;        // EGL_DEVICE_UUID_EXT (EGL_EXT_device_persistent_id)

constexpr const char* kSoftwareDeviceExtension = "EGL_MESA_device_software";

std::string canonicalPath(const std::string& p) {
#if defined(__unix__) || defined(__APPLE__)
    char buf[4096];
    if (::realpath(p.c_str(), buf))
        return buf;
#endif
    return p;
}

/* The device in `devices` that `selector` names, or a reason it names none. The matching rules are
 * device_select.hpp's. */
DeviceChoice findDevice(const EglFns& f, void* const* devices, EGLint count, const std::string& selector) {
    std::vector<EglDeviceDesc> descs(static_cast<std::size_t>(count));
    for (EGLint i = 0; i < count; ++i) {
        EglDeviceDesc& d = descs[static_cast<std::size_t>(i)];
        if (f.eglQueryDeviceStringEXT) {
            const char* file = f.eglQueryDeviceStringEXT(devices[i], kDrmRenderNodeFile);
            if (file)
                d.renderNode = file;
        }
        if (f.eglQueryDeviceStringEXT) {
            const char* exts = f.eglQueryDeviceStringEXT(devices[i], kExtensions);
            d.software = exts && contains(splitWords(exts), kSoftwareDeviceExtension);
        }
        EGLint size = 0;
        if (f.eglQueryDeviceBinaryEXT &&
            f.eglQueryDeviceBinaryEXT(devices[i], kDeviceUuid, static_cast<EGLint>(d.uuid.size()),
                                      d.uuid.data(), &size) &&
            size == static_cast<EGLint>(d.uuid.size()))
            d.hasUuid = true;
    }
    return selectDevice(selector, descs, canonicalPath);
}

class EglShared {
  public:
    const EglApi* api = nullptr;
    EGLDisplay dpy = nullptr;
    int major = 0, minor = 0;
    std::string platform;
    std::vector<std::string> exts;
    std::string clientApis;

    bool atLeast(int maj, int min) const {
        return major > maj || (major == maj && minor >= min);
    }
    /* Extension that EGL 1.5 folded into the core spec counts as present on 1.5. */
    bool has(const char* ext, bool coreIn15 = false) const {
        return (coreIn15 && atLeast(1, 5)) || contains(exts, ext);
    }
    bool supportsApi(EGLenum a) const {
        const std::vector<std::string> apis = splitWords(clientApis.c_str());
        return contains(apis, a == kOpenGlApi ? "OpenGL" : "OpenGL_ES");
    }

    /* `software`: the CPU renderer only (the "software" backend): Mesa's software device through the device
     * platform, or ANGLE's WARP / SwiftShader. Never the GPU, even when there is one. */
    bool open(std::string& error, bool software = false) {
        const EglFns& f = api->fn;
        const char* clientExtList =
            f.eglQueryString(nullptr, kExtensions); // null on EGL < 1.5 without the extension
        const std::vector<std::string> client = splitWords(clientExtList);
        const bool platformBase =
            contains(client, "EGL_EXT_platform_base") && f.eglGetPlatformDisplayEXT != nullptr;

        std::string wanted; // TESSERA_EGL_PLATFORM: surfaceless | device | default | angle
        readEnv("TESSERA_EGL_PLATFORM", wanted);
        const std::string want = software ? "device" : wanted;
        std::string tried;

        // An explicit GPU choice only makes sense on the device platform; the others pick for themselves.
        std::string selectorEnv;
        readEnv("TESSERA_EGL_DEVICE", selectorEnv);
        const std::string selector = software ? "software" : selectorEnv;
        if (!selector.empty() && want != "device" && !want.empty()) {
            error = "TESSERA_EGL_DEVICE needs TESSERA_EGL_PLATFORM=device (or unset)";
            return false;
        }

        auto attempt = [&](const char* name, EGLDisplay d) {
            if (!d)
                return false;
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

        // ANGLE is a library that offers EGL on top of another API: its display is chosen by attributes, not
        // by device.
        if (platformBase && contains(client, "EGL_ANGLE_platform_angle") &&
            (want.empty() || want == "angle" || software)) {
            auto angle = [&](const char* name, std::vector<EGLint> attribs) {
                attribs.push_back(kNone);
                return attempt(name, f.eglGetPlatformDisplayEXT(kPlatformAngle, nullptr, attribs.data()));
            };
            if (software) {
                if (angle("angle-warp", {kAngleType, kAngleTypeD3D11, kAngleDeviceType, kAngleDeviceWarp}) ||
                    angle("angle-swiftshader",
                          {kAngleType, kAngleTypeVulkan, kAngleDeviceType, kAngleDeviceSwiftShader}))
                    return finish();
                error = "this ANGLE has no software device (tried:" + tried + ")";
                return false;
            }
            if (angle("angle", {}))
                return finish();
        }

        if (selector.empty() && (want.empty() || want == "surfaceless") && platformBase &&
            contains(client, "EGL_MESA_platform_surfaceless") &&
            attempt("surfaceless", f.eglGetPlatformDisplayEXT(kPlatformSurfaceless, nullptr, nullptr)))
            return finish();

        if ((want.empty() || want == "device") && platformBase &&
            contains(client, "EGL_EXT_platform_device") && f.eglQueryDevicesEXT != nullptr) {
            void* devices[16] = {};
            EGLint count = 0;
            if (f.eglQueryDevicesEXT(16, devices, &count) && count > 0) {
                if (!selector.empty()) {
                    const DeviceChoice choice = findDevice(f, devices, count, selector);
                    if (choice.index < 0) {
                        error = "TESSERA_EGL_DEVICE=" + selector + " matches none of the " +
                                std::to_string(count) + " EGL device(s): " + choice.reason;
                        return false;
                    }
                    if (attempt("device",
                                f.eglGetPlatformDisplayEXT(kPlatformDevice, devices[choice.index], nullptr)))
                        return finish();
                    error = "the EGL device selected by TESSERA_EGL_DEVICE=" + selector +
                            " could not be opened (tried:" + tried + ")";
                    return false;
                }
                for (EGLint i = 0; i < count; ++i) {
                    if (attempt("device", f.eglGetPlatformDisplayEXT(kPlatformDevice, devices[i], nullptr)))
                        return finish();
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
            if (attempt("default", f.eglGetDisplay(nullptr)))
                return finish();
        }

        error =
            "no usable EGL display (tried:" + (tried.empty() ? std::string(" nothing available") : tried) +
            ")";
        return false;
    }

    /* An EGLDisplay for a caller's window system (X11: our own connection to $DISPLAY; Wayland: the caller's
     * wl_display). EGL hands out the same EGLDisplay for the same native display, so it may already be
     * initialised by the host: that is fine, and it is never terminated (see the class comment). */
    bool openNative(EGLenum platformEnum, void* nativeDisplay, const char* name, std::string& error) {
        const EglFns& f = api->fn;
        const std::vector<std::string> client = splitWords(f.eglQueryString(nullptr, kExtensions));
        const bool wayland = platformEnum == kPlatformWayland;
        if (!f.eglGetPlatformDisplayEXT || !contains(client, "EGL_EXT_platform_base")) {
            error = "this EGL cannot create displays for a window system (EGL_EXT_platform_base)";
            return false;
        }
        if (!contains(client, wayland ? "EGL_EXT_platform_wayland" : "EGL_EXT_platform_x11") &&
            !contains(client, wayland ? "EGL_KHR_platform_wayland" : "EGL_KHR_platform_x11")) {
            error = std::string("this EGL has no ") + name + " platform";
            return false;
        }
        EGLDisplay d = f.eglGetPlatformDisplayEXT(platformEnum, nativeDisplay, nullptr);
        if (!d) {
            error = std::string("eglGetPlatformDisplay failed for the ") + name + " display";
            return false;
        }
        EGLint maj = 0, min = 0;
        if (!f.eglInitialize(d, &maj, &min) || maj < 1 || (maj == 1 && min < 4)) {
            error = std::string("the EGL ") + name + " display could not be initialised";
            return false;
        }
        const char* apis = f.eglQueryString(d, kClientApis);
        if (!apis || !*apis) {
            error = std::string("the EGL ") + name + " display offers no client API";
            return false;
        }
        dpy = d;
        major = maj;
        minor = min;
        platform = name;
        clientApis = apis;
        exts = splitWords(f.eglQueryString(d, kExtensions));
        return finish();
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
    ~EglContext() override {
        destroy();
    }

    TesseraResult describe(TesseraContextInfo& out) const noexcept override {
        out = info;
        return TESSERA_SUCCESS;
    }

    TesseraResult bindCurrent() noexcept override {
        const EglFns& f = shared_->api->fn;
        crossapi::releaseOtherGlxCurrent(); // a host's GLX context on this thread must not make ours fail
                                            // (Mesa: one per thread)
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
        if (!ctx)
            return;
        ScopedBindApi bind(*shared_->api, apiEnum);
        if (f.eglGetCurrentContext() == ctx)
            f.eglMakeCurrent(shared_->dpy, nullptr, nullptr, nullptr);
    }

    // Pbuffers and surfaceless contexts have nothing to present.
    TesseraResult swapBuffers() noexcept override {
        if (!windowSurface)
            return TESSERA_SUCCESS;
        const EglFns& f = shared_->api->fn;
        if (!f.eglSwapBuffers(shared_->dpy, surface)) {
            const EGLint err = f.eglGetError();
            return fail(err == kContextLost ? TESSERA_ERROR_CONTEXT_LOST : TESSERA_ERROR_PLATFORM,
                        eglErrorText("eglSwapBuffers failed (is the window gone?)", err));
        }
        return TESSERA_SUCCESS;
    }

    TesseraResult resizeWindow(int32_t width, int32_t height) noexcept override {
        if (!windowSurface)
            return fail(TESSERA_ERROR_UNSUPPORTED, "this context has no window to resize");
        if (waylandWindow && wl) {
            wl->resize(waylandWindow, width, height, 0, 0); // a Wayland buffer's size is ours to set
            return TESSERA_SUCCESS;
        }
        return TESSERA_SUCCESS; // X11: the surface follows the window
    }

    TesseraResult nativeHandles(TesseraNativeHandles& out) const noexcept override {
        out.kind = TESSERA_NATIVE_EGL;
        out.handles[0] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(shared_->dpy));
        out.handles[1] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(ctx));
        out.handles[2] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(surface));
        out.handles[3] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(config));
        out.handles[4] = static_cast<uint64_t>(apiEnum);
        return TESSERA_SUCCESS;
    }

    TesseraResult setSwapInterval(int32_t interval) noexcept override {
        if (!windowSurface)
            return fail(TESSERA_ERROR_UNSUPPORTED,
                        "swap interval does not apply to EGL pbuffer/surfaceless drawables");
        if (interval < 0)
            return fail(TESSERA_ERROR_INVALID_ARGUMENT, "EGL has no adaptive vsync");
        const EglFns& f = shared_->api->fn;
        if (!f.eglSwapInterval(shared_->dpy, interval)) // acts on the current draw surface, which is ours
            return fail(TESSERA_ERROR_PLATFORM, eglErrorText("eglSwapInterval failed", f.eglGetError()));
        return TESSERA_SUCCESS;
    }

    // EGL can set the interval and clamps it to the config's range, but offers no way to read the result
    // back.
    TesseraResult getSwapControl(TesseraSwapControl& out) noexcept override {
        if (!windowSurface)
            return TESSERA_SUCCESS;
        out.caps = TESSERA_SWAP_CAP_CONTROL;
        EGLint maxInterval = 0;
        if (shared_->api->fn.eglGetConfigAttrib(shared_->dpy, config, kMaxSwapInterval, &maxInterval))
            out.maxInterval = maxInterval;
        return TESSERA_SUCCESS;
    }

    TesseraResult queryResetStatus(int32_t& out) noexcept override {
        return queryResetStatusFrom(resetFn, info.flags, out);
    }

    GlResetStatusFn resetFn = nullptr;
    std::shared_ptr<EglShared> shared_;
    EGLenum apiEnum = kOpenGlApi;
    EGLConfig config = nullptr;
    EGLSurface surface = nullptr; // null = surfaceless
    bool windowSurface = false;   // a caller's window (X11 or Wayland), not a pbuffer
    void* waylandWindow =
        nullptr; // the wl_egl_window we created for the caller's wl_surface; ours to destroy
    const WaylandEgl* wl = nullptr;
    EGLContext ctx = nullptr;
    std::shared_ptr<EglContext> parent; // keeps a share parent's native context alive as long as we are
    TesseraContextInfo info = {};

  private:
    void destroy() noexcept {
        const EglFns& f = shared_->api->fn;
        ScopedBindApi bind(*shared_->api, apiEnum);
        if (ctx) {
            if (f.eglGetCurrentContext() == ctx)
                f.eglMakeCurrent(shared_->dpy, nullptr, nullptr, nullptr);
            f.eglDestroyContext(shared_->dpy, ctx);
            ctx = nullptr;
        }
        if (surface) {
            f.eglDestroySurface(shared_->dpy, surface);
            surface = nullptr;
        }
        if (waylandWindow && wl) { // after the surface that uses it; the wl_surface itself is the caller's
            wl->destroy(waylandWindow);
            waylandWindow = nullptr;
        }
        // Deliberately no eglTerminate: the display may be shared with other users in this process.
    }
};

// ---- configuration --------------------------------------------------------------------------------

std::vector<EGLint> configAttribs(const TesseraContextConfig& c, const FormatAttempt& a, bool floatColor,
                                  EGLint surfaceBit = kPbufferBit) {
    std::vector<EGLint> v;
    auto add = [&v](EGLint key, EGLint value) {
        v.push_back(key);
        v.push_back(value);
    };
    add(kSurfaceType, surfaceBit);
    EGLint renderable = kOpenGlBit;
    if (c.api == TESSERA_API_OPENGL_ES)
        renderable = c.versionMajor >= 3 ? kOpenGlEs3Bit : kOpenGlEs2Bit;
    add(kRenderableType, renderable);
    int r = c.redBits, g = c.greenBits, b = c.blueBits;
    if (r == 0 && g == 0 && b == 0)
        r = g = b = floatColor ? 16 : 8; // sane default for "don't care"
    add(kRedSize, r);
    add(kGreenSize, g);
    add(kBlueSize, b);
    if (c.alphaBits > 0)
        add(kAlphaSize, c.alphaBits);
    if (!a.relaxDepth) {
        if (c.depthBits > 0)
            add(kDepthSize, c.depthBits);
        if (c.stencilBits > 0)
            add(kStencilSize, c.stencilBits);
    }
    if (a.samples > 0) {
        add(kSampleBuffers, 1);
        add(kSamples, a.samples);
    }
    if (floatColor)
        add(kColorComponentType, kColorComponentFloat);
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

TesseraResult planEgl(const EglShared& sh, const TesseraContextConfig& c, EglPlan& plan,
                      bool window = false) {
    const bool strict = c.fallback == TESSERA_FALLBACK_STRICT;
    plan = EglPlan{};

    // EGL pbuffers and surfaceless contexts are single-buffered: there is no swap chain to double.
    if ((c.flags & TESSERA_FLAG_DOUBLE_BUFFER) && !window) {
        if (strict)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "EGL pbuffer/surfaceless drawables are single-buffered: drop "
                        "TESSERA_FLAG_DOUBLE_BUFFER or use best-effort");
        logf(TESSERA_LOG_INFO,
             "EGL: double buffering is not available on pbuffer/surfaceless drawables, request dropped");
    }
    if (c.flags & TESSERA_FLAG_STEREO) {
        if (strict)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "stereo is not available through EGL pbuffers");
        logf(TESSERA_LOG_INFO, "EGL: stereo is not available, request dropped");
    }

    // Soft, extension-dependent features: strict refuses, best effort drops with a log line.
    auto require = [&](bool ok, const char* what, bool& flag) -> TesseraResult {
        if (!flag)
            return TESSERA_SUCCESS;
        if (ok)
            return TESSERA_SUCCESS;
        if (strict)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        std::string("required EGL capability missing: ") + what);
        logf(TESSERA_LOG_INFO, "EGL: %s missing, request dropped", what);
        flag = false;
        return TESSERA_SUCCESS;
    };
    plan.srgb = (c.flags & TESSERA_FLAG_SRGB) != 0;
    if (TesseraResult r =
            require(sh.has("EGL_KHR_gl_colorspace", true), "EGL_KHR_gl_colorspace (sRGB)", plan.srgb);
        r != TESSERA_SUCCESS)
        return r;
    plan.floatColor = (c.flags & TESSERA_FLAG_FLOAT_COLOR) != 0;
    if (plan.floatColor && !sh.has("EGL_EXT_pixel_format_float"))
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                    "float color buffers are not available (EGL_EXT_pixel_format_float)");
    plan.samples = c.samples;

    plan.surfaceless = !window && c.surface == TESSERA_SURFACE_NONE && sh.has("EGL_KHR_surfaceless_context");

    // Context attributes (EGL 1.5 core, or EGL_KHR_create_context).
    const bool createContextExt = sh.has("EGL_KHR_create_context", true);
    plan.minorVersionAttrib = createContextExt;
    const bool needsAttribs = c.versionMajor != 0 || c.profile != TESSERA_PROFILE_ANY ||
                              (c.flags & ~(TESSERA_FLAG_DOUBLE_BUFFER | TESSERA_FLAG_SRGB |
                                           TESSERA_FLAG_FLOAT_COLOR | TESSERA_FLAG_STEREO)) != 0;
    if (needsAttribs && !createContextExt)
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                    "the driver lacks EGL_KHR_create_context (needed for this request)");

    if (c.api == TESSERA_API_OPENGL && c.profile != TESSERA_PROFILE_ANY)
        plan.profile = c.profile == TESSERA_PROFILE_CORE ? kProfileCore : kProfileCompat;

    if (c.priority != TESSERA_PRIORITY_DEFAULT) {
        if (sh.has("EGL_IMG_context_priority"))
            plan.flagAttribs.insert(plan.flagAttribs.end(),
                                    {kNativeContextPriorityLevel, nativePriority(c.priority)});
        else if (strict)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "required extension missing: EGL_IMG_context_priority");
        else
            logf(TESSERA_LOG_INFO, "EGL_IMG_context_priority missing, request dropped");
    }
    if (c.flags & TESSERA_FLAG_DEBUG)
        plan.flagAttribs.insert(plan.flagAttribs.end(), {kContextDebug, 1});
    if ((c.flags & TESSERA_FLAG_FORWARD_COMPATIBLE) && c.api == TESSERA_API_OPENGL)
        plan.flagAttribs.insert(plan.flagAttribs.end(), {kContextForwardCompatible, 1});
    if (c.flags & TESSERA_FLAG_ROBUST_ACCESS) {
        if (sh.has("EGL_EXT_create_context_robustness", true)) {
            plan.flagAttribs.insert(plan.flagAttribs.end(), {kContextRobustAccess, 1});
            if (c.flags & TESSERA_FLAG_RESET_NOTIFICATION)
                plan.flagAttribs.insert(plan.flagAttribs.end(), {kContextResetStrategy, kLoseContextOnReset});
        } else if (strict) {
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "required extension missing: EGL_EXT_create_context_robustness");
        } else {
            logf(TESSERA_LOG_INFO, "EGL: robustness unavailable, request dropped");
        }
    }
    if (c.flags & TESSERA_FLAG_NO_ERROR) {
        if (sh.has("EGL_KHR_create_context_no_error"))
            plan.flagAttribs.insert(plan.flagAttribs.end(), {kContextNoError, 1});
        else if (strict)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "required extension missing: EGL_KHR_create_context_no_error");
        else
            logf(TESSERA_LOG_INFO, "EGL: no-error contexts unavailable, request dropped");
    }
    if (c.flags & TESSERA_FLAG_RELEASE_FLUSH) {
        if (sh.has("EGL_KHR_context_flush_control", true))
            plan.flagAttribs.insert(plan.flagAttribs.end(), {kContextReleaseBehavior, kContextReleaseFlush});
        else if (strict)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "required extension missing: EGL_KHR_context_flush_control");
        else
            logf(TESSERA_LOG_INFO, "EGL: flush control unavailable, request dropped");
    }

    plan.versions = contextVersionLadder(c);
    return TESSERA_SUCCESS;
}

TesseraResult chooseConfig(const EglShared& sh, const TesseraContextConfig& c, const EglPlan& plan,
                           EGLConfig& out) {
    const EglFns& f = sh.api->fn;
    const bool strict = c.fallback == TESSERA_FALLBACK_STRICT;
    for (const FormatAttempt& a : formatAttempts(plan.samples, false, strict)) {
        const std::vector<EGLint> attribs = configAttribs(c, a, plan.floatColor);
        EGLConfig configs[32] = {};
        EGLint count = 0;
        if (f.eglChooseConfig(sh.dpy, attribs.data(), configs, 32, &count) && count > 0) {
            out = configs[0]; // EGL sorts best-first
            if (a.samples != plan.samples || a.relaxDepth)
                logf(TESSERA_LOG_INFO, "EGL: config relaxed (samples %d->%d, depth relaxed %d)", c.samples,
                     a.samples, a.relaxDepth);
            return TESSERA_SUCCESS;
        }
    }
    return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "no EGL config matches the requested configuration");
}

TesseraResult createNativeContext(EglShared& sh, EglContext& ctx, const TesseraContextConfig& c,
                                  const EglPlan& plan, EGLContext shareCtx) {
    const EglFns& f = sh.api->fn;
    EGLint lastError = 0;
    const bool made =
        negotiateVersion(versionMemoryKey("egl", reinterpret_cast<std::uintptr_t>(sh.dpy), c), plan.versions,
                         [&](const GlVersion& version) {
                             std::vector<EGLint> attribs;
                             if (version.first != 0) {
                                 attribs.insert(attribs.end(), {kContextMajorVersion, version.first});
                                 if (plan.minorVersionAttrib)
                                     attribs.insert(attribs.end(), {kContextMinorVersion, version.second});
                             }
                             if (plan.profile)
                                 attribs.insert(attribs.end(), {kContextProfileMask, plan.profile});
                             attribs.insert(attribs.end(), plan.flagAttribs.begin(), plan.flagAttribs.end());
                             attribs.push_back(kNone);

                             EGLContext created =
                                 f.eglCreateContext(sh.dpy, ctx.config, shareCtx, attribs.data());
                             if (created) {
                                 ctx.ctx = created;
                                 return true;
                             }
                             lastError = f.eglGetError();
                             return false;
                         });
    if (made)
        return TESSERA_SUCCESS;
    if (classifyEglCreateError(lastError) == TESSERA_ERROR_OUT_OF_MEMORY)
        return fail(TESSERA_ERROR_OUT_OF_MEMORY, eglErrorText("eglCreateContext failed", lastError));
    // BAD_MATCH / BAD_ATTRIBUTE / BAD_CONFIG: the driver cannot provide this version/profile on this config.
    return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                eglErrorText("the driver cannot create the requested context", lastError));
}

// ---- reporting what we actually got -------------------------------------------------------------

/* Requires the context to be current on the calling thread. */
bool queryInfo(const EglShared& sh, const EglContext& ctx, bool wantSrgb, TesseraContextInfo& info) {
    const EglFns& f = sh.api->fn;
    GlQueryFns gl;
    gl.getString = reinterpret_cast<decltype(gl.getString)>(f.eglGetProcAddress("glGetString"));
    gl.getIntegerv = reinterpret_cast<decltype(gl.getIntegerv)>(f.eglGetProcAddress("glGetIntegerv"));
    gl.getError = reinterpret_cast<decltype(gl.getError)>(f.eglGetProcAddress("glGetError"));
    if (!queryContextInfo(gl, info))
        return false;

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
        if (f.eglQuerySurface(sh.dpy, ctx.surface, kGlColorspace, &cs) && cs == kGlColorspaceSrgb)
            flags |= TESSERA_FLAG_SRGB;
    }
    if (ctx.windowSurface) {
        EGLint buffer = 0;
        if (f.eglQuerySurface(sh.dpy, ctx.surface, kRenderBuffer, &buffer) && buffer == kBackBuffer)
            flags |= TESSERA_FLAG_DOUBLE_BUFFER; // a window surface renders to a back buffer and swaps
    }
    info.flags = flags;
    info.surface = ctx.windowSurface ? TESSERA_SURFACE_WINDOW
                                     : (ctx.surface ? TESSERA_SURFACE_PBUFFER : TESSERA_SURFACE_NONE);
    return true;
}

// ---- surfaces for a caller's window -----------------------------------------------------------------

/* The caller's window as an EGL window surface. EGL cannot say up front which config suits a given native
 * window (X11 needs the visual to match, Wayland takes any), so the candidates of each negotiation step are
 * tried in turn until the driver accepts one. On success `ctx` has its config, surface and (Wayland)
 * wl_egl_window. */
TesseraResult createWindowSurface(EglShared& sh, EglContext& ctx, const TesseraContextConfig& c,
                                  const EglPlan& plan, const TesseraNativeWindow& win) {
    const EglFns& f = sh.api->fn;
    const bool strict = c.fallback == TESSERA_FALLBACK_STRICT;
    const bool wayland = win.kind == TESSERA_WINDOW_WAYLAND;
    const bool win32 =
        win.kind == TESSERA_WINDOW_WIN32; // an HWND, by value, through the plain entry point (ANGLE)

    // The native object EGL wants: Wayland needs a wl_egl_window wrapped around the caller's wl_surface
    // (ours, destroyed with the context); X11 takes the Window id (by pointer for the platform entry point).
    void* nativeWindow = nullptr;
    unsigned long xid = static_cast<unsigned long>(win.window);
    if (wayland) {
        const WaylandEgl* wlApi = waylandEgl();
        if (!wlApi)
            return fail(TESSERA_ERROR_UNSUPPORTED, "libwayland-egl is not available");
        ctx.waylandWindow = wlApi->create(reinterpret_cast<void*>(static_cast<std::uintptr_t>(win.window)),
                                          win.width, win.height);
        ctx.wl = wlApi;
        if (!ctx.waylandWindow)
            return fail(TESSERA_ERROR_PLATFORM, "wl_egl_window_create failed (is the wl_surface valid?)");
        nativeWindow = ctx.waylandWindow;
    }

    EGLint lastError = 0;
    // sRGB cannot be asked for in the config: try the sRGB surface first, then (best effort) without.
    for (int srgbPass = plan.srgb ? 0 : 1; srgbPass < 2; ++srgbPass) {
        const bool wantSrgb = srgbPass == 0;
        for (const FormatAttempt& a : formatAttempts(plan.samples, false, strict)) {
            const std::vector<EGLint> attribs = configAttribs(c, a, plan.floatColor, kWindowBit);
            EGLConfig configs[32] = {};
            EGLint count = 0;
            if (!f.eglChooseConfig(sh.dpy, attribs.data(), configs, 32, &count) || count <= 0)
                continue;
            for (EGLint i = 0; i < count; ++i) {
                std::vector<EGLint> surfaceAttribs;
                if (wantSrgb)
                    surfaceAttribs = {kGlColorspace, kGlColorspaceSrgb};
                surfaceAttribs.push_back(kNone);
                EGLSurface surface = nullptr;
                void* const nativeArg = wayland ? nativeWindow : static_cast<void*>(&xid);
                if (win32) {
                    surface = f.eglCreateWindowSurface(
                        sh.dpy, configs[i], reinterpret_cast<void*>(static_cast<std::uintptr_t>(win.window)),
                        surfaceAttribs.data());
                } else if (f.eglCreatePlatformWindowSurfaceEXT) {
                    surface = f.eglCreatePlatformWindowSurfaceEXT(sh.dpy, configs[i], nativeArg,
                                                                  surfaceAttribs.data());
                } else if (f.eglCreatePlatformWindowSurface) {
                    // EGLAttrib is pointer-sized
                    std::vector<intptr_t> wide(surfaceAttribs.begin(), surfaceAttribs.end());
                    surface = f.eglCreatePlatformWindowSurface(sh.dpy, configs[i], nativeArg, wide.data());
                } else {
                    surface = f.eglCreateWindowSurface(
                        sh.dpy, configs[i],
                        wayland ? nativeWindow : reinterpret_cast<void*>(static_cast<std::uintptr_t>(xid)),
                        surfaceAttribs.data());
                }
                if (!surface) {
                    lastError = f.eglGetError();
                    continue;
                }
                ctx.config = configs[i];
                ctx.surface = surface;
                ctx.windowSurface = true;
                if (a.samples != plan.samples || a.relaxDepth)
                    logf(TESSERA_LOG_INFO, "EGL: window config relaxed (samples %d->%d, depth relaxed %d)",
                         c.samples, a.samples, a.relaxDepth);
                return TESSERA_SUCCESS;
            }
        }
        if (wantSrgb) {
            if (strict)
                return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                            eglErrorText("no EGL config can back an sRGB window surface", lastError));
            logf(TESSERA_LOG_INFO, "EGL: sRGB window surface unavailable, request dropped");
        }
    }
    if (lastError == kBadAlloc)
        return fail(TESSERA_ERROR_OUT_OF_MEMORY, eglErrorText("eglCreateWindowSurface failed", lastError));
    return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                eglErrorText("no EGL config can render into this window", lastError));
}

// ---- the backend --------------------------------------------------------------------------------

/* "egl" is the platform's EGL (a GPU where there is one); "software" is the same code restricted to the CPU
 * renderer (Mesa's software device, or ANGLE's WARP / SwiftShader), selected by name only: it is never in the
 * default order (ADR 0015). */
class EglBackend final : public Backend {
  public:
    explicit EglBackend(bool software) : software_(software) {}

    const char* name() const noexcept override {
        return software_ ? "software" : "egl";
    }

    TesseraResult createContext(const TesseraContextConfig& c,
                                const std::shared_ptr<ContextObject>& shareWith,
                                std::shared_ptr<ContextObject>& out) override {
        std::string error;
        std::shared_ptr<EglShared> sh = getShared(error);
        if (!sh)
            return fail(TESSERA_ERROR_UNSUPPORTED, error);
        return create(sh, c, shareWith, nullptr, out);
    }

    /* A caller's X11 window (through our own connection to its server) or Wayland surface (on the caller's
     * wl_display). */
    TesseraResult createContextForNativeWindow(const TesseraContextConfig& c,
                                               const TesseraNativeWindow& window, uint32_t /*flags*/,
                                               const std::shared_ptr<ContextObject>& shareWith,
                                               std::shared_ptr<ContextObject>& out) override {
        if (window.kind == TESSERA_WINDOW_WIN32) { // ANGLE: the display it already has serves every HWND
#if defined(_WIN32)
            std::string error;
            std::shared_ptr<EglShared> sh = getShared(error);
            if (!sh)
                return fail(TESSERA_ERROR_UNSUPPORTED, error);
            return create(sh, c, shareWith, &window, out);
#else
            return TESSERA_ERROR_UNSUPPORTED;
#endif
        }
        if (window.kind != TESSERA_WINDOW_X11 && window.kind != TESSERA_WINDOW_WAYLAND)
            return TESSERA_ERROR_UNSUPPORTED;
        if (software_) // the windows of a window system are the GPU's; the CPU renderer draws offscreen
            return fail(TESSERA_ERROR_UNSUPPORTED,
                        "the software backend cannot attach to this kind of window");
        const EglApi* api = eglApi();
        if (!api)
            return fail(TESSERA_ERROR_UNSUPPORTED, "libEGL is not available on this system");
        const bool wayland = window.kind == TESSERA_WINDOW_WAYLAND;
        std::string error;
        void* nativeDisplay = nullptr;
        if (wayland) {
            nativeDisplay = reinterpret_cast<void*>(static_cast<std::uintptr_t>(window.display));
        } else {
            nativeDisplay = x11Display(error);
            if (!nativeDisplay)
                return fail(TESSERA_ERROR_UNSUPPORTED, error);
        }
        std::shared_ptr<EglShared> sh;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto key = std::make_pair(window.kind, reinterpret_cast<std::uintptr_t>(nativeDisplay));
            auto it = windowDisplays_.find(key);
            if (it == windowDisplays_.end()) {
                auto s = std::make_shared<EglShared>();
                s->api = api;
                if (!s->openNative(wayland ? kPlatformWayland : kPlatformX11, nativeDisplay,
                                   wayland ? "wayland" : "x11", error))
                    return fail(TESSERA_ERROR_UNSUPPORTED, error);
                it = windowDisplays_.emplace(key, std::move(s)).first;
            }
            sh = it->second;
        }
        return create(sh, c, shareWith, &window, out);
    }

    /* The integer form (an HWND) is ANGLE's on Windows; everywhere else this backend takes
     * TesseraNativeWindow only. */
    TesseraResult createContextForWindow(const TesseraContextConfig& c, uint64_t nativeWindow,
                                         uint32_t /*flags*/, const std::shared_ptr<ContextObject>& shareWith,
                                         std::shared_ptr<ContextObject>& out) override {
        TesseraNativeWindow w = tesseraDefaultNativeWindow();
        w.kind = TESSERA_WINDOW_WIN32;
        w.window = nativeWindow;
        return createContextForNativeWindow(c, w, 0, shareWith, out);
    }

    uint32_t nativeWindowKind() const noexcept override {
        return TESSERA_WINDOW_WIN32;
    }

    /* The creation path shared by pbuffer/surfaceless contexts (`window` null) and contexts for a caller's
     * window. */
    TesseraResult create(const std::shared_ptr<EglShared>& sh, const TesseraContextConfig& c,
                         const std::shared_ptr<ContextObject>& shareWith, const TesseraNativeWindow* window,
                         std::shared_ptr<ContextObject>& out) {
        const EglFns& f = sh->api->fn;

        const EGLenum apiEnum = c.api == TESSERA_API_OPENGL_ES ? kOpenGlEsApi : kOpenGlApi;
        if (!sh->supportsApi(apiEnum))
            return fail(TESSERA_ERROR_UNSUPPORTED,
                        std::string("this EGL implementation has no ") +
                            (c.api == TESSERA_API_OPENGL_ES ? "OpenGL ES" : "desktop OpenGL") + " support");

        std::shared_ptr<EglContext> parent;
        if (shareWith) {
            parent = std::dynamic_pointer_cast<EglContext>(shareWith);
            if (!parent)
                return fail(TESSERA_ERROR_INVALID_ARGUMENT,
                            "cannot share objects with a context from another backend");
            if (parent->apiEnum != apiEnum)
                return fail(TESSERA_ERROR_INVALID_ARGUMENT,
                            "cannot share objects between OpenGL and OpenGL ES contexts");
            if (parent->shared_->dpy != sh->dpy)
                return fail(TESSERA_ERROR_INVALID_ARGUMENT,
                            "cannot share objects between contexts on different EGL displays");
        }

        // EGL state is per thread and per API; put the caller's all back on every path.
        crossapi::EglGuard guard;

        EglPlan plan;
        if (TesseraResult r = planEgl(*sh, c, plan, window != nullptr); r != TESSERA_SUCCESS)
            return r;

        auto ctx = std::make_shared<EglContext>(sh); // RAII: any failure below cleans up through ~EglContext
        ctx->apiEnum = apiEnum;
        if (!f.eglBindAPI(apiEnum))
            return fail(TESSERA_ERROR_UNSUPPORTED, eglErrorText("eglBindAPI failed", f.eglGetError()));

        if (window) {
            if (TesseraResult r = createWindowSurface(*sh, *ctx, c, plan, *window); r != TESSERA_SUCCESS)
                return r;
        } else if (TesseraResult r = chooseConfig(*sh, c, plan, ctx->config); r != TESSERA_SUCCESS) {
            return r;
        }

        if (!window && !plan.surfaceless) {
            auto makePbuffer = [&](bool srgb) {
                std::vector<EGLint> attribs = {kWidth, 1, kHeight, 1};
                if (srgb)
                    attribs.insert(attribs.end(), {kGlColorspace, kGlColorspaceSrgb});
                attribs.push_back(kNone);
                return f.eglCreatePbufferSurface(sh->dpy, ctx->config, attribs.data());
            };
            ctx->surface = makePbuffer(plan.srgb);
            if (!ctx->surface && plan.srgb) {
                // The chosen config cannot back an sRGB surface (EGL has no config attribute to ask for one
                // up front).
                const EGLint err = f.eglGetError();
                if (c.fallback == TESSERA_FALLBACK_STRICT)
                    return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                                eglErrorText("no EGL config can back an sRGB surface", err));
                logf(TESSERA_LOG_INFO, "EGL: sRGB surface unavailable (%s), request dropped",
                     eglErrorText("eglCreatePbufferSurface", err).c_str());
                plan.srgb = false;
                ctx->surface = makePbuffer(false);
            }
            if (!ctx->surface)
                return fail(TESSERA_ERROR_PLATFORM,
                            eglErrorText("eglCreatePbufferSurface failed", f.eglGetError()));
        }

        if (TesseraResult r = createNativeContext(*sh, *ctx, c, plan, parent ? parent->ctx : nullptr);
            r != TESSERA_SUCCESS)
            return r;
        ctx->parent = std::move(parent);

        crossapi::releaseOtherGlxCurrent(); // the host's GLX context (guarded above) must not make ours fail
        if (!f.eglMakeCurrent(sh->dpy, ctx->surface, ctx->surface, ctx->ctx))
            return fail(TESSERA_ERROR_PLATFORM,
                        eglErrorText("eglMakeCurrent failed on the new context", f.eglGetError()));
        const bool ok = queryInfo(*sh, *ctx, plan.srgb, ctx->info);
        // `guard` releases the context again and restores the caller's state; it must not be left to us.
        if (!ok)
            return fail(TESSERA_ERROR_PLATFORM, "could not query the new context (glGetString failed)");

        // The software backend's promise is the CPU: a GPU that happens to be reached through a "software"
        // device is refused (WSL's D3D12 driver is; set LIBGL_ALWAYS_SOFTWARE=1 for Mesa's llvmpipe).
        // UNSUPPORTED, so "wgl,software" falls through.
        if (software_ && !isCpuRenderer(ctx->info.renderer))
            return fail(
                TESSERA_ERROR_UNSUPPORTED,
                std::string("the software backend found '") + ctx->info.renderer +
                    "', which is not a CPU renderer (Mesa: LIBGL_ALWAYS_SOFTWARE=1 selects llvmpipe)");

        // Never negotiable, whatever the fallback policy: API, minimum version, profile.
        if (const char* why = hardRequirementViolation(c, ctx->info))
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, why);

        if (f.eglQueryContext && sh->has("EGL_IMG_context_priority")) {
            EGLint level = 0;
            if (f.eglQueryContext(sh->dpy, ctx->ctx, kNativeContextPriorityLevel, &level))
                ctx->info.priority = priorityFromNative(level);
        }

        // Strict mode promises exactly what was asked for: verify the parts the driver may have loosened.
        if (c.fallback == TESSERA_FALLBACK_STRICT && !contextSatisfies(c, ctx->info))
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "the created context does not satisfy the strict request");

        // Reset polling and debug-message forwarding are bound to the context that is current right now.
        const auto glProc = [&](const char* n) -> void* { return sh->api->fn.eglGetProcAddress(n); };
        ctx->resetFn = resolveResetStatus(glProc);
        if (ctx->info.flags & TESSERA_FLAG_DEBUG)
            installGlDebugForwarder(glProc);

        out = std::move(ctx);
        return TESSERA_SUCCESS;
    }

    void* getProcAddress(const char* name) noexcept override {
        const EglApi* api = eglApi();
        if (!api || !name)
            return nullptr;
        // GLVND hands out a dispatch stub for any name; every GL function starts with "gl" and every EGL
        // function (core and extension: eglCreateImageKHR, eglExportDMABUFImageMESA, ...) with "egl", which
        // interop code needs through this call.
        const bool gl = name[0] == 'g' && name[1] == 'l';
        const bool egl = name[0] == 'e' && name[1] == 'g' && name[2] == 'l';
        if (!gl && !egl)
            return nullptr;
        return api->fn.eglGetProcAddress(name);
    }

  private:
    std::shared_ptr<EglShared> getShared(std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shared_)
            return shared_;
        const EglApi* api = eglApi();
        if (!api) {
            error = "libEGL is not available on this system";
            return nullptr;
        }
        auto s = std::make_shared<EglShared>();
        s->api = api;
        if (!s->open(error, software_))
            return nullptr;
        shared_ = s;
        return shared_;
    }

    const bool software_;
    std::mutex mutex_;
    std::shared_ptr<EglShared> shared_;
    // One EGLDisplay per caller window system connection: (window kind, native display) -> display state.
    std::map<std::pair<uint32_t, std::uintptr_t>, std::shared_ptr<EglShared>> windowDisplays_;
};

} // namespace

std::shared_ptr<Backend> makeEglBackend() {
    return std::make_shared<EglBackend>(false);
}

std::shared_ptr<Backend> makeSoftwareBackend() {
    return std::make_shared<EglBackend>(true);
}

} // namespace tessera::egl
