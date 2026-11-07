// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/common_types.h"
#include "common/settings_enums.h"

namespace Tegra {

template <typename Timing>
[[nodiscard]] u64 ReadGpuTimestamp(const Timing& timing, bool ampere, Settings::GpuClock clock) {
    if (ampere) {
        // EDEN_NX2_GPU_TIME_DIVISOR=<n>: GPU timestamps run n times slower, so a game that sizes
        // its frame by GPU time sees a faster GPU.
        static const u64 time_divisor = [] {
            const char* const value = std::getenv("EDEN_NX2_GPU_TIME_DIVISOR");
            const u64 divisor = value ? std::strtoull(value, nullptr, 10) : 1;
            return divisor != 0 ? divisor : 1;
        }();
        return static_cast<u64>(timing.GetGlobalTimeNs().count()) / time_divisor;
    }
    const u64 ticks = timing.GetGPUTicks();
    switch (clock) {
    case Settings::GpuClock::Boost:
        return ticks / 256;
    case Settings::GpuClock::Overclock:
        return ticks / 512;
    default:
        return ticks;
    }
}

} // namespace Tegra
