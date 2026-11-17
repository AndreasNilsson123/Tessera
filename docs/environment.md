# Environment variables

All are optional and read when the relevant part of Tessera initialises. None is needed in normal use.

| Variable | Effect |
|---|---|
| `TESSERA_BACKEND` | Comma-separated list of backends to use, in order: `wgl` (Windows), `glx`, `egl` (Linux), `cgl` (macOS). Unknown names are ignored with a warning; an empty result means every create fails with `UNSUPPORTED`. Default: the platform's own order (Linux: GLX, then EGL). Read by `tesseraInit` (when the first reference is taken). |
| `TESSERA_EGL_PLATFORM` | Force the EGL display platform: `surfaceless`, `device` or `default`. Default: try them in that order. |
| `TESSERA_EGL_DEVICE` | The GPU to use, on the `device` platform: an index (from `eglQueryDevicesEXT`, unstable between boots), a DRM render node such as `/dev/dri/renderD128`, or a PCI address such as `0000:01:00.0` (resolved through `/dev/dri/by-path`). An explicit choice is never replaced by another GPU: if nothing matches, display creation fails with a message saying so. UUID selection is not implemented. |
| `TESSERA_EGL_LIBRARY` | Path of the EGL library to load instead of `libEGL.so.1` (a specific vendor or ANGLE build). Absolute path required on Windows. |
| `TESSERA_CGL_LIBRARY` | Path of the library to load instead of `/System/Library/Frameworks/OpenGL.framework/OpenGL` (the tests point it at a fake). |
| `TESSERA_NO_XINITTHREADS` | Set to anything to stop the GLX backend calling `XInitThreads()`. Only for hosts that guarantee Xlib is already thread-safe (ADR 0005). |

Tessera never writes to stdout or stderr; use `tesseraSetLogCallback` to see what it decided (backend, relaxed
configurations, dropped requests).

## Java system properties

Set with `-D` on the JVM command line (see `java.md`).

| Property | Effect |
|---|---|
| `tessera.library.path` | Load this native library file instead of the copy embedded in the jar. |
| `tessera.binding` | `auto` (default), `jni` or `ffm`. `ffm` needs Java 22+. |
