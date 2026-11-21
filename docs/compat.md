# Compatibility matrix

Only what was actually run is marked as verified. "Not run" means there is no evidence either way, not that it is broken.
This project has been developed on one machine plus GitHub's hosted runners, so most vendor/OS rows are still empty; filling them
is the point of this file. Last updated at ABI 0.13.0.

## Verified

| Platform | Backend | GPU / driver | Suites run | Result |
|---|---|---|---|---|
| Windows 11 (the development machine) | WGL | NVIDIA GeForce RTX 3060 Laptop, driver 546.30, 120 Hz display (DWM) | everything that needs no CI: `abi`, `header_c`, `core`, `fuzz`, `wgl` (192-combination attribute matrix, window attach in the integer and described forms, pooling, native handles, swap control and DWM frame pacing, priority refusal, probe), `dxinterop` (GL<->D3D11 shared texture), `fault_wgl`, `soak`, `tessera-diag` incl. `--bench-swap`; Java: JNI + FFM (SelfTest, lost events through the real upcalls with the test hook, hostile-argument fuzz under `-Xcheck:jni`), LWJGL 3.3.4, GLFW (own window next to Tessera; attach to a `GLFW_NO_API` window), Java2D OpenGL (AWT), JavaFX 21 (D3D and software pipelines) | pass |
| Ubuntu 22.04 (WSL2 + WSLg) | GLX | Mesa 23.2.1, D3D12 driver over the RTX 3060 | `glx` (window attach, swap control, priority refusal, pooling, native handles), `glx_xinitthreads_host` / `_none`, `fault_glx`; Java JNI (SelfTest, lost events, GLFW on X11) | pass |
| Ubuntu 22.04 (WSL2 + WSLg) | EGL | Mesa 23.2.1 (surfaceless platform, device platform index 0) | `egl` (device selection refusals, priority refusal, pooling, native handles), `eglimage`, `fault_egl`, **`eglwindow_x11` and `eglwindow_wayland`** (an X11 window and a real Wayland surface on WSLg's compositor, through EGL); Java: GLFW on its **Wayland platform** attached through `createForNativeWindow` (`java_glfw_wayland`) | pass |
| macOS on GitHub's hosted `macos-latest` VM (arm64) | CGL | **Apple Software Renderer** (OpenGL 2.1 APPLE-23.1.1 legacy, 4.1 core), no GPU | `cgl` (the real `OpenGL.framework`, first run 19 Nov 2026), `cgl_fake`, `core`, `fuzz`; Java JNI + FFM on JDK 21 and 25 (SelfTest, LWJGL, JavaFX software pipeline; GLFW with `-XstartOnFirstThread`) | pass |
| Windows 11 (the development machine) | EGL on **ANGLE** (the libEGL of VS Code/Electron, `TESSERA_EGL_LIBRARY`) | ANGLE over Direct3D 11 on the NVIDIA RTX 3060 Laptop (ES 3.0), and ANGLE's **WARP** (Microsoft Basic Render Driver) through `TESSERA_BACKEND=software` | `software` and `angle_egl` (`test_software`): pbuffer context, clear and read back, an `HWND` window surface with swap and swap control, desktop GL refused with `UNSUPPORTED`; a relative `TESSERA_EGL_LIBRARY` refused | pass |
| Ubuntu 22.04 (WSL2 + WSLg) | EGL, `software` | Mesa 23.2.1 **llvmpipe** (with `LIBGL_ALWAYS_SOFTWARE=1`; without it the backend refuses the D3D12 GPU driver, which also reports `EGL_MESA_device_software`) | `software`: ES 3.2 context, clear and read back, desktop GL 3.3 core also created | pass (and the refusal path) |
| Windows Server on GitHub's hosted runner | WGL | Microsoft GDI Generic (OpenGL 1.1), no GPU | `wgl` (it found two real bugs: best-effort requests below the minimum version, and a best-effort priority request failing on a driver without `ARB_create_context`), soak, diag | pass |
| Ubuntu 24.04 on GitHub's hosted runner | GLX + EGL | Mesa llvmpipe over Xvfb | `glx`, `egl`, Java JNI (JDK 21) and FFM (JDK 25), JavaFX software and **OpenGL (es2)** pipelines next to Tessera, LWJGL, GLFW | pass |
| Ubuntu 22.04 (WSL2), gcc 11 | none (no GPU needed) | none | ASan+UBSan and TSan over `abi`, `header_c`, `core` (incl. the threaded lost-callback test), `fuzz`, CGL-against-fake | pass |
| Ubuntu 24.04 on GitHub's hosted runner | none | none | libFuzzer on the C API (`fuzz_api`, every entry point added up to 0.12.0) and the version parser: 1 minute per push, 10 minutes nightly | pass |
| vcpkg port (Linux, Windows, macOS), Conan recipe (Linux) | none | none | install from the checkout + `find_package` consumer (C-only) | pass |

JDKs: SelfTest ran on 17 and 21 (JNI), 22, 25 and 28 (FFM), 19 (JNI) locally; 21 (JNI) and 25 (FFM) in CI on Linux and macOS.

## Optional features, by driver (strict requests, from `tessera-diag`)

`ok` = a strict request was satisfied and read back; `refused` = the driver cannot give exactly that (a best-effort request would
drop it with a log line). Nothing here is assumed from a spec: each cell is a context that was created, inspected and destroyed.

| Feature | WGL, NVIDIA 546.30 | GLX, Mesa 23.2 D3D12 | EGL, Mesa 23.2 D3D12 | CGL, Apple Software Renderer |
|---|---|---|---|---|
| Highest GL core profile | 4.6 | 4.2 | 4.2 (GLES 3.1) | 4.1 (forward-compatible only) |
| OpenGL ES contexts | yes (3.2) | yes (3.1) | yes (3.1) | no (UNSUPPORTED: use ANGLE/EGL) |
| Debug context | ok | ok | ok | refused |
| Forward-compatible | ok | ok | ok | ok (always, for core) |
| Robust access / reset notification | ok / ok | **refused** (`GLX_ARB_create_context_robustness` missing) | ok / ok | refused |
| `KHR_no_error` | ok | ok | ok | refused |
| Release-flush control | ok | ok | ok | refused |
| 4x / 8x MSAA | ok / ok | ok / ok | ok / ok (pbuffer) | ok / ok (reported 9 samples for 8) |
| sRGB framebuffer | ok | ok | refused (`BAD_MATCH` on the pbuffer) | refused |
| Float colour buffer | ok (16-bit) | refused | ok (16-bit) | ok (32-bit) |
| Stereo | refused | refused | refused | refused |
| Double-buffered | ok | ok | refused (pbuffers/surfaceless are single-buffered; **window surfaces are double-buffered**) | ok |
| Pbuffer / no-drawable (FBO only) | ok / ok (a 1x1 window; WGL cannot go without) | ok / ok | ok / ok | ok / ok |
| Swap control (window) | control + adaptive + read-back (0, 1, 2, -1 read back as requested) | none | EGL window surfaces: control, no adaptive, no read-back | none |
| Context priority | **no WGL extension exists** (strict refused) | refused (`GLX_EXT_context_priority` not advertised) | refused (`EGL_IMG_context_priority` not advertised) | n/a |
| Device UUID / render node (`EGL_EXT_device_persistent_id`) | n/a | n/a | the single WSL device reports neither | n/a |

Not covered by a cell: a driver that **grants** a context priority, a device UUID that is **reported**, and swap control through a
GLX server that advertises the extensions: those code paths are tested by unit tests over their decision logic and over fakes
(`test_core`), not against a real driver.

## Frame pacing, measured

`tessera-diag --bench-swap` in a 640x360 window on the RTX 3060 Laptop (120 Hz, DWM): vsync on 8.32 ms/frame (120.2 fps, 0 dropped),
off about 0.4 ms (2100+ fps), adaptive 8.32 ms. A single-buffered window never paces.

## Leak and soak data

- Windows, GDI software renderer on a hosted runner, 15 minutes (nightly): handles 8, GDI 0, USER 0 constant; memory 351 MB after
  warm-up, 358 MB at 830 s (flat within the allocator's noise). The ctest timeout had cut the run off at 900 s; fixed.
- Windows, NVIDIA, development machine: 10,200 create/use/destroy cycles on 3 threads, handles/GDI/USER flat.
- Linux: the 30-minute nightly soak has the same timeout fix; the long coexistence workflow (`coexistence-long.yml`) records its
  tables in the job summaries (see the roadmap, chunk 5, for what has been reviewed).
- Memory growth seen with 3 threads on Mesa's D3D12 driver is the driver's (a raw libX11/libGL loop grows the same way).

## Not run

| Combination | Why it matters |
|---|---|
| AMD, Intel (Windows and Linux), NVIDIA proprietary on Linux | Vendor drivers differ in `WGL_ARB_create_context` strictness, robustness, flush control and sRGB/float format support; the attribute matrix and strict/best-effort behaviour should be rerun on each. Context priority and device UUIDs are only grantable there |
| macOS on real hardware (Apple Silicon, Intel) | The hosted VM only has Apple's software renderer: no GPU-specific behaviour, no Metal-backed JavaFX run, no window server session |
| A real X server with a hardware driver (Xorg) | GLX swap-control extensions are untested because neither WSLg nor llvmpipe advertises them |
| ANGLE | Not supported yet (loader restrictions, see ROADMAP 10.3) |
| AWT/Swing windows as an attach target | Their native window ids are not public API |
| macOS `NSView` attach | Designed only (ADR 0014) |
| A real GPU reset (TDR) with robustness | `getResetStatus`, loss, lost events and recreate are tested against fakes, through the Java upcalls with a test hook, and by polling healthy contexts; no driver has been made to reset |
| Multi-GPU machines | Render-node / PCI / UUID matching is tested on faked device lists only; hybrid Intel+NVIDIA/AMD for `adapterLuid` / `gpuPreference` |
| A surface that is shown on Wayland (frame callbacks) | The test surface is never mapped, so vsync swaps are not exercised, only interval 0 |

## How to contribute a row

```
cmake --preset ci && cmake --build --preset ci
build/ci/tools/diag/tessera-diag --json > diag.json   # what this machine can create (docs/diag.md)
build/ci/tools/diag/tessera-diag --bench-swap 3       # frame pacing, where a window can be made
ctest --preset ci-gpu                                  # needs a real driver; prints the renderer string
TESSERA_SOAK_CYCLES=3000 build/ci/tests/test_soak      # leak table
```

Record OS, GPU, driver version, the suites that ran, and failures verbatim.
