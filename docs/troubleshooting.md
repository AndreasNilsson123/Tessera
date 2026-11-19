# Troubleshooting

First: `tesseraGetLastErrorMessage()` (Java: `TesseraException.getMessage()`) names the real cause. Set a log callback with
`tesseraSetLogCallback` to see what the backend tried (backend choice, relaxed formats, restore retries, driver messages with
`TESSERA_FLAG_DEBUG`).

| Symptom | Likely cause and fix |
|---|---|
| `NO_MATCHING_CONFIG` | `TESSERA_FALLBACK_STRICT` (default) and the driver cannot give everything asked for. Check `tesseraGetContextInfo` on a `BEST_EFFORT` context to see what is obtainable. GL 2.1 + forward-compatible is rejected (forward-compat needs 3.0+). |
| `UNSUPPORTED` on every call | No backend could serve the request. The message lists what was tried. Check `TESSERA_BACKEND`, `DISPLAY` (GLX), and that libEGL/libGL/libX11 exist. |
| `UNSUPPORTED` from `tesseraCreateContextForWindow` / `...ForNativeWindow` | The integer form is for Win32 and X11 windows. Wayland surfaces and X11-through-EGL need `tesseraCreateContextForNativeWindow` (EGL); an `NSView` is not implemented (ADR 0014). |
| `INVALID_ARGUMENT` from window attach, "does not modify windows it does not own" | The window has no pixel format; pass `TESSERA_ATTACH_ALLOW_PIXEL_FORMAT` if you know it is fresh. |
| `WRONG_THREAD` | The context is current on another thread; release it there first. Java: a context held by a thread that just exited is retried for 2 s. |
| `CONTEXT_LOST` | GPU reset (robust contexts) or the window vanished. Use `tesseraRecreateContext`. |
| `PLATFORM` after destroying a window | You destroyed a window an attached context renders into. Destroy the context first. |
| `TESSERA_EGL_DEVICE=... matches none` | The index/render node/PCI address is not an EGL device on this system. An explicit choice never silently becomes another GPU. |
| Java: "virtual thread" error | GL needs platform threads. |
| Java: `UnsatisfiedLinkError` / loader | `-Dtessera.library.path=<file>` forces a library; `-Dtessera.binding=jni` forces the JNI binding. |
| X11: process exits with an Xlib error | Not from Tessera's calls (they run under a scoped handler); another library's X error with no handler of its own. |
| Slow context creation (25-30 ms) | Driver cost. Use `tesseraPrewarmContexts` from a background thread (`interop.md`). |
| Warning "N live contexts" | Contexts are never destroyed; each holds driver and OS resources (on Windows also a window). |

## Environment

`TESSERA_BACKEND`, `TESSERA_EGL_PLATFORM`, `TESSERA_EGL_DEVICE`, `TESSERA_EGL_LIBRARY`, `TESSERA_NO_XINITTHREADS`: see
`environment.md`.

## Reporting a problem

Include: the output of `tessera-diag --json` (`docs/diag.md`), the exact config, the error message, and the log callback output. For soak-type problems run `test_soak` (`docs/ci.md`) and attach its table.
