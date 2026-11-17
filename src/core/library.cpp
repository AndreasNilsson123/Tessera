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
    if (!name) return lib;
    wchar_t wide[260];
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, 260);
    if (n <= 0) return lib;
    lib.handle_ = reinterpret_cast<void*>(LoadLibraryExW(wide, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    return lib;
}

void* DynamicLibrary::symbol(const char* name) const noexcept {
    if (!handle_ || !name) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle_), name));
}

void DynamicLibrary::close() noexcept {
    if (handle_) FreeLibrary(reinterpret_cast<HMODULE>(handle_));
    handle_ = nullptr;
}

#else

DynamicLibrary DynamicLibrary::open(const char* name) noexcept {
    DynamicLibrary lib;
    if (!name) return lib;
    lib.handle_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    return lib;
}

void* DynamicLibrary::symbol(const char* name) const noexcept {
    if (!handle_ || !name) return nullptr;
    return dlsym(handle_, name);
}

void DynamicLibrary::close() noexcept {
    if (handle_) dlclose(handle_);
    handle_ = nullptr;
}

#endif

} // namespace tessera
