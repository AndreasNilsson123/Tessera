// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include <algorithm>
#include <string>

#include "core/env.hpp"
#include "core/log.hpp"
#include "core/runtime.hpp"

#if defined(_WIN32)
#include "egl/egl_backend.hpp"
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
 * TESSERA_BACKEND=<name>[,<name>...] restricts and orders the backends (names: wgl, glx, egl, cgl, software).
 * Without it the platform default order applies and the first backend able to serve a request wins; one that
 * cannot serve it at all reports UNSUPPORTED and the next is tried. `software` (the CPU renderer, ADR 0015)
 * is known by name only: it is never part of the default order, so asking for "wgl,software" means "the GPU,
 * else the CPU".
 */
void applyBackendSelection(std::vector<std::shared_ptr<Backend>>& backends,
                           const std::vector<std::shared_ptr<Backend>>& byNameOnly) {
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
        auto matches = [&](const std::shared_ptr<Backend>& b) { return name == b->name(); };
        auto it = std::find_if(backends.begin(), backends.end(), matches);
        std::shared_ptr<Backend> found = it != backends.end() ? *it : nullptr;
        if (!found) {
            auto extra = std::find_if(byNameOnly.begin(), byNameOnly.end(), matches);
            if (extra != byNameOnly.end())
                found = *extra;
        }
        if (!found)
            logf(TESSERA_LOG_WARN, "TESSERA_BACKEND: no backend named '%s' in this build", name.c_str());
        else if (std::find(selected.begin(), selected.end(), found) == selected.end())
            selected.push_back(found);
    }
    backends = std::move(selected);
}

} // namespace

// Platform backends are added here as they land (WGL: Phase 2, GLX: 3, EGL: 4, CGL: 5).
void registerBuiltinBackends(std::vector<std::shared_ptr<Backend>>& out) {
    std::vector<std::shared_ptr<Backend>> byNameOnly;
#if defined(_WIN32)
    out.push_back(win32::makeWglBackend());
    // EGL on Windows exists only through a libEGL the user names (TESSERA_EGL_LIBRARY, i.e. ANGLE): never in
    // the default order.
    byNameOnly.push_back(egl::makeEglBackend());
    byNameOnly.push_back(egl::makeSoftwareBackend());
#elif defined(__linux__)
    out.push_back(x11::makeGlxBackend());
    out.push_back(egl::makeEglBackend());
    byNameOnly.push_back(egl::makeSoftwareBackend());
#elif defined(__APPLE__)
    out.push_back(cgl::makeCglBackend());
#endif
    applyBackendSelection(out, byNameOnly);
}

} // namespace tessera
