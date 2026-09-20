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
      m_pRingBuffer(nullptr),
      m_hStopEvent(NULL),
      m_hAudioEvent(NULL),
      m_pDevice(nullptr),
      m_pAudioClient(nullptr),
      m_pRenderClient(nullptr),
      m_pMixFormat(nullptr) {
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

    // Pre-roll: prime the WASAPI render buffer with initial silence
    BYTE* pPrimeData = nullptr;
    hr = m_pRenderClient->GetBuffer(m_bufferFrameCount, &pPrimeData);
    if (SUCCEEDED(hr) && pPrimeData) {
        m_pRenderClient->ReleaseBuffer(m_bufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    // Launch dedicated render worker thread
    m_isRendering.store(true, std::memory_order_release);
    m_thread = std::thread(&WasapiRenderClient::RenderThreadProc, this);

    return true;
}

void WasapiRenderClient::StopRender() {
    if (!m_isRendering.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    if (m_hStopEvent) {
        SetEvent(m_hStopEvent);
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

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

    // Pre-allocate all staging buffers ONCE before entering the loop.
    // Zero heap allocations inside the audio real-time path.
    const size_t maxFrames = std::max<size_t>(m_bufferFrameCount * 4, 8192);
    std::vector<float> captureStaging(maxFrames * m_captureChannels, 0.0f);
    std::vector<float> resampledStaging(maxFrames * m_captureChannels, 0.0f);
    std::vector<float> channelMappedStaging(maxFrames * m_channels, 0.0f);
    std::vector<float> lastFrameHistory(m_captureChannels, 0.0f);

    double resamplePhase = 0.0;
    bool hasPrerolled = false;
    // Initial pre-roll watermark: 50ms of audio or 2x WASAPI buffer
    const size_t prerollThresholdFrames = std::min<size_t>(m_bufferFrameCount * 2, 2400);

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

        // Check if initial jitter-buffer watermark has been reached
        if (!hasPrerolled) {
            if (m_pRingBuffer && m_pRingBuffer->AvailableRead() < prerollThresholdFrames) {
                // Not enough pre-roll yet: feed silence to WASAPI to avoid underrun click
                BYTE* pData = nullptr;
                HRESULT hrBuf = m_pRenderClient->GetBuffer(framesNeeded, &pData);
                if (SUCCEEDED(hrBuf) && pData) {
                    m_pRenderClient->ReleaseBuffer(framesNeeded, AUDCLNT_BUFFERFLAGS_SILENT);
                    m_silentFramesRendered.fetch_add(framesNeeded, std::memory_order_relaxed);
                }
                continue;
            }
            hasPrerolled = true;
        }

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

        // 1. Calculate how many frames to read from the capture ring buffer
        const bool ratesMatch = (m_captureSampleRate == m_sampleRate);
        size_t framesToReadFromRing = framesNeeded;
        if (!ratesMatch && m_sampleRate > 0) {
            double ratio = static_cast<double>(m_captureSampleRate) / static_cast<double>(m_sampleRate);
            framesToReadFromRing = static_cast<size_t>(std::ceil(framesNeeded * ratio + resamplePhase));
        }

        if (framesToReadFromRing > maxFrames) {
            framesToReadFromRing = maxFrames;
        }

        // Lock-free read from ring buffer (auto-pads with silence on underflow)
        size_t framesRead = m_pRingBuffer->Read(captureStaging.data(), framesToReadFromRing);

        // 2. Fractional resample if sample rates differ
        const float* pFloatSource = captureStaging.data();
        if (!ratesMatch) {
            ResampleFloatInterleaved(captureStaging.data(),
                                     framesToReadFromRing,
                                     resampledStaging.data(),
                                     framesNeeded,
                                     m_captureChannels,
                                     m_captureSampleRate,
                                     m_sampleRate,
                                     resamplePhase,
                                     lastFrameHistory);
            pFloatSource = resampledStaging.data();
        }

        // 3. Channel map if channel counts differ
        const float* pChannelMapped = pFloatSource;
        if (m_captureChannels != m_channels) {
            MapChannelsFloat(pFloatSource,
                             m_captureChannels,
                             channelMappedStaging.data(),
                             m_channels,
                             framesNeeded);
            pChannelMapped = channelMappedStaging.data();
        }

        // 4. Sample format conversion directly into WASAPI render buffer
        ConvertFloatToRaw(pChannelMapped,
                          pRenderData,
                          framesNeeded,
                          m_formatType,
                          m_channels);

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

void WasapiRenderClient::ResampleFloatInterleaved(const float* pIn,
                                                  size_t inFrames,
                                                  float* pOut,
                                                  size_t outFrames,
                                                  size_t channels,
                                                  double inSampleRate,
                                                  double outSampleRate,
                                                  double& ioPhase,
                                                  std::vector<float>& lastFrameHistory) {
    if (inSampleRate == outSampleRate || inFrames == 0 || outFrames == 0) {
        size_t framesToCopy = std::min(inFrames, outFrames);
        std::memcpy(pOut, pIn, framesToCopy * channels * sizeof(float));
        if (outFrames > framesToCopy) {
            std::memset(pOut + framesToCopy * channels, 0, (outFrames - framesToCopy) * channels * sizeof(float));
        }
        if (framesToCopy > 0) {
            for (size_t c = 0; c < channels; ++c) {
                lastFrameHistory[c] = pIn[(framesToCopy - 1) * channels + c];
            }
        }
        return;
    }

    const double ratio = inSampleRate / outSampleRate;
    double pos = ioPhase;

    for (size_t i = 0; i < outFrames; ++i) {
        size_t idx = static_cast<size_t>(pos);
        float frac = static_cast<float>(pos - idx);

        for (size_t c = 0; c < channels; ++c) {
            float s0 = 0.0f;
            float s1 = 0.0f;

            if (idx == 0) {
                s0 = lastFrameHistory[c];
                s1 = (inFrames > 0) ? pIn[0 * channels + c] : 0.0f;
            } else if (idx < inFrames) {
                s0 = pIn[(idx - 1) * channels + c];
                s1 = pIn[idx * channels + c];
            } else {
                s0 = (inFrames > 0) ? pIn[(inFrames - 1) * channels + c] : 0.0f;
                s1 = s0;
            }

            pOut[i * channels + c] = s0 + frac * (s1 - s0);
        }

        pos += ratio;
    }

    // Save history and update phase for seamless next chunk
    if (inFrames > 0) {
        for (size_t c = 0; c < channels; ++c) {
            lastFrameHistory[c] = pIn[(inFrames - 1) * channels + c];
        }
    }
    if (pos >= inFrames) {
        ioPhase = pos - inFrames;
    } else {
        ioPhase = 0.0;
    }
}

void WasapiRenderClient::MapChannelsFloat(const float* pIn,
                                          size_t inChannels,
                                          float* pOut,
                                          size_t outChannels,
                                          size_t frames) {
    if (inChannels == outChannels) {
        std::memcpy(pOut, pIn, frames * outChannels * sizeof(float));
        return;
    }

    if (inChannels == 2 && outChannels == 1) {
        // Stereo to Mono downmix
        for (size_t i = 0; i < frames; ++i) {
            pOut[i] = 0.5f * (pIn[i * 2] + pIn[i * 2 + 1]);
        }
        return;
    }

    if (inChannels == 1 && outChannels == 2) {
        // Mono to Stereo duplication
        for (size_t i = 0; i < frames; ++i) {
            float s = pIn[i];
            pOut[i * 2] = s;
            pOut[i * 2 + 1] = s;
        }
        return;
    }

    if (inChannels == 2 && outChannels > 2) {
        // Stereo to Multi-channel (5.1/7.1): L -> FL, R -> FR, rest zeroed
        for (size_t i = 0; i < frames; ++i) {
            pOut[i * outChannels] = pIn[i * 2];
            pOut[i * outChannels + 1] = pIn[i * 2 + 1];
            for (size_t c = 2; c < outChannels; ++c) {
                pOut[i * outChannels + c] = 0.0f;
            }
        }
        return;
    }

    if (inChannels > 2 && outChannels == 2) {
        // Multi-channel to Stereo (take Front-Left and Front-Right)
        for (size_t i = 0; i < frames; ++i) {
            pOut[i * 2] = pIn[i * inChannels];
            pOut[i * 2 + 1] = pIn[i * inChannels + 1];
        }
        return;
    }

    // General fallback: copy min channels, zero out remaining
    size_t copyChannels = std::min(inChannels, outChannels);
    for (size_t i = 0; i < frames; ++i) {
        for (size_t c = 0; c < copyChannels; ++c) {
            pOut[i * outChannels + c] = pIn[i * inChannels + c];
        }
        for (size_t c = copyChannels; c < outChannels; ++c) {
            pOut[i * outChannels + c] = 0.0f;
        }
    }
}

void WasapiRenderClient::ConvertFloatToRaw(const float* pSrc,
                                           BYTE* pDest,
                                           size_t frames,
                                           SampleFormatType fmt,
                                           size_t channels) {
    const size_t totalSamples = frames * channels;

    switch (fmt) {
        case SampleFormatType::Float32:
            std::memcpy(pDest, pSrc, totalSamples * sizeof(float));
            break;

        case SampleFormatType::Pcm16: {
            int16_t* p16 = reinterpret_cast<int16_t*>(pDest);
            for (size_t i = 0; i < totalSamples; ++i) {
                float f = std::clamp(pSrc[i], -1.0f, 1.0f);
                p16[i] = static_cast<int16_t>(f * 32767.0f);
            }
            break;
        }

        case SampleFormatType::Pcm24In32: {
            int32_t* p32 = reinterpret_cast<int32_t*>(pDest);
            for (size_t i = 0; i < totalSamples; ++i) {
                float f = std::clamp(pSrc[i], -1.0f, 1.0f);
                p32[i] = static_cast<int32_t>(f * 8388607.0f) << 8;
            }
            break;
        }

        case SampleFormatType::Pcm24Packed: {
            for (size_t i = 0; i < totalSamples; ++i) {
                float f = std::clamp(pSrc[i], -1.0f, 1.0f);
                int32_t val = static_cast<int32_t>(f * 8388607.0f);
                pDest[i * 3]     = static_cast<BYTE>(val & 0xFF);
                pDest[i * 3 + 1] = static_cast<BYTE>((val >> 8) & 0xFF);
                pDest[i * 3 + 2] = static_cast<BYTE>((val >> 16) & 0xFF);
            }
            break;
        }

        case SampleFormatType::Pcm32: {
            int32_t* p32 = reinterpret_cast<int32_t*>(pDest);
            for (size_t i = 0; i < totalSamples; ++i) {
                float f = std::clamp(pSrc[i], -1.0f, 1.0f);
                p32[i] = static_cast<int32_t>(f * 2147483647.0f);
            }
            break;
        }

        default:
            std::memset(pDest, 0, totalSamples * sizeof(float));
            break;
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

    if (m_pRingBuffer) {
        stats.bufferUnderruns = m_pRingBuffer->GetUnderrunCount();
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

} // namespace speakerflow
