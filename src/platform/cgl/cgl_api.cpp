#include "cgl_api.hpp"

#include <tessera/tessera.h>

#include "core/env.hpp"
#include "core/log.hpp"

namespace tessera::cgl {
namespace {

template <class Fn> bool load(const DynamicLibrary& lib, const char* name, Fn& out) noexcept {
    out = reinterpret_cast<Fn>(lib.symbol(name));
    return out != nullptr;
}

const CglApi* loadApi() noexcept {
    try {
        auto* api = new CglApi(); // leaked on purpose: other components in the process use the same framework
        std::string path;
        if (!readEnv("TESSERA_CGL_LIBRARY", path)) path = "/System/Library/Frameworks/OpenGL.framework/OpenGL";
        api->lib = DynamicLibrary::open(path.c_str());
        if (!api->lib) {
            logf(TESSERA_LOG_INFO, "CGL backend unavailable: cannot load %s", path.c_str());
            delete api;
            return nullptr;
        }
        CglFns& f = api->fn;
        const bool ok = load(api->lib, "CGLChoosePixelFormat", f.CGLChoosePixelFormat) &&
                        load(api->lib, "CGLDescribePixelFormat", f.CGLDescribePixelFormat) &&
                        load(api->lib, "CGLReleasePixelFormat", f.CGLReleasePixelFormat) &&
                        load(api->lib, "CGLCreateContext", f.CGLCreateContext) &&
                        load(api->lib, "CGLDestroyContext", f.CGLDestroyContext) &&
                        load(api->lib, "CGLSetCurrentContext", f.CGLSetCurrentContext) &&
                        load(api->lib, "CGLGetCurrentContext", f.CGLGetCurrentContext) &&
                        load(api->lib, "CGLErrorString", f.CGLErrorString);
        if (!ok) {
            logf(TESSERA_LOG_ERROR, "CGL backend unavailable: the library is missing required entry points");
            delete api;
            return nullptr;
        }
        return api;
    } catch (...) {
        return nullptr;
    }
}

} // namespace

const CglApi* cglApi() noexcept {
    static const CglApi* api = loadApi(); // thread-safe one-time init
    return api;
}

CglForeignOps::State CglForeignOps::capture() noexcept {
    State s;
    if (const CglApi* api = cglApi()) s.ctx = api->fn.CGLGetCurrentContext();
    return s;
}

void CglForeignOps::restore(const State& s) noexcept {
    const CglApi* api = cglApi();
    if (!api) return;
    if (api->fn.CGLGetCurrentContext() == s.ctx) return; // already as found
    CGLError e = api->fn.CGLSetCurrentContext(s.ctx);
    if (e != kNoError) e = api->fn.CGLSetCurrentContext(s.ctx); // one retry: the failure may be transient
    if (e != kNoError) {
        // The caller's context cannot be put back (e.g. it was destroyed meanwhile). Whatever happens, one of OUR
        // contexts must not be left current on a thread that never asked for it.
        logf(TESSERA_LOG_WARN, "could not restore the previous CGL context (error %d); leaving the thread without one", static_cast<int>(e));
        api->fn.CGLSetCurrentContext(nullptr);
    }
}

} // namespace tessera::cgl
