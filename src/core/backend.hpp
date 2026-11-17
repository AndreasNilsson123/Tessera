#pragma once

#include <tessera/tessera.h>

#include <memory>

namespace tessera {

/* A native context owned by a backend. Destruction releases the native resources. */
class ContextObject {
  public:
    virtual ~ContextObject() = default;
    /* Fill everything except structSize handling (done by the caller). Must not throw. */
    virtual TesseraResult describe(TesseraContextInfo& out) const noexcept = 0;
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
};

} // namespace tessera
