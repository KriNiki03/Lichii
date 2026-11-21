// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdlib>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif
#include "common/settings.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_64.h"
#include "core/arm/dynarmic/dusk_camera_trace.h"
#include "core/arm/dynarmic/dusk_runtime_trace.h"
#include "core/arm/dynarmic/dusk_ultrawide.h"
#include "core/arm/dynarmic/dynarmic_exclusive_monitor.h"
#include "core/arm/dynarmic/window_trace.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "dynarmic/interface/A64/config.h"

namespace Core {

using namespace Common::Literals;

namespace {

void ReportDuskCameraRejection(DuskUltrawide::Session& session, DuskUltrawide::Event event,
                               std::string_view field, u64 observed, std::string_view comparison,
                               u64 expected) {
    if (session.First(event)) {
        LOG_INFO(Core_ARM, "Dusk ultrawide camera rejected: {} observed={:#x}, expected {} {:#x}{}",
                 field, observed, comparison, expected,
                 observed == DuskUltrawide::Unmapped ? " (unmapped)" : "");
    }
}

template <typename T>
T ReadDuskTrace(Memory::Memory& memory, u64 address) {
    T value{};
    if (memory.IsValidVirtualAddressRange(address, sizeof(value))) {
        memory.ReadBlockUnsafe(address, &value, sizeof(value));
    }
    return value;
}

void ObserveDuskRuntime(DuskRuntimeTrace::Session& trace, u32 rva, Dynarmic::A64::Jit& jit,
                        Memory::Memory& memory, u64 tls) {
    using namespace DuskRuntimeTrace;
    std::scoped_lock lock{trace.mutex};
    if (!trace.Active()) {
        return;
    }
    const auto reg = [&](size_t index) { return jit.GetRegister(index); };
    const auto read = [&](u64 address) { return ReadDuskTrace<u64>(memory, address); };
    const u64 sp = jit.GetSP();
    const u64 lr = reg(30);
    const auto worker = std::ranges::find(trace.workers, tls, &Worker::tls);
    const auto replay = std::ranges::find(trace.replays, tls, &Replay::tls);
    const auto event = [&] {
        if (trace.event_count >= 768) {
            if (!std::exchange(trace.overflow, true)) {
                LOG_INFO(Core_ARM, "Dusk runtime trace limit reached");
            }
            return u64{0};
        }
        ++trace.event_count;
        return ++trace.sequence;
    };
    switch (rva) {
    case 0x456bba8:
        if (worker == trace.workers.end() && trace.admitted_workers < 32) {
            ++trace.admitted_workers;
            trace.workers.push_back({tls, sp, ++trace.sequence});
        }
        return;
    case 0x43d1518: {
        const u64 payload = reg(2);
        std::erase_if(trace.lists, [&](const List& list) { return list.payload == payload; });
        if (worker != trace.workers.end() && read(reg(29) + 8) == trace.main_base + 0x433fb04) {
            if (const u64 seq = event()) {
                const u64 header = read(payload + 0x40);
                worker->task = reg(19);
                trace.lists.push_back({worker->id, payload, header});
                LOG_INFO(Core_ARM,
                         "Dusk runtime seq={} list worker={} task={:#x} payload={:#x} header={:#x} count={}",
                         seq, worker->id, worker->task, payload, header,
                         header ? ReadDuskTrace<u32>(memory, header + 8) : 0);
            }
        }
        return;
    }
    case 0x43e33a0: {
        const u64 payload = reg(1);
        const auto list = std::ranges::find(trace.lists, payload, &List::payload);
        if (replay == trace.replays.end() && list != trace.lists.end() && list->header &&
            read(payload + 0x40) == list->header && trace.replays.size() < 32) {
            trace.replays.push_back({tls, sp, *list});
        }
        return;
    }
    case 0x43e6010:
        if (replay != trace.replays.end() && sp + 0x7c0 == replay->sp) {
            trace.replays.erase(replay);
        }
        return;
    case 0x5c41e0: {
        // AddCommandMemory replaces a pool slice even when its address and contents recur.
        const u64 begin = read(reg(1) + 0x18) + reg(2);
        const u64 size = reg(3);
        if (size <= ~u64{0} - begin) {
            trace.Reuse(begin, size);
        }
        return;
    }
    case 0x604604: {
        if (trace.packets.empty() && replay == trace.replays.end()) {
            return;
        }
        const u64 buffer = reg(19);
        const u64 cpu = reg(1) + 0x10;
        const u64 cpu_base = read(buffer + 0x38);
        const u64 gpu_base = read(buffer + 0x60);
        if (!cpu_base || !gpu_base || cpu < cpu_base || cpu - cpu_base >= 0x800000) {
            return;
        }
        const u64 gpu = gpu_base + (cpu - cpu_base);
        trace.Reuse(gpu - 0x10, 0x30);
        if (replay == trace.replays.end() || lr != trace.main_base + 0x43d473c) {
            return;
        }
        if (trace.emitted_count >= Session::MaxPackets) {
            if (!std::exchange(trace.overflow, true)) {
                LOG_INFO(Core_ARM, "Dusk runtime trace packet limit reached");
            }
            return;
        }
        const auto words = ReadDuskTrace<std::array<u32, PacketWords>>(memory, cpu);
        const std::array<const u8*, 2> backing{memory.GetPointerSilent(cpu),
                                              memory.GetPointerSilent(cpu + sizeof(words) - 1)};
        if (words.front() == IndexedMacroHeader && backing.front() && backing.back()) {
            trace.packets.emplace(gpu, Packet{++trace.sequence, replay->list.worker,
                                              replay->list.header, buffer, gpu, backing, words});
            ++trace.emitted_count;
        }
        return;
    }
    default:
        break;
    }
    if (worker == trace.workers.end()) {
        return;
    }
    if (rva == 0x42093a0 &&
        (lr == trace.main_base + 0x433e15c || lr == trace.main_base + 0x4343d78)) {
        worker->dispatch_sp = sp;
        worker->task = reg(1);
        if (const u64 seq = event()) {
            const u64 owner = reg(0);
            const u64 begin = read(owner + 0x70);
            const u64 end = read(owner + 0x78);
            LOG_INFO(Core_ARM,
                     "Dusk runtime seq={} dispatch worker={} return_rva={:#x} owner={:#x} task={:#x} "
                     "primary={:#x} alternate={:#x} guard_flags={:#x} receivers={}",
                     seq, worker->id, lr - trace.main_base, owner, worker->task,
                     read(worker->task + 0x448), read(worker->task + 0x748),
                     ReadDuskTrace<u16>(memory, owner + 0x3a0), end >= begin ? (end - begin) / 8 : 0);
        }
    } else if (worker->dispatch_sp && sp + 0x50 == worker->dispatch_sp) {
        if (rva == 0x4209404) {
            if (const u64 seq = event()) {
                LOG_INFO(Core_ARM, "Dusk runtime seq={} guards-passed worker={}", seq, worker->id);
            }
        } else if (rva == 0x42094f4) {
            worker->target = reg(8);
            if (const u64 seq = event()) {
                const u64 task = reg(1);
                LOG_INFO(Core_ARM,
                         "Dusk runtime seq={} receiver-enter worker={} index={} object={:#x} "
                         "vtable={:#x} target={:#x} task={:#x} task_vtable={:#x} "
                         "volume_vtable={:#x} task_73c={:#x} scene={:#x}",
                         seq, worker->id, reg(4) - 8, reg(0), read(reg(0)), reg(8), task,
                         read(task), read(task + 0x120), ReadDuskTrace<u32>(memory, task + 0x73c),
                         reg(3));
            }
        } else if (rva == 0x42094f8) {
            if (const u64 seq = event()) {
                LOG_INFO(Core_ARM,
                         "Dusk runtime seq={} receiver-return worker={} index={} target={:#x}",
                         seq, worker->id, reg(22) - 8, worker->target);
            }
        }
    } else if (rva == 0x4209588 && sp == worker->dispatch_sp) {
        worker->dispatch_sp = 0;
        if (const u64 seq = event()) {
            LOG_INFO(Core_ARM, "Dusk runtime seq={} dispatcher-return worker={}", seq, worker->id);
        }
    } else if (rva == 0x4346a4c && worker->task == sp + 0xc00) {
        if (const u64 seq = event()) {
            LOG_INFO(Core_ARM, "Dusk runtime seq={} task-frame-release worker={} task={:#x}",
                     seq, worker->id, worker->task);
        }
    } else if (rva == 0x456bbac && sp == worker->sp) {
        if (worker->task) {
            if (const u64 seq = event()) {
                LOG_INFO(Core_ARM, "Dusk runtime seq={} worker-frame-release worker={}",
                         seq, worker->id);
            }
        }
        trace.workers.erase(worker);
    }
}

void ResumeDuskRuntime(u32 rva, u64 pc, Dynarmic::A64::Jit& jit, Memory::Memory& memory) {
    const u64 sp = jit.GetSP();
    const auto call = [&](u64 target) {
        jit.SetRegister(30, pc + 4);
        jit.SetPC(target);
    };
    switch (rva) {
    case 0x456bba8:
        call(pc - 0x1908);
        break;
    case 0x456bbac:
        jit.SetSP(sp + 0xc20);
        break;
    case 0x4346a4c:
        jit.SetSP(sp + 0x1000);
        break;
    case 0x42093a0:
        jit.SetRegister(8, memory.Read8(jit.GetRegister(0) + 0x3a0));
        break;
    case 0x4209404:
        jit.SetRegister(24, memory.Read64(jit.GetRegister(19) + 0x6a8));
        break;
    case 0x42094f4:
        call(jit.GetRegister(8));
        break;
    case 0x42094f8:
        if (jit.GetRegister(25) & 1) {
            jit.SetPC(pc - 0x38);
        }
        break;
    case 0x4209588:
        jit.SetPC(jit.GetRegister(30));
        break;
    case 0x43d1518:
        call(pc + 0x10da44);
        break;
    case 0x43e6010:
        jit.SetSP(sp + 0x720);
        break;
    case 0x604604:
        jit.SetRegister(20, memory.Read64(sp + 0x30));
        jit.SetRegister(19, memory.Read64(sp + 0x38));
        break;
    default: // PACIBSP is a hint in Dynarmic.
        break;
    }
}

template <size_t Size>
std::string CameraWords(Memory::Memory& memory, u64 address) {
    std::array<u32, Size> words;
    if (!memory.IsValidVirtualAddressRange(address, sizeof(words)) ||
        !memory.ReadBlock(address, words.data(), sizeof(words))) {
        return "unmapped";
    }
    return fmt::format("{:08x}", fmt::join(words, " "));
}

void TraceDuskCamera(DuskCameraTrace::Session& trace, const DuskCameraTrace::Site& site,
                     Dynarmic::A64::Jit& jit, Memory::Memory& memory, u64 tls) {
    const u64 rva = site.rva;
    const u64 x0 = jit.GetRegister(0);
    const u64 x1 = jit.GetRegister(1);
    const u64 x19 = jit.GetRegister(19);
    const u64 x20 = jit.GetRegister(20);
    const bool saved_lr = rva == 0x416bb28 || rva == 0x44e6e18 || rva == 0x44de8b4;
    const u64 lr = saved_lr ? memory.Read64(jit.GetRegister(29) + 8) : jit.GetRegister(30);
    const u64 caller = lr >= trace.base ? lr - trace.base : 0;
    const bool volume = rva == 0x44e638c || rva == 0x44e6e18;
    if (volume && caller != DuskCameraTrace::PrimaryReturn &&
        caller != DuskCameraTrace::ReceiverReturn) {
        return;
    }
    if ((rva == 0x44de26c && caller != 0x453d594 && caller != 0x453d7bc) ||
        (rva == 0x44de8b4 && (caller != 0x453ddc8 || !jit.GetRegister(21))) ||
        (rva == 0x1ce2c60 && caller != 0x1cdd2bc)) {
        return;
    }
    const u64 frame = trace.Take(site);
    if (!frame) {
        return;
    }

    std::string data;
    switch (rva) {
    case 0x416abbc:
        data = fmt::format("descriptor={:#x} out={:#x} working_50={}", x0, jit.GetRegister(8),
                           CameraWords<20>(memory, x0 + 0x50));
        break;
    case 0x416ad5c:
        data = fmt::format("descriptor={:#x} out={:#x} projection={}", x20, x19,
                           CameraWords<16>(memory, x19));
        break;
    case 0x416ba38:
        data = fmt::format("source={:#x} descriptor={:#x} source_50={}", x1, x0,
                           CameraWords<12>(memory, x1 + 0x50));
        break;
    case 0x416bb28:
        data = fmt::format("source={:#x} descriptor={:#x} working_50={}", x20, x19,
                           CameraWords<20>(memory, x19 + 0x50));
        break;
    case 0x453d2f8: {
        const u64 source = memory.IsValidVirtualAddressRange(x0 + 8, sizeof(u64))
                               ? memory.Read64(x0 + 8)
                               : 0;
        data = fmt::format("packet={:#x} source={:#x} camera={:#x}", x0, source, x1);
        break;
    }
    case 0x44e638c: {
        const u64 object = x0 - 0x120;
        data = fmt::format("volume={:#x} descriptor={:#x} object={:#x} working_50={} "
                           "view={} projection={} view_projection={}",
                           x0, x1, object, CameraWords<20>(memory, x1 + 0x50),
                           CameraWords<16>(memory, object + 0x10),
                           CameraWords<16>(memory, object + 0x50),
                           CameraWords<16>(memory, object + 0x90));
        break;
    }
    case 0x44e6e18:
        data = fmt::format("volume={:#x} effective_aspect={:08x} planes={}", x19,
                           memory.Read32(x19 + 0x14c), CameraWords<24>(memory, x19 + 0x160));
        break;
    case 0x44de26c: {
        const u64 camera = jit.GetRegister(4);
        const u64 initializer_fp = jit.GetRegister(29);
        const bool valid_frame = memory.IsValidVirtualAddressRange(initializer_fp, 16);
        const u64 initializer_lr = valid_frame ? memory.Read64(initializer_fp + 8) : 0;
        u64 dispatch_lr = 0;
        if (initializer_lr == trace.base + 0x44ef6d4 || initializer_lr == trace.base + 0x44f00a0) {
            // Both return sites belong to 44ef620, which saves its incoming LR at FP+8.
            const u64 dispatch_fp = memory.Read64(initializer_fp);
            if (memory.IsValidVirtualAddressRange(dispatch_fp, 16)) {
                dispatch_lr = memory.Read64(dispatch_fp + 8);
            }
        }
        data = fmt::format("task={:#x} original={:#x} initializer_lr={:#x} dispatch_lr={:#x} "
                           "working_50={}",
                           x0, camera, initializer_lr, dispatch_lr,
                           CameraWords<20>(memory, camera + 0x50));
        break;
    }
    case 0x44de8b4: {
        const u64 descriptor = jit.GetRegister(21);
        data = fmt::format("original={:#x} source={:#x} source_50={}", x20, descriptor,
                           CameraWords<12>(memory, descriptor + 0x50));
        break;
    }
    case 0x1ce2c60: {
        std::array<u64, 3> cameras{};
        const u64 source_slot = trace.base + 0x5b415e0;
        if (!memory.IsValidVirtualAddressRange(x0 + 0x10, sizeof(cameras)) ||
            !memory.ReadBlock(x0 + 0x10, cameras.data(), sizeof(cameras)) ||
            !memory.IsValidVirtualAddressRange(source_slot, sizeof(u64))) {
            return;
        }
        const u64 source_owner = memory.Read64(source_slot);
        if (!source_owner || !memory.IsValidVirtualAddressRange(source_owner + 8, sizeof(u64))) {
            return;
        }
        const u64 source = memory.Read64(source_owner + 8);
        const auto [original, staging, provider] = cameras;
        const u64 table = memory.IsValidVirtualAddressRange(provider, sizeof(u64))
                              ? memory.Read64(provider)
                              : 0;
        const u64 converter = memory.IsValidVirtualAddressRange(table + 0x20, sizeof(u64))
                                  ? memory.Read64(table + 0x20)
                                  : 0;
        data = fmt::format("owner={:#x} original={:#x} staging={:#x} provider={:#x} "
                           "converter={:#x} source={:#x} source_50={} prior_50={}",
                           x0, original, staging, provider, converter, source,
                           CameraWords<6>(memory, source + 0x50),
                           CameraWords<8>(memory, original + 0x50));
        break;
    }
    case 0x4566d20: {
        if (!memory.IsValidVirtualAddressRange(x0 + 0x40, sizeof(u64))) {
            return;
        }
        const u64 original = memory.Read64(x0 + 0x40);
        data = fmt::format("dispatcher={:#x} original={:#x} working_50={}", x0, original,
                           CameraWords<8>(memory, original + 0x50));
        break;
    }
    }
    LOG_INFO(Core_ARM,
             "Dusk CPU camera capture_frame={} {} rva={:#x} return_rva={:#x} lr={:#x} tls={:#x} {}",
             frame, site.name, rva, caller, lr, tls, data);
}

} // Anonymous namespace

DynarmicCallbacks64::DynarmicCallbacks64(ArmDynarmic64& parent, Kernel::KProcess* process)
    : m_parent{parent}, m_memory(process->GetMemory())
    , m_process(process), m_debugger_enabled{parent.m_system.DebuggerEnabled()}
    , m_check_memory_access{m_debugger_enabled || !Settings::values.cpuopt_ignore_memory_aborts.GetValue()}
{}

u64 DynarmicCallbacks64::MemoryRead(u64 vaddr, size_t size) {
    CheckMemoryAccess(vaddr, size, Kernel::DebugWatchpointType::Read);
    u64 value{};
    switch (size) {
    case sizeof(u64): value = m_memory.Read64(vaddr); break;
    case sizeof(u32): value = m_memory.Read32(vaddr); break;
    case sizeof(u16): value = m_memory.Read16(vaddr); break;
    case sizeof(u8): value = m_memory.Read8(vaddr); break;
    default: UNREACHABLE();
    }
    // With memory aborts ignored the guest keeps running on zeros: say where it read from.
    if (auto& unmapped = Core::Memory::UnmappedReadFlag(); unmapped != 0) {
        unmapped = 0;
        static std::atomic<u32> logged{};
        if (logged.fetch_add(1) < 32) {
            LOG_ERROR(Core_ARM, "Unmapped read {:#x} ({} bytes) at pc {:#x} lr {:#x} sp {:#x}", vaddr, size,
                      m_parent.m_jit->GetPC(), m_parent.m_jit->GetRegister(30), m_parent.m_jit->GetSP());
        }
    }
    return value;
}
Dynarmic::A64::Vector DynarmicCallbacks64::MemoryRead128(u64 vaddr) {
    CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Read);
    return {m_memory.Read64(vaddr), m_memory.Read64(vaddr + 8)};
}

std::optional<u32> DynarmicCallbacks64::MemoryReadCode(u64 vaddr) {
    if (!m_memory.IsValidVirtualAddressRange(vaddr, sizeof(u32)))
        return std::nullopt;
    auto const aligned_vaddr = vaddr & ~Core::Memory::YUZU_PAGEMASK;
    if (last_code_addr != aligned_vaddr) {
        m_memory.ReadBlock(aligned_vaddr, &cached_code_page, sizeof(cached_code_page));
        last_code_addr = aligned_vaddr;
    }
    const u32 instruction = cached_code_page.inst[(vaddr & Core::Memory::YUZU_PAGEMASK) / sizeof(u32)];
    namespace Wide = DuskUltrawide;
    const u64 page_offset = vaddr & Memory::YUZU_PAGEMASK;
    if ((page_offset == (Wide::OptionalReturn.rva & Memory::YUZU_PAGEMASK) ||
         page_offset == (Wide::AfterOptional.rva & Memory::YUZU_PAGEMASK)) &&
        DuskUltrawide::Enabled() &&
        m_process->GetProgramId() == DuskCameraTrace::ProgramId) {
        const auto wide = Wide::session.load();
        const auto* site = wide && wide->owner == m_process ? Wide::FindHook(wide->base, vaddr) : nullptr;
        if (site) {
            if (wide->First(Wide::Event::Fetch)) {
                LOG_INFO(Core_ARM, "Dusk ultrawide camera JIT fetched pc={:#x} opcode={:#010x}, expected={:#010x}",
                         vaddr, instruction, site->word);
            }
            if (m_debugger_enabled) {
                ReportDuskCameraRejection(*wide, Wide::Event::Debugger, "debugger enabled", 1, "==", 0);
            } else if (instruction != site->word) {
                ReportDuskCameraRejection(*wide, Wide::Event::FetchOpcode, "JIT instruction",
                                          instruction, "==", site->word);
            } else {
                return DuskCameraTrace::BreakInstruction;
            }
        }
    }
    if (DuskRuntimeTrace::Enabled()) {
        if (!m_dusk_runtime_trace_checked) {
            const auto trace = DuskRuntimeTrace::session.load();
            if (trace && trace->owner == m_process) {
                m_dusk_runtime_trace = trace;
            }
            m_dusk_runtime_trace_checked = true;
        }
        if (m_dusk_runtime_trace && m_dusk_runtime_trace->FindSite(vaddr, instruction)) {
            return DuskCameraTrace::BreakInstruction;
        }
    }
    if (DuskCameraTrace::Enabled()) {
        if (!m_dusk_camera_trace_checked) {
            const auto trace = DuskCameraTrace::session.load();
            if (trace && trace->owner == m_process) {
                m_dusk_camera_trace = trace;
            }
            m_dusk_camera_trace_checked = true;
        }
        if (m_dusk_camera_trace &&
            DuskCameraTrace::FindSite(m_dusk_camera_trace->base, vaddr, instruction)) {
            return DuskCameraTrace::BreakInstruction;
        }
    }
    if (WindowTrace::Enabled() && m_process->GetProgramId() == WindowTrace::ProgramId) {
        if (!m_window_trace) {
            m_window_trace = WindowTrace::sessions.Find(m_process);
        }
        if (m_window_trace) {
            return WindowTrace::InstructionFor(m_window_trace->base, vaddr, instruction);
        }
    }
    return instruction;
}

void DynarmicCallbacks64::MemoryWrite(Dynarmic::A64::VAddr vaddr, u64 value, std::size_t size) {
    if (CheckMemoryAccess(vaddr, size, Kernel::DebugWatchpointType::Write)) {
        switch (size) {
        case sizeof(u64): return m_memory.Write64(vaddr, u64(value));
        case sizeof(u32): return m_memory.Write32(vaddr, u32(value));
        case sizeof(u16): return m_memory.Write16(vaddr, u16(value));
        case sizeof(u8): return m_memory.Write8(vaddr, u8(value));
        default: UNREACHABLE();
        }
    }
}
void DynarmicCallbacks64::MemoryWrite128(u64 vaddr, Dynarmic::A64::Vector value) {
    if (CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Write)) {
        m_memory.Write64(vaddr, value[0]);
        m_memory.Write64(vaddr + 8, value[1]);
    }
}

bool DynarmicCallbacks64::MemoryWriteExclusive8(u64 vaddr, std::uint8_t value, std::uint8_t expected) {
    return CheckMemoryAccess(vaddr, 1, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive8(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive16(u64 vaddr, std::uint16_t value, std::uint16_t expected) {
    return CheckMemoryAccess(vaddr, 2, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive16(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive32(u64 vaddr, std::uint32_t value, std::uint32_t expected) {
    return CheckMemoryAccess(vaddr, 4, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive32(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive64(u64 vaddr, std::uint64_t value, std::uint64_t expected) {
    return CheckMemoryAccess(vaddr, 8, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive64(vaddr, value, expected);
}
bool DynarmicCallbacks64::MemoryWriteExclusive128(u64 vaddr, Dynarmic::A64::Vector value, Dynarmic::A64::Vector expected) {
    return CheckMemoryAccess(vaddr, 16, Kernel::DebugWatchpointType::Write) &&
            m_memory.WriteExclusive128(vaddr, value, expected);
}

void DynarmicCallbacks64::InstructionCacheOperationRaised(Dynarmic::A64::InstructionCacheOperation op, u64 value) {
    last_code_addr = u64(-1); //invalidate cached page
    switch (op) {
    case Dynarmic::A64::InstructionCacheOperation::InvalidateByVAToPoU: {
        static constexpr u64 ICACHE_LINE_SIZE = 64;
        const u64 cache_line_start = value & ~(ICACHE_LINE_SIZE - 1);
        m_parent.InvalidateCacheRange(cache_line_start, ICACHE_LINE_SIZE);
        break;
    }
    case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoU:
        m_parent.ClearInstructionCache();
        break;
    case Dynarmic::A64::InstructionCacheOperation::InvalidateAllToPoUInnerSharable:
    default:
        LOG_DEBUG(Core_ARM, "Unprocesseed instruction cache operation: {}", op);
        break;
    }
    m_parent.m_jit->HaltExecution(Dynarmic::HaltReason::CacheInvalidation);
}

void DynarmicCallbacks64::ExceptionRaised(u64 pc, Dynarmic::A64::Exception exception) {
    if (DuskUltrawide::Enabled() && !m_debugger_enabled &&
        exception == Dynarmic::A64::Exception::Breakpoint) {
        namespace Wide = DuskUltrawide;
        const auto wide = Wide::session.load();
        const auto* site = wide && wide->owner == m_process ? Wide::FindHook(wide->base, pc) : nullptr;
        if (site) {
            auto& jit = *m_parent.m_jit;
            const auto reg = [&](std::size_t index) { return jit.GetRegister(index); };
            const u32 instruction = m_memory.Read32(pc);
            if (instruction != site->word) {
                ReportDuskCameraRejection(*wide, Wide::Event::ExecuteOpcode, "executed instruction",
                                          instruction, "==", site->word);
                ReturnException(pc, PrefetchAbort);
                return;
            }
            const bool early_return = site->rva == Wide::OptionalReturn.rva;
            const u64 lr = reg(30);
            if (early_return) {
                jit.SetPC(lr);
                if (lr != wide->base + Wide::AfterOptional.rva) {
                    return;
                }
            }
            if (wide->First(Wide::Event::Execute)) {
                LOG_INFO(Core_ARM,
                         "Dusk ultrawide camera hook executed pc={:#x} resume={:#x} lr={:#x} "
                         "sp={:#x} fp={:#x} camera_arg={:#x} state={:#x} handoff={:#x} index={:#x} companion={:#x}",
                         pc, jit.GetPC(), lr, jit.GetSP(), reg(29), reg(3), reg(19), reg(20),
                         u32(reg(21)), reg(23));
            }
            const auto read = [&](u64 address, std::size_t size) -> u64 {
                if (!m_memory.IsValidVirtualAddressRange(address, size)) {
                    return Wide::Unmapped;
                }
                if (size == 8) {
                    return m_memory.Read64(address);
                }
                return size == 4 ? m_memory.Read32(address) : m_memory.Read8(address);
            };
            const u64 state = reg(19);
            const u64 optional_state = reg(25);
            if (!early_return) {
                // Dynarmic has advanced PC. Resume the original LDRB at the join.
                jit.SetRegister(8, MemoryRead(optional_state + 0x2ed, 1));
                if (!m_memory.IsValidVirtualAddressRange(optional_state + 0x2ed, 1)) {
                    ReportDuskCameraRejection(*wide, Wide::Event::OriginalLoad, "mapped LDRB address",
                                              Wide::Unmapped, "==", optional_state + 0x2ed);
                    return;
                }
            }
            const auto reject = [&](Wide::Event event, std::string_view field, u64 observed,
                                    std::string_view comparison, u64 expected) {
                ReportDuskCameraRejection(*wide, event, field, observed, comparison, expected);
            };
            const u64 camera = Wide::CameraToOverride(wide->base, *site, jit.GetSP(), reg(29), lr,
                                                     reg(3), state, reg(20), u32(reg(21)), reg(23),
                                                     optional_state, read, reject,
                                                     wide->native_resolution);
            if (camera) {
                const u64 address = camera + Wide::AspectOffset;
                m_memory.Write32(address, Wide::WideAspect);
                if (!wide->Seen(Wide::Event::Applied)) {
                    const u64 actual = read(address, 4);
                    if (actual != Wide::WideAspect) {
                        reject(Wide::Event::WriteReadback, "local aspect readback", actual,
                               "==", Wide::WideAspect);
                    } else if (wide->First(Wide::Event::Applied)) {
                        LOG_INFO(Core_ARM,
                                 "Dusk ultrawide camera applied 43:18 to worker-local copy: "
                                 "camera={:#x} aspect_address={:#x} old={:#010x} readback={:#010x} "
                                 "expected={:#010x}",
                                 camera, address, Wide::StockAspect, actual, Wide::WideAspect);
                    }
                }
            }
            return;
        }
    }
    if (m_dusk_runtime_trace && exception == Dynarmic::A64::Exception::Breakpoint) {
        if (const auto* site = m_dusk_runtime_trace->FindSite(pc, m_memory.Read32(pc))) {
            auto& jit = *m_parent.m_jit;
            if (m_dusk_runtime_trace->Active()) {
                ObserveDuskRuntime(*m_dusk_runtime_trace, site->rva, jit, m_memory, m_tpidrro_el0);
            }
            ResumeDuskRuntime(site->rva, pc, jit, m_memory);
            return;
        }
    }
    if (m_dusk_camera_trace && exception == Dynarmic::A64::Exception::Breakpoint) {
        if (const auto* site =
                DuskCameraTrace::FindSite(m_dusk_camera_trace->base, pc, m_memory.Read32(pc))) {
            auto& jit = *m_parent.m_jit;
            if (m_dusk_camera_trace->IsActive()) {
                TraceDuskCamera(*m_dusk_camera_trace, *site, jit, m_memory, m_tpidrro_el0);
            }
            DuskCameraTrace::Resume(*site, jit, m_memory);
            return;
        }
    }
    if (m_window_trace && exception == Dynarmic::A64::Exception::Breakpoint) {
        const auto point = WindowTrace::Identify(m_window_trace->base, pc, m_memory.Read32(pc));
        if (point != WindowTrace::Point::None) {
            auto& jit = *m_parent.m_jit;
            const u64 x0 = jit.GetRegister(0);
            const u64 x1 = jit.GetRegister(1);
            u64 native = 0;
            if (point == WindowTrace::Point::WindowInitialize) {
                native = m_memory.Read64(x1 + 0x18);
            } else if (point == WindowTrace::Point::NativeInitialize ||
                       point == WindowTrace::Point::Acquire ||
                       point == WindowTrace::Point::Finalize) {
                native = m_memory.Read64(x0);
            }
            const auto identity = m_window_trace->Record(
                point, m_tpidrro_el0, point == WindowTrace::Point::WindowInitialize ? x0 : 0, native);
            if (!identity) {
                WindowTrace::Resume(point, jit, m_memory);
                return;
            }
            u32 registrations = 0;
            if (identity->native != 0) {
                for (u32 i = 0; i < 8; ++i) {
                    const u64 entry = m_window_trace->base + 0xF633F8 + i * 0x128;
                    registrations += m_memory.Read64(entry) == identity->native;
                }
            }
            LOG_INFO(Core_ARM, "Star Fox window trace session={} {} pc={:#x} lr={:#x} tls={:#x} window={:#x} native={:#x} registrations={} x0={:#x}",
                     m_window_trace->id, WindowTrace::Name(point), pc, jit.GetRegister(30),
                     m_tpidrro_el0, identity->window, identity->native, registrations, x0);
            if (point == WindowTrace::Point::NativeInitialize) {
                const u32 count = static_cast<u32>(jit.GetRegister(2));
                LOG_INFO(Core_ARM, "Star Fox window textures session={} tls={:#x} native={:#x} count={}",
                         m_window_trace->id, m_tpidrro_el0, identity->native, count);
                for (u32 i = 0; i < std::min(count, 4U); ++i) {
                    const u64 texture = x1 + i * 0x28;
                    LOG_INFO(Core_ARM, "Star Fox window texture session={} tls={:#x} native={:#x} index={} {}x{} format={} block_format={} pitch_gobs={} block_height={} fd={} pool_size={:#x} offset={:#x}",
                             m_window_trace->id, m_tpidrro_el0, identity->native,
                             i, m_memory.Read32(texture + 8), m_memory.Read32(texture + 12),
                             m_memory.Read8(texture + 16), m_memory.Read8(texture + 17),
                             m_memory.Read32(texture + 20), m_memory.Read32(texture + 24),
                             m_memory.Read32(texture + 28), m_memory.Read32(texture + 32),
                             m_memory.Read32(texture + 36));
                }
            } else if (point == WindowTrace::Point::ValidateTexture) {
                const u64 texture = jit.GetRegister(25);
                LOG_INFO(Core_ARM, "Star Fox window validation session={} tls={:#x} native={:#x} required_size={:#x} alignment={:#x} pool_size={:#x} offset={:#x}",
                         m_window_trace->id, m_tpidrro_el0, identity->native,
                         m_memory.Read64(jit.GetSP() + 0x10), m_memory.Read32(jit.GetSP() + 0x18),
                         m_memory.Read32(texture + 12), m_memory.Read32(texture + 16));
            } else if (point == WindowTrace::Point::Acquire || point == WindowTrace::Point::Finalize) {
                if (point == WindowTrace::Point::Acquire && registrations == 0) {
                    for (u32 i = 0; i < 8; ++i) {
                        const u64 entry = m_window_trace->base + 0xF633F8 + i * 0x128;
                        LOG_INFO(Core_ARM, "Star Fox window slot session={} tls={:#x} requested={:#x} slot={} native={:#x} active={}",
                                 m_window_trace->id, m_tpidrro_el0, identity->native, i,
                                 m_memory.Read64(entry), m_memory.Read8(entry + 0x120));
                    }
                }
            }
            WindowTrace::Resume(point, jit, m_memory);
            return;
        }
    }
    switch (exception) {
    case Dynarmic::A64::Exception::WaitForInterrupt:
    case Dynarmic::A64::Exception::WaitForEvent:
    case Dynarmic::A64::Exception::SendEvent:
    case Dynarmic::A64::Exception::SendEventLocal:
    case Dynarmic::A64::Exception::Yield:
        LOG_TRACE(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X}, cached = {:08X})", std::size_t(exception), pc, m_memory.Read32(pc), MemoryReadCode(pc).value_or(0));
        return;
    case Dynarmic::A64::Exception::NoExecuteFault:
        LOG_CRITICAL(Core_ARM, "Cannot execute instruction at unmapped address {:#016x}", pc);
        ReturnException(pc, PrefetchAbort);
        return;
    default:
        if (m_debugger_enabled) {
            ReturnException(pc, InstructionBreakpoint);
        } else {
            m_parent.LogBacktrace(m_process);
            LOG_CRITICAL(Core_ARM, "ExceptionRaised(exception = {}, pc = {:08X}, code = {:08X})", static_cast<std::size_t>(exception), pc, m_memory.Read32(pc));
        }
    }
}

void DynarmicCallbacks64::CallSVC(u32 svc) {
    m_parent.m_svc = svc;
    m_parent.m_jit->HaltExecution(SupervisorCall);
}

void DynarmicCallbacks64::AddTicks(u64 ticks) {
    ASSERT(!m_parent.m_uses_wall_clock && "Dynarmic ticking disabled");
    // Divide the number of ticks by the amount of CPU cores. TODO(Subv): This yields only a
    // rough approximation of the amount of executed ticks in the system, it may be thrown off
    // if not all cores are doing a similar amount of work. Instead of doing this, we should
    // device a way so that timing is consistent across all cores without increasing the ticks 4
    // times.
    u64 amortized_ticks = ticks / Core::Hardware::NUM_CPU_CORES;
    // Always execute at least one tick.
    amortized_ticks = std::max<u64>(amortized_ticks, 1);
    m_parent.m_system.CoreTiming().AddTicks(amortized_ticks);
}

u64 DynarmicCallbacks64::GetTicksRemaining() {
    ASSERT(!m_parent.m_uses_wall_clock && "Dynarmic ticking disabled");
    return std::max<s64>(m_parent.m_system.CoreTiming().downcount, 0);
}

u64 DynarmicCallbacks64::GetCNTPCT() {
    const u64 ticks = m_parent.m_system.CoreTiming().GetClockTicks();
    // Guest code that waits by polling the counter (title 1's main thread reads it ~17 million
    // times a second while waiting for the GPU) keeps a host core busy at full speed, and the
    // GPU thread is often on the other half of that SMT core. Two reads within ~1.7 us of each
    // other are such a loop: pause, which hands the core's resources to the sibling thread for a
    // moment. The guest sees the same counter. EDEN_NX2_CNTPCT_PAUSE=0 turns it off.
    static const bool pause_spins = [] {
        const char* value = std::getenv("EDEN_NX2_CNTPCT_PAUSE");
        return value == nullptr || value[0] != '0';
    }();
    if (pause_spins && ticks - m_last_cntpct < m_spin_ticks) {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_pause();
        _mm_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }
    m_last_cntpct = ticks;
    return ticks;
}

bool DynarmicCallbacks64::CheckMemoryAccess(u64 addr, u64 size, Kernel::DebugWatchpointType type) {
    if (!m_check_memory_access) {
        return true;
    }

    if (!m_memory.IsValidVirtualAddressRange(addr, size)) {
        LOG_CRITICAL(Core_ARM, "Stopping execution due to unmapped memory access at {:#x}",
                        addr);
        m_parent.m_jit->HaltExecution(PrefetchAbort);
        return false;
    }

    if (!m_debugger_enabled) {
        return true;
    }

    const auto match{m_parent.MatchingWatchpoint(addr, size, type)};
    if (match) {
        m_parent.m_halted_watchpoint = match;
        m_parent.m_jit->HaltExecution(DataAbort);
        return false;
    }

    return true;
}

void DynarmicCallbacks64::ReturnException(u64 pc, Dynarmic::HaltReason hr) {
    m_parent.GetContext(m_parent.m_breakpoint_context);
    m_parent.m_breakpoint_context.pc = pc;
    m_parent.m_jit->HaltExecution(hr);
}

void ArmDynarmic64::MakeJit(Common::PageTable* page_table, std::size_t address_space_bits) {
    Dynarmic::A64::UserConfig config;

    // Callbacks
    config.callbacks = std::addressof(*m_cb);

    // Memory
    if (page_table) {
        // Dynarmic will not write to the page table, const_cast is safe here
        config.page_table = reinterpret_cast<void**>(
            const_cast<Common::PageTable::PageEntryData*>(page_table->entries.data()));
        config.page_table_address_space_bits = std::uint32_t(address_space_bits);
        config.page_table_pointer_mask = Common::PageTable::ATTRIBUTE_MASK;
        config.page_table_marked_bit = uint8_t(0);
        config.silently_mirror_page_table = false;
        config.absolute_offset_page_table = true;
        config.detect_misaligned_access_via_page_table = 16 | 32 | 64 | 128;
        config.only_detect_misalignment_via_page_table_on_page_boundary = true;

        config.fastmem_pointer = page_table->fastmem_arena ?
            std::optional<uintptr_t>{reinterpret_cast<uintptr_t>(page_table->fastmem_arena)} :
            std::nullopt;
        config.fastmem_address_space_bits = std::uint32_t(address_space_bits);
        config.silently_mirror_fastmem = false;

        config.fastmem_exclusive_access = config.fastmem_pointer != std::nullopt;
        config.recompile_on_exclusive_fastmem_failure = true;

        if (reinterpret_cast<u64>(m_system.DeviceMemory().buffer.BackingBasePointer() +
            Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize()) < (1ULL << 39)) {
            // Systems like FreeBSD allocate memory really low by default, and since we pack our page table entries,
            // we have to manually sign extend when our actual pointer is negative.
            config.page_table_sign_extension = std::uint8_t(Common::PageTable::SIGN_BIT);
        }
    }

    // Multi-process state
    config.processor_id = std::uint8_t(m_core_index);
    config.global_monitor = &m_exclusive_monitor.monitor;

    // System registers
    config.tpidrro_el0 = &m_cb->m_tpidrro_el0;
    config.tpidr_el0 = &m_cb->m_tpidr_el0;
    config.dczid_el0 = 4;
    config.ctr_el0 = 0x8444c004;
    // 31.25 MHz for Switch 2 titles (Common::WallClock::SetGuestCNTFRQ, set at boot).
    config.cntfrq_el0 = static_cast<u32>(Common::g_wall_clock.GetGuestCNTFRQ());
    m_cb->m_spin_ticks = config.cntfrq_el0 / 600'000;

    // Unpredictable instructions
    config.define_unpredictable_behaviour = true;

    // Timing
    config.wall_clock_cntpct = m_uses_wall_clock;
    config.enable_cycle_counting = !m_uses_wall_clock;

    // Code cache size
#if defined(ARCHITECTURE_arm64) || defined(__sun__) || defined(__NetBSD__) || defined(__DragonFly__) || defined(__OpenBSD__)
    config.code_cache_size = std::uint32_t(128_MiB);
#else
    config.code_cache_size = std::uint32_t(512_MiB);
#endif

    // Allow memory fault handling to work
    if (m_system.DebuggerEnabled()) {
        config.check_halt_on_memory_access = true;
    }

    // null_jit
    if (!page_table) {
        // Don't waste too much memory on null_jit
        config.code_cache_size = std::uint32_t(8_MiB);
    }

    switch (Settings::values.cpu_accuracy.GetValue()) {
    // Debug mode
    case Settings::CpuAccuracy::Debugging:
        if (!Settings::values.cpuopt_page_tables) {
            config.page_table = nullptr;
        }
        if (!Settings::values.cpuopt_block_linking) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::BlockLinking;
        }
        if (!Settings::values.cpuopt_return_stack_buffer) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ReturnStackBuffer;
        }
        if (!Settings::values.cpuopt_fast_dispatcher) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::FastDispatch;
        }
        if (!Settings::values.cpuopt_context_elimination) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::GetSetElimination;
        }
        if (!Settings::values.cpuopt_const_prop) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::ConstProp;
        }
        if (!Settings::values.cpuopt_misc_ir) {
            config.optimizations &= ~Dynarmic::OptimizationFlag::MiscIROpt;
        }
        if (!Settings::values.cpuopt_reduce_misalign_checks) {
            config.only_detect_misalignment_via_page_table_on_page_boundary = false;
        }
        if (!Settings::values.cpuopt_fastmem) {
            config.fastmem_pointer = std::nullopt;
            config.fastmem_exclusive_access = false;
        }
        if (!Settings::values.cpuopt_fastmem_exclusives) {
            config.fastmem_exclusive_access = false;
        }
        if (!Settings::values.cpuopt_recompile_exclusives) {
            config.recompile_on_exclusive_fastmem_failure = false;
        }
        if (!Settings::values.cpuopt_ignore_memory_aborts) {
            config.check_halt_on_memory_access = true;
        }
        break;
    // Unsafe optimizations
    case Settings::CpuAccuracy::Unsafe:
        config.unsafe_optimizations = true;
        if (!Settings::values.cpuopt_unsafe_host_mmu) {
            config.fastmem_pointer = std::nullopt;
            config.fastmem_exclusive_access = false;
        }
        if (Settings::values.cpuopt_unsafe_unfuse_fma) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
        }
        if (Settings::values.cpuopt_unsafe_reduce_fp_error) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP;
        }
        if (Settings::values.cpuopt_unsafe_inaccurate_nan) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
        }
        if (Settings::values.cpuopt_unsafe_fastmem_check) {
            config.fastmem_address_space_bits = 64;
        }
        if (Settings::values.cpuopt_unsafe_ignore_global_monitor) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        }
        break;
    // Safe optimisations
    case Settings::CpuAccuracy::Auto:
        config.unsafe_optimizations = true;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA;
        config.fastmem_address_space_bits = 64;
        config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        break;
    // Paranoia mode for debugging optimizations
    case Settings::CpuAccuracy::Paranoid:
        config.unsafe_optimizations = false;
        config.optimizations = Dynarmic::no_optimizations;
        break;
    case Settings::CpuAccuracy::Accurate:
    default:
        break;
    }
    if (!Settings::IsFastmemEnabled()) {
        config.fastmem_pointer = std::nullopt;
        config.fastmem_exclusive_access = false;
    }
    m_jit.emplace(config);
}

HaltReason ArmDynarmic64::RunThread(Kernel::KThread* thread) {
    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Run());
}

HaltReason ArmDynarmic64::StepThread(Kernel::KThread* thread) {
    m_jit->ClearExclusiveState();
    return TranslateHaltReason(m_jit->Step());
}

u32 ArmDynarmic64::GetSvcNumber() const {
    return m_svc;
}

void ArmDynarmic64::GetSvcArguments(std::span<uint64_t, 8> args) const {
    Dynarmic::A64::Jit const& j = *m_jit;
    for (size_t i = 0; i < 8; i++)
        args[i] = j.GetRegister(i);
}

void ArmDynarmic64::SetSvcArguments(std::span<const uint64_t, 8> args) {
    Dynarmic::A64::Jit& j = *m_jit;
    for (size_t i = 0; i < 8; i++)
        j.SetRegister(i, args[i]);
}

const Kernel::DebugWatchpoint* ArmDynarmic64::HaltedWatchpoint() const {
    return m_halted_watchpoint;
}

void ArmDynarmic64::RewindBreakpointInstruction() {
    this->SetContext(m_breakpoint_context);
}

ArmDynarmic64::ArmDynarmic64(System& system, bool uses_wall_clock, Kernel::KProcess* process, DynarmicExclusiveMonitor& exclusive_monitor, std::size_t core_index)
    : ArmInterface{uses_wall_clock}, m_system{system}, m_exclusive_monitor{exclusive_monitor}
    , m_cb(std::make_optional<DynarmicCallbacks64>(*this, process))
    , m_core_index{core_index}
{
    auto& page_table = process->GetPageTable().GetBasePageTable();
    auto& page_table_impl = page_table.GetImpl();
    MakeJit(&page_table_impl, page_table.GetAddressSpaceWidth());
}

ArmDynarmic64::~ArmDynarmic64() = default;

void ArmDynarmic64::SetTpidrroEl0(u64 value) {
    m_cb->m_tpidrro_el0 = value;
}

void ArmDynarmic64::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    Dynarmic::A64::Jit const& j = *m_jit;
    auto gpr = j.GetRegisters();
    auto fpr = j.GetVectors();
    // TODO: this is inconvenient
    for (size_t i = 0; i < 29; i++)
        ctx.r[i] = gpr[i];
    ctx.fp = gpr[29];
    ctx.lr = gpr[30];
    ctx.sp = j.GetSP();
    ctx.pc = j.GetPC();
    ctx.pstate = j.GetPstate();
    ctx.v = fpr;
    ctx.fpcr = j.GetFpcr();
    ctx.fpsr = j.GetFpsr();
    ctx.tpidr = m_cb->m_tpidr_el0;
}

void ArmDynarmic64::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    Dynarmic::A64::Jit& j = *m_jit;
    // TODO: this is inconvenient
    std::array<u64, 31> gpr;
    for (size_t i = 0; i < 29; i++)
        gpr[i] = ctx.r[i];
    gpr[29] = ctx.fp;
    gpr[30] = ctx.lr;
    j.SetRegisters(gpr);
    j.SetSP(ctx.sp);
    j.SetPC(ctx.pc);
    j.SetPstate(ctx.pstate);
    j.SetVectors(ctx.v);
    j.SetFpcr(ctx.fpcr);
    j.SetFpsr(ctx.fpsr);
    m_cb->m_tpidr_el0 = ctx.tpidr;
}

void ArmDynarmic64::SignalInterrupt(Kernel::KThread* thread) {
    m_jit->HaltExecution(BreakLoop);
}

void ArmDynarmic64::ClearInstructionCache() {
    m_cb->last_code_addr = u64(-1);
    m_jit->ClearCache();
}

void ArmDynarmic64::InvalidateCacheRange(u64 addr, std::size_t size) {
    // Drop the cached code page too: blocks recompiled after the invalidation would
    // otherwise still decode the old instructions (e.g. a gdbstub breakpoint written
    // into or removed from that page).
    m_cb->last_code_addr = u64(-1);
    m_jit->InvalidateCacheRange(addr, size);
}

} // namespace Core
