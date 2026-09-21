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
#include <array>
#include <mutex>

#include "audio_ring_buffer.h"

namespace speakerflow {

enum class SampleFormatType {
    Unknown,
    Float32,
    Pcm16,
    Pcm24In32,
    Pcm24Packed,
    Pcm32
};

struct WasapiCaptureStats {
    bool isCapturing = false;
    bool isEventDriven = false;
    uint32_t sampleRate = 0;
    uint32_t channels = 0;
    uint32_t bitsPerSample = 0;
    std::string formatTag;
    std::string deviceId;
    std::string deviceFriendlyName;
    uint64_t framesCaptured = 0;
    uint64_t silentFramesCaptured = 0;
    uint64_t packetsCaptured = 0;
    float peakLevel = 0.0f;
    float rmsLevel = 0.0f;
    uint64_t ringBufferUnderruns = 0;
    uint64_t ringBufferOverruns = 0;
    size_t ringBufferFrames = 0;
    size_t ringBufferCapacity = 0;
    std::string lastError;
};

class WasapiCaptureClient {
public:
    WasapiCaptureClient();
    ~WasapiCaptureClient();

    // Non-copyable, non-movable
    WasapiCaptureClient(const WasapiCaptureClient&) = delete;
    WasapiCaptureClient& operator=(const WasapiCaptureClient&) = delete;

    /**
     * @brief Starts WASAPI loopback capture on the specified endpoint or system default render endpoint.
     * @param targetDeviceId Device endpoint ID, or empty for default console render endpoint.
     * @param outError Receives error description if start fails.
     * @return true if capture started successfully, false otherwise.
     */
    bool StartCapture(const std::wstring& targetDeviceId, std::string& outError);

    /**
     * @brief Stops WASAPI loopback capture and joins the capture worker thread.
     */
    void StopCapture();

    /**
     * @brief Checks if capture is currently active.
     */
    bool IsCapturing() const;

    /**
     * @brief Retrieves thread-safe diagnostic stats for telemetry and UI.
     */
    WasapiCaptureStats GetStats() const;

    /**
     * @brief Returns pointer to the captured PCM ring buffer.
     */
    AudioRingBuffer* GetRingBuffer() const { return m_ringBuffer.get(); }

    static constexpr size_t MAX_FANOUT_SLOTS = 8;

    /**
     * @brief Registers or unregisters a branch audio ring buffer to receive captured audio frames.
     * @param slot Slot index [0 .. MAX_FANOUT_SLOTS - 1].
     * @param pBuffer Pointer to branch ring buffer, or nullptr to unregister.
     * @return true if slot index is valid, false otherwise.
     */
    bool SetBranchBuffer(size_t slot, AudioRingBuffer* pBuffer) {
        if (slot >= MAX_FANOUT_SLOTS) return false;
        m_branchBuffers[slot].store(pBuffer, std::memory_order_release);
        return true;
    }

    void ClearBranchBuffers() {
        for (size_t i = 0; i < MAX_FANOUT_SLOTS; ++i) {
            m_branchBuffers[i].store(nullptr, std::memory_order_release);
        }
    }

private:
    void CaptureThreadProc();
    static void ConvertRawToFloat(const BYTE* pSrc,
                                  float* pDest,
                                  size_t frames,
                                  SampleFormatType fmt,
                                  size_t channels);
    void UpdateLevels(const float* pData, size_t totalSamples);

    std::atomic<bool> m_isCapturing;
    std::atomic<bool> m_isEventDriven;
    std::wstring m_targetDeviceId;
    std::string m_deviceFriendlyName;

    uint32_t m_sampleRate;
    uint32_t m_channels;
    uint32_t m_bitsPerSample;
    SampleFormatType m_formatType;
    std::string m_formatTagName;

    std::atomic<uint64_t> m_framesCaptured;
    std::atomic<uint64_t> m_silentFramesCaptured;
    std::atomic<uint64_t> m_packetsCaptured;
    std::atomic<float> m_peakLevel;
    std::atomic<float> m_rmsLevel;

    mutable std::mutex m_errorMutex;
    std::string m_lastError;

    std::unique_ptr<AudioRingBuffer> m_ringBuffer;
    std::array<std::atomic<AudioRingBuffer*>, MAX_FANOUT_SLOTS> m_branchBuffers;

    HANDLE m_hStopEvent;
    HANDLE m_hAudioEvent;
    std::thread m_thread;

    IMMDevice* m_pDevice;
    IAudioClient* m_pAudioClient;
    IAudioCaptureClient* m_pCaptureClient;
    WAVEFORMATEX* m_pMixFormat;
};

} // namespace speakerflow
