// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
// A context from "another library" that belongs to the OTHER windowing API than the Tessera backend under
// test: an EGL context while Tessera uses GLX, a GLX context while Tessera uses EGL. On Mesa a thread has one
// native current context whichever API made it current, so these are the cases where backends can step on
// each other (a GLFW/SDL host on Wayland uses EGL while Tessera's GLX backend is selected, for example).
// Self-contained: its own dlopen'd entry points, no Tessera internals.
#pragma once

#include <dlfcn.h>

#include <cstdio>

#include "platform/egl/egl_types.hpp"
#include "platform/x11/x11_types.hpp"

namespace foreign {

using namespace tessera;

template <class Fn> inline bool bindSym(void* lib, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(dlsym(lib, name));
    return out != nullptr;
}

struct EglCtx {
    egl::EglFns f;
    egl::EGLDisplay dpy = nullptr;
    egl::EGLContext ctx = nullptr;
    egl::EGLSurface surf = nullptr;

    bool create() {
        void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib)
            return false;
        if (!(bindSym(lib, "eglGetDisplay", f.eglGetDisplay) &&
              bindSym(lib, "eglInitialize", f.eglInitialize) && bindSym(lib, "eglBindAPI", f.eglBindAPI) &&
              bindSym(lib, "eglQueryAPI", f.eglQueryAPI) &&
              bindSym(lib, "eglChooseConfig", f.eglChooseConfig) &&
              bindSym(lib, "eglCreateContext", f.eglCreateContext) &&
              bindSym(lib, "eglCreatePbufferSurface", f.eglCreatePbufferSurface) &&
              bindSym(lib, "eglMakeCurrent", f.eglMakeCurrent) &&
              bindSym(lib, "eglGetCurrentContext", f.eglGetCurrentContext) &&
              bindSym(lib, "eglGetCurrentSurface", f.eglGetCurrentSurface) &&
              bindSym(lib, "eglDestroyContext", f.eglDestroyContext) &&
              bindSym(lib, "eglDestroySurface", f.eglDestroySurface)))
            return false;
        dpy = f.eglGetDisplay(nullptr); // what a toolkit would use
        egl::EGLint major = 0, minor = 0;
        if (!dpy || !f.eglInitialize(dpy, &major, &minor) || !f.eglBindAPI(egl::kOpenGlApi))
            return false;
        const egl::EGLint attribs[] = {egl::kSurfaceType,
                                       egl::kPbufferBit,
                                       egl::kRenderableType,
                                       egl::kOpenGlBit,
                                       egl::kRedSize,
                                       8,
                                       egl::kGreenSize,
                                       8,
                                       egl::kBlueSize,
                                       8,
                                       egl::kNone};
        egl::EGLConfig cfg = nullptr;
        egl::EGLint n = 0;
        if (!f.eglChooseConfig(dpy, attribs, &cfg, 1, &n) || n == 0)
            return false;
        ctx = f.eglCreateContext(dpy, cfg, nullptr, nullptr);
        const egl::EGLint pb[] = {egl::kWidth, 1, egl::kHeight, 1, egl::kNone};
        surf = f.eglCreatePbufferSurface(dpy, cfg, pb);
        return ctx != nullptr && surf != nullptr;
    }
    void makeCurrent() const {
        f.eglBindAPI(egl::kOpenGlApi);
        f.eglMakeCurrent(dpy, surf, surf, ctx);
    }
    // Looks without disturbing: puts back the API binding it had to change.
    bool isCurrent() const {
        const egl::EGLenum before = f.eglQueryAPI();
        f.eglBindAPI(egl::kOpenGlApi);
        const bool r = f.eglGetCurrentContext() == ctx && f.eglGetCurrentSurface(egl::kDraw) == surf;
        f.eglBindAPI(before);
        return r;
    }
    bool nothingCurrent() const {
        const egl::EGLenum before = f.eglQueryAPI();
        f.eglBindAPI(egl::kOpenGlApi);
        const bool r = f.eglGetCurrentContext() == nullptr;
        f.eglBindAPI(before);
        return r;
    }
    void destroy() {
        if (!dpy)
            return;
        f.eglBindAPI(egl::kOpenGlApi);
        if (f.eglGetCurrentContext() == ctx)
            f.eglMakeCurrent(dpy, nullptr, nullptr, nullptr);
        if (ctx)
            f.eglDestroyContext(dpy, ctx);
        if (surf)
            f.eglDestroySurface(dpy, surf);
        ctx = nullptr;
        surf = nullptr;
    }
};

struct GlxCtx {
    x11::X11Fns x;
    x11::GlxFns g;
    x11::Display* dpy = nullptr;
    x11::Window win = 0;
    x11::GLXContext ctx = nullptr;

    bool create() {
        void* x11lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        void* gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!x11lib || !gl)
            return false;
        if (!(bindSym(x11lib, "XOpenDisplay", x.XOpenDisplay) &&
              bindSym(x11lib, "XDefaultScreen", x.XDefaultScreen) &&
              bindSym(x11lib, "XRootWindow", x.XRootWindow) &&
              bindSym(x11lib, "XCreateColormap", x.XCreateColormap) &&
              bindSym(x11lib, "XCreateWindow", x.XCreateWindow) &&
              bindSym(x11lib, "XDestroyWindow", x.XDestroyWindow) && bindSym(x11lib, "XFree", x.XFree) &&
              bindSym(gl, "glXChooseFBConfig", g.glXChooseFBConfig) &&
              bindSym(gl, "glXGetVisualFromFBConfig", g.glXGetVisualFromFBConfig) &&
              bindSym(gl, "glXCreateNewContext", g.glXCreateNewContext) &&
              bindSym(gl, "glXDestroyContext", g.glXDestroyContext) &&
              bindSym(gl, "glXMakeContextCurrent", g.glXMakeContextCurrent) &&
              bindSym(gl, "glXGetCurrentContext", g.glXGetCurrentContext) &&
              bindSym(gl, "glXGetCurrentDrawable", g.glXGetCurrentDrawable)))
            return false;
        dpy = x.XOpenDisplay(nullptr);
        if (!dpy)
            return false;
        const int screen = x.XDefaultScreen(dpy);
        const int attribs[] = {
            x11::kGlxXRenderable, 1, x11::kGlxDrawableType, x11::kGlxWindowBit, x11::kGlxRenderType,
            x11::kGlxRgbaBit,     0};
        int n = 0;
        x11::GLXFBConfig* list = g.glXChooseFBConfig(dpy, screen, attribs, &n);
        if (!list || n == 0)
            return false;
        x11::GLXFBConfig fb = list[0];
        x.XFree(list);
        x11::XVisualInfo* vi = g.glXGetVisualFromFBConfig(dpy, fb);
        if (!vi)
            return false;
        const x11::Window root = x.XRootWindow(dpy, screen);
        x11::XSetWindowAttributes a = {};
        a.colormap = x.XCreateColormap(dpy, root, vi->visual, x11::kAllocNone);
        a.override_redirect = 1;
        win = x.XCreateWindow(dpy, root, 0, 0, 1, 1, 0, vi->depth, x11::kInputOutput, vi->visual,
                              x11::kCWColormap | x11::kCWOverrideRedirect, &a);
        x.XFree(vi);
        ctx = g.glXCreateNewContext(dpy, fb, x11::kGlxRgbaType, nullptr, 1);
        return win != 0 && ctx != nullptr;
    }
    void makeCurrent() const {
        g.glXMakeContextCurrent(dpy, win, win, ctx);
    }
    bool isCurrent() const {
        return g.glXGetCurrentContext() == ctx && g.glXGetCurrentDrawable() == win;
    }
    bool nothingCurrent() const {
        return g.glXGetCurrentContext() == nullptr;
    }
    void destroy() {
        if (!dpy)
            return;
        if (g.glXGetCurrentContext() == ctx)
            g.glXMakeContextCurrent(dpy, 0, 0, nullptr);
        if (ctx)
            g.glXDestroyContext(dpy, ctx);
        if (win)
            x.XDestroyWindow(dpy, win);
        ctx = nullptr;
        win = 0;
    }
};

} // namespace foreign
