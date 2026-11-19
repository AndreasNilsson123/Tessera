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

- [ ] **macOS**: run `tests/test_cgl.cpp` on a real Mac (Apple Silicon and Intel). The hosted `macos-latest` job is the first chance; it has not produced a result yet. Biggest risks: hand-written CGL constants, whether `kCGLPFAColorSize` includes alpha, whether core contexts report the forward-compatible bit, GPU-less CI VMs. Then run the Java tests and the JavaFX (Metal) coexistence case there.
- [ ] **Other GPU vendors**: AMD, Intel (Windows and Linux), NVIDIA proprietary on Linux. Re-run the attribute matrix, window attach, `dxinterop` and the soak; fill `docs/compat.md`.
- [ ] **Real X server / non-WSL Linux**: a real Xorg session with a hardware driver, non-GLVND `libGL`, 32-bit. (Xvfb + llvmpipe and clang are done in CI.) Swap-control extensions are untested because neither WSLg nor llvmpipe advertises them.
- [ ] **Multi-GPU**: render-node and PCI matching in `TESSERA_EGL_DEVICE` (index 0 and the refusal paths are tested; matching is not). Add UUID selection (`EGL_EXT_device_persistent_id`).
- [~] **Java on Linux and macOS**: Linux is done (JDK 21 locally and in CI, JNI; JDK 25 FFM in CI). macOS: not run.
- [x] **Other GL users next to Tessera (Bundle 1)**: GLFW through LWJGL (guest guarantees, concurrent rendering and churn, attach to a `GLFW_NO_API` window, terminate with a live Tessera context; Windows WGL and Linux GLX, Mesa D3D12 and llvmpipe; both bindings on Windows), Java2D's OpenGL pipeline (AWT/Swing; `WGLGraphicsConfig` and `GLXGraphicsConfig`), and a longer JavaFX run (`-DTESSERA_JAVA_STRESS=ON`: 6000 frames and thousands of churned contexts on d3d / es2, 0 wrong). **Found and fixed two real bugs:** a Wayland/EGL host (GLFW's Wayland platform) made Tessera's GLX context creation fail with `BadAccess` and its context was never restored (Mesa keeps one current context per thread across GLX and EGL; both backends now capture/restore and release the other API's state, `src/platform/common/cross_api.*`; native tests with a host context of the other API, mutation-checked), and `Tessera.procAddress` returned 0 before Tessera had created a context, so LWJGL could not resolve functions for a host's context. Still not tested: SDL, Qt, macOS, a Wayland compositor in CI (checked by hand on WSLg), hybrid-GPU laptops, runs of hours.
- [~] **JavaFX `es2` (OpenGL) pipeline next to Tessera**: passes on Linux with Mesa (D3D12 and llvmpipe, 25/25 runs after the fix; ~50% crashed before it, which is how the X error handler bug was found). Needs `-Dprism.forceGPU=true` because JavaFX does not allow-list Mesa. Not done: a real hardware driver (NVIDIA/AMD/Intel on Xorg), a run of hours, macOS (Metal/ES2).
- [ ] **Driver issue to watch:** on WSL's Mesa D3D12 driver (Mesa 23.2 over NVIDIA), having 12 GLX contexts alive at once made a later, unrelated `glXDestroyContext` (of a window-attached context) crash inside `swrast_dri.so` (the Gallium megadriver). llvmpipe, NVIDIA/WGL and EGL did not; ASan was clean; a raw-libGL reproduction was not achieved, so it is attributed to the driver but not proven. Tests keep at most 8 contexts alive at once. If it reproduces on a native driver, it is Tessera's to fix.
- [ ] A real GPU reset (TDR / device removal) with a robust context: `GUILTY`/`INNOCENT`/`UNKNOWN`, sticky lost state and recreate are tested only on a fake backend. Also hot-unplug and display changes.

### 10.2 Implemented but never exercised

- [ ] Robust access, reset notification, `KHR_no_error`, release-flush and context priority on drivers that grant them (WGL, GLX `_robustness`/`_no_error`, EGL flush control).
- [ ] DWM swap-interval quirks; `GLX_EXT/MESA/SGI_swap_control` on a server that advertises them.
- [~] WGL share-order validation is not needed (sharing is requested at creation through `wglCreateContextAttribsARB`, and the legacy path calls `wglShareLists` on fresh contexts); context priority (`EGL_IMG_context_priority`, `WGL_*_priority`) is not implemented.
- [x] Fault-injection of the WGL, GLX and EGL failure paths (`tests/test_fault.cpp`): every failable driver call that goes through the backends' function tables (93 / 218 / 301 on WGL / GLX / EGL) is failed once and then every call from there on, against the real driver through the public API, checking result codes, handles, the other library's context and native-object leaks (contexts, pbuffers, windows, colormaps). **It found no bug.** A mutation (skipping a release) makes it report 183 problems, so it does detect leaks; also clean under ASan/UBSan. Not injectable: direct Win32 calls (`CreateWindowEx`, `SetPixelFormat`) and asynchronous X errors.
- [x] The Linux memory growth seen in the soak: **not Tessera's.** With a fake backend Tessera is flat over 270,000 contexts on 3 threads (guard in `test_core`); the same create/use/destroy loop written straight against libX11/libGL grows the same way on Mesa's D3D12 driver with 3 threads (about 2 KB per context, 6.5 MB per 3000 contexts) and not on llvmpipe or with one thread. Not tested: a non-Mesa driver.
- [x] CI: the workflow has run; see the status above. Open: the macOS jobs, and a scheduled (nightly) soak.

### 10.3 Not implemented (known gaps)

- [~] Drawable-less `SURFACE_NONE`: **GLX done** (the context is made current without any drawable; verified on Mesa D3D12 and llvmpipe including offscreen FBO rendering; a hidden window is the fallback for a driver that refuses, exercised by the fault sweep). **WGL is impossible**: `wglMakeCurrent(NULL, rc)` fails with `ERROR_INVALID_HANDLE` on the NVIDIA driver, so WGL keeps its internal 1x1 window (documented in `tessera.h`).
- [ ] Detecting whether the host already called `XInitThreads`.
- [~] GPU selection on Windows (ABI 0.7.0, `adapterLuid` / `gpuPreference`, Java `gpuPreference()` / `adapterLuid()`): **verification, not switching.** A process cannot make Windows load a different GPU's OpenGL driver, so Tessera resolves the adapter through DXGI (`EnumAdapterByGpuPreference`) and checks that the OpenGL driver it got belongs to it by PCI vendor; a mismatch (or several adapters of one vendor, which cannot be told apart) fails with `NO_MATCHING_CONFIG` instead of silently using another GPU. Other backends refuse the request (`UNSUPPORTED`; EGL has `TESSERA_EGL_DEVICE`). Tested here on the one real adapter plus the Basic Render Driver (mutation-checked). **Not implemented:** actually targeting a GPU with `WGL_NV_gpu_affinity` / `WGL_AMD_gpu_association` (this GeForce exposes neither, so it could not be tested); **not tested:** a hybrid Intel+NVIDIA/AMD machine, where the check matters most. Document for hosts: `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance` must be exported by the EXE.
- [ ] ANGLE / Mesa-on-Windows through the EGL backend: the EGL backend is built on Linux only, and the Windows loader is restricted to System32 by design (loading ANGLE from an application directory needs `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` handling and a test). Same for Metal-backed ANGLE on macOS (CGL is deprecated).
- [~] Interop beyond the handles: **EGLImage between two contexts verified** on Linux (`test_eglimage`: Mesa D3D12 and llvmpipe), after fixing the EGL backend not resolving `egl*` names through `tesseraGetProcAddress`. Not done: dma-buf export/import with fences (the Mesa drivers here lack the extensions), IOSurface (macOS), Vulkan `GL_EXT_memory_object` + semaphores. Each needs a consumer to test against.
- [ ] Window attach for EGL (window surfaces), Wayland `wl_surface`, macOS `NSView`. (Windows and X11 are done.)
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
6. **Frame pacing helpers** for window-attached contexts: swap-interval negotiation reported back (what was applied), adaptive-sync availability, and a vsync-off benchmark mode. Only for attached windows; Tessera's own hidden drawables do not present.
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

- [ ] **ES ladder.** Unspecified ES walks 3.2 → 3.1 → 3.0 → 2.0 (EGL, plus WGL/GLX ES-profile paths), reported in
  `info`.
- [ ] **"Minimum" means minimum.** The header documents `versionMajor/Minor` as a *minimum*, but an explicit core
  request is created at exactly that version. Decide (owner decision, with 10.5) between: try upwards from the
  newest version down to the floor, or add an explicit `TESSERA_FLAG_EXACT_VERSION` and make the default a floor.
  Whichever is chosen, document it next to the field and pin it with a test.
- [ ] **Remember what worked.** Cache, per (backend, adapter/renderer identity, profile), the highest version that
  created and the versions that failed, so the second context does not repeat up to nine failing driver calls
  (each of which can emit driver-side error output and costs time in the pre-warm path). Invalidate on
  `tesseraTerminate` and when the adapter changes. Never cache a success as a guarantee: a cached rung that now
  fails falls back to the full ladder.
- [ ] **Map driver refusal codes in one table.** The WGL error mapping (`0x2095`, `0x2096`, `ERROR_INVALID_DATA`,
  `ERROR_INVALID_PARAMETER`) lives inside one backend; move "this error means unsupported combination" for all four
  backends to one reviewed table with a test per row.

Exit: unspecified ES on a 3.x driver returns 3.x; the second identical create makes no failing driver calls
(counted on the fake backend); the "minimum" semantics are decided, documented and tested.

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

### 12.4 Platforms and hosts

- [ ] **Wayland.** EGL on a `wl_display` the *host* owns: attach to a borrowed `wl_surface` through a
  `wl_egl_window`, creating no connection of our own and never dispatching the host's queue. Needs an explicit
  native-display argument on attach (an EGL display must match the one the surface belongs to). Test against a
  compositor in CI (headless weston) once it is available; until then keep the WSLg manual check. **ABI**
- [ ] **EGL window surfaces on X11 and Windows (ANGLE)**, sharing the attach rules above; tied to the loader policy
  decision in 10.5.
- [ ] **Attach with an explicit native display on X11.** Attach currently assumes `$DISPLAY`; accept the host's
  `Display*`/server so a host connected elsewhere (or through a different screen) attaches correctly. **ABI**
- [ ] **Foreign-state matrix.** Extend the coexistence tests from "a host context current" to the other pieces of
  host state each backend can disturb: the bound EGL API, the current drawable (not just context), swap interval
  of a shared window, GLX drawable on a different screen. One table in `docs/coexistence.md` listing what is
  saved/restored per backend.
- [ ] **Detect, don't assume, host threading.** Detect (or at least report) whether the host called `XInitThreads`
  (10.3) and whether the host already initialised EGL on the same display, and log the consequence once.

### 12.5 One way to say "this GPU"

Today: Windows verifies an adapter (never switches), EGL selects through an environment variable, GLX and CGL have
nothing, and the reported context does not say which GPU served it.

- [ ] **Adapter inventory.** `tesseraEnumerateAdapters` returns a uniform descriptor per backend (stable id —
  LUID / PCI address / render node / device UUID / CGL renderer id — plus vendor, name, software flag, and whether
  it drives a display). **ABI**
- [ ] **One selection field.** A config `adapter` (an id from the inventory) honoured wherever the backend can,
  refused with `UNSUPPORTED` where it cannot, under the existing rule that an explicit choice is never silently
  replaced. `TESSERA_EGL_DEVICE` becomes a spelling of it, not the only way. **ABI**
- [ ] **Report who served the context.** `TesseraContextInfo.adapter` (the id and the software/integrated/discrete
  class) for every backend, so applications and the probe can see a hybrid-laptop mismatch instead of inferring it
  from `GL_RENDERER`. **ABI**
- [ ] **CGL renderer selection.** macOS can really target a renderer (renderer id attribute; offline renderers
  already handled last); implement it, with the "allowing offline renderers can wake a discrete GPU" cost
  documented.
- [ ] **Real targeting on Windows** (`WGL_NV_gpu_affinity`, `WGL_AMD_gpu_association`) once hardware that exposes
  them is available (10.3); until then keep verify-only.
- [ ] **Hybrid-laptop advisory.** When verification shows the process got the low-power adapter while a
  high-performance one exists, log the one-line fix for hosts (the exported `NvOptimusEnablement` /
  `AmdPowerXpressRequestHighPerformance` symbols) once per process. Requires an actual hybrid machine to test.

### 12.6 Loss and reset beyond polling

- [ ] **Opportunistic polling.** For contexts created with reset notification, check `glGetGraphicsResetStatus`
  inside `tesseraSwapBuffers` / `tesseraMakeCurrent` (cheap, already on the right thread, context current) so the
  lost callback fires without the application having to remember a poll. Opt-out flag; still no Tessera threads.
- [ ] **Recreate onto a different adapter.** After device removal the same adapter may be gone. Let
  `tesseraRecreateContext` take a policy (same adapter / any hardware / allow software) and report which was used;
  share-group preservation rules unchanged. **ABI**
- [ ] **Real reset test** (10.1): a deliberately hanging workload on a robust context on real hardware to produce
  a genuine `GUILTY`/`INNOCENT`, then recreate with shared objects re-uploaded. Until run, claims stay "fake
  backend only".
- [ ] **Distinguish loss causes.** Separate "GPU reset" from "display/driver removed" from "window gone" in the
  lost callback's status, since the right response differs (re-upload, switch adapter, give up).

### 12.7 Sharing, explained

- [ ] **Preflight.** `tesseraCanShare(a, b)` (or a config + context form) returns *why not* before a failed
  create: API mismatch, profile mismatch (CGL legacy vs core), different backend, different display, lost parent.
  Today these are discovered as failures of the create call. **ABI**
- [ ] **Introspection.** Share-group id and member count for a context, so a host can reason about what survives a
  loss without keeping its own bookkeeping. **ABI**
- [ ] **Cross-format sharing on WGL/GLX.** Establish by test which pixel-format/FBConfig differences break sharing
  on each real driver and record them; make `tesseraCanShare` and the ranking in 12.1 prefer the parent's format
  when a share parent is given.

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

### Chunk 1 - Remote CI and release pipeline proof (first: it unblocks macOS and the release)
Needs: nothing. The first real macOS run (19 Nov) failed to link (`makeCglBackend` was not linked into `test_core`/`test_fuzz`); that is fixed, the next run is the first real CGL result.
- [ ] Get both macOS jobs green: read the first real `test_cgl` result, fix the hand-written CGL constants / alpha-in-ColorSize / forward-compatible-bit risks listed in 10.1, then run the Java tests on the runner.
- [ ] Dispatch `release.yml` as a dry run (no tag), fix whatever breaks (multi-platform assemble, the Doxygen zip, the macOS native), and download and inspect the artifacts.
- [ ] Confirm `nightly.yml` runs (soak + fault sweeps + fuzz) and record the first results; fix flakes.
- [ ] Run vcpkg for real (a CI job) against `packaging/vcpkg`, fix the port, and put the result in `docs/releasing.md`.
- [ ] Doc the review/`@claude` workflows (model pin, cost, who can trigger) in `docs/ci.md`.

### Chunk 2 - Swap control and frame pacing (ABI 0.10.0)
Needs: nothing (real vsync behaviour needs a display; window attach tests already exist on WGL/GLX).
- [ ] Report what was actually applied by `tesseraSetSwapInterval` (read back where the API allows; otherwise "requested / unverifiable") and whether adaptive sync is available (`WGL_EXT_swap_control_tear`, `GLX_EXT_swap_control_tear`).
- [ ] Cover `GLX_EXT/MESA/SGI_swap_control` and the EGL `eglSwapInterval` clamp; test on a fake backend with the extension matrix, and on real WGL; record that WSLg/llvmpipe advertise none.
- [ ] A vsync-off benchmark mode in `tessera-diag` for a window-attached context (frame-time min/avg/p99, dropped-frame count).
- [ ] DWM swap-interval quirk test on Windows (attached window, composited vs. fullscreen).
- [ ] Java binding, `docs/java.md`, ADR 0012.

### Chunk 3 - Window attach beyond WGL/GLX (needs the 10.5 decision "on-screen in 1.0?")
Needs: the owner decision; WSLg for X11 and Wayland testing.
- [ ] EGL window surfaces (an X11 window through EGL), with the same borrow/ownership rules as the WGL/GLX attach.
- [ ] Wayland `wl_surface` attach, tested against WSLg's compositor (and GLFW's Wayland platform through LWJGL).
- [ ] Java `createForWindow` exercised with a real window on Linux (GLFW `NO_API` and AWT), not only argument validation.
- [ ] Design note for macOS `NSView` attach (not implementable without a Mac) and ADR 0013 covering resize, thread and teardown rules for borrowed windows.

### Chunk 4 - Driver features that exist but are unproven or missing
Needs: nothing extra; multi-GPU and AMD/Intel parts of these stay in 10.1.
- [ ] Context priority (`EGL_IMG_context_priority`, `WGL_*_priority`): implement, read back what was granted, test on Mesa.
- [ ] EGL device selection by render node, PCI address and UUID (`EGL_EXT_device_persistent_id`) in `TESSERA_EGL_DEVICE`; test the matching logic with a faked device list.
- [ ] Detect whether the host already called `XInitThreads` (10.3) and document what Tessera does either way.
- [ ] A verification matrix for robust access, reset notification, `KHR_no_error` and release-flush on NVIDIA/WGL and Mesa, recorded in `docs/compat.md` (what was granted, what was refused, what is untested).

### Chunk 5 - Verification and hardening of what already exists
Needs: WSL for the sanitizer builds. A real GPU reset needs the owner's consent to provoke a TDR.
- [ ] Run ASan/UBSan and TSan on Linux for everything added since ABI 0.7.0 (pool top-up, probe, lost events).
- [ ] Extend `fuzz_api` / `test_fuzz` to the newer entry points (`tesseraPoolTopUp`, `tesseraGetPooledCount`, `tesseraIsContextLost`, the callbacks) and fuzz the JNI glue.
- [ ] A Java test that makes lost events travel through the JNI and FFM upcalls end to end (needs a test-only hook in the native library).
- [ ] A longer coexistence run (hours: JavaFX, GLFW and Java2D next to Tessera) as a scheduled job; record the table.
- [ ] A real GPU reset on Windows (a deliberately long shader in a throwaway process to trigger TDR) with a robust context, only with the owner's go-ahead; it can blank the screen.

### Chunk 6 - 1.0 decisions and the freeze machinery
Needs: the owner for the decisions; the tooling needs nothing.
- [ ] A one-page decision memo with a recommendation for each 10.5 item (on-screen in 1.0, GL/GLES floor, bundled ANGLE/Mesa, names and cadence).
- [ ] An ABI check in CI: exported symbols and struct layouts compared with a stored baseline, failing on any change that is not an append (`docs/abi-policy.md` made executable).
- [ ] A compatibility-matrix generator that builds `docs/compat.md` tables from `tessera-diag --json` output, so each new machine is one command.
- [ ] A release-candidate checklist and the freeze procedure (`0.x` to `1.0.0`: header version, ABI version, tag, notes).

### Chunk 7 - Software/headless fallback and ANGLE/Mesa (needs the 10.5 decision "do we ship Mesa/ANGLE?")
Needs: the owner decision; downloaded ANGLE or Mesa builds to test against.
- [ ] ADR on the loader policy (system only vs. bundled), including the `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` handling for Windows.
- [ ] Build the EGL backend on Windows and run it on ANGLE.
- [ ] A software fallback path (OSMesa or llvmpipe through EGL) selectable by `TESSERA_BACKEND`, and tested on a machine with the GPU driver hidden.
- [ ] Packaging: where the optional binaries live in the release and how the SBOM lists them.

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
