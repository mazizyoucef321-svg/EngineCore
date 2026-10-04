// =============================================================================
// EngineCore :: AI-Predictive Cache Engine Implementation
// =============================================================================
#include "shader/predictive_vault.hpp"
#include <iostream>

namespace EngineCore::Shader
{

PredictiveCacheEngine::PredictiveCacheEngine()
    : cachePath_(SystemPrivilegeManager::GetSafeCacheDirectory())
{
}

CacheState PredictiveCacheEngine::EvaluateInferenceModel(const ExecutionMetrics& metrics) noexcept
{
    // High-speed micro-inference heuristics (Zero overhead execution)
    if (metrics.compilationStutterDetected)
    {
        return CacheState::CorruptionDetected;
    }
    
    // CPU fallback trigger threshold
    if (metrics.frameTimeMs > 33.0f && metrics.cpuLoadPercent > 80.0f)
    {
        return CacheState::SuddenStateChange;
    }

    return CacheState::Optimal;
}

void PredictiveCacheEngine::ProcessTelemetry(const ExecutionMetrics& metrics)
{
    CacheState state = EvaluateInferenceModel(metrics);

    if (state == CacheState::CorruptionDetected)
    {
        if (SystemPrivilegeManager::RequestCacheClearPrivilege())
        {
            PurgeCorruptedCache();
        }
    }
    else if (state == CacheState::SuddenStateChange)
    {
        AssistCPUTranslation();
    }
}

void PredictiveCacheEngine::ExecuteUberShader(const UberShaderState& state)
{
    activePermutation_ = state.permutationHash;
}

void PredictiveCacheEngine::PurgeCorruptedCache()
{
    try
    {
        if (std::filesystem::exists(cachePath_))
        {
            std::filesystem::remove_all(cachePath_);
            std::filesystem::create_directories(cachePath_);
        }
    }
    catch (...)
    {
        // Safe fail-silent to protect runtime execution
    }
    cacheEntriesCount_.store(0, std::memory_order_relaxed);
}

void PredictiveCacheEngine::AssistCPUTranslation()
{
    isCpuAssisting_.store(true, std::memory_order_release);
}

} // namespace EngineCore::Shader 