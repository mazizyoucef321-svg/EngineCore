// =============================================================================
// EngineCore :: Launch-Order Guard
// -----------------------------------------------------------------------------
// Problem: applying thread affinity / starting ETW consumers against a game
// that is ALREADY running (and mid-render) can cause migration storms and
// stutter, while doing the same during the game's loading phase is harmless.
//
// This guard makes the order of operations explicit and reversible:
//
//   Searching  -> poll for the configured executable(s) (1.5 s, low priority).
//   Observing  -> target found. An event is raised so the owner can re-target
//                 its ETW consumers; the guard itself touches NOTHING for a
//                 grace period (longer if the process was already warm),
//                 then re-reads the process affinity mask, the thread list,
//                 the DPC-aware steering target and a frame-pacing baseline.
//   Ramping    -> a few threads per step: ideal-processor hints first (soft),
//                 then hard affinity (coolest threads first, hottest last),
//                 only if frame pacing can actually be measured.
//   Steady     -> watch only.
//   RollingBack-> pacing worsened vs. the baseline: undo changes (paced),
//   BackingOff -> wait, retry with halved aggressiveness, soft-only after the
//                 first rollback; GaveUp after maxRollbacks.
//
// Safety boundaries:
//   - Uses documented Win32 APIs only. No hooks, injection or memory access.
//   - Opens the target with PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE
//     and threads with the least rights that work (limited first, full only
//     on ACCESS_DENIED). Repeated denials stop the guard for that process.
//     Titles with kernel anti-cheat may block or flag cross-process thread
//     access: keep the executable allow-list opt-in.
//   - Every change is journaled (previous affinity / ideal processor) and
//     restored on rollback, target exit handling, Stop() and shutdown.
//   - Never touches this process's own threads (DpcSteeringEngine owns those)
//     and is disabled on systems with more than one processor group.
//
// Threading: one internal thread. All GuardConfig callbacks run on that
// thread and must be thread-safe. PollEvent()/GetStatus() may be called from
// any thread. Start()/Stop() must be driven from one controlling thread.
// =============================================================================
#pragma once

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "core/adaptive_profile.hpp"

namespace EngineCore::Core
{

// -----------------------------------------------------------------------------
// Frame-pacing statistics fed by the present-cadence ETW callback.
// Single writer (the ETW thread), lock-free, no allocation: safe to call from
// a noexcept real-time callback.
// -----------------------------------------------------------------------------
class FramePacingStats
{
public:
    struct Snapshot
    {
        bool valid = false;
        double avgFrameMs = 0.0;
        double hitchFraction = 0.0; // Fraction of recent frames > 1.6x the running average.
    };

    void OnPresentInterval(float intervalUs) noexcept
    {
        if (!(intervalUs > 0.0f) || intervalUs > 1.0e6f)
        {
            return;
        }
        const std::uint32_t n = samples_.load(std::memory_order_relaxed);
        float average = averageUs_.load(std::memory_order_relaxed);
        float hitch = hitch_.load(std::memory_order_relaxed);
        if (n == 0)
        {
            average = intervalUs;
            hitch = 0.0f;
        }
        else
        {
            const float isHitch = intervalUs > average * 1.6f ? 1.0f : 0.0f;
            hitch += 0.02f * (isHitch - hitch);
            average += 0.05f * (intervalUs - average);
        }
        averageUs_.store(average, std::memory_order_relaxed);
        hitch_.store(hitch, std::memory_order_relaxed);
        if (n < kValidAfter)
        {
            samples_.store(n + 1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] Snapshot Read() const noexcept
    {
        Snapshot s;
        s.valid = samples_.load(std::memory_order_relaxed) >= kValidAfter;
        s.avgFrameMs = static_cast<double>(averageUs_.load(std::memory_order_relaxed)) / 1000.0;
        s.hitchFraction = static_cast<double>(hitch_.load(std::memory_order_relaxed));
        return s;
    }

    // Call after re-targeting the ETW source so stale data is not trusted.
    void Reset() noexcept
    {
        samples_.store(0, std::memory_order_relaxed);
        averageUs_.store(0.0f, std::memory_order_relaxed);
        hitch_.store(0.0f, std::memory_order_relaxed);
    }

private:
    static constexpr std::uint32_t kValidAfter = 90; // ~1.5 s at 60 fps.
    std::atomic<float> averageUs_{0.0f};
    std::atomic<float> hitch_{0.0f};
    std::atomic<std::uint32_t> samples_{0};
};

enum class LaunchOrder : std::uint8_t
{
    Unknown,
    TargetStartedAfterEngine, // Process created after the guard started: still loading.
    TargetAlreadyRunning, // Process existed before the guard started.
};

enum class GuardState : std::uint8_t
{
    Disabled,
    Searching,
    Observing,
    Ramping,
    Steady,
    RollingBack,
    BackingOff,
    GaveUp, // Inactive for this process instance; resumes when a new instance appears.
};

[[nodiscard]] const char* ToString(GuardState state) noexcept;

enum class HealthSignal : std::uint8_t
{
    Unknown, // No usable frame-pacing data.
    Healthy,
    Degraded,
};

struct TargetProcessInfo
{
    DWORD pid = 0;
    std::uint64_t creationTime100ns = 0;
    std::wstring exeName;
    LaunchOrder order = LaunchOrder::Unknown;
    double ageSeconds = 0.0; // Process age when it was first seen.
};

// Desired placement, expressed in processor group 0 (the only group supported).
struct SteeringTarget
{
    WORD group = 0;
    KAFFINITY mask = 0; // Allowed CPUs for hard affinity.
    std::vector<PROCESSOR_NUMBER> idealOrder; // Coolest CPU first (soft hints).
};

enum class GuardEventKind : std::uint8_t
{
    TargetAcquired, // Re-target ETW consumers now (guard is in its quiet grace period).
    TargetLost,
};

struct GuardEvent
{
    GuardEventKind kind = GuardEventKind::TargetAcquired;
    TargetProcessInfo target;
};

struct GuardStatus
{
    GuardState state = GuardState::Disabled;
    LaunchOrder order = LaunchOrder::Unknown;
    DWORD pid = 0;
    std::uint32_t operationsApplied = 0; // Journaled changes currently in effect.
    std::uint32_t threadsTotal = 0;
    std::uint32_t rollbackCount = 0;
    double graceRemainingSec = 0.0;
};

struct GuardConfig
{
    std::vector<std::wstring> executableNames; // e.g. L"Game.exe". Empty => guard refuses to start.
    std::chrono::milliseconds pollInterval{1500};
    std::chrono::milliseconds tickInterval{250};
    std::chrono::seconds warmProcessAge{25}; // Older than this at first sight => treated as warm.
    std::chrono::seconds maxProfileWait{5}; // Then fall back to conservative parameters.
    std::uint32_t maxAccessDenied = 6;

    // --- Callbacks (invoked on the guard thread; must be thread-safe) ---
    // Tuned parameters; return nullopt while the async profile is not ready.
    std::function<std::optional<TunedParameters>()> tuning;
    // DPC-aware placement. Return nullopt if unavailable.
    std::function<std::optional<SteeringTarget>()> steeringTarget;
    // Frame pacing of the target (present cadence). May be empty.
    std::function<FramePacingStats::Snapshot()> pacing;
};

// One sampled thread of the target (CPU time used to order the ramp).
struct ThreadSample
{
    DWORD tid = 0;
    std::uint64_t cpuTime100ns = 0;
};

struct GuardHandleDeleter
{
    void operator()(HANDLE handle) const noexcept
    {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle);
        }
    }
};
using GuardHandle = std::unique_ptr<void, GuardHandleDeleter>;

class LaunchOrderGuard
{
public:
    LaunchOrderGuard() = default;
    ~LaunchOrderGuard();

    LaunchOrderGuard(const LaunchOrderGuard&) = delete;
    LaunchOrderGuard& operator=(const LaunchOrderGuard&) = delete;

    // Returns false (and stays inactive) if no executables are configured or
    // the system has more than one processor group. Idempotent while running.
    [[nodiscard]] bool Start(GuardConfig config);

    // Stops the thread and restores every change still in effect. Idempotent.
    void Stop() noexcept;

    // Non-blocking; poll once per frame from the thread that owns the ETW consumers.
    [[nodiscard]] std::optional<GuardEvent> PollEvent();

    [[nodiscard]] GuardStatus GetStatus() const;

private:
    using Clock = std::chrono::steady_clock;

    enum class Phase : std::uint8_t { Soft, Hard, Done };
    enum class JournalKind : std::uint8_t { IdealProcessor, Affinity };

    struct JournalEntry
    {
        DWORD tid = 0;
        JournalKind kind = JournalKind::IdealProcessor;
        PROCESSOR_NUMBER ideal{};
        GROUP_AFFINITY affinity{};
    };

    struct Plan
    {
        KAFFINITY hardMask = 0;
        bool hardEnabled = false;
        std::vector<PROCESSOR_NUMBER> idealOrder;
    };

    // ---- Guard-thread only ----
    void Run(std::stop_token stop);
    void Tick();
    void TickSearching(Clock::time_point now);
    void TickObserving(Clock::time_point now);
    void TickRamping(Clock::time_point now);
    void TickSteady(Clock::time_point now);
    void TickRollingBack(Clock::time_point now);
    void TickBackingOff(Clock::time_point now);

    [[nodiscard]] DWORD FindTargetPid();
    [[nodiscard]] bool Attach(DWORD pid, Clock::time_point now);
    [[nodiscard]] bool TargetStillAlive() const noexcept;
    void BeginRamp(Clock::time_point now, const TunedParameters& tp);
    void ApplyBatch(const TunedParameters& tp);
    void EnterHardPhase(const TunedParameters& tp);
    void ApplySoft(const ThreadSample& thread, std::size_t index);
    void ApplyHard(const ThreadSample& thread);
    void NoteDenied();
    void BeginRollback(bool thenGiveUp);
    void RestoreOne(const JournalEntry& entry);
    void RestoreSome(std::size_t count);
    void RestoreAll(bool paced);
    void ResetTargetState();

    [[nodiscard]] TunedParameters Effective() const;
    [[nodiscard]] HealthSignal EvaluateHealth() const;
    [[nodiscard]] bool DegradedForTooLong(std::uint32_t ticksRequired);

    void SetState(GuardState state);
    void PublishStatus(Clock::time_point now);
    void PushEvent(GuardEventKind kind);

    GuardConfig config_;
    std::uint64_t engineStartFileTime_ = 0;

    GuardState state_ = GuardState::Disabled;
    GuardHandle process_;
    TargetProcessInfo target_;
    std::wstring foundExeName_;
    std::optional<TunedParameters> tuned_;

    Clock::time_point nextScan_{};
    Clock::time_point observeStart_{};
    Clock::time_point graceEndsAt_{};
    Clock::time_point nextStep_{};
    Clock::time_point cooldownUntil_{};

    Plan plan_;
    Phase phase_ = Phase::Done;
    std::vector<ThreadSample> threads_;
    std::size_t softIndex_ = 0;
    std::size_t hardIndex_ = 0;
    std::vector<JournalEntry> journal_;

    FramePacingStats::Snapshot baseline_;
    bool baselineValid_ = false;
    std::uint32_t degradedStreak_ = 0;
    std::uint32_t rollbackCount_ = 0;
    std::uint32_t deniedCount_ = 0;
    bool giveUpAfterRollback_ = false;

    // ---- Shared with other threads ----
    mutable std::mutex statusMutex_;
    GuardStatus status_;

    std::mutex eventMutex_;
    std::deque<GuardEvent> events_;

    std::mutex wakeMutex_;
    std::condition_variable_any wakeCv_;

    std::jthread thread_; // Declared last: destroyed (joined) first.
};

} // namespace EngineCore::Core