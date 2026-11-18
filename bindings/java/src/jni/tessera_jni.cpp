// JNI glue for dev.tessera.internal.JniNativeApi. Compiled into the tessera shared library when
// TESSERA_BUILD_JAVA is on, so one native file serves both the JNI binding (Java 11+) and the FFM binding
// (Java 22+, which just dlsym's the C API).
//
// Rules this file follows:
//  - No C++ exception and no JNI exception ever crosses back into the C API or out of a native method
//  uncaught.
//  - Strings handed to the JVM are forced to ASCII first: NewStringUTF with malformed modified-UTF-8 is
//  undefined behaviour,
//    and driver strings (GL_VENDOR, our own messages) are outside our control.
//  - The log callback can fire on any thread, including ones the JVM does not know: it attaches as a daemon
//  and detaches.
#include <jni.h>

#include <tessera/tessera.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

std::atomic<JavaVM*> g_vm{nullptr};
std::atomic<jclass> g_class{nullptr}; // global reference, deliberately never released (see setLogSink)
std::atomic<jmethodID> g_onLog{nullptr};

/* Replaces anything outside printable ASCII so the result is valid in every encoding JNI might assume. */
std::string asciiOnly(const char* text) {
    std::string out;
    if (!text)
        return out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p)
        out.push_back((*p >= 0x20 && *p < 0x7F) ? static_cast<char>(*p) : '?');
    return out;
}

jstring toJString(JNIEnv* env, const char* text) {
    const std::string s = asciiOnly(text);
    return env->NewStringUTF(s.c_str());
}

void TESSERA_CALL logTrampoline(int32_t level, const char* message, void* /*userData*/) {
    JavaVM* vm = g_vm.load();
    jclass cls = g_class.load();
    jmethodID method = g_onLog.load();
    if (!vm || !cls || !method)
        return;

    JNIEnv* env = nullptr;
    bool attached = false;
    const jint status = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8);
    if (status == JNI_EDETACHED) {
        // A native thread (e.g. Tessera's window host) the JVM has never seen. During JVM shutdown this can
        // fail: drop the message.
        if (vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) != JNI_OK)
            return;
        attached = true;
    } else if (status != JNI_OK) {
        return;
    }
    if (jstring text = toJString(env, message)) {
        env->CallStaticVoidMethod(cls, method, static_cast<jint>(level), text);
        env->DeleteLocalRef(text);
    }
    if (env->ExceptionCheck())
        env->ExceptionClear(); // a throwing logger must not unwind into native code
    if (attached)
        vm->DetachCurrentThread();
}

constexpr jint kConfigInts = 14;
constexpr jint kInfoInts = 13;

} // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
    g_vm.store(vm);
    return JNI_VERSION_1_8;
}

// The classloader that loaded this library is going away: stop calling into it.
JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* /*vm*/, void* /*reserved*/) {
    tesseraSetLogCallback(nullptr, nullptr);
    g_onLog.store(nullptr);
    g_class.store(nullptr);
    g_vm.store(nullptr);
}

#define TESSERA_JNI(ret, name) JNIEXPORT ret JNICALL Java_dev_tessera_internal_JniNativeApi_##name

TESSERA_JNI(jint, nInit)(JNIEnv*, jclass) {
    return tesseraInit();
}
TESSERA_JNI(jint, nTerminate)(JNIEnv*, jclass) {
    return tesseraTerminate();
}
TESSERA_JNI(jint, nAbiVersion)(JNIEnv*, jclass) {
    return tesseraGetAbiVersion();
}

TESSERA_JNI(void, nVersion)(JNIEnv* env, jclass, jintArray out) {
    if (!out || env->GetArrayLength(out) < 3)
        return;
    int32_t v[3] = {0, 0, 0};
    tesseraGetVersion(&v[0], &v[1], &v[2]);
    const jint j[3] = {v[0], v[1], v[2]};
    env->SetIntArrayRegion(out, 0, 3, j);
}

// cfg: api, major, minor, profile, flags, surface, fallback, red, green, blue, alpha, depth, stencil, samples
TESSERA_JNI(jint, nCreateContext)(JNIEnv* env, jclass, jintArray cfg, jlong share, jlongArray out) {
    if (!cfg || !out || env->GetArrayLength(cfg) < kConfigInts || env->GetArrayLength(out) < 1)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    jint v[kConfigInts];
    env->GetIntArrayRegion(cfg, 0, kConfigInts, v);
    TesseraContextConfig c = tesseraDefaultContextConfig();
    c.api = static_cast<uint32_t>(v[0]);
    c.versionMajor = v[1];
    c.versionMinor = v[2];
    c.profile = static_cast<uint32_t>(v[3]);
    c.flags = static_cast<uint32_t>(v[4]);
    c.surface = static_cast<uint32_t>(v[5]);
    c.fallback = static_cast<uint32_t>(v[6]);
    c.redBits = v[7];
    c.greenBits = v[8];
    c.blueBits = v[9];
    c.alphaBits = v[10];
    c.depthBits = v[11];
    c.stencilBits = v[12];
    c.samples = v[13];
    c.shareWith = static_cast<TesseraContext>(share);
    TesseraContext handle = TESSERA_NULL_HANDLE;
    const TesseraResult r = tesseraCreateContext(&c, &handle);
    const jlong result = static_cast<jlong>(handle);
    env->SetLongArrayRegion(out, 0, 1, &result);
    return r;
}

TESSERA_JNI(jint, nDestroyContext)(JNIEnv*, jclass, jlong ctx) {
    return tesseraDestroyContext(static_cast<TesseraContext>(ctx));
}

// ints: api, major, minor, profile, flags, surface, samples, red, green, blue, alpha, depth, stencil;
// strings: vendor, renderer, version
TESSERA_JNI(jint, nContextInfo)(JNIEnv* env, jclass, jlong ctx, jintArray ints, jobjectArray strings) {
    if (!ints || !strings || env->GetArrayLength(ints) < kInfoInts || env->GetArrayLength(strings) < 3)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    TesseraContextInfo info = tesseraDefaultContextInfo();
    const TesseraResult r = tesseraGetContextInfo(static_cast<TesseraContext>(ctx), &info);
    if (r != TESSERA_SUCCESS)
        return r;
    const jint v[kInfoInts] = {static_cast<jint>(info.api),
                               info.versionMajor,
                               info.versionMinor,
                               static_cast<jint>(info.profile),
                               static_cast<jint>(info.flags),
                               static_cast<jint>(info.surface),
                               info.samples,
                               info.redBits,
                               info.greenBits,
                               info.blueBits,
                               info.alphaBits,
                               info.depthBits,
                               info.stencilBits};
    env->SetIntArrayRegion(ints, 0, kInfoInts, v);
    const char* texts[3] = {info.vendor, info.renderer, info.version};
    for (jsize i = 0; i < 3; ++i) {
        jstring s = toJString(env, texts[i]);
        if (!s)
            return TESSERA_ERROR_OUT_OF_MEMORY; // an OutOfMemoryError is pending; the Java side sees it first
        env->SetObjectArrayElement(strings, i, s);
        env->DeleteLocalRef(s);
    }
    return TESSERA_SUCCESS;
}

TESSERA_JNI(jint, nMakeCurrent)(JNIEnv*, jclass, jlong ctx) {
    return tesseraMakeCurrent(static_cast<TesseraContext>(ctx));
}
TESSERA_JNI(jlong, nCurrentContext)(JNIEnv*, jclass) {
    return static_cast<jlong>(tesseraGetCurrentContext());
}

TESSERA_JNI(jlong, nProcAddress)(JNIEnv* env, jclass, jstring name) {
    if (!name)
        return 0;
    const char* chars = env->GetStringUTFChars(name, nullptr);
    if (!chars)
        return 0;
    void* p = tesseraGetProcAddress(chars);
    env->ReleaseStringUTFChars(name, chars);
    return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(p));
}

TESSERA_JNI(jint, nSwapBuffers)(JNIEnv*, jclass, jlong ctx) {
    return tesseraSwapBuffers(static_cast<TesseraContext>(ctx));
}
TESSERA_JNI(jint, nSetSwapInterval)(JNIEnv*, jclass, jlong ctx, jint interval) {
    return tesseraSetSwapInterval(static_cast<TesseraContext>(ctx), interval);
}

TESSERA_JNI(jint, nResetStatus)(JNIEnv* env, jclass, jlong ctx, jintArray out) {
    if (!out || env->GetArrayLength(out) < 1)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    int32_t status = TESSERA_RESET_NONE;
    const TesseraResult r = tesseraGetResetStatus(static_cast<TesseraContext>(ctx), &status);
    const jint j = status;
    env->SetIntArrayRegion(out, 0, 1, &j);
    return r;
}

TESSERA_JNI(jint, nRecreate)(JNIEnv* env, jclass, jlong ctx, jlongArray outHandle, jintArray outFlags) {
    if (!outHandle || !outFlags || env->GetArrayLength(outHandle) < 1 || env->GetArrayLength(outFlags) < 1)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    TesseraContext created = TESSERA_NULL_HANDLE;
    uint32_t flags = 0;
    const TesseraResult r = tesseraRecreateContext(static_cast<TesseraContext>(ctx), &created, &flags);
    const jlong h = static_cast<jlong>(created);
    const jint f = static_cast<jint>(flags);
    env->SetLongArrayRegion(outHandle, 0, 1, &h);
    env->SetIntArrayRegion(outFlags, 0, 1, &f);
    return r;
}

TESSERA_JNI(jstring, nLastError)(JNIEnv* env, jclass) {
    return toJString(env, tesseraGetLastErrorMessage());
}
TESSERA_JNI(jstring, nResultString)(JNIEnv* env, jclass, jint code) {
    return toJString(env, tesseraResultString(code));
}

// Enables or disables forwarding of Tessera's log to JniNativeApi.onLog(int, String).
// The class reference taken here is never released: a callback already running on another thread may still be
// using it, and one leaked global reference is far cheaper than a use-after-free. Disabling just unregisters
// the C callback.
TESSERA_JNI(void, nSetLogSink)(JNIEnv* env, jclass cls, jboolean enable) {
    if (!enable) {
        tesseraSetLogCallback(nullptr, nullptr);
        return;
    }
    if (!g_class.load()) {
        jmethodID method = env->GetStaticMethodID(cls, "onLog", "(ILjava/lang/String;)V");
        if (!method)
            return; // NoSuchMethodError pending
        jclass global = static_cast<jclass>(env->NewGlobalRef(cls));
        if (!global)
            return;
        g_onLog.store(method);
        g_class.store(global);
        JavaVM* vm = nullptr;
        if (env->GetJavaVM(&vm) == JNI_OK)
            g_vm.store(vm);
    }
    tesseraSetLogCallback(&logTrampoline, nullptr);
}

} // extern "C"
