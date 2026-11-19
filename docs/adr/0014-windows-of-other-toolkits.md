# ADR 0014: Attaching to windows of other toolkits: X11 and Wayland through EGL, and the NSView design (ABI 0.12.0)
Status: accepted for Win32/X11/Wayland; the Cocoa part is a design only.

## What changed
`tesseraCreateContextForWindow(config, uint64 window, flags)` took one integer, which is all a Win32 `HWND` or an X11 `Window` needs. A Wayland surface cannot be attached without the connection it lives on, and a bare integer says nothing about its kind. `tesseraCreateContextForNativeWindow(config, TesseraNativeWindow*, flags)` describes the window (`kind`, `window`, `display`, initial `width`/`height`); `tesseraResizeWindow` tells the context the window's size changed. The integer form stays and means "the platform's native kind".

## The rules for a borrowed window (all kinds)
1. **Never ours to destroy, move, show, hide or subclass.** Destroying the context leaves the window as it was; the same window can be attached again (tested on every kind).
2. **The window outlives the context.** Destroy the context first. A window that disappears under a live context makes `swapBuffers` fail with an error, never crash (WGL/GLX tested; EGL reports the EGL error).
3. **Anything Tessera creates for it, it owns and tears down in the right order.** Wayland: Tessera wraps the caller's `wl_surface` in a `wl_egl_window`; the EGL surface is destroyed first, then the `wl_egl_window`; the `wl_surface` and `wl_display` are the caller's (after our context is gone the surface is still usable, checked with a round trip).
4. **We never run the caller's event loop.** Wayland requires the caller to dispatch its own queue; Tessera's EGL swap uses a private queue. A swap with vsync on waits for a frame callback the compositor only sends to a *mapped, roled* surface, so a surface that is not shown yet must use `setSwapInterval(0)`.
5. **Threads.** Same as any context: current on one thread at a time. `tesseraResizeWindow` needs the context current on the calling thread (the buffer resize and the next swap belong together).
6. **Display connections.** X11 through EGL uses Tessera's own Xlib connection to `$DISPLAY` (Xlib threading handled as in ADR 0005/0013); the caller's `Display*` is not needed because only the `Window` id crosses. Wayland uses the caller's `wl_display` (EGL hands out one `EGLDisplay` per native display; it is initialised if needed and never terminated, ADR 0006). Contexts on different EGL displays cannot share.

## Backend choice
GLX takes an X11 window first (the default order is WGL, GLX, EGL), so EGL serves X11 only when GLX is unavailable or `TESSERA_BACKEND=egl`. Wayland has no GLX: only EGL can serve it. A backend returns `UNSUPPORTED` for a kind it does not handle so the next one gets a turn, with a message (an earlier version returned it bare and a stale error text from a previous call was shown).

## Config negotiation for windows
EGL window configs are asked for with `EGL_WINDOW_BIT`; EGL cannot say up front which config suits a native window (X11 needs the visual to match), so the candidates of each negotiation step are tried with `eglCreate*WindowSurface` until the driver accepts one. Window surfaces are double-buffered, so `TESSERA_FLAG_DOUBLE_BUFFER` is honoured here (pbuffers still refuse it). Platform entry points: the `EXT` function takes `EGLint` attributes, the EGL 1.5 core one `EGLAttrib` (pointer-sized): mixing them up crashed Mesa, so both are resolved and the right list is built for each. Swap control: `eglSwapInterval` on the current surface (no adaptive; no read-back, only the config's maximum), reported by `tesseraGetSwapControl`.

## Verification
- Native (`tests/test_eglwindow.cpp`, WSLg Mesa): an X11 window through EGL and a **real Wayland surface on WSLg's compositor** made by the test through `libwayland-client` by hand: render, read the pixel back, swap, resize, destroy the context, re-attach, refusals (bad kind, no display, zero size, an XID that is not a window).
- Java (`GlfwCoexistence`): GLFW's Wayland platform window attached through `Context.createForNativeWindow`, plus X11/Win32 windows through the described form.
- `test_wgl`: the described Win32 form, kind mismatches, argument validation.
- Not verified: a window that is destroyed under a live EGL context; GBM/KMS; a surface that is shown and receives frame callbacks (the test surface is never mapped: vsync swaps are covered only by `setSwapInterval(0)`); Wayland on a second compositor.

## macOS: `NSView` (design only, not implemented)
There is no Mac in the loop beyond a hosted CI VM without a window server session worth the name, and Apple has deprecated OpenGL. The shape, so the API need not change later: `TESSERA_WINDOW_COCOA` with `window` = an `NSView*`; the CGL backend creates an `NSOpenGLContext` (not a bare `CGLContextObj`) on the main thread, calls `setView:` and `update` on resize (`tesseraResizeWindow` is the hook: `NSView` frame changes must reach the context), `flushBuffer` for `swapBuffers`, and `setValues:forParameter:NSOpenGLContextParameterSwapInterval` for the interval. Threading is the hard part (AppKit calls must be on the main thread while Tessera contexts are current on arbitrary threads): the implementation would marshal `setView:`/`update` to the main queue and require `swapBuffers` from the render thread only. Until someone can run it, `TESSERA_WINDOW_COCOA` fails with `UNSUPPORTED` and a pointer to this section.
