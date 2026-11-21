// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "runtime.hpp"

#include "env.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "boundary.hpp"
#include "config.hpp"
#include "log.hpp"
#include "platform/common/version_negotiation.hpp"
#include "thread_affinity.hpp"

namespace tessera {

namespace {
// The Tessera context this thread last made current. Validated through the handle table (or the
// pending-destruction list) before use, so a destroyed context or a terminated runtime simply reads as
// "none".
thread_local TesseraContext t_current = TESSERA_NULL_HANDLE;
std::atomic<bool> g_builtinDisabled{false};

// Releases the thread's current context when the thread exits. WGL drivers do this themselves, but GLX does
// not: a context left current on an exited thread stays "busy" and cannot be bound anywhere else (BadAccess).
// Runs as a thread_local destructor, i.e. on the exiting thread and before the C runtime tears down
// pthread-key based driver state. It keeps its own token so it never touches other thread_locals.
struct ThreadExitGuard {
    std::shared_ptr<ThreadToken> token;
    ~ThreadExitGuard() {
        if (token && t_current != TESSERA_NULL_HANDLE)
            Runtime::instance().releaseAtThreadExit(t_current, token);
    }
};
thread_local ThreadExitGuard t_exitGuard;
} // namespace

// ---- pending destruction ("zombies") ---------------------------------------------------------------

std::shared_ptr<ContextObject> Runtime::lookupAny(TesseraContext handle) {
    if (auto object = contexts_.get(handle))
        return object;
    std::lock_guard<std::mutex> lock(zombiesMutex_);
    const auto it = zombies_.find(handle);
    return it == zombies_.end() ? nullptr : it->second;
}

std::shared_ptr<ContextObject> Runtime::takeZombie(TesseraContext handle) {
    std::lock_guard<std::mutex> lock(zombiesMutex_);
    const auto it = zombies_.find(handle);
    if (it == zombies_.end())
        return nullptr;
    std::shared_ptr<ContextObject> object = std::move(it->second);
    zombies_.erase(it);
    return object; // the caller lets this die outside every lock, on the thread that owned the context
}

std::size_t Runtime::pendingDestructions() const {
    std::lock_guard<std::mutex> lock(zombiesMutex_);
    return zombies_.size();
}

void Runtime::releaseAtThreadExit(TesseraContext handle, const std::shared_ptr<ThreadToken>& me) noexcept {
    try {
        if (auto object = lookupAny(handle)) {
            if (object->affinity.ownedBy(me)) {
                object->unbindCurrent();
                object->affinity.release(me);
            }
        }
        takeZombie(handle); // a context destroyed elsewhere while we held it is finally torn down, here
    } catch (...) {
    }
}

// ---- share groups -----------------------------------------------------------------------------------

void Runtime::joinGroup(uint64_t group, TesseraHandle handle) {
    std::lock_guard<std::mutex> lock(groupsMutex_);
    groups_[group].insert(handle);
}

void Runtime::leaveGroup(uint64_t group, TesseraHandle handle) {
    std::lock_guard<std::mutex> lock(groupsMutex_);
    const auto it = groups_.find(group);
    if (it == groups_.end())
        return;
    it->second.erase(handle);
    if (it->second.empty())
        groups_.erase(it);
}

void Runtime::setContextLostCallback(TesseraContextLostCallback callback, void* userData) {
    std::lock_guard<std::mutex> lock(lostCallbackMutex_);
    lostCallback_ = callback;
    lostCallbackUserData_ = userData;
}

void Runtime::markLost(TesseraContext handle, ContextObject& object, int32_t status) {
    bool expected = false;
    if (!object.lost.compare_exchange_strong(expected, true))
        return; // somebody already reported this loss: the callback runs once per context
    object.lostStatus.store(status);
    logf(TESSERA_LOG_WARN, "a context was lost (reset status %d); recreate it with tesseraRecreateContext",
         status);
    TesseraContextLostCallback callback;
    void* userData;
    {
        std::lock_guard<std::mutex> lock(lostCallbackMutex_);
        callback = lostCallback_;
        userData = lostCallbackUserData_;
    }
    if (callback)
        callback(handle, status, userData); // no lock held, so it may recreate or destroy contexts
}

TesseraResult Runtime::isContextLost(TesseraContext handle, int32_t& outLost, int32_t& outStatus) {
    outLost = 0;
    outStatus = TESSERA_RESET_NONE;
    auto object = contexts_.get(handle);
    if (!object)
        return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    if (object->lost.load()) {
        const int32_t status = object->lostStatus.load();
        outLost = 1;
        outStatus = status != TESSERA_RESET_NONE ? status : TESSERA_RESET_UNKNOWN;
    }
    return TESSERA_SUCCESS;
}

// ---- lifecycle --------------------------------------------------------------------------------------

Runtime& Runtime::instance() {
    static Runtime* rt = new Runtime(); // leaked on purpose, see class comment
    return *rt;
}

void Runtime::disableBuiltinBackendsForTesting() noexcept {
    g_builtinDisabled.store(true);
}

int Runtime::refCount() const {
    std::lock_guard<std::mutex> lock(lifecycle_);
    return refCount_;
}

TesseraResult Runtime::init() {
    std::lock_guard<std::mutex> lock(lifecycle_);
    if (refCount_ == 0) {
        std::vector<std::shared_ptr<Backend>> builtin;
        if (!g_builtinDisabled.load())
            registerBuiltinBackends(builtin); // may throw; nothing changed yet
        backends_ = std::move(builtin);
        contexts_.open();
        warnAt_.store(64);
        logf(TESSERA_LOG_DEBUG, "tessera initialised (%zu backend(s))", backends_.size());
    }
    ++refCount_;
    return TESSERA_SUCCESS;
}

TesseraResult Runtime::terminate() {
    std::vector<std::shared_ptr<ContextObject>> leftover;
    std::map<TesseraHandle, std::shared_ptr<ContextObject>> pending;
    std::vector<std::shared_ptr<Backend>> backends;
    {
        std::lock_guard<std::mutex> lock(lifecycle_);
        if (refCount_ == 0)
            return fail(TESSERA_ERROR_NOT_INITIALIZED, "tesseraTerminate without tesseraInit");
        if (--refCount_ > 0)
            return TESSERA_SUCCESS;
        leftover = contexts_.closeAndDrain();
        backends.swap(backends_);
    }
    VersionMemory::instance()
        .clear(); // what worked on a driver is a fact about that run, not about the next one (ADR 0016)
    {
        std::lock_guard<std::mutex> lock(zombiesMutex_);
        pending.swap(zombies_);
    }
    {
        std::lock_guard<std::mutex> lock(groupsMutex_);
        groups_.clear();
    }
    std::vector<PoolEntry> pooled;
    {
        std::lock_guard<std::mutex> lock(poolMutex_);
        pooled.swap(pool_);
    }
    if (!pooled.empty())
        logf(TESSERA_LOG_DEBUG, "tesseraTerminate: releasing %zu pre-warmed context(s)", pooled.size());
    // Destroy outside the lock: native destructors may call into drivers (and log callbacks).
    if (!leftover.empty())
        logf(TESSERA_LOG_WARN, "tesseraTerminate: destroying %zu context(s) that were never destroyed",
             leftover.size());
    if (!pending.empty())
        logf(TESSERA_LOG_WARN,
             "tesseraTerminate: %zu destroyed context(s) were still current on other threads; their native "
             "teardown "
             "happens when the last reference goes away",
             pending.size());
    const auto me = currentThreadToken();
    for (auto& object : leftover) {
        if (object->affinity.ownedBy(me))
            object->unbindCurrent();
    }
    for (auto& entry : pending) {
        if (entry.second->affinity.ownedBy(me))
            entry.second->unbindCurrent();
    }
    leftover.clear();
    pending.clear();
    pooled.clear();
    backends.clear();
    t_current = TESSERA_NULL_HANDLE;
    return TESSERA_SUCCESS;
}

void Runtime::registerBackend(std::shared_ptr<Backend> backend) {
    std::lock_guard<std::mutex> lock(lifecycle_);
    backends_.push_back(std::move(backend));
}

// ---- contexts ---------------------------------------------------------------------------------------

TesseraResult Runtime::createNative(const TesseraContextConfig& config,
                                    const std::shared_ptr<ContextObject>& share, const Attempt& attempt,
                                    std::shared_ptr<ContextObject>& created) {
    std::vector<std::shared_ptr<Backend>> backends;
    {
        std::lock_guard<std::mutex> lock(lifecycle_);
        backends = backends_;
    }

    TesseraResult result = TESSERA_ERROR_UNSUPPORTED;
    created.reset();
    const bool adapter = config.adapterLuid != 0 || config.gpuPreference != TESSERA_GPU_DEFAULT;
    for (const auto& backend : backends) {
        if (adapter && !backend->supportsAdapterSelection())
            continue; // never serve a GPU request from a backend that cannot honour it
        result = attempt(*backend, share, created);
        if (result != TESSERA_ERROR_UNSUPPORTED)
            break;
    }
    if (adapter && result == TESSERA_ERROR_UNSUPPORTED && !created)
        return fail(
            TESSERA_ERROR_UNSUPPORTED,
            "choosing a GPU through the configuration is only implemented for WGL (Windows); on EGL use "
            "TESSERA_EGL_DEVICE");
    if (result == TESSERA_ERROR_UNSUPPORTED && backends.empty())
        return fail(TESSERA_ERROR_UNSUPPORTED, "no OpenGL backend is available in this build/platform");
    if (result != TESSERA_SUCCESS)
        return result;
    if (!created)
        return fail(TESSERA_ERROR_UNKNOWN, "backend reported success without a context");
    return TESSERA_SUCCESS;
}

TesseraResult Runtime::registerContext(const TesseraContextConfig& config,
                                       const std::shared_ptr<ContextObject>& share,
                                       const std::shared_ptr<ContextObject>& created, TesseraContext& out) {
    // Core bookkeeping, set before the object becomes visible to other threads through the table.
    created->request = config;
    {
        std::lock_guard<std::mutex> lock(groupsMutex_);
        created->shareGroup = share ? share->shareGroup : nextGroup_++;
    }

    const TesseraHandle handle = contexts_.insert(created);
    if (handle == TESSERA_NULL_HANDLE) {
        // Closed by a concurrent terminate, or table full. `created` is released by the caller.
        return contexts_.isOpen() ? fail(TESSERA_ERROR_OUT_OF_MEMORY, "too many contexts")
                                  : fail(TESSERA_ERROR_NOT_INITIALIZED, "tesseraTerminate raced with create");
    }
    joinGroup(created->shareGroup, handle);

    // Each context holds driver resources (and on Windows a window plus GDI/USER handles, of which a process
    // only has ~10000), so a runaway count is worth a warning long before creation starts failing.
    const std::size_t live = contexts_.size();
    std::size_t threshold = warnAt_.load();
    if (live >= threshold && warnAt_.compare_exchange_strong(threshold, threshold * 4))
        logf(TESSERA_LOG_WARN,
             "%zu live contexts: each holds driver and OS resources, check for contexts that are never "
             "destroyed",
             live);

    out = handle;
    return TESSERA_SUCCESS;
}

namespace {
/* Two requests are interchangeable for pooling when every field that shapes the context is equal. */
bool sameRequest(const TesseraContextConfig& a, const TesseraContextConfig& b) {
    return a.api == b.api && a.versionMajor == b.versionMajor && a.versionMinor == b.versionMinor &&
           a.profile == b.profile && a.flags == b.flags && a.surface == b.surface &&
           a.fallback == b.fallback && a.redBits == b.redBits && a.greenBits == b.greenBits &&
           a.blueBits == b.blueBits && a.alphaBits == b.alphaBits && a.depthBits == b.depthBits &&
           a.stencilBits == b.stencilBits && a.samples == b.samples && a.adapterLuid == b.adapterLuid &&
           a.gpuPreference == b.gpuPreference && a.priority == b.priority;
}
constexpr std::size_t kMaxPooled = 64;
} // namespace

std::shared_ptr<ContextObject> Runtime::takeFromPool(const TesseraContextConfig& config) {
    std::lock_guard<std::mutex> lock(poolMutex_);
    for (auto it = pool_.begin(); it != pool_.end(); ++it) {
        if (sameRequest(it->request, config)) {
            std::shared_ptr<ContextObject> taken = std::move(it->context);
            pool_.erase(it);
            return taken;
        }
    }
    return nullptr;
}

std::size_t Runtime::pooledContexts() const {
    std::lock_guard<std::mutex> lock(poolMutex_);
    return pool_.size();
}

TesseraResult Runtime::createWith(const TesseraContextConfig& config, bool usePool, const Attempt& attempt,
                                  TesseraContext& out) {
    out = TESSERA_NULL_HANDLE;
    if (!contexts_.isOpen())
        return fail(TESSERA_ERROR_NOT_INITIALIZED, "tesseraInit has not been called");

    std::shared_ptr<ContextObject> share;
    if (config.shareWith != TESSERA_NULL_HANDLE) {
        share = contexts_.get(config.shareWith);
        if (!share)
            return fail(TESSERA_ERROR_INVALID_HANDLE, "shareWith is not a live context");
        if (share->lost.load())
            return fail(TESSERA_ERROR_CONTEXT_LOST,
                        "shareWith is a lost context; share with a surviving member instead");
    }

    std::shared_ptr<ContextObject> created;
    // A pre-warmed context is a never-used one built from the identical request, so it is indistinguishable
    // from a new one.
    if (usePool && !share)
        created = takeFromPool(config);
    if (!created) {
        if (TesseraResult r = createNative(config, share, attempt, created); r != TESSERA_SUCCESS)
            return r;
    }
    return registerContext(config, share, created, out);
}

TesseraResult Runtime::createContext(const TesseraContextConfig& config, TesseraContext& out) {
    return createWith(
        config, true,
        [&](Backend& backend, const std::shared_ptr<ContextObject>& share,
            std::shared_ptr<ContextObject>& created) {
            return backend.createContext(config, share, created);
        },
        out);
}

TesseraResult Runtime::createContextForWindow(const TesseraContextConfig& config, uint64_t nativeWindow,
                                              uint32_t flags, TesseraContext& out) {
    if (nativeWindow == 0) {
        out = TESSERA_NULL_HANDLE;
        return fail(TESSERA_ERROR_INVALID_ARGUMENT, "nativeWindow is 0");
    }
    if (flags & ~TESSERA_ATTACH_ALLOW_PIXEL_FORMAT) {
        out = TESSERA_NULL_HANDLE;
        return fail(TESSERA_ERROR_INVALID_ARGUMENT, "unknown attach flags");
    }
    return createWith(
        config, false,
        [&](Backend& backend, const std::shared_ptr<ContextObject>& share,
            std::shared_ptr<ContextObject>& created) {
            return backend.createContextForWindow(config, nativeWindow, flags, share, created);
        },
        out);
}

std::size_t Runtime::pooledContextsFor(const TesseraContextConfig& config) const {
    std::lock_guard<std::mutex> lock(poolMutex_);
    std::size_t n = 0;
    for (const PoolEntry& e : pool_)
        if (sameRequest(e.request, config))
            ++n;
    return n;
}

std::size_t Runtime::clearPool() {
    std::vector<PoolEntry> dropped;
    {
        std::lock_guard<std::mutex> lock(poolMutex_);
        dropped.swap(pool_);
    }
    return dropped.size(); // the native contexts are destroyed here, outside the lock, on the calling thread
}

TesseraResult Runtime::topUp(const TesseraContextConfig& config, int32_t target, int32_t& created) {
    created = 0;
    if (target < 0)
        return fail(TESSERA_ERROR_INVALID_ARGUMENT, "target is negative");
    const std::size_t have = pooledContextsFor(config);
    if (static_cast<std::size_t>(target) <= have)
        return contexts_.isOpen() ? TESSERA_SUCCESS
                                  : fail(TESSERA_ERROR_NOT_INITIALIZED, "tesseraInit has not been called");
    const int32_t missing = target - static_cast<int32_t>(have);
    const std::size_t before = pooledContexts();
    const TesseraResult r = prewarm(config, missing);
    // A failure part-way leaves what was created in the pool; report it.
    created = static_cast<int32_t>(pooledContexts() - before);
    return r;
}

TesseraResult Runtime::prewarm(const TesseraContextConfig& config, int32_t count) {
    if (!contexts_.isOpen())
        return fail(TESSERA_ERROR_NOT_INITIALIZED, "tesseraInit has not been called");
    if (count < 0)
        return fail(TESSERA_ERROR_INVALID_ARGUMENT, "count is negative");
    if (config.shareWith != TESSERA_NULL_HANDLE)
        return fail(TESSERA_ERROR_INVALID_ARGUMENT,
                    "pre-warmed contexts cannot share with a specific context");
    if (static_cast<std::size_t>(count) > kMaxPooled ||
        pooledContexts() + static_cast<std::size_t>(count) > kMaxPooled)
        return fail(TESSERA_ERROR_INVALID_ARGUMENT, "at most 64 contexts can be pooled in total");

    const auto attempt = [&](Backend& backend, const std::shared_ptr<ContextObject>& share,
                             std::shared_ptr<ContextObject>& created) {
        return backend.createContext(config, share, created);
    };
    for (int32_t i = 0; i < count; ++i) {
        std::shared_ptr<ContextObject> created;
        if (TesseraResult r = createNative(config, nullptr, attempt, created); r != TESSERA_SUCCESS)
            return r;
        std::lock_guard<std::mutex> lock(poolMutex_);
        if (pool_.size() >= kMaxPooled)
            return fail(TESSERA_ERROR_INVALID_ARGUMENT, "at most 64 contexts can be pooled in total");
        pool_.push_back({config, std::move(created)});
    }
    return TESSERA_SUCCESS;
}

TesseraResult Runtime::probe(const TesseraContextConfig& config, TesseraProbeReport& report) {
    report.result = TESSERA_SUCCESS;
    if (!contexts_.isOpen()) {
        report.result = fail(TESSERA_ERROR_NOT_INITIALIZED, "tesseraInit has not been called");
        std::snprintf(report.message, sizeof report.message, "%s", tesseraGetLastErrorMessage());
        return report.result;
    }
    if (config.shareWith != TESSERA_NULL_HANDLE) {
        report.result = fail(TESSERA_ERROR_INVALID_ARGUMENT, "a probe cannot share objects with a context");
        std::snprintf(report.message, sizeof report.message, "%s", tesseraGetLastErrorMessage());
        return report.result;
    }
    std::vector<std::shared_ptr<Backend>> backends;
    {
        std::lock_guard<std::mutex> lock(lifecycle_);
        backends = backends_;
    }
    TesseraResult result = TESSERA_ERROR_UNSUPPORTED;
    std::shared_ptr<ContextObject> created;
    const bool adapter = config.adapterLuid != 0 || config.gpuPreference != TESSERA_GPU_DEFAULT;
    for (const auto& backend : backends) {
        if (adapter && !backend->supportsAdapterSelection())
            continue;
        std::snprintf(report.backend, sizeof report.backend, "%s", backend->name());
        created.reset();
        result = backend->createContext(config, nullptr, created);
        if (result != TESSERA_ERROR_UNSUPPORTED)
            break;
    }
    if (result == TESSERA_SUCCESS && !created)
        result = fail(TESSERA_ERROR_UNKNOWN, "backend reported success without a context");
    if (result == TESSERA_SUCCESS) {
        TesseraContextInfo info;
        std::memset(&info, 0, sizeof info);
        info.structSize = static_cast<uint32_t>(sizeof info);
        result = created->describe(info);
        if (result == TESSERA_SUCCESS)
            report.info = info;
    }
    created.reset(); // native teardown happens here, on this thread, while nothing of ours is current
    report.result = result;
    if (result == TESSERA_SUCCESS)
        report.message[0] = '\0';
    else if (backends.empty())
        std::snprintf(report.message, sizeof report.message, "%s",
                      "no OpenGL backend is available in this build/platform");
    else
        std::snprintf(report.message, sizeof report.message, "%s", tesseraGetLastErrorMessage());
    return result;
}

TesseraResult Runtime::getNativeHandles(TesseraContext handle, TesseraNativeHandles& out) {
    std::shared_ptr<ContextObject> object = contexts_.get(handle);
    if (!object)
        return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    out = tesseraDefaultNativeHandles();
    return object->nativeHandles(out);
}

TesseraResult Runtime::destroyContext(TesseraContext handle) {
    const auto me = currentThreadToken();
    std::shared_ptr<ContextObject> object = contexts_.get(handle);
    if (!object)
        return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");

    if (object->affinity.busyElsewhere(me)) {
        // Current on another live thread, which is the only thread that can release it natively. Kill the
        // handle now and let that thread finish the job when it lets go of the context (or exits).
        std::shared_ptr<ContextObject> removed = contexts_.remove(handle);
        if (!removed)
            return fail(TESSERA_ERROR_INVALID_HANDLE, "context destroyed concurrently");
        leaveGroup(removed->shareGroup, handle);
        {
            std::lock_guard<std::mutex> lock(zombiesMutex_);
            zombies_[handle] = removed;
        }
        // The owner may have released it between our check and the insert above; it could not have found the
        // zombie then, so nobody else will finish this. Re-check, and finish it here if the context is free
        // after all.
        if (!removed->affinity.busyElsewhere(me))
            takeZombie(handle);
        return TESSERA_SUCCESS;
    }

    if (object->affinity.ownedBy(me)) {
        object->unbindCurrent();
        object->affinity.release(me);
    }
    if (t_current == handle)
        t_current = TESSERA_NULL_HANDLE;
    object = contexts_.remove(handle);
    // `object` dies here, outside every lock (later, if another thread still holds a reference).
    if (!object)
        return fail(TESSERA_ERROR_INVALID_HANDLE, "context destroyed concurrently");
    leaveGroup(object->shareGroup, handle);
    return TESSERA_SUCCESS;
}

TesseraResult Runtime::describeContext(TesseraContext handle, TesseraContextInfo& out) {
    std::shared_ptr<ContextObject> object = contexts_.get(handle);
    if (!object)
        return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    return object->describe(out);
}

TesseraResult Runtime::makeCurrent(TesseraContext handle) {
    const auto me = currentThreadToken();

    if (handle == TESSERA_NULL_HANDLE) {
        if (t_current != TESSERA_NULL_HANDLE) {
            const TesseraContext previous = t_current;
            if (auto prev = lookupAny(previous)) {
                if (prev->affinity.ownedBy(me)) {
                    prev->unbindCurrent(); // no-op if a foreign context has since replaced it
                    prev->affinity.release(me);
                }
            }
            takeZombie(previous); // if it was destroyed elsewhere meanwhile, this is where it finally dies
            t_current = TESSERA_NULL_HANDLE;
        }
        return TESSERA_SUCCESS;
    }

    std::shared_ptr<ContextObject> object = contexts_.get(handle);
    if (!object)
        return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    if (object->lost.load())
        return fail(TESSERA_ERROR_CONTEXT_LOST, "the context was lost; recreate it");

    bool wasNew = false;
    if (!object->affinity.tryAcquire(me, &wasNew))
        return fail(TESSERA_ERROR_WRONG_THREAD, "context is current on another thread");

    const TesseraResult r = object->bindCurrent();
    if (r != TESSERA_SUCCESS) {
        if (wasNew)
            object->affinity.release(me);
        if (r == TESSERA_ERROR_CONTEXT_LOST)
            markLost(handle, *object, TESSERA_RESET_UNKNOWN);
        return r;
    }

    // Switching away from another of our contexts: WGL/GLX replace it at the driver, but EGL keeps one
    // current context per client API, so release it explicitly (a no-op where the driver already did).
    if (t_current != TESSERA_NULL_HANDLE && t_current != handle) {
        const TesseraContext previous = t_current;
        if (auto prev = lookupAny(previous)) {
            if (prev->affinity.ownedBy(me))
                prev->unbindCurrent();
            prev->affinity.release(me);
        }
        takeZombie(previous);
    }
    t_current = handle;
    t_exitGuard.token = me; // arms the release-at-thread-exit above
    return TESSERA_SUCCESS;
}

TesseraContext Runtime::currentContext() {
    if (t_current == TESSERA_NULL_HANDLE)
        return TESSERA_NULL_HANDLE;
    const auto me = currentThreadToken();
    auto object = contexts_.get(t_current);
    if (object) {
        if (object->affinity.ownedBy(me))
            return t_current;
        t_current = TESSERA_NULL_HANDLE;
        return TESSERA_NULL_HANDLE;
    }
    // Destroyed elsewhere while current here: the handle is dead to the caller, but keep it so the next
    // release or the thread's exit can finish the native teardown. Anything else is stale.
    if (!lookupAny(t_current))
        t_current = TESSERA_NULL_HANDLE;
    return TESSERA_NULL_HANDLE;
}

TesseraResult Runtime::swapBuffers(TesseraContext handle) {
    auto object = contexts_.get(handle);
    if (!object)
        return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    if (object->lost.load())
        return fail(TESSERA_ERROR_CONTEXT_LOST, "the context was lost; recreate it");
    if (!object->affinity.ownedBy(currentThreadToken()))
        return fail(TESSERA_ERROR_WRONG_THREAD, "context is not current on the calling thread");
#ifdef TESSERA_TEST_HOOKS
    // Test-only (the `java` preset enables it, a release never does): lets a test make "the driver" report a
    // lost context at the next swap, so context-lost events can be followed through the real JNI and FFM
    // upcalls without a GPU reset.
    if (std::string lose; readEnv("TESSERA_TEST_LOSE_ON_SWAP", lose) && lose == "1") {
        markLost(handle, *object, TESSERA_RESET_GUILTY);
        return fail(TESSERA_ERROR_CONTEXT_LOST, "test hook: the context was lost at swap");
    }
#endif
    const TesseraResult r = object->swapBuffers();
    if (r == TESSERA_ERROR_CONTEXT_LOST)
        markLost(handle, *object, TESSERA_RESET_UNKNOWN);
    return r;
}

TesseraResult Runtime::setSwapInterval(TesseraContext handle, int32_t interval) {
    auto object = contexts_.get(handle);
    if (!object)
        return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    if (object->lost.load())
        return fail(TESSERA_ERROR_CONTEXT_LOST, "the context was lost; recreate it");
    if (!object->affinity.ownedBy(currentThreadToken()))
        return fail(TESSERA_ERROR_WRONG_THREAD, "context is not current on the calling thread");
    const TesseraResult r = object->setSwapInterval(interval);
    if (r == TESSERA_SUCCESS)
        object->requestedInterval.store(interval);
    return r;
}

TesseraResult Runtime::createContextForNativeWindow(const TesseraContextConfig& config,
                                                    const TesseraNativeWindow& window, uint32_t flags,
                                                    TesseraContext& out) {
    out = TESSERA_NULL_HANDLE;
    if (window.window == 0)
        return fail(TESSERA_ERROR_INVALID_ARGUMENT, "window is 0");
    if (flags & ~TESSERA_ATTACH_ALLOW_PIXEL_FORMAT)
        return fail(TESSERA_ERROR_INVALID_ARGUMENT, "unknown attach flags");
    switch (window.kind) {
    case TESSERA_WINDOW_WIN32:
    case TESSERA_WINDOW_X11:
        break;
    case TESSERA_WINDOW_WAYLAND:
        if (window.display == 0)
            return fail(TESSERA_ERROR_INVALID_ARGUMENT, "a Wayland window needs its wl_display");
        if (window.width <= 0 || window.height <= 0 || window.width > 65536 || window.height > 65536)
            return fail(TESSERA_ERROR_INVALID_ARGUMENT, "a Wayland window needs a buffer size in 1..65536");
        break;
    case TESSERA_WINDOW_COCOA:
        return fail(TESSERA_ERROR_UNSUPPORTED, "attaching to an NSView is not implemented (docs/adr/0014)");
    default:
        return fail(TESSERA_ERROR_INVALID_ARGUMENT, "unknown window kind");
    }
    return createWith(
        config, false,
        [&](Backend& backend, const std::shared_ptr<ContextObject>& share,
            std::shared_ptr<ContextObject>& created) {
            return backend.createContextForNativeWindow(config, window, flags, share, created);
        },
        out);
}

TesseraResult Runtime::resizeWindow(TesseraContext handle, int32_t width, int32_t height) {
    auto object = contexts_.get(handle);
    if (!object)
        return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    if (width <= 0 || height <= 0 || width > 65536 || height > 65536)
        return fail(TESSERA_ERROR_INVALID_ARGUMENT, "the size must be in 1..65536");
    if (object->lost.load())
        return fail(TESSERA_ERROR_CONTEXT_LOST, "the context was lost; recreate it");
    if (!object->affinity.ownedBy(currentThreadToken()))
        return fail(TESSERA_ERROR_WRONG_THREAD, "context is not current on the calling thread");
    return object->resizeWindow(width, height);
}

TesseraResult Runtime::getSwapControl(TesseraContext handle, TesseraSwapControl& out) {
    auto object = contexts_.get(handle);
    if (!object)
        return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    if (object->lost.load())
        return fail(TESSERA_ERROR_CONTEXT_LOST, "the context was lost; recreate it");
    if (!object->affinity.ownedBy(currentThreadToken()))
        return fail(TESSERA_ERROR_WRONG_THREAD, "context is not current on the calling thread");
    out = tesseraDefaultSwapControl();
    const TesseraResult r = object->getSwapControl(out);
    if (r == TESSERA_ERROR_CONTEXT_LOST)
        markLost(handle, *object, TESSERA_RESET_UNKNOWN);
    if (r != TESSERA_SUCCESS)
        return r;
    out.requested = object->requestedInterval.load();
    if (!(out.caps & TESSERA_SWAP_CAP_READBACK))
        out.applied = TESSERA_SWAP_UNKNOWN; // a backend must not claim a value it did not read
    return TESSERA_SUCCESS;
}

void* Runtime::getProcAddress(const char* name) {
    if (!name || !contexts_.isOpen())
        return nullptr;
    std::vector<std::shared_ptr<Backend>> backends;
    {
        std::lock_guard<std::mutex> lock(lifecycle_);
        backends = backends_;
    }
    for (const auto& backend : backends)
        if (void* p = backend->getProcAddress(name))
            return p;
    return nullptr;
}

// ---- reset notification, loss and recreation ---------------------------------------------------------

TesseraResult Runtime::getResetStatus(TesseraContext handle, int32_t& outStatus) {
    outStatus = TESSERA_RESET_NONE;
    auto object = contexts_.get(handle);
    if (!object)
        return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");
    if (!object->affinity.ownedBy(currentThreadToken()))
        return fail(TESSERA_ERROR_WRONG_THREAD, "context is not current on the calling thread");
    if (object->lost.load()) {
        const int32_t status = object->lostStatus.load();
        outStatus = status != TESSERA_RESET_NONE ? status : TESSERA_RESET_UNKNOWN;
        return TESSERA_SUCCESS;
    }
    int32_t status = TESSERA_RESET_NONE;
    if (TesseraResult r = object->queryResetStatus(status); r != TESSERA_SUCCESS)
        return r;
    if (status != TESSERA_RESET_NONE)
        markLost(handle, *object, status);
    outStatus = status;
    return TESSERA_SUCCESS;
}

TesseraResult Runtime::recreateContext(TesseraContext handle, TesseraContext& out, uint32_t& outFlags) {
    out = TESSERA_NULL_HANDLE;
    outFlags = 0;
    auto object = contexts_.get(handle);
    if (!object)
        return fail(TESSERA_ERROR_INVALID_HANDLE, "not a live context handle");

    TesseraContextConfig config = object->request;
    config.shareWith = TESSERA_NULL_HANDLE;

    // Find a surviving member of the old share group to join, so the group's objects are still there.
    TesseraHandle survivor = TESSERA_NULL_HANDLE;
    {
        std::vector<TesseraHandle> members;
        {
            std::lock_guard<std::mutex> lock(groupsMutex_);
            const auto it = groups_.find(object->shareGroup);
            if (it != groups_.end())
                members.assign(it->second.begin(), it->second.end());
        }
        for (TesseraHandle member : members) {
            if (member == handle)
                continue;
            const auto other = contexts_.get(member);
            if (other && !other->lost.load()) {
                survivor = member;
                break;
            }
        }
    }
    config.shareWith = survivor;

    if (TesseraResult r = createContext(config, out); r != TESSERA_SUCCESS)
        return r;
    if (survivor != TESSERA_NULL_HANDLE)
        outFlags |= TESSERA_RECREATE_SHARE_PRESERVED;
    return TESSERA_SUCCESS;
}

} // namespace tessera
