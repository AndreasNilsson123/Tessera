// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <tessera/tessera.h>

namespace tessera {

// What a driver's refusal to create a context means, for every backend, in one reviewed place (ROADMAP 12.2).
// The backends call these instead of keeping their own `if (code == ...)` chains; test_core has a row per code.
//   NO_MATCHING_CONFIG  the driver cannot provide this combination (version, profile, flags, format): the caller may relax
//   OUT_OF_MEMORY       resources, not the request
//   PLATFORM            anything else (a native failure the caller cannot fix by changing the request)

/* WGL: GetLastError() after wglCreateContextAttribsARB. The ARB spec names ERROR_INVALID_VERSION_ARB (0x2095)
 * and ERROR_INVALID_PROFILE_ARB (0x2096), but drivers also answer a combination they dislike with plain Win32
 * codes (NVIDIA: ERROR_INVALID_DATA), and put various high bits on all of them, hence the mask. */
inline TesseraResult classifyWglCreateError(unsigned long lastError) {
    const unsigned long code = lastError & 0xFFFFul;
    if (code == 0x2095 || code == 0x2096 || code == 0x000D /*ERROR_INVALID_DATA*/ ||
        code == 0x0057 /*ERROR_INVALID_PARAMETER*/)
        return TESSERA_ERROR_NO_MATCHING_CONFIG;
    if (code == 0x0008 /*ERROR_NOT_ENOUGH_MEMORY*/ || code == 0x000E /*ERROR_OUTOFMEMORY*/)
        return TESSERA_ERROR_OUT_OF_MEMORY;
    return TESSERA_ERROR_PLATFORM;
}

/* GLX: the X error code caught around glXCreateContextAttribsARB. An unsupported version or profile surfaces
 * as BadMatch (8), BadValue (2), GLXBadFBConfig or GLXBadProfileARB; BadAlloc (11) is the only one about
 * resources. */
inline TesseraResult classifyGlxCreateError(int xError) {
    return xError == 11 ? TESSERA_ERROR_OUT_OF_MEMORY : TESSERA_ERROR_NO_MATCHING_CONFIG;
}

/* EGL: eglGetError() after eglCreateContext. BAD_MATCH / BAD_ATTRIBUTE / BAD_CONFIG / BAD_PARAMETER: this
 * version/profile on this config is not available; BAD_ALLOC: resources; anything else is the request not
 * being servable too (drivers differ in what they report for an unsupported version), except the
 * display/context states below, which are not about the request. */
inline TesseraResult classifyEglCreateError(int eglError) {
    switch (eglError) {
    case 0x3003: // EGL_BAD_ALLOC
        return TESSERA_ERROR_OUT_OF_MEMORY;
    case 0x3001: // EGL_NOT_INITIALIZED
    case 0x3008: // EGL_BAD_DISPLAY
    case 0x300E: // EGL_CONTEXT_LOST
        return TESSERA_ERROR_PLATFORM;
    default: // BAD_ACCESS, BAD_ATTRIBUTE, BAD_CONFIG, BAD_CONTEXT, BAD_MATCH, BAD_PARAMETER, ...
        return TESSERA_ERROR_NO_MATCHING_CONFIG;
    }
}

/* CGL: the CGLError of CGLCreateContext (a version is chosen with the pixel format, so there is no ladder
 * here). */
inline TesseraResult classifyCglCreateError(int cglError) {
    if (cglError == 10016) // kCGLBadAlloc
        return TESSERA_ERROR_OUT_OF_MEMORY;
    if (cglError == 10009 || cglError == 10002) // kCGLBadMatch, kCGLBadPixelFormat
        return TESSERA_ERROR_NO_MATCHING_CONFIG;
    return TESSERA_ERROR_PLATFORM;
}

} // namespace tessera
