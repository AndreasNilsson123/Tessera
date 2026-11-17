package dev.tessera.javafx;

import dev.tessera.Context;
import dev.tessera.ContextConfig;
import dev.tessera.TesseraException;
import dev.tessera.lwjgl.TesseraGL;
import javafx.animation.AnimationTimer;
import javafx.application.Platform;
import javafx.scene.Scene;
import javafx.scene.image.ImageView;
import javafx.scene.image.PixelFormat;
import javafx.scene.image.WritableImage;
import javafx.scene.layout.StackPane;
import javafx.stage.Stage;
import org.lwjgl.BufferUtils;
import org.lwjgl.opengl.GL11C;
import org.lwjgl.opengl.GL20C;
import org.lwjgl.opengl.GL30C;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Tessera and JavaFX in one JVM, both drawing at the same time: a JavaFX window animates while a background thread renders
 * with LWJGL through a Tessera context, reads every frame back, verifies the pixels, and hands the image to the FX thread.
 * More threads create and destroy contexts in a loop for the whole run. Run it once per JavaFX pipeline
 * ({@code -Dprism.order=d3d} and {@code -Dprism.order=es2}, the latter being JavaFX's own WGL/GLX context) to cover both a
 * GL and a non-GL host. Exit code 0 = everything held.
 *
 * <p>Opens a small real window for a couple of seconds.
 */
public final class FxCoexistence {
    private static final int SIZE = 128;
    private static final int FRAMES = 240;

    private static final List<String> problems = new CopyOnWriteArrayList<>();
    private static final AtomicInteger fxPulses = new AtomicInteger();
    private static final AtomicInteger imagesShown = new AtomicInteger();
    private static final AtomicInteger framesRendered = new AtomicInteger();
    private static final AtomicInteger framesWrong = new AtomicInteger();
    private static final AtomicLong churnIterations = new AtomicLong();
    private static final AtomicBoolean imagePending = new AtomicBoolean();

    private static void problem(String text) {
        problems.add(text);
        System.err.println("PROBLEM: " + text);
    }

    private static ContextConfig renderConfig() {
        return ContextConfig.builder().profile(ContextConfig.Profile.CORE).version(3, 3).build();
    }

    private static Context createRenderContext() {
        try {
            return Context.create(renderConfig());
        } catch (TesseraException e) {
            return Context.create(ContextConfig.builder().version(3, 3).fallback(ContextConfig.Fallback.BEST_EFFORT).build());
        }
    }

    /** The render thread: LWJGL draws a changing colour with a shader into an FBO, we read it back and check it. */
    private static void renderLoop(WritableImage image) {
        try (Context ctx = createRenderContext()) {
            ctx.makeCurrent();
            TesseraGL.createCapabilities();

            final int fbo = GL30C.glGenFramebuffers();
            final int rbo = GL30C.glGenRenderbuffers();
            GL30C.glBindRenderbuffer(GL30C.GL_RENDERBUFFER, rbo);
            GL30C.glRenderbufferStorage(GL30C.GL_RENDERBUFFER, GL11C.GL_RGBA8, SIZE, SIZE);
            GL30C.glBindFramebuffer(GL30C.GL_FRAMEBUFFER, fbo);
            GL30C.glFramebufferRenderbuffer(GL30C.GL_FRAMEBUFFER, GL30C.GL_COLOR_ATTACHMENT0, GL30C.GL_RENDERBUFFER, rbo);
            GL11C.glViewport(0, 0, SIZE, SIZE);

            final int vs = GL20C.glCreateShader(GL20C.GL_VERTEX_SHADER);
            GL20C.glShaderSource(vs, "#version 330 core\nvoid main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }");
            GL20C.glCompileShader(vs);
            final int fs = GL20C.glCreateShader(GL20C.GL_FRAGMENT_SHADER);
            GL20C.glShaderSource(fs, "#version 330 core\nuniform vec4 color;\nout vec4 c;\nvoid main(){ c = color; }");
            GL20C.glCompileShader(fs);
            final int program = GL20C.glCreateProgram();
            GL20C.glAttachShader(program, vs);
            GL20C.glAttachShader(program, fs);
            GL20C.glLinkProgram(program);
            if (GL20C.glGetProgrami(program, GL20C.GL_LINK_STATUS) != GL11C.GL_TRUE) {
                problem("shader link failed: " + GL20C.glGetProgramInfoLog(program));
                return;
            }
            final int colorLocation = GL20C.glGetUniformLocation(program, "color");
            final int vao = GL30C.glGenVertexArrays();
            GL30C.glBindVertexArray(vao);
            GL20C.glUseProgram(program);

            final ByteBuffer pixels = BufferUtils.createByteBuffer(SIZE * SIZE * 4);
            for (int frame = 0; frame < FRAMES; frame++) {
                final int r = (frame * 7) & 0xFF;
                final int g = 128;
                final int b = 255 - r;
                GL20C.glUniform4f(colorLocation, r / 255f, g / 255f, b / 255f, 1f);
                GL11C.glDrawArrays(GL11C.GL_TRIANGLES, 0, 3);
                GL11C.glReadPixels(0, 0, SIZE, SIZE, GL11C.GL_RGBA, GL11C.GL_UNSIGNED_BYTE, pixels);

                // Every pixel must be the colour we drew (checked at the corners and the middle).
                boolean ok = true;
                for (int index : new int[] {0, SIZE / 2 * SIZE + SIZE / 2, SIZE * SIZE - 1}) {
                    ok &= Math.abs((pixels.get(index * 4) & 0xFF) - r) <= 1 && Math.abs((pixels.get(index * 4 + 1) & 0xFF) - g) <= 1
                            && Math.abs((pixels.get(index * 4 + 2) & 0xFF) - b) <= 1;
                }
                if (!ok) {
                    framesWrong.incrementAndGet();
                }
                final int error = GL11C.glGetError();
                if (error != GL11C.GL_NO_ERROR) {
                    problem("GL error 0x" + Integer.toHexString(error) + " in frame " + frame);
                }
                framesRendered.incrementAndGet();

                // Hand the latest frame to JavaFX (dropping frames the FX thread has not consumed yet).
                if (imagePending.compareAndSet(false, true)) {
                    final byte[] bgra = new byte[SIZE * SIZE * 4];
                    for (int i = 0; i < SIZE * SIZE; i++) {
                        bgra[i * 4] = pixels.get(i * 4 + 2);
                        bgra[i * 4 + 1] = pixels.get(i * 4 + 1);
                        bgra[i * 4 + 2] = pixels.get(i * 4);
                        bgra[i * 4 + 3] = (byte) 0xFF;
                    }
                    Platform.runLater(() -> {
                        image.getPixelWriter().setPixels(0, 0, SIZE, SIZE, PixelFormat.getByteBgraPreInstance(), bgra, 0, SIZE * 4);
                        imagesShown.incrementAndGet();
                        imagePending.set(false);
                    });
                }
                Thread.sleep(8);
            }
            GL20C.glUseProgram(0);
            Context.releaseCurrent();
        } catch (Throwable t) {
            problem("render thread failed: " + t);
            t.printStackTrace();
        }
    }

    /** Creates, uses and destroys contexts as fast as it can: Tessera's process-wide side effects must not disturb JavaFX. */
    private static void churn(AtomicBoolean running) {
        try {
            while (running.get()) {
                try (Context c = Context.create(ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).build())) {
                    c.makeCurrent();
                    c.swapBuffers();
                    Context.releaseCurrent();
                }
                churnIterations.incrementAndGet();
                Thread.sleep(5);
            }
        } catch (Throwable t) {
            problem("churn thread failed: " + t);
        }
    }

    public static void main(String[] args) throws Exception {
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> problem("uncaught on " + thread.getName() + ": " + error));
        final String pipeline = System.getProperty("prism.order", "(default)");
        System.out.println("FxCoexistence: JavaFX pipeline order " + pipeline + ", Tessera binding " + dev.tessera.Tessera.binding()
                + ", Java " + Runtime.version());

        final CountDownLatch shown = new CountDownLatch(1);
        final WritableImage image = new WritableImage(SIZE, SIZE);
        final Stage[] stage = new Stage[1];
        Platform.startup(() -> {
            stage[0] = new Stage();
            stage[0].setTitle("Tessera + JavaFX");
            final ImageView view = new ImageView(image);
            final StackPane root = new StackPane(view);
            stage[0].setScene(new Scene(root, SIZE + 40, SIZE + 40));
            new AnimationTimer() {
                @Override
                public void handle(long now) {
                    fxPulses.incrementAndGet();
                }
            }.start();
            stage[0].show();
            shown.countDown();
        });
        if (!shown.await(30, TimeUnit.SECONDS)) {
            problem("the JavaFX window did not open");
            System.exit(1);
        }

        final AtomicBoolean running = new AtomicBoolean(true);
        final List<Thread> threads = new ArrayList<>();
        final Thread render = new Thread(() -> renderLoop(image), "tessera-render");
        threads.add(render);
        for (int i = 0; i < 3; i++) {
            threads.add(new Thread(() -> churn(running), "tessera-churn-" + i));
        }
        final long start = System.nanoTime();
        for (Thread t : threads) {
            t.start();
        }
        render.join();
        running.set(false);
        for (Thread t : threads) {
            t.join();
        }
        final double seconds = (System.nanoTime() - start) / 1e9;

        final CountDownLatch closed = new CountDownLatch(1);
        Platform.runLater(() -> {
            stage[0].close();
            closed.countDown();
        });
        closed.await(10, TimeUnit.SECONDS);
        Platform.exit();

        System.out.printf("  %.1f s: %d frames rendered (%d wrong), %d images shown by JavaFX, %d JavaFX pulses, %d contexts churned%n",
                seconds, framesRendered.get(), framesWrong.get(), imagesShown.get(), fxPulses.get(), churnIterations.get());
        if (framesRendered.get() != FRAMES) {
            problem("only " + framesRendered.get() + " of " + FRAMES + " frames were rendered");
        }
        if (framesWrong.get() != 0) {
            problem(framesWrong.get() + " frames had wrong pixels");
        }
        if (fxPulses.get() < 30) {
            problem("JavaFX barely ran (" + fxPulses.get() + " pulses): it was starved or broken");
        }
        if (imagesShown.get() < FRAMES / 8) {
            problem("JavaFX showed only " + imagesShown.get() + " of the rendered frames");
        }
        if (churnIterations.get() < 20) {
            problem("context churn made little progress (" + churnIterations.get() + ")");
        }
        System.out.println(problems.isEmpty() ? "FxCoexistence: OK" : "FxCoexistence: " + problems.size() + " PROBLEM(S)");
        System.exit(problems.isEmpty() ? 0 : 1);
    }
}
