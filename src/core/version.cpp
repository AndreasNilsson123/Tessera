#include <tessera/tessera.h>

extern "C" {

TESSERA_API void TESSERA_CALL tesseraGetVersion(int32_t* major, int32_t* minor, int32_t* patch) {
    if (major)
        *major = TESSERA_VERSION_MAJOR;
    if (minor)
        *minor = TESSERA_VERSION_MINOR;
    if (patch)
        *patch = TESSERA_VERSION_PATCH;
}

TESSERA_API int32_t TESSERA_CALL tesseraGetAbiVersion(void) {
    return TESSERA_ABI_VERSION;
}

} // extern "C"
