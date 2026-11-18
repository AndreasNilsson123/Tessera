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
#define TESSERA_VERSION_MINOR 6
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
#define TESSERA_ERROR_INVALID_ARGUMENT (-2) /* null pointer, bad enum, structSize too small */
#define TESSERA_ERROR_INVALID_HANDLE (-3)   /* zero, stale (generation mismatch) or wrong type */
#define TESSERA_ERROR_NOT_INITIALIZED (-4)
#define TESSERA_ERROR_OUT_OF_MEMORY (-5)
#define TESSERA_ERROR_UNSUPPORTED (-6) /* not available on this platform / build */
#define TESSERA_ERROR_NO_MATCHING_CONFIG (-7)
#define TESSERA_ERROR_PLATFORM (-8)     /* native API failure; see tesseraGetLastErrorMessage */
#define TESSERA_ERROR_WRONG_THREAD (-9) /* object used from a thread it is not valid on */
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
#define TESSERA_SURFACE_DEFAULT 0x0000u /* backend picks the safest available */
#define TESSERA_SURFACE_HIDDEN_WINDOW 0x0001u
#define TESSERA_SURFACE_PBUFFER 0x0002u
#define TESSERA_SURFACE_NONE                                                                                 \
    0x0003u /* no drawable exposed; render to FBOs (WGL still uses an internal one) */
#define TESSERA_SURFACE_WINDOW                                                                               \
    0x0004u /* reported for contexts made by tesseraCreateContextForWindow; not a request value */

/* TesseraContextConfig::fallback */
#define TESSERA_FALLBACK_STRICT 0x0000u      /* fail unless every requested attribute is met */
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
    int32_t samples;          /* 0 = no multisampling */
    TesseraContext shareWith; /* TESSERA_NULL_HANDLE = none */
    uint64_t reserved[8];     /* must be zero; reserved for future use */
} TesseraContextConfig;

/* Zeroed config with structSize set: `TesseraContextConfig cfg = tesseraDefaultContextConfig();`
 * (A function rather than a brace-initialiser macro: partial initialisers trip -Wmissing-field-initializers.
 * Bindings that cannot call static inline functions just zero the struct and set structSize themselves.) */
static inline TesseraContextConfig tesseraDefaultContextConfig(void) {
    TesseraContextConfig c;
    memset(&c, 0, sizeof c);
    c.structSize = (uint32_t)sizeof c;
    return c;
}

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

static inline TesseraContextInfo tesseraDefaultContextInfo(void) {
    TesseraContextInfo i;
    memset(&i, 0, sizeof i);
    i.structSize = (uint32_t)sizeof i;
    return i;
}

/* ---------------------------------------------------------------- contexts
 *
 * Creating a context never leaves a context current on the calling thread and never disturbs a context
 * the caller (or another library, e.g. JavaFX) already has current. Returns TESSERA_ERROR_UNSUPPORTED
 * while no native backend is available for the platform.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraCreateContext(const TesseraContextConfig* config,
                                                            TesseraContext* outContext);
/* Destroying TESSERA_NULL_HANDLE is an error (INVALID_HANDLE); destroying twice is an error, not UB.
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
    uint32_t structSize; /* in: sizeof(TesseraNativeHandles) */
    uint32_t kind;       /* out: TESSERA_NATIVE_*; decides what handles[] mean */
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

/*
 * Creates `count` contexts for `config` now and keeps them ready. A later tesseraCreateContext with an
 * identical request (and shareWith = NULL handle) takes one of them instead of creating a new one. Blocks
 * until all exist; call it from a background thread. At most 64 contexts are pooled in total. The pool is
 * emptied by tesseraTerminate.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraPrewarmContexts(const TesseraContextConfig* config,
                                                              int32_t count);

/* Contexts currently waiting in the pool (all configurations). */
TESSERA_API int32_t TESSERA_CALL tesseraPooledContextCount(void);

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
    uint32_t structSize;     /* in: sizeof(TesseraProbeReport) */
    TesseraResult result;    /* same as the return value */
    TesseraContextInfo info; /* what the driver gave; all zero unless result == TESSERA_SUCCESS */
    char backend[32]; /* "wgl", "glx", "egl", "cgl": the backend that answered, or the last one that was asked
                       */
    char message[256]; /* why it failed (truncated); empty on success */
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
#define TESSERA_RESET_GUILTY 1   /* this context caused the reset */
#define TESSERA_RESET_INNOCENT 2 /* another context caused it, but this one was affected */
#define TESSERA_RESET_UNKNOWN 3

/*
 * Polls the reset status; `context` must be current on the calling thread. Returns UNSUPPORTED if the context
 * was not created with robust access and reset notification (or the driver lacks the query). For a context
 * already known to be lost it returns the status that was reported.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraGetResetStatus(TesseraContext context, int32_t* outStatus);

/* tesseraRecreateContext flags */
#define TESSERA_RECREATE_SHARE_PRESERVED                                                                     \
    0x1u /* the new context joined a surviving member of the old share group */

/*
 * Creates a new context from the same request as `context` (normally a lost one; the old handle stays valid
 * and must still be destroyed by the caller). If `context` shared objects with other contexts, the new one
 * shares with a surviving member of that group, so the group's textures/buffers are still there
 * (TESSERA_RECREATE_SHARE_PRESERVED). If no member survives, the new context starts an empty group and
 * everything must be re-uploaded. `outFlags` may be NULL.
 */
TESSERA_API TesseraResult TESSERA_CALL tesseraRecreateContext(TesseraContext context, TesseraContext* outNew,
                                                              uint32_t* outFlags);

/*
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
 * Names that do not start with "gl" are always NULL. On X11/GLX a made-up "gl..." name may still resolve to a
 * harmless stub (the GLX loader contract): learn what is supported from the extension strings, not from
 * whether a pointer came back.
 */
TESSERA_API void* TESSERA_CALL tesseraGetProcAddress(const char* name);

/* Presents the back buffer (a no-op for pbuffer surfaces). Like tesseraSetSwapInterval, `context` must be
 * current on the calling thread: that is the one rule every platform (WGL, GLX, EGL) agrees on. */
TESSERA_API TesseraResult TESSERA_CALL tesseraSwapBuffers(TesseraContext context);

/* 0 = no vsync, 1 = vsync, -1 = adaptive (if supported). `context` must be current on the calling thread. */
TESSERA_API TesseraResult TESSERA_CALL tesseraSetSwapInterval(TesseraContext context, int32_t interval);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TESSERA_H */
