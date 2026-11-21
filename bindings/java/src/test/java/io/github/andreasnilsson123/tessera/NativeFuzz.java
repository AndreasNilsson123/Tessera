// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

import io.github.andreasnilsson123.tessera.internal.NativeApi;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;

/**
 * Seeded fuzzing of the glue between Java and the C API: every {@link NativeApi} entry point (JNI or FFM, per
 * {@code -Dtessera.binding}) is called with hostile arguments: made-up, stale and live handles, null / empty / short / long
 * arrays, extreme and random integers, odd strings. The JVM must neither crash nor exit; with {@code -Xcheck:jni} (the ctest
 * entry adds it) any JNI misuse prints a warning, which the test treats as a failure. Java-side exceptions from bad array
 * sizes are tolerated (the FFM binding indexes the arrays it is given; the public API never passes a short one) but counted.
 *
 * <p>Usage: {@code NativeFuzz [iterations] [seed]}. Exits 77 where no context can be created at all.
 */
public final class NativeFuzz {
    private static final long[] HOSTILE_HANDLES = {0L, 1L, -1L, Long.MAX_VALUE, Long.MIN_VALUE, 0xDEADBEEFL, 1L << 32, 0xFFFFFFFFL};
    private static final int[] HOSTILE_INTS = {0, 1, -1, 2, 3, 4, 7, 8, 63, 64, 65, 255, 256, 0x7FFFFFFF, 0x80000000, -2, 1 << 16};

    private final Random rnd;
    private final NativeApi api;
    private final List<Long> live = new ArrayList<>();
    private int javaExceptions;
    private int calls;

    private NativeFuzz(long seed, NativeApi api) {
        this.rnd = new Random(seed);
        this.api = api;
    }

    private int anyInt() {
        return rnd.nextInt(3) == 0 ? HOSTILE_INTS[rnd.nextInt(HOSTILE_INTS.length)] : rnd.nextInt();
    }

    private long anyHandle() {
        final int pick = rnd.nextInt(4);
        if (pick == 0 && !live.isEmpty()) {
            return live.get(rnd.nextInt(live.size()));
        }
        if (pick == 1) {
            return HOSTILE_HANDLES[rnd.nextInt(HOSTILE_HANDLES.length)];
        }
        if (pick == 2 && !live.isEmpty()) {
            return live.get(rnd.nextInt(live.size())) ^ (1L << rnd.nextInt(64)); // one bit off a real handle
        }
        return rnd.nextLong();
    }

    /** null, empty, short, exact or long; the contents random. */
    private int[] ints(int exact) {
        final int kind = rnd.nextInt(8);
        if (kind == 0) {
            return null;
        }
        final int len = kind == 1 ? 0 : kind == 2 ? rnd.nextInt(exact + 1) : kind == 3 ? exact + rnd.nextInt(8) : exact;
        final int[] a = new int[len];
        for (int i = 0; i < len; i++) {
            a[i] = anyInt();
        }
        return a;
    }

    private long[] longs(int exact) {
        final int kind = rnd.nextInt(8);
        if (kind == 0) {
            return null;
        }
        final int len = kind == 1 ? 0 : kind == 2 ? rnd.nextInt(exact + 1) : kind == 3 ? exact + rnd.nextInt(8) : exact;
        final long[] a = new long[len];
        for (int i = 0; i < len; i++) {
            a[i] = rnd.nextLong();
        }
        return a;
    }

    private String[] strings(int exact) {
        final int kind = rnd.nextInt(8);
        if (kind == 0) {
            return null;
        }
        return new String[kind == 1 ? 0 : kind == 2 ? rnd.nextInt(exact + 1) : exact + rnd.nextInt(3)];
    }

    /** A plausible config (so creation sometimes succeeds and later calls reach live objects), sometimes corrupted. */
    private int[] config() {
        if (rnd.nextInt(3) == 0) {
            return ints(30);
        }
        final int[] c = new int[30];
        c[0] = 1;                                // OPENGL
        c[6] = 1;                                // best effort
        c[13] = rnd.nextInt(3) == 0 ? rnd.nextInt(9) : 0;
        if (rnd.nextInt(8) == 0) {
            c[rnd.nextInt(30)] = anyInt(); // including the bytes of the adapter id: any bytes, printable or not
        }
        return c;
    }

    private String anyString() {
        final int kind = rnd.nextInt(6);
        if (kind == 0) {
            return null;
        }
        final StringBuilder sb = new StringBuilder();
        final int len = kind == 1 ? 0 : kind == 2 ? 5000 : rnd.nextInt(40);
        final String alphabet = "glGLXwEgl_0129 \t\n/\\.:*%\u0001\u007få中😀";
        for (int i = 0; i < len; i++) {
            sb.append(alphabet.charAt(rnd.nextInt(alphabet.length())));
        }
        if (rnd.nextInt(3) == 0) {
            sb.insert(0, "gl");
        }
        return sb.toString();
    }

    /** -Dtessera.fuzz.ops=0,3,5 restricts the run to those operations (for bisecting a finding); empty = all. */
    private static final java.util.Set<Integer> ONLY = new java.util.HashSet<>();

    static {
        final String ops = System.getProperty("tessera.fuzz.ops", "");
        for (String part : ops.split(",")) {
            if (!part.trim().isEmpty()) {
                ONLY.add(Integer.parseInt(part.trim()));
            }
        }
    }

    /**
     * Takes ownership of a handle the library handed out: kept (so later calls reach a live object) while few are alive,
     * destroyed otherwise. The bound matters: Mesa's D3D12 driver (WSLg) corrupts the heap once about a dozen GLX contexts are
     * alive at the same time (see ROADMAP 10.1), which this test must not mistake for a Tessera bug.
     */
    private void adopt(long[] out) {
        if (out == null || out.length == 0 || out[0] == 0L) {
            return;
        }
        if (live.size() < 5) {
            live.add(out[0]);
        } else {
            api.destroyContext(out[0]);
        }
    }

    /** Counts that reach the pool's code without creating many contexts at once: hostile only on the non-positive side. */
    private int poolCount() {
        final int pick = rnd.nextInt(6);
        return pick == 0 ? Integer.MIN_VALUE : pick == 1 ? -1 : pick == 2 ? 0 : rnd.nextInt(3);
    }

    private void step() {
        calls++;
        final int op = rnd.nextInt(27);
        if (!ONLY.isEmpty() && !ONLY.contains(op) && op != 0 && op != 24) { // creation and release always run: later ops need handles
            return;
        }
        switch (op) {
            case 0: {
                final long[] out = longs(1);
                final int r = api.createContext(config(), rnd.nextInt(4) == 0 ? anyHandle() : 0L, out);
                if (r == 0) {
                    adopt(out);
                }
                break;
            }
            case 1:
                api.destroyContext(anyHandle());
                if (rnd.nextInt(3) == 0 && !live.isEmpty()) {
                    api.destroyContext(live.remove(rnd.nextInt(live.size())));
                }
                break;
            case 2:
                api.contextInfo(anyHandle(), ints(14), strings(3));
                break;
            case 3:
                api.makeCurrent(anyHandle());
                break;
            case 4:
                api.swapBuffers(anyHandle());
                break;
            case 5:
                api.setSwapInterval(anyHandle(), anyInt());
                break;
            case 6:
                api.resetStatus(anyHandle(), ints(1));
                break;
            case 7:
                api.swapControl(anyHandle(), ints(4));
                break;
            case 8: {
                final long[] out = longs(1);
                if (api.recreate(anyHandle(), out, ints(1)) == 0) {
                    adopt(out);
                }
                break;
            }
            case 9:
                api.isContextLost(anyHandle(), ints(2));
                break;
            case 10:
                api.nativeHandles(anyHandle(), ints(1), longs(6));
                break;
            case 11:
                api.prewarm(config(), poolCount());
                api.poolClear();
                break;
            case 12:
                api.poolTopUp(config(), poolCount(), ints(1));
                api.poolClear();
                break;
            case 13:
                api.pooledCountFor(config(), ints(1));
                break;
            case 14:
                api.poolClear();
                break;
            case 15:
                api.pooledCount();
                break;
            case 16:
                api.probe(config(), ints(14), strings(5));
                break;
            case 17: {
                final long[] out = longs(1);
                if (api.createContextForWindow(config(), anyHandle(), rnd.nextLong(), anyInt(), out) == 0) {
                    adopt(out);
                }
                break;
            }
            case 23: {
                final long[] out = longs(1);
                if (api.createContextForNativeWindow(config(), anyHandle(), rnd.nextInt(6), rnd.nextLong(), 0L /* a display is a trusted pointer */,
                        anyInt(), anyInt(), anyInt(), out) == 0) {
                    adopt(out);
                }
                api.resizeWindow(anyHandle(), anyInt(), anyInt());
                break;
            }
            case 18: {
                final String name = anyString();
                if (name != null) {
                    api.procAddress(name);
                }
                break;
            }
            case 19:
                api.resultString(anyInt());
                break;
            case 20:
                api.lastError();
                break;
            case 21:
                api.currentContext();
                break;
            case 22:
                api.setLogSink(rnd.nextBoolean() ? (level, message) -> {
                } : null);
                api.setLostSink(rnd.nextBoolean() ? (handle, status) -> {
                } : null);
                break;
            case 25: { // the adapter inventory, with hostile indexes and arrays
                api.adapterCount();
                api.adapter(rnd.nextInt(4) == 0 ? anyInt() : rnd.nextInt(3), ints(rnd.nextInt(3) == 0 ? rnd.nextInt(8) : 5),
                        strings(rnd.nextInt(3) == 0 ? rnd.nextInt(5) : 3));
                break;
            }
            case 26: { // loss cause, share group, sharing preflight and recreate policies (ABI 0.15.0), hostile arrays and handles
                api.lossCause(anyHandle(), ints(rnd.nextInt(3) == 0 ? rnd.nextInt(3) : 1));
                api.shareGroup(anyHandle(), longs(rnd.nextInt(3) == 0 ? rnd.nextInt(3) : 1), ints(rnd.nextInt(3) == 0 ? rnd.nextInt(4) : 2));
                api.canShare(config(), anyHandle(), ints(rnd.nextInt(3) == 0 ? rnd.nextInt(3) : 1));
                final long[] out = longs(1);
                if (api.recreateWithPolicy(anyHandle(), rnd.nextInt(3) == 0 ? anyInt() : rnd.nextInt(3), out, ints(1)) == 0) {
                    adopt(out);
                }
                break;
            }
            default: // 24
                api.makeCurrent(0L); // release whatever this thread holds
                break;
        }
    }

    public static void main(String[] args) {
        final int iterations = args.length > 0 ? Integer.parseInt(args[0]) : 3000;
        final long seed = args.length > 1 ? Long.parseLong(args[1]) : System.nanoTime();
        System.out.println("NativeFuzz: " + iterations + " iterations, seed " + seed + " (rerun with: NativeFuzz " + iterations + " " + seed + ")");

        final NativeApi api;
        try {
            api = Tessera.apiInitialized();
            final long[] probe = new long[1];
            final int[] cfg = new int[30];
            cfg[0] = 1;
            cfg[6] = 1;
            if (api.createContext(cfg, 0L, probe) != 0) {
                System.out.println("NativeFuzz: no context can be created here, skipped");
                System.exit(77);
            }
            api.destroyContext(probe[0]);
        } catch (RuntimeException e) {
            System.out.println("NativeFuzz: no usable binding here (" + e + "), skipped");
            System.exit(77);
            return;
        }

        final NativeFuzz fuzz = new NativeFuzz(seed, api);
        for (int i = 0; i < iterations; i++) {
            try {
                fuzz.step();
            } catch (ArrayIndexOutOfBoundsException | NullPointerException | IllegalArgumentException expected) {
                fuzz.javaExceptions++; // a hostile array reached Java-side indexing: legitimate, the public API never does this
            }
        }
        for (long h : fuzz.live) {
            api.destroyContext(h);
        }
        api.makeCurrent(0L);
        api.setLogSink(null);
        api.setLostSink(null);
        api.poolClear();
        System.out.println("NativeFuzz: OK (" + fuzz.calls + " calls, " + fuzz.javaExceptions + " tolerated Java-side exceptions)");
    }
}
