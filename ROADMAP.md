# Tessera Roadmap

Tessera is a native OpenGL / OpenGL ES context creation library. It is meant to be consumed by LWJGL-style
Java applications and to run **inside the same process as other GL users** (JavaFX, Swing/AWT, other native libraries).

## Guiding principles

1. **Be a good guest.** Tessera never mutates process-global state it did not create. Every public call that touches a
   native "current" context saves and restores the caller's previous context/drawable on that thread.
2. **Own everything we use.** Own hidden windows, own window class (unique per module instance), own display connection,
   own message handling. We never subclass, hook, or `SetPixelFormat` a window we did not create.
3. **No surprises across the ABI.** Stable C ABI. No exceptions, no `abort`/`exit`, no output to stdout/stderr, no
   global initializers with side effects. Every failure is an error code plus a thread-local message.
4. **Explicit threading model.** Documented thread affinity per object. Misuse is detected and reported, not UB.
5. **Fail soft, degrade predictably.** Request → negotiate → report what was actually obtained. Never silently hand
   back a context that is weaker than what the caller demanded unless they allowed fallback.
6. **Test against the hostile case first.** JavaFX coexistence, multi-context, multi-thread and teardown tests are
   part of every phase's exit criteria, not a late add-on.

## Architecture (target)

```
┌────────────────────────────────────────────────────────────┐
│ Java bindings   (Panama/FFM primary, JNI fallback)         │  LWJGL-compatible
├────────────────────────────────────────────────────────────┤
│ tessera.h  — stable C ABI, versioned, handle-based         │
├────────────────────────────────────────────────────────────┤
│ Core (C++): config negotiation, context registry,          │
│ current-context tracker, error/log, loader, quirks DB      │
├───────────┬───────────┬───────────┬────────────┬───────────┤
│ WGL       │ GLX       │ EGL       │ CGL/NSGL   │ OSMesa/   │
│ (Windows) │ (X11)     │ (Wayland, │ (macOS)    │ headless  │
│           │           │ X11, GBM, │            │           │
│           │           │ surfaceless, ANGLE)    │           │
└───────────┴───────────┴───────────┴────────────┴───────────┘
```

Key decisions to make in Phase 0 (recorded as ADRs in `docs/adr/`):

- Core language: C++17 with a pure-C public header (recommended) vs. plain C.
- Java binding: FFM/Panama (JDK 22+) as primary; JNI shim for older JDKs / LWJGL 3 integration.
- Handle model: opaque 64-bit handles + generation counter (stale-handle detection) instead of raw pointers.
- Function loading: Tessera exposes `tesseraGetProcAddress` and returns native handles so LWJGL can call
  `GL.createCapabilities()` against our loader. Tessera itself links **no** GL entry points beyond what it needs.

---

## Phase 0 — Foundations (project setup)

- [x] Repo layout: `include/tessera/`, `src/core`, `src/platform/{win32,x11,wayland,egl,cocoa}`, `bindings/java`, `tests`, `docs`.
- [x] CMake: shared + static targets, symbol visibility hidden by default, explicit export macro, versioned soname / DLL version resource.
- [~] CI matrix: Windows (MSVC, clang-cl), Linux (gcc, clang; X11 + Wayland), macOS (arm64 + x64). Software GL (Mesa llvmpipe / WARP-like) for headless runners.
- [~] Tooling: clang-format, clang-tidy, sanitizers (ASan/UBSan/TSan), `-Wall -Wextra -Werror`.
- [x] ABI policy: semantic versioning, `tesseraGetVersion()`, struct-size-prefixed config structs (`sizeof` in first field) so fields can be appended without breaking callers.
- [~] ADRs for the decisions above.

**Exit:** empty library builds and loads on all 3 OSes in CI; a Java smoke test can call `tesseraGetVersion()`.

## Phase 1 — Core API & infrastructure

- [x] Error model: `TesseraResult` enum, thread-local last-error string, optional log callback (never stdout).
- [x] Handle table with generation counters; thread-safe; double-destroy / use-after-destroy returns an error rather than crashing.
- [x] Library init/shutdown with reference counting (`tesseraInit` / `tesseraTerminate` may be called by several independent components in the same process; real teardown on last release).
- [~] **Current-context tracker**: wrapper `ScopedForeignContextGuard` capturing native current context + drawable (+ display on GLX/EGL, + API binding on EGL) and restoring on scope exit, including on error paths.
- [~] Dynamic loader for GL libraries (`opengl32.dll`, `libGL.so.1`/`libGLX`/`libEGL`, `OpenGL.framework`) via `LoadLibrary`/`dlopen` with `RTLD_LOCAL`; never `RTLD_GLOBAL`, never unload libraries we did not load.
- [~] `getProcAddress` with correct per-platform semantics (wglGetProcAddress returns non-null garbage values `1,2,3,-1` on some drivers; core functions must come from the module itself; GLX/EGL extension-vs-core rules).
- [x] Config API: `TesseraContextConfig` (api, major/minor, profile, flags, share, samples, color/depth/stencil bits, sRGB, float, double-buffer, stereo, robustness, release behavior, priority) + `TesseraFallbackPolicy` (strict / best-effort / explicit list).
- [x] Result introspection: `tesseraGetContextInfo` returns what was *actually* created (version string, profile, flags, renderer, vendor, extensions, pixel-format details).

**Status notes:** guard logic, loader and the junk-pointer filter are done and unit-tested. The per-platform pieces (real `Ops` policies, GL library names, `tesseraGetProcAddress`) land with each backend (Phases 2-5). `tesseraCreateContext` returns `UNSUPPORTED` until a backend exists. Still open: a TSan run (needs a Linux/clang runner).

**Exit:** unit tests for handle table, guard, loader, error paths; TSan clean.

## Phase 2 — Windows (WGL) — first full backend

JavaFX on Windows uses D3D (Prism d3d) by default, so WGL conflicts are less likely but LWJGL/AWT/other libs may be present.

- [x] Bootstrap dummy window + legacy context to resolve `wglCreateContextAttribsARB` / `wglChoosePixelFormatARB`; destroy it afterwards; keep the saved/restored foreign context discipline throughout.
- [x] Own window class with per-module unique name (`Tessera_<hmodule>_<n>`), unregistered on terminate; hidden, tool-window, never activated, `WS_EX_NOACTIVATE`, no taskbar entry.
- [x] Dedicated message-pump strategy: windows are created on the thread that will pump them (document and enforce); optional internal "GL host thread" mode.
- [x] Pixel-format negotiation via `WGL_ARB_pixel_format`; fallback ladder to legacy `ChoosePixelFormat`; MSAA, sRGB (`WGL_ARB_framebuffer_sRGB`), float (`WGL_ARB_pixel_format_float`).
- [~] Context creation: core/compat/ES profile, debug, forward-compat, robust-access + reset notification, no-error (`KHR_no_error`), release behavior, context priority.
- [~] Shared contexts (`wglShareLists` / attribs-share) with creation-order validation.
- [x] Offscreen: hidden-window path, `WGL_ARB_pbuffer` path, and surfaceless/FBO-only mode (`make current` with no drawable where supported).
- [~] Swap interval (`WGL_EXT_swap_control` / `_tear`), `SwapBuffers`, DWM-composition quirks.
- [ ] Multi-GPU: enumerate adapters; choose via `WGL_NV_gpu_affinity`, `WGL_AMD_gpu_association`, or DXGI preference; document that exporting `NvOptimusEnablement` must be done by the host EXE and is outside a DLL's control.
- [x] DPI: **never** call `SetProcessDPIAware`/`SetProcessDpiAwareness...` (process-global; would break JavaFX scaling). Report DPI, don't change it.
- [ ] ANGLE / Mesa-on-Windows (`libEGL.dll`) detection through the EGL backend (Phase 4).

**Status notes (Phase 2):**
- Windows live on one private, message-pumping host thread (ADR 0004), so contexts can be created/destroyed from any thread. This replaces the "optional host thread mode" idea: it is always on.
- Done and tested on NVIDIA (RTX 3060, driver 546.30): core/compat/ES contexts, debug/forward-compat flags, MSAA best-effort degradation, sRGB/float formats (code paths), pbuffer, surfaceless-style `SURFACE_NONE` (an internal 1x1 window is still used), sharing, thread-ownership rules, foreign-context preservation, leak check (300 cycles, flat GDI/USER counts).
- `[~]` items: robust access / reset notification / no-error / release-flush are implemented but untested (the test GPU path was not exercised for them); share-order validation and context priority are not implemented; DWM swap quirks untested.
- Not done: multi-GPU selection (`WGL_NV_gpu_affinity`, `WGL_AMD_gpu_association`, DXGI) and ANGLE detection (Phase 4). Only one GPU vendor and driver tested so far.
- Context creation is ~29 ms on the test machine (mostly driver cost, not profiled); pooled creation is in Phase 8 (`tesseraPrewarmContexts`, ~0.01 ms).
- Exit criteria still open: a 10k-cycle soak (300 done) and the JavaFX coexistence run (needs the Phase 7 Java bindings).

**Exit:** create/destroy 10k contexts in a loop with no handle/GDI leaks (verified with GDI/USER object counters); coexistence test with a running JavaFX app (D3D pipeline and `-Dprism.order=sw`) shows no interference.

## Phase 3 — Linux X11 (GLX) backend

JavaFX on Linux uses the `es2` pipeline (GLX-based) so **real conflicts are possible here**.

- [x] **Own `Display*` connection** (`XOpenDisplay`) per Tessera instance (one shared by all its contexts, because GLX share groups only work within one connection); never reuses AWT/JavaFX's. Closed with the backend.
- [~] Xlib threading: **decision changed from the original plan** (ADR 0005). Tessera calls `XInitThreads()` once, before opening its own connection, unless `TESSERA_NO_XINITTHREADS=1`: a single shared connection is used from many threads, which Xlib only allows after that call, and every GL toolkit does the same. The per-thread-connection/XCB alternatives were rejected. Not done: detecting whether the host already called it.
- [x] Scoped `XSetErrorHandler`: installed only around calls that can raise X errors, serialised on a global mutex, chained to the host's handler for errors on other displays, restored exactly (tested). Also protects the restore of the host's context against a host drawable that has since died.
- [~] FBConfig selection via `glXChooseFBConfig`, visual matching, `GLX_ARB_create_context(_profile)`, `_es2_profile` done and tested (core/compat/ES/MSAA degradation/sharing). `_robustness` and `_no_error` are implemented but untested: the test server does not advertise robustness, and no-error was not exercised.
- [~] Offscreen: pbuffer and hidden never-mapped window with our own colormap done and tested. Drawable-less `glXMakeContextCurrent(None, None, ctx)` is **not** implemented: `SURFACE_NONE` still uses an internal window, as on WGL.
- [~] Swap control (`GLX_EXT/MESA/SGI_swap_control`) implemented; the test server advertises none of them (WSLg/Mesa D3D12), so it is untested and correctly reports `UNSUPPORTED`. `glXSwapBuffers` done.
- [~] GLVND vs legacy `libGL.so`: loads `libGL.so.1`, falls back to `libGLX.so.0`; proc addresses are filtered to `gl*` names. Not tested against a legacy (non-GLVND) libGL.
- [x] Context sharing: validated through the single shared connection (texture visible across a share group; parent destroyed first).
- [x] (Added) A context left current on an exiting thread is released by Tessera on that thread's exit. GLX leaves it permanently busy otherwise (`BadAccess`); found by the integration test.
- [x] (Added) `tesseraSwapBuffers` now requires the context to be current on the calling thread, on every platform (EGL requires it; GLX behaviour is driver-defined otherwise).
- [x] (Refactor) WGL and GLX share `ARB_create_context` negotiation, GL info queries, the strict-mode check and the pixel-format relaxation ladder (`src/platform/common/`).

**Status notes (Phase 3):**
- Tested on Ubuntu 22.04 under WSL2/WSLg (Mesa 23.2 D3D12 on the RTX 3060), gcc 11 `-Werror`, plus ASan/UBSan. TSan reports races only inside NVIDIA's uninstrumented WSL driver (concurrent swaps on different contexts through the shared connection); none are attributable to Tessera code, but that is unproven. ASan leak reports are one-time driver allocations (no Tessera frames; fd count flat).
- Not tested: a real Xorg/NVIDIA or AMD/Intel Mesa stack, Xvfb with llvmpipe (CI job added, never run), clang, 32-bit, non-GLVND libGL.

**Exit:** coexistence test with a JavaFX es2 pipeline window + Tessera worker contexts running for hours (Xvfb + real X, with `XSetErrorHandler` interference tests).

## Phase 4 — EGL backend (Wayland, headless, GBM, ANGLE, GLES)

- [~] Platform selection via `EGL_EXT_platform_base`: **surfaceless** (`EGL_MESA_platform_surfaceless`), **device** (`EGL_EXT_platform_device` with `eglQueryDevicesEXT`, pick a GPU with `TESSERA_EGL_DEVICE`) and the **default display** are implemented, in that order (override with `TESSERA_EGL_PLATFORM`). Only the surfaceless platform has been exercised. Native X11 / Wayland / GBM platforms are **not** implemented: Tessera renders to pbuffers or nothing, so it never needs a window, and a native display adds a connection to the host's windowing system for no benefit. ANGLE: not done (see below).
- [x] Display ownership, decided differently from the plan (ADR 0006): EGL hands the same `EGLDisplay` to everyone who asks for the same platform, and `eglInitialize`/`eglTerminate` are not reference counted, so Tessera **never calls `eglTerminate`**. Displays are initialised once and stay initialised; only contexts and surfaces Tessera created are destroyed. Tested: a foreign context on the very same display survives `tesseraTerminate`.
- [x] Per-thread EGL state: the current context/display/draw/read surface of **each** client API plus the bound API (`eglBindAPI`) are saved and restored around every operation; the bound API is also restored after make-current/release/destroy. Tested against GLES and desktop GL foreign contexts. Not preservable: the thread's last-error code (`eglGetError` clears it).
- [~] Contexts: desktop GL (core/compat/flags/version ladder) and GLES 2/3.x done and tested; robustness, no-error, flush control implemented (robustness succeeded on Mesa, the others not exercised). Protected content and priority: not implemented.
- [x] Surfaceless (`EGL_KHR_surfaceless_context`), pbuffer, and Mesa's surfaceless platform; `SURFACE_NONE` is a genuinely drawable-less context here (unlike WGL/GLX). Default/hidden-window requests are served by a pbuffer and reported as such. Double buffering and stereo cannot exist on these drawables: strict mode refuses with a clear message, best effort drops them.
- [ ] Fence sync + dma-buf export for interop: deferred to Phase 8 (nothing consumes it yet).
- [~] Mesa validated (llvmpipe/swrast under CI is configured but unrun; D3D12-over-WSL run locally). ANGLE: **not done**: the Windows loader is restricted to System32 by design, and loading an ANGLE build from an application directory needs `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` handling and a way to test it; `TESSERA_EGL_LIBRARY` exists on all platforms but the EGL backend is only built on Linux so far.
- [x] (Added) Backend selection: `TESSERA_BACKEND=glx,egl` orders/restricts backends; by default GLX is tried first and a request it cannot serve at all (no display) falls through to EGL. Tested headless.
- [x] (Added) Switching between two of our contexts on one thread now releases the previous one explicitly (EGL keeps per-API current contexts).
- [x] (Added) Hardening found by TSan: Tessera's own threads no longer call `glGetString` concurrently (Mesa's software driver formats the result into a global buffer; concurrent callers could read a torn string).

**Status notes (Phase 4):**
- Tested on Ubuntu 22.04 / WSLg: EGL 1.5, Mesa 23.2 (D3D12 on the RTX 3060) via the surfaceless platform; gcc 11 `-Werror`; ASan/UBSan clean; TSan reports only races inside NVIDIA's WSL driver, none with Tessera code on the accessing side.
- Mesa keeps **one** current context per thread (not one per API as the spec says): making a Tessera context current displaces the host's, even across APIs. That is what `tesseraMakeCurrent` documents; create/destroy/failed-create paths still restore everything.
- Not tested: NVIDIA/AMD proprietary EGL, the device platform, the default-display fallback on a real desktop, Wayland, ANGLE, robustness/no-error/flush-control behaviour beyond "creation succeeded", clang.

**Exit:** same conformance suite runs on EGL (Mesa surfaceless) in CI headless. (`test_egl` is the EGL equivalent; CI job configured, never run.)

## Phase 5 — macOS backend (CGL / NSGL)

JavaFX on macOS uses Metal (recent) or ES2/NSGL (older); AppKit threading rules apply.

> **Verification status: unlike every earlier phase, none of this has run against Apple's OpenGL.** There is no Mac in the
> development environment. What *was* verified: the backend compiles warning-free under MSVC and gcc (it uses no Apple
> headers), and its whole control flow passes against a fake `OpenGL.framework` (`tests/fake_cgl.cpp`, run under ASan/UBSan
> and TSan, both clean). What was *not*: that Apple's CGL accepts these constants and attribute lists, how it ranks pixel
> formats, the colour-size convention, and what a hosted macOS VM offers. `tests/test_cgl.cpp` is written to settle exactly
> that on the first run on a Mac; treat the backend as unverified until it passes.

- [~] CGL-only path (no AppKit) for offscreen/headless contexts: `CGLChoosePixelFormat`, `CGLCreateContext`, drawable-less (render to FBOs), any thread, no main-thread requirement. Implemented through `dlopen`ed `OpenGL.framework` with hand-declared types (ADR 0007). Passes against the fake only.
- [ ] NSOpenGLContext path for on-screen: **not implemented, deliberately**: Tessera creates no windows or views anywhere, and a windowed path would pull in AppKit threading rules for a feature nothing here needs.
- [~] Core 3.2/4.1 and legacy 2.1 only, with clear refusals: ES -> `UNSUPPORTED` (left to an ANGLE/EGL backend), compatibility above 2.1, versions above 4.1, and the debug / robust / no-error / flush-control / sRGB-default-framebuffer requests -> `NO_MATCHING_CONFIG` in strict mode, dropped and reported absent in best-effort mode. Core requests try the 4.1 profile first, then 3.2. A format that exists only on an offline GPU is tried last (it can wake a discrete GPU).
- [ ] Deprecation hygiene / Metal-backed ANGLE: CGL is deprecated since macOS 10.14 but is plain C with no compile-time deprecation warnings under `dlopen`; ANGLE (the forward path) is not implemented.
- [x] No `NSApplication` creation, no run loop, no AppKit calls at all: nothing to interfere with AWT/JavaFX. (By construction; not tested alongside JavaFX.)
- [x] (Found by the fake-driver test) `queryContextInfo` no longer copies `RELEASE_FLUSH` from the request; every reported flag is read back from the driver, so a dropped request is never claimed as granted (affected all backends).
- [x] (Added) `TESSERA_CGL_LIBRARY` overrides the library to load; `cgl` joins the `TESSERA_BACKEND` names.

**Status notes (Phase 5):**
- Biggest risks, in order: (1) the numeric constants in `cgl_types.hpp` were written from memory; (2) `CGLDescribePixelFormat(kCGLPFAColorSize)` may or may not include alpha (a heuristic decides the reported per-channel bits); (3) whether `GL_CONTEXT_FLAGS` reports the forward-compatible bit for core contexts; (4) hosted macOS CI is a GPU-less VM, so core-profile and hardware paths may be unavailable there.
- The `cgl_fake` test runs wherever the library builds (Linux, macOS, any POSIX), so the backend's logic is regression-tested on every CI platform; only the real-framework behaviour is open.

**Exit:** headless CGL tests on CI runners; manual coexistence matrix with JavaFX on macOS. (`test_cgl` + an informational macOS job exist and have never run; the JavaFX matrix is untouched.)

## Phase 6 — Context feature completeness & robustness

- [~] Attribute matrix, run on **WGL (NVIDIA), GLX (Mesa) and EGL (Mesa)**: 192 combinations each of API x profile x version x flags, with samples, depth/stencil, fallback policy and surface rotated through them (not the full cross-product). An oracle independent of the library checks that every returned context honours the request (hard attributes always, flags/sizes in strict mode), that created contexts are usable, that failures return no handle, that no result is `PLATFORM`/`UNKNOWN`, and that the host's context is undisturbed by every create. Results: WGL 140 ok / 42 refused / 10 invalid; GLX 67 / 115 / 10; EGL 97 / 85 / 10; zero problems. **Not covered: CGL** (only the fake-driver cases), and robustness/no-error flags are only exercised where a driver grants them.
- [~] Reset notification: `tesseraGetResetStatus`, permanent *lost* state (`CONTEXT_LOST` from make-current/swap/swap-interval), `tesseraRecreateContext` with share-group preservation (ADR 0008). Polling returns `NONE` on real drivers (NVIDIA on WGL, Mesa on EGL; GLX Mesa grants no robustness). **`GUILTY`/`INNOCENT`/`UNKNOWN`, the sticky lost state and group-aware recreate are tested only against a fake backend; no real GPU reset has ever been triggered here.**
- [~] Driver loss / TDR / hot-unplug: only EGL's native `EGL_CONTEXT_LOST` is mapped (to `CONTEXT_LOST`, untested on real hardware). WGL and GLX have no reliable native signal, so loss is detected by polling on robust contexts only. Display changes: nothing done.
- [x] Share-group management: groups are tracked; destruction ordering holds (children keep their parent alive); a context destroyed while current on another thread is **deferred-destroyed on that thread** (release, switch or exit), tested with the teardown thread asserted and a race-closing re-check; `tesseraTerminate` drains pending destructions. **API change:** `tesseraDestroyContext` no longer returns `WRONG_THREAD` in that case.
- [x] Debug output plumbing: a granted debug context forwards `KHR_debug` messages (severity mapped to log level) to the log callback, synchronously, through a stateless function so nothing outlives the context. Tested: a provoked `GL_INVALID_ENUM` arrives in the log on NVIDIA (WGL) and Mesa (GLX, EGL). Not covered: an application replacing the callback (it simply wins), library unload while a foreign party still holds the context.
- [ ] Quirks database: **deliberately not started.** There is no known, reproducible quirk to put in it (the roadmap's examples are either handled generically already - junk `wglGetProcAddress` values, software-rasteriser limits via negotiation - or unverifiable here, e.g. Intel core-profile bugs). Machinery with no entries invites untestable ones; add it with the first real, regression-tested quirk.
- [~] Malformed driver output: **found and fixed undefined behaviour** in the GL version parser (signed overflow on absurd digit strings, which it then accepted as valid; reproduced with UBSan before the fix, table-tested after). Platform extension lists are parsed from NUL-terminated strings with bounds-safe splitting. `GL_NUM_EXTENSIONS`: not applicable, Tessera reads platform extension strings, never GL's.
- [~] Resource limits: a warning at 64, 256, 1024, ... live contexts (tested: once per threshold). No refusal on Windows' ~10,000 GDI/USER handle limit beyond the OS's own error; the handle table's hard cap is 2^20.
- [x] (Found by the matrix) Forward-compatible with an explicit version below 3.0 is now rejected as `INVALID_ARGUMENT`, and drivers' other "invalid request" answers (NVIDIA: `ERROR_INVALID_DATA`) map to `NO_MATCHING_CONFIG` instead of leaking out as `PLATFORM`.
- [x] (Found by the fault sweep) A failing restore of the host's context could leave a Tessera context current on the caller's thread; CGL/GLX/EGL now release it (CGL retries once first), as WGL already did.
- [x] (Strictness) `contextSatisfies` also verifies reset notification, release behaviour and colour/depth/stencil minimums.

**Status notes (Phase 6):**
- New tests: `test_core` (lifecycle with a controllable fake backend; a **mutation check** confirmed the deferred-destruction tests fail when the fix is removed), the shared `robustness_common.hpp` suite in `test_wgl`/`test_glx`/`test_egl`, and in `test_cgl_fake` a fault-injection sweep plus missing/wrong-library runs. ASan/UBSan and TSan clean on everything that runs without a GPU.
- Public API (0.4.0): `tesseraGetResetStatus`, `tesseraRecreateContext`, `TESSERA_RESET_*`, `TESSERA_RECREATE_SHARE_PRESERVED`; the destroy-while-current-elsewhere contract changed.

**Exit:** fault-injection tests (fake loader returning NULLs, failing mid-creation) leave no leaks and restore foreign state every time. **Met for CGL only**: every native call position of five create scenarios was failed in turn (leaks, handles, error codes and the host's context checked), and a missing / non-OpenGL library gives a clean `UNSUPPORTED`. WGL, GLX and EGL talk to real drivers, so their failure paths are covered by the matrix and the earlier tests but have **not** been fault-injected.

## Phase 7 — Java / LWJGL integration

Decision (ADR 0009, resolving the open question about the minimum JDK): **JNI baseline for Java 11+, FFM automatically on Java 22+, one multi-release jar, one native file.** Verified from the same jar on Java 17 and 21 (JNI) and 22, 25 and 28 (FFM); Java 19 runs JNI.

- [x] Java API (`dev.tessera`, placeholder package): `Context` (`AutoCloseable`), `ContextConfig` builder, `ContextInfo`, `Result`/`TesseraException`/`ContextLostException`, `Logger`, `ResetStatus`, `Tessera`. A `Cleaner` safety net destroys contexts that are garbage collected unclosed (tested; it works at all because of Phase 6's deferred destruction).
- [x] LWJGL bridge: `TesseraFunctionProvider` (an `org.lwjgl.system.FunctionProvider`) and `TesseraGL` (`install()`, per-thread `createCapabilities()`). Tested with real LWJGL 3.3.4 through both bindings: version/renderer match, FBO clear and shader draw read back and verified, buffer round trip, a second thread with its own context and capabilities. **Found and documented:** LWJGL's `GL` class loads the system OpenGL the moment it is first touched, so a custom provider needs `Configuration.OPENGL_EXPLICIT_INIT` set first (done by `install()`).
- [~] Bindings: JNI (glue compiled into the tessera library) and FFM, both behind one `NativeApi` interface. FFM is **hand-written instead of `jextract`ed** (no tool installed; the surface is ~20 functions and 2 structs; layouts pinned by offsets plus an ABI-version check).
- [x] Native loading: `-Dtessera.library.path`, else extract the embedded copy to a fresh private temp directory, verify its checksum, `System.load` by absolute path. (Changed from the plan's "version-qualified name": a fresh directory per run is stronger against planted files.)
- [x] Classloader safety: JNI allows one classloader per library, so a second classloader gets a private copy (independent native state) instead of an error; tested with two isolated classloaders using the library at once. FFM shares one instance.
- [~] JavaFX integration doc and sample: `docs/java.md` and the `FxCoexistence` test (JavaFX window + render thread + context churn). **Tested on Windows only, and only on JavaFX's Direct3D and software pipelines.** JavaFX's OpenGL (`es2`) pipeline exists only on Linux/macOS; JavaFX Windows does not ship it (an earlier version of this test silently fell back to software and "passed": now a fallback fails the test).
- [x] Thread-affinity misuse surfaces as `IllegalStateException` (wrong thread, closed context, library shut down); virtual threads are refused for `makeCurrent`. A `makeCurrent` racing a dead holder's OS-thread exit retries briefly (found by the first Java run; `Thread.join()` returns before the OS thread has finished).
- [x] JVM shutdown: the hook stops native log callbacks first, then terminates the library; a JVM exiting with two live contexts, one current on a parked thread, exits in about 0.8 s on every JDK tried. The JNI log trampoline attaches native threads as daemons and never lets an exception unwind into native code; the FFM upcall catches `Throwable`.

**Status notes (Phase 7):**
- New: `bindings/java/` (sources, CMake), `scripts/fetch-java-deps.py` (pinned LWJGL/JavaFX from Maven Central with SHA-1 verification into the git-ignored `third_party/java/`), `cmake --preset java`. 8 Java tests on Windows/NVIDIA: JNI and FFM, library from disk and extracted from the jar, shutdown, LWJGL x2, JavaFX x2; plus the same `SelfTest` on 6 JDKs.
- Build-system lessons recorded in `bindings/java/CMakeLists.txt`: stages must be separate targets (a shared custom-command OUTPUT is re-run in parallel by each target under Visual Studio), and multi-entry classpaths must go in `@argfiles` (`;` does not survive CMake quoting on Windows).
- **Not done / not verified:** the Java tests have not run on Linux or macOS (the JNI glue is only compile-checked on Linux with gcc `-Werror`; I could not install a JDK in the WSL distro); JavaFX's OpenGL pipeline alongside Tessera is therefore untested (the roadmap's riskiest coexistence case, and Phase 3's open exit criterion); no packaging for Maven Central; the package name is a placeholder; Java 8-10 are unsupported (needs `Cleaner`, `Runtime.version()`).

**Exit:** sample app: JavaFX window + background Tessera/LWJGL rendering thread running side by side on all OSes. **Met on Windows** (Direct3D and software JavaFX pipelines, NVIDIA, both bindings' JavaFX runs used JNI on JDK 25); **not yet on Linux or macOS**.

## Phase 8 — Interop & advanced (optional / post-1.0)

- [~] **Interop primitives instead of an interop API** (ADR 0010). `tesseraGetNativeHandles` returns the real `HGLRC`/`HDC`/`HWND`, `Display*`/`GLXContext`/drawable, `EGLDisplay`/`EGLContext`/`EGLConfig`, `CGLContextObj` (borrowed), and `tesseraGetProcAddress` already resolves extension entry points (`wglDX*NV`, `eglCreateImage`, ...). Verified end to end for **WGL_NV_DX_interop2**: `test_dxinterop` has OpenGL render into a texture that is a Direct3D 11 texture, then reads the same memory back through D3D11 and checks the colour (twice, so the handoff is shown to repeat). Handle identity is tested on WGL, GLX and EGL (the platform reports the same context as current). **Not done:** dma-buf export (needs EGL image + fence code and a consumer to test against), IOSurface (needs a Mac), Vulkan `GL_EXT_memory_object` (needs a Vulkan device and semaphores). No wrapper API was added for any of these: nothing in this repo consumes one yet, and the handles are what a consumer needs. CGL handle code is compiled and unit-tested only through the fake.
- [x] **Device selection**: `TESSERA_EGL_DEVICE` accepts an index, a DRM render node (`/dev/dri/renderD128`) or a PCI address (`0000:01:00.0`, resolved through `/dev/dri/by-path`). An explicit choice is never silently replaced by another GPU (previously a non-matching index fell through to the default display). Tested: the refusal paths and index 0 under WSLg Mesa. **Untested:** matching by render node / PCI on a machine with several GPUs (this machine exposes one). **Not implemented:** UUID selection. GLX has no equivalent (the X server decides).
- [~] **Native-window attach**: `tesseraCreateContextForWindow(config, window, flags, &ctx)` on WGL (`HWND`) and GLX (`Window`); `UNSUPPORTED` on EGL/CGL. Ownership rules, tested on the real GPU on Windows: the window is borrowed and never destroyed, resized or shown; an existing pixel format is adopted and checked (strict mode rejects a mismatch); a missing one is set **only** with `TESSERA_ATTACH_ALLOW_PIXEL_FORMAT`, otherwise the call fails and the window is left untouched (mutation-checked: removing the check fails the test); a window destroyed under a live context gives `PLATFORM` errors, not a crash. Rendering and presenting verified by reading back the back buffer on WGL (NVIDIA) and GLX (Mesa/WSLg). **Not done:** `wl_surface` (no Wayland backend), `NSView` (needs a Mac), EGL window surfaces, resize handling (the window owner's job).
- [x] **Context pooling**: `tesseraPrewarmContexts(config, n)` + `tesseraPooledContextCount()`. Only never-used contexts are pooled, so a pooled context is indistinguishable from a new one; a create with an identical request and no `shareWith` takes one. Measured here: cold create 28 ms (WGL) / 28-32 ms (Mesa), pooled 0.015 / 0.008 ms. At most 64; emptied by `tesseraTerminate`; concurrent takers tested (fake backend under ASan/UBSan/TSan, real drivers on WGL, GLX, EGL). Deliberately **not** done: recycling used contexts (would leak GL state between unrelated users), automatic refill, a Java API.
- [ ] Vulkan interop (`GL_EXT_memory_object`, semaphores): not started.
- [ ] Java binding for the Phase 8 calls (`getNativeHandles`, `prewarm`, window attach): not started. The C ABI is now v0.5.0 (additive; nothing existing changed).

**Status notes (Phase 8):**
- Verified: Windows (RTX 3060, `ci-gpu` 5/5 including `dxinterop`); Ubuntu 22.04 under WSLg (GLX and EGL, Mesa) 8/8; ASan/UBSan and TSan clean (6/6 each, no GPU tests; they cover the pool through the fake backend).
- Found by the tests: a visual can map to several GLX framebuffer configurations (single- and double-buffered); attach now prefers the one matching the requested buffering instead of the first.
- GLX attach reads only the `visual` member of an opaque `XWindowAttributes` buffer at a fixed LP64 offset; 32-bit X11 returns `UNSUPPORTED`.
- CI workflow not updated for `dxinterop` (labelled `gpu`, skips with 77 where unavailable) and, as before, never run.

## Phase 9 — Hardening & release

- [ ] Fuzzing of config parsing, `getProcAddress` names, extension-string parsing.
- [ ] Soak tests: 24h create/destroy/switch loops with leak tracking (GDI/USER handles, FDs, X resources, Mach ports).
- [ ] Hardware matrix: NVIDIA, AMD, Intel, Apple Silicon, Mesa llvmpipe, ANGLE; record results in `docs/compat.md`.
- [ ] Coexistence matrix (automated where possible): JavaFX {d3d, es2, sw, metal} × OS × Tessera {onscreen-hidden, offscreen, surfaceless, shared}.
- [ ] Security review: input validation on all handles/pointers/sizes; no unbounded allocations from untrusted strings.
- [ ] Docs: API reference, threading guide, "living with JavaFX/AWT" guide, troubleshooting, ABI stability policy.
- [ ] Packaging: Maven Central artifact with per-platform natives, CMake `find_package` config, vcpkg/conan recipes.
- [ ] 1.0 release: freeze ABI v1.

---

## Coexistence checklist (applies to every change)

- [ ] Does it change anything process-global (DPI awareness, error handlers, signal handlers, locale, env vars, working directory, COM init, `timeBeginPeriod`, AppKit app state)? → Don't, or scope and restore.
- [ ] Does it call `make current`? → Wrapped by the foreign-context guard.
- [ ] Does it create a native window/display/connection? → Own it, name it uniquely, destroy only ours.
- [ ] Does it call `Terminate`/`Close`/`Unload` on something shared? → Only if we created it.
- [ ] Which thread may call it, and is that enforced?
- [ ] Does teardown work when the host app has already torn down its own GL?
- [ ] Is there a test with a foreign context current on the calling thread before and after the call?

## Milestone summary

| Milestone | Phases | Outcome |
|---|---|---|
| M1 – Alpha | 0, 1, 2 | Windows WGL contexts via C API, tested alongside JavaFX |
| M2 – Beta | 3, 4 | Linux GLX + EGL (headless, Wayland, GLES) |
| M3 – RC | 5, 6, 7 | macOS, robustness features, Java/LWJGL bindings |
| M4 – 1.0 | 8 (partial), 9 | Hardened, documented, ABI frozen |

## Open questions

- ~~Minimum supported JDK / LWJGL version?~~ Resolved in ADR 0009: Java 11+ (JNI), FFM on 22+; LWJGL 3.3.x tested.
- Is on-screen rendering to caller-supplied windows in scope for 1.0, or offscreen/hidden only?
- Do we ship our own ANGLE/Mesa builds, or only detect system ones?
- Minimum GL version floor (3.2 core? 2.1 compat?) and GLES floor (2.0?).
