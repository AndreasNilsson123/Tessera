// Fault injection for the real backends (WGL, GLX, EGL), the counterpart of the CGL fake-driver sweep.
//
// The backends call the driver through function tables (and through wglGetProcAddress / glXGetProcAddress for
// extension functions). This test replaces those entries with wrappers that (a) fail the Nth call, or every
// call from the Nth on, and (b) count how many native objects (contexts, pbuffers, windows, colormaps) are
// alive. It then runs a fixed set of scenarios through the PUBLIC API against the real driver, once per
// injection point, and checks after every run:
//   - nothing crashed and every result code is one the library is allowed to return for a failing driver;
//   - a failed create returns no handle, leaves nothing of ours current on the thread and gives a message;
//   - a context created by "another library" is still current afterwards (unless the failing call was a
//   make-current, where
//     the documented policy is to leave the thread with nothing rather than with one of ours);
//   - after everything is destroyed and the library terminated, every native object that was created has been
//   destroyed.
//
// Not injectable here: direct Win32 calls (CreateWindowEx, SetPixelFormat, ...) on Windows and asynchronous X
// errors on X11; only calls that go through the tables above are covered. Needs a real driver (exit 77
// without one).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <tessera/tessera.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#include "platform/win32/wgl_api.hpp"
#elif defined(__linux__)
#include "platform/egl/egl_api.hpp"
#include "platform/x11/glx_api.hpp"
#endif

namespace {

// ---------------------------------------------------------------- injection machinery

enum Kind { kContext, kSurface, kWindow, kColormap, kKinds };
const char* const kKindNames[kKinds] = {"contexts", "pbuffers", "windows", "colormaps"};

std::atomic<long> g_live[kKinds];
std::atomic<long> g_calls{0};      // failable calls seen
std::atomic<long> g_countdown{-1}; // fail when this reaches 0; <0 = never
std::atomic<bool> g_persistent{false};
std::atomic<bool> g_fired{false};
std::string g_firedName; // the call that was failed first (written once, before g_fired is set)
std::atomic<bool> g_failuresOn{true};

bool inject(const char* name) {
    if (!g_failuresOn.load())
        return false;
    ++g_calls;
    if (g_persistent.load() && g_fired.load())
        return true;
    long c = g_countdown.load();
    while (c > 0) {
        if (g_countdown.compare_exchange_weak(c, c - 1)) {
            if (c - 1 == 0) {
                g_firedName = name;
                g_fired = true;
                return true;
            }
            return false;
        }
    }
    return false;
}

using FailOf = int;

template <int Tag, class R, class... A> struct Hook {
    using Fn = R (*)(A...);
    using FailT = std::conditional_t<std::is_void_v<R>, int, R>;
    static inline Fn orig = nullptr;
    static inline FailT fail{};
    static inline const char* name = "";
    static inline bool failable = false;
    static inline int kind = -1;
    static inline int delta = 0; // +1: creator (counted when it succeeds), -1: destroyer (always counted)

    static R call(A... a) {
        if constexpr (std::is_void_v<R>) {
            orig(a...);
            if (kind >= 0)
                g_live[kind] += delta;
        } else {
            if (failable && inject(name))
                return fail;
            R r = orig(a...);
            if (kind >= 0) {
                if (delta > 0) {
                    if (r != fail)
                        ++g_live[kind];
                } else {
                    --g_live[kind];
                }
            }
            return r;
        }
    }
};

// Replaces a table slot. `failable` calls can be failed; `kind`/`delta` keep the live-object counts.
template <class R, class... A> using FnPtr = R (*)(A...);

template <int Tag, class R, class... A>
void installHook(FnPtr<R, A...>& slot, const char* name, bool failable,
                 std::conditional_t<std::is_void_v<R>, int, R> failValue = {}, int kind = -1, int delta = 0) {
    using H = Hook<Tag, R, A...>;
    H::orig = slot;
    H::name = name;
    H::failable = failable;
    H::fail = failValue;
    H::kind = kind;
    H::delta = delta;
    slot = &H::call;
}
#define HOOK(slot, ...) installHook<__COUNTER__>(slot, #slot, __VA_ARGS__)

template <class H, class Fn>
void configureExt(Fn real, const char* name, bool failable, typename H::FailT failValue, int kind,
                  int delta) {
    H::orig = reinterpret_cast<typename H::Fn>(real);
    H::name = name;
    H::failable = failable;
    H::fail = failValue;
    H::kind = kind;
    H::delta = delta;
}

// ---------------------------------------------------------------- scenarios (public API only)

std::vector<std::string> g_problems;
int g_failures = 0;
std::string g_context; // what is being run, for messages

void problem(const std::string& what) {
    if (g_problems.size() < 40)
        g_problems.push_back(g_context + ": " + what);
    ++g_failures;
}

bool allowedFailure(TesseraResult r) {
    // What a failing driver may legitimately turn into. INVALID_ARGUMENT/INVALID_HANDLE/WRONG_THREAD/UNKNOWN
    // would mean the library mistook a driver failure for the caller's mistake or lost track of its own
    // state.
    return r == TESSERA_ERROR_UNSUPPORTED || r == TESSERA_ERROR_NO_MATCHING_CONFIG ||
           r == TESSERA_ERROR_PLATFORM || r == TESSERA_ERROR_OUT_OF_MEMORY || r == TESSERA_ERROR_CONTEXT_LOST;
}

struct Scenario {
    const char* name;
    TesseraContextConfig cfg;
    bool share; // also creates a second context sharing with the first
    bool use;   // make current, draw nothing, swap, release
};

TesseraContextConfig baseConfig() {
    TesseraContextConfig c = tesseraDefaultContextConfig();
    c.api = TESSERA_API_OPENGL;
    c.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    c.depthBits = 24;
    return c;
}

std::vector<Scenario> buildScenarios() {
    std::vector<Scenario> v;
    v.push_back({"plain", baseConfig(), false, true});
    {
        TesseraContextConfig c = baseConfig();
        c.surface = TESSERA_SURFACE_PBUFFER;
        v.push_back({"pbuffer", c, false, true});
    }
    v.push_back({"sharing", baseConfig(), true, true});
    {
        TesseraContextConfig c = baseConfig();
        c.versionMajor = 3;
        c.versionMinor = 3;
        c.profile = TESSERA_PROFILE_CORE;
        c.flags = TESSERA_FLAG_DEBUG;
        v.push_back({"core 3.3 + debug", c, false, true});
    }
    {
        TesseraContextConfig c = baseConfig();
        c.samples = 4;
        c.flags = TESSERA_FLAG_SRGB;
        v.push_back({"msaa + srgb ladder", c, false, false});
    }
    {
        TesseraContextConfig c = baseConfig();
        c.surface = TESSERA_SURFACE_NONE;
        v.push_back({"no surface", c, false, true});
    }
    return v;
}

struct Platform {
    const char* backend = "";
    std::function<bool()> available;
    std::function<void()> armForeign; // makes the other library's context current on this thread
    std::function<bool()> foreignIntact;
    std::function<void()> destroyForeign;
};

Platform g_platform;

TesseraResult create(const TesseraContextConfig& cfg, TesseraContext& out, const char* what) {
    out = 0xDEAD;
    if (g_platform.armForeign)
        g_platform.armForeign(); // every create starts with the other library's context current
    const TesseraResult r = tesseraCreateContext(&cfg, &out);
    if (r == TESSERA_SUCCESS) {
        if (out == 0 || out == 0xDEAD)
            problem(std::string(what) + ": success without a handle");
    } else {
        if (out != TESSERA_NULL_HANDLE)
            problem(std::string(what) + ": failed (" + tesseraResultString(r) + ") but returned a handle");
        if (!allowedFailure(r))
            problem(std::string(what) + ": failure surfaced as " + tesseraResultString(r) + " (" +
                    tesseraGetLastErrorMessage() + ")");
        if (tesseraGetLastErrorMessage()[0] == '\0')
            problem(std::string(what) + ": failure without a message");
        out = TESSERA_NULL_HANDLE;
    }
    if (tesseraGetCurrentContext() != TESSERA_NULL_HANDLE)
        problem(std::string(what) + ": a Tessera context was left current by create");
    // The other library's context must be exactly as it was (unless the failing call was a make-current: if
    // putting it back failed, the documented policy is to leave the thread with nothing rather than with one
    // of ours). (In the persistent mode every make-current fails once the fault has fired, so putting the
    // context back cannot work either.)
    const bool makeCurrentFault =
        g_fired.load() && (g_persistent.load() || g_firedName.find("akeCurrent") != std::string::npos ||
                           g_firedName.find("akeContextCurrent") != std::string::npos);
    if (g_platform.foreignIntact && !makeCurrentFault && !g_platform.foreignIntact())
        problem(std::string(what) +
                ": the other library's context is no longer current (fault: " + g_firedName + ")");
    if (g_platform.armForeign)
        g_platform.armForeign(); // (a failed restore, or the next scenario step, starts from the same state)
    return r;
}

void runScenario(const Scenario& s) {
    g_context += std::string(" / ") + s.name;
    std::vector<TesseraContext> live;
    TesseraContext a = 0;
    if (create(s.cfg, a, "create") == TESSERA_SUCCESS)
        live.push_back(a);
    if (a && s.share) {
        TesseraContextConfig shared = s.cfg;
        shared.shareWith = a;
        TesseraContext b = 0;
        if (create(shared, b, "create (sharing)") == TESSERA_SUCCESS)
            live.push_back(b);
    }
    if (a && s.use) {
        for (TesseraContext c : live) {
            const TesseraResult m = tesseraMakeCurrent(c);
            if (m != TESSERA_SUCCESS && !allowedFailure(m))
                problem(std::string("makeCurrent surfaced as ") + tesseraResultString(m));
            if (m == TESSERA_SUCCESS) {
                const TesseraResult sw = tesseraSwapBuffers(c);
                if (sw != TESSERA_SUCCESS && !allowedFailure(sw))
                    problem(std::string("swapBuffers surfaced as ") + tesseraResultString(sw));
                TesseraContextInfo info = tesseraDefaultContextInfo();
                if (tesseraGetContextInfo(c, &info) != TESSERA_SUCCESS)
                    problem("getContextInfo failed on a live context");
            }
            tesseraMakeCurrent(TESSERA_NULL_HANDLE);
        }
    }
    // Recreate (the path that builds a new context from a stored request) while one is alive.
    if (a && s.use) {
        TesseraContext again = 0;
        const TesseraResult r = tesseraRecreateContext(a, &again, nullptr);
        if (r == TESSERA_SUCCESS)
            live.push_back(again);
        else if (again != TESSERA_NULL_HANDLE)
            problem("recreate failed but returned a handle");
        else if (!allowedFailure(r))
            problem(std::string("recreate surfaced as ") + tesseraResultString(r));
    }
    for (TesseraContext c : live) {
        const TesseraResult d = tesseraDestroyContext(c);
        if (d != TESSERA_SUCCESS)
            problem(std::string("destroy failed: ") + tesseraResultString(d));
    }
    g_context.resize(g_context.size() - (std::string(" / ") + s.name).size());
}

// One full run: init, scenarios, terminate; then the invariants.
void oneRun(const std::string& label) {
    g_context = label;
    for (auto& l : g_live)
        l = 0;
    if (tesseraInit() != TESSERA_SUCCESS) {
        problem("tesseraInit failed");
        return;
    }
    if (g_platform.armForeign)
        g_platform.armForeign();
    for (const Scenario& s : buildScenarios())
        runScenario(s);
    if (tesseraGetCurrentContext() != TESSERA_NULL_HANDLE)
        problem("a Tessera context is still current at the end");
    tesseraTerminate();
    for (int k = 0; k < kKinds; ++k) {
        if (g_live[k] != 0)
            problem(std::string("leak: ") + std::to_string(g_live[k].load()) + " " + kKindNames[k] +
                    " alive after terminate (fault: " + (g_fired.load() ? g_firedName : std::string("none")) +
                    ")");
    }
}

void sweep() {
    // Calibration: how many failable calls does a clean run make?
    g_countdown = -1;
    g_persistent = false;
    g_fired = false;
    g_calls = 0;
    oneRun("clean run");
    const long total = g_calls.load();
    std::printf("  %ld failable driver calls in one run of %zu scenarios\n", total, buildScenarios().size());
    if (total < 3) { // (a software renderer with no extensions makes few failable calls: that is fine; none
                     // means the hooks are unused)
        problem("suspiciously few failable calls: the hooks are not being used");
        return;
    }
    long runs = 0;
    for (int mode = 0; mode < 2; ++mode) {
        for (long n = 1; n <= total; ++n) {
            g_persistent = mode == 1;
            g_fired = false;
            g_firedName.clear();
            g_countdown = n;
            oneRun(std::string(mode ? "from call " : "call ") + std::to_string(n));
            ++runs;
            if (!g_fired.load() && n < total - 5) {
                // the run took a shorter path than the calibration (earlier failure can skip later calls):
                // fine, but note it
            }
        }
    }
    g_countdown = -1;
    g_persistent = false;
    g_fired = false;
    oneRun("clean run afterwards"); // the library must be fully usable once the driver behaves again
    std::printf("  %ld fault runs (each call failed once, then every call from there on)\n", runs);
}

// ---------------------------------------------------------------- platform sections

#if defined(_WIN32)

using namespace tessera::win32;

HGLRC g_frc = nullptr;
HWND g_fwnd = nullptr;
HDC g_fdc = nullptr;
WglApi g_orig;

bool setupPlatform() {
    const WglApi* api = wglApi();
    if (!api)
        return false;
    WglApi* a = const_cast<WglApi*>(api);
    g_orig.createContext = a->createContext;
    g_orig.deleteContext = a->deleteContext;
    g_orig.makeCurrent = a->makeCurrent;
    g_orig.getCurrentContext = a->getCurrentContext;
    g_orig.getProcAddress = a->getProcAddress;

    HOOK(a->createContext, true, static_cast<HGLRC>(nullptr), kContext, 1);
    HOOK(a->deleteContext, false, FALSE, kContext, -1);
    HOOK(a->makeCurrent, true, FALSE);
    HOOK(a->shareLists, true, FALSE);

    // Extension functions come out of wglGetProcAddress: hand out wrapped ones.
    static auto realProc = a->getProcAddress;
    static auto wrapped = +[](LPCSTR name) -> PROC {
        PROC p = realProc(name);
        if (!p)
            return p;
        using CreateAttribs = Hook<9001, HGLRC, HDC, HGLRC, const int*>;
        using ChoosePf = Hook<9002, BOOL, HDC, const int*, const FLOAT*, UINT, int*, UINT*>;
        using GetPfAttrib = Hook<9003, BOOL, HDC, int, int, UINT, const int*, int*>;
        using CreatePb = Hook<9004, HANDLE, HDC, int, int, int, const int*>;
        using GetPbDc = Hook<9005, HDC, HANDLE>;
        using ReleasePbDc = Hook<9006, int, HANDLE, HDC>;
        using DestroyPb = Hook<9007, BOOL, HANDLE>;
        if (!std::strcmp(name, "wglCreateContextAttribsARB")) {
            configureExt<CreateAttribs>(p, name, true, nullptr, kContext, 1);
            return reinterpret_cast<PROC>(&CreateAttribs::call);
        }
        if (!std::strcmp(name, "wglChoosePixelFormatARB")) {
            configureExt<ChoosePf>(p, name, true, FALSE, -1, 0);
            return reinterpret_cast<PROC>(&ChoosePf::call);
        }
        if (!std::strcmp(name, "wglGetPixelFormatAttribivARB")) {
            configureExt<GetPfAttrib>(p, name, true, FALSE, -1, 0);
            return reinterpret_cast<PROC>(&GetPfAttrib::call);
        }
        if (!std::strcmp(name, "wglCreatePbufferARB")) {
            configureExt<CreatePb>(p, name, true, nullptr, kSurface, 1);
            return reinterpret_cast<PROC>(&CreatePb::call);
        }
        if (!std::strcmp(name, "wglGetPbufferDCARB")) {
            configureExt<GetPbDc>(p, name, true, nullptr, -1, 0);
            return reinterpret_cast<PROC>(&GetPbDc::call);
        }
        if (!std::strcmp(name, "wglReleasePbufferDCARB")) {
            configureExt<ReleasePbDc>(p, name, false, 0, -1, 0);
            return reinterpret_cast<PROC>(&ReleasePbDc::call);
        }
        if (!std::strcmp(name, "wglDestroyPbufferARB")) {
            configureExt<DestroyPb>(p, name, false, FALSE, kSurface, -1);
            return reinterpret_cast<PROC>(&DestroyPb::call);
        }
        return p;
    };
    a->getProcAddress = wrapped;

    // The "other library": a plain context on its own window, created through the unhooked entry points.
    g_fwnd =
        CreateWindowExW(0, L"STATIC", L"foreign", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, nullptr, nullptr);
    g_fdc = GetDC(g_fwnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    SetPixelFormat(g_fdc, ChoosePixelFormat(g_fdc, &pfd), &pfd);
    g_frc = g_orig.createContext(g_fdc);
    g_platform.backend = "wgl";
    g_platform.armForeign = [] { g_orig.makeCurrent(g_fdc, g_frc); };
    g_platform.foreignIntact = [] { return g_orig.getCurrentContext() == g_frc; };
    g_platform.destroyForeign = [] {
        g_orig.makeCurrent(nullptr, nullptr);
        if (g_frc)
            g_orig.deleteContext(g_frc);
        if (g_fdc)
            ReleaseDC(g_fwnd, g_fdc);
        if (g_fwnd)
            DestroyWindow(g_fwnd);
    };
    return g_frc != nullptr;
}

#elif defined(__linux__)

using namespace tessera::x11;
using tessera::egl::EglApi;

#if 0
#endif

// ---- GLX
X11Fns g_ox;
GlxFns g_og;
Display* g_fdpy = nullptr;
Window g_fwin = 0;
GLXContext g_fctx = nullptr;

bool setupGlx() {
    const LinuxApi* api = linuxApi();
    if (!api)
        return false;
    LinuxApi* a = const_cast<LinuxApi*>(api);
    g_ox = a->xlib;
    g_og = a->glx;
    HOOK(a->xlib.XCreateColormap, true, static_cast<Colormap>(0), kColormap, 1);
    HOOK(a->xlib.XFreeColormap, false, 0, kColormap, -1);
    HOOK(a->xlib.XCreateWindow, true, static_cast<Window>(0), kWindow, 1);
    HOOK(a->xlib.XDestroyWindow, false, 0, kWindow, -1);
    HOOK(a->glx.glXChooseFBConfig, true, static_cast<GLXFBConfig*>(nullptr));
    HOOK(a->glx.glXGetVisualFromFBConfig, true, static_cast<XVisualInfo*>(nullptr));
    HOOK(a->glx.glXGetFBConfigAttrib, true, 1);
    HOOK(a->glx.glXCreateNewContext, true, static_cast<GLXContext>(nullptr), kContext, 1);
    HOOK(a->glx.glXDestroyContext, false, 0, kContext, -1);
    HOOK(a->glx.glXMakeContextCurrent, true, static_cast<Bool>(0));
    HOOK(a->glx.glXCreatePbuffer, true, static_cast<GLXPbuffer>(0), kSurface, 1);
    HOOK(a->glx.glXDestroyPbuffer, false, 0, kSurface, -1);

    static auto realProc = a->glx.glXGetProcAddress;
    static auto wrapped = +[](const unsigned char* name) -> void* {
        void* p = realProc(name);
        using CreateAttribs = Hook<9101, GLXContext, Display*, GLXFBConfig, GLXContext, Bool, const int*>;
        if (p && !std::strcmp(reinterpret_cast<const char*>(name), "glXCreateContextAttribsARB")) {
            configureExt<CreateAttribs>(p, "glXCreateContextAttribsARB", true, nullptr, kContext, 1);
            return reinterpret_cast<void*>(&CreateAttribs::call);
        }
        return p;
    };
    a->glx.glXGetProcAddress = wrapped;

    g_ox.XInitThreads();
    g_fdpy = g_ox.XOpenDisplay(nullptr);
    if (!g_fdpy)
        return false;
    const int screen = g_ox.XDefaultScreen(g_fdpy);
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
    GLXFBConfig* list = g_og.glXChooseFBConfig(g_fdpy, screen, attribs, &n);
    if (!list || n == 0)
        return false;
    GLXFBConfig fb = list[0];
    g_ox.XFree(list);
    XVisualInfo* vi = g_og.glXGetVisualFromFBConfig(g_fdpy, fb);
    if (!vi)
        return false;
    const Window root = g_ox.XRootWindow(g_fdpy, screen);
    const Colormap cmap = g_ox.XCreateColormap(g_fdpy, root, vi->visual, kAllocNone);
    XSetWindowAttributes wa = {};
    wa.colormap = cmap;
    wa.override_redirect = 1;
    g_fwin = g_ox.XCreateWindow(g_fdpy, root, 0, 0, 1, 1, 0, vi->depth, kInputOutput, vi->visual,
                                kCWColormap | kCWOverrideRedirect, &wa);
    g_ox.XFree(vi);
    g_fctx = g_og.glXCreateNewContext(g_fdpy, fb, kGlxRgbaType, nullptr, 1);
    g_platform.backend = "glx";
    g_platform.armForeign = [] { g_og.glXMakeContextCurrent(g_fdpy, g_fwin, g_fwin, g_fctx); };
    g_platform.foreignIntact = [] {
        return g_og.glXGetCurrentContext() == g_fctx && g_og.glXGetCurrentDrawable() == g_fwin;
    };
    g_platform.destroyForeign = [] {
        if (g_og.glXGetCurrentContext() == g_fctx)
            g_og.glXMakeContextCurrent(g_fdpy, 0, 0, nullptr);
        if (g_fctx)
            g_og.glXDestroyContext(g_fdpy, g_fctx);
        if (g_fwin)
            g_ox.XDestroyWindow(g_fdpy, g_fwin);
    };
    return g_fwin != 0 && g_fctx != nullptr;
}

// ---- EGL
tessera::egl::EglFns g_oe;
tessera::egl::EGLDisplay g_edpy = nullptr;
tessera::egl::EGLContext g_ectx = nullptr;
tessera::egl::EGLSurface g_esurf = nullptr;

bool setupEgl() {
    using namespace tessera::egl;
    const EglApi* api = eglApi();
    if (!api)
        return false;
    EglApi* a = const_cast<EglApi*>(api);
    g_oe = a->fn;
    HOOK(a->fn.eglInitialize, true, static_cast<EGLBoolean>(0));
    HOOK(a->fn.eglBindAPI, true, static_cast<EGLBoolean>(0));
    HOOK(a->fn.eglChooseConfig, true, static_cast<EGLBoolean>(0));
    HOOK(a->fn.eglGetConfigAttrib, true, static_cast<EGLBoolean>(0));
    HOOK(a->fn.eglCreateContext, true, static_cast<EGLContext>(nullptr), kContext, 1);
    HOOK(a->fn.eglDestroyContext, false, static_cast<EGLBoolean>(0), kContext, -1);
    HOOK(a->fn.eglMakeCurrent, true, static_cast<EGLBoolean>(0));
    HOOK(a->fn.eglCreatePbufferSurface, true, static_cast<EGLSurface>(nullptr), kSurface, 1);
    HOOK(a->fn.eglDestroySurface, false, static_cast<EGLBoolean>(0), kSurface, -1);
    if (a->fn.eglGetPlatformDisplayEXT)
        HOOK(a->fn.eglGetPlatformDisplayEXT, true, static_cast<EGLDisplay>(nullptr));

    g_edpy = g_oe.eglGetDisplay(nullptr);
    EGLint major = 0, minor = 0;
    if (!g_edpy || !g_oe.eglInitialize(g_edpy, &major, &minor) || !g_oe.eglBindAPI(kOpenGlApi))
        return false;
    const EGLint attribs[] = {kSurfaceType, kPbufferBit, kRenderableType, kOpenGlBit, kRedSize, 8,
                              kGreenSize,   8,           kBlueSize,       8,          kNone};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!g_oe.eglChooseConfig(g_edpy, attribs, &cfg, 1, &n) || n == 0)
        return false;
    g_ectx = g_oe.eglCreateContext(g_edpy, cfg, nullptr, nullptr);
    const EGLint pb[] = {kWidth, 1, kHeight, 1, kNone};
    g_esurf = g_oe.eglCreatePbufferSurface(g_edpy, cfg, pb);
    g_platform.backend = "egl";
    g_platform.armForeign = [] {
        g_oe.eglBindAPI(kOpenGlApi);
        g_oe.eglMakeCurrent(g_edpy, g_esurf, g_esurf, g_ectx);
    };
    g_platform.foreignIntact = [] {
        g_oe.eglBindAPI(kOpenGlApi);
        return g_oe.eglGetCurrentContext() == g_ectx && g_oe.eglGetCurrentSurface(kDraw) == g_esurf;
    };
    g_platform.destroyForeign = [] {
        g_oe.eglBindAPI(kOpenGlApi);
        g_oe.eglMakeCurrent(g_edpy, nullptr, nullptr, nullptr);
        if (g_ectx)
            g_oe.eglDestroyContext(g_edpy, g_ectx);
        if (g_esurf)
            g_oe.eglDestroySurface(g_edpy, g_esurf);
    };
    return g_ectx != nullptr && g_esurf != nullptr;
}

#endif

} // namespace

int main(int argc, char** argv) {
    const char* backend = nullptr;
#if defined(__linux__)
    backend = argc > 1 ? argv[1]
                       : "glx"; // glx or egl; one backend per process (the hooks patch process-wide tables)
#else
    (void)argc;
    (void)argv;
#endif
    std::printf("test_fault: %s\n", backend ? backend : "wgl");

    bool ready = false;
#if defined(_WIN32)
    ready = setupPlatform();
#elif defined(__linux__)
    setenv("TESSERA_BACKEND", backend, 1);
    ready = !std::strcmp(backend, "egl") ? setupEgl() : setupGlx();
#endif
    if (!ready) {
        std::fprintf(stderr, "test_fault: no usable driver/display for this backend here, skipping\n");
        return 77;
    }
    // Is a context creatable at all without faults? (Otherwise there is nothing to sweep.)
    {
        g_failuresOn = false;
        if (tesseraInit() != TESSERA_SUCCESS)
            return 1;
        TesseraContextConfig c = baseConfig();
        TesseraContext ctx = 0;
        const TesseraResult r = tesseraCreateContext(&c, &ctx);
        if (r == TESSERA_ERROR_UNSUPPORTED) {
            std::fprintf(stderr, "test_fault: backend unavailable here: %s\n", tesseraGetLastErrorMessage());
            return 77;
        }
        if (ctx)
            tesseraDestroyContext(ctx);
        tesseraTerminate();
        g_failuresOn = true;
    }

    sweep();

    if (g_platform.destroyForeign)
        g_platform.destroyForeign();
    for (const std::string& p : g_problems)
        std::fprintf(stderr, "PROBLEM: %s\n", p.c_str());
    if (g_failures == 0)
        std::puts("test_fault: OK");
    else
        std::fprintf(stderr, "test_fault: %d problem(s)%s\n", g_failures,
                     g_problems.size() < static_cast<size_t>(g_failures) ? " (first 40 shown)" : "");
    return g_failures == 0 ? 0 : 1;
}
