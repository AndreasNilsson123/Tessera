// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <string_view>

namespace tessera {

/* Records a description of the last failure on the calling thread. Never throws. */
void setLastError(std::string_view message) noexcept;
void clearLastError() noexcept;
/* Never null; valid until the next setLastError/clearLastError on this thread. */
const char* lastErrorPtr() noexcept;

} // namespace tessera
