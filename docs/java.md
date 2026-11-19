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

## Probing and pre-warming

```java
ProbeReport r = Tessera.probe(config);          // "would this work here, and what would I get?"
if (r.succeeded()) System.out.println(r.info() + " via " + r.backend());
else System.out.println(r.result() + ": " + r.message());

Tessera.prewarm(config, 4);                     // call from a background thread; blocks until 4 contexts exist
Context ctx = Context.create(config);           // an identical request takes a pre-warmed one (~0.01 ms instead of ~30 ms)
```

A probe really creates, inspects and destroys a context, so cache the answer; it never throws for a request that cannot be met
(only for an invalid configuration). Only never-used contexts are pooled, so a pooled context is indistinguishable from a
fresh one; the pool holds at most 64 and is emptied at shutdown. Keep it full from a thread of yours with
`Tessera.topUpPool(config, n)` (creates only what is missing; Tessera starts no threads), inspect it with
`Tessera.pooledContextCount(config)` and empty it with `Tessera.clearPool()`. See `diag.md` and `interop.md`.

## Interop and rendering into your own window

```java
NativeHandles h = ctx.nativeHandles();          // borrowed: valid until ctx is closed; never destroy them
if (h.kind() == NativeHandles.Kind.WGL) { long hglrc = h.get(0), hdc = h.get(1); /* e.g. WGL_NV_DX_interop2 */ }

Context onScreen = Context.createForWindow(config, hwndOrXWindow, /* allowSettingPixelFormat */ false);
```

`NativeHandles` documents which entry means what per platform (WGL, GLX, EGL, CGL). Tested against LWJGL's own `WGL` and `GLX`
bindings: the handles are the objects the driver reports as current (Windows handles are 32-bit values and may appear
sign-extended). `createForWindow` borrows a window you own (Windows `HWND`, X11 `Window`; `UNSUPPORTED` elsewhere): Tessera never
destroys, resizes or shows it, and sets a Windows pixel format only if you say the window is fresh (`ADR 0010`). The Java tests
cover argument validation only: creating a real native window needs a toolkit, and the native tests (`test_wgl`, `test_glx`)
cover the actual attach.

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

## Module path

The jar is a named module, `io.github.andreasnilsson123.tessera`, exporting only the API package (the JNI/FFM bindings and the loader
are not exported). It loads a native library, so give it native access: `--enable-native-access=io.github.andreasnilsson123.tessera`
on the module path (`ALL-UNNAMED` on the class path; JDK 24+ warns without it). Tested in `java_module_jni` / `java_module_ffm`.
The module name ends in digits (from the GitHub namespace), which `javac -Xlint` advises against; it is harmless.

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
| `SwapControl` (`ctx.swapControl()`) | What swap control the context has (`supported()`, `adaptiveSupported()`, `readBack()`), what was last requested and what the driver reports (`applied()`, `null` if it cannot be asked). Needs the context current; a pbuffer reports unsupported instead of failing (ADR 0012). |
| `ContextLostException` | The GPU context was lost; use `ctx.recreate()`. To be told instead of checking every call: `Tessera.setContextLostListener((ctx, status) -> ...)` (once per context, on the thread that found out; it may call `ctx.recreate()`), or ask `ctx.isLost()` / `ctx.lostStatus()` from any thread (see ADR 0011). |

## Building

```
python scripts/fetch-java-deps.py          # optional: LWJGL + JavaFX for the integration tests (checksummed, pinned)
cmake --preset java                        # JAVA_HOME picks the JDK that builds (22+ also builds the FFM binding)
cmake --build --preset java --config Debug
ctest --preset java -C Debug               # needs a real OpenGL driver; JavaFX opens a small window for a few seconds
```

The jar is `build/java/bindings/java/tessera.jar`. Any JDK 11+ can run it.

## Known limits

- Tested end to end on Windows (NVIDIA, JavaFX Direct3D and software pipelines). The Linux JNI glue is compile-checked but the
  Java tests have not run on Linux, and JavaFX's own OpenGL pipeline (Linux only) has not been tried alongside Tessera.
- macOS: not built or tried.
