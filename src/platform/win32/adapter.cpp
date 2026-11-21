// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#include "adapter.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format off
#include <dxgi1_6.h>
// clang-format on

#include <cstring>

#include "core/library.hpp"
#include "core/log.hpp"

namespace tessera::win32 {
namespace {

using PfnCreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);

std::string toUtf8(const wchar_t* w) {
    std::string out;
    if (!w)
        return out;
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n > 1) {
        out.resize(static_cast<std::size_t>(n - 1));
        WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    }
    return out;
}

AdapterInfo describe(IDXGIAdapter1* a) {
    DXGI_ADAPTER_DESC1 d = {};
    a->GetDesc1(&d);
    AdapterInfo info;
    info.luid =
        (static_cast<uint64_t>(static_cast<uint32_t>(d.AdapterLuid.HighPart)) << 32) | d.AdapterLuid.LowPart;
    info.vendorId = d.VendorId;
    info.deviceId = d.DeviceId;
    info.software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    info.dedicatedVideoMemory = d.DedicatedVideoMemory;
    info.name = toUtf8(d.Description);
    return info;
}

// The library is kept for the life of the process: the factory objects we hand out must not outlive it, and
// unloading a system DLL other components use gains nothing.
IDXGIFactory1* createFactory(std::string& error) {
    static DynamicLibrary* const dxgi = new DynamicLibrary(DynamicLibrary::open("dxgi.dll"));
    if (!*dxgi) {
        error = "dxgi.dll could not be loaded";
        return nullptr;
    }
    auto create = reinterpret_cast<PfnCreateFactory1>(dxgi->symbol("CreateDXGIFactory1"));
    if (!create) {
        error = "dxgi.dll has no CreateDXGIFactory1";
        return nullptr;
    }
    IDXGIFactory1* factory = nullptr;
    if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
        error = "CreateDXGIFactory1 failed";
        return nullptr;
    }
    return factory;
}

bool vendorOwns(uint32_t vendorId, const std::string& text) {
    const auto has = [&text](const char* needle) { return text.find(needle) != std::string::npos; };
    switch (vendorId) {
    case 0x10DE:
        return has("NVIDIA");
    case 0x1002:
    case 0x1022:
        return has("ATI") || has("AMD") || has("Radeon");
    case 0x8086:
        return has("Intel");
    case 0x1414:
        return has("Microsoft") || has("GDI Generic") || has("Basic Render");
    case 0x5143:
        return has("Qualcomm") || has("Adreno");
    default:
        return false;
    }
}

} // namespace

bool adapterRequested(const TesseraContextConfig& c) noexcept {
    return c.adapterLuid != 0 || c.gpuPreference != TESSERA_GPU_DEFAULT || c.adapter[0] != '\0';
}

std::string adapterId(uint64_t luid) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "luid:%08X-%08X", static_cast<unsigned>(luid >> 32),
                  static_cast<unsigned>(luid & 0xFFFFFFFFu));
    return buf;
}

bool parseAdapterId(const char* id, uint64_t& luid) {
    if (!id || std::strncmp(id, "luid:", 5) != 0)
        return false;
    const char* p = id + 5;
    uint64_t hi = 0, lo = 0;
    for (int part = 0; part < 2; ++part) {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i, ++p) {
            const char ch = *p;
            int d;
            if (ch >= '0' && ch <= '9')
                d = ch - '0';
            else if (ch >= 'a' && ch <= 'f')
                d = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F')
                d = ch - 'A' + 10;
            else
                return false;
            v = v * 16 + static_cast<uint64_t>(d);
        }
        (part == 0 ? hi : lo) = v;
        if (part == 0) {
            if (*p != '-')
                return false;
            ++p;
        }
    }
    if (*p != '\0')
        return false;
    luid = (hi << 32) | lo;
    return luid != 0;
}

uint32_t adapterClassOf(const AdapterInfo& a) {
    if (a.software)
        return TESSERA_ADAPTER_SOFTWARE;
    return a.dedicatedVideoMemory >= (1ull << 30) ? TESSERA_ADAPTER_DISCRETE : TESSERA_ADAPTER_INTEGRATED;
}

void toPublic(const AdapterInfo& a, TesseraAdapterInfo& out) {
    out = tesseraDefaultAdapterInfo();
    out.flags = adapterClassOf(a);
    out.vendorId = a.vendorId;
    out.deviceId = a.deviceId;
    out.luid = a.luid;
    std::snprintf(out.id, sizeof out.id, "%s", adapterId(a.luid).c_str());
    std::snprintf(out.name, sizeof out.name, "%s", a.name.c_str());
}

const AdapterInfo* identifyServing(const std::vector<AdapterInfo>& all, const char* vendor,
                                   const char* renderer) {
    const std::string text = std::string(vendor ? vendor : "") + " " + (renderer ? renderer : "");
    const AdapterInfo* found = nullptr;
    for (const AdapterInfo& a : all) {
        if (!vendorOwns(a.vendorId, text))
            continue;
        if (found)
            return nullptr; // two adapters of that vendor: cannot be told apart
        found = &a;
    }
    return found;
}

std::string hybridAdvisory(const std::vector<AdapterInfo>& all, const char* vendor, const char* renderer) {
    const AdapterInfo* serving = identifyServing(all, vendor, renderer);
    if (!serving || serving->software)
        return std::string();
    const AdapterInfo* strongest = nullptr;
    for (const AdapterInfo& a : all)
        if (!a.software && (!strongest || a.dedicatedVideoMemory > strongest->dedicatedVideoMemory))
            strongest = &a;
    if (!strongest || strongest->vendorId == serving->vendorId ||
        strongest->dedicatedVideoMemory <= serving->dedicatedVideoMemory)
        return std::string();
    return "this process got the OpenGL driver of \"" + serving->name + "\" while \"" + strongest->name +
           "\" is available; to get the faster GPU the host executable can export NvOptimusEnablement = 1 "
           "(NVIDIA) and "
           "AmdPowerXpressRequestHighPerformance = 1 (AMD), or the user can set it under Settings > System > "
           "Display > Graphics";
}

std::vector<AdapterInfo> enumerateAdapters(std::string& error) {
    std::vector<AdapterInfo> all;
    IDXGIFactory1* factory = createFactory(error);
    if (!factory)
        return all;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
            break;
        all.push_back(describe(a));
        a->Release();
    }
    factory->Release();
    return all;
}

bool resolveAdapter(const TesseraContextConfig& c, AdapterInfo& out, std::string& error) {
    std::string why;
    const std::vector<AdapterInfo> all = enumerateAdapters(why);
    if (all.empty()) {
        error = why.empty() ? "no display adapters were found" : why;
        return false;
    }
    uint64_t wantedLuid = c.adapterLuid;
    if (c.adapter[0] != '\0' && !parseAdapterId(c.adapter, wantedLuid)) {
        error = std::string("'") + c.adapter + "' is not a luid: adapter id";
        return false;
    }
    if (wantedLuid != 0) {
        for (const AdapterInfo& a : all) {
            if (a.luid == wantedLuid) {
                out = a;
                return true;
            }
        }
        error = "no display adapter has the requested LUID";
        return false;
    }

    // Preference: let DXGI decide where it can (Windows 10 1803+), else fall back on dedicated memory.
    IDXGIFactory1* factory = createFactory(why);
    if (factory) {
        IDXGIFactory6* f6 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&f6))) &&
            f6) {
            const DXGI_GPU_PREFERENCE pref = c.gpuPreference == TESSERA_GPU_LOW_POWER
                                                 ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                                                 : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
            for (UINT i = 0;; ++i) {
                IDXGIAdapter1* a = nullptr;
                if (f6->EnumAdapterByGpuPreference(i, pref, __uuidof(IDXGIAdapter1),
                                                   reinterpret_cast<void**>(&a)) == DXGI_ERROR_NOT_FOUND)
                    break;
                const AdapterInfo info = describe(a);
                a->Release();
                if (!info.software) {
                    out = info;
                    f6->Release();
                    factory->Release();
                    return true;
                }
            }
            f6->Release();
        }
        factory->Release();
    }
    const AdapterInfo* best = nullptr;
    for (const AdapterInfo& a : all) {
        if (a.software)
            continue;
        if (!best ||
            (c.gpuPreference == TESSERA_GPU_LOW_POWER ? a.dedicatedVideoMemory < best->dedicatedVideoMemory
                                                      : a.dedicatedVideoMemory > best->dedicatedVideoMemory))
            best = &a;
    }
    if (!best) {
        error = "there is no hardware display adapter";
        return false;
    }
    out = *best;
    return true;
}

std::string verifyAdapter(const AdapterInfo& wanted, const std::vector<AdapterInfo>& all, const char* vendor,
                          const char* renderer) {
    const std::string text = std::string(vendor ? vendor : "") + " " + (renderer ? renderer : "");
    if (!vendorOwns(wanted.vendorId, text))
        return "the OpenGL driver in use (" + text + ") does not belong to the requested adapter \"" +
               wanted.name +
               "\"; Windows decides which GPU's OpenGL driver a process gets (Settings > System > Display > "
               "Graphics, or "
               "NvOptimusEnablement exported by the host executable)";
    int sameVendor = 0;
    for (const AdapterInfo& a : all)
        if (a.vendorId == wanted.vendorId && !a.software)
            ++sameVendor;
    if (sameVendor > 1 && !wanted.software)
        return "several adapters have the same vendor as \"" + wanted.name +
               "\", so it cannot be verified that the OpenGL driver in use is the requested one";
    return std::string();
}

} // namespace tessera::win32
