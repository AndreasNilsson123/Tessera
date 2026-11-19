// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera.internal;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.nio.file.Path;

/**
 * {@link NativeApi} over java.lang.foreign (final since Java 22). Compiled with {@code --release 22} and stored under
 * {@code META-INF/versions/22} of the jar, so older runtimes never see it. Written by hand rather than generated: the C API is
 * small, and the two structures (config, info) are pinned to the layouts in {@code tessera.h} by the offsets below and by the
 * ABI-version check in {@link Natives}.
 *
 * <p>Run with {@code --enable-native-access=ALL-UNNAMED} to avoid the JDK's warning about restricted methods.
 */
final class FfmNativeApi implements NativeApi {

    private static final ValueLayout.OfInt INT = ValueLayout.JAVA_INT;
    private static final ValueLayout.OfLong LONG = ValueLayout.JAVA_LONG;

    // TesseraContextConfig (136 bytes)
    private static final long CONFIG_SIZE = 136;
    private static final long CFG_STRUCT_SIZE = 0, CFG_API = 4, CFG_MAJOR = 8, CFG_MINOR = 12, CFG_PROFILE = 16, CFG_FLAGS = 20,
            CFG_SURFACE = 24, CFG_FALLBACK = 28, CFG_RED = 32, CFG_GREEN = 36, CFG_BLUE = 40, CFG_ALPHA = 44, CFG_DEPTH = 48,
            CFG_STENCIL = 52, CFG_SAMPLES = 56, CFG_SHARE_WITH = 64, CFG_ADAPTER_LUID = 72, CFG_GPU_PREFERENCE = 80;
    // TesseraContextInfo (504 bytes)
    private static final long INFO_SIZE = 504;
    private static final long INFO_STRUCT_SIZE = 0, INFO_API = 4, INFO_MAJOR = 8, INFO_MINOR = 12, INFO_PROFILE = 16, INFO_FLAGS = 20,
            INFO_SURFACE = 24, INFO_SAMPLES = 28, INFO_RED = 32, INFO_GREEN = 36, INFO_BLUE = 40, INFO_ALPHA = 44, INFO_DEPTH = 48,
            INFO_STENCIL = 52, INFO_VENDOR = 56, INFO_RENDERER = 184, INFO_VERSION = 312;

    // TesseraNativeHandles (88 bytes): structSize, kind, handles[6] at 8, reserved[4]
    private static final long NH_SIZE = 88, NH_KIND = 4, NH_HANDLES = 8;
    // TesseraProbeReport (832 bytes): structSize, result, info (a TesseraContextInfo) at 8, backend[32] at 512, message[256] at 544
    private static final long PROBE_SIZE = 832, PROBE_RESULT = 4, PROBE_INFO = 8, PROBE_BACKEND = 512, PROBE_MESSAGE = 544;

    private static volatile LogSink sink;

    private final MethodHandle hInit, hTerminate, hAbi, hVersion, hCreate, hDestroy, hInfo, hMakeCurrent, hCurrent, hProc, hSwap,
            hSwapInterval, hReset, hRecreate, hLastError, hResultString, hSetLog, hNativeHandles, hPrewarm, hPooled, hCreateForWindow, hProbe;
    private final MemorySegment logStub;

    public FfmNativeApi(Path library) {
        final Linker linker = Linker.nativeLinker();
        final SymbolLookup lookup = SymbolLookup.libraryLookup(library, Arena.global());
        hInit = down(linker, lookup, "tesseraInit", FunctionDescriptor.of(INT));
        hTerminate = down(linker, lookup, "tesseraTerminate", FunctionDescriptor.of(INT));
        hAbi = down(linker, lookup, "tesseraGetAbiVersion", FunctionDescriptor.of(INT));
        hVersion = down(linker, lookup, "tesseraGetVersion", FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
        hCreate = down(linker, lookup, "tesseraCreateContext", FunctionDescriptor.of(INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
        hDestroy = down(linker, lookup, "tesseraDestroyContext", FunctionDescriptor.of(INT, LONG));
        hInfo = down(linker, lookup, "tesseraGetContextInfo", FunctionDescriptor.of(INT, LONG, ValueLayout.ADDRESS));
        hMakeCurrent = down(linker, lookup, "tesseraMakeCurrent", FunctionDescriptor.of(INT, LONG));
        hCurrent = down(linker, lookup, "tesseraGetCurrentContext", FunctionDescriptor.of(LONG));
        hProc = down(linker, lookup, "tesseraGetProcAddress", FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.ADDRESS));
        hSwap = down(linker, lookup, "tesseraSwapBuffers", FunctionDescriptor.of(INT, LONG));
        hSwapInterval = down(linker, lookup, "tesseraSetSwapInterval", FunctionDescriptor.of(INT, LONG, INT));
        hReset = down(linker, lookup, "tesseraGetResetStatus", FunctionDescriptor.of(INT, LONG, ValueLayout.ADDRESS));
        hRecreate = down(linker, lookup, "tesseraRecreateContext", FunctionDescriptor.of(INT, LONG, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
        hNativeHandles = down(linker, lookup, "tesseraGetNativeHandles", FunctionDescriptor.of(INT, LONG, ValueLayout.ADDRESS));
        hPrewarm = down(linker, lookup, "tesseraPrewarmContexts", FunctionDescriptor.of(INT, ValueLayout.ADDRESS, INT));
        hPooled = down(linker, lookup, "tesseraPooledContextCount", FunctionDescriptor.of(INT));
        hCreateForWindow = down(linker, lookup, "tesseraCreateContextForWindow",
                FunctionDescriptor.of(INT, ValueLayout.ADDRESS, LONG, INT, ValueLayout.ADDRESS));
        hProbe = down(linker, lookup, "tesseraProbe", FunctionDescriptor.of(INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS));
        hLastError = down(linker, lookup, "tesseraGetLastErrorMessage", FunctionDescriptor.of(ValueLayout.ADDRESS));
        hResultString = down(linker, lookup, "tesseraResultString", FunctionDescriptor.of(ValueLayout.ADDRESS, INT));
        hSetLog = down(linker, lookup, "tesseraSetLogCallback", FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.ADDRESS));
        try {
            final MethodHandle target = MethodHandles.lookup().findStatic(FfmNativeApi.class, "onLog",
                    MethodType.methodType(void.class, int.class, MemorySegment.class, MemorySegment.class));
            logStub = linker.upcallStub(target, FunctionDescriptor.ofVoid(INT, ValueLayout.ADDRESS, ValueLayout.ADDRESS), Arena.global());
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException("cannot create the log upcall", e);
        }
    }

    private static MethodHandle down(Linker linker, SymbolLookup lookup, String name, FunctionDescriptor descriptor) {
        return linker.downcallHandle(lookup.find(name).orElseThrow(() -> new IllegalStateException("the native library has no symbol " + name)), descriptor);
    }

    /** Called by native code on whatever thread logged. An exception escaping an upcall would crash the JVM, so nothing escapes. */
    @SuppressWarnings("unused")
    private static void onLog(int level, MemorySegment message, MemorySegment user) {
        try {
            final LogSink s = sink;
            if (s != null && message.address() != 0L) {
                s.log(level, ascii(message.reinterpret(1024).getString(0)));
            }
        } catch (Throwable ignored) {
            // see above
        }
    }

    private static RuntimeException failed(Throwable t) {
        return t instanceof RuntimeException ? (RuntimeException) t : new IllegalStateException("native call failed: " + t, t);
    }

    /** Forces printable ASCII, matching what the JNI binding returns (driver strings are not guaranteed to be valid UTF-8). */
    private static String ascii(String s) {
        final StringBuilder sb = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            final char c = s.charAt(i);
            sb.append(c >= 0x20 && c < 0x7F ? c : '?');
        }
        return sb.toString();
    }

    @Override
    public String name() {
        return "ffm";
    }

    @Override
    public int init() {
        try {
            return (int) hInit.invokeExact();
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int terminate() {
        try {
            return (int) hTerminate.invokeExact();
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int abiVersion() {
        try {
            return (int) hAbi.invokeExact();
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public void version(int[] out3) {
        try (Arena arena = Arena.ofConfined()) {
            final MemorySegment v = arena.allocate(12, 4);
            hVersion.invokeExact(v, v.asSlice(4), v.asSlice(8));
            for (int i = 0; i < 3; i++) {
                out3[i] = v.get(INT, i * 4L);
            }
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    private static MemorySegment config(Arena arena, int[] cfg, long shareWith) {
        final MemorySegment c = arena.allocate(CONFIG_SIZE, 8);
        c.set(INT, CFG_STRUCT_SIZE, (int) CONFIG_SIZE);
        c.set(INT, CFG_API, cfg[0]);
        c.set(INT, CFG_MAJOR, cfg[1]);
        c.set(INT, CFG_MINOR, cfg[2]);
        c.set(INT, CFG_PROFILE, cfg[3]);
        c.set(INT, CFG_FLAGS, cfg[4]);
        c.set(INT, CFG_SURFACE, cfg[5]);
        c.set(INT, CFG_FALLBACK, cfg[6]);
        c.set(INT, CFG_RED, cfg[7]);
        c.set(INT, CFG_GREEN, cfg[8]);
        c.set(INT, CFG_BLUE, cfg[9]);
        c.set(INT, CFG_ALPHA, cfg[10]);
        c.set(INT, CFG_DEPTH, cfg[11]);
        c.set(INT, CFG_STENCIL, cfg[12]);
        c.set(INT, CFG_SAMPLES, cfg[13]);
        c.set(LONG, CFG_SHARE_WITH, shareWith);
        c.set(INT, CFG_GPU_PREFERENCE, cfg[14]);
        c.set(LONG, CFG_ADAPTER_LUID, (cfg[15] & 0xFFFFFFFFL) | ((long) cfg[16] << 32));
        return c;
    }

    private static void readInfo(MemorySegment info, long base, int[] ints, String[] strings) {
        ints[0] = info.get(INT, base + INFO_API);
        ints[1] = info.get(INT, base + INFO_MAJOR);
        ints[2] = info.get(INT, base + INFO_MINOR);
        ints[3] = info.get(INT, base + INFO_PROFILE);
        ints[4] = info.get(INT, base + INFO_FLAGS);
        ints[5] = info.get(INT, base + INFO_SURFACE);
        ints[6] = info.get(INT, base + INFO_SAMPLES);
        ints[7] = info.get(INT, base + INFO_RED);
        ints[8] = info.get(INT, base + INFO_GREEN);
        ints[9] = info.get(INT, base + INFO_BLUE);
        ints[10] = info.get(INT, base + INFO_ALPHA);
        ints[11] = info.get(INT, base + INFO_DEPTH);
        ints[12] = info.get(INT, base + INFO_STENCIL);
        strings[0] = ascii(info.getString(base + INFO_VENDOR));
        strings[1] = ascii(info.getString(base + INFO_RENDERER));
        strings[2] = ascii(info.getString(base + INFO_VERSION));
    }

    @Override
    public int createContext(int[] cfg, long shareWith, long[] outHandle) {
        try (Arena arena = Arena.ofConfined()) {
            final MemorySegment c = config(arena, cfg, shareWith);
            final MemorySegment out = arena.allocate(8, 8);
            final int r = (int) hCreate.invokeExact(c, out);
            outHandle[0] = out.get(LONG, 0);
            return r;
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int createContextForWindow(int[] cfg, long shareWith, long window, int flags, long[] outHandle) {
        try (Arena arena = Arena.ofConfined()) {
            final MemorySegment c = config(arena, cfg, shareWith);
            final MemorySegment out = arena.allocate(8, 8);
            final int r = (int) hCreateForWindow.invokeExact(c, window, flags, out);
            outHandle[0] = out.get(LONG, 0);
            return r;
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int nativeHandles(long handle, int[] kindOut1, long[] handlesOut6) {
        try (Arena arena = Arena.ofConfined()) {
            final MemorySegment h = arena.allocate(NH_SIZE, 8);
            h.set(INT, 0, (int) NH_SIZE);
            final int r = (int) hNativeHandles.invokeExact(handle, h);
            if (r != 0) {
                return r;
            }
            kindOut1[0] = h.get(INT, NH_KIND);
            for (int i = 0; i < 6; i++) {
                handlesOut6[i] = h.get(LONG, NH_HANDLES + 8L * i);
            }
            return 0;
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int prewarm(int[] cfg, int count) {
        try (Arena arena = Arena.ofConfined()) {
            return (int) hPrewarm.invokeExact(config(arena, cfg, 0L), count);
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int pooledCount() {
        try {
            return (int) hPooled.invokeExact();
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int probe(int[] cfg, int[] infoInts, String[] strings5) {
        try (Arena arena = Arena.ofConfined()) {
            final MemorySegment report = arena.allocate(PROBE_SIZE, 8);
            report.set(INT, 0, (int) PROBE_SIZE);
            report.set(INT, PROBE_INFO + INFO_STRUCT_SIZE, (int) INFO_SIZE);
            final int r = (int) hProbe.invokeExact(config(arena, cfg, 0L), report);
            final String backend = ascii(report.getString(PROBE_BACKEND));
            final String message = ascii(report.getString(PROBE_MESSAGE));
            if (r == -2 && backend.isEmpty() && message.isEmpty()) {
                return r; // INVALID_ARGUMENT before the probe ran: the report was not filled in
            }
            readInfo(report, PROBE_INFO, infoInts, strings5);
            strings5[3] = backend;
            strings5[4] = message;
            return r;
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int destroyContext(long handle) {
        try {
            return (int) hDestroy.invokeExact(handle);
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int contextInfo(long handle, int[] ints, String[] strings) {
        try (Arena arena = Arena.ofConfined()) {
            final MemorySegment info = arena.allocate(INFO_SIZE, 8);
            info.set(INT, INFO_STRUCT_SIZE, (int) INFO_SIZE);
            final int r = (int) hInfo.invokeExact(handle, info);
            if (r != 0) {
                return r;
            }
            ints[0] = info.get(INT, INFO_API);
            ints[1] = info.get(INT, INFO_MAJOR);
            ints[2] = info.get(INT, INFO_MINOR);
            ints[3] = info.get(INT, INFO_PROFILE);
            ints[4] = info.get(INT, INFO_FLAGS);
            ints[5] = info.get(INT, INFO_SURFACE);
            ints[6] = info.get(INT, INFO_SAMPLES);
            ints[7] = info.get(INT, INFO_RED);
            ints[8] = info.get(INT, INFO_GREEN);
            ints[9] = info.get(INT, INFO_BLUE);
            ints[10] = info.get(INT, INFO_ALPHA);
            ints[11] = info.get(INT, INFO_DEPTH);
            ints[12] = info.get(INT, INFO_STENCIL);
            strings[0] = ascii(info.getString(INFO_VENDOR));
            strings[1] = ascii(info.getString(INFO_RENDERER));
            strings[2] = ascii(info.getString(INFO_VERSION));
            return 0;
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int makeCurrent(long handle) {
        try {
            return (int) hMakeCurrent.invokeExact(handle);
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public long currentContext() {
        try {
            return (long) hCurrent.invokeExact();
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public long procAddress(String name) {
        try (Arena arena = Arena.ofConfined()) {
            final MemorySegment p = (MemorySegment) hProc.invokeExact(arena.allocateFrom(name));
            return p.address();
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int swapBuffers(long handle) {
        try {
            return (int) hSwap.invokeExact(handle);
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int setSwapInterval(long handle, int interval) {
        try {
            return (int) hSwapInterval.invokeExact(handle, interval);
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int resetStatus(long handle, int[] out1) {
        try (Arena arena = Arena.ofConfined()) {
            final MemorySegment out = arena.allocate(4, 4);
            final int r = (int) hReset.invokeExact(handle, out);
            out1[0] = out.get(INT, 0);
            return r;
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public int recreate(long handle, long[] outHandle, int[] outFlags) {
        try (Arena arena = Arena.ofConfined()) {
            final MemorySegment h = arena.allocate(8, 8);
            final MemorySegment f = arena.allocate(4, 4);
            final int r = (int) hRecreate.invokeExact(handle, h, f);
            outHandle[0] = h.get(LONG, 0);
            outFlags[0] = f.get(INT, 0);
            return r;
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public String lastError() {
        try {
            final MemorySegment p = (MemorySegment) hLastError.invokeExact();
            return ascii(p.reinterpret(1024).getString(0));
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public String resultString(int code) {
        try {
            final MemorySegment p = (MemorySegment) hResultString.invokeExact(code);
            return ascii(p.reinterpret(256).getString(0));
        } catch (Throwable t) {
            throw failed(t);
        }
    }

    @Override
    public void setLogSink(LogSink newSink) {
        sink = newSink;
        try {
            final MemorySegment callback = newSink == null ? MemorySegment.NULL : logStub; // invokeExact needs exact static types
            hSetLog.invokeExact(callback, MemorySegment.NULL);
        } catch (Throwable t) {
            throw failed(t);
        }
    }
}
