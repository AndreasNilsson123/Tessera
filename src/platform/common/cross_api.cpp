// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "cross_api.hpp"

#include <dlfcn.h>

namespace tessera::crossapi {
namespace {

// True if the process already has the library (someone else loaded it, or we did): never loads it ourselves.
bool alreadyLoaded(const char* name) noexcept {
    if (void* h = dlopen(name, RTLD_NOW | RTLD_NOLOAD)) {
        dlclose(h);
        return true;
    }
    return false;
}

bool eglLoaded() noexcept {
    return alreadyLoaded("libEGL.so.1") && egl::eglApi() != nullptr;
}

bool glxLoaded() noexcept {
    return (alreadyLoaded("libGL.so.1") || alreadyLoaded("libGLX.so.0")) && x11::linuxApi() != nullptr;
}

} // namespace

GlxWithEglOps::State GlxWithEglOps::capture() noexcept {
    State s;
    s.glx = x11::GlxForeignOps::capture();
    if (eglLoaded()) {
        s.egl = egl::EglForeignOps::capture();
        s.hasEgl = true;
    }
    return s;
}

void GlxWithEglOps::restore(const State& s) noexcept {
    x11::GlxForeignOps::restore(s.glx); // first: takes our own GLX context away again
    if (s.hasEgl)
        egl::EglForeignOps::restore(s.egl); // then: puts the host's EGL context back, if that is what it had
}

EglWithGlxOps::State EglWithGlxOps::capture() noexcept {
    State s;
    s.egl = egl::EglForeignOps::capture();
    if (glxLoaded()) {
        s.glx = x11::GlxForeignOps::capture();
        s.hasGlx = true;
    }
    return s;
}

void EglWithGlxOps::restore(const State& s) noexcept {
    egl::EglForeignOps::restore(s.egl);
    if (s.hasGlx)
        x11::GlxForeignOps::restore(s.glx);
}

void releaseOtherEglCurrent() noexcept {
    if (!eglLoaded())
        return;
    const egl::EglApi* api = egl::eglApi();
    const egl::EglForeignOps::State current = egl::EglForeignOps::capture();
    for (int i = 0; i < 2; ++i) {
        const auto& slot = current.slots[i];
        if (!slot.valid || !slot.ctx)
            continue;
        api->fn.eglBindAPI(i == 0 ? egl::kOpenGlEsApi : egl::kOpenGlApi); // same order as EglForeignOps::Slot
        api->fn.eglMakeCurrent(slot.dpy, nullptr, nullptr, nullptr);
    }
    if (current.boundApi)
        api->fn.eglBindAPI(current.boundApi);
}

void releaseOtherGlxCurrent() noexcept {
    if (!glxLoaded())
        return;
    const x11::LinuxApi* api = x11::linuxApi();
    if (api->glx.glXGetCurrentContext() == nullptr)
        return;
    if (x11::Display* dpy = api->glx.glXGetCurrentDisplay()) {
        x11::XErrorScope scope(*api, dpy);
        api->glx.glXMakeContextCurrent(dpy, 0, 0, nullptr);
        scope.sync();
    }
}

} // namespace tessera::crossapi
