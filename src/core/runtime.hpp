#pragma once

#include <tessera/tessera.h>

#include <memory>
#include <mutex>
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

    int refCount() const;
    std::size_t liveContexts() const { return contexts_.size(); }

  private:
    Runtime() : contexts_(kContextTag, kMaxContexts) {}

    static constexpr uint8_t kContextTag = 0x43; // 'C'
    static constexpr uint32_t kMaxContexts = 1u << 20;

    mutable std::mutex lifecycle_;
    int refCount_ = 0;
    std::vector<std::shared_ptr<Backend>> backends_; // guarded by lifecycle_
    HandleTable<ContextObject> contexts_;
};

/* Implemented in src/platform/builtin.cpp: registers whichever backends this build contains. */
void registerBuiltinBackends(std::vector<std::shared_ptr<Backend>>& out);

} // namespace tessera
