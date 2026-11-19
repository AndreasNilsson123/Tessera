// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include <algorithm>
#include <string>

#include "core/env.hpp"
#include "core/log.hpp"
#include "core/runtime.hpp"

#if defined(_WIN32)
#include "win32/wgl_backend.hpp"
#elif defined(__linux__)
#include "egl/egl_backend.hpp"
#include "x11/glx_backend.hpp"
#elif defined(__APPLE__)
#include "cgl/cgl_backend.hpp"
#endif

namespace tessera {
namespace {

/*
 * TESSERA_BACKEND=<name>[,<name>...] restricts and orders the backends (names: wgl, glx, egl, cgl). Without
 * it the platform default order applies and the first backend able to serve a request wins; one that cannot
 * serve it at all reports UNSUPPORTED and the next is tried.
 */
void applyBackendSelection(std::vector<std::shared_ptr<Backend>>& backends) {
    std::string list;
    if (!readEnv("TESSERA_BACKEND", list))
        return;

    std::vector<std::shared_ptr<Backend>> selected;
    for (std::size_t start = 0; start <= list.size();) {
        std::size_t end = list.find(',', start);
        if (end == std::string::npos)
            end = list.size();
        const std::string name = list.substr(start, end - start);
        start = end + 1;
        if (name.empty())
            continue;
        auto it = std::find_if(backends.begin(), backends.end(),
                               [&](const std::shared_ptr<Backend>& b) { return name == b->name(); });
        if (it == backends.end())
            logf(TESSERA_LOG_WARN, "TESSERA_BACKEND: no backend named '%s' in this build", name.c_str());
        else if (std::find(selected.begin(), selected.end(), *it) == selected.end())
            selected.push_back(*it);
    }
    backends = std::move(selected);
}

} // namespace

// Platform backends are added here as they land (WGL: Phase 2, GLX: 3, EGL: 4, CGL: 5).
void registerBuiltinBackends(std::vector<std::shared_ptr<Backend>>& out) {
#if defined(_WIN32)
    out.push_back(win32::makeWglBackend());
#elif defined(__linux__)
    out.push_back(x11::makeGlxBackend());
    out.push_back(egl::makeEglBackend());
#elif defined(__APPLE__)
    out.push_back(cgl::makeCglBackend());
#endif
    applyBackendSelection(out);
}

} // namespace tessera
