// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include "video_core/surface.h"

namespace VideoCommon::DlssContract {

constexpr bool ValidColorFormats(VideoCore::Surface::PixelFormat input,
                                  VideoCore::Surface::PixelFormat output, bool use_history) {
    using VideoCore::Surface::PixelFormat;
    return (input == PixelFormat::B10G11R11_FLOAT && output == input) ||
           (!use_history && input == PixelFormat::A8B8G8R8_SRGB &&
            output == PixelFormat::R16G16B16A16_FLOAT);
}

constexpr std::uint64_t FastFusionProgramId = 0x040041E0212AE000ULL;
constexpr std::array FastFusionCandidates{0xb8ed9b1cd5c24e6dULL, 0x7f0f03c0529ca569ULL,
                                          0x7a06618494a29bcdULL};

constexpr bool IsFastFusionCandidate(std::uint64_t title, std::uint64_t shader) {
    if (title != FastFusionProgramId) {
        return false;
    }
    for (const auto candidate : FastFusionCandidates) {
        if (candidate == shader) {
            return true;
        }
    }
    return false;
}

constexpr bool FitsExtent(std::array<std::uint32_t, 2> requested,
                           std::array<std::uint32_t, 2> available) {
    return requested[0] <= available[0] && requested[1] <= available[1];
}

struct TemporalRegions {
    std::array<std::uint32_t, 2> render;
    std::array<std::uint32_t, 2> output;
    bool reset{};
};

constexpr bool ValidRegions(const TemporalRegions& regions,
                            std::array<std::uint32_t, 2> input_backing,
                            std::array<std::uint32_t, 2> output_backing) {
    return regions.render[0] >= 2 && regions.render[1] >= 2 &&
           FitsExtent(regions.render, input_backing) && FitsExtent(regions.render, regions.output) &&
           FitsExtent(regions.output, output_backing);
}

constexpr bool ValidDepthFormat(VideoCore::Surface::PixelFormat format, bool subrect) {
    using VideoCore::Surface::PixelFormat;
    return format == PixelFormat::D32_FLOAT ||
           (subrect && format == PixelFormat::D24_UNORM_S8_UINT);
}

inline bool ValidGeometry(std::array<std::uint32_t, 2> render,
                          std::array<std::uint32_t, 2> output,
                          std::array<float, 2> jitter, std::array<float, 2> motion_scale) {
    return render[0] >= 2 && render[1] >= 2 && FitsExtent(render, output) &&
           std::isfinite(jitter[0]) && std::isfinite(jitter[1]) &&
           std::isfinite(motion_scale[0]) && std::isfinite(motion_scale[1]);
}

} // namespace VideoCommon::DlssContract
