#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>
#include <array>
#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

#include "wasapi_capture_client.h"
#include "wasapi_render_client.h"
#include "audio_ring_buffer.h"
#include "audio_drift_estimator.h"
#include "audio_drift_controller.h"

namespace speakerflow {

struct WasapiBranchStats {
    std::string branchId;
    std::string endpointId;
    std::string deviceFriendlyName;
    bool active = false;
    WasapiRenderStats renderStats;
    size_t occupancyFrames = 0;
    double occupancyMs = 0.0;
    int64_t occupancyErrorFrames = 0;
    double occupancySlope = 0.0;
    double driftPpm = 0.0;
    bool estimatorStable = false;
    std::string renderState = "Preroll";
    RenderLifecycleState lifecycleState = RenderLifecycleState::Stopped;
    std::string lifecycleStateName = "Stopped";
    uint32_t lastDeviceLossError = 0;
    uint32_t recoveryAttemptCount = 0;
    uint32_t lastRecoveryHresult = 0;
    bool recoveryPending = false;

    // Closed-loop drift controller telemetry from render client
    double trueDriftPpm = 0.0;
    double filteredDriftPpm = 0.0;
    double feedforwardCorrection = 0.0;
    double feedbackCorrection = 0.0;
    double targetMultiplier = 1.0;
    bool isClamped = false;
    bool driftCorrectionEnabled = true;

    // Software acoustic delay configuration (Phase 2H-A)
    double configuredDelayMs = 0.0;
    uint32_t effectiveDelayFrames = 0;
};

struct WasapiFanOutEngineStatus {
    bool isCapturing = false;
    WasapiCaptureStats captureStats;
    std::vector<WasapiBranchStats> branches;
    std::string lastError;
};

class WasapiFanOutEngine {
public:
    static constexpr size_t MAX_BRANCHES = WasapiCaptureClient::MAX_FANOUT_SLOTS;

    WasapiFanOutEngine();
    ~WasapiFanOutEngine();

    // Non-copyable, non-movable
    WasapiFanOutEngine(const WasapiFanOutEngine&) = delete;
    WasapiFanOutEngine& operator=(const WasapiFanOutEngine&) = delete;

    /**
     * @brief Starts WASAPI loopback capture on the target or default endpoint.
     * @param targetDeviceId Device endpoint ID, or empty for default console render endpoint.
     * @param outError Receives error description if start fails.
     * @return true if capture started successfully, false otherwise.
     */
    bool StartCapture(const std::wstring& targetDeviceId, std::string& outError);

    /**
     * @brief Stops loopback capture and all active render branches.
     */
    void StopCapture();

    /**
     * @brief Checks if loopback capture is actively running.
     */
    bool IsCaptureActive() const;

    /**
     * @brief Retrieves capture diagnostic stats.
     */
    WasapiCaptureStats GetCaptureStats() const;

    /**
     * @brief Adds and starts a new render output branch from the active capture stream.
     * @param branchId Unique identifier for this branch.
     * @param endpointId WASAPI endpoint ID for the render device.
     * @param outError Receives error description if start fails.
     * @return true if branch was added and started successfully, false otherwise.
     */
    bool AddOutput(const std::string& branchId,
                   const std::wstring& endpointId,
                   std::string& outError,
                   bool driftCorrectionEnabled = true,
                   double delayMs = 0.0);

    /**
     * @brief Removes and stops an active render output branch.
     * @param branchId Identifier of branch to remove.
     * @return true if branch was found and removed, false otherwise.
     */
    bool RemoveOutput(const std::string& branchId);

    /**
     * @brief Checks if a specific branch is currently active.
     */
    bool IsBranchActive(const std::string& branchId) const;

    /**
     * @brief Enables or disables real-time drift correction for a specific branch (Phase 2E.3-C).
     * @param branchId Identifier of branch.
     * @param enabled True to enable closed-loop correction, false to disable.
     * @return true if branch was found and updated, false otherwise.
     */
    bool SetBranchDriftCorrectionEnabled(const std::string& branchId, bool enabled);

    /**
     * @brief Checks if real-time drift correction is enabled for a specific branch (Phase 2E.3-C).
     * @param branchId Identifier of branch.
     * @return true if enabled (or false if disabled or branch not found).
     */
    bool IsBranchDriftCorrectionEnabled(const std::string& branchId) const;

    /**
     * @brief Configures software acoustic delay in milliseconds for a specific branch (Phase 2H-A).
     * @param branchId Identifier of branch.
     * @param delayMs Delay in milliseconds (0.0 <= delayMs <= 500.0).
     * @param outError Receives error description if configuration fails.
     * @return true if branch was found and updated, false otherwise.
     */
    bool SetBranchDelay(const std::string& branchId, double delayMs, std::string& outError);

    /**
     * @brief Gets current software delay configuration for a specific branch.
     */
    bool GetBranchDelay(const std::string& branchId, double& outDelayMs, size_t& outDelayFrames) const;

    /**
     * @brief Core Audio notification: device state changed.
     *        If matching a lost/pending branch, triggers or accelerates recovery reinitialization.
     */
    void OnDeviceStateChanged(const std::wstring& deviceId, DWORD newState);

    /**
     * @brief Core Audio notification: device added.
     *        If matching a lost/pending branch, triggers immediate reinitialization attempt.
     */
    void OnDeviceAdded(const std::wstring& deviceId);

    /**
     * @brief Core Audio notification: default console render device changed.
     */
    void OnDefaultDeviceChanged(const std::wstring& defaultDeviceId);

    /**
     * @brief Explicitly schedules and triggers immediate recovery attempt for a branch.
     * @param branchId Identifier of branch to recover.
     * @return true if branch was found and scheduled, false otherwise.
     */
    bool TriggerBranchRecovery(const std::string& branchId);

    /**
     * @brief Deterministically simulates device loss on a branch for automated unit testing.
     * @param branchId Identifier of branch.
     * @param hr Simulated device loss HRESULT (default AUDCLNT_E_DEVICE_INVALIDATED).
     * @return true if branch was found and invalidated, false otherwise.
     */
    bool SimulateBranchDeviceLossForTesting(const std::string& branchId, HRESULT hr = 0x88890004);

    /**
     * @brief Retrieves diagnostics for a specific branch.
     */
    bool GetOutputStats(const std::string& branchId, WasapiBranchStats& outStats) const;

    /**
     * @brief Retrieves aggregate status of capture and all registered branches.
     */
    WasapiFanOutEngineStatus GetStatus() const;

    /**
     * @brief Complete shutdown of capture, all branches, and worker threads.
     */
    void Shutdown();

private:
    struct BranchDescriptor {
        std::string branchId;
        std::wstring endpointId;
        std::string deviceFriendlyName;
        size_t slotIndex = 0;
        std::unique_ptr<AudioRingBuffer> ringBuffer;
        std::unique_ptr<WasapiRenderClient> renderClient;
        std::atomic<bool> active{false};
        bool driftCorrectionEnabled = true;
        double delayMs = 0.0;

        // Bounded backoff recovery coordination
        std::chrono::steady_clock::time_point nextRetryTime{};
        std::atomic<uint32_t> recoveryAttempts{0};
    };

    void PopulateBranchStats(const BranchDescriptor& branch, WasapiBranchStats& bStats) const;
    void RecoveryThreadProc();
    void OnRenderClientDeviceLost(WasapiRenderClient* pClient);
    bool HasAnyPendingRecovery() const;
    void RecoverBranchIfDue(size_t slotIndex, const std::chrono::steady_clock::time_point& now);

    mutable std::mutex m_engineMutex;
    WasapiCaptureClient m_captureClient;
    std::wstring m_captureDeviceId;
    std::string m_lastError;

    std::array<std::unique_ptr<BranchDescriptor>, MAX_BRANCHES> m_branches;

    // Single background recovery coordinator thread for all output branches
    std::thread m_recoveryThread;
    std::condition_variable m_recoveryCv;
    std::mutex m_recoveryMutex;
    std::atomic<bool> m_stopRecoveryThread{false};
};

} // namespace speakerflow
