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

#include "tessera_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- version */

#define TESSERA_VERSION_MAJOR 0
#define TESSERA_VERSION_MINOR 3
#define TESSERA_VERSION_PATCH 0

/* Incremented on every incompatible change to the C ABI, independent of the semantic version. */
#define TESSERA_ABI_VERSION 1

/* Runtime version of the loaded library. Any out pointer may be NULL. Never fails. */
TESSERA_API void TESSERA_CALL tesseraGetVersion(int32_t* major, int32_t* minor, int32_t* patch);
/* Runtime ABI version; callers should compare against TESSERA_ABI_VERSION at startup. */
TESSERA_API int32_t TESSERA_CALL tesseraGetAbiVersion(void);

/* ---------------------------------------------------------------- results */

typedef int32_t TesseraResult;

#define TESSERA_SUCCESS 0
/* Errors are negative. Values are append-only and never reused. */
#define TESSERA_ERROR_UNKNOWN (-1)
#define TESSERA_ERROR_INVALID_ARGUMENT (-2)  /* null pointer, bad enum, structSize too small */
#define TESSERA_ERROR_INVALID_HANDLE (-3)    /* zero, stale (generation mismatch) or wrong type */
#define TESSERA_ERROR_NOT_INITIALIZED (-4)
#define TESSERA_ERROR_OUT_OF_MEMORY (-5)
#define TESSERA_ERROR_UNSUPPORTED (-6)       /* not available on this platform / build */
#define TESSERA_ERROR_NO_MATCHING_CONFIG (-7)
#define TESSERA_ERROR_PLATFORM (-8)          /* native API failure; see tesseraGetLastErrorMessage */
#define TESSERA_ERROR_WRONG_THREAD (-9)      /* object used from a thread it is not valid on */
#define TESSERA_ERROR_CONTEXT_LOST (-10)

/* Static, NUL-terminated, never NULL, valid for the process lifetime. */
TESSERA_API const char* TESSERA_CALL tesseraResultString(TesseraResult result);

/*
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

/* `message` is UTF-8, NUL-terminated, valid only for the duration of the call. May be invoked from any
 * thread, never with an internal lock held. */
typedef void(TESSERA_CALL* TesseraLogCallback)(int32_t level, const char* message, void* userData);

/* Process-wide. NULL disables logging (the default). Tessera never writes to stdout/stderr itself. The
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

/* TesseraContextConfig::api */
#define TESSERA_API_OPENGL 0x0001u
#define TESSERA_API_OPENGL_ES 0x0002u

/* TesseraContextConfig::profile */
#define TESSERA_PROFILE_ANY 0x0000u
#define TESSERA_PROFILE_CORE 0x0001u
#define TESSERA_PROFILE_COMPATIBILITY 0x0002u

/* TesseraContextConfig::flags (bitmask) */
#define TESSERA_FLAG_DEBUG 0x0001u
#define TESSERA_FLAG_FORWARD_COMPATIBLE 0x0002u
#define TESSERA_FLAG_ROBUST_ACCESS 0x0004u
#define TESSERA_FLAG_RESET_NOTIFICATION 0x0008u /* implies lose-context-on-reset strategy */
#define TESSERA_FLAG_NO_ERROR 0x0010u
#define TESSERA_FLAG_SRGB 0x0020u
#define TESSERA_FLAG_FLOAT_COLOR 0x0040u
#define TESSERA_FLAG_DOUBLE_BUFFER 0x0080u
#define TESSERA_FLAG_STEREO 0x0100u
#define TESSERA_FLAG_RELEASE_FLUSH 0x0200u /* flush on release (vs. none) */

/* TesseraContextConfig::surface - what the context will be able to render to */
#define TESSERA_SURFACE_DEFAULT 0x0000u     /* backend picks the safest available */
#define TESSERA_SURFACE_HIDDEN_WINDOW 0x0001u
#define TESSERA_SURFACE_PBUFFER 0x0002u
#define TESSERA_SURFACE_NONE 0x0003u        /* no drawable exposed; render to FBOs (WGL still uses an internal one) */

/* TesseraContextConfig::fallback */
#define TESSERA_FALLBACK_STRICT 0x0000u     /* fail unless every requested attribute is met */
#define TESSERA_FALLBACK_BEST_EFFORT 0x0001u /* relax soft attributes (samples, bits); report result */

typedef struct TesseraContextConfig {
    uint32_t structSize; /* in: sizeof(TesseraContextConfig) */
    TesseraEnum api;
    int32_t versionMajor; /* minimum requested version; 0 = any */
    int32_t versionMinor;
    TesseraEnum profile;
    uint32_t flags;
    TesseraEnum surface;
    TesseraEnum fallback;
    int32_t redBits, greenBits, blueBits, alphaBits; /* 0 = don't care */
    int32_t depthBits, stencilBits;
    int32_t samples;           /* 0 = no multisampling */
    TesseraContext shareWith;  /* TESSERA_NULL_HANDLE = none */
    uint64_t reserved[8];      /* must be zero; reserved for future use */
} TesseraContextConfig;

/* Initialiser: `TesseraContextConfig cfg = TESSERA_CONTEXT_CONFIG_INIT;` (sets structSize, zeroes the rest). */
#define TESSERA_CONTEXT_CONFIG_INIT {(uint32_t)sizeof(TesseraContextConfig)}

/* What was actually obtained, as opposed to what was requested. */
typedef struct TesseraContextInfo {
    uint32_t structSize; /* in: sizeof(TesseraContextInfo); the library writes at most this many bytes */
    TesseraEnum api;
    int32_t versionMajor;
    int32_t versionMinor;
    TesseraEnum profile;
    uint32_t flags; /* TESSERA_FLAG_* actually in effect */
    TesseraEnum surface;
    int32_t samples;
    int32_t redBits, greenBits, blueBits, alphaBits;
    int32_t depthBits, stencilBits;
    char vendor[128];   /* GL_VENDOR, UTF-8, NUL-terminated, truncated if necessary */
    char renderer[128]; /* GL_RENDERER */
    char version[128];  /* GL_VERSION */
    uint64_t reserved[8];
} TesseraContextInfo;

#define TESSERA_CONTEXT_INFO_INIT {(uint32_t)sizeof(TesseraContextInfo)}

/* ---------------------------------------------------------------- contexts
 *
 * Creating a context never leaves a context current on the calling thread and never disturbs a context
 * the caller (or another library, e.g. JavaFX) already has current. Returns TESSERA_ERROR_UNSUPPORTED
 * while no native backend is available for the platform.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraCreateContext(const TesseraContextConfig* config,
                                                            TesseraContext* outContext);
/* Destroying TESSERA_NULL_HANDLE is an error (INVALID_HANDLE); destroying twice is an error, not UB. */
TESSERA_API TesseraResult TESSERA_CALL tesseraDestroyContext(TesseraContext context);
TESSERA_API TesseraResult TESSERA_CALL tesseraGetContextInfo(TesseraContext context,
                                                             TesseraContextInfo* outInfo);

/*
 * Thread rules (enforced, not just documented):
 *  - A context is current on at most one thread at a time. Making a context current on a second thread
 *    while the first still holds it fails with TESSERA_ERROR_WRONG_THREAD. If the owning thread has
 *    exited, ownership is reclaimed automatically.
 *  - tesseraDestroyContext on a context that is current on a *different live* thread fails with
 *    WRONG_THREAD and leaves the context valid; release it on its thread first. Destroying a context
 *    that is current on the calling thread releases it first.
 *  - Contexts may be created and destroyed from any thread.
 */

/*
 * Makes `context` current on the calling thread, replacing whatever was current there (including a
 * context created by another library; Tessera does not remember or restore it).
 * TESSERA_NULL_HANDLE releases the Tessera context this thread made current; if the thread's current
 * context is not Tessera's, it is left untouched.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraMakeCurrent(TesseraContext context);

/* The Tessera context made current on the calling thread by tesseraMakeCurrent, or NULL handle. */
TESSERA_API TesseraContext TESSERA_CALL tesseraGetCurrentContext(void);

/*
 * Resolves an OpenGL function by name for LWJGL-style loaders. Extension and post-1.1 functions are only
 * returned while a context is current on the calling thread (a platform limitation). Returns NULL if not
 * found; never returns the placeholder values some drivers use (1, 2, 3, -1).
 */
TESSERA_API void* TESSERA_CALL tesseraGetProcAddress(const char* name);

/* Presents the back buffer (a no-op for single-buffered or pbuffer surfaces). Any thread. */
TESSERA_API TesseraResult TESSERA_CALL tesseraSwapBuffers(TesseraContext context);

/* 0 = no vsync, 1 = vsync, -1 = adaptive (if supported). `context` must be current on the calling thread. */
TESSERA_API TesseraResult TESSERA_CALL tesseraSetSwapInterval(TesseraContext context, int32_t interval);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TESSERA_H */
