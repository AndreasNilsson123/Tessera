// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "error.hpp"

#include <tessera/tessera.h>

#include <cstddef>
#include <cstring>

namespace tessera {
namespace {
// Fixed buffer: no allocation, so recording an error can never itself fail or throw.
constexpr std::size_t kMaxMessage = 512;
thread_local char t_lastError[kMaxMessage] = {0};
} // namespace

void setLastError(std::string_view message) noexcept {
    const std::size_t n = message.size() < kMaxMessage - 1 ? message.size() : kMaxMessage - 1;
    std::memcpy(t_lastError, message.data(), n);
    t_lastError[n] = '\0';
}

void clearLastError() noexcept {
    t_lastError[0] = '\0';
}

const char* lastErrorPtr() noexcept {
    return t_lastError;
}

} // namespace tessera

extern "C" {

TESSERA_API const char* TESSERA_CALL tesseraGetLastErrorMessage(void) {
    return tessera::lastErrorPtr();
}

TESSERA_API const char* TESSERA_CALL tesseraResultString(TesseraResult result) {
    switch (result) {
    case TESSERA_SUCCESS:
        return "success";
    case TESSERA_ERROR_UNKNOWN:
        return "unknown error";
    case TESSERA_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case TESSERA_ERROR_INVALID_HANDLE:
        return "invalid or stale handle";
    case TESSERA_ERROR_NOT_INITIALIZED:
        return "library not initialized";
    case TESSERA_ERROR_OUT_OF_MEMORY:
        return "out of memory";
    case TESSERA_ERROR_UNSUPPORTED:
        return "unsupported on this platform or build";
    case TESSERA_ERROR_NO_MATCHING_CONFIG:
        return "no matching configuration";
    case TESSERA_ERROR_PLATFORM:
        return "native platform error";
    case TESSERA_ERROR_WRONG_THREAD:
        return "object used on the wrong thread";
    case TESSERA_ERROR_CONTEXT_LOST:
        return "context lost";
    default:
        return "unrecognized result code";
    }
}

} // extern "C"
