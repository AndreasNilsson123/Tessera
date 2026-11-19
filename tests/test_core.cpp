// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
// White-box tests for the Phase 1 core. Links the core objects directly (internals are not exported by
// the shared library). A fake backend exercises the full create/describe/destroy/terminate flow without
// needing a GPU or window system.
#include <algorithm>
#include <tessera/tessera.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "platform/common/arb_context.hpp"
#include "platform/common/context_priority.hpp"
#include "platform/common/device_select.hpp"
#include "platform/common/gl_query.hpp"
#include "platform/common/swap_control.hpp"

#include "core/backend.hpp"
#include "core/boundary.hpp"
#include "core/config.hpp"
#include "core/foreign_state.hpp"
#include "core/handle_table.hpp"
#include "core/library.hpp"
#include "core/runtime.hpp"
#if defined(__linux__)
#include "platform/x11/glx_api.hpp"
#endif
#include "core/thread_affinity.hpp"
#include "platform/common/gl_query.hpp"

static int g_failures = 0;
#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                    \
            ++g_failures;                                                                                    \
        }                                                                                                    \
    } while (0)

using namespace tessera;

#define EXPECT(call, want)                                                                                   \
    do {                                                                                                     \
        const TesseraResult got_ = (call);                                                                   \
        if (got_ != (want)) {                                                                                \
            std::fprintf(stderr, "%s:%d: %s -> %s, expected %s (%s)\n", __FILE__, __LINE__, #call,           \
                         tesseraResultString(got_), tesseraResultString(want),                               \
                         tesseraGetLastErrorMessage());                                                      \
            ++g_failures;                                                                                    \
        }                                                                                                    \
    } while (0)

static_assert(sizeof(TesseraContextInfo) == 504, "TesseraContextInfo layout");

// ------------------------------------------------------------------ handle table

struct Dummy {
    explicit Dummy(int v = 0) : value(v) {}
    int value;
};

static void testHandleTable() {
    HandleTable<Dummy> t(0x44, 8);
    CHECK(t.insert(std::make_shared<Dummy>(1)) == TESSERA_NULL_HANDLE); // closed
    t.open();

    const TesseraHandle a = t.insert(std::make_shared<Dummy>(1));
    CHECK(a != TESSERA_NULL_HANDLE);
    CHECK(t.get(a) && t.get(a)->value == 1);
    CHECK(t.get(TESSERA_NULL_HANDLE) == nullptr);
    CHECK(t.get(a ^ (1ull << 56)) == nullptr); // wrong type tag
    CHECK(t.get(a + 1) == nullptr);            // other index, never allocated

    CHECK(t.remove(a) != nullptr);
    CHECK(t.get(a) == nullptr);    // stale
    CHECK(t.remove(a) == nullptr); // double remove
    const TesseraHandle b = t.insert(std::make_shared<Dummy>(2));
    CHECK(b != a); // same slot, new generation
    CHECK(static_cast<uint32_t>(b) == static_cast<uint32_t>(a));
    CHECK(t.get(a) == nullptr && t.get(b)->value == 2);

    // A user of an object keeps it alive across a concurrent destroy.
    std::shared_ptr<Dummy> held = t.get(b);
    t.remove(b);
    CHECK(held->value == 2);

    // Capacity limit.
    std::vector<TesseraHandle> hs;
    for (int i = 0; i < 8; ++i)
        hs.push_back(t.insert(std::make_shared<Dummy>(i)));
    CHECK(t.insert(std::make_shared<Dummy>(99)) == TESSERA_NULL_HANDLE);
    CHECK(t.size() == 8);

    auto drained = t.closeAndDrain();
    CHECK(drained.size() == 8);
    CHECK(t.size() == 0);
    for (TesseraHandle h : hs)
        CHECK(t.get(h) == nullptr);
    CHECK(t.insert(std::make_shared<Dummy>(1)) == TESSERA_NULL_HANDLE);

    // Concurrency smoke test: many threads insert/get/remove with no crashes and no leaks.
    HandleTable<Dummy> c(0x45, 1u << 16);
    c.open();
    std::vector<std::thread> threads;
    for (int th = 0; th < 8; ++th)
        threads.emplace_back([&c, th] {
            for (int i = 0; i < 5000; ++i) {
                TesseraHandle h = c.insert(std::make_shared<Dummy>(th * 100000 + i));
                auto o = c.get(h);
                CHECK(o && o->value == th * 100000 + i);
                CHECK(c.remove(h) != nullptr);
                CHECK(c.get(h) == nullptr);
            }
        });
    for (auto& th : threads)
        th.join();
    CHECK(c.size() == 0);
}

// ------------------------------------------------------------------ config

static void testConfig() {
    TesseraContextConfig out;
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;

    CHECK(normalizeConfig(&cfg, out) == TESSERA_SUCCESS);
    CHECK(normalizeConfig(nullptr, out) == TESSERA_ERROR_INVALID_ARGUMENT);

    TesseraContextConfig bad = cfg;
    bad.structSize = 0;
    CHECK(normalizeConfig(&bad, out) == TESSERA_ERROR_INVALID_ARGUMENT);
    CHECK(std::strstr(tesseraGetLastErrorMessage(), "structSize") != nullptr);

    bad = cfg;
    bad.api = 0;
    CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg;
    bad.api = 99;
    CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg;
    bad.api = TESSERA_API_OPENGL_ES;
    bad.profile = TESSERA_PROFILE_CORE;
    CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg;
    bad.profile = TESSERA_PROFILE_CORE;
    bad.versionMajor = 3;
    bad.versionMinor = 1;
    CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg;
    bad.profile = TESSERA_PROFILE_CORE;
    bad.versionMajor = 3;
    bad.versionMinor = 2;
    CHECK(normalizeConfig(&bad, out) == TESSERA_SUCCESS);
    bad = cfg;
    bad.flags = 0x80000000u;
    CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg;
    bad.flags = TESSERA_FLAG_RESET_NOTIFICATION;
    CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg;
    bad.flags = TESSERA_FLAG_NO_ERROR | TESSERA_FLAG_DEBUG;
    CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg;
    bad.samples = -1;
    CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg;
    bad.depthBits = 99;
    CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg;
    bad.surface = 77;
    CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg;
    bad.reserved[3] = 1;
    CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);

    // Older caller: structSize ends right after `fallback`; the rest must read as zero, not as garbage.
    struct Padded {
        TesseraContextConfig c;
        unsigned char canary[64];
    } padded;
    std::memset(&padded, 0xAB, sizeof padded);
    padded.c = cfg;
    padded.c.structSize = (uint32_t)(offsetof(TesseraContextConfig, fallback) + sizeof(TesseraEnum));
    padded.c.samples = 0x7777; // beyond structSize: must be ignored
    CHECK(normalizeConfig(&padded.c, out) == TESSERA_SUCCESS);
    CHECK(out.samples == 0 && out.redBits == 0);
    CHECK(out.structSize == sizeof(TesseraContextConfig));

    // Newer caller (bigger struct): extra bytes ignored.
    padded.c = cfg;
    padded.c.structSize = (uint32_t)sizeof padded;
    CHECK(normalizeConfig(&padded.c, out) == TESSERA_SUCCESS);
}

// ------------------------------------------------------------------ foreign-state guard

struct FakeOps {
    struct State {
        int value;
    };
    static inline int current = 0;
    static inline int captures = 0;
    static inline int restores = 0;
    static State capture() noexcept {
        ++captures;
        return {current};
    }
    static void restore(const State& s) noexcept {
        ++restores;
        current = s.value;
    }
};

static int guardedEarlyReturn(bool fail) {
    ScopedForeignState<FakeOps> guard;
    FakeOps::current = 42; // pretend we made our own context current
    if (fail)
        return -1;
    return 0;
}

static void testForeignGuard() {
    FakeOps::current = 7;
    {
        ScopedForeignState<FakeOps> g;
        FakeOps::current = 99;
    }
    CHECK(FakeOps::current == 7);

    FakeOps::current = 7;
    CHECK(guardedEarlyReturn(true) == -1);
    CHECK(FakeOps::current == 7);

    FakeOps::current = 7;
    try {
        ScopedForeignState<FakeOps> g;
        FakeOps::current = 5;
        throw 1;
    } catch (int) {
    }
    CHECK(FakeOps::current == 7); // restored on exception

    FakeOps::current = 7;
    {
        ScopedForeignState<FakeOps> g;
        FakeOps::current = 8;
        g.dismiss();
    }
    CHECK(FakeOps::current == 8);
    CHECK(FakeOps::captures == FakeOps::restores + 1); // the dismissed one captured but did not restore
}

// ------------------------------------------------------------------ dynamic loader

static void testLibrary() {
#if defined(_WIN32)
    DynamicLibrary lib = DynamicLibrary::open("kernel32.dll");
    CHECK(lib);
    CHECK(lib.symbol("GetCurrentProcessId") != nullptr);
#else
    DynamicLibrary lib = DynamicLibrary::open("libm.so.6");
    if (!lib)
        lib = DynamicLibrary::open("libSystem.dylib");
    CHECK(lib);
    CHECK(lib.symbol("cos") != nullptr);
#endif
    CHECK(lib.symbol("tessera_definitely_not_a_symbol") == nullptr);
    DynamicLibrary moved = std::move(lib);
    CHECK(!lib && moved);
    moved.close();
    CHECK(!moved);

    CHECK(!DynamicLibrary::open("tessera_no_such_library_xyz.dll"));
    CHECK(!DynamicLibrary::open(nullptr));

    CHECK(!isPlausibleProcAddress(nullptr));
    CHECK(!isPlausibleProcAddress(reinterpret_cast<void*>(1)));
    CHECK(!isPlausibleProcAddress(reinterpret_cast<void*>(2)));
    CHECK(!isPlausibleProcAddress(reinterpret_cast<void*>(3)));
    CHECK(!isPlausibleProcAddress(reinterpret_cast<void*>(static_cast<std::uintptr_t>(-1))));
    CHECK(isPlausibleProcAddress(reinterpret_cast<void*>(0x1000)));
}

// ------------------------------------------------------------------ runtime with fake backends

static std::atomic<int> g_liveFakeContexts{0};

template <std::size_t N> static void copyStr(char (&dst)[N], const char* src) {
    std::size_t i = 0;
    for (; i + 1 < N && src[i] != char(0); ++i)
        dst[i] = src[i];
    dst[i] = char(0);
}

class FakeContext : public ContextObject {
  public:
    explicit FakeContext(const TesseraContextConfig& c, std::shared_ptr<ContextObject> share)
        : cfg(c), shared(std::move(share)) {
        ++g_liveFakeContexts;
    }
    ~FakeContext() override {
        --g_liveFakeContexts;
    }
    TesseraResult describe(TesseraContextInfo& out) const noexcept override {
        out.api = cfg.api;
        out.versionMajor = cfg.versionMajor ? cfg.versionMajor : 4;
        out.versionMinor = cfg.versionMinor;
        out.samples = cfg.samples;
        out.flags = cfg.flags;
        copyStr(out.vendor, "Fake Vendor");
        copyStr(out.renderer, "Fake Renderer");
        copyStr(out.version, "4.6 fake");
        return TESSERA_SUCCESS;
    }
    TesseraContextConfig cfg;
    std::shared_ptr<ContextObject> shared;
};

class FakeBackend : public Backend {
  public:
    FakeBackend(const char* n, TesseraEnum onlyApi) : name_(n), api_(onlyApi) {}
    const char* name() const noexcept override {
        return name_;
    }
    TesseraResult createContext(const TesseraContextConfig& c, const std::shared_ptr<ContextObject>& share,
                                std::shared_ptr<ContextObject>& out) override {
        ++calls;
        if (c.api != api_)
            return TESSERA_ERROR_UNSUPPORTED;
        if (c.samples > 8)
            return TESSERA_ERROR_NO_MATCHING_CONFIG;
        if (c.samples == 5)
            throw std::bad_alloc();
        if (c.samples == 6)
            throw std::runtime_error("boom");
        out = std::make_shared<FakeContext>(c, share);
        return TESSERA_SUCCESS;
    }
    int calls = 0;

  private:
    const char* name_;
    TesseraEnum api_;
};

static std::vector<std::string> g_logs;
static void TESSERA_CALL logSink(int32_t level, const char* msg, void* user) {
    (void)user;
    g_logs.push_back(std::to_string(level) + ":" + msg);
}

static void testRuntime() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    TesseraContext ctx = 1234;

    // Before init.
    CHECK(tesseraTerminate() == TESSERA_ERROR_NOT_INITIALIZED);
    CHECK(tesseraCreateContext(&cfg, &ctx) == TESSERA_ERROR_NOT_INITIALIZED);
    CHECK(ctx == TESSERA_NULL_HANDLE); // out param always reset

    // Reference counting: two inits need two terminates.
    CHECK(tesseraInit() == TESSERA_SUCCESS);
    CHECK(tesseraInit() == TESSERA_SUCCESS);
    CHECK(Runtime::instance().refCount() == 2);

    // No backend registered in this build yet.
    CHECK(tesseraCreateContext(&cfg, &ctx) == TESSERA_ERROR_UNSUPPORTED);
    CHECK(std::strstr(tesseraGetLastErrorMessage(), "backend") != nullptr);

    auto desktop = std::make_shared<FakeBackend>("fake-desktop", TESSERA_API_OPENGL);
    auto es = std::make_shared<FakeBackend>("fake-es", TESSERA_API_OPENGL_ES);
    Runtime::instance().registerBackend(desktop);
    Runtime::instance().registerBackend(es);

    // Argument validation at the C boundary.
    CHECK(tesseraCreateContext(&cfg, nullptr) == TESSERA_ERROR_INVALID_ARGUMENT);
    CHECK(tesseraCreateContext(nullptr, &ctx) == TESSERA_ERROR_INVALID_ARGUMENT);

    // Create, describe, destroy.
    cfg.versionMajor = 3;
    cfg.versionMinor = 3;
    cfg.samples = 4;
    CHECK(tesseraCreateContext(&cfg, &ctx) == TESSERA_SUCCESS);
    CHECK(ctx != TESSERA_NULL_HANDLE);
    CHECK(g_liveFakeContexts == 1);

    TesseraContextInfo info = tesseraDefaultContextInfo();
    CHECK(tesseraGetContextInfo(ctx, &info) == TESSERA_SUCCESS);
    CHECK(info.versionMajor == 3 && info.versionMinor == 3 && info.samples == 4);
    CHECK(std::strcmp(info.renderer, "Fake Renderer") == 0);

    // A short structSize writes only that many bytes (older caller with a smaller struct).
    unsigned char raw[sizeof(TesseraContextInfo) + 16];
    std::memset(raw, 0xCD, sizeof raw);
    auto* small = reinterpret_cast<TesseraContextInfo*>(raw);
    small->structSize = 16;
    CHECK(tesseraGetContextInfo(ctx, small) == TESSERA_SUCCESS);
    CHECK(small->structSize == 16);
    CHECK(raw[16] == 0xCD && raw[17] == 0xCD); // untouched
    CHECK(tesseraGetContextInfo(ctx, nullptr) == TESSERA_ERROR_INVALID_ARGUMENT);
    TesseraContextInfo unset;
    std::memset(&unset, 0, sizeof unset);
    CHECK(tesseraGetContextInfo(ctx, &unset) == TESSERA_ERROR_INVALID_ARGUMENT); // structSize not set

    // Backend fallthrough: ES request is declined by the desktop backend, served by the ES one.
    TesseraContextConfig esCfg = tesseraDefaultContextConfig();
    esCfg.api = TESSERA_API_OPENGL_ES;
    TesseraContext esCtx = 0;
    CHECK(tesseraCreateContext(&esCfg, &esCtx) == TESSERA_SUCCESS);
    CHECK(desktop->calls >= 2 && es->calls == 1);

    // Sharing: valid, then invalid.
    TesseraContextConfig shareCfg = tesseraDefaultContextConfig();
    shareCfg.api = TESSERA_API_OPENGL;
    shareCfg.shareWith = ctx;
    TesseraContext sharing = 0;
    CHECK(tesseraCreateContext(&shareCfg, &sharing) == TESSERA_SUCCESS);
    shareCfg.shareWith = 0xdeadbeefull;
    TesseraContext bogus = 0;
    CHECK(tesseraCreateContext(&shareCfg, &bogus) == TESSERA_ERROR_INVALID_HANDLE);

    // Destroying the share parent while the child lives is safe (child holds a reference).
    CHECK(tesseraDestroyContext(ctx) == TESSERA_SUCCESS);
    CHECK(g_liveFakeContexts == 3 - 0); // parent kept alive by the sharing child; es + sharing + parent
    CHECK(tesseraDestroyContext(sharing) == TESSERA_SUCCESS);
    CHECK(g_liveFakeContexts == 1); // parent now released as well; only esCtx remains

    // Stale / double / null destroy and query.
    CHECK(tesseraDestroyContext(ctx) == TESSERA_ERROR_INVALID_HANDLE);
    CHECK(tesseraDestroyContext(TESSERA_NULL_HANDLE) == TESSERA_ERROR_INVALID_HANDLE);
    CHECK(tesseraGetContextInfo(ctx, &info) == TESSERA_ERROR_INVALID_HANDLE);
    info = tesseraDefaultContextInfo();

    // Backend failures map to the right codes; exceptions never escape.
    TesseraContextConfig failCfg = tesseraDefaultContextConfig();
    failCfg.api = TESSERA_API_OPENGL;
    TesseraContext none = 0;
    failCfg.samples = 16;
    CHECK(tesseraCreateContext(&failCfg, &none) == TESSERA_ERROR_NO_MATCHING_CONFIG);
    failCfg.samples = 5;
    CHECK(tesseraCreateContext(&failCfg, &none) == TESSERA_ERROR_OUT_OF_MEMORY);
    failCfg.samples = 6;
    CHECK(tesseraCreateContext(&failCfg, &none) == TESSERA_ERROR_UNKNOWN);
    CHECK(std::strcmp(tesseraGetLastErrorMessage(), "boom") == 0);
    CHECK(none == TESSERA_NULL_HANDLE);

    // First terminate keeps things alive (refcount 2 -> 1).
    g_logs.clear();
    tesseraSetLogCallback(logSink, nullptr);
    CHECK(tesseraTerminate() == TESSERA_SUCCESS);
    CHECK(g_liveFakeContexts == 1);
    CHECK(tesseraGetContextInfo(esCtx, &info) == TESSERA_SUCCESS);

    // Last terminate destroys leaked contexts, drops backends, and logs a warning.
    CHECK(tesseraTerminate() == TESSERA_SUCCESS);
    CHECK(g_liveFakeContexts == 0);
    CHECK(tesseraGetContextInfo(esCtx, &info) == TESSERA_ERROR_INVALID_HANDLE);
    bool warned = false;
    for (const auto& l : g_logs)
        if (l.rfind("2:", 0) == 0 && l.find("never destroyed") != std::string::npos)
            warned = true;
    CHECK(warned);
    tesseraSetLogCallback(nullptr, nullptr);

    // Re-init works, backends are gone again, old handles stay invalid.
    CHECK(tesseraInit() == TESSERA_SUCCESS);
    CHECK(tesseraCreateContext(&cfg, &ctx) == TESSERA_ERROR_UNSUPPORTED);
    CHECK(tesseraGetContextInfo(esCtx, &info) == TESSERA_ERROR_INVALID_HANDLE);
    CHECK(tesseraTerminate() == TESSERA_SUCCESS);

    // Concurrent init/terminate from many threads ends balanced.
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([] {
            for (int i = 0; i < 500; ++i) {
                CHECK(tesseraInit() == TESSERA_SUCCESS);
                CHECK(tesseraTerminate() == TESSERA_SUCCESS);
            }
        });
    for (auto& t : threads)
        t.join();
    CHECK(Runtime::instance().refCount() == 0);
}

// Thread-ownership rules live in the core, independent of any native backend.
static void testAffinity() {
    CHECK(tesseraInit() == TESSERA_SUCCESS);
    Runtime::instance().registerBackend(std::make_shared<FakeBackend>("fake", TESSERA_API_OPENGL));

    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    TesseraContext ctx = 0;
    CHECK(tesseraCreateContext(&cfg, &ctx) == TESSERA_SUCCESS);

    // The fake backend does not implement native binding, so core-level claims are exercised via UNSUPPORTED
    // being reported without leaving a stale claim behind.
    CHECK(tesseraMakeCurrent(ctx) == TESSERA_ERROR_UNSUPPORTED);
    CHECK(tesseraGetCurrentContext() == TESSERA_NULL_HANDLE);
    std::thread([&] {
        CHECK(tesseraMakeCurrent(ctx) == TESSERA_ERROR_UNSUPPORTED);
    }).join(); // no stuck owner
    CHECK(tesseraSetSwapInterval(ctx, 1) == TESSERA_ERROR_WRONG_THREAD);
    CHECK(tesseraSwapBuffers(ctx) == TESSERA_ERROR_WRONG_THREAD);
    CHECK(tesseraMakeCurrent(TESSERA_NULL_HANDLE) == TESSERA_SUCCESS);
    CHECK(tesseraGetProcAddress("glGetString") == nullptr);
    CHECK(tesseraGetProcAddress(nullptr) == nullptr);

    // ContextAffinity directly: live owner blocks others, a dead owner is reclaimed.
    ContextAffinity aff;
    const auto me = currentThreadToken();
    CHECK(aff.tryAcquire(me));
    std::shared_ptr<ThreadToken> other;
    std::thread([&] { other = currentThreadToken(); }).join(); // thread has exited => token dead
    CHECK(!other->alive.load());
    CHECK(aff.ownedBy(me) && !aff.busyElsewhere(me));
    aff.release(me);
    CHECK(aff.tryAcquire(other));  // claim it as a thread that is already dead...
    CHECK(!aff.busyElsewhere(me)); // ...which does not count as busy
    CHECK(aff.tryAcquire(me));     // and can be taken over
    CHECK(aff.ownedBy(me));
    std::shared_ptr<ThreadToken> live;
    std::atomic<bool> published{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        live = currentThreadToken();
        published.store(true); // release/acquire pair with the load below makes `live` safe to read
        while (!release.load())
            std::this_thread::yield();
    });
    while (!published.load())
        std::this_thread::yield();
    aff.release(me);
    CHECK(aff.tryAcquire(live));
    CHECK(aff.busyElsewhere(me));
    CHECK(!aff.tryAcquire(me)); // a live thread's claim is respected
    release = true;
    holder.join();
    CHECK(aff.tryAcquire(me)); // ...until that thread exits

    CHECK(tesseraDestroyContext(ctx) == TESSERA_SUCCESS);
    CHECK(tesseraTerminate() == TESSERA_SUCCESS);
}

// Whatever a broken driver returns from glGetString(GL_VERSION) must be rejected or parsed, never overflow.
static void testGlVersionParsing() {
    struct Case {
        const char* text;
        bool ok;
        bool es;
        int major, minor;
    };
    const Case cases[] = {
        {"4.6.0 NVIDIA 546.30", true, false, 4, 6},
        {"2.1 Metal - 83.1", true, false, 2, 1},
        {"4.2 (Compatibility Profile) Mesa 23.2.1", true, false, 4, 2},
        {"OpenGL ES 3.2 Mesa 23.2.1", true, true, 3, 2},
        {"OpenGL ES-CM 1.1 Fake", true, true, 1, 1},
        {"1.0", true, false, 1, 0},
        {"10.20", true, false, 10, 20},
        {"", false, false, 0, 0},
        {"garbage", false, false, 0, 0},
        {"OpenGL ES", false, true, 0, 0},
        {"4.", false, false, 0, 0},
        {".6", false, false, 0, 0},
        {"4,6", false, false, 0, 0},
        {" 4.5", false, false, 0, 0},
        {"-4.5", false, false, 0, 0},
        {"99999999999999999999.1", false, false, 0, 0}, // would overflow an int accumulator
        {"4.99999999999999999999", false, false, 0, 0},
        {"2147483648.0", false, false, 0, 0},
        {"OpenGL ES 99999999999.1", false, true, 0, 0},
    };
    for (const Case& c : cases) {
        bool es = false;
        int major = -1, minor = -1;
        const bool ok = parseGlVersion(c.text, es, major, minor);
        if (ok != c.ok) {
            std::fprintf(stderr, "parseGlVersion(\"%s\") -> %d, expected %d\n", c.text, ok, c.ok);
            ++g_failures;
        } else if (ok && (es != c.es || major != c.major || minor != c.minor)) {
            std::fprintf(stderr, "parseGlVersion(\"%s\") -> es=%d %d.%d, expected es=%d %d.%d\n", c.text, es,
                         major, minor, c.es, c.major, c.minor);
            ++g_failures;
        }
    }
    bool es = false;
    int major = 0, minor = 0;
    CHECK(!parseGlVersion(nullptr, es, major, minor));
}

// ---- a fake backend whose contexts can be bound, reset and lost on command
// ----------------------------------

static std::atomic<int> g_liveRobust{0};
static std::atomic<uint32_t> g_swapCaps{0};
static std::atomic<int32_t> g_swapApplied{TESSERA_SWAP_UNKNOWN};
static std::atomic<int32_t> g_swapMax{0};
static std::atomic<TesseraResult> g_swapResult{TESSERA_SUCCESS};
static std::atomic<int> g_resetToReport{0};
static std::atomic<bool> g_bindReportsLost{false};
static std::mutex g_destroyMutex;
static std::thread::id g_lastDestroyThread;

class RobustFakeContext : public ContextObject {
  public:
    explicit RobustFakeContext(const TesseraContextConfig& c) : cfg(c) {
        ++g_liveRobust;
    }
    ~RobustFakeContext() override {
        {
            std::lock_guard<std::mutex> lock(g_destroyMutex);
            g_lastDestroyThread = std::this_thread::get_id(); // which thread performed the "native" teardown
        }
        --g_liveRobust;
    }
    TesseraResult describe(TesseraContextInfo& out) const noexcept override {
        out.api = cfg.api;
        out.versionMajor = cfg.versionMajor;
        out.samples = cfg.samples;
        out.flags = TESSERA_FLAG_ROBUST_ACCESS | TESSERA_FLAG_RESET_NOTIFICATION;
        return TESSERA_SUCCESS;
    }
    TesseraResult bindCurrent() noexcept override {
        if (g_bindReportsLost)
            return fail(TESSERA_ERROR_CONTEXT_LOST, "device lost");
        bound = true;
        return TESSERA_SUCCESS;
    }
    void unbindCurrent() noexcept override {
        bound = false;
    }
    TesseraResult swapBuffers() noexcept override {
        return TESSERA_SUCCESS;
    }
    TesseraResult setSwapInterval(int32_t) noexcept override {
        return TESSERA_SUCCESS;
    }
    TesseraResult queryResetStatus(int32_t& out) noexcept override {
        out = g_resetToReport;
        return TESSERA_SUCCESS;
    }
    TesseraResult getSwapControl(TesseraSwapControl& out) noexcept override {
        out.caps = g_swapCaps;
        out.applied = g_swapApplied; // the core must hide this unless the backend also claims READBACK
        out.maxInterval = g_swapMax;
        return g_swapResult;
    }
    TesseraContextConfig cfg;
    bool bound = false;
};

class RobustFakeBackend : public Backend {
  public:
    const char* name() const noexcept override {
        return "robust-fake";
    }
    TesseraResult createContext(const TesseraContextConfig& c, const std::shared_ptr<ContextObject>&,
                                std::shared_ptr<ContextObject>& out) override {
        out = std::make_shared<RobustFakeContext>(c);
        return TESSERA_SUCCESS;
    }
};

static TesseraContext makeRobust(TesseraContextConfig cfg, TesseraContext shareWith = 0) {
    cfg.shareWith = shareWith;
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    return ctx;
}

// Drives the context into the lost state the way an application would: poll while the "driver" reports a
// reset.
static void loseContext(TesseraContext ctx, int32_t status) {
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    g_resetToReport = status;
    int32_t got = 99;
    EXPECT(tesseraGetResetStatus(ctx, &got), TESSERA_SUCCESS);
    CHECK(got == status);
    g_resetToReport = TESSERA_RESET_NONE; // the driver is quiet again; the context must stay lost regardless
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
}

static void testResetAndLoss() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    TesseraContext ctx = makeRobust(cfg);
    int32_t status = 99;

    EXPECT(tesseraGetResetStatus(ctx, &status), TESSERA_ERROR_WRONG_THREAD); // not current anywhere
    EXPECT(tesseraGetResetStatus(ctx, nullptr), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    g_resetToReport = TESSERA_RESET_NONE;
    EXPECT(tesseraGetResetStatus(ctx, &status), TESSERA_SUCCESS);
    CHECK(status == TESSERA_RESET_NONE);
    EXPECT(tesseraSwapBuffers(ctx), TESSERA_SUCCESS); // healthy

    g_resetToReport = TESSERA_RESET_GUILTY;
    EXPECT(tesseraGetResetStatus(ctx, &status), TESSERA_SUCCESS);
    CHECK(status == TESSERA_RESET_GUILTY);
    g_resetToReport = TESSERA_RESET_NONE; // the loss is permanent: asking again must not "heal" it
    status = 99;
    EXPECT(tesseraGetResetStatus(ctx, &status), TESSERA_SUCCESS);
    CHECK(status == TESSERA_RESET_GUILTY);
    EXPECT(tesseraSwapBuffers(ctx), TESSERA_ERROR_CONTEXT_LOST);
    EXPECT(tesseraSetSwapInterval(ctx, 1), TESSERA_ERROR_CONTEXT_LOST);
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_ERROR_CONTEXT_LOST);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE),
           TESSERA_SUCCESS); // letting go of a lost context always works
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_ERROR_CONTEXT_LOST);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);

    // A loss the backend discovers natively (e.g. EGL_CONTEXT_LOST at make-current) marks the context the
    // same way.
    TesseraContext native = makeRobust(cfg);
    g_bindReportsLost = true;
    EXPECT(tesseraMakeCurrent(native), TESSERA_ERROR_CONTEXT_LOST);
    g_bindReportsLost = false;
    EXPECT(tesseraMakeCurrent(native), TESSERA_ERROR_CONTEXT_LOST); // stays lost
    CHECK(tesseraGetCurrentContext() == TESSERA_NULL_HANDLE);
    EXPECT(tesseraDestroyContext(native), TESSERA_SUCCESS);

    // Each reset status is passed through unchanged.
    for (int32_t s : {TESSERA_RESET_INNOCENT, TESSERA_RESET_UNKNOWN}) {
        TesseraContext c = makeRobust(cfg);
        loseContext(c, s);
        EXPECT(tesseraDestroyContext(c), TESSERA_SUCCESS);
    }
    CHECK(g_liveRobust == 0);
}

// ---- context priority: native encoding, the ARB plan (GLX/WGL), strict satisfaction, validation

static void testContextPriority() {
    using namespace tessera;
    // Encoding round trip; unknown driver answers are DEFAULT, never guessed.
    for (uint32_t p : {TESSERA_PRIORITY_LOW, TESSERA_PRIORITY_MEDIUM, TESSERA_PRIORITY_HIGH})
        CHECK(priorityFromNative(nativePriority(p)) == p);
    CHECK(nativePriority(TESSERA_PRIORITY_DEFAULT) == 0 && nativePriority(77) == 0);
    CHECK(priorityFromNative(0) == TESSERA_PRIORITY_DEFAULT &&
          priorityFromNative(0x3104) == TESSERA_PRIORITY_DEFAULT);
    CHECK(nativePriority(TESSERA_PRIORITY_HIGH) == 0x3101 && nativePriority(TESSERA_PRIORITY_LOW) == 0x3103);

    auto has = [](bool priorityExt) {
        return [priorityExt](const char* e) {
            const std::string s = e;
            return s == "GLX_ARB_create_context" || s == "WGL_ARB_create_context" ||
                   (priorityExt && s == "GLX_EXT_context_priority");
        };
    };
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    ArbContextPlan plan;

    // Without a request nothing changes: the legacy path still suffices.
    CHECK(planArbContext(cfg, "GLX_", true, has(true), plan) == TESSERA_SUCCESS && plan.legacy);

    // With one the legacy path no longer suffices (it cannot carry attributes), and the attribute is sent.
    cfg.priority = TESSERA_PRIORITY_HIGH;
    CHECK(planArbContext(cfg, "GLX_", true, has(true), plan) == TESSERA_SUCCESS && !plan.legacy);
    bool sent = false;
    for (std::size_t i = 0; i + 1 < plan.extra.size(); i += 2)
        sent = sent || (plan.extra[i] == 0x3100 && plan.extra[i + 1] == 0x3101);
    CHECK(sent);

    // The extension is missing: strict refuses, best-effort drops the request (and the plan has no priority
    // attribute).
    CHECK(planArbContext(cfg, "GLX_", true, has(false), plan) == TESSERA_ERROR_NO_MATCHING_CONFIG);
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    CHECK(planArbContext(cfg, "GLX_", true, has(false), plan) == TESSERA_SUCCESS);
    sent = false;
    for (std::size_t i = 0; i + 1 < plan.extra.size(); i += 2)
        sent = sent || plan.extra[i] == 0x3100;
    CHECK(!sent);

    // WGL has no such extension at all, whatever the driver advertises.
    cfg.fallback = TESSERA_FALLBACK_STRICT;
    CHECK(planArbContext(cfg, "WGL_", true, has(true), plan) == TESSERA_ERROR_NO_MATCHING_CONFIG);
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    CHECK(planArbContext(cfg, "WGL_", true, has(true), plan) == TESSERA_SUCCESS);

    // Strict satisfaction compares with what the driver reports it granted.
    TesseraContextConfig want = tesseraDefaultContextConfig();
    want.api = TESSERA_API_OPENGL;
    want.priority = TESSERA_PRIORITY_HIGH;
    TesseraContextInfo got = tesseraDefaultContextInfo();
    got.api = TESSERA_API_OPENGL;
    got.priority = TESSERA_PRIORITY_HIGH;
    CHECK(contextSatisfies(want, got));
    got.priority =
        TESSERA_PRIORITY_MEDIUM; // drivers grant a lower level when the process lacks the privilege
    CHECK(!contextSatisfies(want, got));
    got.priority = TESSERA_PRIORITY_DEFAULT;
    CHECK(!contextSatisfies(want, got));
    want.priority = TESSERA_PRIORITY_DEFAULT; // no request: whatever the driver says is fine
    CHECK(contextSatisfies(want, got));
    got.priority = TESSERA_PRIORITY_LOW;
    CHECK(contextSatisfies(want, got));

    // Validation at the API boundary, and the pool keys on it.
    TesseraContextConfig bad = tesseraDefaultContextConfig();
    bad.api = TESSERA_API_OPENGL;
    bad.priority = 4;
    TesseraContext none = 0;
    EXPECT(tesseraCreateContext(&bad, &none), TESSERA_ERROR_INVALID_ARGUMENT);
    CHECK(none == TESSERA_NULL_HANDLE);
    TesseraContextConfig a = tesseraDefaultContextConfig(), b = a;
    a.api = b.api = TESSERA_API_OPENGL;
    a.priority = TESSERA_PRIORITY_HIGH;
    int32_t countA = -1, countB = -1;
    EXPECT(tesseraGetPooledCount(&a, &countA), TESSERA_SUCCESS);
    EXPECT(tesseraGetPooledCount(&b, &countB), TESSERA_SUCCESS);
    CHECK(countA == 0 && countB == 0);
}

// ---- choosing an EGL device: the matching rules, against faked device lists

static std::string fakeCanonical(const std::string& p) {
    // Stands in for realpath: the by-path links resolve to the render nodes they point at.
    if (p == "/dev/dri/by-path/pci-0000:01:00.0-render")
        return "/dev/dri/renderD128";
    if (p == "/dev/dri/by-path/pci-0000:2f:00.0-render")
        return "/dev/dri/renderD129";
    return p;
}

static tessera::EglDeviceDesc fakeDevice(const char* node, const char* uuid) {
    tessera::EglDeviceDesc d;
    if (node)
        d.renderNode = node;
    if (uuid)
        d.hasUuid = tessera::parseDeviceUuid(uuid, d.uuid);
    return d;
}

static void testDeviceSelection() {
    using namespace tessera;
    const std::string u0 = "11111111-2222-3333-4444-555555555555";
    const std::string u1 = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
    const std::vector<EglDeviceDesc> two = {fakeDevice("/dev/dri/renderD128", u0.c_str()),
                                            fakeDevice("/dev/dri/renderD129", u1.c_str())};

    // Classification.
    CHECK(classifyDeviceSelector("") == DeviceSelectorKind::Empty);
    CHECK(classifyDeviceSelector("0") == DeviceSelectorKind::Index);
    CHECK(classifyDeviceSelector("/dev/dri/renderD128") == DeviceSelectorKind::RenderNode);
    CHECK(classifyDeviceSelector("0000:01:00.0") == DeviceSelectorKind::Pci);
    CHECK(classifyDeviceSelector("0000:2F:00.0") == DeviceSelectorKind::Pci);
    CHECK(classifyDeviceSelector(u0) == DeviceSelectorKind::Uuid);
    CHECK(classifyDeviceSelector("uuid:" + u0) == DeviceSelectorKind::Uuid);
    CHECK(classifyDeviceSelector("11111111222233334444555555555555") == DeviceSelectorKind::Uuid);
    CHECK(classifyDeviceSelector("gpu0") == DeviceSelectorKind::Invalid);
    CHECK(classifyDeviceSelector("0000:zz:00.0") == DeviceSelectorKind::Invalid);
    CHECK(classifyDeviceSelector("11111111-2222-3333-4444-55555555555") ==
          DeviceSelectorKind::Invalid); // short

    // UUID parsing: both spellings, any case, and malformed text.
    std::array<std::uint8_t, 16> a{}, b{}, c{};
    CHECK(parseDeviceUuid(u0, a) && a[0] == 0x11 && a[15] == 0x55);
    CHECK(parseDeviceUuid("11111111222233334444555555555555", b) && a == b);
    CHECK(parseDeviceUuid("UUID:AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE", c) ==
          false); // the prefix is lowercase
    CHECK(parseDeviceUuid("uuid:AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE", c) && c[0] == 0xaa);
    CHECK(!parseDeviceUuid("11111111-2222-3333-4444-55555555555g", c));
    CHECK(!parseDeviceUuid("11111111_2222_3333_4444_555555555555", c));
    CHECK(!parseDeviceUuid("", c));

    // Index.
    CHECK(selectDevice("0", two, fakeCanonical).index == 0);
    CHECK(selectDevice("1", two, fakeCanonical).index == 1);
    CHECK(selectDevice("2", two, fakeCanonical).index == -1);
    CHECK(selectDevice("99999", two, fakeCanonical).index == -1); // beyond any count; must not overflow atoi
    CHECK(selectDevice("0", {}, fakeCanonical).index == -1);

    // Render node, directly and through the PCI by-path link.
    CHECK(selectDevice("/dev/dri/renderD129", two, fakeCanonical).index == 1);
    CHECK(selectDevice("/dev/dri/renderD130", two, fakeCanonical).index == -1);
    CHECK(selectDevice("0000:01:00.0", two, fakeCanonical).index == 0);
    CHECK(selectDevice("0000:2f:00.0", two, fakeCanonical).index == 1);
    CHECK(selectDevice("0000:2F:00.0", two, fakeCanonical).index == 1); // the by-path name is lowercase
    CHECK(selectDevice("0000:ff:1f.7", two, fakeCanonical).index == -1);

    // UUID: stable across boots, so this is the form to persist.
    CHECK(selectDevice(u1, two, fakeCanonical).index == 1);
    CHECK(selectDevice("uuid:" + u0, two, fakeCanonical).index == 0);
    CHECK(selectDevice("AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE", two, fakeCanonical).index == 1);
    const DeviceChoice nope = selectDevice("00000000-0000-0000-0000-000000000000", two, fakeCanonical);
    CHECK(nope.index == -1 && nope.reason.find("no EGL device has UUID") != std::string::npos);

    // A driver without the extension for the form asked for: refused with the reason, never another GPU.
    const std::vector<EglDeviceDesc> noUuid = {fakeDevice("/dev/dri/renderD128", nullptr),
                                               fakeDevice("/dev/dri/renderD129", nullptr)};
    DeviceChoice r = selectDevice(u0, noUuid, fakeCanonical);
    CHECK(r.index == -1 && r.reason.find("EGL_EXT_device_persistent_id") != std::string::npos);
    const std::vector<EglDeviceDesc> noNode = {fakeDevice(nullptr, u0.c_str()),
                                               fakeDevice(nullptr, u1.c_str())};
    r = selectDevice("/dev/dri/renderD128", noNode, fakeCanonical);
    CHECK(r.index == -1 && r.reason.find("EGL_EXT_device_drm_render_node") != std::string::npos);
    CHECK(selectDevice(u1, noNode, fakeCanonical).index == 1); // the UUID still works there

    // Devices where only some report the attribute (a software device next to a GPU).
    const std::vector<EglDeviceDesc> mixed = {fakeDevice(nullptr, nullptr),
                                              fakeDevice("/dev/dri/renderD128", u0.c_str())};
    CHECK(selectDevice("/dev/dri/renderD128", mixed, fakeCanonical).index == 1);
    CHECK(selectDevice(u0, mixed, fakeCanonical).index == 1);
    CHECK(selectDevice("0", mixed, fakeCanonical).index == 0);

    // Ambiguity is a driver bug and is refused rather than guessed.
    const std::vector<EglDeviceDesc> dup = {fakeDevice("/dev/dri/renderD128", u0.c_str()),
                                            fakeDevice("/dev/dri/renderD128", u0.c_str())};
    CHECK(selectDevice("/dev/dri/renderD128", dup, fakeCanonical).index == -1);
    CHECK(selectDevice(u0, dup, fakeCanonical).index == -1);

    // Garbage never selects anything and says what was expected.
    r = selectDevice("gpu0", two, fakeCanonical);
    CHECK(r.index == -1 && r.reason.find("not an index") != std::string::npos);
    CHECK(selectDevice("", two, fakeCanonical).index == -1);
    CHECK(selectDevice(std::string(5000, 'x'), two, fakeCanonical).index == -1);
}

// Several threads lose contexts while another keeps replacing the callback: every loss is reported exactly
// once, to one of the two callbacks, and nothing races (this runs under TSan in CI).
static std::atomic<int> g_lostToA{0}, g_lostToB{0};
static void TESSERA_CALL lostA(TesseraContext, int32_t, void*) {
    ++g_lostToA;
}
static void TESSERA_CALL lostB(TesseraContext, int32_t, void*) {
    ++g_lostToB;
}

static void testContextLostCallbackThreads() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    constexpr int kThreads = 4, kPerThread = 150;
    g_lostToA = g_lostToB = 0;
    g_resetToReport =
        TESSERA_RESET_GUILTY; // every poll reports a reset: each context is lost by its first poll
    tesseraSetContextLostCallback(lostA, nullptr);
    std::atomic<int> done{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                TesseraContext ctx = 0;
                if (tesseraCreateContext(&cfg, &ctx) != TESSERA_SUCCESS)
                    continue;
                int32_t status = 0;
                if (tesseraMakeCurrent(ctx) == TESSERA_SUCCESS) {
                    tesseraGetResetStatus(ctx, &status);
                    tesseraGetResetStatus(ctx, &status); // already lost: no second event
                    tesseraMakeCurrent(TESSERA_NULL_HANDLE);
                }
                tesseraDestroyContext(ctx);
            }
            ++done;
        });
    }
    bool useA = false;
    while (done.load() < kThreads) {
        tesseraSetContextLostCallback(useA ? lostA : lostB, nullptr);
        useA = !useA;
        std::this_thread::yield();
    }
    for (std::thread& w : workers)
        w.join();
    g_resetToReport = TESSERA_RESET_NONE;
    tesseraSetContextLostCallback(nullptr, nullptr);
    CHECK(g_lostToA.load() + g_lostToB.load() == kThreads * kPerThread);
    CHECK(g_liveRobust == 0);
}

// ---- swap control reporting: the pure decision logic, then the core's handling of a backend's answer

static void testSwapControlLogic() {
    using namespace tessera;
    GlxSwapInputs in;
    CHECK(chooseGlxSwapApi(in) == GlxSwapApi::None);
    CHECK(glxSwapCaps(in, true) == 0);

    // An advertised extension whose entry point did not resolve is not usable.
    in.hasExt = true;
    CHECK(chooseGlxSwapApi(in) == GlxSwapApi::None);
    in.extFn = true;
    CHECK(chooseGlxSwapApi(in) == GlxSwapApi::Ext);
    CHECK(glxSwapCaps(in, true) == TESSERA_SWAP_CAP_CONTROL); // no read-back entry point, no tear
    CHECK(glxSwapCaps(in, false) == 0);                       // pbuffer / no drawable: nothing to swap
    in.queryDrawableFn = true;
    CHECK(glxSwapCaps(in, true) == (TESSERA_SWAP_CAP_CONTROL | TESSERA_SWAP_CAP_READBACK));
    in.hasTear = true;
    CHECK(glxSwapCaps(in, true) ==
          (TESSERA_SWAP_CAP_CONTROL | TESSERA_SWAP_CAP_READBACK | TESSERA_SWAP_CAP_ADAPTIVE));

    // Preference order EXT > MESA > SGI; adaptive and drawable read-back only belong to EXT.
    GlxSwapInputs mesa;
    mesa.hasMesa = mesa.mesaFn = mesa.hasSgi = mesa.sgiFn = true;
    mesa.hasTear = true; // advertised, but it extends EXT only
    CHECK(chooseGlxSwapApi(mesa) == GlxSwapApi::Mesa);
    CHECK(glxSwapCaps(mesa, true) == TESSERA_SWAP_CAP_CONTROL);
    mesa.getMesaFn = true;
    CHECK(glxSwapCaps(mesa, true) == (TESSERA_SWAP_CAP_CONTROL | TESSERA_SWAP_CAP_READBACK));
    GlxSwapInputs sgi;
    sgi.hasSgi = sgi.sgiFn = true;
    sgi.queryDrawableFn = sgi.getMesaFn = true; // irrelevant to SGI
    CHECK(chooseGlxSwapApi(sgi) == GlxSwapApi::Sgi);
    CHECK(glxSwapCaps(sgi, true) == TESSERA_SWAP_CAP_CONTROL);
    GlxSwapInputs all = in;
    all.hasMesa = all.mesaFn = all.hasSgi = all.sgiFn = true;
    CHECK(chooseGlxSwapApi(all) == GlxSwapApi::Ext);

    // EXT reports a magnitude and flags the tearing mode separately.
    CHECK(glxAppliedInterval(1, false) == 1);
    CHECK(glxAppliedInterval(1, true) == -1);
    CHECK(glxAppliedInterval(0, true) == 0); // vsync off is never "adaptive"

    WglSwapInputs w;
    CHECK(wglSwapCaps(w, true) == 0);
    w.hasExt = true;
    CHECK(wglSwapCaps(w, true) == 0); // extension without the entry point
    w.setFn = true;
    CHECK(wglSwapCaps(w, true) == TESSERA_SWAP_CAP_CONTROL);
    CHECK(wglSwapCaps(w, false) == 0);
    w.getFn = true;
    w.hasTear = true;
    CHECK(wglSwapCaps(w, true) ==
          (TESSERA_SWAP_CAP_CONTROL | TESSERA_SWAP_CAP_READBACK | TESSERA_SWAP_CAP_ADAPTIVE));
}

static void testSwapControlApi() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    TesseraContext ctx = makeRobust(cfg);
    TesseraSwapControl sc = tesseraDefaultSwapControl();

    // Argument and thread rules mirror tesseraSetSwapInterval.
    EXPECT(tesseraGetSwapControl(ctx, nullptr), TESSERA_ERROR_INVALID_ARGUMENT);
    TesseraSwapControl unsized = {};
    EXPECT(tesseraGetSwapControl(ctx, &unsized), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraGetSwapControl(ctx, &sc), TESSERA_ERROR_WRONG_THREAD); // not current anywhere
    EXPECT(tesseraGetSwapControl(0xBAD, &sc), TESSERA_ERROR_INVALID_HANDLE);
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);

    // A backend with no swap control (the default) reports nothing, successfully.
    g_swapCaps = 0;
    g_swapApplied = 7; // a backend that forgets to clear it must not leak a value without READBACK
    sc = tesseraDefaultSwapControl();
    EXPECT(tesseraGetSwapControl(ctx, &sc), TESSERA_SUCCESS);
    CHECK(sc.caps == 0 && sc.requested == TESSERA_SWAP_UNKNOWN && sc.applied == TESSERA_SWAP_UNKNOWN);
    CHECK(sc.maxInterval == 0);

    // `requested` follows successful calls only.
    EXPECT(tesseraSetSwapInterval(ctx, 1), TESSERA_SUCCESS);
    g_swapCaps = TESSERA_SWAP_CAP_CONTROL | TESSERA_SWAP_CAP_READBACK | TESSERA_SWAP_CAP_ADAPTIVE;
    g_swapApplied = 1;
    g_swapMax = 4;
    EXPECT(tesseraGetSwapControl(ctx, &sc), TESSERA_SUCCESS);
    CHECK(sc.caps == g_swapCaps.load());
    CHECK(sc.requested == 1 && sc.applied == 1 && sc.maxInterval == 4);
    EXPECT(tesseraSetSwapInterval(ctx, -1), TESSERA_SUCCESS);
    g_swapApplied = -1;
    EXPECT(tesseraGetSwapControl(ctx, &sc), TESSERA_SUCCESS);
    CHECK(sc.requested == -1 && sc.applied == -1);

    // A driver that clamps: the request and what is in effect differ, and both are visible.
    EXPECT(tesseraSetSwapInterval(ctx, 8), TESSERA_SUCCESS);
    g_swapApplied = 4;
    EXPECT(tesseraGetSwapControl(ctx, &sc), TESSERA_SUCCESS);
    CHECK(sc.requested == 8 && sc.applied == 4);

    // An older (smaller) caller struct gets only what fits, and its own structSize back.
    TesseraSwapControl small = tesseraDefaultSwapControl();
    small.structSize = 3 * sizeof(uint32_t);
    small.applied = 12345; // beyond the caller's size: must stay untouched
    EXPECT(tesseraGetSwapControl(ctx, &small), TESSERA_SUCCESS);
    CHECK(small.structSize == 3 * sizeof(uint32_t) && small.caps == g_swapCaps.load());
    CHECK(small.applied == 12345);

    // Without READBACK the applied value is hidden by the core even if the backend wrote one.
    g_swapCaps = TESSERA_SWAP_CAP_CONTROL;
    g_swapApplied = 2;
    EXPECT(tesseraGetSwapControl(ctx, &sc), TESSERA_SUCCESS);
    CHECK(sc.applied == TESSERA_SWAP_UNKNOWN);

    // A native "context lost" from the backend marks the context lost, like any other call.
    g_swapResult = TESSERA_ERROR_CONTEXT_LOST;
    EXPECT(tesseraGetSwapControl(ctx, &sc), TESSERA_ERROR_CONTEXT_LOST);
    g_swapResult = TESSERA_SUCCESS;
    EXPECT(tesseraGetSwapControl(ctx, &sc), TESSERA_ERROR_CONTEXT_LOST); // stays lost
    int32_t lost = 0;
    EXPECT(tesseraIsContextLost(ctx, &lost, nullptr), TESSERA_SUCCESS);
    CHECK(lost == 1);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    g_swapCaps = 0;
    g_swapApplied = TESSERA_SWAP_UNKNOWN;
    g_swapMax = 0;
    CHECK(g_liveRobust == 0);
}

// ---- context-lost callback and tesseraIsContextLost

struct LostEvent {
    TesseraContext context;
    int32_t status;
    std::thread::id thread;
};
static std::mutex g_lostMutex;
static std::vector<LostEvent> g_lostEvents;
static TesseraContext g_recreated = TESSERA_NULL_HANDLE; // set by the "auto-recreate" callback below
static bool g_recreateInCallback = false;

static void TESSERA_CALL onLost(TesseraContext context, int32_t status, void* userData) {
    CHECK(userData == &g_lostEvents);
    {
        std::lock_guard<std::mutex> lock(g_lostMutex);
        g_lostEvents.push_back({context, status, std::this_thread::get_id()});
    }
    if (g_recreateInCallback) { // the callback may use the API: this is the "opt-in recreate" pattern
        uint32_t flags = 0;
        CHECK(tesseraRecreateContext(context, &g_recreated, &flags) == TESSERA_SUCCESS);
        int32_t lost = 0;
        CHECK(tesseraIsContextLost(context, &lost, nullptr) == TESSERA_SUCCESS && lost == 1);
    }
}

static void testContextLostCallback() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    int32_t lost = 99, status = 99;

    // Queries on a healthy / dead / bad-argument handle.
    TesseraContext healthy = makeRobust(cfg);
    EXPECT(tesseraIsContextLost(healthy, &lost, &status), TESSERA_SUCCESS);
    CHECK(lost == 0 && status == TESSERA_RESET_NONE);
    EXPECT(tesseraIsContextLost(healthy, &lost, nullptr), TESSERA_SUCCESS); // outStatus is optional
    EXPECT(tesseraIsContextLost(healthy, nullptr, &status), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraIsContextLost(0xBAD, &lost, &status), TESSERA_ERROR_INVALID_HANDLE);
    CHECK(lost == 0);

    // No callback registered: losing a context is silent, and the query still reports it.
    TesseraContext quiet = makeRobust(cfg);
    loseContext(quiet, TESSERA_RESET_GUILTY);
    EXPECT(tesseraIsContextLost(quiet, &lost, &status), TESSERA_SUCCESS);
    CHECK(lost == 1 && status == TESSERA_RESET_GUILTY);
    EXPECT(tesseraDestroyContext(quiet), TESSERA_SUCCESS);

    tesseraSetContextLostCallback(onLost, &g_lostEvents);
    g_lostEvents.clear();

    // Detected by polling: once, with the status, on the polling thread, and the context is already marked.
    TesseraContext polled = makeRobust(cfg);
    loseContext(polled, TESSERA_RESET_INNOCENT);
    EXPECT(tesseraGetResetStatus(polled, &status), TESSERA_ERROR_WRONG_THREAD); // not current: no new event
    CHECK(g_lostEvents.size() == 1);
    CHECK(g_lostEvents[0].context == polled && g_lostEvents[0].status == TESSERA_RESET_INNOCENT);
    CHECK(g_lostEvents[0].thread == std::this_thread::get_id());
    EXPECT(tesseraMakeCurrent(polled), TESSERA_ERROR_CONTEXT_LOST); // later calls do not fire it again
    EXPECT(tesseraSwapBuffers(polled), TESSERA_ERROR_CONTEXT_LOST);
    CHECK(g_lostEvents.size() == 1);

    // Detected natively at make-current: reported as UNKNOWN.
    TesseraContext native = makeRobust(cfg);
    g_bindReportsLost = true;
    EXPECT(tesseraMakeCurrent(native), TESSERA_ERROR_CONTEXT_LOST);
    g_bindReportsLost = false;
    CHECK(g_lostEvents.size() == 2);
    CHECK(g_lostEvents[1].context == native && g_lostEvents[1].status == TESSERA_RESET_UNKNOWN);

    // A context that is never lost never fires, and a healthy poll does not either.
    EXPECT(tesseraMakeCurrent(healthy), TESSERA_SUCCESS);
    g_resetToReport = TESSERA_RESET_NONE;
    EXPECT(tesseraGetResetStatus(healthy, &status), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    CHECK(g_lostEvents.size() == 2);

    // The callback may recreate: the whole "re-create on loss" helper fits in a few lines.
    g_recreateInCallback = true;
    TesseraContext victim = makeRobust(cfg);
    loseContext(victim, TESSERA_RESET_GUILTY);
    g_recreateInCallback = false;
    CHECK(g_lostEvents.size() == 3 && g_lostEvents[2].context == victim);
    CHECK(g_recreated != TESSERA_NULL_HANDLE);
    EXPECT(tesseraMakeCurrent(g_recreated), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(g_recreated), TESSERA_SUCCESS);
    g_recreated = TESSERA_NULL_HANDLE;

    // Disabled again: silent.
    tesseraSetContextLostCallback(nullptr, nullptr);
    g_lostEvents.clear();
    TesseraContext after = makeRobust(cfg);
    loseContext(after, TESSERA_RESET_GUILTY);
    CHECK(g_lostEvents.empty());

    for (TesseraContext c : {healthy, polled, native, victim, after})
        EXPECT(tesseraDestroyContext(c), TESSERA_SUCCESS);
    CHECK(g_liveRobust == 0);
}

static void testRecreateAndShareGroups() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    cfg.versionMajor = 3;
    cfg.samples = 4;
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;

    TesseraContext a = makeRobust(cfg);
    TesseraContext b = makeRobust(cfg, a);
    TesseraContext c = makeRobust(cfg, a);
    uint32_t flags = 99;
    TesseraContext n1 = 0;

    // The old share group has survivors: the new context joins it, with the same request as the lost one.
    loseContext(a, TESSERA_RESET_GUILTY);
    EXPECT(tesseraRecreateContext(a, &n1, &flags), TESSERA_SUCCESS);
    CHECK(flags & TESSERA_RECREATE_SHARE_PRESERVED);
    TesseraContextInfo info = tesseraDefaultContextInfo();
    EXPECT(tesseraGetContextInfo(n1, &info), TESSERA_SUCCESS);
    CHECK(info.versionMajor == 3 && info.samples == 4);
    EXPECT(tesseraMakeCurrent(n1), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);

    // Sharing with a lost context is refused: the caller must pick a survivor.
    TesseraContextConfig sharing = cfg;
    sharing.shareWith = a;
    TesseraContext refused = 0;
    EXPECT(tesseraCreateContext(&sharing, &refused), TESSERA_ERROR_CONTEXT_LOST);
    CHECK(refused == TESSERA_NULL_HANDLE);

    // Lose one more; the group still has c and n1.
    loseContext(b, TESSERA_RESET_INNOCENT);
    TesseraContext n2 = 0;
    EXPECT(tesseraRecreateContext(b, &n2, &flags), TESSERA_SUCCESS);
    CHECK(flags & TESSERA_RECREATE_SHARE_PRESERVED);

    // Every member lost: nothing to join, so the new context starts a fresh group and says so.
    loseContext(c, TESSERA_RESET_UNKNOWN);
    loseContext(n1, TESSERA_RESET_UNKNOWN);
    loseContext(n2, TESSERA_RESET_UNKNOWN);
    TesseraContext n3 = 0;
    flags = 99;
    EXPECT(tesseraRecreateContext(a, &n3, &flags), TESSERA_SUCCESS);
    CHECK(flags == 0);
    EXPECT(tesseraMakeCurrent(n3), TESSERA_SUCCESS); // and it is healthy
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);

    // Destroying members shrinks the group; a context that never shared has no group to preserve.
    EXPECT(tesseraDestroyContext(a), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(b), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(c), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(n1), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(n2), TESSERA_SUCCESS);
    TesseraContext alone = makeRobust(cfg);
    loseContext(alone, TESSERA_RESET_GUILTY);
    TesseraContext n4 = 0;
    flags = 99;
    EXPECT(tesseraRecreateContext(alone, &n4, &flags), TESSERA_SUCCESS);
    CHECK(flags == 0);
    EXPECT(tesseraDestroyContext(alone), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(n3), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(n4), TESSERA_SUCCESS);

    TesseraContext none = 0;
    EXPECT(tesseraRecreateContext(0xBAD, &none, &flags), TESSERA_ERROR_INVALID_HANDLE);
    EXPECT(tesseraRecreateContext(a, nullptr, &flags),
           TESSERA_ERROR_INVALID_ARGUMENT); // `a` is destroyed: still no crash
    CHECK(g_liveRobust == 0);
}

static void testDeferredDestruction() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;

    // Destroyed by one thread while another holds it: the handle dies at once, the teardown happens ON the
    // owner.
    TesseraContext ctx = makeRobust(cfg);
    std::atomic<bool> held{false}, destroyed{false};
    std::thread::id ownerId;
    std::thread owner([&] {
        ownerId = std::this_thread::get_id();
        EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
        held = true;
        while (!destroyed.load())
            std::this_thread::yield();
        CHECK(g_liveRobust == 1); // still alive: only this thread may release it
        CHECK(tessera::Runtime::instance().pendingDestructions() == 1);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE),
               TESSERA_SUCCESS); // the owner lets go: teardown happens here
        CHECK(g_liveRobust == 0);
    });
    while (!held.load())
        std::this_thread::yield();
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_ERROR_INVALID_HANDLE);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_ERROR_INVALID_HANDLE);
    destroyed = true;
    owner.join();
    CHECK(g_liveRobust == 0 && tessera::Runtime::instance().pendingDestructions() == 0);
    {
        std::lock_guard<std::mutex> lock(g_destroyMutex);
        CHECK(g_lastDestroyThread == ownerId);
    }

    // The owner exits without releasing: its exit hook performs the teardown, on its own thread.
    TesseraContext leaving = makeRobust(cfg);
    held = false;
    destroyed = false;
    std::thread::id leaverId;
    std::thread leaver([&] {
        leaverId = std::this_thread::get_id();
        EXPECT(tesseraMakeCurrent(leaving), TESSERA_SUCCESS);
        held = true;
        while (!destroyed.load())
            std::this_thread::yield();
    });
    while (!held.load())
        std::this_thread::yield();
    EXPECT(tesseraDestroyContext(leaving), TESSERA_SUCCESS);
    CHECK(g_liveRobust == 1);
    destroyed = true;
    leaver.join();
    CHECK(g_liveRobust == 0 && tessera::Runtime::instance().pendingDestructions() == 0);
    {
        std::lock_guard<std::mutex> lock(g_destroyMutex);
        CHECK(g_lastDestroyThread == leaverId);
    }

    // The owner switches to another context: the switch completes the teardown.
    TesseraContext first = makeRobust(cfg), second = makeRobust(cfg);
    held = false;
    destroyed = false;
    std::thread switcher([&] {
        EXPECT(tesseraMakeCurrent(first), TESSERA_SUCCESS);
        held = true;
        while (!destroyed.load())
            std::this_thread::yield();
        EXPECT(tesseraMakeCurrent(second), TESSERA_SUCCESS);
        CHECK(g_liveRobust == 1); // `first` is gone, `second` remains
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    });
    while (!held.load())
        std::this_thread::yield();
    EXPECT(tesseraDestroyContext(first), TESSERA_SUCCESS);
    destroyed = true;
    switcher.join();
    EXPECT(tesseraDestroyContext(second), TESSERA_SUCCESS);
    CHECK(g_liveRobust == 0);

    // tesseraTerminate while a destroyed context is still held elsewhere: nothing leaks, nothing crashes.
    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    EXPECT(tesseraInit(), TESSERA_SUCCESS);
    tessera::Runtime::instance().registerBackend(std::make_shared<RobustFakeBackend>());
    TesseraContext heldAtTerminate = makeRobust(cfg);
    held = false;
    destroyed = false;
    std::atomic<bool> terminated{false};
    std::thread late([&] {
        EXPECT(tesseraMakeCurrent(heldAtTerminate), TESSERA_SUCCESS);
        held = true;
        while (!terminated.load())
            std::this_thread::yield();
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE),
               TESSERA_SUCCESS); // the runtime is gone: harmless no-op
    });
    while (!held.load())
        std::this_thread::yield();
    EXPECT(tesseraDestroyContext(heldAtTerminate), TESSERA_SUCCESS);
    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    terminated = true;
    late.join();
    CHECK(g_liveRobust == 0);
    EXPECT(tesseraInit(), TESSERA_SUCCESS);
    tessera::Runtime::instance().registerBackend(std::make_shared<RobustFakeBackend>());
}

static void testResourceWarnings() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    g_logs.clear();
    tesseraSetLogCallback(logSink, nullptr);
    std::vector<TesseraContext> all;
    for (int i = 0; i < 300; ++i)
        all.push_back(makeRobust(cfg));
    tesseraSetLogCallback(nullptr, nullptr);
    int warnings = 0;
    for (const auto& l : g_logs)
        if (l.rfind("2:", 0) == 0 && l.find("live contexts") != std::string::npos)
            ++warnings;
    CHECK(warnings == 2); // once at 64 and once at 256, not once per context
    for (TesseraContext ctx : all)
        EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    CHECK(g_liveRobust == 0);
}

#if defined(__linux__)
// ---- the scoped X error handler must survive another library leaving it installed
// ------------------------------------
static int g_hostHandlerCalls = 0;
static int hostXHandler(tessera::x11::Display*, tessera::x11::XErrorEvent*) {
    ++g_hostHandlerCalls;
    return 0;
}

static void testXErrorScopeStaleInstall() {
    const tessera::x11::LinuxApi* api = tessera::x11::linuxApi();
    if (!api) {
        std::puts("  (libX11/libGL not available, skipped)");
        return;
    }
    using tessera::x11::XErrorHandler;
    const XErrorHandler original =
        api->xlib.XSetErrorHandler(&hostXHandler); // the "host application" installs its handler
    {
        tessera::x11::XErrorScope scope(*api, nullptr);
        // While we are active, another library pushes its own handler: it receives OUR handler as the one to
        // restore later.
        XErrorHandler ours = api->xlib.XSetErrorHandler(&hostXHandler);
        (void)api->xlib.XSetErrorHandler(ours);
        (void)ours;
    }
    // After the scope ended the host handler is back. Now the other library "pops" late and reinstalls what
    // it was given.
    XErrorHandler installed = api->xlib.XSetErrorHandler(&hostXHandler);
    CHECK(installed == &hostXHandler);
    // Emulate the leak: install our own handler again via a scope's constructor path, then abandon it without
    // restoring.
    XErrorHandler leaked = nullptr;
    {
        tessera::x11::XErrorScope scope(*api, nullptr);
        leaked = api->xlib.XSetErrorHandler(&hostXHandler); // = scopedHandler while the scope is active
        (void)api->xlib.XSetErrorHandler(leaked);
    }
    CHECK(leaked != nullptr && leaked != &hostXHandler);
    api->xlib.XSetErrorHandler(
        leaked); // the late restore: scopedHandler is now installed with no scope active

    {
        tessera::x11::XErrorScope scope(
            *api, nullptr); // used to record scopedHandler as "previous" and recurse forever
        XErrorHandler current = api->xlib.XSetErrorHandler(&hostXHandler);
        CHECK(current == leaked);
        (void)api->xlib.XSetErrorHandler(current);
        tessera::x11::XErrorEvent ev = {};
        g_hostHandlerCalls = 0;
        current(reinterpret_cast<tessera::x11::Display*>(0x1234), &ev); // an error on somebody else's display
        CHECK(g_hostHandlerCalls == 1); // forwarded to the real host handler exactly once, no recursion
    }
    // And the stale install is cleaned up: the host handler, not ours, is what the scope restored.
    XErrorHandler after = api->xlib.XSetErrorHandler(original);
    CHECK(after == &hostXHandler);
}
#endif

static void testPoolAndNativeApi() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    CHECK(tesseraPooledContextCount() == 0);

    // Pre-warmed contexts are real, counted, and handed out only for an identical request.
    EXPECT(tesseraPrewarmContexts(&cfg, 4), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() == 4);
    CHECK(g_liveRobust == 4);
    TesseraContext a = 0;
    EXPECT(tesseraCreateContext(&cfg, &a), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() == 3 && g_liveRobust == 4); // taken, not created
    TesseraContextConfig other = cfg;
    other.depthBits = 16;
    TesseraContext b = 0;
    EXPECT(tesseraCreateContext(&other, &b), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() == 3 && g_liveRobust == 5);
    TesseraContextConfig sharing = cfg;
    sharing.shareWith = a;
    TesseraContext c = 0;
    EXPECT(tesseraCreateContext(&sharing, &c), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() == 3 && g_liveRobust == 6); // sharing never takes a pooled context

    // Limits and misuse.
    EXPECT(tesseraPrewarmContexts(&cfg, 62),
           TESSERA_ERROR_INVALID_ARGUMENT); // 3 pooled + 62 > 64: refused up front
    CHECK(tesseraPooledContextCount() == 3);
    EXPECT(tesseraPrewarmContexts(&cfg, 61), TESSERA_SUCCESS); // exactly 64 in total
    CHECK(tesseraPooledContextCount() == 64);
    EXPECT(tesseraPrewarmContexts(&cfg, 1), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraPrewarmContexts(&sharing, 1), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraPrewarmContexts(&cfg, -1), TESSERA_ERROR_INVALID_ARGUMENT);

    // Many threads taking at once: each gets its own context, none twice, the count stays exact.
    std::vector<TesseraContext> taken(16, 0);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < taken.size(); ++i)
        threads.emplace_back([&, i] { EXPECT(tesseraCreateContext(&cfg, &taken[i]), TESSERA_SUCCESS); });
    for (auto& th : threads)
        th.join();
    std::sort(taken.begin(), taken.end());
    CHECK(std::adjacent_find(taken.begin(), taken.end()) == taken.end());
    CHECK(tesseraPooledContextCount() == 64 - 16);

    // A failing request pools nothing and leaves nothing behind.
    TesseraContextConfig impossible = cfg;
    impossible.samples = 99; // fails validation (a backend failure is covered by the real-driver tests)
    const int32_t before = tesseraPooledContextCount();
    EXPECT(tesseraPrewarmContexts(&impossible, 3), TESSERA_ERROR_INVALID_ARGUMENT);
    CHECK(tesseraPooledContextCount() == before);

    // Terminate empties the pool and destroys every pooled native context.
    for (TesseraContext h : taken)
        EXPECT(tesseraDestroyContext(h), TESSERA_SUCCESS);
    for (TesseraContext h : {a, b, c})
        EXPECT(tesseraDestroyContext(h), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() > 0);
    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() == 0);
    CHECK(g_liveRobust == 0);
    EXPECT(tesseraInit(), TESSERA_SUCCESS);
    tessera::Runtime::instance().registerBackend(std::make_shared<RobustFakeBackend>());

    // Backends that offer neither native handles nor window attach say so, without side effects.
    TesseraContext ctx = makeRobust(cfg);
    TesseraNativeHandles nh = tesseraDefaultNativeHandles();
    EXPECT(tesseraGetNativeHandles(ctx, &nh), TESSERA_ERROR_UNSUPPORTED);
    TesseraContext none = 0;
    EXPECT(tesseraCreateContextForWindow(&cfg, 0x1234, 0, &none), TESSERA_ERROR_UNSUPPORTED);
    CHECK(none == TESSERA_NULL_HANDLE && g_liveRobust == 1);
    EXPECT(tesseraCreateContextForWindow(&cfg, 0, 0, &none), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraCreateContextForWindow(&cfg, 0x1234, 0x80, &none), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
}

static void testProbe() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    const std::size_t liveBefore = tessera::Runtime::instance().liveContexts();
    const int32_t pooledBefore = tesseraPooledContextCount();

    TesseraProbeReport rep = tesseraDefaultProbeReport();
    EXPECT(tesseraProbe(&cfg, &rep), TESSERA_SUCCESS);
    CHECK(rep.result == TESSERA_SUCCESS && std::strcmp(rep.backend, "robust-fake") == 0 &&
          rep.message[0] == '\0');
    CHECK(g_liveRobust == 0);                                         // created and destroyed again
    CHECK(tessera::Runtime::instance().liveContexts() == liveBefore); // no handle was registered
    CHECK(tesseraPooledContextCount() == pooledBefore);

    // A probe never takes a pooled context (it must test the driver, not a cache).
    EXPECT(tesseraPrewarmContexts(&cfg, 1), TESSERA_SUCCESS);
    EXPECT(tesseraProbe(&cfg, &rep), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() == pooledBefore + 1 && g_liveRobust == 1);

    // Argument errors: report untouched for a bad config/report, filled in for a request-level failure.
    TesseraProbeReport untouched = tesseraDefaultProbeReport();
    untouched.message[0] = 'x';
    TesseraContextConfig bad = cfg;
    bad.samples = 99;
    EXPECT(tesseraProbe(&bad, &untouched), TESSERA_ERROR_INVALID_ARGUMENT);
    CHECK(untouched.message[0] == 'x');
    EXPECT(tesseraProbe(nullptr, &rep), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraProbe(&cfg, nullptr), TESSERA_ERROR_INVALID_ARGUMENT);
    TesseraProbeReport unsized;
    std::memset(&unsized, 0, sizeof unsized);
    EXPECT(tesseraProbe(&cfg, &unsized), TESSERA_ERROR_INVALID_ARGUMENT);
    TesseraContextConfig sharing = cfg;
    sharing.shareWith = 1;
    rep = tesseraDefaultProbeReport();
    EXPECT(tesseraProbe(&sharing, &rep), TESSERA_ERROR_INVALID_ARGUMENT);
    CHECK(rep.result == TESSERA_ERROR_INVALID_ARGUMENT && std::strstr(rep.message, "share") != nullptr);

    // A caller with a smaller report struct gets only what fits.
    unsigned char raw[sizeof(TesseraProbeReport) + 16];
    std::memset(raw, 0xCD, sizeof raw);
    auto* small = reinterpret_cast<TesseraProbeReport*>(raw);
    small->structSize = 8;
    EXPECT(tesseraProbe(&cfg, small), TESSERA_SUCCESS);
    CHECK(small->structSize == 8 && small->result == TESSERA_SUCCESS && raw[8] == 0xCD);

    // Many threads probing at once.
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < 50; ++i) {
                TesseraProbeReport r = tesseraDefaultProbeReport();
                EXPECT(tesseraProbe(&cfg, &r), TESSERA_SUCCESS);
            }
        });
    for (auto& th : threads)
        th.join();
    CHECK(g_liveRobust == 1); // only the pooled one remains
    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    CHECK(g_liveRobust == 0);
    TesseraProbeReport before = tesseraDefaultProbeReport();
    EXPECT(tesseraProbe(&cfg, &before), TESSERA_ERROR_NOT_INITIALIZED);
    CHECK(before.result == TESSERA_ERROR_NOT_INITIALIZED);
    EXPECT(tesseraInit(), TESSERA_SUCCESS);
    tessera::Runtime::instance().registerBackend(std::make_shared<RobustFakeBackend>());
}

#if defined(__linux__) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define TESSERA_RSS_TEST_OFF 1
#endif
#endif
#else
#define TESSERA_RSS_TEST_OFF 1
#endif

#ifndef TESSERA_RSS_TEST_OFF
static long residentKb() {
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
        if (line.rfind("VmRSS:", 0) == 0)
            return std::atol(line.c_str() + 6);
    return -1;
}

// Tessera's own bookkeeping (handle table, share groups, zombies, thread tokens, pool) must not grow with the
// number of contexts created and destroyed. A fake backend takes every driver out of the picture: when the
// soak test showed resident memory growing on Linux, this is what showed it was Mesa's Direct3D12 driver (the
// same loop against raw libGL grows the same way) and not us.
static void testNoGrowthWithManyContexts() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    const auto work = [&cfg](int cycles) {
        for (int i = 0; i < cycles; ++i) {
            TesseraContext a = 0, b = 0, c = 0;
            tesseraCreateContext(&cfg, &a);
            TesseraContextConfig sharing = cfg;
            sharing.shareWith = a;
            tesseraCreateContext(&sharing, &b);
            tesseraCreateContext(&cfg, &c);
            tesseraMakeCurrent(a);
            tesseraMakeCurrent(b);
            tesseraMakeCurrent(TESSERA_NULL_HANDLE);
            tesseraDestroyContext(a);
            tesseraDestroyContext(b);
            tesseraDestroyContext(c);
        }
    };
    const auto run = [&work](int cycles) {
        std::vector<std::thread> threads;
        for (int t = 0; t < 3; ++t)
            threads.emplace_back(work, cycles);
        for (auto& t : threads)
            t.join();
    };
    run(5000); // warm-up: allocator arenas, thread-local state
    const long before = residentKb();
    run(30000); // 270,000 contexts
    const long after = residentKb();
    std::printf("  resident memory %ld KB -> %ld KB over 270000 contexts\n", before, after);
    CHECK(after - before < 2048); // a leak of even 8 bytes per context would be ~2 MB; the real figure is ~0
    CHECK(g_liveRobust == 0 && tessera::Runtime::instance().liveContexts() == 0 &&
          tessera::Runtime::instance().pendingDestructions() == 0);
}
#endif

static void testAdapterRequestsAgainstFake() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    cfg.gpuPreference = TESSERA_GPU_LOW_POWER;
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx),
           TESSERA_ERROR_UNSUPPORTED); // the fake backend cannot choose a GPU
    CHECK(ctx == TESSERA_NULL_HANDLE && g_liveRobust == 0);
    cfg.gpuPreference = 7;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_ERROR_INVALID_ARGUMENT);
    cfg.gpuPreference = TESSERA_GPU_HIGH_PERFORMANCE;
    cfg.adapterLuid = 5;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_ERROR_INVALID_ARGUMENT); // both at once
    cfg.gpuPreference = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_ERROR_UNSUPPORTED);
    TesseraContextConfig pooled = tesseraDefaultContextConfig();
    pooled.api = TESSERA_API_OPENGL;
    EXPECT(tesseraPrewarmContexts(&pooled, 1), TESSERA_SUCCESS);
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_ERROR_UNSUPPORTED);
    CHECK(tesseraPooledContextCount() >= 1); // the default pool entry is not used for a GPU request
    TesseraContext plain = makeRobust(pooled);
    EXPECT(tesseraDestroyContext(plain), TESSERA_SUCCESS);
}

static void testPoolTopUpAgainstFake() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    tesseraPoolClear();
    int32_t created = -1;
    EXPECT(tesseraPoolTopUp(&cfg, 3, &created), TESSERA_SUCCESS);
    CHECK(created == 3 && g_liveRobust == 3);
    EXPECT(tesseraPoolTopUp(&cfg, 3, &created), TESSERA_SUCCESS);
    CHECK(created == 0 && g_liveRobust == 3);
    CHECK(tesseraPoolClear() == 3 && g_liveRobust == 0); // clearing destroys the native contexts
    EXPECT(tesseraPoolTopUp(&cfg, 2, &created), TESSERA_SUCCESS);
    EXPECT(tesseraTerminate(), TESSERA_SUCCESS); // terminate empties the pool too
    CHECK(g_liveRobust == 0 && tesseraPooledContextCount() == 0);
    int32_t count = 7;
    EXPECT(tesseraGetPooledCount(&cfg, &count), TESSERA_SUCCESS);
    CHECK(count == 0);
    EXPECT(tesseraPoolTopUp(&cfg, 1, &created), TESSERA_ERROR_NOT_INITIALIZED);
    EXPECT(tesseraInit(), TESSERA_SUCCESS);
    tessera::Runtime::instance().registerBackend(std::make_shared<RobustFakeBackend>());
}

static void testLifecycleRobustness() {
    CHECK(tesseraInit() == TESSERA_SUCCESS);
    tessera::Runtime::instance().registerBackend(std::make_shared<RobustFakeBackend>());
    testResetAndLoss();
    testContextLostCallback();
    testContextLostCallbackThreads();
    testDeviceSelection();
    testContextPriority();
    testSwapControlLogic();
    testSwapControlApi();
    testRecreateAndShareGroups();
    testDeferredDestruction();
    testResourceWarnings();
    testPoolAndNativeApi();
#if defined(__linux__)
    testXErrorScopeStaleInstall();
#endif
    testProbe();
    testAdapterRequestsAgainstFake();
    testPoolTopUpAgainstFake();
#ifndef TESSERA_RSS_TEST_OFF
    testNoGrowthWithManyContexts();
#endif
    CHECK(tesseraTerminate() == TESSERA_SUCCESS);
}

int main() {
    Runtime::disableBuiltinBackendsForTesting(); // inject fakes only; real backends have their own tests
    testHandleTable();
    testGlVersionParsing();
    testConfig();
    testForeignGuard();
    testLibrary();
    testRuntime();
    testAffinity();
    testLifecycleRobustness();
    if (g_failures == 0)
        std::puts("test_core: OK");
    return g_failures == 0 ? 0 : 1;
}
