// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Context-lost events through the real native upcalls (JNI or FFM, per {@code -Dtessera.binding}). The library is built with the
 * test hook {@code TESSERA_TEST_HOOKS}, and the environment variable {@code TESSERA_TEST_LOSE_ON_SWAP=1} makes the next
 * {@code swapBuffers} report a lost context, which is exactly what a driver's {@code CONTEXT_LOST} looks like to the core.
 * Exits 77 (skipped) in a library without the hook.
 */
public final class LostEventsTest {
    private static int failures;

    private static void check(boolean ok, String what) {
        if (!ok) {
            System.out.println("FAILED: " + what);
            failures++;
        }
    }

    public static void main(String[] args) throws Exception {
        final ContextConfig cfg = ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).build();
        final List<String> events = new CopyOnWriteArrayList<>();
        final List<Thread> threads = new CopyOnWriteArrayList<>();
        final List<Context> replacements = new CopyOnWriteArrayList<>();
        final AtomicInteger throwing = new AtomicInteger();

        // A first probe: does this library have the hook at all?
        try (Context probe = Context.create(cfg)) {
            probe.makeCurrent();
            try {
                probe.swapBuffers();
                System.out.println("LostEventsTest: no test hook in this library (or TESSERA_TEST_LOSE_ON_SWAP is not set): skipped");
                Context.releaseCurrent();
                System.exit(77);
            } catch (ContextLostException expected) {
                // the hook is there
            }
            Context.releaseCurrent();
        }

        Tessera.setContextLostListener((lost, status) -> {
            events.add(status.toString());
            threads.add(Thread.currentThread());
            // The "re-create on loss" helper from the documentation: it runs inside the upcall and uses the API.
            final Context.Recreation r = lost.recreate();
            replacements.add(r.context());
            check(lost.isLost(), "the context is already marked lost inside the listener");
        });

        // Detected at swap: one event, on the calling thread, with the status the core assigned; later calls stay quiet.
        events.clear();
        threads.clear();
        try (Context ctx = Context.create(cfg)) {
            check(!ctx.isLost(), "healthy before");
            ctx.makeCurrent();
            try {
                ctx.swapBuffers();
                check(false, "swap should report a lost context");
            } catch (ContextLostException expected) {
                // as designed
            }
            check(events.size() == 1 && events.get(0).equals("GUILTY"), "one GUILTY event: " + events);
            check(threads.size() == 1 && threads.get(0) == Thread.currentThread(), "the event arrives on the detecting thread");
            check(ctx.isLost() && ctx.lostStatus() == ResetStatus.GUILTY, "isLost / lostStatus agree: " + ctx.lostStatus());
            try {
                ctx.swapBuffers();
            } catch (ContextLostException expected) {
                // still lost
            }
            check(events.size() == 1, "no second event for the same context: " + events);
            Context.releaseCurrent();
        }
        check(replacements.size() == 1, "the listener recreated the context once");
        for (Context r : replacements) {
            check(!r.isLost(), "the replacement is healthy");
            r.close();
        }
        replacements.clear();

        // A listener that throws must not unwind into native code: the swap still fails with CONTEXT_LOST, nothing crashes.
        Tessera.setContextLostListener((lost, status) -> {
            throwing.incrementAndGet();
            throw new IllegalStateException("a misbehaving listener");
        });
        try (Context ctx = Context.create(cfg)) {
            ctx.makeCurrent();
            try {
                ctx.swapBuffers();
                check(false, "swap should report a lost context");
            } catch (ContextLostException expected) {
                // as designed
            }
            check(throwing.get() == 1, "the throwing listener ran once");
            Context.releaseCurrent();
        }

        // Removed: silent.
        Tessera.setContextLostListener(null);
        events.clear();
        try (Context ctx = Context.create(cfg)) {
            ctx.makeCurrent();
            try {
                ctx.swapBuffers();
            } catch (ContextLostException expected) {
                // as designed
            }
            check(events.isEmpty() && ctx.isLost(), "no listener, no event, but the state is still queryable");
            Context.releaseCurrent();
        }

        // Another thread detects it: the event arrives there.
        Tessera.setContextLostListener((lost, status) -> threads.add(Thread.currentThread()));
        threads.clear();
        final Thread worker = new Thread(() -> {
            try (Context ctx = Context.create(cfg)) {
                ctx.makeCurrent();
                try {
                    ctx.swapBuffers();
                } catch (ContextLostException expected) {
                    // as designed
                }
                Context.releaseCurrent();
            }
        }, "loss-detector");
        worker.start();
        worker.join();
        check(threads.size() == 1 && threads.get(0) == worker, "detected on a worker thread, delivered on it: " + threads);
        Tessera.setContextLostListener(null);

        System.out.println(failures == 0 ? "LostEventsTest: OK" : "LostEventsTest: " + failures + " FAILED");
        System.exit(failures == 0 ? 0 : 1);
    }
}
