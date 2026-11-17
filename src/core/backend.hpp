#pragma once

#include <tessera/tessera.h>

#include <memory>

#include "thread_affinity.hpp"

namespace tessera {

/* A native context owned by a backend. Destruction releases the native resources. */
class ContextObject {
  public:
    virtual ~ContextObject() = default;
    /* Fill everything except structSize handling (done by the caller). Must not throw. */
    virtual TesseraResult describe(TesseraContextInfo& out) const noexcept = 0;

    /* Native make-current / release. Thread ownership is tracked by the core (see `affinity`); these only
     * talk to the driver. unbindCurrent must leave the thread alone if this context is not the native
     * current one (some other library may have changed it since). */
    virtual TesseraResult bindCurrent() noexcept { return TESSERA_ERROR_UNSUPPORTED; }
    virtual void unbindCurrent() noexcept {}
    virtual TesseraResult swapBuffers() noexcept { return TESSERA_ERROR_UNSUPPORTED; }
    /* Called only while the context is current on the calling thread. */
    virtual TesseraResult setSwapInterval(int32_t /*interval*/) noexcept { return TESSERA_ERROR_UNSUPPORTED; }

    ContextAffinity affinity;
};

/*
 * A platform backend (WGL, GLX, EGL, CGL, ...). Implementations must:
 *  - save/restore the caller's native current context (ForeignContextGuard) around everything they do,
 *  - return TESSERA_ERROR_UNSUPPORTED (without side effects) if they cannot serve the request at all,
 *    so the next backend gets a chance,
 *  - report any other failure with a specific code and last-error message.
 */
class Backend {
  public:
    virtual ~Backend() = default;
    virtual const char* name() const noexcept = 0;
    virtual TesseraResult createContext(const TesseraContextConfig& config,
                                        const std::shared_ptr<ContextObject>& shareWith,
                                        std::shared_ptr<ContextObject>& out) = 0;
    /* Null if unknown. May be called with or without a context current. */
    virtual void* getProcAddress(const char* /*name*/) noexcept { return nullptr; }
};

} // namespace tessera
