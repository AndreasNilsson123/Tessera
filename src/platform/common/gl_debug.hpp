// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <functional>

namespace tessera {

/*
 * Forwards the current context's KHR_debug messages (glDebugMessageCallback) to the Tessera log callback,
 * mapping GL severity to log level (high -> error, medium -> warn, low -> info, notification -> debug).
 *
 * Call with the context current, and only for a context that was granted TESSERA_FLAG_DEBUG (otherwise the
 * enable calls would raise GL errors in the application's error queue). Messages are made synchronous so they
 * arrive on the thread that issued the GL call and are never delivered after the context is gone. The
 * callback is a plain function in this library with no per-context state, so there is nothing to outlive the
 * context. An application that installs its own glDebugMessageCallback simply replaces this one.
 *
 * `getProc` resolves a GL function by name through the backend's loader. Returns false if the context has no
 * debug output (the GL entry points are missing); leaves the GL error state clean either way.
 */
bool installGlDebugForwarder(const std::function<void*(const char*)>& getProc) noexcept;

} // namespace tessera
