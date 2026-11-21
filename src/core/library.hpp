// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <cstdint>
#include <utility>

namespace tessera {

/*
 * Move-only handle to a dynamically loaded library (GL, EGL, GLX, ...).
 *
 *  - Windows: loaded from System32 only (LOAD_LIBRARY_SEARCH_SYSTEM32), never the application
 *    directory or PATH, so a planted opengl32.dll cannot be picked up.
 *  - POSIX: dlopen(RTLD_NOW | RTLD_LOCAL); symbols never leak into the global namespace.
 *  - We hold exactly one OS reference and drop exactly that one on destruction, so a library already
 *    loaded by someone else (JavaFX, LWJGL) stays loaded.
 */
class DynamicLibrary {
  public:
    DynamicLibrary() noexcept = default;
    ~DynamicLibrary() {
        close();
    }

    DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    /* `name` is UTF-8, e.g. "opengl32.dll" or "libEGL.so.1". Returns an empty (false) library on failure. */
    static DynamicLibrary open(const char* name) noexcept;

    /* A library the user pointed us to by full path (an optional bundled ANGLE or Mesa, ADR 0015). Windows:
     * the path must be absolute, and the library's own directory is searched for ITS dependencies
     * (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) then System32, never the current directory or PATH; so libEGL.dll
     * finds the libGLESv2.dll next to it and nothing planted elsewhere. POSIX: dlopen(path). A bare name is
     * refused on Windows (it would search System32 only, which is what open() is for). */
    static DynamicLibrary openPath(const char* path) noexcept;

    /* The operating system's error code of the last failed openPath on this thread (Windows: GetLastError,
     * e.g. 126 = the file or one of ITS dependencies was not found, 193 = wrong architecture); 0 elsewhere.
     * For log lines. */
    static unsigned long lastOpenError() noexcept;

    /* Null if not found or the library is empty. */
    void* symbol(const char* name) const noexcept;

    explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }
    void close() noexcept;

  private:
    void* handle_ = nullptr;
};

/* True for "C:\x\y.dll", "\\server\share\y.dll" and "/usr/lib/y.so": a path that does not depend on a search
 * order. */
bool isAbsolutePath(const char* path) noexcept;

/*
 * wglGetProcAddress (and some other loaders) return 0, 1, 2, 3 or -1 instead of NULL for functions
 * they do not provide. Those values must be treated as "not found".
 */
inline bool isPlausibleProcAddress(const void* p) noexcept {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    return v > 3 && v != static_cast<std::uintptr_t>(-1);
}

} // namespace tessera
