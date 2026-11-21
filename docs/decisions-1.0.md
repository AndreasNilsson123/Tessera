# Decisions for 1.0 (ROADMAP 10.5)

One page for the owner. Each item has the question, what the repository says today, a recommendation, and what it costs to be
wrong. **Nothing here is decided until the owner ticks it in ROADMAP 10.5**; the recommendations are mine. Append-only (`abi-policy.md`)
is what makes most of them cheap: a 1.0 that ships *less* can add in 1.x, a 1.0 that ships something wrong cannot take it back.

## 1. Is on-screen rendering (caller-supplied windows) part of 1.0?

**Today.** `tesseraCreateContextForWindow` (HWND, X11) and `tesseraCreateContextForNativeWindow` (HWND, X11, Wayland), `tesseraResizeWindow`
and the swap-control report exist (ABI 0.12.0, ADRs 0012 and 0014). Tested: WGL on NVIDIA and on the hosted software renderer, GLX and
EGL on X11, EGL on a real Wayland surface (WSLg), GLFW windows from Java on X11 and Wayland. **Not tested:** a mapped Wayland surface that
receives frame callbacks, AWT/Swing (no supported window id), macOS `NSView` (design only, `TESSERA_WINDOW_COCOA` is reserved and returns
UNSUPPORTED), exclusive fullscreen, variable refresh.

**Recommendation: yes, with a scope statement.** 1.0 promises the Win32, X11 and Wayland forms as documented, and says in the header that
Cocoa returns UNSUPPORTED until a macOS implementation exists (it is a slot, so adding it is an addition). Without it Tessera cannot serve
anyone who draws into a window they own, which is the first thing people ask of a context library. Condition: run the Wayland case on a
mapped surface (a second compositor) before the tag, or mark it in the docs as unverified.

**If wrong:** the surface is only additive; a bug is fixed in 1.x. The risk is a documented behaviour (resize, ownership of a
`wl_egl_window`) that turns out to be wrong on a real desktop; the rules are in ADR 0014 and can be tightened but not reversed.

## 2. Minimum GL/GLES floor, and what "minimum version" means

**Today.** The header says `versionMajor/Minor` is a *minimum*, but an explicit core request is created at exactly that version
(ROADMAP 12.2). Tested floors: desktop GL 2.1 compatibility to 4.6 core on NVIDIA; GL 4.2 core / ES 3.1 on Mesa D3D12; 4.1 core,
forward-compatible only, on Apple (2.1 legacy); GL 1.1 on the Windows software renderer (only best-effort requests succeed there, which is what the CI test checks). GLES 2.0 requests succeed on NVIDIA and Mesa.

**Implemented (ABI 0.13.0, ADR 0016) as recommended below, pending your decision.** **Recommendation: no enforced floor; a documented support floor, and "minimum" must mean what it says.**
- Library behaviour: it creates what the driver gives and reports it (`info`), refusing only what cannot satisfy the request. It does not
  hard-code a version below which it refuses.
- Support floor (what the tests cover and what a bug report is accepted against): **OpenGL 3.2 core, OpenGL 2.1 compatibility, OpenGL ES
  2.0**. Anything lower works if the driver allows it and is not tested.
- Semantics: keep the field a floor (the header's words), make the default try the newest version at or above it, and add the
  exact-version request as a flag (`TESSERA_FLAG_EXACT_VERSION`, bit taken from the free flags: an addition). Pin it with a test (12.2).
  This **must be done before the freeze**, because switching the default afterwards is a behaviour change of an existing field.

**If wrong:** the floor is documentation (changeable any day); the semantics are the part that cannot change after 1.0.

## 3. Ship ANGLE/Mesa, or only detect system ones?

**Today.** The EGL backend loads the system's `libEGL`; Windows loads only from System32 by design; there is no ANGLE or Mesa binary in
the repository or the release, and no Windows EGL build (ROADMAP 10.3, chunk 7).

**Recommendation: system only for 1.0; bundling is an optional, separate artifact later (1.x).** Shipping a GL implementation means
owning its updates, security fixes and licenses (ANGLE is BSD-3, Mesa is MIT, but the build matrix is large and the CVE exposure is
yours); the SBOM and the loader rules (`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`) are real work that chunk 7 plans. Nothing in the 1.0 ABI
needs it: the backend selection (`TESSERA_BACKEND`) and the EGL path already take whatever the loader finds. A user who wants ANGLE
today can put it first on the loader path; document that and test it once on Windows.

**If wrong:** adding a bundled-binaries package later changes no ABI. Shipping one now and abandoning it would.

## 4. Names, licence, cadence

**Today.** Maven `io.github.andreasnilsson123:tessera` (Central namespace not yet verified), Java package
`io.github.andreasnilsson123.tessera`, C prefix `tessera*`/`Tessera*`, licence Apache-2.0, "Tessera contributors" (`releasing.md`).

**Recommendation.**
- Keep the C names and the licence: they are in every header and binding and cost the most to change; Apache-2.0 includes a patent grant,
  which suits a library next to GPU drivers. Do a trademark/name search on "Tessera" before the tag (the name is common in software); the
  Java group id is the only name that is project-specific, and it is cheap to move before the first Central upload and impossible after.
- Cadence: **1.0.0 when the checklist in `freeze.md` is green, then** patch releases as needed, a minor about every quarter (additions
  only), a major only for a real break (new `TESSERA_ABI_VERSION` and soname). A release is a tag; CI does the rest (`release.yml`).
- Support: the last minor of the current major gets fixes; there is no long-term branch until there is a user who needs one.

## 5. Freeze ABI v1

**Today.** The ABI has been append-only since 0.3 (checked mechanically against the git history, `freeze.md`). It changed shape last in
0.12.0 (additions). What is still missing from the freeze gate in 10.5: another GPU vendor on Windows, macOS on a real machine, the
12.2 semantics above, and a real GPU reset test (10.1).

**Recommendation: freeze after items 1 to 4 are decided, the 12.2 semantics are implemented, and either an AMD/Intel run and a real
Mac run exist or the owner marks them `ACCEPTED-FOR-1.0`.** The machinery is ready (`freeze.md`: `release-check.py --freeze`). The reason
to wait for hardware is not the C layout (it is small and append-only) but the *behaviour* contract, which only more drivers can test.

## Decision record (to fill in)

| # | Decision | Owner's answer | Date |
|---|---|---|---|
| 1 | On-screen rendering in 1.0 (scope as above) | | |
| 2 | Floors and the meaning of "minimum version" | | |
| 3 | ANGLE/Mesa: system only for 1.0 | | |
| 4 | Names, licence, cadence | | |
| 5 | Freeze gate (hardware accepted or required) | | |
