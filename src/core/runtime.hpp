#pragma once

#include <tessera/tessera.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "backend.hpp"
#include "handle_table.hpp"

namespace tessera {

/*
 * Process-wide state, created on first use and intentionally never destroyed (so unloading the library
 * or process exit cannot run destructors against half-torn-down drivers). init()/terminate() are
 * reference counted; real teardown happens when the last reference is released.
 */
class Runtime {
  public:
    static Runtime& instance();

    TesseraResult init();
    TesseraResult terminate();

    /* Backends are consulted in registration order. Builtin platform backends are registered by init();
     * all backends are dropped when the last reference is released. */
    void registerBackend(std::shared_ptr<Backend> backend);

    TesseraResult createContext(const TesseraContextConfig& config, TesseraContext& out);
    TesseraResult destroyContext(TesseraContext handle);
    TesseraResult describeContext(TesseraContext handle, TesseraContextInfo& out);
    TesseraResult makeCurrent(TesseraContext handle);
    TesseraContext currentContext();
    TesseraResult swapBuffers(TesseraContext handle);
    TesseraResult setSwapInterval(TesseraContext handle, int32_t interval);
    void* getProcAddress(const char* name);
    TesseraResult getResetStatus(TesseraContext handle, int32_t& outStatus);
    TesseraResult recreateContext(TesseraContext handle, TesseraContext& out, uint32_t& outFlags);
    TesseraResult getNativeHandles(TesseraContext handle, TesseraNativeHandles& out);
    TesseraResult createContextForWindow(const TesseraContextConfig& config, uint64_t nativeWindow, uint32_t flags, TesseraContext& out);
    TesseraResult prewarm(const TesseraContextConfig& config, int32_t count);
    std::size_t pooledContexts() const;

    /* Called from a thread_local destructor on an exiting thread; see ThreadExitGuard. */
    void releaseAtThreadExit(TesseraContext handle, const std::shared_ptr<ThreadToken>& thread) noexcept;

    /* Test hook: init() will not register the platform backends (so tests can inject fakes). */
    static void disableBuiltinBackendsForTesting() noexcept;

    int refCount() const;
    std::size_t liveContexts() const { return contexts_.size(); }
    /* Contexts destroyed while current on another thread, whose native teardown is still waiting for that thread. */
    std::size_t pendingDestructions() const;

  private:
    Runtime() : contexts_(kContextTag, kMaxContexts) {}

    // A destroyed context may still be current on another thread. Its handle is dead for everyone, but the object
    // is kept here until the owning thread releases it (only that thread can), then dropped on that thread.
    std::shared_ptr<ContextObject> lookupAny(TesseraContext handle);
    std::shared_ptr<ContextObject> takeZombie(TesseraContext handle);

    /* The creation path shared by create / create-for-window / recreate: pick a backend that can serve the request, then register. */
    using Attempt = std::function<TesseraResult(Backend&, const std::shared_ptr<ContextObject>& share, std::shared_ptr<ContextObject>& out)>;
    TesseraResult createWith(const TesseraContextConfig& config, bool usePool, const Attempt& attempt, TesseraContext& out);
    TesseraResult createNative(const TesseraContextConfig& config, const std::shared_ptr<ContextObject>& share, const Attempt& attempt,
                               std::shared_ptr<ContextObject>& created);
    TesseraResult registerContext(const TesseraContextConfig& config, const std::shared_ptr<ContextObject>& share,
                                  const std::shared_ptr<ContextObject>& created, TesseraContext& out);
    std::shared_ptr<ContextObject> takeFromPool(const TesseraContextConfig& config);

    void joinGroup(uint64_t group, TesseraHandle handle);
    void leaveGroup(uint64_t group, TesseraHandle handle);
    void markLost(ContextObject& object, int32_t status);

    static constexpr uint8_t kContextTag = 0x43; // 'C'
    static constexpr uint32_t kMaxContexts = 1u << 20;

    mutable std::mutex lifecycle_;
    int refCount_ = 0;
    std::vector<std::shared_ptr<Backend>> backends_; // guarded by lifecycle_
    HandleTable<ContextObject> contexts_;

    std::mutex groupsMutex_; // never held while calling into contexts_ other than get()
    std::map<uint64_t, std::set<TesseraHandle>> groups_;
    uint64_t nextGroup_ = 1;

    struct PoolEntry {
        TesseraContextConfig request;
        std::shared_ptr<ContextObject> context;
    };
    mutable std::mutex poolMutex_;
    std::vector<PoolEntry> pool_; // contexts that have never been used, ready to hand out

    mutable std::mutex zombiesMutex_;
    std::map<TesseraHandle, std::shared_ptr<ContextObject>> zombies_;

    std::atomic<std::size_t> warnAt_{64}; // next live-context count at which to warn
};

/* Implemented in src/platform/builtin.cpp: registers whichever backends this build contains. */
void registerBuiltinBackends(std::vector<std::shared_ptr<Backend>>& out);

} // namespace tessera
