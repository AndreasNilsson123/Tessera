// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors

// The software backend and EGL-on-ANGLE (ADR 0015), through the public API only.
//
//   test_software software   TESSERA_BACKEND=software: the CPU renderer (Mesa's software device on Linux,
//   ANGLE's WARP or
//                            SwiftShader on Windows), even when a GPU is present
//   test_software egl        (Windows) TESSERA_BACKEND=egl: ANGLE on the real GPU, plus an HWND window
//   surface
//
// On Windows the test needs an ANGLE build: TESSERA_EGL_LIBRARY must be the absolute path of its libEGL.dll
// (ctest sets it from -DTESSERA_ANGLE_DIR). Exits 77 (skipped) where the EGL backend is not available at all.
#include <tessera/tessera.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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

using ClearColor = void (*)(float, float, float, float);
using ClearFn = void (*)(unsigned);
using ReadFn = void (*)(int, int, int, int, unsigned, unsigned, void*);
using FinishFn = void (*)();
template <class Fn> static Fn proc(const char* name) {
    return reinterpret_cast<Fn>(tesseraGetProcAddress(name));
}

static std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static bool isSoftwareRenderer(const std::string& renderer) {
    const std::string r = lower(renderer);
    for (const char* word :
         {"llvmpipe", "softpipe", "swrast", "warp", "basic render", "swiftshader", "software"})
        if (r.find(word) != std::string::npos)
            return true;
    return false;
}

// Clears to orange and reads one pixel back: proves the context renders, not just that it exists.
static void renderAndRead(const char* what) {
    auto clearColor = proc<ClearColor>("glClearColor");
    auto clear = proc<ClearFn>("glClear");
    auto readPixels = proc<ReadFn>("glReadPixels");
    auto finish = proc<FinishFn>("glFinish");
    CHECK(clearColor && clear && readPixels && finish);
    if (!clearColor || !clear || !readPixels || !finish)
        return;
    clearColor(1.0f, 0.5f, 0.0f, 1.0f);
    clear(0x4000); // GL_COLOR_BUFFER_BIT
    finish();
    unsigned char px[4] = {};
    readPixels(0, 0, 1, 1, 0x1908 /*RGBA*/, 0x1401 /*UNSIGNED_BYTE*/, px);
    std::printf("  %s: pixel %u %u %u %u\n", what, px[0], px[1], px[2], px[3]);
    CHECK(px[0] == 255);
    CHECK(px[1] >= 126 && px[1] <= 129);
    CHECK(px[2] == 0);
}

#if defined(_WIN32)
static HWND makeWindow(int w, int h) {
    return CreateWindowExW(0, L"STATIC", L"tessera-angle", WS_OVERLAPPEDWINDOW, 0, 0, w, h, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}
#endif

static void logToStdout(int32_t level, const char* message, void*) {
    if (level <= TESSERA_LOG_INFO)
        std::printf("  [tessera %d] %s
", level, message);
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a driver that exits the process must not eat the output
    const std::string mode = argc > 1 ? argv[1] : "software";
    if (mode != "software" && mode != "egl") {
        std::fprintf(stderr, "usage: test_software software|egl\n");
        return 2;
    }
    tesseraSetLogCallback(logToStdout, nullptr);
    setEnv("TESSERA_BACKEND", mode.c_str());
    EXPECT(tesseraInit(), TESSERA_SUCCESS);

    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL_ES; // ANGLE offers nothing else; Mesa's software device does both
    cfg.versionMajor = 2;
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;

    TesseraProbeReport probe = tesseraDefaultProbeReport();
    const TesseraResult pr = tesseraProbe(&cfg, &probe);
    if (pr == TESSERA_ERROR_UNSUPPORTED) {
        std::printf("skipped: %s (%s)\n", tesseraResultString(pr), tesseraGetLastErrorMessage());
        tesseraTerminate();
        return 77;
    }
    EXPECT(pr, TESSERA_SUCCESS);
    std::printf("backend %s, renderer %s, version %s\n", probe.backend, probe.info.renderer,
                probe.info.version);
    CHECK(mode == probe.backend);

    TesseraContext ctx = TESSERA_NULL_HANDLE;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    TesseraContextInfo info = tesseraDefaultContextInfo();
    EXPECT(tesseraGetContextInfo(ctx, &info), TESSERA_SUCCESS);
    CHECK(info.api == TESSERA_API_OPENGL_ES);
    if (mode == "software")
        CHECK(
            isSoftwareRenderer(info.renderer)); // the point of the backend: not the GPU, even if there is one
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    renderAndRead("pbuffer");

    // A desktop GL request: Mesa's software device has it; ANGLE does not and must say so, not crash or lie.
    TesseraContextConfig gl = tesseraDefaultContextConfig();
    gl.api = TESSERA_API_OPENGL;
    gl.versionMajor = 3;
    gl.versionMinor = 3;
    gl.profile = TESSERA_PROFILE_CORE;
    TesseraContext glCtx = TESSERA_NULL_HANDLE;
    const TesseraResult glr = tesseraCreateContext(&gl, &glCtx);
#if defined(_WIN32)
    CHECK(glr == TESSERA_ERROR_UNSUPPORTED); // ANGLE is OpenGL ES only
    std::printf("  desktop GL on ANGLE: %s (%s)\n", tesseraResultString(glr), tesseraGetLastErrorMessage());
#else
    std::printf("  desktop GL 3.3 core on the software device: %s\n", tesseraResultString(glr));
    if (glr == TESSERA_SUCCESS)
        EXPECT(tesseraDestroyContext(glCtx), TESSERA_SUCCESS);
#endif

#if defined(_WIN32)
    // An HWND through EGL: the window surface of ANGLE.
    HWND hwnd = makeWindow(200, 120);
    CHECK(hwnd != nullptr);
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        TesseraContext wctx = TESSERA_NULL_HANDLE;
        EXPECT(tesseraCreateContextForWindow(&cfg, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd)),
                                             0, &wctx),
               TESSERA_SUCCESS);
        if (wctx) {
            TesseraContextInfo wi = tesseraDefaultContextInfo();
            EXPECT(tesseraGetContextInfo(wctx, &wi), TESSERA_SUCCESS);
            CHECK(wi.surface == TESSERA_SURFACE_WINDOW);
            EXPECT(tesseraMakeCurrent(wctx), TESSERA_SUCCESS);
            renderAndRead("window");
            EXPECT(tesseraSwapBuffers(wctx), TESSERA_SUCCESS);
            TesseraSwapControl sc = tesseraDefaultSwapControl();
            EXPECT(tesseraSetSwapInterval(wctx, 0), TESSERA_SUCCESS);
            EXPECT(tesseraGetSwapControl(wctx, &sc), TESSERA_SUCCESS);
            CHECK((sc.caps & TESSERA_SWAP_CAP_CONTROL) != 0);
            EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
            EXPECT(tesseraDestroyContext(wctx), TESSERA_SUCCESS);
        }
        DestroyWindow(hwnd);
    }
    // The system-wide default: no TESSERA_EGL_LIBRARY means System32 only, so a relative name must be
    // refused, not searched.
#endif

    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
