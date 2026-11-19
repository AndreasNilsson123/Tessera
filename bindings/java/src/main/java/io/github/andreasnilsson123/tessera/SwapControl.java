// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

/**
 * What a context's swap control can do and what is in effect, from {@link Context#swapControl()}. Only a context that renders
 * to a window (from {@link Context#createForWindow}, or the hidden window of a WGL/GLX context) has a swap interval; the
 * window must be double-buffered for {@link Context#swapBuffers()} to pace at all.
 */
public final class SwapControl {
    /** Marks "not known" in the native layer (INT32_MIN). */
    private static final int UNKNOWN = Integer.MIN_VALUE;

    private final int caps;
    private final int requested;
    private final int applied;
    private final int maxInterval;

    SwapControl(int caps, int requested, int applied, int maxInterval) {
        this.caps = caps;
        this.requested = requested;
        this.applied = applied;
        this.maxInterval = maxInterval;
    }

    /** True if {@link Context#setSwapInterval(int)} can work on this context. */
    public boolean supported() {
        return (caps & 0x1) != 0;
    }

    /** True if an interval of {@code -1} (adaptive vsync) is accepted. */
    public boolean adaptiveSupported() {
        return (caps & 0x2) != 0;
    }

    /** True if {@link #applied()} is read back from the driver rather than unknown. */
    public boolean readBack() {
        return (caps & 0x4) != 0;
    }

    /** The last interval {@link Context#setSwapInterval(int)} succeeded with on this context, or {@code null} if never set. */
    public Integer requested() {
        return requested == UNKNOWN ? null : requested;
    }

    /**
     * The interval the driver reports it is using (negative = adaptive), or {@code null} if the driver cannot be asked. Drivers
     * clamp requests and compositors can override them: compare with {@link #requested()}.
     */
    public Integer applied() {
        return applied == UNKNOWN ? null : applied;
    }

    /** The largest interval the drawable accepts (larger requests are clamped), or 0 if not known. */
    public int maxInterval() {
        return maxInterval;
    }

    @Override
    public String toString() {
        return "SwapControl{supported=" + supported() + ", adaptive=" + adaptiveSupported() + ", requested=" + requested()
                + ", applied=" + applied() + ", max=" + maxInterval + "}";
    }
}
