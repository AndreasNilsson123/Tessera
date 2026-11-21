// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "cgl_backend.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "cgl_api.hpp"
#include "core/boundary.hpp"
#include "core/library.hpp"
#include "core/log.hpp"
#include "platform/common/arb_context.hpp"
#include "platform/common/create_errors.hpp"
#include "platform/common/gl_query.hpp"

namespace tessera::cgl {
namespace {

std::string cglErrorText(const CglApi& api, const char* what, CGLError e) {
    const char* text = api.fn.CGLErrorString ? api.fn.CGLErrorString(e) : nullptr;
    char tail[32];
    std::snprintf(tail, sizeof tail, " (%d)", static_cast<int>(e));
    return std::string(what) + ": " + (text ? text : "unknown CGL error") + tail;
}

/* What the pixel format actually provided, read before the format is released. */
struct PixelFormatInfo {
    int colorSize = 0;
    int alphaSize = 0;
    int depthSize = 0;
    int stencilSize = 0;
    int samples = 0;
    bool doubleBuffer = false;
    bool stereo = false;
    bool colorFloat = false;
    int rendererId = 0; // kCGLPFARendererID: which renderer the format belongs to
};

// ---- a native context ---------------------------------------------------------------------------

class CglContext final : public ContextObject {
  public:
    explicit CglContext(const CglApi& api) : api_(api) {}
    ~CglContext() override {
        destroy();
    }

    TesseraResult describe(TesseraContextInfo& out) const noexcept override {
        out = info;
        return TESSERA_SUCCESS;
    }

    TesseraResult bindCurrent() noexcept override {
        const CGLError e = api_.fn.CGLSetCurrentContext(ctx);
        if (e != kNoError)
            return fail(TESSERA_ERROR_PLATFORM, cglErrorText(api_, "CGLSetCurrentContext failed", e));
        return TESSERA_SUCCESS;
    }

    void unbindCurrent() noexcept override {
        if (ctx && api_.fn.CGLGetCurrentContext() == ctx)
            api_.fn.CGLSetCurrentContext(nullptr);
    }

    // There is no drawable, hence nothing to present; render to framebuffer objects.
    TesseraResult swapBuffers() noexcept override {
        return TESSERA_SUCCESS;
    }

    TesseraResult nativeHandles(TesseraNativeHandles& out) const noexcept override {
        out.kind = TESSERA_NATIVE_CGL;
        out.handles[0] = static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(ctx));
        return TESSERA_SUCCESS;
    }

    TesseraResult setSwapInterval(int32_t) noexcept override {
        return fail(TESSERA_ERROR_UNSUPPORTED, "swap interval does not apply to drawable-less CGL contexts");
    }

    const CglApi& api_;
    CGLContextObj ctx = nullptr;
    int32_t profile = kProfileLegacy;
    std::shared_ptr<CglContext> parent; // keeps a share parent's native context alive as long as we are
    TesseraContextInfo info = {};

  private:
    void destroy() noexcept {
        if (!ctx)
            return;
        if (api_.fn.CGLGetCurrentContext() == ctx)
            api_.fn.CGLSetCurrentContext(nullptr);
        api_.fn.CGLDestroyContext(ctx);
        ctx = nullptr;
    }
};

// ---- negotiation ----------------------------------------------------------------------------------

struct CglPlan {
    std::vector<int32_t> profiles; // newest first
    bool floatColor = false;
    int samples = 0;
};

/*
 * Decides what to ask CGL for. macOS has exactly two kinds of desktop OpenGL context: a legacy 2.1
 * compatibility context and a core context (3.2 up to 4.1, always forward-compatible). Everything else is
 * refused (strict) or dropped with a log line (best effort), never silently approximated.
 */
TesseraResult planCgl(const TesseraContextConfig& c, const CglContext* parent, CglPlan& plan) {
    const bool strict = c.fallback == TESSERA_FALLBACK_STRICT;
    plan = CglPlan{};

    if (c.api == TESSERA_API_OPENGL_ES)
        return fail(TESSERA_ERROR_UNSUPPORTED,
                    "OpenGL ES is not available through CGL (use an ANGLE/EGL backend)");

    auto unavailable = [&](bool requested, const char* what) -> TesseraResult {
        if (!requested)
            return TESSERA_SUCCESS;
        if (strict)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        std::string(what) + " is not available through CGL");
        logf(TESSERA_LOG_INFO, "CGL: %s is not available, request dropped", what);
        return TESSERA_SUCCESS;
    };
    if (TesseraResult r = unavailable((c.flags & TESSERA_FLAG_DEBUG) != 0, "a debug context");
        r != TESSERA_SUCCESS)
        return r;
    if (TesseraResult r =
            unavailable((c.flags & (TESSERA_FLAG_ROBUST_ACCESS | TESSERA_FLAG_RESET_NOTIFICATION)) != 0,
                        "robust access / reset notification");
        r != TESSERA_SUCCESS)
        return r;
    if (TesseraResult r = unavailable((c.flags & TESSERA_FLAG_NO_ERROR) != 0, "a no-error context");
        r != TESSERA_SUCCESS)
        return r;
    if (TesseraResult r =
            unavailable((c.flags & TESSERA_FLAG_RELEASE_FLUSH) != 0, "flush-on-release control");
        r != TESSERA_SUCCESS)
        return r;
    if (TesseraResult r = unavailable((c.flags & TESSERA_FLAG_SRGB) != 0, "an sRGB default framebuffer");
        r != TESSERA_SUCCESS)
        return r;

    if (c.versionMajor > 4 || (c.versionMajor == 4 && c.versionMinor > 1))
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "macOS provides at most OpenGL 4.1");
    if (c.profile == TESSERA_PROFILE_COMPATIBILITY && c.versionMajor >= 3)
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "macOS has no compatibility profile above OpenGL 2.1");
    if (c.versionMajor == 2 && c.versionMinor > 1)
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "legacy macOS contexts top out at OpenGL 2.1");

    // Core is what you get for any request of 3.0+ (macOS has no 3.0/3.1 compatibility contexts) or an
    // explicit core profile.
    const bool core = c.profile == TESSERA_PROFILE_CORE || c.versionMajor >= 3;
    if (core)
        plan.profiles = c.versionMajor >= 4 ? std::vector<int32_t>{kProfile4Core}
                                            : std::vector<int32_t>{kProfile4Core, kProfile32Core};
    else
        plan.profiles = {kProfileLegacy};

    // Core contexts are forward-compatible by definition; a legacy context cannot be.
    if (!core) {
        if (TesseraResult r = unavailable((c.flags & TESSERA_FLAG_FORWARD_COMPATIBLE) != 0,
                                          "a forward-compatible legacy context");
            r != TESSERA_SUCCESS)
            return r;
    }

    // Shared objects need compatible formats: a child follows its parent's profile, and cannot mix legacy
    // with core.
    if (parent) {
        const bool parentCore = parent->profile != kProfileLegacy;
        if (parentCore != core)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "cannot share objects between a legacy (2.1) and a core context");
        plan.profiles = {parent->profile};
    }

    plan.floatColor = (c.flags & TESSERA_FLAG_FLOAT_COLOR) != 0;
    plan.samples = c.samples;
    return TESSERA_SUCCESS;
}

/* "cgl:0x00024600" -> the renderer id; false for anything else. */
bool parseRendererId(const char* id, int32_t& out) {
    if (!id || std::strncmp(id, "cgl:0x", 6) != 0 || id[6] == '\0')
        return false;
    uint32_t v = 0;
    int digits = 0;
    for (const char* p = id + 6; *p; ++p, ++digits) {
        int d;
        if (*p >= '0' && *p <= '9')
            d = *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F')
            d = *p - 'A' + 10;
        else
            return false;
        if (digits >= 8)
            return false;
        v = v * 16 + static_cast<uint32_t>(d);
    }
    out = static_cast<int32_t>(v);
    return true;
}

std::string rendererIdText(int32_t id) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "cgl:0x%08x", static_cast<unsigned>(id));
    return buf;
}

std::vector<PixelFormatAttribute> pixelFormatAttribs(const TesseraContextConfig& c, int32_t profile,
                                                     const FormatAttempt& a, bool floatColor,
                                                     bool allowOffline, int32_t rendererId = 0) {
    std::vector<PixelFormatAttribute> v;
    auto add = [&v](PixelFormatAttribute key, int32_t value) {
        v.push_back(key);
        v.push_back(value);
    };
    add(kPFAOpenGLProfile, profile);
    int rgb = c.redBits + c.greenBits + c.blueBits;
    if (rgb == 0)
        rgb = floatColor ? 48 : 24; // sane default for "don't care"
    add(kPFAColorSize, rgb);
    if (c.alphaBits > 0)
        add(kPFAAlphaSize, c.alphaBits);
    if (!a.relaxDepth) {
        if (c.depthBits > 0)
            add(kPFADepthSize, c.depthBits);
        if (c.stencilBits > 0)
            add(kPFAStencilSize, c.stencilBits);
    }
    if (c.flags & TESSERA_FLAG_DOUBLE_BUFFER)
        v.push_back(kPFADoubleBuffer);
    if (c.flags & TESSERA_FLAG_STEREO)
        v.push_back(kPFAStereo);
    if (a.samples > 0) {
        add(kPFASampleBuffers, 1);
        add(kPFASamples, a.samples);
    }
    if (floatColor)
        v.push_back(kPFAColorFloat);
    if (rendererId != 0)
        add(kPFARendererID, rendererId); // an explicit GPU: this renderer or nothing
    if (allowOffline)
        v.push_back(kPFAAllowOfflineRenderers);
    v.push_back(0);
    return v;
}

PixelFormatInfo describePixelFormat(const CglApi& api, CGLPixelFormatObj pf) {
    auto get = [&](PixelFormatAttribute attr) {
        GLint value = 0;
        api.fn.CGLDescribePixelFormat(pf, 0, attr, &value);
        return static_cast<int>(value);
    };
    PixelFormatInfo p;
    p.colorSize = get(kPFAColorSize);
    p.alphaSize = get(kPFAAlphaSize);
    p.depthSize = get(kPFADepthSize);
    p.stencilSize = get(kPFAStencilSize);
    p.samples = get(kPFASamples);
    p.doubleBuffer = get(kPFADoubleBuffer) != 0;
    p.stereo = get(kPFAStereo) != 0;
    p.colorFloat = get(kPFAColorFloat) != 0;
    p.rendererId = get(kPFARendererID);
    return p;
}

/* Requires the context to be current on the calling thread. */
bool queryInfo(const CglApi& api, const PixelFormatInfo& p, TesseraContextInfo& info) {
    GlQueryFns gl;
    gl.getString = reinterpret_cast<decltype(gl.getString)>(api.lib.symbol("glGetString"));
    gl.getIntegerv = reinterpret_cast<decltype(gl.getIntegerv)>(api.lib.symbol("glGetIntegerv"));
    gl.getError = reinterpret_cast<decltype(gl.getError)>(api.lib.symbol("glGetError"));
    if (!queryContextInfo(gl, info))
        return false;

    // CGL's colour size may or may not include the alpha channel; if removing alpha leaves a whole number of
    // >=5-bit RGB channels, it did. (UNVERIFIED on real hardware; only affects the reported per-channel bit
    // counts.)
    int rgb = p.colorSize;
    if (p.alphaSize > 0 && p.colorSize > p.alphaSize && (p.colorSize - p.alphaSize) % 3 == 0 &&
        (p.colorSize - p.alphaSize) >= 15)
        rgb = p.colorSize - p.alphaSize;
    info.redBits = info.greenBits = info.blueBits = rgb / 3;
    info.alphaBits = p.alphaSize;
    info.depthBits = p.depthSize;
    info.stencilBits = p.stencilSize;
    info.samples = p.samples;
    if (p.doubleBuffer)
        info.flags |= TESSERA_FLAG_DOUBLE_BUFFER;
    if (p.stereo)
        info.flags |= TESSERA_FLAG_STEREO;
    if (p.colorFloat)
        info.flags |= TESSERA_FLAG_FLOAT_COLOR;
    info.surface = TESSERA_SURFACE_NONE; // CGL contexts here never have a drawable
    if (p.rendererId != 0) {             // which renderer (GPU) the pixel format belongs to
        std::snprintf(info.adapter, sizeof info.adapter, "%s", rendererIdText(p.rendererId).c_str());
        if (p.rendererId == kRendererGeneric || p.rendererId == kRendererGenericFloat ||
            p.rendererId == kRendererAppleSW)
            info.adapterClass |= TESSERA_ADAPTER_SOFTWARE;
    }
    return true;
}

// ---- the backend --------------------------------------------------------------------------------

class CglBackend final : public Backend {
  public:
    const char* name() const noexcept override {
        return "cgl";
    }

    /* A renderer id ("cgl:0x00024600") is honoured through the pixel format; LUIDs, preferences and other
     * backends' ids are not ours. */
    bool supportsAdapterSelection(const TesseraContextConfig& c) const noexcept override {
        int32_t id = 0;
        return c.adapterLuid == 0 && c.gpuPreference == TESSERA_GPU_DEFAULT && parseRendererId(c.adapter, id);
    }

    void enumerateAdapters(std::vector<TesseraAdapterInfo>& out) override {
        const CglApi* api = cglApi();
        if (!api || !api->fn.CGLQueryRendererInfo || !api->fn.CGLDescribeRenderer ||
            !api->fn.CGLDestroyRendererInfo)
            return;
        CGLRendererInfoObj info = nullptr;
        GLint count = 0;
        if (api->fn.CGLQueryRendererInfo(0xFFFFFFFFu, &info, &count) != kNoError || !info)
            return;
        for (GLint i = 0; i < count; ++i) {
            auto prop = [&](CGLRendererProperty p) {
                GLint v = 0;
                api->fn.CGLDescribeRenderer(info, i, p, &v);
                return static_cast<int32_t>(v);
            };
            const int32_t id = prop(kRPRendererID);
            TesseraAdapterInfo a = tesseraDefaultAdapterInfo();
            std::snprintf(a.id, sizeof a.id, "%s", rendererIdText(id).c_str());
            const bool accelerated = prop(kRPAccelerated) != 0;
            if (!accelerated || id == kRendererGeneric || id == kRendererGenericFloat ||
                id == kRendererAppleSW)
                a.flags |= TESSERA_ADAPTER_SOFTWARE;
            const int mb = prop(kRPVideoMemoryMB);
            std::snprintf(a.name, sizeof a.name, "CGL renderer 0x%08x (%s%s%s)", static_cast<unsigned>(id),
                          accelerated ? "accelerated" : "software", prop(kRPOnline) ? "" : ", offline",
                          mb > 0 ? (", " + std::to_string(mb) + " MB").c_str() : "");
            out.push_back(a);
        }
        api->fn.CGLDestroyRendererInfo(info);
    }

    TesseraResult createContext(const TesseraContextConfig& c,
                                const std::shared_ptr<ContextObject>& shareWith,
                                std::shared_ptr<ContextObject>& out) override {
        const CglApi* apiPtr = cglApi();
        if (!apiPtr)
            return fail(TESSERA_ERROR_UNSUPPORTED, "OpenGL.framework (CGL) is not available");
        const CglApi& api = *apiPtr;

        std::shared_ptr<CglContext> parent;
        if (shareWith) {
            parent = std::dynamic_pointer_cast<CglContext>(shareWith);
            if (!parent)
                return fail(TESSERA_ERROR_INVALID_ARGUMENT,
                            "cannot share objects with a context from another backend");
        }

        // An explicit renderer: honoured through the pixel format, which offers this renderer or nothing.
        // Allowing offline renderers (below) can wake a discrete GPU; asking for one by id is the caller's
        // own choice to pay that.
        int32_t wantedRenderer = 0;
        if (c.adapter[0] != '\0' && !parseRendererId(c.adapter, wantedRenderer))
            return TESSERA_ERROR_UNSUPPORTED; // another backend's kind of id

        // Making the new context current changes this thread's CGL state; the caller's goes back on every
        // path.
        ForeignContextGuard guard;

        CglPlan plan;
        if (TesseraResult r = planCgl(c, parent.get(), plan); r != TESSERA_SUCCESS)
            return r;

        // Newest profile first; within it the usual relaxation ladder. A format that only exists on an
        // offline (not currently display-driving) GPU is tried last, because allowing offline renderers can
        // wake a discrete GPU.
        CGLPixelFormatObj pf = nullptr;
        int32_t chosenProfile = kProfileLegacy;
        CGLError lastError = kNoError;
        const bool strict = c.fallback == TESSERA_FALLBACK_STRICT;
        bool relaxedLogged = false;
        for (int32_t profile : plan.profiles) {
            for (const FormatAttempt& attempt : formatAttempts(plan.samples, false, strict)) {
                for (int offline = 0; offline < 2 && !pf; ++offline) {
                    const std::vector<PixelFormatAttribute> attribs = pixelFormatAttribs(
                        c, profile, attempt, plan.floatColor, offline != 0, wantedRenderer);
                    GLint count = 0;
                    CGLPixelFormatObj candidate = nullptr;
                    const CGLError e = api.fn.CGLChoosePixelFormat(attribs.data(), &candidate, &count);
                    if (e == kNoError && candidate && count > 0) {
                        pf = candidate;
                        chosenProfile = profile;
                        if (attempt.samples != plan.samples || attempt.relaxDepth) {
                            relaxedLogged = true;
                            logf(TESSERA_LOG_INFO,
                                 "CGL: pixel format relaxed (samples %d->%d, depth relaxed %d)", c.samples,
                                 attempt.samples, attempt.relaxDepth);
                        }
                    } else if (e != kNoError) {
                        lastError = e;
                    }
                }
                if (pf)
                    break;
            }
            if (pf)
                break;
        }
        if (!pf) {
            if (lastError == kBadAlloc)
                return fail(TESSERA_ERROR_OUT_OF_MEMORY,
                            cglErrorText(api, "CGLChoosePixelFormat failed", lastError));
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        lastError == kNoError
                            ? std::string("no CGL pixel format matches the requested configuration")
                            : cglErrorText(api, "no CGL pixel format matches the requested configuration",
                                           lastError));
        }

        auto ctx = std::make_shared<CglContext>(api); // RAII: any failure below cleans up through ~CglContext
        ctx->profile = chosenProfile;
        const CGLError created = api.fn.CGLCreateContext(pf, parent ? parent->ctx : nullptr, &ctx->ctx);
        const PixelFormatInfo pfInfo = created == kNoError ? describePixelFormat(api, pf) : PixelFormatInfo{};
        api.fn.CGLReleasePixelFormat(pf); // the context keeps its own reference
        if (created != kNoError || !ctx->ctx) {
            ctx->ctx = nullptr;
            switch (classifyCglCreateError(created)) { // create_errors.hpp
            case TESSERA_ERROR_OUT_OF_MEMORY:
                return fail(TESSERA_ERROR_OUT_OF_MEMORY,
                            cglErrorText(api, "CGLCreateContext failed", created));
            case TESSERA_ERROR_NO_MATCHING_CONFIG:
                return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                            cglErrorText(api, "CGLCreateContext rejected the pixel format", created));
            default:
                return fail(TESSERA_ERROR_PLATFORM, cglErrorText(api, "CGLCreateContext failed", created));
            }
        }
        ctx->parent = std::move(parent);

        if (TesseraResult r = ctx->bindCurrent(); r != TESSERA_SUCCESS)
            return r;
        const bool ok = queryInfo(api, pfInfo, ctx->info);
        // `guard` puts the caller's context back; it must not be left to us.
        if (!ok)
            return fail(TESSERA_ERROR_PLATFORM, "could not query the new context (glGetString failed)");

        // Never negotiable, whatever the fallback policy: API, minimum version, profile.
        if (const char* why = hardRequirementViolation(c, ctx->info))
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, why);

        // Strict mode promises exactly what was asked for: verify the parts the driver may have loosened.
        if (strict && !contextSatisfies(c, ctx->info))
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "the created context does not satisfy the strict request");

        // CGLChoosePixelFormat treats the sample count as a wish and hands back the closest format instead of
        // failing, so the ladder above never sees a relaxation; what was granted is only known from the
        // read-back.
        if (!strict && !relaxedLogged && ctx->info.samples < c.samples)
            logf(TESSERA_LOG_INFO, "CGL: pixel format relaxed (samples %d->%d)", c.samples,
                 ctx->info.samples);

        out = std::move(ctx);
        return TESSERA_SUCCESS;
    }

    void* getProcAddress(const char* name) noexcept override {
        const CglApi* api = cglApi();
        if (!api || !name)
            return nullptr;
        // Only GL functions: the framework also exports CGL*, NS* and C library symbols we must not hand out.
        if (name[0] != 'g' || name[1] != 'l')
            return nullptr;
        return api->lib.symbol(name);
    }
};

} // namespace

std::shared_ptr<Backend> makeCglBackend() {
    return std::make_shared<CglBackend>();
}

} // namespace tessera::cgl
