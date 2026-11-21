// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/hex_util.h"

namespace Kernel {
class KProcess;
}

namespace Core::DuskRuntimeTrace {

inline bool Enabled() {
    static const bool enabled = [] {
        const char* flag = std::getenv("EDEN_DUSK_RUNTIME_TRACE");
        const char* marker = std::getenv("EDEN_DUSK_CAMERA_TRACE");
        return flag && std::string_view{flag} == "1" && marker && *marker;
    }();
    return enabled;
}

struct Site {
    u32 rva;
    u32 instruction;
};

inline constexpr auto SdkBuildId = Common::HexStringToArray<32>(
    "18265daea2105b31f25346adfbbd09b5edf1207f000000000000000000000000");
inline constexpr std::array MainSites{
    Site{0x456bba8, 0x97fff9be}, // Worker call and outer frame release
    Site{0x456bbac, 0x913083ff},
    Site{0x4346a4c, 0x914007ff}, // First task frame release
    Site{0x42093a0, 0x394e8008}, // Dispatcher entry, guard passed, call, return, exit
    Site{0x4209404, 0xf9435678},
    Site{0x42094f4, 0xd63f0100},
    Site{0x42094f8, 0x3707fe59},
    Site{0x4209588, 0xd65f03c0},
    Site{0x43d1518, 0x94043691}, // First task's primary list publication
    Site{0x43e33a0, 0xd503237f}, // Replay entry and common epilogue
    Site{0x43e6010, 0x911c83ff},
};
inline constexpr std::array SdkSites{
    Site{0x5c41e0, 0xd503237f}, // AddCommandMemory
    Site{0x604604, 0xa9434ff4}, // DrawElementsBaseVertex after command writes
};

inline constexpr u32 IndexedMacroHeader = 0xa0050e04;
inline constexpr size_t PacketWords = 6;

struct Packet {
    u64 event{};
    u64 worker{};
    u64 header{};
    u64 buffer{};
    u64 gpu{};
    std::array<const u8*, 2> backing{};
    std::array<u32, PacketWords> words{};
};

struct Source {
    u64 event{};
    u64 worker{};
    u64 header{};
    u64 buffer{};
    u64 submission{};
    u64 gpu{};
    u32 entry{};
    s32 channel{};
    u64 space{};
};

struct SubmittedPacket {
    Packet packet;
    u32 entry{};
    u32 offset{};
};

inline thread_local Source current_source;

class SourceScope {
public:
    explicit SourceScope(Source source) : saved{std::exchange(current_source, source)} {}
    ~SourceScope() {
        current_source = saved;
    }

private:
    Source saved;
};

struct Worker {
    u64 tls{};
    u64 sp{};
    u64 id{};
    u64 dispatch_sp{};
    u64 task{};
    u64 target{};
};

struct List {
    u64 worker{};
    u64 payload{};
    u64 header{};
};

struct Replay {
    u64 tls{};
    u64 sp{};
    List list{};
};

struct Session {
    using Clock = std::chrono::steady_clock;
    static constexpr size_t MaxPackets = 8192;

    Session(const Kernel::KProcess* owner_, u64 base, u64 sdk)
        : owner{owner_}, main_base{base}, sdk_base{sdk} {}

    const Site* FindSite(u64 pc, u32 instruction) const {
        for (const auto& site : MainSites) {
            if (pc == main_base + site.rva && instruction == site.instruction) {
                return &site;
            }
        }
        for (const auto& site : SdkSites) {
            if (pc == sdk_base + site.rva && instruction == site.instruction) {
                return &site;
            }
        }
        return nullptr;
    }

    bool Active() const {
        return Clock::now().time_since_epoch().count() < deadline.load(std::memory_order_relaxed);
    }

    bool ObservingGpu() const {
        const auto until = deadline.load(std::memory_order_relaxed);
        return until && Clock::now().time_since_epoch() <
                            Clock::duration{until} + std::chrono::seconds{2};
    }

    void Arm() {
        std::scoped_lock lock{mutex};
        workers.clear();
        lists.clear();
        replays.clear();
        packets.clear();
        admitted_workers = 0;
        event_count = 0;
        draw_count = 0;
        emitted_count = 0;
        submitted_count = 0;
        summary_pending = true;
        last_world_frame = 0;
        last_tagged_frame = 0;
        last_tagged_world_frame = 0;
        overflow = false;
        deadline.store((Clock::now() + std::chrono::seconds{2}).time_since_epoch().count(),
                       std::memory_order_relaxed);
    }

    void Reuse(u64 begin, u64 size) {
        if (size == 0) {
            return;
        }
        constexpr u64 overlap = PacketWords * sizeof(u32) - 1;
        packets.erase(packets.lower_bound(begin > overlap ? begin - overlap : 0),
                      packets.lower_bound(begin + size));
    }

    const Kernel::KProcess* const owner;
    const u64 main_base;
    const u64 sdk_base;
    std::mutex mutex;
    std::atomic<Clock::duration::rep> deadline{};
    u64 sequence{};
    u64 submission{};
    u32 admitted_workers{};
    u32 event_count{};
    u32 draw_count{};
    u32 emitted_count{};
    u32 submitted_count{};
    u64 last_world_frame{};
    u64 last_tagged_frame{};
    u64 last_tagged_world_frame{};
    bool overflow{};
    bool summary_pending{};
    std::vector<Worker> workers;
    std::vector<List> lists;
    std::vector<Replay> replays;
    std::map<u64, Packet> packets;
};

inline std::atomic<std::shared_ptr<Session>> session;
inline u64 validated_main_base{};
inline const Kernel::KProcess* validated_main_owner{};

} // namespace Core::DuskRuntimeTrace
