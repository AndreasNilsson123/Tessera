// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

import io.github.andreasnilsson123.tessera.internal.NativeApi;

import java.lang.ref.Cleaner;
import java.lang.ref.WeakReference;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * An OpenGL or OpenGL ES context that lives alongside whatever else the process is doing (JavaFX, AWT, other native
 * libraries): creating, using and closing it never disturbs another library's current context.
 *
 * <pre>{@code
 * try (Context ctx = Context.create(ContextConfig.builder().profile(Profile.CORE).version(3, 3).build())) {
 *     ctx.makeCurrent();
 *     // ... GL calls ...
 *     Context.releaseCurrent();
 * }
 * }</pre>
 *
 * <p><b>Threads.</b> A context is current on at most one thread. {@link #makeCurrent()} on another thread while it is held
 * throws {@link IllegalStateException}; if the holder exits, the context is released for it. {@link #close()} may be called
 * from any thread, even one that is not the holder: the handle dies immediately and the native context is torn down on the
 * holding thread when it lets go.
 *
 * <p><b>Cleanup.</b> A context that is garbage collected without being closed is destroyed by a safety net, but closing it
 * explicitly is the only deterministic way to free its GPU resources.
 */
public final class Context implements AutoCloseable {

    private static final Cleaner CLEANER = Cleaner.create(task -> {
        final Thread thread = new Thread(task, "tessera-cleaner");
        thread.setDaemon(true);
        return thread;
    });
    private static final Map<Long, WeakReference<Context>> LIVE = new ConcurrentHashMap<>();

    /** The native handle plus the destruction logic; deliberately does not reference the {@link Context}. */
    private static final class State implements Runnable {
        final long handle;
        final AtomicBoolean closed = new AtomicBoolean();

        State(long handle) {
            this.handle = handle;
        }

        @Override
        public void run() {
            if (closed.compareAndSet(false, true)) {
                LIVE.remove(handle);
                try {
                    final NativeApi api = Tessera.api();
                    api.destroyContext(handle);
                } catch (RuntimeException ignored) {
                    // the JVM is shutting down (Tessera.terminate already destroyed everything) or the handle is gone
                }
            }
        }
    }

    private final State state;
    private final Cleaner.Cleanable cleanable;
    private final ContextConfig config;
    /** The thread that last made this context current through us; see {@link #makeCurrent()}. */
    private volatile WeakReference<Thread> lastHolder = new WeakReference<>(null);

    private Context(long handle, ContextConfig config) {
        this.state = new State(handle);
        this.config = config;
        this.cleanable = CLEANER.register(this, state);
        LIVE.put(handle, new WeakReference<>(this));
    }

    /**
     * Creates a context. Fails with {@link TesseraException} {@link Result#NO_MATCHING_CONFIG} if (in strict mode) the system
     * cannot provide what was asked for, and {@link Result#UNSUPPORTED} if no backend can serve the request at all.
     */
    public static Context create(ContextConfig config) {
        Objects.requireNonNull(config, "config");
        final NativeApi api = Tessera.apiInitialized();
        final Context share = config.shareWith();
        final long shareHandle = share == null ? 0L : share.handle();
        final long[] out = new long[1];
        Tessera.check(api, api.createContext(config.toNative(), shareHandle, out));
        return new Context(out[0], config);
    }

    /**
     * Creates a context that renders into a window <b>you</b> own (Windows: an {@code HWND}; Linux/X11: a {@code Window} id on
     * the X server of {@code $DISPLAY}). Anywhere else this fails with {@link Result#UNSUPPORTED}. Present with
     * {@link #swapBuffers()}; {@link ContextInfo#surface()} reports {@link ContextConfig.Surface#WINDOW}.
     *
     * <p>The window is borrowed: Tessera never destroys, resizes, shows or hides it, and it must outlive every use of the
     * context (destroy the context first). If the window already has a pixel format it is adopted and, with
     * {@link ContextConfig.Fallback#STRICT}, checked against the request. A Windows window's pixel format can be set only once
     * in its lifetime, so Tessera sets one only if you pass {@code allowSettingPixelFormat = true}, because only you know the
     * window is fresh; otherwise a window without a pixel format is refused with {@link Result#INVALID_ARGUMENT} and left
     * untouched.
     *
     * @param config the surface must be {@link ContextConfig.Surface#DEFAULT}
     */
    public static Context createForWindow(ContextConfig config, long nativeWindow, boolean allowSettingPixelFormat) {
        Objects.requireNonNull(config, "config");
        final NativeApi api = Tessera.apiInitialized();
        final Context share = config.shareWith();
        final long shareHandle = share == null ? 0L : share.handle();
        final long[] out = new long[1];
        Tessera.check(api, api.createContextForWindow(config.toNative(), shareHandle, nativeWindow, allowSettingPixelFormat ? 1 : 0, out));
        return new Context(out[0], config);
    }

    /**
     * The operating-system objects behind this context, for interop with other libraries; borrowed, valid until this context
     * is closed (see {@link NativeHandles}).
     */
    public NativeHandles nativeHandles() {
        final NativeApi api = Tessera.api();
        final int[] kind = new int[1];
        final long[] handles = new long[NativeHandles.COUNT];
        Tessera.check(api, api.nativeHandles(handle(), kind, handles));
        return new NativeHandles(kind[0], handles);
    }

    long handle() {
        if (state.closed.get()) {
            throw new IllegalStateException("the context is closed");
        }
        return state.handle;
    }

    public boolean isClosed() {
        return state.closed.get();
    }

    public ContextConfig config() {
        return config;
    }

    /** What the driver actually gave us. */
    public ContextInfo info() {
        final NativeApi api = Tessera.api();
        final int[] ints = new int[13];
        final String[] strings = new String[3];
        Tessera.check(api, api.contextInfo(handle(), ints, strings));
        return new ContextInfo(ints, strings);
    }

    /**
     * Makes this context current on the calling thread, replacing whatever was current there (including a context that
     * belongs to another library; it is not remembered or restored).
     *
     * @throws IllegalStateException if it is current on another live thread, was closed, or this is a virtual thread
     * @throws ContextLostException  if the context was lost
     */
    public void makeCurrent() {
        Tessera.requirePlatformThread("Context.makeCurrent()");
        final NativeApi api = Tessera.api();
        final long handle = handle();
        int result = api.makeCurrent(handle);
        if (result == Result.WRONG_THREAD.code()) {
            // Thread.join() returns when the Java thread has terminated, but the operating-system thread is still finishing;
            // the native library releases a context its holder left behind only when that OS thread really exits. If the
            // holder we know of is already dead, that is imminent: allow it a moment instead of reporting a false conflict.
            final Thread holder = lastHolder.get();
            if (holder != null && holder != Thread.currentThread() && !holder.isAlive()) {
                final long deadline = System.nanoTime() + 2_000_000_000L;
                while (result == Result.WRONG_THREAD.code() && System.nanoTime() < deadline) {
                    Thread.onSpinWait();
                    try {
                        Thread.sleep(1);
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        break;
                    }
                    result = api.makeCurrent(handle);
                }
            }
        }
        Tessera.check(api, result);
        lastHolder = new WeakReference<>(Thread.currentThread());
    }

    /** Releases the Tessera context this thread made current, if any. A current context that is not ours is left alone. */
    public static void releaseCurrent() {
        final NativeApi api = Tessera.api();
        Tessera.check(api, api.makeCurrent(0L));
    }

    /** The context this thread made current through Tessera, or {@code null}. */
    public static Context current() {
        final long handle = Tessera.api().currentContext();
        if (handle == 0L) {
            return null;
        }
        final WeakReference<Context> ref = LIVE.get(handle);
        return ref == null ? null : ref.get();
    }

    /** Presents the back buffer (a no-op for surfaces that have none). The context must be current on this thread. */
    public void swapBuffers() {
        final NativeApi api = Tessera.api();
        Tessera.check(api, api.swapBuffers(handle()));
    }

    /** 0 = no vsync, 1 = vsync, -1 = adaptive where supported. The context must be current on this thread. */
    public void setSwapInterval(int interval) {
        final NativeApi api = Tessera.api();
        Tessera.check(api, api.setSwapInterval(handle(), interval));
    }

    /**
     * Polls for a GPU reset. Needs a context created with {@link ContextConfig.Flags#ROBUST_ACCESS} and
     * {@link ContextConfig.Flags#RESET_NOTIFICATION}, current on this thread. Anything but {@link ResetStatus#NONE} means the
     * context is now lost for good.
     *
     * @throws TesseraException with {@link Result#UNSUPPORTED} if the context cannot report resets
     */
    public ResetStatus resetStatus() {
        final NativeApi api = Tessera.api();
        final int[] out = new int[1];
        Tessera.check(api, api.resetStatus(handle(), out));
        return ResetStatus.of(out[0]);
    }

    /** The result of {@link #recreate()}. */
    public static final class Recreation {
        private final Context context;
        private final boolean sharePreserved;

        Recreation(Context context, boolean sharePreserved) {
            this.context = context;
            this.sharePreserved = sharePreserved;
        }

        public Context context() {
            return context;
        }

        /** True if the new context joined a surviving member of the old share group, so shared textures/buffers still exist. */
        public boolean sharePreserved() {
            return sharePreserved;
        }
    }

    /**
     * Builds a replacement from the same request (normally after a loss). This context stays valid and must still be closed.
     * If it shared objects with others, the replacement shares with a surviving member and {@link Recreation#sharePreserved()}
     * is true; otherwise everything must be uploaded again.
     */
    public Recreation recreate() {
        final NativeApi api = Tessera.api();
        final long[] handle = new long[1];
        final int[] flags = new int[1];
        Tessera.check(api, api.recreate(handle(), handle, flags));
        return new Recreation(new Context(handle[0], config), (flags[0] & 1) != 0);
    }

    /** Destroys the context. Idempotent, any thread (see the class comment for what happens if another thread holds it). */
    @Override
    public void close() {
        cleanable.clean();
    }

    @Override
    public String toString() {
        return "Context{" + (state.closed.get() ? "closed" : "0x" + Long.toHexString(state.handle)) + ", " + config + "}";
    }
}
