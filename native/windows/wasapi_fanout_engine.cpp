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
                                   std::string& outError) {
    std::lock_guard<std::mutex> lock(m_engineMutex);
    m_lastError.clear();

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

void WasapiFanOutEngine::PopulateBranchStats(const BranchDescriptor& branch, WasapiBranchStats& bStats) const {
    bStats.branchId = branch.branchId;
    bStats.endpointId = WideToUtf8(branch.endpointId.c_str());
    bStats.deviceFriendlyName = branch.deviceFriendlyName;
    bStats.active = branch.active.load(std::memory_order_relaxed);

    if (branch.renderClient) {
        bStats.renderStats = branch.renderClient->GetStats();
        bStats.renderState = bStats.renderStats.bufferStateName;

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
    } else {
        bStats.renderStats = WasapiRenderStats{};
        bStats.renderState = "Stopped";
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
}

} // namespace speakerflow
