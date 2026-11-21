// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

/** Result codes of the native library ({@code TesseraResult}). */
public enum Result {
    SUCCESS(0),
    UNKNOWN(-1),
    INVALID_ARGUMENT(-2),
    INVALID_HANDLE(-3),
    NOT_INITIALIZED(-4),
    OUT_OF_MEMORY(-5),
    UNSUPPORTED(-6),
    NO_MATCHING_CONFIG(-7),
    PLATFORM(-8),
    WRONG_THREAD(-9),
    CONTEXT_LOST(-10),
    /** The window the context rendered into is gone (ABI 0.15.0); the context is lost, with cause {@link LossCause#WINDOW_GONE}. */
    WINDOW_GONE(-11);

    private final int code;

    Result(int code) {
        this.code = code;
    }

    public int code() {
        return code;
    }

    /** Maps a native code; codes this version does not know become {@link #UNKNOWN}. */
    public static Result of(int code) {
        for (Result r : values()) {
            if (r.code == code) {
                return r;
            }
        }
        return UNKNOWN;
    }
}
