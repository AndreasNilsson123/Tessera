// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "wgl_api.hpp"

#include <cstdio>

#include "core/log.hpp"
#include <tessera/tessera.h>

namespace tessera::win32 {
namespace {

template <class Fn> bool load(const DynamicLibrary& lib, const char* name, Fn& out) noexcept {
    out = reinterpret_cast<Fn>(lib.symbol(name));
    return out != nullptr;
}

const WglApi* loadApi() noexcept {
    try {
        auto* api = new WglApi(); // leaked on purpose, see header
        api->lib = DynamicLibrary::open("opengl32.dll");
        if (!api->lib) {
            logf(TESSERA_LOG_ERROR, "opengl32.dll could not be loaded");
            delete api;
            return nullptr;
        }
        const bool ok = load(api->lib, "wglCreateContext", api->createContext) &&
                        load(api->lib, "wglDeleteContext", api->deleteContext) &&
                        load(api->lib, "wglMakeCurrent", api->makeCurrent) &&
                        load(api->lib, "wglGetCurrentContext", api->getCurrentContext) &&
                        load(api->lib, "wglGetCurrentDC", api->getCurrentDC) &&
                        load(api->lib, "wglGetProcAddress", api->getProcAddress) &&
                        load(api->lib, "wglShareLists", api->shareLists);
        if (!ok) {
            logf(TESSERA_LOG_ERROR, "opengl32.dll is missing core WGL entry points");
            delete api;
            return nullptr;
        }
        return api;
    } catch (...) {
        return nullptr;
    }
}

} // namespace

const WglApi* wglApi() noexcept {
    static const WglApi* api = loadApi(); // thread-safe one-time init
    return api;
}

WglForeignOps::State WglForeignOps::capture() noexcept {
    State s;
    if (const WglApi* api = wglApi()) {
        s.rc = api->getCurrentContext();
        s.dc = api->getCurrentDC();
    }
    return s;
}

void WglForeignOps::restore(const State& s) noexcept {
    const WglApi* api = wglApi();
    if (!api)
        return;
    if (api->getCurrentContext() == s.rc && api->getCurrentDC() == s.dc)
        return; // already as found
    if (!api->makeCurrent(s.rc ? s.dc : nullptr, s.rc)) {
        // The caller's DC/window may have been destroyed in the meantime; leave the thread clean rather
        // than leaving one of our contexts current by accident.
        logf(TESSERA_LOG_WARN, "could not restore the previous OpenGL context (error 0x%08lX)",
             static_cast<unsigned long>(GetLastError()));
        api->makeCurrent(nullptr, nullptr);
    }
}

std::string describeWin32Error(const char* prefix, DWORD code) {
    char buffer[256] = {0};
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
                                   0, buffer, static_cast<DWORD>(sizeof buffer) - 1, nullptr);
    std::string text = prefix;
    text += ": ";
    if (n > 0) {
        std::string msg(buffer, n);
        while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' '))
            msg.pop_back();
        text += msg;
    } else {
        text += "unknown error";
    }
    char tail[32];
    std::snprintf(tail, sizeof tail, " (0x%08lX)", static_cast<unsigned long>(code));
    text += tail;
    return text;
}

} // namespace tessera::win32
