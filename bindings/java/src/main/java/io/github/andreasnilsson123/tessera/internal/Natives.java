// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera.internal;

import java.io.IOException;
import java.nio.file.Path;

/**
 * Chooses and loads the binding, once per classloader. {@code -Dtessera.binding=auto|jni|ffm} (default auto: FFM on Java 22+
 * when it can be set up, otherwise JNI).
 */
public final class Natives {
    /** The ABI version this Java layer was written against ({@code TESSERA_ABI_VERSION}). */
    public static final int EXPECTED_ABI = 1;

    private static NativeApi api;
    private static String fallbackNote;

    private Natives() {
    }

    public static synchronized NativeApi get() {
        if (api == null) {
            api = create();
        }
        return api;
    }

    /** Why FFM was not used when it would have been the default (null if it was used or not applicable). */
    public static synchronized String fallbackNote() {
        return fallbackNote;
    }

    private static NativeApi create() {
        final String mode = System.getProperty("tessera.binding", "auto");
        if (!mode.equals("auto") && !mode.equals("jni") && !mode.equals("ffm")) {
            throw new IllegalArgumentException("tessera.binding must be auto, jni or ffm, not '" + mode + "'");
        }
        final Path library;
        try {
            library = NativeLoader.locate();
        } catch (IOException e) {
            final UnsatisfiedLinkError error = new UnsatisfiedLinkError("cannot locate the Tessera native library: " + e.getMessage());
            error.initCause(e);
            throw error;
        }

        NativeApi chosen = null;
        if (!mode.equals("jni")) {
            if (Runtime.version().feature() >= 22) {
                try {
                    // Compiled for Java 22 and stored under META-INF/versions/22 of the jar: absent on older runtimes.
                    chosen = (NativeApi) Class.forName("io.github.andreasnilsson123.tessera.internal.FfmNativeApi").getConstructor(Path.class).newInstance(library);
                } catch (ReflectiveOperationException | LinkageError | RuntimeException e) {
                    final Throwable cause = e.getCause() != null ? e.getCause() : e;
                    if (mode.equals("ffm")) {
                        throw new IllegalStateException("tessera.binding=ffm requested but the FFM binding cannot be used: " + cause, cause);
                    }
                    fallbackNote = "FFM binding unavailable (" + cause + "); using JNI";
                }
            } else if (mode.equals("ffm")) {
                throw new IllegalStateException("tessera.binding=ffm needs Java 22 or newer (running " + Runtime.version() + ")");
            }
        }
        if (chosen == null) {
            chosen = new JniNativeApi(library);
        }

        if (chosen.abiVersion() != EXPECTED_ABI) {
            throw new IllegalStateException("the native Tessera library has ABI version " + chosen.abiVersion() + " but this Java layer needs "
                    + EXPECTED_ABI + " (mixed versions?)");
        }
        return chosen;
    }
}
