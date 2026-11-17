#include "core/runtime.hpp"

#if defined(_WIN32)
#include "win32/wgl_backend.hpp"
#elif defined(__linux__)
#include "x11/glx_backend.hpp"
#endif

namespace tessera {

// Platform backends are added here as they land (WGL: Phase 2, GLX: 3, EGL: 4, CGL: 5).
void registerBuiltinBackends(std::vector<std::shared_ptr<Backend>>& out) {
#if defined(_WIN32)
    out.push_back(win32::makeWglBackend());
#elif defined(__linux__)
    out.push_back(x11::makeGlxBackend());
#else
    (void)out;
#endif
}

} // namespace tessera
