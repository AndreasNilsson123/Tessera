// Phase 0 smoke + ABI-shape tests. Intentionally dependency-free (no test framework yet).
#include <tessera/tessera.h>

#include <cstdio>
#include <cstring>
#include <thread>

static int g_failures = 0;
#define CHECK(cond)                                                                                      \
    do {                                                                                                 \
        if (!(cond)) {                                                                                   \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                \
            ++g_failures;                                                                                \
        }                                                                                                \
    } while (0)

// Layout guards: these sizes are part of the ABI. Changing them is an ABI break.
static_assert(sizeof(TesseraHandle) == 8, "handles are 64-bit");
static_assert(sizeof(TesseraResult) == 4, "results are 32-bit");
static_assert(sizeof(TesseraContextInfo) == 504, "TesseraContextInfo layout");
static_assert(sizeof(TesseraProbeReport) == 832, "TesseraProbeReport layout");
static_assert(sizeof(TesseraContextConfig) == 4 * 15 + 4 /*pad*/ + 8 + 64, "TesseraContextConfig layout");

int main() {
    int32_t major = -1, minor = -1, patch = -1;
    tesseraGetVersion(&major, &minor, &patch);
    CHECK(major == TESSERA_VERSION_MAJOR);
    CHECK(minor == TESSERA_VERSION_MINOR);
    CHECK(patch == TESSERA_VERSION_PATCH);
    tesseraGetVersion(nullptr, nullptr, nullptr); // must not crash

    CHECK(tesseraGetAbiVersion() == TESSERA_ABI_VERSION);

    CHECK(std::strcmp(tesseraResultString(TESSERA_SUCCESS), "success") == 0);
    CHECK(tesseraResultString(-12345) != nullptr);
    CHECK(tesseraGetLastErrorMessage() != nullptr);
    CHECK(tesseraGetLastErrorMessage()[0] == '\0');

    // Last-error storage is thread-local: a fresh thread starts empty.
    std::thread([] { CHECK(tesseraGetLastErrorMessage()[0] == '\0'); }).join();

    if (g_failures == 0) std::puts("test_abi: OK");
    return g_failures == 0 ? 0 : 1;
}
