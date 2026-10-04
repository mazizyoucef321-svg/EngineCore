// =============================================================================
// EngineCore :: AI Speculative Frame Pacing & Low-Latency Dispatcher
// =============================================================================
#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <memory>

namespace EngineCore::Pipeline
{

// -----------------------------------------------------------------------------
// Security & Process Privilege Elevation for High-Priority Scheduling
// -----------------------------------------------------------------------------
class RealtimePrivilegeManager
{
public:
    // Requests HIGH_PRIORITY_CLASS / REALTIME_PRIORITY_CLASS rights for precision timing
    [[nodiscard]] static bool GrantSchedulingPrivileges() noexcept
    {
        HANDLE tokenHandle = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tokenHandle))
        {
            return false;
        }

        TOKEN_PRIVILEGES tp{};
        LUID luid{};

        if (!LookupPrivilegeValueW(nullptr, L"SE_INC_BASE_PRIORITY_NAME", &luid))
        {
            CloseHandle(tokenHandle);
            return false;
        }

        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        const BOOL result = AdjustTokenPrivileges(tokenHandle, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr);
        CloseHandle(tokenHandle);

        if (result && GetLastError() == ERROR_SUCCESS)
        {
            // Set thread affinity and process execution priority
            SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
            return true;
        }
        return false;
    }
};

// -----------------------------------------------------------------------------
// Pipeline Telemetry & Frame Synthesis Structures
// -----------------------------------------------------------------------------
struct CommandPacket
{
    std::uint64_t commandId = 0;
    std::uint64_t timestampNs = 0;
    void* pGpuBufferAddress = nullptr;
    std::size_t payloadSize = 0;
};

struct FramePacingMetrics
{
    float targetFrameTimeMs = 16.667f; // 60 FPS Target Baseline
    float currentFrameTimeMs = 0.0f;
    bool stutterDetected = false;
};

// -----------------------------------------------------------------------------
// AI Speculative Frame Pacing Engine (Station A -> B -> C)
// -----------------------------------------------------------------------------
class FramePacingAIEngine
{
public:
    FramePacingAIEngine();
    ~FramePacingAIEngine() = default;

    // Station A -> B: High-Speed Micro-Dispatch over PCIe to VRAM
    bool FastDispatchToGPU(const CommandPacket& packet) noexcept;

    // Station B -> C: Frame Pacing Monitor & Speculative Synthesis
    void ProcessFramePacing(const FramePacingMetrics& metrics);

    [[nodiscard]] std::uint64_t GetSynthesizedFrameCount() const noexcept {
        return synthesizedFramesCount_.load(std::memory_order_relaxed);
    }

private:
    void InjectSpeculativeFrame() noexcept;
    void CompensateFrameLatency(float deltaMs) noexcept;

    std::atomic<bool> isHighPriorityActive_{false};
    std::atomic<std::uint64_t> synthesizedFramesCount_{0};
    std::chrono::high_resolution_clock::time_point lastFrameTime_;
};

} // namespace EngineCore::Pipeline