// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include "core/foreign_state.hpp"
#include "platform/egl/egl_api.hpp"
#include "platform/x11/glx_api.hpp"

namespace tessera::crossapi {

/*
 * A thread has ONE native current context as far as Mesa's libGL/libEGL are concerned, whichever of GLX and
 * EGL made it current; other drivers keep them apart. A host that renders through EGL (GLFW or SDL on
 * Wayland, a toolkit on Android-style stacks) and a Tessera backend that uses GLX (or the reverse) therefore
 * step on each other's thread state: making our GLX context current while the host's EGL context is current
 * failed outright (BadAccess) and, even where it worked, the host's context would not have been put back.
 *
 * These policies capture and restore the state of BOTH APIs, but only of an API whose library the process has
 * already loaded (looking must never load libEGL into a process that does not use it), and `releaseOther*`
 * makes the thread free of the other API's context before ours becomes current ("making a Tessera context
 * current replaces whatever was current" is the documented contract; it must not fail because the replaced
 * context belongs to the other API).
 */

struct GlxWithEglOps {
    struct State {
        x11::GlxForeignOps::State glx;
        egl::EglForeignOps::State egl;
        bool hasEgl = false;
    };
    static State capture() noexcept;
    static void restore(const State& s) noexcept;
};

struct EglWithGlxOps {
    struct State {
        egl::EglForeignOps::State egl;
        x11::GlxForeignOps::State glx;
        bool hasGlx = false;
    };
    static State capture() noexcept;
    static void restore(const State& s) noexcept;
};

using GlxGuard = ScopedForeignState<GlxWithEglOps>;
using EglGuard = ScopedForeignState<EglWithGlxOps>;

/* Make no EGL (resp. GLX) context current on this thread, if that library is loaded in the process and one
 * is. */
void releaseOtherEglCurrent() noexcept;
void releaseOtherGlxCurrent() noexcept;

} // namespace tessera::crossapi
