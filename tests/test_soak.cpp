// Create / use / switch / destroy loops against the real driver, with leak tracking. Public API only.
//
//   TESSERA_SOAK_CYCLES   cycles per thread (default 1500; the Phase 2 exit criterion is 10000)
//   TESSERA_SOAK_SECONDS  if set, run for this long instead (e.g. 86400 for the 24 h soak) and ignore the
//   cycle count TESSERA_SOAK_THREADS  worker threads (default 3)
//
// Tracked per process: Windows = kernel handles, GDI objects, USER objects, private bytes; Linux = open file
// descriptors, resident set size. The first 10% of the run is warm-up (drivers allocate lazily); after it,
// growth beyond a small tolerance fails the test. Exits 77 where no context can be created.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1 // getenv: the test reads its own knobs
#endif
#include <tessera/tessera.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
// clang-format off
#include <windows.h> // must precede psapi.h: sorting the includes alphabetically breaks the build
#include <psapi.h>
// clang-format on
#else
#include <dirent.h>
#include <fstream>
#include <string>
#include <unistd.h>
#endif

namespace {

struct Usage {
    long handles = 0; // kernel handles / open fds
    long gdi = 0;     // Windows only
    long user = 0;    // Windows only
    long memoryKb = 0;
};

Usage measure() {
    Usage u;
#if defined(_WIN32)
    DWORD h = 0;
    GetProcessHandleCount(GetCurrentProcess(), &h);
    u.handles = static_cast<long>(h);
    u.gdi = static_cast<long>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS));
    u.user = static_cast<long>(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS));
    PROCESS_MEMORY_COUNTERS_EX pm = {};
    pm.cb = sizeof pm;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm))
        u.memoryKb = static_cast<long>(pm.PrivateUsage / 1024);
#else
    if (DIR* d = opendir("/proc/self/fd")) {
        while (readdir(d))
            ++u.handles;
        closedir(d);
        u.handles -= 3; // ., .., and the descriptor opendir itself holds
    }
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line))
        if (line.rfind("VmRSS:", 0) == 0)
            u.memoryKb = std::atol(line.c_str() + 6);
#endif
    return u;
}

std::atomic<long> g_failures{0};
std::atomic<long> g_cycles{0};

#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s (%s)\n", __FILE__, __LINE__, #cond,                \
                         tesseraGetLastErrorMessage());                                                      \
            ++g_failures;                                                                                    \
        }                                                                                                    \
    } while (0)

using GlClearColor = void (*)(float, float, float, float);
using GlClear = void (*)(unsigned);
using GlFinish = void (*)();

// One cycle: three contexts (one pbuffer, two sharing), each made current and drawn with, then destroyed in a
// rotating order.
void cycle(long n) {
    TesseraContextConfig base = tesseraDefaultContextConfig();
    base.api = TESSERA_API_OPENGL;
    base.flags = TESSERA_FLAG_DOUBLE_BUFFER;
    base.fallback = TESSERA_FALLBACK_BEST_EFFORT;

    TesseraContext a = 0, b = 0, c = 0;
    CHECK(tesseraCreateContext(&base, &a) == TESSERA_SUCCESS);
    if (!a)
        return;
    TesseraContextConfig share = base;
    share.shareWith = a;
    CHECK(tesseraCreateContext(&share, &b) == TESSERA_SUCCESS);
    TesseraContextConfig pb = base;
    pb.surface = TESSERA_SURFACE_PBUFFER;
    pb.flags = 0;
    if (tesseraCreateContext(&pb, &c) != TESSERA_SUCCESS)
        c = 0; // pbuffers are optional

    TesseraContext all[3] = {a, b, c};
    for (TesseraContext ctx : all) {
        if (!ctx)
            continue;
        CHECK(tesseraMakeCurrent(ctx) == TESSERA_SUCCESS); // switching from the previous one releases it
        auto clearColor = reinterpret_cast<GlClearColor>(tesseraGetProcAddress("glClearColor"));
        auto clear = reinterpret_cast<GlClear>(tesseraGetProcAddress("glClear"));
        auto finish = reinterpret_cast<GlFinish>(tesseraGetProcAddress("glFinish"));
        if (clearColor && clear && finish) {
            clearColor(0.1f, 0.2f, 0.3f, 1.f);
            clear(0x4000);
            finish();
        }
        CHECK(tesseraSwapBuffers(ctx) == TESSERA_SUCCESS);
    }
    // Destroy while one of them is still current (the harder case), in a rotating order.
    for (int i = 0; i < 3; ++i) {
        TesseraContext ctx = all[(static_cast<long>(i) + n) % 3];
        if (ctx)
            CHECK(tesseraDestroyContext(ctx) == TESSERA_SUCCESS);
    }
    CHECK(tesseraMakeCurrent(TESSERA_NULL_HANDLE) == TESSERA_SUCCESS);
    ++g_cycles;
}

} // namespace

int main() {
    const char* cyclesEnv = std::getenv("TESSERA_SOAK_CYCLES");
    const char* secondsEnv = std::getenv("TESSERA_SOAK_SECONDS");
    const char* threadsEnv = std::getenv("TESSERA_SOAK_THREADS");
    const long cyclesPerThread = cyclesEnv ? std::atol(cyclesEnv) : 1500;
    const long seconds = secondsEnv ? std::atol(secondsEnv) : 0;
    const int threads = threadsEnv ? std::atoi(threadsEnv) : 3;

    if (tesseraInit() != TESSERA_SUCCESS)
        return 1;
    {
        TesseraContextConfig cfg = tesseraDefaultContextConfig();
        cfg.api = TESSERA_API_OPENGL;
        TesseraContext probe = 0;
        if (tesseraCreateContext(&cfg, &probe) != TESSERA_SUCCESS) {
            std::fprintf(stderr, "test_soak: no OpenGL context here (%s), skipping\n",
                         tesseraGetLastErrorMessage());
            return 77;
        }
        tesseraDestroyContext(probe);
    }

    // Measurements are only meaningful while no context is alive, so the run is cut into phases; every phase
    // joins its workers and then samples. Phase 0 is the warm-up (drivers allocate lazily) and sets the
    // baseline.
    const int phases = 10;
    const auto start = std::chrono::steady_clock::now();
    const auto runPhase = [&](long cycles, double phaseSeconds) {
        const auto phaseStart = std::chrono::steady_clock::now();
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; ++t)
            workers.emplace_back([&, t] {
                for (long n = 0;; ++n) {
                    if (phaseSeconds > 0) {
                        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - phaseStart)
                                .count() >= phaseSeconds)
                            break;
                    } else if (n >= cycles) {
                        break;
                    }
                    cycle(n + t);
                }
            });
        for (auto& w : workers)
            w.join();
    };

    Usage baseline{}, peak{};
    for (int phase = 0; phase <= phases; ++phase) {
        if (seconds)
            runPhase(0, phase == 0 ? static_cast<double>(seconds) / 10.0
                                   : static_cast<double>(seconds) * 0.9 / phases);
        else
            runPhase(phase == 0 ? std::max(1L, cyclesPerThread / 10)
                                : std::max(1L, cyclesPerThread * 9 / 10 / phases),
                     0);
        const Usage now = measure();
        if (phase == 0)
            baseline = now;
        peak.handles = std::max(peak.handles, now.handles);
        peak.gdi = std::max(peak.gdi, now.gdi);
        peak.user = std::max(peak.user, now.user);
        peak.memoryKb = std::max(peak.memoryKb, now.memoryKb);
        std::printf("%s  %ld cycles, %.0f s: handles %ld, gdi %ld, user %ld, memory %ld KB\n",
                    phase == 0 ? "warm-up" : "       ", g_cycles.load(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                    now.handles, now.gdi, now.user, now.memoryKb);
        std::fflush(stdout);
    }

    const Usage end = measure();
    std::printf("finished: %ld cycles x 3 contexts (%d threads)\n", g_cycles.load(), threads);
    std::printf("growth since the end of warm-up: handles %+ld, gdi %+ld, user %+ld, memory %+ld KB\n",
                end.handles - baseline.handles, end.gdi - baseline.gdi, end.user - baseline.user,
                end.memoryKb - baseline.memoryKb);
    // Tolerances: a few handles/objects may legitimately differ (driver worker threads, lazily created
    // helpers); a leak grows with the cycle count, so any real one blows far past these over thousands of
    // cycles.
    CHECK(end.handles - baseline.handles <= 16);
    CHECK(end.gdi - baseline.gdi <= 8);
    CHECK(end.user - baseline.user <= 8);
    CHECK(end.memoryKb - baseline.memoryKb <= 64 * 1024);
    CHECK(tesseraTerminate() == TESSERA_SUCCESS);
    if (g_failures == 0)
        std::puts("test_soak: OK");
    return g_failures == 0 ? 0 : 1;
}
