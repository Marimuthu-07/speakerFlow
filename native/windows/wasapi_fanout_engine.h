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

#include "wasapi_capture_client.h"
#include "wasapi_render_client.h"
#include "audio_ring_buffer.h"
#include "audio_drift_estimator.h"

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
    bool AddOutput(const std::string& branchId, const std::wstring& endpointId, std::string& outError);

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
        std::unique_ptr<AudioDriftEstimator> driftEstimator;
        std::atomic<bool> active{false};
        mutable uint64_t lastRecoveryCount = 0;
    };

    void PopulateBranchStats(const BranchDescriptor& branch, WasapiBranchStats& bStats) const;

    mutable std::mutex m_engineMutex;
    WasapiCaptureClient m_captureClient;
    std::wstring m_captureDeviceId;
    std::string m_lastError;

    std::array<std::unique_ptr<BranchDescriptor>, MAX_BRANCHES> m_branches;
};

} // namespace speakerflow
