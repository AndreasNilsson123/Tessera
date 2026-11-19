# Interop, window attach and pooling

## Native handles

```c
TesseraNativeHandles h = tesseraDefaultNativeHandles();
tesseraGetNativeHandles(ctx, &h);   // h.kind says how to read h.handles[]
```

| kind | `handles[0..]` |
|---|---|
| `TESSERA_NATIVE_WGL` | `HGLRC`, `HDC`, `HWND` (0 for pbuffer), `HPBUFFERARB` (0 for window), pixel format |
| `TESSERA_NATIVE_GLX` | `Display*`, `GLXContext`, `GLXDrawable`, `GLXFBConfig`, `Window` (0 for pbuffer), screen |
| `TESSERA_NATIVE_EGL` | `EGLDisplay`, `EGLContext`, `EGLSurface` (0 if surfaceless), `EGLConfig`, client API |
| `TESSERA_NATIVE_CGL` | `CGLContextObj` |

They are borrowed: valid until the context is destroyed; never destroy, close or terminate them (an `EGLDisplay`/`Display*`
is shared by all Tessera contexts of the backend). Extension functions come from `tesseraGetProcAddress`.

**Worked example, Linux (tested):** `tests/test_eglimage.cpp` hands a texture from one Tessera context to another that shares
nothing with it, through an `EGLImage` (`EGL_KHR_gl_texture_2D_image` + `GL_OES_EGL_image`), using only the EGL handles and
`tesseraGetProcAddress`; the second context reads the colour the first rendered, and sees later changes (same memory). Passes on
Mesa D3D12 and llvmpipe. `tesseraGetProcAddress` resolves `egl*` names on the EGL backend too (it used to refuse everything but
`gl*`, which made EGL extension functions unreachable although this page said otherwise; found while writing the test).
dma-buf export/import is not tested: the Mesa drivers available here do not offer `EGL_MESA_image_dma_buf_export` /
`EGL_EXT_image_dma_buf_import`.

**Worked example, Windows (tested):** `tests/test_dxinterop.cpp` shares a Direct3D 11 texture with OpenGL using
`WGL_NV_DX_interop2` (`wglDXOpenDeviceNV`, `wglDXRegisterObjectNV`, lock/unlock), has OpenGL clear it, and reads the colour
back through D3D11. Pick the D3D11 adapter that is the same GPU as the context (the test matches by vendor id).

## Rendering into your own window

```c
TesseraContext ctx;
tesseraCreateContextForWindow(&cfg, (uint64_t)(uintptr_t)hwnd, 0, &ctx);   // WGL: HWND, GLX: Window
```

- The window stays yours: never destroyed, resized, shown or hidden by Tessera. It must outlive the context's use.
- If it already has a pixel format (Windows) it is adopted and, with `TESSERA_FALLBACK_STRICT`, checked against the request.
- If it has none, the call **fails** unless you pass `TESSERA_ATTACH_ALLOW_PIXEL_FORMAT` (a window's format can be set only
  once, ever, so only you know whether it is fresh).
- Present with `tesseraSwapBuffers`; `info.surface == TESSERA_SURFACE_WINDOW`.
- The integer form takes a Win32 `HWND` (WGL) or an X11 `Window` (GLX). For anything else describe the window (ADR 0014):

```c
TesseraNativeWindow w = tesseraDefaultNativeWindow();
w.kind = TESSERA_WINDOW_WAYLAND;                 // or TESSERA_WINDOW_X11 / TESSERA_WINDOW_WIN32
w.window = (uint64_t)(uintptr_t)wlSurface;       // wl_surface*
w.display = (uint64_t)(uintptr_t)wlDisplay;      // wl_display* the surface belongs to
w.width = 640; w.height = 480;                   // size of the surface's buffer
tesseraCreateContextForNativeWindow(&cfg, &w, 0, &ctx);   // through EGL: the wl_surface is wrapped in a wl_egl_window we own
tesseraMakeCurrent(ctx);
tesseraSetSwapInterval(ctx, 0);                  // a surface the compositor has not mapped never gets a frame callback
...
tesseraResizeWindow(ctx, newWidth, newHeight);   // when the window changed size (current on this thread)
```

  Wayland needs EGL (no GLX there); an X11 window goes to GLX first and to EGL (Tessera's own connection to `$DISPLAY`) when GLX
  is unavailable or `TESSERA_BACKEND=egl`. You keep dispatching your own Wayland queue. `TESSERA_WINDOW_COCOA` (an `NSView`) is
  `UNSUPPORTED`: the design is in ADR 0014. CGL has no window attach.

## Pooling

```c
tesseraPrewarmContexts(&cfg, 4);      // blocks; call from a background thread
tesseraCreateContext(&cfg, &ctx);     // identical request, no shareWith: takes a pooled one (~0.01 ms instead of ~28 ms)
```

Only never-used contexts are pooled, so nothing leaks between users. Max 64; emptied by `tesseraTerminate`.

Tessera starts no threads of its own, so keeping the pool full is a call YOUR thread makes:

```c
tesseraPoolTopUp(&cfg, 2, &created);      // bring the pool for exactly this request up to 2; creates only what is missing
tesseraGetPooledCount(&cfg, &waiting);    // contexts waiting for exactly this request
tesseraPoolClear();                       // destroy all waiting contexts (returns how many)
```

Typical use: after `tesseraCreateContext` took a pooled one, call `tesseraPoolTopUp` from a background thread. Java:
`Tessera.topUpPool(config, 2)`, `Tessera.pooledContextCount(config)`, `Tessera.clearPool()`.

## Choosing a GPU (EGL)

`TESSERA_EGL_DEVICE=<index | /dev/dri/renderD128 | 0000:01:00.0>` (see `environment.md`). A choice that matches nothing is an
error, never a different GPU.

## Choosing a GPU

Windows: `cfg.gpuPreference = TESSERA_GPU_HIGH_PERFORMANCE` (or `_LOW_POWER`), or `cfg.adapterLuid = <DXGI LUID>`.

A process cannot make Windows load a different GPU's OpenGL driver (that is per application in Windows' graphics settings, or the host
EXE exports `NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance`), so this is a **check, not a switch**: Tessera resolves
the adapter through DXGI and verifies that the OpenGL driver it got belongs to it by PCI vendor. A mismatch fails with
`NO_MATCHING_CONFIG` and a message naming both; two adapters of the same vendor cannot be told apart and are reported as
unverifiable. An explicit LUID is always a hard requirement; a preference is a failure in strict mode and a logged warning in
best-effort mode. Other backends return `UNSUPPORTED` for a non-default value (EGL: `TESSERA_EGL_DEVICE`). Java:
`ContextConfig.builder().gpuPreference(...)` / `.adapterLuid(...)`. It does not use `WGL_NV_gpu_affinity` or
`WGL_AMD_gpu_association` (workstation cards only; untestable here).

## No drawable at all (`TESSERA_SURFACE_NONE`)

EGL and GLX make the context current without any drawable (GLX falls back to a hidden window on a driver that refuses). WGL cannot:
`wglMakeCurrent` needs a DC, so WGL keeps an internal 1x1 window and still reports `SURFACE_NONE` (nothing is exposed to render to).

## Not provided

dma-buf export, IOSurface, Vulkan memory objects/semaphores (use the handles above), Wayland/`NSView` attach, UUID device
selection.
