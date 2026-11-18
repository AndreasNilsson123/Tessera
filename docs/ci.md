# Continuous integration

Workflow: `.github/workflows/ci.yml`. Every job uses the presets in `CMakePresets.json`, so a local run and a CI run
configure the project identically.

| Job | What it runs | Blocking |
|---|---|---|
| `build` | MSVC (Debug+Release), gcc (Debug+Release), clang: build with warnings as errors, run all tests that need no GPU (`abi`, `header_c`, `core`, `fuzz`, `cgl_fake`, ...). The macOS arm64 entry is `experimental` | yes (macOS: no) |
| `sanitizers` | clang ASan+UBSan and TSan over the same tests | yes |
| `wgl-integration` | the WGL test, soak and diag against the hosted runner's software OpenGL (GDI Generic, GL 1.1) | yes |
| `gl-linux-software` | the GLX and EGL tests under Xvfb with Mesa llvmpipe (software GL, no GPU) | yes |
| `java-linux` | the Java bindings and their tests (JNI on JDK 21, FFM on JDK 25, LWJGL, JavaFX software and OpenGL pipelines) on Linux under Xvfb + Mesa | yes |
| `lint` | clang-format 18.1.8 check, clang-tidy build | yes |
| `cgl-macos` | the CGL test against the real OpenGL.framework on a hosted macOS VM | no (informational: has never produced a result) |

`cgl_fake` (the macOS backend against a fake framework, including a fault-injection sweep) and the two unavailable-library runs execute on every POSIX job and in the sanitizer presets; `core` includes the lifecycle/threading tests that need no GPU. Tests labelled `gpu` (`wgl` on Windows, `glx` and `egl` on Linux, `cgl` on macOS) need a real OpenGL driver and a display and are excluded from the default `ci` test preset.

## Running locally

```
cmake --preset ci && cmake --build --preset ci --config Debug && ctest --preset ci -C Debug
ctest --preset ci-gpu -C Debug        # also runs the GPU-labelled tests (needs a driver)
```

Linux, or Windows via WSL (copies the tree to a scratch directory, builds there):

```
scripts/check-linux.sh                 # "ci" preset
scripts/check-linux.sh asan-ubsan tsan
```

TSan aborts under the high-entropy ASLR of newer kernels (including WSL2); the script runs it under `setarch -R`, and the
CI job lowers `vm.mmap_rnd_bits`.

## Status and known gaps

- Verified locally: the `ci` preset on Windows/MSVC (including the real-GPU `wgl` test), and `ci`, `asan-ubsan`, `tsan` on Ubuntu 22.04/gcc 11 (WSL), where the `glx` and `egl` tests also ran against WSLg (Mesa D3D12).
- The `glx`/`egl` tests under TSan report races inside NVIDIA's WSL driver (uninstrumented, none with Tessera code on the accessing side); GPU tests are therefore excluded from the sanitizer presets.
- The workflow has run on GitHub Actions since the first push, and every job except the two macOS ones passes. The macOS jobs
  were cancelled within a second with no runner assigned (the `macos-14` label never got a machine), so the label was changed to
  `macos-latest`; **whether the CGL backend passes on a real macOS VM is still unknown.**
- What the hosted runners exercise that local runs cannot: clang 18 and gcc 13 warnings, GDI Generic GL 1.1 on Windows (it
  found that best-effort requests must still honour the minimum version), Mesa llvmpipe, and JavaFX es2 (which needs
  `-Dprism.forceGPU=true` to accept Mesa).
- `lint`: the code base is formatted with clang-format 18.1.8 (the version CI pins; checked with
  `git ls-files '*.c' '*.cpp' '*.h' '*.hpp' | xargs clang-format --dry-run --Werror`, 0 violations). Run clang-format before
  committing. Do not let it reorder Windows headers: `<windows.h>` must precede `<psapi.h>` (guarded with
  `// clang-format off`). The clang-tidy step passes in CI.
- No x86-64 macOS job, and no GPU job: real-driver coverage comes from local runs until a self-hosted runner exists.

## Java

`cmake --preset java` builds the bindings (needs a JDK; `JAVA_HOME` selects it) and `ctest --preset java -C Debug` runs the
Java tests, which need a real OpenGL driver and so carry the `gpu` label. `python scripts/fetch-java-deps.py` fetches the
pinned LWJGL and JavaFX jars that the optional integration tests use. See `docs/java.md`. Locally verified on Windows (NVIDIA)
with JDKs 17, 19, 21, 22, 25 and 28.

## Fuzzing and soak (Phase 9)

- `fuzz` (no GPU, in every preset including ASan/UBSan/TSan): seeded random, structure-aware inputs against the public API
  with exact-size heap buffers, so any over-read/over-write is a sanitizer report. `TESSERA_FUZZ_SEED` and
  `TESSERA_FUZZ_ITERATIONS` (default 20000 per section) change the stream; a failing run prints its seed. Run a few seeds at
  100000 iterations before a release: `for s in 1 2 3; do TESSERA_FUZZ_SEED=$s TESSERA_FUZZ_ITERATIONS=100000 build/asan-ubsan/tests/test_fuzz; done`.
- `soak` (label `gpu;soak`, real driver): create/use/switch/destroy loops on several threads, with handle / GDI / USER / fd /
  memory counters sampled only while no context is alive. The first tenth is warm-up; growth after it fails the test.
  `TESSERA_SOAK_CYCLES` (per thread, default 1500, about a minute) or `TESSERA_SOAK_SECONDS` (e.g. 86400) scale it. The
  informational GPU CI jobs run the default size.

## Fault injection

`fault_wgl` (Windows) and `fault_glx` / `fault_egl` (Linux, one backend per process) run `tests/test_fault.cpp`: the backends'
driver entry points are wrapped, each failable call is failed once and then every call from there on, and the public-API scenarios
(plain, pbuffer, sharing, core + debug, MSAA/sRGB ladder, no surface, recreate) must neither crash nor leak native objects nor
disturb the other library's context. Label `gpu;fault`, about a minute each.

## Coexistence tests (Java)

`java_glfw` (GLFW through LWJGL), `java_awt_opengl` (Java2D OpenGL pipeline) and the JavaFX tests run in the blocking Java jobs
under Xvfb. They exit 77 (skipped) where the host library cannot come up (no display, or Java2D refuses the driver). Both need
the LWJGL GLFW jars, which `scripts/fetch-java-deps.py` now downloads. There is no Windows Java job: the hosted runner's GDI
renderer is OpenGL 1.1, which the LWJGL rendering tests cannot use. `-DTESSERA_JAVA_STRESS=ON` adds a 6000-frame JavaFX run
(label `stress`, about 1-2 minutes) for nightly or local use.
