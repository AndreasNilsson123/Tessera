// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera.internal;

import java.nio.file.Path;

/** {@link NativeApi} over JNI: the glue lives inside the Tessera shared library itself (tessera_jni.cpp). */
public final class JniNativeApi implements NativeApi {

    private static volatile LogSink sink;
    private static volatile LostSink lostSink;

    /** Loads the library for this classloader; throws {@link UnsatisfiedLinkError} if that is impossible. */
    public JniNativeApi(Path library) {
        NativeLoader.loadJni(library);
    }

    @Override
    public String name() {
        return "jni";
    }

    @Override
    public int init() {
        return nInit();
    }

    @Override
    public int terminate() {
        return nTerminate();
    }

    @Override
    public int abiVersion() {
        return nAbiVersion();
    }

    @Override
    public void version(int[] out3) {
        nVersion(out3);
    }

    @Override
    public int createContext(int[] cfg, long shareWith, long[] outHandle) {
        return nCreateContext(cfg, shareWith, outHandle);
    }

    @Override
    public int destroyContext(long handle) {
        return nDestroyContext(handle);
    }

    @Override
    public int contextInfo(long handle, int[] ints, String[] strings) {
        return nContextInfo(handle, ints, strings);
    }

    @Override
    public int recreateWithPolicy(long handle, int policy, long[] outHandle, int[] outFlags) {
        return nRecreateWithPolicy(handle, policy, outHandle, outFlags);
    }

    @Override
    public int lossCause(long handle, int[] out1) {
        return nLossCause(handle, out1);
    }

    @Override
    public int canShare(int[] cfg, long parent, int[] out1) {
        return nCanShare(cfg, parent, out1);
    }

    @Override
    public int shareGroup(long handle, long[] ids1, int[] counts2) {
        return nShareGroup(handle, ids1, counts2);
    }

    @Override
    public int adapterCount() {
        return nAdapterCount();
    }

    @Override
    public int adapter(int index, int[] ints5, String[] strings3) {
        return nAdapter(index, ints5, strings3);
    }

    @Override
    public int makeCurrent(long handle) {
        return nMakeCurrent(handle);
    }

    @Override
    public long currentContext() {
        return nCurrentContext();
    }

    @Override
    public long procAddress(String name) {
        return nProcAddress(name);
    }

    @Override
    public int swapBuffers(long handle) {
        return nSwapBuffers(handle);
    }

    @Override
    public int setSwapInterval(long handle, int interval) {
        return nSetSwapInterval(handle, interval);
    }

    @Override
    public int resetStatus(long handle, int[] out1) {
        return nResetStatus(handle, out1);
    }

    @Override
    public int swapControl(long handle, int[] out4) {
        return nSwapControl(handle, out4);
    }

    @Override
    public int isContextLost(long handle, int[] out2) {
        return nIsContextLost(handle, out2);
    }

    @Override
    public void setLostSink(LostSink newSink) {
        lostSink = newSink;
        nSetLostSink(newSink != null);
    }

    /** Called from native code on whatever thread detected the loss. */
    @SuppressWarnings("unused")
    private static void onContextLost(long handle, int status) {
        final LostSink s = lostSink;
        if (s != null) {
            try {
                s.lost(handle, status);
            } catch (Throwable ignored) {
                // a listener that throws must not unwind into native code
            }
        }
    }

    @Override
    public int recreate(long handle, long[] outHandle, int[] outFlags) {
        return nRecreate(handle, outHandle, outFlags);
    }

    @Override
    public int nativeHandles(long handle, int[] kindOut1, long[] handlesOut6) {
        return nNativeHandles(handle, kindOut1, handlesOut6);
    }

    @Override
    public int prewarm(int[] cfg, int count) {
        return nPrewarm(cfg, count);
    }

    @Override
    public int pooledCount() {
        return nPooledCount();
    }

    @Override
    public int poolTopUp(int[] cfg, int target, int[] created1) {
        return nPoolTopUp(cfg, target, created1);
    }

    @Override
    public int pooledCountFor(int[] cfg, int[] count1) {
        return nPooledCountFor(cfg, count1);
    }

    @Override
    public int poolClear() {
        return nPoolClear();
    }

    @Override
    public int createContextForNativeWindow(int[] cfg, long shareWith, int kind, long window, long display, int width, int height,
                                            int flags, long[] outHandle) {
        return nCreateContextForNativeWindow(cfg, shareWith, kind, window, display, width, height, flags, outHandle);
    }

    @Override
    public int resizeWindow(long handle, int width, int height) {
        return nResizeWindow(handle, width, height);
    }

    @Override
    public int createContextForWindow(int[] cfg, long shareWith, long window, int flags, long[] outHandle) {
        return nCreateContextForWindow(cfg, shareWith, window, flags, outHandle);
    }

    @Override
    public int probe(int[] cfg, int[] infoInts, String[] strings5) {
        return nProbe(cfg, infoInts, strings5);
    }

    @Override
    public String lastError() {
        return nLastError();
    }

    @Override
    public String resultString(int code) {
        return nResultString(code);
    }

    @Override
    public void setLogSink(LogSink newSink) {
        sink = newSink;
        nSetLogSink(newSink != null);
    }

    /** Called from native code on whatever thread logged. */
    @SuppressWarnings("unused")
    private static void onLog(int level, String message) {
        final LogSink s = sink;
        if (s != null) {
            try {
                s.log(level, message);
            } catch (Throwable ignored) {
                // a logger that throws must not unwind into native code
            }
        }
    }

    private static native int nNativeHandles(long handle, int[] kindOut1, long[] handlesOut6);

    private static native int nPrewarm(int[] cfg, int count);

    private static native int nPooledCount();

    private static native int nPoolTopUp(int[] cfg, int target, int[] created1);

    private static native int nPooledCountFor(int[] cfg, int[] count1);

    private static native int nPoolClear();

    private static native int nCreateContextForNativeWindow(int[] cfg, long shareWith, int kind, long window, long display, int width,
                                                            int height, int flags, long[] outHandle);

    private static native int nResizeWindow(long handle, int width, int height);

    private static native int nCreateContextForWindow(int[] cfg, long shareWith, long window, int flags, long[] outHandle);

    private static native int nProbe(int[] cfg, int[] infoInts, String[] strings5);

    private static native int nInit();

    private static native int nTerminate();

    private static native int nAbiVersion();

    private static native void nVersion(int[] out3);

    private static native int nCreateContext(int[] cfg, long shareWith, long[] outHandle);

    private static native int nDestroyContext(long handle);

    private static native int nContextInfo(long handle, int[] ints, String[] strings);

    private static native int nRecreateWithPolicy(long handle, int policy, long[] outHandle, int[] outFlags);

    private static native int nLossCause(long handle, int[] out1);

    private static native int nCanShare(int[] cfg, long parent, int[] out1);

    private static native int nShareGroup(long handle, long[] ids1, int[] counts2);

    private static native int nAdapterCount();

    private static native int nAdapter(int index, int[] ints5, String[] strings3);

    private static native int nMakeCurrent(long handle);

    private static native long nCurrentContext();

    private static native long nProcAddress(String name);

    private static native int nSwapBuffers(long handle);

    private static native int nSetSwapInterval(long handle, int interval);

    private static native int nResetStatus(long handle, int[] out1);

    private static native int nSwapControl(long handle, int[] out4);

    private static native int nRecreate(long handle, long[] outHandle, int[] outFlags);

    private static native String nLastError();

    private static native String nResultString(int code);

    private static native void nSetLogSink(boolean enable);

    private static native int nIsContextLost(long handle, int[] out2);

    private static native void nSetLostSink(boolean enable);
}
