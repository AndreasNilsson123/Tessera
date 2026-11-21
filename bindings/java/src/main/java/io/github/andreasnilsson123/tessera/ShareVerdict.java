// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

/** The answer of {@link Tessera#canShare(ContextConfig, Context)}: whether a new context could share objects with a parent, and if not why. */
public enum ShareVerdict {
    OK(0),
    /** The parent is lost: share with a surviving member of its group. */
    PARENT_LOST(1),
    /** This backend does not share between OpenGL and OpenGL ES contexts (EGL; WGL and GLX drivers accepted it). */
    API_MISMATCH(2),
    /** e.g. a core and a legacy context (macOS). */
    PROFILE_MISMATCH(3),
    /** The GPU the request names is not one the parent's backend can serve. */
    DIFFERENT_BACKEND(4),
    /** The request names a GPU other than the one the parent runs on. */
    DIFFERENT_ADAPTER(5),
    /** The parent lives on another display or connection. */
    DIFFERENT_DISPLAY(6),
    /** The request needs a framebuffer format the parent's cannot share with (WGL/NVIDIA: a deeper depth buffer). */
    FORMAT_MISMATCH(7);

    private final int nativeValue;

    ShareVerdict(int v) {
        nativeValue = v;
    }

    /** True for {@link #OK}. */
    public boolean canShare() {
        return this == OK;
    }

    static ShareVerdict of(int v) {
        for (ShareVerdict s : values()) {
            if (s.nativeValue == v) {
                return s;
            }
        }
        return DIFFERENT_BACKEND; // a reason this version does not know: not a "yes"
    }
}
