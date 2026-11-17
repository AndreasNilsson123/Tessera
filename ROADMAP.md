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
- [ ] CI matrix: Windows (MSVC, clang-cl), Linux (gcc, clang; X11 + Wayland), macOS (arm64 + x64). Software GL (Mesa llvmpipe / WARP-like) for headless runners.
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

- [ ] Bootstrap dummy window + legacy context to resolve `wglCreateContextAttribsARB` / `wglChoosePixelFormatARB`; destroy it afterwards; keep the saved/restored foreign context discipline throughout.
- [ ] Own window class with per-module unique name (`Tessera_<hmodule>_<n>`), unregistered on terminate; hidden, tool-window, never activated, `WS_EX_NOACTIVATE`, no taskbar entry.
- [ ] Dedicated message-pump strategy: windows are created on the thread that will pump them (document and enforce); optional internal "GL host thread" mode.
- [ ] Pixel-format negotiation via `WGL_ARB_pixel_format`; fallback ladder to legacy `ChoosePixelFormat`; MSAA, sRGB (`WGL_ARB_framebuffer_sRGB`), float (`WGL_ARB_pixel_format_float`).
- [ ] Context creation: core/compat/ES profile, debug, forward-compat, robust-access + reset notification, no-error (`KHR_no_error`), release behavior, context priority.
- [ ] Shared contexts (`wglShareLists` / attribs-share) with creation-order validation.
- [ ] Offscreen: hidden-window path, `WGL_ARB_pbuffer` path, and surfaceless/FBO-only mode (`make current` with no drawable where supported).
- [ ] Swap interval (`WGL_EXT_swap_control` / `_tear`), `SwapBuffers`, DWM-composition quirks.
- [ ] Multi-GPU: enumerate adapters; choose via `WGL_NV_gpu_affinity`, `WGL_AMD_gpu_association`, or DXGI preference; document that exporting `NvOptimusEnablement` must be done by the host EXE and is outside a DLL's control.
- [ ] DPI: **never** call `SetProcessDPIAware`/`SetProcessDpiAwareness...` (process-global; would break JavaFX scaling). Report DPI, don't change it.
- [ ] ANGLE / Mesa-on-Windows (`libEGL.dll`) detection through the EGL backend (Phase 4).

**Exit:** create/destroy 10k contexts in a loop with no handle/GDI leaks (verified with GDI/USER object counters); coexistence test with a running JavaFX app (D3D pipeline and `-Dprism.order=sw`) shows no interference.

## Phase 3 — Linux X11 (GLX) backend

JavaFX on Linux uses the `es2` pipeline (GLX-based) so **real conflicts are possible here**.

- [ ] **Own `Display*` connection** (`XOpenDisplay`) per Tessera instance; never reuse AWT/JavaFX's. Close only ours.
- [ ] Xlib threading: do not require `XInitThreads` ourselves from a library constructor; detect whether it was called (best effort) and prefer an XCB-based or per-thread connection model so we are safe either way. Install no global `XSetErrorHandler` permanently — scope a handler around calls that can raise `BadMatch`/`GLXBadFBConfig`, chained to the previous handler, thread-guarded (X error handler is process-global, so use a mutex and restore).
- [ ] FBConfig selection via `glXChooseFBConfig`, visual matching, `GLX_ARB_create_context(_profile)`, `_robustness`, `_no_error`, `_es2_profile`.
- [ ] Offscreen: pbuffer, hidden unmapped window with colormap of our own, and drawable-less `glXMakeContextCurrent(None, None, ctx)` where supported.
- [ ] Swap control (`GLX_EXT/MESA/SGI_swap_control`), `glXSwapBuffers`.
- [ ] GLVND vs legacy `libGL.so` handling; `libGLX.so.0` preferred.
- [ ] Context sharing correctness across displays (shared contexts must share a server); document and validate.

**Exit:** coexistence test with a JavaFX es2 pipeline window + Tessera worker contexts running for hours (Xvfb + real X, with `XSetErrorHandler` interference tests).

## Phase 4 — EGL backend (Wayland, headless, GBM, ANGLE, GLES)

- [ ] Platform selection via `EGL_EXT_platform_base` (`eglGetPlatformDisplayEXT`): X11, Wayland, GBM, device (`EGL_EXT_platform_device`), surfaceless, ANGLE.
- [ ] Own `EGLDisplay` / `wl_display` connection; reference-counted `eglInitialize`/`eglTerminate` safe even if another component initialized the same `EGLDisplay` (EGL initialization is refcount-less: **never call `eglTerminate` on a display we did not create/own**; prefer a private `wl_display`/`EGLDevice` display).
- [ ] Per-thread EGL state handling: save/restore `eglGetCurrentContext/Surface(READ/DRAW)/Display` and `eglBindAPI` (the bound API is per-thread state!).
- [ ] Contexts: desktop GL and GLES 2/3.x, robustness, protected content, priority, `EGL_KHR_create_context_no_error`.
- [ ] Surfaceless (`EGL_KHR_surfaceless_context`), pbuffer, and `EGL_MESA_platform_surfaceless` headless rendering; device enumeration for headless GPUs (`EGL_EXT_device_enumeration`/`_query`).
- [ ] Fence sync (`EGL_KHR_fence_sync`) + `EGL_ANDROID_native_fence_sync` / dma-buf export (`EGL_MESA_image_dma_buf_export`) for later interop (Phase 8).
- [ ] ANGLE and Mesa llvmpipe validated as first-class targets.

**Exit:** same conformance suite runs on EGL (Mesa surfaceless) in CI headless.

## Phase 5 — macOS backend (CGL / NSGL)

JavaFX on macOS uses Metal (recent) or ES2/NSGL (older); AppKit threading rules apply.

- [ ] CGL-only path (no AppKit) for offscreen/headless contexts: `CGLChoosePixelFormat`, `CGLCreateContext`; works from any thread, avoids all main-thread requirements.
- [ ] NSOpenGLContext path for on-screen when required, with explicit "must be created on main thread" enforcement (dispatch helper, never block the main thread from the main thread).
- [ ] Core 3.2/4.1 profile only (macOS limitation), GLES via ANGLE-backed EGL; clear errors for unsupported requests.
- [ ] Deprecation hygiene: isolate all deprecated API usage; Metal-backed ANGLE as the forward path.
- [ ] No `NSApplication` creation or run-loop interference; Tessera must work when AWT/JavaFX already owns the app.

**Exit:** headless CGL tests on CI runners; manual coexistence matrix with JavaFX on macOS.

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
