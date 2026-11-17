package dev.tessera;

import java.util.concurrent.CountDownLatch;

/**
 * The JVM must exit promptly even when contexts are still alive and one is still current on a parked thread: the shutdown hook
 * stops native log callbacks and terminates the library, destroying whatever is left. (ctest enforces the time limit.)
 */
public final class ShutdownTest {
    public static void main(String[] args) throws Exception {
        Tessera.setLogger((level, message) -> {
        });
        final CountDownLatch held = new CountDownLatch(1);
        final Context leaked = Context.create(ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).build());
        final Thread parked = new Thread(() -> {
            leaked.makeCurrent();
            held.countDown();
            try {
                Thread.sleep(Long.MAX_VALUE);
            } catch (InterruptedException ignored) {
                // exiting
            }
        }, "parked-holder");
        parked.setDaemon(true);
        parked.start();
        held.await();
        // A second context nobody ever closes or uses again.
        Context.create(ContextConfig.builder().fallback(ContextConfig.Fallback.BEST_EFFORT).build());
        System.out.println("ShutdownTest: exiting with 2 live contexts, one current on another thread");
    }
}
