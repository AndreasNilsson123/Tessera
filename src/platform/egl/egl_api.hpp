// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include "core/foreign_state.hpp"
#include "core/library.hpp"
#include "egl_types.hpp"

namespace tessera::egl {

/*
 * libEGL, loaded at run time. Created on first use and never freed (other components in the process use the
 * same library). `TESSERA_EGL_LIBRARY` overrides the library to load, e.g. a specific vendor or ANGLE build;
 * on Windows it must be an absolute path.
 */
struct EglApi {
    DynamicLibrary lib;
    EglFns fn;
};

/* Null if libEGL (or any core entry point) is missing. */
const EglApi* eglApi() noexcept;

/*
 * EGL keeps *per-thread* state that other libraries rely on, so a guest must put all of it back:
 *  - the current context/display/surfaces of EACH client API (a thread can have a GLES and a desktop GL
 * context current at the same time), and
 *  - the thread's bound client API (eglBindAPI), which decides what eglGetCurrentContext() reports.
 * (The thread's last-error code, eglGetError, cannot be saved without clearing it; it is not preserved.)
 */
struct EglForeignOps {
    struct Slot {
        bool valid = false;
        EGLContext ctx = nullptr;
        EGLDisplay dpy = nullptr;
        EGLSurface draw = nullptr;
        EGLSurface read = nullptr;
    };
    struct State {
        EGLenum boundApi = 0;
        Slot slots[2];
    };
    static State capture() noexcept;
    static void restore(const State& state) noexcept;
};

using ForeignContextGuard = ScopedForeignState<EglForeignOps>;

/* Binds an API for the duration of a scope and puts the thread's previously bound API back. */
class ScopedBindApi {
  public:
    ScopedBindApi(const EglApi& api, EGLenum bind) noexcept;
    ~ScopedBindApi();
    ScopedBindApi(const ScopedBindApi&) = delete;
    ScopedBindApi& operator=(const ScopedBindApi&) = delete;

  private:
    const EglApi& api_;
    EGLenum previous_;
};

} // namespace tessera::egl
