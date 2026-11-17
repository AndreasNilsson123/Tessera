package dev.tessera.lwjgl;

import dev.tessera.Context;
import dev.tessera.ContextConfig;
import dev.tessera.ContextInfo;
import dev.tessera.TesseraException;
import org.lwjgl.BufferUtils;
import org.lwjgl.opengl.GL11C;
import org.lwjgl.opengl.GL15C;
import org.lwjgl.opengl.GL20C;
import org.lwjgl.opengl.GL30C;
import org.lwjgl.opengl.GLCapabilities;

import java.nio.ByteBuffer;
import java.util.concurrent.atomic.AtomicReference;

/**
 * LWJGL rendering through Tessera contexts: real GL calls, results read back and checked. Needs LWJGL on the classpath and an
 * OpenGL driver. Exit code 0 = all passed.
 */
public final class LwjglTest {
    private static int failures;

    private static void check(boolean condition, String what) {
        if (!condition) {
            System.err.println("FAILED: " + what);
            failures++;
        }
    }

    private static Context createContext() {
        try {
            return Context.create(ContextConfig.builder().profile(ContextConfig.Profile.CORE).version(3, 3).build());
        } catch (TesseraException e) {
            System.out.println("  (no core 3.3 here: " + e.result() + "; using the default profile)");
            return Context.create(ContextConfig.builder().version(3, 3).fallback(ContextConfig.Fallback.BEST_EFFORT).build());
        }
    }

    /** Renders into an off-screen framebuffer and returns the pixel at the centre. */
    private static int[] renderAndRead(boolean useShader) {
        final int size = 64;
        final int fbo = GL30C.glGenFramebuffers();
        final int rbo = GL30C.glGenRenderbuffers();
        GL30C.glBindRenderbuffer(GL30C.GL_RENDERBUFFER, rbo);
        GL30C.glRenderbufferStorage(GL30C.GL_RENDERBUFFER, GL11C.GL_RGBA8 , size, size);
        GL30C.glBindFramebuffer(GL30C.GL_FRAMEBUFFER, fbo);
        GL30C.glFramebufferRenderbuffer(GL30C.GL_FRAMEBUFFER, GL30C.GL_COLOR_ATTACHMENT0, GL30C.GL_RENDERBUFFER, rbo);
        check(GL30C.glCheckFramebufferStatus(GL30C.GL_FRAMEBUFFER) == GL30C.GL_FRAMEBUFFER_COMPLETE, "framebuffer complete");
        GL11C.glViewport(0, 0, size, size);

        if (!useShader) {
            GL11C.glClearColor(0.25f, 0.5f, 0.75f, 1f);
            GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
        } else {
            GL11C.glClearColor(0f, 0f, 0f, 1f);
            GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
            final int vs = GL20C.glCreateShader(GL20C.GL_VERTEX_SHADER);
            GL20C.glShaderSource(vs, "#version 330 core\nvoid main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }");
            GL20C.glCompileShader(vs);
            check(GL20C.glGetShaderi(vs, GL20C.GL_COMPILE_STATUS) == GL11C.GL_TRUE, "vertex shader compiles: " + GL20C.glGetShaderInfoLog(vs));
            final int fs = GL20C.glCreateShader(GL20C.GL_FRAGMENT_SHADER);
            GL20C.glShaderSource(fs, "#version 330 core\nout vec4 c;\nvoid main(){ c = vec4(0.0, 1.0, 0.0, 1.0); }");
            GL20C.glCompileShader(fs);
            check(GL20C.glGetShaderi(fs, GL20C.GL_COMPILE_STATUS) == GL11C.GL_TRUE, "fragment shader compiles: " + GL20C.glGetShaderInfoLog(fs));
            final int program = GL20C.glCreateProgram();
            GL20C.glAttachShader(program, vs);
            GL20C.glAttachShader(program, fs);
            GL20C.glLinkProgram(program);
            check(GL20C.glGetProgrami(program, GL20C.GL_LINK_STATUS) == GL11C.GL_TRUE, "program links: " + GL20C.glGetProgramInfoLog(program));
            final int vao = GL30C.glGenVertexArrays(); // core profile needs one even without attributes
            GL30C.glBindVertexArray(vao);
            GL20C.glUseProgram(program);
            GL11C.glDrawArrays(GL11C.GL_TRIANGLES, 0, 3);
            GL20C.glUseProgram(0);
            GL30C.glDeleteVertexArrays(vao);
            GL20C.glDeleteProgram(program);
            GL20C.glDeleteShader(vs);
            GL20C.glDeleteShader(fs);
        }

        final ByteBuffer pixel = BufferUtils.createByteBuffer(4);
        GL11C.glReadPixels(size / 2, size / 2, 1, 1, GL11C.GL_RGBA, GL11C.GL_UNSIGNED_BYTE, pixel);
        final int[] rgba = {pixel.get(0) & 0xFF, pixel.get(1) & 0xFF, pixel.get(2) & 0xFF, pixel.get(3) & 0xFF};
        GL30C.glBindFramebuffer(GL30C.GL_FRAMEBUFFER, 0);
        GL30C.glDeleteFramebuffers(fbo);
        GL30C.glDeleteRenderbuffers(rbo);
        check(GL11C.glGetError() == GL11C.GL_NO_ERROR, "no GL error");
        return rgba;
    }

    private static boolean near(int[] actual, int r, int g, int b, int a) {
        return Math.abs(actual[0] - r) <= 1 && Math.abs(actual[1] - g) <= 1 && Math.abs(actual[2] - b) <= 1 && Math.abs(actual[3] - a) <= 1;
    }

    public static void main(String[] args) throws Exception {
        System.out.println("LWJGL + Tessera (" + dev.tessera.Tessera.binding() + ")");
        try (Context ctx = createContext()) {
            final ContextInfo info = ctx.info();
            System.out.println("  " + info);
            ctx.makeCurrent();

            final GLCapabilities caps = TesseraGL.createCapabilities();
            check(caps.OpenGL33, "LWJGL sees OpenGL 3.3+ (" + info.version() + ")");
            check(GL11C.glGetString(GL11C.GL_VERSION).equals(info.version()), "glGetString(GL_VERSION) matches the context info: "
                    + GL11C.glGetString(GL11C.GL_VERSION));
            check(GL11C.glGetString(GL11C.GL_RENDERER).equals(info.renderer()), "glGetString(GL_RENDERER)");
            check(GL11C.glGetInteger(GL30C.GL_MAJOR_VERSION) >= 3, "glGetInteger(GL_MAJOR_VERSION)");

            System.out.println("clear and read back");
            final int[] cleared = renderAndRead(false);
            check(near(cleared, 64, 128, 191, 255), "cleared colour read back: " + java.util.Arrays.toString(cleared));

            System.out.println("shader draw and read back");
            final int[] drawn = renderAndRead(true);
            check(near(drawn, 0, 255, 0, 255), "shader output read back: " + java.util.Arrays.toString(drawn));

            // buffer objects, an extension-era function (glMapBuffer family) and per-thread capabilities
            System.out.println("buffers");
            final int buffer = GL15C.glGenBuffers();
            GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, buffer);
            final ByteBuffer data = BufferUtils.createByteBuffer(16);
            for (int i = 0; i < 16; i++) {
                data.put(i, (byte) (i * 3));
            }
            GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, data, GL15C.GL_STATIC_DRAW);
            final ByteBuffer back = BufferUtils.createByteBuffer(16);
            GL15C.glGetBufferSubData(GL15C.GL_ARRAY_BUFFER, 0, back);
            check(back.equals(data), "buffer round trip");
            GL15C.glDeleteBuffers(buffer);

            // a second thread with its own context and its own capabilities, while this one stays current
            System.out.println("second thread");
            final AtomicReference<int[]> threadPixel = new AtomicReference<>();
            final AtomicReference<Throwable> failure = new AtomicReference<>();
            final Thread worker = new Thread(() -> {
                try (Context other = createContext()) {
                    other.makeCurrent();
                    TesseraGL.createCapabilities();
                    threadPixel.set(renderAndRead(true));
                    Context.releaseCurrent();
                } catch (Throwable t) {
                    failure.set(t);
                }
            }, "render-2");
            worker.start();
            worker.join();
            check(failure.get() == null, "second thread rendered: " + failure.get());
            check(threadPixel.get() != null && near(threadPixel.get(), 0, 255, 0, 255), "second thread's pixels");
            check(Context.current() == ctx, "this thread's context is untouched by the other thread");
            check(GL11C.glGetError() == GL11C.GL_NO_ERROR, "no GL error after the second thread");
            Context.releaseCurrent();
        }
        System.out.println(failures == 0 ? "LwjglTest: OK" : "LwjglTest: " + failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
