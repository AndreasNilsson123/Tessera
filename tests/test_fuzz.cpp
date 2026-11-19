// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
// Seeded, deterministic fuzzing of everything that parses untrusted input: configuration structs, handles,
// struct sizes, proc-address names and driver version strings. Runs in seconds without a GPU (a fake backend
// serves the requests) and is meant to run under ASan/UBSan: every caller-provided buffer is allocated with
// EXACTLY the size claimed, so one byte read or written past it is a sanitizer report.
// TESSERA_FUZZ_ITERATIONS scales the run (default 20000 per section); TESSERA_FUZZ_SEED changes the stream. A
// failing run prints its seed.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1 // getenv: the test reads its own knobs
#endif
#include <tessera/tessera.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/runtime.hpp"
#include "platform/common/gl_query.hpp"

using namespace tessera;

static int g_failures = 0;
#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                    \
            ++g_failures;                                                                                    \
        }                                                                                                    \
    } while (0)

namespace {

std::atomic<int> g_live{0};

class FuzzContext : public ContextObject {
  public:
    FuzzContext() {
        ++g_live;
    }
    ~FuzzContext() override {
        --g_live;
    }
    TesseraResult describe(TesseraContextInfo& out) const noexcept override {
        std::memcpy(out.vendor, "fuzz", 5);
        return TESSERA_SUCCESS;
    }
    TesseraResult nativeHandles(TesseraNativeHandles& out) const noexcept override {
        out.kind = TESSERA_NATIVE_EGL;
        out.handles[0] = 1;
        return TESSERA_SUCCESS;
    }
};

class FuzzBackend : public Backend {
  public:
    const char* name() const noexcept override {
        return "fuzz";
    }
    TesseraResult createContext(const TesseraContextConfig&, const std::shared_ptr<ContextObject>&,
                                std::shared_ptr<ContextObject>& out) override {
        out = std::make_shared<FuzzContext>();
        return TESSERA_SUCCESS;
    }
    TesseraResult createContextForWindow(const TesseraContextConfig& c, uint64_t, uint32_t,
                                         const std::shared_ptr<ContextObject>& s,
                                         std::shared_ptr<ContextObject>& out) override {
        return createContext(c, s, out);
    }
    void* getProcAddress(const char* name) noexcept override {
        // Mirrors the real backends' rule: only "gl..." names resolve.
        return (name[0] == 'g' && name[1] == 'l') ? reinterpret_cast<void*>(&g_live) : nullptr;
    }
};

std::mt19937_64 g_rng;
uint64_t rnd() {
    return g_rng();
}
uint32_t pick(uint32_t n) {
    return static_cast<uint32_t>(rnd() % n);
}

// A struct image of exactly `size` bytes in its own allocation: valid-looking fields most of the time,
// garbage otherwise.
struct Exact {
    std::unique_ptr<unsigned char[]> data;
    std::size_t size;
};
// The one thing a caller must always provide is the leading structSize field, so buffers are never smaller
// than that.
Exact exactBuffer(std::size_t size) {
    if (size < sizeof(uint32_t))
        size = sizeof(uint32_t);
    Exact e{std::make_unique<unsigned char[]>(size ? size : 1), size};
    for (std::size_t i = 0; i < size; ++i)
        e.data[i] = static_cast<unsigned char>(rnd());
    return e;
}

uint32_t interesting() {
    static const uint32_t v[] = {0,  1,  2,  3,  4,   5,   7,           8,           16,
                                 24, 32, 64, 65, 255, 256, 0x7fffffffu, 0x80000000u, 0xffffffffu};
    return v[pick(sizeof v / sizeof *v)];
}

// A configuration image with a plausible structSize and each field either sane or hostile.
Exact configImage() {
    const std::size_t full = sizeof(TesseraContextConfig);
    std::size_t size;
    switch (pick(5)) {
    case 0:
        size = pick(static_cast<uint32_t>(full) + 70);
        break; // anything, including 0..3 (not even structSize fits)
    case 1:
        size = full;
        break;
    case 2:
        size = full + pick(64);
        break; // a newer caller
    default:
        size = pick(static_cast<uint32_t>(full));
        break; // an older caller
    }
    Exact e = exactBuffer(size);
    size = e.size; // exactBuffer enforces room for the leading structSize
    {
        uint32_t claimed = pick(8) == 0 ? interesting() : static_cast<uint32_t>(size);
        // A claim larger than the allocation would be the CALLER's bug; keep the claim honest except for
        // tiny/zero values.
        if (claimed > size)
            claimed = static_cast<uint32_t>(size);
        std::memcpy(e.data.get(), &claimed, sizeof claimed);
    }
    if (size >= sizeof(TesseraContextConfig) && pick(3) != 0) {
        TesseraContextConfig c = tesseraDefaultContextConfig();
        std::memcpy(&c, e.data.get(), sizeof c);
        c.structSize = static_cast<uint32_t>(sizeof c);
        c.api = pick(4) ? TESSERA_API_OPENGL : interesting();
        c.versionMajor = pick(3) ? static_cast<int32_t>(pick(5)) : static_cast<int32_t>(interesting());
        c.versionMinor = pick(3) ? static_cast<int32_t>(pick(7)) : static_cast<int32_t>(interesting());
        c.samples = pick(3) ? static_cast<int32_t>(pick(9)) : static_cast<int32_t>(interesting());
        c.flags = pick(2) ? (rnd() & 0x7ff) : static_cast<uint32_t>(rnd());
        c.shareWith = pick(4) ? TESSERA_NULL_HANDLE : rnd();
        std::memcpy(e.data.get(), &c, sizeof c);
    }
    return e;
}

int iterations() {
    const char* v = std::getenv("TESSERA_FUZZ_ITERATIONS");
    return v ? std::atoi(v) : 20000;
}

// ---------------------------------------------------------------- sections

void fuzzConfigs() {
    std::vector<TesseraContext> live;
    for (int i = 0; i < iterations(); ++i) {
        Exact e = configImage();
        const auto* cfg = reinterpret_cast<const TesseraContextConfig*>(e.data.get());
        TesseraContext ctx = 0xDEADBEEF;
        switch (pick(5)) {
        case 0: {
            const TesseraResult r = tesseraCreateContext(cfg, &ctx);
            if (r == TESSERA_SUCCESS)
                live.push_back(ctx);
            else
                CHECK(ctx == TESSERA_NULL_HANDLE);
            break;
        }
        case 1: {
            const TesseraResult r =
                tesseraCreateContextForWindow(cfg, rnd(), pick(4) ? 0 : static_cast<uint32_t>(rnd()), &ctx);
            if (r == TESSERA_SUCCESS)
                live.push_back(ctx);
            else
                CHECK(ctx == TESSERA_NULL_HANDLE);
            break;
        }
        case 2: {
            // Counts are capped, so a hostile one is rejected rather than allocating; the pool is emptied
            // again below.
            const TesseraResult r =
                tesseraPrewarmContexts(cfg, static_cast<int32_t>(pick(3) ? pick(3) : interesting()));
            (void)r;
            break;
        }
        case 3: {
            // Probe into a report buffer of exactly the claimed size.
            const std::size_t rsz = pick(4)
                                        ? sizeof(TesseraProbeReport)
                                        : 4 + pick(static_cast<uint32_t>(sizeof(TesseraProbeReport)) + 40);
            Exact rep = exactBuffer(rsz);
            uint32_t claim = pick(6) == 0 ? pick(4) : static_cast<uint32_t>(rsz);
            std::memcpy(rep.data.get(), &claim, sizeof claim);
            tesseraProbe(cfg, reinterpret_cast<TesseraProbeReport*>(rep.data.get()));
            tesseraProbe(cfg, nullptr);
            break;
        }
        default: {
            tesseraCreateContext(cfg, nullptr);
            break;
        }
        }
        if (live.size() > 200 || (!live.empty() && pick(10) == 0)) {
            tesseraDestroyContext(live.back());
            live.pop_back();
        }
    }
    for (TesseraContext c : live)
        CHECK(tesseraDestroyContext(c) == TESSERA_SUCCESS);
}

void fuzzHandlesAndOutputs() {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL;
    TesseraContext real = 0;
    CHECK(tesseraCreateContext(&cfg, &real) == TESSERA_SUCCESS);

    for (int i = 0; i < iterations(); ++i) {
        // Real, stale, and made-up handles: nothing may crash, and made-up ones are always rejected.
        const uint64_t h = pick(4) == 0 ? real : (pick(2) ? rnd() : (real ^ (1ull << pick(64))));
        const bool isReal = h == real;

        // Output structs of exactly the claimed size.
        const std::size_t infoSize = pick(4)
                                         ? sizeof(TesseraContextInfo)
                                         : 4 + pick(static_cast<uint32_t>(sizeof(TesseraContextInfo)) + 40);
        Exact info = exactBuffer(infoSize);
        {
            uint32_t claim = pick(6) == 0 ? pick(4) : static_cast<uint32_t>(infoSize);
            std::memcpy(info.data.get(), &claim, sizeof claim);
        }
        const TesseraResult ri =
            tesseraGetContextInfo(h, reinterpret_cast<TesseraContextInfo*>(info.data.get()));
        if (!isReal)
            CHECK(ri != TESSERA_SUCCESS);

        const std::size_t nhSize = pick(4)
                                       ? sizeof(TesseraNativeHandles)
                                       : 4 + pick(static_cast<uint32_t>(sizeof(TesseraNativeHandles)) + 40);
        Exact nh = exactBuffer(nhSize);
        {
            uint32_t claim = pick(6) == 0 ? pick(4) : static_cast<uint32_t>(nhSize);
            std::memcpy(nh.data.get(), &claim, sizeof claim);
        }
        const TesseraResult rn =
            tesseraGetNativeHandles(h, reinterpret_cast<TesseraNativeHandles*>(nh.data.get()));
        if (!isReal)
            CHECK(rn != TESSERA_SUCCESS);

        int32_t status = 7;
        const TesseraResult rs = tesseraGetResetStatus(h, pick(8) ? &status : nullptr);
        if (!isReal)
            CHECK(rs != TESSERA_SUCCESS);

        // Swap control into a buffer of exactly the claimed size, and the lost-state query with optional out
        // pointers.
        {
            const std::size_t scSize = pick(4)
                                           ? sizeof(TesseraSwapControl)
                                           : 4 + pick(static_cast<uint32_t>(sizeof(TesseraSwapControl)) + 40);
            Exact sc = exactBuffer(scSize);
            uint32_t claim = pick(6) == 0 ? pick(4) : static_cast<uint32_t>(scSize);
            std::memcpy(sc.data.get(), &claim, sizeof claim);
            const TesseraResult rsc =
                tesseraGetSwapControl(h, reinterpret_cast<TesseraSwapControl*>(sc.data.get()));
            if (!isReal)
                CHECK(rsc != TESSERA_SUCCESS);
            int32_t lost = 9, lostStatus = 9;
            const TesseraResult rl =
                tesseraIsContextLost(h, pick(8) ? &lost : nullptr, pick(2) ? &lostStatus : nullptr);
            if (!isReal)
                CHECK(rl != TESSERA_SUCCESS);
            else if (rl == TESSERA_SUCCESS)
                CHECK(lost == 0 || lost == 1);
        }
        // The pool with a hostile config image and extreme targets: bounded, never a crash.
        if (pick(8) == 0) {
            Exact c = exactBuffer(sizeof(TesseraContextConfig));
            for (std::size_t b = 0; b < sizeof(TesseraContextConfig); ++b)
                c.data[b] = static_cast<unsigned char>(rnd());
            uint32_t claim = pick(2) ? static_cast<uint32_t>(sizeof(TesseraContextConfig))
                                     : pick(static_cast<uint32_t>(
                                           sizeof(TesseraContextConfig))); // never beyond the allocation
            std::memcpy(c.data.get(), &claim, sizeof claim);
            const auto* cfgImage = reinterpret_cast<const TesseraContextConfig*>(c.data.get());
            int32_t created = 3, count = 3;
            tesseraPoolTopUp(cfgImage, static_cast<int32_t>(interesting()), pick(2) ? &created : nullptr);
            tesseraGetPooledCount(cfgImage, pick(8) ? &count : nullptr);
        }

        if (!isReal) {
            CHECK(tesseraMakeCurrent(h) != TESSERA_SUCCESS);
            CHECK(tesseraSwapBuffers(h) != TESSERA_SUCCESS);
            CHECK(tesseraSetSwapInterval(h, static_cast<int32_t>(interesting())) != TESSERA_SUCCESS);
            TesseraContext out = 5;
            CHECK(tesseraRecreateContext(h, &out, nullptr) != TESSERA_SUCCESS && out == TESSERA_NULL_HANDLE);
            CHECK(tesseraDestroyContext(h) != TESSERA_SUCCESS);
        }
        // The last error message is always a valid, terminated string.
        const char* msg = tesseraGetLastErrorMessage();
        CHECK(msg != nullptr && std::strlen(msg) < 1024);
        CHECK(tesseraResultString(static_cast<TesseraResult>(interesting())) != nullptr);
    }
    CHECK(tesseraDestroyContext(real) == TESSERA_SUCCESS);
    CHECK(tesseraDestroyContext(real) == TESSERA_ERROR_INVALID_HANDLE); // stale after destroy
}

void fuzzProcNames() {
    static const char alphabet[] = "glGLXwEgl_0129 \t\n/\\.:*%\x01\x7f\xff";
    for (int i = 0; i < iterations(); ++i) {
        const std::size_t len = pick(4) ? pick(24) : pick(5000); // includes very long names
        std::string name;
        for (std::size_t k = 0; k < len; ++k)
            name += alphabet[pick(sizeof alphabet - 1)];
        if (pick(3) == 0)
            name = "gl" + name;
        void* p = tesseraGetProcAddress(name.c_str());
        if (!(name.size() >= 2 && name[0] == 'g' && name[1] == 'l'))
            CHECK(p == nullptr);
    }
    CHECK(tesseraGetProcAddress(nullptr) == nullptr);
    CHECK(tesseraGetProcAddress("") == nullptr);
}

void fuzzVersionStrings() {
    static const char* const seeds[] = {"4.6.0 NVIDIA 552.22",
                                        "OpenGL ES 3.2 Mesa",
                                        "OpenGL ES-CM 1.1",
                                        "3.3 (Core Profile) Mesa 23.2",
                                        "",
                                        " ",
                                        "OpenGL ES",
                                        "99999999999999999999.1",
                                        "-1.-1",
                                        "1.",
                                        ".1",
                                        "4.x",
                                        "OpenGL ES 3"};
    static const char alphabet[] = "0123456789. OpenGLESCM-()\xff";
    for (int i = 0; i < iterations() * 2; ++i) {
        std::string s;
        if (pick(2)) {
            s = seeds[pick(sizeof seeds / sizeof *seeds)];
            if (!s.empty() && pick(2))
                s[pick(static_cast<uint32_t>(s.size()))] = static_cast<char>(rnd());
        } else {
            const std::size_t len = pick(40);
            for (std::size_t k = 0; k < len; ++k)
                s += alphabet[pick(sizeof alphabet - 1)];
        }
        bool es = false;
        int major = -7, minor = -7;
        const bool ok = parseGlVersion(s.c_str(), es, major, minor);
        if (ok)
            CHECK(major >= 0 && major <= 9999 && minor >= 0 && minor <= 9999);
    }
    bool es;
    int a, b;
    CHECK(!parseGlVersion(nullptr, es, a, b));
}

void fuzzConcurrent() {
    // Several threads hammering the same API with hostile input at once (for TSan).
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([t] {
            std::mt19937_64 local(0x5eed + static_cast<uint64_t>(t));
            for (int i = 0; i < iterations() / 4; ++i) {
                Exact e{std::make_unique<unsigned char[]>(sizeof(TesseraContextConfig)),
                        sizeof(TesseraContextConfig)}; // (the shared RNG is not thread-safe)
                TesseraContextConfig c = tesseraDefaultContextConfig();
                c.samples = static_cast<int32_t>(local() % 3);
                std::memcpy(e.data.get(), &c, sizeof c);
                TesseraContext ctx = 0;
                if (tesseraCreateContext(reinterpret_cast<TesseraContextConfig*>(e.data.get()), &ctx) ==
                    TESSERA_SUCCESS) {
                    tesseraGetResetStatus(ctx, nullptr);
                    tesseraDestroyContext(ctx);
                    tesseraDestroyContext(ctx); // stale
                }
                tesseraGetProcAddress("glFoo");
                tesseraGetProcAddress(reinterpret_cast<const char*>(&c));
            }
        });
    for (auto& th : threads)
        th.join();
}

} // namespace

int main() {
    const char* s = std::getenv("TESSERA_FUZZ_SEED");
    const uint64_t seed = s ? std::strtoull(s, nullptr, 10) : 20260101ull;
    g_rng.seed(seed);
    std::printf("test_fuzz: seed %llu, %d iterations per section\n", static_cast<unsigned long long>(seed),
                iterations());

    Runtime::disableBuiltinBackendsForTesting();
    CHECK(tesseraInit() == TESSERA_SUCCESS);
    Runtime::instance().registerBackend(std::make_shared<FuzzBackend>());

    std::puts("  fuzzConfigs");
    std::fflush(stdout);
    fuzzConfigs();
    std::puts("  fuzzHandlesAndOutputs");
    std::fflush(stdout);
    fuzzHandlesAndOutputs();
    std::puts("  fuzzProcNames");
    std::fflush(stdout);
    fuzzProcNames();
    std::puts("  fuzzVersionStrings");
    std::fflush(stdout);
    fuzzVersionStrings();
    std::puts("  fuzzConcurrent");
    std::fflush(stdout);
    fuzzConcurrent();

    CHECK(tesseraTerminate() == TESSERA_SUCCESS);
    CHECK(g_live == 0); // every native context created by any of the above was released
    if (g_failures == 0)
        std::puts("test_fuzz: OK");
    else
        std::fprintf(stderr, "test_fuzz: FAILED (seed %llu)\n", static_cast<unsigned long long>(seed));
    return g_failures == 0 ? 0 : 1;
}
