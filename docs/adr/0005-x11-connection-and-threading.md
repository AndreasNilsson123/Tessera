# ADR 0005: One private X connection, XInitThreads, scoped error handlers (Linux/X11)
Status: accepted (Phase 3). Supersedes the Phase 3 sketch in the roadmap that preferred avoiding `XInitThreads`.

**Connection.** The backend opens one private `Display` (never the host's) and uses it for every context. GLX share groups only work between contexts on the same connection, so a connection per context was rejected. Windows are never mapped (`override_redirect`, 1x1), so no window manager is involved and, unlike Win32, X windows have no thread affinity: no helper thread is needed.

**Threading.** A single connection used from arbitrary application threads requires Xlib's locking, which exists only after `XInitThreads()`. Tessera calls it once, before its first `XOpenDisplay`, unless `TESSERA_NO_XINITTHREADS` is set. This is a process-wide side effect, but it is idempotent and is what GLFW, SDL, GTK and AWT already do, so in any process that also hosts a toolkit it has already happened. Rejected: per-thread connections (breaks sharing), routing all X traffic through one internal thread (GL calls on user threads issue X requests too, so it does not remove the need), XCB (GLX/Mesa still sit on Xlib).

**Errors.** The Xlib error handler is process-global and its default action terminates the process. Around each call that can raise an X error (context creation, make-current, drawable creation, teardown, and restoring the host's context) Tessera installs a handler under a global mutex, records errors raised on its own display, forwards errors from any other display to the previously installed handler, and restores that handler exactly.

**Thread exit.** GLX leaves a context bound to an exited thread permanently busy. Tessera releases the context a thread made current from a `thread_local` destructor when that thread exits.

**Loading.** `libX11.so.6` and `libGL.so.1` (fallback `libGLX.so.0`) are `dlopen`ed with `RTLD_LOCAL`; the handful of Xlib/GLX types needed are declared in `src/platform/x11/x11_types.hpp` with LP64 layout asserts. No X11/GL development packages are needed to build, and a machine without them reports `UNSUPPORTED`.
