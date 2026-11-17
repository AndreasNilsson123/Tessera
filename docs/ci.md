# Continuous integration

Workflow: `.github/workflows/ci.yml`. Every job uses the presets in `CMakePresets.json`, so a local run and a CI run
configure the project identically.

| Job | What it runs | Blocking |
|---|---|---|
| `build` | MSVC (Debug+Release), gcc (Debug+Release), clang, macOS arm64: build with warnings as errors, run all tests that need no GPU (`abi`, `header_c`, `core`) | yes |
| `sanitizers` | clang ASan+UBSan and TSan over the same tests | yes |
| `wgl-integration` | the WGL test against the hosted runner's software OpenGL | no (informational) |
| `lint` | clang-format check, clang-tidy build | no (informational) |

Tests labelled `gpu` (currently `wgl`) need a real OpenGL driver and are excluded from the default `ci` test preset.

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

- Verified locally: the `ci` preset on Windows/MSVC, and `ci`, `asan-ubsan`, `tsan` on Ubuntu 22.04/gcc 11 (WSL).
- Never run yet: the workflow itself (no remote exists), clang builds, macOS, the `lint` job, and the runner image names
  (`windows-2022`, `ubuntu-24.04`, `macos-14`). Expect to adjust on first push.
- `lint` is informational because the existing code was not produced by clang-format; blocking it needs a one-time
  reformat commit.
- No x86-64 macOS job, and no GPU job: real-driver coverage comes from local runs until a self-hosted runner exists.
