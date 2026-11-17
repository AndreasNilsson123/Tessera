#pragma once

#include <tessera/tessera.h>

#include <cstddef>
#include <functional>

#if defined(_WIN32)
#define TESSERA_GLCALL __stdcall
#else
#define TESSERA_GLCALL
#endif

namespace tessera {

/* The three core GL entry points needed to describe a context. Resolved by the backend. */
struct GlQueryFns {
    const unsigned char*(TESSERA_GLCALL* getString)(unsigned) = nullptr;
    void(TESSERA_GLCALL* getIntegerv)(unsigned, int*) = nullptr;
    unsigned(TESSERA_GLCALL* getError)() = nullptr;
};

/* Bounded, always NUL-terminated copy (src may be null). */
void copyString(char* dst, std::size_t cap, const char* src) noexcept;

/*
 * Describes the context that is current on the calling thread: api, version, profile, context flags
 * (debug/forward-compatible/robust/no-error/reset-notification) and the vendor/renderer/version strings.
 * Zeroes `info` first. Platform code adds the drawable's pixel-format fields afterwards.
 * Everything reported is read back from the driver, never copied from the request: a flag the driver quietly dropped
 * (best-effort mode) must not show up as granted.
 * Returns false if the driver gave no usable version string.
 */
bool queryContextInfo(const GlQueryFns& gl, TesseraContextInfo& info);

/*
 * Parses a GL_VERSION string ("4.6.0 NVIDIA 546.30", "OpenGL ES 3.2 Mesa ...", "OpenGL ES-CM 1.1"). False for anything
 * malformed; must never overflow or read past the terminator, whatever a broken driver returns.
 */
bool parseGlVersion(const char* text, bool& isEs, int& major, int& minor) noexcept;

/* glGetGraphicsResetStatus (core, KHR, ARB or EXT spelling), resolved through the backend's loader; null if absent. */
using GlResetStatusFn = unsigned(TESSERA_GLCALL*)();
GlResetStatusFn resolveResetStatus(const std::function<void*(const char*)>& getProc);

/*
 * Shared body of ContextObject::queryResetStatus. `grantedFlags` is what the context actually got (info.flags): without
 * both robust access and reset notification the driver gives no meaningful answer, so this reports UNSUPPORTED.
 */
TesseraResult queryResetStatusFrom(GlResetStatusFn fn, uint32_t grantedFlags, int32_t& out);

/* Strict-mode check: does `info` (including its pixel-format fields) honour everything in `requested`? */
bool contextSatisfies(const TesseraContextConfig& requested, const TesseraContextInfo& info) noexcept;

} // namespace tessera
