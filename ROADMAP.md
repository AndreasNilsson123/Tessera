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
- Context creation is ~29 ms on the test machine (mostly driver cost, not profiled); pooling is in Phase 8.
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

- [ ] Full attribute matrix tested per backend: version × profile × flags × samples × robustness.
- [ ] Reset notification: poll wrapper `tesseraGetResetStatus` (`glGetGraphicsResetStatus`), "context lost" event, recreate helper preserving share groups.
- [ ] Driver-loss/TDR handling (Windows), GPU hot-unplug (Linux), display changes: surfaced as `TESSERA_CONTEXT_LOST`, never a crash.
- [ ] Share-group management: tracked groups, destruction ordering rules, refcounts so a context in use by another thread is deferred-destroyed, not UB.
- [ ] Debug output plumbing: `KHR_debug` callback forwarded to the log callback, with callback lifetime safety across context destroy.
- [ ] Quirks database (`quirks.def`): driver/renderer/version matched workarounds (e.g. Intel core-profile bugs, `wglGetProcAddress` junk values, Mesa software limits), each entry with a regression test or linked issue.
- [ ] Extension/version string parsing robust to malformed driver output; `GL_NUM_EXTENSIONS` path for core profile.
- [ ] Resource limits: max contexts per process warnings, handle table growth caps.

**Exit:** fault-injection tests (fake loader returning NULLs, failing mid-creation) leave no leaks and restore foreign state every time.

## Phase 7 — Java / LWJGL integration

- [ ] Java API (`tessera-java`): `TesseraContext`, `TesseraConfig` builder, `AutoCloseable`, `Cleaner` as safety net (never rely on it).
- [ ] LWJGL bridge: expose `getProcAddress` as an `org.lwjgl.system.FunctionProvider`, and a `makeCurrent()` that works with `GL.createCapabilities()`/`GL.setCapabilities()`; document thread-local capabilities handling.
- [ ] Panama (FFM) bindings generated with `jextract`, checked into the repo; JNI fallback built from same header.
- [ ] Native library loading: extract-and-load with checksum, unique temp dir, version-qualified name so two classloaders/apps don't clash; `System.load` only, never `System.loadLibrary` search-path surprises.
- [ ] Classloader / multi-instance safety: multiple copies of the Java jar loaded by different classloaders in one JVM must not double-init native state (the native refcounted init from Phase 1 covers this).
- [ ] JavaFX integration doc + samples: Tessera context on a worker thread rendering to a texture/FBO, read back into `WritableImage`/`PixelBuffer`, never touching the JavaFX render thread's context.
- [ ] Thread-affinity checks surfaced as Java exceptions (`IllegalStateException`) instead of native crashes.
- [ ] JVM shutdown ordering: shutdown hook-safe teardown; no native callbacks into dead JVM (callback trampolines invalidated first).

**Exit:** sample app: JavaFX window + background Tessera/LWJGL rendering thread running side by side on all OSes.

## Phase 8 — Interop & advanced (optional / post-1.0)

- [ ] Shared-texture export for zero-copy handoff: WGL_NV_DX_interop2, dma-buf (Linux), IOSurface (macOS).
- [ ] Vulkan interop (`GL_EXT_memory_object`, semaphores).
- [ ] GLX/EGL "headless render node" selection by PCI bus ID / UUID.
- [ ] Native-window attach (render into a caller-supplied `HWND`/`Window`/`wl_surface`/`NSView`) with strict ownership rules (we never `SetPixelFormat` a foreign window unless the caller declares it fresh).
- [ ] Context pooling for apps that create/destroy many short-lived contexts.

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

- Minimum supported JDK / LWJGL version? (drives Panama vs JNI priority)
- Is on-screen rendering to caller-supplied windows in scope for 1.0, or offscreen/hidden only?
- Do we ship our own ANGLE/Mesa builds, or only detect system ones?
- Minimum GL version floor (3.2 core? 2.1 compat?) and GLES floor (2.0?).
