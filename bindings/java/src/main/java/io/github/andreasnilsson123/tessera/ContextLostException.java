// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

/**
 * The GPU context was lost (a reset, or the device went away). The context stays lost: create a replacement with
 * {@link Context#recreate()}.
 */
public class ContextLostException extends TesseraException {
    private static final long serialVersionUID = 1L;

    public ContextLostException(String message) {
        super(Result.CONTEXT_LOST, message);
    }
}
