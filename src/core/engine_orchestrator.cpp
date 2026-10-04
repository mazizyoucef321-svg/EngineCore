// =============================================================================
//  EngineCore :: Central Orchestrator implementation
//  src/core/engine_orchestrator.cpp
//
//  Build: MSVC /std:c++20 /W4, link advapi32.lib (pragma below).
// =============================================================================
#include "core/engine_orchestrator.hpp"

#include <evntrace.h>
#include <evntcons.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace enginecore {
namespace {

// -----------------------------------------------------------------------------
//  Time helpers (QPC). ETW is configured with ClientContext=1 so its timestamps
//  are QPC ticks and directly comparable with QueryPerformanceCounter.
// -----------------------------------------------------------------------------
[[nodiscard]] std::int64_t QpcFrequency() noexcept {
    static const std::int64_t f = [] {
        LARGE_INTEGER v{};
        ::QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return f;
}

[[nodiscard]] std::int64_t NowQpc() noexcept {
    LARGE_INTEGER v{};
    ::QueryPerformanceCounter(&v);
    return v.QuadPart;
}

[[nodiscard]] double QpcToMs(std::int64_t ticks) noexcept {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(QpcFrequency());
}

[[nodiscard]] double QpcToUs(std::int64_t ticks) noexcept {
    return static_cast<double>(ticks) * 1'000'000.0 / static_cast<double>(QpcFrequency());
}

// -----------------------------------------------------------------------------
//  RAII handle
// -----------------------------------------------------------------------------
struct HandleCloser {
    void operator()(HANDLE h) const noexcept {
        if (h != nullptr && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

// -----------------------------------------------------------------------------
//  SeqLock: single-writer / multi-reader publication of a trivially-copyable T.
//  Payload is stored as relaxed atomic 64-bit words, so there is no formal data
//  race (unlike memcpy-based seqlocks). Writer is wait-free; readers retry only
//  while the writer is mid-store (a handful of stores).
// -----------------------------------------------------------------------------
template <class T>
class SeqLock {
    static_assert(std::is_trivially_copyable_v<T>, "SeqLock payload must be trivially copyable");
    static_assert(sizeof(T) % sizeof(std::uint64_t) == 0, "SeqLock payload size must be a multiple of 8");
    static constexpr std::size_t kWords = sizeof(T) / sizeof(std::uint64_t);

public:
    void Store(const T& value) noexcept {
        std::array<std::uint64_t, kWords> w{};
        std::memcpy(w.data(), &value, sizeof(T));

        const std::uint64_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);           // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < kWords; ++i)
            words_[i].store(w[i], std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);           // even: stable
    }

    void Load(T& out) const noexcept {
        std::array<std::uint64_t, kWords> w{};
        for (;;) {
            const std::uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) { ::YieldProcessor(); continue; }
            for (std::size_t i = 0; i < kWords; ++i)
                w[i] = words_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s1) break;
            ::YieldProcessor();
        }
        std::memcpy(&out, w.data(), sizeof(T));
    }

private:
    std::atomic<std::uint64_t> seq_{0};
    std::array<std::atomic<std::uint64_t>, kWords> words_{};
};

// -----------------------------------------------------------------------------
//  Shared counters (all lock-free atomics; relaxed is enough for telemetry)
// -----------------------------------------------------------------------------
struct Counters {
    // ETW
    std::atomic<std::uint64_t> etwEvents{0};
    std::atomic<std::uint64_t> presentCount{0};
    std::atomic<std::int64_t>  lastPresentStartQpc{0};
    std::atomic<std::uint32_t> lastIntervalUs{0};
    std::atomic<std::uint32_t> emaIntervalUs{0};
    std::atomic<std::uint32_t> presentDurationUs{0};

    // Early warning
    std::atomic<std::uint64_t> ewSent{0};
    std::atomic<float>         ewLastProgress{0.0f};
    std::atomic<std::uint32_t> ewDispatchCostNs{0};

    // Pacing
    std::atomic<std::uint64_t> lateFrames{0};
    std::atomic<std::uint64_t> generated{0};
    std::atomic<std::uint64_t> genRejected{0};
    std::atomic<std::uint32_t> consecutive{0};
    std::atomic<std::int64_t>  budgetWonUs{0};

    // Shader predictor
    std::atomic<std::uint64_t> psoEnqueued{0};
    std::atomic<std::uint64_t> purges{0};
    std::atomic<std::uint64_t> urgentPasses{0};
    std::atomic<std::uint64_t> generation{0};

    // Health
    std::atomic<std::uint64_t> agentFaults{0};
};

// Everything the agents & ETW pump share. Owned by Impl, outlives all threads.
struct SharedState {
    OrchestratorConfig cfg;
    EngineOrchestrator::Dependencies deps;
    SeqLock<FrameInput> input;
    Counters c;
};

// -----------------------------------------------------------------------------
//  AgentBase: RAII jthread that sleeps on an atomic (WaitOnAddress) and runs
//  OnWake() each time it is signalled. No polling, no mutexes.
//
//  IMPORTANT: every concrete agent's destructor calls Stop() so the thread is
//  joined before the derived members are destroyed.
// -----------------------------------------------------------------------------
class AgentBase {
public:
    AgentBase(std::wstring name, int threadPriority, std::atomic<std::uint64_t>& faultCounter)
        : name_(std::move(name)), priority_(threadPriority), faults_(faultCounter) {}

    virtual ~AgentBase() = default;
    AgentBase(const AgentBase&)            = delete;
    AgentBase& operator=(const AgentBase&) = delete;

    void Start() {
        thread_ = std::jthread([this](std::stop_token st) { Run(st); });
    }

    void Stop() noexcept {
        if (thread_.joinable()) {
            thread_.request_stop();
            Wake();               // guarantee the wait() returns
            thread_.join();
        }
    }

    /// Non-blocking: bump the signal word and wake the sleeper.
    void Wake() noexcept {
        signal_.fetch_add(1, std::memory_order_release);
        signal_.notify_one();
    }

protected:
    virtual void OnWake() = 0;

private:
    void Run(std::stop_token st) noexcept {
        ::SetThreadDescription(::GetCurrentThread(), name_.c_str());
        ::SetThreadPriority(::GetCurrentThread(), priority_);

        std::uint32_t seen = 0;
        while (!st.stop_requested()) {
            signal_.wait(seen, std::memory_order_acquire);   // blocks while signal_ == seen
            seen = signal_.load(std::memory_order_acquire);
            if (st.stop_requested()) break;
            try {
                OnWake();
            } catch (...) {
                faults_.fetch_add(1, std::memory_order_relaxed); // never let an agent kill the process
            }
        }
    }

    std::wstring                        name_;
    int                                 priority_;
    std::atomic<std::uint64_t>&         faults_;
    std::atomic<std::uint32_t>          signal_{0};
    std::jthread                        thread_;
};

// =============================================================================
//  AGENT 1: EarlySpeculativePipelineWarningAgent
//  Fires exactly once per frame when CPU progress crosses ~10%.
// =============================================================================
class EarlySpeculativePipelineWarningAgent final : public AgentBase {
public:
    explicit EarlySpeculativePipelineWarningAgent(SharedState& s)
        : AgentBase(L"EngineCore.EarlyWarning", THREAD_PRIORITY_ABOVE_NORMAL, s.c.agentFaults), s_(s) {}
    ~EarlySpeculativePipelineWarningAgent() override { Stop(); }

protected:
    void OnWake() override {
        FrameInput in{};
        s_.input.Load(in);

        if (in.frameIndex != lastFrame_) {      // new frame: re-arm
            lastFrame_ = in.frameIndex;
            fired_     = false;
        }
        if (fired_ || in.cpuFrameProgress < s_.cfg.earlyWarningProgress) return;
        fired_ = true;

        auto& sink = s_.deps.gpuHintSink;
        if (!sink) return;

        float pressure = 0.0f;
        if (s_.deps.vramMonitor) pressure = s_.deps.vramMonitor->QueryStats().pressure;

        NextFrameHint hint{};
        hint.targetFrameIndex       = in.frameIndex + 1;
        hint.timestampQpc           = static_cast<std::uint64_t>(NowQpc());
        hint.cpuProgressAtTrigger   = in.cpuFrameProgress;
        hint.expectedFrameTimeMs    = in.targetFrameTimeMs > 0.0f ? in.targetFrameTimeMs
                                                                  : s_.cfg.defaultTargetFrameTimeMs;
        hint.vramPressure           = pressure;
        hint.preferReducedResidency = pressure >= s_.cfg.highVramPressure;

        const std::int64_t t0 = NowQpc();
        sink->PrepareNextFramePipeline(hint);
        const std::int64_t t1 = NowQpc();

        s_.c.ewLastProgress.store(in.cpuFrameProgress, std::memory_order_relaxed);
        s_.c.ewDispatchCostNs.store(static_cast<std::uint32_t>(QpcToUs(t1 - t0) * 1000.0),
                                    std::memory_order_relaxed);
        s_.c.ewSent.fetch_add(1, std::memory_order_relaxed);
    }

private:
    SharedState&  s_;
    std::uint64_t lastFrame_{~0ull};
    bool          fired_{false};
};

// =============================================================================
//  AGENT 2: FramePacingAgent
//  Detects late frame delivery (ETW present cadence, fallback: CPU frame time)
//  and requests generated/blended frames, bounded by a consecutive-frame guard.
// =============================================================================
class FramePacingAgent final : public AgentBase {
public:
    explicit FramePacingAgent(SharedState& s)
        : AgentBase(L"EngineCore.FramePacing", THREAD_PRIORITY_NORMAL, s.c.agentFaults), s_(s) {}
    ~FramePacingAgent() override { Stop(); }

protected:
    void OnWake() override {
        FrameInput in{};
        s_.input.Load(in);

        const std::uint64_t presents = s_.c.presentCount.load(std::memory_order_relaxed);
        if (in.frameIndex == lastFrame_ && presents == lastPresents_) return; // nothing new
        lastFrame_    = in.frameIndex;
        lastPresents_ = presents;

        const double targetMs = in.targetFrameTimeMs > 0.0f ? in.targetFrameTimeMs
                                                            : s_.cfg.defaultTargetFrameTimeMs;

        // Prefer real display-path timing from ETW when it is fresh.
        const std::int64_t lastStart = s_.c.lastPresentStartQpc.load(std::memory_order_relaxed);
        const bool etwFresh = lastStart != 0 &&
                              QpcToMs(NowQpc() - lastStart) < s_.cfg.etwFreshnessMs;
        const double measuredMs = etwFresh
            ? s_.c.lastIntervalUs.load(std::memory_order_relaxed) / 1000.0
            : static_cast<double>(in.cpuFrameTimeMs);
        if (measuredMs <= 0.0) return;

        const bool late = measuredMs > targetMs * s_.cfg.lateTolerance;

        if (!late) {
            if (consecutive_ != 0) {
                consecutive_ = 0;
                s_.c.consecutive.store(0, std::memory_order_relaxed);
            }
            SetCriticalPhase(false);
            return;
        }

        s_.c.lateFrames.fetch_add(1, std::memory_order_relaxed);
        SetCriticalPhase(true);

        const double deficitMs = measuredMs - targetMs;

        if (consecutive_ >= s_.cfg.maxConsecutiveGenerated || !s_.deps.frameGeneration) {
            s_.c.genRejected.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Budget estimate: the generated frame fills the display slot for a small
        // fixed cost, letting the real pipeline catch up by (slot - genCost).
        const double wonMs = std::max(0.0, std::min(deficitMs, targetMs) -
                                           static_cast<double>(s_.cfg.frameGenCostMs));

        FrameGenRequest rq{};
        rq.frameIndex       = in.frameIndex;
        rq.deficitMs        = static_cast<float>(deficitMs);
        rq.blendWeight      = static_cast<float>(std::clamp(deficitMs / targetMs, 0.25, 1.0));
        rq.recoveryBudgetMs = static_cast<float>(wonMs);

        if (s_.deps.frameGeneration->SubmitGeneratedFrame(rq)) {
            ++consecutive_;
            s_.c.consecutive.store(consecutive_, std::memory_order_relaxed);
            s_.c.generated.fetch_add(1, std::memory_order_relaxed);
            s_.c.budgetWonUs.fetch_add(static_cast<std::int64_t>(wonMs * 1000.0),
                                       std::memory_order_relaxed);
        } else {
            s_.c.genRejected.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    void SetCriticalPhase(bool active) noexcept {
        if (criticalPhase_ == active) return;
        criticalPhase_ = active;
        if (s_.deps.dpcSteering) s_.deps.dpcSteering->SetLatencyCriticalPhase(active);
    }

    SharedState&  s_;
    std::uint64_t lastFrame_{~0ull};
    std::uint64_t lastPresents_{~0ull};
    std::uint32_t consecutive_{0};
    bool          criticalPhase_{false};
};

// =============================================================================
//  AGENT 3: ShaderCompilationPredictorAgent
//  Derives heading from position deltas, projects a ray of streaming cells, and
//  requests PSO compilation. On an abrupt turn: purge -> new generation ->
//  urgent compile pass for the new target area.
// =============================================================================
class ShaderCompilationPredictorAgent final : public AgentBase {
public:
    explicit ShaderCompilationPredictorAgent(SharedState& s)
        : AgentBase(L"EngineCore.ShaderPredictor", THREAD_PRIORITY_BELOW_NORMAL, s.c.agentFaults), s_(s) {}
    ~ShaderCompilationPredictorAgent() override { Stop(); }

protected:
    void OnWake() override {
        FrameInput in{};
        s_.input.Load(in);
        if (in.timestampQpc == lastTs_) return;

        if (!havePrev_) {
            prevPos_ = in.playerPosition;
            lastTs_  = in.timestampQpc;
            havePrev_ = true;
            return;
        }

        const double dt = QpcToMs(static_cast<std::int64_t>(in.timestampQpc - lastTs_)) / 1000.0;
        if (dt < 0.001) return;                       // too close together: noisy delta

        Float3 v{(in.playerPosition.x - prevPos_.x) / static_cast<float>(dt),
                 (in.playerPosition.y - prevPos_.y) / static_cast<float>(dt),
                 (in.playerPosition.z - prevPos_.z) / static_cast<float>(dt)};
        prevPos_ = in.playerPosition;
        lastTs_  = in.timestampQpc;

        if (s_.cfg.planarPrediction) v.y = 0.0f;
        const float speed = Length(v);
        if (speed < s_.cfg.minSpeed) return;          // standing still: keep current predictions

        const Float3 dir{v.x / speed, v.y / speed, v.z / speed};

        bool abrupt = false;
        if (haveHeading_) abrupt = Dot(dir, heading_) < s_.cfg.abruptTurnCosine;

        if (abrupt) {
            // 1) Invalidate everything predicted for the old heading.
            const std::uint64_t gen = s_.c.generation.fetch_add(1, std::memory_order_relaxed) + 1;
            if (s_.deps.psoCompiler) s_.deps.psoCompiler->PurgeBefore(gen);
            predicted_.clear();
            s_.c.purges.fetch_add(1, std::memory_order_relaxed);

            // 2) Snap heading (no smoothing) and run an accelerated pass.
            heading_ = dir;
            PredictAndEnqueue(in.playerPosition, heading_, gen,
                              PsoPriority::Urgent, s_.cfg.urgentLookaheadCells);
            s_.c.urgentPasses.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (haveHeading_) {
                const float a = s_.cfg.headingSmoothing;
                heading_ = Normalize({heading_.x + (dir.x - heading_.x) * a,
                                      heading_.y + (dir.y - heading_.y) * a,
                                      heading_.z + (dir.z - heading_.z) * a}, dir);
            } else {
                heading_ = dir;
            }
            haveHeading_ = true;
            if (predicted_.size() > kMaxTrackedCells) predicted_.clear(); // bound memory
            PredictAndEnqueue(in.playerPosition, heading_,
                              s_.c.generation.load(std::memory_order_relaxed),
                              PsoPriority::Normal, s_.cfg.lookaheadCells);
        }
        haveHeading_ = true;
    }

private:
    static constexpr std::size_t kMaxTrackedCells = 4096;

    static float Dot(const Float3& a, const Float3& b) noexcept {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
    static float Length(const Float3& a) noexcept { return std::sqrt(Dot(a, a)); }
    static Float3 Normalize(const Float3& a, const Float3& fallback) noexcept {
        const float l = Length(a);
        return l > 1e-6f ? Float3{a.x / l, a.y / l, a.z / l} : fallback;
    }

    // Pack three signed 21-bit cell coordinates into one 64-bit key.
    static std::uint64_t PackCell(std::int32_t x, std::int32_t y, std::int32_t z) noexcept {
        constexpr std::int64_t  bias = 1 << 20;
        constexpr std::uint64_t mask = (1ull << 21) - 1ull;
        return ((static_cast<std::uint64_t>(x + bias) & mask)) |
               ((static_cast<std::uint64_t>(y + bias) & mask) << 21) |
               ((static_cast<std::uint64_t>(z + bias) & mask) << 42);
    }

    void PredictAndEnqueue(const Float3& pos, const Float3& dir, std::uint64_t generation,
                           PsoPriority prio, std::uint32_t cells) {
        if (!s_.deps.psoCompiler) return;
        const float cs = s_.cfg.cellSize;
        std::uint64_t enq = 0;

        for (std::uint32_t i = 1; i <= cells; ++i) {
            const Float3 p{pos.x + dir.x * cs * static_cast<float>(i),
                           pos.y + dir.y * cs * static_cast<float>(i),
                           pos.z + dir.z * cs * static_cast<float>(i)};
            const auto cx = static_cast<std::int32_t>(std::floor(p.x / cs));
            const auto cy = static_cast<std::int32_t>(std::floor(p.y / cs));
            const auto cz = static_cast<std::int32_t>(std::floor(p.z / cs));
            const std::uint64_t key = PackCell(cx, cy, cz);

            if (!predicted_.insert(key).second) continue;   // already requested

            PsoCompileRequest rq{};
            rq.cellKey    = key;
            rq.generation = generation;
            rq.priority   = prio;
            rq.cellCenter = {(static_cast<float>(cx) + 0.5f) * cs,
                             (static_cast<float>(cy) + 0.5f) * cs,
                             (static_cast<float>(cz) + 0.5f) * cs};

            if (s_.deps.psoCompiler->EnqueueCompile(rq)) ++enq;
            else predicted_.erase(key);                      // retry on a later tick
        }
        if (enq) s_.c.psoEnqueued.fetch_add(enq, std::memory_order_relaxed);
    }

    SharedState&                    s_;
    std::unordered_set<std::uint64_t> predicted_;
    Float3        prevPos_{};
    Float3        heading_{};
    std::uint64_t lastTs_{0};
    bool          havePrev_{false};
    bool          haveHeading_{false};
};

// =============================================================================
//  ETW present-telemetry session (consumer side only, pure user mode)
//
//  Provider : Microsoft-Windows-DXGI  {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}
//  Events   : 42 = Present_Start, 43 = Present_Stop
//  Filter   : Kernel-side PID filter (EVENT_FILTER_TYPE_PID) + defensive check.
//  Note     : Event IDs follow the manifest used by common present-analysis
//             tools; verify with `logman query providers Microsoft-Windows-DXGI`
//             if a future Windows build changes them.
// =============================================================================
constexpr GUID kDxgiProvider =
    {0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};
constexpr USHORT kEvtPresentStart = 42;
constexpr USHORT kEvtPresentStop  = 43;

class EtwPresentSession {
public:
    EtwPresentSession(Counters& counters, AgentBase* wakeOnPresent) noexcept
        : c_(counters), wake_(wakeOnPresent) {}
    ~EtwPresentSession() { Stop(); }

    EtwPresentSession(const EtwPresentSession&)            = delete;
    EtwPresentSession& operator=(const EtwPresentSession&) = delete;

    /// Returns ERROR_SUCCESS or a Win32 error code.
    [[nodiscard]] ULONG Start(DWORD pid) {
        pid_  = pid;
        name_ = L"EngineCoreTelemetry_" + std::to_wstring(pid);

        ULONG rc = StartTraceW(&session_, name_.c_str(), InitProps());
        if (rc == ERROR_ALREADY_EXISTS) {          // stale session from a crashed run
            ControlTraceW(0, name_.c_str(), InitProps(), EVENT_TRACE_CONTROL_STOP);
            session_ = 0;
            rc = StartTraceW(&session_, name_.c_str(), InitProps());
        }
        if (rc != ERROR_SUCCESS) { session_ = 0; return rc; }

        std::array<ULONG, 1> pids{pid};
        EVENT_FILTER_DESCRIPTOR fd{};
        fd.Ptr  = reinterpret_cast<ULONGLONG>(pids.data());
        fd.Size = static_cast<ULONG>(sizeof(pids));
        fd.Type = EVENT_FILTER_TYPE_PID;

        ENABLE_TRACE_PARAMETERS ep{};
        ep.Version         = ENABLE_TRACE_PARAMETERS_VERSION_2;
        ep.EnableFilterDesc = &fd;
        ep.FilterDescCount = 1;

        rc = EnableTraceEx2(session_, &kDxgiProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            TRACE_LEVEL_INFORMATION, ~0ull, 0, 0, &ep);
        if (rc != ERROR_SUCCESS) { StopSession(); return rc; }

        EVENT_TRACE_LOGFILEW lf{};
        lf.LoggerName          = const_cast<LPWSTR>(name_.c_str());
        lf.ProcessTraceMode    = PROCESS_TRACE_MODE_REAL_TIME |
                                 PROCESS_TRACE_MODE_EVENT_RECORD |
                                 PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        lf.EventRecordCallback = &EtwPresentSession::OnEvent;
        lf.Context             = this;

        consumer_ = OpenTraceW(&lf);
        if (consumer_ == INVALID_PROCESSTRACE_HANDLE) {
            rc = ::GetLastError();
            consumer_ = 0;
            StopSession();
            return rc != ERROR_SUCCESS ? rc : ERROR_OPEN_FAILED;
        }

        pump_ = std::jthread([this] {
            TRACEHANDLE h = consumer_;
            ProcessTrace(&h, 1, nullptr, nullptr);   // blocks until the session stops
        });
        active_.store(true, std::memory_order_release);
        return ERROR_SUCCESS;
    }

    void Stop() noexcept {
        active_.store(false, std::memory_order_release);
        StopSession();                       // makes ProcessTrace() return
        if (pump_.joinable()) pump_.join();
        if (consumer_ != 0) { CloseTrace(consumer_); consumer_ = 0; }
    }

    [[nodiscard]] bool Active() const noexcept { return active_.load(std::memory_order_acquire); }

private:
    EVENT_TRACE_PROPERTIES* InitProps() {
        const std::size_t bytes = sizeof(EVENT_TRACE_PROPERTIES) + (name_.size() + 1) * sizeof(wchar_t);
        propsBuf_.assign(bytes, std::byte{0});
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf_.data());
        p->Wnode.BufferSize    = static_cast<ULONG>(bytes);
        p->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
        p->Wnode.ClientContext = 1;                       // QPC timestamps
        p->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
        p->FlushTimer          = 1;
        p->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
        return p;
    }

    void StopSession() noexcept {
        if (session_ == 0) return;
        try {
            ControlTraceW(session_, nullptr, InitProps(), EVENT_TRACE_CONTROL_STOP);
        } catch (...) {}
        session_ = 0;
    }

    // ETW delivers events on the pump thread. Keep this path tiny & lock-free.
    static void WINAPI OnEvent(PEVENT_RECORD rec) noexcept {
        auto* self = static_cast<EtwPresentSession*>(rec->UserContext);
        if (self) self->Handle(*rec);
    }

    void Handle(const EVENT_RECORD& r) noexcept {
        c_.etwEvents.fetch_add(1, std::memory_order_relaxed);
        if (r.EventHeader.ProcessId != pid_) return;
        if (!IsEqualGUID(r.EventHeader.ProviderId, kDxgiProvider)) return;

        const USHORT id = r.EventHeader.EventDescriptor.Id;
        const std::int64_t ts = r.EventHeader.TimeStamp.QuadPart;

        if (id == kEvtPresentStart) {
            const std::int64_t prev = c_.lastPresentStartQpc.exchange(ts, std::memory_order_relaxed);
            if (prev != 0 && ts > prev) {
                const auto us = static_cast<std::uint32_t>(QpcToUs(ts - prev));
                c_.lastIntervalUs.store(us, std::memory_order_relaxed);
                const std::uint32_t old = c_.emaIntervalUs.load(std::memory_order_relaxed);
                c_.emaIntervalUs.store(old == 0 ? us : static_cast<std::uint32_t>(old * 0.8 + us * 0.2),
                                       std::memory_order_relaxed);
            }
            c_.presentCount.fetch_add(1, std::memory_order_relaxed);
        } else if (id == kEvtPresentStop) {
            const std::int64_t start = c_.lastPresentStartQpc.load(std::memory_order_relaxed);
            if (start != 0 && ts > start)
                c_.presentDurationUs.store(static_cast<std::uint32_t>(QpcToUs(ts - start)),
                                           std::memory_order_relaxed);
            if (wake_) wake_->Wake();        // let the pacing agent judge this present
        }
    }

    Counters&               c_;
    AgentBase*              wake_;           // non-owning; outlives this object
    DWORD                   pid_{0};
    std::wstring            name_;
    std::vector<std::byte>  propsBuf_;
    TRACEHANDLE             session_{0};
    TRACEHANDLE             consumer_{0};
    std::jthread            pump_;
    std::atomic<bool>       active_{false};
};

} // namespace

// =============================================================================
//  EngineOrchestrator::Impl
//  Member order matters: agents/etw are destroyed BEFORE shared state.
// =============================================================================
struct EngineOrchestrator::Impl {
    Impl(Dependencies d, OrchestratorConfig c) {
        state.cfg  = c;
        state.deps = std::move(d);
    }

    SharedState state;
    UniqueHandle targetProcess;
    DWORD        targetPid{0};
    ULONG        etwStatus{0};

    std::atomic<bool>          awake{false};
    std::atomic<std::uint64_t> lastWokenFrame{~0ull};
    std::atomic<std::uint64_t> lastEarlyFrame{~0ull};

    std::unique_ptr<EarlySpeculativePipelineWarningAgent> early;
    std::unique_ptr<FramePacingAgent>                     pacing;
    std::unique_ptr<ShaderCompilationPredictorAgent>      shader;
    std::unique_ptr<EtwPresentSession>                    etw;
};

// -----------------------------------------------------------------------------
EngineOrchestrator::EngineOrchestrator(Dependencies deps, OrchestratorConfig config)
    : impl_(std::make_unique<Impl>(std::move(deps), config)) {}

EngineOrchestrator::~EngineOrchestrator() { Shutdown(); }

HRESULT EngineOrchestrator::AwakenSystem(DWORD targetProcessId) {
    Impl& I = *impl_;
    if (I.awake.load(std::memory_order_acquire)) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    if (targetProcessId == 0) return E_INVALIDARG;

    // 1. Validate the target with the least privilege that gives us liveness checks.
    UniqueHandle h(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, targetProcessId));
    if (!h) return HRESULT_FROM_WIN32(::GetLastError());
    if (::WaitForSingleObject(h.get(), 0) == WAIT_OBJECT_0)
        return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);       // already exited

    try {
        I.targetProcess = std::move(h);
        I.targetPid     = targetProcessId;

        // 2. Build + start agents (they sleep until signalled).
        I.early  = std::make_unique<EarlySpeculativePipelineWarningAgent>(I.state);
        I.pacing = std::make_unique<FramePacingAgent>(I.state);
        I.shader = std::make_unique<ShaderCompilationPredictorAgent>(I.state);
        I.early->Start();
        I.pacing->Start();
        I.shader->Start();

        // 3. Telemetry pipeline. Failure here degrades, it does not abort.
        I.etw = std::make_unique<EtwPresentSession>(I.state.c, I.pacing.get());
        I.etwStatus = I.etw->Start(targetProcessId);
        if (I.etwStatus != ERROR_SUCCESS) I.etw.reset();
    } catch (...) {
        Shutdown();
        return E_FAIL;
    }

    I.awake.store(true, std::memory_order_release);
    return I.etwStatus == ERROR_SUCCESS ? S_OK : S_FALSE;
}

void EngineOrchestrator::Shutdown() noexcept {
    Impl& I = *impl_;
    I.awake.store(false, std::memory_order_release);

    // Order: stop the event source first so nothing wakes a dying agent.
    I.etw.reset();
    if (I.early)  I.early->Stop();
    if (I.pacing) I.pacing->Stop();
    if (I.shader) I.shader->Stop();
    I.early.reset();
    I.pacing.reset();
    I.shader.reset();

    if (I.state.deps.dpcSteering) I.state.deps.dpcSteering->SetLatencyCriticalPhase(false);
    I.targetProcess.reset();
}

void EngineOrchestrator::Tick(const FrameInput& in) noexcept {
    Impl& I = *impl_;
    if (!I.awake.load(std::memory_order_acquire)) return;

    I.state.input.Store(in);                                   // wait-free publish

    // Gate wakes so we don't issue a WakeByAddress syscall per call.
    if (I.lastWokenFrame.exchange(in.frameIndex, std::memory_order_relaxed) != in.frameIndex) {
        I.pacing->Wake();
        I.shader->Wake();
    }
    if (in.cpuFrameProgress >= I.state.cfg.earlyWarningProgress &&
        I.lastEarlyFrame.exchange(in.frameIndex, std::memory_order_relaxed) != in.frameIndex) {
        I.early->Wake();
    }
}

void EngineOrchestrator::Update() noexcept {
    Impl& I = *impl_;
    if (!I.awake.load(std::memory_order_acquire)) return;
    I.early->Wake();
    I.pacing->Wake();
    I.shader->Wake();
}

bool EngineOrchestrator::IsAwake() const noexcept {
    return impl_->awake.load(std::memory_order_acquire);
}

TelemetrySnapshot EngineOrchestrator::GetTelemetrySnapshot() const noexcept {
    const Impl& I = *impl_;
    const Counters& c = I.state.c;
    constexpr auto rlx = std::memory_order_relaxed;

    TelemetrySnapshot s{};
    s.captureQpc  = static_cast<std::uint64_t>(NowQpc());
    s.systemAwake = I.awake.load(std::memory_order_acquire);
    s.targetProcessId = I.targetPid;
    s.targetProcessAlive = I.targetProcess &&
        ::WaitForSingleObject(I.targetProcess.get(), 0) == WAIT_TIMEOUT;

    FrameInput in{};
    I.state.input.Load(in);
    s.lastFrameIndex  = in.frameIndex;
    s.lastCpuProgress = in.cpuFrameProgress;

    s.etwActive            = I.etw && I.etw->Active();
    s.etwStartStatus       = I.etwStatus;
    s.etwEventsSeen        = c.etwEvents.load(rlx);
    s.presentCount         = c.presentCount.load(rlx);
    s.presentIntervalMs    = c.lastIntervalUs.load(rlx) / 1000.0;
    s.presentIntervalEmaMs = c.emaIntervalUs.load(rlx) / 1000.0;
    s.presentDurationMs    = c.presentDurationUs.load(rlx) / 1000.0;

    s.earlyWarningsSent        = c.ewSent.load(rlx);
    s.lastEarlyWarningProgress = c.ewLastProgress.load(rlx);
    s.lastEarlyDispatchCostUs  = c.ewDispatchCostNs.load(rlx) / 1000.0;

    s.lateFrames           = c.lateFrames.load(rlx);
    s.generatedFrames      = c.generated.load(rlx);
    s.generationRejected   = c.genRejected.load(rlx);
    s.consecutiveGenerated = c.consecutive.load(rlx);
    s.timeBudgetWonMs      = c.budgetWonUs.load(rlx) / 1000.0;

    s.psoRequestsEnqueued   = c.psoEnqueued.load(rlx);
    s.directionChangePurges = c.purges.load(rlx);
    s.urgentPasses          = c.urgentPasses.load(rlx);
    s.predictionGeneration  = c.generation.load(rlx);

    if (I.state.deps.vramMonitor)  s.vram = I.state.deps.vramMonitor->QueryStats();
    if (I.state.deps.dpcSteering)  s.dpc  = I.state.deps.dpcSteering->QueryStats();

    s.agentFaults = c.agentFaults.load(rlx);
    return s;
}

} // namespace enginecore