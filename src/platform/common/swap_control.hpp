// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <tessera/tessera.h>

#include <cstdint>

namespace tessera {

/* Which swap-control API a GLX connection can use, in order of preference. EXT is per drawable and can be
 * read back (glXQueryDrawable); MESA and SGI act on the current context's drawable, MESA can be read back,
 * SGI cannot. */
enum class GlxSwapApi { None, Ext, Mesa, Sgi };

struct GlxSwapInputs {
    bool hasExt = false, hasMesa = false, hasSgi = false; // extension advertised
    bool extFn = false, mesaFn = false, sgiFn = false;    // entry point resolved
    bool hasTear = false;                                 // GLX_EXT_swap_control_tear
    bool queryDrawableFn = false, getMesaFn = false;      // read-back entry points
};

inline GlxSwapApi chooseGlxSwapApi(const GlxSwapInputs& in) noexcept {
    if (in.hasExt && in.extFn)
        return GlxSwapApi::Ext;
    if (in.hasMesa && in.mesaFn)
        return GlxSwapApi::Mesa;
    if (in.hasSgi && in.sgiFn)
        return GlxSwapApi::Sgi;
    return GlxSwapApi::None;
}

/* TESSERA_SWAP_CAP_* for a GLX window drawable (pbuffers and drawable-less contexts have none: the caller
 * passes windowDrawable=false). Adaptive vsync exists only through EXT_swap_control_tear, which extends the
 * EXT API. */
inline uint32_t glxSwapCaps(const GlxSwapInputs& in, bool windowDrawable) noexcept {
    if (!windowDrawable)
        return 0;
    const GlxSwapApi api = chooseGlxSwapApi(in);
    if (api == GlxSwapApi::None)
        return 0;
    uint32_t caps = TESSERA_SWAP_CAP_CONTROL;
    if (api == GlxSwapApi::Ext && in.hasTear)
        caps |= TESSERA_SWAP_CAP_ADAPTIVE;
    if ((api == GlxSwapApi::Ext && in.queryDrawableFn) || (api == GlxSwapApi::Mesa && in.getMesaFn))
        caps |= TESSERA_SWAP_CAP_READBACK;
    return caps;
}

struct WglSwapInputs {
    bool hasExt = false, hasTear = false;
    bool setFn = false, getFn = false;
};

inline uint32_t wglSwapCaps(const WglSwapInputs& in, bool windowDrawable) noexcept {
    if (!windowDrawable || !in.hasExt || !in.setFn)
        return 0;
    uint32_t caps = TESSERA_SWAP_CAP_CONTROL;
    if (in.hasTear)
        caps |= TESSERA_SWAP_CAP_ADAPTIVE;
    if (in.getFn)
        caps |= TESSERA_SWAP_CAP_READBACK;
    return caps;
}

/* GLX_EXT_swap_control reports the interval as a magnitude and flags the late-swap-tearing mode separately.
 */
inline int32_t glxAppliedInterval(int32_t magnitude, bool lateSwapsTear) noexcept {
    return lateSwapsTear && magnitude > 0 ? -magnitude : magnitude;
}

} // namespace tessera
