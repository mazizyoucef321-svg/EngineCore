// =============================================================================
//  EngineCore :: PresentMon-style ETW Present Hooks implementation
//  src/telemetry/PresentMon_hooks.cpp
// =============================================================================
#include "telemetry/PresentMon_hooks.hpp"

namespace EngineCore::Telemetry {

PresentMonHooks::PresentMonHooks() = default;
PresentMonHooks::~PresentMonHooks() { Stop(); }

StartResult PresentMonHooks::Start(const PresentMonConfig& config) {
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
        // not a bug. See the header's "PRIVILEGE FALLBACK" note.
        if (rc == ERROR_ACCESS_DENIED) { privilegeDenied_ = true; return StartResult::DegradedNoPrivilege; }
        return StartResult::DegradedOtherError;
    }

    if (!EnableProvider(kDxgiProviderGuid)) { StopSessionOnly(); return StartResult::DegradedOtherError; }
    if (config_.enableDxgKrnlExperimental) {
        // Best-effort: DxgKrnl is optional, so its failure doesn't abort the
        // whole hook - DXGI alone is already fully functional on its own.
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

void PresentMonHooks::Stop() noexcept {
    active_.store(false, std::memory_order_release);
    StopSessionOnly(); // makes the blocking ProcessTrace() call above return
    if (pump_.joinable()) pump_.join();
    if (consumer_ != 0) { ::CloseTrace(consumer_); consumer_ = 0; }
}

Stats PresentMonHooks::GetStats() const noexcept {
    Stats s{};
    s.dxgiEventsSeen      = dxgiEventsSeen_.load(std::memory_order_relaxed);
    s.dxgKrnlEventsSeen   = dxgKrnlEventsSeen_.load(std::memory_order_relaxed);
    s.presentStartsSeen   = presentStartsSeen_.load(std::memory_order_relaxed);
    s.presentStopsSeen    = presentStopsSeen_.load(std::memory_order_relaxed);
    s.hintSinkInvocations = hintSinkInvocations_.load(std::memory_order_relaxed);
    s.privilegeDenied     = privilegeDenied_;
    s.agentFaults         = agentFaults_.load(std::memory_order_relaxed);
    return s;
}

EVENT_TRACE_PROPERTIES* PresentMonHooks::TraceProps() {
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

bool PresentMonHooks::EnableProvider(const GUID& providerGuid) {
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

void PresentMonHooks::StopSessionOnly() noexcept {
    if (session_ == 0) return;
    ::ControlTraceW(session_, nullptr, TraceProps(), EVENT_TRACE_CONTROL_STOP);
    session_ = 0;
}

void WINAPI PresentMonHooks::OnEventRecordThunk(PEVENT_RECORD record) noexcept {
    if (auto* self = static_cast<PresentMonHooks*>(record->UserContext)) self->HandleEvent(*record);
}

// The hot path. No heap allocation, no locks beyond the SeqLock's atomic
// word stores, no syscalls other than the ETW plumbing that got us here.
void PresentMonHooks::HandleEvent(const EVENT_RECORD& record) noexcept {
    const bool isDxgi = IsEqualGUID(record.EventHeader.ProviderId, kDxgiProviderGuid);
    const bool isDxgKrnl = !isDxgi && IsEqualGUID(record.EventHeader.ProviderId, kDxgKrnlProviderGuid);
    if (!isDxgi && !isDxgKrnl) return;

    if (isDxgi) dxgiEventsSeen_.fetch_add(1, std::memory_order_relaxed);
    else        dxgKrnlEventsSeen_.fetch_add(1, std::memory_order_relaxed);

    if (static_cast<LONG>(record.EventHeader.ProcessId) != static_cast<LONG>(config_.targetProcessId)) return;

    const std::int64_t ts = record.EventHeader.TimeStamp.QuadPart;

    if (isDxgKrnl) {
        // Timestamp-only refinement per the header's DxgKrnl note - no
        // payload field parsing. Does not drive present pairing.
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

            // FPS is the START-to-START cadence. (Previously the Start->Stop
            // duration of the Present() call itself was fed to the estimator,
            // which made it report ~3000 FPS for a 60 Hz app.)
            if (prevStart != 0 && ts > prevStart) estimator_.OnSample(QpcDeltaToMs(prevStart, ts));
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

            if (config_.hintSink) {
                config_.hintSink->OnPresentTelemetry(data);
                hintSinkInvocations_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } catch (...) {
        // Genuinely unexpected only (e.g. a hint sink that violated its
        // noexcept contract via an override that still throws under
        // optimization edge cases). Never reached by privilege denial - see
        // the header's "PRIVILEGE FALLBACK" note.
        agentFaults_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace EngineCore::Telemetry