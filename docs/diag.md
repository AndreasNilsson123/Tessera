# tessera-diag and tesseraProbe

## tesseraProbe

```c
TesseraContextConfig cfg = tesseraDefaultContextConfig();
cfg.api = TESSERA_API_OPENGL; cfg.versionMajor = 4; cfg.versionMinor = 5; cfg.profile = TESSERA_PROFILE_CORE;
TesseraProbeReport rep = tesseraDefaultProbeReport();
TesseraResult r = tesseraProbe(&cfg, &rep);      // same result a tesseraCreateContext would give
// rep.info: what the driver would give (version, profile, flags, bit depths, vendor/renderer strings)
// rep.backend: "wgl" | "glx" | "egl" | "cgl"; rep.message: the reason when r != TESSERA_SUCCESS
```

A probe really creates the context, reads it back and destroys it, because that is the only honest answer. It returns no handle,
bypasses the pre-warm pool (it must test the driver, not a cache), leaves nothing current, and leaves the caller's own current
context as it was (tested on WGL, GLX and EGL with a foreign context current). Probes can run on any thread and in parallel.
A probe cannot share (`shareWith` must be 0). Cost: one context creation (~30 ms here); cache the answer.

## tessera-diag

```
tessera-diag                 text report for every backend that exists on this platform
tessera-diag --json          same as JSON
tessera-diag --backend egl   one backend only (wgl, glx, egl, cgl); a TESSERA_BACKEND already set is respected
tessera-diag --quick         a short list of cases
tessera-diag --bench-swap [S]  frame pacing in a real window: vsync on, off and adaptive, S seconds each (default 2)
```

It uses only the public API. Each case is a **strict** request, so `[ -- ]` means the driver cannot give exactly that. The
generic cases are single-buffered (headless EGL cannot be double-buffered); a separate case asks for double buffering.
Exit status: 0 if some backend could create a context, 1 if none could, 2 for a usage error.

When reporting a problem or adding a row to `compat.md`, paste `tessera-diag --json` (it contains the platform, the
`TESSERA_*`/`DISPLAY` variables that are set, the GL strings and every case's result and message).

Built by default (`-DTESSERA_BUILD_TOOLS=OFF` to skip) and installed with the library.

## Not covered

It does not list GPUs or EGL devices (only the one the backend picks), does not run drawing tests, and cannot see a driver's
behaviour that only appears after creation (a probe only proves creation and the reported attributes).

## Swap control and frame pacing

Each backend section of the normal report has a `swap control` line (what a default window's context offers: `control`,
`adaptive (-1)`, `read-back`, or `none`); the JSON has the same under `swapControl`. `--bench-swap` renders into a window of its
own (Win32, or X11 on `$DISPLAY`; macOS and headless EGL cannot) and prints, per interval, the frame count, mean, min, p99 and
max frame time, fps and the number of dropped frames (longer than 1.5x the median):

```
interval  1 (driver reports 1):   235 frames, mean 8.319 ms (120.2 fps), min 5.126, p99 8.858, max 11.350, dropped 0
interval  0 (driver reports 0):  4252 frames, mean 0.470 ms (2128.6 fps), ...
interval -1 (driver reports -1):  239 frames, mean 8.319 ms (120.2 fps), ...
```

(RTX 3060 Laptop on a 120 Hz display, composited by DWM.) It exits 1 where the driver has no swap control, because there is nothing
to compare. See ADR 0012.
