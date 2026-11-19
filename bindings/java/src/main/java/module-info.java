// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors

/**
 * OpenGL / OpenGL ES context creation that is safe to use in the same process as other OpenGL users (JavaFX, AWT, GLFW, ...).
 *
 * <p>Only {@code io.github.andreasnilsson123.tessera} is API. The {@code ...internal} package (the JNI and FFM bindings and
 * the native-library loader) is deliberately not exported.
 *
 * <p>The module loads a native library, so it needs native access: {@code --enable-native-access=io.github.andreasnilsson123.tessera}
 * on the module path ({@code ALL-UNNAMED} on the class path); JDK 24 and later warn without it.
 */
module io.github.andreasnilsson123.tessera {
    exports io.github.andreasnilsson123.tessera;
}
