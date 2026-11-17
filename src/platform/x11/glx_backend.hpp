#pragma once

#include <memory>

#include "core/backend.hpp"

namespace tessera::x11 {

/* Linux/X11 backend: GLX 1.3 with the ARB_create_context family. */
std::shared_ptr<Backend> makeGlxBackend();

} // namespace tessera::x11
