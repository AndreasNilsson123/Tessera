// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera.lwjgl;

import io.github.andreasnilsson123.tessera.Context;
import io.github.andreasnilsson123.tessera.ContextConfig;
import io.github.andreasnilsson123.tessera.ContextInfo;
import io.github.andreasnilsson123.tessera.Result;
import io.github.andreasnilsson123.tessera.TesseraException;
import org.lwjgl.BufferUtils;
import org.lwjgl.glfw.GLFW;
import org.lwjgl.glfw.GLFWErrorCallback;
import org.lwjgl.glfw.GLFWNativeWin32;
import org.lwjgl.glfw.GLFWNativeX11;
import org.lwjgl.opengl.GL;
import org.lwjgl.opengl.GL11C;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Tessera next to GLFW in one process: GLFW (through LWJGL) owns a window and an OpenGL context of its own, Tessera creates,
 * uses and destroys contexts around it, on this thread and on others, and renders into a GLFW-created window it does not own.
 * What is checked:
 * <ul>
 *   <li>creating and closing Tessera contexts never changes the context current on the GLFW thread (the native objects are
 *       compared, not GLFW's bookkeeping);</li>
 *   <li>GLFW keeps rendering correct pixels while three threads churn Tessera contexts;</li>
 *   <li>{@link Context#createForWindow} renders into a {@code GLFW_NO_API} window and leaves the window alone;</li>
 *   <li>GLFW can be terminated while a Tessera context is still current on another thread.</li>
 * </ul>
 * Needs a display and an OpenGL driver. Exit code 0 = passed, 77 = skipped (GLFW cannot open a window here).
 */
public final class GlfwCoexistence {
    private static int failures;
    private static final boolean WAYLAND = !System.getProperty("os.name", "").toLowerCase().contains("win")
            && System.getProperty("glfw.platform", "x11").equals("wayland");
    private static final boolean WINDOWS = System.getProperty("os.name", "").toLowerCase().contains("win");

    private static void check(boolean condition, String what) {
        if (!condition) {
            System.err.println("FAILED: " + what);
            failures++;
        }
    }

    private static ContextConfig plain() {
        return ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).build();
    }

    /** The native context current on this thread, as the platform reports it (not GLFW's own bookkeeping). */
    private static long nativeCurrent() {
        if (WAYLAND) {
            return 0L; // GLFW's Wayland platform uses EGL; LWJGL's EGL binding is not part of the test dependencies
        }
        return WINDOWS ? org.lwjgl.opengl.WGL.wglGetCurrentContext() : org.lwjgl.opengl.GLX.glXGetCurrentContext();
    }

    /** Clears the current framebuffer and reads one pixel back: {r, g, b, a}. */
    private static int[] clearAndRead(float r, float g, float b) {
        GL11C.glClearColor(r, g, b, 1f);
        GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
        final ByteBuffer px = BufferUtils.createByteBuffer(4);
        GL11C.glReadPixels(0, 0, 1, 1, GL11C.GL_RGBA, GL11C.GL_UNSIGNED_BYTE, px);
        return new int[] {px.get(0) & 0xFF, px.get(1) & 0xFF, px.get(2) & 0xFF, px.get(3) & 0xFF};
    }

    private static boolean is(int[] px, int r, int g, int b) {
        return Math.abs(px[0] - r) <= 1 && Math.abs(px[1] - g) <= 1 && Math.abs(px[2] - b) <= 1;
    }

    public static void main(String[] args) throws Exception {
        System.out.println("GLFW + Tessera (" + io.github.andreasnilsson123.tessera.Tessera.binding() + ", GLFW " + GLFW.glfwGetVersionString() + ")");
        TesseraGL.install(); // LWJGL resolves OpenGL through Tessera; GLFW's context needs nothing else from it
        GLFWErrorCallback.createPrint(System.err).set();
        // Linux: GLFW picks Wayland (EGL contexts) over X11 (GLX contexts) when it can. -Dglfw.platform=x11|wayland chooses; the
        // default is X11, which is what the Xvfb-based CI offers, and the native comparison below follows the choice.
        final String platform = System.getProperty("glfw.platform", "x11");
        if (!WINDOWS) {
            GLFW.glfwInitHint(GLFW.GLFW_PLATFORM, platform.equals("wayland") ? GLFW.GLFW_PLATFORM_WAYLAND : GLFW.GLFW_PLATFORM_X11);
        }
        if (!GLFW.glfwInit()) {
            System.out.println("GLFW could not initialise here (no display?): skipping");
            System.exit(77);
        }
        GLFW.glfwDefaultWindowHints();
        GLFW.glfwWindowHint(GLFW.GLFW_VISIBLE, GLFW.GLFW_FALSE);
        final long window = GLFW.glfwCreateWindow(64, 64, "glfw", 0L, 0L);
        if (window == 0L) {
            System.out.println("GLFW could not create a window with an OpenGL context here: skipping");
            GLFW.glfwTerminate();
            System.exit(77);
        }
        GLFW.glfwMakeContextCurrent(window);
        GL.createCapabilities();
        final long glfwContext = nativeCurrent();
        check(WAYLAND || glfwContext != 0L, "GLFW's context is current natively");
        System.out.println("  GLFW context: " + GL11C.glGetString(GL11C.GL_RENDERER));

        System.out.println("guest guarantees");
        try (Context c = Context.create(plain())) {
            check(nativeCurrent() == glfwContext, "creating a Tessera context leaves GLFW's context current");
            check(GLFW.glfwGetCurrentContext() == window, "GLFW still believes its window is current");
            final ContextInfo info = c.info();
            check(info.versionMajor() >= 1, "the Tessera context is real: " + info);
            check(nativeCurrent() == glfwContext, "describing it leaves GLFW's context current");
        }
        check(nativeCurrent() == glfwContext, "closing a Tessera context leaves GLFW's context current");
        check(is(clearAndRead(1f, 0f, 0f), 255, 0, 0), "GLFW still renders after all of that");

        System.out.println("two contexts, two framebuffers");
        try (Context c = Context.create(plain())) {
            c.makeCurrent(); // replaces GLFW's context on this thread (documented), GLFW's is simply not current for a moment
            GL.createCapabilities();
            check(is(clearAndRead(0f, 1f, 0f), 0, 255, 0), "Tessera renders green");
            Context.releaseCurrent();
        }
        GLFW.glfwMakeContextCurrent(window);
        GL.createCapabilities();
        check(is(clearAndRead(0f, 0f, 1f), 0, 0, 255), "GLFW renders blue afterwards");

        System.out.println("GLFW renders while three threads churn Tessera contexts");
        final AtomicBoolean running = new AtomicBoolean(true);
        final AtomicLong churned = new AtomicLong();
        final AtomicReference<Throwable> churnFailure = new AtomicReference<>();
        final List<Thread> threads = new ArrayList<>();
        for (int i = 0; i < 3; i++) {
            final Thread t = new Thread(() -> {
                try {
                    org.lwjgl.opengl.GLCapabilities caps = null; // function pointers are per driver: build once per thread
                    while (running.get()) {
                        try (Context c = Context.create(plain())) {
                            c.makeCurrent();
                            if (caps == null) {
                                caps = GL.createCapabilities();
                            } else {
                                GL.setCapabilities(caps);
                            }
                            if (!is(clearAndRead(0.5f, 0.25f, 0.75f), 128, 64, 191)) {
                                churnFailure.compareAndSet(null, new IllegalStateException("wrong pixel on a churn context"));
                            }
                            Context.releaseCurrent();
                        }
                        churned.incrementAndGet();
                        Thread.sleep(2);
                    }
                } catch (Throwable e) {
                    churnFailure.compareAndSet(null, e);
                }
            }, "tessera-churn-" + i);
            threads.add(t);
            t.start();
        }
        final long end = System.nanoTime() + 3_000_000_000L;
        long frames = 0;
        long wrong = 0;
        long lostContext = 0;
        while (System.nanoTime() < end) {
            final float v = (frames % 2 == 0) ? 1f : 0f;
            final int[] px = clearAndRead(v, 1f - v, 0.5f);
            if (!is(px, Math.round(v * 255), Math.round((1f - v) * 255), 128)) {
                wrong++;
            }
            if (nativeCurrent() != glfwContext) {
                lostContext++;
            }
            GLFW.glfwSwapBuffers(window);
            GLFW.glfwPollEvents();
            frames++;
        }
        running.set(false);
        for (Thread t : threads) {
            t.join();
        }
        System.out.println("  " + frames + " GLFW frames (" + wrong + " wrong, GLFW's context lost " + lostContext + " times), "
                + churned.get() + " Tessera contexts churned");
        check(churnFailure.get() == null, "churn threads: " + churnFailure.get());
        check(wrong == 0, wrong + " GLFW frames had wrong pixels");
        check(lostContext == 0, "GLFW's native context stopped being current " + lostContext + " times");
        check(frames > 30, "GLFW barely ran (" + frames + " frames)");
        check(churned.get() > 10, "churn made little progress (" + churned.get() + ")");

        System.out.println("Tessera renders into a GLFW-created window");
        attachToGlfwWindow();
        // A Tessera context made current on this thread replaced GLFW's (documented): hand the thread back to GLFW.
        GLFW.glfwMakeContextCurrent(window);

        System.out.println("GLFW is terminated while a Tessera context is current on another thread");
        final Context orphan = Context.create(plain());
        final AtomicBoolean held = new AtomicBoolean();
        final AtomicBoolean release = new AtomicBoolean();
        final AtomicReference<Throwable> orphanFailure = new AtomicReference<>();
        final Thread holder = new Thread(() -> {
            try {
                orphan.makeCurrent();
                held.set(true);
                while (!release.get()) {
                    Thread.sleep(1);
                }
                Context.releaseCurrent();
            } catch (Throwable e) {
                orphanFailure.set(e);
                held.set(true);
            }
        }, "tessera-holder");
        holder.start();
        while (!held.get()) {
            Thread.sleep(1);
        }
        GLFW.glfwMakeContextCurrent(0L);
        GLFW.glfwDestroyWindow(window);
        GLFW.glfwTerminate();
        release.set(true);
        holder.join();
        check(orphanFailure.get() == null, "the holder thread: " + orphanFailure.get());
        orphan.close();
        check(Context.current() == null, "nothing left current");

        System.out.println(failures == 0 ? "GlfwCoexistence: OK" : "GlfwCoexistence: " + failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }

    private static void attachToGlfwWindow() {
        GLFW.glfwDefaultWindowHints();
        GLFW.glfwWindowHint(GLFW.GLFW_CLIENT_API, GLFW.GLFW_NO_API); // GLFW creates the window and nothing GL-related for it
        GLFW.glfwWindowHint(GLFW.GLFW_VISIBLE, GLFW.GLFW_FALSE);
        final long win = GLFW.glfwCreateWindow(64, 64, "attach", 0L, 0L);
        if (win == 0L) {
            System.out.println("  (GLFW cannot create a window without a client API here: skipped)");
            return;
        }
        final int[] sizeBefore = new int[2];
        GLFW.glfwGetWindowSize(win, new int[1], new int[1]);
        {
            final int[] w0 = new int[1];
            final int[] h0 = new int[1];
            GLFW.glfwGetWindowSize(win, w0, h0);
            sizeBefore[0] = w0[0];
            sizeBefore[1] = h0[0];
        }
        if (WAYLAND) {
            System.out.println("  (a Wayland window has no X11/Win32 id: attach is not applicable)");
            GLFW.glfwDestroyWindow(win);
            return;
        }
        final long id = WINDOWS ? GLFWNativeWin32.glfwGetWin32Window(win) : GLFWNativeX11.glfwGetX11Window(win);
        check(id != 0L, "GLFW exposes the native window");

        if (WINDOWS) {
            // The window has no pixel format, and a Windows window's can be set only once: refused unless we say it is fresh.
            final TesseraException refused = expectFailure(() -> Context.createForWindow(plain(), id, false).close());
            check(refused != null && refused.result() == Result.INVALID_ARGUMENT, "a window without a pixel format is refused: " + refused);
        }
        Context ctx;
        try {
            ctx = Context.createForWindow(plain(), id, true);
        } catch (TesseraException e) {
            if (!WINDOWS && e.result() == Result.NO_MATCHING_CONFIG) {
                System.out.println("  (this X11 window's visual has no GLX framebuffer configuration: " + e.getMessage() + ")");
                GLFW.glfwDestroyWindow(win);
                return;
            }
            check(false, "createForWindow on a GLFW window: " + e);
            GLFW.glfwDestroyWindow(win);
            return;
        }
        try {
            final ContextInfo info = ctx.info();
            check(info.surface() == ContextConfig.Surface.WINDOW, "reported as a window context: " + info.surface());
            check(ctx.nativeHandles().get(WINDOWS ? 2 : 4) == id, "the native handles name GLFW's window");
            ctx.makeCurrent();
            GL.createCapabilities();
            check(is(clearAndRead(1f, 0.5f, 0f), 255, 128, 0), "rendered into the GLFW window's back buffer");
            ctx.swapBuffers();
            Context.releaseCurrent();
        } finally {
            ctx.close();
        }
        // The window is still GLFW's: alive, usable, and can be attached again (its pixel format is now adopted).
        check(!GLFW.glfwWindowShouldClose(win), "the window survived the context");
        final int[] w = new int[1];
        final int[] h = new int[1];
        GLFW.glfwGetWindowSize(win, w, h);
        check(w[0] == sizeBefore[0] && h[0] == sizeBefore[1], "the window was not resized: " + w[0] + "x" + h[0]);
        try (Context again = Context.createForWindow(plain(), id, false)) {
            again.makeCurrent();
            GL.createCapabilities();
            check(is(clearAndRead(0f, 1f, 1f), 0, 255, 255), "attached a second time");
            Context.releaseCurrent();
        } catch (TesseraException e) {
            // Linux: nothing is adopted or refused for X11; either way it must not be a crash. Windows must succeed.
            check(!WINDOWS, "re-attaching to a window that now has a pixel format: " + e);
        }
        GLFW.glfwDestroyWindow(win);
    }

    private static TesseraException expectFailure(Runnable body) {
        try {
            body.run();
        } catch (TesseraException e) {
            return e;
        }
        return null;
    }
}
