#include "runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "boundary.hpp"
#include "config.hpp"
#include "log.hpp"
#include "thread_affinity.hpp"

namespace tessera {

namespace {
// The Tessera context this thread last made current. Always validated through the handle table before
// use, so a destroyed context or a terminated runtime simply reads as "none".
thread_local TesseraContext t_current = TESSERA_NULL_HANDLE;
std::atomic<bool> g_builtinDisabled{false};
} // namespace

Runtime& Runtime::instance() {
    static Runtime* rt = new Runtime(); // leaked on purpose, see class comment
    return *rt;
}

void Runtime::disableBuiltinBackendsForTesting() noexcept { g_builtinDisabled.store(true); }

int Runtime::refCount() const {
    std::lock_guard<std::mutex> lock(lifecycle_);
    return refCount_;
}

TesseraResult Runtime::init() {
    std::lock_guard<std::mutex> lock(lifecycle_);
    if (refCount_ == 0) {
        std::vector<std::shared_ptr<Backend>> builtin;
        if (!g_builtinDisabled.load()) registerBuiltinBackends(builtin); // may throw; nothing changed yet
        backends_ = std::move(builtin);
        contexts_.open();
        logf(TESSERA_LOG_DEBUG, "tessera initialised (%zu backend(s))", backends_.size());
    }
    ++refCount_;
    return TESSERA_SUCCESS;
}

TesseraResult Runtime::terminate() {
    std::vector<std::shared_ptr<ContextObject>> leftover;
    std::vector<std::shared_ptr<Backend>> backends;
    {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (refCount_ == 0) return fail(TESSERA_ERROR_NOT_INITIALIZED, "tesseraTerminate without tesseraInit");
        if (--refCount_ > 0) return TESSERA_SUCCESS;
        leftover = contexts_.closeAndDrain();
        backends.swap(backends_);
    }
    // Destroy outside the lock: native destructors may call into drivers (and log callbacks).
    if (!leftover.empty())
        logf(TESSERA_LOG_WARN, "tesseraTerminate: destroying %zu context(s) that were never destroyed",
             leftover.size());
    const auto me = currentThreadToken();
    for (auto& object : leftover) {
        if (object->affinity.ownedBy(me)) object->unbindCurrent();
    }
    leftover.clear();
    backends.clear();
    t_current = TESSERA_NULL_HANDLE;
    return TESSERA_SUCCESS;
}

void Runtime::registerBackend(std::shared_ptr<Backend> backend) {
    std::lock_guard<std::mutex> lock(lifecycle_);
    backends_.push_back(std::move(backend));
}

TesseraResult Runtime::createContext(const TesseraContextConfig& config, TesseraContext& out) {
    out = TESSERA_NULL_HANDLE;
    if (!contexts_.isOpen()) return fail(TESSERA_ERROR_NOT_INITIALIZED, "tesseraInit has not been called");

    std::shared_ptr<ContextObject> share;
    if (config.shareWith != TESSERA_NULL_HANDLE) {
        share = contexts_.get(config.shareWith);
        if (!share) return fail(TESSERA_ERROR_INVALID_HANDLE, "shareWith is not a live context");
    }

    std::vector<std::shared_ptr<Backend>> backends;
    {
        std::lock_guard<std::mutex> lock(lifecycle_);
        backends = backends_;
    }

    TesseraResult result = TESSERA_ERROR_UNSUPPORTED;
    std::shared_ptr<ContextObject> created;
    for (const auto& backend : backends) {
        result = backend->createContext(config, share, created);
        if (result != TESSERA_ERROR_UNSUPPORTED) break;
    }
    if (result == TESSERA_ERROR_UNSUPPORTED && backends.empty())
        return fail(TESSERA_ERROR_UNSUPPORTED, "no OpenGL backend is available in this build/platform");
    if (result != TESSERA_SUCCESS) return result;
    if (!created) return fail(TESSERA_ERROR_UNKNOWN, "backend reported success without a context");

    const TesseraHandle handle = contexts_.insert(created);
    if (handle == TESSERA_NULL_HANDLE) {
        // Closed by a concurrent terminate, or table full. `created` is released on return.
        return contexts_.isOpen() ? fail(TESSERA_ERROR_OUT_OF_MEMORY, "too many contexts")
                                  : fail(TESSERA_ERROR_NOT_INITIALIZED, "tesseraTerminate raced with create");
    }
    out = handle;
    return TESSERA_SUCCESS;
}

TesseraResult Runtime::destroyContext(TesseraContext handle) {
    const auto me = currentThreadToken();
    std::shared_ptr<ContextObject> object = contexts_.get(handle);
    if (!object) return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    if (object->affinity.busyElsewhere(me))
        return fail(TESSERA_ERROR_WRONG_THREAD,
                    "context is current on another thread; release it there before destroying it");
    if (object->affinity.ownedBy(me)) {
        object->unbindCurrent();
        object->affinity.release(me);
    }
    if (t_current == handle) t_current = TESSERA_NULL_HANDLE;
    object = contexts_.remove(handle);
    // `object` dies here, outside every lock (later, if another thread still holds a reference).
    return object ? TESSERA_SUCCESS : fail(TESSERA_ERROR_INVALID_HANDLE, "context destroyed concurrently");
}

TesseraResult Runtime::describeContext(TesseraContext handle, TesseraContextInfo& out) {
    std::shared_ptr<ContextObject> object = contexts_.get(handle);
    if (!object) return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    return object->describe(out);
}

TesseraResult Runtime::makeCurrent(TesseraContext handle) {
    const auto me = currentThreadToken();

    if (handle == TESSERA_NULL_HANDLE) {
        if (t_current != TESSERA_NULL_HANDLE) {
            if (auto prev = contexts_.get(t_current)) {
                if (prev->affinity.ownedBy(me)) {
                    prev->unbindCurrent(); // no-op if a foreign context has since replaced it
                    prev->affinity.release(me);
                }
            }
            t_current = TESSERA_NULL_HANDLE;
        }
        return TESSERA_SUCCESS;
    }

    std::shared_ptr<ContextObject> object = contexts_.get(handle);
    if (!object) return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");

    bool wasNew = false;
    if (!object->affinity.tryAcquire(me, &wasNew))
        return fail(TESSERA_ERROR_WRONG_THREAD, "context is current on another thread");

    const TesseraResult r = object->bindCurrent();
    if (r != TESSERA_SUCCESS) {
        if (wasNew) object->affinity.release(me);
        return r;
    }

    // Binding replaced the thread's previous context at the driver; drop our claim on it.
    if (t_current != TESSERA_NULL_HANDLE && t_current != handle) {
        if (auto prev = contexts_.get(t_current)) prev->affinity.release(me);
    }
    t_current = handle;
    return TESSERA_SUCCESS;
}

TesseraContext Runtime::currentContext() {
    if (t_current == TESSERA_NULL_HANDLE) return TESSERA_NULL_HANDLE;
    const auto me = currentThreadToken();
    auto object = contexts_.get(t_current);
    if (!object || !object->affinity.ownedBy(me)) {
        t_current = TESSERA_NULL_HANDLE;
        return TESSERA_NULL_HANDLE;
    }
    return t_current;
}

TesseraResult Runtime::swapBuffers(TesseraContext handle) {
    auto object = contexts_.get(handle);
    if (!object) return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    return object->swapBuffers();
}

TesseraResult Runtime::setSwapInterval(TesseraContext handle, int32_t interval) {
    auto object = contexts_.get(handle);
    if (!object) return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    if (!object->affinity.ownedBy(currentThreadToken()))
        return fail(TESSERA_ERROR_WRONG_THREAD, "context is not current on the calling thread");
    return object->setSwapInterval(interval);
}

void* Runtime::getProcAddress(const char* name) {
    if (!name || !contexts_.isOpen()) return nullptr;
    std::vector<std::shared_ptr<Backend>> backends;
    {
        std::lock_guard<std::mutex> lock(lifecycle_);
        backends = backends_;
    }
    for (const auto& backend : backends)
        if (void* p = backend->getProcAddress(name)) return p;
    return nullptr;
}

} // namespace tessera
