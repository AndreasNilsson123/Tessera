# Environment variables

All are optional and read when the relevant part of Tessera initialises. None is needed in normal use.

| Variable | Effect |
|---|---|
| `TESSERA_BACKEND` | Comma-separated list of backends to use, in order: `wgl` (Windows), `glx`, `egl` (Linux), `cgl` (macOS). Unknown names are ignored with a warning; an empty result means every create fails with `UNSUPPORTED`. Default: the platform's own order (Linux: GLX, then EGL). Read by `tesseraInit` (when the first reference is taken). |
| `TESSERA_EGL_PLATFORM` | Force the EGL display platform: `surfaceless`, `device` or `default`. Default: try them in that order. |
| `TESSERA_EGL_DEVICE` | With the `device` platform, the index of the GPU to use (from `eglQueryDevicesEXT`). |
| `TESSERA_EGL_LIBRARY` | Path of the EGL library to load instead of `libEGL.so.1` (a specific vendor or ANGLE build). Absolute path required on Windows. |
| `TESSERA_CGL_LIBRARY` | Path of the library to load instead of `/System/Library/Frameworks/OpenGL.framework/OpenGL` (the tests point it at a fake). |
| `TESSERA_NO_XINITTHREADS` | Set to anything to stop the GLX backend calling `XInitThreads()`. Only for hosts that guarantee Xlib is already thread-safe (ADR 0005). |

Tessera never writes to stdout or stderr; use `tesseraSetLogCallback` to see what it decided (backend, relaxed
configurations, dropped requests).
