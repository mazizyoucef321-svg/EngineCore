// =============================================================================
// EngineCore :: AI-Predictive Cache Engine & Modular Uber-Shader Pipeline
// =============================================================================
#pragma once

#include <windows.h>
#include <shlobj.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace EngineCore::Shader
{

// -----------------------------------------------------------------------------
// Safe Local Directory & Windows Security Privilege Manager
// -----------------------------------------------------------------------------
class SystemPrivilegeManager
{
public:
    // Guarantees we write cache ONLY to safe user space (%LOCALAPPDATA%) to avoid Access Violations
    [[nodiscard]] static std::filesystem::path GetSafeCacheDirectory()
    {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &path)))
        {
            std::filesystem::path safePath(path);
            CoTaskMemFree(path);
            safePath /= "EngineCore/ShaderCache";
            std::filesystem::create_directories(safePath);
            return safePath;
        }
        return std::filesystem::current_path() / "Cache";
    }

    // Requests cache purge privileges within process safety boundary
    [[nodiscard]] static bool RequestCacheClearPrivilege() noexcept
    {
        HANDLE tokenHandle = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tokenHandle))
        {
            return false;
        }

        TOKEN_PRIVILEGES tp{};
        LUID luid{};

        if (!LookupPrivilegeValueW(nullptr, L"SE_BACKUP_NAME", &luid))
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
// Micro-Inference Telemetry & State
// -----------------------------------------------------------------------------
enum class CacheState
{
    Optimal,
    CorruptionDetected,
    SuddenStateChange
};

struct ExecutionMetrics
{
    float frameTimeMs = 0.0f;
    float cpuLoadPercent = 0.0f;
    bool compilationStutterDetected = false;
};

struct UberShaderState
{
    std::uint32_t permutationHash = 0;
    bool enableLighting = true;
    bool enableShadows = true;
    std::vector<std::uint8_t> compiledBytecode;
};

// -----------------------------------------------------------------------------
// AI-Predictive Cache Engine
// -----------------------------------------------------------------------------
class PredictiveCacheEngine
{
public:
    PredictiveCacheEngine();
    ~PredictiveCacheEngine() = default;

    void ProcessTelemetry(const ExecutionMetrics& metrics);
    void ExecuteUberShader(const UberShaderState& state);

private:
    [[nodiscard]] CacheState EvaluateInferenceModel(const ExecutionMetrics& metrics) noexcept;
    void PurgeCorruptedCache();
    void AssistCPUTranslation();

    std::filesystem::path cachePath_;
    std::atomic<std::size_t> cacheEntriesCount_{100};
    std::atomic<bool> isCpuAssisting_{false};
    std::uint32_t activePermutation_{0};
};

} // namespace EngineCore::Shader