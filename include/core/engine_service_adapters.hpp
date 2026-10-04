#pragma once
#include "core/dpc_steering_engine.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "core/engine_orchestrator.hpp"
#include "telemetry/vram_monitor.hpp"

namespace enginecore::adapters {

using Microsoft::WRL::ComPtr;

// -----------------------------------------------------------------------------
// 1. DPC steering: wired through callables until the real API is confirmed
// -----------------------------------------------------------------------------
class DpcSteeringAdapter final : public IDpcSteeringService {
public:
    explicit DpcSteeringAdapter(std::shared_ptr<::EngineCore::Core::DpcSteeringEngine> engine)
        : engine_(std::move(engine)) {}

    void SetLatencyCriticalPhase(bool active) noexcept override {
        criticalPhase_.store(active, std::memory_order_relaxed);
    }

    [[nodiscard]] DpcSteeringStats QueryStats() const noexcept override {
        if (!engine_) return {};
        try {
            const auto snap = engine_->GetSnapshot();   // CongestionSnapshot
            (void)snap;
            // TODO: map fields once I see CongestionSnapshot (lines 43-60)
        } catch (...) {}
        return {};
    }

    [[nodiscard]] bool CriticalPhaseActive() const noexcept {
        return criticalPhase_.load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<::EngineCore::Core::DpcSteeringEngine> engine_;
    std::atomic<bool> criticalPhase_{false};
};

// -----------------------------------------------------------------------------
// 2. VRAM: wraps EngineCore::Telemetry::VramBudgetMonitor
// -----------------------------------------------------------------------------
class VramBudgetAdapter final : public IVramBudgetService {
public:
    explicit VramBudgetAdapter(std::shared_ptr<::EngineCore::Telemetry::VramBudgetMonitor> monitor)
        : monitor_(std::move(monitor)) {}

    [[nodiscard]] VramBudgetStats QueryStats() const noexcept override {
        if (!monitor_) return {};
        const auto snap = monitor_->GetSnapshot();
        return VramBudgetStats{
            .budgetBytes = snap.budgetBytes,
            .usageBytes  = snap.currentUsageBytes,
            .pressure    = static_cast<float>(snap.usageFraction)
        };
    }

private:
    std::shared_ptr<::EngineCore::Telemetry::VramBudgetMonitor> monitor_;
};

// -----------------------------------------------------------------------------
// 3. GPU hint sink: D3D12 queue fence signal (atomic counter, thread-safe)
// -----------------------------------------------------------------------------
class D3D12GpuPipelineHintSink final : public IGpuPipelineHintSink {
public:
    D3D12GpuPipelineHintSink(ComPtr<ID3D12CommandQueue> queue, ComPtr<ID3D12Fence> fence)
        : queue_(std::move(queue)), fence_(std::move(fence)) {}

    void PrepareNextFramePipeline(const NextFrameHint& hint) noexcept override {
        if (!queue_ || !fence_) return;

        const std::uint64_t value = fenceValue_.fetch_add(1, std::memory_order_relaxed) + 1;
        queue_->Signal(fence_.Get(), value);

        if (hint.preferReducedResidency) {
            // TODO: Evict()/MakeResident() on low-priority heaps
        }
    }

private:
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence>        fence_;
    std::atomic<std::uint64_t> fenceValue_{0};
};

}// namespace enginecore::adapters