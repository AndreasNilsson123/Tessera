// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <tessera/tessera.h>

#include <atomic>
#include <memory>

#include "boundary.hpp"
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
     * current one (some other library may have changed it since). A native "context lost" condition should
     * be reported as TESSERA_ERROR_CONTEXT_LOST so the core can mark the context lost. */
    virtual TesseraResult bindCurrent() noexcept {
        return TESSERA_ERROR_UNSUPPORTED;
    }
    virtual void unbindCurrent() noexcept {}
    virtual TesseraResult swapBuffers() noexcept {
        return TESSERA_ERROR_UNSUPPORTED;
    }
    /* Called only while the context is current on the calling thread. */
    virtual TesseraResult setSwapInterval(int32_t /*interval*/) noexcept {
        return TESSERA_ERROR_UNSUPPORTED;
    }

    /* The window this context renders into changed size (Wayland). Called with the context current.
     * UNSUPPORTED = nothing to do or no window; kinds that follow the window by themselves return SUCCESS. */
    virtual TesseraResult resizeWindow(int32_t /*width*/, int32_t /*height*/) noexcept {
        return TESSERA_ERROR_UNSUPPORTED;
    }

    /* Fills caps / applied / maxInterval of `out` (pre-set to "nothing known" by the core, which also fills
     * `requested`). Called with the context current on the calling thread. The default reports no swap
     * control. */
    virtual TesseraResult getSwapControl(TesseraSwapControl& /*out*/) noexcept {
        return TESSERA_SUCCESS;
    }

    /* Polls the driver for a GPU reset (glGetGraphicsResetStatus). Called with the context current on the
     * calling thread. `out` is a TESSERA_RESET_* value. UNSUPPORTED if the context has no reset notification.
     */
    virtual TesseraResult queryResetStatus(int32_t& /*out*/) noexcept {
        return TESSERA_ERROR_UNSUPPORTED;
    }

    /* Fills `out` (kind and handles; structSize is handled by the caller). Borrowed objects, see tessera.h.
     */
    virtual TesseraResult nativeHandles(TesseraNativeHandles& /*out*/) const noexcept {
        return TESSERA_ERROR_UNSUPPORTED;
    }

    ContextAffinity affinity;

    // ---- bookkeeping owned by the core (set by Runtime right after creation; backends leave it alone)
    // -------
    TesseraContextConfig request = {}; // what this context was created from, for tesseraRecreateContext
    uint64_t shareGroup = 0;           // contexts sharing objects have the same group id
    std::atomic<int32_t> requestedInterval{TESSERA_SWAP_UNKNOWN}; // last successful tesseraSetSwapInterval
    std::atomic<bool> lost{false};
    std::atomic<int32_t> lostStatus{TESSERA_RESET_NONE};
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
    /* Can this backend honour TesseraContextConfig::adapterLuid / gpuPreference? Backends that cannot are
     * skipped for such a request (so it fails with UNSUPPORTED instead of silently using whatever GPU the
     * backend picks). */
    virtual bool supportsAdapterSelection() const noexcept {
        return false;
    }
    /* Renders into a caller-owned window; see tesseraCreateContextForWindow. UNSUPPORTED by default (without
     * side effects). */
    virtual TesseraResult createContextForWindow(const TesseraContextConfig& /*config*/,
                                                 uint64_t /*nativeWindow*/, uint32_t /*flags*/,
                                                 const std::shared_ptr<ContextObject>& /*shareWith*/,
                                                 std::shared_ptr<ContextObject>& /*out*/) {
        return TESSERA_ERROR_UNSUPPORTED;
    }
    /* Renders into a caller-owned window of any kind (see tesseraCreateContextForNativeWindow). The default
     * serves the kinds the integer form already covers (Win32 and X11) through createContextForWindow;
     * anything else is UNSUPPORTED. */
    virtual TesseraResult createContextForNativeWindow(const TesseraContextConfig& config,
                                                       const TesseraNativeWindow& window, uint32_t flags,
                                                       const std::shared_ptr<ContextObject>& shareWith,
                                                       std::shared_ptr<ContextObject>& out) {
        if (window.kind == nativeWindowKind())
            return createContextForWindow(config, window.window, flags, shareWith, out);
        return fail(TESSERA_ERROR_UNSUPPORTED, "this backend cannot attach to that kind of window");
    }
    /* The TESSERA_WINDOW_* kind whose handle createContextForWindow takes (0 = none). */
    virtual uint32_t nativeWindowKind() const noexcept {
        return 0;
    }
    /* Null if unknown. May be called with or without a context current. */
    virtual void* getProcAddress(const char* /*name*/) noexcept {
        return nullptr;
    }
};

} // namespace tessera
