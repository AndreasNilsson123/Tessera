// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
package io.github.andreasnilsson123.tessera.lwjgl;

import io.github.andreasnilsson123.tessera.Tessera;
import org.lwjgl.system.FunctionProvider;

import java.nio.ByteBuffer;

/**
 * LWJGL {@link FunctionProvider} that resolves OpenGL functions through Tessera, so {@code GL.createCapabilities()} works for
 * contexts Tessera created. Extension and post-1.1 functions only resolve while a Tessera context is current on the calling
 * thread, which is exactly when LWJGL asks.
 */
public final class TesseraFunctionProvider implements FunctionProvider {

    public static final TesseraFunctionProvider INSTANCE = new TesseraFunctionProvider();

    private TesseraFunctionProvider() {
    }

    /** LWJGL passes the name as a NUL-terminated ASCII buffer. */
    @Override
    public long getFunctionAddress(ByteBuffer functionName) {
        final StringBuilder name = new StringBuilder(functionName.remaining());
        for (int i = functionName.position(); i < functionName.limit(); i++) {
            final byte b = functionName.get(i);
            if (b == 0) {
                break;
            }
            name.append((char) (b & 0x7F));
        }
        return Tessera.procAddress(name.toString());
    }
}
