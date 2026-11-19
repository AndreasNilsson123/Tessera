// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <tessera/tessera.h>

#include <exception>
#include <new>

#include "error.hpp"

namespace tessera {

/*
 * Every extern "C" entry point that can fail runs its body through guarded(), so no C++ exception can
 * ever cross the ABI boundary. The body returns a TesseraResult.
 */
template <class F> TesseraResult guarded(F&& body) noexcept {
    try {
        return body();
    } catch (const std::bad_alloc&) {
        setLastError("out of memory");
        return TESSERA_ERROR_OUT_OF_MEMORY;
    } catch (const std::exception& e) {
        setLastError(e.what());
        return TESSERA_ERROR_UNKNOWN;
    } catch (...) {
        setLastError("unknown C++ exception");
        return TESSERA_ERROR_UNKNOWN;
    }
}

/* Records `message` and returns `code`, for compact failure returns. */
inline TesseraResult fail(TesseraResult code, std::string_view message) noexcept {
    setLastError(message);
    return code;
}

} // namespace tessera
