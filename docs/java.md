# Using Tessera from Java

`tessera.jar` is a multi-release jar (Java 11+) with the native library embedded. It uses JNI on Java 11-21 and
`java.lang.foreign` on Java 22+, chosen automatically (see ADR 0009).

```java
ContextConfig config = ContextConfig.builder()
        .profile(ContextConfig.Profile.CORE).version(3, 3)      // minimums; the real result is in ctx.info()
        .depthBits(24)
        .build();

try (Context ctx = Context.create(config)) {
    ctx.makeCurrent();                    // on a platform thread; this thread now owns the context
    // ... OpenGL calls ...
    Context.releaseCurrent();
}
```

`Fallback.STRICT` (the default) fails with `TesseraException`/`NO_MATCHING_CONFIG` unless everything requested is met;
`Fallback.BEST_EFFORT` relaxes soft attributes (samples, depth, flags the driver cannot give) and `ctx.info()` reports what
was really obtained. Nothing is read back from the request: every flag, size, version and profile in `ContextInfo` comes from
the driver.

## Threads

- A context is current on at most one thread. `makeCurrent()` on another thread while it is held throws
  `IllegalStateException`. If the holder exits, the context is released for it.
- `close()` works from any thread. Closing a context another thread still holds succeeds at once and invalidates the handle;
  the holder's native teardown happens when it releases, switches, or exits. This is what makes a `Cleaner` or finalizer
  thread safe to use.
- **Use platform threads for GL.** Virtual threads can move between OS threads between two calls, which a GL context cannot
  survive, so `makeCurrent()` refuses them.
- A context that is garbage collected without `close()` is destroyed by a safety net, but that is not deterministic.

## LWJGL

```java
Context ctx = Context.create(config);
ctx.makeCurrent();
GLCapabilities caps = TesseraGL.createCapabilities();   // every thread that renders, after makeCurrent()
GL11C.glClear(GL11C.GL_COLOR_BUFFER_BIT);
```

`TesseraGL.install()` (called for you by `createCapabilities()`) must run before anything else touches
`org.lwjgl.opengl.GL`: LWJGL's `GL` class loads the system OpenGL library the moment it is first used, which would leave no
room for Tessera's function provider. It sets LWJGL's `OPENGL_EXPLICIT_INIT` flag for you; if something got there first,
the exception says so, and `-Dorg.lwjgl.opengl.explicitInit=true` on the command line is the other way out. LWJGL keeps one
provider per classloader, so Tessera's cannot share a classloader with LWJGL's own GLFW/system OpenGL.

Compile `tessera-lwjgl.jar` (the adapter) next to LWJGL; the core jar has no LWJGL dependency.

## JavaFX

JavaFX owns its render thread and (on Linux) its own OpenGL context there. Do not touch either. The pattern that works, and
is tested (`FxCoexistence`):

1. Render on **your own platform thread** with its own Tessera context, into an FBO (or any texture).
2. `glReadPixels` into a `ByteBuffer`/`byte[]`.
3. Hand it to JavaFX with `Platform.runLater(...)` and `WritableImage.getPixelWriter().setPixels(...)` (or a `PixelBuffer`).
   Drop frames the FX thread has not consumed yet instead of queueing them.

Creating and destroying contexts on other threads while JavaFX renders is safe: Tessera creates its own hidden windows,
connections and displays and puts back whatever context a thread had current (tested with three threads churning contexts
the whole time).

## System properties and JVM flags

| Property / flag | Meaning |
|---|---|
| `-Dtessera.library.path=<file>` | Load this native library instead of the one embedded in the jar. |
| `-Dtessera.binding=auto\|jni\|ffm` | Force a binding. `ffm` needs Java 22+. |
| `--enable-native-access=ALL-UNNAMED` | Silences the JDK 24+ warning about restricted methods (`System.load`, FFM). |

The native library's own environment variables (`TESSERA_BACKEND`, `TESSERA_EGL_*`, ...) are in `environment.md`.

## Errors

| Exception | When |
|---|---|
| `IllegalStateException` | Wrong thread, closed context, library shut down, virtual thread. A programming error. |
| `TesseraException` (`result()`) | The system could not do it: `NO_MATCHING_CONFIG`, `UNSUPPORTED`, `INVALID_ARGUMENT`, `PLATFORM`, ... |
| `ContextLostException` | The GPU context was lost; use `ctx.recreate()`. |

## Building

```
python scripts/fetch-java-deps.py          # optional: LWJGL + JavaFX for the integration tests (checksummed, pinned)
cmake --preset java                        # JAVA_HOME picks the JDK that builds (22+ also builds the FFM binding)
cmake --build --preset java --config Debug
ctest --preset java -C Debug               # needs a real OpenGL driver; JavaFX opens a small window for a few seconds
```

The jar is `build/java/bindings/java/tessera.jar`. Any JDK 11+ can run it.

## Known limits

- The package name `dev.tessera` is a placeholder.
- Tested end to end on Windows (NVIDIA, JavaFX Direct3D and software pipelines). The Linux JNI glue is compile-checked but the
  Java tests have not run on Linux, and JavaFX's own OpenGL pipeline (Linux only) has not been tried alongside Tessera.
- macOS: not built or tried.
