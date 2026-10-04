// =============================================================================
// EngineCore :: VRAM Budget Telemetry & Low-Latency Flip-Model SwapChain Setup
// -----------------------------------------------------------------------------
// 1. VramBudgetMonitor polls IDXGIAdapter3::QueryVideoMemoryInfo on a small
//    background std::jthread and publishes snapshots through a lock-free
//    two-slot seqlock. The render thread reads with GetSnapshot(), which never
//    takes a lock and never waits for the polling thread.
// 2. VramBudgetMonitor::ConfigureFlipModelSwapChain builds an
//    IDXGISwapChain1 that uses DXGI_SWAP_EFFECT_FLIP_DISCARD with optional
//    tearing and an optional frame-latency waitable object.
//
// Constraints honoured by this module:
//   - User mode only, no elevation. Only documented DXGI 1.5 APIs are used
//     (no D3DKMT*, no driver calls, no hooks, no injection).
//   - Failures are reported with std::error_code; nothing here lets an
//     exception escape the polling thread.
// =============================================================================
#pragma once

#include <windows.h>

#include <dxgi1_5.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>

namespace EngineCore::Telemetry
{

// -----------------------------------------------------------------------------
// Point-in-time view of the local (dedicated) video memory segment.
// A default-constructed snapshot (budgetBytes == 0) means "no reading yet".
// -----------------------------------------------------------------------------
struct VramBudgetSnapshot
{
    std::uint64_t budgetBytes = 0; // OS-recommended usage ceiling for this process.
    std::uint64_t currentUsageBytes = 0; // Current usage of this process.
    double usageFraction = 0.0; // usage / budget; values above 1.0 mean over budget.
    bool isPressureHigh = false; // usageFraction >= the configured threshold.
};

// -----------------------------------------------------------------------------
// Swap chain options. Only the first three members are required for basic use.
// -----------------------------------------------------------------------------
struct FlipModelConfig
{
    UINT bufferCount = 2; // Flip model needs 2..16 buffers.
    bool allowTearing = false; // Silently disabled if the system cannot tear.
    bool enableLowLatencyWait = false; // Creates the frame-latency waitable object.

    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM; // Must be a flip-model-legal format.
    UINT width = 0; // 0 = use the window's client width.
    UINT height = 0; // 0 = use the window's client height.
    UINT maxFrameLatency = 1; // Used only with enableLowLatencyWait; clamped to [1, bufferCount].
};

// -----------------------------------------------------------------------------
// Minimal RAII owner for a Win32 kernel HANDLE (events, waitable objects).
// -----------------------------------------------------------------------------
class UniqueWin32Handle
{
public:
    UniqueWin32Handle() noexcept = default;
    explicit UniqueWin32Handle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueWin32Handle() { Reset(); }

    UniqueWin32Handle(const UniqueWin32Handle&) = delete;
    UniqueWin32Handle& operator=(const UniqueWin32Handle&) = delete;
    UniqueWin32Handle(UniqueWin32Handle&& other) noexcept : handle_(other.Release()) {}
    UniqueWin32Handle& operator=(UniqueWin32Handle&& other) noexcept
    {
        if (this != &other)
        {
            Reset(other.Release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] bool Valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    HANDLE Release() noexcept
    {
        const HANDLE released = handle_;
        handle_ = nullptr;
        return released;
    }

    void Reset(HANDLE replacement = nullptr) noexcept
    {
        if (Valid())
        {
            CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_ = nullptr;
};

// -----------------------------------------------------------------------------
// Result of ConfigureFlipModelSwapChain.
// -----------------------------------------------------------------------------
struct FlipModelSwapChain
{
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    UniqueWin32Handle frameLatencyWaitable; // Invalid unless enableLowLatencyWait was requested.
    bool tearingEnabled = false; // True only if requested AND supported by the system.
    UINT swapChainFlags = 0; // Pass unchanged to IDXGISwapChain::ResizeBuffers.

    // Flags for Present/Present1. Tearing may only be requested with sync
    // interval 0, so this returns DXGI_PRESENT_ALLOW_TEARING only then.
    [[nodiscard]] UINT PresentFlags(UINT syncInterval) const noexcept
    {
        return (tearingEnabled && syncInterval == 0) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    }

    // Blocks until DXGI is ready for the next frame. Call at the START of the
    // frame, before sampling input. Returns true immediately if no waitable
    // object exists, and false on timeout or wait failure.
    [[nodiscard]] bool WaitForNextFrame(DWORD timeoutMs = 1000) const noexcept
    {
        if (!frameLatencyWaitable.Valid())
        {
            return true;
        }
        return WaitForSingleObjectEx(frameLatencyWaitable.Get(), timeoutMs, TRUE) == WAIT_OBJECT_0;
    }
};

// Invoked on the polling thread when isPressureHigh flips (either direction).
// Keep it short; hand heavy work (asset eviction) to your own queue.
using VramPressureCallback = std::function<void(const VramBudgetSnapshot&)>;

// -----------------------------------------------------------------------------
// VramBudgetMonitor
// -----------------------------------------------------------------------------
class VramBudgetMonitor
{
public:
    VramBudgetMonitor() = default;
    ~VramBudgetMonitor() { Shutdown(); }

    VramBudgetMonitor(const VramBudgetMonitor&) = delete;
    VramBudgetMonitor& operator=(const VramBudgetMonitor&) = delete;
    VramBudgetMonitor(VramBudgetMonitor&&) = delete;
    VramBudgetMonitor& operator=(VramBudgetMonitor&&) = delete;

    // Validates arguments, takes a first reading synchronously and starts the
    // polling thread. `pressureThreshold` must be in (0, 1]. `pollInterval` is
    // clamped to [10 ms, 60 s]. Calling Initialize again restarts monitoring.
    // Initialize/Shutdown must be driven from one controlling thread.
    [[nodiscard]] std::error_code Initialize(IDXGIAdapter* adapter, double pressureThreshold,
                                              std::chrono::milliseconds pollInterval = std::chrono::milliseconds(500),
                                              std::uint32_t nodeIndex = 0);

    // Stops the polling thread and releases all resources. Idempotent. After
    // it returns, GetSnapshot() yields an empty snapshot.
    void Shutdown() noexcept;

    // Lock-free and wait-free for practical purposes; safe from any thread.
    [[nodiscard]] VramBudgetSnapshot GetSnapshot() const noexcept;

    // Optional transition notification (see VramPressureCallback). Thread-safe.
    void SetPressureCallback(VramPressureCallback callback);

    // Number of failed QueryVideoMemoryInfo calls since Initialize.
    [[nodiscard]] std::uint64_t QueryFailureCount() const noexcept
    {
        return queryFailures_.load(std::memory_order_relaxed);
    }

    // Creates a flip-model swap chain for `hwnd`.
    //   factory               : the IDXGIFactory2 that created the adapter/device.
    //   presentDeviceOrQueue  : Direct3D 12: the DIRECT command queue.
    //                           Direct3D 11: the ID3D11Device.
    // Effects: DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SCALING_NONE, one sample,
    // and Alt+Enter handling disabled via DXGI_MWA_NO_ALT_ENTER. `out` is only
    // modified on success.
    [[nodiscard]] static std::error_code ConfigureFlipModelSwapChain(IDXGIFactory2* factory,
                                                                      IUnknown* presentDeviceOrQueue, HWND hwnd,
                                                                      const FlipModelConfig& config,
                                                                      FlipModelSwapChain& out);

private:
    // One published snapshot. All fields are atomics so concurrent reads
    // during a (rare) overwrite are well-defined; the sequence counter tells
    // the reader whether it observed a torn value.
    struct Slot
    {
        std::atomic<std::uint32_t> sequence{0}; // Odd while being written.
        std::atomic<std::uint64_t> budgetBytes{0};
        std::atomic<std::uint64_t> currentUsageBytes{0};
        std::atomic<std::uint64_t> usageFractionBits{0}; // std::bit_cast of the double.
        std::atomic<bool> pressureHigh{false};
    };

    void Publish(const VramBudgetSnapshot& snapshot) noexcept; // Single writer only.
    void PollLoop(std::stop_token token);
    void SampleOnce() noexcept;
    void NotifyPressureChange(const VramBudgetSnapshot& snapshot) noexcept;

    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
    double threshold_ = 0.92;
    std::uint32_t nodeIndex_ = 0;
    std::chrono::milliseconds pollInterval_{500};

    UniqueWin32Handle stopEvent_; // Manual-reset, signalled by the stop_token callback.
    UniqueWin32Handle budgetEvent_; // Auto-reset, signalled by DXGI on budget changes.
    DWORD budgetCookie_ = 0;
    bool budgetRegistered_ = false;
    bool lastPressureHigh_ = false; // Owned by the polling thread once it runs.

    std::atomic<std::uint64_t> queryFailures_{0};

    // Two-slot seqlock: the writer only ever touches the slot that is NOT the
    // latest one, so a reader of the latest slot is never blocked by a writer
    // that was preempted mid-update.
    Slot slots_[2];
    std::atomic<std::uint32_t> latest_{0};

    std::mutex callbackMutex_;
    VramPressureCallback callback_;

    std::jthread pollThread_; // Declared last: destroyed (and joined) first.
};

} // namespace EngineCore::Telemetry