// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
// Integration tests for the WGL backend, through the public C API only. Needs a working OpenGL driver
// (a software renderer is fine). A raw WGL context created here plays the role of "another library in the
// same process" (JavaFX, AWT, LWJGL...) whose current context Tessera must never disturb.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>

#include <dxgi.h>

#include <tessera/tessera.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

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

#include "interop_common.hpp"
#include "robustness_common.hpp"

using GlGetString = const unsigned char*(WINAPI*)(unsigned);
using GlGenTextures = void(WINAPI*)(int, unsigned*);
using GlIsTexture = unsigned char(WINAPI*)(unsigned);
using GlBindTexture = void(WINAPI*)(unsigned, unsigned);

static TesseraContextConfig baseConfig() {
    TesseraContextConfig c = tesseraDefaultContextConfig();
    c.api = TESSERA_API_OPENGL;
    c.flags = TESSERA_FLAG_DOUBLE_BUFFER;
    return c;
}

// ---------------------------------------------------------------- a "foreign" library's context

struct ForeignContext {
    HWND hwnd = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    bool create() {
        hwnd = CreateWindowExW(0, L"STATIC", L"foreign", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, nullptr,
                               nullptr);
        if (!hwnd)
            return false;
        dc = GetDC(hwnd);
        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof pfd;
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        const int pf = ChoosePixelFormat(dc, &pfd);
        if (!pf || !SetPixelFormat(dc, pf, &pfd))
            return false;
        rc = wglCreateContext(dc);
        return rc != nullptr;
    }
    void makeCurrent() const {
        wglMakeCurrent(dc, rc);
    }
    bool isCurrent() const {
        return wglGetCurrentContext() == rc && wglGetCurrentDC() == dc;
    }
    void destroy() {
        if (wglGetCurrentContext() == rc)
            wglMakeCurrent(nullptr, nullptr);
        if (rc)
            wglDeleteContext(rc);
        if (hwnd) {
            ReleaseDC(hwnd, dc);
            DestroyWindow(hwnd);
        }
        *this = ForeignContext{};
    }
};

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
    CHECK(info.profile == TESSERA_PROFILE_COMPATIBILITY); // plain request = legacy/compatibility context
    CHECK(info.versionMajor >= 1);
    CHECK(info.renderer[0] != '\0' && info.version[0] != '\0');
    CHECK(info.redBits >= 8 && info.greenBits >= 8 && info.blueBits >= 8);
    CHECK(info.flags & TESSERA_FLAG_DOUBLE_BUFFER);
    CHECK(info.surface == TESSERA_SURFACE_HIDDEN_WINDOW);

    // Creating a context leaves nothing current.
    CHECK(wglGetCurrentContext() == nullptr);
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

    // Our context made current, then the foreign library takes the thread back: our release must not drop it.
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    CHECK(!foreign.isCurrent());
    foreign.makeCurrent();
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    CHECK(foreign.isCurrent());

    // Releasing when we never made anything current is a no-op that leaves the foreign context alone.
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    CHECK(foreign.isCurrent());

    // Destroying a context the foreign library displaced must not touch the foreign context either.
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    foreign.makeCurrent();
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
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
    CHECK(wglGetCurrentContext() != nullptr);

    auto glGetString = reinterpret_cast<GlGetString>(tesseraGetProcAddress("glGetString"));
    CHECK(glGetString != nullptr);
    if (glGetString)
        CHECK(std::strcmp(reinterpret_cast<const char*>(glGetString(0x1F01)), info.renderer) == 0);
    CHECK(tesseraGetProcAddress("glThisFunctionDoesNotExist") == nullptr);
    CHECK(tesseraGetProcAddress(nullptr) == nullptr);
    if (info.versionMajor >= 2)
        CHECK(tesseraGetProcAddress("glGenBuffers") != nullptr);

    // WGL_EXT_swap_control is missing on Microsoft's GDI software renderer: UNSUPPORTED is the right answer
    // there.
    for (int interval = 0; interval <= 1; ++interval) {
        const TesseraResult si = tesseraSetSwapInterval(ctx, interval);
        CHECK(si == TESSERA_SUCCESS || si == TESSERA_ERROR_UNSUPPORTED);
    }
    EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS);

    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    CHECK(wglGetCurrentContext() == nullptr);
    CHECK(tesseraGetCurrentContext() == TESSERA_NULL_HANDLE);
    EXPECT(tesseraSetSwapInterval(ctx, 1), TESSERA_ERROR_WRONG_THREAD); // not current anywhere

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

    // Destroy from a different thread than the one that created it (window lives on Tessera's host thread).
    std::thread([&] { EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS); }).join();

    // Create on one thread, use and destroy on others.
    TesseraContext made = 0;
    std::thread([&] { EXPECT(tesseraCreateContext(&cfg, &made), TESSERA_SUCCESS); }).join();
    std::thread([&] {
        EXPECT(tesseraMakeCurrent(made), TESSERA_SUCCESS);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    }).join();
    std::thread([&] { EXPECT(tesseraDestroyContext(made), TESSERA_SUCCESS); }).join();
}

static void testConfigNegotiation() {
    // Strict + impossible requests fail with a specific code instead of returning something weaker.
    TesseraContextConfig bad = baseConfig();
    bad.fallback = TESSERA_FALLBACK_STRICT;
    bad.versionMajor = 9;
    bad.versionMinor = 9;
    TesseraContext none = 0;
    EXPECT(tesseraCreateContext(&bad, &none), TESSERA_ERROR_NO_MATCHING_CONFIG);
    CHECK(none == TESSERA_NULL_HANDLE);

    // Best effort relaxes multisampling instead of failing.
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

    // Core + debug + forward-compatible, strict. Drivers without 3.3 core legitimately say no match.
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

    // Core with no version: the ladder should find the newest the driver offers.
    TesseraContextConfig ladder = baseConfig();
    ladder.profile = TESSERA_PROFILE_CORE;
    if (tesseraCreateContext(&ladder, &ctx) == TESSERA_SUCCESS) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        std::printf("  core ladder -> GL %d.%d\n", info.versionMajor, info.versionMinor);
        CHECK(info.versionMajor >= 3);
        tesseraDestroyContext(ctx);
    }

    // OpenGL ES through WGL: only where the driver exposes it.
    TesseraContextConfig es = baseConfig();
    es.api = TESSERA_API_OPENGL_ES;
    const TesseraResult er = tesseraCreateContext(&es, &ctx);
    CHECK(er == TESSERA_SUCCESS || er == TESSERA_ERROR_UNSUPPORTED || er == TESSERA_ERROR_NO_MATCHING_CONFIG);
    std::printf("  OpenGL ES request: %s\n", tesseraResultString(er));
    if (er == TESSERA_SUCCESS)
        tesseraDestroyContext(ctx);

    // Surface types.
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
        bindTexture(0x0DE1, tex); // texture names only become "real" once bound
        CHECK(tex != 0 && isTexture(tex));
        EXPECT(tesseraMakeCurrent(b), TESSERA_SUCCESS); // switching our own contexts on one thread
        CHECK(tesseraGetCurrentContext() == b);
        CHECK(isTexture(tex)); // visible through the share group
    }
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);

    // Destroying the parent first leaves the child fully working.
    EXPECT(tesseraDestroyContext(a), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(b), TESSERA_SUCCESS);
    if (isTexture)
        CHECK(isTexture(tex));
    EXPECT(tesseraDestroyContext(b), TESSERA_SUCCESS); // destroys while current on this thread
    CHECK(wglGetCurrentContext() == nullptr);
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

static void testNoLeaks() {
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
    const DWORD gdi0 = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD user0 = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    cycle(300);
    const DWORD gdi1 = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD user1 = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    std::printf("  GDI objects %lu -> %lu, USER objects %lu -> %lu after 300 create/destroy cycles\n",
                (unsigned long)gdi0, (unsigned long)gdi1, (unsigned long)user0, (unsigned long)user1);
    CHECK(gdi1 <= gdi0 + 2);
    CHECK(user1 <= user0 + 2);
}

static void testTerminateAndReinit() {
    TesseraContextConfig cfg = baseConfig();
    TesseraContext leaked = 0;
    EXPECT(tesseraCreateContext(&cfg, &leaked), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(leaked), TESSERA_SUCCESS); // leaked while current on this thread

    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    CHECK(wglGetCurrentContext() == nullptr);
    EXPECT(tesseraDestroyContext(leaked), TESSERA_ERROR_INVALID_HANDLE);

    EXPECT(tesseraInit(), TESSERA_SUCCESS); // host thread and window class are recreated
    TesseraContext again = 0;
    EXPECT(tesseraCreateContext(&cfg, &again), TESSERA_SUCCESS);
    if (again)
        EXPECT(tesseraDestroyContext(again), TESSERA_SUCCESS);
}

static robust::Hooks wglHooks() {
    robust::Hooks h;
    h.base = baseConfig();
    h.nothingCurrentNatively = [] { return wglGetCurrentContext() == nullptr; };
    return h;
}
static void testDeferredDestroy() {
    robust::deferredDestroy(wglHooks());
}
static void testResetStatus() {
    robust::resetStatus(wglHooks());
}
static void testDebugForwarding() {
    robust::debugForwarding(wglHooks());
}
static void testAttributeMatrix() {
    ForeignContext foreign;
    robust::Hooks h = wglHooks();
    if (foreign.create()) {
        h.armForeign = [&] { foreign.makeCurrent(); };
        h.foreignIntact = [&] { return foreign.isCurrent(); };
    }
    robust::reportMatrix(h);
    foreign.destroy();
}

// ---------------------------------------------------------------- Phase 8

static void testPrewarm() {
    interop::prewarm(wglHooks());
}
static void testProbe() {
    ForeignContext foreign; // a context from "another library" must survive every probe
    robust::Hooks h = wglHooks();
    if (foreign.create()) {
        h.armForeign = [&] { foreign.makeCurrent(); };
        h.foreignIntact = [&] { return foreign.isCurrent(); };
    }
    interop::probe(h);
    foreign.destroy();
}

static void testNativeHandles() {
    interop::HandleExpectations e;
    e.kind = TESSERA_NATIVE_WGL;
    e.contextIndex = 0;
    e.mustBeNonZero = {0, 1, 2, 4}; // HGLRC, HDC, HWND (the hidden window), pixel format
    e.nativeCurrent = [] {
        return static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(wglGetCurrentContext()));
    };
    interop::nativeHandles(wglHooks(), e);

    // The native objects are real: the DC and context can be used directly by another library.
    TesseraContextConfig cfg = baseConfig();
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    TesseraNativeHandles nh = tesseraDefaultNativeHandles();
    EXPECT(tesseraGetNativeHandles(ctx, &nh), TESSERA_SUCCESS);
    CHECK(GetPixelFormat(reinterpret_cast<HDC>(static_cast<std::uintptr_t>(nh.handles[1]))) ==
          static_cast<int>(nh.handles[4]));
    CHECK(IsWindow(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(nh.handles[2]))));
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);

    // A pbuffer context reports the pbuffer instead of a window.
    TesseraContextConfig pb = baseConfig();
    pb.surface = TESSERA_SURFACE_PBUFFER;
    if (tesseraCreateContext(&pb, &ctx) == TESSERA_SUCCESS) {
        nh = tesseraDefaultNativeHandles();
        EXPECT(tesseraGetNativeHandles(ctx, &nh), TESSERA_SUCCESS);
        CHECK(nh.handles[3] != 0);
        EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    }
}

// ---------------------------------------------------------------- GPU selection (adapterLuid /
// gpuPreference)

struct TestAdapter {
    uint64_t luid;
    unsigned vendor;
    bool software;
    std::string name;
};

static std::vector<TestAdapter> dxgiAdapters() {
    std::vector<TestAdapter> v;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f))) || !f)
        return v;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 d = {};
        a->GetDesc1(&d);
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name - 1, nullptr, nullptr);
        v.push_back({(static_cast<uint64_t>(static_cast<uint32_t>(d.AdapterLuid.HighPart)) << 32) |
                         d.AdapterLuid.LowPart,
                     d.VendorId, (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0, name});
        a->Release();
    }
    f->Release();
    return v;
}

static void testAdapterSelection() {
    TesseraContextConfig base = baseConfig();
    base.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraContext probe = 0;
    EXPECT(tesseraCreateContext(&base, &probe), TESSERA_SUCCESS);
    TesseraContextInfo info = tesseraDefaultContextInfo();
    if (probe)
        tesseraGetContextInfo(probe, &info);
    if (probe)
        tesseraDestroyContext(probe);
    const std::string glText = std::string(info.vendor) + " " + info.renderer;
    const bool microsoftGl = glText.find("Microsoft") != std::string::npos;
    const auto adapters = dxgiAdapters();
    std::printf("  OpenGL driver: %s; %zu DXGI adapter(s)\n", glText.c_str(), adapters.size());
    for (const auto& a : adapters)
        std::printf("    %s vendor 0x%04x luid %016llx%s\n", a.name.c_str(), a.vendor,
                    static_cast<unsigned long long>(a.luid), a.software ? " (software)" : "");

    // Which DXGI adapter is the OpenGL driver's own? (by vendor; only meaningful when exactly one hardware
    // adapter matches)
    const TestAdapter* mine = nullptr;
    int hardware = 0;
    for (const auto& a : adapters) {
        if (a.software)
            continue;
        ++hardware;
        const bool nv = a.vendor == 0x10DE && glText.find("NVIDIA") != std::string::npos;
        const bool amd = (a.vendor == 0x1002 || a.vendor == 0x1022) &&
                         (glText.find("ATI") != std::string::npos || glText.find("AMD") != std::string::npos);
        const bool intel = a.vendor == 0x8086 && glText.find("Intel") != std::string::npos;
        if (nv || amd || intel)
            mine = &a;
    }

    TesseraContext ctx = 0;
    // Unknown LUID: refused, with a reason, never a handle.
    TesseraContextConfig bogus = base;
    bogus.adapterLuid = 0x1234567800000001ull;
    EXPECT(tesseraCreateContext(&bogus, &ctx), TESSERA_ERROR_NO_MATCHING_CONFIG);
    CHECK(ctx == TESSERA_NULL_HANDLE && std::strstr(tesseraGetLastErrorMessage(), "LUID") != nullptr);

    // Both fields together, and an unknown preference, are argument errors.
    TesseraContextConfig both = base;
    both.adapterLuid = 1;
    both.gpuPreference = TESSERA_GPU_HIGH_PERFORMANCE;
    EXPECT(tesseraCreateContext(&both, &ctx), TESSERA_ERROR_INVALID_ARGUMENT);
    TesseraContextConfig unknown = base;
    unknown.gpuPreference = 9;
    EXPECT(tesseraCreateContext(&unknown, &ctx), TESSERA_ERROR_INVALID_ARGUMENT);

    // An adapter that is not the one the OpenGL driver belongs to is refused, in best-effort mode too (an
    // explicit LUID is hard).
    for (const auto& a : adapters) {
        if (&a == mine || microsoftGl)
            continue;
        TesseraContextConfig other = base;
        other.adapterLuid = a.luid;
        const TesseraResult r = tesseraCreateContext(&other, &ctx);
        if (a.software && hardware == 1 && mine) {
            // the software adapter (Microsoft Basic Render Driver) is not what NVIDIA's OpenGL driver runs on
            CHECK(r == TESSERA_ERROR_NO_MATCHING_CONFIG);
            CHECK(std::strstr(tesseraGetLastErrorMessage(), "does not belong to the requested adapter") !=
                  nullptr);
        }
        if (r == TESSERA_SUCCESS && ctx) {
            std::fprintf(stderr, "adapter %s was accepted but the OpenGL driver is %s\n", a.name.c_str(),
                         glText.c_str());
            ++g_failures;
            tesseraDestroyContext(ctx);
        }
        ctx = 0;
    }

    if (mine) {
        // The real one works, by LUID and by preference.
        TesseraContextConfig byLuid = base;
        byLuid.adapterLuid = mine->luid;
        EXPECT(tesseraCreateContext(&byLuid, &ctx), TESSERA_SUCCESS);
        if (ctx) {
            TesseraContextInfo got = tesseraDefaultContextInfo();
            tesseraGetContextInfo(ctx, &got);
            CHECK(std::strcmp(got.renderer, info.renderer) == 0);
            EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
        }
        if (hardware == 1) { // with a single hardware adapter, both preferences name it
            for (unsigned pref : {TESSERA_GPU_HIGH_PERFORMANCE, TESSERA_GPU_LOW_POWER}) {
                TesseraContextConfig p = base;
                p.fallback = TESSERA_FALLBACK_STRICT;
                p.gpuPreference = pref;
                EXPECT(tesseraCreateContext(&p, &ctx), TESSERA_SUCCESS);
                if (ctx)
                    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
            }
        }
        // Pre-warmed contexts are keyed on the request: a default pool entry is not handed out for an adapter
        // request.
        TesseraContextConfig plain = base;
        EXPECT(tesseraPrewarmContexts(&plain, 1), TESSERA_SUCCESS);
        const int before = tesseraPooledContextCount();
        EXPECT(tesseraCreateContext(&byLuid, &ctx), TESSERA_SUCCESS);
        CHECK(tesseraPooledContextCount() == before); // the pooled default context was left alone
        if (ctx)
            EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
        TesseraProbeReport rep = tesseraDefaultProbeReport();
        EXPECT(tesseraProbe(&byLuid, &rep), TESSERA_SUCCESS);
        TesseraProbeReport badRep = tesseraDefaultProbeReport();
        EXPECT(tesseraProbe(&bogus, &badRep), TESSERA_ERROR_NO_MATCHING_CONFIG);
        CHECK(badRep.message[0] != '\0');
    } else {
        std::printf(
            "  (the OpenGL driver does not map to exactly one DXGI adapter here: positive cases skipped)\n");
    }
}

static HWND rawWindow(bool withPixelFormat, bool doubleBuffer) {
    HWND w =
        CreateWindowExW(0, L"STATIC", L"attach", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    if (w && withPixelFormat) {
        HDC dc = GetDC(w);
        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof pfd;
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | (doubleBuffer ? PFD_DOUBLEBUFFER : 0);
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        const int pf = ChoosePixelFormat(dc, &pfd);
        SetPixelFormat(dc, pf, &pfd);
        ReleaseDC(w, dc);
    }
    return w;
}

static int windowPixelFormat(HWND w) {
    HDC dc = GetDC(w);
    const int pf = GetPixelFormat(dc);
    ReleaseDC(w, dc);
    return pf;
}

static uint64_t id(HWND w) {
    return static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(w));
}

static void testWindowAttach() {
    using GlClearColor = void(WINAPI*)(float, float, float, float);
    using GlClear = void(WINAPI*)(unsigned);
    using GlReadPixels = void(WINAPI*)(int, int, int, int, unsigned, unsigned, void*);
    TesseraContextConfig cfg = baseConfig(); // asks for a double-buffered format
    TesseraContext ctx = 0;

    // A window that has no pixel format: refused unless the caller allows us to give it one, and left exactly
    // as it was.
    HWND fresh = rawWindow(false, false);
    CHECK(fresh != nullptr);
    EXPECT(tesseraCreateContextForWindow(&cfg, id(fresh), 0, &ctx), TESSERA_ERROR_INVALID_ARGUMENT);
    CHECK(std::strstr(tesseraGetLastErrorMessage(), "does not modify windows it does not own") != nullptr);
    CHECK(ctx == TESSERA_NULL_HANDLE);
    CHECK(windowPixelFormat(fresh) == 0);

    // With permission: Tessera sets the pixel format (once, for good) and renders into the window.
    EXPECT(tesseraCreateContextForWindow(&cfg, id(fresh), TESSERA_ATTACH_ALLOW_PIXEL_FORMAT, &ctx),
           TESSERA_SUCCESS);
    if (ctx) {
        CHECK(windowPixelFormat(fresh) != 0);
        TesseraContextInfo info = tesseraDefaultContextInfo();
        EXPECT(tesseraGetContextInfo(ctx, &info), TESSERA_SUCCESS);
        CHECK(info.surface == TESSERA_SURFACE_WINDOW);
        CHECK(info.flags & TESSERA_FLAG_DOUBLE_BUFFER);
        TesseraNativeHandles nh = tesseraDefaultNativeHandles();
        EXPECT(tesseraGetNativeHandles(ctx, &nh), TESSERA_SUCCESS);
        CHECK(nh.handles[2] == id(fresh) && nh.handles[3] == 0);

        EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
        auto clearColor = reinterpret_cast<GlClearColor>(tesseraGetProcAddress("glClearColor"));
        auto clear = reinterpret_cast<GlClear>(tesseraGetProcAddress("glClear"));
        auto readPixels = reinterpret_cast<GlReadPixels>(tesseraGetProcAddress("glReadPixels"));
        CHECK(clearColor && clear && readPixels);
        if (clearColor && clear && readPixels) {
            clearColor(1.f, 0.f, 0.f, 1.f);
            clear(0x4000); // GL_COLOR_BUFFER_BIT
            unsigned char px[4] = {};
            readPixels(0, 0, 1, 1, 0x1908, 0x1401, px); // the window's back buffer
            CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255);
        }
        EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS); // real presentation to the caller's window
        const TesseraResult si = tesseraSetSwapInterval(ctx, 0);
        CHECK(si == TESSERA_SUCCESS || si == TESSERA_ERROR_UNSUPPORTED);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
        EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
        // Destroying the context never touches the window.
        CHECK(IsWindow(fresh));
        CHECK(windowPixelFormat(fresh) != 0);

        // The window now has a format, so it can be attached again without any permission: it is adopted.
        EXPECT(tesseraCreateContextForWindow(&cfg, id(fresh), 0, &ctx), TESSERA_SUCCESS);
        if (ctx)
            EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    }

    // A window whose owner already chose a (single-buffered) format: adopted as it is, then checked against
    // the request.
    HWND owned = rawWindow(true, false);
    TesseraContextConfig strict = cfg;
    strict.fallback =
        TESSERA_FALLBACK_STRICT; // wants double buffering, which this window's format cannot give
    EXPECT(tesseraCreateContextForWindow(&strict, id(owned), 0, &ctx), TESSERA_ERROR_NO_MATCHING_CONFIG);
    CHECK(ctx == TESSERA_NULL_HANDLE);
    TesseraContextConfig relaxed = cfg;
    relaxed.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    EXPECT(tesseraCreateContextForWindow(&relaxed, id(owned), TESSERA_ATTACH_ALLOW_PIXEL_FORMAT, &ctx),
           TESSERA_SUCCESS);
    if (ctx) {
        TesseraContextInfo info = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &info);
        CHECK(!(info.flags & TESSERA_FLAG_DOUBLE_BUFFER)); // reported truthfully: the window decided
        EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    }

    // A window destroyed while a context renders into it: errors, never a crash, and the context can still be
    // destroyed.
    HWND doomed = rawWindow(false, false);
    EXPECT(tesseraCreateContextForWindow(&relaxed, id(doomed), TESSERA_ATTACH_ALLOW_PIXEL_FORMAT, &ctx),
           TESSERA_SUCCESS);
    if (ctx) {
        EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
        DestroyWindow(doomed);
        EXPECT(tesseraSwapBuffers(ctx), TESSERA_ERROR_PLATFORM);
        CHECK(std::strstr(tesseraGetLastErrorMessage(), "no longer exists") != nullptr);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
        EXPECT(tesseraMakeCurrent(ctx), TESSERA_ERROR_PLATFORM);
        EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    }

    // Created on another thread than the one that owns the window: windows are thread-affine for messages,
    // not for rendering.
    HWND crossThread = rawWindow(false, false);
    std::thread([&] {
        TesseraContext c = 0;
        EXPECT(
            tesseraCreateContextForWindow(&relaxed, id(crossThread), TESSERA_ATTACH_ALLOW_PIXEL_FORMAT, &c),
            TESSERA_SUCCESS);
        if (c) {
            EXPECT(tesseraMakeCurrent(c), TESSERA_SUCCESS);
            EXPECT(tesseraSwapBuffers(c), TESSERA_SUCCESS);
            EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
            EXPECT(tesseraDestroyContext(c), TESSERA_SUCCESS);
        }
    }).join();

    // Argument handling.
    TesseraContext none = 0;
    EXPECT(tesseraCreateContextForWindow(&cfg, 0, 0, &none), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraCreateContextForWindow(&cfg, 0x1234, 0, &none),
           TESSERA_ERROR_INVALID_ARGUMENT); // not a window
    EXPECT(tesseraCreateContextForWindow(&cfg, id(owned), 0x80, &none),
           TESSERA_ERROR_INVALID_ARGUMENT); // unknown flag
    EXPECT(tesseraCreateContextForWindow(nullptr, id(owned), 0, &none), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraCreateContextForWindow(&cfg, id(owned), 0, nullptr), TESSERA_ERROR_INVALID_ARGUMENT);
    TesseraContextConfig wrongSurface = relaxed;
    wrongSurface.surface = TESSERA_SURFACE_PBUFFER;
    EXPECT(tesseraCreateContextForWindow(&wrongSurface, id(owned), 0, &none), TESSERA_ERROR_INVALID_ARGUMENT);
    TesseraContextConfig windowSurface = relaxed;
    windowSurface.surface = TESSERA_SURFACE_WINDOW; // accepted as an alias of DEFAULT here...
    EXPECT(tesseraCreateContextForWindow(&windowSurface, id(owned), 0, &none), TESSERA_SUCCESS);
    if (none)
        tesseraDestroyContext(none);
    // ... but not as a request for an ordinary context.
    TesseraContext ordinary = 0;
    EXPECT(tesseraCreateContext(&windowSurface, &ordinary), TESSERA_ERROR_INVALID_ARGUMENT);

    DestroyWindow(fresh);
    DestroyWindow(owned);
    DestroyWindow(crossThread);
}

int main() {
    if (tesseraInit() != TESSERA_SUCCESS) {
        std::fprintf(stderr, "tesseraInit failed: %s\n", tesseraGetLastErrorMessage());
        return 1;
    }

    // Probe once: no GL driver at all is an environment problem, reported distinctly.
    {
        TesseraContextConfig cfg = baseConfig();
        TesseraContext probe = 0;
        const TesseraResult r = tesseraCreateContext(&cfg, &probe);
        if (r != TESSERA_SUCCESS) {
            std::fprintf(stderr, "test_wgl: cannot create a basic context here: %s (%s)\n",
                         tesseraResultString(r), tesseraGetLastErrorMessage());
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
        {"leak check", testNoLeaks},
        {"deferred destruction", testDeferredDestroy},
        {"reset status / recreate", testResetStatus},
        {"debug message forwarding", testDebugForwarding},
        {"attribute matrix", testAttributeMatrix},
        {"pre-warmed contexts", testPrewarm},
        {"probe", testProbe},
        {"native handles", testNativeHandles},
        {"window attach", testWindowAttach},
        {"GPU selection", testAdapterSelection},
        {"terminate / re-init", testTerminateAndReinit},
    };
    for (const Section& sec : sections) {
        std::puts(sec.name);
        const ULONGLONG t0 = GetTickCount64();
        sec.fn();
        std::printf("  [%llu ms]\n", static_cast<unsigned long long>(GetTickCount64() - t0));
    }

    tesseraTerminate();
    if (g_failures == 0)
        std::puts("test_wgl: OK");
    return g_failures == 0 ? 0 : 1;
}
