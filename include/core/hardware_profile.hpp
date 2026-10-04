// =============================================================================
//  EngineCore :: Hardware Profiler Layer
//  include/core/hardware_profiler.hpp
// =============================================================================
#pragma once

#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>

namespace enginecore {

using Microsoft::WRL::ComPtr;

// أنواع البروفايل المتاحة حسب نوع العتاد
enum class HardwareProfileType : std::uint8_t {
    IntegratedShared, // كارت مدمج (Intel UHD / Iris / AMD APU) يشارك الـ RAM
    DedicatedDiscrete // كارت منفصل (NVIDIA RTX / AMD RX) يملك VRAM مستقلة
};

// هيكل البيانات المحتوي على مواصفات الجهاز
struct HardwareCaps {
    HardwareProfileType profileType{HardwareProfileType::IntegratedShared};
    std::uint64_t dedicatedVramBytes{0};
    std::uint64_t sharedSystemMemoryBytes{0};
    std::uint32_t logicalCpuCores{0};
    wchar_t gpuDescription[128]{};
    bool isUMA{true};
};

class HardwareProfiler {
public:
    /// تقوم هذه الدالة بفحص العتاد عبر DirectX 12/DXGI وتحديد البروفايل المناسب تلقائياً
    [[nodiscard]] static HardwareCaps DetectHardwareProfile(IDXGIAdapter1* adapter) noexcept {
        HardwareCaps caps{};

        // 1. كشف عدد أنوية المعالج (CPU Cores)
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        caps.logicalCpuCores = sysInfo.dwNumberOfProcessors;

        // Inspect the same adapter selected for the D3D12 device.
        DXGI_ADAPTER_DESC1 desc{};
        if (adapter && SUCCEEDED(adapter->GetDesc1(&desc))) {
            caps.dedicatedVramBytes = desc.DedicatedVideoMemory;
            caps.sharedSystemMemoryBytes = desc.SharedSystemMemory;
            wcsncpy_s(caps.gpuDescription, desc.Description, 127);

            constexpr std::uint64_t kDedicatedThreshold = 512 * 1024 * 1024;
            if (caps.dedicatedVramBytes >= kDedicatedThreshold && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                caps.profileType = HardwareProfileType::DedicatedDiscrete;
                caps.isUMA = false;
            } else {
                caps.profileType = HardwareProfileType::IntegratedShared;
                caps.isUMA = true;
            }
        }
        return caps;
    }
};

} // namespace enginecore