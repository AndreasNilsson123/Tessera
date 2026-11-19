// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <string>

namespace tessera {

/* Reads an environment variable (portable: MSVC deprecates getenv). False if unset or empty. */
bool readEnv(const char* name, std::string& out);

} // namespace tessera
