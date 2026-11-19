# ADR 0012: Swap control reporting (ABI 0.10.0)
Status: accepted.

**Problem.** `tesseraSetSwapInterval` returned success when the driver accepted the call, which says nothing about what the application then gets. Measured on this machine's NVIDIA driver the call is honest, but the API could not tell a program that: drivers clamp (`GLX_MAX_SWAP_INTERVAL_EXT`), a compositor can override, extensions are missing (Mesa's D3D12 and llvmpipe advertise none), and adaptive vsync exists only with `*_swap_control_tear`. Programs that want frame pacing had to probe extension strings themselves.

**Decision.** `tesseraGetSwapControl(context, &TesseraSwapControl)` (current context, same rule as setting) reports:
- `caps`: `CONTROL` (setting can work: an extension exists and the drawable is a window), `ADAPTIVE` (-1 accepted), `READBACK` (`applied` is real);
- `requested`: the last interval a `tesseraSetSwapInterval` on this context succeeded with, kept by the core (so it is identical on every backend);
- `applied`: what the driver reports, or `TESSERA_SWAP_UNKNOWN`. The core hides a value the backend did not mark `READBACK`, so a backend cannot leak a guess;
- `maxInterval`: the clamp where the API exposes it (GLX EXT), else 0.

A context with no swap interval (pbuffer, surfaceless, CGL, EGL here) **succeeds with caps 0**, not an error, so "ask what I can do" is always a legal question.

**Per backend.**
- WGL: `WGL_EXT_swap_control` set, `wglGetSwapIntervalEXT` read back, adaptive from `_tear`. Verified on NVIDIA: 0, 1, 2 and -1 read back as requested.
- GLX: preference EXT > MESA > SGI, decided by `chooseGlxSwapApi` (pure, unit-tested). EXT reads back through `glXQueryDrawable` (`GLX_SWAP_INTERVAL_EXT`, max, and `LATE_SWAPS_TEAR` to restore the sign), MESA through `glXGetSwapIntervalMESA`, SGI cannot be read back. Adaptive only through EXT.
- EGL and CGL: caps 0 (their contexts here have no window surface). Window surfaces would add `eglSwapInterval` and its config min/max.

**What counts as paced.** A single-buffered window has nothing to swap, so `SwapBuffers` returns at once whatever the interval; the header and Java docs say the window must be double-buffered. `tessera-diag --bench-swap` found this the hard way (an unpaced vsync-on run) and now requests a double-buffered format.

**Frame pacing measured.** `tessera-diag --bench-swap [S]` renders into its own window and reports, per interval, frames, mean/min/p99/max frame time and a dropped-frame count (frames longer than 1.5x the median). Windows and X11 only. On the development machine (RTX 3060 Laptop, 120 Hz, DWM composited): vsync on 8.32 ms/frame (120.2 fps, 0 dropped), off about 0.4 ms (2100+ fps), adaptive 8.32 ms. `test_wgl` asserts the same relations (on slower than off, on within 0.7-1.6 of the refresh period) against whatever display the machine has; on the hosted GDI renderer, which has no swap control, it checks that asking says UNSUPPORTED.

**Not done / not verified.** The EXT/MESA/SGI branches of GLX are verified by the pure selection tests but not against a server that advertises them (WSLg and llvmpipe advertise none): the real-server `applied` read-back on GLX is untested. DWM behaviour in exclusive fullscreen, on a variable-refresh display and under Optimus are untested.
