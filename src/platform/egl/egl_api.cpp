// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "egl_api.hpp"

#include <cstdlib>

#include <tessera/tessera.h>

#include "core/env.hpp"
#include "core/log.hpp"

namespace tessera::egl {
namespace {

template <class Fn> bool load(const DynamicLibrary& lib, const char* name, Fn& out) noexcept {
    out = reinterpret_cast<Fn>(lib.symbol(name));
    return out != nullptr;
}

const EglApi* loadApi() noexcept {
    try {
        auto* api = new EglApi(); // leaked on purpose, see header
        if (std::string path; readEnv("TESSERA_EGL_LIBRARY", path) && !path.empty()) {
            // A file the user named (an optional bundled ANGLE or Mesa): by full path, its own folder
            // searched for its dependencies (ADR 0015). On Windows a bare name or relative path is refused
            // rather than searched for.
            api->lib = DynamicLibrary::openPath(path.c_str());
            if (!api->lib)
                logf(TESSERA_LOG_WARN,
                     "TESSERA_EGL_LIBRARY=%s could not be loaded (Windows needs an absolute path)",
                     path.c_str());
        } else {
#if defined(_WIN32)
            api->lib = DynamicLibrary::open(
                "libEGL.dll"); // System32 only: present when a system component ships ANGLE
#else
            api->lib = DynamicLibrary::open("libEGL.so.1");
#endif
        }
        if (!api->lib) {
            logf(TESSERA_LOG_INFO, "EGL backend unavailable: libEGL not found");
            delete api;
            return nullptr;
        }
        EglFns& f = api->fn;
        const bool ok = load(api->lib, "eglQueryString", f.eglQueryString) &&
                        load(api->lib, "eglGetError", f.eglGetError) &&
                        load(api->lib, "eglGetDisplay", f.eglGetDisplay) &&
                        load(api->lib, "eglInitialize", f.eglInitialize) &&
                        load(api->lib, "eglBindAPI", f.eglBindAPI) &&
                        load(api->lib, "eglQueryAPI", f.eglQueryAPI) &&
                        load(api->lib, "eglChooseConfig", f.eglChooseConfig) &&
                        load(api->lib, "eglGetConfigAttrib", f.eglGetConfigAttrib) &&
                        load(api->lib, "eglCreateContext", f.eglCreateContext) &&
                        load(api->lib, "eglDestroyContext", f.eglDestroyContext) &&
                        load(api->lib, "eglMakeCurrent", f.eglMakeCurrent) &&
                        load(api->lib, "eglGetCurrentContext", f.eglGetCurrentContext) &&
                        load(api->lib, "eglGetCurrentDisplay", f.eglGetCurrentDisplay) &&
                        load(api->lib, "eglGetCurrentSurface", f.eglGetCurrentSurface) &&
                        load(api->lib, "eglCreatePbufferSurface", f.eglCreatePbufferSurface) &&
                        load(api->lib, "eglDestroySurface", f.eglDestroySurface) &&
                        load(api->lib, "eglQuerySurface", f.eglQuerySurface) &&
                        load(api->lib, "eglSwapBuffers", f.eglSwapBuffers) &&
                        load(api->lib, "eglSwapInterval", f.eglSwapInterval) &&
                        load(api->lib, "eglCreateWindowSurface", f.eglCreateWindowSurface) &&
                        load(api->lib, "eglGetProcAddress", f.eglGetProcAddress);
        if (!ok) {
            logf(TESSERA_LOG_ERROR, "EGL backend unavailable: libEGL is missing core entry points");
            delete api;
            return nullptr;
        }
        f.eglGetPlatformDisplayEXT = reinterpret_cast<decltype(f.eglGetPlatformDisplayEXT)>(
            f.eglGetProcAddress("eglGetPlatformDisplayEXT"));
        f.eglQueryDevicesEXT =
            reinterpret_cast<decltype(f.eglQueryDevicesEXT)>(f.eglGetProcAddress("eglQueryDevicesEXT"));
        f.eglQueryDeviceStringEXT = reinterpret_cast<decltype(f.eglQueryDeviceStringEXT)>(
            f.eglGetProcAddress("eglQueryDeviceStringEXT"));
        f.eglQueryContext =
            reinterpret_cast<decltype(f.eglQueryContext)>(f.eglGetProcAddress("eglQueryContext"));
        f.eglCreatePlatformWindowSurfaceEXT = reinterpret_cast<decltype(f.eglCreatePlatformWindowSurfaceEXT)>(
            f.eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT"));
        f.eglCreatePlatformWindowSurface = reinterpret_cast<decltype(f.eglCreatePlatformWindowSurface)>(
            f.eglGetProcAddress("eglCreatePlatformWindowSurface"));
        f.eglQueryDeviceBinaryEXT = reinterpret_cast<decltype(f.eglQueryDeviceBinaryEXT)>(
            f.eglGetProcAddress("eglQueryDeviceBinaryEXT"));
        return api;
    } catch (...) {
        return nullptr;
    }
}

constexpr EGLenum kApis[2] = {kOpenGlEsApi, kOpenGlApi};

} // namespace

const EglApi* eglApi() noexcept {
    static const EglApi* api = loadApi(); // thread-safe one-time init
    return api;
}

EglForeignOps::State EglForeignOps::capture() noexcept {
    State s;
    const EglApi* api = eglApi();
    if (!api)
        return s;
    const EglFns& f = api->fn;
    s.boundApi = f.eglQueryAPI();
    for (int i = 0; i < 2; ++i) {
        // eglGetCurrentContext() answers for the thread's *bound* API, so bind each in turn.
        if (!f.eglBindAPI(kApis[i]))
            continue; // this API is not available at all
        Slot& slot = s.slots[i];
        slot.valid = true;
        slot.ctx = f.eglGetCurrentContext();
        slot.dpy = f.eglGetCurrentDisplay();
        slot.draw = f.eglGetCurrentSurface(kDraw);
        slot.read = f.eglGetCurrentSurface(kRead);
    }
    if (s.boundApi != 0)
        f.eglBindAPI(s.boundApi);
    return s;
}

void EglForeignOps::restore(const State& s) noexcept {
    const EglApi* api = eglApi();
    if (!api)
        return;
    const EglFns& f = api->fn;
    for (int i = 0; i < 2; ++i) {
        const Slot& slot = s.slots[i];
        if (!slot.valid || !f.eglBindAPI(kApis[i]))
            continue;
        const EGLContext now = f.eglGetCurrentContext();
        if (now == slot.ctx && f.eglGetCurrentSurface(kDraw) == slot.draw &&
            f.eglGetCurrentSurface(kRead) == slot.read)
            continue; // already as found
        if (slot.ctx) {
            if (!f.eglMakeCurrent(slot.dpy, slot.draw, slot.read, slot.ctx)) {
                logf(
                    TESSERA_LOG_WARN,
                    "could not restore the previous EGL context (error 0x%x); leaving the thread without one",
                    static_cast<unsigned>(f.eglGetError()));
                // Never leave one of OUR contexts current on a thread that did not ask for it.
                if (f.eglGetCurrentContext() != nullptr)
                    f.eglMakeCurrent(f.eglGetCurrentDisplay(), nullptr, nullptr, nullptr);
            }
        } else if (now) {
            // Nothing was current for this API before us: release whatever display now holds our context.
            f.eglMakeCurrent(f.eglGetCurrentDisplay(), nullptr, nullptr, nullptr);
        }
    }
    if (s.boundApi != 0)
        f.eglBindAPI(s.boundApi);
}

ScopedBindApi::ScopedBindApi(const EglApi& api, EGLenum bind) noexcept
    : api_(api), previous_(api.fn.eglQueryAPI()) {
    api_.fn.eglBindAPI(bind);
}

ScopedBindApi::~ScopedBindApi() {
    if (previous_ != 0)
        api_.fn.eglBindAPI(previous_);
}

} // namespace tessera::egl
