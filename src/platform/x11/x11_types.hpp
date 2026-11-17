#pragma once

/*
 * The small slice of Xlib and GLX that the X11 backend needs, declared by hand.
 *
 * Why: Tessera loads libX11 and libGL at run time (like opengl32.dll on Windows), so building it needs no
 * X11/GL development packages, and a machine without them gets a clean "unsupported" error instead of a
 * missing-library failure at startup. The ABI of these structures is fixed by the X protocol libraries; the
 * static_asserts pin the LP64 layouts so a typo fails the build rather than corrupting memory.
 */

#include <cstddef>
#include <cstdint>

namespace tessera::x11 {

struct Display;           // opaque
struct Visual;            // opaque
struct GLXContextRec;     // opaque
struct GLXFBConfigRec;    // opaque

using XID = unsigned long;
using Window = XID;
using Colormap = XID;
using VisualID = unsigned long;
using GLXDrawable = XID;
using GLXPbuffer = XID;
using GLXContext = GLXContextRec*;
using GLXFBConfig = GLXFBConfigRec*;
using Bool = int;

struct XVisualInfo {
    Visual* visual;
    VisualID visualid;
    int screen;
    int depth;
    int c_class;
    unsigned long red_mask;
    unsigned long green_mask;
    unsigned long blue_mask;
    int colormap_size;
    int bits_per_rgb;
};

struct XSetWindowAttributes {
    XID background_pixmap;
    unsigned long background_pixel;
    XID border_pixmap;
    unsigned long border_pixel;
    int bit_gravity;
    int win_gravity;
    int backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    Bool save_under;
    long event_mask;
    long do_not_propagate_mask;
    Bool override_redirect;
    Colormap colormap;
    XID cursor;
};

struct XErrorEvent {
    int type;
    Display* display;
    XID resourceid;
    unsigned long serial;
    unsigned char error_code;
    unsigned char request_code;
    unsigned char minor_code;
};

using XErrorHandler = int (*)(Display*, XErrorEvent*);

#if defined(__LP64__)
static_assert(sizeof(XVisualInfo) == 64, "XVisualInfo layout");
static_assert(sizeof(XSetWindowAttributes) == 112, "XSetWindowAttributes layout");
static_assert(offsetof(XSetWindowAttributes, event_mask) == 72, "XSetWindowAttributes::event_mask offset");
static_assert(offsetof(XSetWindowAttributes, colormap) == 96, "XSetWindowAttributes::colormap offset");
static_assert(sizeof(XErrorEvent) == 40, "XErrorEvent layout");
static_assert(offsetof(XErrorEvent, error_code) == 32, "XErrorEvent::error_code offset");
#endif

// ---- Xlib constants -----------------------------------------------------------------------------
constexpr int kInputOutput = 1;
constexpr int kAllocNone = 0;
constexpr unsigned long kCWBorderPixel = 1UL << 3;
constexpr unsigned long kCWOverrideRedirect = 1UL << 9;
constexpr unsigned long kCWEventMask = 1UL << 11;
constexpr unsigned long kCWColormap = 1UL << 13;
constexpr int kBadAlloc = 11;

// ---- GLX constants ------------------------------------------------------------------------------
constexpr int kGlxDoubleBuffer = 5;
constexpr int kGlxStereo = 6;
constexpr int kGlxRedSize = 8;
constexpr int kGlxGreenSize = 9;
constexpr int kGlxBlueSize = 10;
constexpr int kGlxAlphaSize = 11;
constexpr int kGlxDepthSize = 12;
constexpr int kGlxStencilSize = 13;
constexpr int kGlxXVisualType = 0x22;
constexpr int kGlxTrueColor = 0x8002;
constexpr int kGlxDrawableType = 0x8010;
constexpr int kGlxRenderType = 0x8011;
constexpr int kGlxXRenderable = 0x8012;
constexpr int kGlxRgbaType = 0x8014;
constexpr int kGlxWindowBit = 0x1;
constexpr int kGlxPbufferBit = 0x4;
constexpr int kGlxRgbaBit = 0x1;
constexpr int kGlxRgbaFloatBit = 0x4; // GLX_RGBA_FLOAT_BIT_ARB
constexpr int kGlxSampleBuffers = 100000;
constexpr int kGlxSamples = 100001;
constexpr int kGlxFramebufferSrgb = 0x20B2; // GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB
constexpr int kGlxPbufferHeight = 0x8040;
constexpr int kGlxPbufferWidth = 0x8041;
constexpr int kGlxPreservedContents = 0x801B;

// ---- function pointer types ---------------------------------------------------------------------
struct X11Fns {
    Display* (*XOpenDisplay)(const char*) = nullptr;
    int (*XCloseDisplay)(Display*) = nullptr;
    int (*XInitThreads)() = nullptr;
    int (*XDefaultScreen)(Display*) = nullptr;
    Window (*XRootWindow)(Display*, int) = nullptr;
    Colormap (*XCreateColormap)(Display*, Window, Visual*, int) = nullptr;
    int (*XFreeColormap)(Display*, Colormap) = nullptr;
    Window (*XCreateWindow)(Display*, Window, int, int, unsigned, unsigned, unsigned, int, unsigned, Visual*,
                            unsigned long, XSetWindowAttributes*) = nullptr;
    int (*XDestroyWindow)(Display*, Window) = nullptr;
    int (*XFree)(void*) = nullptr;
    int (*XSync)(Display*, Bool) = nullptr;
    XErrorHandler (*XSetErrorHandler)(XErrorHandler) = nullptr;
};

struct GlxFns {
    Bool (*glXQueryExtension)(Display*, int*, int*) = nullptr;
    Bool (*glXQueryVersion)(Display*, int*, int*) = nullptr;
    const char* (*glXQueryExtensionsString)(Display*, int) = nullptr;
    GLXFBConfig* (*glXChooseFBConfig)(Display*, int, const int*, int*) = nullptr;
    int (*glXGetFBConfigAttrib)(Display*, GLXFBConfig, int, int*) = nullptr;
    XVisualInfo* (*glXGetVisualFromFBConfig)(Display*, GLXFBConfig) = nullptr;
    GLXContext (*glXCreateNewContext)(Display*, GLXFBConfig, int, GLXContext, Bool) = nullptr;
    void (*glXDestroyContext)(Display*, GLXContext) = nullptr;
    Bool (*glXMakeContextCurrent)(Display*, GLXDrawable, GLXDrawable, GLXContext) = nullptr;
    GLXContext (*glXGetCurrentContext)() = nullptr;
    Display* (*glXGetCurrentDisplay)() = nullptr;
    GLXDrawable (*glXGetCurrentDrawable)() = nullptr;
    GLXDrawable (*glXGetCurrentReadDrawable)() = nullptr;
    void (*glXSwapBuffers)(Display*, GLXDrawable) = nullptr;
    void* (*glXGetProcAddress)(const unsigned char*) = nullptr; // glXGetProcAddressARB
    GLXPbuffer (*glXCreatePbuffer)(Display*, GLXFBConfig, const int*) = nullptr;
    void (*glXDestroyPbuffer)(Display*, GLXPbuffer) = nullptr;
};

} // namespace tessera::x11
