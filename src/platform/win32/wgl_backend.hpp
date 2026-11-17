#pragma once

#include <memory>

#include "core/backend.hpp"

namespace tessera::win32 {

/* Windows backend: WGL with the ARB_create_context / ARB_pixel_format family. */
std::shared_ptr<Backend> makeWglBackend();

} // namespace tessera::win32
