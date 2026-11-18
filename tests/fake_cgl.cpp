// A fake OpenGL.framework: just enough of CGL and a handful of GL functions to drive the macOS backend on any
// platform. It models the *shape* of CGL as the backend relies on it (pixel-format matching with limits,
// profile- compatible sharing, per-thread current context, share-group textures) and counts live objects so
// leaks fail tests. It cannot validate Apple's real behaviour or constant values; that needs a Mac.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#if defined(_WIN32)
#define FAKE_EXPORT extern "C" __declspec(dllexport)
#else
#define FAKE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

constexpr int kBadAttribute = 10000, kBadPixelFormat = 10002, kBadContext = 10004, kBadMatch = 10009,
              kBadAlloc = 10016;
constexpr int kProfileLegacy = 0x1000, kProfile32 = 0x3200, kProfile4 = 0x4100;

} // namespace

struct CGLPixelFormatRec {
    int profile = 0, colorSize = 0, alpha = 0, depth = 0, stencil = 0, samples = 0;
    bool dbl = false, stereo = false, flt = false;
};
struct CGLContextRec {
    int profile = 0;
    int group = 0;
};

namespace {

std::mutex g_mutex;
std::set<CGLContextRec*> g_contexts;
std::set<CGLPixelFormatRec*> g_formats;
std::map<int, std::set<unsigned>> g_textures; // share group -> texture names
int g_nextGroup = 1;
unsigned g_nextTexture = 1;
std::atomic<int> g_chooseCalls{0};
// Fault injection: every mutating call counts itself; the call whose number equals g_failAt fails. (Resources
// are still released when a *release* call "fails", so the live-object counters measure the backend's
// behaviour, not the fake's.)
std::atomic<int> g_calls{0};
std::atomic<int> g_failAt{0};
bool injectedFault() {
    const int n = ++g_calls;
    return g_failAt.load() == n;
}
thread_local CGLContextRec* t_current = nullptr;

bool envSet(const char* name) {
    const char* v = std::getenv(name);
    return v && *v;
}
int envInt(const char* name, int fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::atoi(v) : fallback;
}

} // namespace

FAKE_EXPORT int CGLChoosePixelFormat(const int* attribs, CGLPixelFormatRec** pix, int* npix) {
    ++g_chooseCalls;
    *pix = nullptr;
    *npix = 0;
    if (injectedFault())
        return kBadAlloc;
    CGLPixelFormatRec want;
    int sampleBuffers = 0;
    bool offline = false, havePofile = false;
    for (const int* a = attribs; *a != 0;) {
        switch (*a) {
        case 5:
            want.dbl = true;
            ++a;
            break;
        case 6:
            want.stereo = true;
            ++a;
            break;
        case 58:
            want.flt = true;
            ++a;
            break;
        case 96:
            offline = true;
            ++a;
            break;
        case 8:
            want.colorSize = a[1];
            a += 2;
            break;
        case 11:
            want.alpha = a[1];
            a += 2;
            break;
        case 12:
            want.depth = a[1];
            a += 2;
            break;
        case 13:
            want.stencil = a[1];
            a += 2;
            break;
        case 55:
            sampleBuffers = a[1];
            a += 2;
            break;
        case 56:
            want.samples = a[1];
            a += 2;
            break;
        case 99:
            want.profile = a[1];
            havePofile = true;
            a += 2;
            break;
        default:
            return kBadAttribute; // an attribute this "driver" has never heard of
        }
    }
    if (!havePofile)
        return kBadAttribute;
    if (want.profile != kProfileLegacy && want.profile != kProfile32 && want.profile != kProfile4)
        return kBadAttribute;
    // Simulated hardware limits, adjustable from the environment between calls.
    if (want.profile == kProfile4 && envSet("FAKE_CGL_NO_GL4"))
        return 0;
    if (envSet("FAKE_CGL_NEEDS_OFFLINE") && !offline)
        return 0; // only reachable on an "offline" GPU
    if (want.flt && envSet("FAKE_CGL_NO_FLOAT"))
        return 0;
    if (sampleBuffers && want.samples > envInt("FAKE_CGL_MAX_SAMPLES", 4))
        return 0;
    if (!sampleBuffers)
        want.samples = 0;

    auto* pf = new CGLPixelFormatRec(want);
    pf->colorSize = want.flt ? 64 : 32; // includes the alpha channel
    pf->alpha = want.flt ? 16 : 8;
    if (pf->depth == 0)
        pf->depth = 24;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_formats.insert(pf);
    *pix = pf;
    *npix = 1;
    return 0;
}

FAKE_EXPORT int CGLDescribePixelFormat(CGLPixelFormatRec* pf, int /*screen*/, int attr, int* value) {
    if (injectedFault())
        return kBadPixelFormat; // leaves *value as the caller initialised it
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_formats.count(pf))
        return kBadPixelFormat;
    switch (attr) {
    case 5:
        *value = pf->dbl;
        break;
    case 6:
        *value = pf->stereo;
        break;
    case 8:
        *value = pf->colorSize;
        break;
    case 11:
        *value = pf->alpha;
        break;
    case 12:
        *value = pf->depth;
        break;
    case 13:
        *value = pf->stencil;
        break;
    case 56:
        *value = pf->samples;
        break;
    case 58:
        *value = pf->flt;
        break;
    default:
        *value = 0;
        break;
    }
    return 0;
}

FAKE_EXPORT int CGLReleasePixelFormat(CGLPixelFormatRec* pf) {
    const bool fault = injectedFault();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_formats.erase(pf))
        return kBadPixelFormat;
    delete pf;
    return fault ? kBadPixelFormat : 0;
}

FAKE_EXPORT int CGLCreateContext(CGLPixelFormatRec* pf, CGLContextRec* share, CGLContextRec** ctx) {
    const bool fault = injectedFault();
    std::lock_guard<std::mutex> lock(g_mutex);
    *ctx = nullptr;
    if (fault)
        return kBadAlloc;
    if (!g_formats.count(pf))
        return kBadPixelFormat;
    if (share && !g_contexts.count(share))
        return kBadContext;
    if (share && share->profile != pf->profile)
        return kBadMatch; // legacy and core cannot share
    auto* c = new CGLContextRec();
    c->profile = pf->profile;
    c->group = share ? share->group : g_nextGroup++;
    g_contexts.insert(c);
    *ctx = c;
    return 0;
}

FAKE_EXPORT int CGLDestroyContext(CGLContextRec* ctx) {
    const bool fault = injectedFault();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_contexts.erase(ctx))
        return kBadContext;
    if (t_current == ctx)
        t_current = nullptr;
    delete ctx;
    return fault ? kBadContext : 0;
}

FAKE_EXPORT int CGLSetCurrentContext(CGLContextRec* ctx) {
    if (injectedFault())
        return kBadContext; // current context unchanged
    if (ctx) {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_contexts.count(ctx))
            return kBadContext;
    }
    t_current = ctx;
    return 0;
}

FAKE_EXPORT CGLContextRec* CGLGetCurrentContext() {
    return t_current;
}

FAKE_EXPORT const char* CGLErrorString(int error) {
    switch (error) {
    case 0:
        return "no error";
    case kBadAttribute:
        return "invalid pixel format attribute";
    case kBadPixelFormat:
        return "invalid pixel format";
    case kBadContext:
        return "invalid context";
    case kBadMatch:
        return "invalid pixel format/context match";
    default:
        return "fake CGL error";
    }
}

// ---- a few GL functions -------------------------------------------------------------------------

FAKE_EXPORT const unsigned char* glGetString(unsigned name) {
    if (injectedFault())
        return nullptr;
    if (!t_current)
        return nullptr; // like a real driver: no current context, no answer
    switch (name) {
    case 0x1F00:
        return reinterpret_cast<const unsigned char*>("Fake Vendor");
    case 0x1F01:
        return reinterpret_cast<const unsigned char*>("Fake CGL Renderer");
    case 0x1F02:
        if (t_current->profile == kProfile4)
            return reinterpret_cast<const unsigned char*>("4.1 FakeCGL");
        if (t_current->profile == kProfile32)
            return reinterpret_cast<const unsigned char*>("3.2 FakeCGL");
        return reinterpret_cast<const unsigned char*>("2.1 FakeCGL");
    default:
        return reinterpret_cast<const unsigned char*>("");
    }
}

FAKE_EXPORT void glGetIntegerv(unsigned name, int* out) {
    if (injectedFault())
        return; // leaves *out untouched
    if (!t_current)
        return;
    const bool core = t_current->profile != kProfileLegacy;
    if (name == 0x9126)
        *out = core ? 1 : 0; // GL_CONTEXT_PROFILE_MASK: core bit
    else if (name == 0x821E)
        *out = core ? 1 : 0; // GL_CONTEXT_FLAGS: forward-compatible (always set on core)
}

FAKE_EXPORT unsigned glGetError() {
    return 0;
}

FAKE_EXPORT void glGenTextures(int n, unsigned* out) {
    if (!t_current)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < n; ++i) {
        out[i] = g_nextTexture++;
        g_textures[t_current->group].insert(out[i]);
    }
}

FAKE_EXPORT void glBindTexture(unsigned /*target*/, unsigned /*id*/) {}

FAKE_EXPORT unsigned char glIsTexture(unsigned id) {
    if (!t_current)
        return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_textures[t_current->group].count(id) ? 1 : 0;
}

// ---- introspection for the test -----------------------------------------------------------------

FAKE_EXPORT int fake_cgl_live_contexts() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_contexts.size());
}
FAKE_EXPORT int fake_cgl_live_pixel_formats() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_formats.size());
}
FAKE_EXPORT int fake_cgl_choose_calls() {
    return g_chooseCalls.load();
}
// Restarts the call counter; the n-th call after this fails (0 = never).
FAKE_EXPORT void fake_cgl_set_fail_at(int n) {
    g_calls = 0;
    g_failAt = n;
}
FAKE_EXPORT int fake_cgl_calls() {
    return g_calls.load();
}
