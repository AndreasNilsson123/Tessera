package dev.tessera.lwjgl;

import dev.tessera.Context;
import org.lwjgl.opengl.GL;
import org.lwjgl.opengl.GLCapabilities;
import org.lwjgl.system.Configuration;

/**
 * The few calls that connect LWJGL's OpenGL bindings to Tessera contexts.
 *
 * <pre>{@code
 * try (Context ctx = Context.create(config)) {
 *     ctx.makeCurrent();
 *     GLCapabilities caps = TesseraGL.createCapabilities();   // once per thread that uses the context
 *     GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
 * }
 * }</pre>
 *
 * <p>LWJGL keeps a single OpenGL function provider per classloader, and its {@code GL} class loads the system OpenGL
 * library the instant it is first touched unless {@code Configuration.OPENGL_EXPLICIT_INIT} is set. {@link #install()} sets
 * that flag and then installs Tessera's provider, so it has to run before anything else uses {@code GL} (or start the JVM
 * with {@code -Dorg.lwjgl.opengl.explicitInit=true}). An application that also wants LWJGL's own GLFW/system-library
 * OpenGL in the same classloader cannot mix the two: pick one.
 */
public final class TesseraGL {

    private static boolean installed;

    private TesseraGL() {
    }

    /** Makes LWJGL resolve OpenGL through Tessera. Idempotent. */
    public static synchronized void install() {
        if (!installed) {
            // Without this, the GL class initialiser loads the system library before we can install our provider.
            Configuration.OPENGL_EXPLICIT_INIT.set(true);
            try {
                GL.create(TesseraFunctionProvider.INSTANCE);
            } catch (IllegalStateException e) {
                throw new IllegalStateException("LWJGL's OpenGL was already initialised (by another provider or by the system library) "
                        + "before TesseraGL.install() ran. Call it before anything else touches org.lwjgl.opengl.GL, or start the JVM "
                        + "with -Dorg.lwjgl.opengl.explicitInit=true: " + e.getMessage(), e);
            }
            installed = true;
        }
    }

    /**
     * Creates LWJGL's {@link GLCapabilities} for the Tessera context current on this thread and makes them this thread's
     * capabilities. Call it on every thread that renders with LWJGL, after {@link Context#makeCurrent()}.
     */
    public static GLCapabilities createCapabilities() {
        if (Context.current() == null) {
            throw new IllegalStateException("make a Tessera context current on this thread first");
        }
        install();
        return GL.createCapabilities();
    }

    /** Releases LWJGL's OpenGL state (the reverse of {@link #install()}). */
    public static synchronized void uninstall() {
        if (installed) {
            GL.destroy();
            installed = false;
        }
    }
}
