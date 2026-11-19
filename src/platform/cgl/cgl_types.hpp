// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

/*
 * The slice of CGL (Core OpenGL, OpenGL.framework) the macOS backend needs, declared by hand. CGL is a plain
 * C API, so no Objective-C and no Apple headers are involved: the framework is loaded with dlopen like
 * libGL/libEGL elsewhere, which also lets this code be compiled and tested on other platforms against a fake
 * library.
 *
 * VERIFICATION STATUS: the constants below are written from documentation and memory and have NOT been
 * checked against Apple's headers or run on a Mac. They are the first thing to check if the backend
 * misbehaves on real hardware. (The pixel-format attribute and profile values are the same ones every
 * CGL/NSOpenGL program uses.)
 */

#include <cstdint>

namespace tessera::cgl {

using CGLError = int32_t;
using GLint = int32_t;
struct CGLContextRec;     // opaque
struct CGLPixelFormatRec; // opaque
using CGLContextObj = CGLContextRec*;
using CGLPixelFormatObj = CGLPixelFormatRec*;
/* CGLPixelFormatAttribute is a 4-byte enum; the attribute list is passed as an array of those. */
using PixelFormatAttribute = int32_t;

// ---- errors ---------------------------------------------------------------------------------------
constexpr CGLError kNoError = 0;
constexpr CGLError kBadAttribute = 10000;
constexpr CGLError kBadPixelFormat = 10002;
constexpr CGLError kBadContext = 10004;
constexpr CGLError kBadMatch = 10009;
constexpr CGLError kBadAlloc = 10016;

// ---- pixel format attributes (kCGLPFA...) ---------------------------------------------------------
constexpr PixelFormatAttribute kPFADoubleBuffer = 5;
constexpr PixelFormatAttribute kPFAStereo = 6;
constexpr PixelFormatAttribute kPFAColorSize = 8;
constexpr PixelFormatAttribute kPFAAlphaSize = 11;
constexpr PixelFormatAttribute kPFADepthSize = 12;
constexpr PixelFormatAttribute kPFAStencilSize = 13;
constexpr PixelFormatAttribute kPFASampleBuffers = 55;
constexpr PixelFormatAttribute kPFASamples = 56;
constexpr PixelFormatAttribute kPFAColorFloat = 58;
constexpr PixelFormatAttribute kPFAAllowOfflineRenderers = 96;
constexpr PixelFormatAttribute kPFAOpenGLProfile = 99;

// ---- OpenGL profile values (kCGLOGLPVersion_...) --------------------------------------------------
constexpr int32_t kProfileLegacy = 0x1000; // OpenGL 2.1, compatibility
constexpr int32_t kProfile32Core = 0x3200; // 3.2 core (a newer core version may be returned)
constexpr int32_t kProfile4Core = 0x4100;  // 4.1 core

// ---- function table -------------------------------------------------------------------------------
struct CglFns {
    CGLError (*CGLChoosePixelFormat)(const PixelFormatAttribute*, CGLPixelFormatObj*, GLint*) = nullptr;
    CGLError (*CGLDescribePixelFormat)(CGLPixelFormatObj, GLint, PixelFormatAttribute, GLint*) = nullptr;
    CGLError (*CGLReleasePixelFormat)(CGLPixelFormatObj) = nullptr;
    CGLError (*CGLCreateContext)(CGLPixelFormatObj, CGLContextObj, CGLContextObj*) = nullptr;
    CGLError (*CGLDestroyContext)(CGLContextObj) = nullptr;
    CGLError (*CGLSetCurrentContext)(CGLContextObj) = nullptr;
    CGLContextObj (*CGLGetCurrentContext)() = nullptr;
    const char* (*CGLErrorString)(CGLError) = nullptr;
};

} // namespace tessera::cgl
