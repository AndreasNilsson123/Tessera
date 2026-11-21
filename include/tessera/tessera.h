// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
/*
 * Tessera - OpenGL / OpenGL ES context creation library. Public C ABI.
 *
 * ABI RULES (see docs/abi-policy.md)
 *  - Pure C99 header; usable from C++, JNI, Panama/jextract.
 *  - No exceptions, abort() or stdout/stderr output cross this boundary.
 *  - All objects are opaque 64-bit handles (index + generation); 0 is always invalid.
 *  - Every input struct begins with `uint32_t structSize`, set by the caller to sizeof(its struct).
 *    New fields are only ever appended; the library reads only structSize bytes.
 *  - Enums are fixed-width (uint32_t / int32_t) typedefs plus #define constants, never C enums,
 *    so their size cannot vary by compiler.
 *  - Before 1.0 (TESSERA_VERSION_MAJOR == 0) the ABI may change between minor versions.
 *
 * Single source of truth for the version: the three macros below (CMake parses this file).
 */
#ifndef TESSERA_H
#define TESSERA_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tessera_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- version */

#define TESSERA_VERSION_MAJOR 0
#define TESSERA_VERSION_MINOR 14
#define TESSERA_VERSION_PATCH 0

/** Incremented on every incompatible change to the C ABI, independent of the semantic version. */
#define TESSERA_ABI_VERSION 1

/** Runtime version of the loaded library. Any out pointer may be NULL. Never fails. */
TESSERA_API void TESSERA_CALL tesseraGetVersion(int32_t* major, int32_t* minor, int32_t* patch);
/** Runtime ABI version; callers should compare against TESSERA_ABI_VERSION at startup. */
TESSERA_API int32_t TESSERA_CALL tesseraGetAbiVersion(void);

/* ---------------------------------------------------------------- results */

typedef int32_t TesseraResult;

#define TESSERA_SUCCESS 0
/** Errors are negative. Values are append-only and never reused. */
#define TESSERA_ERROR_UNKNOWN (-1)
#define TESSERA_ERROR_INVALID_ARGUMENT (-2) /**< null pointer, bad enum, structSize too small */
#define TESSERA_ERROR_INVALID_HANDLE (-3)   /**< zero, stale (generation mismatch) or wrong type */
#define TESSERA_ERROR_NOT_INITIALIZED (-4)
#define TESSERA_ERROR_OUT_OF_MEMORY (-5)
#define TESSERA_ERROR_UNSUPPORTED (-6) /**< not available on this platform / build */
#define TESSERA_ERROR_NO_MATCHING_CONFIG (-7)
#define TESSERA_ERROR_PLATFORM (-8)     /**< native API failure; see tesseraGetLastErrorMessage */
#define TESSERA_ERROR_WRONG_THREAD (-9) /**< object used from a thread it is not valid on */
#define TESSERA_ERROR_CONTEXT_LOST (-10)

/** Static, NUL-terminated, never NULL, valid for the process lifetime. */
TESSERA_API const char* TESSERA_CALL tesseraResultString(TesseraResult result);

/**
 * Thread-local description of the last failure on the calling thread (UTF-8, NUL-terminated).
 * Never NULL. Only meaningful immediately after a call that returned an error (successful calls do not
 * clear it). Valid until the next Tessera call on this thread.
 */
TESSERA_API const char* TESSERA_CALL tesseraGetLastErrorMessage(void);

/* -------------------------------------------------------------- lifecycle
 *
 * tesseraInit/tesseraTerminate are reference counted and thread-safe: any number of independent
 * components in one process (or several classloaders in one JVM) may each call tesseraInit and later
 * tesseraTerminate. Real teardown, including destruction of every remaining context, happens when the
 * last reference is released. Do not call tesseraTerminate from inside a log callback.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraInit(void);
TESSERA_API TesseraResult TESSERA_CALL tesseraTerminate(void);

/* ---------------------------------------------------------------- logging */

#define TESSERA_LOG_ERROR 1
#define TESSERA_LOG_WARN 2
#define TESSERA_LOG_INFO 3
#define TESSERA_LOG_DEBUG 4

/** `message` is UTF-8, NUL-terminated, valid only for the duration of the call. May be invoked from any
 * thread, never with an internal lock held. */
typedef void(TESSERA_CALL* TesseraLogCallback)(int32_t level, const char* message, void* userData);

/** Process-wide. NULL disables logging (the default). Tessera never writes to stdout/stderr itself. The
 * caller must keep `userData` valid until the callback is replaced and any in-flight call returned. */
TESSERA_API void TESSERA_CALL tesseraSetLogCallback(TesseraLogCallback callback, void* userData);

/* ---------------------------------------------------------------- handles */

typedef uint64_t TesseraHandle;
#define TESSERA_NULL_HANDLE ((TesseraHandle)0)

typedef TesseraHandle TesseraContext;

/* ----------------------------------------------------- context configuration
 *
 * Declared now so the shape of the ABI is reviewed early; the functions that consume it arrive in
 * Phase 1 (tesseraCreateContext, ...). Zero-initialise, then set structSize.
 * All "0 = default / don't care" unless noted.
 */

typedef uint32_t TesseraEnum;

/** TesseraContextConfig::api */
#define TESSERA_API_OPENGL 0x0001u
#define TESSERA_API_OPENGL_ES 0x0002u

/** TesseraContextConfig::profile */
#define TESSERA_PROFILE_ANY 0x0000u
#define TESSERA_PROFILE_CORE 0x0001u
#define TESSERA_PROFILE_COMPATIBILITY 0x0002u

/** TesseraContextConfig::flags (bitmask) */
#define TESSERA_FLAG_DEBUG 0x0001u
#define TESSERA_FLAG_FORWARD_COMPATIBLE 0x0002u
#define TESSERA_FLAG_ROBUST_ACCESS 0x0004u
#define TESSERA_FLAG_RESET_NOTIFICATION 0x0008u /**< implies lose-context-on-reset strategy */
#define TESSERA_FLAG_NO_ERROR 0x0010u
#define TESSERA_FLAG_SRGB 0x0020u
#define TESSERA_FLAG_FLOAT_COLOR 0x0040u
#define TESSERA_FLAG_DOUBLE_BUFFER 0x0080u
#define TESSERA_FLAG_STEREO 0x0100u
#define TESSERA_FLAG_RELEASE_FLUSH 0x0200u /**< flush on release (vs. none) */
/** Ask the driver for exactly versionMajor.versionMinor, not for the newest version at or above it (added in
 * 0.13.0): the library makes that one request and no more. Without the flag it tries the newest version the
 * driver has, down to the requested one. Drivers may still answer a request with a newer, compatible version
 * (Mesa does for core profiles; NVIDIA and Apple do not): TesseraContextInfo says what was granted, and the
 * requested version remains a minimum either way. */
#define TESSERA_FLAG_EXACT_VERSION 0x0400u

/** TesseraContextConfig::surface - what the context will be able to render to */
#define TESSERA_SURFACE_DEFAULT 0x0000u /**< backend picks the safest available */
#define TESSERA_SURFACE_HIDDEN_WINDOW 0x0001u
#define TESSERA_SURFACE_PBUFFER 0x0002u
/**
 * NONE: no drawable is exposed; render to framebuffer objects. EGL and GLX really have none (GLX falls back
 * to a hidden window on a driver that refuses a drawable-less make-current); WGL cannot (wglMakeCurrent needs
 * a DC) and keeps a 1x1 window.
 */
#define TESSERA_SURFACE_NONE 0x0003u
/** Reported for contexts made by tesseraCreateContextForWindow; not a request value. */
#define TESSERA_SURFACE_WINDOW 0x0004u

/**
 * TesseraContextConfig::gpuPreference. Windows (WGL) only; any other backend refuses a non-default value with
 * UNSUPPORTED (on EGL use TESSERA_EGL_DEVICE). A process cannot make Windows load a different GPU's OpenGL
 * driver, so this does not switch GPUs: Tessera resolves the adapter the request names through DXGI and
 * CHECKS that the OpenGL driver it got belongs to it (by PCI vendor). A mismatch, or an adapter that cannot
 * be verified because several share a vendor, fails with NO_MATCHING_CONFIG: you never silently get another
 * GPU. For HIGH_PERFORMANCE / LOW_POWER the check is a failure in strict mode and a logged warning in
 * best-effort mode; for an explicit adapterLuid it is always a failure.
 */
#define TESSERA_GPU_DEFAULT 0x0000u /**< no request */
#define TESSERA_GPU_HIGH_PERFORMANCE                                                                         \
    0x0001u /**< the adapter DXGI considers the fastest (never a software adapter) */
#define TESSERA_GPU_LOW_POWER                                                                                \
    0x0002u /**< the adapter DXGI considers the most frugal (usually the integrated GPU) */

/**
 * TesseraContextConfig::priority: a hint for how the GPU schedules this context's work against other
 * contexts. GLX: GLX_EXT_context_priority; EGL: EGL_IMG_context_priority. WGL has no such extension. A
 * request is never silently ignored: STRICT fails with NO_MATCHING_CONFIG when it cannot be honoured (no
 * extension, or the driver granted a different level, which drivers do when the process lacks the privilege
 * for HIGH); BEST_EFFORT logs and goes on. TesseraContextInfo::priority reports what the driver says it
 * granted.
 */
#define TESSERA_PRIORITY_DEFAULT 0u /**< no request / the driver's default */
#define TESSERA_PRIORITY_LOW 1u
#define TESSERA_PRIORITY_MEDIUM 2u
#define TESSERA_PRIORITY_HIGH 3u

/** TesseraAdapterInfo::flags and TesseraContextInfo::adapterClass. Integrated/discrete are known where the
 * platform says so (Windows: by dedicated video memory, a heuristic; macOS: by renderer); neither set = not
 * known. */
#define TESSERA_ADAPTER_SOFTWARE 0x1u   /**< runs on the CPU (WARP, llvmpipe, Apple Software Renderer) */
#define TESSERA_ADAPTER_INTEGRATED 0x2u /**< shares memory with the CPU */
#define TESSERA_ADAPTER_DISCRETE 0x4u   /**< has its own memory */

/** TesseraContextConfig::fallback */
#define TESSERA_FALLBACK_STRICT 0x0000u      /**< fail unless every requested attribute is met */
#define TESSERA_FALLBACK_BEST_EFFORT 0x0001u /**< relax soft attributes (samples, bits); report result */

typedef struct TesseraContextConfig {
    uint32_t structSize; /**< in: sizeof(TesseraContextConfig) */
    TesseraEnum api;
    int32_t versionMajor; /**< minimum requested version; 0 = any. The context is the newest the driver offers
                             that is at least this (see TESSERA_FLAG_EXACT_VERSION); OpenGL ES with 0
                             tries 3.2, 3.1, 3.0, 2.0 */
    int32_t versionMinor;
    TesseraEnum profile;
    uint32_t flags;
    TesseraEnum surface;
    TesseraEnum fallback;
    int32_t redBits, greenBits, blueBits, alphaBits; /**< 0 = don't care */
    int32_t depthBits, stencilBits;
    int32_t samples;          /**< 0 = no multisampling */
    TesseraContext shareWith; /**< TESSERA_NULL_HANDLE = none */
    /* Added in 0.7.0, taken out of the reserved space (the size did not change): zero = no request. */
    uint64_t adapterLuid; /**< Windows: DXGI adapter LUID as (HighPart << 32) | LowPart; see TESSERA_GPU_* */
    uint32_t gpuPreference; /**< TESSERA_GPU_*; not combinable with adapterLuid */
    uint32_t priority;      /**< TESSERA_PRIORITY_* (added in 0.11.0, taken from the reserved space) */
    /** Which GPU, as an id from tesseraEnumerateAdapters (added in 0.14.0, the last of the reserved space,
     * the size did not change): "" = no request. NUL-terminated ASCII, at most 47 characters. See
     * TesseraAdapterInfo for the forms. An explicit adapter is never silently replaced by another: a backend
     * that cannot honour it reports UNSUPPORTED, one that cannot find it NO_MATCHING_CONFIG. Not combinable
     * with adapterLuid or gpuPreference. */
    char adapter[48];
} TesseraContextConfig;

/** Zeroed config with structSize set: `TesseraContextConfig cfg = tesseraDefaultContextConfig();`
 * (A function rather than a brace-initialiser macro: partial initialisers trip -Wmissing-field-initializers.
 * Bindings that cannot call static inline functions just zero the struct and set structSize themselves.) */
static inline TesseraContextConfig tesseraDefaultContextConfig(void) {
    TesseraContextConfig c;
    memset(&c, 0, sizeof c);
    c.structSize = (uint32_t)sizeof c;
    return c;
}

/** What was actually obtained, as opposed to what was requested. */
typedef struct TesseraContextInfo {
    uint32_t structSize; /**< in: sizeof(TesseraContextInfo); the library writes at most this many bytes */
    TesseraEnum api;
    int32_t versionMajor;
    int32_t versionMinor;
    TesseraEnum profile;
    uint32_t flags; /**< TESSERA_FLAG_* actually in effect */
    TesseraEnum surface;
    int32_t samples;
    int32_t redBits, greenBits, blueBits, alphaBits;
    int32_t depthBits, stencilBits;
    char vendor[128];   /**< GL_VENDOR, UTF-8, NUL-terminated, truncated if necessary */
    char renderer[128]; /**< GL_RENDERER */
    char version[128];  /**< GL_VERSION */
    uint32_t
        priority; /**< TESSERA_PRIORITY_*: what the driver reports it granted; DEFAULT where it cannot say */
    /** TESSERA_ADAPTER_SOFTWARE / _INTEGRATED / _DISCRETE of the GPU that served the context (0 = not known).
     * Added in 0.14.0. */
    uint32_t adapterClass;
    /** The id (see TesseraAdapterInfo) of the GPU that served the context, as far as the backend can tell; ""
     * when it cannot (GLX, a Windows machine with two adapters of one vendor). Added in 0.14.0, taken from
     * the reserved space. */
    char adapter[48];
    uint64_t reserved[1];
} TesseraContextInfo;

static inline TesseraContextInfo tesseraDefaultContextInfo(void) {
    TesseraContextInfo i;
    memset(&i, 0, sizeof i);
    i.structSize = (uint32_t)sizeof i;
    return i;
}

/* ---------------------------------------------------------------- adapters (0.14.0)
 *
 * One way to say "this GPU": the inventory below lists them, `TesseraContextConfig::adapter` asks for one,
 * and `TesseraContextInfo::adapter` says which one served a context. The id is text, stable across runs where
 * the platform has a stable name for the device: "luid:00000000-0000ABCD"  Windows, the DXGI adapter LUID
 * (HighPart-LowPart in hex): WGL, and the same value as adapterLuid
 *   "uuid:xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"  EGL, the device UUID (EGL_EXT_device_persistent_id)
 *   "node:/dev/dri/renderD128"  EGL, the DRM render node
 *   "index:N"                 EGL, the position in eglQueryDevicesEXT (NOT stable across boots; only when
 * nothing better exists) "cgl:0x00024600"          macOS, the CGL renderer id Nothing here switches GPUs
 * behind your back: where a backend cannot honour an id it says UNSUPPORTED (GLX has no way to name a GPU);
 * where Windows decides the driver (WGL), the id is verified, never assumed (see TESSERA_GPU_*).
 */
typedef struct TesseraAdapterInfo {
    uint32_t structSize; /**< in, per element: sizeof(TesseraAdapterInfo); also the array stride */
    uint32_t flags;      /**< TESSERA_ADAPTER_* */
    uint32_t vendorId;   /**< PCI vendor id, 0 = not known */
    uint32_t deviceId;   /**< PCI device id, 0 = not known */
    uint64_t luid;    /**< Windows: the adapter LUID as TesseraContextConfig::adapterLuid takes it; else 0 */
    char backend[16]; /**< "wgl", "egl", "cgl": the backend that can use this adapter */
    char id[48];      /**< what TesseraContextConfig::adapter takes */
    char name[128];   /**< UTF-8, for people */
    uint64_t reserved[4];
} TesseraAdapterInfo;

static inline TesseraAdapterInfo tesseraDefaultAdapterInfo(void) {
    TesseraAdapterInfo a;
    memset(&a, 0, sizeof a);
    a.structSize = (uint32_t)sizeof a;
    return a;
}

/**
 * The GPUs the backends of this build can use, backend by backend in the order they are tried. Write up to
 * `capacity` entries into `out` (each with structSize set, which is also the stride); `*outCount` is always
 * the TOTAL, so call once with capacity 0 and out NULL to size the array. A backend that cannot list (GLX)
 * contributes nothing; an empty list is not an error. Needs tesseraInit.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraEnumerateAdapters(TesseraAdapterInfo* out, int32_t capacity,
                                                                int32_t* outCount);

/* ---------------------------------------------------------------- contexts
 *
 * Creating a context never leaves a context current on the calling thread and never disturbs a context
 * the caller (or another library, e.g. JavaFX) already has current. Returns TESSERA_ERROR_UNSUPPORTED
 * while no native backend is available for the platform.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraCreateContext(const TesseraContextConfig* config,
                                                            TesseraContext* outContext);
/** Destroying TESSERA_NULL_HANDLE is an error (INVALID_HANDLE); destroying twice is an error, not UB.
 * Contexts that were created sharing with this one keep working (they hold their own reference). */
TESSERA_API TesseraResult TESSERA_CALL tesseraDestroyContext(TesseraContext context);
TESSERA_API TesseraResult TESSERA_CALL tesseraGetContextInfo(TesseraContext context,
                                                             TesseraContextInfo* outInfo);

/* ------------------------------------------------------------------- native handles
 *
 * What other libraries need to interoperate with a Tessera context: share a texture with Direct3D
 * (WGL_NV_DX_interop2), export a buffer as a dma-buf, wrap it for Vulkan or Metal, ... Tessera does not wrap
 * those APIs; it hands out the native objects (and tesseraGetProcAddress resolves their extension functions,
 * e.g. "wglDXOpenDeviceNV").
 *
 * The handles are BORROWED: valid until the context is destroyed, owned by Tessera. Never destroy, close,
 * terminate or unregister them. In particular a Display / EGLDisplay is shared by every Tessera context of
 * the backend. Calls that need the context current (most extension functions) are subject to the usual rule:
 * it is current on one thread at a time.
 */
#define TESSERA_NATIVE_NONE 0u
#define TESSERA_NATIVE_WGL 1u
#define TESSERA_NATIVE_GLX 2u
#define TESSERA_NATIVE_EGL 3u
#define TESSERA_NATIVE_CGL 4u

typedef struct TesseraNativeHandles {
    uint32_t structSize; /**< in: sizeof(TesseraNativeHandles) */
    uint32_t kind;       /**< out: TESSERA_NATIVE_*; decides what handles[] mean */
    /*
     * WGL: [0] HGLRC  [1] HDC (window or pbuffer)  [2] HWND (0 for a pbuffer)  [3] HPBUFFERARB (0 for a
     * window)  [4] pixel format GLX: [0] Display*  [1] GLXContext  [2] GLXDrawable  [3] GLXFBConfig  [4]
     * Window (0 for a pbuffer)  [5] screen EGL: [0] EGLDisplay  [1] EGLContext  [2] EGLSurface (0 if
     * surfaceless)  [3] EGLConfig  [4] client API (EGL_OPENGL_API / _ES_API) CGL: [0] CGLContextObj Unused
     * entries are 0. Pointers are stored as integers so the layout is the same on every ABI.
     */
    uint64_t handles[6];
    uint64_t reserved[4];
} TesseraNativeHandles;

static inline TesseraNativeHandles tesseraDefaultNativeHandles(void) {
    TesseraNativeHandles h;
    memset(&h, 0, sizeof h);
    h.structSize = (uint32_t)sizeof h;
    return h;
}

TESSERA_API TesseraResult TESSERA_CALL tesseraGetNativeHandles(TesseraContext context,
                                                               TesseraNativeHandles* outHandles);

/* ------------------------------------------------------------- pre-warmed contexts
 *
 * Creating a context costs tens of milliseconds (a window, a pixel format, a driver context, the driver's own
 * setup), which matters to programs that start many short-lived contexts. Only contexts that were NEVER used
 * are pooled, so what you get is indistinguishable from a freshly created one: reusing a used context would
 * carry its GL state and objects over to an unrelated user, which this library does not do.
 */

/**
 * Creates `count` contexts for `config` now and keeps them ready. A later tesseraCreateContext with an
 * identical request (and shareWith = NULL handle) takes one of them instead of creating a new one. Blocks
 * until all exist; call it from a background thread. At most 64 contexts are pooled in total. The pool is
 * emptied by tesseraTerminate.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraPrewarmContexts(const TesseraContextConfig* config,
                                                              int32_t count);

/** Contexts currently waiting in the pool (all configurations). */
TESSERA_API int32_t TESSERA_CALL tesseraPooledContextCount(void);

/**
 * Keeping the pool full. Tessera starts no threads of its own, so refilling is something YOUR thread does,
 * when it suits you: after creating a context, call tesseraPoolTopUp from a background thread to bring the
 * number of waiting contexts for exactly this request back to `target`. It creates only what is missing
 * (`*outCreated`, optional), never destroys anything, and the total for all requests stays capped at 64.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraPoolTopUp(const TesseraContextConfig* config, int32_t target,
                                                        int32_t* outCreated);

/** Contexts currently waiting for exactly this request (what tesseraPoolTopUp counts). */
TESSERA_API TesseraResult TESSERA_CALL tesseraGetPooledCount(const TesseraContextConfig* config,
                                                             int32_t* outCount);

/** Destroys every waiting context (all requests), e.g. before a long idle period. Returns how many there
 * were.
 */
TESSERA_API int32_t TESSERA_CALL tesseraPoolClear(void);

/* ----------------------------------------------------------- rendering into a caller's window
 *
 * Normally Tessera creates its own hidden drawables and never touches anyone else's windows. This is the one
 * exception, and it follows strict ownership rules:
 *  - The window is BORROWED. Tessera never destroys, resizes, shows, hides or subclasses it, and destroying
 * the context never destroys it. The window must outlive every use of the context; once it is gone, calls on
 * the context fail.
 *  - The window's pixel format (Windows) / visual (X11) is decided by whoever created the window. If it
 * already has one, Tessera adopts it and checks it against the request (STRICT fails with NO_MATCHING_CONFIG
 * if it does not satisfy it).
 *  - A Windows window can have its pixel format set only once, ever. Tessera sets one only if the caller says
 * so with TESSERA_ATTACH_ALLOW_PIXEL_FORMAT and the window has none yet; otherwise it fails rather than touch
 * the window.
 *  - Supported on WGL (nativeWindow = HWND) and GLX (nativeWindow = X11 Window id, on the X server of
 * $DISPLAY). Other backends return TESSERA_ERROR_UNSUPPORTED. Presenting uses tesseraSwapBuffers;
 * tesseraSetSwapInterval works as usual. The context reports TESSERA_SURFACE_WINDOW.
 */
#define TESSERA_ATTACH_ALLOW_PIXEL_FORMAT 0x1u

TESSERA_API TesseraResult TESSERA_CALL tesseraCreateContextForWindow(const TesseraContextConfig* config,
                                                                     uint64_t nativeWindow, uint32_t flags,
                                                                     TesseraContext* outContext);

/* ------------------------------------------------------------------------ probing
 *
 * "Would this request work here, and what would I get?" Creates the context for real (that is the only honest
 * answer), records what the driver reports, and destroys it again. No handle is returned, the pool is
 * bypassed (a probe must test the driver, not a cached context), nothing is left current, and the caller's
 * current context is undisturbed.
 *
 * Returns what tesseraCreateContext would return for the same request. `report` is filled in whenever
 * `config` and `report` themselves are valid, success or not; with INVALID_ARGUMENT for those two it is left
 * untouched.
 */
typedef struct TesseraProbeReport {
    uint32_t structSize;     /**< in: sizeof(TesseraProbeReport) */
    TesseraResult result;    /**< same as the return value */
    TesseraContextInfo info; /**< what the driver gave; all zero unless result == TESSERA_SUCCESS */
    char backend[32];  /**< "wgl", "glx", "egl", "cgl": the backend that answered, or the last one that was
                        * asked
                        */
    char message[256]; /**< why it failed (truncated); empty on success */
    uint64_t reserved[4];
} TesseraProbeReport;

static inline TesseraProbeReport tesseraDefaultProbeReport(void) {
    TesseraProbeReport r;
    memset(&r, 0, sizeof r);
    r.structSize = (uint32_t)sizeof r;
    r.info.structSize = (uint32_t)sizeof r.info;
    return r;
}

TESSERA_API TesseraResult TESSERA_CALL tesseraProbe(const TesseraContextConfig* config,
                                                    TesseraProbeReport* outReport);

/* ------------------------------------------------- reset notification and context loss
 *
 * A context created with TESSERA_FLAG_ROBUST_ACCESS | TESSERA_FLAG_RESET_NOTIFICATION can be polled for a GPU
 * reset (driver crash, TDR, GPU removed). Like glGetGraphicsResetStatus, which this wraps, the application
 * decides when to poll (typically once per frame). Once a reset is reported the context is *lost*: it stays
 * lost, tesseraMakeCurrent and tesseraSwapBuffers on it return TESSERA_ERROR_CONTEXT_LOST, and it should be
 * replaced with tesseraRecreateContext. A backend that learns of the loss natively (e.g. EGL_CONTEXT_LOST)
 * marks it the same way.
 */
#define TESSERA_RESET_NONE 0
#define TESSERA_RESET_GUILTY 1   /**< this context caused the reset */
#define TESSERA_RESET_INNOCENT 2 /**< another context caused it, but this one was affected */
#define TESSERA_RESET_UNKNOWN 3

/**
 * Context-loss events. Polling stays the application's job (Tessera starts no threads), but the moment ANY
 * call learns that a context is lost (tesseraGetResetStatus, or a native "context lost" report from
 * tesseraMakeCurrent / tesseraSwapBuffers) the callback below runs once for that context, so code that owns
 * many contexts does not need its own polling loop around every call site.
 *
 * `resetStatus` is a TESSERA_RESET_* value other than NONE (UNKNOWN when the loss was reported natively). The
 * callback runs synchronously on the thread that detected the loss, after the context is marked lost and with
 * no Tessera lock held, so it may call tesseraRecreateContext, tesseraDestroyContext and tesseraIsContextLost
 * (but not tesseraTerminate, and it must not make the lost context current). Once per context, even if
 * several threads notice the loss at the same moment. `context` is valid when the call starts (another thread
 * may destroy it meanwhile).
 */
typedef void(TESSERA_CALL* TesseraContextLostCallback)(TesseraContext context, int32_t resetStatus,
                                                       void* userData);

/** Process-wide; NULL disables (the default). The caller keeps `userData` valid until the callback is
 * replaced and any in-flight call returned. */
TESSERA_API void TESSERA_CALL tesseraSetContextLostCallback(TesseraContextLostCallback callback,
                                                            void* userData);

/**
 * Has Tessera already seen this context get lost? Never queries the driver and works from any thread (the
 * context need not be current): it only reports what an earlier call found out. `outLost` receives 0 or 1,
 * `outStatus` (optional) the TESSERA_RESET_* value, NONE while the context is healthy. INVALID_HANDLE for a
 * dead handle.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraIsContextLost(TesseraContext context, int32_t* outLost,
                                                            int32_t* outStatus);

/**
 * Polls the reset status; `context` must be current on the calling thread. Returns UNSUPPORTED if the context
 * was not created with robust access and reset notification (or the driver lacks the query). For a context
 * already known to be lost it returns the status that was reported.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraGetResetStatus(TesseraContext context, int32_t* outStatus);

/** tesseraRecreateContext flags */
#define TESSERA_RECREATE_SHARE_PRESERVED                                                                     \
    0x1u /**< the new context joined a surviving member of the old share group */

/**
 * Creates a new context from the same request as `context` (normally a lost one; the old handle stays valid
 * and must still be destroyed by the caller). If `context` shared objects with other contexts, the new one
 * shares with a surviving member of that group, so the group's textures/buffers are still there
 * (TESSERA_RECREATE_SHARE_PRESERVED). If no member survives, the new context starts an empty group and
 * everything must be re-uploaded. `outFlags` may be NULL.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraRecreateContext(TesseraContext context, TesseraContext* outNew,
                                                              uint32_t* outFlags);

/**
 * Thread rules (enforced, not just documented):
 *  - A context is current on at most one thread at a time. Making a context current on a second thread
 *    while the first still holds it fails with TESSERA_ERROR_WRONG_THREAD. If the owning thread has
 *    exited, ownership is reclaimed automatically.
 *  - Contexts may be created and destroyed from any thread. Destroying a context that is current on a
 *    *different live* thread succeeds at once and invalidates the handle everywhere, but the native context
 *    cannot be released from outside the owning thread: it is torn down on that thread when it next releases
 *    it (tesseraMakeCurrent(NULL)), switches to another context, or exits. (This is what makes it safe for a
 *    finalizer/Cleaner thread to destroy a context the render thread still holds.) Destroying a context that
 *    is current on the calling thread releases it first.
 */

/**
 * Makes `context` current on the calling thread, replacing whatever was current there (including a
 * context created by another library; Tessera does not remember or restore it).
 * TESSERA_NULL_HANDLE releases the Tessera context this thread made current; if the thread's current
 * context is not Tessera's, it is left untouched.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraMakeCurrent(TesseraContext context);

/** The Tessera context made current on the calling thread by tesseraMakeCurrent, or NULL handle. */
TESSERA_API TesseraContext TESSERA_CALL tesseraGetCurrentContext(void);

/**
 * Resolves an OpenGL function by name for LWJGL-style loaders. Extension and post-1.1 functions are only
 * returned while a context is current on the calling thread (a platform limitation). Returns NULL if not
 * found; never returns the placeholder values some drivers use (1, 2, 3, -1).
 * Names that do not start with "gl" are always NULL. On X11/GLX a made-up "gl..." name may still resolve to a
 * harmless stub (the GLX loader contract): learn what is supported from the extension strings, not from
 * whether a pointer came back.
 */
TESSERA_API void* TESSERA_CALL tesseraGetProcAddress(const char* name);

/** Presents the back buffer (a no-op for pbuffer surfaces). Like tesseraSetSwapInterval, `context` must be
 * current on the calling thread: that is the one rule every platform (WGL, GLX, EGL) agrees on. */
TESSERA_API TesseraResult TESSERA_CALL tesseraSwapBuffers(TesseraContext context);

/** 0 = no vsync, 1 = vsync, -1 = adaptive (if supported). `context` must be current on the calling thread.
 * Success means the driver accepted the call, not that this is the interval you now get (drivers clamp, and a
 * compositor can override): ask tesseraGetSwapControl what is in effect. */
TESSERA_API TesseraResult TESSERA_CALL tesseraSetSwapInterval(TesseraContext context, int32_t interval);

/**
 * @name Windows of other toolkits (HWND, X11, Wayland, ...)
 * tesseraCreateContextForWindow takes one integer, which is all a Win32 or X11 window needs. A Wayland
 * surface cannot be attached without the connection it belongs to, and a window handle without saying which
 * kind it is invites the wrong guess, so this describes the window. The same borrowing rules apply as for
 * tesseraCreateContextForWindow: Tessera never destroys, hides, moves or subclasses the window, and the
 * window must outlive the context (destroy the context first).
 *
 *  - TESSERA_WINDOW_WIN32: `window` is an HWND (WGL; flags as for tesseraCreateContextForWindow).
 *  - TESSERA_WINDOW_X11: `window` is an X11 Window id on the server of $DISPLAY (GLX, or EGL when that is the
 * backend in use). `display` may be a Display* only for documentation: Tessera always uses its own connection
 * to the same server.
 *  - TESSERA_WINDOW_WAYLAND: `window` is a wl_surface*, `display` the wl_display* it belongs to (EGL).
 * `width`/`height` give the size of the surface's buffer. Tessera creates a wl_egl_window for it, which it
 * owns and destroys with the context; the wl_surface and wl_display stay yours, and your event queue must be
 * dispatched by you (Tessera does not). After the window changes size call tesseraResizeWindow.
 *  - TESSERA_WINDOW_COCOA: reserved for an NSView; UNSUPPORTED today (see docs/adr/0014).
 * @{
 */
#define TESSERA_WINDOW_WIN32 1u
#define TESSERA_WINDOW_X11 2u
#define TESSERA_WINDOW_WAYLAND 3u
#define TESSERA_WINDOW_COCOA 4u

typedef struct TesseraNativeWindow {
    uint32_t structSize; /**< in: sizeof(TesseraNativeWindow) */
    uint32_t kind;       /**< TESSERA_WINDOW_* */
    uint64_t window;     /**< HWND, X11 Window, wl_surface*, NSView*; pointers stored as integers */
    uint64_t display;    /**< Wayland: wl_display* (required); X11: ignored; others 0 */
    int32_t width;       /**< Wayland: initial buffer width in pixels (required, > 0); others ignored */
    int32_t height;      /**< Wayland: initial buffer height in pixels (required, > 0); others ignored */
    uint64_t reserved[4];
} TesseraNativeWindow;

static inline TesseraNativeWindow tesseraDefaultNativeWindow(void) {
    TesseraNativeWindow w;
    memset(&w, 0, sizeof w);
    w.structSize = (uint32_t)sizeof w;
    return w;
}

/** Like tesseraCreateContextForWindow for any kind of window. `flags`: TESSERA_ATTACH_ALLOW_PIXEL_FORMAT
 * (WIN32 only). */
TESSERA_API TesseraResult TESSERA_CALL tesseraCreateContextForNativeWindow(const TesseraContextConfig* config,
                                                                           const TesseraNativeWindow* window,
                                                                           uint32_t flags,
                                                                           TesseraContext* outContext);

/**
 * Tells the context's surface that its window changed size (Wayland: resizes the wl_egl_window; the other
 * kinds follow the window by themselves and accept the call). `context` must be current on the calling
 * thread, and the next tesseraSwapBuffers presents a buffer of the new size. INVALID_ARGUMENT for a
 * non-positive size, UNSUPPORTED for a context that has no window.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraResizeWindow(TesseraContext context, int32_t width,
                                                           int32_t height);
/** @} */

/**
 * @name Swap control and frame pacing
 * What a context's swap control can do and what is actually in effect. Only contexts that render to a window
 * (tesseraCreateContextForWindow, or the hidden window of a WGL/GLX context) have a swap interval; pbuffer,
 * surfaceless and CGL contexts report no capabilities. The window must be double-buffered: swapping a
 * single-buffered window returns at once, so no interval can pace it.
 * @{
 */
/** tesseraSetSwapInterval can work on this context (a swap-control extension exists and the drawable is a
 * window). */
#define TESSERA_SWAP_CAP_CONTROL 0x1u
/** An interval of -1 (adaptive vsync, WGL/GLX_EXT_swap_control_tear) is accepted. */
#define TESSERA_SWAP_CAP_ADAPTIVE 0x2u
/** `applied` is read back from the driver; without this flag it is TESSERA_SWAP_UNKNOWN. */
#define TESSERA_SWAP_CAP_READBACK 0x4u
/** Value of a TesseraSwapControl field that is not known (INT32_MIN). */
#define TESSERA_SWAP_UNKNOWN ((int32_t)(-2147483647 - 1))

typedef struct TesseraSwapControl {
    uint32_t structSize; /**< in: sizeof(TesseraSwapControl) */
    uint32_t caps;       /**< out: TESSERA_SWAP_CAP_* */
    /** The last interval a tesseraSetSwapInterval call on this context succeeded with, or
     * TESSERA_SWAP_UNKNOWN. */
    int32_t requested;
    /** The interval the driver reports it is using (negative = adaptive), or TESSERA_SWAP_UNKNOWN (no
     * READBACK). */
    int32_t applied;
    /** The largest interval the drawable accepts (drivers clamp larger requests); 0 if not known. */
    int32_t maxInterval;
    uint32_t reserved32; /**< zero */
    uint64_t reserved[4];
} TesseraSwapControl;

static inline TesseraSwapControl tesseraDefaultSwapControl(void) {
    TesseraSwapControl s;
    memset(&s, 0, sizeof s);
    s.structSize = (uint32_t)sizeof s;
    s.requested = TESSERA_SWAP_UNKNOWN;
    s.applied = TESSERA_SWAP_UNKNOWN;
    return s;
}

/**
 * Fills `outInfo`. `context` must be current on the calling thread (the drivers read the interval of the
 * current drawable). Succeeds with caps 0 for a context that has no swap interval; check `caps`, not the
 * result code. The library writes at most the caller's structSize bytes.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraGetSwapControl(TesseraContext context,
                                                             TesseraSwapControl* outInfo);
/** @} */

#ifdef __cplusplus
} /**< extern "C" */
#endif

#endif /**< TESSERA_H */
