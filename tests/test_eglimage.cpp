// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors

// Zero-copy handoff between two Tessera contexts through an EGLImage, using only what Tessera exposes: the
// native handles (EGLDisplay, EGLContext) and tesseraGetProcAddress (which resolves the EGL and GL extension
// entry points). The Linux counterpart of test_dxinterop: Tessera has no interop API of its own on purpose,
// the extension is the API.
//
// Context A renders a colour into a texture; the texture becomes an EGLImage (EGL_KHR_gl_texture_2D_image);
// context B, which does NOT share objects with A, binds that image as a texture (GL_OES_EGL_image) and reads
// the colour back. Exits 77 (skipped) where the EGL backend or the extensions are missing. (dma-buf
// export/import, the usual next step, needs EGL_MESA_image_dma_buf_export or EGL_EXT_image_dma_buf_import,
// which the Mesa drivers available for testing here do not offer.)
#include <tessera/tessera.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

using EglCreateImage = void* (*)(void*, void*, unsigned, void*, const int*);
using EglDestroyImage = unsigned (*)(void*, void*);
using ImageTarget = void (*)(unsigned, void*);
using GenFn = void (*)(int, unsigned*);
using BindFn = void (*)(unsigned, unsigned);
using TexImage = void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
using TexParam = void (*)(unsigned, unsigned, int);
using FboTex = void (*)(unsigned, unsigned, unsigned, unsigned, int);
using ClearColor = void (*)(float, float, float, float);
using ClearFn = void (*)(unsigned);
using ReadFn = void (*)(int, int, int, int, unsigned, unsigned, void*);
using FinishFn = void (*)();

template <class Fn> static Fn proc(const char* name) {
    return reinterpret_cast<Fn>(tesseraGetProcAddress(name));
}

constexpr unsigned kTexture2D = 0x0DE1, kRgba = 0x1908, kUnsignedByte = 0x1401, kFramebuffer = 0x8D40,
                   kColorAttachment0 = 0x8CE0;
constexpr unsigned kTextureMinFilter = 0x2801, kTextureMagFilter = 0x2800, kNearest = 0x2600;
constexpr unsigned kEglGlTexture2dKhr = 0x30B1;
constexpr int kEglGlTextureLevelKhr = 0x30BC, kEglNone = 0x3038;

int main() {
    setenv("TESSERA_BACKEND", "egl", 1);
    if (tesseraInit() != TESSERA_SUCCESS)
        return 1;
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraContext a = 0, b = 0;
    if (tesseraCreateContext(&cfg, &a) != TESSERA_SUCCESS) {
        std::fprintf(stderr, "test_eglimage: no EGL context here (%s), skipping\n",
                     tesseraGetLastErrorMessage());
        return 77;
    }
    EXPECT(tesseraCreateContext(&cfg, &b), TESSERA_SUCCESS); // not shared with `a`
    TesseraNativeHandles ha = tesseraDefaultNativeHandles(), hb = tesseraDefaultNativeHandles();
    EXPECT(tesseraGetNativeHandles(a, &ha), TESSERA_SUCCESS);
    EXPECT(tesseraGetNativeHandles(b, &hb), TESSERA_SUCCESS);
    CHECK(ha.kind == TESSERA_NATIVE_EGL && ha.handles[0] == hb.handles[0] && ha.handles[1] != hb.handles[1]);
    void* dpy = reinterpret_cast<void*>(static_cast<uintptr_t>(ha.handles[0]));
    void* ctxA = reinterpret_cast<void*>(static_cast<uintptr_t>(ha.handles[1]));

    EXPECT(tesseraMakeCurrent(a), TESSERA_SUCCESS);
    auto createImage = proc<EglCreateImage>("eglCreateImageKHR");
    auto destroyImage = proc<EglDestroyImage>("eglDestroyImageKHR");
    auto imageTarget = proc<ImageTarget>("glEGLImageTargetTexture2DOES");
    auto genTextures = proc<GenFn>("glGenTextures");
    auto genFramebuffers = proc<GenFn>("glGenFramebuffers");
    auto bindTexture = proc<BindFn>("glBindTexture");
    auto bindFramebuffer = proc<BindFn>("glBindFramebuffer");
    auto texImage = proc<TexImage>("glTexImage2D");
    auto texParam = proc<TexParam>("glTexParameteri");
    auto fboTexture = proc<FboTex>("glFramebufferTexture2D");
    auto clearColor = proc<ClearColor>("glClearColor");
    auto clear = proc<ClearFn>("glClear");
    auto readPixels = proc<ReadFn>("glReadPixels");
    auto finish = proc<FinishFn>("glFinish");
    if (!createImage || !destroyImage || !imageTarget || !genTextures || !genFramebuffers || !bindTexture ||
        !bindFramebuffer || !texImage || !texParam || !fboTexture || !clearColor || !clear || !readPixels ||
        !finish) {
        std::fprintf(stderr, "test_eglimage: EGL_KHR_gl_texture_2D_image / GL_OES_EGL_image entry points are "
                             "not available, skipping\n");
        return 77;
    }

    // A: render a colour into a texture.
    unsigned texA = 0, fboA = 0;
    genTextures(1, &texA);
    bindTexture(kTexture2D, texA);
    texImage(kTexture2D, 0, static_cast<int>(kRgba), 64, 64, 0, kRgba, kUnsignedByte, nullptr);
    texParam(kTexture2D, kTextureMinFilter, static_cast<int>(kNearest)); // complete without mipmaps
    texParam(kTexture2D, kTextureMagFilter, static_cast<int>(kNearest));
    genFramebuffers(1, &fboA);
    bindFramebuffer(kFramebuffer, fboA);
    fboTexture(kFramebuffer, kColorAttachment0, kTexture2D, texA, 0);
    clearColor(0.2f, 0.4f, 0.6f, 1.f);
    clear(0x4000);
    finish();
    const int attribs[] = {kEglGlTextureLevelKhr, 0, kEglNone};
    void* image = createImage(dpy, ctxA, kEglGlTexture2dKhr,
                              reinterpret_cast<void*>(static_cast<uintptr_t>(texA)), attribs);
    if (!image) {
        std::fprintf(stderr, "test_eglimage: eglCreateImageKHR refused a GL texture here, skipping\n");
        return 77;
    }
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);

    // B: a different context, no shared objects, binds the same memory as a texture.
    EXPECT(tesseraMakeCurrent(b), TESSERA_SUCCESS);
    unsigned texB = 0, fboB = 0;
    genTextures(1, &texB);
    bindTexture(kTexture2D, texB);
    imageTarget(kTexture2D, image);
    genFramebuffers(1, &fboB);
    bindFramebuffer(kFramebuffer, fboB);
    fboTexture(kFramebuffer, kColorAttachment0, kTexture2D, texB, 0);
    unsigned char px[4] = {};
    readPixels(10, 10, 1, 1, kRgba, kUnsignedByte, px);
    std::printf(
        "  context B reads (%d,%d,%d,%d) from the image context A rendered (expected about 51,102,153,255)\n",
        px[0], px[1], px[2], px[3]);
    CHECK(std::abs(px[0] - 51) <= 1 && std::abs(px[1] - 102) <= 1 && std::abs(px[2] - 153) <= 1 &&
          px[3] == 255);

    // Changes made through A after the image exists are visible to B (the memory is the same).
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(a), TESSERA_SUCCESS);
    bindFramebuffer(kFramebuffer, fboA);
    clearColor(1.f, 0.f, 0.f, 1.f);
    clear(0x4000);
    finish();
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(b), TESSERA_SUCCESS);
    bindFramebuffer(kFramebuffer, fboB);
    readPixels(10, 10, 1, 1, kRgba, kUnsignedByte, px);
    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);

    CHECK(destroyImage(dpy, image) != 0);
    EXPECT(tesseraDestroyContext(a), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(b), TESSERA_SUCCESS);
    tesseraTerminate();
    if (g_failures == 0)
        std::puts("test_eglimage: OK");
    return g_failures == 0 ? 0 : 1;
}
