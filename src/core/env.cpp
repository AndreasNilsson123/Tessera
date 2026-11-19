// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "env.hpp"

#include <cstdlib>

namespace tessera {

bool readEnv(const char* name, std::string& out) {
    out.clear();
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        return false;
    out.assign(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (!value)
        return false;
    out.assign(value);
#endif
    return !out.empty();
}

} // namespace tessera
