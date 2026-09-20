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

namespace speakerflow {

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
    size_t ringBufferFrames = 0;
    size_t ringBufferCapacity = 0;
    double bufferDurationMs = 0.0;
    double ringBufferOccupancyMs = 0.0;
    std::string lastError;
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

private:
    void RenderThreadProc();

    static void ResampleFloatInterleaved(const float* pIn,
                                         size_t inFrames,
                                         float* pOut,
                                         size_t outFrames,
                                         size_t channels,
                                         double inSampleRate,
                                         double outSampleRate,
                                         double& ioPhase,
                                         std::vector<float>& lastFrameHistory);

    static void MapChannelsFloat(const float* pIn,
                                 size_t inChannels,
                                 float* pOut,
                                 size_t outChannels,
                                 size_t frames);

    static void ConvertFloatToRaw(const float* pSrc,
                                  BYTE* pDest,
                                  size_t frames,
                                  SampleFormatType fmt,
                                  size_t channels);

    std::atomic<bool> m_isRendering;
    std::atomic<bool> m_isEventDriven;
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
};

} // namespace speakerflow
