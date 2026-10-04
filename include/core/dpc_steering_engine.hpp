// =============================================================================
// EngineCore :: Real-Time DPC / IRQ Isolation & Thread Steering Engine
// =============================================================================
#pragma once

#include <windows.h>
#include <evntcons.h>
#include <evntrace.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "core/thread_steering.hpp"

namespace EngineCore::Core
{

class EtwSession;

struct DpcSteeringConfig
{
    std::wstring sessionName = L"EngineCore-DpcIsrTrace";
    ULONG traceBufferSizeKb = 64;
    std::chrono::milliseconds evaluationInterval{1000};
    std::chrono::milliseconds minReassignDwell{5000};
    double hotThreshold = 0.04;
    double coolThreshold = 0.02;
    bool avoidCpu0 = true;
    bool preferPerformanceCores = true;
};

struct CoreCongestion
{
    PROCESSOR_NUMBER cpu{};
    double dpcFraction = 0.0;
    double isrFraction = 0.0;
    std::uint64_t dpcCount = 0;
    std::uint64_t isrCount = 0;
    double maxDpcMicros = 0.0;
    bool hot = false;
};

struct CongestionSnapshot
{
    std::vector<CoreCongestion> cores;
    std::chrono::steady_clock::time_point capturedAt{};
    bool traceActive = false;
    std::uint64_t rejectedEvents = 0;
};

class DpcSteeringEngine
{
    friend class EtwSession;

public:
    // نقل CpuCounters لجزء الـ public يتيح لـ EtwSession ولمكونات النظام رؤيته
    struct alignas(64) CpuCounters
    {
        std::atomic<std::uint64_t> dpcTicks{0};
        std::atomic<std::uint64_t> isrTicks{0};
        std::atomic<std::uint64_t> dpcCount{0};
        std::atomic<std::uint64_t> isrCount{0};
        std::atomic<std::uint64_t> maxDpcTicks{0};
        std::uint64_t reservedPadding[3]{};
    };

    explicit DpcSteeringEngine(DpcSteeringConfig config = {});
    ~DpcSteeringEngine();

    DpcSteeringEngine(const DpcSteeringEngine&) = delete;
    DpcSteeringEngine& operator=(const DpcSteeringEngine&) = delete;
    DpcSteeringEngine(DpcSteeringEngine&&) = delete;
    DpcSteeringEngine& operator=(DpcSteeringEngine&&) = delete;

    [[nodiscard]] std::error_code Start();
    void Stop() noexcept;

    bool RegisterCriticalThread(DWORD threadId);
    bool RegisterCurrentThread();
    void UnregisterCriticalThread(DWORD threadId);

    [[nodiscard]] CongestionSnapshot GetSnapshot() const;
    [[nodiscard]] bool IsTraceActive() const noexcept { return traceActive_.load(std::memory_order_acquire); }

    // إتاحة HandleEvent لقراءتها من قِبل الـ EventRecordCallback الخاص بـ ETW
    void HandleEvent(const EVENT_RECORD& record) noexcept;

private:
    struct PrevCounters
    {
        std::uint64_t dpcTicks = 0;
        std::uint64_t isrTicks = 0;
        std::uint64_t dpcCount = 0;
        std::uint64_t isrCount = 0;
    };

    struct SteeringPlan
    {
        WORD group = 0;
        KAFFINITY mask = 0;
        std::vector<PROCESSOR_NUMBER> idealOrder;

        [[nodiscard]] bool Valid() const noexcept { return mask != 0 && !idealOrder.empty(); }
    };

    static VOID WINAPI OnEventRecord(PEVENT_RECORD record) noexcept;

    void SteeringLoop(std::stop_token token);
    void ResetBaseline();
    void EvaluateAndSteer();
    void ApplySteering(const CongestionSnapshot& snapshot);
    [[nodiscard]] SteeringPlan BuildPlan(const CongestionSnapshot& snapshot) const;
    [[nodiscard]] std::optional<std::size_t> FlatIndex(WORD group, std::uint32_t index) const noexcept;

    DpcSteeringConfig config_;
    CpuTopology topology_;
    std::vector<CpuCounters> counters_;
    std::vector<LogicalCoreInfo> candidates_;
    std::vector<PROCESSOR_NUMBER> cpuMap_;
    std::vector<std::size_t> groupBase_;
    std::uint64_t qpcFrequency_ = 0;
    std::uint64_t maxSaneEventTicks_ = 0;

    std::atomic<std::uint64_t> rejectedEvents_{0};
    std::atomic<bool> traceActive_{false};

    std::vector<PrevCounters> previous_;
    std::vector<char> hot_;
    std::uint64_t lastWindowTicks_ = 0;
    std::optional<SteeringPlan> lastPlan_;
    std::optional<std::chrono::steady_clock::time_point> lastApply_;

    mutable std::mutex threadsMutex_;
    std::vector<DWORD> criticalThreads_;
    std::atomic<bool> registrationDirty_{false};

    mutable std::mutex snapshotMutex_;
    CongestionSnapshot snapshot_;

    ThreadSteering steering_;
    std::unique_ptr<EtwSession> trace_;

    std::mutex wakeMutex_;
    std::condition_variable_any wakeCv_;
    std::jthread steeringThread_;
};

} // namespace EngineCore::Core