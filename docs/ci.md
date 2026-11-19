# Continuous integration

Workflow: `.github/workflows/ci.yml`. Every job uses the presets in `CMakePresets.json`, so a local run and a CI run
configure the project identically.

| Job | What it runs | Blocking |
|---|---|---|
| `build` | MSVC (Debug+Release), gcc (Debug+Release), clang, macOS arm64: build with warnings as errors, run all tests that need no GPU (`abi`, `header_c`, `core`, `fuzz`, `cgl_fake`, ...) | yes (macOS: informational, but green since 19 Nov 2026) |
| `sanitizers` | clang ASan+UBSan and TSan over the same tests | yes |
| `wgl-integration` | the WGL test, soak and diag against the hosted runner's software OpenGL (GDI Generic, GL 1.1) | yes |
| `gl-linux-software` | the GLX and EGL tests under Xvfb with Mesa llvmpipe (software GL, no GPU) | yes |
| `java-linux` | the Java bindings and their tests (JNI on JDK 21, FFM on JDK 25, LWJGL, GLFW, JavaFX software and OpenGL pipelines) on Linux under Xvfb + Mesa | yes |
| `java-macos` | the Java bindings on the hosted macOS VM (JDK 21 and 25) | no (informational) |
| `cgl-macos` | the CGL test against the real OpenGL.framework on the hosted macOS VM (Apple Software Renderer), then `tessera-diag` | no (informational) |
| `conan`, `vcpkg` | the Conan recipe (Linux) and the vcpkg port (Linux, Windows, macOS): install from the checkout, then build a C-only consumer with `find_package(Tessera)` | yes |
| `libfuzzer` | libFuzzer on the C API and the version parser, 1 minute | yes |
| `lint` | clang-format 18.1.8 check, clang-tidy build, the Doxygen C API reference with warnings as errors | yes |

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

- Verified locally: the `ci` preset on Windows/MSVC (including the real-GPU `wgl` test), and `ci`, `asan-ubsan`, `tsan` on Ubuntu 22.04 (WSL), where the `glx`, `egl` and `eglwindow_*` tests also ran against WSLg (Mesa D3D12, X11 and a real Wayland compositor).
- The `glx`/`egl` tests under TSan report races inside NVIDIA's WSL driver (uninstrumented, none with Tessera code on the accessing side); GPU tests are therefore excluded from the sanitizer presets.
- Everything above runs on GitHub Actions. What the hosted runners exercise that local runs cannot: clang 18 and gcc 13 warnings (gcc's `-Wpedantic` rejects `dlsym` results cast to function pointers), GDI Generic GL 1.1 on Windows (best-effort requests below the minimum version; best-effort priority on a driver without `ARB_create_context`), Mesa llvmpipe, JavaFX es2 (needs `-Dprism.forceGPU=true` to accept Mesa), the **first real macOS run** (a link error: the CGL backend was not linked into two test executables; then a CGL "relaxed" log that never fired because CGL hands back the closest pixel format instead of failing), and the vcpkg job (found that the CMake package did not call `find_dependency(Threads)` and that a static `libtessera` did not carry the C++ runtime for C-only consumers).
- `lint`: the code base is formatted with clang-format 18.1.8 (the version CI pins; checked with `git ls-files '*.c' '*.cpp' '*.h' '*.hpp' | xargs clang-format --dry-run --Werror`, 0 violations). Run clang-format before committing. Do not let it reorder Windows headers: `<windows.h>` must precede `<psapi.h>` (guarded with `// clang-format off`). The Doxygen step needs `HAVE_DOT = NO`: the runner has no Graphviz.
- No x86-64 macOS job, and no GPU job: real-driver coverage comes from local runs until a self-hosted runner exists.

## The Claude workflows

| Workflow | Trigger | What it does | Cost control |
|---|---|---|---|
| `claude-code-review.yml` | a PR from this repository that touches `src/`, `include/`, `bindings/`, `tests/`, `tools/`, `cmake/`, the CMake files or the workflows, not a draft, not Dependabot | runs `/code-review:code-review` and comments on the PR | `paths:` filter (docs-only PRs are skipped); `concurrency` cancels the review of a superseded push; forks are skipped (no secrets); pinned to **Sonnet 5.5** |
| `claude.yml` | a comment, review or issue containing `@claude` | does what the comment asks, on the PR or issue; has write access to contents, pull requests and issues | the action only responds to users with write access; pinned to **Sonnet 5.5** |

Both authenticate with the `CLAUDE_CODE_OAUTH_TOKEN` repository secret (created by `/install-github-app`). To change the model edit `claude_args: '--model ...'` in the workflow file; leaving it out uses whatever default the action ships.

## The other workflows

- **`nightly.yml`** (03:17 UTC, or on demand): 30-minute soak and the fault sweeps on Linux/Mesa, a 15-minute soak on Windows/GDI, libFuzzer for 10 minutes per target, the JavaFX/GLFW/AWT stress (30,000 frames). The soak test sets its own duration, so its ctest timeout is effectively unlimited (a 900 s ctest timeout had cut both soaks off on the first run). Failures do not block merges.
- **`coexistence-long.yml`** (Sundays, or on demand with `hours` 1-5): the same checks for hours: soak on Linux and Windows and the JavaFX stress at 40 frames/s x hours. Results are in the job summaries.
- **`release.yml`** (a `vX.Y.Z` tag, or on demand as a dry run): builds the native library on Windows, Linux and macOS, assembles the multi-platform jar, the Doxygen C API reference, the SBOM and checksums, and drafts a GitHub release. See `docs/releasing.md`.

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

## Fuzzing with libFuzzer

`cmake --preset fuzz` (clang) builds `fuzz_api` (the public C API against a fake backend: config images, structs, handles, proc
names; the first byte selects the entry point) and `fuzz_glversion` (the GL_VERSION parser) with ASan + UBSan. Seeds are in
`tests/fuzz/corpus/<target>`.

```
build/fuzz/tests/fuzz_api -max_total_time=60 tests/fuzz/corpus/fuzz_api
```

The CI `fuzz` job runs a minute per target; `nightly.yml` runs ten. A struct whose `structSize` claims more than the caller
allocated is the caller's bug and is clamped by the harness (the first run of `fuzz_api` "found" exactly that, in the harness, not in
the library). Locally: 15.9 million inputs in 91 s on `fuzz_api`, 5.2 million in 16 s on `fuzz_glversion`, no findings.

## Release and nightly workflows

`release.yml` (tag `vX.Y.Z`) builds every platform, assembles the Java release and drafts a GitHub release; `nightly.yml` runs a 30
minute soak, the fault sweeps, the long fuzz runs and the JavaFX/GLFW/AWT stress on software GL. Neither has run on GitHub yet.
See `releasing.md`. The `conan` job builds and tests the Conan package on every push.
