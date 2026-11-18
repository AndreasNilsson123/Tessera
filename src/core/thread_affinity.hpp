#pragma once

#include <atomic>
#include <memory>
#include <mutex>

namespace tessera {

/*
 * Identity of a thread that survives thread-id reuse and tells us when the thread has exited.
 * A context "current" on a thread that has since died would otherwise be stuck forever (drivers release
 * a dying thread's current context behind our back), so ownership held by a dead token is reclaimable.
 */
struct ThreadToken {
    std::atomic<bool> alive{true};
};

/* Token of the calling thread; marked dead by a thread_local destructor when the thread exits. */
std::shared_ptr<ThreadToken> currentThreadToken();

/* Which thread (if any) a context is current on. Thread-safe. */
class ContextAffinity {
  public:
    /* True if `me` now owns it (newly or already). False if another live thread owns it.
     * `wasNew` (optional) reports whether ownership changed. */
    bool tryAcquire(const std::shared_ptr<ThreadToken>& me, bool* wasNew = nullptr) {
        std::lock_guard<std::mutex> lock(m_);
        if (owner_ == me) {
            if (wasNew)
                *wasNew = false;
            return true;
        }
        if (owner_ && owner_->alive.load())
            return false;
        owner_ = me;
        if (wasNew)
            *wasNew = true;
        return true;
    }

    void release(const std::shared_ptr<ThreadToken>& me) {
        std::lock_guard<std::mutex> lock(m_);
        if (owner_ == me)
            owner_.reset();
    }

    bool ownedBy(const std::shared_ptr<ThreadToken>& me) const {
        std::lock_guard<std::mutex> lock(m_);
        return owner_ == me;
    }

    /* Owned by a thread other than `me` that is still alive. */
    bool busyElsewhere(const std::shared_ptr<ThreadToken>& me) const {
        std::lock_guard<std::mutex> lock(m_);
        return owner_ && owner_ != me && owner_->alive.load();
    }

  private:
    mutable std::mutex m_;
    std::shared_ptr<ThreadToken> owner_;
};

} // namespace tessera
