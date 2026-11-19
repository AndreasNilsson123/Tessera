// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
/*
 * The public header promises to be plain C99. This file is compiled as C (not C++) with the project's
 * warning flags, so any C++-ism that sneaks into tessera.h breaks the build.
 */
#include <tessera/tessera.h>

#include <stdio.h>

int main(void) {
    int32_t major = -1, minor = -1, patch = -1;
    tesseraGetVersion(&major, &minor, &patch);

    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    TesseraContextInfo info = tesseraDefaultContextInfo();

    if (major != TESSERA_VERSION_MAJOR || minor != TESSERA_VERSION_MINOR || patch != TESSERA_VERSION_PATCH)
        return 1;
    if (tesseraGetAbiVersion() != TESSERA_ABI_VERSION)
        return 2;
    if (cfg.structSize != sizeof(TesseraContextConfig) || info.structSize != sizeof(TesseraContextInfo))
        return 3;
    if (cfg.api != 0 || cfg.shareWith != TESSERA_NULL_HANDLE)
        return 4;

    puts("header_c: OK");
    return 0;
}
