// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <tessera/tessera.h>

namespace tessera {

/*
 * Validates a caller-supplied config and produces a fully-populated copy.
 *  - structSize smaller than the current struct: missing trailing fields are treated as zero (older caller).
 *  - structSize larger: extra bytes are ignored (newer caller); we only read what we know.
 *  - structSize must at least cover the fields up to and including `fallback` (the v1 core).
 * On failure returns INVALID_ARGUMENT with a descriptive last-error message.
 */
TesseraResult normalizeConfig(const TesseraContextConfig* in, TesseraContextConfig& out) noexcept;

} // namespace tessera
