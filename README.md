# Tessera

A native OpenGL / OpenGL ES **context creation** library for applications that share a process with other OpenGL users
(JavaFX, AWT/Swing, GLFW, other native libraries). It creates, negotiates and manages contexts; it never takes over a
window, a display connection or the thread's current context that it did not create. Stable C ABI, C++17 core, Java bindings
(JNI for Java 11+, `java.lang.foreign` for Java 22+), an LWJGL adapter.

**Status: pre-1.0 (0.12).** Built and tested on Windows (WGL, NVIDIA), Linux (GLX and EGL on Mesa, software and D3D12, X11 and Wayland) and macOS (CGL, on GitHub's hosted VM with Apple's software renderer), with CI on all three. **Not tried:** other GPU vendors (AMD, Intel), a real Mac with a GPU, a real Xorg/Wayland desktop session, a second GPU. `ROADMAP.md` lists exactly what is and is not verified; `docs/compat.md` is the matrix; the ABI may change between minor versions until 1.0 (`docs/abi-policy.md`, `docs/freeze.md`).

## What it does

- WGL (Windows), GLX and EGL (Linux, headless/surfaceless included), CGL (macOS; run on a hosted VM with the software renderer only); desktop GL 2.1-4.6 and GLES.
- Negotiates what you ask for and reports what you got (`tesseraGetContextInfo`, `tesseraProbe`); strict or best-effort.
- Restores whatever context the calling thread had current after every call; owns its hidden windows and X connection.
- Share groups, context loss + recreate, KHR_debug forwarding, pre-warmed contexts, native handles for interop
  (e.g. GL <-> Direct3D 11 shared textures), rendering into your own window (WGL, GLX).
- `tessera-diag`: a report of what each backend can create on a machine.

## Use it

```c
#include <tessera/tessera.h>

tesseraInit();
TesseraContextConfig cfg = tesseraDefaultContextConfig();
cfg.profile = TESSERA_PROFILE_CORE; cfg.versionMajor = 3; cfg.versionMinor = 3;
TesseraContext ctx;
if (tesseraCreateContext(&cfg, &ctx) == TESSERA_SUCCESS) {
    tesseraMakeCurrent(ctx);
    /* ... GL calls via tesseraGetProcAddress ... */
    tesseraDestroyContext(ctx);
}
tesseraTerminate();
```

```java
try (Context ctx = Context.create(ContextConfig.builder().profile(Profile.CORE).version(3, 3).build())) {
    ctx.makeCurrent();
    // ... LWJGL via TesseraGL ...
}
```

CMake: `find_package(Tessera CONFIG REQUIRED)` and link `Tessera::tessera`. Conan recipe in `packaging/conan`. Maven coordinates
(not published yet): `io.github.andreasnilsson123:tessera`.

## Build

```
cmake --preset ci && cmake --build --preset ci --config Debug && ctest --preset ci -C Debug   # no GPU needed
ctest --preset ci-gpu -C Debug                                                                  # real driver tests
cmake --preset java && cmake --build --preset java --config Debug && ctest --preset java -C Debug   # Java (needs a JDK)
```

Presets for sanitizers (`asan-ubsan`, `tsan`) and libFuzzer (`fuzz`) exist; see `docs/ci.md`.

## Documentation

| | |
|---|---|
| `include/tessera/tessera.h` | the API reference (every function and struct is documented there) |
| `docs/threading.md`, `docs/coexistence.md`, `docs/troubleshooting.md` | living in a shared process |
| `docs/interop.md`, `docs/diag.md`, `docs/java.md`, `docs/environment.md` | features and configuration |
| `docs/abi-policy.md`, `docs/adr/` | stability policy and design decisions |
| `docs/security-review.md`, `docs/compat.md` | what was reviewed and what has actually been run where |
| `docs/ci.md`, `docs/releasing.md` | CI, fuzzing, soak tests, releasing |
| `ROADMAP.md` | status, open work, decisions needed before 1.0 |

## License

Apache-2.0, see `LICENSE` and `NOTICE`; third-party notices in `THIRD_PARTY_NOTICES.md` (nothing third-party is shipped).
Copyright 2026 Tessera contributors.
