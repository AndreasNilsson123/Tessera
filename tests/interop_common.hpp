// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
// Checks for the Phase 8 additions that are independent of the platform: pre-warmed contexts and native
// handles. It needs `g_failures`, `CHECK` and `EXPECT` from the including file, defined before this header
// is included.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include "robustness_common.hpp"

namespace interop {

using Clock = std::chrono::steady_clock;

inline double millisSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------- pre-warmed contexts

inline void prewarm(const robust::Hooks& h) {
    TesseraContextConfig cfg = h.base;
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT; // so the same test is valid on every backend
    CHECK(tesseraPooledContextCount() == 0);

    // What a cold create costs here.
    TesseraContext cold = 0;
    auto t0 = Clock::now();
    EXPECT(tesseraCreateContext(&cfg, &cold), TESSERA_SUCCESS);
    const double coldMs = millisSince(t0);
    TesseraContextInfo coldInfo = tesseraDefaultContextInfo();
    if (cold)
        tesseraGetContextInfo(cold, &coldInfo);
    if (cold)
        EXPECT(tesseraDestroyContext(cold), TESSERA_SUCCESS);

    // Pre-warm three; the next three creates take them without touching the driver.
    EXPECT(tesseraPrewarmContexts(&cfg, 3), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() == 3);
    std::vector<TesseraContext> warm;
    double warmMs = 0;
    for (int i = 0; i < 3; ++i) {
        TesseraContext c = 0;
        t0 = Clock::now();
        EXPECT(tesseraCreateContext(&cfg, &c), TESSERA_SUCCESS);
        warmMs += millisSince(t0);
        warm.push_back(c);
        CHECK(tesseraPooledContextCount() == 2 - i);
        // Indistinguishable from a freshly created one.
        TesseraContextInfo info = tesseraDefaultContextInfo();
        EXPECT(tesseraGetContextInfo(c, &info), TESSERA_SUCCESS);
        CHECK(info.versionMajor == coldInfo.versionMajor && info.versionMinor == coldInfo.versionMinor);
        CHECK(info.profile == coldInfo.profile && info.flags == coldInfo.flags &&
              info.depthBits == coldInfo.depthBits);
        CHECK(std::strcmp(info.renderer, coldInfo.renderer) == 0);
        EXPECT(tesseraMakeCurrent(c), TESSERA_SUCCESS);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    }
    warmMs /= 3;
    std::printf("  create: cold %.2f ms, pre-warmed %.3f ms on average\n", coldMs, warmMs);
    CHECK(coldMs < 10.0 || warmMs < coldMs / 2); // only meaningful where creation is not already near-free

    // Pool empty: the next create is a normal one and still works.
    TesseraContext fourth = 0;
    EXPECT(tesseraCreateContext(&cfg, &fourth), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() == 0);
    if (fourth)
        EXPECT(tesseraDestroyContext(fourth), TESSERA_SUCCESS);

    // A different request does not take a pooled context; neither does one that wants to share.
    TesseraContextConfig other = cfg;
    other.depthBits = 16;
    EXPECT(tesseraPrewarmContexts(&cfg, 1), TESSERA_SUCCESS);
    TesseraContext o = 0;
    EXPECT(tesseraCreateContext(&other, &o), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() == 1);
    TesseraContextConfig sharing = cfg;
    sharing.shareWith = warm[0];
    TesseraContext s = 0;
    EXPECT(tesseraCreateContext(&sharing, &s), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() == 1); // sharing needs a parent it can be created with: never pooled
    TesseraContext match = 0;
    EXPECT(tesseraCreateContext(&cfg, &match), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() == 0);
    for (TesseraContext c : {o, s, match}) {
        if (c)
            EXPECT(tesseraDestroyContext(c), TESSERA_SUCCESS);
    }
    for (TesseraContext c : warm)
        EXPECT(tesseraDestroyContext(c), TESSERA_SUCCESS);

    // Misuse.
    TesseraContextConfig bad = cfg;
    bad.shareWith = 1234;
    EXPECT(tesseraPrewarmContexts(&bad, 1), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraPrewarmContexts(&cfg, -1), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraPrewarmContexts(&cfg, 65), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraPrewarmContexts(nullptr, 1), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraPrewarmContexts(&cfg, 0), TESSERA_SUCCESS); // nothing to do
    TesseraContextConfig invalid = cfg;
    invalid.versionMajor = 9;
    invalid.versionMinor = 9;
    invalid.fallback = TESSERA_FALLBACK_STRICT;
    EXPECT(tesseraPrewarmContexts(&invalid, 2),
           TESSERA_ERROR_NO_MATCHING_CONFIG); // an impossible request pools nothing
    CHECK(tesseraPooledContextCount() == 0);

    // Concurrent takers get one each, never the same context twice.
    EXPECT(tesseraPrewarmContexts(&cfg, 8), TESSERA_SUCCESS);
    std::atomic<int> failures{0};
    std::vector<TesseraContext> taken(8, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            if (tesseraCreateContext(&cfg, &taken[static_cast<std::size_t>(t)]) != TESSERA_SUCCESS)
                ++failures;
        });
    for (auto& t : threads)
        t.join();
    CHECK(failures == 0);
    CHECK(tesseraPooledContextCount() == 0);
    for (TesseraContext c : taken)
        if (c)
            EXPECT(tesseraDestroyContext(c), TESSERA_SUCCESS);

    // tesseraTerminate empties the pool (and the library works again afterwards).
    EXPECT(tesseraPrewarmContexts(&cfg, 2), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() == 2);
    EXPECT(tesseraTerminate(), TESSERA_SUCCESS);
    CHECK(tesseraPooledContextCount() == 0);
    EXPECT(tesseraInit(), TESSERA_SUCCESS);
    TesseraContext after = 0;
    EXPECT(tesseraCreateContext(&cfg, &after), TESSERA_SUCCESS);
    if (after)
        EXPECT(tesseraDestroyContext(after), TESSERA_SUCCESS);
}

// ---------------------------------------------------------------- native handles

struct HandleExpectations {
    uint32_t kind = 0;
    int contextIndex = 0;           // which handles[] entry is the native context object
    std::vector<int> mustBeNonZero; // entries that must be set for a normal (hidden-window) context
    std::function<uint64_t()>
        nativeCurrent; // the platform's own idea of the current context on this thread (may be empty)
};

inline void nativeHandles(const robust::Hooks& h, const HandleExpectations& e) {
    TesseraContextConfig cfg = h.base;
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    if (!ctx)
        return;

    TesseraNativeHandles nh = tesseraDefaultNativeHandles();
    EXPECT(tesseraGetNativeHandles(ctx, &nh), TESSERA_SUCCESS);
    CHECK(nh.kind == e.kind);
    for (int i : e.mustBeNonZero) {
        if (nh.handles[i] == 0) {
            std::fprintf(stderr, "native handle [%d] is 0\n", i);
            ++g_failures;
        }
    }
    // They are the real objects: the platform reports the same context as current once we make it so.
    if (e.nativeCurrent) {
        EXPECT(tesseraMakeCurrent(ctx), TESSERA_SUCCESS);
        CHECK(e.nativeCurrent() == nh.handles[e.contextIndex]);
        EXPECT(tesseraMakeCurrent(TESSERA_NULL_HANDLE), TESSERA_SUCCESS);
    }

    // Distinct contexts have distinct native objects; the handles are stable for the context's lifetime.
    TesseraContext second = 0;
    EXPECT(tesseraCreateContext(&cfg, &second), TESSERA_SUCCESS);
    TesseraNativeHandles nh2 = tesseraDefaultNativeHandles(), again = tesseraDefaultNativeHandles();
    if (second) {
        EXPECT(tesseraGetNativeHandles(second, &nh2), TESSERA_SUCCESS);
        CHECK(nh2.handles[e.contextIndex] != nh.handles[e.contextIndex]);
        EXPECT(tesseraDestroyContext(second), TESSERA_SUCCESS);
    }
    EXPECT(tesseraGetNativeHandles(ctx, &again), TESSERA_SUCCESS);
    CHECK(std::memcmp(nh.handles, again.handles, sizeof nh.handles) == 0);

    // Argument handling and the struct-size protocol: a caller with a smaller struct gets only what fits.
    EXPECT(tesseraGetNativeHandles(ctx, nullptr), TESSERA_ERROR_INVALID_ARGUMENT);
    TesseraNativeHandles unset;
    std::memset(&unset, 0, sizeof unset);
    EXPECT(tesseraGetNativeHandles(ctx, &unset), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraGetNativeHandles(0xBAD, &nh), TESSERA_ERROR_INVALID_HANDLE);
    unsigned char raw[sizeof(TesseraNativeHandles) + 16];
    std::memset(raw, 0xCD, sizeof raw);
    auto* narrow = reinterpret_cast<TesseraNativeHandles*>(raw);
    narrow->structSize = 8; // structSize + kind only
    EXPECT(tesseraGetNativeHandles(ctx, narrow), TESSERA_SUCCESS);
    CHECK(narrow->structSize == 8 && narrow->kind == e.kind);
    CHECK(raw[8] == 0xCD && raw[9] == 0xCD); // nothing written beyond what the caller's struct holds

    EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
    EXPECT(tesseraGetNativeHandles(ctx, &nh), TESSERA_ERROR_INVALID_HANDLE); // gone with the context
}

// ---------------------------------------------------------------- probing

inline void probe(const robust::Hooks& h) {
    TesseraContextConfig cfg = h.base;
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;

    // The probe reports exactly what a real context of the same request has.
    TesseraContext real = 0;
    EXPECT(tesseraCreateContext(&cfg, &real), TESSERA_SUCCESS);
    TesseraContextInfo want = tesseraDefaultContextInfo();
    if (real)
        tesseraGetContextInfo(real, &want);

    if (h.armForeign)
        h.armForeign();
    TesseraProbeReport rep = tesseraDefaultProbeReport();
    EXPECT(tesseraProbe(&cfg, &rep), TESSERA_SUCCESS);
    if (h.foreignIntact)
        CHECK(h.foreignIntact()); // the caller's context is undisturbed
    CHECK(rep.result == TESSERA_SUCCESS && rep.message[0] == '\0');
    CHECK(rep.backend[0] != '\0');
    CHECK(rep.info.versionMajor == want.versionMajor && rep.info.versionMinor == want.versionMinor);
    CHECK(rep.info.profile == want.profile && rep.info.flags == want.flags &&
          rep.info.depthBits == want.depthBits);
    CHECK(std::strcmp(rep.info.renderer, want.renderer) == 0 &&
          std::strcmp(rep.info.vendor, want.vendor) == 0);
    std::printf("  probe: %s via %s\n", rep.info.renderer, rep.backend);
    CHECK(tesseraGetCurrentContext() == TESSERA_NULL_HANDLE); // nothing of ours was left current

    // A request that cannot be met: same code as a real create, with a reason, and no side effects.
    TesseraContextConfig impossible = cfg;
    impossible.fallback = TESSERA_FALLBACK_STRICT;
    impossible.versionMajor = 9;
    impossible.versionMinor = 9;
    TesseraContext none = 0;
    const TesseraResult expected = tesseraCreateContext(&impossible, &none);
    CHECK(expected != TESSERA_SUCCESS);
    TesseraProbeReport bad = tesseraDefaultProbeReport();
    const TesseraResult got = tesseraProbe(&impossible, &bad);
    CHECK(got == expected && bad.result == expected);
    CHECK(bad.message[0] != '\0' && bad.info.versionMajor == 0);
    if (h.foreignIntact)
        CHECK(h.foreignIntact());

    // Probing repeatedly does not accumulate anything.
    for (int i = 0; i < 20; ++i) {
        TesseraProbeReport r = tesseraDefaultProbeReport();
        EXPECT(tesseraProbe(&cfg, &r), TESSERA_SUCCESS);
    }
    if (real)
        EXPECT(tesseraDestroyContext(real), TESSERA_SUCCESS);
}

// ---------------------------------------------------------------- GPU requests on backends that cannot
// honour them

// Only WGL can check an adapter request; every other backend must refuse it instead of silently using
// whatever GPU it picks.
inline void adapterRequestsRefused(const robust::Hooks& h) {
    TesseraContextConfig cfg = h.base;
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    cfg.gpuPreference = TESSERA_GPU_HIGH_PERFORMANCE;
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_ERROR_UNSUPPORTED);
    CHECK(ctx == TESSERA_NULL_HANDLE);
    CHECK(std::strstr(tesseraGetLastErrorMessage(), "no backend can honour") != nullptr);
    cfg.gpuPreference = TESSERA_GPU_DEFAULT;
    cfg.adapterLuid = 0x1234;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_ERROR_UNSUPPORTED);
    TesseraProbeReport rep = tesseraDefaultProbeReport();
    EXPECT(tesseraProbe(&cfg, &rep), TESSERA_ERROR_UNSUPPORTED);
    CHECK(rep.result == TESSERA_ERROR_UNSUPPORTED);
    EXPECT(tesseraPrewarmContexts(&cfg, 1), TESSERA_ERROR_UNSUPPORTED);
    CHECK(tesseraPooledContextCount() == 0);
    // The same request without the GPU fields works.
    cfg.adapterLuid = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    if (ctx)
        EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);
}

// ---------------------------------------------------------------- keeping the pool full

inline void topUp(const robust::Hooks& h) {
    TesseraContextConfig cfg = h.base;
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraContextConfig other = cfg;
    other.depthBits = 16;
    CHECK(tesseraPoolClear() >= 0);
    CHECK(tesseraPooledContextCount() == 0);

    int32_t created = -1, count = -1;
    EXPECT(tesseraPoolTopUp(&cfg, 2, &created), TESSERA_SUCCESS);
    CHECK(created == 2);
    EXPECT(tesseraGetPooledCount(&cfg, &count), TESSERA_SUCCESS);
    CHECK(count == 2 && tesseraPooledContextCount() == 2);
    EXPECT(tesseraPoolTopUp(&cfg, 2, &created), TESSERA_SUCCESS); // already there: nothing to create
    CHECK(created == 0 && tesseraPooledContextCount() == 2);
    EXPECT(tesseraPoolTopUp(&cfg, 1, &created), TESSERA_SUCCESS); // never shrinks
    CHECK(created == 0 && tesseraPooledContextCount() == 2);

    // Counts are per request.
    EXPECT(tesseraPoolTopUp(&other, 1, &created), TESSERA_SUCCESS);
    CHECK(created == 1 && tesseraPooledContextCount() == 3);
    EXPECT(tesseraGetPooledCount(&other, &count), TESSERA_SUCCESS);
    CHECK(count == 1);
    EXPECT(tesseraGetPooledCount(&cfg, &count), TESSERA_SUCCESS);
    CHECK(count == 2);

    // The usual pattern: take one, then bring the pool back to its target.
    TesseraContext ctx = 0;
    EXPECT(tesseraCreateContext(&cfg, &ctx), TESSERA_SUCCESS);
    EXPECT(tesseraGetPooledCount(&cfg, &count), TESSERA_SUCCESS);
    CHECK(count == 1);
    EXPECT(tesseraPoolTopUp(&cfg, 2, &created), TESSERA_SUCCESS);
    CHECK(created == 1);
    if (ctx)
        EXPECT(tesseraDestroyContext(ctx), TESSERA_SUCCESS);

    // Limits and misuse.
    CHECK(tesseraPoolTopUp(&cfg, 65, &created) == TESSERA_ERROR_INVALID_ARGUMENT && created == 0);
    EXPECT(tesseraPoolTopUp(&cfg, -1, &created), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraPoolTopUp(nullptr, 1, &created), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraGetPooledCount(&cfg, nullptr), TESSERA_ERROR_INVALID_ARGUMENT);
    TesseraContextConfig sharing = cfg;
    sharing.shareWith = 1;
    EXPECT(tesseraPoolTopUp(&sharing, 1, &created), TESSERA_ERROR_INVALID_ARGUMENT);
    EXPECT(tesseraPoolTopUp(&cfg, 1, nullptr), TESSERA_SUCCESS); // outCreated is optional

    // Concurrent top-ups of the same request: nothing is lost or double-counted, and the cap holds. (Kept to
    // at most 8 live contexts: Mesa's Direct3D12 driver on WSL crashed later, in an unrelated destroy, with
    // 12 contexts alive at once; llvmpipe, WGL and NVIDIA's driver have no such limit. Not reproducible with
    // raw libGL, ASan-clean, so recorded in the roadmap as a driver issue.)
    CHECK(tesseraPoolClear() >= 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&cfg] {
            int32_t made = 0;
            tesseraPoolTopUp(&cfg, 2, &made);
        });
    for (auto& t : threads)
        t.join();
    CHECK(tesseraPooledContextCount() >= 2 && tesseraPooledContextCount() <= 8);

    // Clear destroys everything, and says how much it was.
    const int32_t waiting = tesseraPooledContextCount();
    CHECK(tesseraPoolClear() == waiting && tesseraPooledContextCount() == 0);
    CHECK(tesseraPoolClear() == 0);
}

// ---------------------------------------------------------------- sharing, explained (ABI 0.15.0)

// Share-group introspection, and which differences in the request break object sharing on THIS driver. The
// table is printed (it is what docs/compat.md records) and tesseraCanShare must agree with it: a "no" is a
// failed create; a "yes" is a successful one.
inline void sharingExplained(const robust::Hooks& h) {
    TesseraContextConfig base = h.base;
    base.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraContext parent = 0, sibling = 0, other = 0;
    EXPECT(tesseraCreateContext(&base, &parent), TESSERA_SUCCESS);
    if (!parent)
        return;
    TesseraContextConfig withParent = base;
    withParent.shareWith = parent;
    EXPECT(tesseraCreateContext(&withParent, &sibling), TESSERA_SUCCESS);
    EXPECT(tesseraCreateContext(&base, &other), TESSERA_SUCCESS);

    // Introspection: the group of parent and sibling, and the one of `other`.
    TesseraShareGroupInfo gp = tesseraDefaultShareGroupInfo(), gs = gp, go = gp;
    EXPECT(tesseraGetShareGroup(parent, &gp), TESSERA_SUCCESS);
    EXPECT(tesseraGetShareGroup(sibling, &gs), TESSERA_SUCCESS);
    EXPECT(tesseraGetShareGroup(other, &go), TESSERA_SUCCESS);
    CHECK(gp.groupId == gs.groupId && gp.groupId != go.groupId);
    CHECK(gp.members == 2 && gp.alive == 2 && go.members == 1);

    struct Variant {
        const char* what;
        void (*tweak)(TesseraContextConfig&);
    };
    static const Variant variants[] = {
        {"same request", [](TesseraContextConfig&) {}},
        {"depth 16 instead of the default", [](TesseraContextConfig& c) { c.depthBits = 16; }},
        {"stencil 8", [](TesseraContextConfig& c) { c.stencilBits = 8; }},
        {"4x MSAA", [](TesseraContextConfig& c) { c.samples = 4; }},
        {"sRGB framebuffer", [](TesseraContextConfig& c) { c.flags |= TESSERA_FLAG_SRGB; }},
        {"float colour buffer", [](TesseraContextConfig& c) { c.flags |= TESSERA_FLAG_FLOAT_COLOR; }},
        {"alpha 8", [](TesseraContextConfig& c) { c.alphaBits = 8; }},
        {"debug context", [](TesseraContextConfig& c) { c.flags |= TESSERA_FLAG_DEBUG; }},
        {"depth 16 + core 3.3 (ARB creation)",
         [](TesseraContextConfig& c) {
             c.depthBits = 16;
             c.profile = TESSERA_PROFILE_CORE;
             c.versionMajor = 3;
             c.versionMinor = 3;
         }},
        {"depth 16 + debug (ARB creation)",
         [](TesseraContextConfig& c) {
             c.depthBits = 16;
             c.flags |= TESSERA_FLAG_DEBUG;
         }},
        {"depth 32", [](TesseraContextConfig& c) { c.depthBits = 32; }},
        {"no depth, no stencil",
         [](TesseraContextConfig& c) {
             c.depthBits = 0;
             c.stencilBits = 0;
             c.flags |= TESSERA_FLAG_DEBUG;
         }},
        {"core 3.3 profile",
         [](TesseraContextConfig& c) {
             c.profile = TESSERA_PROFILE_CORE;
             c.versionMajor = 3;
             c.versionMinor = 3;
         }},
        {"OpenGL ES", [](TesseraContextConfig& c) { c.api = TESSERA_API_OPENGL_ES; }},
    };
    std::printf(
        "  what changes between a parent and a context that shares with it (backend verdict | create):\n");
    for (const Variant& v : variants) {
        TesseraContextConfig want = base;
        v.tweak(want);
        int32_t verdict = -1;
        const TesseraResult asked = tesseraCanShare(&want, parent, &verdict);
        CHECK(asked == TESSERA_SUCCESS);
        // Can the request be made at all here (a float framebuffer, say, is not on every driver)? Then
        // sharing is not the question.
        TesseraContext alone = 0;
        const TesseraResult standalone = tesseraCreateContext(&want, &alone);
        if (alone)
            EXPECT(tesseraDestroyContext(alone), TESSERA_SUCCESS);
        if (standalone != TESSERA_SUCCESS) {
            std::printf("    %-34s verdict %d | (cannot be created here at all: %s)\n", v.what, verdict,
                        tesseraResultString(standalone));
            continue;
        }
        TesseraContextConfig share = want;
        share.shareWith = parent;
        TesseraContext made = 0;
        const TesseraResult r = tesseraCreateContext(&share, &made);
        std::printf("    %-34s verdict %d | %s%s%s\n", v.what, verdict, tesseraResultString(r),
                    r == TESSERA_SUCCESS ? "" : ": ",
                    r == TESSERA_SUCCESS ? "" : tesseraGetLastErrorMessage());
        // Agreement: the library says "no" only for things that really fail, and "yes" only where the create
        // worked, EXCEPT where the request itself cannot be made here at all (then both are "no" or the
        // verdict is a statement about sharing alone).
        if (verdict != TESSERA_SHARE_OK)
            CHECK(r != TESSERA_SUCCESS);
        if (verdict == TESSERA_SHARE_OK && r != TESSERA_SUCCESS)
            CHECK(r ==
                  TESSERA_ERROR_NO_MATCHING_CONFIG); // a driver refusing two framebuffer formats (the hosted
                                                     // GDI renderer): explained, not a PLATFORM error
        if (r == TESSERA_SUCCESS) {
            CHECK(verdict == TESSERA_SHARE_OK);
            TesseraShareGroupInfo g = tesseraDefaultShareGroupInfo();
            EXPECT(tesseraGetShareGroup(made, &g), TESSERA_SUCCESS);
            CHECK(g.groupId == gp.groupId && g.members == 3);
            EXPECT(tesseraDestroyContext(made), TESSERA_SUCCESS);
        }
    }
    // A dead parent is an error, not a verdict.
    int32_t why = -1;
    EXPECT(tesseraCanShare(&base, TESSERA_NULL_HANDLE, &why), TESSERA_ERROR_INVALID_HANDLE);

    for (TesseraContext c : {parent, sibling, other})
        EXPECT(tesseraDestroyContext(c), TESSERA_SUCCESS);
}

} // namespace interop
