// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace tessera {
namespace {
struct LogState {
    std::mutex mutex;
    TesseraLogCallback callback = nullptr;
    void* userData = nullptr;
};

// Intentionally leaked: avoids static-destruction-order problems when the library is unloaded while
// other threads (e.g. JVM threads) may still log.
LogState& state() {
    static LogState* s = new LogState();
    return *s;
}
} // namespace

void setLogCallback(TesseraLogCallback callback, void* userData) noexcept {
    LogState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.callback = callback;
    s.userData = userData;
}

void logf(int32_t level, const char* fmt, ...) noexcept {
    LogState& s = state();
    TesseraLogCallback cb;
    void* ud;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        cb = s.callback;
        ud = s.userData;
    }
    if (!cb)
        return;

    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    buffer[sizeof buffer - 1] = '\0';
    cb(level, buffer, ud); // called without any lock held
}

} // namespace tessera

extern "C" {
TESSERA_API void TESSERA_CALL tesseraSetLogCallback(TesseraLogCallback callback, void* userData) {
    tessera::setLogCallback(callback, userData);
}
}
