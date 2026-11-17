// Integration tests for the WGL backend, through the public C API only. Needs a working OpenGL driver
// (a software renderer is fine). A raw WGL context created here plays the role of "another library in the
// same process" (JavaFX, AWT, LWJGL...) whose current context Tessera must never disturb.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>

#include <tessera/tessera.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                                                      \
    do {                                                                                                 \
        if (!(cond)) {                                                                                   \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                \
            ++g_failures;                                                                                \
        }                                                                                                \
    } while (0)
#define EXPECT(call, want)                                                                               \
    do {                                                                                                 \
        const TesseraResult got_ = (call);                                                               \
        if (got_ != (want)) {                                                                            \
            std::fprintf(stderr, "%s:%d: %s -> %s, expected %s (%s)\n", __FILE__, __LINE__, #call,       \
                         tesseraResultString(got_), tesseraResultString(want), tesseraGetLastErrorMessage()); \
            ++g_failures;                                                                                \
        }                                                                                                \
    } while (0)

using GlGetString = const unsigned char*(WINAPI*)(unsigned);
using GlGenTextures = void(WINAPI*)(int, unsigned*);
using GlIsTexture = unsigned char(WINAPI*)(unsigned);
using GlBindTexture = void(WINAPI*)(unsigned, unsigned);

static TesseraContextConfig baseConfig() {
    TesseraContextConfig c = TESSERA_CONTEXT_CONFIG_INIT;
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
        hwnd = CreateWindowExW(0, L"STATIC", L"foreign", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
        if (!hwnd) return false;
        dc = GetDC(hwnd);
        PIXELFORMATDESCRIPTOR pfd = {};
        pfd.nSize = sizeof pfd;
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        pfd.cDepthBits = 24;
        const int pf = ChoosePixelFormat(dc, &pfd);
        if (!pf || !SetPixelFormat(dc, pf, &pfd)) return false;
        rc = wglCreateContext(dc);
        return rc != nullptr;
    }
    void makeCurrent() const { wglMakeCurrent(dc, rc); }
    bool isCurrent() const { return wglGetCurrentContext() == rc && wglGetCurrentDC() == dc; }
    void destroy() {
        if (wglGetCurrentContext() == rc) wglMakeCurrent(nullptr, nullptr);
        if (rc) wglDeleteContext(rc);
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
    if (!ctx) return;

    TesseraContextInfo info = TESSERA_CONTEXT_INFO_INIT;
    EXPECT(tesseraGetContextInfo(ctx, &info), TESSERA_SUCCESS);
    std::printf("  default context: %s | %s | GL %s\n    api=%u %d.%d profile=%u flags=0x%x rgba=%d%d%d%d d=%d s=%d samples=%d\n",
                info.vendor, info.renderer, info.version, info.api, info.versionMajor, info.versionMinor, info.profile,
                info.flags, info.redBits, info.greenBits, info.blueBits, info.alphaBits, info.depthBits,
                info.stencilBits, info.samples);
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

    EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS);
    CHECK(foreign.isCurrent());

    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    if (coreCtx) EXPECT(tesseraDestroyContext(coreCtx), TESSERA_SUCCESS);
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
    if (!ctx) return;
    TesseraContextInfo info = TESSERA_CONTEXT_INFO_INIT;
    tesseraGetContextInfo(ctx, &info);

    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    CHECK(tesseraGetCurrentContext() == ctx);
    CHECK(wglGetCurrentContext() != nullptr);

    auto glGetString = reinterpret_cast<GlGetString>(tesseraGetProcAddress("glGetString"));
    CHECK(glGetString != nullptr);
    if (glGetString) CHECK(std::strcmp(reinterpret_cast<const char*>(glGetString(0x1F01)), info.renderer) == 0);
    CHECK(tesseraGetProcAddress("glThisFunctionDoesNotExist") == nullptr);
    CHECK(tesseraGetProcAddress(nullptr) == nullptr);
    if (info.versionMajor >= 2) CHECK(tesseraGetProcAddress("glGenBuffers") != nullptr);

    EXPECT(tesseraSetSwapInterval(ctx, 0), TESSERA_SUCCESS);
    EXPECT(tesseraSetSwapInterval(ctx, 1), TESSERA_SUCCESS);
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
    if (!ctx) return;

    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    std::thread([&] {
        EXPECT(tesseraMakeCurrent(ctx), TESSERA_ERROR_WRONG_THREAD);
        EXPECT(tesseraDestroyContext(ctx), TESSERA_ERROR_WRONG_THREAD);
        EXPECT(tesseraSetSwapInterval(ctx, 1), TESSERA_ERROR_WRONG_THREAD);
        EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS); // presenting does not need ownership
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
        TesseraContextInfo info = TESSERA_CONTEXT_INFO_INIT;
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
        TesseraContextInfo info = TESSERA_CONTEXT_INFO_INIT;
        tesseraGetContextInfo(ctx, &info);
        std::printf("  core request -> GL %d.%d profile=%u flags=0x%x\n", info.versionMajor, info.versionMinor,
                    info.profile, info.flags);
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
        TesseraContextInfo info = TESSERA_CONTEXT_INFO_INIT;
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
    if (er == TESSERA_SUCCESS) tesseraDestroyContext(ctx);

    // Surface types.
    TesseraContextConfig pb = baseConfig();
    pb.surface = TESSERA_SURFACE_PBUFFER;
    const TesseraResult pr = tesseraCreateContext(&pb, &ctx);
    CHECK(pr == TESSERA_SUCCESS || pr == TESSERA_ERROR_UNSUPPORTED || pr == TESSERA_ERROR_NO_MATCHING_CONFIG);
    std::printf("  pbuffer request: %s\n", tesseraResultString(pr));
    if (pr == TESSERA_SUCCESS) {
        TesseraContextInfo info = TESSERA_CONTEXT_INFO_INIT;
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
        TesseraContextInfo info = TESSERA_CONTEXT_INFO_INIT;
        tesseraGetContextInfo(ctx, &info);
        CHECK(info.surface == TESSERA_SURFACE_NONE);
        tesseraDestroyContext(ctx);
    }
}

static void testSharing() {
    TesseraContextConfig cfg = baseConfig();
    TesseraContext a = 0, b = 0;
    EXPECT(tesseraCreateContext(&cfg, &a), TESSERA_SUCCESS);
    if (!a) return;
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
    if (isTexture) CHECK(isTexture(tex));
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
                if (tesseraCreateContext(&cfg, &ctx) != TESSERA_SUCCESS) { ++failures; continue; }
                if (tesseraMakeCurrent(ctx) != TESSERA_SUCCESS) ++failures;
                if (tesseraSwapBuffers(ctx) != TESSERA_SUCCESS) ++failures;
                if (tesseraDestroyContext(ctx) != TESSERA_SUCCESS) ++failures;
            }
        });
    for (auto& t : threads) t.join();
    CHECK(failures == 0);
}

static void testNoLeaks() {
    TesseraContextConfig cfg = baseConfig();
    auto cycle = [&](int n) {
        for (int i = 0; i < n; ++i) {
            TesseraContext ctx = 0;
            if (tesseraCreateContext(&cfg, &ctx) != TESSERA_SUCCESS) { ++g_failures; return; }
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
    if (again) EXPECT(tesseraDestroyContext(again), TESSERA_SUCCESS);
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
            std::fprintf(stderr, "test_wgl: cannot create a basic context here: %s (%s)\n", tesseraResultString(r),
                         tesseraGetLastErrorMessage());
            return 1;
        }
        tesseraDestroyContext(probe);
    }

    struct Section { const char* name; void (*fn)(); };
    const Section sections[] = {
        {"basic create/info", testBasicCreateAndInfo},
        {"foreign context untouched", testForeignContextUntouched},
        {"make current / procs / swap", testMakeCurrentAndProcs},
        {"thread rules", testThreadRules},
        {"config negotiation", testConfigNegotiation},
        {"sharing", testSharing},
        {"concurrent creation", testConcurrentCreation},
        {"leak check", testNoLeaks},
        {"terminate / re-init", testTerminateAndReinit},
    };
    for (const Section& sec : sections) {
        std::puts(sec.name);
        const ULONGLONG t0 = GetTickCount64();
        sec.fn();
        std::printf("  [%llu ms]\n", static_cast<unsigned long long>(GetTickCount64() - t0));
    }

    tesseraTerminate();
    if (g_failures == 0) std::puts("test_wgl: OK");
    return g_failures == 0 ? 0 : 1;
}
