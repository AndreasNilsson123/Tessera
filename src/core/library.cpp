// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "library.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace tessera {

#if defined(_WIN32)

DynamicLibrary DynamicLibrary::open(const char* name) noexcept {
    DynamicLibrary lib;
    if (!name)
        return lib;
    wchar_t wide[260];
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, 260);
    if (n <= 0)
        return lib;
    lib.handle_ = reinterpret_cast<void*>(LoadLibraryExW(wide, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    return lib;
}

DynamicLibrary DynamicLibrary::openPath(const char* path) noexcept {
    DynamicLibrary lib;
    if (!path || !isAbsolutePath(path))
        return lib;
    wchar_t wide[1024];
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1024);
    if (n <= 0)
        return lib;
    // DLL_LOAD_DIR is only honoured with an absolute path (checked above); SYSTEM32 keeps the usual system
    // DLLs reachable.
    lib.handle_ = reinterpret_cast<void*>(
        LoadLibraryExW(wide, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32));
    return lib;
}

bool isAbsolutePath(const char* path) noexcept {
    if (!path)
        return false;
    const unsigned char c0 = static_cast<unsigned char>(path[0]);
    const bool drive = ((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')) && path[1] == ':' &&
                       (path[2] == '\\' || path[2] == '/');
    const bool unc = path[0] == '\\' && path[1] == '\\';
    return drive || unc;
}

void* DynamicLibrary::symbol(const char* name) const noexcept {
    if (!handle_ || !name)
        return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle_), name));
}

void DynamicLibrary::close() noexcept {
    if (handle_)
        FreeLibrary(reinterpret_cast<HMODULE>(handle_));
    handle_ = nullptr;
}

#else

DynamicLibrary DynamicLibrary::open(const char* name) noexcept {
    DynamicLibrary lib;
    if (!name)
        return lib;
    lib.handle_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    return lib;
}

DynamicLibrary DynamicLibrary::openPath(const char* path) noexcept {
    return open(path);
}

bool isAbsolutePath(const char* path) noexcept {
    return path && path[0] == '/';
}

void* DynamicLibrary::symbol(const char* name) const noexcept {
    if (!handle_ || !name)
        return nullptr;
    return dlsym(handle_, name);
}

void DynamicLibrary::close() noexcept {
    if (handle_)
        dlclose(handle_);
    handle_ = nullptr;
}

#endif

} // namespace tessera
