# Living with JavaFX, AWT, GLFW and other OpenGL users

Tessera is a guest in your process. What it promises, and how each promise is checked:

| Promise | How it is checked |
|---|---|
| Whatever context your thread had current is current again after any create/destroy/inspect call | Every backend test makes a foreign context current first and verifies it afterwards, including on failure paths (fault-injected on the CGL fake) |
| Tessera never uses, destroys or modifies your windows, displays or connections | Own hidden windows and an own `Display*`; leak and ownership tests (window attach never changes a window's pixel format unless told to) |
| Tessera never terminates a shared display (EGL) | `testTerminateKeepsSharedDisplayAlive` |
| Process-global settings are untouched (error handlers, DPI awareness, locale, env, working directory) | X error handler is installed only around our own calls, chained, and restored exactly (tested); nothing else is changed |
| No Tessera context is ever left current on your thread | Restore-failure policy: retry, then release (fault-injected) |
| Works next to GLFW (a window and context of its own, Tessera around it and in its windows) | `java_glfw` (Windows WGL; Linux GLX; Wayland/EGL checked by hand, see below) |
| Works next to Java2D's OpenGL pipeline (AWT/Swing owning WGL/GLX contexts in the same JVM) | `java_awt_opengl` (Windows `WGLGraphicsConfig`, Linux `GLXGraphicsConfig`) |
| A host context of the *other* windowing API (EGL while Tessera uses GLX, GLX while it uses EGL) is neither broken nor disturbed | `testForeignEglContext` in `test_glx`, `testForeignGlxContext` in `test_egl` |
| Works while JavaFX renders | `java_javafx_d3d` and `java_javafx_sw` (Windows): three threads churn contexts for the whole FX run |

## Patterns

- **JavaFX:** render on your own platform thread with your own Tessera context into an FBO, read pixels, hand them to
  `PixelBuffer`/`WritableImage` via `Platform.runLater`. Do not touch the FX render thread or its context. (`java.md`)
- **AWT/Swing:** same idea; do not call Tessera from the EDT for rendering, and do not attach to AWT canvases (their native
  window belongs to AWT; `tesseraCreateContextForWindow` is for windows you own).
- **GLFW/SDL next to Tessera:** they own their windows and contexts; Tessera's contexts never share objects with theirs
  (OpenGL object sharing across libraries is not possible without the other library's cooperation). Exchange data through
  textures with the interop handles (`interop.md`) or through pixels. Tested with GLFW through LWJGL (`GlfwCoexistence`):
  creating, using and closing Tessera contexts never changes the native context current on the GLFW thread; GLFW renders correct
  frames while three threads churn contexts; Tessera renders into a `GLFW_NO_API` window via `createForWindow` and leaves the
  window alone; GLFW can be terminated with a Tessera context still current on another thread. After `Context.makeCurrent()` on
  a thread, GLFW's context is *not* current there any more (the documented replace rule), so call `glfwMakeContextCurrent`
  again before using GLFW on that thread. If you feed LWJGL's `GL` through Tessera's function provider, Tessera initialises
  itself on the first lookup: you do not need a Tessera context before GLFW's context can resolve functions.
- **Wayland/EGL hosts (GLFW, SDL on Wayland):** the host's context is an EGL one while Tessera's default backend order tries GLX
  first. On Mesa a thread has one native current context whichever API made it current, and this used to make Tessera's
  context creation fail with an X `BadAccess`; both backends now capture and restore the other API's thread state (only when
  that library is already loaded in the process) and release it before their own context becomes current. Verified natively
  against a real EGL (resp. GLX) host context and by hand with GLFW's Wayland platform on WSLg
  (`-Dglfw.platform=wayland`); not part of CI, which has no Wayland compositor.
- **AWT/Swing with `-Dsun.java2d.opengl=True`:** Java2D's OpenGL pipeline keeps its own contexts on its render-queue thread.
  `AwtCoexistence` paints a window and reads an accelerated image back every round while Tessera renders and churns; 0 wrong
  pixels on both platforms.
- **Several Tessera users in one process** (two libraries each linking Tessera): `tesseraInit` is reference counted, but each
  *copy* of the library has its own state. They are independent guests of each other, with the same guarantees.

## What is not covered

- Windows `java_javafx_d3d`/`sw`, Linux `es2`/`sw` (Mesa D3D12 and llvmpipe) and a longer stress run are tested. Not run: JavaFX's
  OpenGL pipeline on a real hardware driver under Xorg, macOS (Metal/ES2), hybrid-GPU laptops, runs of hours.
- Wayland only by hand (WSLg), not in CI; macOS, ANGLE, SDL and Qt have not been exercised. See `compat.md` for the full list of
  what has and has not been run.
