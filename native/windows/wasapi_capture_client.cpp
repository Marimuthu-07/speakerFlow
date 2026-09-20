#include "wasapi_capture_client.h"

#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <cmath>
#include <sstream>
#include <iomanip>

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

WasapiCaptureClient::WasapiCaptureClient()
    : m_isCapturing(false),
      m_isEventDriven(false),
      m_sampleRate(48000),
      m_channels(2),
      m_bitsPerSample(32),
      m_formatType(SampleFormatType::Float32),
      m_formatTagName("IEEE_FLOAT"),
      m_framesCaptured(0),
      m_silentFramesCaptured(0),
      m_packetsCaptured(0),
      m_peakLevel(0.0f),
      m_rmsLevel(0.0f),
      m_hStopEvent(NULL),
      m_hAudioEvent(NULL),
      m_pDevice(nullptr),
      m_pAudioClient(nullptr),
      m_pCaptureClient(nullptr),
      m_pMixFormat(nullptr) {
}

WasapiCaptureClient::~WasapiCaptureClient() {
    StopCapture();
}

bool WasapiCaptureClient::IsCapturing() const {
    return m_isCapturing.load(std::memory_order_acquire);
}

bool WasapiCaptureClient::StartCapture(const std::wstring& targetDeviceId, std::string& outError) {
    StopCapture();

    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError.clear();

    m_targetDeviceId = targetDeviceId;
    m_framesCaptured.store(0, std::memory_order_relaxed);
    m_silentFramesCaptured.store(0, std::memory_order_relaxed);
    m_packetsCaptured.store(0, std::memory_order_relaxed);
    m_peakLevel.store(0.0f, std::memory_order_relaxed);
    m_rmsLevel.store(0.0f, std::memory_order_relaxed);

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
        outError = FormatHresult("GetAudioEndpoint", hr);
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
        outError = "Unsupported capture mix format encountered.";
        m_lastError = outError;
        return false;
    }

    // Try event-driven loopback capture first (Windows 10/11)
    m_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    m_hAudioEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    REFERENCE_TIME hnsRequestedDuration = 2000000; // 200ms buffer
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
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

    // Fallback to timer-driven loopback if event-driven flag rejected by driver
    if (!eventDriven) {
        if (m_hAudioEvent) {
            CloseHandle(m_hAudioEvent);
            m_hAudioEvent = NULL;
        }
        m_pAudioClient->Release();
        m_pAudioClient = nullptr;

        hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&m_pAudioClient);
        if (SUCCEEDED(hr) && m_pAudioClient) {
            streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK;
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
        outError = FormatHresult("Initialize(IAudioClient LOOPBACK)", hr);
        m_lastError = outError;
        return false;
    }

    m_isEventDriven.store(eventDriven, std::memory_order_relaxed);

    // Obtain IAudioCaptureClient
    hr = m_pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_pCaptureClient);
    if (FAILED(hr) || !m_pCaptureClient) {
        if (m_hAudioEvent) { CloseHandle(m_hAudioEvent); m_hAudioEvent = NULL; }
        if (m_hStopEvent) { CloseHandle(m_hStopEvent); m_hStopEvent = NULL; }
        if (m_pMixFormat) { CoTaskMemFree(m_pMixFormat); m_pMixFormat = nullptr; }
        if (m_pAudioClient) { m_pAudioClient->Release(); m_pAudioClient = nullptr; }
        if (m_pDevice) { m_pDevice->Release(); m_pDevice = nullptr; }
        outError = FormatHresult("GetService(IAudioCaptureClient)", hr);
        m_lastError = outError;
        return false;
    }

    // Allocate ring buffer (65536 frames ≈ 1.36s at 48kHz)
    m_ringBuffer = std::make_unique<AudioRingBuffer>(65536, m_channels);

    // Launch dedicated capture worker thread
    m_isCapturing.store(true, std::memory_order_release);
    m_thread = std::thread(&WasapiCaptureClient::CaptureThreadProc, this);

    return true;
}

void WasapiCaptureClient::StopCapture() {
    if (!m_isCapturing.exchange(false, std::memory_order_acq_rel)) {
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

    if (m_pCaptureClient) {
        m_pCaptureClient->Release();
        m_pCaptureClient = nullptr;
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
}

void WasapiCaptureClient::CaptureThreadProc() {
    HRESULT hrCom = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    DWORD taskIndex = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!hMmcss) {
        taskIndex = 0;
        hMmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    }

    HRESULT hrStart = m_pAudioClient->Start();
    if (FAILED(hrStart)) {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_lastError = FormatHresult("IAudioClient::Start", hrStart);
        m_isCapturing.store(false, std::memory_order_release);
        if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return;
    }

    UINT32 bufferFrameCount = 0;
    m_pAudioClient->GetBufferSize(&bufferFrameCount);
    size_t stagingCapacity = std::max<size_t>(bufferFrameCount, 4096) * m_channels;
    std::vector<float> stagingBuffer(stagingCapacity, 0.0f);

    const bool isEventDriven = m_isEventDriven.load(std::memory_order_relaxed);
    HANDLE waitHandles[2] = { m_hStopEvent, m_hAudioEvent };
    const DWORD waitCount = isEventDriven ? 2 : 1;

    while (m_isCapturing.load(std::memory_order_relaxed)) {
        DWORD waitRes = WaitForMultipleObjects(waitCount, waitHandles, FALSE, isEventDriven ? 100 : 10);
        if (waitRes == WAIT_OBJECT_0) {
            // Stop event signaled
            break;
        }

        UINT32 packetLength = 0;
        HRESULT hrPacket = m_pCaptureClient->GetNextPacketSize(&packetLength);

        while (SUCCEEDED(hrPacket) && packetLength > 0 && m_isCapturing.load(std::memory_order_relaxed)) {
            BYTE* pData = nullptr;
            UINT32 numFramesRead = 0;
            DWORD flags = 0;
            UINT64 devPos = 0;
            UINT64 qpcPos = 0;

            HRESULT hrBuf = m_pCaptureClient->GetBuffer(&pData, &numFramesRead, &flags, &devPos, &qpcPos);
            if (FAILED(hrBuf)) {
                break;
            }

            if (numFramesRead > 0) {
                const size_t totalSamples = numFramesRead * m_channels;
                if (totalSamples > stagingBuffer.size()) {
                    stagingBuffer.resize(totalSamples);
                }

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(stagingBuffer.begin(), stagingBuffer.begin() + totalSamples, 0.0f);
                    m_silentFramesCaptured.fetch_add(numFramesRead, std::memory_order_relaxed);
                    m_peakLevel.store(0.0f, std::memory_order_relaxed);
                    m_rmsLevel.store(0.0f, std::memory_order_relaxed);
                } else if (pData) {
                    ConvertRawToFloat(pData, stagingBuffer.data(), numFramesRead, m_formatType, m_channels);
                    m_framesCaptured.fetch_add(numFramesRead, std::memory_order_relaxed);
                    UpdateLevels(stagingBuffer.data(), totalSamples);
                }

                if (m_ringBuffer) {
                    m_ringBuffer->Write(stagingBuffer.data(), numFramesRead);
                }

                m_packetsCaptured.fetch_add(1, std::memory_order_relaxed);
            }

            m_pCaptureClient->ReleaseBuffer(numFramesRead);
            hrPacket = m_pCaptureClient->GetNextPacketSize(&packetLength);
        }

        if (FAILED(hrPacket)) {
            if (hrPacket == AUDCLNT_E_DEVICE_INVALIDATED) {
                std::lock_guard<std::mutex> lock(m_errorMutex);
                m_lastError = "WASAPI capture device was invalidated or disconnected.";
                break;
            }
        }
    }

    m_pAudioClient->Stop();

    if (hMmcss) {
        AvRevertMmThreadCharacteristics(hMmcss);
    }
    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
}

void WasapiCaptureClient::ConvertRawToFloat(const BYTE* pSrc,
                                           float* pDest,
                                           size_t frames,
                                           SampleFormatType fmt,
                                           size_t channels) {
    const size_t totalSamples = frames * channels;

    switch (fmt) {
        case SampleFormatType::Float32:
            std::memcpy(pDest, pSrc, totalSamples * sizeof(float));
            break;

        case SampleFormatType::Pcm16: {
            const int16_t* p16 = reinterpret_cast<const int16_t*>(pSrc);
            for (size_t i = 0; i < totalSamples; ++i) {
                pDest[i] = static_cast<float>(p16[i]) / 32768.0f;
            }
            break;
        }

        case SampleFormatType::Pcm24In32: {
            const int32_t* p32 = reinterpret_cast<const int32_t*>(pSrc);
            for (size_t i = 0; i < totalSamples; ++i) {
                pDest[i] = static_cast<float>(p32[i] >> 8) / 8388608.0f;
            }
            break;
        }

        case SampleFormatType::Pcm24Packed: {
            for (size_t i = 0; i < totalSamples; ++i) {
                int32_t val = (static_cast<int32_t>(pSrc[i * 3]) << 8) |
                              (static_cast<int32_t>(pSrc[i * 3 + 1]) << 16) |
                              (static_cast<int32_t>(pSrc[i * 3 + 2]) << 24);
                pDest[i] = static_cast<float>(val >> 8) / 8388608.0f;
            }
            break;
        }

        case SampleFormatType::Pcm32: {
            const int32_t* p32 = reinterpret_cast<const int32_t*>(pSrc);
            for (size_t i = 0; i < totalSamples; ++i) {
                pDest[i] = static_cast<float>(p32[i]) / 2147483648.0f;
            }
            break;
        }

        default:
            std::fill(pDest, pDest + totalSamples, 0.0f);
            break;
    }
}

void WasapiCaptureClient::UpdateLevels(const float* pData, size_t totalSamples) {
    if (!pData || totalSamples == 0) return;

    float maxAmp = 0.0f;
    double sumSq = 0.0;

    for (size_t i = 0; i < totalSamples; ++i) {
        float a = std::fabs(pData[i]);
        if (a > maxAmp) maxAmp = a;
        sumSq += static_cast<double>(a) * static_cast<double>(a);
    }

    float rms = static_cast<float>(std::sqrt(sumSq / static_cast<double>(totalSamples)));
    m_peakLevel.store(maxAmp, std::memory_order_relaxed);
    m_rmsLevel.store(rms, std::memory_order_relaxed);
}

WasapiCaptureStats WasapiCaptureClient::GetStats() const {
    WasapiCaptureStats stats;
    stats.isCapturing = m_isCapturing.load(std::memory_order_acquire);
    stats.isEventDriven = m_isEventDriven.load(std::memory_order_relaxed);
    stats.sampleRate = m_sampleRate;
    stats.channels = m_channels;
    stats.bitsPerSample = m_bitsPerSample;
    stats.formatTag = m_formatTagName;
    stats.deviceId = WideToUtf8(m_targetDeviceId.c_str());
    stats.deviceFriendlyName = m_deviceFriendlyName;
    stats.framesCaptured = m_framesCaptured.load(std::memory_order_relaxed);
    stats.silentFramesCaptured = m_silentFramesCaptured.load(std::memory_order_relaxed);
    stats.packetsCaptured = m_packetsCaptured.load(std::memory_order_relaxed);
    stats.peakLevel = m_peakLevel.load(std::memory_order_relaxed);
    stats.rmsLevel = m_rmsLevel.load(std::memory_order_relaxed);

    if (m_ringBuffer) {
        stats.ringBufferUnderruns = m_ringBuffer->GetUnderrunCount();
        stats.ringBufferOverruns = m_ringBuffer->GetOverrunCount();
        stats.ringBufferFrames = m_ringBuffer->AvailableRead();
        stats.ringBufferCapacity = m_ringBuffer->GetCapacity();
    }

    std::lock_guard<std::mutex> lock(m_errorMutex);
    stats.lastError = m_lastError;
    return stats;
}

} // namespace speakerflow
