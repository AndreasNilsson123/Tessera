#include "core/runtime.hpp"

namespace tessera {

// Platform backends are added here as they land (WGL: Phase 2, GLX: 3, EGL: 4, CGL: 5).
void registerBuiltinBackends(std::vector<std::shared_ptr<Backend>>& /*out*/) {}

} // namespace tessera
