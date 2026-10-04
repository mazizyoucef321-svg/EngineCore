// =============================================================================
// EngineCore :: VRAM Budget Telemetry & Low-Latency Flip-Model SwapChain Setup
// (Implementation)
// =============================================================================
#include "telemetry/vram_monitor.hpp"

#include <algorithm>
#include <bit>
#include <utility>

namespace EngineCore::Telemetry
{

namespace
{
    // HRESULTs are carried in the system category, which resolves both Win32
    // error codes and HRESULT_FROM_WIN32 values to readable messages.
    [[nodiscard]] std::error_code FromHresult(HRESULT hr) noexcept
    {
        return std::error_code(static_cast<int>(hr), std::system_category());
    }

    [[nodiscard]] std::error_code InvalidArgument() noexcept
    {
        return std::make_error_code(std::errc::invalid_argument);
    }

    [[nodiscard]] VramBudgetSnapshot MakeSnapshot(const DXGI_QUERY_VIDEO_MEMORY_INFO& info, double threshold) noexcept
    {
        VramBudgetSnapshot snapshot;
        snapshot.budgetBytes = info.Budget;
        snapshot.currentUsageBytes = info.CurrentUsage;
        snapshot.usageFraction =
            info.Budget > 0 ? static_cast<double>(info.CurrentUsage) / static_cast<double>(info.Budget) : 0.0;
        snapshot.isPressureHigh = info.Budget > 0 && snapshot.usageFraction >= threshold;
        return snapshot;
    }

    // Formats DXGI accepts for a flip-model swap chain (sRGB variants must be
    // applied through the render target view instead).
    [[nodiscard]] bool IsFlipModelFormat(DXGI_FORMAT format) noexcept
    {
        switch (format)
        {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return true;
        default:
            return false;
        }
    }
} // namespace

// =============================================================================
// Lock-free snapshot publication
// =============================================================================
void VramBudgetMonitor::Publish(const VramBudgetSnapshot& snapshot) noexcept
{
    // Write into the slot readers are NOT currently directed at.
    const std::uint32_t target = latest_.load(std::memory_order_relaxed) ^ 1u;
    Slot& slot = slots_[target];

    const std::uint32_t sequence = slot.sequence.load(std::memory_order_relaxed);
    slot.sequence.store(sequence + 1u, std::memory_order_relaxed); // Odd: write in progress.
    std::atomic_thread_fence(std::memory_order_release);

    slot.budgetBytes.store(snapshot.budgetBytes, std::memory_order_relaxed);
    slot.currentUsageBytes.store(snapshot.currentUsageBytes, std::memory_order_relaxed);
    slot.usageFractionBits.store(std::bit_cast<std::uint64_t>(snapshot.usageFraction), std::memory_order_relaxed);
    slot.pressureHigh.store(snapshot.isPressureHigh, std::memory_order_relaxed);

    slot.sequence.store(sequence + 2u, std::memory_order_release); // Even: complete.
    latest_.store(target, std::memory_order_release); // Make the new slot visible to readers.
}

VramBudgetSnapshot VramBudgetMonitor::GetSnapshot() const noexcept
{
    for (;;)
    {
        const std::uint32_t index = latest_.load(std::memory_order_acquire);
        const Slot& slot = slots_[index];

        const std::uint32_t before = slot.sequence.load(std::memory_order_acquire);
        if ((before & 1u) != 0u)
        {
            continue; // Writer lapped this slot; re-read `latest_` (it has moved on).
        }

        VramBudgetSnapshot snapshot;
        snapshot.budgetBytes = slot.budgetBytes.load(std::memory_order_relaxed);
        snapshot.currentUsageBytes = slot.currentUsageBytes.load(std::memory_order_relaxed);
        snapshot.usageFraction = std::bit_cast<double>(slot.usageFractionBits.load(std::memory_order_relaxed));
        snapshot.isPressureHigh = slot.pressureHigh.load(std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.sequence.load(std::memory_order_relaxed) == before)
        {
            return snapshot; // No overwrite happened while reading: values are consistent.
        }
    }
}

// =============================================================================
// Lifetime
// =============================================================================
std::error_code VramBudgetMonitor::Initialize(IDXGIAdapter* adapter, double pressureThreshold,
                                               std::chrono::milliseconds pollInterval, std::uint32_t nodeIndex)
{
    Shutdown();

    // `!(x > 0 && x <= 1)` also rejects NaN.
    if (adapter == nullptr || !(pressureThreshold > 0.0 && pressureThreshold <= 1.0) || pollInterval.count() <= 0)
    {
        return InvalidArgument();
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    HRESULT hr = adapter->QueryInterface(IID_PPV_ARGS(&adapter3));
    if (FAILED(hr))
    {
        return FromHresult(hr); // Adapter/driver predates WDDM 2.0 (no IDXGIAdapter3).
    }

    // First reading is taken synchronously so GetSnapshot() is meaningful
    // immediately, and so an unusable adapter fails Initialize() itself.
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    hr = adapter3->QueryVideoMemoryInfo(nodeIndex, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
    if (FAILED(hr))
    {
        return FromHresult(hr);
    }

    UniqueWin32Handle stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent.Valid())
    {
        return FromHresult(HRESULT_FROM_WIN32(GetLastError()));
    }

    // Budget-change notification is an optimisation: if it cannot be set up,
    // plain interval polling is still fully correct.
    UniqueWin32Handle budgetEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    DWORD cookie = 0;
    bool registered = false;
    if (budgetEvent.Valid())
    {
        registered = SUCCEEDED(adapter3->RegisterVideoMemoryBudgetChangeNotificationEvent(budgetEvent.Get(), &cookie));
    }

    const VramBudgetSnapshot first = MakeSnapshot(info, pressureThreshold);

    adapter_ = std::move(adapter3);
    threshold_ = pressureThreshold;
    nodeIndex_ = nodeIndex;
    pollInterval_ = std::clamp(pollInterval, std::chrono::milliseconds(10), std::chrono::milliseconds(60000));
    stopEvent_ = std::move(stopEvent);
    budgetEvent_ = std::move(budgetEvent);
    budgetCookie_ = cookie;
    budgetRegistered_ = registered;
    lastPressureHigh_ = first.isPressureHigh;
    queryFailures_.store(0, std::memory_order_relaxed);
    Publish(first);

    try
    {
        pollThread_ = std::jthread([this](std::stop_token token) { PollLoop(std::move(token)); });
    }
    catch (const std::system_error& error)
    {
        Shutdown();
        return error.code();
    }
    catch (...)
    {
        Shutdown();
        return std::make_error_code(std::errc::not_enough_memory);
    }

    return {};
}

void VramBudgetMonitor::Shutdown() noexcept
{
    if (pollThread_.joinable())
    {
        pollThread_.request_stop(); // Fires the stop_callback that signals stopEvent_.
        pollThread_.join();
    }

    if (adapter_ != nullptr)
    {
        if (budgetRegistered_)
        {
            adapter_->UnregisterVideoMemoryBudgetChangeNotification(budgetCookie_);
        }
        adapter_.Reset();
        Publish(VramBudgetSnapshot{}); // The polling thread is gone: sole writer again.
    }

    budgetRegistered_ = false;
    budgetCookie_ = 0;
    budgetEvent_.Reset();
    stopEvent_.Reset();
}

void VramBudgetMonitor::SetPressureCallback(VramPressureCallback callback)
{
    std::lock_guard lock(callbackMutex_);
    callback_ = std::move(callback);
}

// =============================================================================
// Polling thread
// =============================================================================
void VramBudgetMonitor::PollLoop(std::stop_token token)
{
    // Turns a stop request into an immediate wake-up instead of waiting out
    // the poll interval.
    std::stop_callback wake(token, [this]() noexcept { SetEvent(stopEvent_.Get()); });

    const HANDLE waits[2] = {stopEvent_.Get(), budgetEvent_.Get()};
    const DWORD waitCount = budgetRegistered_ ? 2u : 1u;
    const auto timeoutMs = static_cast<DWORD>(pollInterval_.count());

    while (!token.stop_requested())
    {
        const DWORD result = WaitForMultipleObjects(waitCount, waits, FALSE, timeoutMs);
        if (result != WAIT_TIMEOUT && result != WAIT_OBJECT_0 + 1)
        {
            break; // Stop event signalled (WAIT_OBJECT_0) or the wait itself failed.
        }
        SampleOnce(); // Interval elapsed, or DXGI reported a budget change.
    }
}

void VramBudgetMonitor::SampleOnce() noexcept
{
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter_->QueryVideoMemoryInfo(nodeIndex_, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
    {
        // Keep publishing the last good snapshot rather than a bogus zero.
        queryFailures_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const VramBudgetSnapshot snapshot = MakeSnapshot(info, threshold_);
    Publish(snapshot);

    if (snapshot.isPressureHigh != lastPressureHigh_)
    {
        lastPressureHigh_ = snapshot.isPressureHigh;
        NotifyPressureChange(snapshot);
    }
}

void VramBudgetMonitor::NotifyPressureChange(const VramBudgetSnapshot& snapshot) noexcept
{
    // A throwing user callback must never take down the polling thread.
    try
    {
        VramPressureCallback callback;
        {
            std::lock_guard lock(callbackMutex_);
            callback = callback_;
        }
        if (callback)
        {
            callback(snapshot); // Invoked outside the lock: it may call SetPressureCallback.
        }
    }
    catch (...)
    {
    }
}

// =============================================================================
// Flip-model swap chain factory
// =============================================================================
std::error_code VramBudgetMonitor::ConfigureFlipModelSwapChain(IDXGIFactory2* factory, IUnknown* presentDeviceOrQueue,
                                                                HWND hwnd, const FlipModelConfig& config,
                                                                FlipModelSwapChain& out)
{
    if (factory == nullptr || presentDeviceOrQueue == nullptr || hwnd == nullptr || IsWindow(hwnd) == FALSE ||
        config.bufferCount < 2 || config.bufferCount > static_cast<UINT>(DXGI_MAX_SWAP_CHAIN_BUFFERS) ||
        !IsFlipModelFormat(config.format))
    {
        return InvalidArgument();
    }

    // Tearing is only enabled when the system actually supports it; requesting
    // the flag on an unsupported system would make swap chain creation fail.
    bool tearingSupported = false;
    if (config.allowTearing)
    {
        Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory5))))
        {
            BOOL allowTearing = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing,
                                                         sizeof(allowTearing))))
            {
                tearingSupported = (allowTearing != FALSE);
            }
        }
    }

    UINT flags = 0;
    if (tearingSupported)
    {
        flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    if (config.enableLowLatencyWait)
    {
        flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = config.width; // 0 lets DXGI use the window's client area.
    desc.Height = config.height;
    desc.Format = config.format;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1; // Flip model does not support MSAA swap chains.
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = config.bufferCount;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = flags;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
    HRESULT hr = factory->CreateSwapChainForHwnd(presentDeviceOrQueue, hwnd, &desc, nullptr, nullptr, &swapChain);
    if (FAILED(hr))
    {
        return FromHresult(hr);
    }

    // Must follow swap chain creation on the same factory to take effect.
    hr = factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(hr))
    {
        return FromHresult(hr);
    }

    UniqueWin32Handle waitable;
    if (config.enableLowLatencyWait)
    {
        Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain2;
        hr = swapChain.As(&swapChain2);
        if (FAILED(hr))
        {
            return FromHresult(hr);
        }

        const UINT latency = std::clamp(config.maxFrameLatency, 1u, config.bufferCount);
        hr = swapChain2->SetMaximumFrameLatency(latency);
        if (FAILED(hr))
        {
            return FromHresult(hr);
        }

        waitable.Reset(swapChain2->GetFrameLatencyWaitableObject());
        if (!waitable.Valid())
        {
            return FromHresult(E_FAIL);
        }
    }

    // Commit only after every step succeeded.
    out.swapChain = std::move(swapChain);
    out.frameLatencyWaitable = std::move(waitable);
    out.tearingEnabled = tearingSupported;
    out.swapChainFlags = flags;
    return {};
}

} // namespace EngineCore::Telemetry