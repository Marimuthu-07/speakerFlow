#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <psapi.h>
#include <node_api.h>

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cmath>
#include <sstream>
#include <iomanip>

static bool g_com_initialized_by_us = false;

static std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return "";
    int len = (int)wcslen(wstr);
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr, len, NULL, 0, NULL, NULL);
    if (sizeNeeded <= 0) return "";
    std::string str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, len, &str[0], sizeNeeded, NULL, NULL);
    return str;
}

static std::wstring GetWideStringFromNapi(napi_env env, napi_value val) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, val, NULL, 0, &len) != napi_ok || len == 0) {
        return L"";
    }
    std::vector<char> utf8Buffer(len + 1, 0);
    size_t copied = 0;
    if (napi_get_value_string_utf8(env, val, utf8Buffer.data(), utf8Buffer.size(), &copied) != napi_ok || copied == 0) {
        return L"";
    }
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, utf8Buffer.data(), (int)copied, NULL, 0);
    if (sizeNeeded <= 0) return L"";
    std::wstring wstr(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8Buffer.data(), (int)copied, &wstr[0], sizeNeeded);
    return wstr;
}

static napi_value CreateNapiStringFromWide(napi_env env, const wchar_t* wstr) {
    if (!wstr) {
        napi_value val;
        napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &val);
        return val;
    }
    std::string utf8 = WideToUtf8(wstr);
    napi_value val;
    napi_create_string_utf8(env, utf8.c_str(), utf8.length(), &val);
    return val;
}

static std::wstring GetProcessNameFromPid(DWORD pid) {
    std::wstring name;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        wchar_t exePath[MAX_PATH] = { 0 };
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
            const wchar_t* pFileName = wcsrchr(exePath, L'\\');
            pFileName = pFileName ? (pFileName + 1) : exePath;
            name = pFileName;
            size_t len = name.length();
            if (len > 4 && _wcsicmp(name.c_str() + len - 4, L".exe") == 0) {
                name = name.substr(0, len - 4);
            }
        }
        CloseHandle(hProcess);
    }
    return name;
}

static std::string FormatHresultError(const char* action, HRESULT hr) {
    std::stringstream ss;
    ss << action << " failed with HRESULT 0x"
       << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << (uint32_t)hr;
    return ss.str();
}

// -----------------------------------------------------------------------------
// Lookup Helpers
// -----------------------------------------------------------------------------

static HRESULT FindSessionSimpleVolume(const std::wstring& targetSessionId, ISimpleAudioVolume** ppVolume) {
    if (!ppVolume) return E_POINTER;
    *ppVolume = NULL;

    IMMDeviceEnumerator* pEnumerator = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) {
        return hr;
    }

    IMMDeviceCollection* pCollection = NULL;
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    pEnumerator->Release();
    if (FAILED(hr) || !pCollection) {
        return hr;
    }

    UINT devCount = 0;
    pCollection->GetCount(&devCount);
    bool found = false;

    for (UINT d = 0; d < devCount && !found; d++) {
        IMMDevice* pDevice = NULL;
        if (FAILED(pCollection->Item(d, &pDevice)) || !pDevice) continue;

        LPWSTR pstrDevId = NULL;
        std::wstring deviceId;
        if (SUCCEEDED(pDevice->GetId(&pstrDevId)) && pstrDevId) {
            deviceId = pstrDevId;
            CoTaskMemFree(pstrDevId);
        }

        IAudioSessionManager2* pSessionManager = NULL;
        hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pSessionManager);
        if (SUCCEEDED(hr) && pSessionManager) {
            IAudioSessionEnumerator* pSessionEnum = NULL;
            hr = pSessionManager->GetSessionEnumerator(&pSessionEnum);
            if (SUCCEEDED(hr) && pSessionEnum) {
                int sessionCount = 0;
                pSessionEnum->GetCount(&sessionCount);

                for (int s = 0; s < sessionCount; s++) {
                    IAudioSessionControl* pSessionControl = NULL;
                    if (FAILED(pSessionEnum->GetSession(s, &pSessionControl)) || !pSessionControl) continue;

                    IAudioSessionControl2* pSessionControl2 = NULL;
                    if (SUCCEEDED(pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2)) && pSessionControl2) {
                        if (pSessionControl2->IsSystemSoundsSession() != S_OK) {
                            DWORD pid = 0;
                            pSessionControl2->GetProcessId(&pid);

                            LPWSTR pInstanceId = NULL;
                            std::wstring uniqueSessionId;
                            if (SUCCEEDED(pSessionControl2->GetSessionInstanceIdentifier(&pInstanceId)) && pInstanceId && wcslen(pInstanceId) > 0) {
                                uniqueSessionId = pInstanceId;
                                CoTaskMemFree(pInstanceId);
                            } else {
                                uniqueSessionId = deviceId + L":" + std::to_wstring(pid) + L":" + std::to_wstring(s);
                            }

                            if (uniqueSessionId == targetSessionId) {
                                hr = pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)ppVolume);
                                if (SUCCEEDED(hr) && *ppVolume) {
                                    found = true;
                                }
                            }
                        }
                        pSessionControl2->Release();
                    }
                    pSessionControl->Release();
                    if (found) break;
                }
                pSessionEnum->Release();
            }
            pSessionManager->Release();
        }
        pDevice->Release();
    }

    pCollection->Release();
    return found ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static HRESULT FindEndpointVolume(const std::wstring& targetDeviceId, IAudioEndpointVolume** ppEndpointVolume) {
    if (!ppEndpointVolume) return E_POINTER;
    *ppEndpointVolume = NULL;

    IMMDeviceEnumerator* pEnumerator = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) {
        return hr;
    }

    IMMDevice* pDevice = NULL;
    hr = pEnumerator->GetDevice(targetDeviceId.c_str(), &pDevice);
    if (SUCCEEDED(hr) && pDevice) {
        IMMEndpoint* pEndpoint = NULL;
        if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IMMEndpoint), (void**)&pEndpoint)) && pEndpoint) {
            EDataFlow dataFlow;
            if (SUCCEEDED(pEndpoint->GetDataFlow(&dataFlow)) && dataFlow == eRender) {
                hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)ppEndpointVolume);
                pEndpoint->Release();
                pDevice->Release();
                pEnumerator->Release();
                return hr;
            }
            pEndpoint->Release();
        }
        pDevice->Release();
    }

    IMMDeviceCollection* pCollection = NULL;
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    pEnumerator->Release();
    if (FAILED(hr) || !pCollection) {
        return hr;
    }

    UINT count = 0;
    pCollection->GetCount(&count);
    bool found = false;

    for (UINT i = 0; i < count; i++) {
        IMMDevice* pDev = NULL;
        if (FAILED(pCollection->Item(i, &pDev)) || !pDev) continue;

        LPWSTR pstrId = NULL;
        if (SUCCEEDED(pDev->GetId(&pstrId)) && pstrId) {
            if (targetDeviceId == pstrId) {
                hr = pDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)ppEndpointVolume);
                if (SUCCEEDED(hr) && *ppEndpointVolume) {
                    found = true;
                }
            }
            CoTaskMemFree(pstrId);
        }
        pDev->Release();
        if (found) break;
    }

    pCollection->Release();
    return found ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

// -----------------------------------------------------------------------------
// Real-Time Event Notification Architecture
// -----------------------------------------------------------------------------

struct CoreAudioEventPayload {
    std::string type;
    std::string deviceId;
    std::string sessionId;
    int state = 0;
    int volumePercent = 0;
    bool mute = false;
};

class SessionEventsClient;
class SessionNotificationClient;
class NotificationClient;

struct MonitoredSession {
    std::wstring sessionId;
    IAudioSessionControl* pControl = nullptr;
    SessionEventsClient* pEventsClient = nullptr;
};

struct MonitoredEndpoint {
    std::wstring deviceId;
    IAudioSessionManager2* pSessionManager = nullptr;
    SessionNotificationClient* pSessionNotificationClient = nullptr;
    std::vector<MonitoredSession> sessions;
};

static std::mutex g_monitorMutex;
static std::atomic<bool> g_monitoringActive(false);
static napi_threadsafe_function g_tsfn = nullptr;
static IMMDeviceEnumerator* g_pDeviceEnumerator = nullptr;
static NotificationClient* g_pNotificationClient = nullptr;
static std::vector<MonitoredEndpoint> g_monitoredEndpoints;

static void CallJsCallback(napi_env env, napi_value js_cb, void* context, void* data) {
    if (env == NULL || js_cb == NULL || data == NULL) {
        if (data) delete static_cast<CoreAudioEventPayload*>(data);
        return;
    }
    CoreAudioEventPayload* payload = static_cast<CoreAudioEventPayload*>(data);

    napi_value jsObj;
    if (napi_create_object(env, &jsObj) == napi_ok) {
        napi_value typeVal;
        napi_create_string_utf8(env, payload->type.c_str(), payload->type.length(), &typeVal);
        napi_set_named_property(env, jsObj, "type", typeVal);

        if (!payload->deviceId.empty()) {
            napi_value devVal;
            napi_create_string_utf8(env, payload->deviceId.c_str(), payload->deviceId.length(), &devVal);
            napi_set_named_property(env, jsObj, "deviceId", devVal);
        }

        if (!payload->sessionId.empty()) {
            napi_value sessVal;
            napi_create_string_utf8(env, payload->sessionId.c_str(), payload->sessionId.length(), &sessVal);
            napi_set_named_property(env, jsObj, "sessionId", sessVal);
        }

        if (payload->type == "session-volume-changed") {
            napi_value volVal, muteVal;
            napi_create_int32(env, payload->volumePercent, &volVal);
            napi_get_boolean(env, payload->mute, &muteVal);
            napi_set_named_property(env, jsObj, "volumePercent", volVal);
            napi_set_named_property(env, jsObj, "mute", muteVal);
        }

        if (payload->type == "session-state-changed" || payload->type == "device-state-changed") {
            napi_value stateVal;
            napi_create_int32(env, payload->state, &stateVal);
            napi_set_named_property(env, jsObj, "state", stateVal);
        }

        napi_value undefinedVal;
        napi_get_undefined(env, &undefinedVal);
        napi_value argv[1] = { jsObj };
        napi_call_function(env, undefinedVal, js_cb, 1, argv, NULL);
    }

    delete payload;
}

static void DispatchEvent(const CoreAudioEventPayload& payload) {
    std::lock_guard<std::mutex> lock(g_monitorMutex);
    if (!g_monitoringActive || !g_tsfn) {
        return;
    }
    CoreAudioEventPayload* data = new CoreAudioEventPayload(payload);
    napi_status status = napi_call_threadsafe_function(g_tsfn, data, napi_tsfn_nonblocking);
    if (status != napi_ok) {
        delete data;
    }
}

static void OnEndpointAdded(LPCWSTR pwstrDeviceId);
static void OnEndpointRemoved(LPCWSTR pwstrDeviceId);
static void OnEndpointStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState);
static void OnSessionDisconnectedInternal(const std::wstring& deviceId, const std::wstring& sessionId);
static void RegisterEndpointMonitoring(IMMDevice* pDevice, const std::wstring& deviceId);
static void UnregisterEndpointMonitoring(const std::wstring& deviceId);
static void StopAllMonitoringInternal();

class NotificationClient : public IMMNotificationClient {
private:
    LONG m_cRef;
public:
    NotificationClient() : m_cRef(1) {}
    virtual ~NotificationClient() {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppvObject = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = NULL;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_cRef);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ulRef = InterlockedDecrement(&m_cRef);
        if (ulRef == 0) {
            delete this;
        }
        return ulRef;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) override {
        if (!pwstrDeviceId) return S_OK;

        bool isRender = false;
        if (g_pDeviceEnumerator) {
            IMMDevice* pDev = NULL;
            if (SUCCEEDED(g_pDeviceEnumerator->GetDevice(pwstrDeviceId, &pDev)) && pDev) {
                IMMEndpoint* pEp = NULL;
                if (SUCCEEDED(pDev->QueryInterface(__uuidof(IMMEndpoint), (void**)&pEp)) && pEp) {
                    EDataFlow df;
                    if (SUCCEEDED(pEp->GetDataFlow(&df)) && df == eRender) {
                        isRender = true;
                    }
                    pEp->Release();
                }
                pDev->Release();
            }
        }

        if (isRender) {
            CoreAudioEventPayload payload;
            payload.type = "device-state-changed";
            payload.deviceId = WideToUtf8(pwstrDeviceId);
            payload.state = (int)dwNewState;
            DispatchEvent(payload);

            OnEndpointStateChanged(pwstrDeviceId, dwNewState);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override {
        if (!pwstrDeviceId) return S_OK;

        bool isRender = false;
        if (g_pDeviceEnumerator) {
            IMMDevice* pDev = NULL;
            if (SUCCEEDED(g_pDeviceEnumerator->GetDevice(pwstrDeviceId, &pDev)) && pDev) {
                IMMEndpoint* pEp = NULL;
                if (SUCCEEDED(pDev->QueryInterface(__uuidof(IMMEndpoint), (void**)&pEp)) && pEp) {
                    EDataFlow df;
                    if (SUCCEEDED(pEp->GetDataFlow(&df)) && df == eRender) {
                        isRender = true;
                    }
                    pEp->Release();
                }
                pDev->Release();
            }
        }

        if (isRender) {
            CoreAudioEventPayload payload;
            payload.type = "device-added";
            payload.deviceId = WideToUtf8(pwstrDeviceId);
            DispatchEvent(payload);

            OnEndpointAdded(pwstrDeviceId);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override {
        if (!pwstrDeviceId) return S_OK;

        bool wasMonitored = false;
        {
            std::lock_guard<std::mutex> lock(g_monitorMutex);
            std::wstring devId = pwstrDeviceId;
            for (const auto& ep : g_monitoredEndpoints) {
                if (ep.deviceId == devId) {
                    wasMonitored = true;
                    break;
                }
            }
        }

        if (wasMonitored) {
            CoreAudioEventPayload payload;
            payload.type = "device-removed";
            payload.deviceId = WideToUtf8(pwstrDeviceId);
            DispatchEvent(payload);

            OnEndpointRemoved(pwstrDeviceId);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override {
        if (flow == eRender && role == eConsole) {
            CoreAudioEventPayload payload;
            payload.type = "default-device-changed";
            payload.deviceId = pwstrDefaultDeviceId ? WideToUtf8(pwstrDefaultDeviceId) : "";
            DispatchEvent(payload);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key) override {
        return S_OK;
    }
};

class SessionEventsClient : public IAudioSessionEvents {
private:
    LONG m_cRef;
    std::wstring m_sessionId;
    std::wstring m_deviceId;
public:
    SessionEventsClient(const std::wstring& sessionId, const std::wstring& deviceId)
        : m_cRef(1), m_sessionId(sessionId), m_deviceId(deviceId) {}
    virtual ~SessionEventsClient() {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionEvents)) {
            *ppvObject = static_cast<IAudioSessionEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = NULL;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_cRef);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ulRef = InterlockedDecrement(&m_cRef);
        if (ulRef == 0) {
            delete this;
        }
        return ulRef;
    }

    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR NewDisplayName, LPCGUID EventContext) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR NewIconPath, LPCGUID EventContext) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float NewVolume, BOOL NewMute, LPCGUID EventContext) override {
        CoreAudioEventPayload payload;
        payload.type = "session-volume-changed";
        payload.sessionId = WideToUtf8(m_sessionId.c_str());
        payload.deviceId = WideToUtf8(m_deviceId.c_str());
        payload.volumePercent = (int)std::round(NewVolume * 100.0f);
        payload.mute = (NewMute != FALSE);
        DispatchEvent(payload);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD ChannelCount, float NewChannelVolumeArray[], DWORD ChangedChannel, LPCGUID EventContext) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID NewGroupingParam, LPCGUID EventContext) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState NewState) override {
        CoreAudioEventPayload payload;
        payload.type = "session-state-changed";
        payload.sessionId = WideToUtf8(m_sessionId.c_str());
        payload.deviceId = WideToUtf8(m_deviceId.c_str());
        payload.state = (int)NewState;
        DispatchEvent(payload);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason DisconnectReason) override {
        CoreAudioEventPayload payload;
        payload.type = "session-disconnected";
        payload.sessionId = WideToUtf8(m_sessionId.c_str());
        payload.deviceId = WideToUtf8(m_deviceId.c_str());
        payload.state = (int)DisconnectReason;
        DispatchEvent(payload);

        OnSessionDisconnectedInternal(m_deviceId, m_sessionId);
        return S_OK;
    }
};

class SessionNotificationClient : public IAudioSessionNotification {
private:
    LONG m_cRef;
    std::wstring m_deviceId;
public:
    SessionNotificationClient(const std::wstring& deviceId) : m_cRef(1), m_deviceId(deviceId) {}
    virtual ~SessionNotificationClient() {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
        if (!ppvObject) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionNotification)) {
            *ppvObject = static_cast<IAudioSessionNotification*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = NULL;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_cRef);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG ulRef = InterlockedDecrement(&m_cRef);
        if (ulRef == 0) {
            delete this;
        }
        return ulRef;
    }

    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl *NewSession) override {
        if (!NewSession) return S_OK;

        IAudioSessionControl2* pSessionControl2 = NULL;
        if (SUCCEEDED(NewSession->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2)) && pSessionControl2) {
            if (pSessionControl2->IsSystemSoundsSession() == S_OK) {
                pSessionControl2->Release();
                return S_OK;
            }

            DWORD pid = 0;
            pSessionControl2->GetProcessId(&pid);
            if (pid == 0) {
                pSessionControl2->Release();
                return S_OK;
            }

            LPWSTR pInstanceId = NULL;
            std::wstring sessionId;
            if (SUCCEEDED(pSessionControl2->GetSessionInstanceIdentifier(&pInstanceId)) && pInstanceId && wcslen(pInstanceId) > 0) {
                sessionId = pInstanceId;
                CoTaskMemFree(pInstanceId);
            } else {
                sessionId = m_deviceId + L":" + std::to_wstring(pid);
            }
            pSessionControl2->Release();

            CoreAudioEventPayload payload;
            payload.type = "session-created";
            payload.sessionId = WideToUtf8(sessionId.c_str());
            payload.deviceId = WideToUtf8(m_deviceId.c_str());
            DispatchEvent(payload);

            std::lock_guard<std::mutex> lock(g_monitorMutex);
            if (!g_monitoringActive) return S_OK;

            for (auto& ep : g_monitoredEndpoints) {
                if (ep.deviceId == m_deviceId) {
                    SessionEventsClient* pEvents = new SessionEventsClient(sessionId, m_deviceId);
                    if (SUCCEEDED(NewSession->RegisterAudioSessionNotification(pEvents))) {
                        MonitoredSession sess;
                        sess.sessionId = sessionId;
                        sess.pControl = NewSession;
                        sess.pControl->AddRef();
                        sess.pEventsClient = pEvents;
                        ep.sessions.push_back(sess);
                    } else {
                        pEvents->Release();
                    }
                    break;
                }
            }
        }
        return S_OK;
    }
};

static void RegisterEndpointMonitoring(IMMDevice* pDevice, const std::wstring& deviceId) {
    for (const auto& ep : g_monitoredEndpoints) {
        if (ep.deviceId == deviceId) {
            return;
        }
    }

    IMMEndpoint* pEndpoint = NULL;
    if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IMMEndpoint), (void**)&pEndpoint)) && pEndpoint) {
        EDataFlow dataFlow;
        HRESULT hrFlow = pEndpoint->GetDataFlow(&dataFlow);
        pEndpoint->Release();
        if (FAILED(hrFlow) || dataFlow != eRender) {
            return;
        }
    }

    IAudioSessionManager2* pMgr = NULL;
    HRESULT hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pMgr);
    if (FAILED(hr) || !pMgr) {
        return;
    }

    SessionNotificationClient* pNotif = new SessionNotificationClient(deviceId);
    hr = pMgr->RegisterSessionNotification(pNotif);
    if (FAILED(hr)) {
        pNotif->Release();
        pMgr->Release();
        return;
    }

    MonitoredEndpoint ep;
    ep.deviceId = deviceId;
    ep.pSessionManager = pMgr;
    ep.pSessionNotificationClient = pNotif;

    IAudioSessionEnumerator* pEnum = NULL;
    if (SUCCEEDED(pMgr->GetSessionEnumerator(&pEnum)) && pEnum) {
        int count = 0;
        pEnum->GetCount(&count);
        for (int s = 0; s < count; s++) {
            IAudioSessionControl* pCtrl = NULL;
            if (SUCCEEDED(pEnum->GetSession(s, &pCtrl)) && pCtrl) {
                IAudioSessionControl2* pCtrl2 = NULL;
                if (SUCCEEDED(pCtrl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pCtrl2)) && pCtrl2) {
                    if (pCtrl2->IsSystemSoundsSession() != S_OK) {
                        DWORD pid = 0;
                        pCtrl2->GetProcessId(&pid);
                        if (pid != 0) {
                            LPWSTR pInstId = NULL;
                            std::wstring sessionId;
                            if (SUCCEEDED(pCtrl2->GetSessionInstanceIdentifier(&pInstId)) && pInstId && wcslen(pInstId) > 0) {
                                sessionId = pInstId;
                                CoTaskMemFree(pInstId);
                            } else {
                                sessionId = deviceId + L":" + std::to_wstring(pid) + L":" + std::to_wstring(s);
                            }

                            SessionEventsClient* pEvents = new SessionEventsClient(sessionId, deviceId);
                            if (SUCCEEDED(pCtrl->RegisterAudioSessionNotification(pEvents))) {
                                MonitoredSession sess;
                                sess.sessionId = sessionId;
                                sess.pControl = pCtrl;
                                sess.pControl->AddRef();
                                sess.pEventsClient = pEvents;
                                ep.sessions.push_back(sess);
                            } else {
                                pEvents->Release();
                            }
                        }
                    }
                    pCtrl2->Release();
                }
                pCtrl->Release();
            }
        }
        pEnum->Release();
    }

    g_monitoredEndpoints.push_back(ep);
}

static void UnregisterEndpointMonitoring(const std::wstring& deviceId) {
    for (auto it = g_monitoredEndpoints.begin(); it != g_monitoredEndpoints.end(); ++it) {
        if (it->deviceId == deviceId) {
            for (auto& sess : it->sessions) {
                if (sess.pControl && sess.pEventsClient) {
                    sess.pControl->UnregisterAudioSessionNotification(sess.pEventsClient);
                    sess.pEventsClient->Release();
                    sess.pControl->Release();
                }
            }
            it->sessions.clear();

            if (it->pSessionManager && it->pSessionNotificationClient) {
                it->pSessionManager->UnregisterSessionNotification(it->pSessionNotificationClient);
                it->pSessionNotificationClient->Release();
                it->pSessionManager->Release();
            }
            g_monitoredEndpoints.erase(it);
            break;
        }
    }
}

static void OnEndpointAdded(LPCWSTR pwstrDeviceId) {
    if (!pwstrDeviceId) return;
    std::wstring devId = pwstrDeviceId;

    std::lock_guard<std::mutex> lock(g_monitorMutex);
    if (!g_monitoringActive || !g_pDeviceEnumerator) return;

    IMMDevice* pDevice = NULL;
    if (SUCCEEDED(g_pDeviceEnumerator->GetDevice(pwstrDeviceId, &pDevice)) && pDevice) {
        DWORD state = 0;
        if (SUCCEEDED(pDevice->GetState(&state)) && (state == DEVICE_STATE_ACTIVE)) {
            IMMEndpoint* pEndpoint = NULL;
            if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IMMEndpoint), (void**)&pEndpoint)) && pEndpoint) {
                EDataFlow dataFlow;
                if (SUCCEEDED(pEndpoint->GetDataFlow(&dataFlow)) && dataFlow == eRender) {
                    RegisterEndpointMonitoring(pDevice, devId);
                }
                pEndpoint->Release();
            }
        }
        pDevice->Release();
    }
}

static void OnEndpointStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState) {
    if (!pwstrDeviceId) return;
    std::wstring devId = pwstrDeviceId;

    std::lock_guard<std::mutex> lock(g_monitorMutex);
    if (!g_monitoringActive || !g_pDeviceEnumerator) return;

    if (dwNewState == DEVICE_STATE_ACTIVE) {
        IMMDevice* pDevice = NULL;
        if (SUCCEEDED(g_pDeviceEnumerator->GetDevice(pwstrDeviceId, &pDevice)) && pDevice) {
            IMMEndpoint* pEndpoint = NULL;
            if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IMMEndpoint), (void**)&pEndpoint)) && pEndpoint) {
                EDataFlow dataFlow;
                if (SUCCEEDED(pEndpoint->GetDataFlow(&dataFlow)) && dataFlow == eRender) {
                    RegisterEndpointMonitoring(pDevice, devId);
                }
                pEndpoint->Release();
            }
            pDevice->Release();
        }
    } else {
        UnregisterEndpointMonitoring(devId);
    }
}

static void OnEndpointRemoved(LPCWSTR pwstrDeviceId) {
    if (!pwstrDeviceId) return;
    std::wstring devId = pwstrDeviceId;

    std::lock_guard<std::mutex> lock(g_monitorMutex);
    if (!g_monitoringActive) return;

    UnregisterEndpointMonitoring(devId);
}

static void OnSessionDisconnectedInternal(const std::wstring& deviceId, const std::wstring& sessionId) {
    std::lock_guard<std::mutex> lock(g_monitorMutex);
    if (!g_monitoringActive) return;

    for (auto& ep : g_monitoredEndpoints) {
        if (ep.deviceId == deviceId) {
            for (auto it = ep.sessions.begin(); it != ep.sessions.end(); ++it) {
                if (it->sessionId == sessionId) {
                    if (it->pControl && it->pEventsClient) {
                        it->pControl->UnregisterAudioSessionNotification(it->pEventsClient);
                        it->pEventsClient->Release();
                        it->pControl->Release();
                    }
                    ep.sessions.erase(it);
                    break;
                }
            }
            break;
        }
    }
}

static void StopAllMonitoringInternal() {
    std::lock_guard<std::mutex> lock(g_monitorMutex);
    g_monitoringActive = false;

    if (g_pDeviceEnumerator && g_pNotificationClient) {
        g_pDeviceEnumerator->UnregisterEndpointNotificationCallback(g_pNotificationClient);
        g_pNotificationClient->Release();
        g_pNotificationClient = nullptr;
    }
    if (g_pDeviceEnumerator) {
        g_pDeviceEnumerator->Release();
        g_pDeviceEnumerator = nullptr;
    }

    for (auto& ep : g_monitoredEndpoints) {
        for (auto& sess : ep.sessions) {
            if (sess.pControl && sess.pEventsClient) {
                sess.pControl->UnregisterAudioSessionNotification(sess.pEventsClient);
                sess.pEventsClient->Release();
                sess.pControl->Release();
            }
        }
        ep.sessions.clear();

        if (ep.pSessionManager && ep.pSessionNotificationClient) {
            ep.pSessionManager->UnregisterSessionNotification(ep.pSessionNotificationClient);
            ep.pSessionNotificationClient->Release();
            ep.pSessionManager->Release();
        }
    }
    g_monitoredEndpoints.clear();

    if (g_tsfn) {
        napi_release_threadsafe_function(g_tsfn, napi_tsfn_abort);
        g_tsfn = nullptr;
    }
}

static bool StartMonitoringInternal(napi_env env, napi_value jsCallback) {
    StopAllMonitoringInternal();

    std::lock_guard<std::mutex> lock(g_monitorMutex);

    napi_value resourceName;
    napi_create_string_utf8(env, "CoreAudioMonitoringCallback", NAPI_AUTO_LENGTH, &resourceName);

    napi_status status = napi_create_threadsafe_function(
        env,
        jsCallback,
        NULL,
        resourceName,
        0,
        1,
        NULL,
        NULL,
        NULL,
        CallJsCallback,
        &g_tsfn
    );

    if (status != napi_ok || !g_tsfn) {
        return false;
    }

    g_monitoringActive = true;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&g_pDeviceEnumerator);
    if (FAILED(hr) || !g_pDeviceEnumerator) {
        g_monitoringActive = false;
        napi_release_threadsafe_function(g_tsfn, napi_tsfn_abort);
        g_tsfn = nullptr;
        return false;
    }

    g_pNotificationClient = new NotificationClient();
    hr = g_pDeviceEnumerator->RegisterEndpointNotificationCallback(g_pNotificationClient);
    if (FAILED(hr)) {
        g_pNotificationClient->Release();
        g_pNotificationClient = nullptr;
        g_pDeviceEnumerator->Release();
        g_pDeviceEnumerator = nullptr;
        g_monitoringActive = false;
        napi_release_threadsafe_function(g_tsfn, napi_tsfn_abort);
        g_tsfn = nullptr;
        return false;
    }

    IMMDeviceCollection* pCollection = NULL;
    hr = g_pDeviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (SUCCEEDED(hr) && pCollection) {
        UINT count = 0;
        pCollection->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* pDevice = NULL;
            if (SUCCEEDED(pCollection->Item(i, &pDevice)) && pDevice) {
                LPWSTR pstrId = NULL;
                if (SUCCEEDED(pDevice->GetId(&pstrId)) && pstrId) {
                    std::wstring devId = pstrId;
                    CoTaskMemFree(pstrId);
                    RegisterEndpointMonitoring(pDevice, devId);
                }
                pDevice->Release();
            }
        }
        pCollection->Release();
    }

    return true;
}

// -----------------------------------------------------------------------------
// N-API Methods
// -----------------------------------------------------------------------------

static napi_value Method_Init(napi_env env, napi_callback_info info) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        g_com_initialized_by_us = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        g_com_initialized_by_us = false;
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value Method_Destroy(napi_env env, napi_callback_info info) {
    StopAllMonitoringInternal();

    if (g_com_initialized_by_us) {
        CoUninitialize();
        g_com_initialized_by_us = false;
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value Method_GetDefaultOutput(napi_env env, napi_callback_info info) {
    IMMDeviceEnumerator* pEnumerator = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) {
        napi_value nullVal;
        napi_get_null(env, &nullVal);
        return nullVal;
    }

    IMMDevice* pDefaultDevice = NULL;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDefaultDevice);
    pEnumerator->Release();

    if (FAILED(hr) || !pDefaultDevice) {
        napi_value nullVal;
        napi_get_null(env, &nullVal);
        return nullVal;
    }

    LPWSTR pstrId = NULL;
    hr = pDefaultDevice->GetId(&pstrId);
    pDefaultDevice->Release();

    if (FAILED(hr) || !pstrId) {
        napi_value nullVal;
        napi_get_null(env, &nullVal);
        return nullVal;
    }

    napi_value result = CreateNapiStringFromWide(env, pstrId);
    CoTaskMemFree(pstrId);
    return result;
}

static napi_value Method_ListOutputDevices(napi_env env, napi_callback_info info) {
    IMMDeviceEnumerator* pEnumerator = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) {
        napi_throw_error(env, NULL, "Failed to create MMDeviceEnumerator.");
        return NULL;
    }

    std::wstring defaultDeviceId;
    IMMDevice* pDefaultDevice = NULL;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDefaultDevice);
    if (SUCCEEDED(hr) && pDefaultDevice) {
        LPWSTR pDefId = NULL;
        if (SUCCEEDED(pDefaultDevice->GetId(&pDefId)) && pDefId) {
            defaultDeviceId = pDefId;
            CoTaskMemFree(pDefId);
        }
        pDefaultDevice->Release();
    }

    IMMDeviceCollection* pCollection = NULL;
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    pEnumerator->Release();

    if (FAILED(hr) || !pCollection) {
        napi_value emptyArr;
        napi_create_array_with_length(env, 0, &emptyArr);
        return emptyArr;
    }

    UINT count = 0;
    pCollection->GetCount(&count);

    napi_value jsArray;
    napi_create_array_with_length(env, count, &jsArray);

    for (UINT i = 0; i < count; i++) {
        IMMDevice* pDevice = NULL;
        if (FAILED(pCollection->Item(i, &pDevice)) || !pDevice) {
            continue;
        }

        LPWSTR pstrId = NULL;
        std::wstring deviceId;
        if (SUCCEEDED(pDevice->GetId(&pstrId)) && pstrId) {
            deviceId = pstrId;
            CoTaskMemFree(pstrId);
        }

        std::wstring friendlyName = deviceId;
        IPropertyStore* pProps = NULL;
        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
                if (varName.vt == VT_LPWSTR && varName.pwszVal && wcslen(varName.pwszVal) > 0) {
                    friendlyName = varName.pwszVal;
                }
            }
            PropVariantClear(&varName);
            pProps->Release();
        }

        bool isDefault = (!defaultDeviceId.empty() && defaultDeviceId == deviceId);

        napi_value jsDevice;
        napi_create_object(env, &jsDevice);

        napi_value idVal = CreateNapiStringFromWide(env, deviceId.c_str());
        napi_set_named_property(env, jsDevice, "id", idVal);

        napi_value nameVal = CreateNapiStringFromWide(env, friendlyName.c_str());
        napi_set_named_property(env, jsDevice, "name", nameVal);

        napi_value isDefaultVal;
        napi_get_boolean(env, isDefault, &isDefaultVal);
        napi_set_named_property(env, jsDevice, "isDefault", isDefaultVal);

        napi_set_element(env, jsArray, i, jsDevice);

        pDevice->Release();
    }

    pCollection->Release();
    return jsArray;
}

static napi_value Method_ListApplicationStreams(napi_env env, napi_callback_info info) {
    IMMDeviceEnumerator* pEnumerator = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) {
        napi_throw_error(env, NULL, "Failed to create MMDeviceEnumerator.");
        return NULL;
    }

    IMMDeviceCollection* pCollection = NULL;
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    pEnumerator->Release();

    if (FAILED(hr) || !pCollection) {
        napi_value emptyArr;
        napi_create_array_with_length(env, 0, &emptyArr);
        return emptyArr;
    }

    UINT devCount = 0;
    pCollection->GetCount(&devCount);

    napi_value jsArray;
    napi_create_array(env, &jsArray);
    uint32_t streamIndex = 0;

    for (UINT d = 0; d < devCount; d++) {
        IMMDevice* pDevice = NULL;
        if (FAILED(pCollection->Item(d, &pDevice)) || !pDevice) {
            continue;
        }

        LPWSTR pstrDevId = NULL;
        std::wstring deviceId;
        if (SUCCEEDED(pDevice->GetId(&pstrDevId)) && pstrDevId) {
            deviceId = pstrDevId;
            CoTaskMemFree(pstrDevId);
        }

        std::wstring devFriendlyName = deviceId;
        IPropertyStore* pProps = NULL;
        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
            PROPVARIANT varDevName;
            PropVariantInit(&varDevName);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varDevName))) {
                if (varDevName.vt == VT_LPWSTR && varDevName.pwszVal && wcslen(varDevName.pwszVal) > 0) {
                    devFriendlyName = varDevName.pwszVal;
                }
            }
            PropVariantClear(&varDevName);
            pProps->Release();
        }

        IAudioSessionManager2* pSessionManager = NULL;
        hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pSessionManager);
        if (SUCCEEDED(hr) && pSessionManager) {
            IAudioSessionEnumerator* pSessionEnum = NULL;
            hr = pSessionManager->GetSessionEnumerator(&pSessionEnum);
            if (SUCCEEDED(hr) && pSessionEnum) {
                int sessionCount = 0;
                pSessionEnum->GetCount(&sessionCount);

                for (int s = 0; s < sessionCount; s++) {
                    IAudioSessionControl* pSessionControl = NULL;
                    if (FAILED(pSessionEnum->GetSession(s, &pSessionControl)) || !pSessionControl) {
                        continue;
                    }

                    IAudioSessionControl2* pSessionControl2 = NULL;
                    if (SUCCEEDED(pSessionControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pSessionControl2)) && pSessionControl2) {
                        if (pSessionControl2->IsSystemSoundsSession() == S_OK) {
                            pSessionControl2->Release();
                            pSessionControl->Release();
                            continue;
                        }

                        DWORD pid = 0;
                        pSessionControl2->GetProcessId(&pid);
                        if (pid == 0) {
                            pSessionControl2->Release();
                            pSessionControl->Release();
                            continue;
                        }

                        LPWSTR pInstanceId = NULL;
                        std::wstring uniqueSessionId;
                        if (SUCCEEDED(pSessionControl2->GetSessionInstanceIdentifier(&pInstanceId)) && pInstanceId && wcslen(pInstanceId) > 0) {
                            uniqueSessionId = pInstanceId;
                            CoTaskMemFree(pInstanceId);
                        } else {
                            uniqueSessionId = deviceId + L":" + std::to_wstring(pid) + L":" + std::to_wstring(s);
                        }

                        AudioSessionState state;
                        bool isActive = false;
                        if (SUCCEEDED(pSessionControl->GetState(&state))) {
                            isActive = (state == AudioSessionStateActive);
                        }

                        ISimpleAudioVolume* pVolume = NULL;
                        float volLevel = 1.0f;
                        BOOL isMuted = FALSE;
                        if (SUCCEEDED(pSessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&pVolume)) && pVolume) {
                            pVolume->GetMasterVolume(&volLevel);
                            pVolume->GetMute(&isMuted);
                            pVolume->Release();
                        }

                        int volumePercent = (int)std::round(volLevel * 100.0f);

                        LPWSTR pDispName = NULL;
                        std::wstring appName;
                        if (SUCCEEDED(pSessionControl2->GetDisplayName(&pDispName)) && pDispName && wcslen(pDispName) > 0) {
                            appName = pDispName;
                            CoTaskMemFree(pDispName);
                        } else {
                            if (pDispName) CoTaskMemFree(pDispName);
                            pDispName = NULL;
                            if (SUCCEEDED(pSessionControl->GetDisplayName(&pDispName)) && pDispName && wcslen(pDispName) > 0) {
                                appName = pDispName;
                                CoTaskMemFree(pDispName);
                            } else {
                                if (pDispName) CoTaskMemFree(pDispName);
                                appName = GetProcessNameFromPid(pid);
                                if (appName.empty()) {
                                    appName = L"Process " + std::to_wstring(pid);
                                }
                            }
                        }

                        napi_value jsStream;
                        napi_create_object(env, &jsStream);

                        napi_value idVal = CreateNapiStringFromWide(env, uniqueSessionId.c_str());
                        napi_set_named_property(env, jsStream, "id", idVal);

                        napi_value pidVal;
                        napi_create_uint32(env, pid, &pidVal);
                        napi_set_named_property(env, jsStream, "processId", pidVal);

                        napi_value nameVal = CreateNapiStringFromWide(env, appName.c_str());
                        napi_set_named_property(env, jsStream, "name", nameVal);

                        napi_value isActiveVal;
                        napi_get_boolean(env, isActive, &isActiveVal);
                        napi_set_named_property(env, jsStream, "isActive", isActiveVal);

                        napi_value volVal;
                        napi_create_int32(env, volumePercent, &volVal);
                        napi_set_named_property(env, jsStream, "volumePercent", volVal);

                        napi_value muteVal;
                        napi_get_boolean(env, (bool)isMuted, &muteVal);
                        napi_set_named_property(env, jsStream, "mute", muteVal);

                        napi_value sinkIdVal = CreateNapiStringFromWide(env, deviceId.c_str());
                        napi_set_named_property(env, jsStream, "currentSinkId", sinkIdVal);

                        napi_value sinkNameVal = CreateNapiStringFromWide(env, devFriendlyName.c_str());
                        napi_set_named_property(env, jsStream, "currentSinkName", sinkNameVal);

                        napi_set_element(env, jsArray, streamIndex++, jsStream);

                        pSessionControl2->Release();
                    }
                    pSessionControl->Release();
                }
                pSessionEnum->Release();
            }
            pSessionManager->Release();
        }
        pDevice->Release();
    }

    pCollection->Release();
    return jsArray;
}

static napi_value Method_SetSessionVolume(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 2) {
        napi_throw_type_error(env, NULL, "setSessionVolume requires 2 arguments: (sessionId, volumePercent)");
        return NULL;
    }

    std::wstring sessionId = GetWideStringFromNapi(env, args[0]);
    if (sessionId.empty()) {
        napi_throw_type_error(env, NULL, "sessionId must be a non-empty string");
        return NULL;
    }

    double vol = 100.0;
    if (napi_get_value_double(env, args[1], &vol) != napi_ok) {
        napi_throw_type_error(env, NULL, "volumePercent must be a number");
        return NULL;
    }

    if (vol < 0.0) vol = 0.0;
    if (vol > 100.0) vol = 100.0;
    float scalar = (float)(vol / 100.0);

    ISimpleAudioVolume* pVolume = NULL;
    HRESULT hr = FindSessionSimpleVolume(sessionId, &pVolume);
    if (FAILED(hr) || !pVolume) {
        std::string err = FormatHresultError("FindSessionSimpleVolume", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    hr = pVolume->SetMasterVolume(scalar, NULL);
    pVolume->Release();

    if (FAILED(hr)) {
        std::string err = FormatHresultError("ISimpleAudioVolume::SetMasterVolume", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value Method_SetSessionMute(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 2) {
        napi_throw_type_error(env, NULL, "setSessionMute requires 2 arguments: (sessionId, muted)");
        return NULL;
    }

    std::wstring sessionId = GetWideStringFromNapi(env, args[0]);
    if (sessionId.empty()) {
        napi_throw_type_error(env, NULL, "sessionId must be a non-empty string");
        return NULL;
    }

    bool muted = false;
    if (napi_get_value_bool(env, args[1], &muted) != napi_ok) {
        napi_throw_type_error(env, NULL, "muted must be a boolean");
        return NULL;
    }

    ISimpleAudioVolume* pVolume = NULL;
    HRESULT hr = FindSessionSimpleVolume(sessionId, &pVolume);
    if (FAILED(hr) || !pVolume) {
        std::string err = FormatHresultError("FindSessionSimpleVolume", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    hr = pVolume->SetMute(muted ? TRUE : FALSE, NULL);
    pVolume->Release();

    if (FAILED(hr)) {
        std::string err = FormatHresultError("ISimpleAudioVolume::SetMute", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value Method_SetEndpointVolume(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 2) {
        napi_throw_type_error(env, NULL, "setEndpointVolume requires 2 arguments: (deviceId, volumePercent)");
        return NULL;
    }

    std::wstring deviceId = GetWideStringFromNapi(env, args[0]);
    if (deviceId.empty()) {
        napi_throw_type_error(env, NULL, "deviceId must be a non-empty string");
        return NULL;
    }

    double vol = 100.0;
    if (napi_get_value_double(env, args[1], &vol) != napi_ok) {
        napi_throw_type_error(env, NULL, "volumePercent must be a number");
        return NULL;
    }

    if (vol < 0.0) vol = 0.0;
    if (vol > 100.0) vol = 100.0;
    float scalar = (float)(vol / 100.0);

    IAudioEndpointVolume* pEndpointVolume = NULL;
    HRESULT hr = FindEndpointVolume(deviceId, &pEndpointVolume);
    if (FAILED(hr) || !pEndpointVolume) {
        std::string err = FormatHresultError("FindEndpointVolume", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    hr = pEndpointVolume->SetMasterVolumeLevelScalar(scalar, NULL);
    pEndpointVolume->Release();

    if (FAILED(hr)) {
        std::string err = FormatHresultError("IAudioEndpointVolume::SetMasterVolumeLevelScalar", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value Method_SetEndpointMute(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 2) {
        napi_throw_type_error(env, NULL, "setEndpointMute requires 2 arguments: (deviceId, muted)");
        return NULL;
    }

    std::wstring deviceId = GetWideStringFromNapi(env, args[0]);
    if (deviceId.empty()) {
        napi_throw_type_error(env, NULL, "deviceId must be a non-empty string");
        return NULL;
    }

    bool muted = false;
    if (napi_get_value_bool(env, args[1], &muted) != napi_ok) {
        napi_throw_type_error(env, NULL, "muted must be a boolean");
        return NULL;
    }

    IAudioEndpointVolume* pEndpointVolume = NULL;
    HRESULT hr = FindEndpointVolume(deviceId, &pEndpointVolume);
    if (FAILED(hr) || !pEndpointVolume) {
        std::string err = FormatHresultError("FindEndpointVolume", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    hr = pEndpointVolume->SetMute(muted ? TRUE : FALSE, NULL);
    pEndpointVolume->Release();

    if (FAILED(hr)) {
        std::string err = FormatHresultError("IAudioEndpointVolume::SetMute", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value Method_StartMonitoring(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 1) {
        napi_throw_type_error(env, NULL, "startMonitoring requires 1 callback argument");
        return NULL;
    }

    napi_valuetype type;
    napi_typeof(env, args[0], &type);
    if (type != napi_function) {
        napi_throw_type_error(env, NULL, "startMonitoring argument must be a function");
        return NULL;
    }

    bool success = StartMonitoringInternal(env, args[0]);
    napi_value result;
    napi_get_boolean(env, success, &result);
    return result;
}

static napi_value Method_StopMonitoring(napi_env env, napi_callback_info info) {
    StopAllMonitoringInternal();
    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

NAPI_MODULE_INIT() {
    napi_value fn;

    napi_create_function(env, NULL, 0, Method_Init, NULL, &fn);
    napi_set_named_property(env, exports, "init", fn);

    napi_create_function(env, NULL, 0, Method_ListOutputDevices, NULL, &fn);
    napi_set_named_property(env, exports, "listOutputDevices", fn);

    napi_create_function(env, NULL, 0, Method_ListApplicationStreams, NULL, &fn);
    napi_set_named_property(env, exports, "listApplicationStreams", fn);

    napi_create_function(env, NULL, 0, Method_GetDefaultOutput, NULL, &fn);
    napi_set_named_property(env, exports, "getDefaultOutput", fn);

    napi_create_function(env, NULL, 0, Method_SetSessionVolume, NULL, &fn);
    napi_set_named_property(env, exports, "setSessionVolume", fn);

    napi_create_function(env, NULL, 0, Method_SetSessionMute, NULL, &fn);
    napi_set_named_property(env, exports, "setSessionMute", fn);

    napi_create_function(env, NULL, 0, Method_SetEndpointVolume, NULL, &fn);
    napi_set_named_property(env, exports, "setEndpointVolume", fn);

    napi_create_function(env, NULL, 0, Method_SetEndpointMute, NULL, &fn);
    napi_set_named_property(env, exports, "setEndpointMute", fn);

    napi_create_function(env, NULL, 0, Method_StartMonitoring, NULL, &fn);
    napi_set_named_property(env, exports, "startMonitoring", fn);

    napi_create_function(env, NULL, 0, Method_StopMonitoring, NULL, &fn);
    napi_set_named_property(env, exports, "stopMonitoring", fn);

    napi_create_function(env, NULL, 0, Method_Destroy, NULL, &fn);
    napi_set_named_property(env, exports, "destroy", fn);

    return exports;
}
