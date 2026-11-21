// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

/** What may change when {@link Context#recreate(RecreatePolicy)} rebuilds a lost context. */
public enum RecreatePolicy {
    /** The same request, including any adapter named in it (what {@link Context#recreate()} does). */
    SAME_ADAPTER(0),
    /** Drop the adapter request; any GPU, but never a CPU renderer. */
    ANY_HARDWARE(1),
    /** Drop the adapter request; fall back to the software backend if no GPU can serve. */
    ALLOW_SOFTWARE(2);

    final int nativeValue;

    RecreatePolicy(int v) {
        nativeValue = v;
    }
}
