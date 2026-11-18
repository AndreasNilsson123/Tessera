// Robustness checks shared by the real-driver integration tests (WGL, GLX, EGL). Public API only.
//
// Include after defining, in the including file:  `static int g_failures;`  `CHECK(cond)`  `EXPECT(call,
// want)`.
#pragma once

#include <tessera/tessera.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace robust {

struct Hooks {
    TesseraContextConfig base; // a configuration that works on this backend
    std::function<bool()>
        nothingCurrentNatively;          // true if the platform sees no current context on this thread
    std::function<void()> armForeign;    // makes the "other library's" context current again
    std::function<bool()> foreignIntact; // true if the "other library's" state is as the test left it
};

inline void waitFor(const std::atomic<bool>& flag) {
    while (!flag.load())
        std::this_thread::yield();
}

// ---------------------------------------------------------------- deferred destruction

// A context destroyed while another thread holds it current: the handle dies at once, the native teardown
// happens on the owner when it lets go (explicitly, by switching, or by exiting).
inline void deferredDestroy(const Hooks& h) {
    TesseraContextConfig cfg = h.base;
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    if (!ctx)
        return;

    // 1. Held by this thread, destroyed from another one.
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    std::thread([&] { EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS); }).join();
    TesseraContextInfo info = tesseraDefaultContextInfo();
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_ERROR_INVALID_HANDLE);
    EXPECT(tesseraGetContextInfo(ctx, &info), TESSERA_ERROR_INVALID_HANDLE);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_ERROR_INVALID_HANDLE); // and not a second time
    CHECK(tesseraGetCurrentContext() == TESSERA_NULL_HANDLE);         // the handle is dead for the owner too
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE),
           TESSERA_SUCCESS); // finishes the destruction, on this thread
    if (h.nothingCurrentNatively)
        CHECK(h.nothingCurrentNatively());

    // 2. Held by a thread that exits without releasing: its exit finishes the destruction.
    TesseraContext held = 0;
    EXPECT(tesseraCreateContext(&cfg, &held), TESSERA_SUCCESS);
    std::atomic<bool> acquired{false}, destroyed{false};
    std::thread owner([&] {
        EXPECT(tesseraMakeCurrent(held), TESSERA_SUCCESS);
        acquired = true;
        waitFor(destroyed); // leaves without releasing
    });
    waitFor(acquired);
    EXPECT(tesseraDestroyContext(held), TESSERA_SUCCESS);
    destroyed = true;
    owner.join();

    // 3. Held by a thread that then switches to another context: the switch finishes it.
    TesseraContext first = 0, second = 0;
    EXPECT(tesseraCreateContext(&cfg, &first), TESSERA_SUCCESS);
    EXPECT(tesseraCreateContext(&cfg, &second), TESSERA_SUCCESS);
    std::atomic<bool> inFirst{false}, firstDestroyed{false}, switched{false};
    std::thread switcher([&] {
        EXPECT(tesseraMakeCurrent(first), TESSERA_SUCCESS);
        inFirst = true;
        waitFor(firstDestroyed);
        EXPECT(tesseraMakeCurrent(second), TESSERA_SUCCESS);
        switched = true;
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    });
    waitFor(inFirst);
    EXPECT(tesseraDestroyContext(first), TESSERA_SUCCESS);
    firstDestroyed = true;
    switcher.join();
    CHECK(switched.load());
    EXPECT(tesseraDestroyContext(second), TESSERA_SUCCESS);

    // Everything still works afterwards.
    TesseraContext after = 0;
    EXPECT(tesseraCreateContext(&cfg, &after), TESSERA_SUCCESS);
    if (after) {
        EXPECT(tesseraMakeCurrent(after), TESSERA_SUCCESS);
        EXPECT(tesseraDestroyContext(after), TESSERA_SUCCESS);
    }
    if (h.foreignIntact)
        CHECK(h.foreignIntact());
}

// ---------------------------------------------------------------- reset notification

inline void resetStatus(const Hooks& h) {
    TesseraContextConfig cfg = h.base;
    TesseraContext ctx = 0;
    int32_t status = 99;

    // A context without reset notification has nothing to poll.
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    if (ctx) {
        EXPECT(tesseraGetResetStatus(ctx, &status), TESSERA_ERROR_WRONG_THREAD); // not current
        EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
        status = 99;
        EXPECT(tesseraGetResetStatus(ctx, &status), TESSERA_ERROR_UNSUPPORTED);
        CHECK(status == TESSERA_RESET_NONE);
        EXPECT(tesseraGetResetStatus(ctx, nullptr), TESSERA_ERROR_INVALID_ARGUMENT);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
        EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    }
    EXPECT(tesseraGetResetStatus(0xBAD, &status), TESSERA_ERROR_INVALID_HANDLE);

    // A robust context with reset notification, if the driver grants one.
    cfg.flags |= TESSERA_FLAG_ROBUST_ACCESS | TESSERA_FLAG_RESET_NOTIFICATION;
    cfg.fallback = TESSERA_FALLBACK_STRICT;
    ctx = 0;
    const TesseraResult r = tesseraCreateContext(&cfg, &ctx);
    CHECK(r == TESSERA_SUCCESS || r == TESSERA_ERROR_NO_MATCHING_CONFIG);
    if (r != TESSERA_SUCCESS) {
        std::printf("  robust + reset notification not granted here: %s\n", tesseraResultString(r));
        return;
    }
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    for (int i = 0; i < 100; ++i) {
        status = 99;
        EXPECT(tesseraGetResetStatus(ctx, &status), TESSERA_SUCCESS);
        CHECK(status == TESSERA_RESET_NONE); // no GPU reset is happening
    }
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);

    // Recreating (as one would after a loss) gives a working context from the same request.
    TesseraContext again = 0;
    uint32_t flags = 99;
    EXPECT(tesseraRecreateContext(ctx, &again, &flags), TESSERA_SUCCESS);
    CHECK(flags == 0); // it shared with nothing, so there is no group to preserve
    if (again) {
        TesseraContextInfo a = tesseraDefaultContextInfo(), b = tesseraDefaultContextInfo();
        tesseraGetContextInfo(ctx, &a);
        tesseraGetContextInfo(again, &b);
        CHECK((b.flags & TESSERA_FLAG_ROBUST_ACCESS) && (b.flags & TESSERA_FLAG_RESET_NOTIFICATION));
        CHECK(a.versionMajor == b.versionMajor && a.profile == b.profile);
        EXPECT(tesseraMakeCurrent(again), TESSERA_SUCCESS);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
        EXPECT(tesseraDestroyContext(again), TESSERA_SUCCESS);
    }
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    TesseraContext none = 0;
    EXPECT(tesseraRecreateContext(0xBAD, &none, nullptr), TESSERA_ERROR_INVALID_HANDLE);
}

// ---------------------------------------------------------------- debug message forwarding

inline std::mutex& debugLogMutex() {
    static std::mutex m;
    return m;
}
inline std::vector<std::string>& debugLogLines() {
    static std::vector<std::string> v;
    return v;
}
inline void TESSERA_CALL captureLog(int32_t /*level*/, const char* message, void* /*user*/) {
    std::lock_guard<std::mutex> lock(debugLogMutex());
    debugLogLines().emplace_back(message);
}

// A debug context routes the driver's KHR_debug messages into the Tessera log callback.
inline void debugForwarding(const Hooks& h) {
    TesseraContextConfig cfg = h.base;
    cfg.flags |= TESSERA_FLAG_DEBUG;
    TesseraContext ctx = 0;
    const TesseraResult r = tesseraCreateContext(&cfg, &ctx);
    CHECK(r == TESSERA_SUCCESS || r == TESSERA_ERROR_NO_MATCHING_CONFIG);
    if (r != TESSERA_SUCCESS) {
        std::printf("  debug context not granted here: %s\n", tesseraResultString(r));
        return;
    }
    EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
    using BindTexture = void (*)(unsigned, unsigned);
    auto bindTexture = reinterpret_cast<BindTexture>(tesseraGetProcAddress("glBindTexture"));
    const bool hasDebug = tesseraGetProcAddress("glDebugMessageCallback") != nullptr ||
                          tesseraGetProcAddress("glDebugMessageCallbackKHR") != nullptr;
    if (!bindTexture || !hasDebug) {
        std::printf("  (context has no KHR_debug entry points, forwarding not checked)\n");
    } else {
        {
            std::lock_guard<std::mutex> lock(debugLogMutex());
            debugLogLines().clear();
        }
        tesseraSetLogCallback(&captureLog, nullptr);
        bindTexture(0xDEAD, 0); // not a texture target: GL_INVALID_ENUM, reported through KHR_debug
        tesseraSetLogCallback(nullptr, nullptr);
        std::lock_guard<std::mutex> lock(debugLogMutex());
        bool seen = false;
        for (const std::string& line : debugLogLines())
            if (line.find("GL debug [") != std::string::npos) {
                seen = true;
                std::printf("  forwarded: %s\n", line.c_str());
                break;
            }
        CHECK(seen);
    }
    EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
}

// ---------------------------------------------------------------- attribute matrix

struct MatrixResult {
    int combos = 0;
    std::map<std::string, int> byResult;
    std::vector<std::string> problems;
};

// An oracle independent of the library's own strict-mode check: does what came back honour what was asked?
inline bool honours(const TesseraContextConfig& req, const TesseraContextInfo& info, std::string& why) {
    const bool strict = req.fallback == TESSERA_FALLBACK_STRICT;
    auto bad = [&why](const char* what) {
        why = what;
        return false;
    };
    // Never negotiable, in either mode.
    if (info.api != req.api)
        return bad("api");
    if (req.versionMajor != 0 &&
        (info.versionMajor < req.versionMajor ||
         (info.versionMajor == req.versionMajor && info.versionMinor < req.versionMinor)))
        return bad("version below the requested minimum");
    if (req.profile != TESSERA_PROFILE_ANY && info.profile != req.profile)
        return bad("profile");
    if (req.surface == TESSERA_SURFACE_NONE && info.surface != TESSERA_SURFACE_NONE)
        return bad("surface none");
    if (req.surface == TESSERA_SURFACE_PBUFFER && info.surface != TESSERA_SURFACE_PBUFFER)
        return bad("surface pbuffer");
    if (!strict)
        return true; // best effort may relax the rest, but only by reporting it truthfully (checked below)

    constexpr uint32_t flagsToCheck = TESSERA_FLAG_DEBUG | TESSERA_FLAG_FORWARD_COMPATIBLE |
                                      TESSERA_FLAG_ROBUST_ACCESS | TESSERA_FLAG_RESET_NOTIFICATION |
                                      TESSERA_FLAG_NO_ERROR | TESSERA_FLAG_SRGB | TESSERA_FLAG_FLOAT_COLOR |
                                      TESSERA_FLAG_DOUBLE_BUFFER | TESSERA_FLAG_STEREO |
                                      TESSERA_FLAG_RELEASE_FLUSH;
    if (req.flags & ~info.flags & flagsToCheck)
        return bad("a requested flag is not reported as granted");
    if (info.samples < req.samples)
        return bad("samples");
    if (info.depthBits < req.depthBits || info.stencilBits < req.stencilBits)
        return bad("depth/stencil");
    if (info.redBits < req.redBits || info.alphaBits < req.alphaBits)
        return bad("colour bits");
    return true;
}

inline std::string describe(const TesseraContextConfig& c) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "api=%u profile=%u v=%d.%d flags=0x%x samples=%d d=%d s=%d %s surface=%u",
                  c.api, c.profile, c.versionMajor, c.versionMinor, c.flags, c.samples, c.depthBits,
                  c.stencilBits, c.fallback == TESSERA_FALLBACK_STRICT ? "strict" : "best-effort", c.surface);
    return buf;
}

inline MatrixResult attributeMatrix(const Hooks& h) {
    struct Version {
        TesseraEnum api, profile;
        int major, minor;
    };
    const Version versions[] = {
        {TESSERA_API_OPENGL, TESSERA_PROFILE_ANY, 0, 0},
        {TESSERA_API_OPENGL, TESSERA_PROFILE_ANY, 2, 1},
        {TESSERA_API_OPENGL, TESSERA_PROFILE_ANY, 3, 3},
        {TESSERA_API_OPENGL, TESSERA_PROFILE_CORE, 0, 0},
        {TESSERA_API_OPENGL, TESSERA_PROFILE_CORE, 3, 2},
        {TESSERA_API_OPENGL, TESSERA_PROFILE_CORE, 3, 3},
        {TESSERA_API_OPENGL, TESSERA_PROFILE_CORE, 4, 1},
        {TESSERA_API_OPENGL, TESSERA_PROFILE_CORE, 4, 5},
        {TESSERA_API_OPENGL, TESSERA_PROFILE_CORE, 4, 6},
        {TESSERA_API_OPENGL, TESSERA_PROFILE_COMPATIBILITY, 3, 2},
        {TESSERA_API_OPENGL, TESSERA_PROFILE_COMPATIBILITY, 4, 5},
        {TESSERA_API_OPENGL_ES, TESSERA_PROFILE_ANY, 0, 0},
        {TESSERA_API_OPENGL_ES, TESSERA_PROFILE_ANY, 2, 0},
        {TESSERA_API_OPENGL_ES, TESSERA_PROFILE_ANY, 3, 0},
        {TESSERA_API_OPENGL_ES, TESSERA_PROFILE_ANY, 3, 2},
        {TESSERA_API_OPENGL, TESSERA_PROFILE_CORE, 9, 9},
    };
    const uint32_t flagSets[] = {0,
                                 TESSERA_FLAG_DEBUG,
                                 TESSERA_FLAG_FORWARD_COMPATIBLE,
                                 TESSERA_FLAG_DEBUG | TESSERA_FLAG_FORWARD_COMPATIBLE,
                                 TESSERA_FLAG_ROBUST_ACCESS,
                                 TESSERA_FLAG_ROBUST_ACCESS | TESSERA_FLAG_RESET_NOTIFICATION,
                                 TESSERA_FLAG_NO_ERROR,
                                 TESSERA_FLAG_SRGB,
                                 TESSERA_FLAG_FLOAT_COLOR,
                                 TESSERA_FLAG_DOUBLE_BUFFER,
                                 TESSERA_FLAG_RELEASE_FLUSH,
                                 TESSERA_FLAG_STEREO};
    const int samples[] = {0, 4, 64};
    const int depth[][2] = {{0, 0}, {24, 8}};
    const TesseraEnum surfaces[] = {TESSERA_SURFACE_DEFAULT, TESSERA_SURFACE_NONE, TESSERA_SURFACE_PBUFFER};

    MatrixResult result;
    int i = 0;
    for (const Version& v : versions) {
        for (uint32_t flags : flagSets) {
            TesseraContextConfig cfg = h.base;
            cfg.api = v.api;
            cfg.profile = v.profile;
            cfg.versionMajor = v.major;
            cfg.versionMinor = v.minor;
            cfg.flags = flags;
            // Rotate the remaining dimensions so the product stays small but every value meets every other.
            cfg.samples = samples[i % 3];
            cfg.depthBits = depth[(i / 3) % 2][0];
            cfg.stencilBits = depth[(i / 3) % 2][1];
            cfg.fallback = (i / 6) % 2 ? TESSERA_FALLBACK_BEST_EFFORT : TESSERA_FALLBACK_STRICT;
            cfg.surface = surfaces[(i / 12) % 3];
            ++i;
            ++result.combos;

            if (h.armForeign)
                h.armForeign(); // a Tessera make-current legitimately displaces it, so re-arm each time
            TesseraContext ctx = 0;
            const TesseraResult r = tesseraCreateContext(&cfg, &ctx);
            ++result.byResult[tesseraResultString(r)];
            const std::string label = describe(cfg);
            // Creating (successfully or not) must leave the other library's context exactly as it was.
            if (h.foreignIntact && !h.foreignIntact())
                result.problems.push_back(label + " -> disturbed the other library's state");

            if (r == TESSERA_SUCCESS) {
                TesseraContextInfo info = tesseraDefaultContextInfo();
                if (tesseraGetContextInfo(ctx, &info) != TESSERA_SUCCESS) {
                    result.problems.push_back(label + " -> created, but tesseraGetContextInfo failed");
                } else {
                    std::string why;
                    if (!honours(cfg, info, why))
                        result.problems.push_back(label +
                                                  " -> returned a context that breaks the request: " + why);
                }
                // A context that was created must be usable.
                if (tesseraMakeCurrent(ctx) != TESSERA_SUCCESS)
                    result.problems.push_back(label + " -> created but cannot be made current");
                if (tesseraSwapBuffers(ctx) != TESSERA_SUCCESS)
                    result.problems.push_back(label + " -> created but swap failed");
                if (tesseraMakeCurrent(TESSERA_NULL_HANDLE) != TESSERA_SUCCESS)
                    result.problems.push_back(label + " -> release failed");
                if (tesseraDestroyContext(ctx) != TESSERA_SUCCESS)
                    result.problems.push_back(label + " -> destroy failed");
            } else if (r != TESSERA_ERROR_NO_MATCHING_CONFIG && r != TESSERA_ERROR_UNSUPPORTED &&
                       r != TESSERA_ERROR_INVALID_ARGUMENT) {
                // A valid request may be refused, but never with a code that means "something broke".
                result.problems.push_back(label + " -> " + tesseraResultString(r) + " (" +
                                          tesseraGetLastErrorMessage() + ")");
            }
            if (ctx != 0 && r != TESSERA_SUCCESS)
                result.problems.push_back(label + " -> failed but returned a handle");
        }
    }
    return result;
}

inline void reportMatrix(const Hooks& h) {
    const MatrixResult m = attributeMatrix(h);
    std::printf("  %d combinations:", m.combos);
    for (const auto& entry : m.byResult)
        std::printf("  %s=%d", entry.first.c_str(), entry.second);
    std::printf("\n");
    for (const std::string& p : m.problems) {
        std::fprintf(stderr, "  MATRIX PROBLEM: %s\n", p.c_str());
        ++g_failures;
    }
}

} // namespace robust
