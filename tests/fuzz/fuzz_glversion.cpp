// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors

// libFuzzer target: the parser for GL_VERSION strings that drivers hand back (untrusted input from outside
// the process).
#include <cstdint>
#include <string>

#include "platform/common/gl_query.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string text(reinterpret_cast<const char*>(data),
                           size); // NUL-terminated copy of exactly the input
    bool es = false;
    int major = -1, minor = -1;
    if (tessera::parseGlVersion(text.c_str(), es, major, minor)) {
        if (major < 0 || major > 9999 || minor < 0 || minor > 9999)
            __builtin_trap(); // a "valid" result outside the documented range is a bug
    }
    return 0;
}
