// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

/** Why a context was lost, from {@link Context#lossCause()}: the right response differs. */
public enum LossCause {
    /** Healthy. */
    NONE(0),
    /** The driver reported a GPU reset through the robustness query ({@link Context#lostStatus()} says who was to blame): re-upload your resources. */
    GPU_RESET(1),
    /** The backend reported "context lost" itself (display, driver or power event), with no reset status: recreate, perhaps on another adapter. */
    DEVICE(2),
    /** The window the context rendered into was destroyed: do not recreate onto it. */
    WINDOW_GONE(3);

    private final int nativeValue;

    LossCause(int v) {
        nativeValue = v;
    }

    static LossCause of(int v) {
        for (LossCause c : values()) {
            if (c.nativeValue == v) {
                return c;
            }
        }
        return DEVICE;
    }
}
