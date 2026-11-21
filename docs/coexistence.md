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

## What each backend saves and restores (the foreign-state matrix)

Every create, destroy, inspect and attach call runs inside a guard that captures this per thread, does its work, and puts it back
(`ScopedForeignState`, one `*ForeignOps` per backend). If the restore itself fails, the guard retries and then releases, so no
Tessera context is ever left current on the host's thread. "Not touched" means Tessera neither reads nor writes it.

| State | WGL | GLX | EGL | CGL |
|---|---|---|---|---|
| The thread's current context | saved and restored (`HGLRC`) | saved and restored (`GLXContext`) | saved and restored, **for both client APIs** (OpenGL and OpenGL ES each have one) | saved and restored (`CGLContextObj`) |
| The current drawable(s) | `HDC` | display, draw **and** read drawable | display, draw **and** read surface, per API | none (a CGL context has no drawable here) |
| The bound client API (`eglBindAPI`) | n/a | n/a | saved and restored; Tessera binds only for its own call (`ScopedBindApi`) | n/a |
| The other API's thread state (Mesa: one native current context per thread, whichever API made it) | n/a | the host's EGL context is captured (only if libEGL is already loaded) and released before ours becomes current | the host's GLX context likewise | n/a |
| The window's swap interval | never touched by attach or any other call; set only by your `tesseraSetSwapInterval`, on the context's own drawable | same | same (acts on the current draw surface, which is ours) | n/a |
| The window's pixel format / visual | never changed on a window you hand over, unless you pass the allow flag (WGL) | never (GLX cannot) | never | n/a |
| Connections and displays | own hidden window, own `HDC`; the host's are never used | own `Display*` (a second connection to the server the window is on, from the host's `Display*`'s name or `$DISPLAY`); the host's connection is never used or closed | own X connection for X11 windows; the host's `wl_display` is used as given for Wayland and never dispatched; an `EGLDisplay` the host already initialised is shared, **never terminated** | n/a |
| X error handler | n/a | installed only around our own calls, chained, restored exactly | n/a (EGL reports errors by return value) | n/a |
| Last error (`GetLastError`, `eglGetError`, `glGetError`) | not preserved: our calls overwrite it (a thread's last-error code cannot be saved portably; ADR 0006 says so for EGL) | not preserved | not preserved | not preserved |

What is checked: `test_wgl`/`test_glx`/`test_egl`/`test_cgl_fake` make a foreign context current first and verify it afterwards on
success and failure paths (fault-injected on the CGL fake), including a foreign context of the *other* API (`testForeignEglContext`,
`testForeignGlxContext`), a host EGL display initialised before Tessera (`egl_host_first`) and a window attached with the host's own
`Display*` (`test_glx`, `test_eglwindow`). **Not covered:** a GLX drawable on a different *screen* of one server, and an X server other
than `$DISPLAY`'s (one machine only: both resolve to the same server here).

## What is not covered

- Windows `java_javafx_d3d`/`sw`, Linux `es2`/`sw` (Mesa D3D12 and llvmpipe) and a longer stress run are tested. Not run: JavaFX's
  OpenGL pipeline on a real hardware driver under Xorg, macOS (Metal/ES2), hybrid-GPU laptops, runs of hours.
- Wayland only by hand (WSLg), not in CI; macOS, ANGLE, SDL and Qt have not been exercised. See `compat.md` for the full list of
  what has and has not been run.
