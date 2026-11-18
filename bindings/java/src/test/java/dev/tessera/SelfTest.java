package dev.tessera;

import dev.tessera.internal.NativeApi;

import java.io.File;
import java.lang.reflect.Method;
import java.net.URL;
import java.net.URLClassLoader;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/**
 * End-to-end test of the Java API against the real native library (needs an OpenGL driver). Plain main() like the native
 * tests: no test framework to download. Exit code 0 = all passed.
 */
public final class SelfTest {
    private static int failures;

    static void check(boolean condition, String what) {
        if (!condition) {
            System.err.println("FAILED: " + what);
            failures++;
        }
    }

    interface Body {
        void run() throws Throwable;
    }

    static <T extends Throwable> T expect(Class<T> type, String what, Body body) {
        try {
            body.run();
        } catch (Throwable t) {
            if (type.isInstance(t)) {
                return type.cast(t);
            }
            System.err.println("FAILED: " + what + ": expected " + type.getSimpleName() + " but got " + t);
            failures++;
            return null;
        }
        System.err.println("FAILED: " + what + ": nothing was thrown, expected " + type.getSimpleName());
        failures++;
        return null;
    }

    static ContextConfig plain() {
        // Best effort so the same test runs on every backend (EGL has no double buffering, for example).
        return ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).build();
    }

    // ------------------------------------------------------------------------------------------------------

    static void configAndInfo() {
        try (Context ctx = Context.create(plain())) {
            final ContextInfo info = ctx.info();
            System.out.println("  " + info);
            check(info.versionMajor() >= 1, "version reported");
            check(!info.renderer().isEmpty() && !info.version().isEmpty(), "renderer/version strings");
            for (char c : (info.vendor() + info.renderer() + info.version()).toCharArray()) {
                check(c >= 0x20 && c < 0x7F, "info strings are printable ASCII");
            }
            check(info.api() == ContextConfig.Api.OPENGL, "api");
            check(ctx.config().api() == ContextConfig.Api.OPENGL, "config kept");
            check(Context.current() == null, "creating leaves nothing current");
        }

        // A strict impossible request fails with a result code and the native explanation.
        final TesseraException e = expect(TesseraException.class, "version 9.9 strict", () -> Context.create(ContextConfig.builder().version(9, 9).build()).close());
        if (e != null) {
            check(e.result() == Result.NO_MATCHING_CONFIG, "9.9 -> NO_MATCHING_CONFIG, was " + e.result());
            check(e.getMessage() != null && e.getMessage().length() > 20, "message explains why: " + e.getMessage());
        }
        final TesseraException bad = expect(TesseraException.class, "reset notification without robust access",
                () -> Context.create(ContextConfig.builder().flags(ContextConfig.Flags.RESET_NOTIFICATION).build()).close());
        if (bad != null) {
            check(bad.result() == Result.INVALID_ARGUMENT, "invalid combination -> INVALID_ARGUMENT, was " + bad.result());
        }

        // A core profile, where the driver offers one.
        try (Context core = Context.create(ContextConfig.builder().profile(ContextConfig.Profile.CORE).version(3, 3).build())) {
            final ContextInfo info = core.info();
            check(info.profile() == ContextConfig.Profile.CORE, "core profile reported");
            check(info.versionMajor() > 3 || (info.versionMajor() == 3 && info.versionMinor() >= 3), "core >= 3.3");
            System.out.println("  core: " + info);
        } catch (TesseraException e2) {
            check(e2.result() == Result.NO_MATCHING_CONFIG, "core refused only as NO_MATCHING_CONFIG: " + e2);
        }
    }

    static void threadRules() throws Exception {
        try (Context ctx = Context.create(plain())) {
            ctx.makeCurrent();
            check(Context.current() == ctx, "current() on the holder");
            check(Tessera.procAddress("glGetString") != 0L, "glGetString resolves while current");
            check(Tessera.procAddress("notAGlFunction") == 0L, "non-gl names do not resolve");
            ctx.swapBuffers();

            final AtomicReference<Throwable> other = new AtomicReference<>();
            final AtomicReference<Context> seen = new AtomicReference<>(ctx);
            final Thread t = new Thread(() -> {
                try {
                    seen.set(Context.current());
                    ctx.makeCurrent();
                } catch (Throwable x) {
                    other.set(x);
                }
            });
            t.start();
            t.join();
            check(other.get() instanceof IllegalStateException, "another thread cannot take a held context: " + other.get());
            check(seen.get() == null, "current() is per thread");
            Context.releaseCurrent();
            check(Context.current() == null, "released");
            expect(IllegalStateException.class, "swap on a context that is not current", ctx::swapBuffers);
        }

        final Context closed = Context.create(plain());
        closed.close();
        closed.close(); // idempotent
        check(closed.isClosed(), "isClosed");
        expect(IllegalStateException.class, "makeCurrent on a closed context", closed::makeCurrent);
        expect(IllegalStateException.class, "info on a closed context", closed::info);
    }

    static void closeFromAnotherThread() throws Exception {
        final Context ctx = Context.create(plain());
        ctx.makeCurrent();
        final Thread closer = new Thread(ctx::close, "closer");
        closer.start();
        closer.join();
        check(ctx.isClosed(), "closed by another thread while held");
        check(Context.current() == null, "a closed context is no longer current() for its holder");
        Context.releaseCurrent(); // the native teardown completes here, on the holder
        // The library is still fully usable.
        try (Context again = Context.create(plain())) {
            again.makeCurrent();
            Context.releaseCurrent();
        }

        // A holder that exits without releasing: the context is not stranded.
        final Context stranded = Context.create(plain());
        final Thread holder = new Thread(stranded::makeCurrent, "holder");
        holder.start();
        holder.join();
        stranded.makeCurrent();
        Context.releaseCurrent();
        stranded.close();
    }

    static long[] leakContexts(int n) {
        final long[] handles = new long[n];
        for (int i = 0; i < n; i++) {
            final Context c = Context.create(plain()); // deliberately never closed
            handles[i] = c.handle();
        }
        return handles;
    }

    static void cleanerSafetyNet() throws Exception {
        final long[] handles = leakContexts(5);
        final NativeApi api = Tessera.api();
        final long deadline = System.nanoTime() + 20_000_000_000L;
        boolean allGone = false;
        while (System.nanoTime() < deadline && !allGone) {
            System.gc();
            Thread.sleep(50);
            allGone = true;
            for (long h : handles) {
                if (api.contextInfo(h, new int[13], new String[3]) != Result.INVALID_HANDLE.code()) {
                    allGone = false;
                }
            }
        }
        check(allGone, "contexts that were garbage collected without close() were destroyed by the safety net");
    }

    static void logging() {
        final List<String> lines = new CopyOnWriteArrayList<>();
        final List<Logger.Level> levels = new CopyOnWriteArrayList<>();
        Tessera.setLogger((level, message) -> {
            levels.add(level);
            lines.add(message);
        });
        try (Context ctx = Context.create(ContextConfig.builder().samples(64).fallback(ContextConfig.Fallback.BEST_EFFORT).build())) {
            if (ctx.info().samples() < 64) {
                check(lines.stream().anyMatch(l -> l.contains("relaxed")), "relaxing the request is logged: " + lines);
            }
        }
        Tessera.setLogger(null);
        final int before = lines.size();
        try (Context ctx = Context.create(ContextConfig.builder().samples(64).fallback(ContextConfig.Fallback.BEST_EFFORT).build())) {
            ctx.info();
        }
        check(lines.size() == before, "no messages after the logger was removed");

        // A logger that throws must neither crash the JVM nor break the call that triggered the message.
        Tessera.setLogger((level, message) -> {
            throw new IllegalStateException("logger failure");
        });
        try (Context ctx = Context.create(ContextConfig.builder().samples(64).fallback(ContextConfig.Fallback.BEST_EFFORT).build())) {
            ctx.info();
        } finally {
            Tessera.setLogger(null);
        }
    }

    static void resetAndRecreate() {
        // Without reset notification there is nothing to poll.
        try (Context plainCtx = Context.create(plain())) {
            plainCtx.makeCurrent();
            final TesseraException e = expect(TesseraException.class, "resetStatus without notification", plainCtx::resetStatus);
            if (e != null) {
                check(e.result() == Result.UNSUPPORTED, "UNSUPPORTED, was " + e.result());
            }
            Context.releaseCurrent();
        }
        try (Context robust = Context.create(ContextConfig.builder()
                .flags(ContextConfig.Flags.ROBUST_ACCESS | ContextConfig.Flags.RESET_NOTIFICATION).build())) {
            robust.makeCurrent();
            for (int i = 0; i < 20; i++) {
                check(robust.resetStatus() == ResetStatus.NONE, "no reset is happening");
            }
            Context.releaseCurrent();
            final Context.Recreation r = robust.recreate();
            check(!r.sharePreserved(), "nothing was shared");
            check(r.context().info().hasFlag(ContextConfig.Flags.ROBUST_ACCESS), "same request");
            r.context().makeCurrent();
            Context.releaseCurrent();
            r.context().close();
        } catch (TesseraException e) {
            check(e.result() == Result.NO_MATCHING_CONFIG, "robustness refused only as NO_MATCHING_CONFIG: " + e);
            System.out.println("  (robust context not granted here)");
        }
    }

    static void probeAndPool() {
        // A probe answers like Context.create would, without keeping anything.
        final ProbeReport ok = Tessera.probe(plain());
        System.out.println("  " + ok);
        check(ok.succeeded() && ok.result() == Result.SUCCESS, "a plain request can be met");
        check(ok.info() != null && ok.info().versionMajor() >= 1 && !ok.info().renderer().isEmpty(), "the probe reports the driver's info");
        check(!ok.backend().isEmpty() && ok.message().isEmpty(), "backend named, no failure message");
        try (Context real = Context.create(plain())) {
            check(real.info().versionMajor() == ok.info().versionMajor() && real.info().renderer().equals(ok.info().renderer()),
                    "the probe agrees with a real context");
        }
        final ProbeReport no = Tessera.probe(ContextConfig.builder().version(9, 9).build());
        check(!no.succeeded() && no.result() == Result.NO_MATCHING_CONFIG, "9.9 -> NO_MATCHING_CONFIG, was " + no.result());
        check(no.info() == null && !no.message().isEmpty() && !no.backend().isEmpty(), "failure: no info, a reason, the backend that was asked");
        final TesseraException invalid = expect(TesseraException.class, "probe of an invalid configuration",
                () -> Tessera.probe(ContextConfig.builder().flags(ContextConfig.Flags.RESET_NOTIFICATION).build()));
        if (invalid != null) {
            check(invalid.result() == Result.INVALID_ARGUMENT, "invalid combination -> INVALID_ARGUMENT, was " + invalid.result());
        }
        try (Context share = Context.create(plain())) {
            expect(IllegalArgumentException.class, "probe cannot share",
                    () -> Tessera.probe(ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).shareWith(share).build()));
            expect(IllegalArgumentException.class, "prewarm cannot share",
                    () -> Tessera.prewarm(ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).shareWith(share).build(), 1));
        }
        check(Context.current() == null, "probing leaves nothing current");

        // Pre-warmed contexts are handed out for identical requests only.
        final int before = Tessera.pooledContextCount();
        Tessera.prewarm(plain(), 2);
        check(Tessera.pooledContextCount() == before + 2, "two contexts pooled: " + Tessera.pooledContextCount());
        Tessera.probe(plain());
        check(Tessera.pooledContextCount() == before + 2, "a probe does not take a pooled context");
        final Context a = Context.create(plain());
        check(Tessera.pooledContextCount() == before + 1, "create took one");
        final Context other = Context.create(ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).depthBits(16).build());
        check(Tessera.pooledContextCount() == before + 1, "a different request did not");
        final Context b = Context.create(plain());
        check(Tessera.pooledContextCount() == before, "second create took the other one");
        a.makeCurrent();
        check(Context.current() == a && a.info().versionMajor() >= 1, "a pooled context works like a new one");
        Context.releaseCurrent();
        a.close();
        b.close();
        other.close();
        expect(TesseraException.class, "pooling more than the limit", () -> Tessera.prewarm(plain(), 65));
    }

    static void nativeHandles() {
        final String os = System.getProperty("os.name", "").toLowerCase();
        try (Context a = Context.create(plain()); Context b = Context.create(plain())) {
            final NativeHandles h = a.nativeHandles();
            System.out.println("  " + h);
            if (os.contains("win")) {
                check(h.kind() == NativeHandles.Kind.WGL, "Windows -> WGL, was " + h.kind());
                check(h.get(0) != 0 && h.get(1) != 0 && h.get(4) != 0, "HGLRC, HDC and a pixel format are set");
            } else if (os.contains("mac")) {
                check(h.kind() == NativeHandles.Kind.CGL, "macOS -> CGL, was " + h.kind());
            } else {
                check(h.kind() == NativeHandles.Kind.GLX || h.kind() == NativeHandles.Kind.EGL, "Linux -> GLX or EGL, was " + h.kind());
                check(h.get(0) != 0 && h.get(1) != 0, "display and context are set");
            }
            check(h.contextHandle() != 0, "a context object");
            check(h.equals(a.nativeHandles()), "stable for the context's lifetime");
            check(h.contextHandle() != b.nativeHandles().contextHandle(), "distinct contexts, distinct native objects");
            check(h.toArray().length == NativeHandles.COUNT && h.toArray() != h.toArray(), "toArray copies");
            expect(IndexOutOfBoundsException.class, "index range", () -> h.get(NativeHandles.COUNT));
        }
        final Context closed = Context.create(plain());
        closed.close();
        expect(IllegalStateException.class, "handles of a closed context", closed::nativeHandles);
    }

    static void windowAttachValidation() {
        // A real window needs a native toolkit (see the native tests); here only what the Java layer can check on its own.
        final TesseraException none = expect(TesseraException.class, "window 0", () -> Context.createForWindow(plain(), 0L, false).close());
        if (none != null) {
            check(none.result() == Result.INVALID_ARGUMENT, "window 0 -> INVALID_ARGUMENT, was " + none.result());
        }
        final TesseraException bogus = expect(TesseraException.class, "not a window", () -> Context.createForWindow(plain(), 0x1234L, false).close());
        if (bogus != null) {
            check(bogus.result() == Result.INVALID_ARGUMENT || bogus.result() == Result.UNSUPPORTED,
                    "a bogus window id -> INVALID_ARGUMENT (WGL, GLX) or UNSUPPORTED (EGL, CGL), was " + bogus.result());
        }
        check(Context.current() == null, "failed attaches leave nothing current");
    }

    static void gpuSelection() {
        final boolean windows = System.getProperty("os.name", "").toLowerCase().contains("win");
        final ContextConfig.Builder base = ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT);
        final ContextConfig high = base.gpuPreference(ContextConfig.GpuPreference.HIGH_PERFORMANCE).build();
        check(high.gpuPreference() == ContextConfig.GpuPreference.HIGH_PERFORMANCE && high.adapterLuid() == 0, "preference kept");
        final ContextConfig luid = ContextConfig.builder().adapterLuid(0x1234567800000001L).build();
        check(luid.adapterLuid() == 0x1234567800000001L && luid.toNative()[15] == 1 && luid.toNative()[16] == 0x12345678, "64-bit LUID split into two ints");

        final TesseraException both = expect(TesseraException.class, "preference and LUID together", () -> Context.create(
                ContextConfig.builder().gpuPreference(ContextConfig.GpuPreference.LOW_POWER).adapterLuid(5).build()).close());
        if (both != null) {
            check(both.result() == Result.INVALID_ARGUMENT, "both -> INVALID_ARGUMENT, was " + both.result());
        }
        if (windows) {
            // An unknown adapter is refused with a reason; a preference either works or is refused, never a wrong GPU.
            final ProbeReport bogus = Tessera.probe(luid);
            check(bogus.result() == Result.NO_MATCHING_CONFIG && bogus.message().contains("LUID"), "unknown LUID: " + bogus);
            final ProbeReport p = Tessera.probe(high);
            check(p.result() == Result.SUCCESS || p.result() == Result.NO_MATCHING_CONFIG, "preference: " + p);
            if (p.succeeded()) {
                try (Context c = Context.create(high)) {
                    check(c.info().renderer().equals(p.info().renderer()), "same GPU as the probe");
                }
            }
        } else {
            final TesseraException unsupported = expect(TesseraException.class, "GPU request off Windows", () -> Context.create(high).close());
            if (unsupported != null) {
                check(unsupported.result() == Result.UNSUPPORTED, "UNSUPPORTED off Windows, was " + unsupported.result());
            }
            check(Tessera.probe(high).result() == Result.UNSUPPORTED, "probe agrees");
        }
    }

    static void sharing() {
        final Context a = Context.create(plain());
        try (Context b = Context.create(ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).shareWith(a).build())) {
            a.makeCurrent();
            Context.releaseCurrent();
            b.makeCurrent();
            Context.releaseCurrent();
            a.close(); // the parent first: the child keeps working
            b.makeCurrent();
            Context.releaseCurrent();
        } finally {
            a.close(); // idempotent
        }
    }

    static void virtualThreadsAreRefused() throws Exception {
        final Method ofVirtual;
        try {
            ofVirtual = Thread.class.getMethod("ofVirtual");
        } catch (NoSuchMethodException e) {
            return; // Java < 21
        }
        final Object builder;
        try {
            builder = ofVirtual.invoke(null);
        } catch (java.lang.reflect.InvocationTargetException e) {
            if (e.getCause() instanceof UnsupportedOperationException) {
                return; // Java 19/20: virtual threads are a preview API and not enabled
            }
            throw e;
        }
        try (Context ctx = Context.create(plain())) {
            final Method start = Class.forName("java.lang.Thread$Builder").getMethod("start", Runnable.class);
            final AtomicReference<Throwable> failure = new AtomicReference<>();
            final Thread t = (Thread) start.invoke(builder, (Runnable) () -> {
                try {
                    ctx.makeCurrent();
                } catch (Throwable x) {
                    failure.set(x);
                }
            });
            t.join();
            check(failure.get() instanceof IllegalStateException && String.valueOf(failure.get().getMessage()).contains("platform thread"),
                    "virtual threads are refused: " + failure.get());
        }
    }

    /** Used by the classloader test: one complete round trip. */
    public static void smoke() {
        try (Context ctx = Context.create(plain())) {
            ctx.makeCurrent();
            ctx.info();
            Context.releaseCurrent();
        }
    }

    static void classLoaders() throws Exception {
        // Two isolated classloaders each loading the whole Java layer (and so each wanting to load the native library).
        final List<URL> urls = new ArrayList<>();
        for (String entry : System.getProperty("java.class.path").split(File.pathSeparator)) {
            urls.add(new File(entry).toURI().toURL());
        }
        final AtomicReference<Throwable> failure = new AtomicReference<>();
        final List<Thread> threads = new ArrayList<>();
        final List<String> bindings = new CopyOnWriteArrayList<>();
        for (int i = 0; i < 2; i++) {
            final URLClassLoader loader = new URLClassLoader(urls.toArray(new URL[0]), ClassLoader.getPlatformClassLoader());
            final Thread t = new Thread(() -> {
                try {
                    final Class<?> self = Class.forName("dev.tessera.SelfTest", true, loader);
                    check(self != SelfTest.class, "the second copy really is a different class");
                    self.getMethod("smoke").invoke(null);
                    bindings.add(String.valueOf(Class.forName("dev.tessera.Tessera", true, loader).getMethod("binding").invoke(null)));
                } catch (Throwable x) {
                    failure.set(x);
                }
            }, "loader-" + i);
            threads.add(t);
        }
        for (Thread t : threads) {
            t.start();
        }
        for (Thread t : threads) {
            t.join();
        }
        check(failure.get() == null, "two classloaders each use the library: " + failure.get());
        check(bindings.size() == 2, "both classloaders finished");
        smoke(); // and the main loader still works
    }

    public static void main(String[] args) throws Exception {
        final boolean noLoaders = args.length > 0 && args[0].equals("--no-classloaders");
        final int[] v = Tessera.nativeVersion();
        System.out.println("Tessera native " + v[0] + "." + v[1] + "." + v[2] + " via " + Tessera.binding()
                + " on Java " + Runtime.version() + (bindingNote() == null ? "" : " (" + bindingNote() + ")"));

        final String[] names = {"config and info", "thread rules", "close from another thread", "garbage-collection safety net", "logging",
                "reset and recreate", "sharing", "probe and pre-warming", "native handles", "window attach (validation)", "GPU selection", "virtual threads",
                "classloaders"};
        final Body[] bodies = {SelfTest::configAndInfo, SelfTest::threadRules, SelfTest::closeFromAnotherThread, SelfTest::cleanerSafetyNet,
                SelfTest::logging, SelfTest::resetAndRecreate, SelfTest::sharing, SelfTest::probeAndPool, SelfTest::nativeHandles,
                SelfTest::windowAttachValidation, SelfTest::gpuSelection, SelfTest::virtualThreadsAreRefused, SelfTest::classLoaders};
        for (int i = 0; i < bodies.length; i++) {
            if (noLoaders && names[i].equals("classloaders")) {
                continue;
            }
            System.out.println(names[i]);
            final int before = failures;
            try {
                bodies[i].run();
            } catch (Throwable t) {
                t.printStackTrace();
                failures++;
            }
            if (failures == before) {
                System.out.println("  ok");
            }
        }
        System.out.println(failures == 0 ? "SelfTest: OK" : "SelfTest: " + failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }

    private static String bindingNote() {
        return dev.tessera.internal.Natives.fallbackNote();
    }
}
