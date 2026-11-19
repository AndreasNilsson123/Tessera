// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
// Zero-copy handoff between OpenGL and Direct3D 11 through a Tessera context, using only what Tessera
// exposes: the native handles and tesseraGetProcAddress (which resolves the WGL_NV_DX_interop2 entry points).
// Tessera has no interop API of its own on purpose: the extension is the API, and the missing piece was
// access to the native objects.
//
// OpenGL renders into a texture that is really a D3D11 texture; D3D11 then reads the same memory back and
// checks the colour. Needs an NVIDIA-style WGL_NV_DX_interop2 driver and a D3D11 adapter on the same GPU.
// Exits 77 (skipped) otherwise.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

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

template <class T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() {
        if (p)
            p->Release();
    }
    T** operator&() {
        return &p;
    }
    T* operator->() const {
        return p;
    }
};

// WGL_NV_DX_interop2
using PfnOpenDevice = HANDLE(WINAPI*)(void*);
using PfnCloseDevice = BOOL(WINAPI*)(HANDLE);
using PfnRegister = HANDLE(WINAPI*)(HANDLE, void*, unsigned, unsigned, unsigned);
using PfnUnregister = BOOL(WINAPI*)(HANDLE, HANDLE);
using PfnLock = BOOL(WINAPI*)(HANDLE, int, HANDLE*);
// OpenGL
using PfnGenTextures = void(WINAPI*)(int, unsigned*);
using PfnDeleteTextures = void(WINAPI*)(int, const unsigned*);
using PfnGenFramebuffers = void(WINAPI*)(int, unsigned*);
using PfnDeleteFramebuffers = void(WINAPI*)(int, const unsigned*);
using PfnBindFramebuffer = void(WINAPI*)(unsigned, unsigned);
using PfnFramebufferTexture2D = void(WINAPI*)(unsigned, unsigned, unsigned, unsigned, int);
using PfnCheckFramebufferStatus = unsigned(WINAPI*)(unsigned);
using PfnClearColor = void(WINAPI*)(float, float, float, float);
using PfnClear = void(WINAPI*)(unsigned);
using PfnFinish = void(WINAPI*)();

template <class Fn> static Fn proc(const char* name) {
    return reinterpret_cast<Fn>(tesseraGetProcAddress(name));
}

// Picks the DXGI adapter that is the same GPU as the OpenGL context (a laptop usually has two).
static IDXGIAdapter1* matchingAdapter(const char* glVendor) {
    unsigned wanted = 0;
    if (std::strstr(glVendor, "NVIDIA"))
        wanted = 0x10DE;
    else if (std::strstr(glVendor, "ATI") || std::strstr(glVendor, "AMD"))
        wanted = 0x1002;
    else if (std::strstr(glVendor, "Intel"))
        wanted = 0x8086;
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return nullptr;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
            return nullptr;
        DXGI_ADAPTER_DESC1 d = {};
        a->GetDesc1(&d);
        if (wanted != 0 && d.VendorId == wanted && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            return a;
        a->Release();
    }
}

int main() {
    if (tesseraInit() != TESSERA_SUCCESS)
        return 1;
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    cfg.flags = TESSERA_FLAG_DOUBLE_BUFFER;
    TesseraContext ctx = 0;
    if (tesseraCreateContext(&cfg, &ctx) != TESSERA_SUCCESS) {
        std::fprintf(stderr, "test_dxinterop: no OpenGL context here: %s\n", tesseraGetLastErrorMessage());
        return 77;
    }
    TesseraContextInfo info = tesseraDefaultContextInfo();
    tesseraGetContextInfo(ctx, &info);
    std::printf("OpenGL: %s | %s\n", info.vendor, info.renderer);

    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    TesseraNativeHandles nh = tesseraDefaultNativeHandles();
    EXPECT(tesseraGetNativeHandles(ctx, &nh), TESSERA_SUCCESS);
    // The handle Tessera hands out is the very context the driver considers current.
    CHECK(reinterpret_cast<HGLRC>(static_cast<std::uintptr_t>(nh.handles[0])) == wglGetCurrentContext());

    auto openDevice = proc<PfnOpenDevice>("wglDXOpenDeviceNV");
    auto closeDevice = proc<PfnCloseDevice>("wglDXCloseDeviceNV");
    auto registerObject = proc<PfnRegister>("wglDXRegisterObjectNV");
    auto unregisterObject = proc<PfnUnregister>("wglDXUnregisterObjectNV");
    auto lockObjects = proc<PfnLock>("wglDXLockObjectsNV");
    auto unlockObjects = proc<PfnLock>("wglDXUnlockObjectsNV");
    if (!openDevice || !closeDevice || !registerObject || !unregisterObject || !lockObjects ||
        !unlockObjects) {
        std::fprintf(stderr,
                     "test_dxinterop: WGL_NV_DX_interop2 is not available on this driver, skipping\n");
        return 77;
    }
    auto genTextures = proc<PfnGenTextures>("glGenTextures");
    auto deleteTextures = proc<PfnDeleteTextures>("glDeleteTextures");
    auto genFramebuffers = proc<PfnGenFramebuffers>("glGenFramebuffers");
    auto deleteFramebuffers = proc<PfnDeleteFramebuffers>("glDeleteFramebuffers");
    auto bindFramebuffer = proc<PfnBindFramebuffer>("glBindFramebuffer");
    auto framebufferTexture2D = proc<PfnFramebufferTexture2D>("glFramebufferTexture2D");
    auto checkFramebufferStatus = proc<PfnCheckFramebufferStatus>("glCheckFramebufferStatus");
    auto clearColor = proc<PfnClearColor>("glClearColor");
    auto clear = proc<PfnClear>("glClear");
    auto finish = proc<PfnFinish>("glFinish");
    CHECK(genTextures && deleteTextures && genFramebuffers && deleteFramebuffers && bindFramebuffer &&
          framebufferTexture2D && checkFramebufferStatus && clearColor && clear && finish);

    // A D3D11 device on the same GPU.
    Com<IDXGIAdapter1> adapter;
    adapter.p = matchingAdapter(info.vendor);
    if (!adapter.p) {
        std::fprintf(stderr, "test_dxinterop: no D3D11 adapter matching the OpenGL GPU, skipping\n");
        return 77;
    }
    Com<ID3D11Device> device;
    Com<ID3D11DeviceContext> d3d;
    if (FAILED(D3D11CreateDevice(adapter.p, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &d3d))) {
        std::fprintf(stderr, "test_dxinterop: cannot create a D3D11 device, skipping\n");
        return 77;
    }
    constexpr UINT kSize = 64;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = kSize;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Com<ID3D11Texture2D> shared;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &shared))) {
        std::fprintf(stderr, "test_dxinterop: cannot create the D3D11 texture\n");
        return 1;
    }
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)))
        return 1;

    // Hand the D3D11 texture to OpenGL.
    HANDLE glDevice = openDevice(device.p);
    if (!glDevice) {
        std::fprintf(stderr,
                     "test_dxinterop: wglDXOpenDeviceNV failed (error 0x%lx): this GPU/driver pairing does "
                     "not support it, skipping\n",
                     static_cast<unsigned long>(GetLastError()));
        return 77;
    }
    unsigned glTexture = 0;
    genTextures(1, &glTexture);
    HANDLE object = registerObject(glDevice, shared.p, glTexture, 0x0DE1 /*GL_TEXTURE_2D*/,
                                   0x0001 /*WGL_ACCESS_READ_WRITE_NV*/);
    CHECK(object != nullptr);
    unsigned fbo = 0;
    genFramebuffers(1, &fbo);

    auto roundTrip = [&](float r, float g, float b, const char* label) {
        // OpenGL draws into the shared texture while it holds the lock...
        CHECK(lockObjects(glDevice, 1, &object));
        bindFramebuffer(0x8D40 /*GL_FRAMEBUFFER*/, fbo);
        framebufferTexture2D(0x8D40, 0x8CE0 /*GL_COLOR_ATTACHMENT0*/, 0x0DE1, glTexture, 0);
        CHECK(checkFramebufferStatus(0x8D40) == 0x8CD5 /*GL_FRAMEBUFFER_COMPLETE*/);
        clearColor(r, g, b, 1.f);
        clear(0x4000);
        bindFramebuffer(0x8D40, 0);
        finish();
        CHECK(unlockObjects(glDevice, 1, &object));
        // ... and Direct3D reads the very same memory.
        d3d->CopyResource(staging.p, shared.p);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(d3d->Map(staging.p, 0, D3D11_MAP_READ, 0, &mapped))) {
            CHECK(false);
            return;
        }
        const unsigned char* row =
            static_cast<const unsigned char*>(mapped.pData) + 10 * mapped.RowPitch + 10 * 4;
        const int er = static_cast<int>(r * 255 + 0.5f), eg = static_cast<int>(g * 255 + 0.5f),
                  eb = static_cast<int>(b * 255 + 0.5f);
        std::printf("  %s: D3D11 sees (%d,%d,%d,%d), OpenGL drew (%d,%d,%d,255)\n", label, row[0], row[1],
                    row[2], row[3], er, eg, eb);
        CHECK(std::abs(row[0] - er) <= 1 && std::abs(row[1] - eg) <= 1 && std::abs(row[2] - eb) <= 1 &&
              row[3] == 255);
        d3d->Unmap(staging.p, 0);
    };
    roundTrip(0.2f, 0.4f, 0.6f, "first handoff ");
    roundTrip(0.9f, 0.1f, 0.3f, "second handoff"); // the texture can be handed back and forth, not just once

    // Tear down in the order the extension requires, then confirm Tessera itself is unharmed by all of it.
    deleteFramebuffers(1, &fbo);
    CHECK(unregisterObject(glDevice, object));
    deleteTextures(1, &glTexture);
    CHECK(closeDevice(glDevice));
    EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    tesseraTerminate();

    if (g_failures == 0)
        std::puts("test_dxinterop: OK");
    return g_failures == 0 ? 0 : 1;
}
