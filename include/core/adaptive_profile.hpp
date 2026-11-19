// =============================================================================
// EngineCore :: Adaptive System Profile (async, one-shot hardware tiering)
// -----------------------------------------------------------------------------
// Runs ONCE on a short-lived background thread at startup. It reads CPU
// topology (GetLogicalProcessorInformationEx) and RAM (GlobalMemoryStatusEx),
// combines them with GPU-memory facts the caller already detected via DXGI,
// scores the machine, and derives a tier (Entry / Mid / High) plus a set of
// tuned parameters other modules read instead of hard-coded constants.
//
// Notes:
//   - The score thresholds are STARTING HEURISTICS. Calibrate them against
//     real traces from your target machines before trusting the tier names.
//   - Everything here is read-only system queries; no handles to other
//     processes, no privileges.
//   - StartAsync() must be called from one thread before other threads call
//     TryGet()/WaitFor(). After that, all accessors are thread-safe.
// =============================================================================
#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <thread>

namespace EngineCore::Core
{

enum class SystemTier : std::uint8_t
{
    Entry,
    Mid,
    High,
};

[[nodiscard]] const char* ToString(SystemTier tier) noexcept;

struct CpuSummary
{
    std::uint32_t physicalCores = 0;
    std::uint32_t logicalProcessors = 0;
    std::uint32_t processorGroups = 1;
    bool smtActive = false; // Hyper-Threading / SMT present on at least one core.
    bool hybrid = false; // More than one EfficiencyClass reported (P/E cores).
};

struct MemorySummary
{
    std::uint64_t totalPhysicalBytes = 0;
    std::uint64_t availablePhysicalBytes = 0;
};

// GPU facts the caller already has from DXGI (keeps this module DXGI-free).
struct GpuMemoryHint
{
    bool isUma = true;
    std::uint64_t dedicatedVramBytes = 0;
    std::uint64_t sharedSystemMemoryBytes = 0;
};

// -----------------------------------------------------------------------------
// Parameters derived from the tier. Defaults are the conservative Entry-tier
// values, so a default-constructed struct is always safe to use.
// -----------------------------------------------------------------------------
struct TunedParameters
{
    // ---- Thread steering / launch-order guard ----
    // Single 0..1 scalar for modules that want one "how hard to push" knob
    // (e.g. a future CoreVault). The launch-order guard does NOT read it; it
    // uses the concrete values below.
    float steeringAggressiveness = 0.25f;
    bool avoidCpu0 = false; // Excluding CPU0 only pays off with enough logical CPUs.
    bool allowHardAffinity = false; // false => ideal-processor hints only.
    std::uint32_t minHardAffinityCpus = 3;

    // Observation window before the guard touches an existing process.
    std::chrono::milliseconds graceLoadingProcess{1500}; // target started after EngineCore (still loading).
    std::chrono::milliseconds graceWarmProcess{10000}; // target already running, mid-render.

    // Gradual ramp.
    std::uint32_t rampThreadsPerStep = 1;
    std::chrono::milliseconds rampStepInterval{750};

    // Stutter detection / rollback.
    std::uint32_t degradedTicksBeforeRollback = 3; // guard ticks (250 ms each by default).
    double hitchTolerance = 0.06; // absolute increase in hitch fraction tolerated.
    double frameTimeTolerance = 0.15; // relative increase in average frame time tolerated.
    std::chrono::seconds backoffCooldown{30};
    std::uint32_t maxRollbacks = 2;

    // ---- VRAM management ----
    float vramHighPressure = 0.80f; // fraction of budget at which purging starts.
    std::uint64_t purgeStaleFrames = 120; // evict resources unused for this many frames.
    std::uint32_t purgeMaxEntriesPerPass = 8;
    std::chrono::milliseconds purgeCooldown{500};

    [[nodiscard]] static TunedParameters Conservative() noexcept;
};

struct AdaptiveProfile
{
    SystemTier tier = SystemTier::Entry;
    int score = 0; // 0..9, see Classify().
    CpuSummary cpu;
    MemorySummary memory;
    GpuMemoryHint gpu;
    std::uint64_t gpuBudgetBytes = 0; // Estimated usable GPU memory (heuristic for UMA parts).
    bool fromFallback = false; // True if the queries failed and safe defaults were used.
    TunedParameters tuned;
};

class SystemTierProfiler
{
public:
    SystemTierProfiler() = default;
    ~SystemTierProfiler() = default; // jthread member joins.

    SystemTierProfiler(const SystemTierProfiler&) = delete;
    SystemTierProfiler& operator=(const SystemTierProfiler&) = delete;

    // Starts the one-shot background query. Idempotent.
    void StartAsync(GpuMemoryHint gpu);

    // Non-blocking: the finished profile, or nullopt if not ready / not started.
    [[nodiscard]] std::optional<AdaptiveProfile> TryGet() const;

    // Bounded wait for consumers that need the result at a known point.
    [[nodiscard]] std::optional<AdaptiveProfile> WaitFor(std::chrono::milliseconds timeout) const;

    // Pure function (no system calls): unit-testable decision matrix.
    [[nodiscard]] static AdaptiveProfile Classify(const CpuSummary& cpu, const MemorySummary& memory,
                                                   const GpuMemoryHint& gpu);

    // Safe profile used when the queries fail or the result is needed too early.
    [[nodiscard]] static AdaptiveProfile FallbackProfile(const GpuMemoryHint& gpu);

private:
    std::shared_future<AdaptiveProfile> future_;
    bool started_ = false;
    std::jthread worker_; // Declared last: destroyed (joined) first.
};

} // namespace EngineCore::Core