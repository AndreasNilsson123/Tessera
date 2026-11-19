// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors

// Coverage-guided (libFuzzer) fuzzing of the public C API against a fake backend: caller structs of exactly
// the claimed size, hostile handles and proc-address names. The first input byte selects the entry point, the
// rest is the payload. Everything the seeded test_fuzz covers at random, but steered by coverage. Build with
// the `fuzz` preset (clang); see docs/ci.md.
#include <tessera/tessera.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/runtime.hpp"

using namespace tessera;

namespace {

class FuzzContext : public ContextObject {
  public:
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
    bool supportsAdapterSelection() const noexcept override {
        return true;
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
        static int anchor;
        return (name[0] == 'g' && name[1] == 'l') ? static_cast<void*>(&anchor) : nullptr;
    }
};

void setup() {
    static const bool once = [] {
        Runtime::disableBuiltinBackendsForTesting();
        tesseraInit();
        Runtime::instance().registerBackend(std::make_shared<FuzzBackend>());
        return true;
    }();
    (void)once;
}

// The payload copied into an allocation of exactly its size, so any over-read or over-write is a sanitizer
// report.
struct Exact {
    std::unique_ptr<unsigned char[]> data;
    size_t size;
    Exact(const uint8_t* p, size_t n) : data(new unsigned char[n ? n : 1]), size(n) {
        if (n)
            std::memcpy(data.get(), p, n);
        // A caller's structSize can be smaller or larger than ours, but it can never exceed what the caller
        // allocated: a struct that claims more than its allocation is the CALLER's bug (undefined by
        // contract), not an input to fuzz.
        uint32_t claimed = 0;
        if (n >= sizeof claimed) {
            std::memcpy(&claimed, data.get(), sizeof claimed);
            if (claimed > n) {
                claimed = static_cast<uint32_t>(n);
                std::memcpy(data.get(), &claimed, sizeof claimed);
            }
        }
    }
};

uint64_t take64(const uint8_t*& p, size_t& n) {
    uint64_t v = 0;
    for (int i = 0; i < 8 && n; ++i, --n)
        v |= static_cast<uint64_t>(*p++) << (8 * i);
    return v;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    setup();
    if (size == 0)
        return 0;
    const uint8_t selector = data[0] % 13;
    ++data;
    --size;
    // Real handles are needed to reach the code behind the handle check: keep a few contexts alive.
    static TesseraContext live[2] = {0, 0};
    if (!live[0]) {
        TesseraContextConfig c = tesseraDefaultContextConfig();
        c.api = TESSERA_API_OPENGL;
        tesseraCreateContext(&c, &live[0]);
        tesseraCreateContext(&c, &live[1]);
    }

    switch (selector) {
    case 0:   // create from an arbitrary config image
    case 1: { // ... for a window
        const Exact e(data, size);
        const auto* cfg = reinterpret_cast<const TesseraContextConfig*>(e.data.get());
        if (size < sizeof(uint32_t))
            return 0;
        TesseraContext ctx = 0;
        const TesseraResult r = selector == 0 ? tesseraCreateContext(cfg, &ctx)
                                              : tesseraCreateContextForWindow(cfg, 0x1234, 0, &ctx);
        if (r == TESSERA_SUCCESS)
            tesseraDestroyContext(ctx);
        break;
    }
    case 2: { // prewarm (bounded by the library) and probe
        const Exact e(data, size);
        if (size < sizeof(uint32_t))
            return 0;
        const auto* cfg = reinterpret_cast<const TesseraContextConfig*>(e.data.get());
        tesseraPrewarmContexts(cfg, 1);
        TesseraProbeReport rep = tesseraDefaultProbeReport();
        tesseraProbe(cfg, &rep);
        break;
    }
    case 3: { // info into a buffer of exactly the claimed size
        uint64_t handleBits = take64(data, size);
        const Exact e(data, size);
        if (size < sizeof(uint32_t))
            return 0;
        tesseraGetContextInfo(handleBits & 1 ? live[0] : handleBits,
                              reinterpret_cast<TesseraContextInfo*>(e.data.get()));
        break;
    }
    case 4: { // native handles
        uint64_t handleBits = take64(data, size);
        const Exact e(data, size);
        if (size < sizeof(uint32_t))
            return 0;
        tesseraGetNativeHandles(handleBits & 1 ? live[0] : handleBits,
                                reinterpret_cast<TesseraNativeHandles*>(e.data.get()));
        break;
    }
    case 5: { // probe report
        const Exact e(data, size);
        TesseraContextConfig c = tesseraDefaultContextConfig();
        c.api = TESSERA_API_OPENGL;
        if (size < sizeof(uint32_t))
            return 0;
        tesseraProbe(&c, reinterpret_cast<TesseraProbeReport*>(e.data.get()));
        break;
    }
    case 6: { // handles in every state of disrepair
        const uint64_t h = take64(data, size);
        tesseraMakeCurrent(h);
        tesseraSwapBuffers(h);
        tesseraSetSwapInterval(h, static_cast<int32_t>(h >> 32));
        int32_t status = 0;
        tesseraGetResetStatus(h, &status);
        TesseraContext again = 0;
        if (tesseraRecreateContext(h, &again, nullptr) == TESSERA_SUCCESS)
            tesseraDestroyContext(again);
        TesseraSwapControl sc = tesseraDefaultSwapControl();
        tesseraGetSwapControl(h, &sc);
        int32_t lost = 0;
        tesseraIsContextLost(h, &lost, &status);
        tesseraMakeCurrent(TESSERA_NULL_HANDLE);
        break;
    }
    case 7: { // proc address names
        const std::string name(reinterpret_cast<const char*>(data), size);
        tesseraGetProcAddress(name.c_str());
        break;
    }
    case 8: { // swap control into a buffer of exactly the claimed size, for a live and a hostile handle
        const uint64_t handleBits = take64(data, size);
        const Exact e(data, size);
        if (size < sizeof(uint32_t))
            return 0;
        tesseraMakeCurrent(live[0]);
        tesseraGetSwapControl(handleBits & 1 ? live[0] : handleBits,
                              reinterpret_cast<TesseraSwapControl*>(e.data.get()));
        tesseraMakeCurrent(TESSERA_NULL_HANDLE);
        break;
    }
    case 9: { // the pool, with an arbitrary config image and arbitrary targets
        const int32_t target = static_cast<int32_t>(take64(data, size));
        const Exact e(data, size);
        if (size < sizeof(uint32_t))
            return 0;
        const auto* cfg = reinterpret_cast<const TesseraContextConfig*>(e.data.get());
        int32_t created = 0, count = 0;
        tesseraPoolTopUp(cfg, target & 0xF, &created);
        tesseraGetPooledCount(cfg, &count);
        tesseraPoolTopUp(cfg, target & 0xF, nullptr);
        if (target & 0x10)
            tesseraPoolClear();
        break;
    }
    case 10: { // is-lost queries: hostile handles, optional and missing out pointers
        const uint64_t h = take64(data, size);
        int32_t lost = 0, status = 0;
        tesseraIsContextLost(h, &lost, (h & 1) ? &status : nullptr);
        tesseraIsContextLost(live[1], (h & 2) ? &lost : nullptr, &status);
        break;
    }
    case 11: { // the lost callback: installed, replaced and removed around operations that may lose a context
        static int calls = 0;
        tesseraSetContextLostCallback([](TesseraContext, int32_t, void* user) { ++*static_cast<int*>(user); },
                                      &calls);
        const uint64_t h = take64(data, size);
        int32_t status = 0;
        tesseraGetResetStatus(h, &status);
        tesseraSetContextLostCallback(nullptr, nullptr);
        break;
    }
    default: { // result strings and the last error text
        tesseraResultString(static_cast<TesseraResult>(take64(data, size)));
        (void)tesseraGetLastErrorMessage();
        break;
    }
    }
    return 0;
}
