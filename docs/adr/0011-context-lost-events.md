# ADR 0011: Context-lost events instead of a polling loop
Status: accepted (ABI 0.9.0).

**Problem.** ADR 0008 made loss permanent and made polling the application's job. Code that owns many contexts (a pool, several windows, a share group) ends up wrapping every `tesseraGetResetStatus` / `tesseraMakeCurrent` / `tesseraSwapBuffers` call in the same "did that mean the context is lost? then recreate" block, and a loss that a backend reports natively (`EGL_CONTEXT_LOST` at make-current) is easy to miss because it is only visible as an error code at one call site.

**Decision.** Two additions, no new threads, no change to when the driver is queried:

- `tesseraSetContextLostCallback(callback, userData)`: process-wide, like the log callback. It runs **once per context**, at the moment the core marks the context lost (the existing `markLost`, whose compare-and-swap is what makes it once), on the thread that detected the loss, with no Tessera lock held. Because it runs after the context is marked and with no lock, it may call `tesseraRecreateContext`, `tesseraDestroyContext` and `tesseraIsContextLost`. It may not call `tesseraTerminate`, and must not make the lost context current (that would just fail with `CONTEXT_LOST`).
- `tesseraIsContextLost(context, &lost, &status)`: reads the recorded state, never queries the driver, any thread, the context need not be current.

**What was deliberately not done.**
- *No automatic recreation inside the library.* A recreated context has a new handle, so the library could not make the old handle "become" the new one, and the application must re-upload when the share group did not survive (`TESSERA_RECREATE_SHARE_PRESERVED`). The "opt-in helper" is therefore the callback plus the existing `tesseraRecreateContext`; the Java layer documents the three-line listener. The callback test does exactly this from inside the callback.
- *No background polling thread* (principle 4 and ADR 0008): detection still happens only inside calls the application makes.
- *No detection without robustness.* A context created without `ROBUST_ACCESS | RESET_NOTIFICATION` cannot learn of a reset; it is reported only if a backend call returns `CONTEXT_LOST`. Unchanged from ADR 0008 (Windows TDR still has no reliable signal in WGL/GLX).

**Java.** `Tessera.setContextLostListener(ContextLostListener)`, `Context.isLost()`, `Context.lostStatus()`. The listener receives the `Context` object; the binding holds contexts weakly, so a context that was already collected is not reported (it is unreachable, and its destructor frees the native context). Delivery is a JNI upcall (`onContextLost`, attaching foreign threads as daemons) or an FFM upcall stub; neither lets an exception escape.

**Verification.** Native: fake backend with injected resets (`tests/test_core.cpp`, `testContextLostCallback`): once per context, each status passed through, native-loss path reports `UNKNOWN`, later calls on a lost context do not fire again, healthy polls do not fire, the callback recreating the context from inside itself, disabling, and the query's argument handling; mutation-checked (suppressing the callback fails it). Java: installing and removing the listener, `isLost` on healthy and closed contexts, on both bindings. **Not verified:** event delivery through the JNI/FFM upcalls end to end (a real loss cannot be provoked on the hardware here, and the Java layer has no fake backend), and a real GPU reset.
