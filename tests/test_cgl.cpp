// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
// macOS integration test: the public C API against the REAL OpenGL.framework (CGL). Registered only on Apple
// platforms.
//
// STATUS: written without access to a Mac, never run. It exists so that the first run on real hardware
// immediately shows whether the hand-declared CGL constants in src/platform/cgl/cgl_types.hpp are right: the
// raw "other library" context below is created with the very same attribute values, so a wrong constant fails
// here instead of silently mis-selecting a pixel format. Apple's GL implementation is also the only thing
// that can show whether the colour-size heuristic and the version/profile behaviour the backend assumes hold
// up.
//
// Hosted macOS CI runners are VMs without a GPU: expect software OpenGL there and possibly no core profile.
#include <tessera/tessera.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstring>
#include <thread>

#include "platform/cgl/cgl_types.hpp"

using namespace tessera::cgl;

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

using GlGetString = const unsigned char* (*)(unsigned);
using GlGenTextures = void (*)(int, unsigned*);
using GlBindTexture = void (*)(unsigned, unsigned);
using GlIsTexture = unsigned char (*)(unsigned);

static CglFns g_cgl;

template <class Fn> static bool bind(void* lib, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(dlsym(lib, name));
    return out != nullptr;
}

static bool loadRaw() {
    void* lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return false;
    return bind(lib, "CGLChoosePixelFormat", g_cgl.CGLChoosePixelFormat) &&
           bind(lib, "CGLReleasePixelFormat", g_cgl.CGLReleasePixelFormat) &&
           bind(lib, "CGLCreateContext", g_cgl.CGLCreateContext) &&
           bind(lib, "CGLDestroyContext", g_cgl.CGLDestroyContext) &&
           bind(lib, "CGLSetCurrentContext", g_cgl.CGLSetCurrentContext) &&
           bind(lib, "CGLGetCurrentContext", g_cgl.CGLGetCurrentContext);
}

static TesseraContextConfig baseConfig() {
    TesseraContextConfig c = tesseraDefaultContextConfig();
    c.api = TESSERA_API_OPENGL;
    return c;
}

static void testBasicLegacy() {
    TesseraContextConfig cfg = baseConfig();
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    if (!ctx)
        return;
    TesseraContextInfo info = tesseraDefaultContextInfo();
    EXPECT(tesseraGetContextInfo(ctx, &info), TESSERA_SUCCESS);
    std::printf(
        "  legacy: %s | %s | GL %s\n    %d.%d profile=%u flags=0x%x rgba=%d%d%d%d d=%d s=%d samples=%d\n",
        info.vendor, info.renderer, info.version, info.versionMajor, info.versionMinor, info.profile,
        info.flags, info.redBits, info.greenBits, info.blueBits, info.alphaBits, info.depthBits,
        info.stencilBits, info.samples);
    CHECK(info.api == TESSERA_API_OPENGL);
    CHECK(info.versionMajor == 2 && info.versionMinor == 1); // what the legacy profile is documented to give
    CHECK(info.profile == TESSERA_PROFILE_COMPATIBILITY);
    CHECK(info.surface == TESSERA_SURFACE_NONE);
    CHECK(info.redBits >= 8 && info.greenBits >= 8 && info.blueBits >= 8); // checks the colour-size heuristic
    CHECK(g_cgl.CGLGetCurrentContext() == nullptr);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
}

static void testCore() {
    TesseraContextConfig cfg = baseConfig();
    cfg.profile = TESSERA_PROFILE_CORE;
    cfg.fallback = TESSERA_FALLBACK_STRICT;
    TesseraContext ctx = 0;
    const TesseraResult r = tesseraCreateContext(&cfg, &ctx);
    CHECK(r == TESSERA_SUCCESS ||
          r == TESSERA_ERROR_NO_MATCHING_CONFIG); // a VM without a core-capable renderer may refuse
    std::printf("  core: %s\n", tesseraResultString(r));
    if (r != TESSERA_SUCCESS)
        return;
    TesseraContextInfo info = tesseraDefaultContextInfo();
    tesseraGetContextInfo(ctx, &info);
    std::printf("    %s\n", info.version);
    CHECK(info.profile == TESSERA_PROFILE_CORE);
    CHECK(info.versionMajor > 3 || (info.versionMajor == 3 && info.versionMinor >= 2));
    CHECK(info.flags & TESSERA_FLAG_FORWARD_COMPATIBLE);
    tesseraDestroyContext(ctx);
}

static void testForeignUntouchedAndCurrent() {
    // The "other library": a raw legacy CGL context, built with the backend's own constants.
    const PixelFormatAttribute attribs[] = {kPFAOpenGLProfile, kProfileLegacy, 0};
    CGLPixelFormatObj pf = nullptr;
    GLint n = 0;
    CHECK(g_cgl.CGLChoosePixelFormat(attribs, &pf, &n) == kNoError && pf &&
          n > 0); // fails if a constant is wrong
    CGLContextObj foreign = nullptr;
    CHECK(g_cgl.CGLCreateContext(pf, nullptr, &foreign) == kNoError && foreign);
    if (pf)
        g_cgl.CGLReleasePixelFormat(pf);
    g_cgl.CGLSetCurrentContext(foreign);

    TesseraContextConfig cfg = baseConfig();
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    CHECK(g_cgl.CGLGetCurrentContext() == foreign);

    TesseraContextConfig bad = baseConfig();
    bad.samples = 64;
    bad.fallback = TESSERA_FALLBACK_STRICT;
    TesseraContext none = 0;
    EXPECT(tesseraCreateContext(&bad, &none), TESSERA_ERROR_NO_MATCHING_CONFIG);
    CHECK(g_cgl.CGLGetCurrentContext() == foreign);

    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    CHECK(tesseraGetCurrentContext() == ctx);
    auto glGetString = reinterpret_cast<GlGetString>(tesseraGetProcAddress("glGetString"));
    CHECK(glGetString != nullptr);
    if (glGetString)
        CHECK(glGetString(0x1F02) != nullptr);
    CHECK(tesseraGetProcAddress("CGLCreateContext") == nullptr);
    g_cgl.CGLSetCurrentContext(foreign);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    CHECK(g_cgl.CGLGetCurrentContext() == foreign); // releasing ours must not drop theirs

    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    CHECK(g_cgl.CGLGetCurrentContext() == foreign);
    g_cgl.CGLSetCurrentContext(nullptr);
    if (foreign)
        g_cgl.CGLDestroyContext(foreign);
}

static void testSharingAndThreads() {
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
    unsigned tex = 0;
    CHECK(genTextures && bindTexture && isTexture);
    if (genTextures && bindTexture && isTexture) {
        genTextures(1, &tex);
        bindTexture(0x0DE1, tex);
        CHECK(isTexture(tex));
        EXPECT(tesseraMakeCurrent(b), TESSERA_SUCCESS);
        CHECK(isTexture(tex)); // visible through the share group
    }
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);

    std::thread([&] {
        EXPECT(tesseraMakeCurrent(a), TESSERA_SUCCESS);
        EXPECT(tesseraMakeCurrent(b), TESSERA_SUCCESS);
    }).join(); // exits with b current: Tessera releases it on thread exit
    EXPECT(tesseraMakeCurrent(b), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    std::thread([&] { EXPECT(tesseraDestroyContext(a), TESSERA_SUCCESS); }).join();
    EXPECT(tesseraDestroyContext(b), TESSERA_SUCCESS);
}

int main() {
    if (!loadRaw()) {
        std::fprintf(stderr, "test_cgl: OpenGL.framework not available, skipping\n");
        return 77;
    }
    if (tesseraInit() != TESSERA_SUCCESS) {
        std::fprintf(stderr, "tesseraInit failed: %s\n", tesseraGetLastErrorMessage());
        return 1;
    }
    {
        TesseraContextConfig cfg = baseConfig();
        TesseraContext probe = 0;
        const TesseraResult r = tesseraCreateContext(&cfg, &probe);
        if (r != TESSERA_SUCCESS) {
            std::fprintf(stderr, "test_cgl: cannot create a basic context: %s (%s)\n", tesseraResultString(r),
                         tesseraGetLastErrorMessage());
            return 1;
        }
        tesseraDestroyContext(probe);
    }

    struct Section {
        const char* name;
        void (*fn)();
    };
    const Section sections[] = {
        {"legacy context", testBasicLegacy},
        {"core context", testCore},
        {"foreign context untouched / current / procs", testForeignUntouchedAndCurrent},
        {"sharing and threads", testSharingAndThreads},
    };
    for (const Section& sec : sections) {
        std::puts(sec.name);
        sec.fn();
    }
    tesseraTerminate();
    if (g_failures == 0)
        std::puts("test_cgl: OK");
    return g_failures == 0 ? 0 : 1;
}
