// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <tessera/tessera.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tessera::win32 {

/* A display adapter as DXGI reports it. */
struct AdapterInfo {
    uint64_t luid = 0; // (HighPart << 32) | LowPart, the value of TesseraContextConfig::adapterLuid
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    bool software = false; // Microsoft Basic Render Driver / WARP
    uint64_t dedicatedVideoMemory = 0;
    std::string name; // UTF-8
};

/* All adapters, in DXGI order. Empty (with `error`) if dxgi.dll is unavailable. */
std::vector<AdapterInfo> enumerateAdapters(std::string& error);

/* Which adapter the request names: the LUID, or the DXGI choice for HIGH_PERFORMANCE / LOW_POWER (software
 * adapters are never chosen by preference). False with `error` if there is none. Not called for a request
 * without an adapter. */
bool resolveAdapter(const TesseraContextConfig& config, AdapterInfo& out, std::string& error);

/*
 * Does the OpenGL driver that produced `vendor`/`renderer` belong to `wanted`? A process cannot choose which
 * GPU's OpenGL driver Windows loads for it (that is decided per application in Windows' graphics settings, or
 * by the host EXE exporting NvOptimusEnablement / AmdPowerXpressRequestHighPerformance), so Tessera does not
 * pretend to select: it checks. The check is by PCI vendor, so it can say "this is the NVIDIA driver, not the
 * Intel one"; two adapters of the same vendor cannot be told apart and are reported as unverifiable.
 *
 * Returns an empty string if the driver matches, otherwise the reason.
 */
std::string verifyAdapter(const AdapterInfo& wanted, const std::vector<AdapterInfo>& all, const char* vendor,
                          const char* renderer);

bool adapterRequested(const TesseraContextConfig& config) noexcept;

} // namespace tessera::win32
