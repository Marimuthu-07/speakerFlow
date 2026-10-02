#include "wasapi_fanout_engine.h"

namespace speakerflow {

static std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return "";
    int len = (int)wcslen(wstr);
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr, len, NULL, 0, NULL, NULL);
    if (sizeNeeded <= 0) return "";
    std::string str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, len, &str[0], sizeNeeded, NULL, NULL);
    return str;
}

WasapiFanOutEngine::WasapiFanOutEngine() {
    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        m_branches[i] = nullptr;
    }
    m_recoveryThread = std::thread(&WasapiFanOutEngine::RecoveryThreadProc, this);
}

WasapiFanOutEngine::~WasapiFanOutEngine() {
    Shutdown();
}

bool WasapiFanOutEngine::StartCapture(const std::wstring& targetDeviceId, std::string& outError) {
    std::lock_guard<std::mutex> lock(m_engineMutex);
    m_lastError.clear();

    // Stop any currently running capture and branches
    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i]) {
            m_captureClient.SetBranchBuffer(i, nullptr);
            m_branches[i]->active.store(false, std::memory_order_release);
            m_branches[i]->renderClient->StopRender();
            m_branches[i].reset();
        }
    }
    m_captureClient.StopCapture();

    m_captureDeviceId = targetDeviceId;
    bool ok = m_captureClient.StartCapture(targetDeviceId, outError);
    if (!ok) {
        m_lastError = outError;
    }
    return ok;
}

void WasapiFanOutEngine::StopCapture() {
    std::lock_guard<std::mutex> lock(m_engineMutex);

    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i]) {
            m_captureClient.SetBranchBuffer(i, nullptr);
            m_branches[i]->active.store(false, std::memory_order_release);
            m_branches[i]->renderClient->StopRender();
            m_branches[i].reset();
        }
    }

    m_captureClient.StopCapture();
}

bool WasapiFanOutEngine::IsCaptureActive() const {
    return m_captureClient.IsCapturing();
}

WasapiCaptureStats WasapiFanOutEngine::GetCaptureStats() const {
    return m_captureClient.GetStats();
}

bool WasapiFanOutEngine::AddOutput(const std::string& branchId,
                                   const std::wstring& endpointId,
                                   std::string& outError,
                                   bool driftCorrectionEnabled,
                                   double delayMs) {
    std::lock_guard<std::mutex> lock(m_engineMutex);
    m_lastError.clear();

    if (std::isnan(delayMs) || std::isinf(delayMs) || delayMs < 0.0 || delayMs > 500.0) {
        outError = "Output delay must be a finite number between 0.0 and 500.0 milliseconds.";
        m_lastError = outError;
        return false;
    }

    if (!m_captureClient.IsCapturing()) {
        outError = "Cannot add output: WASAPI loopback capture is not active.";
        m_lastError = outError;
        return false;
    }

    if (branchId.empty()) {
        outError = "Branch identifier cannot be empty.";
        m_lastError = outError;
        return false;
    }

    // Check for duplicate branchId
    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId) {
            outError = "Branch with identifier '" + branchId + "' already exists.";
            m_lastError = outError;
            return false;
        }
    }

    // Feedback loop prevention check:
    // Rendering captured audio back to the same endpoint creates a harsh acoustic/digital feedback loop.
    WasapiCaptureStats capStats = m_captureClient.GetStats();
    std::string targetEndpointUtf8 = WideToUtf8(endpointId.c_str());
    bool isSameEndpoint = false;
    if (endpointId.empty() && capStats.deviceId.empty()) {
        isSameEndpoint = true;
    } else if (!endpointId.empty() && targetEndpointUtf8 == capStats.deviceId) {
        isSameEndpoint = true;
    }

    if (isSameEndpoint) {
        outError = "Feedback loop prevention: Cannot render to the same endpoint (" +
                   (capStats.deviceFriendlyName.empty() ? "Default Endpoint" : capStats.deviceFriendlyName) +
                   ") that is being captured via WASAPI loopback. Select a secondary physical or virtual output endpoint.";
        m_lastError = outError;
        return false;
    }

    // Check for duplicate endpointId across active branches
    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->endpointId == endpointId) {
            outError = "Output endpoint is already in use by branch '" + m_branches[i]->branchId + "'.";
            m_lastError = outError;
            return false;
        }
    }

    // Find free slot
    size_t freeSlot = MAX_BRANCHES;
    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (!m_branches[i]) {
            freeSlot = i;
            break;
        }
    }

    if (freeSlot >= MAX_BRANCHES) {
        outError = "Maximum output branch limit (" + std::to_string(MAX_BRANCHES) + ") reached.";
        m_lastError = outError;
        return false;
    }

    // Preallocate branch ring buffer (65536 frames ≈ 1.36s at 48kHz)
    const size_t ringCapacity = 65536;
    auto ringBuffer = std::make_unique<AudioRingBuffer>(ringCapacity, capStats.channels);

    // Initialize and start render client on the target endpoint
    auto renderClient = std::make_unique<WasapiRenderClient>();
    renderClient->SetDriftCorrectionEnabled(driftCorrectionEnabled);
    std::string delayErr;
    if (!renderClient->SetDelayMs(delayMs, delayErr)) {
        outError = delayErr;
        m_lastError = outError;
        return false;
    }

    renderClient->SetDeviceLostCallback([this](WasapiRenderClient* c) {
        OnRenderClientDeviceLost(c);
    });
    bool renderOk = renderClient->StartRender(endpointId,
                                              ringBuffer.get(),
                                              capStats.sampleRate,
                                              capStats.channels,
                                              outError);
    if (!renderOk) {
        m_lastError = outError;
        return false;
    }

    // Atomically register ring buffer to receive captured frames
    m_captureClient.SetBranchBuffer(freeSlot, ringBuffer.get());

    // Register branch descriptor
    auto desc = std::make_unique<BranchDescriptor>();
    desc->branchId = branchId;
    desc->endpointId = endpointId;
    desc->deviceFriendlyName = renderClient->GetStats().deviceFriendlyName;
    desc->slotIndex = freeSlot;
    desc->ringBuffer = std::move(ringBuffer);
    desc->renderClient = std::move(renderClient);
    desc->active.store(true, std::memory_order_release);
    desc->driftCorrectionEnabled = driftCorrectionEnabled;
    desc->delayMs = delayMs;
    desc->recoveryAttempts.store(0, std::memory_order_relaxed);
    desc->nextRetryTime = std::chrono::steady_clock::now();

    m_branches[freeSlot] = std::move(desc);
    return true;
}

bool WasapiFanOutEngine::RemoveOutput(const std::string& branchId) {
    std::lock_guard<std::mutex> lock(m_engineMutex);

    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId) {
            // 1. Atomically detach from capture distribution so no new frames are pushed
            m_captureClient.SetBranchBuffer(i, nullptr);
            m_branches[i]->active.store(false, std::memory_order_release);

            // 2. Stop render client and join its worker thread
            m_branches[i]->renderClient->StopRender();

            // 3. Release branch memory
            m_branches[i].reset();
            return true;
        }
    }

    return false;
}

bool WasapiFanOutEngine::IsBranchActive(const std::string& branchId) const {
    std::lock_guard<std::mutex> lock(m_engineMutex);

    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId) {
            return m_branches[i]->active.load(std::memory_order_acquire);
        }
    }

    return false;
}

bool WasapiFanOutEngine::SetBranchDriftCorrectionEnabled(const std::string& branchId, bool enabled) {
    std::lock_guard<std::mutex> lock(m_engineMutex);

    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId) {
            if (m_branches[i]->renderClient) {
                m_branches[i]->renderClient->SetDriftCorrectionEnabled(enabled);
                return true;
            }
        }
    }

    return false;
}

bool WasapiFanOutEngine::IsBranchDriftCorrectionEnabled(const std::string& branchId) const {
    std::lock_guard<std::mutex> lock(m_engineMutex);

    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId) {
            if (m_branches[i]->renderClient) {
                return m_branches[i]->renderClient->IsDriftCorrectionEnabled();
            }
        }
    }

    return false;
}

bool WasapiFanOutEngine::SetBranchDelay(const std::string& branchId, double delayMs, std::string& outError) {
    std::lock_guard<std::mutex> lock(m_engineMutex);
    m_lastError.clear();

    if (std::isnan(delayMs) || std::isinf(delayMs) || delayMs < 0.0 || delayMs > 500.0) {
        outError = "Output delay must be a finite number between 0.0 and 500.0 milliseconds.";
        m_lastError = outError;
        return false;
    }

    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId) {
            if (m_branches[i]->renderClient) {
                if (!m_branches[i]->renderClient->SetDelayMs(delayMs, outError)) {
                    m_lastError = outError;
                    return false;
                }
                m_branches[i]->delayMs = delayMs;
                return true;
            }
        }
    }

    outError = "Branch with identifier '" + branchId + "' was not found.";
    m_lastError = outError;
    return false;
}

bool WasapiFanOutEngine::GetBranchDelay(const std::string& branchId, double& outDelayMs, size_t& outDelayFrames) const {
    std::lock_guard<std::mutex> lock(m_engineMutex);

    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId) {
            if (m_branches[i]->renderClient) {
                outDelayMs = m_branches[i]->renderClient->GetConfiguredDelayMs();
                outDelayFrames = m_branches[i]->renderClient->GetEffectiveDelayFrames();
                return true;
            }
        }
    }

    return false;
}

void WasapiFanOutEngine::PopulateBranchStats(const BranchDescriptor& branch, WasapiBranchStats& bStats) const {
    bStats.branchId = branch.branchId;
    bStats.endpointId = WideToUtf8(branch.endpointId.c_str());
    bStats.deviceFriendlyName = branch.deviceFriendlyName;
    bStats.active = branch.active.load(std::memory_order_relaxed);

    if (branch.renderClient) {
        bStats.renderStats = branch.renderClient->GetStats();
        bStats.renderState = bStats.renderStats.bufferStateName;
        bStats.lifecycleState = bStats.renderStats.lifecycleState;
        bStats.lifecycleStateName = bStats.renderStats.lifecycleStateName;
        bStats.lastDeviceLossError = bStats.renderStats.lastDeviceLossError;
        bStats.recoveryAttemptCount = bStats.renderStats.recoveryAttemptCount;
        bStats.lastRecoveryHresult = bStats.renderStats.lastRecoveryHresult;
        bStats.recoveryPending = bStats.renderStats.recoveryPending;

        DriftTelemetry telem{};
        DriftControllerStatus ctrlStatus{};
        branch.renderClient->GetDriftSnapshot(telem, ctrlStatus);

        bStats.occupancyFrames = bStats.renderStats.ringBufferFrames;
        bStats.occupancyMs = bStats.renderStats.ringBufferOccupancyMs;
        bStats.occupancyErrorFrames = telem.occupancyError;
        bStats.occupancySlope = telem.occupancySlope;
        bStats.driftPpm = telem.driftPpm;
        bStats.estimatorStable = telem.isStable;

        bStats.trueDriftPpm = ctrlStatus.trueDriftPpm;
        bStats.filteredDriftPpm = ctrlStatus.filteredDriftPpm;
        bStats.feedforwardCorrection = ctrlStatus.feedforwardCorrection;
        bStats.feedbackCorrection = ctrlStatus.feedbackCorrection;
        bStats.targetMultiplier = ctrlStatus.targetMultiplier;
        bStats.isClamped = ctrlStatus.isClamped;
        bStats.driftCorrectionEnabled = ctrlStatus.driftCorrectionEnabled;
        bStats.configuredDelayMs = bStats.renderStats.configuredDelayMs;
        bStats.effectiveDelayFrames = bStats.renderStats.effectiveDelayFrames;
    } else {
        bStats.renderStats = WasapiRenderStats{};
        bStats.renderState = "Stopped";
        bStats.lifecycleState = RenderLifecycleState::Stopped;
        bStats.lifecycleStateName = "Stopped";
        bStats.lastDeviceLossError = 0;
        bStats.recoveryAttemptCount = 0;
        bStats.lastRecoveryHresult = 0;
        bStats.recoveryPending = false;
        bStats.occupancyFrames = 0;
        bStats.occupancyMs = 0.0;
        bStats.occupancyErrorFrames = 0;
        bStats.occupancySlope = 0.0;
        bStats.driftPpm = 0.0;
        bStats.estimatorStable = false;
        bStats.trueDriftPpm = 0.0;
        bStats.filteredDriftPpm = 0.0;
        bStats.feedforwardCorrection = 0.0;
        bStats.feedbackCorrection = 0.0;
        bStats.targetMultiplier = 1.0;
        bStats.isClamped = false;
        bStats.driftCorrectionEnabled = true;
        bStats.configuredDelayMs = branch.delayMs;
        bStats.effectiveDelayFrames = 0;
    }
}

bool WasapiFanOutEngine::GetOutputStats(const std::string& branchId, WasapiBranchStats& outStats) const {
    std::lock_guard<std::mutex> lock(m_engineMutex);

    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId) {
            PopulateBranchStats(*m_branches[i], outStats);
            return true;
        }
    }

    return false;
}

WasapiFanOutEngineStatus WasapiFanOutEngine::GetStatus() const {
    std::lock_guard<std::mutex> lock(m_engineMutex);

    WasapiFanOutEngineStatus status;
    status.isCapturing = m_captureClient.IsCapturing();
    status.captureStats = m_captureClient.GetStats();
    status.lastError = m_lastError;

    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i]) {
            WasapiBranchStats bStats;
            PopulateBranchStats(*m_branches[i], bStats);
            status.branches.push_back(bStats);
        }
    }

    return status;
}

void WasapiFanOutEngine::Shutdown() {
    StopCapture();

    if (!m_stopRecoveryThread.exchange(true)) {
        m_recoveryCv.notify_all();
        if (m_recoveryThread.joinable()) {
            m_recoveryThread.join();
        }
    }
}

void WasapiFanOutEngine::OnDeviceStateChanged(const std::wstring& deviceId, DWORD newState) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        for (size_t i = 0; i < MAX_BRANCHES; ++i) {
            if (m_branches[i] && m_branches[i]->active.load(std::memory_order_relaxed)) {
                if (m_branches[i]->endpointId == deviceId || (m_branches[i]->endpointId.empty() && deviceId.empty())) {
                    if (newState == DEVICE_STATE_ACTIVE) {
                        m_branches[i]->renderClient->ScheduleRecovery();
                        m_branches[i]->nextRetryTime = std::chrono::steady_clock::now();
                        notify = true;
                    } else {
                        m_branches[i]->renderClient->ScheduleRecovery();
                        notify = true;
                    }
                }
            }
        }
    }
    if (notify) {
        m_recoveryCv.notify_all();
    }
}

void WasapiFanOutEngine::OnDeviceAdded(const std::wstring& deviceId) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        for (size_t i = 0; i < MAX_BRANCHES; ++i) {
            if (m_branches[i] && m_branches[i]->active.load(std::memory_order_relaxed)) {
                if (m_branches[i]->endpointId == deviceId) {
                    m_branches[i]->renderClient->ScheduleRecovery();
                    m_branches[i]->nextRetryTime = std::chrono::steady_clock::now();
                    notify = true;
                }
            }
        }
    }
    if (notify) {
        m_recoveryCv.notify_all();
    }
}

void WasapiFanOutEngine::OnDefaultDeviceChanged(const std::wstring& defaultDeviceId) {
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        for (size_t i = 0; i < MAX_BRANCHES; ++i) {
            if (m_branches[i] && m_branches[i]->active.load(std::memory_order_relaxed)) {
                if (m_branches[i]->endpointId.empty()) {
                    m_branches[i]->renderClient->ScheduleRecovery();
                    m_branches[i]->nextRetryTime = std::chrono::steady_clock::now();
                    notify = true;
                }
            }
        }
    }
    if (notify) {
        m_recoveryCv.notify_all();
    }
}

bool WasapiFanOutEngine::TriggerBranchRecovery(const std::string& branchId) {
    bool scheduled = false;
    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        for (size_t i = 0; i < MAX_BRANCHES; ++i) {
            if (m_branches[i] && m_branches[i]->branchId == branchId &&
                m_branches[i]->active.load(std::memory_order_relaxed)) {
                m_branches[i]->renderClient->ScheduleRecovery();
                m_branches[i]->nextRetryTime = std::chrono::steady_clock::now();
                scheduled = true;
                break;
            }
        }
    }
    if (scheduled) {
        m_recoveryCv.notify_all();
    }
    return scheduled;
}

bool WasapiFanOutEngine::SimulateBranchDeviceLossForTesting(const std::string& branchId, HRESULT hr) {
    std::lock_guard<std::mutex> lock(m_engineMutex);
    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId &&
            m_branches[i]->active.load(std::memory_order_relaxed)) {
            m_branches[i]->renderClient->SimulateDeviceLossForTesting(hr);
            return true;
        }
    }
    return false;
}

void WasapiFanOutEngine::OnRenderClientDeviceLost(WasapiRenderClient* pClient) {
    if (!pClient) return;
    bool scheduled = false;
    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        for (size_t i = 0; i < MAX_BRANCHES; ++i) {
            if (m_branches[i] && m_branches[i]->renderClient.get() == pClient &&
                m_branches[i]->active.load(std::memory_order_relaxed)) {
                if (m_branches[i]->renderClient->ScheduleRecovery()) {
                    m_branches[i]->recoveryAttempts.store(0, std::memory_order_relaxed);
                    m_branches[i]->nextRetryTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
                    scheduled = true;
                }
                break;
            }
        }
    }
    if (scheduled) {
        m_recoveryCv.notify_all();
    }
}

bool WasapiFanOutEngine::HasAnyPendingRecovery() const {
    std::lock_guard<std::mutex> lock(m_engineMutex);
    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->active.load(std::memory_order_relaxed)) {
            RenderLifecycleState st = m_branches[i]->renderClient->GetLifecycleState();
            if (st == RenderLifecycleState::DeviceLost || st == RenderLifecycleState::RecoveryPending) {
                return true;
            }
        }
    }
    return false;
}

void WasapiFanOutEngine::RecoveryThreadProc() {
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    while (!m_stopRecoveryThread.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> cvLock(m_recoveryMutex);

        auto now = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point earliestWakeup = now + std::chrono::hours(1);
        bool hasPending = false;

        {
            std::lock_guard<std::mutex> lock(m_engineMutex);
            for (size_t i = 0; i < MAX_BRANCHES; ++i) {
                if (m_branches[i] && m_branches[i]->active.load(std::memory_order_acquire)) {
                    RenderLifecycleState st = m_branches[i]->renderClient->GetLifecycleState();
                    if (st == RenderLifecycleState::DeviceLost) {
                        m_branches[i]->renderClient->ScheduleRecovery();
                        st = RenderLifecycleState::RecoveryPending;
                        m_branches[i]->nextRetryTime = now + std::chrono::milliseconds(50);
                    }
                    if (st == RenderLifecycleState::RecoveryPending) {
                        hasPending = true;
                        if (m_branches[i]->nextRetryTime <= now) {
                            earliestWakeup = now;
                        } else if (m_branches[i]->nextRetryTime < earliestWakeup) {
                            earliestWakeup = m_branches[i]->nextRetryTime;
                        }
                    }
                }
            }
        }

        if (m_stopRecoveryThread.load(std::memory_order_acquire)) {
            break;
        }

        if (!hasPending) {
            m_recoveryCv.wait(cvLock, [this]() {
                return m_stopRecoveryThread.load(std::memory_order_acquire) || HasAnyPendingRecovery();
            });
        } else if (earliestWakeup > now) {
            m_recoveryCv.wait_until(cvLock, earliestWakeup, [this]() {
                return m_stopRecoveryThread.load(std::memory_order_acquire);
            });
        }

        if (m_stopRecoveryThread.load(std::memory_order_acquire)) {
            break;
        }

        // Process branches due for recovery
        now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < MAX_BRANCHES; ++i) {
            RecoverBranchIfDue(i, now);
        }
    }

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
}

void WasapiFanOutEngine::RecoverBranchIfDue(size_t slotIndex, const std::chrono::steady_clock::time_point& now) {
    if (slotIndex >= MAX_BRANCHES) return;

    WasapiRenderClient* pClient = nullptr;
    std::string branchId;
    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        if (!m_branches[slotIndex] || !m_branches[slotIndex]->active.load(std::memory_order_acquire)) {
            return;
        }
        if (m_branches[slotIndex]->renderClient->GetLifecycleState() != RenderLifecycleState::RecoveryPending) {
            return;
        }
        if (m_branches[slotIndex]->nextRetryTime > now) {
            return;
        }
        pClient = m_branches[slotIndex]->renderClient.get();
        branchId = m_branches[slotIndex]->branchId;
    }

    if (!pClient) return;

    // Call ReinitializeRender outside m_engineMutex
    std::string err;
    bool ok = pClient->ReinitializeRender(err);

    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        // Verify branch is still active and unmodified
        if (!m_branches[slotIndex] || !m_branches[slotIndex]->active.load(std::memory_order_acquire) ||
            m_branches[slotIndex]->branchId != branchId) {
            // Branch was stopped or removed during reinitialization attempt
            if (pClient && ok) {
                pClient->StopRender();
            }
            return;
        }

        if (ok) {
            m_branches[slotIndex]->recoveryAttempts.store(0, std::memory_order_relaxed);
        } else {
            uint32_t attempts = m_branches[slotIndex]->recoveryAttempts.fetch_add(1, std::memory_order_relaxed) + 1;
            if (attempts >= 5) {
                // Max recovery attempts reached; ReinitializeRender set state to Failed
            } else {
                // Backoff: 100ms * 2^(attempts-1): 100ms, 200ms, 400ms, 800ms...
                uint32_t delayMs = 100 * (1 << std::min<uint32_t>(attempts, 4));
                m_branches[slotIndex]->nextRetryTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
            }
        }
    }
}

} // namespace speakerflow
