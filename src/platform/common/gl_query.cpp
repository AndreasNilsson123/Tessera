#include "gl_query.hpp"

#include <cstring>
#include <mutex>

namespace tessera {
namespace {

constexpr unsigned kGlVendor = 0x1F00, kGlRenderer = 0x1F01, kGlVersion = 0x1F02;
constexpr unsigned kGlContextFlags = 0x821E, kGlProfileMask = 0x9126, kGlResetStrategy = 0x8256;
constexpr int kLoseContextOnReset = 0x8252;
constexpr unsigned kGlContextReleaseBehavior = 0x82FB;
constexpr int kGlReleaseBehaviorFlush = 0x82FC;

bool parseVersion(const char* text, bool& isEs, int& major, int& minor) {
    if (!text) return false;
    isEs = std::strncmp(text, "OpenGL ES", 9) == 0;
    const char* p = text;
    if (isEs) {
        p += 9;
        if (std::strncmp(p, "-CM", 3) == 0 || std::strncmp(p, "-CL", 3) == 0) p += 3;
        while (*p == ' ') ++p;
    }
    // "<major>.<minor>..." - parsed by hand (locale-independent, no CRT scanf).
    auto number = [&p](int& out) {
        if (*p < '0' || *p > '9') return false;
        int v = 0;
        for (; *p >= '0' && *p <= '9'; ++p) v = v * 10 + (*p - '0');
        out = v;
        return true;
    };
    if (!number(major) || *p != '.') return false;
    ++p;
    return number(minor);
}

} // namespace

void copyString(char* dst, std::size_t cap, const char* src) noexcept {
    std::size_t i = 0;
    if (src)
        for (; i + 1 < cap && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

bool queryContextInfo(const GlQueryFns& gl, TesseraContextInfo& info) {
    if (!gl.getString || !gl.getIntegerv || !gl.getError) return false;

    std::memset(&info, 0, sizeof info);
    info.structSize = sizeof info;

    // glGetString hands back a pointer into driver-owned memory. Some drivers (Mesa's software rasteriser formats the
    // result into one global buffer) are not safe when several threads ask at once, and the pointer may be rewritten
    // while we copy from it. Tessera can at least keep its own threads from doing that to each other: query and copy
    // under one process-wide lock. (Other libraries' threads are beyond our control.)
    static std::mutex* const stringLock = new std::mutex(); // leaked on purpose: usable during static destruction
    char versionText[sizeof info.version];
    {
        std::lock_guard<std::mutex> lock(*stringLock);
        copyString(versionText, sizeof versionText, reinterpret_cast<const char*>(gl.getString(kGlVersion)));
        copyString(info.vendor, sizeof info.vendor, reinterpret_cast<const char*>(gl.getString(kGlVendor)));
        copyString(info.renderer, sizeof info.renderer, reinterpret_cast<const char*>(gl.getString(kGlRenderer)));
    }
    copyString(info.version, sizeof info.version, versionText);

    bool isEs = false;
    int major = 0, minor = 0;
    if (!parseVersion(versionText, isEs, major, minor)) return false;
    info.api = isEs ? TESSERA_API_OPENGL_ES : TESSERA_API_OPENGL;
    info.versionMajor = major;
    info.versionMinor = minor;

    while (gl.getError() != 0) {} // drain anything pending from before us

    if (isEs) {
        info.profile = TESSERA_PROFILE_ANY;
    } else if (major > 3 || (major == 3 && minor >= 2)) {
        int mask = 0;
        gl.getIntegerv(kGlProfileMask, &mask);
        // A context created without a profile mask reports 0 on some drivers; it is a compatibility context.
        info.profile = (mask & 1) ? TESSERA_PROFILE_CORE : TESSERA_PROFILE_COMPATIBILITY;
    } else {
        info.profile = TESSERA_PROFILE_COMPATIBILITY;
    }

    uint32_t flags = 0;
    if (major >= 3) {
        int f = 0;
        gl.getIntegerv(kGlContextFlags, &f);
        if (f & 0x1) flags |= TESSERA_FLAG_FORWARD_COMPATIBLE;
        if (f & 0x2) flags |= TESSERA_FLAG_DEBUG;
        if (f & 0x4) {
            flags |= TESSERA_FLAG_ROBUST_ACCESS;
            int strategy = 0;
            gl.getIntegerv(kGlResetStrategy, &strategy);
            if (strategy == kLoseContextOnReset) flags |= TESSERA_FLAG_RESET_NOTIFICATION;
        }
        if (f & 0x8) flags |= TESSERA_FLAG_NO_ERROR;
        while (gl.getError() != 0) {}
    }
    // Release behaviour (GL 4.5 / KHR_context_flush_control). Left at -1 where the query does not exist, in which
    // case nothing is claimed.
    int release = -1;
    gl.getIntegerv(kGlContextReleaseBehavior, &release);
    if (release == kGlReleaseBehaviorFlush) flags |= TESSERA_FLAG_RELEASE_FLUSH;
    while (gl.getError() != 0) {}
    info.flags = flags;
    return true;
}

bool contextSatisfies(const TesseraContextConfig& c, const TesseraContextInfo& i) noexcept {
    const bool versionOk = c.versionMajor == 0 || i.versionMajor > c.versionMajor ||
                           (i.versionMajor == c.versionMajor && i.versionMinor >= c.versionMinor);
    const bool apiOk = i.api == c.api;
    const bool profileOk = c.profile == TESSERA_PROFILE_ANY || i.profile == c.profile;
    const bool samplesOk = i.samples >= c.samples;
    const uint32_t checked = TESSERA_FLAG_DEBUG | TESSERA_FLAG_FORWARD_COMPATIBLE | TESSERA_FLAG_ROBUST_ACCESS |
                             TESSERA_FLAG_NO_ERROR | TESSERA_FLAG_SRGB | TESSERA_FLAG_FLOAT_COLOR |
                             TESSERA_FLAG_DOUBLE_BUFFER | TESSERA_FLAG_STEREO;
    const uint32_t missing = c.flags & ~i.flags & checked;
    return versionOk && apiOk && profileOk && samplesOk && missing == 0;
}

} // namespace tessera
