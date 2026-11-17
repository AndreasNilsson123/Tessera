#include "runtime.hpp"

#include <algorithm>
#include <cstring>

#include "boundary.hpp"
#include "config.hpp"
#include "log.hpp"

namespace tessera {

Runtime& Runtime::instance() {
    static Runtime* rt = new Runtime(); // leaked on purpose, see class comment
    return *rt;
}

int Runtime::refCount() const {
    std::lock_guard<std::mutex> lock(lifecycle_);
    return refCount_;
}

TesseraResult Runtime::init() {
    std::lock_guard<std::mutex> lock(lifecycle_);
    if (refCount_ == 0) {
        std::vector<std::shared_ptr<Backend>> builtin;
        registerBuiltinBackends(builtin); // may throw; nothing has been changed yet
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
    leftover.clear();
    backends.clear();
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
    std::shared_ptr<ContextObject> object = contexts_.remove(handle);
    if (!object) return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    // `object` dies here, outside every lock (unless another thread is still using it).
    return TESSERA_SUCCESS;
}

TesseraResult Runtime::describeContext(TesseraContext handle, TesseraContextInfo& out) {
    std::shared_ptr<ContextObject> object = contexts_.get(handle);
    if (!object) return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    return object->describe(out);
}

} // namespace tessera
