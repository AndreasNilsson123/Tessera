#pragma once

/*
 * The slice of EGL the backend needs, declared by hand (see x11_types.hpp for why): libEGL is loaded at run time,
 * so building needs no EGL headers and a machine without libEGL just reports "unsupported".
 */

#include <cstdint>

namespace tessera::egl {

using EGLint = int32_t;
using EGLBoolean = unsigned int;
using EGLenum = unsigned int;
using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;

// ---- constants ----------------------------------------------------------------------------------
constexpr EGLint kNone = 0x3038;
constexpr EGLint kAlphaSize = 0x3021, kBlueSize = 0x3022, kGreenSize = 0x3023, kRedSize = 0x3024;
constexpr EGLint kDepthSize = 0x3025, kStencilSize = 0x3026;
constexpr EGLint kSamples = 0x3031, kSampleBuffers = 0x3032;
constexpr EGLint kSurfaceType = 0x3033, kRenderableType = 0x3040;
constexpr EGLint kPbufferBit = 0x0001;
constexpr EGLint kOpenGlEsBit = 0x0001, kOpenGlEs2Bit = 0x0004, kOpenGlBit = 0x0008, kOpenGlEs3Bit = 0x0040;
constexpr EGLint kWidth = 0x3057, kHeight = 0x3056;
constexpr EGLint kColorComponentType = 0x3339, kColorComponentFloat = 0x333B; // EGL_EXT_pixel_format_float
constexpr EGLint kGlColorspace = 0x309D, kGlColorspaceSrgb = 0x3089;          // EGL_KHR_gl_colorspace
constexpr EGLint kVendor = 0x3053, kVersion = 0x3054, kExtensions = 0x3055, kClientApis = 0x308D;
constexpr EGLint kDraw = 0x3059, kRead = 0x305A;
constexpr EGLenum kOpenGlEsApi = 0x30A0, kOpenGlApi = 0x30A2;

constexpr EGLint kContextMajorVersion = 0x3098, kContextMinorVersion = 0x30FB;
constexpr EGLint kContextProfileMask = 0x30FD;
constexpr EGLint kContextDebug = 0x31B0, kContextForwardCompatible = 0x31B1, kContextRobustAccess = 0x31B2;
constexpr EGLint kContextNoError = 0x31B3;
constexpr EGLint kContextResetStrategy = 0x31BD, kLoseContextOnReset = 0x31BF;
constexpr EGLint kContextReleaseBehavior = 0x2097, kContextReleaseFlush = 0x2098;
constexpr EGLint kProfileCore = 0x1, kProfileCompat = 0x2;

constexpr EGLenum kPlatformDevice = 0x313F;      // EGL_EXT_platform_device
constexpr EGLenum kPlatformSurfaceless = 0x31DD; // EGL_MESA_platform_surfaceless

constexpr EGLint kBadAlloc = 0x3003, kBadMatch = 0x3009, kBadAccess = 0x3002;

// ---- function tables ----------------------------------------------------------------------------
struct EglFns {
    const char* (*eglQueryString)(EGLDisplay, EGLint) = nullptr;
    EGLint (*eglGetError)() = nullptr;
    EGLDisplay (*eglGetDisplay)(void*) = nullptr;
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
    EGLBoolean (*eglBindAPI)(EGLenum) = nullptr;
    EGLenum (*eglQueryAPI)() = nullptr;
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
    EGLBoolean (*eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint*) = nullptr;
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
    EGLBoolean (*eglDestroyContext)(EGLDisplay, EGLContext) = nullptr;
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    EGLContext (*eglGetCurrentContext)() = nullptr;
    EGLDisplay (*eglGetCurrentDisplay)() = nullptr;
    EGLSurface (*eglGetCurrentSurface)(EGLint) = nullptr;
    EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*) = nullptr;
    EGLBoolean (*eglDestroySurface)(EGLDisplay, EGLSurface) = nullptr;
    EGLBoolean (*eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint*) = nullptr;
    void* (*eglGetProcAddress)(const char*) = nullptr;

    // Resolved through eglGetProcAddress; may be null.
    EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum, void*, const EGLint*) = nullptr;
    EGLBoolean (*eglQueryDevicesEXT)(EGLint, void**, EGLint*) = nullptr;
};

} // namespace tessera::egl
