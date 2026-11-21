// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
// JNI glue for io.github.andreasnilsson123.tessera.internal.JniNativeApi. Compiled into the tessera shared
// library when TESSERA_BUILD_JAVA is on, so one native file serves both the JNI binding (Java 11+) and the
// FFM binding (Java 22+, which just dlsym's the C API).
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
#include <vector>

namespace {

std::atomic<JavaVM*> g_vm{nullptr};
std::atomic<jclass> g_class{nullptr}; // global reference, deliberately never released (see setLogSink)
std::atomic<jmethodID> g_onLog{nullptr};
std::atomic<jmethodID> g_onLost{nullptr};

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

void TESSERA_CALL lostTrampoline(TesseraContext context, int32_t status, void* /*userData*/) {
    JavaVM* vm = g_vm.load();
    jclass cls = g_class.load();
    jmethodID method = g_onLost.load();
    if (!vm || !cls || !method)
        return;
    JNIEnv* env = nullptr;
    bool attached = false;
    const jint attach = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8);
    if (attach == JNI_EDETACHED) {
        if (vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) != JNI_OK)
            return;
        attached = true;
    } else if (attach != JNI_OK) {
        return;
    }
    env->CallStaticVoidMethod(cls, method, static_cast<jlong>(context), static_cast<jint>(status));
    if (env->ExceptionCheck())
        env->ExceptionClear(); // a throwing listener must not unwind into native code
    if (attached)
        vm->DetachCurrentThread();
}

// cfg ints 18..29 hold the 48 bytes of the adapter id, four to an int, least significant byte first.
constexpr jint kConfigInts = 30;
constexpr jint kInfoInts = 15; // [14] = adapterClass

// cfg: api, major, minor, profile, flags, surface, fallback, red, green, blue, alpha, depth, stencil, samples
TesseraContextConfig configFrom(const jint* v, TesseraContext share) {
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
    c.gpuPreference = static_cast<uint32_t>(v[14]);
    c.priority = static_cast<uint32_t>(v[17]);
    c.adapterLuid =
        (static_cast<uint64_t>(static_cast<uint32_t>(v[16])) << 32) | static_cast<uint32_t>(v[15]);
    c.shareWith = share;
    for (int i = 0; i < 12; ++i) {
        const uint32_t w = static_cast<uint32_t>(v[18 + i]);
        for (int k = 0; k < 4; ++k)
            c.adapter[4 * i + k] = static_cast<char>((w >> (8 * k)) & 0xFFu);
    }
    return c;
}

void infoToInts(const TesseraContextInfo& info, jint* v) {
    v[0] = static_cast<jint>(info.api);
    v[1] = info.versionMajor;
    v[2] = info.versionMinor;
    v[3] = static_cast<jint>(info.profile);
    v[4] = static_cast<jint>(info.flags);
    v[5] = static_cast<jint>(info.surface);
    v[6] = info.samples;
    v[7] = info.redBits;
    v[8] = info.greenBits;
    v[9] = info.blueBits;
    v[10] = info.alphaBits;
    v[11] = info.depthBits;
    v[12] = info.stencilBits;
    v[13] = static_cast<jint>(info.priority);
    v[14] = static_cast<jint>(info.adapterClass);
}

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

#define TESSERA_JNI(ret, name)                                                                               \
    JNIEXPORT ret JNICALL Java_io_github_andreasnilsson123_tessera_internal_JniNativeApi_##name

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
    const TesseraContextConfig c = configFrom(v, static_cast<TesseraContext>(share));
    TesseraContext handle = TESSERA_NULL_HANDLE;
    const TesseraResult r = tesseraCreateContext(&c, &handle);
    const jlong result = static_cast<jlong>(handle);
    env->SetLongArrayRegion(out, 0, 1, &result);
    return r;
}

TESSERA_JNI(jint, nCreateContextForWindow)
(JNIEnv* env, jclass, jintArray cfg, jlong share, jlong window, jint flags, jlongArray out) {
    if (!cfg || !out || env->GetArrayLength(cfg) < kConfigInts || env->GetArrayLength(out) < 1)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    jint v[kConfigInts];
    env->GetIntArrayRegion(cfg, 0, kConfigInts, v);
    const TesseraContextConfig c = configFrom(v, static_cast<TesseraContext>(share));
    TesseraContext handle = TESSERA_NULL_HANDLE;
    const TesseraResult r = tesseraCreateContextForWindow(&c, static_cast<uint64_t>(window),
                                                          static_cast<uint32_t>(flags), &handle);
    const jlong result = static_cast<jlong>(handle);
    env->SetLongArrayRegion(out, 0, 1, &result);
    return r;
}

TESSERA_JNI(jint, nCreateContextForNativeWindow)
(JNIEnv* env, jclass, jintArray cfg, jlong share, jint kind, jlong window, jlong display, jint width,
 jint height, jint flags, jlongArray out) {
    if (!cfg || !out || env->GetArrayLength(cfg) < kConfigInts || env->GetArrayLength(out) < 1)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    jint v[kConfigInts];
    env->GetIntArrayRegion(cfg, 0, kConfigInts, v);
    const TesseraContextConfig c = configFrom(v, static_cast<TesseraContext>(share));
    TesseraNativeWindow w = tesseraDefaultNativeWindow();
    w.kind = static_cast<uint32_t>(kind);
    w.window = static_cast<uint64_t>(window);
    w.display = static_cast<uint64_t>(display);
    w.width = width;
    w.height = height;
    TesseraContext handle = TESSERA_NULL_HANDLE;
    const TesseraResult r =
        tesseraCreateContextForNativeWindow(&c, &w, static_cast<uint32_t>(flags), &handle);
    const jlong result = static_cast<jlong>(handle);
    env->SetLongArrayRegion(out, 0, 1, &result);
    return r;
}

TESSERA_JNI(jint, nResizeWindow)(JNIEnv*, jclass, jlong ctx, jint width, jint height) {
    return tesseraResizeWindow(static_cast<TesseraContext>(ctx), width, height);
}

TESSERA_JNI(jint, nNativeHandles)(JNIEnv* env, jclass, jlong ctx, jintArray kindOut, jlongArray handlesOut) {
    if (!kindOut || !handlesOut || env->GetArrayLength(kindOut) < 1 || env->GetArrayLength(handlesOut) < 6)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    TesseraNativeHandles h = tesseraDefaultNativeHandles();
    const TesseraResult r = tesseraGetNativeHandles(static_cast<TesseraContext>(ctx), &h);
    if (r != TESSERA_SUCCESS)
        return r;
    const jint kind = static_cast<jint>(h.kind);
    env->SetIntArrayRegion(kindOut, 0, 1, &kind);
    jlong values[6];
    for (int i = 0; i < 6; ++i)
        values[i] = static_cast<jlong>(h.handles[i]);
    env->SetLongArrayRegion(handlesOut, 0, 6, values);
    return TESSERA_SUCCESS;
}

TESSERA_JNI(jint, nPrewarm)(JNIEnv* env, jclass, jintArray cfg, jint count) {
    if (!cfg || env->GetArrayLength(cfg) < kConfigInts)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    jint v[kConfigInts];
    env->GetIntArrayRegion(cfg, 0, kConfigInts, v);
    const TesseraContextConfig c = configFrom(v, TESSERA_NULL_HANDLE);
    return tesseraPrewarmContexts(&c, count);
}

TESSERA_JNI(jint, nPoolTopUp)(JNIEnv* env, jclass, jintArray cfg, jint target, jintArray created) {
    if (!cfg || !created || env->GetArrayLength(cfg) < kConfigInts || env->GetArrayLength(created) < 1)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    jint v[kConfigInts];
    env->GetIntArrayRegion(cfg, 0, kConfigInts, v);
    const TesseraContextConfig c = configFrom(v, TESSERA_NULL_HANDLE);
    int32_t made = 0;
    const TesseraResult r = tesseraPoolTopUp(&c, target, &made);
    const jint out = made;
    env->SetIntArrayRegion(created, 0, 1, &out);
    return r;
}

TESSERA_JNI(jint, nPooledCountFor)(JNIEnv* env, jclass, jintArray cfg, jintArray count) {
    if (!cfg || !count || env->GetArrayLength(cfg) < kConfigInts || env->GetArrayLength(count) < 1)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    jint v[kConfigInts];
    env->GetIntArrayRegion(cfg, 0, kConfigInts, v);
    const TesseraContextConfig c = configFrom(v, TESSERA_NULL_HANDLE);
    int32_t n = 0;
    const TesseraResult r = tesseraGetPooledCount(&c, &n);
    const jint out = n;
    env->SetIntArrayRegion(count, 0, 1, &out);
    return r;
}

TESSERA_JNI(jint, nPoolClear)(JNIEnv*, jclass) {
    return tesseraPoolClear();
}

TESSERA_JNI(jint, nPooledCount)(JNIEnv*, jclass) {
    return tesseraPooledContextCount();
}

// infoInts as nContextInfo; strings: vendor, renderer, version, backend, message, adapter
TESSERA_JNI(jint, nProbe)(JNIEnv* env, jclass, jintArray cfg, jintArray infoInts, jobjectArray strings) {
    if (!cfg || !infoInts || !strings || env->GetArrayLength(cfg) < kConfigInts ||
        env->GetArrayLength(infoInts) < kInfoInts || env->GetArrayLength(strings) < 6)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    jint v[kConfigInts];
    env->GetIntArrayRegion(cfg, 0, kConfigInts, v);
    const TesseraContextConfig c = configFrom(v, TESSERA_NULL_HANDLE);
    TesseraProbeReport report = tesseraDefaultProbeReport();
    const TesseraResult r = tesseraProbe(&c, &report);
    if (r == TESSERA_ERROR_INVALID_ARGUMENT && report.backend[0] == '\0' && report.message[0] == '\0')
        return r; // rejected before the probe ran: the report was not filled in
    jint out[kInfoInts];
    infoToInts(report.info, out);
    env->SetIntArrayRegion(infoInts, 0, kInfoInts, out);
    const char* texts[6] = {report.info.vendor, report.info.renderer, report.info.version,
                            report.backend,     report.message,       report.info.adapter};
    for (jsize i = 0; i < 6; ++i) {
        jstring s = toJString(env, texts[i]);
        if (!s)
            return TESSERA_ERROR_OUT_OF_MEMORY; // an OutOfMemoryError is pending
        env->SetObjectArrayElement(strings, i, s);
        env->DeleteLocalRef(s);
    }
    return r;
}

TESSERA_JNI(jint, nDestroyContext)(JNIEnv*, jclass, jlong ctx) {
    return tesseraDestroyContext(static_cast<TesseraContext>(ctx));
}

// ints: api, major, minor, profile, flags, surface, samples, red, green, blue, alpha, depth, stencil;
// strings: vendor, renderer, version
TESSERA_JNI(jint, nContextInfo)(JNIEnv* env, jclass, jlong ctx, jintArray ints, jobjectArray strings) {
    if (!ints || !strings || env->GetArrayLength(ints) < kInfoInts || env->GetArrayLength(strings) < 4)
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
                               info.stencilBits,
                               static_cast<jint>(info.priority),
                               static_cast<jint>(info.adapterClass)};
    env->SetIntArrayRegion(ints, 0, kInfoInts, v);
    const char* texts[4] = {info.vendor, info.renderer, info.version, info.adapter};
    for (jsize i = 0; i < 4; ++i) {
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

// The adapter inventory (ABI 0.14.0): the count, then one entry at a time (ints: flags, vendor, device, luid
// low, luid high; strings: backend, id, name). The list is re-read on each call, which is cheap and keeps the
// JNI surface to two calls.
TESSERA_JNI(jint, nAdapterCount)(JNIEnv*, jclass) {
    int32_t count = 0;
    const TesseraResult r = tesseraEnumerateAdapters(nullptr, 0, &count);
    return r == TESSERA_SUCCESS ? count : r; // a negative result is an error code
}

TESSERA_JNI(jint, nAdapter)(JNIEnv* env, jclass, jint index, jintArray ints, jobjectArray strings) {
    if (!ints || !strings || env->GetArrayLength(ints) < 5 || env->GetArrayLength(strings) < 3 || index < 0)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    int32_t count = 0;
    if (const TesseraResult r = tesseraEnumerateAdapters(nullptr, 0, &count); r != TESSERA_SUCCESS)
        return r;
    if (index >= count)
        return TESSERA_ERROR_INVALID_ARGUMENT; // the inventory shrank between the two calls
    std::vector<TesseraAdapterInfo> list(static_cast<std::size_t>(count), tesseraDefaultAdapterInfo());
    if (const TesseraResult r = tesseraEnumerateAdapters(list.data(), count, &count); r != TESSERA_SUCCESS)
        return r;
    if (index >= count)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    const TesseraAdapterInfo& a = list[static_cast<std::size_t>(index)];
    const jint v[5] = {static_cast<jint>(a.flags), static_cast<jint>(a.vendorId),
                       static_cast<jint>(a.deviceId), static_cast<jint>(a.luid & 0xFFFFFFFFu),
                       static_cast<jint>(a.luid >> 32)};
    env->SetIntArrayRegion(ints, 0, 5, v);
    const char* texts[3] = {a.backend, a.id, a.name};
    for (jsize i = 0; i < 3; ++i) {
        jstring s = toJString(env, texts[i]);
        if (!s)
            return TESSERA_ERROR_OUT_OF_MEMORY;
        env->SetObjectArrayElement(strings, i, s);
        env->DeleteLocalRef(s);
    }
    return TESSERA_SUCCESS;
}

TESSERA_JNI(jint, nSwapControl)(JNIEnv* env, jclass, jlong ctx, jintArray out) {
    if (!out || env->GetArrayLength(out) < 4)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    TesseraSwapControl sc = tesseraDefaultSwapControl();
    const TesseraResult r = tesseraGetSwapControl(static_cast<TesseraContext>(ctx), &sc);
    const jint values[4] = {static_cast<jint>(sc.caps), sc.requested, sc.applied, sc.maxInterval};
    env->SetIntArrayRegion(out, 0, 4, values);
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

TESSERA_JNI(jint, nRecreateWithPolicy)
(JNIEnv* env, jclass, jlong ctx, jint policy, jlongArray outHandle, jintArray outFlags) {
    if (!outHandle || !outFlags || env->GetArrayLength(outHandle) < 1 || env->GetArrayLength(outFlags) < 1 ||
        policy < 0)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    TesseraContext created = TESSERA_NULL_HANDLE;
    uint32_t flags = 0;
    const TesseraResult r = tesseraRecreateContextWithPolicy(static_cast<TesseraContext>(ctx),
                                                             static_cast<uint32_t>(policy), &created, &flags);
    const jlong h = static_cast<jlong>(created);
    const jint f = static_cast<jint>(flags);
    env->SetLongArrayRegion(outHandle, 0, 1, &h);
    env->SetIntArrayRegion(outFlags, 0, 1, &f);
    return r;
}

TESSERA_JNI(jint, nLossCause)(JNIEnv* env, jclass, jlong ctx, jintArray out) {
    if (!out || env->GetArrayLength(out) < 1)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    int32_t cause = TESSERA_LOSS_NONE;
    const TesseraResult r = tesseraGetLossCause(static_cast<TesseraContext>(ctx), &cause);
    const jint v = static_cast<jint>(cause);
    env->SetIntArrayRegion(out, 0, 1, &v);
    return r;
}

TESSERA_JNI(jint, nCanShare)(JNIEnv* env, jclass, jintArray cfg, jlong parent, jintArray out) {
    if (!cfg || !out || env->GetArrayLength(cfg) < kConfigInts || env->GetArrayLength(out) < 1)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    jint v[kConfigInts];
    env->GetIntArrayRegion(cfg, 0, kConfigInts, v);
    const TesseraContextConfig c = configFrom(v, TESSERA_NULL_HANDLE);
    int32_t reason = TESSERA_SHARE_OK;
    const TesseraResult r = tesseraCanShare(&c, static_cast<TesseraContext>(parent), &reason);
    const jint rv = static_cast<jint>(reason);
    env->SetIntArrayRegion(out, 0, 1, &rv);
    return r;
}

TESSERA_JNI(jint, nShareGroup)(JNIEnv* env, jclass, jlong ctx, jlongArray ids, jintArray counts) {
    if (!ids || !counts || env->GetArrayLength(ids) < 1 || env->GetArrayLength(counts) < 2)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    TesseraShareGroupInfo g = tesseraDefaultShareGroupInfo();
    const TesseraResult r = tesseraGetShareGroup(static_cast<TesseraContext>(ctx), &g);
    if (r != TESSERA_SUCCESS)
        return r;
    const jlong id = static_cast<jlong>(g.groupId);
    const jint c[2] = {g.members, g.alive};
    env->SetLongArrayRegion(ids, 0, 1, &id);
    env->SetIntArrayRegion(counts, 0, 2, c);
    return TESSERA_SUCCESS;
}

TESSERA_JNI(jint, nIsContextLost)(JNIEnv* env, jclass, jlong ctx, jintArray out) {
    if (!out || env->GetArrayLength(out) < 2)
        return TESSERA_ERROR_INVALID_ARGUMENT;
    int32_t lost = 0, status = TESSERA_RESET_NONE;
    const TesseraResult r = tesseraIsContextLost(static_cast<TesseraContext>(ctx), &lost, &status);
    const jint values[2] = {lost, status};
    env->SetIntArrayRegion(out, 0, 2, values);
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

// Enables or disables forwarding of context-lost events to JniNativeApi.onContextLost(long, int). Same
// class-reference policy as nSetLogSink (a global reference that is never released).
TESSERA_JNI(void, nSetLostSink)(JNIEnv* env, jclass cls, jboolean enable) {
    if (!enable) {
        tesseraSetContextLostCallback(nullptr, nullptr);
        return;
    }
    if (!g_onLost.load()) {
        jmethodID method = env->GetStaticMethodID(cls, "onContextLost", "(JI)V");
        if (!method)
            return; // NoSuchMethodError pending
        if (!g_class.load()) {
            jclass global = static_cast<jclass>(env->NewGlobalRef(cls));
            if (!global)
                return;
            g_class.store(global);
        }
        JavaVM* vm = nullptr;
        if (env->GetJavaVM(&vm) == JNI_OK)
            g_vm.store(vm);
        g_onLost.store(method);
    }
    tesseraSetContextLostCallback(&lostTrampoline, nullptr);
}

} // extern "C"
