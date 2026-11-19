// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include "cgl_types.hpp"
#include "core/foreign_state.hpp"
#include "core/library.hpp"

namespace tessera::cgl {

/*
 * OpenGL.framework (CGL + the GL entry points), loaded at run time. Created on first use and never freed.
 * `TESSERA_CGL_LIBRARY` overrides the library path (used to run the backend against a fake library in tests).
 */
struct CglApi {
    DynamicLibrary lib;
    CglFns fn;
};

/* Null if the library or any required entry point is missing. */
const CglApi* cglApi() noexcept;

/* The current CGL context is plain thread-local state, so the snapshot is just that pointer. */
struct CglForeignOps {
    struct State {
        CGLContextObj ctx = nullptr;
    };
    static State capture() noexcept;
    static void restore(const State& state) noexcept;
};

using ForeignContextGuard = ScopedForeignState<CglForeignOps>;

} // namespace tessera::cgl
