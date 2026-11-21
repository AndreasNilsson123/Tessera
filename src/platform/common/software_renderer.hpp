// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <string>

namespace tessera {

/*
 * Whether a GL_RENDERER string names a renderer that runs on the CPU. The "software" backend (ADR 0015)
 * promises exactly that, and the EGL extension that suggests it does not deliver: Mesa's
 * EGL_MESA_device_software marks a device that uses the software window system, which includes GPU drivers
 * (WSL's D3D12 driver reports it for the RTX it runs on). So the promise is checked on what the driver says
 * it is.
 */
inline bool isCpuRenderer(const std::string& renderer) {
    std::string r;
    for (char c : renderer)
        r.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    static const char* const words[] = {
        "llvmpipe", // Mesa, LLVM JIT
        "softpipe", // Mesa, reference
        "swrast",   // Mesa classic / "Software Rasterizer"
        "software rasterizer",
        "swiftshader",  // Google's, Vulkan on the CPU (ANGLE's fallback)
        "basic render", // "Microsoft Basic Render Driver": what Direct3D calls WARP
        "warp",
        "gdi generic",
        "apple software renderer"};
    for (const char* w : words)
        if (r.find(w) != std::string::npos)
            return true;
    return false;
}

} // namespace tessera
