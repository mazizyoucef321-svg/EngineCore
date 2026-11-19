// =============================================================================
//  EngineCore :: Application Entry Point
//  src/main.cpp
//
//  Pure Win32 + DXGI 1.4 + D3D12 init loop. Wires EngineOrchestrator to the
//  real DpcSteeringEngine / VramBudgetMonitor and renders a Dear ImGui
//  light-theme telemetry dashboard with an ambient RGB fog header.
//
//  BUILD NOTES (read before compiling)
//  ------------------------------------
//  * Assumes Dear ImGui (imgui.h, imgui_impl_win32.h, imgui_impl_dx12.h) is
//    already on your include path and its .cpp files are in your build.
//    This build uses the InitInfo-based ImGui_ImplDX12_Init() signature
//    (newer ImGui). If your ImGui is older (classic 6-argument signature),
//    see the comment at INIT_IMGUI_DX12 below.
//  * DpcSteeringAdapter::QueryStats() (in engine_service_adapters.hpp)
//    currently returns an empty DpcSteeringStats{} because CongestionSnapshot's
//    member layout hasn't been shared yet -> the DPC-latency plot will read 0
//    until that mapping is filled in.
//  * IPsoCompilerService has no confirmed real implementation available to
//    this file, so DemoPsoCompilerService below is a genuine, working, but
//    non-production, in-memory stand-in. Swap it for your real PSO cloud
//    network class once its header/API is available.
//  * wsprintfA (Win32's own sprintf) implements only a small legacy subset
//    of printf - notably NO %f and NO %zu - so it silently produces garbage
//    for those. Every label/overlay below that carries a float or a size_t
//    goes through FormatBuf(), a thin wrapper over the real CRT vsnprintf,
//    instead.
//  * PresentMonHooks (telemetry/PresentMon_hooks.hpp) is wired in as an
//    INDEPENDENT, ADDITIONAL early-pipeline-hint trigger: it fires on real
//    ETW-observed present cadence, whereas EngineOrchestrator's own
//    EarlySpeculativePipelineWarningAgent fires on CPU frame progress. Both
//    feed the same deps.gpuHintSink. See PresentMon_hooks.hpp's file-header
//    note for why these are complementary signals, not duplicate ones.
// =============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

#include "core/engine_orchestrator.hpp"
#include "core/engine_service_adapters.hpp"
#include "core/dpc_steering_engine.hpp"
#include "network/pso_cloud_network.hpp"
#include "core/hardware_profile.hpp"
#include "core/security_manager.hpp"
#include "dsv/vault_manager.hpp"
#include "telemetry/vram_monitor.hpp"
#include "telemetry/PresentMon_hooks.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// =============================================================================
//  Small helpers
// =============================================================================
constexpr UINT kFrameCount = 3;

ID3D12DescriptorHeap* g_imguiSrvHeap = nullptr;

float  g_dpiScale = 1.0f;                           // set once in wWinMain (system DPI / 96)
ImVec4 g_accent(0.03f, 0.57f, 0.70f, 1.0f);         // brand accent, set by ApplyEngineCoreTheme
std::atomic<bool> g_resizePending{false};           // WM_SIZE -> main loop (swap chain resize)
std::atomic<UINT> g_resizeW{0}, g_resizeH{0};

void FatalIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[512];
        wsprintfA(buf, "EngineCore fatal error\n\n%s\nHRESULT = 0x%08X", what, static_cast<unsigned>(hr));
        MessageBoxA(nullptr, buf, "EngineCore", MB_OK | MB_ICONERROR);
        std::exit(1);
    }
}

template <std::size_t N>
void FormatBuf(char (&buf)[N], const char* fmt, ...) noexcept {
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, N, fmt, args);
    va_end(args);
    buf[N - 1] = '\0';
}

[[nodiscard]] double QpcToMs(std::int64_t ticks, std::int64_t freq) noexcept {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq);
}

template <std::size_t N>
class RingBuffer {
public:
    void Push(float v) noexcept {
        data_[head_] = v;
        head_ = (head_ + 1) % N;
        if (count_ < N) ++count_;
    }
    [[nodiscard]] std::array<float, N> Ordered() const noexcept {
        std::array<float, N> out{};
        for (std::size_t i = 0; i < count_; ++i)
            out[i] = data_[(head_ + N - count_ + i) % N];
        return out;
    }
    [[nodiscard]] std::size_t Count() const noexcept { return count_; }
    [[nodiscard]] float Max() const noexcept {
        float m = 0.0f;
        for (std::size_t i = 0; i < count_; ++i) m = std::max(m, data_[i]);
        return m;
    }
    [[nodiscard]] float Avg() const noexcept {
        if (count_ == 0) return 0.0f;
        float sum = 0.0f;
        for (std::size_t i = 0; i < count_; ++i) sum += data_[i];
        return sum / static_cast<float>(count_);
    }

private:
    std::array<float, N> data_{};
    std::size_t head_{0};
    std::size_t count_{0};
};

class DemoPsoCompilerService final : public enginecore::IPsoCompilerService {
public:
    bool EnqueueCompile(const enginecore::PsoCompileRequest& rq) noexcept override {
        if (!enabled_.load(std::memory_order_relaxed)) return false;
        std::scoped_lock lock(mutex_);
        if (queue_.size() >= kMaxQueued) queue_.pop_front();
        queue_.push_back(rq);
        enqueuedTotal_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void PurgeBefore(std::uint64_t generation) noexcept override {
        std::scoped_lock lock(mutex_);
        std::erase_if(queue_, [generation](const auto& r) { return r.generation < generation; });
    }

    void SetEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool Enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t EnqueuedTotal() const noexcept {
        return enqueuedTotal_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::size_t QueueDepth() const noexcept {
        std::scoped_lock lock(mutex_);
        return queue_.size();
    }

private:
    static constexpr std::size_t kMaxQueued = 512;
    mutable std::mutex mutex_;
    std::deque<enginecore::PsoCompileRequest> queue_;
    std::atomic<bool> enabled_{true};
    std::atomic<std::uint64_t> enqueuedTotal_{0};
};

// -----------------------------------------------------------------------------
//  PresentCadenceHintBridge
//  Bridges PresentMon_hooks.hpp's real present-cadence trigger into the same
//  GPU hint sink the orchestrator's CPU-progress-based agent already drives.
//  Two independent triggers feeding one call - see PresentMon_hooks.hpp's
//  file-header note on why that's complementary, not duplicate, signaling.
//  OnPresentTelemetry() runs on the ETW pump thread: no locks, no allocation,
//  matching the noexcept/non-blocking contract IEarlyPipelineHintSink requires.
// -----------------------------------------------------------------------------
class PresentCadenceHintBridge final : public EngineCore::Telemetry::IEarlyPipelineHintSink {
public:
    explicit PresentCadenceHintBridge(std::shared_ptr<enginecore::IGpuPipelineHintSink> sink)
        : sink_(std::move(sink)) {}

    void OnPresentTelemetry(const EngineCore::Telemetry::PresentTelemetryData& data) noexcept override {
        if (!sink_ || data.kind != EngineCore::Telemetry::PresentEventKind::PresentStart) return;
        enginecore::NextFrameHint hint{};
        hint.targetFrameIndex = data.frameSequence + 1;
        hint.timestampQpc = static_cast<std::uint64_t>(data.presentStartQpc);
        hint.expectedFrameTimeMs = data.presentIntervalUs > 0.0f ? data.presentIntervalUs / 1000.0f : 16.667f;
        sink_->PrepareNextFramePipeline(hint);
    }

private:
    std::shared_ptr<enginecore::IGpuPipelineHintSink> sink_;
};

class DemoVramResourcePool {
public:
    void CreateEntries(ID3D12Device* device, std::size_t count, UINT64 bytesEach) {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytesEach;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (std::size_t i = 0; i < count; ++i) {
            ComPtr<ID3D12Resource> res;
            const HRESULT hr = device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&res));
            if (SUCCEEDED(hr)) entries_.push_back({std::move(res), 0, bytesEach});
        }
    }

    void Touch(std::size_t index, std::uint64_t frameIndex) noexcept {
        if (index < entries_.size()) entries_[index].lastUsedFrame = frameIndex;
    }

    std::size_t FlushInactive(std::uint64_t currentFrame, std::uint64_t staleFrames) {
        std::size_t evicted = 0;
        for (auto& e : entries_) {
            if (e.resource && (currentFrame - e.lastUsedFrame) >= staleFrames) {
                e.resource.Reset();
                ++evicted;
            }
        }
        return evicted;
    }

    [[nodiscard]] std::uint64_t EstimateFreeableBytes(std::uint64_t currentFrame,
                                                       std::uint64_t staleFrames) const noexcept {
        std::uint64_t bytes = 0;
        for (const auto& e : entries_) {
            if (e.resource && (currentFrame - e.lastUsedFrame) >= staleFrames) bytes += e.sizeBytes;
        }
        return bytes;
    }

    [[nodiscard]] std::size_t LiveCount() const noexcept {
        std::size_t n = 0;
        for (auto& e : entries_) if (e.resource) ++n;
        return n;
    }
    [[nodiscard]] std::size_t TotalCount() const noexcept { return entries_.size(); }

private:
    struct Entry {
        ComPtr<ID3D12Resource> resource;
        std::uint64_t lastUsedFrame{0};
        UINT64 sizeBytes{0};
    };
    std::vector<Entry> entries_;
};

constexpr std::uint64_t kDemoStaleFrames = 1;

bool ReadWindowsDarkMode(bool& darkMode) {
    DWORD appsUseLightTheme = 1;
    DWORD valueSize = sizeof(appsUseLightTheme);
    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &appsUseLightTheme,
        &valueSize);
    if (status != ERROR_SUCCESS)
        return false;

    darkMode = appsUseLightTheme == 0;
    return true;
}

void ApplyEngineCoreTheme(bool darkMode) {
    ImGuiStyle& style = ImGui::GetStyle();
    const float sc = g_dpiScale;
    style.WindowRounding = 0.0f;
    style.FrameRounding = 10.0f * sc;
    style.GrabRounding = 10.0f * sc;
    style.ChildRounding = 10.0f * sc;
    style.PopupRounding = 10.0f * sc;
    style.ScrollbarRounding = 10.0f * sc;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 1.0f;
    style.ItemSpacing = ImVec2(10.0f * sc, 10.0f * sc);
    style.FramePadding = ImVec2(10.0f * sc, 6.0f * sc);
    style.CellPadding = ImVec2(8.0f * sc, 6.0f * sc);
    style.WindowPadding = ImVec2(20.0f * sc, 18.0f * sc);
    // Brand accent (matches the EngineCore logo's cyan).
    g_accent = darkMode ? ImVec4(0.13f, 0.83f, 0.93f, 1.00f) : ImVec4(0.03f, 0.57f, 0.70f, 1.00f);

    ImVec4* c = style.Colors;
    const ImVec4 paper = darkMode ? ImVec4(0.00f, 0.00f, 0.00f, 1.00f)
                                  : ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    const ImVec4 paperChild = darkMode ? ImVec4(0.07f, 0.07f, 0.08f, 1.00f)
                                       : ImVec4(0.93f, 0.93f, 0.94f, 1.00f);
    const ImVec4 ink = darkMode ? ImVec4(0.94f, 0.94f, 0.95f, 1.00f)
                                : ImVec4(0.07f, 0.07f, 0.08f, 1.00f);
    const ImVec4 inkFaint = darkMode ? ImVec4(0.62f, 0.62f, 0.65f, 1.00f)
                                     : ImVec4(0.45f, 0.45f, 0.47f, 1.00f);
    const ImVec4 border = darkMode ? ImVec4(0.38f, 0.38f, 0.42f, 0.75f)
                                   : ImVec4(0.55f, 0.55f, 0.58f, 0.70f);
    const ImVec4 shade = darkMode ? ImVec4(1.00f, 1.00f, 1.00f, 1.00f)
                                  : ImVec4(0.00f, 0.00f, 0.00f, 1.00f);

    c[ImGuiCol_Text]                 = ink;
    c[ImGuiCol_TextDisabled]         = inkFaint;
    c[ImGuiCol_WindowBg]             = paper;
    c[ImGuiCol_ChildBg]              = paperChild;
    c[ImGuiCol_PopupBg]              = darkMode ? ImVec4(0.08f, 0.08f, 0.09f, 0.98f)
                                                  : ImVec4(0.99f, 0.99f, 1.00f, 0.98f);
    c[ImGuiCol_Border]               = border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = ImVec4(shade.x, shade.y, shade.z, 0.055f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(shade.x, shade.y, shade.z, 0.10f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(shade.x, shade.y, shade.z, 0.16f);
    c[ImGuiCol_TitleBg]              = paper;
    c[ImGuiCol_TitleBgActive]        = paper;
    c[ImGuiCol_MenuBarBg]            = paper;
    c[ImGuiCol_ScrollbarBg]          = paper;
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(shade.x, shade.y, shade.z, 0.15f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(shade.x, shade.y, shade.z, 0.25f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(shade.x, shade.y, shade.z, 0.35f);
    c[ImGuiCol_CheckMark]            = g_accent;
    c[ImGuiCol_SliderGrab]           = ImVec4(shade.x, shade.y, shade.z, 0.5f);
    c[ImGuiCol_SliderGrabActive]     = ink;
    c[ImGuiCol_Button]               = ImVec4(shade.x, shade.y, shade.z, 0.05f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(shade.x, shade.y, shade.z, 0.10f);
    c[ImGuiCol_ButtonActive]         = ImVec4(shade.x, shade.y, shade.z, 0.16f);
    c[ImGuiCol_Header]               = ImVec4(shade.x, shade.y, shade.z, 0.06f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(shade.x, shade.y, shade.z, 0.11f);
    c[ImGuiCol_HeaderActive]         = ImVec4(shade.x, shade.y, shade.z, 0.16f);
    c[ImGuiCol_Separator]            = border;
    c[ImGuiCol_PlotLines]            = g_accent;
    c[ImGuiCol_PlotLinesHovered]     = ImVec4(g_accent.x, g_accent.y, g_accent.z, 0.85f);
    c[ImGuiCol_PlotHistogram]        = g_accent;
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(g_accent.x, g_accent.y, g_accent.z, 0.85f);
}

// Ambient RGB header. Cost: 33 HSV conversions + 64 gradient quads + 24 soft circles
// per frame (the first version did ~480 conversions and 240 quads). `t` is supplied
// by the caller so the animation can be frozen while the window is in the background.
void DrawRgbFogHeaderBar(float width, float height, float t, const char* title, const char* subtitle) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + width, p0.y + height);

    dl->PushClipRect(p0, p1, true);

    constexpr int kSegments = 32;
    constexpr float kHueSpeed = 0.05f;
    const float segW = width / static_cast<float>(kSegments);

    std::array<ImVec4, kSegments + 1> cols{};
    for (int i = 0; i <= kSegments; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(kSegments);
        const float drift = 0.05f * std::sin(t * 0.6f + u * 6.2831853f);
        const float hue = std::fmod(t * kHueSpeed + u + drift + 1.0f, 1.0f);
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(hue, 0.90f, 1.0f, r, g, b);
        cols[static_cast<std::size_t>(i)] = ImVec4(r, g, b, 1.0f);
    }

    // Two stacked rows, each a true vertical gradient -> smooth fade, no banding.
    constexpr float kAlphas[3] = {0.34f, 0.10f, 0.0f};
    for (int row = 0; row < 2; ++row) {
        const float y0 = p0.y + height * (row == 0 ? 0.0f : 0.5f);
        const float y1 = p0.y + height * (row == 0 ? 0.5f : 1.0f);
        const float aTop = kAlphas[row], aBot = kAlphas[row + 1];
        for (int i = 0; i < kSegments; ++i) {
            const ImVec4& a = cols[static_cast<std::size_t>(i)];
            const ImVec4& b = cols[static_cast<std::size_t>(i) + 1];
            const ImU32 tl = ImGui::ColorConvertFloat4ToU32(ImVec4(a.x, a.y, a.z, aTop));
            const ImU32 tr = ImGui::ColorConvertFloat4ToU32(ImVec4(b.x, b.y, b.z, aTop));
            const ImU32 br = ImGui::ColorConvertFloat4ToU32(ImVec4(b.x, b.y, b.z, aBot));
            const ImU32 bl = ImGui::ColorConvertFloat4ToU32(ImVec4(a.x, a.y, a.z, aBot));
            dl->AddRectFilledMultiColor(ImVec2(p0.x + segW * static_cast<float>(i), y0),
                                        ImVec2(p0.x + segW * static_cast<float>(i + 1), y1), tl, tr, br, bl);
        }
    }

    struct Puff { float phase, speed, ySpread, radius, satur; };
    static const std::array<Puff, 4> kPuffs{{
        {0.00f, 0.08f, 0.35f, 90.0f, 0.80f},
        {0.27f, -0.05f, 0.55f, 70.0f, 0.85f},
        {0.55f, 0.11f, 0.20f, 110.0f, 0.75f},
        {0.81f, -0.07f, 0.65f, 60.0f, 0.90f},
    }};
    constexpr int kRings = 6;
    for (std::size_t i = 0; i < kPuffs.size(); ++i) {
        const Puff& puff = kPuffs[i];
        const float x = p0.x + width * std::fmod(t * puff.speed + puff.phase + 4.0f, 1.0f);
        const float y = p0.y + height * (0.5f + puff.ySpread * 0.5f * std::sin(t * 0.3f + static_cast<float>(i)));
        const float hue = std::fmod(t * kHueSpeed + puff.phase, 1.0f);
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(hue, puff.satur, 1.0f, r, g, b);
        const float radius = puff.radius * g_dpiScale;
        for (int ring = kRings; ring >= 1; --ring) {
            const float ringT = static_cast<float>(ring) / static_cast<float>(kRings);
            dl->AddCircleFilled(ImVec2(x, y), radius * ringT,
                                ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 0.09f * (1.0f - ringT))), 24);
        }
    }

    // Title text drawn over the fog (the fog is faint, so plain ink stays readable).
    if (title) {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float titleSize = ImGui::GetFontSize() * 1.6f;
        const float padX = st.WindowPadding.x * 0.8f;
        const ImU32 ink = ImGui::ColorConvertFloat4ToU32(st.Colors[ImGuiCol_Text]);
        const ImU32 faint = ImGui::ColorConvertFloat4ToU32(st.Colors[ImGuiCol_TextDisabled]);
        const ImVec2 titlePos(p0.x + padX, p0.y + (height - titleSize) * 0.5f);
        dl->AddText(ImGui::GetFont(), titleSize, titlePos, ink, title);
        if (subtitle) {
            const float titleW = ImGui::GetFont()->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, title).x;
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                        ImVec2(titlePos.x + titleW + 14.0f * g_dpiScale,
                               p0.y + (height - ImGui::GetFontSize()) * 0.5f + 2.0f * g_dpiScale),
                        faint, subtitle);
        }
    }

    dl->PopClipRect();
    ImGui::Dummy(ImVec2(width, height));
}

bool LiquidGlassButton(const char* label, const ImVec2& size, bool active = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 realSize(size.x, size.y);
    const bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + realSize.x, pos.y + realSize.y));
    const ImVec4 shade = ImGui::GetStyle().Colors[ImGuiCol_Text];

    if (hovered) {
        for (int i = 3; i >= 1; --i) {
            const float expand = static_cast<float>(i) * 3.0f;
            const float alpha = 0.05f / static_cast<float>(i);
            dl->AddRectFilled(ImVec2(pos.x - expand, pos.y - expand),
                              ImVec2(pos.x + realSize.x + expand, pos.y + realSize.y + expand),
                              ImGui::ColorConvertFloat4ToU32(ImVec4(shade.x, shade.y, shade.z, alpha)),
                              16.0f);
        }
    }

    ImGui::PushStyleColor(ImGuiCol_Button,
        ImVec4(shade.x, shade.y, shade.z, active ? 0.14f : 0.045f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(shade.x, shade.y, shade.z, 0.09f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(shade.x, shade.y, shade.z, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_Border,
        active ? ImVec4(g_accent.x, g_accent.y, g_accent.z, 0.95f)
               : ImVec4(shade.x, shade.y, shade.z, hovered ? 0.40f : 0.15f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f);

    const bool clicked = ImGui::Button(label, realSize);

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return clicked;
}

// -----------------------------------------------------------------------------
//  PowerToggleButton: the master system on/off control. Deliberately styled
//  differently from LiquidGlassButton (pill shape, semantic green/red,
//  colored glow) so it reads as THE power switch, not just another toggle
//  in the row below it.
// -----------------------------------------------------------------------------
bool PowerToggleButton(const char* label, bool isOn, const ImVec2& size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool hovered = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));

    const ImVec4 onColor(0.11f, 0.52f, 0.26f, 1.0f);
    const ImVec4 offColor(0.70f, 0.16f, 0.16f, 1.0f);
    const ImVec4 base = isOn ? onColor : offColor;
    const float pillRounding = size.y * 0.5f;

    if (hovered) {
        for (int i = 3; i >= 1; --i) {
            const float expand = static_cast<float>(i) * 4.0f;
            const float alpha = 0.10f / static_cast<float>(i);
            dl->AddRectFilled(ImVec2(pos.x - expand, pos.y - expand),
                              ImVec2(pos.x + size.x + expand, pos.y + size.y + expand),
                              ImGui::ColorConvertFloat4ToU32(ImVec4(base.x, base.y, base.z, alpha)),
                              pillRounding + expand);
        }
    }

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(base.x, base.y, base.z, isOn ? 0.20f : 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(base.x, base.y, base.z, 0.28f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(base.x, base.y, base.z, 0.36f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(base.x, base.y, base.z, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(base.x * 0.75f, base.y * 0.75f, base.z * 0.75f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, pillRounding);

    const bool clicked = ImGui::Button(label, size);

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    return clicked;
}

// -----------------------------------------------------------------------------
//  StatusDot: a small filled circle (drawn via ImDrawList, so it never
//  depends on the active font having a glyph for a bullet/circle character)
//  followed by a label, used for the subsystem health strip.
// -----------------------------------------------------------------------------
void StatusDot(const char* label, const ImVec4& color, float radius = 5.0f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float lineHeight = ImGui::GetTextLineHeight();
    dl->AddCircleFilled(ImVec2(pos.x + radius, pos.y + lineHeight * 0.5f), radius,
                        ImGui::ColorConvertFloat4ToU32(color), 16);
    ImGui::Dummy(ImVec2(radius * 2.0f + 6.0f, lineHeight));
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextUnformatted(label);
}

[[nodiscard]] float StatusDotWidth(const char* label, float radius = 5.0f) {
    return radius * 2.0f + 6.0f + 4.0f + ImGui::CalcTextSize(label).x;
}

// Keeps the next item on the current line if it still fits in the window, otherwise
// lets it wrap to the next line (so chip rows never overflow a narrow window).
void SameLineWrap(float nextItemWidth, float spacing) {
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    if (ImGui::GetItemRectMax().x + spacing + nextItemWidth <= right) ImGui::SameLine(0.0f, spacing);
}

[[nodiscard]] std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// One telemetry plot: title, line plot filling the cell width, one-line footer.
template <std::size_t N>
void PlotCard(const char* title, const char* id, const RingBuffer<N>& rb, float yMin, float yMax,
              const char* overlay, const char* footer, float height) {
    ImGui::TextUnformatted(title);
    const auto data = rb.Ordered();
    ImGui::PlotLines(id, data.data(), static_cast<int>(rb.Count()), 0, overlay, yMin, yMax,
                     ImVec2(ImGui::GetContentRegionAvail().x, height));
    ImGui::TextDisabled("%s", footer);
}

std::atomic<bool> g_running{true};
constexpr WORD kAppIconResourceId = 101;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                g_resizeW.store(LOWORD(lParam), std::memory_order_relaxed);
                g_resizeH.store(HIWORD(lParam), std::memory_order_relaxed);
                g_resizePending.store(true, std::memory_order_release);
            }
            return 0;
        case WM_DESTROY:
            g_running.store(false, std::memory_order_relaxed);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

ComPtr<IDXGIAdapter1> PickHardwareAdapter(IDXGIFactory4* factory) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        __uuidof(ID3D12Device), nullptr)))
            return adapter;
    }
    return nullptr;
}

// Enables/disables a privilege that is already present in this process's token.
// Disabling always works; enabling only works if the token holds the privilege.
bool SetPrivilegeEnabled(const wchar_t* name, bool enable) noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return false;
    }
    tp.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    const bool ok = (GetLastError() == ERROR_SUCCESS); // ERROR_NOT_ALL_ASSIGNED => not held
    CloseHandle(token);
    return ok;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // Per-monitor DPI awareness: without it Windows bitmap-stretches the whole
    // window (the blurry text in the old build). Must run before any window exists.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_dpiScale = std::clamp(static_cast<float>(GetDpiForSystem()) / 96.0f, 1.0f, 3.0f);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    wc.lpszClassName = L"EngineCoreWindowClass";
    RegisterClassExW(&wc);

    const HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"EngineCore :: Telemetry Orchestrator",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        static_cast<int>(1280 * g_dpiScale), static_cast<int>(820 * g_dpiScale),
        nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    ComPtr<IDXGIFactory4> factory;
    FatalIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2 failed");

    ComPtr<IDXGIAdapter1> adapter1 = PickHardwareAdapter(factory.Get());
    if (!adapter1) { MessageBoxA(nullptr, "No D3D12-capable hardware adapter found.", "EngineCore", MB_ICONERROR); return 1; }

    ComPtr<ID3D12Device> device;
    FatalIfFailed(D3D12CreateDevice(adapter1.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)),
                 "D3D12CreateDevice failed");

    const enginecore::HardwareCaps hwProfile =
        enginecore::HardwareProfiler::DetectHardwareProfile(adapter1.Get());
    {
        char dbg[256];
        const char* profileName = hwProfile.profileType == enginecore::HardwareProfileType::IntegratedShared
            ? "IntegratedShared" : "DedicatedDiscrete";
        std::snprintf(dbg, sizeof(dbg),
                     "EngineCore: hardware profile = %s (UMA=%d, dedicated VRAM=%.0f MB, %u logical cores)\n",
                     profileName, hwProfile.isUMA,
                     static_cast<double>(hwProfile.dedicatedVramBytes) / (1024.0 * 1024.0),
                     hwProfile.logicalCpuCores);
        OutputDebugStringA(dbg);
    }

    // -------------------------------------------------------------------
    // 3b. Security / privilege check, before anything that needs it.
    //     DpcSteeringEngine's ETW session (step 9) needs SeSystemProfilePrivilege
    //     to trace kernel DPC/ISR events; RequestPrivilege can only ENABLE a
    //     privilege already present (disabled) in this process's token, never
    //     grant one that isn't there - so this is a no-op, not an elevation,
    //     on a standard non-admin token. We request it unconditionally and
    //     just log what happened; DpcSteeringEngine already falls back to its
    //     documented degraded (metrics-only) mode on ETW failure either way.
    //     PresentMonHooks (step 9c) has the same ETW-privilege dependency and
    //     the same documented degrade-gracefully behavior.
    // -------------------------------------------------------------------
    const bool isAdmin = EngineCore::Security::SecurityManager::IsRunAsAdmin();
    const bool gotProfilePrivilege =
        EngineCore::Security::SecurityManager::RequestPrivilege(L"SeSystemProfilePrivilege");
    {
        char dbg[160];
        std::snprintf(dbg, sizeof(dbg), "EngineCore: running as admin=%d, SeSystemProfilePrivilege granted=%d\n",
                     isAdmin, gotProfilePrivilege);
        OutputDebugStringA(dbg);
    }

    D3D12_COMMAND_QUEUE_DESC gqDesc{};
    gqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> graphicsQueue;
    FatalIfFailed(device->CreateCommandQueue(&gqDesc, IID_PPV_ARGS(&graphicsQueue)), "CreateCommandQueue (graphics) failed");

    D3D12_COMMAND_QUEUE_DESC cqDesc{};
    cqDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> hintComputeQueue;
    FatalIfFailed(device->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&hintComputeQueue)), "CreateCommandQueue (compute) failed");

    ComPtr<ID3D12Fence> hintFence;
    FatalIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hintFence)), "CreateFence (hint) failed");

    EngineCore::Telemetry::FlipModelConfig swapCfg{};
    swapCfg.bufferCount = kFrameCount;
    swapCfg.allowTearing = true;
    swapCfg.enableLowLatencyWait = true;
    swapCfg.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapCfg.maxFrameLatency = 1;

    EngineCore::Telemetry::FlipModelSwapChain flip{};
    {
        const std::error_code ec = EngineCore::Telemetry::VramBudgetMonitor::ConfigureFlipModelSwapChain(
            factory.Get(), graphicsQueue.Get(), hwnd, swapCfg, flip);
        if (ec) FatalIfFailed(E_FAIL, ec.message().c_str());
    }
    ComPtr<IDXGISwapChain3> swapChain3;
    FatalIfFailed(flip.swapChain.As(&swapChain3), "QueryInterface(IDXGISwapChain3) failed");

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    FatalIfFailed(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)), "CreateDescriptorHeap (RTV) failed");
    const UINT rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    FatalIfFailed(device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)), "CreateDescriptorHeap (SRV) failed");

    std::array<ComPtr<ID3D12Resource>, kFrameCount> backBuffers;
    auto CreateRtvs = [&] {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kFrameCount; ++i) {
            FatalIfFailed(flip.swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])), "swapChain->GetBuffer failed");
            device->CreateRenderTargetView(backBuffers[i].Get(), nullptr, h);
            h.ptr += rtvStride;
        }
    };
    CreateRtvs();

    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> cmdAllocs;
    for (UINT i = 0; i < kFrameCount; ++i)
        FatalIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAllocs[i])),
                     "CreateCommandAllocator failed");

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    FatalIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAllocs[0].Get(), nullptr,
                                            IID_PPV_ARGS(&cmdList)), "CreateCommandList failed");
    cmdList->Close();

    ComPtr<ID3D12Fence> frameFence;
    FatalIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&frameFence)), "CreateFence (frame) failed");
    std::array<UINT64, kFrameCount> frameFenceValues{};
    UINT64 nextFenceValue = 1;
    const HANDLE frameFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    auto WaitForFrame = [&](UINT frameIdx) {
        const UINT64 target = frameFenceValues[frameIdx];
        if (target != 0 && frameFence->GetCompletedValue() < target) {
            frameFence->SetEventOnCompletion(target, frameFenceEvent);
            WaitForSingleObject(frameFenceEvent, INFINITE);
        }
    };

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    {
        // Segoe UI instead of ImGui's built-in pixel font; falls back to the built-in one.
        wchar_t winDir[MAX_PATH]{};
        std::string fontPath;
        if (GetWindowsDirectoryW(winDir, MAX_PATH) > 0) fontPath = WideToUtf8(winDir) + "\\Fonts\\segoeui.ttf";
        const float fontPx = std::round(17.0f * g_dpiScale);
        ImFont* font = fontPath.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(fontPath.c_str(), fontPx);
        if (!font) {
            ImFontConfig fc;
            fc.SizePixels = fontPx;
            io.Fonts->AddFontDefault(&fc);
        }
    }
    ImGui::StyleColorsLight();
    bool darkTheme = false;
    ReadWindowsDarkMode(darkTheme);
    ApplyEngineCoreTheme(darkTheme);

    ImGui_ImplWin32_Init(hwnd);

    g_imguiSrvHeap = srvHeap.Get();

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = device.Get();
    initInfo.CommandQueue = graphicsQueue.Get();
    initInfo.NumFramesInFlight = kFrameCount;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    initInfo.SrvDescriptorHeap = srvHeap.Get();
    initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
                                       D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
        *outCpu = g_imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
        *outGpu = g_imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
    };
    initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE,
                                      D3D12_GPU_DESCRIPTOR_HANDLE) {};
    ImGui_ImplDX12_Init(&initInfo);

    auto dpcEngine = std::make_shared<EngineCore::Core::DpcSteeringEngine>();
    if (const std::error_code ec = dpcEngine->Start(); ec) {
        OutputDebugStringA(("DpcSteeringEngine::Start failed: " + ec.message() + "\n").c_str());
    } else {
        dpcEngine->RegisterCurrentThread();
    }

    enginecore::OrchestratorConfig orchConfig{};
    if (hwProfile.profileType == enginecore::HardwareProfileType::IntegratedShared) {
        orchConfig.earlyWarningProgress = 0.40f;
        orchConfig.highVramPressure = 0.85f;
    }

    auto vramMonitor = std::make_shared<EngineCore::Telemetry::VramBudgetMonitor>();
    {
        ComPtr<IDXGIAdapter> plainAdapter;
        adapter1.As(&plainAdapter);
        const std::error_code ec = vramMonitor->Initialize(plainAdapter.Get(), orchConfig.highVramPressure,
                                                            std::chrono::milliseconds(500));
        if (ec) OutputDebugStringA(("VramBudgetMonitor::Initialize failed: " + ec.message() + "\n").c_str());
    }

    auto psoService = std::make_shared<DemoPsoCompilerService>();

    DemoVramResourcePool vramPool;
    vramPool.CreateEntries(device.Get(), 32, 4 * 1024 * 1024);

    // -------------------------------------------------------------------
    // 9b. DSV shader-cache vault: one-time startup integrity scan + purge of
    //     zero-byte/corrupted cache entries, before anything starts reading
    //     from this directory. Fully async (ScanAndPurgeAsync) so it never
    //     blocks window creation or the first frame; a short-lived detached
    //     thread just waits on the future and logs the outcome.
    // -------------------------------------------------------------------
    const std::filesystem::path shaderCacheDir =
        EngineCore::Security::SecurityManager::GetSecuredAppDataPath(L"ShaderCache");
    auto shaderVault = std::make_shared<EngineCore::Dsv::VaultManager>(shaderCacheDir);
    {
        EngineCore::Dsv::ScanOptions scanOpts{};
        scanOpts.requireManifest = true;
        EngineCore::Dsv::PurgeOptions purgeOpts{};
        auto scanFuture = std::make_shared<std::future<EngineCore::Dsv::VaultManager::ScanAndPurgeResult>>(
            shaderVault->ScanAndPurgeAsync(scanOpts, purgeOpts));
        std::thread([shaderVault, scanFuture] {
            const auto result = scanFuture->get();
            char dbg[192];
            std::snprintf(dbg, sizeof(dbg),
                         "EngineCore: shader vault startup pass - scanned=%zu purged=%zu skipped(locked)=%zu failed=%zu\n",
                         result.scan.size(), result.purge.purged.size(), result.purge.skippedLocked.size(),
                         result.purge.failed.size());
            OutputDebugStringA(dbg);
        }).detach();
    }

    enginecore::EngineOrchestrator::Dependencies deps;
    deps.dpcSteering   = std::make_shared<enginecore::adapters::DpcSteeringAdapter>(dpcEngine);
    deps.vramMonitor   = std::make_shared<enginecore::adapters::VramBudgetAdapter>(vramMonitor);
    deps.gpuHintSink   = std::make_shared<enginecore::adapters::D3D12GpuPipelineHintSink>(hintComputeQueue, hintFence);
    deps.psoCompiler   = psoService;

    enginecore::EngineOrchestrator orchestrator(deps, orchConfig);
    const DWORD targetPid = GetCurrentProcessId();
    HRESULT awaken = orchestrator.AwakenSystem(targetPid);
    bool etwDegraded = (awaken == S_FALSE);
    if (FAILED(awaken)) {
        OutputDebugStringA("EngineOrchestrator::AwakenSystem failed - telemetry will not run.\n");
    }

    // -------------------------------------------------------------------
    // 9c. PresentMonHooks: a second, independent ETW-driven early-pipeline-
    //     hint trigger based on REAL present cadence, feeding the same
    //     deps.gpuHintSink the orchestrator's CPU-progress-based agent
    //     already drives. See PresentMon_hooks.hpp and the file-header note
    //     at the top of this file for why these are complementary, not
    //     duplicate, signals. Degrades gracefully (no admin/Performance Log
    //     Users membership -> DegradedNoPrivilege, not a crash) exactly like
    //     DpcSteeringEngine and EngineOrchestrator's own ETW session do.
    //     pmCfg is kept in outer scope (not just this block) so the master
    //     power toggle in the dashboard can call presentMonHooks.Start(pmCfg)
    //     again later without having to reconstruct it.
    // -------------------------------------------------------------------
    auto presentHintBridge = std::make_shared<PresentCadenceHintBridge>(deps.gpuHintSink);
    EngineCore::Telemetry::PresentMonHooks presentMonHooks;
    EngineCore::Telemetry::PresentMonConfig pmCfg{};
    {
        pmCfg.targetProcessId = targetPid;
        pmCfg.hintSink = presentHintBridge.get();
        pmCfg.safetyFloorFps = 45.0;
        const auto pmResult = presentMonHooks.Start(pmCfg);
        if (pmResult == EngineCore::Telemetry::StartResult::DegradedNoPrivilege) {
            OutputDebugStringA("EngineCore: PresentMonHooks degraded - no ETW privilege.\n");
        } else if (pmResult != EngineCore::Telemetry::StartResult::Active) {
            OutputDebugStringA("EngineCore: PresentMonHooks failed to start.\n");
        }
    }

    LARGE_INTEGER qpcFreq{};
    QueryPerformanceFrequency(&qpcFreq);
    LARGE_INTEGER prevTime{};
    QueryPerformanceCounter(&prevTime);

    // Plots are sampled at 10 Hz (not once per frame) so 240 samples = 24 s of history
    // regardless of frame rate; frame-work samples keep the worst frame in each bucket.
    RingBuffer<240> vramPlot, workPlot, dpcPlot;
    constexpr double kPlotSampleMs = 100.0;
    double plotAccumMs = 0.0;
    float plotWorkMax = 0.0f, plotDpcMax = 0.0f;
    double lastWorkMs = 0.0;   // CPU time spent building+submitting the previous frame (excludes vsync wait)
    float animTime = 0.0f;     // header animation clock; frozen while the window is in the background

    const std::string gpuName = WideToUtf8(hwProfile.gpuDescription);
    const char* const profileLabel = hwProfile.profileType == enginecore::HardwareProfileType::IntegratedShared
        ? "Integrated / shared memory" : "Dedicated GPU";
    UINT backW = 0, backH = 0;
    {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        backW = static_cast<UINT>(rc.right - rc.left);
        backH = static_cast<UINT>(rc.bottom - rc.top);
    }
    std::uint64_t appFrameIndex = 0;

    // Subsystem toggle state, hoisted above the loop (still `static` inside
    // wWinMain would work too, but these only ever need one instance and
    // the master power switch below needs to read/write all four from one
    // place, so plain locals read better here than four separate statics).
    bool dpcSteeringOn = true; // dpcEngine->Start() already ran once, above (degrades gracefully if ETW isn't available).
    bool psoAgentOn = true;
    bool etwSessionOn = true; // orchestrator.AwakenSystem() already ran once, above.
    bool presentMonOn = true; // presentMonHooks.Start() already ran once, above.
    bool systemOn = true;     // master switch; true because every subsystem above was started at launch.
    bool themeManuallySelected = false;
    ULONGLONG nextThemeCheck = GetTickCount64();

        bool lowModeOn = false;
    bool prevDpcOn = false, prevEtwOn = false, prevPmOn = false;

    auto SetLowMode = [&](bool enable) {
        if (enable == lowModeOn) return;
        lowModeOn = enable;
        if (enable) {
            // Remember what was running so turning Low Mode off restores it.
            prevDpcOn = dpcSteeringOn;
            prevEtwOn = etwSessionOn;
            prevPmOn  = presentMonOn;

            // Stop the ETW consumers first, then drop the privilege they needed.
            if (dpcSteeringOn) dpcEngine->Stop();
            if (etwSessionOn)  orchestrator.Shutdown();
            if (presentMonOn)  presentMonHooks.Stop();
            EngineCore::Security::SecurityManager::IsRunAsAdmin(); // no-op; keeps include used
            SetPrivilegeEnabled(L"SeSystemProfilePrivilege", false);

            dpcSteeringOn = etwSessionOn = presentMonOn = false;
            OutputDebugStringA("EngineCore: Low Mode ON\n");
        } else {
            EngineCore::Security::SecurityManager::RequestPrivilege(L"SeSystemProfilePrivilege");

            if (prevDpcOn) {
                if (const std::error_code ec = dpcEngine->Start(); !ec) dpcEngine->RegisterCurrentThread();
                dpcSteeringOn = true;
            }
            if (prevEtwOn) {
                awaken = orchestrator.AwakenSystem(targetPid);
                etwDegraded = (awaken == S_FALSE);
                etwSessionOn = SUCCEEDED(awaken);
            }
            if (prevPmOn) {
                presentMonHooks.Start(pmCfg);
                presentMonOn = true;
            }
            OutputDebugStringA("EngineCore: Low Mode OFF\n");
        }
    };

    while (g_running.load(std::memory_order_relaxed)) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running.store(false, std::memory_order_relaxed); break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running.load(std::memory_order_relaxed)) break;

        // Minimized: nothing to draw. Sleep until input/messages instead of spinning the
        // GPU (Present on a minimized flip-model window is wasted work).
        if (IsIconic(hwnd)) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            QueryPerformanceCounter(&prevTime);
            continue;
        }

        // Swap-chain resize (previously the window could be resized but the back
        // buffers never followed, leaving a cropped / stretched dashboard).
        if (g_resizePending.exchange(false, std::memory_order_acq_rel)) {
            const UINT w = g_resizeW.load(std::memory_order_relaxed);
            const UINT h = g_resizeH.load(std::memory_order_relaxed);
            if (w > 0 && h > 0 && (w != backW || h != backH)) {
                for (UINT i = 0; i < kFrameCount; ++i) WaitForFrame(i);
                for (auto& bb : backBuffers) bb.Reset();
                FatalIfFailed(flip.swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, flip.swapChainFlags),
                              "IDXGISwapChain::ResizeBuffers failed");
                CreateRtvs();
                backW = w;
                backH = h;
            }
        }

        const bool foreground = (GetForegroundWindow() == hwnd);

        const ULONGLONG nowMs = GetTickCount64();
        if (!themeManuallySelected && nowMs >= nextThemeCheck) {
            nextThemeCheck = nowMs + 1000;
            bool detectedDarkTheme = darkTheme;
            if (ReadWindowsDarkMode(detectedDarkTheme) && detectedDarkTheme != darkTheme) {
                darkTheme = detectedDarkTheme;
                ApplyEngineCoreTheme(darkTheme);
            }
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double cpuFrameTimeMs = QpcToMs(now.QuadPart - prevTime.QuadPart, qpcFreq.QuadPart);
        prevTime = now;
        if (foreground) animTime += static_cast<float>(std::min(cpuFrameTimeMs, 100.0) / 1000.0);

        const float orbitT = static_cast<float>(ImGui::GetTime()) * 0.4f;
        const enginecore::Float3 demoPlayerPos{std::cos(orbitT) * 40.0f, 0.0f, std::sin(orbitT) * 40.0f};

        enginecore::FrameInput frameIn{};
        frameIn.frameIndex = appFrameIndex;
        frameIn.timestampQpc = static_cast<std::uint64_t>(now.QuadPart);
        frameIn.cpuFrameProgress = 0.0f;
        frameIn.targetFrameTimeMs = 16.667f;
        frameIn.cpuFrameTimeMs = static_cast<float>(cpuFrameTimeMs);
        frameIn.playerPosition = demoPlayerPos;
        orchestrator.Tick(frameIn);

        frameIn.cpuFrameProgress = orchConfig.earlyWarningProgress;
        orchestrator.Tick(frameIn);

        const enginecore::TelemetrySnapshot snap = orchestrator.GetTelemetrySnapshot();
        // Worst DPC: the orchestrator's adapter value, or the engine's own lock-free readout.
        const double dpcNowUs = std::max(snap.dpc.worstDpcLatencyUs, dpcEngine->WorstDpcMicros());

        plotAccumMs += cpuFrameTimeMs;
        plotWorkMax = std::max(plotWorkMax, static_cast<float>(lastWorkMs));
        plotDpcMax = std::max(plotDpcMax, static_cast<float>(dpcNowUs));
        if (plotAccumMs >= kPlotSampleMs) {
            plotAccumMs = 0.0;
            vramPlot.Push(snap.vram.pressure * 100.0f);
            workPlot.Push(plotWorkMax);
            dpcPlot.Push(plotDpcMax);
            plotWorkMax = 0.0f;
            plotDpcMax = 0.0f;
        }

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("EngineCoreDashboard", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);

        const float u = ImGui::GetFontSize();
        const ImGuiStyle& st = ImGui::GetStyle();
        const float contentW = ImGui::GetContentRegionAvail().x;

        DrawRgbFogHeaderBar(contentW, u * 3.4f, animTime, "EngineCore", "Telemetry Orchestrator");
        ImGui::Spacing();

        // --- Environment line (left) + theme switch (right) ----------------------
        if (ImGui::BeginTable("##info", 2, ImGuiTableFlags_SizingStretchProp)) {
            const char* themeLbl = darkTheme ? "Light theme" : "Dark theme";
            const float themeBtnW = ImGui::CalcTextSize(themeLbl).x + st.FramePadding.x * 2.0f;
            ImGui::TableSetupColumn("##a", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("##b", ImGuiTableColumnFlags_WidthFixed, themeBtnW);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextWrapped("%s  |  %s  |  %u logical cores  |  %s", gpuName.c_str(), profileLabel,
                               static_cast<unsigned>(hwProfile.logicalCpuCores),
                               isAdmin ? "Administrator" : "Standard user");
            ImGui::TableNextColumn();
            if (ImGui::Button(themeLbl, ImVec2(themeBtnW, 0.0f))) {
                darkTheme = !darkTheme;
                themeManuallySelected = true;
                ApplyEngineCoreTheme(darkTheme);
            }
            ImGui::EndTable();
        }

        // --- Master power switch + Low Mode --------------------------------------
        if (ImGui::BeginTable("##power", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##m", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("##l", ImGuiTableColumnFlags_WidthStretch, 1.4f);

            ImGui::TableNextColumn();
            {
                char powerLabel[48];
                FormatBuf(powerLabel, systemOn ? "SYSTEM ONLINE - click to power off" : "SYSTEM OFFLINE - click to power on");
                ImGui::BeginDisabled(lowModeOn);
                const bool powerClicked =
                    PowerToggleButton(powerLabel, systemOn, ImVec2(ImGui::GetContentRegionAvail().x, u * 3.6f));
                ImGui::EndDisabled();
                if (powerClicked) {
                    systemOn = !systemOn;
                    if (systemOn) {
                        if (const std::error_code ec = dpcEngine->Start(); !ec) dpcEngine->RegisterCurrentThread();
                        dpcSteeringOn = true;

                        psoService->SetEnabled(true);
                        psoAgentOn = true;

                        awaken = orchestrator.AwakenSystem(targetPid);
                        etwDegraded = (awaken == S_FALSE);
                        if (FAILED(awaken)) OutputDebugStringA("EngineOrchestrator::AwakenSystem failed on system power-on.\n");
                        etwSessionOn = true;

                        const auto pmResult = presentMonHooks.Start(pmCfg);
                        if (pmResult == EngineCore::Telemetry::StartResult::DegradedNoPrivilege) {
                            OutputDebugStringA("EngineCore: PresentMonHooks degraded on system power-on - no ETW privilege.\n");
                        }
                        presentMonOn = true;
                    } else {
                        dpcEngine->Stop();
                        dpcSteeringOn = false;

                        psoService->SetEnabled(false);
                        psoAgentOn = false;

                        orchestrator.Shutdown();
                        etwSessionOn = false;

                        presentMonHooks.Stop();
                        presentMonOn = false;
                    }
                }
            }

            ImGui::TableNextColumn();
            {
                char lowLabel[64];
                FormatBuf(lowLabel, "Low Mode  [%s]\nStops ETW, DPC, PresentMon", lowModeOn ? "ON" : "OFF");
                if (LiquidGlassButton(lowLabel, ImVec2(ImGui::GetContentRegionAvail().x, u * 3.6f), lowModeOn))
                    SetLowMode(!lowModeOn);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Disables the SeSystemProfilePrivilege, PresentMon hooks, ETW kernel telemetry\n"
                                      "and DPC steering. Turning it off restores whatever was running before.");
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();

        // --- Subsystem health chips (wrap on narrow windows) ---------------------
        {
            const ImVec4 green(0.15f, 0.58f, 0.27f, 1.0f);
            const ImVec4 gray(0.55f, 0.55f, 0.58f, 1.0f);
            const ImVec4 amber(0.82f, 0.55f, 0.10f, 1.0f);
            const ImVec4 red(0.75f, 0.16f, 0.16f, 1.0f);

            const bool dpcTrace = dpcEngine->IsTraceActive();
            const float pressureRatio =
                orchConfig.highVramPressure > 0.0f ? snap.vram.pressure / orchConfig.highVramPressure : 0.0f;
            ImVec4 vramColor = green;
            if (pressureRatio >= 1.0f) vramColor = red;
            else if (pressureRatio >= 0.75f) vramColor = amber;
            char vramChip[32];
            FormatBuf(vramChip, "VRAM %.0f%%", snap.vram.pressure * 100.0f);

            struct Chip { const char* label; ImVec4 color; };
            const Chip chips[] = {
                {dpcSteeringOn && !dpcTrace ? "DPC Steering (no trace - needs admin)" : "DPC Steering",
                 dpcSteeringOn ? (dpcTrace ? green : amber) : gray},
                {"PSO Agent", psoAgentOn ? green : gray},
                {"ETW Telemetry", etwSessionOn ? (snap.etwActive ? green : amber) : gray},
                {"PresentMon", presentMonOn ? green : gray},
                {vramChip, vramColor},
            };
            for (std::size_t i = 0; i < std::size(chips); ++i) {
                if (i > 0) SameLineWrap(StatusDotWidth(chips[i].label), 26.0f * g_dpiScale);
                StatusDot(chips[i].label, chips[i].color);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Controls: responsive grid of toggle/action buttons ------------------
        {
            const int ctlCols = std::clamp(static_cast<int>(contentW / (u * 14.0f)), 1, 5);
            const float btnH = u * 3.6f;
            if (ImGui::BeginTable("##controls", ctlCols, ImGuiTableFlags_SizingStretchSame)) {
                // 1) DPC steering (locked while Low Mode owns it)
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(lowModeOn);
                {
                    char label[64];
                    FormatBuf(label, "Toggle DPC Affinity Steering\n[%s]", dpcSteeringOn ? "ON" : "OFF");
                    if (LiquidGlassButton(label, ImVec2(ImGui::GetContentRegionAvail().x, btnH), dpcSteeringOn)) {
                        dpcSteeringOn = !dpcSteeringOn;
                        if (dpcSteeringOn) {
                            if (const std::error_code ec = dpcEngine->Start(); !ec) dpcEngine->RegisterCurrentThread();
                        } else {
                            dpcEngine->Stop();
                        }
                    }
                }
                ImGui::EndDisabled();

                // 2) Flush inactive VRAM
                ImGui::TableNextColumn();
                {
                    char label[96];
                    const std::uint64_t freeableBytes = vramPool.EstimateFreeableBytes(appFrameIndex, kDemoStaleFrames);
                    FormatBuf(label, "Flush Inactive VRAM\n%.1f MB freeable", static_cast<double>(freeableBytes) / (1024.0 * 1024.0));
                    if (LiquidGlassButton(label, ImVec2(ImGui::GetContentRegionAvail().x, btnH))) {
                        vramPool.FlushInactive(appFrameIndex, kDemoStaleFrames);
                    }
                }

                // 3) Force early pipeline hint
                ImGui::TableNextColumn();
                {
                    char label[64];
                    FormatBuf(label, "Force Early Pipeline Hint\n(%.0f%% Trigger)", orchConfig.earlyWarningProgress * 100.0f);
                    if (LiquidGlassButton(label, ImVec2(ImGui::GetContentRegionAvail().x, btnH))) {
                        enginecore::NextFrameHint hint{};
                        hint.targetFrameIndex = appFrameIndex + 1;
                        hint.timestampQpc = static_cast<std::uint64_t>(now.QuadPart);
                        hint.cpuProgressAtTrigger = orchConfig.earlyWarningProgress;
                        hint.expectedFrameTimeMs = frameIn.targetFrameTimeMs;
                        hint.vramPressure = snap.vram.pressure;
                        hint.preferReducedResidency = snap.vram.pressure >= orchConfig.highVramPressure;
                        deps.gpuHintSink->PrepareNextFramePipeline(hint);
                    }
                }

                // 4) PSO compiling agent
                ImGui::TableNextColumn();
                {
                    char label[64];
                    FormatBuf(label, "Toggle PSO Compiling Agent\n[%s]", psoAgentOn ? "ON" : "OFF");
                    if (LiquidGlassButton(label, ImVec2(ImGui::GetContentRegionAvail().x, btnH), psoAgentOn)) {
                        psoAgentOn = !psoAgentOn;
                        psoService->SetEnabled(psoAgentOn);
                    }
                }

                // 5) ETW telemetry session (locked while Low Mode owns it)
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(lowModeOn);
                {
                    char label[64];
                    FormatBuf(label, "Toggle ETW Telemetry Session\n[%s]", etwSessionOn ? "ON" : "OFF");
                    if (LiquidGlassButton(label, ImVec2(ImGui::GetContentRegionAvail().x, btnH), etwSessionOn)) {
                        etwSessionOn = !etwSessionOn;
                        if (etwSessionOn) {
                            awaken = orchestrator.AwakenSystem(targetPid);
                            etwDegraded = (awaken == S_FALSE);
                            if (FAILED(awaken)) OutputDebugStringA("EngineOrchestrator::AwakenSystem failed on manual re-awaken.\n");
                        } else {
                            orchestrator.Shutdown();
                        }
                    }
                }
                ImGui::EndDisabled();

                ImGui::EndTable();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Plots: three columns when wide, stacked when narrow -----------------
        {
            const int plotCols = contentW >= u * 48.0f ? 3 : 1;
            const float plotH = u * 8.0f;
            if (ImGui::BeginTable("##plots", plotCols, ImGuiTableFlags_SizingStretchSame)) {
                char overlay[32];
                char footer[96];

                ImGui::TableNextColumn();
                FormatBuf(overlay, "%.1f %%", snap.vram.pressure * 100.0f);
                FormatBuf(footer, "used %.0f / %.0f MB  |  peak %.1f %%",
                          static_cast<double>(snap.vram.usageBytes) / (1024.0 * 1024.0),
                          static_cast<double>(snap.vram.budgetBytes) / (1024.0 * 1024.0), vramPlot.Max());
                PlotCard("VRAM usage %", "##vram", vramPlot, 0.0f, 100.0f, overlay, footer, plotH);

                ImGui::TableNextColumn();
                FormatBuf(overlay, "%.2f ms", lastWorkMs);
                FormatBuf(footer, "interval %.1f ms (%.0f FPS, vsync-paced)", cpuFrameTimeMs,
                          cpuFrameTimeMs > 0.0 ? 1000.0 / cpuFrameTimeMs : 0.0);
                PlotCard("UI frame work (ms)", "##work", workPlot, 0.0f, std::max(4.0f, workPlot.Max() * 1.25f),
                         overlay, footer, plotH);

                ImGui::TableNextColumn();
                FormatBuf(overlay, "%.1f us", dpcNowUs);
                FormatBuf(footer, "%s", dpcEngine->IsTraceActive() ? "trace active  |  peak per 100 ms"
                                                                    : "trace inactive - run as Administrator");
                PlotCard("Worst DPC latency (us)", "##dpc", dpcPlot, 0.0f, std::max(100.0f, dpcPlot.Max() * 1.25f),
                         overlay, footer, plotH);

                ImGui::EndTable();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- Counters grid --------------------------------------------------------
        {
            const int statCols = std::clamp(static_cast<int>(contentW / (u * 11.0f)), 2, 6);
            if (ImGui::BeginTable("##stats", statCols, ImGuiTableFlags_SizingStretchSame)) {
                auto Tile = [](const char* label, const char* value) {
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", label);
                    ImGui::TextUnformatted(value);
                    ImGui::Spacing();
                };
                char b[64];

                FormatBuf(b, "%s%s", snap.etwActive ? "active" : "inactive", etwDegraded ? " (degraded)" : "");
                Tile("ETW session", b);
                FormatBuf(b, "%lu", static_cast<unsigned long>(snap.targetProcessId));
                Tile("Target PID", b);
                Tile("Target process", snap.targetProcessAlive ? "alive" : "gone");
                FormatBuf(b, "%llu", static_cast<unsigned long long>(appFrameIndex));
                Tile("App frame", b);
                FormatBuf(b, "%llu", static_cast<unsigned long long>(snap.earlyWarningsSent));
                Tile("Early warnings sent", b);
                FormatBuf(b, "%llu", static_cast<unsigned long long>(snap.psoRequestsEnqueued));
                Tile("PSOs enqueued", b);
                FormatBuf(b, "%llu", static_cast<unsigned long long>(snap.directionChangePurges));
                Tile("Direction purges", b);
                FormatBuf(b, "%llu", static_cast<unsigned long long>(snap.generatedFrames));
                Tile("Generated frames", b);
                FormatBuf(b, "%llu", static_cast<unsigned long long>(snap.agentFaults));
                Tile("Agent faults", b);
                FormatBuf(b, "%.1f%s", presentMonHooks.SafetyValveEstimator().EstimatedFps(),
                          presentMonHooks.SafetyValveEstimator().IsBelowSafetyFloor() ? " (below floor)" : "");
                Tile("Present FPS", b);

                ImGui::EndTable();
            }
        }

        ImGui::End();
        ImGui::Render();

        const UINT backIdx = swapChain3->GetCurrentBackBufferIndex();
        WaitForFrame(backIdx);

        cmdAllocs[backIdx]->Reset();
        cmdList->Reset(cmdAllocs[backIdx].Get(), nullptr);

        D3D12_RESOURCE_BARRIER toRT{};
        toRT.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRT.Transition.pResource = backBuffers[backIdx].Get();
        toRT.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toRT.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toRT.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &toRT);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(backIdx) * rtvStride;
        const float clearColor[4] = {
            darkTheme ? 0.0f : 1.0f,
            darkTheme ? 0.0f : 1.0f,
            darkTheme ? 0.0f : 1.0f,
            1.0f
        };
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

        ID3D12DescriptorHeap* heaps[] = {srvHeap.Get()};
        cmdList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmdList.Get());

        D3D12_RESOURCE_BARRIER toPresent = toRT;
        toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmdList->ResourceBarrier(1, &toPresent);

        cmdList->Close();
        ID3D12CommandList* lists[] = {cmdList.Get()};
        graphicsQueue->ExecuteCommandLists(1, lists);

        LARGE_INTEGER submitQpc{};
        QueryPerformanceCounter(&submitQpc);
        lastWorkMs = QpcToMs(submitQpc.QuadPart - now.QuadPart, qpcFreq.QuadPart);

        flip.swapChain->Present(1, flip.PresentFlags(1));

        frameFenceValues[backIdx] = nextFenceValue;
        graphicsQueue->Signal(frameFence.Get(), nextFenceValue);
        ++nextFenceValue;

        (void)flip.WaitForNextFrame();

        // Not the foreground window (e.g. the game has focus): drop to ~15 FPS so the
        // dashboard stops competing with the game for CPU/GPU. Input still wakes us.
        if (!foreground) MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
        ++appFrameIndex;
    }

    for (UINT i = 0; i < kFrameCount; ++i) WaitForFrame(i);
    CloseHandle(frameFenceEvent);

    orchestrator.Shutdown();
    presentMonHooks.Stop();
    vramMonitor->Shutdown();
    dpcEngine->Stop();

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}