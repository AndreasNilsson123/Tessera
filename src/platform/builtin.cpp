#include "core/runtime.hpp"

#if defined(_WIN32)
#include "win32/wgl_backend.hpp"
#endif

namespace tessera {

// Platform backends are added here as they land (WGL: Phase 2, GLX: 3, EGL: 4, CGL: 5).
void registerBuiltinBackends(std::vector<std::shared_ptr<Backend>>& out) {
#if defined(_WIN32)
    out.push_back(win32::makeWglBackend());
#else
    (void)out;
#endif
}

} // namespace tessera
