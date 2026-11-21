# ADR 0018: loss without polling, loss causes, recreate policies, and sharing explained (ROADMAP 12.6 and 12.7)

Status: accepted (chunk 10, ABI 0.15.0: four functions, one struct, one result code, one flag; no struct size changed).

## Loss, beyond polling (12.6)

1. **Opportunistic polling.** A context created with `ROBUST_ACCESS | RESET_NOTIFICATION` is asked for its reset status inside
   `tesseraMakeCurrent` (after the bind) and `tesseraSwapBuffers` (after a successful swap): one `glGetGraphicsResetStatus` on the
   right thread with the context current. A reset found there marks the context lost, fires the lost callback and returns
   `CONTEXT_LOST`; a failed make-current leaves nothing current. `TESSERA_FLAG_NO_RESET_POLL` turns it off (explicit
   `tesseraGetResetStatus` keeps working). No threads were added (principle 4). Contexts without reset notification are never queried.
2. **Loss causes.** `tesseraGetLossCause`: `GPU_RESET` (a reset status from the robustness query: re-upload), `DEVICE` (the backend said
   "context lost" natively: display, driver or power event; recreate, maybe elsewhere), `WINDOW_GONE` (the window was destroyed: do not
   recreate onto it). `TESSERA_ERROR_WINDOW_GONE` is the swap result in that case and also marks the context lost. Detected where the
   platform says so: WGL (`IsWindow`, `ERROR_INVALID_WINDOW_HANDLE`), GLX (`BadWindow`/`BadDrawable`/`GLXBadDrawable`: **verified**, X
   error 160 on Mesa), EGL (`BAD_NATIVE_WINDOW`/`BAD_SURFACE`; Mesa's X11 platform lets a swap to a destroyed window pass silently, so
   the EGL case is implemented but **not observed**). The first cause wins.
3. **Recreate policies.** `tesseraRecreateContextWithPolicy`: `SAME_ADAPTER` (what `tesseraRecreateContext` always did), `ANY_HARDWARE`
   (drop the adapter request, refuse a CPU result), `ALLOW_SOFTWARE` (drop it and fall back to the software backend of ADR 0015 when no
   GPU can serve). The outcome flags say `ADAPTER_CHANGED` and `SOFTWARE`; when the device changed and cannot share with the survivors,
   the new context starts an empty group rather than failing (`SHARE_PRESERVED` unset). The plain `tesseraRecreateContext` still
   reports only `SHARE_PRESERVED`.
4. **Not done:** the real reset (TDR) test: it can blank the screen and needs the owner's explicit go-ahead; the loss paths are tested on
   fake backends, through the real JNI/FFM upcalls (test hook), and `GPU_RESET` follows the same code. Claims stay "fake backend only".

## Sharing, explained (12.7)

1. **Preflight.** `tesseraCanShare(config, parent, &reason)` answers before a create, touching no driver: `PARENT_LOST`, `API_MISMATCH`
   (EGL refuses GL/ES sharing; WGL and GLX drivers accepted it, so they do not predict it), `PROFILE_MISMATCH` (CGL core vs legacy),
   `DIFFERENT_BACKEND` (the request names a GPU the parent's backend cannot serve), `DIFFERENT_ADAPTER`, `DIFFERENT_DISPLAY` (GLX window
   of another server, another EGL device), `FORMAT_MISMATCH`. A "yes" is best knowledge; a "no" is a create that really fails (tested
   against the real create on every backend). To make "which backend serves a share request" decidable, every backend now answers
   `UNSUPPORTED` (not `INVALID_ARGUMENT`) for a parent of another backend, so the creation falls through to the parent's own backend.
2. **Introspection.** `tesseraGetShareGroup`: group id, members, alive. No bookkeeping on the host's side to know what survives a loss.
3. **Which format differences break sharing: measured.** One table per real driver (printed by the tests, recorded in `compat.md`).
   The only break found is **NVIDIA/WGL with a different depth-buffer size** (`wglShareLists` and `wglCreateContextAttribsARB` both fail
   with a 16 or 32-bit depth against the parent's 24); stencil, multisampling, sRGB, float, alpha, the debug flag, a core profile and GL
   against ES shared. Hence the rule implemented for WGL: **when a share parent is given and its pixel format already serves the request**
   (`formatServesRequest`: no fewer bits, samples, the same buffering and colour flags), the child uses the parent's format; a request that
   needs a deeper buffer than the parent's is `NO_MATCHING_CONFIG` with the reason, and `tesseraCanShare` says `FORMAT_MISMATCH` first.
   Mesa (GLX and EGL, D3D12 and llvmpipe) shared across every difference tried, so nothing is done there.

## Consequences

Java: `Context.lossCause()`, `shareGroup()`, `recreate(RecreatePolicy)`, `Tessera.canShare`, `Flags.NO_RESET_POLL`, `Result.WINDOW_GONE`,
both bindings, fuzzed. The native fuzzers cover all four functions.
