// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <tessera/tessera.h>

namespace tessera {

void setLogCallback(TesseraLogCallback callback, void* userData) noexcept;

/* printf-style; formats into a bounded buffer, never allocates, never throws. No-op if no callback. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
void logf(int32_t level, const char* fmt, ...) noexcept;

} // namespace tessera
