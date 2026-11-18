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

} // namespace tessera::egl
