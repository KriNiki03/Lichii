// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string_view>

#include "common/common_types.h"
#include "common/hex_util.h"

namespace Kernel {
class KProcess;
}

namespace Core::DuskCameraTrace {

inline constexpr u64 ProgramId = 0x0400D1D02399A000ULL;
inline constexpr auto BuildId = Common::HexStringToArray<32>(
    "14925e87e1acd4d5e9d071afeed230a1d4569f98000000000000000000000000");

inline bool Enabled() {
    static const bool enabled = [] {
        const char* cpu = std::getenv("EDEN_DUSK_CAMERA_CPU_TRACE");
        const char* marker = std::getenv("EDEN_DUSK_CAMERA_TRACE");
        return cpu && std::string_view{cpu} == "1" && marker && *marker;
    }();
    return enabled;
}

struct Site {
    u32 rva;
    u32 instruction;
    std::string_view name;
};

inline constexpr std::array Sites{
    Site{0x416abbc, 0xd503237f, "perspective-in"},  // PACIBSP
    Site{0x416ad5c, 0xa9444ff4, "perspective-out"}, // LDP X20, X19, [SP, #0x40]
    Site{0x416ba38, 0xd503237f, "descriptor-in"},
    Site{0x416bb28, 0xa9414ff4, "descriptor-out"}, // LDP X20, X19, [SP, #0x10]
    Site{0x453d2f8, 0xd503237f, "setup"},
    Site{0x44e638c, 0xd503237f, "volume-in"},
    Site{0x44e6e18, 0x910743ff, "volume-out"}, // ADD SP, SP, #0x1d0
    Site{0x44de26c, 0xd503237f, "primary-task-in"},
    Site{0x44de8b4, 0xaa1503e0, "primary-serialize-out"}, // MOV X0, X21
    Site{0x1ce2c60, 0xd503237f, "camera-rebuild-in"},
    Site{0x4566d20, 0xd503237f, "camera-dispatch-in"},
};

inline constexpr u32 BreakInstruction = 0xd4200000;
inline constexpr u32 PrimaryReturn = 0x44de6c0;
inline constexpr u32 ReceiverReturn = 0x44e31d4;
inline constexpr u64 CaptureDelay = 3;

constexpr const Site* FindSite(u64 base, u64 address, u32 instruction) {
    if (address < base) {
        return nullptr;
    }
    for (const auto& site : Sites) {
        if (address - base == site.rva && instruction == site.instruction) {
            return &site;
        }
    }
    return nullptr;
}

template <typename Jit, typename Memory>
void Resume(const Site& site, Jit& jit, Memory& memory) {
    switch (site.rva) {
    case 0x416ad5c:
    case 0x416bb28: {
        const u64 sp = jit.GetSP();
        const u64 offset = site.rva == 0x416ad5c ? 0x40 : 0x10;
        jit.SetRegister(20, memory.Read64(sp + offset));
        jit.SetRegister(19, memory.Read64(sp + offset + 8));
        break;
    }
    case 0x44e6e18:
        jit.SetSP(jit.GetSP() + 0x1d0);
        break;
    case 0x44de8b4:
        jit.SetRegister(0, jit.GetRegister(21));
        break;
    default:
        // Dynarmic treats PACIBSP as a hint. The exception already advanced PC.
        break;
    }
}

class Session {
public:
    Session(const Kernel::KProcess& process, u64 base_) : owner{&process}, base{base_} {}

    // Called once per Vulkan frame when the existing marker is configured.
    bool Tick(u64 frame, bool edge) {
        std::scoped_lock lock{mutex};
        if (edge) {
            capture_frame = frame + CaptureDelay;
            deadline = Clock::now() + std::chrono::seconds{1};
            active.store(true, std::memory_order_release);
        }
        counts.fill(0);
        if (capture_frame && frame > capture_frame) {
            active.store(false, std::memory_order_release);
            capture_frame = 0;
        }
        return frame == capture_frame;
    }

    bool IsActive() const {
        return active.load(std::memory_order_acquire);
    }

    u64 Take(const Site& site) {
        std::scoped_lock lock{mutex};
        if (!IsActive() || Clock::now() >= deadline) {
            active.store(false, std::memory_order_release);
            return 0;
        }
        auto& count = counts[&site - Sites.data()];
        if (!capture_frame || count >= 24) {
            return 0;
        }
        ++count;
        return capture_frame;
    }

    const Kernel::KProcess* const owner;
    const u64 base;

private:
    using Clock = std::chrono::steady_clock;
    std::mutex mutex;
    std::atomic<bool> active{};
    Clock::time_point deadline{};
    u64 capture_frame{};
    std::array<unsigned, Sites.size()> counts{};
};

inline std::atomic<std::shared_ptr<Session>> session;

} // namespace Core::DuskCameraTrace
