// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <tessera/tessera.h>

namespace tessera {

/*
 * Sharing and framebuffer formats (ROADMAP 12.7, ADR 0018). Measured on NVIDIA/WGL: a context cannot share
 * objects with a parent whose depth buffer has a different size (16 or 32 against 24: wglShareLists and
 * wglCreateContextAttribsARB both fail), while a different stencil, multisampling, sRGB or float format
 * shared fine. So when a share parent is given, the child should use the PARENT's format if that format
 * already serves the request, and only otherwise a format of its own.
 *
 * True when a context whose format is described by `parent` (the info read back from it) satisfies every
 * framebuffer part of `want`: no fewer colour, alpha, depth and stencil bits, no fewer samples, and the
 * buffering and colour flags it asked for.
 */
inline bool formatServesRequest(const TesseraContextConfig& want, const TesseraContextInfo& parent) {
    if (want.redBits > parent.redBits || want.greenBits > parent.greenBits ||
        want.blueBits > parent.blueBits || want.alphaBits > parent.alphaBits ||
        want.depthBits > parent.depthBits || want.stencilBits > parent.stencilBits ||
        want.samples > parent.samples)
        return false;
    constexpr uint32_t kFormatFlags =
        TESSERA_FLAG_DOUBLE_BUFFER | TESSERA_FLAG_STEREO | TESSERA_FLAG_SRGB | TESSERA_FLAG_FLOAT_COLOR;
    return (want.flags & kFormatFlags & ~parent.flags) == 0;
}

} // namespace tessera
