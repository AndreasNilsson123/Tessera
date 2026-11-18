#include "config.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "boundary.hpp"

namespace tessera {
namespace {

constexpr uint32_t kKnownFlags =
    TESSERA_FLAG_DEBUG | TESSERA_FLAG_FORWARD_COMPATIBLE | TESSERA_FLAG_ROBUST_ACCESS |
    TESSERA_FLAG_RESET_NOTIFICATION | TESSERA_FLAG_NO_ERROR | TESSERA_FLAG_SRGB | TESSERA_FLAG_FLOAT_COLOR |
    TESSERA_FLAG_DOUBLE_BUFFER | TESSERA_FLAG_STEREO | TESSERA_FLAG_RELEASE_FLUSH;

// Minimum acceptable structSize: everything up to and including `fallback`.
constexpr std::size_t kMinSize = offsetof(TesseraContextConfig, fallback) + sizeof(TesseraEnum);

TesseraResult bad(const char* what) noexcept {
    char buf[160];
    std::snprintf(buf, sizeof buf, "invalid TesseraContextConfig: %s", what);
    return fail(TESSERA_ERROR_INVALID_ARGUMENT, buf);
}

} // namespace

TesseraResult normalizeConfig(const TesseraContextConfig* in, TesseraContextConfig& out) noexcept {
    if (!in)
        return fail(TESSERA_ERROR_INVALID_ARGUMENT, "config is NULL");
    if (in->structSize < kMinSize)
        return bad("structSize too small (was it set?)");

    std::memset(&out, 0, sizeof out);
    const std::size_t n = in->structSize < sizeof out ? in->structSize : sizeof out;
    std::memcpy(&out, in, n);
    out.structSize = static_cast<uint32_t>(sizeof out);

    if (out.api != TESSERA_API_OPENGL && out.api != TESSERA_API_OPENGL_ES)
        return bad("unknown api");

    if (out.profile != TESSERA_PROFILE_ANY && out.profile != TESSERA_PROFILE_CORE &&
        out.profile != TESSERA_PROFILE_COMPATIBILITY)
        return bad("unknown profile");
    if (out.api == TESSERA_API_OPENGL_ES && out.profile != TESSERA_PROFILE_ANY)
        return bad("profile applies to desktop OpenGL only");
    if (out.profile != TESSERA_PROFILE_ANY && out.versionMajor != 0 &&
        (out.versionMajor < 3 || (out.versionMajor == 3 && out.versionMinor < 2)))
        return bad("core/compatibility profiles require OpenGL 3.2 or newer");

    if (out.flags & ~kKnownFlags)
        return bad("unknown flag bits");
    if ((out.flags & TESSERA_FLAG_FORWARD_COMPATIBLE) && out.api != TESSERA_API_OPENGL)
        return bad("forward-compatible applies to desktop OpenGL only");
    if ((out.flags & TESSERA_FLAG_FORWARD_COMPATIBLE) && out.versionMajor != 0 && out.versionMajor < 3)
        return bad("forward-compatible contexts require OpenGL 3.0 or newer");
    if ((out.flags & TESSERA_FLAG_RESET_NOTIFICATION) && !(out.flags & TESSERA_FLAG_ROBUST_ACCESS))
        return bad("reset notification requires robust access");
    if ((out.flags & TESSERA_FLAG_NO_ERROR) &&
        (out.flags & (TESSERA_FLAG_DEBUG | TESSERA_FLAG_ROBUST_ACCESS)))
        return bad("no-error cannot be combined with debug or robust access");

    if (out.surface > TESSERA_SURFACE_NONE)
        return bad("unknown surface type");
    if (out.fallback > TESSERA_FALLBACK_BEST_EFFORT)
        return bad("unknown fallback policy");

    if (out.versionMajor < 0 || out.versionMinor < 0 || out.versionMajor > 9 || out.versionMinor > 9)
        return bad("version out of range");
    const int32_t bits[] = {out.redBits,   out.greenBits, out.blueBits,
                            out.alphaBits, out.depthBits, out.stencilBits};
    for (int32_t b : bits)
        if (b < 0 || b > 32)
            return bad("bit depth out of range 0..32");
    if (out.samples < 0 || out.samples > 64)
        return bad("samples out of range 0..64");

    for (uint64_t r : out.reserved)
        if (r != 0)
            return bad("reserved fields must be zero");

    return TESSERA_SUCCESS;
}

} // namespace tessera
