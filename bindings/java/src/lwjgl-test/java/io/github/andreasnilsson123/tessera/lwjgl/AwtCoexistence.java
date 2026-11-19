// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera.lwjgl;

import io.github.andreasnilsson123.tessera.Context;
import io.github.andreasnilsson123.tessera.ContextConfig;
import org.lwjgl.BufferUtils;
import org.lwjgl.opengl.GL;
import org.lwjgl.opengl.GL11C;
import org.lwjgl.opengl.GLCapabilities;

import javax.swing.JFrame;
import javax.swing.JPanel;
import javax.swing.SwingUtilities;
import java.awt.Color;
import java.awt.Graphics;
import java.awt.GraphicsConfiguration;
import java.awt.GraphicsEnvironment;
import java.awt.image.VolatileImage;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Tessera next to Java2D's OpenGL pipeline ({@code -Dsun.java2d.opengl=True}): AWT/Swing then owns WGL (Windows) or GLX (Linux)
 * contexts of its own on its render-queue thread, in the same process as Tessera's. Swing paints continuously into a window and
 * into a {@link VolatileImage} (an accelerated surface whose pixels are read back and compared every round) while a render thread
 * and three churn threads create, use and destroy Tessera contexts. Anything one side does to the other's current context shows
 * up as wrong pixels, exceptions or a stalled pipeline.
 *
 * Skipped (exit 77) where the OpenGL pipeline did not come up: no display, or a driver Java2D does not accept (it checks the
 * GL version and extensions, so software renderers are often refused). The test prints which pipeline it is running on.
 */
public final class AwtCoexistence {
    private static int failures;

    private static void check(boolean condition, String what) {
        if (!condition) {
            System.err.println("FAILED: " + what);
            failures++;
        }
    }

    private static ContextConfig plain() {
        return ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).build();
    }

    private static boolean near(ByteBuffer px, int r, int g, int b) {
        return Math.abs((px.get(0) & 0xFF) - r) <= 1 && Math.abs((px.get(1) & 0xFF) - g) <= 1 && Math.abs((px.get(2) & 0xFF) - b) <= 1;
    }

    public static void main(String[] args) throws Exception {
        System.out.println("AWT OpenGL pipeline + Tessera (" + io.github.andreasnilsson123.tessera.Tessera.binding() + ", Java " + Runtime.version() + ")");
        if (GraphicsEnvironment.isHeadless()) {
            System.out.println("no display: skipping");
            System.exit(77);
        }
        final GraphicsConfiguration gc = GraphicsEnvironment.getLocalGraphicsEnvironment().getDefaultScreenDevice().getDefaultConfiguration();
        final String pipeline = gc.getClass().getName();
        System.out.println("  default graphics configuration: " + pipeline);
        if (!pipeline.contains("WGL") && !pipeline.contains("GLX") && !pipeline.contains("OGL")) {
            System.out.println("Java2D's OpenGL pipeline is not active here (the driver was not accepted): skipping");
            System.exit(77);
        }
        TesseraGL.install();

        final AtomicLong paints = new AtomicLong();
        final AtomicLong volatileRounds = new AtomicLong();
        final AtomicLong volatileWrong = new AtomicLong();
        final VolatileImage[] vi = new VolatileImage[1];
        final JFrame[] frame = new JFrame[1];
        SwingUtilities.invokeAndWait(() -> {
            frame[0] = new JFrame("tessera + java2d opengl");
            frame[0].setUndecorated(true);
            final JPanel panel = new JPanel() {
                @Override
                protected void paintComponent(Graphics g) {
                    super.paintComponent(g);
                    final long n = paints.incrementAndGet();
                    g.setColor(new Color((int) (n * 7 % 256), (int) (n * 13 % 256), 128));
                    g.fillRect(0, 0, getWidth(), getHeight());
                    g.setColor(Color.WHITE);
                    g.drawOval(8, 8, getWidth() - 16, getHeight() - 16);
                }
            };
            frame[0].add(panel);
            frame[0].setSize(160, 160);
            frame[0].setLocation(40, 40);
            frame[0].setVisible(true);
            vi[0] = frame[0].createVolatileImage(32, 32);
        });

        final AtomicBoolean running = new AtomicBoolean(true);
        final AtomicLong tesseraFrames = new AtomicLong();
        final AtomicLong tesseraWrong = new AtomicLong();
        final AtomicLong churned = new AtomicLong();
        final AtomicReference<Throwable> failure = new AtomicReference<>();
        final List<Thread> threads = new ArrayList<>();

        threads.add(new Thread(() -> {
            try (Context ctx = Context.create(plain())) {
                ctx.makeCurrent();
                final GLCapabilities caps = GL.createCapabilities();
                final ByteBuffer px = BufferUtils.createByteBuffer(4);
                long n = 0;
                while (running.get()) {
                    final float v = (n++ % 2 == 0) ? 1f : 0f;
                    GL11C.glClearColor(v, 1f - v, 0.5f, 1f);
                    GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
                    GL11C.glReadPixels(0, 0, 1, 1, GL11C.GL_RGBA, GL11C.GL_UNSIGNED_BYTE, px);
                    if (!near(px, Math.round(v * 255), Math.round((1f - v) * 255), 128)) {
                        tesseraWrong.incrementAndGet();
                    }
                    tesseraFrames.incrementAndGet();
                    Thread.sleep(2);
                }
                check(caps != null, "capabilities");
                Context.releaseCurrent();
            } catch (Throwable t) {
                failure.compareAndSet(null, t);
            }
        }, "tessera-render"));
        for (int i = 0; i < 3; i++) {
            threads.add(new Thread(() -> {
                try {
                    GLCapabilities caps = null;
                    while (running.get()) {
                        try (Context c = Context.create(plain())) {
                            c.makeCurrent();
                            if (caps == null) {
                                caps = GL.createCapabilities();
                            } else {
                                GL.setCapabilities(caps);
                            }
                            GL11C.glClearColor(0.25f, 0.5f, 0.75f, 1f);
                            GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
                            GL11C.glFinish();
                            Context.releaseCurrent();
                        }
                        churned.incrementAndGet();
                        Thread.sleep(2);
                    }
                } catch (Throwable t) {
                    failure.compareAndSet(null, t);
                }
            }, "tessera-churn-" + i));
        }
        for (Thread t : threads) {
            t.start();
        }

        // Java2D's side: keep repainting the window, and fill/read back an accelerated image on the EDT.
        final long end = System.nanoTime() + 4_000_000_000L;
        while (System.nanoTime() < end) {
            final int round = (int) volatileRounds.get();
            SwingUtilities.invokeAndWait(() -> {
                frame[0].repaint();
                final Color expected = (round % 2 == 0) ? Color.RED : Color.BLUE;
                do {
                    if (vi[0].validate(frame[0].getGraphicsConfiguration()) == VolatileImage.IMAGE_INCOMPATIBLE) {
                        vi[0] = frame[0].createVolatileImage(32, 32);
                    }
                    final Graphics g = vi[0].getGraphics();
                    g.setColor(expected);
                    g.fillRect(0, 0, 32, 32);
                    g.dispose();
                } while (vi[0].contentsLost());
                if (vi[0].getSnapshot().getRGB(16, 16) != (expected.getRGB() | 0xFF000000)) {
                    volatileWrong.incrementAndGet();
                }
            });
            volatileRounds.incrementAndGet();
            Thread.sleep(10);
        }
        running.set(false);
        for (Thread t : threads) {
            t.join();
        }
        SwingUtilities.invokeAndWait(() -> frame[0].dispose());

        System.out.println("  Java2D: " + paints.get() + " paints, " + volatileRounds.get() + " accelerated-image rounds ("
                + volatileWrong.get() + " wrong); Tessera: " + tesseraFrames.get() + " frames (" + tesseraWrong.get() + " wrong), "
                + churned.get() + " contexts churned");
        check(failure.get() == null, "a Tessera thread failed: " + failure.get());
        check(tesseraWrong.get() == 0, tesseraWrong.get() + " Tessera frames had wrong pixels");
        check(volatileWrong.get() == 0, volatileWrong.get() + " Java2D accelerated-image reads were wrong");
        check(paints.get() > 20, "Java2D barely painted (" + paints.get() + ")");
        check(volatileRounds.get() > 30, "Java2D's rounds stalled (" + volatileRounds.get() + ")");
        check(tesseraFrames.get() > 20, "the Tessera render thread barely ran (" + tesseraFrames.get() + ")");
        check(churned.get() > 10, "churn made little progress (" + churned.get() + ")");
        System.out.println(failures == 0 ? "AwtCoexistence: OK" : "AwtCoexistence: " + failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
