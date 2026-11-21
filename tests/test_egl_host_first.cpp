// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors

// A host (GLFW, SDL, a toolkit) that initialised the EGL display BEFORE Tessera does: Tessera notices
// (eglQueryString answers before any eglInitialize of ours), says so once in its log, shares the display, and
// does not terminate it. Needs a fresh process, so it is its own executable. Exits 77 where there is no
// surfaceless EGL platform.
#include <tessera/tessera.h>

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int g_failures = 0;
#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                    \
            ++g_failures;                                                                                    \
        }                                                                                                    \
    } while (0)

static std::string g_log;
static void onLog(int32_t level, const char* message, void*) {
    if (level <= TESSERA_LOG_INFO) {
        g_log += message;
        g_log += '\n';
    }
}

int main() {
    setenv("TESSERA_BACKEND", "egl", 1);
    void* lib = dlopen("libEGL.so.1", RTLD_NOW);
    if (!lib)
        return 77;
    using GetProc = void* (*)(const char*);
    using GetPlatformDisplay = void* (*)(unsigned, void*, const int*);
    using Initialize = unsigned (*)(void*, int*, int*);
    using QueryString = const char* (*)(void*, int);
    auto getProc = reinterpret_cast<GetProc>(dlsym(lib, "eglGetProcAddress"));
    auto init = reinterpret_cast<Initialize>(dlsym(lib, "eglInitialize"));
    auto query = reinterpret_cast<QueryString>(dlsym(lib, "eglQueryString"));
    if (!getProc || !init || !query)
        return 77;
    auto getPlatformDisplay = reinterpret_cast<GetPlatformDisplay>(getProc("eglGetPlatformDisplayEXT"));
    const char* clientExts = query(nullptr, 0x3055);
    if (!getPlatformDisplay || !clientExts || !std::strstr(clientExts, "EGL_MESA_platform_surfaceless"))
        return 77;

    // The host: the same display Tessera will ask for (the surfaceless platform), initialised first.
    void* dpy = getPlatformDisplay(0x31DD, nullptr, nullptr);
    int major = 0, minor = 0;
    if (!dpy || !init(dpy, &major, &minor))
        return 77;
    CHECK(query(dpy, 0x3054) != nullptr); // the premise: the version string is available once initialised

    tesseraSetLogCallback(onLog, nullptr);
    if (tesseraInit() != TESSERA_SUCCESS)
        return 1;
    TesseraContextConfig cfg = tesseraDefaultContextConfig();
    cfg.api = TESSERA_API_OPENGL_ES;
    cfg.fallback = TESSERA_FALLBACK_BEST_EFFORT;
    TesseraContext ctx = 0;
    const TesseraResult r = tesseraCreateContext(&cfg, &ctx);
    if (r != TESSERA_SUCCESS) {
        std::printf("skipped: %s\n", tesseraGetLastErrorMessage());
        tesseraTerminate();
        return 77;
    }
    CHECK(g_log.find("already initialised by the host") != std::string::npos);
    std::printf("%s", g_log.c_str());
    tesseraDestroyContext(ctx);
    CHECK(tesseraTerminate() == TESSERA_SUCCESS);
    CHECK(query(dpy, 0x3054) !=
          nullptr); // the host's display is still initialised: Tessera did not terminate it

    // Once per display: a second round says nothing new.
    g_log.clear();
    if (tesseraInit() == TESSERA_SUCCESS) {
        if (tesseraCreateContext(&cfg, &ctx) == TESSERA_SUCCESS)
            tesseraDestroyContext(ctx);
        tesseraTerminate();
    }
    CHECK(g_log.find("already initialised by the host") == std::string::npos);

    if (g_failures)
        return 1;
    std::printf("ok\n");
    return 0;
}
