# Living with JavaFX, AWT, GLFW and other OpenGL users

Tessera is a guest in your process. What it promises, and how each promise is checked:

| Promise | How it is checked |
|---|---|
| Whatever context your thread had current is current again after any create/destroy/inspect call | Every backend test makes a foreign context current first and verifies it afterwards, including on failure paths (fault-injected on the CGL fake) |
| Tessera never uses, destroys or modifies your windows, displays or connections | Own hidden windows and an own `Display*`; leak and ownership tests (window attach never changes a window's pixel format unless told to) |
| Tessera never terminates a shared display (EGL) | `testTerminateKeepsSharedDisplayAlive` |
| Process-global settings are untouched (error handlers, DPI awareness, locale, env, working directory) | X error handler is installed only around our own calls, chained, and restored exactly (tested); nothing else is changed |
| No Tessera context is ever left current on your thread | Restore-failure policy: retry, then release (fault-injected) |
| Works while JavaFX renders | `java_javafx_d3d` and `java_javafx_sw` (Windows): three threads churn contexts for the whole FX run |

## Patterns

- **JavaFX:** render on your own platform thread with your own Tessera context into an FBO, read pixels, hand them to
  `PixelBuffer`/`WritableImage` via `Platform.runLater`. Do not touch the FX render thread or its context. (`java.md`)
- **AWT/Swing:** same idea; do not call Tessera from the EDT for rendering, and do not attach to AWT canvases (their native
  window belongs to AWT; `tesseraCreateContextForWindow` is for windows you own).
- **GLFW/SDL next to Tessera:** they own their windows and contexts; Tessera's contexts never share objects with theirs
  (OpenGL object sharing across libraries is not possible without the other library's cooperation). Exchange data through
  textures with the interop handles (`interop.md`) or through pixels.
- **Several Tessera users in one process** (two libraries each linking Tessera): `tesseraInit` is reference counted, but each
  *copy* of the library has its own state. They are independent guests of each other, with the same guarantees.

## What is not covered

- JavaFX's OpenGL (es2, Linux) pipeline next to Tessera has **not** been run. Only the Windows pipelines (D3D, software) are
  tested. See `compat.md` for the full list of what has and has not been run.
- Wayland, macOS and ANGLE have not been exercised.
