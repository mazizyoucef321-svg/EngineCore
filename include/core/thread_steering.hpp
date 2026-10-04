// =============================================================================
// EngineCore :: Thread Steering & DPC Isolation Module
// -----------------------------------------------------------------------------
// Provides CPU topology discovery (P-core / E-core / cache-partition heuristics)
// and safe, documented-API-based thread affinity steering to keep application
// worker threads away from CPU 0, which on most Windows systems is the primary
// target for device interrupts (ISRs) and deferred procedure calls (DPCs).
//
// Design constraints:
//   - Read-only process/thread handle queries against OUR OWN process only.
//   - No cross-process memory access, no hooking, no code injection.
//   - Uses only documented Win32 APIs for affinity/priority manipulation.
//     A single, narrowly-scoped NTAPI call (NtSetInformationProcess) is used
//     exclusively to toggle ProcessPriorityBoost on the current process; this
//     is a well-known, non-invasive tuning call and touches no other process.
// =============================================================================
#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace EngineCore::Core
{

// -----------------------------------------------------------------------------
// Strongly-typed classification of a logical processor's performance role.
// -----------------------------------------------------------------------------
enum class CoreClass : std::uint8_t
{
    Unknown = 0,
    EfficiencyCore, // "E-core" (low EfficiencyClass value on hybrid parts)
    PerformanceCore, // "P-core" (high EfficiencyClass value on hybrid parts)
};

// -----------------------------------------------------------------------------
// Describes a single logical processor as seen by the OS scheduler.
// -----------------------------------------------------------------------------
struct LogicalCoreInfo
{
    WORD group = 0; // Processor group number (systems with >64 logical CPUs).
    std::uint32_t index = 0; // Logical processor index within the group [0, 63].
    KAFFINITY groupAffinityMask = 0; // Single-bit mask of this core within its group.
    CoreClass coreClass = CoreClass::Unknown;
    std::uint8_t efficiencyClass = 0; // Raw EfficiencyClass value reported by the OS.
    std::size_t l3CacheBytes = 0; // Size of the L3 partition this core participates in.
    bool isPreferredCacheCore = false; // Heuristic flag for large-L3 ("3D V-Cache"-style) cores.
};

// -----------------------------------------------------------------------------
// Full logical topology snapshot for the local machine.
// -----------------------------------------------------------------------------
class CpuTopology
{
public:
    // Performs a full topology enumeration. Throws std::system_error on
    // unrecoverable Win32 API failure (e.g. insufficient buffer allocation).
    static CpuTopology Query();

    [[nodiscard]] const std::vector<LogicalCoreInfo>& Cores() const noexcept { return cores_; }

    // Returns all cores classified as performance cores. On non-hybrid parts
    // (no EfficiencyClass distinction reported by the OS) this returns every
    // core, since all cores are equally "performant".
    [[nodiscard]] std::vector<LogicalCoreInfo> PerformanceCores() const;

    // Returns cores flagged by the L3-partition-size heuristic as the most
    // cache-favored set (candidate "3D V-Cache" CCD on capable AMD parts).
    // Falls back to PerformanceCores() when no distinguishable partition exists.
    [[nodiscard]] std::vector<LogicalCoreInfo> PreferredCacheCores() const;

    // True if the OS reports more than one distinct EfficiencyClass value,
    // i.e. this is a hybrid (big.LITTLE-style) part.
    [[nodiscard]] bool IsHybrid() const noexcept { return isHybrid_; }

    // Builds a GROUP_AFFINITY mask for `group` that excludes logical
    // processor index 0. Used to steer threads away from CPU 0, which most
    // commonly services platform interrupts and DPCs on Windows.
    [[nodiscard]] static GROUP_AFFINITY BuildAffinityExcludingCore0(
        const std::vector<LogicalCoreInfo>& candidates, WORD group);

private:
    std::vector<LogicalCoreInfo> cores_;
    bool isHybrid_ = false;
};

// -----------------------------------------------------------------------------
// RAII wrapper around a native thread HANDLE opened against our own process.
// Automatically closes the handle on destruction.
// -----------------------------------------------------------------------------
class ScopedThreadHandle
{
public:
    ScopedThreadHandle() noexcept = default;
    explicit ScopedThreadHandle(HANDLE handle) noexcept : handle_(handle) {}

    ScopedThreadHandle(const ScopedThreadHandle&) = delete;
    ScopedThreadHandle& operator=(const ScopedThreadHandle&) = delete;

    ScopedThreadHandle(ScopedThreadHandle&& other) noexcept : handle_(other.Release()) {}
    ScopedThreadHandle& operator=(ScopedThreadHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            handle_ = other.Release();
        }
        return *this;
    }

    ~ScopedThreadHandle() { Reset(); }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] bool Valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    HANDLE Release() noexcept
    {
        HANDLE h = handle_;
        handle_ = nullptr;
        return h;
    }

    void Reset(HANDLE newHandle = nullptr) noexcept
    {
        if (Valid())
        {
            CloseHandle(handle_);
        }
        handle_ = newHandle;
    }

private:
    HANDLE handle_ = nullptr;
};

// -----------------------------------------------------------------------------
// Lightweight description of one thread belonging to the current process,
// obtained via a read-only Toolhelp32 snapshot (no other process is touched).
// -----------------------------------------------------------------------------
struct OwnedThreadDescriptor
{
    DWORD threadId = 0;
    std::wstring debugName; // Populated only if SetThreadDescription was used by the owner.
};

// -----------------------------------------------------------------------------
// ThreadSteering: orchestrates enumeration of the current process's threads
// and applies affinity / priority steering decisions to keep hot-path worker
// threads away from CPU 0 and, where beneficial, onto performance or
// cache-favored cores.
// -----------------------------------------------------------------------------
class ThreadSteering
{
public:
    ThreadSteering();

    // Enumerates all threads currently owned by this process (read-only
    // Toolhelp32 snapshot, restricted to GetCurrentProcessId()).
    [[nodiscard]] std::vector<OwnedThreadDescriptor> EnumerateOwnThreads() const;

    // Applies an affinity mask that excludes logical CPU 0 (per the supplied
    // topology) to every thread currently owned by this process. Threads
    // that cannot be opened (rare, e.g. mid-teardown) are skipped rather than
    // treated as a hard failure. Returns the number of threads successfully
    // re-steered.
    std::size_t IsolateProcessFromCore0(const CpuTopology& topology);

    // Applies an explicit GROUP_AFFINITY to a single OS thread id owned by
    // this process. Returns false if the handle could not be opened or the
    // affinity call failed; never throws.
    bool SetThreadGroupAffinity(DWORD threadId, const GROUP_AFFINITY& affinity) noexcept;

    // Sets an ideal processor hint (soft scheduling preference, not a hard
    // pin) for a thread owned by this process.
    bool SetThreadIdealCore(DWORD threadId, WORD group, std::uint32_t coreIndex) noexcept;

    // Raises this process's scheduling priority class. `priorityClass` must
    // be one of the documented Win32 priority class constants
    // (e.g. HIGH_PRIORITY_CLASS, ABOVE_NORMAL_PRIORITY_CLASS).
    bool SetProcessPriorityClass(DWORD priorityClass) noexcept;

    // Disables or enables Windows' dynamic priority boost for this process.
    // Implemented via NtSetInformationProcess(ProcessPriorityBoost), resolved
    // dynamically from ntdll.dll. This call only ever targets the current
    // process handle (GetCurrentProcess(), a pseudo-handle) and performs no
    // cross-process operation of any kind.
    bool SetProcessPriorityBoostDisabled(bool disabled) noexcept;

private:
    using NtSetInformationProcessFn = LONG(__stdcall*)(HANDLE, ULONG, PVOID, ULONG);
    NtSetInformationProcessFn ntSetInformationProcess_ = nullptr;
};

} // namespace EngineCore::Core