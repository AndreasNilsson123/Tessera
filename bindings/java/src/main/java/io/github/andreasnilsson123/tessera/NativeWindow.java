// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

/**
 * A window that belongs to another toolkit, for {@link Context#createForNativeWindow}. Tessera only borrows it: it never
 * destroys, moves, shows or hides it, and the window must outlive the context.
 */
public final class NativeWindow {
    /** The window system a handle belongs to. */
    public enum Kind {
        WIN32(1), X11(2), WAYLAND(3), COCOA(4);
        final int nativeValue;

        Kind(int v) {
            nativeValue = v;
        }
    }

    private final Kind kind;
    private final long window;
    private final long display;
    private final int width;
    private final int height;

    private NativeWindow(Kind kind, long window, long display, int width, int height) {
        this.kind = kind;
        this.window = window;
        this.display = display;
        this.width = width;
        this.height = height;
    }

    /** A Windows {@code HWND}. */
    public static NativeWindow win32(long hwnd) {
        return new NativeWindow(Kind.WIN32, hwnd, 0L, 0, 0);
    }

    /** An X11 {@code Window} id on the server of {@code $DISPLAY} (Tessera uses its own connection to it). */
    public static NativeWindow x11(long window) {
        return new NativeWindow(Kind.X11, window, 0L, 0, 0);
    }

    /**
     * A Wayland surface: the {@code wl_surface*}, the {@code wl_display*} it belongs to and the size of its buffer in pixels.
     * Tessera wraps the surface in a {@code wl_egl_window} that it owns; you keep dispatching your own event queue, and call
     * {@link Context#resizeWindow} when the window changes size. A swap with vsync on only completes once the compositor
     * has shown the surface (a surface without a role never receives the frame callback), so {@code setSwapInterval(0)} for
     * windows that are not mapped yet.
     */
    public static NativeWindow wayland(long wlDisplay, long wlSurface, int width, int height) {
        return new NativeWindow(Kind.WAYLAND, wlSurface, wlDisplay, width, height);
    }

    /** An {@code NSView*}. Not implemented: {@link Context#createForNativeWindow} fails with {@link Result#UNSUPPORTED}. */
    public static NativeWindow cocoa(long nsView) {
        return new NativeWindow(Kind.COCOA, nsView, 0L, 0, 0);
    }

    public Kind kind() {
        return kind;
    }

    public long window() {
        return window;
    }

    public long display() {
        return display;
    }

    public int width() {
        return width;
    }

    public int height() {
        return height;
    }

    @Override
    public String toString() {
        return "NativeWindow{" + kind + ", 0x" + Long.toHexString(window) + (display != 0L ? ", display 0x" + Long.toHexString(display) : "")
                + (width > 0 ? ", " + width + "x" + height : "") + "}";
    }
}
