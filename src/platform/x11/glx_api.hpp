// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include "core/foreign_state.hpp"
#include "core/library.hpp"
#include "x11_types.hpp"

namespace tessera::x11 {

/*
 * libX11 + libGL, loaded at run time. Created on first use and intentionally never freed: other components
 * in the process (JavaFX/GTK, AWT, LWJGL) use the same libraries, and dlclose of a GL driver is hazardous.
 */
struct LinuxApi {
    DynamicLibrary x11;
    DynamicLibrary gl;
    X11Fns xlib;
    GlxFns glx;
};

/* Null if libX11 or libGL (or any required entry point) is missing. */
const LinuxApi* linuxApi() noexcept;

/*
 * Has anyone (normally the host: GLFW, SDL, GTK, AWT) already called XInitThreads()? libX11 keeps the answer
 * in its exported
 * `_Xglobal_lock`, which XInitThreads sets. 1 = yes, 0 = no, -1 = this libX11 does not export the symbol
 * (unknown).
 */
int xlibThreadsInitialized(const DynamicLibrary& x11) noexcept;

/*
 * Xlib's error handler is process-global and its default action is to terminate the process. While a
 * scope is alive, X errors raised on `display` are recorded instead; errors on any other display are passed
 * to whatever handler was installed before, so we never swallow errors that belong to the host application.
 * Scopes serialise on a global mutex (the handler slot is shared by the whole process) and must not nest.
 */
class XErrorScope {
  public:
    XErrorScope(const LinuxApi& api, Display* display);
    ~XErrorScope();
    XErrorScope(const XErrorScope&) = delete;
    XErrorScope& operator=(const XErrorScope&) = delete;

    /* Flushes the connection and returns the first X error code seen on `display` (0 = none). */
    int sync();

  private:
    const LinuxApi& api_;
    Display* display_;
    XErrorHandler previous_ = nullptr;
};

/* Snapshot policy for ScopedForeignState: the thread's current GLX context, display and drawables. */
struct GlxForeignOps {
    struct State {
        GLXContext ctx = nullptr;
        Display* dpy = nullptr;
        GLXDrawable draw = 0;
        GLXDrawable read = 0;
    };
    static State capture() noexcept;
    static void restore(const State& state) noexcept;
};

using ForeignContextGuard = ScopedForeignState<GlxForeignOps>;

} // namespace tessera::x11
