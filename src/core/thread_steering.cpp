// =============================================================================
// EngineCore :: Thread Steering & DPC Isolation Module (Implementation)
// =============================================================================
#include "core/thread_steering.hpp"

#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_map>

namespace EngineCore::Core
{

namespace
{
    // Process-level information class used to toggle dynamic priority boost.
    // This value is stable public ABI (documented historically by Microsoft
    // in the Platform SDK headers under winternl.h in various forms) and is
    // used only against the current process's pseudo-handle.
    constexpr ULONG kProcessPriorityBoost = 0x1D;

    // Converts a raw Win32 error into a std::system_error for hard failures
    // that should abort topology construction (buffer sizing, etc).
    [[noreturn]] void ThrowLastError(const char* what)
    {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), what);
    }

    // Returns the zero-based bit position of the lowest set bit in `mask`,
    // or -1 if the mask is empty.
    int LowestSetBitIndex(KAFFINITY mask)
    {
        unsigned long index = 0;
        if (mask == 0)
        {
            return -1;
        }
#if defined(_WIN64)
        if (_BitScanForward64(&index, static_cast<unsigned __int64>(mask)))
        {
            return static_cast<int>(index);
        }
#else
        if (_BitScanForward(&index, static_cast<unsigned long>(mask)))
        {
            return static_cast<int>(index);
        }
#endif
        return -1;
    }
} // namespace

// -----------------------------------------------------------------------------
// CpuTopology::Query
// -----------------------------------------------------------------------------
CpuTopology CpuTopology::Query()
{
    CpuTopology topology;
    DWORD bufferSize = 0;

    // Determine buffer size
    GetLogicalProcessorInformationEx(RelationAll, nullptr, &bufferSize);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        return topology;
    }

    std::vector<std::uint8_t> buffer(bufferSize);
    auto* pInfo = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());

    if (!GetLogicalProcessorInformationEx(RelationAll, pInfo, &bufferSize))
    {
        return topology;
    }

    DWORD offset = 0;
    std::uint8_t maxEfficiency = 0;
    std::uint8_t minEfficiency = 0xFF;

    while (offset < bufferSize)
    {
        auto* pCurrent = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);

        if (pCurrent->Relationship == RelationProcessorCore)
        {
            std::uint8_t effClass = pCurrent->Processor.EfficiencyClass;
            // حماية باستخدام الأقواس لتجنب ماكرو Windows max/min
            maxEfficiency = (std::max)(maxEfficiency, effClass);
            minEfficiency = (std::min)(minEfficiency, effClass);

            for (WORD g = 0; g < pCurrent->Processor.GroupCount; ++g)
            {
                KAFFINITY mask = pCurrent->Processor.GroupMask[g].Mask;
                WORD group = pCurrent->Processor.GroupMask[g].Group;

                for (std::uint32_t idx = 0; idx < 64; ++idx)
                {
                    if ((mask & (1ULL << idx)) != 0)
                    {
                        LogicalCoreInfo core;
                        core.group = group;
                        core.index = idx;
                        core.groupAffinityMask = (1ULL << idx);
                        core.efficiencyClass = effClass;
                        topology.cores_.push_back(core);
                    }
                }
            }
        }
        offset += pCurrent->Size;
    }

    topology.isHybrid_ = (maxEfficiency > minEfficiency);

    // Classify P-cores vs E-cores
    for (auto& core : topology.cores_)
    {
        if (!topology.isHybrid_)
        {
            core.coreClass = CoreClass::PerformanceCore;
        }
        else
        {
            core.coreClass = (core.efficiencyClass == maxEfficiency) 
                ? CoreClass::PerformanceCore 
                : CoreClass::EfficiencyCore;
        }
    }

    return topology;
}

std::vector<LogicalCoreInfo> CpuTopology::PerformanceCores() const
{
    std::vector<LogicalCoreInfo> result;
    result.reserve(cores_.size());
    for (const auto& core : cores_)
    {
        if (core.coreClass == CoreClass::PerformanceCore)
        {
            result.push_back(core);
        }
    }
    return result;
}

std::vector<LogicalCoreInfo> CpuTopology::PreferredCacheCores() const
{
    std::vector<LogicalCoreInfo> result;
    for (const auto& core : cores_)
    {
        if (core.isPreferredCacheCore)
        {
            result.push_back(core);
        }
    }
    if (result.empty())
    {
        return PerformanceCores();
    }
    return result;
}

GROUP_AFFINITY CpuTopology::BuildAffinityExcludingCore0(
    const std::vector<LogicalCoreInfo>& candidates, WORD group)
{
    GROUP_AFFINITY affinity{};
    affinity.Group = group;
    affinity.Mask = 0;

    for (const auto& core : candidates)
    {
        if (core.group != group)
        {
            continue;
        }
        // Exclude logical processor index 0 within the requested group;
        // this is conventionally the core most heavily used for platform
        // interrupt servicing and DPC delivery on Windows.
        if (core.index == 0)
        {
            continue;
        }
        affinity.Mask |= core.groupAffinityMask;
    }

    return affinity;
}

// -----------------------------------------------------------------------------
// ThreadSteering
// -----------------------------------------------------------------------------
ThreadSteering::ThreadSteering()
{
    // Resolve NtSetInformationProcess dynamically. This keeps the hard
    // Win32 import table free of NTAPI symbols and allows graceful
    // degradation if, for any reason, the export is unavailable.
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"); ntdll != nullptr)
    {
        ntSetInformationProcess_ =
            reinterpret_cast<NtSetInformationProcessFn>(GetProcAddress(ntdll, "NtSetInformationProcess"));
    }
}

std::vector<OwnedThreadDescriptor> ThreadSteering::EnumerateOwnThreads() const
{
    std::vector<OwnedThreadDescriptor> threads;

    // CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD) enumerates threads across
    // the *entire system*; we filter strictly to GetCurrentProcessId() below
    // so no other process's threads are ever touched or reported.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return threads;
    }

    const DWORD ownPid = GetCurrentProcessId();

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(THREADENTRY32);

    if (Thread32First(snapshot, &entry))
    {
        do
        {
            if (entry.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(entry.th32OwnerProcessID) &&
                entry.th32OwnerProcessID == ownPid)
            {
                OwnedThreadDescriptor descriptor;
                descriptor.threadId = entry.th32ThreadID;
                threads.push_back(std::move(descriptor));
            }
            entry.dwSize = sizeof(THREADENTRY32);
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return threads;
}

std::size_t ThreadSteering::IsolateProcessFromCore0(const CpuTopology& topology)
{
    std::size_t steeredCount = 0;

    // Prefer performance cores when the system is hybrid; otherwise use the
    // full core list. Either way, core index 0 is excluded by
    // BuildAffinityExcludingCore0.
    const auto candidates = topology.IsHybrid() ? topology.PerformanceCores() : topology.Cores();
    if (candidates.empty())
    {
        return 0;
    }

    // Steering is computed per-group; most consumer systems have a single
    // processor group (<=64 logical CPUs), but the code remains correct on
    // multi-group server topologies.
    const WORD targetGroup = candidates.front().group;
    const GROUP_AFFINITY affinity = CpuTopology::BuildAffinityExcludingCore0(candidates, targetGroup);
    if (affinity.Mask == 0)
    {
        // Degenerate case: excluding core 0 left no candidate bits (e.g. a
        // single-core VM). Do nothing rather than pin threads to an empty set.
        return 0;
    }

    for (const auto& thread : EnumerateOwnThreads())
    {
        if (SetThreadGroupAffinity(thread.threadId, affinity))
        {
            ++steeredCount;
        }
    }

    return steeredCount;
}

bool ThreadSteering::SetThreadGroupAffinity(DWORD threadId, const GROUP_AFFINITY& affinity) noexcept
{
    ScopedThreadHandle handle(OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, threadId));
    if (!handle.Valid())
    {
        return false;
    }

    GROUP_AFFINITY previous{};
    // NOTE: explicitly global-qualified (::) because this member function
    // shares its name with the Win32 API SetThreadGroupAffinity; unqualified
    // lookup here would otherwise resolve back to this very member function.
    return ::SetThreadGroupAffinity(handle.Get(), &affinity, &previous) != FALSE;
}

bool ThreadSteering::SetThreadIdealCore(DWORD threadId, WORD group, std::uint32_t coreIndex) noexcept
{
    ScopedThreadHandle handle(OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, threadId));
    if (!handle.Valid())
    {
        return false;
    }

    PROCESSOR_NUMBER idealProcessor{};
    idealProcessor.Group = group;
    idealProcessor.Number = static_cast<BYTE>(coreIndex);
    idealProcessor.Reserved = 0;

    return SetThreadIdealProcessorEx(handle.Get(), &idealProcessor, nullptr) != FALSE;
}

bool ThreadSteering::SetProcessPriorityClass(DWORD priorityClass) noexcept
{
    return ::SetPriorityClass(GetCurrentProcess(), priorityClass) != FALSE;
}

bool ThreadSteering::SetProcessPriorityBoostDisabled(bool disabled) noexcept
{
    if (ntSetInformationProcess_ != nullptr)
    {
        ULONG value = disabled ? TRUE : FALSE;
        const LONG status =
            ntSetInformationProcess_(GetCurrentProcess(), kProcessPriorityBoost, &value, sizeof(value));
        if (status >= 0) // NT_SUCCESS
        {
            return true;
        }
    }

    // Documented fallback: SetProcessPriorityBoost is the public Win32
    // equivalent and is preferred whenever available.
    return ::SetProcessPriorityBoost(GetCurrentProcess(), disabled ? TRUE : FALSE) != FALSE;
}

} // namespace EngineCore::Core