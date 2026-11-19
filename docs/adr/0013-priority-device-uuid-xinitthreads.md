# ADR 0013: Context priority, EGL device selection by UUID, XInitThreads detection (ABI 0.11.0)
Status: accepted.

## Context priority
`TesseraContextConfig::priority` (`TESSERA_PRIORITY_{DEFAULT,LOW,MEDIUM,HIGH}`, taken from the reserved space at offset 84) and `TesseraContextInfo::priority` (offset 440, what the driver reports it granted).
- **GLX**: `GLX_EXT_context_priority`, attribute `GLX_CONTEXT_PRIORITY_LEVEL_EXT` in the `glXCreateContextAttribsARB` list; read back with `glXQueryContext`. **EGL**: `EGL_IMG_context_priority`, same attribute and values; read back with `eglQueryContext`. The two extensions share their enum values, so the encoding is one header (`platform/common/context_priority.hpp`).
- **WGL has no such extension** (none in the Khronos WGL list as far as we know, and none of the strings are in NVIDIA's `nvoglv64.dll`). A WGL request is refused in strict mode and dropped with a log line in best-effort mode; the context reports `DEFAULT`.
- **Strict means exactly the level read back.** Drivers grant a lower level silently when the process lacks the privilege for HIGH, so `contextSatisfies` compares the read-back with the request; best-effort keeps the context and reports what it got. A request is never reported as granted because it was requested.
- The legacy create path cannot carry attributes, so a priority request forces the attribute path; the pool keys on priority.

## EGL device selection by UUID
`TESSERA_EGL_DEVICE` accepts `<uuid>` / `uuid:<uuid>` (dashed or 32 hex digits) next to the index, render node and PCI address. A UUID (`EGL_EXT_device_persistent_id`, `EGL_DEVICE_UUID_EXT`) survives reboots and re-plugging, unlike an index. The matching rules are one pure function (`platform/common/device_select.hpp`) over a list of device descriptions, tested against faked multi-GPU lists: index range and overflow, render node directly and through `/dev/dri/by-path` (PCI), any-case UUIDs, devices that report only some attributes, ambiguity (a driver bug: refused, not guessed), and refusals that name the missing extension. A 32-digit all-decimal UUID is classified as a UUID before an index.
Not verified: a machine with two GPUs. On the development machine the single EGL device's real render node and UUID are looked up through raw libEGL and used as selectors (see `test_egl`).

## XInitThreads
Tessera still calls `XInitThreads` before its first `XOpenDisplay` (ADR 0005), but now reads libX11's exported `_Xglobal_lock` first (`XInitThreads` sets it and returns early when set), so it knows whether the host already did. It logs one INFO line saying which case it was and which side acted, and skips the call when the host did it. A host that has *not* initialised Xlib threading and already has displays open is the only case where Tessera's late call is a hazard; the log line is how that is diagnosed, and `TESSERA_NO_XINITTHREADS=1` still opts out. If libX11 does not export the symbol the state is reported as unknown and the call is made as before. Tested in two fresh processes (`glx_xinitthreads_host`, `glx_xinitthreads_none`) with libX11's own lock as the independent witness.
