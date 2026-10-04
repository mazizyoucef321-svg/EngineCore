// =============================================================================
// EngineCore :: Early Speculative Pipeline Warning & Input-Aware Priority Engine
// =============================================================================
#pragma once

#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <vector>

namespace EngineCore::Pipeline
{

// -----------------------------------------------------------------------------
// Security & Low-Latency GPU Access Privilege Manager
// -----------------------------------------------------------------------------
class SpeculativeSecurityManager
{
public:
    // Secures high-priority DirectX / DXGI queue status privileges
    [[nodiscard]] static bool SecureGpuQueuePrivileges() noexcept
    {
        HANDLE tokenHandle = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tokenHandle))
        {
            return false;
        }

        TOKEN_PRIVILEGES tp{};
        LUID luid{};

        // Request System Profile privilege for low-latency GPU synchronization
        if (!LookupPrivilegeValueW(nullptr, L"SE_SYSTEM_PROFILE_NAME", &luid))
        {
            CloseHandle(tokenHandle);
            return false;
        }

        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        const BOOL result = AdjustTokenPrivileges(tokenHandle, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr);
        CloseHandle(tokenHandle);

        return result && GetLastError() == ERROR_SUCCESS;
    }
};

// -----------------------------------------------------------------------------
// Input Vector & Camera State Payload
// -----------------------------------------------------------------------------
struct CameraTransform
{
    float position[3]{0.0f, 0.0f, 0.0f};
    float rotation[4]{0.0f, 0.0f, 0.0f, 1.0f}; // Quaternion
    std::uint64_t inputSequenceId = 0;
};

struct FrameSpeculationHeader
{
    std::uint32_t estimatedResourceCost = 0;
    bool requiresVramPreload = false;
    CameraTransform cameraState;
};

// -----------------------------------------------------------------------------
// Speculative Warning & Input-Aware Priority Engine
// -----------------------------------------------------------------------------
class SpeculativePipelineEngine
{
public:
    SpeculativePipelineEngine();
    ~SpeculativePipelineEngine() = default;

    // Station 1: Triggers early GPU preload signal at 10% CPU frame execution mark
    void TriggerEarlySpeculativeWarning(float cpuFrameProgressPercent, const FrameSpeculationHeader& header) noexcept;

    // Station 2: Input-Aware Priority - Flushes stale queued frames & forces fresh camera updates
    void UpdateInputVectorAndFlushQueue(const CameraTransform& latestCamera) noexcept;

    [[nodiscard]] std::uint64_t GetFlushedFramesCount() const noexcept {
        return flushedStaleFrames_.load(std::memory_order_relaxed);
    }

private:
    void FlushStaleRenderQueue() noexcept;

    Microsoft::WRL::ComPtr<IDXGIAdapter3> dxgiAdapter_;
    std::atomic<std::uint64_t> latestProcessedInputId_{0};
    std::atomic<std::uint64_t> flushedStaleFrames_{0};
    std::atomic<bool> isGpuPreloadSignaled_{false};
};

} // namespace EngineCore::Pipeline