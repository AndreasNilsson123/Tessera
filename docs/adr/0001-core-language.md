# ADR 0001: C++17 core behind a pure C99 public header
Status: accepted (Phase 0)

The core is C++17 (RAII for the foreign-context guard and handle tables); the public header is pure C99 with `extern "C"`, so it is consumable from JNI, Panama/jextract and other languages. No C++ types, exceptions or STL cross the boundary; every entry point must be `noexcept`-safe (catch-all at the boundary, mapped to `TESSERA_ERROR_UNKNOWN`).
