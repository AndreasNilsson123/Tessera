# Continuous integration

Workflow: `.github/workflows/ci.yml`. Every job uses the presets in `CMakePresets.json`, so a local run and a CI run
configure the project identically.

| Job | What it runs | Blocking |
|---|---|---|
| `build` | MSVC (Debug+Release), gcc (Debug+Release), clang, macOS arm64: build with warnings as errors, run all tests that need no GPU (`abi`, `header_c`, `core`) | yes |
| `sanitizers` | clang ASan+UBSan and TSan over the same tests | yes |
| `wgl-integration` | the WGL test against the hosted runner's software OpenGL | no (informational) |
| `gl-linux-software` | the GLX and EGL tests under Xvfb with Mesa llvmpipe (software GL, no GPU) | no (informational until first green run) |
| `cgl-macos` | the CGL test against the real OpenGL.framework on a hosted macOS VM (never run) | no (informational) |
| `java-linux` | the Java bindings and their tests (JNI + FFM, LWJGL, JavaFX) on Linux under Xvfb + Mesa (never run) | no (informational) |
| `lint` | clang-format check, clang-tidy build | no (informational) |

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
- Never run yet: the workflow itself (no remote exists), clang builds, macOS, the `lint`, `gl-linux-software`, `cgl-macos` and `java-linux` jobs, and the runner image names
  (`windows-2022`, `ubuntu-24.04`, `macos-14`). Expect to adjust on first push.
- `lint` is informational because the existing code was not produced by clang-format; blocking it needs a one-time
  reformat commit.
- No x86-64 macOS job, and no GPU job: real-driver coverage comes from local runs until a self-hosted runner exists.

## Java

`cmake --preset java` builds the bindings (needs a JDK; `JAVA_HOME` selects it) and `ctest --preset java -C Debug` runs the
Java tests, which need a real OpenGL driver and so carry the `gpu` label. `python scripts/fetch-java-deps.py` fetches the
pinned LWJGL and JavaFX jars that the optional integration tests use. See `docs/java.md`. Locally verified on Windows (NVIDIA)
with JDKs 17, 19, 21, 22, 25 and 28; the Linux job above is the first attempt at Linux and has never run.

## Fuzzing and soak (Phase 9)

- `fuzz` (no GPU, in every preset including ASan/UBSan/TSan): seeded random, structure-aware inputs against the public API
  with exact-size heap buffers, so any over-read/over-write is a sanitizer report. `TESSERA_FUZZ_SEED` and
  `TESSERA_FUZZ_ITERATIONS` (default 20000 per section) change the stream; a failing run prints its seed. Run a few seeds at
  100000 iterations before a release: `for s in 1 2 3; do TESSERA_FUZZ_SEED=$s TESSERA_FUZZ_ITERATIONS=100000 build/asan-ubsan/tests/test_fuzz; done`.
- `soak` (label `gpu;soak`, real driver): create/use/switch/destroy loops on several threads, with handle / GDI / USER / fd /
  memory counters sampled only while no context is alive. The first tenth is warm-up; growth after it fails the test.
  `TESSERA_SOAK_CYCLES` (per thread, default 1500, about a minute) or `TESSERA_SOAK_SECONDS` (e.g. 86400) scale it. The
  informational GPU CI jobs run the default size.

