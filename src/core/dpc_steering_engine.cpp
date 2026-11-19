// =============================================================================
// EngineCore :: Real-Time DPC / IRQ Isolation & Thread Steering Implementation
// =============================================================================
#include "core/dpc_steering_engine.hpp"

#define INITGUID
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <sddl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <numeric>
#include <system_error>

#pragma comment(lib, "advapi32.lib")

namespace EngineCore::Core
{

// Static assertion to ensure 64-byte alignment per logical CPU structure (cache line isolation).
static_assert(sizeof(DpcSteeringEngine::CpuCounters) == 64, "CpuCounters structure size must be exactly 64 bytes");

// GUIDs for Windows Kernel Trace Control DPC & ISR Event Identifiers
// {2c101032-98e7-41a3-be3b-721f316422c6} - SystemTraceControlGuid
static constexpr GUID KERNEL_DPC_ISR_GUID = { 0x2c101032, 0x98e7, 0x41a3, { 0xbe, 0x3b, 0x72, 0x1f, 0x31, 0x64, 0x22, 0xc6 } };

class EtwSession
{
public:
    EtwSession(std::wstring name, ULONG bufferSizeKb, DpcSteeringEngine* parent)
        : sessionName_(std::move(name)), bufferSizeKb_(bufferSizeKb), parent_(parent)
    {
    }

    ~EtwSession()
    {
        Stop();
    }

    std::error_code Start()
    {
        ULONG propertiesSize = sizeof(EVENT_TRACE_PROPERTIES) + static_cast<ULONG>((sessionName_.size() + 1) * sizeof(wchar_t));
        propertiesBuffer_.resize(propertiesSize, 0);

        auto* pProperties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertiesBuffer_.data());
        pProperties->Wnode.BufferSize = propertiesSize;
        pProperties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        pProperties->Wnode.ClientContext = 1; // QPC Clock Resolution
        pProperties->BufferSize = bufferSizeKb_;
        // A custom-named session only receives kernel DPC/ISR events when it is a
        // "system logger" session (Windows 8+). Without EVENT_TRACE_SYSTEM_LOGGER_MODE
        // EnableFlags is ignored for a non-"NT Kernel Logger" name, the trace starts
        // fine and then delivers nothing - which is why the DPC plot always read 0.
        pProperties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_SYSTEM_LOGGER_MODE;
        pProperties->EnableFlags = EVENT_TRACE_FLAG_DPC | EVENT_TRACE_FLAG_INTERRUPT;
        pProperties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

        // Stop any stale orphaned session with the same identifier
        ControlTraceW(0, sessionName_.c_str(), pProperties, EVENT_TRACE_CONTROL_STOP);

        ULONG status = StartTraceW(&sessionHandle_, sessionName_.c_str(), pProperties);
        if (status != ERROR_SUCCESS)
        {
            return std::error_code(static_cast<int>(status), std::system_category());
        }

        consumerThread_ = std::jthread([this](std::stop_token token) {
            RunConsumer(token);
        });

        return {};
    }

    void Stop() noexcept
    {
        if (sessionHandle_ != 0)
        {
            auto* pProperties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propertiesBuffer_.data());
            ControlTraceW(sessionHandle_, sessionName_.c_str(), pProperties, EVENT_TRACE_CONTROL_STOP);
            sessionHandle_ = 0;
        }

        if (consumerThread_.joinable())
        {
            consumerThread_.request_stop();
            consumerThread_.join();
        }
    }

private:
    void RunConsumer(std::stop_token token)
    {
        EVENT_TRACE_LOGFILEW logFile{};
        logFile.LoggerName = const_cast<LPWSTR>(sessionName_.c_str());
        logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        logFile.Context = parent_;
        logFile.EventRecordCallback = [](PEVENT_RECORD record) {
            if (record && record->UserContext)
            {
                auto* engine = static_cast<DpcSteeringEngine*>(record->UserContext);
                engine->HandleEvent(*record);
            }
        };

        TRACEHANDLE traceHandle = OpenTraceW(&logFile);
        if (traceHandle == INVALID_PROCESSTRACE_HANDLE)
        {
            return;
        }

        while (!token.stop_requested())
        {
            ULONG status = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
            if (status != ERROR_SUCCESS)
            {
                break;
            }
        }

        CloseTrace(traceHandle);
    }

    std::wstring sessionName_;
    ULONG bufferSizeKb_;
    DpcSteeringEngine* parent_;
    TRACEHANDLE sessionHandle_ = 0;
    std::vector<std::uint8_t> propertiesBuffer_;
    std::jthread consumerThread_;
};

DpcSteeringEngine::DpcSteeringEngine(DpcSteeringConfig config)
    : config_(std::move(config)), topology_(CpuTopology::Query())
{
    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    qpcFrequency_ = static_cast<std::uint64_t>(qpcFreq.QuadPart);
    maxSaneEventTicks_ = qpcFrequency_ * 2; // 2 seconds safety threshold for corrupt events

    std::size_t totalCores = 0;
    std::uint32_t currentGroup = 0;

    for (const auto& core : topology_.Cores())
    {
        if (core.group >= groupBase_.size())
        {
            groupBase_.resize(core.group + 1, totalCores);
        }
        cpuMap_.push_back(PROCESSOR_NUMBER{ static_cast<WORD>(core.group), static_cast<UCHAR>(core.index) });
        
        if (config_.avoidCpu0 && core.group == 0 && core.index == 0)
        {
            // Omit CPU 0 from candidate pools
        }
        else if (!config_.preferPerformanceCores || core.coreClass == CoreClass::PerformanceCore)
        {
            candidates_.push_back(core);
        }

        totalCores++;
    }

    counters_ = std::vector<CpuCounters>(totalCores);
    previous_.resize(totalCores);
    hot_.resize(totalCores, 0);

    snapshot_.cores.resize(totalCores);
    for (std::size_t i = 0; i < totalCores; ++i)
    {
        snapshot_.cores[i].cpu = cpuMap_[i];
    }
}

DpcSteeringEngine::~DpcSteeringEngine()
{
    Stop();
}

std::error_code DpcSteeringEngine::Start()
{
    if (steeringThread_.joinable())
    {
        return {};
    }

    trace_ = std::make_unique<EtwSession>(config_.sessionName, config_.traceBufferSizeKb, this);
    std::error_code etwErr = trace_->Start();

    traceActive_.store(!etwErr, std::memory_order_release);

    steeringThread_ = std::jthread([this](std::stop_token token) {
        SteeringLoop(token);
    });

    return etwErr;
}

void DpcSteeringEngine::Stop() noexcept
{
    if (trace_)
    {
        trace_->Stop();
        trace_.reset();
    }

    traceActive_.store(false, std::memory_order_release);

    if (steeringThread_.joinable())
    {
        steeringThread_.request_stop();
        wakeCv_.notify_all();
        steeringThread_.join();
    }
}

bool DpcSteeringEngine::RegisterCriticalThread(DWORD threadId)
{
    HANDLE hThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId);
    if (!hThread)
    {
        return false;
    }

    DWORD targetProcId = GetProcessIdOfThread(hThread);
    CloseHandle(hThread);

    if (targetProcId != GetCurrentProcessId())
    {
        return false; // Security boundary violation check
    }

    std::lock_guard lock(threadsMutex_);
    if (std::find(criticalThreads_.begin(), criticalThreads_.end(), threadId) == criticalThreads_.end())
    {
        criticalThreads_.push_back(threadId);
        registrationDirty_.store(true, std::memory_order_release);
    }

    return true;
}

bool DpcSteeringEngine::RegisterCurrentThread()
{
    return RegisterCriticalThread(GetCurrentThreadId());
}

void DpcSteeringEngine::UnregisterCriticalThread(DWORD threadId)
{
    std::lock_guard lock(threadsMutex_);
    auto it = std::remove(criticalThreads_.begin(), criticalThreads_.end(), threadId);
    if (it != criticalThreads_.end())
    {
        criticalThreads_.erase(it, criticalThreads_.end());
        registrationDirty_.store(true, std::memory_order_release);
    }
}

CongestionSnapshot DpcSteeringEngine::GetSnapshot() const
{
    std::lock_guard lock(snapshotMutex_);
    return snapshot_;
}

void DpcSteeringEngine::HandleEvent(const EVENT_RECORD& record) noexcept
{
    // PerfInfo kernel events: ThreadedDPC=66, ISR=67, DPC=68, TimerDPC=69.
    // Payload starts with InitialTime (QPC ticks when the routine began); the
    // event itself is stamped when the routine ENDS, so
    //     duration = EventHeader.TimeStamp - InitialTime.
    // (The old code read the first 8 payload bytes as the duration itself; that
    // value is an absolute timestamp, always > maxSaneEventTicks_, so every
    // event was rejected.)
    const UCHAR opcode = record.EventHeader.EventDescriptor.Opcode;
    const std::uint32_t cpuIndex = record.BufferContext.ProcessorIndex;

    if (cpuIndex >= counters_.size())
    {
        rejectedEvents_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Process duration payloads if event payload is large enough
    if (record.UserDataLength >= sizeof(std::uint64_t))
    {
        std::uint64_t initialTicks = 0;
        std::memcpy(&initialTicks, record.UserData, sizeof(initialTicks)); // unaligned-safe
        const auto endTicks = static_cast<std::uint64_t>(record.EventHeader.TimeStamp.QuadPart);
        if (endTicks <= initialTicks || (endTicks - initialTicks) > maxSaneEventTicks_)
        {
            rejectedEvents_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::uint64_t durationTicks = endTicks - initialTicks;

        auto& targetCore = counters_[cpuIndex];
        if (opcode == 66 || opcode == 68 || opcode == 69) // (Threaded/Timer) DPC routine execution
        {
            targetCore.dpcTicks.fetch_add(durationTicks, std::memory_order_relaxed);
            targetCore.dpcCount.fetch_add(1, std::memory_order_relaxed);

            // Update Max DPC metric atomically
            std::uint64_t currentMax = targetCore.maxDpcTicks.load(std::memory_order_relaxed);
            while (durationTicks > currentMax && 
                   !targetCore.maxDpcTicks.compare_exchange_weak(currentMax, durationTicks, std::memory_order_relaxed))
            {}
        }
        else if (opcode == 67) // ISR Routine execution
        {
            targetCore.isrTicks.fetch_add(durationTicks, std::memory_order_relaxed);
            targetCore.isrCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void DpcSteeringEngine::SteeringLoop(std::stop_token token)
{
    ResetBaseline();

    while (!token.stop_requested())
    {
        std::unique_lock lock(wakeMutex_);
        wakeCv_.wait_for(lock, token, config_.evaluationInterval, [] { return false; });

        if (token.stop_requested())
        {
            break;
        }

        EvaluateAndSteer();
    }
}

void DpcSteeringEngine::ResetBaseline()
{
    for (std::size_t i = 0; i < counters_.size(); ++i)
    {
        previous_[i].dpcTicks = counters_[i].dpcTicks.load(std::memory_order_relaxed);
        previous_[i].isrTicks = counters_[i].isrTicks.load(std::memory_order_relaxed);
        previous_[i].dpcCount = counters_[i].dpcCount.load(std::memory_order_relaxed);
        previous_[i].isrCount = counters_[i].isrCount.load(std::memory_order_relaxed);
    }
    lastWindowTicks_ = QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&lastWindowTicks_)) ? lastWindowTicks_ : 0;
}

void DpcSteeringEngine::EvaluateAndSteer()
{
    LARGE_INTEGER nowQpc{};
    QueryPerformanceCounter(&nowQpc);

    std::uint64_t elapsedTicks = static_cast<std::uint64_t>(nowQpc.QuadPart) - lastWindowTicks_;
    if (elapsedTicks == 0 || qpcFrequency_ == 0)
    {
        return;
    }

    lastWindowTicks_ = static_cast<std::uint64_t>(nowQpc.QuadPart);
    double windowTimeSeconds = static_cast<double>(elapsedTicks) / static_cast<double>(qpcFrequency_);

    CongestionSnapshot newSnapshot{};
    newSnapshot.capturedAt = std::chrono::steady_clock::now();
    newSnapshot.traceActive = IsTraceActive();
    newSnapshot.rejectedEvents = rejectedEvents_.load(std::memory_order_relaxed);
    newSnapshot.cores.resize(counters_.size());

    for (std::size_t i = 0; i < counters_.size(); ++i)
    {
        std::uint64_t curDpcTicks = counters_[i].dpcTicks.load(std::memory_order_relaxed);
        std::uint64_t curIsrTicks = counters_[i].isrTicks.load(std::memory_order_relaxed);
        std::uint64_t curDpcCount = counters_[i].dpcCount.load(std::memory_order_relaxed);
        std::uint64_t curIsrCount = counters_[i].isrCount.load(std::memory_order_relaxed);
        std::uint64_t maxDpc = counters_[i].maxDpcTicks.exchange(0, std::memory_order_relaxed);

        std::uint64_t deltaDpcTicks = curDpcTicks - previous_[i].dpcTicks;
        std::uint64_t deltaIsrTicks = curIsrTicks - previous_[i].isrTicks;

        previous_[i].dpcTicks = curDpcTicks;
        previous_[i].isrTicks = curIsrTicks;
        previous_[i].dpcCount = curDpcCount;
        previous_[i].isrCount = curIsrCount;

        auto& coreSnap = newSnapshot.cores[i];
        coreSnap.cpu = cpuMap_[i];
        coreSnap.dpcCount = curDpcCount;
        coreSnap.isrCount = curIsrCount;
        coreSnap.dpcFraction = (static_cast<double>(deltaDpcTicks) / static_cast<double>(qpcFrequency_)) / windowTimeSeconds;
        coreSnap.isrFraction = (static_cast<double>(deltaIsrTicks) / static_cast<double>(qpcFrequency_)) / windowTimeSeconds;
        coreSnap.maxDpcMicros = (static_cast<double>(maxDpc) / static_cast<double>(qpcFrequency_)) * 1000000.0;

        // Apply Congestion Hysteresis Filter
        double totalFraction = coreSnap.dpcFraction + coreSnap.isrFraction;
        if (hot_[i])
        {
            if (totalFraction < config_.coolThreshold)
            {
                hot_[i] = 0;
            }
        }
        else
        {
            if (totalFraction > config_.hotThreshold)
            {
                hot_[i] = 1;
            }
        }
        coreSnap.hot = (hot_[i] != 0);
    }

    {
        double worstUs = 0.0;
        for (const auto& c : newSnapshot.cores) worstUs = std::max(worstUs, c.maxDpcMicros);
        worstDpcMicros_.store(worstUs, std::memory_order_relaxed); // lock-free readout for the UI/orchestrator

        std::lock_guard lock(snapshotMutex_);
        snapshot_ = newSnapshot; // copy: ApplySteering() below still reads newSnapshot
    }

    ApplySteering(newSnapshot);
}

void DpcSteeringEngine::ApplySteering(const CongestionSnapshot& snapshot)
{
    auto now = std::chrono::steady_clock::now();
    bool dirty = registrationDirty_.exchange(false, std::memory_order_acq_rel);

    if (!dirty && lastApply_.has_value() && (now - *lastApply_) < config_.minReassignDwell)
    {
        return;
    }

    SteeringPlan plan = BuildPlan(snapshot);
    if (!plan.Valid())
    {
        return;
    }

    std::vector<DWORD> threadsToSteer;
    {
        std::lock_guard lock(threadsMutex_);
        threadsToSteer = criticalThreads_;
    }

    for (DWORD tid : threadsToSteer)
    {
        HANDLE hThread = OpenThread(THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (hThread)
        {
            GROUP_AFFINITY ga{};
            ga.Group = plan.group;
            ga.Mask = plan.mask;
            SetThreadGroupAffinity(hThread, &ga, nullptr);

            if (!plan.idealOrder.empty())
            {
                SetThreadIdealProcessorEx(hThread, &plan.idealOrder[0], nullptr);
            }

            CloseHandle(hThread);
        }
    }

    lastPlan_ = plan;
    lastApply_ = now;
}

DpcSteeringEngine::SteeringPlan DpcSteeringEngine::BuildPlan(const CongestionSnapshot& snapshot) const
{
    SteeringPlan plan{};
    if (candidates_.empty())
    {
        return plan;
    }

    std::vector<LogicalCoreInfo> coolCandidates;
    for (const auto& candidate : candidates_)
    {
        auto flatIdx = FlatIndex(static_cast<WORD>(candidate.group), candidate.index);
        if (flatIdx.has_value() && *flatIdx < snapshot.cores.size())
        {
            if (!snapshot.cores[*flatIdx].hot)
            {
                coolCandidates.push_back(candidate);
            }
        }
    }

    // Fallback if all candidate cores are hot
    const auto& pool = coolCandidates.empty() ? candidates_ : coolCandidates;

    plan.group = static_cast<WORD>(pool.front().group);
    for (const auto& core : pool)
    {
        if (core.group == plan.group)
        {
            plan.mask |= core.groupAffinityMask;
            plan.idealOrder.push_back(PROCESSOR_NUMBER{ static_cast<WORD>(core.group), static_cast<UCHAR>(core.index) });
        }
    }

    return plan;
}

std::optional<std::size_t> DpcSteeringEngine::FlatIndex(WORD group, std::uint32_t index) const noexcept
{
    if (group < groupBase_.size())
    {
        std::size_t base = groupBase_[group];
        std::size_t candidateIndex = base + index;
        if (candidateIndex < cpuMap_.size() && cpuMap_[candidateIndex].Group == group && cpuMap_[candidateIndex].Number == index)
        {
            return candidateIndex;
        }
    }
    return std::nullopt;
}

} // namespace EngineCore::Core