// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors

// Version negotiation against the real driver (ROADMAP 12.2, ADR 0016), public API only: versionMajor/Minor
// is a minimum, so a request for "core 3.3" gets the newest core context the driver has;
// TESSERA_FLAG_EXACT_VERSION gets exactly 3.3; OpenGL ES with no version gets the newest ES. Exits 77
// (skipped) where no context can be made at all.
#include <tessera/tessera.h>

#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                    \
            ++g_failures;                                                                                    \
        }                                                                                                    \
    } while (0)

struct Got {
    TesseraResult result = TESSERA_ERROR_UNKNOWN;
    int major = 0, minor = 0;
    unsigned profile = 0;
    char renderer[128] = {};
};

static bool atLeast(const Got& g, int major, int minor) {
    return g.major > major || (g.major == major && g.minor >= minor);
}
static bool same(const Got& a, const Got& b) {
    return a.major == b.major && a.minor == b.minor;
}

static Got create(unsigned api, int major, int minor, unsigned profile, unsigned flags) {
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = api;
    cfg.versionMajor = major;
    cfg.versionMinor = minor;
    cfg.profile = profile;
    cfg.flags = flags;
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    Got g;
    TesseraContext ctx = TESSERA_NULL_HANDLE;
    g.result = tesseraCreateContext(&cfg, &ctx);
    if (g.result != TESSERA_SUCCESS)
        return g;
    TesseraContextInfo info = tesseraDefaultContextInfo();
    if (tesseraGetContextInfo(ctx, &info) == TESSERA_SUCCESS) {
        g.major = info.versionMajor;
        g.minor = info.versionMinor;
        g.profile = info.profile;
        std::snprintf(g.renderer, sizeof g.renderer, "%s", info.renderer);
    }
    tesseraDestroyContext(ctx);
    return g;
}

int main() {
    if (tesseraInit() != TESSERA_SUCCESS)
        return 1;

    TesseraContextConfig probeCfg = tesseraDefaultContextConfig();
    probeCfg.api = TESSERA_API_OPENGL;
    probeCfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraProbeReport rep = tesseraDefaultProbeReport();
    if (tesseraProbe(&probeCfg, &rep) != TESSERA_SUCCESS) {
        std::printf("skipped: no context can be created here (%s)\n", tesseraGetLastErrorMessage());
        tesseraTerminate();
        return 77;
    }
    std::printf("backend %s, %s\n", rep.backend, rep.info.renderer);

    // ---- desktop GL, core profile
    const Got maxCore =
        create(TESSERA_API_OPENGL, 0, 0, TESSERA_PROFILE_CORE, 0); // the old "no version" ladder: the newest
    if (maxCore.result == TESSERA_SUCCESS) {
        std::printf("  newest core context: %d.%d\n", maxCore.major, maxCore.minor);
        CHECK(atLeast(maxCore, 3, 2));
        if (atLeast(maxCore, 3, 3)) {
            // The floor: asking for 3.3 gives the newest, not 3.3.
            const Got floor33 = create(TESSERA_API_OPENGL, 3, 3, TESSERA_PROFILE_CORE, 0);
            std::printf("  core, at least 3.3: %d.%d\n", floor33.major, floor33.minor);
            CHECK(floor33.result == TESSERA_SUCCESS);
            CHECK(same(floor33, maxCore));
            // Exact: one request for 3.3, no ladder. The driver may still hand back something newer and
            // compatible (Mesa does for core profiles, NVIDIA does not), so the only promise is the minimum;
            // NVIDIA/WGL pins the behaviour.
            const Got exact33 =
                create(TESSERA_API_OPENGL, 3, 3, TESSERA_PROFILE_CORE, TESSERA_FLAG_EXACT_VERSION);
            std::printf("  core, exactly 3.3: %d.%d\n", exact33.major, exact33.minor);
            CHECK(exact33.result == TESSERA_SUCCESS);
            CHECK(atLeast(exact33, 3, 3));
            if (std::strcmp(rep.backend, "wgl") == 0 && std::strstr(rep.info.vendor, "NVIDIA"))
                CHECK(exact33.major == 3 && exact33.minor == 3);
            // Asking again (the remembered version) changes nothing.
            CHECK(same(create(TESSERA_API_OPENGL, 3, 3, TESSERA_PROFILE_CORE, 0), maxCore));
        }
        // Above what the driver has: refused, with or without EXACT_VERSION (the minimum is never relaxed).
        const Got tooNew = create(TESSERA_API_OPENGL, 9, 0, TESSERA_PROFILE_CORE, 0);
        CHECK(tooNew.result == TESSERA_ERROR_NO_MATCHING_CONFIG ||
              tooNew.result == TESSERA_ERROR_UNSUPPORTED);
        const Got tooNewExact =
            create(TESSERA_API_OPENGL, 9, 0, TESSERA_PROFILE_CORE, TESSERA_FLAG_EXACT_VERSION);
        CHECK(tooNewExact.result == TESSERA_ERROR_NO_MATCHING_CONFIG ||
              tooNewExact.result == TESSERA_ERROR_UNSUPPORTED);
    } else {
        std::printf("  no core profile here (%s)\n", tesseraResultString(maxCore.result));
    }

    // ---- OpenGL ES: unspecified means the newest ES, not 2.0
    const Got es = create(TESSERA_API_OPENGL_ES, 0, 0, TESSERA_PROFILE_ANY, 0);
    if (es.result == TESSERA_SUCCESS) {
        std::printf("  OpenGL ES, no version: %d.%d (%s)\n", es.major, es.minor, es.renderer);
        CHECK(atLeast(es, 2, 0));
        const Got es2 = create(TESSERA_API_OPENGL_ES, 2, 0, TESSERA_PROFILE_ANY, 0);
        CHECK(es2.result == TESSERA_SUCCESS && same(es2, es)); // a floor of 2.0 is the same newest
        const Got esExact =
            create(TESSERA_API_OPENGL_ES, 0, 0, TESSERA_PROFILE_ANY, TESSERA_FLAG_EXACT_VERSION);
        CHECK(esExact.result == TESSERA_SUCCESS &&
              atLeast(esExact, 2, 0)); // the old behaviour: ask for 2.0 (a driver may give more)
        if (atLeast(es, 3, 0)) {
            const Got es3 = create(TESSERA_API_OPENGL_ES, 3, 0, TESSERA_PROFILE_ANY, 0);
            CHECK(es3.result == TESSERA_SUCCESS && same(es3, es) && atLeast(es3, 3, 0));
        }
    } else {
        std::printf("  no OpenGL ES here (%s)\n", tesseraResultString(es.result));
    }

    tesseraTerminate();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
