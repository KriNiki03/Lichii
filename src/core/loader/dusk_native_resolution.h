// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstring>
#include <optional>
#include <span>

#include "core/arm/dynarmic/dusk_ultrawide.h"

namespace Core::DuskUltrawide {

struct ResolutionInstruction {
    u32 rva;
    u32 original;
    u32 replacement;
};

constexpr u32 MoveW(u32 reg, u32 value) {
    return 0x52800000 | (value << 5) | reg;
}

constexpr u32 BranchForward(u32 from, u32 to) {
    return 0x14000000 | ((to - from) / 4);
}

inline constexpr std::array ResolutionInstructions{
    // Startup descriptor: backing extent and the aspect constraint on render resources.
    ResolutionInstruction{0x1c11fac, 0xb94002c8, MoveW(8, NativeWidth)},
    ResolutionInstruction{0x1c11fbc, 0xb94002e8, MoveW(8, NativeHeight)},
    ResolutionInstruction{0x1c1201c, 0xd2800208, 0xd2800568}, // MOV X8, #43
    ResolutionInstruction{0x1c12020, 0xf2c00128, 0xf2c00248}, // MOVK X8, #18, LSL #32
    // Graphics-scale notification (W22/W21), operation-mode change (W20/W19),
    // and the startup comparison (W10/W9).
    ResolutionInstruction{0x16c1d54, 0xb940054b, MoveW(21, NativeHeight)},
    ResolutionInstruction{0x16c1d58, 0x8b090a69, MoveW(22, NativeWidth)},
    ResolutionInstruction{0x16c1d5c, 0xb9402129, BranchForward(0x16c1d5c, 0x16c1dd4)},
    ResolutionInstruction{0x16c2040, 0xb940052a, MoveW(19, NativeHeight)},
    ResolutionInstruction{0x16c2044, 0xb9402108, MoveW(20, NativeWidth)},
    ResolutionInstruction{0x16c2048, 0x1b087d53, BranchForward(0x16c2048, 0x16c20c8)},
    ResolutionInstruction{0x1c124fc, 0xb9400569, MoveW(9, NativeHeight)},
    ResolutionInstruction{0x1c12500, 0x1b0a7d29, MoveW(10, NativeWidth)},
    ResolutionInstruction{0x1c12504, 0x7100099f, BranchForward(0x1c12504, 0x1c125bc)},
};

// Return the first rejected RVA. Validate the entire group before changing text.
inline std::optional<u32> ApplyNativeResolutionPatch(std::span<u8> text, u32 text_rva) {
    for (const auto& site : ResolutionInstructions) {
        if (site.rva < text_rva || u64{site.rva - text_rva} + sizeof(u32) > text.size()) {
            return site.rva;
        }
        u32 word;
        std::memcpy(&word, text.data() + (site.rva - text_rva), sizeof(word));
        if (word != site.original) {
            return site.rva;
        }
    }
    for (const auto& site : ResolutionInstructions) {
        std::memcpy(text.data() + (site.rva - text_rva), &site.replacement, sizeof(u32));
    }
    return std::nullopt;
}

} // namespace Core::DuskUltrawide
