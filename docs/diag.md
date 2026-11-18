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
