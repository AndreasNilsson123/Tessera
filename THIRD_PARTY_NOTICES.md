# Third-party notices

**What Tessera ships contains no third-party code.** The native library, the `tessera-diag` tool and the Java jar
(`io.github.andreasnilsson123:tessera`) are built from the sources in this repository only. They `dlopen`/`LoadLibrary` the
platform's own OpenGL, EGL, X11 and DXGI libraries at run time; those belong to the operating system and the GPU vendor and are
not redistributed.

The OpenGL, GLX, WGL, EGL and Xlib declarations in `src/platform/*/` are hand-written from the public specifications (numeric
constants and function signatures); no Khronos or X.Org header text is copied.

## Used only to build and test (downloaded on demand, never part of a release artifact)

| Component | Version | License | Used by |
|---|---|---|---|
| LWJGL (`lwjgl`, `lwjgl-opengl`, `lwjgl-glfw`, natives) | 3.3.4 | BSD-3-Clause | `java_lwjgl`, `java_glfw`, `java_awt_opengl` tests and the optional `tessera-lwjgl` adapter's compile classpath |
| GLFW (inside LWJGL's natives) | 3.5.0 | zlib/libpng | `java_glfw` |
| OpenJFX (`javafx-base`, `javafx-graphics`) | 21.0.5 | GPL-2.0 with Classpath Exception | `java_javafx_*` tests |
| Mesa (system package on CI/WSL) | 23.x | MIT and others | the Linux software-rendering CI jobs |

`scripts/fetch-java-deps.py` pins these versions and verifies every download against the SHA-1 Maven Central publishes. Nothing
above is linked into, embedded in or distributed with Tessera.
