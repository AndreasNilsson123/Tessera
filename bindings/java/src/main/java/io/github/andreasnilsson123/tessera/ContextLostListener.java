// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

/**
 * Told, once per context, that a context was lost (see {@link Tessera#setContextLostListener}). Runs synchronously on the
 * thread whose call discovered the loss (a {@link Context#resetStatus()} poll, or a make-current / swap that the driver
 * reported as lost), which may be a thread the JVM did not start. It must not throw and should return quickly.
 *
 * <p>It may call {@link Context#recreate()} and {@link Context#close()}: the "re-create on loss" helper is
 * <pre>{@code
 * Tessera.setContextLostListener((lost, status) -> {
 *     Context.Recreation r = lost.recreate();
 *     replacements.put(lost, r.context()); // hand it to the render thread; re-upload if !r.sharePreserved()
 * });
 * }</pre>
 * It must not make the lost context current, and must not call {@link Tessera#terminate()}.
 */
@FunctionalInterface
public interface ContextLostListener {
    /**
     * @param context the lost context; it stays valid (and must still be closed) until you close it
     * @param status  why: {@link ResetStatus#GUILTY}, {@link ResetStatus#INNOCENT} or {@link ResetStatus#UNKNOWN} (the last
     *                also when the driver reported the loss without a reset status)
     */
    void contextLost(Context context, ResetStatus status);
}
