// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace tessera {

/* What the EGL backend learns about one EGL device (eglQueryDevicesEXT order). Empty / false = not exposed.
 */
struct EglDeviceDesc {
    std::string renderNode;                 // EGL_DRM_RENDER_NODE_FILE_EXT, e.g. /dev/dri/renderD128
    bool software = false;                  // EGL_MESA_device_software: a CPU renderer (llvmpipe, softpipe)
    std::string name;                       // EGL_RENDERER_EXT (EGL_EXT_device_query_name), "" if not exposed
    bool hasUuid = false;                   // EGL_EXT_device_persistent_id
    std::array<std::uint8_t, 16> uuid = {}; // EGL_DEVICE_UUID_EXT
};

enum class DeviceSelectorKind { Empty, Index, RenderNode, Pci, Uuid, Software, Invalid };

/* Result of resolving TESSERA_EGL_DEVICE against a device list. `index` is >= 0 on success. */
struct DeviceChoice {
    int index = -1;
    std::string reason; // why it failed; empty on success
};

namespace detail {
inline int hexValue(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
inline std::string lower(std::string s) {
    for (char& ch : s)
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    return s;
}
inline bool startsWith(const std::string& s, const char* prefix) {
    const std::string p(prefix);
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
} // namespace detail

/* A UUID as 32 hex digits, or in the 8-4-4-4-12 form, optionally behind "uuid:". False for anything else. */
inline bool parseDeviceUuid(const std::string& text, std::array<std::uint8_t, 16>& out) {
    std::string s = detail::startsWith(text, "uuid:") ? text.substr(5) : text;
    std::string digits;
    if (s.size() == 36) {
        const std::size_t dashes[] = {8, 13, 18, 23};
        for (std::size_t i = 0; i < s.size(); ++i) {
            const bool dash = i == dashes[0] || i == dashes[1] || i == dashes[2] || i == dashes[3];
            if (dash) {
                if (s[i] != '-')
                    return false;
            } else {
                digits.push_back(s[i]);
            }
        }
    } else if (s.size() == 32) {
        digits = s;
    } else {
        return false;
    }
    for (std::size_t i = 0; i < 16; ++i) {
        const int hi = detail::hexValue(digits[2 * i]), lo = detail::hexValue(digits[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return true;
}

/* domain:bus:device.function, e.g. 0000:01:00.0 (hex digits, any case). */
inline bool looksLikePciAddress(const std::string& s) {
    if (s.size() < 12 || s[4] != ':' || s[7] != ':' || s[10] != '.')
        return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 4 || i == 7 || i == 10)
            continue;
        if (detail::hexValue(s[i]) < 0)
            return false;
    }
    return true;
}

inline DeviceSelectorKind classifyDeviceSelector(const std::string& s) {
    if (s.empty())
        return DeviceSelectorKind::Empty;
    if (detail::lower(s) == "software")
        return DeviceSelectorKind::Software;
    std::array<std::uint8_t, 16> ignore;
    if (parseDeviceUuid(s, ignore)) // before Index: a 32-digit UUID can consist of decimal digits only
        return DeviceSelectorKind::Uuid;
    if (s.find_first_not_of("0123456789") == std::string::npos)
        return DeviceSelectorKind::Index;
    if (looksLikePciAddress(s))
        return DeviceSelectorKind::Pci;
    if (s[0] == '/')
        return DeviceSelectorKind::RenderNode;
    return DeviceSelectorKind::Invalid;
}

/*
 * Resolves `selector` (an index, a render node path, a PCI address or a UUID) to a device. `canonical` turns
 * a path into its real path (realpath on POSIX; the tests pass a fake), so /dev/dri/by-path links and
 * renderD128 compare equal. A choice that matches nothing, or more than one device (two devices cannot share
 * a render node or a UUID, so that means a broken driver), fails with a reason: an explicit choice is never
 * replaced by another GPU.
 */
inline DeviceChoice selectDevice(const std::string& selector, const std::vector<EglDeviceDesc>& devices,
                                 const std::function<std::string(const std::string&)>& canonical) {
    DeviceChoice r;
    const int count = static_cast<int>(devices.size());
    auto fail = [&](std::string why) {
        r.index = -1;
        r.reason = std::move(why);
        return r;
    };
    switch (classifyDeviceSelector(selector)) {
    case DeviceSelectorKind::Empty:
        return fail("no device selector given");
    case DeviceSelectorKind::Index: {
        if (selector.size() > 4)
            return fail("device index " + selector + " is out of range (" + std::to_string(count) +
                        " device(s))");
        const int i = std::atoi(selector.c_str());
        if (i >= count)
            return fail("device index " + selector + " is out of range (" + std::to_string(count) +
                        " device(s))");
        r.index = i;
        return r;
    }
    case DeviceSelectorKind::Software: {
        for (int i = 0; i < count; ++i) {
            if (devices[static_cast<std::size_t>(i)]
                    .software) { // the first one: every software device does the same job
                r.index = i;
                return r;
            }
        }
        return fail("no EGL device is a software renderer (EGL_MESA_device_software)");
    }
    case DeviceSelectorKind::Uuid: {
        std::array<std::uint8_t, 16> want;
        parseDeviceUuid(selector, want);
        int found = -1, matches = 0, withUuid = 0;
        for (int i = 0; i < count; ++i) {
            if (!devices[static_cast<std::size_t>(i)].hasUuid)
                continue;
            ++withUuid;
            if (devices[static_cast<std::size_t>(i)].uuid == want) {
                found = i;
                ++matches;
            }
        }
        if (withUuid == 0)
            return fail("no EGL device reports a UUID (EGL_EXT_device_persistent_id is not available)");
        if (matches == 0)
            return fail("no EGL device has UUID " + selector);
        if (matches > 1)
            return fail("UUID " + selector + " matches " + std::to_string(matches) + " EGL devices");
        r.index = found;
        return r;
    }
    case DeviceSelectorKind::Pci:
    case DeviceSelectorKind::RenderNode: {
        std::string node = selector;
        if (classifyDeviceSelector(selector) == DeviceSelectorKind::Pci)
            node = "/dev/dri/by-path/pci-" + detail::lower(selector) + "-render";
        const std::string wanted = canonical(node);
        int found = -1, matches = 0, withNode = 0;
        for (int i = 0; i < count; ++i) {
            const std::string& file = devices[static_cast<std::size_t>(i)].renderNode;
            if (file.empty())
                continue;
            ++withNode;
            if (canonical(file) == wanted) {
                found = i;
                ++matches;
            }
        }
        if (withNode == 0)
            return fail(
                "no EGL device reports a DRM render node (EGL_EXT_device_drm_render_node is not available)");
        if (matches == 0)
            return fail("no EGL device has render node " + wanted);
        if (matches > 1)
            return fail("render node " + wanted + " matches " + std::to_string(matches) + " EGL devices");
        r.index = found;
        return r;
    }
    case DeviceSelectorKind::Invalid:
        break;
    }
    return fail(
        "'" + selector +
        "' is not an index, a /dev/dri render node, a PCI address (0000:01:00.0), a UUID or 'software'");
}

/* The id tesseraEnumerateAdapters gives an EGL device (and TesseraContextConfig::adapter takes back): the
 * UUID if the device has one, else its DRM render node, else its position (which is not stable across boots,
 * so it is the last resort). */
inline std::string eglAdapterId(const EglDeviceDesc& d, int index) {
    static const char* const hex = "0123456789abcdef";
    if (d.hasUuid) {
        std::string s = "uuid:";
        for (std::size_t i = 0; i < d.uuid.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                s.push_back('-');
            s.push_back(hex[d.uuid[i] >> 4]);
            s.push_back(hex[d.uuid[i] & 15]);
        }
        return s;
    }
    if (!d.renderNode.empty() && d.renderNode.size() + 5 < 48)
        return "node:" + d.renderNode;
    return "index:" + std::to_string(index);
}

/* An adapter id of the EGL kinds as a TESSERA_EGL_DEVICE selector ("uuid:..." and "pci:..." and "node:..."
 * and "index:N" become what selectDevice understands). False for anything else ("luid:", "cgl:", garbage). */
inline bool eglSelectorFromAdapterId(const std::string& id, std::string& selector) {
    if (detail::startsWith(id, "uuid:") && id.size() > 5) {
        selector = id; // selectDevice takes the prefix
        return classifyDeviceSelector(selector) == DeviceSelectorKind::Uuid;
    }
    if (detail::startsWith(id, "node:") && id.size() > 5) {
        selector = id.substr(5);
        return classifyDeviceSelector(selector) == DeviceSelectorKind::RenderNode;
    }
    if (detail::startsWith(id, "pci:") && id.size() > 4) {
        selector = id.substr(4);
        return classifyDeviceSelector(selector) == DeviceSelectorKind::Pci;
    }
    if (detail::startsWith(id, "index:") && id.size() > 6) {
        selector = id.substr(6);
        return classifyDeviceSelector(selector) == DeviceSelectorKind::Index;
    }
    return false;
}

} // namespace tessera
