// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Tessera contributors
#pragma once

#include <tessera/tessera.h>

#include <cstdint>

namespace tessera {

/* GLX_EXT_context_priority and EGL_IMG_context_priority use the same attribute and values. */
constexpr int kNativeContextPriorityLevel = 0x3100; // *_CONTEXT_PRIORITY_LEVEL_{EXT,IMG}
constexpr int kNativePriorityHigh = 0x3101;
constexpr int kNativePriorityMedium = 0x3102;
constexpr int kNativePriorityLow = 0x3103;

/* TESSERA_PRIORITY_* to the native value; 0 for DEFAULT (which is never sent). */
inline int nativePriority(uint32_t priority) noexcept {
    switch (priority) {
    case TESSERA_PRIORITY_LOW:
        return kNativePriorityLow;
    case TESSERA_PRIORITY_MEDIUM:
        return kNativePriorityMedium;
    case TESSERA_PRIORITY_HIGH:
        return kNativePriorityHigh;
    default:
        return 0;
    }
}

/* What a driver read-back means; anything unexpected is DEFAULT (we do not guess). */
inline uint32_t priorityFromNative(int native) noexcept {
    switch (native) {
    case kNativePriorityLow:
        return TESSERA_PRIORITY_LOW;
    case kNativePriorityMedium:
        return TESSERA_PRIORITY_MEDIUM;
    case kNativePriorityHigh:
        return TESSERA_PRIORITY_HIGH;
    default:
        return TESSERA_PRIORITY_DEFAULT;
    }
}

} // namespace tessera
