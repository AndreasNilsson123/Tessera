#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

#include "core/foreign_state.hpp"
#include "core/library.hpp"

namespace tessera::win32 {

/*
 * The core WGL entry points, loaded dynamically from System32\opengl32.dll so Tessera has no static GL
 * dependency and can report a clean error on machines without a GL driver.
 *
 * The instance is created on first use and intentionally never freed: unloading a GL ICD in a process
 * that other components (JavaFX, AWT, LWJGL) may also be using is far more dangerous than keeping one
 * module reference alive. LoadLibrary is reference counted, so this never affects anyone else.
 */
struct WglApi {
    DynamicLibrary lib;
    HGLRC(WINAPI* createContext)(HDC) = nullptr;
    BOOL(WINAPI* deleteContext)(HGLRC) = nullptr;
    BOOL(WINAPI* makeCurrent)(HDC, HGLRC) = nullptr;
    HGLRC(WINAPI* getCurrentContext)() = nullptr;
    HDC(WINAPI* getCurrentDC)() = nullptr;
    PROC(WINAPI* getProcAddress)(LPCSTR) = nullptr;
    BOOL(WINAPI* shareLists)(HGLRC, HGLRC) = nullptr;
};

/* Null if opengl32.dll or any core entry point is missing. */
const WglApi* wglApi() noexcept;

/* Snapshot policy for ScopedForeignState: the thread's current WGL context and its DC. */
struct WglForeignOps {
    struct State {
        HGLRC rc = nullptr;
        HDC dc = nullptr;
    };
    static State capture() noexcept;
    static void restore(const State& state) noexcept;
};

using ForeignContextGuard = ScopedForeignState<WglForeignOps>;

/* "<prefix>: <system message> (0x<code>)" for a Win32/WGL error code. */
std::string describeWin32Error(const char* prefix, DWORD code);

} // namespace tessera::win32
