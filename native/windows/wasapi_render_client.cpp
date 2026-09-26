#include "wasapi_render_client.h"

#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>

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

static std::string FormatHresult(const char* action, HRESULT hr) {
    std::stringstream ss;
    ss << action << " failed with HRESULT 0x"
       << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << (uint32_t)hr;
    return ss.str();
}

WasapiRenderClient::WasapiRenderClient()
    : m_isRendering(false),
      m_isEventDriven(false),
      m_sampleRate(48000),
      m_channels(2),
      m_bitsPerSample(32),
      m_formatType(SampleFormatType::Float32),
      m_formatTagName("IEEE_FLOAT"),
      m_bufferFrameCount(0),
      m_captureSampleRate(48000),
      m_captureChannels(2),
      m_framesRendered(0),
      m_silentFramesRendered(0),
      m_underrunRecoveryCount(0),
      m_resampleRatioMultiplier(1.0),
      m_bufferState(RenderBufferState::Preroll),
      m_pRingBuffer(nullptr),
      m_hStopEvent(NULL),
      m_hAudioEvent(NULL),
      m_pDevice(nullptr),
      m_pAudioClient(nullptr),
      m_pRenderClient(nullptr),
      m_pMixFormat(nullptr),
      m_publishedVersion(0) {
    m_driftEstimator.Reset();
    m_driftController.Reset();
    for (size_t i = 0; i < NUM_SNAPSHOT_SLOTS; ++i) {
        m_readerCounts[i].store(0, std::memory_order_relaxed);
        m_snapshots[i].telemetry = m_driftEstimator.GetTelemetry();
        m_snapshots[i].controllerStatus = m_driftController.GetStatus();
    }
}

WasapiRenderClient::~WasapiRenderClient() {
    StopRender();
}

bool WasapiRenderClient::IsRendering() const {
    return m_isRendering.load(std::memory_order_acquire);
}

bool WasapiRenderClient::StartRender(const std::wstring& targetDeviceId,
                                     AudioRingBuffer* pRingBuffer,
                                     uint32_t captureSampleRate,
                                     uint32_t captureChannels,
                                     std::string& outError) {
    StopRender();

    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError.clear();

    if (!pRingBuffer) {
        outError = "Ring buffer pointer is null.";
        m_lastError = outError;
        return false;
    }

    m_targetDeviceId = targetDeviceId;
    m_pRingBuffer = pRingBuffer;
    m_captureSampleRate = captureSampleRate ? captureSampleRate : 48000;
    m_captureChannels = captureChannels ? captureChannels : 2;

    m_framesRendered.store(0, std::memory_order_relaxed);
    m_silentFramesRendered.store(0, std::memory_order_relaxed);
    m_underrunRecoveryCount.store(0, std::memory_order_relaxed);
    m_resampleRatioMultiplier.store(1.0, std::memory_order_relaxed);
    m_bufferState.store(RenderBufferState::Preroll, std::memory_order_relaxed);

    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                  NULL,
                                  CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) {
        outError = FormatHresult("CoCreateInstance(MMDeviceEnumerator)", hr);
        m_lastError = outError;
        return false;
    }

    if (m_targetDeviceId.empty()) {
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_pDevice);
    } else {
        hr = pEnumerator->GetDevice(m_targetDeviceId.c_str(), &m_pDevice);
    }
    pEnumerator->Release();

    if (FAILED(hr) || !m_pDevice) {
        outError = FormatHresult("GetAudioEndpoint(Render)", hr);
        m_lastError = outError;
        return false;
    }

    // Retrieve friendly name
    m_deviceFriendlyName.clear();
    IPropertyStore* pProps = nullptr;
    if (SUCCEEDED(m_pDevice->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
            if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                m_deviceFriendlyName = WideToUtf8(varName.pwszVal);
            }
        }
        PropVariantClear(&varName);
        pProps->Release();
    }

    // Activate IAudioClient
    hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&m_pAudioClient);
    if (FAILED(hr) || !m_pAudioClient) {
        m_pDevice->Release();
        m_pDevice = nullptr;
        outError = FormatHresult("Activate(IAudioClient)", hr);
        m_lastError = outError;
        return false;
    }

    // Get native mix format
    hr = m_pAudioClient->GetMixFormat(&m_pMixFormat);
    if (FAILED(hr) || !m_pMixFormat) {
        m_pAudioClient->Release();
        m_pAudioClient = nullptr;
        m_pDevice->Release();
        m_pDevice = nullptr;
        outError = FormatHresult("GetMixFormat", hr);
        m_lastError = outError;
        return false;
    }

    m_sampleRate = m_pMixFormat->nSamplesPerSec;
    m_channels = m_pMixFormat->nChannels;
    m_bitsPerSample = m_pMixFormat->wBitsPerSample;

    // Detect format type
    m_formatType = SampleFormatType::Unknown;
    m_formatTagName = "Unknown";

    if (m_pMixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        m_formatType = SampleFormatType::Float32;
        m_formatTagName = "IEEE_FLOAT";
    } else if (m_pMixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* pExt = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(m_pMixFormat);
        if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            m_formatType = SampleFormatType::Float32;
            m_formatTagName = "EXTENSIBLE_IEEE_FLOAT";
        } else if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            if (m_bitsPerSample == 16) {
                m_formatType = SampleFormatType::Pcm16;
                m_formatTagName = "EXTENSIBLE_PCM_16";
            } else if (m_bitsPerSample == 24) {
                m_formatType = (m_pMixFormat->nBlockAlign == m_channels * 4)
                                   ? SampleFormatType::Pcm24In32
                                   : SampleFormatType::Pcm24Packed;
                m_formatTagName = "EXTENSIBLE_PCM_24";
            } else if (m_bitsPerSample == 32) {
                m_formatType = SampleFormatType::Pcm32;
                m_formatTagName = "EXTENSIBLE_PCM_32";
            }
        }
    } else if (m_pMixFormat->wFormatTag == WAVE_FORMAT_PCM) {
        if (m_bitsPerSample == 16) {
            m_formatType = SampleFormatType::Pcm16;
            m_formatTagName = "PCM_16";
        } else if (m_bitsPerSample == 24) {
            m_formatType = (m_pMixFormat->nBlockAlign == m_channels * 4)
                               ? SampleFormatType::Pcm24In32
                               : SampleFormatType::Pcm24Packed;
            m_formatTagName = "PCM_24";
        } else if (m_bitsPerSample == 32) {
            m_formatType = SampleFormatType::Pcm32;
            m_formatTagName = "PCM_32";
        }
    }

    if (m_formatType == SampleFormatType::Unknown) {
        CoTaskMemFree(m_pMixFormat);
        m_pMixFormat = nullptr;
        m_pAudioClient->Release();
        m_pAudioClient = nullptr;
        m_pDevice->Release();
        m_pDevice = nullptr;
        outError = "Unsupported render mix format encountered.";
        m_lastError = outError;
        return false;
    }

    // Try event-driven shared mode rendering
    m_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    m_hAudioEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    REFERENCE_TIME hnsRequestedDuration = 1000000; // 100ms buffer
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = m_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    streamFlags,
                                    hnsRequestedDuration,
                                    0,
                                    m_pMixFormat,
                                    NULL);

    bool eventDriven = false;
    if (SUCCEEDED(hr)) {
        hr = m_pAudioClient->SetEventHandle(m_hAudioEvent);
        if (SUCCEEDED(hr)) {
            eventDriven = true;
        }
    }

    // Fallback to timer-driven if event-driven flag rejected
    if (!eventDriven) {
        if (m_hAudioEvent) {
            CloseHandle(m_hAudioEvent);
            m_hAudioEvent = NULL;
        }
        m_pAudioClient->Release();
        m_pAudioClient = nullptr;

        hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&m_pAudioClient);
        if (SUCCEEDED(hr) && m_pAudioClient) {
            streamFlags = 0;
            hr = m_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                            streamFlags,
                                            hnsRequestedDuration,
                                            0,
                                            m_pMixFormat,
                                            NULL);
        }
    }

    if (FAILED(hr)) {
        if (m_hAudioEvent) { CloseHandle(m_hAudioEvent); m_hAudioEvent = NULL; }
        if (m_hStopEvent) { CloseHandle(m_hStopEvent); m_hStopEvent = NULL; }
        if (m_pMixFormat) { CoTaskMemFree(m_pMixFormat); m_pMixFormat = nullptr; }
        if (m_pAudioClient) { m_pAudioClient->Release(); m_pAudioClient = nullptr; }
        if (m_pDevice) { m_pDevice->Release(); m_pDevice = nullptr; }
        outError = FormatHresult("Initialize(IAudioClient RENDER)", hr);
        m_lastError = outError;
        return false;
    }

    m_isEventDriven.store(eventDriven, std::memory_order_relaxed);

    // Obtain IAudioRenderClient
    hr = m_pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&m_pRenderClient);
    if (FAILED(hr) || !m_pRenderClient) {
        if (m_hAudioEvent) { CloseHandle(m_hAudioEvent); m_hAudioEvent = NULL; }
        if (m_hStopEvent) { CloseHandle(m_hStopEvent); m_hStopEvent = NULL; }
        if (m_pMixFormat) { CoTaskMemFree(m_pMixFormat); m_pMixFormat = nullptr; }
        if (m_pAudioClient) { m_pAudioClient->Release(); m_pAudioClient = nullptr; }
        if (m_pDevice) { m_pDevice->Release(); m_pDevice = nullptr; }
        outError = FormatHresult("GetService(IAudioRenderClient)", hr);
        m_lastError = outError;
        return false;
    }

    m_bufferFrameCount = 0;
    m_pAudioClient->GetBufferSize(&m_bufferFrameCount);

    const size_t maxFrames = std::max<size_t>(m_bufferFrameCount * 4, 8192);
    if (!m_pipeline.Initialize(m_captureSampleRate,
                               m_captureChannels,
                               m_sampleRate,
                               m_channels,
                               m_formatType,
                               maxFrames)) {
        if (m_hAudioEvent) { CloseHandle(m_hAudioEvent); m_hAudioEvent = NULL; }
        if (m_hStopEvent) { CloseHandle(m_hStopEvent); m_hStopEvent = NULL; }
        if (m_pMixFormat) { CoTaskMemFree(m_pMixFormat); m_pMixFormat = nullptr; }
        if (m_pRenderClient) { m_pRenderClient->Release(); m_pRenderClient = nullptr; }
        if (m_pAudioClient) { m_pAudioClient->Release(); m_pAudioClient = nullptr; }
        if (m_pDevice) { m_pDevice->Release(); m_pDevice = nullptr; }
        outError = "Failed to initialize AudioFormatPipeline.";
        m_lastError = outError;
        return false;
    }

    // Pre-roll: prime the WASAPI render buffer with initial silence
    BYTE* pPrimeData = nullptr;
    hr = m_pRenderClient->GetBuffer(m_bufferFrameCount, &pPrimeData);
    if (SUCCEEDED(hr) && pPrimeData) {
        m_pRenderClient->ReleaseBuffer(m_bufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    // Initialize drift estimator and controller for closed-loop real-time compensation
    const size_t targetOccupancy = static_cast<size_t>(m_captureSampleRate * 0.050);
    m_driftEstimator.SetSampleRate(m_captureSampleRate);
    m_driftEstimator.SetTargetOccupancy(targetOccupancy);
    m_driftEstimator.Reset();
    m_driftController.Reset();
    m_resampleRatioMultiplier.store(1.0, std::memory_order_relaxed);

    PublishTelemetrySnapshot(m_driftEstimator.GetTelemetry(), m_driftController.GetStatus());

    // Launch dedicated render worker thread
    m_isRendering.store(true, std::memory_order_release);
    m_thread = std::thread(&WasapiRenderClient::RenderThreadProc, this);

    return true;
}

void WasapiRenderClient::StopRender() {
    m_bufferState.store(RenderBufferState::Preroll, std::memory_order_relaxed);

    if (!m_isRendering.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (m_hStopEvent) {
        SetEvent(m_hStopEvent);
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_driftEstimator.Reset();
    m_driftController.Reset();
    m_resampleRatioMultiplier.store(1.0, std::memory_order_relaxed);
    m_pipeline.Reset();

    PublishTelemetrySnapshot(m_driftEstimator.GetTelemetry(), m_driftController.GetStatus());

    if (m_hAudioEvent) {
        CloseHandle(m_hAudioEvent);
        m_hAudioEvent = NULL;
    }
    if (m_hStopEvent) {
        CloseHandle(m_hStopEvent);
        m_hStopEvent = NULL;
    }

    if (m_pRenderClient) {
        m_pRenderClient->Release();
        m_pRenderClient = nullptr;
    }
    if (m_pAudioClient) {
        m_pAudioClient->Release();
        m_pAudioClient = nullptr;
    }
    if (m_pDevice) {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }
    if (m_pMixFormat) {
        CoTaskMemFree(m_pMixFormat);
        m_pMixFormat = nullptr;
    }

    m_pRingBuffer = nullptr;
}

void WasapiRenderClient::RenderThreadProc() {
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    DWORD taskIndex = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Playback", &taskIndex);
    if (!hMmcss) {
        taskIndex = 0;
        hMmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    }

    HRESULT hrStart = m_pAudioClient->Start();
    if (FAILED(hrStart)) {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = FormatHresult("IAudioClient::Start(Render)", hrStart);
        m_isRendering.store(false, std::memory_order_release);
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return;
    }

    // Pre-allocate staging buffer ONCE before entering the real-time loop.
    // Zero heap allocations inside the audio real-time path.
    const size_t maxFrames = std::max<size_t>(m_bufferFrameCount * 4, 8192);
    std::vector<float> captureStaging(maxFrames * m_captureChannels, 0.0f);

    RenderBufferState bufferState = RenderBufferState::Preroll;
    m_bufferState.store(RenderBufferState::Preroll, std::memory_order_relaxed);

    // On startup: reset drift estimator and controller to neutral ratio 1.0
    m_driftEstimator.Reset();
    m_driftController.Reset();
    m_resampleRatioMultiplier.store(1.0, std::memory_order_relaxed);
    m_pipeline.SetResampleRatioMultiplier(1.0);
    PublishTelemetrySnapshot(m_driftEstimator.GetTelemetry(), m_driftController.GetStatus());

    // Watermarks for underrun recovery hysteresis:
    // High watermark: 50ms of audio (or 2x WASAPI buffer)
    const size_t highWatermarkFrames = std::max<size_t>(m_bufferFrameCount * 2,
                                                        static_cast<size_t>(m_captureSampleRate * 0.050));
    // Low watermark minimum: ~5ms
    const size_t lowWatermarkMinFrames = static_cast<size_t>(m_captureSampleRate * 0.005);

    const bool isEventDriven = m_isEventDriven.load(std::memory_order_relaxed);
    HANDLE waitHandles[2] = { m_hStopEvent, m_hAudioEvent };
    const DWORD waitCount = isEventDriven ? 2 : 1;

    while (m_isRendering.load(std::memory_order_relaxed)) {
        DWORD waitRes = WaitForMultipleObjects(waitCount, waitHandles, FALSE, isEventDriven ? 100 : 10);
        if (waitRes == WAIT_OBJECT_0) {
            // Stop event signaled
            break;
        }

        UINT32 paddingFrames = 0;
        HRESULT hrPad = m_pAudioClient->GetCurrentPadding(&paddingFrames);
        if (FAILED(hrPad)) {
            if (hrPad == AUDCLNT_E_DEVICE_INVALIDATED) {
                std::lock_guard<std::mutex> lock(m_errorMutex);
                m_lastError = "WASAPI render device was invalidated or disconnected.";
            }
            break;
        }

        if (paddingFrames >= m_bufferFrameCount) {
            continue;
        }

        UINT32 framesNeeded = m_bufferFrameCount - paddingFrames;
        if (framesNeeded == 0) {
            continue;
        }

        size_t available = (m_pRingBuffer != nullptr) ? m_pRingBuffer->AvailableRead() : 0;
        size_t framesToReadEstimate = m_pipeline.GetRequiredInFrames(framesNeeded);
        if (framesToReadEstimate > maxFrames) {
            framesToReadEstimate = maxFrames;
        }
        const size_t lowWatermarkFrames = std::max(framesToReadEstimate, lowWatermarkMinFrames);

        // State machine: Preroll -> Running <-> Recovery
        if (bufferState == RenderBufferState::Preroll) {
            if (available < highWatermarkFrames) {
                // Prime buffer with silence until initial 50ms watermark is reached
                BYTE* pData = nullptr;
                HRESULT hrBuf = m_pRenderClient->GetBuffer(framesNeeded, &pData);
                if (SUCCEEDED(hrBuf) && pData) {
                    m_pRenderClient->ReleaseBuffer(framesNeeded, AUDCLNT_BUFFERFLAGS_SILENT);
                    m_silentFramesRendered.fetch_add(framesNeeded, std::memory_order_relaxed);
                }
                m_driftEstimator.Reset();
                m_driftController.Reset();
                m_resampleRatioMultiplier.store(1.0, std::memory_order_relaxed);
                m_pipeline.SetResampleRatioMultiplier(1.0);
                PublishTelemetrySnapshot(m_driftEstimator.GetTelemetry(), m_driftController.GetStatus());
                continue;
            }
            bufferState = RenderBufferState::Running;
            m_bufferState.store(RenderBufferState::Running, std::memory_order_relaxed);
            m_driftEstimator.Reset();
            m_driftController.Reset();
            m_resampleRatioMultiplier.store(1.0, std::memory_order_relaxed);
            m_pipeline.SetResampleRatioMultiplier(1.0);
            PublishTelemetrySnapshot(m_driftEstimator.GetTelemetry(), m_driftController.GetStatus());
        } else if (bufferState == RenderBufferState::Running) {
            if (available < lowWatermarkFrames) {
                // Genuine starvation detected: enter Recovery and pause consumption
                bufferState = RenderBufferState::Recovery;
                m_bufferState.store(RenderBufferState::Recovery, std::memory_order_relaxed);
                m_underrunRecoveryCount.fetch_add(1, std::memory_order_relaxed);
                m_pipeline.Reset(); // Clear resampler history across silence gap

                // Reset drift estimator and controller on underrun recovery to prevent stale drift accumulation
                m_driftEstimator.Reset();
                m_driftController.Reset();
                m_resampleRatioMultiplier.store(1.0, std::memory_order_relaxed);
                m_pipeline.SetResampleRatioMultiplier(1.0);
                PublishTelemetrySnapshot(m_driftEstimator.GetTelemetry(), m_driftController.GetStatus());

                BYTE* pData = nullptr;
                HRESULT hrBuf = m_pRenderClient->GetBuffer(framesNeeded, &pData);
                if (SUCCEEDED(hrBuf) && pData) {
                    m_pRenderClient->ReleaseBuffer(framesNeeded, AUDCLNT_BUFFERFLAGS_SILENT);
                    m_silentFramesRendered.fetch_add(framesNeeded, std::memory_order_relaxed);
                }
                continue;
            }
        } else if (bufferState == RenderBufferState::Recovery) {
            if (available < highWatermarkFrames) {
                // In recovery: wait for ring buffer to refill to high watermark (50ms)
                BYTE* pData = nullptr;
                HRESULT hrBuf = m_pRenderClient->GetBuffer(framesNeeded, &pData);
                if (SUCCEEDED(hrBuf) && pData) {
                    m_pRenderClient->ReleaseBuffer(framesNeeded, AUDCLNT_BUFFERFLAGS_SILENT);
                    m_silentFramesRendered.fetch_add(framesNeeded, std::memory_order_relaxed);
                }
                m_driftEstimator.Reset();
                m_driftController.Reset();
                m_resampleRatioMultiplier.store(1.0, std::memory_order_relaxed);
                m_pipeline.SetResampleRatioMultiplier(1.0);
                PublishTelemetrySnapshot(m_driftEstimator.GetTelemetry(), m_driftController.GetStatus());
                continue;
            }
            // Refilled to high watermark: resume steady-state rendering
            bufferState = RenderBufferState::Running;
            m_bufferState.store(RenderBufferState::Running, std::memory_order_relaxed);
            m_driftEstimator.Reset();
            m_driftController.Reset();
            m_resampleRatioMultiplier.store(1.0, std::memory_order_relaxed);
            m_pipeline.SetResampleRatioMultiplier(1.0);
            PublishTelemetrySnapshot(m_driftEstimator.GetTelemetry(), m_driftController.GetStatus());
        }

        // Running state with sufficient buffer: update closed-loop drift estimator & controller
        m_driftEstimator.Update(available, m_framesRendered.load(std::memory_order_relaxed));
        DriftTelemetry tel = m_driftEstimator.GetTelemetry();
        double currentRatio = m_driftController.Update(tel);

        // Apply ratio multiplier through existing atomic hook and pipeline
        m_resampleRatioMultiplier.store(currentRatio, std::memory_order_relaxed);
        if (currentRatio != m_pipeline.GetResampleRatioMultiplier()) {
            m_pipeline.SetResampleRatioMultiplier(currentRatio);
        }

        // Publish lock-free telemetry snapshot to inactive slot and publish atomically
        PublishTelemetrySnapshot(tel, m_driftController.GetStatus());

        // Calculate input frames needed by AudioFormatPipeline with updated ratio
        size_t framesToReadFromRing = m_pipeline.GetRequiredInFrames(framesNeeded);
        if (framesToReadFromRing > maxFrames) {
            framesToReadFromRing = maxFrames;
        }

        // Running state with sufficient buffer: acquire render buffer
        BYTE* pRenderData = nullptr;
        HRESULT hrBuf = m_pRenderClient->GetBuffer(framesNeeded, &pRenderData);
        if (FAILED(hrBuf)) {
            if (hrBuf == AUDCLNT_E_DEVICE_INVALIDATED) {
                std::lock_guard<std::mutex> lock(m_errorMutex);
                m_lastError = "WASAPI render device was invalidated or disconnected.";
            }
            break;
        }

        if (!m_pRingBuffer) {
            m_pRenderClient->ReleaseBuffer(framesNeeded, AUDCLNT_BUFFERFLAGS_SILENT);
            m_silentFramesRendered.fetch_add(framesNeeded, std::memory_order_relaxed);
            continue;
        }

        // Lock-free read from ring buffer
        size_t framesRead = m_pRingBuffer->Read(captureStaging.data(), framesToReadFromRing);

        // Process through AudioFormatPipeline: Resample -> Channel Map -> Format Convert
        size_t framesProduced = 0;
        m_pipeline.Process(captureStaging.data(),
                           framesRead,
                           pRenderData,
                           framesNeeded,
                           framesProduced);

        m_pRenderClient->ReleaseBuffer(framesNeeded, 0);
        m_framesRendered.fetch_add(framesNeeded, std::memory_order_relaxed);
    }

    m_pAudioClient->Stop();

    if (hMmcss) {
        AvRevertMmThreadCharacteristics(hMmcss);
    }
    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
}

WasapiRenderStats WasapiRenderClient::GetStats() const {
    WasapiRenderStats stats;
    stats.isRendering = m_isRendering.load(std::memory_order_acquire);
    stats.isEventDriven = m_isEventDriven.load(std::memory_order_relaxed);
    stats.sampleRate = m_sampleRate;
    stats.channels = m_channels;
    stats.bitsPerSample = m_bitsPerSample;
    stats.formatTag = m_formatTagName;
    stats.deviceId = WideToUtf8(m_targetDeviceId.c_str());
    stats.deviceFriendlyName = m_deviceFriendlyName;
    stats.framesRendered = m_framesRendered.load(std::memory_order_relaxed);
    stats.silentFramesRendered = m_silentFramesRendered.load(std::memory_order_relaxed);
    stats.underrunRecoveries = m_underrunRecoveryCount.load(std::memory_order_relaxed);
    stats.bufferUnderruns = stats.underrunRecoveries;
    stats.bufferState = m_bufferState.load(std::memory_order_relaxed);
    switch (stats.bufferState) {
        case RenderBufferState::Preroll:  stats.bufferStateName = "Preroll"; break;
        case RenderBufferState::Running:  stats.bufferStateName = "Running"; break;
        case RenderBufferState::Recovery: stats.bufferStateName = "Recovery"; break;
        default: stats.bufferStateName = "Unknown"; break;
    }
    stats.resampleRatioMultiplier = m_resampleRatioMultiplier.load(std::memory_order_relaxed);

    if (m_pRingBuffer) {
        stats.bufferUnderruns += m_pRingBuffer->GetUnderrunCount();
        stats.bufferOverruns = m_pRingBuffer->GetOverrunCount();
        stats.ringBufferFrames = m_pRingBuffer->AvailableRead();
        stats.ringBufferCapacity = m_pRingBuffer->GetCapacity();
        if (m_captureSampleRate > 0) {
            stats.ringBufferOccupancyMs = (static_cast<double>(stats.ringBufferFrames) / m_captureSampleRate) * 1000.0;
        }
    }

    if (m_sampleRate > 0 && m_bufferFrameCount > 0) {
        stats.bufferDurationMs = (static_cast<double>(m_bufferFrameCount) / m_sampleRate) * 1000.0;
    }

    std::lock_guard<std::mutex> lock(m_errorMutex);
    stats.lastError = m_lastError;
    return stats;
}

void WasapiRenderClient::SetResampleRatioMultiplier(double multiplier) {
    if (multiplier > 0.01 && multiplier < 100.0) {
        m_resampleRatioMultiplier.store(multiplier, std::memory_order_relaxed);
    }
}

double WasapiRenderClient::GetResampleRatioMultiplier() const {
    return m_resampleRatioMultiplier.load(std::memory_order_relaxed);
}

uint64_t WasapiRenderClient::GetUnderrunRecoveryCount() const {
    return m_underrunRecoveryCount.load(std::memory_order_relaxed);
}

RenderBufferState WasapiRenderClient::GetBufferState() const {
    return m_bufferState.load(std::memory_order_relaxed);
}

bool WasapiRenderClient::IsBufferRunning() const {
    return m_bufferState.load(std::memory_order_relaxed) == RenderBufferState::Running;
}

void WasapiRenderClient::PublishTelemetrySnapshot(const DriftTelemetry& telemetry,
                                                 const DriftControllerStatus& status) {
    uint32_t currentVer = m_publishedVersion.load(std::memory_order_seq_cst);
    uint32_t currentSlot = currentVer % NUM_SNAPSHOT_SLOTS;

    // Find a free slot that is NOT currently published and has NO active readers
    int writeSlot = -1;
    for (size_t i = 1; i < NUM_SNAPSHOT_SLOTS; ++i) {
        size_t candidate = (currentSlot + i) % NUM_SNAPSHOT_SLOTS;
        if (m_readerCounts[candidate].load(std::memory_order_seq_cst) == 0) {
            writeSlot = static_cast<int>(candidate);
            break;
        }
    }

    if (writeSlot < 0) {
        // All alternate slots currently held by readers; skip publishing this cycle.
        // Non-blocking, bounded publication attempt for the real-time audio thread.
        return;
    }

    // writeSlot is guaranteed not published and has 0 active readers
    m_snapshots[writeSlot].telemetry = telemetry;
    m_snapshots[writeSlot].controllerStatus = status;

    // Advance monotonic version counter such that (currentVer + offset) % NUM_SNAPSHOT_SLOTS == writeSlot
    uint32_t offset = (static_cast<uint32_t>(writeSlot) + NUM_SNAPSHOT_SLOTS - (currentVer % NUM_SNAPSHOT_SLOTS)) % NUM_SNAPSHOT_SLOTS;
    if (offset == 0) offset = NUM_SNAPSHOT_SLOTS;
    m_publishedVersion.store(currentVer + offset, std::memory_order_seq_cst);
}

DriftTelemetry WasapiRenderClient::GetDriftTelemetry() const {
    while (true) {
        uint32_t ver = m_publishedVersion.load(std::memory_order_seq_cst);
        uint32_t slot = ver % NUM_SNAPSHOT_SLOTS;
        m_readerCounts[slot].fetch_add(1, std::memory_order_seq_cst);
        if (m_publishedVersion.load(std::memory_order_seq_cst) == ver) {
            DriftTelemetry result = m_snapshots[slot].telemetry;
            m_readerCounts[slot].fetch_sub(1, std::memory_order_seq_cst);
            return result;
        }
        m_readerCounts[slot].fetch_sub(1, std::memory_order_seq_cst);
    }
}

DriftControllerStatus WasapiRenderClient::GetDriftControllerStatus() const {
    while (true) {
        uint32_t ver = m_publishedVersion.load(std::memory_order_seq_cst);
        uint32_t slot = ver % NUM_SNAPSHOT_SLOTS;
        m_readerCounts[slot].fetch_add(1, std::memory_order_seq_cst);
        if (m_publishedVersion.load(std::memory_order_seq_cst) == ver) {
            DriftControllerStatus result = m_snapshots[slot].controllerStatus;
            m_readerCounts[slot].fetch_sub(1, std::memory_order_seq_cst);
            return result;
        }
        m_readerCounts[slot].fetch_sub(1, std::memory_order_seq_cst);
    }
}

void WasapiRenderClient::GetDriftSnapshot(DriftTelemetry& outTelemetry,
                                         DriftControllerStatus& outStatus) const {
    while (true) {
        uint32_t ver = m_publishedVersion.load(std::memory_order_seq_cst);
        uint32_t slot = ver % NUM_SNAPSHOT_SLOTS;
        m_readerCounts[slot].fetch_add(1, std::memory_order_seq_cst);
        if (m_publishedVersion.load(std::memory_order_seq_cst) == ver) {
            outTelemetry = m_snapshots[slot].telemetry;
            outStatus = m_snapshots[slot].controllerStatus;
            m_readerCounts[slot].fetch_sub(1, std::memory_order_seq_cst);
            return;
        }
        m_readerCounts[slot].fetch_sub(1, std::memory_order_seq_cst);
    }
}

} // namespace speakerflow
