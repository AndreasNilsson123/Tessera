# ADR 0016: version negotiation (a minimum is a minimum), the memory of what worked, one table of refusals

Status: accepted (chunk 8, ABI 0.13.0: one new flag, no struct change). Implements the recommendation of `docs/decisions-1.0.md` item 2;
the owner's decision record is still to be filled in.

## Context

The header said `versionMajor/versionMinor` is a *minimum*, but an explicit core request was created at exactly that version (a request
for "3.3 core" on a driver with 4.6 gave 3.3), unspecified OpenGL ES always asked for 2.0, every failed rung was a real driver call
repeated on every create, and each backend kept its own idea of what a driver's refusal means. Changing the first after 1.0 would be a
behaviour change of an existing field, which is why it is done before the freeze.

## Decisions

1. **The field is a floor.** `versionLadder` (`src/platform/common/version_negotiation.hpp`) returns the versions to try, newest first, down
   to the requested one: desktop GL 4.6 .. 3.2 (core and compatibility profiles start at 3.2; a lower floor, such as 2.1, is the last rung),
   OpenGL ES 3.2, 3.1, 3.0, 2.0. No version is the old behaviour (a profile walks the whole ladder, no profile leaves it to the driver);
   unspecified ES now walks 3.2 down instead of asking for 2.0. Verified on the development machine: "core, at least 3.3" gives 4.6 on
   NVIDIA/WGL, 4.2 on Mesa D3D12, 4.5 on llvmpipe; ES with no version gives 3.2 (NVIDIA, llvmpipe) and 3.1 (Mesa D3D12).
2. **`TESSERA_FLAG_EXACT_VERSION` (0x0400) asks the driver for exactly that version**: one request, no ladder (the pre-0.13.0 behaviour).
   It cannot promise more than the driver does: Mesa answers a core request for 3.3 with its newest compatible core context (4.2, 4.5),
   which the spec allows; NVIDIA/WGL gives exactly 3.3. `TesseraContextInfo` says what was granted, and the minimum is never relaxed,
   with or without the flag (`hardRequirementViolation`, unchanged). The flag takes part in the pool's request identity like every flag.
3. **What worked is remembered, never trusted** (`VersionMemory`). Per (backend, display/adapter scope, API, profile, floor, context flags,
   priority, adapter request) the version that last succeeded is tried first; if it fails, the ladder runs in full, as if nothing were
   remembered (a different GPU behind the same name, a driver update, a lost display). Single-rung ladders remember nothing. The memory is
   dropped by the last `tesseraTerminate`. Unit-tested with a counting driver: first create on a 4.2 driver makes 5 calls, the second makes 1,
   a driver that changed makes 8 and finds the new answer; every key component changes the key (sRGB and double buffering do not).
4. **One table of what a refusal means** (`create_errors.hpp`): WGL (the masked Win32 codes, `ERROR_INVALID_DATA`, ...), GLX (X errors),
   EGL, CGL, each row tested. A small behaviour change came with it: EGL's `NOT_INITIALIZED`, `BAD_DISPLAY` and `CONTEXT_LOST` are now
   `PLATFORM` (they say nothing about the request) instead of `NO_MATCHING_CONFIG`.

## Consequences

- A request for "core 3.3" now yields the newest core context: more driver calls the first time (a driver whose newest core is 4.2 fails
  4.6 .. 4.3 once per process and request shape, then one call), and shaders written for `#version 330` keep working on a newer core
  context (core profiles are backwards compatible); an application that needs the old behaviour sets `EXACT_VERSION`.
- Java: `ContextConfig.Flags.EXACT_VERSION`; the `version(...)` Javadoc says "minimum".
- Not done from 12.2: nothing; not verified: macOS (CGL chooses a version through the pixel format, there is no ladder; the macOS CI runs
  `test_versions` and `exact >= requested` is all it asserts), a driver that crashes on an unsupported version request (none seen).
