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

Verified here: WGL (NVIDIA), GLX and EGL (Mesa), Java JNI+FFM on Windows (JavaFX D3D and software pipelines), Java JNI on Linux (JavaFX software **and OpenGL/es2** pipelines, LWJGL; Mesa D3D12 via WSLg and llvmpipe), ASan/UBSan/TSan on everything that needs no GPU.

**CI (GitHub Actions) has run** and everything passes except the two macOS jobs, which never got a runner (relabelled `macos-latest`; result unknown). Passing: MSVC Debug/Release, gcc 13 Debug/Release, clang 18, ASan+UBSan, TSan, WGL on the hosted GDI-Generic GL 1.1 renderer, GLX+EGL on Xvfb/llvmpipe, Java on JDK 21 (JNI) and 25 (FFM), lint (clang-format 18.1.8 + clang-tidy). Running it found and fixed: a stack-overflow in the scoped X error handler (JavaFX es2), a crash at thread exit after `tesseraTerminate` (GLX connection now kept open), best-effort requests returning a context below the requested minimum version (GDI GL 1.1), plus several warnings that only newer compilers report. All of those jobs are now blocking. See Phase 10 for everything else.

### Decisions that departed from the original plan (kept because they explain the code)

- Windows work happens on one private message-pumping host thread (always on), not an optional mode (ADR 0004).
- `XInitThreads()` is called by Tessera before opening its own connection (ADR 0005); never `eglTerminate` (ADR 0006).
- EGL has no native X11/Wayland/GBM platforms: Tessera renders to pbuffers or nothing and needs no window system connection.
- FFM bindings are hand-written, not `jextract`ed; the JNI glue is compiled into the same native library (ADR 0009).
- `tesseraDestroyContext` of a context current on another thread succeeds and defers the teardown to that thread (changed API in 0.4.0).
- Interop is "hand out the native objects", not a wrapper API (ADR 0010).
- No quirks database yet: there is not one reproducible quirk to put in it.

---

## Phase 10 — Finish what is unverified (the road to 1.0)

Everything here was implemented-but-untested, deliberately skipped, or needs something the development machine does not have. Grouped by what is needed to close it.

### 10.1 Needs other hardware or operating systems

- [ ] **macOS**: run `tests/test_cgl.cpp` on a real Mac (Apple Silicon and Intel). The hosted `macos-latest` job is the first chance; it has not produced a result yet. Biggest risks: hand-written CGL constants, whether `kCGLPFAColorSize` includes alpha, whether core contexts report the forward-compatible bit, GPU-less CI VMs. Then run the Java tests and the JavaFX (Metal) coexistence case there.
- [ ] **Other GPU vendors**: AMD, Intel (Windows and Linux), NVIDIA proprietary on Linux. Re-run the attribute matrix, window attach, `dxinterop` and the soak; fill `docs/compat.md`.
- [ ] **Real X server / non-WSL Linux**: a real Xorg session with a hardware driver, non-GLVND `libGL`, 32-bit. (Xvfb + llvmpipe and clang are done in CI.) Swap-control extensions are untested because neither WSLg nor llvmpipe advertises them.
- [ ] **Multi-GPU**: render-node and PCI matching in `TESSERA_EGL_DEVICE` (index 0 and the refusal paths are tested; matching is not). Add UUID selection (`EGL_EXT_device_persistent_id`).
- [~] **Java on Linux and macOS**: Linux is done (JDK 21 locally and in CI, JNI; JDK 25 FFM in CI). macOS: not run.
- [~] **JavaFX `es2` (OpenGL) pipeline next to Tessera**: passes on Linux with Mesa (D3D12 and llvmpipe, 25/25 runs after the fix; ~50% crashed before it, which is how the X error handler bug was found). Needs `-Dprism.forceGPU=true` because JavaFX does not allow-list Mesa. Not done: a real hardware driver (NVIDIA/AMD/Intel on Xorg), a run of hours, macOS (Metal/ES2).
- [ ] **A real GPU reset** (TDR / device removal) with a robust context: `GUILTY`/`INNOCENT`/`UNKNOWN`, sticky lost state and recreate are tested only on a fake backend. Also hot-unplug and display changes.

### 10.2 Implemented but never exercised

- [ ] Robust access, reset notification, `KHR_no_error`, release-flush and context priority on drivers that grant them (WGL, GLX `_robustness`/`_no_error`, EGL flush control).
- [ ] DWM swap-interval quirks; `GLX_EXT/MESA/SGI_swap_control` on a server that advertises them.
- [ ] Share-order validation on WGL (not implemented) and context priority (not implemented).
- [ ] Fault-injection of the WGL, GLX and EGL failure paths (done for CGL only; the others are covered by the matrix and earlier tests).
- [ ] The Linux memory growth seen in the soak (+23-27 MB per ~1800 cycles with 3 threads, +2.5 MB with 1): LeakSanitizer shows no Tessera leak, but reachable growth (ours or Mesa's per-thread caches) is not ruled out. Compare against a driver that is not Mesa-on-D3D12, or instrument the handle table/groups/zombies sizes.
- [x] CI: the workflow has run; see the status above. Open: the macOS jobs, and a scheduled (nightly) soak.

### 10.3 Not implemented (known gaps)

- [ ] Drawable-less `SURFACE_NONE` on WGL and GLX (currently an internal 1x1 window).
- [ ] Detecting whether the host already called `XInitThreads`.
- [ ] Multi-GPU selection on Windows (`WGL_NV_gpu_affinity`, `WGL_AMD_gpu_association`, DXGI preference); document that `NvOptimusEnablement` must be exported by the host EXE.
- [ ] ANGLE / Mesa-on-Windows through the EGL backend: the EGL backend is built on Linux only, and the Windows loader is restricted to System32 by design (loading ANGLE from an application directory needs `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` handling and a test). Same for Metal-backed ANGLE on macOS (CGL is deprecated).
- [ ] Interop beyond the handles: dma-buf export/import with fences (Linux), IOSurface (macOS), Vulkan `GL_EXT_memory_object` + semaphores. Each needs a consumer to test against.
- [ ] Window attach for EGL (window surfaces), Wayland `wl_surface`, macOS `NSView`. (Windows and X11 are done.)
- [ ] The quirks database: add the machinery with the first real, regression-tested quirk, not before.

### 10.4 Release engineering

- [ ] **Java bindings for the Phase 8 and probe calls** (`getNativeHandles`, `prewarm`, window attach, `probe`) and tests; choose the real package name (`dev.tessera` is a placeholder); consider a `module-info`.
- [ ] **Maven Central artifact** with per-platform natives (the jar currently embeds only the natives of the machine that built it), signing, javadoc/sources jars.
- [ ] **vcpkg and Conan recipes** (the CMake package itself is verified).
- [ ] Generated API reference (the header comments are the reference today).
- [ ] Coverage-guided fuzzing (libFuzzer/AFL targets) and fuzzing of the JNI glue and Java layer; an independent security review.
- [ ] The 24 h soak (`TESSERA_SOAK_SECONDS=86400`), tracking X resources and Mach ports as well as handles and memory.
- [ ] Coexistence matrix filled in: JavaFX {d3d, es2, sw, metal} x OS x Tessera {hidden, offscreen, surfaceless, shared}. Only d3d and sw on Windows are done.

### 10.5 Decisions only the project owner can make (these gate the 1.0 freeze)

- [ ] Is on-screen rendering into caller-supplied windows part of 1.0? (It exists for WGL/GLX; it is the biggest API surface still open.)
- [ ] Minimum GL and GLES version floor (3.2 core? 2.1 compat? GLES 2.0?).
- [ ] Do we ship our own ANGLE/Mesa builds, or only detect system ones?
- [ ] Real package/group names, license, release cadence.
- [ ] **Freeze ABI v1.** Deliberately not done: the ABI changed as recently as 0.5.0 and only one GPU vendor has been tested. Freezing is a one-line version change; living with a wrong freeze is not. Suggested gate: 10.1 (at least AMD or Intel on Windows, and macOS) plus the decisions above plus CI green on a real remote.

**Exit for 1.0:** the 10.1 items that have hardware available are green and recorded in `docs/compat.md`; 10.5 decided; CI has run green at least once; ABI frozen and tagged.

---

## Phase 11 — Feature candidates (post-1.0 unless noted)

New capabilities worth considering, with a recommendation. Items 1 and 2 are done; the rest are not started. Ordered by value per effort.

1. ~~**`tessera-diag` command-line tool**~~ **Done** (`docs/diag.md`): text and JSON report, per-backend strict-request matrix, runs on Windows (WGL) and Linux (GLX, EGL under WSLg); macOS/CGL untested like the rest of CGL.
2. ~~**Capability probe API**~~ **Done**: `tesseraProbe` (ABI 0.6.0), tested on WGL/GLX/EGL with a foreign context current, on the fake backend, fuzzed, and under ASan/UBSan/TSan. Not done: a Java binding for it.
3. **Software/headless fallback backend** (OSMesa or a bundled llvmpipe/WARP path): guarantees `createContext` works on machines with no GPU or broken drivers (CI runners, remote desktops, containers). Related to the "do we ship Mesa" decision.
4. **Windows multi-GPU selection** (10.3) exposed as a config field (`adapterPreference`: default / high-performance / low-power / by LUID), with the same "never silently another GPU" rule used for EGL.
5. **Context-loss recovery helper**: a callback or event when a context is lost, plus an opt-in "recreate and re-share" helper, so applications do not each write the same polling loop.
6. **Frame pacing helpers** for window-attached contexts: swap-interval negotiation reported back (what was applied), adaptive-sync availability, and a vsync-off benchmark mode. Only for attached windows; Tessera's own hidden drawables do not present.
7. **Pool improvements**: automatic background refill with a user-supplied thread (or a callback), per-configuration pool sizes, a Java API. Only never-used contexts, as before.
8. **Metal/ANGLE path on macOS** and **ANGLE on Windows/Linux** as an alternative GLES provider (10.3), once a loader policy is decided.
9. **More language bindings** over the C ABI, cheap because it is stable and struct-size-prefixed: C#/.NET (P/Invoke), Rust (`-sys` crate), Python (`ctypes`/`cffi`). Only worth it if there is a consumer; the Java layer is the template.
10. **Wayland-native window attach** and **EGL window surfaces** (10.3), if the answer to "on-screen in 1.0?" is yes.
11. **Release hardening**: SBOM, reproducible builds, signed releases, a nightly soak on a self-hosted GPU runner publishing the table to the repo.
12. **Out of scope on purpose** (keep it that way unless the principles change): creating or managing windows for the application, loading GL function pointers for the application beyond `getProcAddress`, wrapping GL itself, input handling. Tessera is the context layer; GLFW/SDL/LWJGL own the rest.

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
| M5 – 1.0 | 10 | Open: other hardware, a real CI run, owner decisions, then freeze |
| Later | 11 | Candidates |
