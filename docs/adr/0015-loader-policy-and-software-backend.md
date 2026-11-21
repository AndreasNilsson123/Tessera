# ADR 0015: loader policy for optional GL providers, EGL on Windows (ANGLE), and the software backend

Status: accepted (chunk 7, ABI unchanged: 0.12.0). Recommended by `docs/decisions-1.0.md` item 3; the owner's decision record is still to be filled in.

## Context

Tessera loads the GL it finds: `opengl32.dll` (System32), `libGL.so.1`/`libEGL.so.1` (the loader's search), the OpenGL framework. Three
things were missing: a way to use an ANGLE or Mesa that is not on the system (an application that ships its own), a way to ask for the
CPU renderer when there is no usable GPU driver, and an answer to "do we ship those binaries ourselves?".

## Decisions

**1. System first, an explicit path second, nothing bundled by default.** The release does not contain ANGLE or Mesa. A user (or an
application that ships its own copy) names the file with `TESSERA_EGL_LIBRARY=<absolute path of libEGL>`. Without it, behaviour is
unchanged: System32 only on Windows (so a planted `libEGL.dll` next to the application is never picked up), the normal dynamic-loader
search elsewhere. Shipping a GL implementation means owning its security updates and licences; that stays a separate, optional download
(decision 5), not a property of the library.

**2. Windows: `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`, absolute paths only.** `DynamicLibrary::openPath` refuses
anything that is not `C:\...` or `\\server\...` (a relative name would make the result depend on the current directory), and loads the
file so that its *own dependencies* (ANGLE's `libGLESv2.dll`, `d3dcompiler_47.dll`) are looked up in its own folder, then in System32;
never the current directory, never `PATH`. Verified: a bare `libEGL.dll` in `TESSERA_EGL_LIBRARY` is refused (the backend reports "libEGL is
not available" and the test skips), an absolute path works with the dependencies beside it.

**3. EGL on Windows exists only for ANGLE, and only by name.** `egl_api.cpp` and `egl_backend.cpp` now build on Windows. The backend is
registered but not in the default order (WGL is the Windows default and covers desktop OpenGL); `TESSERA_BACKEND=egl` selects it, and it
needs a `libEGL` (System32 or `TESSERA_EGL_LIBRARY`). ANGLE is recognised by `EGL_ANGLE_platform_angle` and opened with
`eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, ...)`; the backend never calls `eglTerminate` (ADR 0006), and the ANGLE display is
shared like any other. What ANGLE gives: **OpenGL ES only** (a desktop-GL request is refused with `UNSUPPORTED` and a message, not
faked), pbuffer and surfaceless contexts, and `HWND` window surfaces through `tesseraCreateContextForWindow` (ADR 0014's rules apply:
borrowed window, swap control = control only).

**4. The `software` backend is a promise about the CPU, checked on what the driver says.** `TESSERA_BACKEND=software` runs the same EGL
code restricted to a CPU renderer: Mesa's software device through the device platform (`TESSERA_EGL_DEVICE=software`) on Linux, ANGLE's
WARP (Direct3D 11 software device) with SwiftShader (Vulkan) as the second choice on Windows. It is never in the default order: asking for
`TESSERA_BACKEND=wgl,software` means "the GPU, else the CPU" (a backend that cannot serve returns `UNSUPPORTED` and the next is tried).
Because `EGL_MESA_device_software` also marks GPU drivers that use the software window system (**WSL's D3D12 driver reports it for the
RTX it runs on**, found by the test), the backend creates the context and then checks `GL_RENDERER` (`isCpuRenderer`: llvmpipe, softpipe,
swrast, Software Rasterizer, SwiftShader, Microsoft Basic Render Driver / WARP, GDI Generic, Apple Software Renderer); a GPU is refused
with `UNSUPPORTED` and a message naming it. It does not mutate the process environment to force Mesa's software path: on a machine with
a GPU the user sets `LIBGL_ALWAYS_SOFTWARE=1` (the message says so).

**5. Optional providers are separate artifacts with their own SBOM entries.** `scripts/package-provider.py` packs a folder of provider
binaries as `tessera-<v>-<name>-<os>-<arch>.zip` (files, licence texts, `PROVIDER.json` with every file's SHA-256, a README on how to use
them) and refuses a folder without a licence file. `scripts/assemble-release.py --provider <...>.provider.json` copies the zip next to the
release artifacts, lists it in `SHA256SUMS`, and adds a CycloneDX component with `"scope": "optional"`, its licence, its upstream, and
one nested file component per binary. The Tessera jar and library still embed no third-party code, which the SBOM keeps saying.
The release workflow does not build or fetch a provider today (there is no upstream binary release of ANGLE to depend on; Chrome, Edge
and Electron ship builds, and a project that wants one builds ANGLE from source and runs the script).

## Consequences

- No ABI change: backend names are strings (`probe.backend` can now say `software`), the selection is the existing `TESSERA_BACKEND`.
- Tests: `tests/test_software.cpp` (Linux with Mesa's software device; Windows with `-DTESSERA_ANGLE_DIR=<folder>`: the CPU renderer,
  ANGLE on the real GPU, an `HWND` surface, the desktop-GL refusal), `test_core` (device selector `software`, renderer names), the
  loader behaviour above, `package-provider.py --self-test`. CI's Windows job fetches the ANGLE of a pinned Electron release (hash checked; the runner image's only libEGL, Firefox's, does not load standalone: error 126) and runs both tests on the hosted runner, where ANGLE has no GPU and uses WARP; Linux (llvmpipe) runs the software test in CI.
- Known quirk (observed, not explained): with ANGLE's hardware D3D11 device the process loses buffered stdout when it exits normally
  (the test sets stdout unbuffered); the CPU device does not. The exit status is correct.
- Not done: Metal-backed ANGLE on macOS; a bundled provider in the official release (decision 1); SwiftShader on Windows was not exercised
  (WARP answered first); a window system's windows on the software backend (it refuses X11/Wayland attach by design).
