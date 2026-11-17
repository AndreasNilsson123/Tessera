package dev.tessera;

import dev.tessera.internal.NativeApi;
import dev.tessera.internal.Natives;

import java.lang.reflect.Method;

/**
 * Entry point: native library lifecycle, logging and function lookup. Most programs only need {@link Context}.
 *
 * <p><b>Lifecycle.</b> Creating a {@link Context} initialises the native library on first use and releases it again when the
 * JVM shuts down. {@link #init()} / {@link #terminate()} are optional extra reference counts for code that wants
 * deterministic teardown; they are safe to mix with the implicit one.
 *
 * <p><b>Threads.</b> An OpenGL context can be current on one operating-system thread at a time. Use platform threads for
 * rendering: a virtual thread may be moved between OS threads between two calls, which is why making a context current
 * from one is refused.
 */
public final class Tessera {

    private static final Object LOCK = new Object();
    private static boolean implicitRef;
    private static boolean shutDown;
    private static Logger logger;

    private Tessera() {
    }

    /** "jni" or "ffm": which binding is in use. Loads the native library if that has not happened yet. */
    public static String binding() {
        return Natives.get().name();
    }

    /** Native library version {@code {major, minor, patch}}. */
    public static int[] nativeVersion() {
        final int[] v = new int[3];
        Natives.get().version(v);
        return v;
    }

    /** Adds a native reference count; every call must be paired with {@link #terminate()}. */
    public static void init() {
        final NativeApi api = api();
        check(api, api.init());
    }

    /** Releases a reference taken by {@link #init()}. When the last reference (including the implicit one) goes, all remaining contexts are destroyed. */
    public static void terminate() {
        final NativeApi api = api();
        check(api, api.terminate());
    }

    /**
     * Receives native log messages (backend choices, relaxed configurations, dropped requests, driver debug messages of debug
     * contexts). {@code null} stops logging. The logger can be called on any thread.
     */
    public static void setLogger(Logger newLogger) {
        final NativeApi api = Natives.get();
        synchronized (LOCK) {
            logger = newLogger;
        }
        api.setLogSink(newLogger == null ? null : (level, message) -> {
            final Logger l;
            synchronized (LOCK) {
                l = logger;
            }
            if (l != null) {
                l.log(Logger.Level.of(level), message);
            }
        });
    }

    /**
     * Address of an OpenGL function for loaders such as LWJGL, or 0. Extension and post-1.1 functions only resolve while a
     * context is current on the calling thread. On some platforms a made-up {@code gl...} name can still return a harmless
     * stub, so learn what is supported from the extension strings, not from this being non-zero.
     */
    public static long procAddress(String name) {
        return api().procAddress(name);
    }

    // ------------------------------------------------------------------ internal

    /** The binding, after making sure the native library is initialised for implicit use. */
    static NativeApi apiInitialized() {
        final NativeApi api = api();
        synchronized (LOCK) {
            if (!implicitRef) {
                final int r = api.init();
                if (r != 0) {
                    throw exceptionFor(api, r);
                }
                implicitRef = true;
                Runtime.getRuntime().addShutdownHook(new Thread(Tessera::shutdown, "tessera-shutdown"));
            }
        }
        return api;
    }

    static NativeApi api() {
        synchronized (LOCK) {
            if (shutDown) {
                throw new IllegalStateException("Tessera has been shut down (the JVM is exiting)");
            }
        }
        return Natives.get();
    }

    private static void shutdown() {
        final NativeApi api;
        synchronized (LOCK) {
            if (shutDown) {
                return;
            }
            shutDown = true;
            api = Natives.get();
        }
        // First stop native callbacks into this JVM, then let the library tear its contexts down.
        api.setLogSink(null);
        synchronized (LOCK) {
            if (implicitRef) {
                implicitRef = false;
                api.terminate();
            }
        }
    }

    static void check(NativeApi api, int code) {
        if (code != 0) {
            throw exceptionFor(api, code);
        }
    }

    private static RuntimeException exceptionFor(NativeApi api, int code) {
        final Result result = Result.of(code);
        final String message = api.lastError();
        switch (result) {
            case WRONG_THREAD:
            case INVALID_HANDLE:
            case NOT_INITIALIZED:
                return new IllegalStateException(result + ": " + message);
            case CONTEXT_LOST:
                return new ContextLostException(message);
            default:
                return new TesseraException(result, message);
        }
    }

    private static final Method IS_VIRTUAL = findIsVirtual();

    private static Method findIsVirtual() {
        try {
            return Thread.class.getMethod("isVirtual"); // Java 21+
        } catch (NoSuchMethodException e) {
            return null;
        }
    }

    /** Refuses virtual threads for operations whose meaning depends on the OS thread. */
    static void requirePlatformThread(String what) {
        if (IS_VIRTUAL != null) {
            try {
                if ((Boolean) IS_VIRTUAL.invoke(Thread.currentThread())) {
                    throw new IllegalStateException(what + " must run on a platform thread: a virtual thread can move between OS threads, "
                            + "and an OpenGL context belongs to exactly one");
                }
            } catch (ReflectiveOperationException e) {
                // cannot tell: allow
            }
        }
    }
}
