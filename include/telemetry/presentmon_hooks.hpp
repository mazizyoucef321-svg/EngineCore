// =============================================================================
//  EngineCore :: PresentMon-style ETW Present Hooks
//  include/telemetry/PresentMon_hooks.hpp
//
//  Header-only. Hooks real-time ETW present telemetry and feeds it, with a
//  lock-free ring buffer, into an EarlyPipelineHintAgent-shaped sink and a
//  frame-delivery safety-valve estimator.
//
//  WHAT THIS ACTUALLY HOOKS, AND WHY
//  ----------------------------------
//  "PresentMon" (the open-source frame-timing tool popularized by Intel)
//  does not expose its own ETW provider - it consumes the same standard
//  Microsoft providers this file does:
//    - Microsoft-Windows-DXGI        {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}
//      User-mode Present_Start (id 42) / Present_Stop (id 43). This is the
//      primary, always-reliable source: real QPC-domain timestamps for the
//      actual DXGI Present() call on this process's swap chain(s).
//    - Microsoft-Windows-DxgKrnl     {802EC45A-1E99-4B83-9920-87C98277BA9D}
//      Optional, OPT IN via PresentMonConfig::enableDxgKrnlExperimental.
//      This is the real kernel-mode GPU scheduler provider and is where true
//      hardware flip/VSync timestamps live - but its event IDs and payload
//      field layout differ across Windows/WDDM versions in ways this header
//      does not claim confident knowledge of. Rather than guess at field
//      names, DxgKrnl events are consumed for their EVENT_HEADER.TimeStamp
//      only (a QPC tick every ETW event carries regardless of schema) to
//      refine timing resolution, never for payload parsing. Verify your
//      system's actual DxgKrnl event layout with GPUView or `tracefmt` if
//      you want to extend this.
//    - Event IDs above: verify against your Windows build with
//      `logman query providers Microsoft-Windows-DXGI`, same as the
//      orchestrator's own ETW session (engine_orchestrator.cpp) - these
//      have been stable across recent Windows releases but are not a
//      contractual guarantee from Microsoft.
//
//  PRESENT-PAIR CORRELATION
//  --------------------------
//  Present_Start/Stop are paired as "most recent unmatched Start, for each
//  Stop" - correct for the common single-swap-chain-per-process case (which
//  matches EngineCore's own render loop in main.cpp). A process juggling
//  several concurrent swap chains would need to correlate by the event's
//  SwapChainAddress field instead; that requires TDH payload parsing this
//  header deliberately does not attempt (see the DxgKrnl note above for why).
//
//  LATENCY OF THE HOT PATH
//  --------------------------
//  "Sub-microsecond dispatch" is a design goal, not a hardware guarantee -
//  no software can promise a wall-clock number that depends on OS thread
//  scheduling. What this header *can* and does guarantee: the ETW callback
//  path performs zero heap allocations, takes no locks, and makes no
//  syscalls other than the unavoidable ETW plumbing itself - every
//  controllable source of added latency is removed. IEarlyPipelineHintSink
//  implementations must uphold the same contract (see its declaration).
//
//  RELATIONSHIP TO EngineOrchestrator'S EXISTING EARLY-WARNING AGENT
//  ---------------------------------------------------------------------
//  engine_orchestrator.cpp's EarlySpeculativePipelineWarningAgent triggers
//  on CPU FRAME PROGRESS (e.g. "10%/40% of this frame's CPU work is done").
//  This header triggers on REAL PRESENT CADENCE (e.g. "a present just
//  started on the GPU"). These are two different, complementary signals,
//  not two implementations of the same thing - wire this header's sink
//  callback as an independent, additional trigger path, not a replacement.
//
//  PRIVILEGE FALLBACK
//  --------------------
//  A real-time ETW session normally needs Administrator or membership in
//  "Performance Log Users". If Start() can't get that, it does NOT count
//  this as an agent fault (it's an expected, handled condition, not a bug):
//  Stats::privilegeDenied is set, Stats::agentFaults stays at 0, and the
//  hook simply stays inactive so the caller can fall back to its own
//  CPU-timer-based estimates.
// =============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#pragma comment(lib, "advapi32.lib") // StartTraceW/EnableTraceEx2/OpenTraceW/ProcessTrace/ControlTraceW

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace EngineCore::Telemetry {

// =============================================================================
//  Time helpers (QPC)
// =============================================================================

/// QPC frequency, queried once and cached. Thread-safe (Meyers singleton,
/// C++11+ guarantees the static init is race-free) and allocation-free after
/// the first call.
[[nodiscard]] inline std::int64_t QpcFrequency() noexcept {
    static const std::int64_t freq = [] {
        LARGE_INTEGER v{};
        ::QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return freq;
}

[[nodiscard]] inline double QpcDeltaToUs(std::int64_t startTicks, std::int64_t stopTicks) noexcept {
    return static_cast<double>(stopTicks - startTicks) * 1'000'000.0 / static_cast<double>(QpcFrequency());
}

[[nodiscard]] inline double QpcDeltaToMs(std::int64_t startTicks, std::int64_t stopTicks) noexcept {
    return static_cast<double>(stopTicks - startTicks) * 1'000.0 / static_cast<double>(QpcFrequency());
}

// =============================================================================
//  PresentTelemetryData: the lock-free telemetry bridge
//  Trivially copyable, fixed-size, no padding surprises - required for the
//  word-wise SeqLock below to be correct. Every field is a plain fixed-width
//  type on purpose; no std::string/std::vector may ever be added here.
// =============================================================================
enum class PresentEventKind : std::uint8_t { PresentStart = 0, PresentStop = 1 };
enum class PresentSource : std::uint8_t { Dxgi = 0, DxgKrnl = 1 };

struct PresentTelemetryData {
    std::uint64_t frameSequence{0};      ///< Monotonic count of PresentStop events observed.
    std::int64_t  presentStartQpc{0};    ///< QPC tick of the correlated Present_Start.
    std::int64_t  presentStopQpc{0};     ///< QPC tick of this Present_Stop. 0 for a Start-only record.
    float         frameLatencyUs{0.0f};  ///< presentStopQpc - presentStartQpc, in microseconds.
    float         presentIntervalUs{0.0f}; ///< This Start - previous Start, in microseconds.
    std::int32_t  queueDepth{0};         ///< Present_Start events seen with no Stop yet, at this moment.
    PresentEventKind kind{PresentEventKind::PresentStart};
    PresentSource     source{PresentSource::Dxgi};
    std::uint8_t      _padding[2]{};     ///< Explicit padding: keeps the struct's size a multiple of 8.
};
static_assert(std::is_trivially_copyable_v<PresentTelemetryData>,
             "PresentTelemetryData must stay trivially copyable for the SeqLock/ring buffer below");
static_assert(sizeof(PresentTelemetryData) % sizeof(std::uint64_t) == 0,
             "PresentTelemetryData size must be a multiple of 8 bytes for the word-wise SeqLock");

// =============================================================================
//  SeqLock<T>: single-writer / multi-reader publication of a small,
//  trivially-copyable T using only atomic<uint64_t> words - no hardware
//  DWCAS assumption, no torn reads, no locks. Same technique already proven
//  in engine_orchestrator.cpp's FrameInput publication.
// =============================================================================
template <class T>
class SeqLock {
    static_assert(std::is_trivially_copyable_v<T>, "SeqLock payload must be trivially copyable");
    static_assert(sizeof(T) % sizeof(std::uint64_t) == 0, "SeqLock payload size must be a multiple of 8");
    static constexpr std::size_t kWords = sizeof(T) / sizeof(std::uint64_t);

public:
    // Single-writer only. Calling Store() from more than one thread
    // concurrently is undefined - this class provides SPMC semantics.
    void Store(const T& value) noexcept {
        std::array<std::uint64_t, kWords> words{};
        std::memcpy(words.data(), &value, sizeof(T));

        const std::uint64_t seq = seq_.load(std::memory_order_relaxed);
        seq_.store(seq + 1, std::memory_order_relaxed); // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < kWords; ++i) words_[i].store(words[i], std::memory_order_relaxed);
        seq_.store(seq + 2, std::memory_order_release); // even: stable again
    }

    // Any number of concurrent readers; never blocks on the writer for
    // longer than the handful of word-stores above.
    void Load(T& out) const noexcept {
        std::array<std::uint64_t, kWords> words{};
        for (;;) {
            const std::uint64_t s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) { ::YieldProcessor(); continue; }
            for (std::size_t i = 0; i < kWords; ++i) words[i] = words_[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s1) break;
            ::YieldProcessor();
        }
        std::memcpy(&out, words.data(), sizeof(T));
    }

private:
    std::atomic<std::uint64_t> seq_{0};
    std::array<std::atomic<std::uint64_t>, kWords> words_{};
};

// =============================================================================
//  AtomicRingBuffer<T, N>: N SeqLock slots, no heap allocation (N is a
//  compile-time capacity), single-producer / multi-consumer.
// =============================================================================
template <class T, std::size_t N>
class AtomicRingBuffer {
    static_assert(N >= 2, "Ring buffer needs at least 2 slots");

public:
    /// Single-writer only (the ETW pump thread in PresentMonHooks below).
    void Push(const T& value) noexcept {
        const std::uint64_t idx = writeIndex_.load(std::memory_order_relaxed);
        slots_[idx % N].Store(value);
        writeIndex_.store(idx + 1, std::memory_order_release);
    }

    /// Most recently pushed sample. Returns false if nothing has been
    /// pushed yet (leaves `out` unmodified).
    [[nodiscard]] bool TryGetLatest(T& out) const noexcept {
        const std::uint64_t idx = writeIndex_.load(std::memory_order_acquire);
        if (idx == 0) return false;
        slots_[(idx - 1) % N].Load(out);
        return true;
    }

    /// Copies up to maxCount most recent samples into `out`, oldest first.
    /// Returns the number actually written (<= min(maxCount, N, TotalPushed())).
    std::size_t CopyRecent(T* out, std::size_t maxCount) const noexcept {
        const std::uint64_t total = writeIndex_.load(std::memory_order_acquire);
        const std::size_t available = static_cast<std::size_t>(total < N ? total : N);
        const std::size_t count = maxCount < available ? maxCount : available;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint64_t idx = total - count + i;
            slots_[idx % N].Load(out[i]);
        }
        return count;
    }

    [[nodiscard]] std::uint64_t TotalPushed() const noexcept { return writeIndex_.load(std::memory_order_relaxed); }

private:
    std::array<SeqLock<T>, N> slots_{};
    std::atomic<std::uint64_t> writeIndex_{0};
};

// =============================================================================
//  IEarlyPipelineHintSink
//  Your EarlyPipelineHintAgent (or engine_orchestrator's
//  EarlySpeculativePipelineWarningAgent, or any other agent) implements this
//  to receive present telemetry directly from the ETW pump thread.
// =============================================================================
class IEarlyPipelineHintSink {
public:
    virtual ~IEarlyPipelineHintSink() = default;

    /// Called synchronously from the ETW event-pump thread for every
    /// Present_Start/Stop this hook observes. MUST be noexcept (enforced by
    /// the signature) and MUST NOT allocate, lock, or block - the hot-path
    /// latency guarantee described at the top of this file depends on every
    /// sink upholding the same contract the hook itself does.
    virtual void OnPresentTelemetry(const PresentTelemetryData& data) noexcept = 0;
};

// =============================================================================
//  FrameDeliveryEstimator: EMA-smoothed FPS/frame-time estimate feeding the
//  SafetyValve's floor-protection check (~40-60 FPS per the spec; default 45).
// =============================================================================
class FrameDeliveryEstimator {
public:
    /// alpha in (0,1]: higher = more responsive, lower = smoother. 0.2 is a
    /// reasonable default (roughly a 5-sample effective window).
    explicit FrameDeliveryEstimator(double alpha = 0.2, double safetyFloorFps = 45.0) noexcept
        : alpha_(alpha), safetyFloorFps_(safetyFloorFps) {}

    /// Call once per completed frame (i.e. once per Present_Stop) with the
    /// measured present-interval in milliseconds.
    void OnSample(double intervalMs) noexcept {
        if (intervalMs <= 0.0) return; // guard against a bad/first sample
        double prev = emaIntervalMs_.load(std::memory_order_relaxed);
        const double next = (prev == 0.0) ? intervalMs : (prev + alpha_ * (intervalMs - prev));
        emaIntervalMs_.store(next, std::memory_order_relaxed);
    }

    [[nodiscard]] double EstimatedFrameTimeMs() const noexcept { return emaIntervalMs_.load(std::memory_order_relaxed); }
    [[nodiscard]] double EstimatedFps() const noexcept {
        const double ms = EstimatedFrameTimeMs();
        return ms > 0.0 ? 1000.0 / ms : 0.0;
    }
    /// True once there is enough data AND the estimate has dropped below the
    /// configured safety floor (e.g. feed this straight into your SafetyValve).
    [[nodiscard]] bool IsBelowSafetyFloor() const noexcept {
        const double fps = EstimatedFps();
        return fps > 0.0 && fps < safetyFloorFps_;
    }
    void SetSafetyFloorFps(double fps) noexcept { safetyFloorFps_ = fps; }
    [[nodiscard]] double SafetyFloorFps() const noexcept { return safetyFloorFps_; }

private:
    double alpha_;
    double safetyFloorFps_;
    std::atomic<double> emaIntervalMs_{0.0}; // Practically lock-free on x64/MSVC for an 8-byte-aligned double.
};

// =============================================================================
//  Configuration / results / stats
// =============================================================================
struct PresentMonConfig {
    DWORD targetProcessId{0};
    IEarlyPipelineHintSink* hintSink{nullptr}; ///< May be null: hook still runs, just no callback fires.
    bool  enableDxgKrnlExperimental{false};    ///< See the file-header DxgKrnl note before enabling.
    double safetyFloorFps{45.0};               ///< Midpoint of the requested ~40-60 FPS floor range.
};

enum class StartResult : std::uint8_t {
    Active,              ///< Real-time ETW session is up and consuming events.
    DegradedNoPrivilege, ///< Caller isn't Administrator / in "Performance Log Users"; NOT an agent fault.
    DegradedOtherError,  ///< Some other StartTraceW/EnableTraceEx2/OpenTraceW failure; see GetLastStartError().
    AlreadyActive,       ///< Start() called twice without an intervening Stop().
};

struct Stats {
    std::uint64_t dxgiEventsSeen{0};
    std::uint64_t dxgKrnlEventsSeen{0};
    std::uint64_t presentStartsSeen{0};
    std::uint64_t presentStopsSeen{0};
    std::uint64_t hintSinkInvocations{0};
    bool          privilegeDenied{false}; ///< See the file-header "PRIVILEGE FALLBACK" note.
    std::uint64_t agentFaults{0};         ///< Genuinely unexpected exceptions only; privilege denial is NOT one.
};

// =============================================================================
//  PresentMonHooks<RingCapacity>
//  RingCapacity is a compile-time template parameter specifically so the
//  ring buffer is a fixed-size array member, not a heap allocation - this is
//  what makes "zero heap allocations in the hot path" structurally true
//  rather than just a comment.
// =============================================================================
template <std::size_t RingCapacity = 256>
class PresentMonHooks {
public:
    PresentMonHooks() = default;
    ~PresentMonHooks() { Stop(); }

    PresentMonHooks(const PresentMonHooks&)            = delete;
    PresentMonHooks& operator=(const PresentMonHooks&) = delete;

    [[nodiscard]] StartResult Start(const PresentMonConfig& config) {
        if (active_.load(std::memory_order_acquire)) return StartResult::AlreadyActive;

        config_ = config;
        estimator_.SetSafetyFloorFps(config_.safetyFloorFps);
        sessionName_ = L"EngineCore_PresentMonHooks_" + std::to_wstring(config_.targetProcessId);

        ULONG rc = ::StartTraceW(&session_, sessionName_.c_str(), TraceProps());
        if (rc == ERROR_ALREADY_EXISTS) { // stale session from a crashed prior run
            ::ControlTraceW(0, sessionName_.c_str(), TraceProps(), EVENT_TRACE_CONTROL_STOP);
            session_ = 0;
            rc = ::StartTraceW(&session_, sessionName_.c_str(), TraceProps());
        }
        if (rc != ERROR_SUCCESS) {
            session_ = 0;
            lastStartError_ = rc;
            // ERROR_ACCESS_DENIED here means "not Administrator / not in
            // Performance Log Users" - the documented, expected failure mode,
            // not a bug. See the file-header "PRIVILEGE FALLBACK" note.
            if (rc == ERROR_ACCESS_DENIED) { stats_.privilegeDenied = true; return StartResult::DegradedNoPrivilege; }
            return StartResult::DegradedOtherError;
        }

        if (!EnableProvider(kDxgiProviderGuid)) { StopSessionOnly(); return StartResult::DegradedOtherError; }
        if (config_.enableDxgKrnlExperimental) {
            // Best-effort: DxgKrnl is optional, so its failure doesn't abort
            // the whole hook - DXGI alone is already fully functional.
            EnableProvider(kDxgKrnlProviderGuid);
        }

        EVENT_TRACE_LOGFILEW logFile{};
        logFile.LoggerName = const_cast<LPWSTR>(sessionName_.c_str());
        logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                                   PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        logFile.EventRecordCallback = &PresentMonHooks::OnEventRecordThunk;
        logFile.Context = this;

        consumer_ = ::OpenTraceW(&logFile);
        if (consumer_ == INVALID_PROCESSTRACE_HANDLE) {
            lastStartError_ = ::GetLastError();
            consumer_ = 0;
            StopSessionOnly();
            return StartResult::DegradedOtherError;
        }

        active_.store(true, std::memory_order_release);
        pump_ = std::jthread([this](std::stop_token) {
            TRACEHANDLE handle = consumer_;
            ::ProcessTrace(&handle, 1, nullptr, nullptr); // blocks until Stop() tears the session down
        });
        return StartResult::Active;
    }

    void Stop() noexcept {
        active_.store(false, std::memory_order_release);
        StopSessionOnly(); // makes the blocking ProcessTrace() call above return
        if (pump_.joinable()) pump_.join();
        if (consumer_ != 0) { ::CloseTrace(consumer_); consumer_ = 0; }
    }

    [[nodiscard]] bool IsActive() const noexcept { return active_.load(std::memory_order_acquire); }
    [[nodiscard]] ULONG GetLastStartError() const noexcept { return lastStartError_; }

    [[nodiscard]] const AtomicRingBuffer<PresentTelemetryData, RingCapacity>& TelemetryRingBuffer() const noexcept {
        return ring_;
    }
    [[nodiscard]] const FrameDeliveryEstimator& SafetyValveEstimator() const noexcept { return estimator_; }

    [[nodiscard]] Stats GetStats() const noexcept {
        Stats s{};
        s.dxgiEventsSeen       = dxgiEventsSeen_.load(std::memory_order_relaxed);
        s.dxgKrnlEventsSeen    = dxgKrnlEventsSeen_.load(std::memory_order_relaxed);
        s.presentStartsSeen    = presentStartsSeen_.load(std::memory_order_relaxed);
        s.presentStopsSeen     = presentStopsSeen_.load(std::memory_order_relaxed);
        s.hintSinkInvocations  = hintSinkInvocations_.load(std::memory_order_relaxed);
        s.privilegeDenied      = stats_.privilegeDenied;
        s.agentFaults          = agentFaults_.load(std::memory_order_relaxed);
        return s;
    }

private:
    static constexpr GUID kDxgiProviderGuid =
        {0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};
    static constexpr GUID kDxgKrnlProviderGuid =
        {0x802EC45A, 0x1E99, 0x4B83, {0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D}};
    static constexpr USHORT kEvtPresentStart = 42;
    static constexpr USHORT kEvtPresentStop  = 43;

    EVENT_TRACE_PROPERTIES* TraceProps() {
        const std::size_t bytes = sizeof(EVENT_TRACE_PROPERTIES) + (sessionName_.size() + 1) * sizeof(wchar_t);
        propsBuffer_.assign(bytes, std::byte{0});
        auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuffer_.data());
        p->Wnode.BufferSize    = static_cast<ULONG>(bytes);
        p->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
        p->Wnode.ClientContext = 1; // QPC timestamps
        p->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
        p->FlushTimer          = 1;
        p->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
        return p;
    }

    [[nodiscard]] bool EnableProvider(const GUID& providerGuid) {
        std::array<ULONG, 1> pids{config_.targetProcessId};
        EVENT_FILTER_DESCRIPTOR filter{};
        filter.Ptr  = reinterpret_cast<ULONGLONG>(pids.data());
        filter.Size = static_cast<ULONG>(sizeof(pids));
        filter.Type = EVENT_FILTER_TYPE_PID;

        ENABLE_TRACE_PARAMETERS params{};
        params.Version          = ENABLE_TRACE_PARAMETERS_VERSION_2;
        params.EnableFilterDesc = &filter;
        params.FilterDescCount  = 1;

        const ULONG rc = ::EnableTraceEx2(session_, &providerGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                          TRACE_LEVEL_INFORMATION, ~0ull, 0, 0, &params);
        if (rc != ERROR_SUCCESS) lastStartError_ = rc;
        return rc == ERROR_SUCCESS;
    }

    void StopSessionOnly() noexcept {
        if (session_ == 0) return;
        ::ControlTraceW(session_, nullptr, TraceProps(), EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
    }

    static void WINAPI OnEventRecordThunk(PEVENT_RECORD record) noexcept {
        if (auto* self = static_cast<PresentMonHooks*>(record->UserContext)) self->HandleEvent(*record);
    }

    // The hot path. No heap allocation, no locks beyond the SeqLock's atomic
    // word stores, no syscalls other than the ETW plumbing that got us here.
    void HandleEvent(const EVENT_RECORD& record) noexcept {
        const bool isDxgi = IsEqualGUID(record.EventHeader.ProviderId, kDxgiProviderGuid);
        const bool isDxgKrnl = !isDxgi && IsEqualGUID(record.EventHeader.ProviderId, kDxgKrnlProviderGuid);
        if (!isDxgi && !isDxgKrnl) return;

        if (isDxgi) dxgiEventsSeen_.fetch_add(1, std::memory_order_relaxed);
        else        dxgKrnlEventsSeen_.fetch_add(1, std::memory_order_relaxed);

        if (static_cast<LONG>(record.EventHeader.ProcessId) != static_cast<LONG>(config_.targetProcessId)) return;

        const std::int64_t ts = record.EventHeader.TimeStamp.QuadPart;

        if (isDxgKrnl) {
            // Timestamp-only refinement per the file-header DxgKrnl note -
            // no payload field parsing. Does not drive present pairing.
            return;
        }

        const USHORT id = record.EventHeader.EventDescriptor.Id;
        if (id != kEvtPresentStart && id != kEvtPresentStop) return;

        try {
            if (id == kEvtPresentStart) {
                queueDepth_.fetch_add(1, std::memory_order_relaxed);

                const std::int64_t prevStart = lastStartQpc_.exchange(ts, std::memory_order_relaxed);
                const std::uint64_t seq = presentStartsSeen_.fetch_add(1, std::memory_order_relaxed);

                PresentTelemetryData data{};
                data.frameSequence = seq;
                data.presentStartQpc = ts;
                data.presentStopQpc = 0;
                data.presentIntervalUs = (prevStart != 0 && ts > prevStart)
                    ? static_cast<float>(QpcDeltaToUs(prevStart, ts)) : 0.0f;
                data.queueDepth = queueDepth_.load(std::memory_order_relaxed);
                data.kind = PresentEventKind::PresentStart;
                data.source = PresentSource::Dxgi;
                ring_.Push(data);

                if (config_.hintSink) {
                    config_.hintSink->OnPresentTelemetry(data);
                    hintSinkInvocations_.fetch_add(1, std::memory_order_relaxed);
                }
            } else { // kEvtPresentStop
                std::int32_t depth = queueDepth_.load(std::memory_order_relaxed);
                if (depth > 0) queueDepth_.fetch_sub(1, std::memory_order_relaxed);

                const std::int64_t start = lastStartQpc_.load(std::memory_order_relaxed);
                const std::uint64_t seq = presentStopsSeen_.fetch_add(1, std::memory_order_relaxed);

                PresentTelemetryData data{};
                data.frameSequence = seq;
                data.presentStartQpc = start;
                data.presentStopQpc = ts;
                data.frameLatencyUs = (start != 0 && ts > start)
                    ? static_cast<float>(QpcDeltaToUs(start, ts)) : 0.0f;
                data.queueDepth = depth;
                data.kind = PresentEventKind::PresentStop;
                data.source = PresentSource::Dxgi;
                ring_.Push(data);

                if (start != 0 && ts > start) estimator_.OnSample(QpcDeltaToMs(start, ts));

                if (config_.hintSink) {
                    config_.hintSink->OnPresentTelemetry(data);
                    hintSinkInvocations_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } catch (...) {
            // Genuinely unexpected only (e.g. a hint sink that violated its
            // noexcept contract via an override that still throws under
            // optimization edge cases). Never reached by privilege denial -
            // see the file-header "PRIVILEGE FALLBACK" note.
            agentFaults_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    PresentMonConfig config_{};
    std::wstring sessionName_;
    std::vector<std::byte> propsBuffer_;
    TRACEHANDLE session_{0};
    TRACEHANDLE consumer_{0};
    std::jthread pump_;
    std::atomic<bool> active_{false};
    ULONG lastStartError_{ERROR_SUCCESS};

    AtomicRingBuffer<PresentTelemetryData, RingCapacity> ring_;
    FrameDeliveryEstimator estimator_{0.2, 45.0};

    std::atomic<std::int64_t> lastStartQpc_{0};
    std::atomic<std::int32_t> queueDepth_{0};

    std::atomic<std::uint64_t> dxgiEventsSeen_{0};
    std::atomic<std::uint64_t> dxgKrnlEventsSeen_{0};
    std::atomic<std::uint64_t> presentStartsSeen_{0};
    std::atomic<std::uint64_t> presentStopsSeen_{0};
    std::atomic<std::uint64_t> hintSinkInvocations_{0};
    std::atomic<std::uint64_t> agentFaults_{0};
    Stats stats_{}; // only ::privilegeDenied is read from here; the rest come from the atomics above
};

} // namespace EngineCore::Telemetry