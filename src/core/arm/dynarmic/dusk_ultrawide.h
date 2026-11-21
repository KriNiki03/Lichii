// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string_view>

#include "common/common_types.h"

namespace Kernel {
class KProcess;
}

namespace Core::DuskUltrawide {

inline constexpr u32 CopyReturn = 0x456b980;
inline constexpr u32 CopyReturnInstruction = 0x3942d268; // LDRB W8, [X19, #0xb4]
inline constexpr u32 AspectOffset = 0x6c;
inline constexpr u32 NativeWidth = 3440;
inline constexpr u32 NativeHeight = 1440;
inline constexpr u32 StockAspect = std::bit_cast<u32>(16.f / 9.f);
inline constexpr u32 WideAspect = std::bit_cast<u32>(43.f / 18.f);
inline constexpr u64 Unmapped = ~u64{0};

struct Instruction {
    u32 rva;
    u32 word;
};

inline constexpr Instruction OptionalReturn{0x43a9f14, 0xd65f03c0}; // RET
inline constexpr Instruction AfterOptional{0x456bb1c, 0x394bb728}; // LDRB W8, [X25, #0x2ed]
inline constexpr std::array Hooks{OptionalReturn, AfterOptional};

constexpr const Instruction* FindHook(u64 base, u64 pc) {
    for (const auto& site : Hooks) {
        if (pc == base + site.rva) {
            return &site;
        }
    }
    return nullptr;
}

inline constexpr std::array Instructions{
    Instruction{0x456cb50, 0x97fffb5c}, // Caller of the worker
    Instruction{0x456b8c4, 0xa9ba7bfd}, // Save frame and caller return
    Instruction{0x456b8e0, 0xd13083ff}, // Allocate worker frame
    Instruction{0x456b94c, 0xd10383a0}, // Address the local camera
    Instruction{0x456b97c, 0x940ef1e5}, // Copy the camera
    Instruction{CopyReturn, CopyReturnInstruction},
    Instruction{0x456baf8, 0x394bb328}, // Optional consumer enable flag
    Instruction{0x456bafc, 0x34000108}, // Skip the call when disabled
    Instruction{0x456bb00, 0xf9400320}, // Optional renderer context
    Instruction{0x456bb04, 0xb40000c0}, // Skip the call without a context
    Instruction{0x456bb08, 0xd10383a3}, // Pass the local camera in X3
    Instruction{0x456bb18, 0x97f8f8f3}, // Call optional consumer
    Instruction{0x43a9ee4, 0x39480008}, // Optional context enable byte
    Instruction{0x43a9ee8, 0x34000168}, // Return immediately if disabled
    Instruction{0x43a9f08, 0x34000069}, // Return if graphics feature is disabled
    Instruction{0x43a9f10, 0x34000048}, // Enter active body or fall through to RET
    OptionalReturn,
    Instruction{0x43aa600, 0xa8c67bfd}, // Restore caller return after active body
    Instruction{0x43aa604, 0xd65f0fff}, // RETAB from active body
    AfterOptional,
    Instruction{0x456bb54, 0x97ff4682}, // Construct primary task
    Instruction{0x456bb98, 0xb9000be9}, // Store nested task selector (0x21)
    Instruction{0x456bba8, 0x97fff9be}, // Run nested tasks
};

inline bool NativeResolutionRequested() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_DUSK_NATIVE_ULTRAWIDE");
        return value && std::string_view{value} == "1";
    }();
    return enabled;
}

inline bool CameraRequested() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_DUSK_ULTRAWIDE");
        return value && std::string_view{value} == "1";
    }();
    return enabled;
}

inline bool Enabled() {
    return NativeResolutionRequested() || CameraRequested();
}

enum class Event : u8 {
    Fetch, Execute, Debugger, FetchOpcode, ExecuteOpcode, OriginalLoad, FrameSize, FrameAlignment,
    StackPointer, CopyDestination, WorkerCaller, HandoffVtable, HandoffCopy, HandoffReceiver,
    Request, RenderState, CopySource, Companion, SourceOverlap, RequestState, RequestIndex,
    OptionalState, OptionalConsumer, FieldOfView, NearClip, FarClip,
    Width, Height, Aspect, WriteReadback, Applied,
    WorldGeometry, WideReconstructionGeometry, PresentGeometry, WidePresentGeometry,
};
static_assert(static_cast<u32>(Event::WidePresentGeometry) < 64);

struct Session {
    const Kernel::KProcess* owner;
    u64 base;
    bool native_resolution{};
    std::atomic<u64> events{};

    bool First(Event event) {
        const u64 bit = u64{1} << static_cast<u32>(event);
        if (events.load(std::memory_order_relaxed) & bit) {
            return false;
        }
        return (events.fetch_or(bit, std::memory_order_relaxed) & bit) == 0;
    }

    bool Seen(Event event) const {
        return (events.load(std::memory_order_relaxed) & (u64{1} << static_cast<u32>(event))) != 0;
    }
};
inline std::atomic<std::shared_ptr<Session>> session;

struct IgnoreRejection {
    void operator()(Event, std::string_view, u64, std::string_view, u64) const {}
};

template <typename Read, typename Reject = IgnoreRejection>
u64 CameraToOverride(u64 base, const Instruction& site, u64 sp, u64 fp, u64 lr,
                     u64 camera_arg, u64 render_state, u64 handoff, u32 request_index,
                     u64 companion, u64 optional_state, Read read, Reject reject = {},
                     bool native_resolution = false) {
    constexpr u64 FrameSize = 0xc20;
    constexpr u64 CameraSize = 0xd0;
    const auto require = [&](bool valid, Event event, std::string_view field, u64 observed,
                             std::string_view comparison, u64 expected) {
        if (!valid) {
            reject(event, field, observed, comparison, expected);
        }
        return valid;
    };
    const auto equal = [&](Event event, std::string_view field, u64 observed, u64 expected) {
        return require(observed == expected && observed != Unmapped, event, field, observed,
                       "==", expected);
    };
    const auto pointer = [&](Event event, std::string_view field, u64 observed) {
        return require(observed != 0 && observed != Unmapped, event, field, observed, "!=", 0);
    };
    const bool early_return = site.rva == OptionalReturn.rva;
    if (early_return && lr != base + AfterOptional.rva) {
        return 0;
    }
    if (!require(fp >= FrameSize, Event::FrameSize, "x29", fp, ">=", FrameSize) ||
        !equal(Event::FrameAlignment, "x29 & 0xf", fp & 0xf, 0) ||
        !equal(Event::StackPointer, "sp", sp, fp - FrameSize) ||
        (early_return && !equal(Event::CopyDestination, "optional camera x3", camera_arg, fp - 0xe0)) ||
        !equal(Event::WorkerCaller, "saved worker return", read(fp + 8, 8), base + 0x456cb54) ||
        !equal(Event::HandoffVtable, "handoff vtable", read(handoff, 8), base + 0x59c3698) ||
        !equal(Event::HandoffCopy, "handoff copy slot", read(base + 0x59c3698, 8),
               base + 0x45649a0) ||
        !equal(Event::HandoffReceiver, "handoff receiver slot", read(base + 0x59c36a8, 8),
               base + 0x45651d8)) {
        return 0;
    }

    const u64 request = read(handoff + 0x10, 8);
    if (!pointer(Event::Request, "worker request", request) ||
        !pointer(Event::RenderState, "render state x19", render_state) ||
        !equal(Event::OptionalState, "optional state x25", optional_state, render_state + 0xc8b0)) {
        return 0;
    }
    const u64 source = read(request + 8, 8);
    if (!pointer(Event::CopySource, "request camera source", source) ||
        !equal(Event::Companion, "companion x23", companion,
               source <= Unmapped - CameraSize ? source + CameraSize : Unmapped) ||
        !require(source >= fp || companion <= sp, Event::SourceOverlap, "request camera source",
                 source, "outside worker frame ending at", fp) ||
        !equal(Event::RequestState, "request render state", read(request, 8), render_state) ||
        !equal(Event::RequestIndex, "request index", read(request + 0x10, 4), request_index)) {
        return 0;
    }

    const u64 camera = fp - 0xe0;
    const auto field = [&](u64 offset) { return read(camera + offset, 4); };
    // The active body returns with LR pointing to this join. The leaf RET can modify the
    // camera before joining because its three early-exit paths never consume X3.
    if (!early_return && lr == base + AfterOptional.rva) {
        if (field(AspectOffset) != WideAspect) {
            reject(Event::OptionalConsumer, "worker join LR without an early-return override",
                   lr, "!=", base + AfterOptional.rva);
        }
        return 0;
    }

    const auto fov_word = field(0x50);
    const auto near_word = field(0x54);
    const auto far_word = field(0x58);
    const float vertical_fov = std::bit_cast<float>(u32(fov_word));
    const float near_clip = std::bit_cast<float>(u32(near_word));
    const float far_clip = std::bit_cast<float>(u32(far_word));
    const auto width = field(0x5c);
    const auto height = field(0x60);
    const bool wide_camera = native_resolution && width == NativeWidth && height == NativeHeight;
    if (!require(vertical_fov > 0.f && vertical_fov < 3.141593f, Event::FieldOfView,
                 "local vertical FOV bits", fov_word, "positive and <", std::bit_cast<u32>(3.141593f)) ||
        !require(near_clip > 0.f, Event::NearClip, "local near plane bits", near_word, ">", 0) ||
        !require(far_clip > near_clip && std::isfinite(far_clip), Event::FarClip,
                 "local far plane bits", far_word, "finite and > near plane", near_word) ||
        (!wide_camera && (!equal(Event::Width, "local camera width", width, 1920) ||
                          !equal(Event::Height, "local camera height", height, 1080))) ||
        !equal(Event::Aspect, "local aspect bits at +0x6c", field(AspectOffset), StockAspect)) {
        return 0;
    }
    return camera;
}

} // namespace Core::DuskUltrawide
