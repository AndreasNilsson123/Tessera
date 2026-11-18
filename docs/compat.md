# Compatibility matrix

Only what was actually run is marked as verified. "Not run" means there is no evidence either way, not that it is broken.
This project has been developed on one machine, so most rows are empty; filling them is the point of this file.

## Verified

| Platform | Backend | GPU / driver | Suites run | Result |
|---|---|---|---|---|
| Windows 11 | WGL | NVIDIA GeForce RTX 3060 Laptop, driver 546.30 | `abi`, `header_c`, `core`, `fuzz`, `wgl` (incl. 192-combination attribute matrix, window attach, pooling, native handles), `dxinterop` (GL<->D3D11 shared texture), `soak`; Java: JNI + FFM, embedded, shutdown, LWJGL 3.3.4, JavaFX 21.0.5 (D3D and software pipelines) | pass |
| Ubuntu 22.04 (WSL2 + WSLg) | GLX | Mesa 23.2.1, D3D12 driver over the RTX 3060 | `glx` (incl. window attach, pooling, native handles) | pass |
| Ubuntu 22.04 (WSL2 + WSLg) | EGL | Mesa 23.2.1 (surfaceless platform; device platform index 0) | `egl` (incl. device selection refusal paths, pooling, native handles) | pass |
| Ubuntu 22.04, gcc 11 | none | no GPU | ASan+UBSan, TSan: `abi`, `header_c`, `core`, `fuzz`, CGL-against-fake tests | pass |

JDKs: SelfTest ran on 17 and 21 (JNI), 22, 25 and 28 (FFM), 19 (JNI, FFM preview ignored).

## Not run

| Combination | Why it matters |
|---|---|
| AMD, Intel (Windows and Linux), NVIDIA proprietary on Linux | Vendor drivers differ in `WGL_ARB_create_context` strictness, robustness, flush control and sRGB/float format support; the attribute matrix and strict/best-effort behaviour should be rerun on each |
| Apple Silicon / macOS (CGL) | The CGL backend has only ever run against a fake driver (`test_cgl.cpp` is ready for a real Mac) |
| Mesa llvmpipe on a real X server (Xvfb) | CI configures it, never run |
| ANGLE | Not supported yet (loader restrictions, see ROADMAP 10.3) |
| Wayland, `wl_surface` attach | No Wayland backend |
| JavaFX es2 (OpenGL) pipeline next to Tessera (Linux) | The only JavaFX pipeline where a real GL-context conflict is possible |
| Java tests on Linux/macOS | JNI glue is compile-checked only |
| Real GPU reset (TDR) with robustness | `getResetStatus`/loss paths are tested against fakes and by polling a healthy context |
| Multi-GPU machines | Render-node / PCI device selection matching |

## How to contribute a row

```
cmake --preset ci && cmake --build --preset ci
build/ci/tools/diag/tessera-diag --json > diag.json   # what this machine can create (docs/diag.md)
ctest --preset ci-gpu                           # needs a real driver; prints the renderer string
TESSERA_SOAK_CYCLES=3000 build/ci/tests/test_soak   # leak table
```

Record OS, GPU, driver version, the suites that ran, and failures verbatim.
