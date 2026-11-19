// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera.internal;

/**
 * The C API as the Java layer sees it, implemented twice: {@link JniNativeApi} (Java 11+) and, from Java 22,
 * {@code FfmNativeApi} (java.lang.foreign). Everything returns the native {@code TesseraResult} code; out-parameters are
 * arrays. Not public API: use {@code io.github.andreasnilsson123.tessera.Tessera} and {@code io.github.andreasnilsson123.tessera.Context}.
 */
public interface NativeApi {

    /** Receives native log messages: level 1 = error ... 4 = debug. */
    interface LogSink {
        void log(int level, String message);
    }

    /** "jni" or "ffm". */
    String name();

    int init();

    int terminate();

    int abiVersion();

    /** major, minor, patch. */
    void version(int[] out3);

    /** cfg layout: see {@code ContextConfig#toNative()}. */
    int createContext(int[] cfg, long shareWith, long[] outHandle);

    int destroyContext(long handle);

    /** ints: api, major, minor, profile, flags, surface, samples, r, g, b, a, depth, stencil; strings: vendor, renderer, version. */
    int contextInfo(long handle, int[] ints, String[] strings);

    int makeCurrent(long handle);

    long currentContext();

    long procAddress(String name);

    int swapBuffers(long handle);

    int setSwapInterval(long handle, int interval);

    int resetStatus(long handle, int[] out1);

    int recreate(long handle, long[] outHandle, int[] outFlags);

    /** kind (1 WGL, 2 GLX, 3 EGL, 4 CGL), then up to six handles (see tessera.h for the meaning per kind). */
    int nativeHandles(long handle, int[] kindOut1, long[] handlesOut6);

    int prewarm(int[] cfg, int count);

    int pooledCount();

    /** Like {@link #createContext} for a caller's window; {@code flags} bit 0 = allow setting the pixel format. */
    int createContextForWindow(int[] cfg, long shareWith, long window, int flags, long[] outHandle);

    /**
     * Returns the probe's result code. infoInts as in {@link #contextInfo}; strings: vendor, renderer, version, backend, message.
     * The report is filled whether or not the request could be met; if the arguments themselves were rejected the strings stay null.
     */
    int probe(int[] cfg, int[] infoInts, String[] strings5);

    /** Message for the last failure on the calling thread. Only meaningful right after a failing call, on the same thread. */
    String lastError();

    String resultString(int code);

    /** {@code null} stops forwarding. */
    void setLogSink(LogSink sink);
}
