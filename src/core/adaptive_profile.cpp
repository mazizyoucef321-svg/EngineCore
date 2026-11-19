// =============================================================================
// EngineCore :: Adaptive System Profile (Implementation)
// =============================================================================
#include "core/adaptive_profile.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace EngineCore::Core
{

namespace
{
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;

    // Counts physical cores / logical processors / SMT / hybrid layout.
    CpuSummary QueryCpu()
    {
        CpuSummary summary;
        summary.processorGroups = std::max<std::uint32_t>(1, GetActiveProcessorGroupCount());

        // Two-call pattern, retried: the required size can change between calls.
        std::vector<std::byte> buffer;
        DWORD length = 0;
        bool ok = false;
        for (int attempt = 0; attempt < 4 && !ok; ++attempt)
        {
            auto* info = buffer.empty() ? nullptr : reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
            if (GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length))
            {
                ok = true;
            }
            else if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
            {
                buffer.resize(length);
            }
            else
            {
                break;
            }
        }

        if (ok)
        {
            std::uint8_t minEfficiency = 255;
            std::uint8_t maxEfficiency = 0;
            for (DWORD offset = 0; offset < length;)
            {
                const auto* entry =
                    reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
                if (entry->Size == 0)
                {
                    break; // Malformed data: never loop forever.
                }
                if (entry->Relationship == RelationProcessorCore)
                {
                    const PROCESSOR_RELATIONSHIP& core = entry->Processor;
                    ++summary.physicalCores;
                    if ((core.Flags & LTP_PC_SMT) != 0)
                    {
                        summary.smtActive = true;
                    }
                    minEfficiency = std::min(minEfficiency, core.EfficiencyClass);
                    maxEfficiency = std::max(maxEfficiency, core.EfficiencyClass);
                    for (WORD g = 0; g < core.GroupCount; ++g)
                    {
                        summary.logicalProcessors += static_cast<std::uint32_t>(
                            std::popcount(static_cast<std::uint64_t>(core.GroupMask[g].Mask)));
                    }
                }
                offset += entry->Size;
            }
            summary.hybrid = maxEfficiency > minEfficiency;
        }

        if (summary.logicalProcessors == 0)
        {
            // API failed: fall back to the coarse count (physical core count unknown).
            SYSTEM_INFO info{};
            GetSystemInfo(&info);
            summary.logicalProcessors = info.dwNumberOfProcessors;
            summary.physicalCores = info.dwNumberOfProcessors;
            summary.smtActive = false;
        }
        if (summary.logicalProcessors == 0)
        {
            throw std::runtime_error("processor count unavailable");
        }
        return summary;
    }

    MemorySummary QueryMemory()
    {
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (!GlobalMemoryStatusEx(&status))
        {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                     "GlobalMemoryStatusEx failed");
        }
        return MemorySummary{status.ullTotalPhys, status.ullAvailPhys};
    }

    // Tier -> concrete numbers. Lower tiers purge VRAM earlier and more eagerly
    // (less slack) but ramp thread changes more slowly and with longer grace.
    TunedParameters MakeTuned(SystemTier tier, const CpuSummary& cpu, const GpuMemoryHint& gpu)
    {
        using namespace std::chrono_literals;
        TunedParameters t; // Defaults == Entry tier.

        switch (tier)
        {
        case SystemTier::Entry:
            t.steeringAggressiveness = 0.25f;
            t.graceLoadingProcess = 1500ms;
            t.graceWarmProcess = 10000ms;
            t.rampThreadsPerStep = 1;
            t.rampStepInterval = 750ms;
            t.hitchTolerance = 0.06;
            t.backoffCooldown = 30s;
            t.vramHighPressure = 0.80f;
            t.purgeStaleFrames = 120;
            t.purgeMaxEntriesPerPass = 8;
            t.purgeCooldown = 500ms;
            break;
        case SystemTier::Mid:
            t.steeringAggressiveness = 0.50f;
            t.graceLoadingProcess = 1000ms;
            t.graceWarmProcess = 6000ms;
            t.rampThreadsPerStep = 2;
            t.rampStepInterval = 500ms;
            t.hitchTolerance = 0.05;
            t.backoffCooldown = 20s;
            t.vramHighPressure = 0.90f;
            t.purgeStaleFrames = 300;
            t.purgeMaxEntriesPerPass = 4;
            t.purgeCooldown = 1000ms;
            break;
        case SystemTier::High:
            t.steeringAggressiveness = 0.80f;
            t.graceLoadingProcess = 750ms;
            t.graceWarmProcess = 3000ms;
            t.rampThreadsPerStep = 4;
            t.rampStepInterval = 300ms;
            t.hitchTolerance = 0.04;
            t.backoffCooldown = 15s;
            t.vramHighPressure = 0.92f;
            t.purgeStaleFrames = 600;
            t.purgeMaxEntriesPerPass = 2;
            t.purgeCooldown = 2000ms;
            break;
        }

        // Excluding CPU0 removes 1/N of the machine: only worthwhile with enough CPUs.
        t.avoidCpu0 = cpu.logicalProcessors >= 6;
        // Hard affinity needs headroom and a single processor group.
        t.allowHardAffinity = cpu.logicalProcessors >= 4 && cpu.processorGroups == 1;
        // Shared-memory GPUs leave less slack: start purging earlier.
        if (gpu.isUma)
        {
            t.vramHighPressure = std::min(t.vramHighPressure, 0.85f);
        }
        return t;
    }
} // namespace

const char* ToString(SystemTier tier) noexcept
{
    switch (tier)
    {
    case SystemTier::Entry: return "Entry";
    case SystemTier::Mid: return "Mid";
    case SystemTier::High: return "High";
    }
    return "Unknown";
}

TunedParameters TunedParameters::Conservative() noexcept
{
    TunedParameters t; // Entry-tier defaults.
    t.allowHardAffinity = false;
    t.avoidCpu0 = false;
    return t;
}

AdaptiveProfile SystemTierProfiler::Classify(const CpuSummary& cpu, const MemorySummary& memory,
                                              const GpuMemoryHint& gpu)
{
    AdaptiveProfile profile;
    profile.cpu = cpu;
    profile.memory = memory;
    profile.gpu = gpu;

    // UMA: the iGPU can borrow system RAM, but never more than DXGI reports
    // and (heuristically) no more than half of physical memory.
    profile.gpuBudgetBytes = gpu.isUma ? std::min(gpu.sharedSystemMemoryBytes, memory.totalPhysicalBytes / 2)
                                       : gpu.dedicatedVramBytes;

    const double ramGiB = static_cast<double>(memory.totalPhysicalBytes) / kGiB;
    const double gpuGiB = static_cast<double>(profile.gpuBudgetBytes) / kGiB;

    // Thresholds tolerate OS / iGPU carve-outs (a "16 GB" PC reports ~15 GiB).
    const int cpuScore = cpu.physicalCores >= 8 ? 3 : cpu.physicalCores >= 6 ? 2 : cpu.physicalCores >= 4 ? 1 : 0;
    const int ramScore = ramGiB >= 23.0 ? 3 : ramGiB >= 14.0 ? 2 : ramGiB >= 6.5 ? 1 : 0;
    int gpuScore = 0;
    if (gpu.isUma)
    {
        gpuScore = gpuGiB >= 4.0 ? 1 : 0; // An iGPU is bandwidth-bound regardless of how much RAM it may borrow.
    }
    else
    {
        gpuScore = gpuGiB >= 8.0 ? 3 : gpuGiB >= 4.0 ? 2 : gpuGiB >= 2.0 ? 1 : 0;
    }

    profile.score = cpuScore + ramScore + gpuScore; // 0..9
    profile.tier = profile.score >= 7 ? SystemTier::High : profile.score >= 4 ? SystemTier::Mid : SystemTier::Entry;
    if (gpu.isUma && profile.tier == SystemTier::High)
    {
        profile.tier = SystemTier::Mid; // Integrated graphics never classify as High-End.
    }

    profile.tuned = MakeTuned(profile.tier, cpu, gpu);
    return profile;
}

AdaptiveProfile SystemTierProfiler::FallbackProfile(const GpuMemoryHint& gpu)
{
    AdaptiveProfile profile;
    profile.tier = SystemTier::Entry;
    profile.gpu = gpu;
    profile.fromFallback = true;
    profile.tuned = TunedParameters::Conservative();
    return profile;
}

void SystemTierProfiler::StartAsync(GpuMemoryHint gpu)
{
    if (started_)
    {
        return;
    }
    started_ = true;

    std::promise<AdaptiveProfile> promise;
    future_ = promise.get_future().share();

    worker_ = std::jthread(
        [promise = std::move(promise), gpu](std::stop_token) mutable
        {
            // Background priority: this must never compete with startup work.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            AdaptiveProfile result;
            try
            {
                result = SystemTierProfiler::Classify(QueryCpu(), QueryMemory(), gpu);
            }
            catch (...)
            {
                result = SystemTierProfiler::FallbackProfile(gpu);
            }
            promise.set_value(std::move(result));
        });
}

std::optional<AdaptiveProfile> SystemTierProfiler::TryGet() const
{
    if (!future_.valid() || future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    {
        return std::nullopt;
    }
    return future_.get();
}

std::optional<AdaptiveProfile> SystemTierProfiler::WaitFor(std::chrono::milliseconds timeout) const
{
    if (!future_.valid() || future_.wait_for(timeout) != std::future_status::ready)
    {
        return std::nullopt;
    }
    return future_.get();
}

} // namespace EngineCore::Core