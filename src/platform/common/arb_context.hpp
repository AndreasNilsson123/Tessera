// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <tessera/tessera.h>

#include <functional>
#include <utility>
#include <vector>

namespace tessera {

/*
 * Context-creation attribute negotiation shared by the backends that expose the ARB_create_context
 * family under different prefixes: WGL ("WGL_") and GLX ("GLX_"). The attribute names and values are
 * identical in both; only the extension strings and the create call differ.
 */
struct ArbContextPlan {
    /* True when plain wglCreateContext / glXCreateNewContext already gives what was asked for. */
    bool legacy = false;
    /* Versions to try in order; {0,0} means "omit the version attributes". */
    std::vector<std::pair<int, int>> versions;

    /* Complete zero-terminated attribute list for one attempt. */
    std::vector<int> attribs(std::pair<int, int> version) const;

    int profileBit = 0;
    int ctxFlags = 0;
    std::vector<int> extra; // key,value pairs independent of the version
};

using HasExtension = std::function<bool(const char*)>;

/*
 * Versions to try for a request, newest first when the caller did not name one: {0,0} = omit the version,
 * ES defaults to 2.0, core/compatibility without a version walks 4.6 down to 3.2. Shared with the EGL
 * backend, whose attribute encoding differs but whose negotiation policy is the same.
 */
std::vector<std::pair<int, int>> contextVersionLadder(const TesseraContextConfig& config);

/*
 * Decides how to create `config`. `prefix` is "WGL_" or "GLX_"; `haveCreateAttribs` says whether the
 * create-with-attributes entry point was resolved. Strict configs fail when a requested extension is
 * missing; best-effort drops the request and logs it. Failure codes follow the Backend contract:
 * UNSUPPORTED = this platform cannot do ES at all (let another backend try), NO_MATCHING_CONFIG = the
 * driver cannot satisfy the request.
 */
TesseraResult planArbContext(const TesseraContextConfig& config, const char* prefix, bool haveCreateAttribs,
                             const HasExtension& hasExtension, ArbContextPlan& plan);

/* Pixel-format / framebuffer-config features that depend on extensions. */
struct FormatRequest {
    int samples = 0;
    bool srgb = false;
};

/*
 * Applies the extension gating common to WGL and GLX: strict configs fail with NO_MATCHING_CONFIG when
 * multisampling or sRGB is unavailable, best effort drops them. Float color buffers have no fallback.
 */
TesseraResult gateFormatFeatures(const TesseraContextConfig& config, const char* prefix,
                                 const HasExtension& hasExtension, FormatRequest& out);

/* One way of asking for a pixel format / fbconfig; later attempts are progressively weaker. */
struct FormatAttempt {
    int samples;
    bool srgb;
    bool relaxDepth;
};

/*
 * The negotiation ladder: the exact request first; in best-effort mode then halve the sample count down to
 * zero, dropping sRGB and finally the depth/stencil minimums at each step. Strict mode has one attempt.
 */
std::vector<FormatAttempt> formatAttempts(int samples, bool srgb, bool strict);

} // namespace tessera
