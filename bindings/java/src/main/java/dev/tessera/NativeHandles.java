package dev.tessera;

import java.util.Arrays;

/**
 * The operating-system objects behind a {@link Context}, for interoperating with other libraries: sharing a texture with
 * Direct3D ({@code WGL_NV_DX_interop2}), importing an EGL image, handing a {@code GLXContext} to a toolkit, and so on. Tessera
 * does not wrap those APIs; it hands out the objects, and {@link Tessera#procAddress(String)} resolves their extension functions.
 *
 * <p><b>The objects are borrowed.</b> They stay valid until the {@link Context} is closed and belong to Tessera: never destroy,
 * close or terminate them (an {@code EGLDisplay} or X {@code Display*} is shared by every context of the backend). Use them only
 * while the context is open, and respect the usual rule that a context is current on one thread at a time.
 *
 * <p>Pointers are carried as {@code long}. What {@link #get(int)} means depends on {@link #kind()}:
 *
 * <pre>
 * WGL: 0 HGLRC  1 HDC  2 HWND (0 for a pbuffer)  3 HPBUFFERARB (0 for a window)  4 pixel format
 * GLX: 0 Display*  1 GLXContext  2 GLXDrawable  3 GLXFBConfig  4 Window (0 for a pbuffer)  5 screen
 * EGL: 0 EGLDisplay  1 EGLContext  2 EGLSurface (0 if surfaceless)  3 EGLConfig  4 client API (EGL_OPENGL_API / EGL_OPENGL_ES_API)
 * CGL: 0 CGLContextObj
 * </pre>
 *
 * Unused entries are 0.
 */
public final class NativeHandles {

    /** Which windowing API the handles belong to. */
    public enum Kind {
        WGL(1), GLX(2), EGL(3), CGL(4), UNKNOWN(0);

        private final int nativeValue;

        Kind(int nativeValue) {
            this.nativeValue = nativeValue;
        }

        static Kind of(int v) {
            for (Kind k : values()) {
                if (k.nativeValue == v) {
                    return k;
                }
            }
            return UNKNOWN;
        }
    }

    /** Number of entries in the handle array. */
    public static final int COUNT = 6;

    private final Kind kind;
    private final long[] handles;

    NativeHandles(int kind, long[] handles) {
        this.kind = Kind.of(kind);
        this.handles = handles.clone();
    }

    public Kind kind() {
        return kind;
    }

    /** Entry {@code index} (0 to {@link #COUNT} - 1); see the class comment for the meaning per {@link Kind}. */
    public long get(int index) {
        if (index < 0 || index >= COUNT) {
            throw new IndexOutOfBoundsException("index " + index);
        }
        return handles[index];
    }

    /** A copy of all entries. */
    public long[] toArray() {
        return handles.clone();
    }

    /** The native context object: HGLRC, GLXContext, EGLContext or CGLContextObj. */
    public long contextHandle() {
        return kind == Kind.GLX || kind == Kind.EGL ? handles[1] : handles[0];
    }

    @Override
    public boolean equals(Object o) {
        return o instanceof NativeHandles && ((NativeHandles) o).kind == kind && Arrays.equals(((NativeHandles) o).handles, handles);
    }

    @Override
    public int hashCode() {
        return 31 * kind.hashCode() + Arrays.hashCode(handles);
    }

    @Override
    public String toString() {
        final StringBuilder sb = new StringBuilder("NativeHandles{").append(kind);
        for (int i = 0; i < COUNT; i++) {
            sb.append(i == 0 ? ": " : ", ").append("0x").append(Long.toHexString(handles[i]));
        }
        return sb.append('}').toString();
    }
}
