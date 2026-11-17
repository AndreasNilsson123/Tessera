#pragma once

#include <memory>

#include "core/backend.hpp"

namespace tessera::cgl {

/*
 * macOS backend over CGL: drawable-less contexts for rendering into framebuffer objects. CGL needs no window, no
 * NSApplication and no particular thread, so it coexists with AppKit/JavaFX without touching either. macOS offers
 * only OpenGL 2.1 (legacy) and 3.2-4.1 core; there is no ES, no debug/robust/no-error context flags and no sRGB
 * default framebuffer (see ADR 0007).
 */
std::shared_ptr<Backend> makeCglBackend();

} // namespace tessera::cgl
