# ABI policy

The public interface is the C header `include/tessera/tessera.h`. Java (Panama/JNI) and any other binding are generated from or written against it.

- **Versioning:** `TESSERA_VERSION_*` is semantic. `TESSERA_ABI_VERSION` increments on every incompatible change. Before 1.0 the ABI may change between minor versions; from 1.0 it is frozen within a major version.
- **Structs:** the first field is `uint32_t structSize`, set by the caller. The library reads only `structSize` bytes, rejects values smaller than the v1 minimum, and treats missing newer fields as zero/default. Fields are only appended; `reserved` fields must be zero.
- **Enums:** fixed-width integer typedefs + `#define` constants; values are append-only and never reused.
- **Handles:** opaque `uint64_t`, 0 invalid, generation-checked (stale handles return `TESSERA_ERROR_INVALID_HANDLE`).
- **Errors:** every function returns `TesseraResult` (or has a documented no-fail contract). Details via thread-local `tesseraGetLastErrorMessage`.
- **Forbidden across the boundary:** exceptions, `abort`/`exit`, stdout/stderr output, ownership transfer of CRT-allocated memory (the library allocates and frees its own).
- **Symbols:** hidden by default; only `TESSERA_API` functions are exported. `tests/test_abi.cpp` pins struct sizes.
- **Enforcement:** `scripts/abi-check.py` (ctest `abi_check`) compares the header with `docs/abi/baseline.json`: a removed or changed function, field or `#define`, a shrunk struct, or an export the header does not declare fails the build; additions pass and are recorded with `--update`. The procedure for 1.0 and for every release is `freeze.md`.
