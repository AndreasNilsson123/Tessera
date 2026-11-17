// White-box tests for the Phase 1 core. Links the core objects directly (internals are not exported by
// the shared library). A fake backend exercises the full create/describe/destroy/terminate flow without
// needing a GPU or window system.
#include <algorithm>
#include <tessera/tessera.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "core/backend.hpp"
#include "core/boundary.hpp"
#include "core/config.hpp"
#include "core/foreign_state.hpp"
#include "core/handle_table.hpp"
#include "core/library.hpp"
#include "core/runtime.hpp"
#include "platform/common/gl_query.hpp"
#include "core/thread_affinity.hpp"

static int g_failures = 0;
#define CHECK(cond)                                                                                      \
    do {                                                                                                 \
        if (!(cond)) {                                                                                   \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                \
            ++g_failures;                                                                                \
        }                                                                                                \
    } while (0)

using namespace tessera;

#define EXPECT(call, want)                                                                               \
    do {                                                                                                 \
        const TesseraResult got_ = (call);                                                               \
        if (got_ != (want)) {                                                                            \
            std::fprintf(stderr, "%s:%d: %s -> %s, expected %s (%s)\n", __FILE__, __LINE__, #call,       \
                         tesseraResultString(got_), tesseraResultString(want), tesseraGetLastErrorMessage()); \
            ++g_failures;                                                                                \
        }                                                                                                \
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
    CHECK(t.get(a) == nullptr);       // stale
    CHECK(t.remove(a) == nullptr);    // double remove
    const TesseraHandle b = t.insert(std::make_shared<Dummy>(2));
    CHECK(b != a);                    // same slot, new generation
    CHECK(static_cast<uint32_t>(b) == static_cast<uint32_t>(a));
    CHECK(t.get(a) == nullptr && t.get(b)->value == 2);

    // A user of an object keeps it alive across a concurrent destroy.
    std::shared_ptr<Dummy> held = t.get(b);
    t.remove(b);
    CHECK(held->value == 2);

    // Capacity limit.
    std::vector<TesseraHandle> hs;
    for (int i = 0; i < 8; ++i) hs.push_back(t.insert(std::make_shared<Dummy>(i)));
    CHECK(t.insert(std::make_shared<Dummy>(99)) == TESSERA_NULL_HANDLE);
    CHECK(t.size() == 8);

    auto drained = t.closeAndDrain();
    CHECK(drained.size() == 8);
    CHECK(t.size() == 0);
    for (TesseraHandle h : hs) CHECK(t.get(h) == nullptr);
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
    for (auto& th : threads) th.join();
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

    bad = cfg; bad.api = 0;                                 CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg; bad.api = 99;                                CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg; bad.api = TESSERA_API_OPENGL_ES; bad.profile = TESSERA_PROFILE_CORE;
                                                            CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg; bad.profile = TESSERA_PROFILE_CORE; bad.versionMajor = 3; bad.versionMinor = 1;
                                                            CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg; bad.profile = TESSERA_PROFILE_CORE; bad.versionMajor = 3; bad.versionMinor = 2;
                                                            CHECK(normalizeConfig(&bad, out) == TESSERA_SUCCESS);
    bad = cfg; bad.flags = 0x80000000u;                     CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg; bad.flags = TESSERA_FLAG_RESET_NOTIFICATION; CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg; bad.flags = TESSERA_FLAG_NO_ERROR | TESSERA_FLAG_DEBUG;
                                                            CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg; bad.samples = -1;                            CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg; bad.depthBits = 99;                          CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg; bad.surface = 77;                            CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);
    bad = cfg; bad.reserved[3] = 1;                         CHECK(normalizeConfig(&bad, out) != TESSERA_SUCCESS);

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
    struct State { int value; };
    static inline int current = 0;
    static inline int captures = 0;
    static inline int restores = 0;
    static State capture() noexcept { ++captures; return {current}; }
    static void restore(const State& s) noexcept { ++restores; current = s.value; }
};

static int guardedEarlyReturn(bool fail) {
    ScopedForeignState<FakeOps> guard;
    FakeOps::current = 42; // pretend we made our own context current
    if (fail) return -1;
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
    } catch (int) {}
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
    if (!lib) lib = DynamicLibrary::open("libSystem.dylib");
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
    for (; i + 1 < N && src[i] != char(0); ++i) dst[i] = src[i];
    dst[i] = char(0);
}

class FakeContext : public ContextObject {
  public:
    explicit FakeContext(const TesseraContextConfig& c, std::shared_ptr<ContextObject> share)
        : cfg(c), shared(std::move(share)) {
        ++g_liveFakeContexts;
    }
    ~FakeContext() override { --g_liveFakeContexts; }
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
    const char* name() const noexcept override { return name_; }
    TesseraResult createContext(const TesseraContextConfig& c, const std::shared_ptr<ContextObject>& share,
                                std::shared_ptr<ContextObject>& out) override {
        ++calls;
        if (c.api != api_) return TESSERA_ERROR_UNSUPPORTED;
        if (c.samples > 8) return TESSERA_ERROR_NO_MATCHING_CONFIG;
        if (c.samples == 5) throw std::bad_alloc();
        if (c.samples == 6) throw std::runtime_error("boom");
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
    cfg.versionMajor = 3; cfg.versionMinor = 3; cfg.samples = 4;
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
    failCfg.samples = 16; CHECK(tesseraCreateContext(&failCfg, &none) == TESSERA_ERROR_NO_MATCHING_CONFIG);
    failCfg.samples = 5;  CHECK(tesseraCreateContext(&failCfg, &none) == TESSERA_ERROR_OUT_OF_MEMORY);
    failCfg.samples = 6;  CHECK(tesseraCreateContext(&failCfg, &none) == TESSERA_ERROR_UNKNOWN);
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
        if (l.rfind("2:", 0) == 0 && l.find("never destroyed") != std::string::npos) warned = true;
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
    for (auto& t : threads) t.join();
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
    std::thread([&] { CHECK(tesseraMakeCurrent(ctx) == TESSERA_ERROR_UNSUPPORTED); }).join(); // no stuck owner
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
    CHECK(aff.tryAcquire(other)); // claim it as a thread that is already dead...
    CHECK(!aff.busyElsewhere(me)); // ...which does not count as busy
    CHECK(aff.tryAcquire(me));    // and can be taken over
    CHECK(aff.ownedBy(me));
    std::shared_ptr<ThreadToken> live;
    std::atomic<bool> published{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {
        live = currentThreadToken();
        published.store(true); // release/acquire pair with the load below makes `live` safe to read
        while (!release.load()) std::this_thread::yield();
    });
    while (!published.load()) std::this_thread::yield();
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
    struct Case { const char* text; bool ok; bool es; int major, minor; };
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
            std::fprintf(stderr, "parseGlVersion(\"%s\") -> es=%d %d.%d, expected es=%d %d.%d\n", c.text, es, major, minor,
                         c.es, c.major, c.minor);
            ++g_failures;
        }
    }
    bool es = false;
    int major = 0, minor = 0;
    CHECK(!parseGlVersion(nullptr, es, major, minor));
}

// ---- a fake backend whose contexts can be bound, reset and lost on command ----------------------------------

static std::atomic<int> g_liveRobust{0};
static std::atomic<int> g_resetToReport{0};
static std::atomic<bool> g_bindReportsLost{false};
static std::mutex g_destroyMutex;
static std::thread::id g_lastDestroyThread;

class RobustFakeContext : public ContextObject {
  public:
    explicit RobustFakeContext(const TesseraContextConfig& c) : cfg(c) { ++g_liveRobust; }
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
        if (g_bindReportsLost) return fail(TESSERA_ERROR_CONTEXT_LOST, "device lost");
        bound = true;
        return TESSERA_SUCCESS;
    }
    void unbindCurrent() noexcept override { bound = false; }
    TesseraResult swapBuffers() noexcept override { return TESSERA_SUCCESS; }
    TesseraResult setSwapInterval(int32_t) noexcept override { return TESSERA_SUCCESS; }
    TesseraResult queryResetStatus(int32_t& out) noexcept override {
        out = g_resetToReport;
        return TESSERA_SUCCESS;
    }
    TesseraContextConfig cfg;
    bool bound = false;
};

class RobustFakeBackend : public Backend {
  public:
    const char* name() const noexcept override { return "robust-fake"; }
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

// Drives the context into the lost state the way an application would: poll while the "driver" reports a reset.
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
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS); // letting go of a lost context always works
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_ERROR_CONTEXT_LOST);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);

    // A loss the backend discovers natively (e.g. EGL_CONTEXT_LOST at make-current) marks the context the same way.
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
    EXPECT(tesseraRecreateContext(a, nullptr, &flags), TESSERA_ERROR_INVALID_ARGUMENT); // `a` is destroyed: still no crash
    CHECK(g_liveRobust == 0);
}

static void testDeferredDestruction() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;

    // Destroyed by one thread while another holds it: the handle dies at once, the teardown happens ON the owner.
    TesseraContext ctx = makeRobust(cfg);
    std::atomic<bool> held{false}, destroyed{false};
    std::thread::id ownerId;
    std::thread owner([&] {
        ownerId = std::this_thread::get_id();
        EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
        held = true;
        while (!destroyed.load()) std::this_thread::yield();
        CHECK(g_liveRobust == 1);                               // still alive: only this thread may release it
        CHECK(tessera::Runtime::instance().pendingDestructions() == 1);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS); // the owner lets go: teardown happens here
        CHECK(g_liveRobust == 0);
    });
    while (!held.load()) std::this_thread::yield();
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
        while (!destroyed.load()) std::this_thread::yield();
    });
    while (!held.load()) std::this_thread::yield();
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
        while (!destroyed.load()) std::this_thread::yield();
        EXPECT(tesseraMakeCurrent(second), TESSERA_SUCCESS);
        CHECK(g_liveRobust == 1); // `first` is gone, `second` remains
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    });
    while (!held.load()) std::this_thread::yield();
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
        while (!terminated.load()) std::this_thread::yield();
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS); // the runtime is gone: harmless no-op
    });
    while (!held.load()) std::this_thread::yield();
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
    for (int i = 0; i < 300; ++i) all.push_back(makeRobust(cfg));
    tesseraSetLogCallback(nullptr, nullptr);
    int warnings = 0;
    for (const auto& l : g_logs)
        if (l.rfind("2:", 0) == 0 && l.find("live contexts") != std::string::npos) ++warnings;
    CHECK(warnings == 2); // once at 64 and once at 256, not once per context
    for (TesseraContext ctx : all) EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    CHECK(g_liveRobust == 0);
}

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
    EXPECT(tesseraPrewarmContexts(&cfg, 62), TESSERA_ERROR_INVALID_ARGUMENT); // 3 pooled + 62 > 64: refused up front
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
    for (auto& th : threads) th.join();
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
    for (TesseraContext h : taken) EXPECT(tesseraDestroyContext(h), TESSERA_SUCCESS);
    for (TesseraContext h : {a, b, c}) EXPECT(tesseraDestroyContext(h), TESSERA_SUCCESS);
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

static void testLifecycleRobustness() {
    CHECK(tesseraInit() == TESSERA_SUCCESS);
    tessera::Runtime::instance().registerBackend(std::make_shared<RobustFakeBackend>());
    testResetAndLoss();
    testRecreateAndShareGroups();
    testDeferredDestruction();
    testResourceWarnings();
    testPoolAndNativeApi();
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
    if (g_failures == 0) std::puts("test_core: OK");
    return g_failures == 0 ? 0 : 1;
}
