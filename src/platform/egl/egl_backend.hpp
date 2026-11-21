// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <memory>

#include "core/backend.hpp"

namespace tessera::egl {

/*
 * EGL backend: headless/offscreen contexts for desktop OpenGL and OpenGL ES with no window system. Drawables
 * are pbuffers or none at all (EGL_KHR_surfaceless_context), so it works on servers, in containers and under
 * Wayland without ever touching the host's windows.
 */
std::shared_ptr<Backend> makeEglBackend();

/* The same EGL code restricted to a CPU renderer; only ever selected by name (TESSERA_BACKEND=software). */
std::shared_ptr<Backend> makeSoftwareBackend();

} // namespace tessera::egl
