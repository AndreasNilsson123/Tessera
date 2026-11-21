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

## Status at a glance

Phases 0-9 are **built and tested on the one machine this was developed on** (Windows 11 + NVIDIA RTX 3060 Laptop, driver 546.30; Ubuntu 22.04 under WSLg with Mesa 23.2 D3D12). Everything that needs other hardware, other operating systems, a remote CI, or a decision from the project owner has been moved to **Phase 10**; ideas for new capabilities are in **Phase 11**. Nothing below is claimed beyond what `docs/compat.md` lists as run.

| Phase | What shipped | Detail |
|---|---|---|
| 0 Foundations | CMake layout, hidden-by-default symbols, ABI policy, presets (`ci`, `asan-ubsan`, `tsan`, `tidy`, `java`), clang-format/tidy, GitHub Actions workflow (never run) | `docs/abi-policy.md`, `docs/ci.md` |
| 1 Core API | Error model, generation-checked handles, refcounted init/terminate, foreign-context guard, loader, config negotiation, `getInfo` read back from the driver | ADR 0001, 0003 |
| 2 Windows (WGL) | Bootstrap + ARB context/pixel-format negotiation, own window host thread, pbuffer/hidden-window/none surfaces, sharing, swap interval | ADR 0004 |
| 3 Linux (GLX) | Own `Display*`, scoped X error handler, FBConfig/ARB_create_context, pbuffer/hidden window, thread-exit release | ADR 0005 |
| 4 EGL | Surfaceless/device/default platforms, per-API state save/restore, never `eglTerminate`, GLES 2/3 and desktop GL, backend selection | ADR 0006 |
| 5 macOS (CGL) | Whole backend, passing against a fake `OpenGL.framework` with fault injection | ADR 0007 |
| 6 Robustness | Deferred destruction, reset status + lost state + recreate, debug-output forwarding, 192-combination attribute matrix, fault sweep | ADR 0008 |
| 7 Java | JNI (11+) and FFM (22+) in one multi-release jar, LWJGL adapter, JavaFX coexistence tests | ADR 0009, `docs/java.md` |
| 8 Interop & advanced | Native handles (GL<->D3D11 shared texture proven), window attach (WGL, GLX), context pre-warming, GPU selection by index/render node/PCI | ADR 0010, `docs/interop.md` |
| 9 Hardening | Seeded fuzzing under sanitizers (found and fixed an over-read), leak-tracked soak (10,200 cycles flat on Windows), security self-review, threading/coexistence/troubleshooting/compat docs, verified CMake package | `docs/security-review.md`, `docs/compat.md` |
| post-9 Diagnostics | `tesseraProbe` (ABI 0.6.0) and the `tessera-diag` tool | `docs/diag.md` |
| post-9 Context-loss events | `tesseraSetContextLostCallback` / `tesseraIsContextLost` (ABI 0.9.0), Java `setContextLostListener` / `isLost` | ADR 0011 |
| post-9 Swap control and pacing | `tesseraGetSwapControl` (ABI 0.10.0), `tessera-diag --bench-swap`, Java `swapControl()` | ADR 0012 |
| post-9 Driver features | context priority, EGL device selection by UUID, `XInitThreads` detection (ABI 0.11.0) | ADR 0013 |
| post-9 Other toolkits' windows | `tesseraCreateContextForNativeWindow`, Wayland and X11 windows through EGL, `tesseraResizeWindow` (ABI 0.12.0) | ADR 0014 |
| 12 Selection depth | **Planned, not started:** format ranking and enumeration, relaxation and acceleration reporting, quirk table, adapter inventory, attempt trace | see Phase 12 |

Verified here: WGL (NVIDIA), GLX and EGL (Mesa), Java JNI+FFM on Windows (JavaFX D3D and software pipelines), Java JNI on Linux (JavaFX software **and OpenGL/es2** pipelines, LWJGL; Mesa D3D12 via WSLg and llvmpipe), ASan/UBSan/TSan on everything that needs no GPU.

**CI (GitHub Actions) has run** and everything passes except the two macOS jobs, which never got a runner (relabelled `macos-latest`; result unknown). Passing: MSVC Debug/Release, gcc 13 Debug/Release, clang 18, ASan+UBSan, TSan, WGL on the hosted GDI-Generic GL 1.1 renderer, GLX+EGL on Xvfb/llvmpipe, Java on JDK 21 (JNI) and 25 (FFM), lint (clang-format 18.1.8 + clang-tidy). Running it found and fixed: a stack-overflow in the scoped X error handler (JavaFX es2), a crash at thread exit after `tesseraTerminate` (GLX connection now kept open), best-effort requests returning a context below the requested minimum version (GDI GL 1.1), plus several warnings that only newer compilers report. All of those jobs are now blocking. See Phase 10 for everything else.

### Decisions that departed from the original plan (kept because they explain the code)

- Windows work happens on one private message-pumping host thread (always on), not an optional mode (ADR 0004).
- `XInitThreads()` is called by Tessera before opening its own connection (ADR 0005); never `eglTerminate` (ADR 0006).
- EGL has no native X11/Wayland/GBM platforms: Tessera renders to pbuffers or nothing and needs no window system connection.
- FFM bindings are hand-written, not `jextract`ed; the JNI glue is compiled into the same native library (ADR 0009).
- `tesseraDestroyContext` of a context current on another thread succeeds and defers the teardown to that thread (changed API in 0.4.0).
- Interop is "hand out the native objects", not a wrapper API (ADR 0010).
- No quirks database yet: workarounds are one-off code in the backends. Phase 12.3 turns them into a tested, switch-off-able table.

---

## Phase 10 — Finish what is unverified (the road to 1.0)

Everything here was implemented-but-untested, deliberately skipped, or needs something the development machine does not have. Grouped by what is needed to close it.

### 10.1 Needs other hardware or operating systems

- [~] **macOS**: the CGL backend has now run against the **real `OpenGL.framework`** on GitHub's hosted `macos-latest` VM (19 Nov 2026): `test_cgl` passes, `tessera-diag` shows Apple Software Renderer (OpenGL 2.1 legacy, 4.1 core, forward-compatible only), the Java tests pass on JDK 21 (JNI) and 25 (FFM) including LWJGL, JavaFX software and GLFW (with `-XstartOnFirstThread`). Real findings: a link error in two test executables, and a "relaxed" log line that never fired because CGL returns the closest pixel format instead of failing. **Not done:** a real Mac (Apple Silicon and Intel) with a GPU, JavaFX on Metal, hardware-specific behaviour, `NSView` attach.
- [ ] **Other GPU vendors**: AMD, Intel (Windows and Linux), NVIDIA proprietary on Linux. Re-run the attribute matrix, window attach, `dxinterop` and the soak; fill `docs/compat.md`.
- [ ] **Real X server / non-WSL Linux**: a real Xorg session with a hardware driver, non-GLVND `libGL`, 32-bit. (Xvfb + llvmpipe and clang are done in CI.) Swap-control extensions are untested because neither WSLg nor llvmpipe advertises them.
- [~] **Multi-GPU**: render-node, PCI and **UUID** (`EGL_EXT_device_persistent_id`) matching in `TESSERA_EGL_DEVICE` is implemented and tested against faked multi-GPU device lists (index range, by-path resolution, any-case UUIDs, partial attributes, ambiguity, missing extension); the one real EGL device of WSLg reports neither a render node nor a UUID, so the real-device positive cases have not run. A machine with two GPUs is still needed.
- [~] **Java on Linux and macOS**: Linux is done (JDK 21 locally and in CI, JNI; JDK 25 FFM in CI). macOS: not run.
- [x] **Other GL users next to Tessera (Bundle 1)**: GLFW through LWJGL (guest guarantees, concurrent rendering and churn, attach to a `GLFW_NO_API` window, terminate with a live Tessera context; Windows WGL and Linux GLX, Mesa D3D12 and llvmpipe; both bindings on Windows), Java2D's OpenGL pipeline (AWT/Swing; `WGLGraphicsConfig` and `GLXGraphicsConfig`), and a longer JavaFX run (`-DTESSERA_JAVA_STRESS=ON`: 6000 frames and thousands of churned contexts on d3d / es2, 0 wrong). **Found and fixed two real bugs:** a Wayland/EGL host (GLFW's Wayland platform) made Tessera's GLX context creation fail with `BadAccess` and its context was never restored (Mesa keeps one current context per thread across GLX and EGL; both backends now capture/restore and release the other API's state, `src/platform/common/cross_api.*`; native tests with a host context of the other API, mutation-checked), and `Tessera.procAddress` returned 0 before Tessera had created a context, so LWJGL could not resolve functions for a host's context. Still not tested: SDL, Qt, macOS, a Wayland compositor in CI (checked by hand on WSLg), hybrid-GPU laptops, runs of hours.
- [~] **JavaFX `es2` (OpenGL) pipeline next to Tessera**: passes on Linux with Mesa (D3D12 and llvmpipe, 25/25 runs after the fix; ~50% crashed before it, which is how the X error handler bug was found). Needs `-Dprism.forceGPU=true` because JavaFX does not allow-list Mesa. Not done: a real hardware driver (NVIDIA/AMD/Intel on Xorg), a run of hours, macOS (Metal/ES2).
- [ ] **Driver issue to watch:** on WSL's Mesa D3D12 driver (Mesa 23.2 over NVIDIA), having 12 GLX contexts alive at once made a later, unrelated `glXDestroyContext` (of a window-attached context) crash inside `swrast_dri.so` (the Gallium megadriver). llvmpipe, NVIDIA/WGL and EGL did not; ASan was clean; a raw-libGL reproduction was not achieved, so it is attributed to the driver but not proven. Tests keep at most 8 contexts alive at once. If it reproduces on a native driver, it is Tessera's to fix. **Reproduced again on 19 Nov 2026 through Tessera:** the Java layer fuzzer (`NativeFuzz`) left a dozen GLX contexts alive and glibc reported heap corruption in about 90% of seeds; ASan over Tessera's own code was clean, and with the live-context count bounded 0 of 30 seeds failed, which is what points at the driver. Still not reproduced against a raw libGL, still not seen on llvmpipe or NVIDIA.
- [ ] A real GPU reset (TDR / device removal) with a robust context: `GUILTY`/`INNOCENT`/`UNKNOWN`, sticky lost state and recreate are tested only on a fake backend. Also hot-unplug and display changes.

### 10.2 Implemented but never exercised

- [~] Robust access, reset notification, `KHR_no_error`, release-flush: the strict-request matrix on NVIDIA/WGL, Mesa GLX, Mesa EGL and Apple's software CGL is recorded in `docs/compat.md` (NVIDIA grants all four; Mesa GLX refuses robustness; CGL refuses all). Context priority is implemented for GLX and EGL (ADR 0013), WGL has no extension; no driver here grants it. **Not verified:** the granted paths of `GLX_EXT_context_priority` / `EGL_IMG_context_priority`, and a real reset with a robust context.
- [~] DWM swap-interval behaviour: measured on a composited window (120 Hz: vsync on 8.32 ms/frame, off 0.3-0.5 ms, all of 0/1/2/-1 read back); `GLX_EXT/MESA/SGI_swap_control` selection is unit-tested, but no server here advertises them (ADR 0012). Exclusive fullscreen, variable refresh and Optimus are untested.
- [~] WGL share-order validation is not needed (sharing is requested at creation through `wglCreateContextAttribsARB`, and the legacy path calls `wglShareLists` on fresh contexts); context priority is implemented for GLX and EGL and does not exist for WGL (ADR 0013).
- [x] Fault-injection of the WGL, GLX and EGL failure paths (`tests/test_fault.cpp`): every failable driver call that goes through the backends' function tables (93 / 218 / 301 on WGL / GLX / EGL) is failed once and then every call from there on, against the real driver through the public API, checking result codes, handles, the other library's context and native-object leaks (contexts, pbuffers, windows, colormaps). **It found no bug.** A mutation (skipping a release) makes it report 183 problems, so it does detect leaks; also clean under ASan/UBSan. Not injectable: direct Win32 calls (`CreateWindowEx`, `SetPixelFormat`) and asynchronous X errors.
- [x] The Linux memory growth seen in the soak: **not Tessera's.** With a fake backend Tessera is flat over 270,000 contexts on 3 threads (guard in `test_core`); the same create/use/destroy loop written straight against libX11/libGL grows the same way on Mesa's D3D12 driver with 3 threads (about 2 KB per context, 6.5 MB per 3000 contexts) and not on llvmpipe or with one thread. Not tested: a non-Mesa driver.
- [x] CI: the workflow has run; see the status above. Open: the macOS jobs, and a scheduled (nightly) soak.

### 10.3 Not implemented (known gaps)

- [~] Drawable-less `SURFACE_NONE`: **GLX done** (the context is made current without any drawable; verified on Mesa D3D12 and llvmpipe including offscreen FBO rendering; a hidden window is the fallback for a driver that refuses, exercised by the fault sweep). **WGL is impossible**: `wglMakeCurrent(NULL, rc)` fails with `ERROR_INVALID_HANDLE` on the NVIDIA driver, so WGL keeps its internal 1x1 window (documented in `tessera.h`).
- [x] Detecting whether the host already called `XInitThreads` (ADR 0013): libX11's `_Xglobal_lock`, logged, tested in two processes.
- [~] GPU selection on Windows (ABI 0.7.0, `adapterLuid` / `gpuPreference`, Java `gpuPreference()` / `adapterLuid()`): **verification, not switching.** A process cannot make Windows load a different GPU's OpenGL driver, so Tessera resolves the adapter through DXGI (`EnumAdapterByGpuPreference`) and checks that the OpenGL driver it got belongs to it by PCI vendor; a mismatch (or several adapters of one vendor, which cannot be told apart) fails with `NO_MATCHING_CONFIG` instead of silently using another GPU. Other backends refuse the request (`UNSUPPORTED`; EGL has `TESSERA_EGL_DEVICE`). Tested here on the one real adapter plus the Basic Render Driver (mutation-checked). **Not implemented:** actually targeting a GPU with `WGL_NV_gpu_affinity` / `WGL_AMD_gpu_association` (this GeForce exposes neither, so it could not be tested); **not tested:** a hybrid Intel+NVIDIA/AMD machine, where the check matters most. Document for hosts: `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance` must be exported by the EXE.
- [~] ANGLE through the EGL backend on Windows: done (ADR 0015: absolute-path loading with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`, ES contexts, `HWND` surfaces, a CPU device), tested on the development machine. Mesa on Windows and Metal-backed ANGLE on macOS (CGL is deprecated) are not done.
- [~] Interop beyond the handles: **EGLImage between two contexts verified** on Linux (`test_eglimage`: Mesa D3D12 and llvmpipe), after fixing the EGL backend not resolving `egl*` names through `tesseraGetProcAddress`. Not done: dma-buf export/import with fences (the Mesa drivers here lack the extensions), IOSurface (macOS), Vulkan `GL_EXT_memory_object` + semaphores. Each needs a consumer to test against.
- [~] Window attach: EGL window surfaces (X11) and Wayland `wl_surface` done and tested on WSLg (ADR 0014, ABI 0.12.0). macOS `NSView` is designed only.
- [~] The quirks database: now planned as Phase 12.3. The rule is unchanged (no entry without a reproducible symptom and a test); the first entries are the workarounds already in the code.

### 10.4 Release engineering

- [x] **Java bindings for the Phase 8 and probe calls** (`Context.nativeHandles`, `Tessera.prewarm`, `Context.createForWindow`, `Tessera.probe`; JNI and FFM): tested on Windows (both bindings, plus LWJGL identity checks against `WGL`) and on Linux (JNI, `GLX` identity check). Window attach is covered by argument validation only in Java (a real window needs a toolkit; the native tests cover it). The package is now the real one, `io.github.andreasnilsson123.tessera` (Maven `io.github.andreasnilsson123:tessera`).
- [~] **Maven artifacts**: `scripts/assemble-release.py` assembles one multi-platform multi-release jar (natives for Windows, Linux, macOS, `module-info`), sources/javadoc jars, the LWJGL adapter, POMs, a CycloneDX SBOM, checksums and a Maven bundle, reproducibly; verified on a Windows+Linux pair (the assembled jar runs `SelfTest` from its embedded natives on both, and as a named module). `release.yml` drives it from CI with a draft GitHub release. **Not done:** the workflow itself has not run; GPG signing and the Central Portal upload are untested; the macOS native has never run; nothing is published (`docs/releasing.md`).
- [~] **Conan recipe verified** (`conan create` builds, installs and runs a consumer on Linux; also a CI job). **vcpkg port written but untested** (vcpkg could not be run here); neither is submitted to a public registry.
- [~] API reference: `tessera.h` is the C reference (documented in place); Javadoc is generated into the release (`-javadoc.jar`). The C header's comments are Doxygen-style and `doxygen docs/Doxyfile` builds an HTML reference with warnings as errors (verified locally; a CI lint step and a release asset `tessera-<v>-c-api-docs.zip` exist but the release workflow has not run).
- [~] Coverage-guided fuzzing: libFuzzer targets for the C API (`fuzz_api`) and the version parser (`fuzz_glversion`), `fuzz` preset, CI job (1 min) and nightly (10 min); 15.9 M inputs locally, no findings (one harness bug fixed). Not done: fuzzing the JNI glue and Java layer, an independent security review.
- [~] Soak: `nightly.yml` runs 30 min on Linux and 15 min on Windows (software GL) plus the fault sweeps. The 24 h run on real hardware and X resource / Mach port tracking are not done.
- [ ] Coexistence matrix filled in: JavaFX {d3d, es2, sw, metal} x OS x Tessera {hidden, offscreen, surfaceless, shared}. Only d3d and sw on Windows are done.

### 10.5 Decisions only the project owner can make (these gate the 1.0 freeze)

- [ ] Is on-screen rendering into caller-supplied windows part of 1.0? (It exists for WGL/GLX; it is the biggest API surface still open.)
- [ ] Minimum GL and GLES version floor (3.2 core? 2.1 compat? GLES 2.0?).
- [ ] Do we ship our own ANGLE/Mesa builds, or only detect system ones?
- [ ] Real package/group names, license, release cadence.
- [ ] **Freeze ABI v1.** Deliberately not done: the ABI changed as recently as 0.7.0 and only one GPU vendor has been tested. Freezing is a one-line version change; living with a wrong freeze is not. Suggested gate: 10.1 (at least AMD or Intel on Windows, and macOS) plus the decisions above plus CI green on a real remote **plus the Phase 12 ABI batch (0.10.0)**, so the freeze is not followed by additive-but-needed fields such as `info.relaxed`, adapter ids and the attempt trace. Also decide here whether "minimum version" means a floor or an exact request (12.2).

**Exit for 1.0:** the 10.1 items that have hardware available are green and recorded in `docs/compat.md`; 10.5 decided; CI has run green at least once; ABI frozen and tagged.

---

## Phase 11 — Feature candidates (post-1.0 unless noted)

New capabilities worth considering, with a recommendation. Items 1, 2, 5 and 7 are done; the rest are not started. Ordered by value per effort.

1. ~~**`tessera-diag` command-line tool**~~ **Done** (`docs/diag.md`): text and JSON report, per-backend strict-request matrix, runs on Windows (WGL) and Linux (GLX, EGL under WSLg); macOS/CGL untested like the rest of CGL.
2. ~~**Capability probe API**~~ **Done**: `tesseraProbe` (ABI 0.6.0), tested on WGL/GLX/EGL with a foreign context current, on the fake backend, fuzzed, and under ASan/UBSan/TSan. Java: `Tessera.probe` (JNI and FFM).
3. **Software/headless fallback backend** (OSMesa or a bundled llvmpipe/WARP path): guarantees `createContext` works on machines with no GPU or broken drivers (CI runners, remote desktops, containers). Related to the "do we ship Mesa" decision.
4. **Windows multi-GPU selection** (10.3) exposed as a config field (`adapterPreference`: default / high-performance / low-power / by LUID), with the same "never silently another GPU" rule used for EGL.
5. ~~**Context-loss recovery helper**~~ **Done** (ABI 0.9.0, ADR 0011): a once-per-context lost callback and `tesseraIsContextLost` (Java `setContextLostListener`, `isLost`); the "recreate and re-share" helper is the existing `tesseraRecreateContext` called from the callback (an automatic in-library recreate would change handles under the application). Tested on the fake backend (mutation-checked); the Java upcalls' event delivery and a real GPU reset are not verified.
6. ~~**Frame pacing helpers**~~ **Done** (ABI 0.10.0, ADR 0012) for window-attached contexts: swap-interval negotiation reported back (what was applied), adaptive-sync availability, and a vsync-off benchmark mode. Only for attached windows; Tessera's own hidden drawables do not present.
7. ~~**Pool improvements**~~ **Done** (ABI 0.8.0): `tesseraPoolTopUp` / `tesseraGetPooledCount` / `tesseraPoolClear` (per-request counts; refilling is a call your own thread makes, Tessera starts no threads), Java `topUpPool` / `pooledContextCount(config)` / `clearPool`, tested on the fake backend and on WGL/GLX/EGL. Only never-used contexts, as before.
8. **Metal/ANGLE path on macOS** and **ANGLE on Windows/Linux** as an alternative GLES provider (10.3), once a loader policy is decided.
9. **More language bindings** over the C ABI, cheap because it is stable and struct-size-prefixed: C#/.NET (P/Invoke), Rust (`-sys` crate), Python (`ctypes`/`cffi`). Only worth it if there is a consumer; the Java layer is the template.
10. **Wayland-native window attach** and **EGL window surfaces** (10.3), if the answer to "on-screen in 1.0?" is yes.
11. **Release hardening**: SBOM, reproducible builds, signed releases, a nightly soak on a self-hosted GPU runner publishing the table to the repo.
12. **Out of scope on purpose** (keep it that way unless the principles change): creating or managing windows for the application, loading GL function pointers for the application beyond `getProcAddress`, wrapping GL itself, input handling. Tessera is the context layer; GLFW/SDL/LWJGL own the rest.

---

## Phase 12 — Context selection depth

Goal: make *choosing* a context as strong as *creating and surviving with* one. Today the negotiation is
declarative and verified (request → plan → create → read back → strict re-check), but the choice itself is
largely delegated to the driver (`configs[0]`, "the server sorts best-first"), failures are explained by one
message, driver knowledge is ad hoc, and the host cannot take part in the decision. These tasks close those gaps
without breaking the principles above: no threads, nothing process-global, nothing silently weaker.

Rules for every item: it lands with a fake-backend test (so it runs in CI without a GPU), a real-driver test where
one exists, and an entry in `docs/compat.md` for what was actually run. Items marked **ABI** change
`tessera.h` and must be batched into one ABI bump (0.10.0) *before* the 1.0 freeze (see 10.5).

### 12.1 Selecting the framebuffer format ourselves

Today: attributes are built per attempt and the first result wins; the relaxation order (samples → sRGB → depth)
is fixed and only logged.

- [ ] **Deterministic candidate ranking.** Stop trusting driver ordering. Fetch the candidate list per backend
  (`wglGetPixelFormatAttribivARB` loop, `glXGetFBConfigs`, `eglGetConfigs`, CGL pixel-format enumeration) and rank
  in one shared function: no missing required attribute, then smallest excess in colour/depth/stencil, exact sample
  count over larger, no config caveat (EGL `SLOW`/`NON_CONFORMANT`), native-visual-matching on GLX. Tested with a
  fake driver that returns the same set in shuffled and adversarial orders (result must not change).
- [ ] **Say what was relaxed.** Replace the log-only message with data: `TesseraContextInfo.relaxed` bitmask
  (samples, sRGB, depth, stencil, alpha, version, flags). Strict mode never sets it; best-effort always reports it.
  **ABI**
- [ ] **Relaxation priority.** Let the caller say what may be given up first (a small ordered list in the config,
  default = today's order) so a request that cares about depth more than MSAA can say so. **ABI**
- [ ] **Hardware vs. software awareness.** Classify what we got (WGL generic-format flag / GDI renderer, EGL caveat
  and renderer string, `llvmpipe`/`WARP`/`Basic Render` identities) into `TesseraContextInfo.accelerated`, and add a
  `TESSERA_FLAG_REQUIRE_ACCELERATED` request that fails with `NO_MATCHING_CONFIG` instead of returning GL 1.1 or a
  software rasteriser. (CI already hit "GDI Generic GL 1.1 returned to a request that wanted more"; this makes the
  distinction a first-class result instead of a version check.) **ABI**
- [ ] **Enumerate before you choose.** `tesseraEnumerateConfigs(config, out[], capacity, &count)`: the formats the
  driver offers that satisfy a request, with their bits/samples/sRGB/float/caveat, no context created. A
  `configIndex` in the (reserved) config space then selects one. `tesseraProbe` is rebuilt on top of it. **ABI**
- [ ] **Window-format handoff.** Attach currently *adopts* whatever format the host's window already has, or sets
  one only with `ALLOW_PIXEL_FORMAT`. Add the other direction: `tesseraChooseWindowFormat(config, …)` returns the
  native id the host must create its window with (WGL pixel-format index, X visual id + depth, EGL native visual
  id), so a toolkit can build the right window first and attach strictly. Tested with a window created by an
  unrelated library from the returned id. **ABI**

Exit: ranking is order-independent under the shuffle test; every relaxation appears in `info.relaxed`; a host can
choose a format, create its own window with it, and attach under STRICT with no pixel-format mutation.

### 12.2 Version and profile negotiation

Today: a ladder (4.6 → 3.2) exists only for "core/compat with no version"; an explicit version is tried exactly
once; ES is fixed to 2.0 when unspecified; every failed rung is a real driver call.

- [x] **ES ladder** (ABI 0.13.0, ADR 0016): unspecified ES walks 3.2, 3.1, 3.0, 2.0 on every backend that creates by version (EGL, WGL, GLX ES profiles); verified on NVIDIA/WGL (3.2), Mesa D3D12 (3.1) and llvmpipe (3.2). **Not verified:** GLX/WGL ES profiles on a driver that has only `*_create_context_es2_profile` with 3.x.
- [x] **"Minimum" means minimum**: the field is a floor (newest first, down to the request) and `TESSERA_FLAG_EXACT_VERSION` asks the driver for exactly it; the owner's decision (10.5) is still to be recorded, this is the recommendation of `docs/decisions-1.0.md`. Pinned by `test_core` (ladder shapes, every row) and `test_versions` (real drivers: floor = newest on WGL, GLX, EGL, software; exact = 3.3 on NVIDIA; Mesa may answer newer).
- [x] **Remember what worked**: `VersionMemory`, counted on a driver stand-in (5 calls the first time, 1 the second, a changed driver falls back to the full ladder); dropped on the last `tesseraTerminate`. Not shared across processes (on purpose).
- [x] **Map driver refusal codes in one table**: `create_errors.hpp` for WGL, GLX, EGL, CGL, a test per row.
  CI (21 Nov 2026, all green): `test_versions` ran on Windows (WGL on the runner's software GL; ANGLE/WARP now reports ES 3.1 where it reported 3.0 before, the ladder at work), Linux llvmpipe (GLX and EGL) and macOS (CGL: 4.1 for "at least 3.3", as the pixel format dictates).

Exit (met, see above): unspecified ES on a 3.x driver returns 3.x; the second identical create makes no failing driver calls (counted on a driver stand-in); the "minimum" semantics are documented and tested; the owner's decision is the one open part.

### 12.3 Driver quirks, with evidence

This replaces the open "quirks database" item in 10.3. The rule stays: **no entry without a reproducible symptom
and a regression test.** What changes is that the first entries already exist, scattered as one-off code.

- [ ] **Driver identity, once per backend.** Capture `GL_VENDOR` / `GL_RENDERER` / `GL_VERSION`, the platform
  driver version (Windows driver version string, Mesa version from `GL_VERSION`, EGL vendor) and the extension set
  at bootstrap (the WGL bootstrap context and the EGL/GLX display already exist), expose it read-only to the
  planning code.
- [ ] **Data-driven quirk table.** Entries are `{ id, backend, vendor/renderer match, driver-version range, OS,
  action, evidence, test }`; actions are a closed set (drop an attribute, reorder or skip an attempt, force a
  fallback surface, tolerate an error code, cap a resource count, warn). Applied quirks are logged at INFO with
  their id, reported in `TesseraProbeReport` and `tessera-diag`, and can be switched off with
  `TESSERA_QUIRKS=off` (or per id) so a host can tell whether a quirk is the cause of a problem.
- [ ] **Seed it with what this repo already knows** (each becomes an entry plus its existing test, then the
  ad-hoc code is removed): WGL `wglMakeCurrent(NULL, rc)` refused by NVIDIA (hidden-window fallback), NVIDIA
  answering an unsupported combination with `ERROR_INVALID_DATA`, GLVND returning a stub for any `gl*` name, one
  current context per thread across GLX and EGL on Mesa, Mesa D3D12 `glXDestroyContext` crash with many live
  contexts (10.1; mitigation = cap/serialise, only if reproduced on a second system).
- [ ] **Evidence intake.** `tessera-diag --report` writes a self-contained, anonymised bundle (identity, extension
  list, per-backend strict matrix, applied quirks, Tessera version) meant to be attached to an issue; a quirk PR
  must reference one. Nothing is sent anywhere.
- [ ] **Expiry.** Each entry carries the driver version it was last seen on; `docs/compat.md` lists entries with no
  re-verification in a release cycle so dead workarounds get removed rather than accumulate.

Exit: the table ships with at least the seed entries, every entry fails its test when disabled, and no
vendor-name `if` remains in backend code outside the table.

### 12.4 Platforms and hosts - DONE (chunk 9, ABI 0.14.0, ADRs 0014, 0015, 0017)

- [x] **Wayland.** EGL on a `wl_display` the *host* owns: attach to a borrowed `wl_surface` through a `wl_egl_window`, no connection of
  our own, never dispatching the host's queue (ABI 0.12.0, ADR 0014; `display` is the native-display argument). Tested against a real
  compositor (WSLg) with a hand-made client and with GLFW's Wayland window from Java. **Not verified:** a headless weston in CI (not
  added), a mapped surface with frame callbacks.
- [x] **EGL window surfaces on X11 and Windows (ANGLE)**: X11 in 0.12.0; ANGLE `HWND` surfaces in chunk 7 (ADR 0015), run in CI on the
  hosted Windows runner.
- [x] **Attach with an explicit native display on X11** (0.14.0): `TesseraNativeWindow::display` may be the host's `Display*`; Tessera
  reads the server name (`XDisplayString`) and opens its own connection to that server, for GLX and EGL (the host's connection is
  never used: Xlib is not thread-safe without `XInitThreads`). Tested with the default connection and one opened with an explicit
  name (`test_glx`, `test_eglwindow`). **Not verified:** a server other than `$DISPLAY`'s, another screen.
- [x] **Foreign-state matrix**: `docs/coexistence.md` lists what each backend saves and restores (context, draw/read drawable, bound
  EGL API, the other API's thread state, connections, the X error handler) and what is deliberately not preserved (last-error codes),
  written from the `*ForeignOps` code and the tests that check it. The existing coexistence tests already cover context, other-API
  and display ownership; **not covered:** a GLX drawable on a different screen.
- [x] **Detect, don't assume, host threading**: `XInitThreads` detection (0.11.0, ADR 0013) and, new, an EGL display the host already
  initialised is detected (`eglQueryString` before our `eglInitialize`) and logged once; `egl_host_first` runs it in a fresh process.

### 12.5 One way to say "this GPU" - DONE except real targeting on Windows and hardware we do not have (chunk 9, ABI 0.14.0, ADR 0017)

- [x] **Adapter inventory**: `tesseraEnumerateAdapters` (WGL: DXGI, `luid:`; EGL: devices, `uuid:`/`node:`/`index:`; CGL: renderers,
  `cgl:0x...`; GLX lists nothing). Java `Tessera.enumerateAdapters()`. Integrated vs discrete on Windows is a memory heuristic.
- [x] **One selection field**: `TesseraContextConfig::adapter` (the last reserved bytes, size unchanged), honoured by WGL (as a LUID,
  verified), EGL (device platform, one display per device) and CGL (renderer id), `UNSUPPORTED` on GLX, never replaced by another GPU;
  `TESSERA_EGL_DEVICE` stays as the environment spelling.
- [x] **Report who served the context**: `TesseraContextInfo::adapter` and `adapterClass` on every backend (GLX: class only).
- [x] **CGL renderer selection**: `kCGLPFARendererID`, offline renderers allowed as the last attempt (cost documented in ADR 0017).
  Verified against the fake CGL (3 renderers, one offline, one software) and, in CI, the real framework on the macOS VM.
  CI (21 Nov 2026): on the real macOS framework the hosted VM has one renderer, `cgl:0x01020400` (Apple Software Renderer, software),
  which serves the default context and is selected by its own id; its id carries bits beyond the 0x20400 family (the family mask is
  applied). The Windows runner has Hyper-V's adapter next to WARP (two adapters of one vendor, both "Microsoft"): an explicit,
  verified id is reported as requested there, and a default context's adapter is unknown (ambiguous), as designed.
- [ ] **Real targeting on Windows** (`WGL_NV_gpu_affinity`, `WGL_AMD_gpu_association`): not done; there is still no hardware here that
  exposes them. Windows stays verify-only.
- [~] **Hybrid-laptop advisory**: implemented and logged once per process; the decision is a pure function covered by unit tests with fake
  adapter lists. **Not verified on a real hybrid machine** (this one has a single GPU plus WARP), so no claim that the advice appears
  where it should on one.

### 12.6 Loss and reset beyond polling - DONE except the real reset (chunk 10, ABI 0.15.0, ADR 0018)

- [x] **Opportunistic polling**: a robust context with reset notification is checked inside `tesseraMakeCurrent` and `tesseraSwapBuffers`;
  the lost callback fires without the application polling; opt-out `TESSERA_FLAG_NO_RESET_POLL`; still no Tessera threads. Tested on the
  fake backend (swap, make-current after a reset while away, the opt-out, contexts without reset notification) and, through the real
  upcalls, in Java.
- [x] **Recreate onto a different adapter**: `tesseraRecreateContextWithPolicy` (same adapter / any hardware / allow software) with
  `ADAPTER_CHANGED` and `SOFTWARE` reported; the share-group rule unchanged (an empty group when the new device cannot share). Tested on a
  fake backend with two GPUs and a CPU device (removal, fallback, refusal); the Java SelfTest runs all three policies on the real driver.
- [ ] **Real reset test** (10.1): **not done, waiting for the owner's go-ahead** (it can blank the screen). Claims stay "fake backend only".
- [x] **Distinguish loss causes**: `tesseraGetLossCause` (`GPU_RESET`, `DEVICE`, `WINDOW_GONE`) and `TESSERA_ERROR_WINDOW_GONE`. WGL and GLX
  window-gone are verified on real windows; the EGL case is implemented but Mesa's X11 platform does not report it (the swap passes).

### 12.7 Sharing, explained - DONE (chunk 10, ABI 0.15.0, ADR 0018)

- [x] **Preflight**: `tesseraCanShare` with reasons (parent lost, API, profile, backend, adapter, display, format); on every real backend
  the verdict is compared with the real create for ten request variants (`sharingExplained`), on the CGL fake and fake backends for the
  rest. **Not verified:** CGL on a real Mac beyond the hosted VM, and a "yes" is never a promise.
- [x] **Introspection**: `tesseraGetShareGroup` (group id, members, alive); Java `Context.shareGroup()`.
- [x] **Cross-format sharing on WGL/GLX**: measured on NVIDIA/WGL (a different depth-buffer size breaks it, nothing else tried does), Mesa
  GLX D3D12 and EGL D3D12/llvmpipe (nothing breaks it); the library now reuses the parent's pixel format on WGL when it serves the
  request, and refuses a deeper one with a reason. The "ranking in 12.1" part waits for 12.1 itself. **Not measured:** AMD, Intel, a
  non-GLVND Linux driver.

### 12.8 Explaining a failed selection

- [ ] **Attempt trace.** `TesseraProbeReport` and the thread-local message currently carry only the last failure.
  Record every attempt (version rung, relaxation step, config id, driver error) in an ordered list and return it
  from probe; log it at DEBUG on any failed create. **ABI**
- [ ] **`tessera-diag` explains, not just reports:** for a failing request, print the first rung that failed and
  the smallest change to the request that succeeds ("drop sRGB", "lower to 4.3"), computed by the same ranking and
  ladder code (never a second implementation).
- [ ] **Compat table from data.** Generate the per-driver table in `docs/compat.md` from `tessera-diag` JSON
  output instead of by hand, so what is claimed equals what was run.

### Suggested order

| Order | Item | Why first | Touches ABI |
|---|---|---|---|
| 1 | 12.1 relaxed-report + acceleration class | cheapest honest-answer gain; unblocks CI-style "software returned" cases | yes |
| 2 | 12.3 quirk table with seed entries | turns existing ad-hoc code into tested data; needs no new hardware | no |
| 3 | 12.8 attempt trace | makes every later item debuggable | yes |
| 4 | 12.1 ranking + enumerate + window-format handoff | the largest structural gap; biggest integration win | yes |
| 5 | 12.2 ES ladder + version cache + "minimum" decision | small, mostly internal; one owner decision | maybe |
| 6 | 12.5 adapter inventory/selection/report | needs hybrid/multi-GPU hardware for real confidence | yes |
| 7 | 12.6, 12.7 | depend on 12.1/12.5 shapes | yes |
| 8 | 12.4 Wayland and host-display attach | needs a compositor in CI | yes |

Items 1, 3, 4, 5, 6, 7 and 8 together are the **0.10.0** ABI batch: do the ABI-touching parts in one bump so the
1.0 freeze is not preceded by a string of small breaking releases.

---

## Next chunks (planned 2026-11-19)

Phases 10 and 11 above are the backlog; this is the order to work through it. Each chunk is a unit that can be finished, tested and committed on its own. "Needs" says what, besides this machine (Windows 11 + NVIDIA, WSL Ubuntu with Mesa) and GitHub Actions, a chunk depends on.

### Chunk 1 - Remote CI and release pipeline proof - DONE
- [x] Both macOS jobs green (first real CGL run 19 Nov 2026: `test_cgl` passes on Apple's software renderer; Java JNI+FFM pass); two real bugs found and fixed (link error, CGL "relaxed" log).
- [x] `release.yml` dry run succeeds end to end (three native builds, assemble, Doxygen zip); first run had found a missing output directory and the GDI-renderer test failure.
- [x] `nightly.yml` runs; the first run found the soak ctest timeout (fixed); the rerun passes.
- [x] vcpkg port installs and links a C-only consumer on Linux, Windows and macOS in CI (found two packaging bugs, fixed).
- [x] Workflows documented in `docs/ci.md` (models, triggers, cost controls).

### Chunk 2 - Swap control and frame pacing (ABI 0.10.0) - DONE (ADR 0012)
Needs: nothing (real vsync behaviour needs a display; window attach tests already exist on WGL/GLX).
- [x] `tesseraGetSwapControl` reports what is in effect: capabilities (control / adaptive / read-back), the last requested interval, what the driver reports (`applied`, hidden unless the backend really read it) and the clamp (`maxInterval`, GLX EXT). WGL reads back through `wglGetSwapIntervalEXT` (NVIDIA: 0, 1, 2, -1 all read back as requested), GLX through `glXQueryDrawable` / `glXGetSwapIntervalMESA`.
- [x] `GLX_EXT/MESA/SGI_swap_control` selection and the adaptive/read-back rules are one pure function set (`platform/common/swap_control.hpp`) covered by a case matrix in `test_core` (advertised-but-unresolved, preference order, EXT-only adaptive); the core's handling (requested tracking, hiding unread values, struct sizes, lost-context) is tested on a fake backend. **Not verified:** a real server that advertises the GLX extensions (WSLg and llvmpipe advertise none, so the GLX test runs the "nothing available" branch there).
- [x] `tessera-diag --bench-swap [S]`: vsync on / off / adaptive in a window of its own (Win32 and X11), frames, mean/min/p99/max, fps and dropped frames, text and JSON; the normal report gained a swap-control line. Its first run found that a single-buffered window never paces (documented in `tessera.h`).
- [x] DWM test on a real composited window (`test_wgl`, "swap control and frame pacing"): on a 120 Hz display vsync on = 8.32 ms/frame (120.2 fps), off = 0.3-0.5 ms; asserts the relation on whatever display is present and the UNSUPPORTED path on the GDI renderer. **Not verified:** exclusive fullscreen, variable refresh, Optimus.
- [x] Java `Context.swapControl()` / `SwapControl` (JNI and FFM, both tested), `docs/java.md`, `docs/diag.md`, ADR 0012.

### Chunk 3 - Window attach beyond WGL/GLX (ABI 0.12.0, ADR 0014) - DONE except AWT
Needs: the 10.5 decision "on-screen in 1.0?" (**started on the owner's instruction to do chunks 1-5; please confirm the decision in 10.5**); WSLg for X11 and Wayland testing.
- [x] EGL window surfaces: `tesseraCreateContextForNativeWindow` / `tesseraResizeWindow`; an X11 window through EGL (Tessera's own connection to `$DISPLAY`) and a Wayland surface (a `wl_egl_window` owned by the context), with the borrow/ownership rules of the WGL/GLX attach. Tested in `tests/test_eglwindow.cpp` (render and read back, swap, resize, destroy, re-attach, refusals) and `test_wgl` (the described Win32 form, kind mismatches, argument validation). Found on the way: the EGL 1.5 core platform entry point takes pointer-sized attributes (mixing it up with the EXT one crashed Mesa).
- [x] Wayland attach on WSLg's compositor: the test makes a real `wl_surface` through `libwayland-client` by hand; and Java attaches to **GLFW's own Wayland window** (`java_glfw_wayland`, LWJGL, JNI). **Not verified:** a surface that is shown and receives frame callbacks (the test surface is never mapped, so only interval 0 is exercised; a vsync swap on an unmapped surface would block), a second compositor, a window destroyed under a live EGL context.
- [~] Java with a real window on Linux: GLFW `GLFW_NO_API` windows on X11 (integer and described form) and on Wayland, both bindings. **AWT/Swing is not done**: the native window ids of AWT components are not public API (they need JDK-internal access), so there is nothing supported to attach to.
- [x] macOS `NSView`: design note and the `TESSERA_WINDOW_COCOA` slot in the ABI (ADR 0014, section "macOS"); not implementable without a Mac. ADR 0014 also records the resize, thread and teardown rules for every borrowed window.

### Chunk 4 - Driver features that exist but are unproven or missing (ABI 0.11.0, ADR 0013)
Needs: nothing extra; multi-GPU and AMD/Intel parts of these stay in 10.1.
- [x] Context priority: `TesseraContextConfig::priority` / `TesseraContextInfo::priority`, GLX (`GLX_EXT_context_priority`) and EGL (`EGL_IMG_context_priority`) with read-back; strict compares the read-back, best-effort drops and logs; **WGL has no such extension** (refused in strict mode). Java `ContextConfig.Priority` / `ContextInfo.priority()`. Tested: the plan and strict-satisfaction logic and validation (`test_core`), strict refusal on real WGL, GLX and EGL (Mesa advertises neither extension), Java on both bindings. **Not verified:** a driver that grants a priority (needs e.g. Intel/AMD Mesa or NVIDIA on Linux); the granted path is covered by the plan/satisfaction unit tests only.
- [x] EGL device selection by render node, PCI address and **UUID** (`EGL_EXT_device_persistent_id`): the matching rules are one pure function tested against faked multi-GPU lists (index range, by-path PCI resolution, any-case/32-digit UUIDs, partial attribute support, ambiguity refusal, missing-extension reasons). `test_egl` also names the real device by its render node and UUID where the driver reports them. **Not verified:** a machine with two GPUs; WSLg's single device reports neither a render node nor a UUID, so the real-device positive cases did not run there.
- [x] `XInitThreads` detection: libX11's `_Xglobal_lock` says whether the host already initialised Xlib threading; Tessera logs which side acted and skips the call when the host did it. Tested in two fresh processes (host first / not) with libX11's own lock as the witness.
- [x] Robust access, reset notification, `KHR_no_error`, release-flush matrix on NVIDIA/WGL and Mesa GLX/EGL, recorded in `docs/compat.md` (what was granted, refused, untested).

### Chunk 5 - Verification and hardening of what already exists
Needs: WSL for the sanitizer builds. A real GPU reset needs the owner's consent to provoke a TDR.
- [x] ASan/UBSan and TSan on Linux (WSL, gcc 11) over everything added since ABI 0.7.0: pass, including the new threaded lost-callback test (4 threads losing contexts while the callback is replaced). The GPU-backed tests stay out of the sanitizer presets (NVIDIA's WSL driver is uninstrumented).
- [x] Fuzzing the newer entry points: `fuzz_api` (libFuzzer: **11.1 million inputs in 2 minutes**, no findings) and the seeded `test_fuzz` cover `tesseraGetSwapControl`, the pool (`TopUp`, `GetPooledCount`, `Clear`), `tesseraIsContextLost` and the lost callback; and a new Java layer fuzzer, `NativeFuzz`, throws hostile handles/arrays/strings at every JNI and FFM entry point under `-Xcheck:jni` (JNI misuse fails the test). It found **no Tessera bug**. On Linux it did find the Mesa D3D12 problem already noted in 10.1: it reproduced through Tessera (heap corruption detected by glibc in about 90% of seeds) as soon as the fuzzer let a dozen GLX contexts stay alive; ASan on Tessera's own code stayed clean, and with the live-context count bounded 0 of 30 seeds fail.
- [x] Lost events through the real upcalls: the `java` preset builds a test hook (`TESSERA_TEST_HOOKS`, never in a release) that makes `swapBuffers` report a lost context; `LostEventsTest` follows the event through JNI and FFM (once per context, on the detecting thread, recreate from inside the listener, a throwing listener, removal, a second thread).
- [x] The longer coexistence run as a scheduled job: `coexistence-long.yml` (weekly, or on demand with 1-5 hours): soak on Linux and Windows and the JavaFX stress next to Tessera, tables in the job summaries. First run (1 hour, 19 Nov 2026): Linux and Windows soaks passed 3600 s; the JavaFX es2 stress next to Tessera passed (1191 s).
- [ ] A real GPU reset on Windows: **not done, waiting for the owner's go-ahead**. Provoking a TDR (a deliberately long shader) can blank the screen and make other applications lose their GPU context; the test is easy to write (robust context, run the shader in a throwaway process, expect `GUILTY`/`INNOCENT`, recreate), but it should be run on purpose, not as a side effect.

### Chunk 6 - 1.0 decisions and the freeze machinery - DONE (the owner's decisions themselves are still open)
Needs: the owner for the decisions; the tooling needs nothing.
- [x] A one-page decision memo with a recommendation for each 10.5 item: `docs/decisions-1.0.md` (on-screen in 1.0 with a scope statement, no enforced GL floor but a documented support floor and a floor-means-floor version semantics to implement before the freeze, system-only ANGLE/Mesa for 1.0, keep names and licence with a name search, cadence). It has a decision record table; **the decisions are the owner's and none is made yet**.
- [x] An ABI check in CI: `scripts/abi-check.py` compares the header (functions with signatures, struct layouts with offsets, `#define` values) with `docs/abi/baseline.json` and the exports of the built library (a pure-Python PE reader on Windows, `nm` elsewhere) with the header; registered as ctest `abi_check` and `abi_check_selftest`, so every CI job that has Python runs it. Verified: a mutated header fails (changed signature, field type, macro value); a stale library fails; run against the git history of the header it reports exactly one break between 0.2 and 0.12 (the removal of the `TESSERA_CONTEXT_*_INIT` macros in 0.3) and found a macro-parser bug in the process. The export listing passed through `nm` on WSL (Ubuntu 22.04) and on the hosted macOS runner (`nm -gU`), and through the PE reader on Windows; CI is green on 7b27439.
- [x] A compatibility-matrix generator: `scripts/compat-from-diag.py` turns `tessera-diag --json` files into the "Verified" rows and the feature-by-driver table (ctest `compat_generator`, run on two stored real outputs in `docs/compat-samples/`); its Windows output equals the hand-written column of `docs/compat.md`. `docs/compat.md` itself was not regenerated, it has hand-made content the diag does not produce.
- [x] A release-candidate checklist and the freeze procedure: `docs/freeze.md`, backed by `scripts/release-check.py` (`--rc`, `--freeze`, `--tag`, `--set-version`; ctest `release_check`). It found real drift on its first run (the vcpkg manifest said 0.9.0, the header 0.12.0; fixed) and `--freeze` lists every open ROADMAP 10.x item as a reason not to freeze (an accepted gap is marked `ACCEPTED-FOR-1.0`). Also fixed: the README still said 0.7 and "macOS never run".

### Chunk 8 - Version negotiation (12.2) - DONE (this is Phase 12.2; the four items are listed there)

### Chunk 9 - Hosts and GPUs (12.4 and 12.5) - DONE except real targeting on Windows (see the two sections above)

### Chunk 10 - Loss and sharing (12.6 and 12.7) - DONE except the real reset test (see the two sections above)

### Chunk 7 - Software/headless fallback and ANGLE/Mesa - DONE on the recommendation of `docs/decisions-1.0.md` item 3 (system only for 1.0; the owner has not recorded the decision)
Needs: the 10.5 decision "do we ship Mesa/ANGLE?" (**started on the owner's go-ahead; it assumes "no bundled binaries in the release", the reversible choice**), an ANGLE build to test against.
- [x] ADR 0015 (loader policy): system first, then an explicit absolute path (`TESSERA_EGL_LIBRARY`); on Windows `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | SEARCH_SYSTEM32` through `DynamicLibrary::openPath`, absolute paths only. Verified: a relative `TESSERA_EGL_LIBRARY` is refused (the test skips), an absolute one finds `libGLESv2.dll`/`d3dcompiler_47.dll` beside it. **Not verified:** that a planted DLL in the current directory is ignored (follows from the flags, no test plants one).
- [x] The EGL backend builds and runs on Windows against **ANGLE** (the libEGL that VS Code/Electron ships): `TESSERA_BACKEND=egl` gives ES 3.0 over Direct3D 11 on the RTX 3060, a pbuffer context renders and reads back, an `HWND` window surface swaps (`tesseraCreateContextForWindow` through EGL), desktop GL is refused with a clear `UNSUPPORTED`. CI's Windows job downloads the ANGLE inside a pinned Electron 33.2.0 release (SHA-256 checked before use) and runs both tests on the hosted runner (no GPU: ANGLE falls back to WARP; green, 21 Nov 2026). **Not done:** Metal-backed ANGLE on macOS, ANGLE on Linux, Vulkan-backed ANGLE with a real driver.
- [x] The `software` backend: `TESSERA_BACKEND=software` (never in the default order; `wgl,software` = GPU else CPU). Windows: ANGLE's WARP (verified: "Microsoft Basic Render Driver" while the NVIDIA GPU is present and untouched). Linux: Mesa's software device, with the renderer checked afterwards (`isCpuRenderer`); it found that WSL's D3D12 GPU driver also reports `EGL_MESA_device_software`, so on that machine the backend correctly refuses unless `LIBGL_ALWAYS_SOFTWARE=1` is set (then llvmpipe, verified). **Not verified:** a machine whose GPU driver is truly absent (tested with the GPU present and refused/ignored; CI's llvmpipe runner is the CPU-only case, pending), SwiftShader on Windows (WARP answered first), OSMesa (not implemented; llvmpipe through EGL covers it).
- [x] Packaging and SBOM: `scripts/package-provider.py` makes `tessera-<v>-<name>-<os>-<arch>.zip` (+ `PROVIDER.json` with hashes, a README) and refuses a folder without a licence; `scripts/assemble-release.py --provider` copies it next to the release artifacts, checksums it and adds an `"optional"` CycloneDX component with nested file hashes (verified end to end on a real ANGLE folder; the Maven bundle stays free of it). The official release workflow does not build or fetch a provider.

Suggested order: 1, 2, 4, 5 now (no outside decisions); 6 in parallel with the owner's decisions; 3 and 7 once those are made.

---

## Coexistence checklist (applies to every change)

- [ ] Does it change anything process-global (DPI awareness, error handlers, signal handlers, locale, env vars, working directory, COM init, `timeBeginPeriod`, AppKit app state)? → Don't, or scope and restore.
- [ ] Does it call `make current`? → Wrapped by the foreign-context guard.
- [ ] Does it create a native window/display/connection? → Own it, name it uniquely, destroy only ours.
- [ ] Does it call `Terminate`/`Close`/`Unload` on something shared? → Only if we created it.
- [ ] Which thread may call it, and is that enforced?
- [ ] Does teardown work when the host app has already torn down its own GL?
- [ ] Is there a test with a foreign context current on the calling thread before and after the call?
- [ ] Does it read caller memory? → Only up to the caller's `structSize`; add the case to `tests/test_fuzz.cpp`.

## Milestones

| Milestone | Phases | State |
|---|---|---|
| M1 – Alpha | 0, 1, 2 | Done (Windows WGL, JavaFX D3D/sw coexistence) |
| M2 – Beta | 3, 4 | Done on Mesa/WSLg; not on a real Xorg/Wayland desktop |
| M3 – RC | 5, 6, 7 | Done except macOS (fake driver only) and Java on Linux/macOS |
| M4 – Hardened | 8, 9 | Done on one machine |
| M4.5 – Selection depth | 12 (ABI batch 0.10.0) | Planned; ABI parts must precede the freeze, the rest can follow 1.0 |
| M5 – 1.0 | 10 | Open: other hardware, a real CI run, owner decisions, then freeze |
| Later | 11 | Candidates |
