// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera;

/**
 * A native call failed. {@link #result()} says why; the message carries the native library's own explanation.
 *
 * <p>Two kinds of failure are reported with standard exceptions instead, because they are programming errors rather than
 * environmental ones: using a context from the wrong thread, or after it was closed, throws
 * {@link IllegalStateException}.
 */
public class TesseraException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    private final Result result;

    public TesseraException(Result result, String message) {
        super(result + ": " + message);
        this.result = result;
    }

    public Result result() {
        return result;
    }
}
