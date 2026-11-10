// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace VideoCommon {

struct DlssModeRange {
    std::uint32_t mode{};
    std::array<std::uint32_t, 2> min{};
    std::array<std::uint32_t, 2> max{};
};

constexpr std::uint32_t SelectDlssMode(std::span<const DlssModeRange> modes,
                                       std::array<std::uint32_t, 2> render,
                                       std::uint32_t current, std::uint32_t preferred) {
    std::uint32_t first_fit{};
    bool preferred_fits = false;
    for (const auto& [mode, min, max] : modes) {
        if (!mode || render[0] < min[0] || render[1] < min[1] ||
            render[0] > max[0] || render[1] > max[1]) {
            continue;
        }
        if (mode == current) {
            return mode;
        }
        preferred_fits |= mode == preferred;
        if (!first_fit) {
            first_fit = mode;
        }
    }
    return preferred_fits ? preferred : first_fit;
}

enum class DlssEvaluationResult { Succeeded, OptionsFailed, TokenFailed, ConstantsFailed, EvaluateFailed };

template <typename Options, typename Token, typename Begin, typename Constants, typename Evaluate,
          typename Restore>
DlssEvaluationResult RecordDlssEvaluation(Options options, Token token, Begin begin,
                                         Constants constants, Evaluate evaluate, Restore restore) {
    if (!options()) {
        return DlssEvaluationResult::OptionsFailed;
    }
    if (!token()) {
        return DlssEvaluationResult::TokenFailed;
    }
    begin();
    const bool constants_ok = constants();
    const bool evaluated = constants_ok && evaluate();
    restore();
    return !constants_ok ? DlssEvaluationResult::ConstantsFailed
           : !evaluated ? DlssEvaluationResult::EvaluateFailed
                        : DlssEvaluationResult::Succeeded;
}

} // namespace VideoCommon
