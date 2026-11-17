// extern "C" entry points for lifecycle and contexts. Everything goes through guarded().
#include <tessera/tessera.h>

#include <cstring>

#include "boundary.hpp"
#include "config.hpp"
#include "runtime.hpp"

using namespace tessera;

extern "C" {

TESSERA_API TesseraResult TESSERA_CALL tesseraInit(void) {
    return guarded([] { return Runtime::instance().init(); });
}

TESSERA_API TesseraResult TESSERA_CALL tesseraTerminate(void) {
    return guarded([] { return Runtime::instance().terminate(); });
}

TESSERA_API TesseraResult TESSERA_CALL tesseraCreateContext(const TesseraContextConfig* config,
                                                            TesseraContext* outContext) {
    return guarded([&]() -> TesseraResult {
        if (!outContext) return fail(TESSERA_ERROR_INVALID_ARGUMENT, "outContext is NULL");
        *outContext = TESSERA_NULL_HANDLE;
        TesseraContextConfig normalized;
        if (TesseraResult r = normalizeConfig(config, normalized); r != TESSERA_SUCCESS) return r;
        return Runtime::instance().createContext(normalized, *outContext);
    });
}

TESSERA_API TesseraResult TESSERA_CALL tesseraDestroyContext(TesseraContext context) {
    return guarded([&] { return Runtime::instance().destroyContext(context); });
}

TESSERA_API TesseraResult TESSERA_CALL tesseraGetContextInfo(TesseraContext context,
                                                             TesseraContextInfo* outInfo) {
    return guarded([&]() -> TesseraResult {
        if (!outInfo) return fail(TESSERA_ERROR_INVALID_ARGUMENT, "outInfo is NULL");
        const uint32_t callerSize = outInfo->structSize;
        if (callerSize < sizeof(uint32_t) * 2)
            return fail(TESSERA_ERROR_INVALID_ARGUMENT, "outInfo->structSize not set");
        TesseraContextInfo full;
        std::memset(&full, 0, sizeof full);
        if (TesseraResult r = Runtime::instance().describeContext(context, full); r != TESSERA_SUCCESS)
            return r;
        // Write at most what the caller's (possibly older/newer) struct can hold.
        full.structSize = callerSize < sizeof full ? callerSize : static_cast<uint32_t>(sizeof full);
        std::memcpy(outInfo, &full, full.structSize);
        return TESSERA_SUCCESS;
    });
}

} // extern "C"
