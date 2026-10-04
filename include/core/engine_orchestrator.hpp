// =============================================================================
//  EngineCore :: Central Orchestrator
//  include/core/engine_orchestrator.hpp
//
//  Target : MSVC, /std:c++20, Windows 10 1607+ (SetThreadDescription)
//  Mode   : Pure user-mode. Win32 + ETW consumer only. No drivers, no kernel
//           hooks, no code injection, no privilege escalation.
//
//  ROLE
//  ----
//  EngineOrchestrator is the ECU / hub that wires the infrastructure services
//  (DPC steering, VRAM budget monitor) to three micro-agents:
//
//    1. EarlySpeculativePipelineWarningAgent
//         ~10% into the CPU frame -> hint sent to the GPU side so it can begin
//         preparing pipeline state for frame N+1 in parallel.
//    2. FramePacingAgent
//         Watches present cadence (ETW) and, when a frame is late, requests a
//         generated / blended frame to hold display pacing and recover budget.
//    3. ShaderCompilationPredictorAgent
//         Predicts player heading, pre-compiles PSOs for upcoming cells, and on
//         an abrupt heading change purges stale predictions + runs an urgent
//         compile pass for the new target area.
//
//  HONEST NOTES ON WHAT "USER MODE" CAN DO
//  ---------------------------------------
//  * There is no user-mode API to push raw messages over the PCIe bus. The
//    early-warning path is therefore expressed through IGpuPipelineHintSink;
//    the renderer's implementation maps it onto legitimate mechanisms
//    (D3D12 fence signal on a dedicated queue, pre-warm work on the copy /
//    compute queue, residency hints via IDXGIDevice / ID3D12Device). The driver
//    then moves it over the bus.
//  * Frame generation is delegated to IFrameGenerationBackend (your engine's
//    FSR3 / DLSS-G / custom optical-flow wrapper). The agent decides *when*.
//  * ETW real-time sessions normally need Administrator or membership in the
//    "Performance Log Users" group. If the session cannot start, the system
//    runs in DEGRADED mode (agents fall back to FrameInput timing) and
//    AwakenSystem() returns S_FALSE.
//
//  THREADING CONTRACT
//  ------------------
//  * Tick() is single-producer (call from the game/main thread only).
//    It is wait-free for the caller: a seqlock store + atomic wake.
//  * Agents run on their own std::jthread and sleep on std::atomic::wait
//    (WaitOnAddress); zero polling, zero locks on the hot path.
//  * GetTelemetrySnapshot() is safe from any thread (UI/profiler).
//  * Service interfaces below MUST be internally thread-safe for the methods
//    invoked here.
// =============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#include <memory>

namespace enginecore {

// -----------------------------------------------------------------------------
//  Plain data types
// -----------------------------------------------------------------------------
struct Float3 {
    float x{0.0f}, y{0.0f}, z{0.0f};
};

/// Per-tick input published by the game thread. Trivially copyable, 40 bytes
/// (must stay a multiple of 8: it travels through a word-wise seqlock).
struct FrameInput {
    std::uint64_t frameIndex{0};       ///< Monotonic frame counter.
    std::uint64_t timestampQpc{0};     ///< QueryPerformanceCounter at sample time.
    float         cpuFrameProgress{0}; ///< 0..1 fraction of CPU frame work done.
    float         targetFrameTimeMs{0};///< Display budget (e.g. 16.667). 0 = use config default.
    float         cpuFrameTimeMs{0};   ///< Last completed CPU frame time (fallback metric).
    Float3        playerPosition{};    ///< World-space position (Y-up assumed).
};

// -----------------------------------------------------------------------------
//  Service interfaces (thin adapters over the concrete infrastructure classes)
//
//  DpcSteeringEngine and VramBudgetMonitor are wrapped by small adapters that
//  implement these interfaces, keeping the orchestrator testable and free of
//  hard dependencies. All pointers in Dependencies may be null (agent then
//  no-ops for that service).
// -----------------------------------------------------------------------------
struct DpcSteeringStats {
    std::uint64_t dpcsSteered{0};
    double        worstDpcLatencyUs{0.0};
};

class IDpcSteeringService {
public:
    virtual ~IDpcSteeringService() = default;
    /// Tell the steering engine we're inside a latency-critical window.
    virtual void SetLatencyCriticalPhase(bool active) noexcept = 0;
    [[nodiscard]] virtual DpcSteeringStats QueryStats() const noexcept = 0;
};

struct VramBudgetStats {
    std::uint64_t budgetBytes{0};
    std::uint64_t usageBytes{0};
    float         pressure{0.0f}; ///< usage / budget, 0..1+
};

class IVramBudgetService {
public:
    virtual ~IVramBudgetService() = default;
    [[nodiscard]] virtual VramBudgetStats QueryStats() const noexcept = 0;
};

/// Early hint describing the NEXT frame (N+1).
struct NextFrameHint {
    std::uint64_t targetFrameIndex{0};
    std::uint64_t timestampQpc{0};
    float         cpuProgressAtTrigger{0};
    float         expectedFrameTimeMs{0};
    float         vramPressure{0};
    bool          preferReducedResidency{false}; ///< Set when VRAM pressure is high.
};

class IGpuPipelineHintSink {
public:
    virtual ~IGpuPipelineHintSink() = default;
    /// Must be non-blocking. Typical impl: signal a D3D12 fence / enqueue
    /// pre-warm work on a secondary queue.
    virtual void PrepareNextFramePipeline(const NextFrameHint& hint) noexcept = 0;
};

struct FrameGenRequest {
    std::uint64_t frameIndex{0};
    float         deficitMs{0};        ///< How far behind schedule the frame is.
    float         blendWeight{0};      ///< 0..1 suggested interpolation weight.
    float         recoveryBudgetMs{0}; ///< Estimated time won for following frames.
};

class IFrameGenerationBackend {
public:
    virtual ~IFrameGenerationBackend() = default;
    /// Returns true if a generated/blended frame was accepted for presentation.
    virtual bool SubmitGeneratedFrame(const FrameGenRequest& request) noexcept = 0;
};

enum class PsoPriority : std::uint8_t { Normal = 0, Urgent = 1 };

struct PsoCompileRequest {
    std::uint64_t cellKey{0};     ///< Packed spatial cell id.
    std::uint64_t generation{0};  ///< Prediction generation (see PurgeBefore).
    PsoPriority   priority{PsoPriority::Normal};
    Float3        cellCenter{};
};

class IPsoCompilerService {
public:
    virtual ~IPsoCompilerService() = default;
    virtual bool EnqueueCompile(const PsoCompileRequest& request) noexcept = 0;
    /// Drop every queued request whose generation < the argument.
    virtual void PurgeBefore(std::uint64_t generation) noexcept = 0;
};

// -----------------------------------------------------------------------------
//  Configuration
// -----------------------------------------------------------------------------
struct OrchestratorConfig {
    // --- EarlySpeculativePipelineWarningAgent
    float earlyWarningProgress{0.10f};   ///< ~10% of CPU frame.
    float highVramPressure{0.90f};

    // --- FramePacingAgent
    float defaultTargetFrameTimeMs{16.667f};
    float lateTolerance{1.10f};          ///< Late if interval > target * this.
    std::uint32_t maxConsecutiveGenerated{2}; ///< Guard against runaway generation.
    float frameGenCostMs{0.5f};          ///< Estimated cost of a generated frame.
    double etwFreshnessMs{250.0};        ///< ETW data older than this is ignored.

    // --- ShaderCompilationPredictorAgent
    float         cellSize{16.0f};       ///< World units per PSO streaming cell.
    std::uint32_t lookaheadCells{6};
    std::uint32_t urgentLookaheadCells{10};
    float         abruptTurnCosine{0.5f};///< cos(60 deg): below this = abrupt turn.
    float         minSpeed{0.5f};        ///< Units/s; below = "standing still".
    float         headingSmoothing{0.35f};
    bool          planarPrediction{true};///< Ignore Y when computing heading.
};

// -----------------------------------------------------------------------------
//  Telemetry snapshot (for UI / profiler). Every field is individually atomic-
//  sourced; the struct as a whole is "eventually consistent", not transactional.
// -----------------------------------------------------------------------------
struct TelemetrySnapshot {
    std::uint64_t captureQpc{0};
    bool          systemAwake{false};
    bool          targetProcessAlive{false};
    DWORD         targetProcessId{0};

    // Input
    std::uint64_t lastFrameIndex{0};
    float         lastCpuProgress{0};

    // ETW
    bool          etwActive{false};
    ULONG         etwStartStatus{0};        ///< Win32 error from session start (0 = OK).
    std::uint64_t etwEventsSeen{0};
    std::uint64_t presentCount{0};
    double        presentIntervalMs{0};     ///< Last raw present-to-present interval.
    double        presentIntervalEmaMs{0};
    double        presentDurationMs{0};

    // EarlySpeculativePipelineWarningAgent
    std::uint64_t earlyWarningsSent{0};
    float         lastEarlyWarningProgress{0};
    double        lastEarlyDispatchCostUs{0};

    // FramePacingAgent
    std::uint64_t lateFrames{0};
    std::uint64_t generatedFrames{0};
    std::uint64_t generationRejected{0};
    std::uint32_t consecutiveGenerated{0};
    double        timeBudgetWonMs{0};

    // ShaderCompilationPredictorAgent
    std::uint64_t psoRequestsEnqueued{0};
    std::uint64_t directionChangePurges{0};
    std::uint64_t urgentPasses{0};
    std::uint64_t predictionGeneration{0};

    // Infrastructure
    VramBudgetStats  vram{};
    DpcSteeringStats dpc{};

    // Health
    std::uint64_t agentFaults{0};           ///< Exceptions swallowed in agent loops.
};

// -----------------------------------------------------------------------------
//  EngineOrchestrator
// -----------------------------------------------------------------------------
class EngineOrchestrator {
public:
    struct Dependencies {
        std::shared_ptr<IDpcSteeringService>   dpcSteering;
        std::shared_ptr<IVramBudgetService>    vramMonitor;
        std::shared_ptr<IGpuPipelineHintSink>  gpuHintSink;
        std::shared_ptr<IFrameGenerationBackend> frameGeneration;
        std::shared_ptr<IPsoCompilerService>   psoCompiler;
    };

    explicit EngineOrchestrator(Dependencies deps, OrchestratorConfig config = {});
    ~EngineOrchestrator();

    EngineOrchestrator(const EngineOrchestrator&)            = delete;
    EngineOrchestrator& operator=(const EngineOrchestrator&) = delete;
    EngineOrchestrator(EngineOrchestrator&&)                 = delete;
    EngineOrchestrator& operator=(EngineOrchestrator&&)      = delete;

    /// Validates the target process, starts the ETW present-telemetry session
    /// (DXGI provider, PID-filtered) and spins up the three agents.
    ///   S_OK    : fully operational.
    ///   S_FALSE : running DEGRADED (ETW unavailable; see snapshot.etwStartStatus).
    ///   FAILED  : nothing started (bad PID, already awake, thread failure...).
    [[nodiscard]] HRESULT AwakenSystem(DWORD targetProcessId);

    /// Stops ETW then agents. Idempotent. Also called by the destructor.
    void Shutdown() noexcept;

    /// Publish a new sample and wake the agents that need it. Wait-free for the
    /// caller. SINGLE-PRODUCER: call from one thread only. Call it at frame
    /// start and again once cpuFrameProgress >= ~0.10 for the early warning.
    void Tick(const FrameInput& input) noexcept;

    /// Poll-style kick: wakes all agents to re-evaluate the latest published
    /// sample and the newest ETW data, without publishing anything new.
    void Update() noexcept;

    /// Thread-safe, lock-free snapshot for UI/profiler visualisation.
    [[nodiscard]] TelemetrySnapshot GetTelemetrySnapshot() const noexcept;

    [[nodiscard]] bool IsAwake() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace enginecore