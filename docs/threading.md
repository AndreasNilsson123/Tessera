# Threading guide

The rules, all enforced by the library (a violation returns an error code; it never corrupts anything):

1. **A context is current on at most one thread at a time.** `tesseraMakeCurrent(ctx)` on a thread while another thread
   holds `ctx` fails with `TESSERA_ERROR_WRONG_THREAD`. Release first (`tesseraMakeCurrent(TESSERA_NULL_HANDLE)`) on the
   holder, then make it current elsewhere.
2. **One current Tessera context per thread.** Making another current on the same thread releases the previous one first.
3. **`tesseraSwapBuffers`, `tesseraSetSwapInterval`, `tesseraGetResetStatus` need the context current on the calling
   thread.**
4. **Everything else may be called from any thread:** create, destroy, `getInfo`, `getNativeHandles`, `getProcAddress`,
   prewarm, recreate. Creation on several threads at once is supported and tested.
5. **Destroying a context another thread holds is legal.** The handle dies immediately for everyone; the native teardown
   happens when the holder releases it, switches, or exits (ADR 0008). That is what makes finalizers and `Cleaner` threads
   safe.
6. **A thread that exits with a Tessera context current** releases it automatically (a `thread_local` exit hook). Do not rely
   on it for fast paths; release explicitly.
7. **Your own context stays yours.** Whatever context a thread had current before a Tessera call is current again after it,
   for every call that creates, destroys or inspects a context. The exceptions are the calls whose purpose is to change the
   current context (`tesseraMakeCurrent`): making a Tessera context current replaces whatever was current on that thread
   (Mesa EGL even across APIs), and releasing it leaves the thread with nothing current, not with your old context.

## Per backend

- **WGL:** all window and pixel-format work happens on one private thread owned by Tessera (ADR 0004), so your threads
  never need a message pump for us and we never touch yours.
- **GLX:** one `Display*` of our own shared by all contexts; Tessera calls `XInitThreads` before opening it unless
  `TESSERA_NO_XINITTHREADS=1` (ADR 0005). If your process uses Xlib from several threads it must have called it too (every GL
  toolkit does).
- **EGL:** the `EGLDisplay` is shared per process and never terminated by Tessera.

## Library lifetime

`tesseraInit`/`tesseraTerminate` are reference counted. Terminate destroys everything Tessera created; handles from before
are invalid afterwards. Calling anything after the last terminate returns `NOT_INITIALIZED` (or a null/zero result for the
no-fail calls).

## Java

Use platform threads for GL; virtual threads are refused by `makeCurrent`. See `java.md`.
