#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>

#include "audio_ring_buffer.h"
#include "wasapi_capture_client.h" // For SampleFormatType
#include "audio_format_pipeline.h"
#include "audio_drift_estimator.h"
#include "audio_drift_controller.h"

namespace speakerflow {

/**
 * RenderLifecycleState:
 * - Represents the lifecycle of the WASAPI audio endpoint and render worker thread.
 * - Distinct from RenderBufferState (which tracks audio ring-buffer state: Preroll / Running / Recovery).
 * - DeviceLost indicates the underlying WASAPI audio endpoint was invalidated or disconnected
 *   (e.g., AUDCLNT_E_DEVICE_INVALIDATED), meaning this endpoint instance cannot continue and
 *   requires external cleanup and future recovery.
 * - Automatic recovery is intentionally NOT part of Phase 2F.1-A.
 */
enum class RenderLifecycleState : uint32_t {
    Stopped = 0,
    Starting = 1,
    Running = 2,
    DeviceLost = 3,
    Stopping = 4,
    Failed = 5
};

inline const char* RenderLifecycleStateToString(RenderLifecycleState state) {
    switch (state) {
        case RenderLifecycleState::Stopped:    return "Stopped";
        case RenderLifecycleState::Starting:   return "Starting";
        case RenderLifecycleState::Running:    return "Running";
        case RenderLifecycleState::DeviceLost: return "DeviceLost";
        case RenderLifecycleState::Stopping:   return "Stopping";
        case RenderLifecycleState::Failed:     return "Failed";
        default: return "Unknown";
    }
}

enum class RenderBufferState : uint32_t {
    Preroll = 0,
    Running = 1,
    Recovery = 2
};

struct WasapiRenderStats {
    bool isRendering = false;
    bool isEventDriven = false;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t bitsPerSample = 0;
    std::string formatTag;
    std::string deviceId;
    std::string deviceFriendlyName;
    uint64_t framesRendered = 0;
    uint64_t silentFramesRendered = 0;
    uint64_t bufferUnderruns = 0;
    uint64_t bufferOverruns = 0;
    uint64_t underrunRecoveries = 0;
    size_t ringBufferFrames = 0;
    size_t ringBufferCapacity = 0;
    double bufferDurationMs = 0.0;
    double ringBufferOccupancyMs = 0.0;
    RenderBufferState bufferState = RenderBufferState::Preroll;
    std::string bufferStateName = "Preroll";
    double resampleRatioMultiplier = 1.0;
    bool driftCorrectionEnabled = true;
    std::string lastError;
    RenderLifecycleState lifecycleState = RenderLifecycleState::Stopped;
    std::string lifecycleStateName = "Stopped";
    uint32_t lastDeviceLossError = 0;
};

class WasapiRenderClient {
public:
    WasapiRenderClient();
    ~WasapiRenderClient();

    // Non-copyable, non-movable
    WasapiRenderClient(const WasapiRenderClient&) = delete;
    WasapiRenderClient& operator=(const WasapiRenderClient&) = delete;

    /**
     * @brief Starts WASAPI shared-mode render on the specified endpoint, consuming from pRingBuffer.
     * @param targetDeviceId Device endpoint ID, or empty for default console render endpoint.
     * @param pRingBuffer Lock-free ring buffer containing captured float32 PCM.
     * @param captureSampleRate Sample rate of the incoming captured audio.
     * @param captureChannels Channel count of the incoming captured audio.
     * @param outError Receives error description if start fails.
     * @return true if render started successfully, false otherwise.
     */
    bool StartRender(const std::wstring& targetDeviceId,
                     AudioRingBuffer* pRingBuffer,
                     uint32_t captureSampleRate,
                     uint32_t captureChannels,
                     std::string& outError);

    /**
     * @brief Stops WASAPI rendering and joins the worker thread.
     */
    void StopRender();

    /**
     * @brief Checks if rendering is currently active.
     */
    bool IsRendering() const;

    /**
     * @brief Retrieves thread-safe diagnostic stats for telemetry and UI.
     */
    WasapiRenderStats GetStats() const;

    /**
     * @brief Sets resampling ratio multiplier for fine clock-drift adjustments (Phase 2E ready).
     */
    void SetResampleRatioMultiplier(double multiplier);

    /**
     * @brief Gets current resampling ratio multiplier.
     */
    double GetResampleRatioMultiplier() const;

    /**
     * @brief Gets count of underrun recovery events experienced by this render branch.
     */
    uint64_t GetUnderrunRecoveryCount() const;

    /**
     * @brief Gets the current internal buffer state (Preroll, Running, Recovery).
     */
    RenderBufferState GetBufferState() const;

    /**
     * @brief Checks if rendering has reached the genuine steady Running state.
     */
    bool IsBufferRunning() const;

    /**
     * @brief Gets a copy of the latest DriftTelemetry snapshot from the render-thread estimator.
     */
    DriftTelemetry GetDriftTelemetry() const;

    /**
     * @brief Gets a copy of the latest DriftControllerStatus snapshot from the render-thread controller.
     */
    DriftControllerStatus GetDriftControllerStatus() const;

    /**
     * @brief Enables or disables real-time drift correction for this render branch (Phase 2E.3-C).
     */
    void SetDriftCorrectionEnabled(bool enabled);

    /**
     * @brief Checks if real-time drift correction is enabled for this render branch (Phase 2E.3-C).
     */
    bool IsDriftCorrectionEnabled() const;

    /**
     * @brief Gets current render endpoint/thread lifecycle state.
     */
    RenderLifecycleState GetLifecycleState() const;

    /**
     * @brief Gets current lifecycle state as human-readable string.
     */
    std::string GetLifecycleStateName() const;

    /**
     * @brief Gets the last HRESULT recorded during device invalidation (or S_OK).
     */
    HRESULT GetLastDeviceLossHresult() const;

    /**
     * @brief Simulates device loss (AUDCLNT_E_DEVICE_INVALIDATED) for deterministic unit testing.
     */
    void SimulateDeviceLossForTesting(HRESULT hr = 0x88890004 /* AUDCLNT_E_DEVICE_INVALIDATED */);

    /**
     * @brief Gets a coherent combined snapshot of both DriftTelemetry and DriftControllerStatus
     *        from the render thread in a single atomic reader lease (lock-free, non-blocking reader).
     */
    void GetDriftSnapshot(DriftTelemetry& outTelemetry, DriftControllerStatus& outStatus) const;

private:
    void RenderThreadProc();
    void CleanupResources();

    std::atomic<bool> m_isRendering;
    std::atomic<bool> m_isEventDriven;
    std::atomic<RenderLifecycleState> m_lifecycleState{RenderLifecycleState::Stopped};
    std::atomic<HRESULT> m_lastDeviceLossHr{S_OK};
    mutable std::mutex m_controlMutex;
    std::wstring m_targetDeviceId;
    std::string m_deviceFriendlyName;

    uint32_t m_sampleRate;
    uint32_t m_channels;
    uint32_t m_bitsPerSample;
    SampleFormatType m_formatType;
    std::string m_formatTagName;
    UINT32 m_bufferFrameCount;

    uint32_t m_captureSampleRate;
    uint32_t m_captureChannels;

    std::atomic<uint64_t> m_framesRendered;
    std::atomic<uint64_t> m_silentFramesRendered;
    std::atomic<uint64_t> m_underrunRecoveryCount;
    std::atomic<double> m_resampleRatioMultiplier;
    std::atomic<bool> m_driftCorrectionEnabled{true};
    std::atomic<RenderBufferState> m_bufferState{RenderBufferState::Preroll};

    mutable std::mutex m_errorMutex;
    std::string m_lastError;

    AudioRingBuffer* m_pRingBuffer;

    HANDLE m_hStopEvent;
    HANDLE m_hAudioEvent;
    std::thread m_thread;

    IMMDevice* m_pDevice;
    IAudioClient* m_pAudioClient;
    IAudioRenderClient* m_pRenderClient;
    WAVEFORMATEX* m_pMixFormat;

    AudioFormatPipeline m_pipeline;

    AudioDriftEstimator m_driftEstimator;
    AudioDriftController m_driftController;

    // Multi-slot lock-free snapshot publication with reader lifetime protection.
    // Guaranteed zero data races: writer never overwrites a slot while readerCount > 0.
    struct DriftSnapshot {
        DriftTelemetry telemetry{};
        DriftControllerStatus controllerStatus{};
    };

    static constexpr size_t NUM_SNAPSHOT_SLOTS = 4;
    DriftSnapshot m_snapshots[NUM_SNAPSHOT_SLOTS]{};
    mutable std::atomic<uint32_t> m_readerCounts[NUM_SNAPSHOT_SLOTS]{};
    std::atomic<uint32_t> m_publishedVersion{0};

    void PublishTelemetrySnapshot(const DriftTelemetry& telemetry, const DriftControllerStatus& status);
};

} // namespace speakerflow
