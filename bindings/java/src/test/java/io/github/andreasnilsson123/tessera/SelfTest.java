// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

import io.github.andreasnilsson123.tessera.internal.NativeApi;

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
        adapters();
        sharingExplained();
        // EXACT_VERSION is a flag the library knows (ABI 0.13.0): it is accepted and the minimum still holds.
        try (Context exact = Context.create(ContextConfig.builder().profile(ContextConfig.Profile.CORE).version(3, 3)
                .flags(ContextConfig.Flags.EXACT_VERSION).build())) {
            final ContextInfo info = exact.info();
            check(info.versionMajor() > 3 || (info.versionMajor() == 3 && info.versionMinor() >= 3), "exact core >= 3.3");
        } catch (TesseraException e3) {
            check(e3.result() == Result.NO_MATCHING_CONFIG, "exact core refused only as NO_MATCHING_CONFIG: " + e3);
        }
    }

    /** One way to say "this GPU" (ABI 0.14.0): the inventory, selection by id, and who served the context. */
    static void adapters() {
        final java.util.List<AdapterInfo> list = Tessera.enumerateAdapters();
        for (AdapterInfo a : list) {
            System.out.println("  adapter " + a);
            check(!a.id().isEmpty() && a.id().length() <= 47 && !a.backend().isEmpty(), "adapter fields");
        }
        // The context says who served it (the id is "" where the backend cannot tell, e.g. GLX).
        try (Context ctx = Context.create(ContextConfig.builder().build())) {
            final ContextInfo info = ctx.info();
            System.out.println("  served by '" + info.adapter() + "' class " + info.adapterClass());
            if (!info.adapter().isEmpty()) {
                check(list.stream().anyMatch(a -> a.id().equals(info.adapter())), "the serving adapter is in the inventory");
            }
        }
        // Asking for an adapter by id: the one that served us is honoured where selection exists, never replaced by another.
        for (AdapterInfo a : list) {
            try (Context c = Context.create(ContextConfig.builder().adapter(a.id()).fallback(ContextConfig.Fallback.BEST_EFFORT).build())) {
                check(c.info().adapter().equals(a.id()), "selected adapter " + a.id() + " is the one reported: " + c.info().adapter());
            } catch (TesseraException e) {
                check(e.result() == Result.NO_MATCHING_CONFIG || e.result() == Result.UNSUPPORTED,
                        "an adapter that cannot serve is refused, not replaced: " + e);
            }
        }
        final TesseraException none = expect(TesseraException.class, "adapter of another backend",
                () -> Context.create(ContextConfig.builder().adapter("cgl:0x0000ffff").build()).close());
        check(none.result() == Result.UNSUPPORTED || none.result() == Result.NO_MATCHING_CONFIG, "no such adapter: " + none.result());
        expect(IllegalArgumentException.class, "adapter id too long", () -> ContextConfig.builder().adapter("x".repeat(48)));
        expect(IllegalArgumentException.class, "adapter id not ASCII", () -> ContextConfig.builder().adapter("caf\u00e9"));
        expect(TesseraException.class, "adapter with adapterLuid",
                () -> Context.create(ContextConfig.builder().adapter("luid:00000000-00000001").adapterLuid(1L).build()).close());
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
                if (api.contextInfo(h, new int[15], new String[4]) != Result.INVALID_HANDLE.code()) {
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
        // The lost-context listener can be installed and removed; a healthy context is never reported (a real loss needs a
        // driver reset, which the native tests inject on a fake backend).
        final java.util.concurrent.atomic.AtomicInteger lostEvents = new java.util.concurrent.atomic.AtomicInteger();
        Tessera.setContextLostListener((lost, status) -> lostEvents.incrementAndGet());
        try (Context healthy = Context.create(plain())) {
            check(!healthy.isLost() && healthy.lostStatus() == ResetStatus.NONE, "a fresh context is not lost");
        }
        try (Context robust = Context.create(ContextConfig.builder()
                .flags(ContextConfig.Flags.ROBUST_ACCESS | ContextConfig.Flags.RESET_NOTIFICATION).build())) {
            robust.makeCurrent();
            for (int i = 0; i < 20; i++) {
                check(robust.resetStatus() == ResetStatus.NONE, "no reset is happening");
            }
            check(!robust.isLost() && lostEvents.get() == 0, "polling a healthy context reports nothing");
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
        Tessera.setContextLostListener(null);
        final Context closed = Context.create(plain());
        closed.close();
        expect(IllegalStateException.class, "isLost on a closed context", closed::isLost);
    }

    static void priority() {
        check(ContextConfig.builder().build().priority() == ContextConfig.Priority.DEFAULT, "no priority by default");
        final ContextConfig strict = ContextConfig.builder().priority(ContextConfig.Priority.HIGH).build();
        check(strict.priority() == ContextConfig.Priority.HIGH, "the builder keeps the priority");
        // Strict: either the driver grants exactly HIGH and says so, or the request is refused. Never a silent downgrade.
        try (Context ctx = Context.create(strict)) {
            check(ctx.info().priority() == ContextConfig.Priority.HIGH, "strict means the level the driver reports: " + ctx.info().priority());
        } catch (TesseraException e) {
            check(e.result() == Result.NO_MATCHING_CONFIG, "refused only as NO_MATCHING_CONFIG: " + e);
        }
        // Best effort always yields a context, and what it reports is the driver's answer (DEFAULT where it cannot say).
        try (Context ctx = Context.create(ContextConfig.builder().priority(ContextConfig.Priority.LOW)
                .fallback(ContextConfig.Fallback.BEST_EFFORT).build())) {
            System.out.println("  best-effort LOW -> " + ctx.info().priority());
        }
    }

    static void swapControl() {
        // A pbuffer has no swap interval: the query succeeds and says so.
        try (Context pb = Context.create(ContextConfig.builder().surface(ContextConfig.Surface.PBUFFER)
                .fallback(ContextConfig.Fallback.BEST_EFFORT).build())) {
            pb.makeCurrent();
            final SwapControl none = pb.swapControl();
            check(!none.supported() && !none.adaptiveSupported() && none.applied() == null, "a pbuffer has no swap control: " + none);
            Context.releaseCurrent();
        } catch (TesseraException e) {
            check(e.result() == Result.NO_MATCHING_CONFIG, "pbuffer refused only as NO_MATCHING_CONFIG: " + e);
        }
        // The default (hidden-window) context reports what the driver offers, consistently.
        try (Context ctx = Context.create(ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).build())) {
            expect(IllegalStateException.class, "swapControl needs a current context", ctx::swapControl);
            ctx.makeCurrent();
            final SwapControl sc = ctx.swapControl();
            System.out.println("  " + sc);
            check(sc.requested() == null, "nothing requested yet");
            if (sc.supported()) {
                ctx.setSwapInterval(0);
                final SwapControl after = ctx.swapControl();
                check(Integer.valueOf(0).equals(after.requested()), "requested follows the call: " + after);
                if (after.readBack()) {
                    check(Integer.valueOf(0).equals(after.applied()), "the driver reports what was set: " + after);
                }
            } else {
                final TesseraException e = expect(TesseraException.class, "setSwapInterval without swap control", () -> ctx.setSwapInterval(1));
                if (e != null) {
                    check(e.result() == Result.UNSUPPORTED, "UNSUPPORTED, was " + e.result());
                }
            }
            Context.releaseCurrent();
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

        // The described form: a Wayland window is refused without its display or size, and the kinds nobody implements say so.
        final TesseraException noDisplay = expect(TesseraException.class, "wayland without a display",
                () -> Context.createForNativeWindow(plain(), NativeWindow.wayland(0L, 0x1000L, 64, 64), false).close());
        if (noDisplay != null) {
            check(noDisplay.result() == Result.INVALID_ARGUMENT, "wayland without display -> INVALID_ARGUMENT, was " + noDisplay.result());
        }
        final TesseraException noSize = expect(TesseraException.class, "wayland without a size",
                () -> Context.createForNativeWindow(plain(), NativeWindow.wayland(0x2000L, 0x1000L, 0, 64), false).close());
        if (noSize != null) {
            check(noSize.result() == Result.INVALID_ARGUMENT, "wayland without size -> INVALID_ARGUMENT, was " + noSize.result());
        }
        final TesseraException cocoa = expect(TesseraException.class, "an NSView",
                () -> Context.createForNativeWindow(plain(), NativeWindow.cocoa(0x1000L), false).close());
        if (cocoa != null) {
            check(cocoa.result() == Result.UNSUPPORTED, "an NSView -> UNSUPPORTED, was " + cocoa.result());
        }
        final TesseraException zero = expect(TesseraException.class, "native window 0",
                () -> Context.createForNativeWindow(plain(), NativeWindow.x11(0L), false).close());
        if (zero != null) {
            check(zero.result() == Result.INVALID_ARGUMENT, "window 0 -> INVALID_ARGUMENT, was " + zero.result());
        }
        try (Context ctx = Context.create(plain())) {
            ctx.makeCurrent();
            final TesseraException noWindow = expect(TesseraException.class, "resize without a window", () -> ctx.resizeWindow(10, 10));
            if (noWindow != null) {
                check(noWindow.result() == Result.UNSUPPORTED, "resize on a windowless context -> UNSUPPORTED, was " + noWindow.result());
            }
            final TesseraException badSize = expect(TesseraException.class, "resize to nothing", () -> ctx.resizeWindow(0, 10));
            if (badSize != null) {
                check(badSize.result() == Result.INVALID_ARGUMENT, "zero size -> INVALID_ARGUMENT, was " + badSize.result());
            }
            Context.releaseCurrent();
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

    static void poolTopUp() {
        Tessera.clearPool();
        final ContextConfig cfg = plain();
        check(Tessera.topUpPool(cfg, 2) == 2 && Tessera.pooledContextCount(cfg) == 2, "two contexts pooled for this request");
        check(Tessera.topUpPool(cfg, 2) == 0, "already full: nothing created");
        try (Context c = Context.create(cfg)) {
            check(Tessera.pooledContextCount(cfg) == 1, "create took one");
            check(Tessera.topUpPool(cfg, 2) == 1, "topped up again");
            c.makeCurrent();
            Context.releaseCurrent();
        }
        final ContextConfig other = ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).depthBits(16).build();
        check(Tessera.pooledContextCount(other) == 0, "counts are per request");
        expect(IllegalArgumentException.class, "top-up cannot share", () -> {
            try (Context share = Context.create(cfg)) {
                Tessera.topUpPool(ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).shareWith(share).build(), 1);
            }
        });
        expect(TesseraException.class, "target beyond the cap", () -> Tessera.topUpPool(cfg, 65));
        check(Tessera.clearPool() >= 1 && Tessera.pooledContextCount(cfg) == 0, "cleared");
    }

    /** Sharing explained, loss causes and recreate policies (ABI 0.15.0). */
    static void sharingExplained() {
        try (Context a = Context.create(plain())) {
            try (Context b = Context.create(ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).shareWith(a).build());
                    Context c = Context.create(plain())) {
                final ShareGroup ga = a.shareGroup();
                check(ga.id() == b.shareGroup().id() && ga.id() != c.shareGroup().id(), "group ids: " + ga);
                check(ga.members() == 2 && ga.alive() == 2 && c.shareGroup().members() == 1, "group counts: " + ga);
            }
            check(a.shareGroup().members() == 1, "the group shrinks when a member closes");
            check(a.lossCause() == LossCause.NONE, "a healthy context has no loss cause");
            // The verdict agrees with what a create would do.
            final ShareVerdict same = Tessera.canShare(plain(), a);
            check(same == ShareVerdict.OK && same.canShare(), "the same request can share: " + same);
            final ContextConfig es = ContextConfig.builder().api(ContextConfig.Api.OPENGL_ES).fallback(ContextConfig.Fallback.BEST_EFFORT)
                    .shareWith(a).build();
            final ShareVerdict esVerdict = Tessera.canShare(es, a);
            boolean created;
            try (Context e = Context.create(es)) {
                created = !e.isLost();
            } catch (TesseraException e) {
                created = false;
            }
            // a "no" must fail the create; a "yes" may still fail on the request itself (ES is not available everywhere)
            check(!(created && !esVerdict.canShare()), "a context that was created had been predicted impossible: " + esVerdict);

            // Recreate policies on a healthy context: the same adapter always works; the others work or say why.
            try (Context same2 = a.recreate(RecreatePolicy.SAME_ADAPTER).context()) {
                check(!same2.info().renderer().isEmpty(), "recreated on the same adapter");
            }
            try (Context sw = a.recreate(RecreatePolicy.ALLOW_SOFTWARE).context()) {
                check(!sw.info().renderer().isEmpty(), "recreated, software allowed");
            }
            try (Context hw = a.recreate(RecreatePolicy.ANY_HARDWARE).context()) {
                check((hw.info().adapterClass() & AdapterInfo.SOFTWARE) == 0, "any-hardware never returns a CPU renderer");
            } catch (TesseraException e) {
                check(e.result() == Result.NO_MATCHING_CONFIG, "only a CPU renderer is left: " + e);
            }
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
                    final Class<?> self = Class.forName("io.github.andreasnilsson123.tessera.SelfTest", true, loader);
                    check(self != SelfTest.class, "the second copy really is a different class");
                    self.getMethod("smoke").invoke(null);
                    bindings.add(String.valueOf(Class.forName("io.github.andreasnilsson123.tessera.Tessera", true, loader).getMethod("binding").invoke(null)));
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
                "reset and recreate", "swap control", "context priority", "sharing", "probe and pre-warming", "keeping the pool full", "native handles", "window attach (validation)", "GPU selection", "virtual threads",
                "classloaders"};
        final Body[] bodies = {SelfTest::configAndInfo, SelfTest::threadRules, SelfTest::closeFromAnotherThread, SelfTest::cleanerSafetyNet,
                SelfTest::logging, SelfTest::resetAndRecreate, SelfTest::swapControl, SelfTest::priority, SelfTest::sharing, SelfTest::probeAndPool, SelfTest::poolTopUp, SelfTest::nativeHandles,
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
        return io.github.andreasnilsson123.tessera.internal.Natives.fallbackNote();
    }
}
