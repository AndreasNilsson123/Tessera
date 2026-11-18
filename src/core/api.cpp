// extern "C" entry points for lifecycle and contexts. Everything goes through guarded().
#include <tessera/tessera.h>

#include <cstddef>
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

TESSERA_API TesseraResult TESSERA_CALL tesseraMakeCurrent(TesseraContext context) {
    return guarded([&] { return Runtime::instance().makeCurrent(context); });
}

TESSERA_API TesseraContext TESSERA_CALL tesseraGetCurrentContext(void) {
    try {
        return Runtime::instance().currentContext();
    } catch (...) {
        return TESSERA_NULL_HANDLE;
    }
}

TESSERA_API void* TESSERA_CALL tesseraGetProcAddress(const char* name) {
    try {
        return Runtime::instance().getProcAddress(name);
    } catch (...) {
        return nullptr;
    }
}

TESSERA_API TesseraResult TESSERA_CALL tesseraSwapBuffers(TesseraContext context) {
    return guarded([&] { return Runtime::instance().swapBuffers(context); });
}

TESSERA_API TesseraResult TESSERA_CALL tesseraSetSwapInterval(TesseraContext context, int32_t interval) {
    return guarded([&] { return Runtime::instance().setSwapInterval(context, interval); });
}

TESSERA_API TesseraResult TESSERA_CALL tesseraGetResetStatus(TesseraContext context, int32_t* outStatus) {
    return guarded([&]() -> TesseraResult {
        if (!outStatus) return fail(TESSERA_ERROR_INVALID_ARGUMENT, "outStatus is NULL");
        *outStatus = TESSERA_RESET_NONE;
        return Runtime::instance().getResetStatus(context, *outStatus);
    });
}

TESSERA_API TesseraResult TESSERA_CALL tesseraRecreateContext(TesseraContext context, TesseraContext* outNew,
                                                              uint32_t* outFlags) {
    return guarded([&]() -> TesseraResult {
        if (!outNew) return fail(TESSERA_ERROR_INVALID_ARGUMENT, "outNew is NULL");
        *outNew = TESSERA_NULL_HANDLE;
        if (outFlags) *outFlags = 0;
        uint32_t flags = 0;
        const TesseraResult r = Runtime::instance().recreateContext(context, *outNew, flags);
        if (outFlags) *outFlags = flags;
        return r;
    });
}

TESSERA_API TesseraResult TESSERA_CALL tesseraGetNativeHandles(TesseraContext context, TesseraNativeHandles* outHandles) {
    return guarded([&]() -> TesseraResult {
        if (!outHandles) return fail(TESSERA_ERROR_INVALID_ARGUMENT, "outHandles is NULL");
        const uint32_t callerSize = outHandles->structSize;
        if (callerSize < sizeof(uint32_t) * 2) return fail(TESSERA_ERROR_INVALID_ARGUMENT, "outHandles->structSize not set");
        TesseraNativeHandles full = tesseraDefaultNativeHandles();
        if (TesseraResult r = Runtime::instance().getNativeHandles(context, full); r != TESSERA_SUCCESS) return r;
        // Write at most what the caller's (possibly older/newer) struct can hold.
        full.structSize = callerSize < sizeof full ? callerSize : static_cast<uint32_t>(sizeof full);
        std::memcpy(outHandles, &full, full.structSize);
        return TESSERA_SUCCESS;
    });
}

TESSERA_API TesseraResult TESSERA_CALL tesseraPrewarmContexts(const TesseraContextConfig* config, int32_t count) {
    return guarded([&]() -> TesseraResult {
        TesseraContextConfig normalized;
        if (TesseraResult r = normalizeConfig(config, normalized); r != TESSERA_SUCCESS) return r;
        return Runtime::instance().prewarm(normalized, count);
    });
}

TESSERA_API int32_t TESSERA_CALL tesseraPooledContextCount(void) {
    try {
        return static_cast<int32_t>(Runtime::instance().pooledContexts());
    } catch (...) {
        return 0;
    }
}

TESSERA_API TesseraResult TESSERA_CALL tesseraCreateContextForWindow(const TesseraContextConfig* config, uint64_t nativeWindow,
                                                                     uint32_t flags, TesseraContext* outContext) {
    return guarded([&]() -> TesseraResult {
        if (!outContext) return fail(TESSERA_ERROR_INVALID_ARGUMENT, "outContext is NULL");
        *outContext = TESSERA_NULL_HANDLE;
        if (!config) return fail(TESSERA_ERROR_INVALID_ARGUMENT, "config is NULL");
        // The surface of an attached context is the window: accept DEFAULT or WINDOW, validate the rest as usual. Only
        // `structSize` bytes of the caller's struct may be read (it can be older and smaller than ours).
        TesseraContextConfig copy;
        std::memset(&copy, 0, sizeof copy);
        const uint32_t claimed = config->structSize;
        std::memcpy(&copy, config, claimed < sizeof copy ? claimed : sizeof copy);
        constexpr std::size_t kSurfaceEnd = offsetof(TesseraContextConfig, surface) + sizeof(TesseraEnum);
        if (claimed >= kSurfaceEnd) {
            if (copy.surface == TESSERA_SURFACE_WINDOW) copy.surface = TESSERA_SURFACE_DEFAULT;
            else if (copy.surface != TESSERA_SURFACE_DEFAULT)
                return fail(TESSERA_ERROR_INVALID_ARGUMENT, "the surface of a context for a window must be TESSERA_SURFACE_DEFAULT or TESSERA_SURFACE_WINDOW");
        }
        TesseraContextConfig normalized;
        if (TesseraResult r = normalizeConfig(&copy, normalized); r != TESSERA_SUCCESS) return r;
        return Runtime::instance().createContextForWindow(normalized, nativeWindow, flags, *outContext);
    });
}

TESSERA_API TesseraResult TESSERA_CALL tesseraProbe(const TesseraContextConfig* config, TesseraProbeReport* outReport) {
    return guarded([&]() -> TesseraResult {
        if (!outReport) return fail(TESSERA_ERROR_INVALID_ARGUMENT, "outReport is NULL");
        const uint32_t callerSize = outReport->structSize;
        if (callerSize < sizeof(uint32_t) * 2) return fail(TESSERA_ERROR_INVALID_ARGUMENT, "outReport->structSize not set");
        TesseraContextConfig normalized;
        if (TesseraResult r = normalizeConfig(config, normalized); r != TESSERA_SUCCESS) return r;
        TesseraProbeReport full = tesseraDefaultProbeReport();
        const TesseraResult result = Runtime::instance().probe(normalized, full);
        // Write at most what the caller's (possibly older/newer) struct can hold.
        full.structSize = callerSize < sizeof full ? callerSize : static_cast<uint32_t>(sizeof full);
        std::memcpy(outReport, &full, full.structSize);
        return result;
    });
}

} // extern "C"
