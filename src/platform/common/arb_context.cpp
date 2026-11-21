// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "arb_context.hpp"
#include "version_negotiation.hpp"

#include "context_priority.hpp"

#include <string>

#include "core/boundary.hpp"
#include "core/log.hpp"

namespace tessera {
namespace {

constexpr int kCtxMajor = 0x2091;
constexpr int kCtxMinor = 0x2092;
constexpr int kCtxFlags = 0x2094;
constexpr int kCtxProfileMask = 0x9126;
constexpr int kCtxNoError = 0x31B3;
constexpr int kCtxResetStrategy = 0x8256;
constexpr int kCtxLoseOnReset = 0x8252;
constexpr int kCtxReleaseBehavior = 0x2097;
constexpr int kCtxReleaseFlush = 0x2098;

constexpr int kProfileCore = 0x1;
constexpr int kProfileCompat = 0x2;
constexpr int kProfileEs2 = 0x4;
constexpr int kFlagDebug = 0x1;
constexpr int kFlagForward = 0x2;
constexpr int kFlagRobust = 0x4;

/* True when plain legacy creation gives exactly what was asked for (no version/profile/flags). */
bool legacySuffices(const TesseraContextConfig& c) {
    constexpr uint32_t kContextFlags = TESSERA_FLAG_DEBUG | TESSERA_FLAG_FORWARD_COMPATIBLE |
                                       TESSERA_FLAG_ROBUST_ACCESS | TESSERA_FLAG_RESET_NOTIFICATION |
                                       TESSERA_FLAG_NO_ERROR | TESSERA_FLAG_RELEASE_FLUSH;
    return c.api == TESSERA_API_OPENGL && c.profile == TESSERA_PROFILE_ANY && c.versionMajor <= 2 &&
           (c.flags & kContextFlags) == 0 && c.priority == TESSERA_PRIORITY_DEFAULT;
}

} // namespace

std::vector<std::pair<int, int>> contextVersionLadder(const TesseraContextConfig& c) {
    return versionLadder(c); // version_negotiation.hpp
}

std::vector<int> ArbContextPlan::attribs(std::pair<int, int> version) const {
    std::vector<int> a;
    if (version.first != 0) {
        a.insert(a.end(), {kCtxMajor, version.first, kCtxMinor, version.second});
    }
    if (profileBit)
        a.insert(a.end(), {kCtxProfileMask, profileBit});
    if (ctxFlags)
        a.insert(a.end(), {kCtxFlags, ctxFlags});
    a.insert(a.end(), extra.begin(), extra.end());
    a.push_back(0);
    return a;
}

TesseraResult planArbContext(const TesseraContextConfig& c, const char* prefix, bool haveCreateAttribs,
                             const HasExtension& has, ArbContextPlan& plan) {
    plan = ArbContextPlan{};
    const bool strict = c.fallback == TESSERA_FALLBACK_STRICT;
    const std::string p = prefix;

    // A priority request is decided first: it can only be honoured through GLX_EXT_context_priority (which
    // needs the attribute-based create call); WGL has no such extension at all. Strict refuses what cannot be
    // honoured, best-effort drops it, and the rest of the plan then proceeds as if it had not been asked for
    // (so a driver without ARB_create_context still gets its legacy context).
    TesseraContextConfig eff = c;
    bool sendPriority = false;
    if (c.priority != TESSERA_PRIORITY_DEFAULT) {
        std::string why;
        if (p != "GLX_")
            why =
                "context priority is not available through " + p.substr(0, 3) + " (no such extension exists)";
        else if (!haveCreateAttribs || !has("GLX_ARB_create_context"))
            why = "context priority needs GLX_ARB_create_context, which the driver lacks";
        else if (!has("GLX_EXT_context_priority"))
            why = "required extension missing: GLX_EXT_context_priority";
        if (why.empty()) {
            sendPriority = true;
        } else if (strict) {
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, why);
        } else {
            logf(TESSERA_LOG_INFO, "%s, request dropped", why.c_str());
            eff.priority = TESSERA_PRIORITY_DEFAULT;
        }
    }

    if (legacySuffices(eff)) {
        plan.legacy = true;
        return TESSERA_SUCCESS;
    }

    if (!haveCreateAttribs || !has((p + "ARB_create_context").c_str()))
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                    "the driver lacks " + p + "ARB_create_context (needed for this request)");

    if (c.api == TESSERA_API_OPENGL_ES) {
        if (!has((p + "EXT_create_context_es2_profile").c_str()) &&
            !has((p + "EXT_create_context_es_profile").c_str()))
            return fail(TESSERA_ERROR_UNSUPPORTED, "OpenGL ES contexts are not available through " +
                                                       std::string(prefix) + " on this driver");
        plan.profileBit = kProfileEs2;
    } else if (c.profile != TESSERA_PROFILE_ANY) {
        if (!has((p + "ARB_create_context_profile").c_str()))
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "the driver lacks " + p + "ARB_create_context_profile");
        plan.profileBit = c.profile == TESSERA_PROFILE_CORE ? kProfileCore : kProfileCompat;
    }

    // Strict fails on a missing extension; best effort logs and drops the request.
    auto require = [&](const std::string& ext, bool& ok) {
        ok = has(ext.c_str());
        if (ok)
            return TESSERA_SUCCESS;
        if (strict)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "required extension missing: " + ext);
        logf(TESSERA_LOG_INFO, "%s missing, request dropped", ext.c_str());
        return TESSERA_SUCCESS;
    };

    if (c.flags & TESSERA_FLAG_DEBUG)
        plan.ctxFlags |= kFlagDebug;
    if (c.flags & TESSERA_FLAG_FORWARD_COMPATIBLE)
        plan.ctxFlags |= kFlagForward;
    if (c.flags & TESSERA_FLAG_ROBUST_ACCESS) {
        bool ok = false;
        if (TesseraResult r = require(p + "ARB_create_context_robustness", ok); r != TESSERA_SUCCESS)
            return r;
        if (ok) {
            plan.ctxFlags |= kFlagRobust;
            if (c.flags & TESSERA_FLAG_RESET_NOTIFICATION)
                plan.extra.insert(plan.extra.end(), {kCtxResetStrategy, kCtxLoseOnReset});
        }
    }
    if (c.flags & TESSERA_FLAG_NO_ERROR) {
        bool ok = false;
        if (TesseraResult r = require(p + "ARB_create_context_no_error", ok); r != TESSERA_SUCCESS)
            return r;
        if (ok)
            plan.extra.insert(plan.extra.end(), {kCtxNoError, 1});
    }
    if (c.flags & TESSERA_FLAG_RELEASE_FLUSH) {
        bool ok = false;
        if (TesseraResult r = require(p + "ARB_context_flush_control", ok); r != TESSERA_SUCCESS)
            return r;
        if (ok)
            plan.extra.insert(plan.extra.end(), {kCtxReleaseBehavior, kCtxReleaseFlush});
    }

    if (sendPriority)
        plan.extra.insert(plan.extra.end(), {kNativeContextPriorityLevel, nativePriority(c.priority)});

    plan.versions = contextVersionLadder(c);
    return TESSERA_SUCCESS;
}

TesseraResult gateFormatFeatures(const TesseraContextConfig& c, const char* prefix, const HasExtension& has,
                                 FormatRequest& out) {
    const bool strict = c.fallback == TESSERA_FALLBACK_STRICT;
    const std::string p = prefix;
    out.samples = c.samples;
    out.srgb = (c.flags & TESSERA_FLAG_SRGB) != 0;

    if (out.samples > 0 && !has((p + "ARB_multisample").c_str())) {
        if (strict)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG,
                        "multisampling (" + p + "ARB_multisample) is not available");
        out.samples = 0;
    }
    if (out.srgb && !has((p + "ARB_framebuffer_sRGB").c_str()) &&
        !has((p + "EXT_framebuffer_sRGB").c_str())) {
        if (strict)
            return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "sRGB framebuffers are not available");
        out.srgb = false;
    }
    if ((c.flags & TESSERA_FLAG_FLOAT_COLOR) && !has((p + "ARB_pixel_format_float").c_str()) &&
        !has((p + "ARB_fbconfig_float").c_str()))
        return fail(TESSERA_ERROR_NO_MATCHING_CONFIG, "float color buffers are not available");
    return TESSERA_SUCCESS;
}

std::vector<FormatAttempt> formatAttempts(int samples, bool srgb, bool strict) {
    std::vector<FormatAttempt> attempts;
    for (int s = samples;; s = s > 1 ? s / 2 : 0) {
        attempts.push_back({s, srgb, false});
        if (!strict) {
            if (srgb)
                attempts.push_back({s, false, false});
            attempts.push_back({s, false, true});
        }
        if (strict || s == 0)
            break;
    }
    return attempts;
}

} // namespace tessera
