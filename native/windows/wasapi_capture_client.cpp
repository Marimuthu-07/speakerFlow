#include "wasapi_capture_client.h"

#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#include <cmath>
#include <sstream>
#include <iomanip>

#include <audioclientactivationparams.h>
#include <psapi.h>

#pragma comment(lib, "mmdevapi.lib")

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

static std::string GetProcessFriendlyName(DWORD pid) {
    if (pid == 0) return "";
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) {
        return "PID " + std::to_string(pid);
    }
    wchar_t exePath[MAX_PATH] = { 0 };
    DWORD size = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
        const wchar_t* pFileName = wcsrchr(exePath, L'\\');
        pFileName = pFileName ? (pFileName + 1) : exePath;
        name = WideToUtf8(pFileName);
    } else {
        name = "PID " + std::to_string(pid);
    }
    CloseHandle(hProcess);
    return name;
}

static bool ParseMixFormat(const WAVEFORMATEX* pMixFormat,
                           SampleFormatType& outType,
                           std::string& outTag) {
    if (!pMixFormat) return false;
    outType = SampleFormatType::Unknown;
    outTag = "Unknown";

    if (pMixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        outType = SampleFormatType::Float32;
        outTag = "IEEE_FLOAT";
        return true;
    } else if (pMixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE* pExt = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(pMixFormat);
        if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            outType = SampleFormatType::Float32;
            outTag = "EXTENSIBLE_IEEE_FLOAT";
            return true;
        } else if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            if (pMixFormat->wBitsPerSample == 16) {
                outType = SampleFormatType::Pcm16;
                outTag = "EXTENSIBLE_PCM_16";
                return true;
            } else if (pMixFormat->wBitsPerSample == 24) {
                outType = (pMixFormat->nBlockAlign == pMixFormat->nChannels * 4)
                                   ? SampleFormatType::Pcm24In32
                                   : SampleFormatType::Pcm24Packed;
                outTag = "EXTENSIBLE_PCM_24";
                return true;
            } else if (pMixFormat->wBitsPerSample == 32) {
                outType = SampleFormatType::Pcm32;
                outTag = "EXTENSIBLE_PCM_32";
                return true;
            }
        }
    } else if (pMixFormat->wFormatTag == WAVE_FORMAT_PCM) {
        if (pMixFormat->wBitsPerSample == 16) {
            outType = SampleFormatType::Pcm16;
            outTag = "PCM_16";
            return true;
        } else if (pMixFormat->wBitsPerSample == 24) {
            outType = (pMixFormat->nBlockAlign == pMixFormat->nChannels * 4)
                               ? SampleFormatType::Pcm24In32
                               : SampleFormatType::Pcm24Packed;
            outTag = "PCM_24";
            return true;
        } else if (pMixFormat->wBitsPerSample == 32) {
            outType = SampleFormatType::Pcm32;
            outTag = "PCM_32";
            return true;
        }
    }
    return false;
}

class ProcessLoopbackActivationHandler : public IActivateAudioInterfaceCompletionHandler {
public:
    ProcessLoopbackActivationHandler()
        : m_refCount(1),
          m_hCompleted(CreateEvent(NULL, TRUE, FALSE, NULL)),
          m_hrResult(E_FAIL),
          m_pAudioClient(nullptr) {}

    ~ProcessLoopbackActivationHandler() {
        if (m_hCompleted) CloseHandle(m_hCompleted);
        if (m_pAudioClient) m_pAudioClient->Release();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler) ||
            riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* pAsyncOp) override {
        if (!pAsyncOp) {
            m_hrResult = E_POINTER;
            SetEvent(m_hCompleted);
            return S_OK;
        }
        HRESULT hrActivate = S_OK;
        IUnknown* pUnk = nullptr;
        HRESULT hr = pAsyncOp->GetActivateResult(&hrActivate, &pUnk);
        if (SUCCEEDED(hr) && SUCCEEDED(hrActivate) && pUnk) {
            hr = pUnk->QueryInterface(IID_PPV_ARGS(&m_pAudioClient));
            pUnk->Release();
            m_hrResult = hr;
        } else {
            m_hrResult = FAILED(hr) ? hr : hrActivate;
        }
        SetEvent(m_hCompleted);
        return S_OK;
    }

    bool WaitForCompletion(DWORD timeoutMs) {
        return WaitForSingleObject(m_hCompleted, timeoutMs) == WAIT_OBJECT_0;
    }

    HRESULT GetResult() const { return m_hrResult; }
    IAudioClient* DetachAudioClient() {
        IAudioClient* pClient = m_pAudioClient;
        m_pAudioClient = nullptr;
        return pClient;
    }

private:
    long m_refCount;
    HANDLE m_hCompleted;
    HRESULT m_hrResult;
    IAudioClient* m_pAudioClient;
};

WasapiCaptureClient::WasapiCaptureClient()
    : m_isCapturing(false),
      m_isEventDriven(false),
      m_captureMode(WasapiCaptureMode::EndpointLoopback),
      m_targetProcessId(0),
      m_hTargetProcess(NULL),
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
    for (size_t i = 0; i < MAX_FANOUT_SLOTS; ++i) {
        m_branchBuffers[i].store(nullptr, std::memory_order_relaxed);
        m_slotInWrite[i].store(false, std::memory_order_relaxed);
    }
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

    m_captureMode = WasapiCaptureMode::EndpointLoopback;
    m_targetProcessId = 0;
    m_targetDeviceId = targetDeviceId;
    m_referenceDeviceId.clear();
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

    if (!ParseMixFormat(m_pMixFormat, m_formatType, m_formatTagName)) {
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

    // Allocate or reset ring buffer (65536 frames ≈ 1.36s at 48kHz)
    if (!m_ringBuffer || m_ringBuffer->GetChannels() != m_channels) {
        m_ringBuffer = std::make_unique<AudioRingBuffer>(65536, m_channels);
    } else {
        m_ringBuffer->Reset();
    }

    // Launch dedicated capture worker thread
    m_isCapturing.store(true, std::memory_order_release);
    m_thread = std::thread(&WasapiCaptureClient::CaptureThreadProc, this);

    return true;
}

bool WasapiCaptureClient::StartProcessCapture(DWORD targetProcessId, const std::wstring& referenceDeviceId, std::string& outError) {
    StopCapture();

    std::lock_guard<std::mutex> lock(m_errorMutex);
    m_lastError.clear();

    if (targetProcessId == 0) {
        outError = "Invalid target process ID (0).";
        m_lastError = outError;
        return false;
    }

    m_captureMode = WasapiCaptureMode::ProcessLoopback;
    m_targetProcessId = targetProcessId;
    m_referenceDeviceId = referenceDeviceId;
    m_targetDeviceId.clear();

    // Open target process handle with SYNCHRONIZE for fast termination detection
    m_hTargetProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, targetProcessId);
    if (!m_hTargetProcess) {
        m_hTargetProcess = OpenProcess(SYNCHRONIZE, FALSE, targetProcessId);
    }
    if (!m_hTargetProcess) {
        DWORD err = GetLastError();
        if (err == ERROR_INVALID_PARAMETER) {
            outError = "Target process does not exist or has already terminated.";
            m_lastError = outError;
            return false;
        }
        // If access denied (e.g. system protected process), proceed without waitable process handle;
        // termination will be caught via WASAPI's packet/device invalidation.
    }

    m_framesCaptured.store(0, std::memory_order_relaxed);
    m_silentFramesCaptured.store(0, std::memory_order_relaxed);
    m_packetsCaptured.store(0, std::memory_order_relaxed);
    m_peakLevel.store(0.0f, std::memory_order_relaxed);
    m_rmsLevel.store(0.0f, std::memory_order_relaxed);

    m_deviceFriendlyName = "App: " + GetProcessFriendlyName(targetProcessId);

    // Acquire reference render format from default or reference endpoint
    IMMDeviceEnumerator* pEnumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                  NULL,
                                  CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) {
        if (m_hTargetProcess) { CloseHandle(m_hTargetProcess); m_hTargetProcess = NULL; }
        outError = FormatHresult("CoCreateInstance(MMDeviceEnumerator)", hr);
        m_lastError = outError;
        return false;
    }

    IMMDevice* pRefDevice = nullptr;
    if (m_referenceDeviceId.empty()) {
        hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pRefDevice);
    } else {
        hr = pEnumerator->GetDevice(m_referenceDeviceId.c_str(), &pRefDevice);
    }
    pEnumerator->Release();

    if (FAILED(hr) || !pRefDevice) {
        if (m_hTargetProcess) { CloseHandle(m_hTargetProcess); m_hTargetProcess = NULL; }
        outError = FormatHresult("GetAudioEndpoint(ReferenceDevice)", hr);
        m_lastError = outError;
        return false;
    }

    IAudioClient* pRefClient = nullptr;
    hr = pRefDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pRefClient);
    pRefDevice->Release();
    if (FAILED(hr) || !pRefClient) {
        if (m_hTargetProcess) { CloseHandle(m_hTargetProcess); m_hTargetProcess = NULL; }
        outError = FormatHresult("Activate(ReferenceClient)", hr);
        m_lastError = outError;
        return false;
    }

    hr = pRefClient->GetMixFormat(&m_pMixFormat);
    pRefClient->Release();
    if (FAILED(hr) || !m_pMixFormat) {
        if (m_hTargetProcess) { CloseHandle(m_hTargetProcess); m_hTargetProcess = NULL; }
        outError = FormatHresult("GetMixFormat(ReferenceClient)", hr);
        m_lastError = outError;
        return false;
    }

    m_sampleRate = m_pMixFormat->nSamplesPerSec;
    m_channels = m_pMixFormat->nChannels;
    m_bitsPerSample = m_pMixFormat->wBitsPerSample;

    if (!ParseMixFormat(m_pMixFormat, m_formatType, m_formatTagName)) {
        CoTaskMemFree(m_pMixFormat);
        m_pMixFormat = nullptr;
        if (m_hTargetProcess) { CloseHandle(m_hTargetProcess); m_hTargetProcess = NULL; }
        outError = "Unsupported reference mix format for process loopback.";
        m_lastError = outError;
        return false;
    }

    // Activate process loopback IAudioClient
    AUDIOCLIENT_ACTIVATION_PARAMS params = {};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = targetProcessId;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activateParams;
    PropVariantInit(&activateParams);
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    ProcessLoopbackActivationHandler* pHandler = new ProcessLoopbackActivationHandler();
    IActivateAudioInterfaceAsyncOperation* pAsyncOp = nullptr;

    hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
        __uuidof(IAudioClient),
        &activateParams,
        pHandler,
        &pAsyncOp
    );

    if (FAILED(hr)) {
        pHandler->Release();
        CoTaskMemFree(m_pMixFormat);
        m_pMixFormat = nullptr;
        if (m_hTargetProcess) { CloseHandle(m_hTargetProcess); m_hTargetProcess = NULL; }
        outError = FormatHresult("ActivateAudioInterfaceAsync(PROCESS_LOOPBACK)", hr);
        m_lastError = outError;
        return false;
    }

    bool completed = pHandler->WaitForCompletion(3000);
    if (!completed) {
        if (pAsyncOp) pAsyncOp->Release();
        pHandler->Release();
        CoTaskMemFree(m_pMixFormat);
        m_pMixFormat = nullptr;
        if (m_hTargetProcess) { CloseHandle(m_hTargetProcess); m_hTargetProcess = NULL; }
        outError = "ActivateAudioInterfaceAsync timed out after 3000ms.";
        m_lastError = outError;
        return false;
    }

    HRESULT actResult = pHandler->GetResult();
    if (FAILED(actResult)) {
        if (pAsyncOp) pAsyncOp->Release();
        pHandler->Release();
        CoTaskMemFree(m_pMixFormat);
        m_pMixFormat = nullptr;
        if (m_hTargetProcess) { CloseHandle(m_hTargetProcess); m_hTargetProcess = NULL; }
        outError = FormatHresult("Process Loopback Activation", actResult);
        m_lastError = outError;
        return false;
    }

    m_pAudioClient = pHandler->DetachAudioClient();
    if (pAsyncOp) pAsyncOp->Release();
    pHandler->Release();

    if (!m_pAudioClient) {
        CoTaskMemFree(m_pMixFormat);
        m_pMixFormat = nullptr;
        if (m_hTargetProcess) { CloseHandle(m_hTargetProcess); m_hTargetProcess = NULL; }
        outError = "Activation completed but IAudioClient interface was null.";
        m_lastError = outError;
        return false;
    }

    // Initialize event-driven capture
    m_hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    m_hAudioEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    REFERENCE_TIME hnsRequestedDuration = 2000000; // 200ms
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

    if (!eventDriven || FAILED(hr)) {
        if (m_hAudioEvent) { CloseHandle(m_hAudioEvent); m_hAudioEvent = NULL; }
        if (m_hStopEvent) { CloseHandle(m_hStopEvent); m_hStopEvent = NULL; }
        if (m_pMixFormat) { CoTaskMemFree(m_pMixFormat); m_pMixFormat = nullptr; }
        if (m_pAudioClient) { m_pAudioClient->Release(); m_pAudioClient = nullptr; }
        if (m_hTargetProcess) { CloseHandle(m_hTargetProcess); m_hTargetProcess = NULL; }
        outError = FormatHresult("Initialize(IAudioClient PROCESS_LOOPBACK)", hr);
        m_lastError = outError;
        return false;
    }

    m_isEventDriven.store(true, std::memory_order_relaxed);

    hr = m_pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_pCaptureClient);
    if (FAILED(hr) || !m_pCaptureClient) {
        if (m_hAudioEvent) { CloseHandle(m_hAudioEvent); m_hAudioEvent = NULL; }
        if (m_hStopEvent) { CloseHandle(m_hStopEvent); m_hStopEvent = NULL; }
        if (m_pMixFormat) { CoTaskMemFree(m_pMixFormat); m_pMixFormat = nullptr; }
        if (m_pAudioClient) { m_pAudioClient->Release(); m_pAudioClient = nullptr; }
        if (m_hTargetProcess) { CloseHandle(m_hTargetProcess); m_hTargetProcess = NULL; }
        outError = FormatHresult("GetService(IAudioCaptureClient)", hr);
        m_lastError = outError;
        return false;
    }

    if (!m_ringBuffer || m_ringBuffer->GetChannels() != m_channels) {
        m_ringBuffer = std::make_unique<AudioRingBuffer>(65536, m_channels);
    } else {
        m_ringBuffer->Reset();
    }

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
    if (m_hTargetProcess) {
        CloseHandle(m_hTargetProcess);
        m_hTargetProcess = NULL;
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

    m_targetProcessId = 0;
    m_captureMode = WasapiCaptureMode::EndpointLoopback;

    ClearBranchBuffers();
}

void WasapiCaptureClient::DetachBranchBufferSync(size_t slot) {
    if (slot >= MAX_FANOUT_SLOTS) return;
    m_branchBuffers[slot].store(nullptr, std::memory_order_release);
    // Yield until any in-flight write to this slot on the capture thread completes
    while (m_slotInWrite[slot].load(std::memory_order_acquire)) {
        std::this_thread::yield();
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
    HANDLE waitHandles[3] = { m_hStopEvent, NULL, NULL };
    DWORD waitCount = 1;
    if (isEventDriven && m_hAudioEvent) {
        waitHandles[waitCount++] = m_hAudioEvent;
    }
    DWORD processWaitIdx = 0;
    if (m_hTargetProcess) {
        processWaitIdx = waitCount;
        waitHandles[waitCount++] = m_hTargetProcess;
    }

    while (m_isCapturing.load(std::memory_order_relaxed)) {
        DWORD waitRes = WaitForMultipleObjects(waitCount, waitHandles, FALSE, isEventDriven ? 100 : 10);
        if (waitRes == WAIT_OBJECT_0) {
            // Stop event signaled
            break;
        }

        if (processWaitIdx > 0 && waitRes == (WAIT_OBJECT_0 + processWaitIdx)) {
            // Target process terminated
            std::lock_guard<std::mutex> lock(m_errorMutex);
            m_lastError = "Target application process terminated.";
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

                // Lock-free fan-out to registered branch buffers with active write protection
                for (size_t slot = 0; slot < MAX_FANOUT_SLOTS; ++slot) {
                    AudioRingBuffer* pBranchBuf = m_branchBuffers[slot].load(std::memory_order_acquire);
                    if (pBranchBuf) {
                        m_slotInWrite[slot].store(true, std::memory_order_release);
                        // Re-verify slot buffer has not been cleared concurrently
                        if (m_branchBuffers[slot].load(std::memory_order_acquire) == pBranchBuf) {
                            pBranchBuf->Write(stagingBuffer.data(), numFramesRead);
                        }
                        m_slotInWrite[slot].store(false, std::memory_order_release);
                    }
                }

                m_packetsCaptured.fetch_add(1, std::memory_order_relaxed);
            }

            m_pCaptureClient->ReleaseBuffer(numFramesRead);
            hrPacket = m_pCaptureClient->GetNextPacketSize(&packetLength);
        }

        if (FAILED(hrPacket)) {
            if (hrPacket == AUDCLNT_E_DEVICE_INVALIDATED || hrPacket == AUDCLNT_E_RESOURCES_INVALIDATED) {
                std::lock_guard<std::mutex> lock(m_errorMutex);
                m_lastError = "WASAPI capture device was invalidated or target process disconnected.";
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
    stats.captureMode = m_captureMode;
    stats.targetProcessId = m_targetProcessId;
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
