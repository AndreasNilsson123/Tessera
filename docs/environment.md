# Environment variables

All are optional and read when the relevant part of Tessera initialises. None is needed in normal use.

| Variable | Effect |
|---|---|
| `TESSERA_BACKEND` | Comma-separated list of backends to use, in order: `wgl` (Windows), `glx`, `egl` (Linux), `cgl` (macOS), `software` (the CPU renderer: Mesa llvmpipe through EGL, ANGLE's WARP or SwiftShader; known by name only, never in the default order: `wgl,software` means "the GPU, else the CPU"; ADR 0015). Unknown names are ignored with a warning; an empty result means every create fails with `UNSUPPORTED`. Default: the platform's own order (Linux: GLX, then EGL). Read by `tesseraInit` (when the first reference is taken). |
| `TESSERA_EGL_PLATFORM` | Force the EGL display platform: `surfaceless`, `device`, `default` or `angle`. Default: try them in that order (an ANGLE `libEGL` is recognised by its client extension and used first). |
| `TESSERA_EGL_DEVICE` | (A spelling of `TesseraContextConfig::adapter`, which beats it and has the same forms with a prefix: `uuid:`, `node:`, `pci:`, `index:`; see ADR 0017.) The GPU to use, on the `device` platform: an index (from `eglQueryDevicesEXT`, unstable between boots), a DRM render node such as `/dev/dri/renderD128`, a PCI address such as `0000:01:00.0` (resolved through `/dev/dri/by-path`), a UUID, or `software` (the first device that reports `EGL_MESA_device_software`; the `software` backend then checks that the renderer really is a CPU one). An explicit choice is never replaced by another GPU: if nothing matches, display creation fails with a message saying so. A device UUID (`EGL_EXT_device_persistent_id`; `uuid:` prefix optional, dashed or 32 hex digits) names a GPU stably across reboots. |
| `TESSERA_EGL_LIBRARY` | Path of the EGL library to load instead of `libEGL.so.1` (a specific vendor, or an optional ANGLE / Mesa build, ADR 0015). Windows: the **absolute** path of `libEGL.dll`; a bare or relative name is refused (the default is System32 only). The library's own folder is searched for its dependencies (`libGLESv2.dll`, `d3dcompiler_47.dll`), then System32; never the current directory or `PATH`. |
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
