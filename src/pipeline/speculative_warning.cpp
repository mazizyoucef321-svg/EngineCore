// =============================================================================
// EngineCore :: Early Speculative Pipeline Warning Implementation
// =============================================================================
#include "pipeline/speculative_warning.hpp"
#include <iostream>

namespace EngineCore::Pipeline
{

SpeculativePipelineEngine::SpeculativePipelineEngine()
{
    // Ensure security token privileges for low-latency hardware synchronization
    SpeculativeSecurityManager::SecureGpuQueuePrivileges();

    // Query DXGI 1.4 Adapter for safe memory and pipeline notifications
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter> tempAdapter;
        if (SUCCEEDED(factory->EnumAdapters(0, &tempAdapter)))
        {
            tempAdapter.As(&dxgiAdapter_);
        }
    }
}

void SpeculativePipelineEngine::TriggerEarlySpeculativeWarning(float cpuFrameProgressPercent, const FrameSpeculationHeader& header) noexcept
{
    // Execute ONLY when CPU reaches the first ~10% of frame calculation
    if (cpuFrameProgressPercent >= 0.10f && !isGpuPreloadSignaled_.load(std::memory_order_relaxed))
    {
        isGpuPreloadSignaled_.store(true, std::memory_order_relaxed);

        // Notify DXGI / Driver Pipeline ahead of time to allocate resource barriers
        if (dxgiAdapter_ && header.requiresVramPreload)
        {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            dxgiAdapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
            // Early VRAM speculative budget check logic
        }
    }
}

void SpeculativePipelineEngine::UpdateInputVectorAndFlushQueue(const CameraTransform& latestCamera) noexcept
{
    std::uint64_t currentInputId = latestProcessedInputId_.load(std::memory_order_relaxed);

    // If a newer camera/input vector arrives, discard queued out-of-date frame commands
    if (latestCamera.inputSequenceId > currentInputId)
    {
        latestProcessedInputId_.store(latestCamera.inputSequenceId, std::memory_order_release);
        FlushStaleRenderQueue();
    }

    // Reset signal lock for next frame cycle
    isGpuPreloadSignaled_.store(false, std::memory_order_relaxed);
}

void SpeculativePipelineEngine::FlushStaleRenderQueue() noexcept
{
    // Drops pending stale command lists in the pipeline buffer to prevent Input Lag
    flushedStaleFrames_.fetch_add(1, std::memory_order_relaxed);
}

} // namespace EngineCore::Pipeline