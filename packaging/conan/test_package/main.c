/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Tessera contributors */
#include <tessera/tessera.h>

#include <stdio.h>

int main(void) {
    int32_t major = 0, minor = 0, patch = 0;
    tesseraGetVersion(&major, &minor, &patch);
    TesseraContextConfig c = tesseraDefaultContextConfig();
    printf("tessera %d.%d.%d (ABI %d, header ABI %d), config size %u\n", major, minor, patch,
           tesseraGetAbiVersion(), TESSERA_ABI_VERSION, c.structSize);
    /* Init/terminate need no GPU or display: this proves the package links and runs. */
    return tesseraInit() == TESSERA_SUCCESS && tesseraTerminate() == TESSERA_SUCCESS ? 0 : 1;
}
