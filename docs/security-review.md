# Security review (input validation)

A self-review by the author of the code, **not** an independent audit. Scope: everything that crosses the library boundary or
reads from outside the process's own data. Method: read every public entry point and every use of `memcpy`/`getenv`/format
functions/numeric parsing (`grep`), then fuzz the result (`tests/test_fuzz.cpp`, under ASan/UBSan/TSan).

## Boundary rules (and where they hold)

| Input | Rule | Status |
|---|---|---|
| Handles | 64-bit, type-tagged, generation-checked; stale/made-up handles return `INVALID_HANDLE`, never dereferenced blindly | fuzzed (random, bit-flipped, stale) |
| Caller structs (`structSize` first) | Only `structSize` bytes are read; smaller-than-minimum rejected; larger accepted, extra ignored; output structs are written only up to the caller's claimed size | fuzzed with exact-size heap buffers; **one bug found and fixed**: `tesseraCreateContextForWindow` copied `sizeof` our struct from the caller's (possibly smaller) one |
| Output pointers | NULL checked; outputs are zeroed/nulled before any work, so failure never leaves garbage | fuzzed |
| Strings from callers | `tesseraGetProcAddress` takes an arbitrary C string; only `gl*` names resolve on GLX/EGL (GLVND would otherwise return a stub for anything); WGL validates the returned pointer | fuzzed (incl. 5000-char names, control and high bytes) |
| Strings from drivers | `GL_VERSION`/vendor/renderer copied into fixed buffers with truncation; version parser limits each component to 4 digits (an earlier signed-overflow UB found by UBSan) | fuzzed (2x iterations) |
| Environment variables | `TESSERA_BACKEND`, `_EGL_PLATFORM`, `_EGL_DEVICE`, `_EGL_LIBRARY`, `_NO_XINITTHREADS`, `DISPLAY`. Numeric selector limited to 4 digits (found while reviewing: `atoi` on a huge digit string is signed-overflow UB) | reviewed |
| Allocation from untrusted sizes | None: struct sizes are never used to allocate; counts are capped (prewarm 64; context table 2^20; list of EGL devices 16) | reviewed |
| Error / log text | Fixed-size thread-local buffer and bounded `vsnprintf`; no allocation, cannot throw | reviewed |
| Exceptions | Nothing propagates across the C boundary (`guarded()`); a thrown exception becomes a result code | `core` tests inject `bad_alloc`/`runtime_error` |

## Trust decisions to know about

- **`TESSERA_EGL_LIBRARY` loads the library at that path.** Environment variables are controlled by whoever starts the
  process, so this is not a privilege boundary, but setuid/elevated hosts should scrub it. The Windows loader is restricted to
  System32 for the default libraries.
- **Java:** the embedded native library is extracted to a fresh private directory (0700 on POSIX), checksum-verified, and loaded
  by absolute path; `-Dtessera.library.path` is trusted like any system property.
- **Native handles** (`tesseraGetNativeHandles`) expose driver objects; misuse by the caller (destroying them) is the caller's
  bug, documented as such.
- **Window attach** takes a caller's native window id on trust that it belongs to this process/server; it validates that the
  id is a window (`IsWindow` / `XGetWindowAttributes` under a scoped error handler) but cannot know who owns it.
- Driver code runs in-process; a buggy driver can crash us. TSan reports inside uninstrumented vendor drivers are known and
  documented in the roadmap.

## Not done

- No independent review, no coverage-guided fuzzing (the fuzz test is seeded random with structure-aware inputs; libFuzzer
  targets would find deeper cases), no fuzzing of the Java layer or the JNI glue.
- Extension-string parsing is trivial word splitting inside the EGL backend and is exercised only through real drivers.
