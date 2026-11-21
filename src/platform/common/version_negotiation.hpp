// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <tessera/tessera.h>

namespace tessera {

/*
 * Version negotiation (ROADMAP 12.2, ADR 0016), pure logic shared by every backend that creates contexts by
 * version (WGL, GLX, EGL). Three decisions live here so they are tested once, without a driver:
 *
 *   1. Which versions to try, newest first (versionLadder).
 *        - versionMajor/Minor is a MINIMUM, as the header always said: the ladder runs from the newest
 * version down to that floor, so a request for "3.3 core" is served by the newest core context the driver
 * offers.
 *        - TESSERA_FLAG_EXACT_VERSION turns the floor into exactly that version (what the library did before
 * 0.13.0).
 *        - OpenGL ES with no version walks 3.2, 3.1, 3.0, 2.0.
 *   2. In which order to try them (VersionMemory): the version that worked last time for the same request
 * comes first, so the second identical request makes one driver call instead of up to ten failing ones. A
 * remembered version is never trusted: if it fails now, the whole ladder runs as if nothing had been
 * remembered.
 *   3. What a driver's refusal means (classifyCreateError, create_errors.hpp).
 */

using GlVersion = std::pair<int, int>;

namespace detail {
inline bool atLeast(const GlVersion& v, const GlVersion& floor) {
    return v.first > floor.first || (v.first == floor.first && v.second >= floor.second);
}
} // namespace detail

/* The versions to try, newest first. {0,0} means "no version attributes: whatever the driver considers its
 * default". */
inline std::vector<GlVersion> versionLadder(const TesseraContextConfig& c) {
    const bool exact = (c.flags & TESSERA_FLAG_EXACT_VERSION) != 0;
    const GlVersion floor{c.versionMajor, c.versionMinor};

    if (c.api == TESSERA_API_OPENGL_ES) {
        static const GlVersion all[] = {{3, 2}, {3, 1}, {3, 0}, {2, 0}};
        if (c.versionMajor == 0)
            return exact ? std::vector<GlVersion>{{2, 0}}
                         : std::vector<GlVersion>(std::begin(all), std::end(all));
        if (exact)
            return {floor};
        std::vector<GlVersion> out;
        for (const GlVersion& v : all)
            if (detail::atLeast(v, floor))
                out.push_back(v);
        if (out.empty() ||
            out.back() != floor) // a floor that is not a known rung (ES 1.1, a future 3.3) is still asked for
            out.push_back(floor);
        return out;
    }

    // Desktop GL. Core and compatibility profiles start at 3.2; below that the floor itself is the only rung.
    static const GlVersion all[] = {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {4, 2}, {4, 1}, {4, 0}, {3, 3}, {3, 2}};
    if (c.versionMajor == 0) {
        if (c.profile != TESSERA_PROFILE_ANY)
            return std::vector<GlVersion>(std::begin(all), std::end(all));
        return {{0, 0}};
    }
    if (exact)
        return {floor};
    std::vector<GlVersion> out;
    for (const GlVersion& v : all)
        if (detail::atLeast(v, floor))
            out.push_back(v);
    if (out.empty() || out.back() != floor)
        out.push_back(floor);
    return out;
}

/* What identifies "the same request on the same driver" for the memory below. `scope` is the backend's own
 * notion of which display/adapter it is talking to (the EGLDisplay, the X display, the adapter), so a
 * different GPU is a different entry. */
inline std::string versionMemoryKey(const char* backend, std::uintptr_t scope,
                                    const TesseraContextConfig& c) {
    constexpr uint32_t kContextFlags = TESSERA_FLAG_DEBUG | TESSERA_FLAG_FORWARD_COMPATIBLE |
                                       TESSERA_FLAG_ROBUST_ACCESS | TESSERA_FLAG_RESET_NOTIFICATION |
                                       TESSERA_FLAG_NO_ERROR | TESSERA_FLAG_RELEASE_FLUSH |
                                       TESSERA_FLAG_EXACT_VERSION;
    return std::string(backend) + "|" + std::to_string(scope) + "|" + std::to_string(c.api) + "|" +
           std::to_string(c.profile) + "|" + std::to_string(c.versionMajor) + "." +
           std::to_string(c.versionMinor) + "|" + std::to_string(c.flags & kContextFlags) + "|" +
           std::to_string(c.priority) + "|" + std::to_string(c.adapterLuid) + "|" +
           std::to_string(c.gpuPreference);
}

class VersionMemory {
  public:
    static VersionMemory& instance() {
        static VersionMemory* const m = new VersionMemory(); // never destroyed: used until the process exits
        return *m;
    }

    /* `ladder` reordered for this request: the remembered version first (if it is one of the rungs), then the
     * others in their normal order. */
    std::vector<GlVersion> order(const std::string& key, const std::vector<GlVersion>& ladder) {
        GlVersion remembered{-1, -1};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const Entry& e : entries_)
                if (e.key == key)
                    remembered = e.version;
        }
        const auto it = std::find(ladder.begin(), ladder.end(), remembered);
        if (it == ladder.end() || it == ladder.begin())
            return ladder;
        std::vector<GlVersion> out{remembered};
        for (const GlVersion& v : ladder)
            if (v != remembered)
                out.push_back(v);
        return out;
    }

    void remember(const std::string& key, GlVersion version) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Entry& e : entries_) {
            if (e.key == key) {
                e.version = version;
                return;
            }
        }
        if (entries_.size() >= 256) // a bound, not a policy: requests are few in practice
            entries_.erase(entries_.begin());
        entries_.push_back({key, version});
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

    std::size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

  private:
    struct Entry {
        std::string key;
        GlVersion version;
    };
    std::mutex mutex_;
    std::vector<Entry> entries_;
};

/* Tries the ladder in the remembered order. `tryRung(version)` returns true when the driver created the
 * context. On success the version is remembered (only when there was a choice to remember). Returns false
 * when every rung failed. */
template <class TryRung>
bool negotiateVersion(const std::string& key, const std::vector<GlVersion>& ladder, TryRung&& tryRung,
                      GlVersion* chosen = nullptr) {
    VersionMemory& memory = VersionMemory::instance();
    for (const GlVersion& v : memory.order(key, ladder)) {
        if (tryRung(v)) {
            if (ladder.size() > 1)
                memory.remember(key, v);
            if (chosen)
                *chosen = v;
            return true;
        }
    }
    return false;
}

} // namespace tessera
