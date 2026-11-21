# ADR 0017: one way to say "this GPU", and the host's own X display (ROADMAP 12.4 and 12.5)

Status: accepted (chunk 9, ABI 0.14.0: one function, one struct, two fields taken from reserved space, no struct size changed).

## Context

Choosing a GPU had three unrelated spellings (a LUID and a preference on Windows, `TESSERA_EGL_DEVICE` on EGL, nothing on GLX and
CGL) and a context did not say which GPU served it. On Linux, window attach assumed the window lives on `$DISPLAY`.

## Decisions

1. **An inventory.** `tesseraEnumerateAdapters(out, capacity, &count)` lists, backend by backend, what each can use: id, name, PCI ids
   where known, `SOFTWARE` / `INTEGRATED` / `DISCRETE`, the LUID on Windows. Sizing call with capacity 0; `count` is always the total;
   elements are strided by `out[0].structSize`. A backend that cannot list (GLX) lists nothing, which is not an error.
   - WGL: DXGI adapters, id `luid:HHHHHHHH-LLLLLLLL`. Integrated vs discrete is a **heuristic** (dedicated video memory under 1 GiB).
   - EGL: devices from `eglQueryDevicesEXT`, id `uuid:` (`EGL_EXT_device_persistent_id`), else `node:/dev/dri/...`, else `index:N` (not
     stable across boots, the last resort); name from `EGL_EXT_device_query_name` when the driver has it; PCI ids from sysfs.
   - CGL: `CGLQueryRendererInfo`, id `cgl:0x00024600`; software for the Apple CPU renderers and non-accelerated ones.
2. **One selection field**, `TesseraContextConfig::adapter` (`char[48]`, the last of the reserved space). The id goes to whichever backend
   understands its prefix; the others skip the request, so it ends as `UNSUPPORTED` rather than "some other GPU" (the old rule, now for
   every backend). A malformed or unknown id is `NO_MATCHING_CONFIG` where the backend is the right one. It cannot be combined with
   `adapterLuid`/`gpuPreference` (`INVALID_ARGUMENT`), nor with a window (the window system's GPU) or the `software` backend.
   - WGL: `luid:` = `adapterLuid`, with the same verification (Windows decides the driver, so Tessera checks).
   - EGL: one display per device selector (the device platform), never terminated (ADR 0006); the config beats `TESSERA_EGL_DEVICE`.
   - CGL: `kCGLPFARendererID` in the pixel format: that renderer or nothing; an offline (asleep) renderer needs the offline allowance,
     which the loop already tries last (waking a discrete GPU is the price of asking for it by name).
   - GLX: `UNSUPPORTED`; the X server picks.
3. **Report who served the context**: `TesseraContextInfo::adapter` (id, `""` where unknown) and `adapterClass`. WGL identifies the one
   adapter whose vendor owns the OpenGL driver (two adapters of one vendor: unknown, as in the verification); EGL reports the device it
   opened; CGL the renderer of the pixel format. Every backend marks `SOFTWARE` from the renderer string (`isCpuRenderer`).
4. **Hybrid laptops**: when no GPU was requested and the process got the OpenGL driver of a weaker adapter of another vendor while a
   stronger one exists, a warning names the fix once per process (`NvOptimusEnablement`, `AmdPowerXpressRequestHighPerformance`, the
   Windows graphics setting). The decision is a pure function over the adapter list.
5. **Host windows on X11** (12.4): `TesseraNativeWindow::display` may be the host's `Display*`. Tessera reads the server name once
   (`XDisplayString`) and opens **its own** connection to it (GLX and EGL), because a host's Xlib connection is not thread-safe unless
   it called `XInitThreads`; the host's connection is never used or closed. Zero keeps `$DISPLAY`.
6. **EGL initialised by the host** (12.4): before our own `eglInitialize`, `eglQueryString(EGL_VERSION)` answers only if someone already
   initialised the display; this is logged once per display ("shares it and will not terminate it"). Wayland already took the host's
   `wl_display` (ADR 0014); ANGLE window surfaces came with ADR 0015.

## Consequences and limits

- ABI 0.14.0 is additive (`abi_check` passes with the config and info structs the same size). Java: `Tessera.enumerateAdapters()`,
  `ContextConfig.Builder.adapter(String)`, `ContextInfo.adapter()/adapterClass()`, both bindings, fuzzed under `-Xcheck:jni`.
- Verified on the development machine (NVIDIA + WARP: inventory, serving adapter, selection by id, refusals), WSL (EGL: one device,
  `index:0`; GLX: refusal; explicit-display attach on X11 for GLX and EGL), the CGL fake (three renderers, one offline, one software),
  and CI. **Not verified:** a second real GPU anywhere (so the hybrid advisory is only unit-tested, with fake adapter lists), an EGL
  device with a UUID or render node (WSL's has neither), `EGL_EXT_device_query_name`, a real Mac with several renderers, an X server on
  another machine or screen (the explicit display was the same server as `$DISPLAY`), `WGL_NV_gpu_affinity` / `WGL_AMD_gpu_association`
  (still verify-only on Windows, ROADMAP 10.3).
