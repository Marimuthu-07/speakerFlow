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

static inline bool EndpointIdsMatch(const std::wstring& idA, const std::wstring& idB) {
    if (idA.empty() && idB.empty()) return true;
    if (idA.empty() || idB.empty()) return false;
    return _wcsicmp(idA.c_str(), idB.c_str()) == 0;
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
    auto ringBuffer = std::make_shared<AudioRingBuffer>(ringCapacity, capStats.channels);

    // Initialize and start render client on the target endpoint
    auto renderClient = std::make_shared<WasapiRenderClient>();
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
    desc->branchVolume.store(1.0f, std::memory_order_relaxed);
    desc->branchMuted.store(false, std::memory_order_relaxed);

    // Apply current master gain and initial branch gain to new render client
    renderClient->SetMasterVolume(m_masterVolume.load(std::memory_order_relaxed));
    renderClient->SetMasterMute(m_masterMuted.load(std::memory_order_relaxed));
    renderClient->SetBranchVolume(1.0f);
    renderClient->SetBranchMute(false);

    desc->ringBuffer = std::move(ringBuffer);
    desc->renderClient = std::move(renderClient);
    desc->active.store(true, std::memory_order_release);
    desc->driftCorrectionEnabled = driftCorrectionEnabled;
    desc->delayMs = delayMs;
    desc->recoveryAttempts.store(0, std::memory_order_relaxed);
    desc->nextRetryTime = std::chrono::steady_clock::now();
    desc->lastKnownDeviceState = DEVICE_STATE_ACTIVE;

    m_branches[freeSlot] = std::move(desc);
    return true;
}

bool WasapiFanOutEngine::RemoveOutput(const std::string& branchId) {
    std::lock_guard<std::mutex> lock(m_engineMutex);

    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId) {
            // 1. Synchronously detach capture distribution and wait for any in-flight write to complete
            m_captureClient.DetachBranchBufferSync(i);
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

void WasapiFanOutEngine::SetMasterVolume(float volume) {
    float clamped = std::max(0.0f, std::min(1.0f, volume));
    m_masterVolume.store(clamped, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_engineMutex);
    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->active.load(std::memory_order_relaxed) && m_branches[i]->renderClient) {
            m_branches[i]->renderClient->SetMasterVolume(clamped);
        }
    }
}

float WasapiFanOutEngine::GetMasterVolume() const {
    return m_masterVolume.load(std::memory_order_acquire);
}

void WasapiFanOutEngine::SetMasterMute(bool mute) {
    m_masterMuted.store(mute, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_engineMutex);
    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->active.load(std::memory_order_relaxed) && m_branches[i]->renderClient) {
            m_branches[i]->renderClient->SetMasterMute(mute);
        }
    }
}

bool WasapiFanOutEngine::IsMasterMuted() const {
    return m_masterMuted.load(std::memory_order_acquire);
}

bool WasapiFanOutEngine::SetBranchVolume(const std::string& branchId, float volume) {
    float clamped = std::max(0.0f, std::min(1.0f, volume));
    std::lock_guard<std::mutex> lock(m_engineMutex);
    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId) {
            m_branches[i]->branchVolume.store(clamped, std::memory_order_release);
            if (m_branches[i]->renderClient) {
                m_branches[i]->renderClient->SetBranchVolume(clamped);
            }
            return true;
        }
    }
    return false;
}

bool WasapiFanOutEngine::GetBranchVolume(const std::string& branchId, float& outVolume) const {
    std::lock_guard<std::mutex> lock(m_engineMutex);
    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId) {
            outVolume = m_branches[i]->branchVolume.load(std::memory_order_acquire);
            return true;
        }
    }
    return false;
}

bool WasapiFanOutEngine::SetBranchMute(const std::string& branchId, bool mute) {
    std::lock_guard<std::mutex> lock(m_engineMutex);
    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId) {
            m_branches[i]->branchMuted.store(mute, std::memory_order_release);
            if (m_branches[i]->renderClient) {
                m_branches[i]->renderClient->SetBranchMute(mute);
            }
            return true;
        }
    }
    return false;
}

bool WasapiFanOutEngine::IsBranchMuted(const std::string& branchId, bool& outMuted) const {
    std::lock_guard<std::mutex> lock(m_engineMutex);
    for (size_t i = 0; i < MAX_BRANCHES; ++i) {
        if (m_branches[i] && m_branches[i]->branchId == branchId) {
            outMuted = m_branches[i]->branchMuted.load(std::memory_order_acquire);
            return true;
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
        bStats.deviceState = bStats.renderStats.deviceState;
        bStats.waitingForDevice = bStats.renderStats.waitingForDevice;
        bStats.lastRecoveryTimestampMs = bStats.renderStats.lastRecoveryTimestampMs;

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

        bStats.masterVolume = bStats.renderStats.masterVolume;
        bStats.masterMuted = bStats.renderStats.masterMuted;
        bStats.branchVolume = bStats.renderStats.branchVolume;
        bStats.branchMuted = bStats.renderStats.branchMuted;
        bStats.effectiveGain = bStats.renderStats.effectiveGain;
    } else {
        bStats.renderStats = WasapiRenderStats{};
        bStats.renderState = "Stopped";
        bStats.lifecycleState = RenderLifecycleState::Stopped;
        bStats.lifecycleStateName = "Stopped";
        bStats.lastDeviceLossError = 0;
        bStats.recoveryAttemptCount = 0;
        bStats.lastRecoveryHresult = 0;
        bStats.recoveryPending = false;
        bStats.deviceState = 0;
        bStats.waitingForDevice = false;
        bStats.lastRecoveryTimestampMs = 0;
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

        bStats.masterVolume = m_masterVolume.load(std::memory_order_relaxed);
        bStats.masterMuted = m_masterMuted.load(std::memory_order_relaxed);
        bStats.branchVolume = branch.branchVolume.load(std::memory_order_relaxed);
        bStats.branchMuted = branch.branchMuted.load(std::memory_order_relaxed);
        bStats.effectiveGain = (bStats.masterMuted || bStats.branchMuted) ? 0.0f : (bStats.masterVolume * bStats.branchVolume);
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
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        for (size_t i = 0; i < MAX_BRANCHES; ++i) {
            if (m_branches[i] && m_branches[i]->active.load(std::memory_order_relaxed)) {
                if (EndpointIdsMatch(m_branches[i]->endpointId, deviceId)) {
                    m_branches[i]->lastStateChangeTime = now;
                    m_branches[i]->lastKnownDeviceState = newState;

                    if (newState == DEVICE_STATE_ACTIVE) {
                        RenderLifecycleState st = m_branches[i]->renderClient->GetLifecycleState();
                        // Phase 2F.1-I FIX 2: Reset exhausted recovery attempts when device returns
                        m_branches[i]->recoveryAttempts.store(0, std::memory_order_relaxed);

                        // Duplicate suppression & flurry extension: if recovery is already scheduled and waiting for settling delay, push forward
                        if (st == RenderLifecycleState::RecoveryPending && m_branches[i]->nextRetryTime > now) {
                            m_branches[i]->nextRetryTime = now + std::chrono::milliseconds(4000);
                            notify = true;
                            continue;
                        }
                        // Phase 2F.1-H Fix 1: Must allow recovery from Running state
                        m_branches[i]->renderClient->ScheduleRecovery(true);
                        // Phase 2F.1-H Fix 2: ~4000 ms adaptive settling delay for Bluetooth endpoint arrival
                        m_branches[i]->nextRetryTime = now + std::chrono::milliseconds(4000);
                        notify = true;
                    } else {
                        // Phase 2F.1-H Fix 1 & 4: Tracked endpoint transitioned away from ACTIVE (e.g. UNPLUGGED)
                        // Detach capture distribution and transition branch to WaitingForDevice state so dead client does not remain Running
                        m_captureClient.DetachBranchBufferSync(i);
                        m_branches[i]->renderClient->SetWaitingForDeviceState(true);
                        // Rearm/push settling deadline forward for future reconnect
                        m_branches[i]->nextRetryTime = now + std::chrono::milliseconds(4000);
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
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        for (size_t i = 0; i < MAX_BRANCHES; ++i) {
            if (m_branches[i] && m_branches[i]->active.load(std::memory_order_relaxed)) {
                if (EndpointIdsMatch(m_branches[i]->endpointId, deviceId)) {
                    m_branches[i]->lastStateChangeTime = now;
                    m_branches[i]->lastKnownDeviceState = DEVICE_STATE_ACTIVE;

                    RenderLifecycleState st = m_branches[i]->renderClient->GetLifecycleState();
                    // Phase 2F.1-I FIX 2: Reset exhausted recovery attempts when device returns
                    m_branches[i]->recoveryAttempts.store(0, std::memory_order_relaxed);

                    // Duplicate suppression & flurry extension: if recovery is already scheduled and waiting for settling delay, push forward
                    if (st == RenderLifecycleState::RecoveryPending && m_branches[i]->nextRetryTime > now) {
                        m_branches[i]->nextRetryTime = now + std::chrono::milliseconds(4000);
                        notify = true;
                        continue;
                    }
                    // Phase 2F.1-H Fix 1: Must allow recovery from Running state
                    m_branches[i]->renderClient->ScheduleRecovery(true);
                    // Phase 2F.1-H Fix 2: ~4000 ms adaptive settling delay for Bluetooth endpoint arrival
                    m_branches[i]->nextRetryTime = now + std::chrono::milliseconds(4000);
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
                    m_branches[i]->renderClient->ScheduleRecovery(true);
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

bool WasapiFanOutEngine::TriggerBranchRecovery(const std::string& branchId, bool forceImmediate) {
    bool scheduled = false;
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        for (size_t i = 0; i < MAX_BRANCHES; ++i) {
            if (m_branches[i] && m_branches[i]->branchId == branchId &&
                m_branches[i]->active.load(std::memory_order_relaxed)) {

                RenderLifecycleState current = m_branches[i]->renderClient->GetLifecycleState();
                bool isPending = (current == RenderLifecycleState::RecoveryPending);
                bool stateChanged = m_branches[i]->renderClient->ScheduleRecovery(true);

                if (!stateChanged && !isPending && current != RenderLifecycleState::WaitingForDevice) {
                    break;
                }

                // Phase 2F.1-I FIX 2: If currently in WaitingForDevice or retries were exhausted, reset budget on re-arm
                if (current == RenderLifecycleState::WaitingForDevice ||
                    m_branches[i]->recoveryAttempts.load(std::memory_order_relaxed) >= 5) {
                    m_branches[i]->recoveryAttempts.store(0, std::memory_order_relaxed);
                }

                // Phase 2F.1-H Fix 3: If forceImmediate is false (automatic JS reconciliation) and
                // a settling delay is already active in the future, preserve nextRetryTime!
                if (!forceImmediate && m_branches[i]->nextRetryTime > now) {
                    scheduled = true;
                    break;
                }

                // Explicit manual trigger (forceImmediate == true) or regular scheduling
                m_branches[i]->recoveryAttempts.store(0, std::memory_order_relaxed);
                m_branches[i]->nextRetryTime = now;
                m_branches[i]->lastStateChangeTime = std::chrono::steady_clock::time_point{};
                m_branches[i]->lastKnownDeviceState = DEVICE_STATE_ACTIVE;
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
                // Detach capture buffer immediately so capture thread does not pump samples to lost endpoint
                m_captureClient.DetachBranchBufferSync(i);
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
            if (st == RenderLifecycleState::DeviceLost ||
                st == RenderLifecycleState::RecoveryPending ||
                st == RenderLifecycleState::WaitingForDevice) {
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
                    if (st == RenderLifecycleState::RecoveryPending || st == RenderLifecycleState::WaitingForDevice) {
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
            m_recoveryCv.wait_until(cvLock, earliestWakeup);
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

    std::shared_ptr<WasapiRenderClient> pClient;
    std::shared_ptr<AudioRingBuffer> pFreshRing;
    std::string branchId;
    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        if (!m_branches[slotIndex] || !m_branches[slotIndex]->active.load(std::memory_order_acquire)) {
            return;
        }
        RenderLifecycleState st = m_branches[slotIndex]->renderClient->GetLifecycleState();
        if (st != RenderLifecycleState::RecoveryPending && st != RenderLifecycleState::WaitingForDevice) {
            return;
        }
        if (m_branches[slotIndex]->nextRetryTime > now) {
            return;
        }
        // Phase 2F.1-H Fix 2: Stability check - endpoint must have remained stable for >= 1500 ms since last transition
        if (m_branches[slotIndex]->lastStateChangeTime.time_since_epoch().count() > 0) {
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_branches[slotIndex]->lastStateChangeTime).count();
            if (elapsedMs < 1500) {
                m_branches[slotIndex]->nextRetryTime = m_branches[slotIndex]->lastStateChangeTime + std::chrono::milliseconds(1500);
                return;
            }
        }
        // If last known device state was not ACTIVE (e.g. UNPLUGGED), defer recovery until device arrival
        if (m_branches[slotIndex]->lastKnownDeviceState != DEVICE_STATE_ACTIVE) {
            return;
        }
        pClient = m_branches[slotIndex]->renderClient;
        branchId = m_branches[slotIndex]->branchId;
    }

    if (!pClient) return;

    // 1. Synchronously detach capture distribution so no concurrent write touches the buffer
    m_captureClient.DetachBranchBufferSync(slotIndex);

    // 2. Allocate a brand new clean AudioRingBuffer and retain shared ownership both locally
    //    and in the branch descriptor so concurrent removal cannot destroy it while in use.
    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        if (!m_branches[slotIndex] || !m_branches[slotIndex]->active.load(std::memory_order_acquire) ||
            m_branches[slotIndex]->branchId != branchId) {
            return;
        }
        pFreshRing = std::make_shared<AudioRingBuffer>(65536, m_captureClient.GetChannels());
        m_branches[slotIndex]->ringBuffer = pFreshRing;
    }

    // 3. Reinitialize WASAPI render resources with the clean buffer kept alive by pFreshRing
    std::string err;
    bool ok = pClient->ReinitializeRender(err, pFreshRing.get());

    {
        std::lock_guard<std::mutex> lock(m_engineMutex);
        // Verify branch is still active and unmodified
        if (!m_branches[slotIndex] || !m_branches[slotIndex]->active.load(std::memory_order_acquire) ||
            m_branches[slotIndex]->branchId != branchId) {
            // Branch was stopped or removed during reinitialization attempt
            if (pClient) {
                pClient->StopRender();
            }
            return;
        }

        if (ok) {
            // Wait for render thread to transition to Running state (ready to consume)
            for (int retry = 0; retry < 50; ++retry) {
                if (pClient->GetLifecycleState() == RenderLifecycleState::Running) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            m_branches[slotIndex]->recoveryAttempts.store(0, std::memory_order_relaxed);
            pClient->SetMasterVolume(m_masterVolume.load(std::memory_order_relaxed));
            pClient->SetMasterMute(m_masterMuted.load(std::memory_order_relaxed));
            pClient->SetBranchVolume(m_branches[slotIndex]->branchVolume.load(std::memory_order_relaxed));
            pClient->SetBranchMute(m_branches[slotIndex]->branchMuted.load(std::memory_order_relaxed));

            // 4. Re-attach the clean buffer to capture fan-out now that consumer is running and ready
            m_captureClient.SetBranchBuffer(slotIndex, m_branches[slotIndex]->ringBuffer.get());
        } else {
            // Phase 2F.1-I FIX 1: NEVER LEAVE A RECOVERY BRANCH WITH A NULL CAPTURE BUFFER
            if (m_branches[slotIndex]->ringBuffer) {
                m_captureClient.SetBranchBuffer(slotIndex, m_branches[slotIndex]->ringBuffer.get());
            }

            RenderLifecycleState newState = pClient->GetLifecycleState();
            if (newState == RenderLifecycleState::WaitingForDevice) {
                // Device unavailable or invalidarg -> wait with conservative fallback timer (2000ms)
                // or until Core Audio OnDeviceAdded / OnDeviceStateChanged wakes immediately
                m_branches[slotIndex]->nextRetryTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
            } else if (newState == RenderLifecycleState::RecoveryPending) {
                uint32_t attempts = m_branches[slotIndex]->recoveryAttempts.fetch_add(1, std::memory_order_relaxed) + 1;
                if (attempts >= 5) {
                    // Initial fast retries budget exhausted.
                    // Transition to WaitingForDevice to remain recoverable instead of permanent terminal Failed!
                    pClient->SetWaitingForDeviceState();
                    m_branches[slotIndex]->nextRetryTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
                } else {
                    // Backoff: 100ms * 2^(attempts-1): 100ms, 200ms, 400ms, 800ms...
                    uint32_t delayMs = 100 * (1 << std::min<uint32_t>(attempts, 4));
                    m_branches[slotIndex]->nextRetryTime = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
                }
            }
        }
    }
}

} // namespace speakerflow
