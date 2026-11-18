#pragma once

namespace tessera {

/*
 * RAII guard for the "be a good guest" rule: whatever context/drawable the calling thread had current
 * when we were entered is captured, and put back when the scope ends - on every path, including errors.
 *
 * `Ops` is a platform policy:
 *     struct Ops {
 *         using State = ...;                       // trivially copyable snapshot of native current state
 *         static State capture() noexcept;
 *         static void restore(const State&) noexcept;   // must never throw
 *     };
 * Real policies arrive with each backend (WGL: Phase 2, GLX: 3, EGL: 4, CGL: 5). The logic lives here,
 * independent of any GL library, so it is unit-testable with a fake policy.
 */
template <class Ops> class ScopedForeignState {
  public:
    ScopedForeignState() noexcept : state_(Ops::capture()) {}
    ~ScopedForeignState() {
        if (active_)
            Ops::restore(state_);
    }

    ScopedForeignState(const ScopedForeignState&) = delete;
    ScopedForeignState& operator=(const ScopedForeignState&) = delete;

    /* Skip restoration (e.g. the caller explicitly wants a different context left current). */
    void dismiss() noexcept {
        active_ = false;
    }

  private:
    typename Ops::State state_;
    bool active_ = true;
};

struct NullForeignOps {
    struct State {};
    static State capture() noexcept {
        return {};
    }
    static void restore(const State&) noexcept {}
};

} // namespace tessera
