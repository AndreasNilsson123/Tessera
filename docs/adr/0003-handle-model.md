# ADR 0003: Opaque 64-bit generation-checked handles
Status: accepted (Phase 0)

Handles are `uint64_t` = `tag(8) << 56 | generation(24) << 32 | index(32)` into a library-owned, per-type table. The type tag stops a handle of one kind being accepted as another; generation 0 is never used, so 0 is never valid. Rationale: stale/double-destroyed handles from Java (GC, classloaders) are detected and reported instead of becoming use-after-free; 0 is never valid; handles are trivially marshalled by Panama/JNI. Implemented in Phase 1 (`src/core/handle_table.hpp`). `get()` returns a shared_ptr so an object in use survives a concurrent destroy; objects are released outside the table lock.
