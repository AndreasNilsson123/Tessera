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

/* ---- the text id of tesseraEnumerateAdapters ("luid:00000000-0000ABCD"): pure, tested without DXGI ---- */

std::string adapterId(uint64_t luid);
/* True when `id` is a well-formed "luid:" id; `luid` receives it. A different prefix ("uuid:", "cgl:") is
 * false. */
bool parseAdapterId(const char* id, uint64_t& luid);
/* TESSERA_ADAPTER_*: software from DXGI's flag; integrated vs discrete by dedicated video memory (under 1 GiB
 * = shares the CPU's memory), which is a heuristic and documented as one. */
uint32_t adapterClassOf(const AdapterInfo& a);
void toPublic(const AdapterInfo& a, TesseraAdapterInfo& out);

/* The adapter whose driver produced `vendor`/`renderer`: the one hardware adapter of that vendor, or the
 * software adapter for the Microsoft renderers; null when it cannot be told (two adapters of one vendor, a
 * vendor Tessera does not know). */
const AdapterInfo* identifyServing(const std::vector<AdapterInfo>& all, const char* vendor,
                                   const char* renderer);

/* Hybrid laptops: the OpenGL driver the process got belongs to a lower-power adapter while a stronger one
 * exists. Returns the one-line advice (what a host can do), or "" when there is nothing to say. Pure. */
std::string hybridAdvisory(const std::vector<AdapterInfo>& all, const char* vendor, const char* renderer);

} // namespace tessera::win32
