#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <unknwn.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <psapi.h>
#include <node_api.h>
#include "wasapi_capture_client.h"
#include "wasapi_render_client.h"
#include "wasapi_fanout_engine.h"

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <memory>

static bool g_com_initialized_by_us = false;
static std::unique_ptr<speakerflow::WasapiCaptureClient> g_captureClient;
static std::mutex g_captureMutex;
static std::unique_ptr<speakerflow::WasapiRenderClient> g_renderClient;
static std::mutex g_renderMutex;
static std::unique_ptr<speakerflow::WasapiFanOutEngine> g_fanoutEngine;
static std::mutex g_fanoutMutex;

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
// COM Apartment & WinRT AudioPolicyConfig Helpers
// -----------------------------------------------------------------------------

class ScopedCoInitialize {
public:
    ScopedCoInitialize() {
        m_hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    }
    ~ScopedCoInitialize() {
        if (SUCCEEDED(m_hr)) {
            CoUninitialize();
        }
    }
    bool isModeChanged() const { return m_hr == RPC_E_CHANGED_MODE; }
private:
    HRESULT m_hr;
};

typedef HRESULT (WINAPI *pfn_WindowsCreateStringReference)(
    PCWSTR sourceString,
    UINT32 length,
    void* hstringHeader,
    void** string
);

typedef HRESULT (WINAPI *pfn_WindowsCreateString)(
    PCWSTR sourceString,
    UINT32 length,
    void** string
);

typedef HRESULT (WINAPI *pfn_WindowsDeleteString)(
    void* string
);

typedef PCWSTR (WINAPI *pfn_WindowsGetStringRawBuffer)(
    void* string,
    UINT32* length
);

typedef HRESULT (WINAPI *pfn_RoGetActivationFactory)(
    void* activatableClassId,
    REFIID iid,
    void** factory
);

// Method 25: SetPersistedDefaultAudioEndpoint
typedef HRESULT (STDMETHODCALLTYPE *SetPersistedDefaultAudioEndpointFunc)(
    void* thisPtr,
    DWORD processId,
    EDataFlow flow,
    ERole role,
    void* hstringDeviceId
);

// Method 26: GetPersistedDefaultAudioEndpoint
typedef HRESULT (STDMETHODCALLTYPE *GetPersistedDefaultAudioEndpointFunc)(
    void* thisPtr,
    DWORD processId,
    EDataFlow flow,
    ERole role,
    void** phstringDeviceId
);

struct WinRtApi {
    pfn_WindowsCreateString WindowsCreateString = nullptr;
    pfn_WindowsDeleteString WindowsDeleteString = nullptr;
    pfn_WindowsGetStringRawBuffer WindowsGetStringRawBuffer = nullptr;
    pfn_WindowsCreateStringReference WindowsCreateStringReference = nullptr;
    pfn_RoGetActivationFactory RoGetActivationFactory = nullptr;
    bool loaded = false;

    static WinRtApi& Instance() {
        static WinRtApi s_instance;
        return s_instance;
    }

    WinRtApi() {
        HMODULE hComBase = LoadLibraryW(L"combase.dll");
        if (hComBase) {
            WindowsCreateString = (pfn_WindowsCreateString)GetProcAddress(hComBase, "WindowsCreateString");
            WindowsDeleteString = (pfn_WindowsDeleteString)GetProcAddress(hComBase, "WindowsDeleteString");
            WindowsGetStringRawBuffer = (pfn_WindowsGetStringRawBuffer)GetProcAddress(hComBase, "WindowsGetStringRawBuffer");
            WindowsCreateStringReference = (pfn_WindowsCreateStringReference)GetProcAddress(hComBase, "WindowsCreateStringReference");
            RoGetActivationFactory = (pfn_RoGetActivationFactory)GetProcAddress(hComBase, "RoGetActivationFactory");
            loaded = (WindowsCreateString && WindowsDeleteString && WindowsGetStringRawBuffer &&
                      WindowsCreateStringReference && RoGetActivationFactory);
        }
    }
};

static HRESULT GetAudioPolicyConfig(void** ppPolicyConfig) {
    if (!ppPolicyConfig) return E_POINTER;
    *ppPolicyConfig = NULL;

    auto& api = WinRtApi::Instance();
    if (!api.loaded) {
        return E_FAIL;
    }

    LPCWSTR className = L"Windows.Media.Internal.AudioPolicyConfig";
    char header[64] = {0};
    void* hClassString = nullptr;
    HRESULT hr = api.WindowsCreateStringReference(className, (UINT32)wcslen(className), header, &hClassString);
    if (FAILED(hr)) return hr;

    // Windows 11 and Windows 10 >= 21390 IID
    const GUID iidWin11 = {0xab3d4648, 0xe242, 0x459f, {0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24}};
    hr = api.RoGetActivationFactory(hClassString, iidWin11, ppPolicyConfig);
    if (SUCCEEDED(hr) && *ppPolicyConfig) {
        return S_OK;
    }

    // Windows 10 fallback IID (< 21390)
    const GUID iidWin10 = {0x2a59116d, 0x6c4f, 0x45e0, {0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58}};
    hr = api.RoGetActivationFactory(hClassString, iidWin10, ppPolicyConfig);
    return hr;
}

static std::wstring ExtractEndpointIdFromPath(const std::wstring& path) {
    if (path.rfind(L"{0.0.0.", 0) == 0 && path.find(L"#") == std::wstring::npos) {
        return path;
    }
    size_t start = path.find(L"{0.0.0.");
    if (start == std::wstring::npos) return L"";
    size_t secondBrace = path.find(L"}.{", start);
    if (secondBrace == std::wstring::npos) return L"";
    size_t end = path.find(L"}", secondBrace + 3);
    if (end == std::wstring::npos) return L"";
    return path.substr(start, end - start + 1);
}

static std::wstring GetFullDevicePath(IMMDevice* pDevice, const std::wstring& endpointId) {
    if (pDevice) {
        IPropertyStore* pStore = NULL;
        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pStore)) && pStore) {
            PROPERTYKEY keyDevicePath = { {0x9c119480, 0xddc2, 0x4954, {0xa1, 0x50, 0x5b, 0xd2, 0x40, 0xd4, 0x54, 0xad}}, 1 };
            PROPVARIANT pvPath;
            PropVariantInit(&pvPath);
            if (SUCCEEDED(pStore->GetValue(keyDevicePath, &pvPath)) && pvPath.vt == VT_LPWSTR && pvPath.pwszVal) {
                std::wstring path = pvPath.pwszVal;
                PropVariantClear(&pvPath);
                pStore->Release();
                return path;
            }
            PropVariantClear(&pvPath);
            pStore->Release();
        }
    }
    return L"\\\\?\\SWD#MMDEVAPI#" + endpointId + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
}

static DWORD FindProcessIdFromSession(const std::wstring& targetSessionId) {
    if (targetSessionId.empty()) return 0;

    bool isNumeric = true;
    for (wchar_t c : targetSessionId) {
        if (!iswdigit(c)) { isNumeric = false; break; }
    }
    if (isNumeric) {
        try {
            return (DWORD)std::stoul(targetSessionId);
        } catch (...) {
            return 0;
        }
    }

    IMMDeviceEnumerator* pEnumerator = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) return 0;

    IMMDeviceCollection* pCollection = NULL;
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    pEnumerator->Release();
    if (FAILED(hr) || !pCollection) return 0;

    UINT devCount = 0;
    pCollection->GetCount(&devCount);
    DWORD foundPid = 0;

    for (UINT d = 0; d < devCount && foundPid == 0; d++) {
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

                        LPWSTR pSessId = NULL;
                        std::wstring sessionIdentifier;
                        if (SUCCEEDED(pSessionControl2->GetSessionIdentifier(&pSessId)) && pSessId && wcslen(pSessId) > 0) {
                            sessionIdentifier = pSessId;
                            CoTaskMemFree(pSessId);
                        }

                        if (uniqueSessionId == targetSessionId ||
                            sessionIdentifier == targetSessionId ||
                            (pid != 0 && std::to_wstring(pid) == targetSessionId)) {
                            foundPid = pid;
                        }
                        pSessionControl2->Release();
                    }
                    pSessionControl->Release();
                    if (foundPid != 0) break;
                }
                pSessionEnum->Release();
            }
            pSessionManager->Release();
        }
        pDevice->Release();
    }

    pCollection->Release();
    return foundPid;
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
                            AudioSessionState sessionState;
                            if (SUCCEEDED(pSessionControl->GetState(&sessionState)) && sessionState == AudioSessionStateExpired) {
                                pSessionControl2->Release();
                                pSessionControl->Release();
                                continue;
                            }

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

                            if (uniqueSessionId == targetSessionId || (pid != 0 && std::to_wstring(pid) == targetSessionId)) {
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
            bool matches = (targetDeviceId == pstrId);
            if (!matches) {
                IPropertyStore* pProps = NULL;
                if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pProps)) && pProps) {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName))) {
                        if (varName.vt == VT_LPWSTR && varName.pwszVal && targetDeviceId == varName.pwszVal) {
                            matches = true;
                        }
                    }
                    PropVariantClear(&varName);
                    pProps->Release();
                }
            }
            if (matches) {
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
        {
            std::lock_guard<std::mutex> lock(g_monitorMutex);
            std::wstring devId = pwstrDeviceId;
            for (const auto& ep : g_monitoredEndpoints) {
                if (ep.deviceId == devId) {
                    isRender = true;
                    break;
                }
            }
        }

        if (!isRender && g_pDeviceEnumerator) {
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

            {
                std::lock_guard<std::mutex> lock(g_fanoutMutex);
                if (g_fanoutEngine) {
                    g_fanoutEngine->OnDeviceStateChanged(pwstrDeviceId, dwNewState);
                }
            }
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

            {
                std::lock_guard<std::mutex> lock(g_fanoutMutex);
                if (g_fanoutEngine) {
                    g_fanoutEngine->OnDeviceAdded(pwstrDeviceId);
                }
            }
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

        {
            std::lock_guard<std::mutex> lock(g_fanoutMutex);
            if (g_fanoutEngine) {
                g_fanoutEngine->OnDeviceStateChanged(pwstrDeviceId, DEVICE_STATE_NOTPRESENT);
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) override {
        if (flow == eRender && role == eConsole) {
            CoreAudioEventPayload payload;
            payload.type = "default-device-changed";
            payload.deviceId = pwstrDefaultDeviceId ? WideToUtf8(pwstrDefaultDeviceId) : "";
            DispatchEvent(payload);

            {
                std::lock_guard<std::mutex> lock(g_fanoutMutex);
                if (g_fanoutEngine) {
                    g_fanoutEngine->OnDefaultDeviceChanged(pwstrDefaultDeviceId ? pwstrDefaultDeviceId : L"");
                }
            }
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

    // Per-session volume and mute state tracking for duplicate event suppression
    std::mutex m_stateMutex;
    bool m_hasLastState = false;
    int m_lastVolume = -1;
    bool m_lastMute = false;

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
        int volumePercent = (int)std::round(NewVolume * 100.0f);
        bool mute = (NewMute != FALSE);

        bool shouldEmit = false;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            if (!m_hasLastState) {
                m_hasLastState = true;
                m_lastVolume = volumePercent;
                m_lastMute = mute;
                shouldEmit = true;
            } else if (m_lastVolume != volumePercent || m_lastMute != mute) {
                m_lastVolume = volumePercent;
                m_lastMute = mute;
                shouldEmit = true;
            } else {
                shouldEmit = false;
            }
        }

        if (shouldEmit) {
            CoreAudioEventPayload payload;
            payload.type = "session-volume-changed";
            payload.sessionId = WideToUtf8(m_sessionId.c_str());
            payload.deviceId = WideToUtf8(m_deviceId.c_str());
            payload.volumePercent = volumePercent;
            payload.mute = mute;
            DispatchEvent(payload);
        }
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

            AudioSessionState state;
            if (SUCCEEDED(NewSession->GetState(&state)) && state == AudioSessionStateExpired) {
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
                    bool alreadyRegistered = false;

                    for (const auto& existing : ep.sessions) {
                        if (existing.sessionId == sessionId) {
                        alreadyRegistered = true;
                        break;
                        }
                    }

                    if (!alreadyRegistered) {
                        SessionEventsClient* pEvents = new SessionEventsClient(sessionId, m_deviceId);
                        HRESULT hrReg = NewSession->RegisterAudioSessionNotification(pEvents);
                        if (SUCCEEDED(hrReg)) {
                            MonitoredSession sess;
                            sess.sessionId = sessionId;
                            sess.pControl = NewSession;
                            sess.pControl->AddRef();
                            sess.pEventsClient = pEvents;
                            ep.sessions.push_back(sess);
                        } else {
                            pEvents->Release();
                        }
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
                        AudioSessionState state;
                        if (SUCCEEDED(pCtrl->GetState(&state)) && state == AudioSessionStateExpired) {
                            pCtrl2->Release();
                            pCtrl->Release();
                            continue;
                        }

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

                            bool alreadyRegistered = false;

                            for (const auto& existing : ep.sessions) {
                                if (existing.sessionId == sessionId) {
                                    alreadyRegistered = true;
                                    break;
                                }
                            }

                            if (!alreadyRegistered) {
                                SessionEventsClient* pEvents = new SessionEventsClient(sessionId, deviceId);
                                HRESULT hrReg = pCtrl->RegisterAudioSessionNotification(pEvents);
                                if (SUCCEEDED(hrReg)) {
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
    {
        std::lock_guard<std::mutex> lock(g_fanoutMutex);
        if (g_fanoutEngine) {
            g_fanoutEngine->Shutdown();
            g_fanoutEngine.reset();
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_renderMutex);
        if (g_renderClient) {
            g_renderClient->StopRender();
            g_renderClient.reset();
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_captureMutex);
        if (g_captureClient) {
            g_captureClient->StopCapture();
            g_captureClient.reset();
        }
    }

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

        int volPercent = 100;
        bool isMuted = false;
        IAudioEndpointVolume* pEpVol = NULL;
        if (SUCCEEDED(pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&pEpVol)) && pEpVol) {
            float scalar = 1.0f;
            if (SUCCEEDED(pEpVol->GetMasterVolumeLevelScalar(&scalar))) {
                volPercent = (int)std::round(scalar * 100.0f);
            }
            BOOL muteVal = FALSE;
            if (SUCCEEDED(pEpVol->GetMute(&muteVal))) {
                isMuted = (muteVal != FALSE);
            }
            pEpVol->Release();
        }

        napi_value jsDevice;
        napi_create_object(env, &jsDevice);

        napi_value idVal = CreateNapiStringFromWide(env, deviceId.c_str());
        napi_set_named_property(env, jsDevice, "id", idVal);

        napi_value nameVal = CreateNapiStringFromWide(env, friendlyName.c_str());
        napi_set_named_property(env, jsDevice, "name", nameVal);

        napi_value isDefaultVal;
        napi_get_boolean(env, isDefault, &isDefaultVal);
        napi_set_named_property(env, jsDevice, "isDefault", isDefaultVal);

        napi_value volVal;
        napi_create_int32(env, volPercent, &volVal);
        napi_set_named_property(env, jsDevice, "volumePercent", volVal);

        napi_value mutePropVal;
        napi_get_boolean(env, isMuted, &mutePropVal);
        napi_set_named_property(env, jsDevice, "mute", mutePropVal);

        napi_set_element(env, jsArray, i, jsDevice);

        pDevice->Release();
    }

    pCollection->Release();
    return jsArray;
}

static napi_value Method_ListApplicationStreams(napi_env env, napi_callback_info info) {
    void* pPolicyConfig = NULL;
    GetAudioPolicyConfig(&pPolicyConfig);

    IMMDeviceEnumerator* pEnumerator = NULL;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) {
        if (pPolicyConfig) ((IUnknown*)pPolicyConfig)->Release();
        napi_throw_error(env, NULL, "Failed to create MMDeviceEnumerator.");
        return NULL;
    }

    IMMDeviceCollection* pCollection = NULL;
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    pEnumerator->Release();

    if (FAILED(hr) || !pCollection) {
        if (pPolicyConfig) ((IUnknown*)pPolicyConfig)->Release();
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

                        AudioSessionState state;
                        if (FAILED(pSessionControl->GetState(&state)) || state == AudioSessionStateExpired) {
                            pSessionControl2->Release();
                            pSessionControl->Release();
                            continue;
                        }
                        bool isActive = (state == AudioSessionStateActive);

                        LPWSTR pInstanceId = NULL;
                        std::wstring uniqueSessionId;
                        if (SUCCEEDED(pSessionControl2->GetSessionInstanceIdentifier(&pInstanceId)) && pInstanceId && wcslen(pInstanceId) > 0) {
                            uniqueSessionId = pInstanceId;
                            CoTaskMemFree(pInstanceId);
                        } else {
                            uniqueSessionId = deviceId + L":" + std::to_wstring(pid) + L":" + std::to_wstring(s);
                        }

                        LPWSTR pSessionId = NULL;
                        std::wstring sessionIdentifier;
                        if (SUCCEEDED(pSessionControl2->GetSessionIdentifier(&pSessionId)) && pSessionId && wcslen(pSessionId) > 0) {
                            sessionIdentifier = pSessionId;
                            CoTaskMemFree(pSessionId);
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

                        if (!sessionIdentifier.empty()) {
                            napi_value sessIdVal = CreateNapiStringFromWide(env, sessionIdentifier.c_str());
                            napi_set_named_property(env, jsStream, "sessionIdentifier", sessIdVal);
                        }

                        napi_value pidVal;
                        napi_create_uint32(env, pid, &pidVal);
                        napi_set_named_property(env, jsStream, "processId", pidVal);

                        napi_value nameVal = CreateNapiStringFromWide(env, appName.c_str());
                        napi_set_named_property(env, jsStream, "name", nameVal);

                        napi_value stateVal;
                        napi_create_int32(env, (int)state, &stateVal);
                        napi_set_named_property(env, jsStream, "state", stateVal);

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

                        std::wstring persistedSinkId;
                        if (pPolicyConfig && pid != 0) {
                            void** vtable = *(void***)pPolicyConfig;
                            auto GetPersistedDefaultAudioEndpoint = (GetPersistedDefaultAudioEndpointFunc)vtable[26];
                            void* hPersisted = NULL;
                            if (SUCCEEDED(GetPersistedDefaultAudioEndpoint(pPolicyConfig, pid, eRender, eConsole, &hPersisted)) && hPersisted) {
                                UINT32 rawLen = 0;
                                auto& api = WinRtApi::Instance();
                                PCWSTR rawStr = api.WindowsGetStringRawBuffer ? api.WindowsGetStringRawBuffer(hPersisted, &rawLen) : NULL;
                                if (rawStr) {
                                    persistedSinkId = ExtractEndpointIdFromPath(rawStr);
                                }
                                if (api.WindowsDeleteString) api.WindowsDeleteString(hPersisted);
                            }
                        }

                        if (!persistedSinkId.empty()) {
                            napi_value persistedSinkVal = CreateNapiStringFromWide(env, persistedSinkId.c_str());
                            napi_set_named_property(env, jsStream, "persistedSinkId", persistedSinkVal);
                        } else {
                            napi_value nullVal;
                            napi_get_null(env, &nullVal);
                            napi_set_named_property(env, jsStream, "persistedSinkId", nullVal);
                        }

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

    if (pPolicyConfig) {
        ((IUnknown*)pPolicyConfig)->Release();
        pPolicyConfig = NULL;
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

static napi_value Method_GetEndpointVolume(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 1) {
        napi_throw_type_error(env, NULL, "getEndpointVolume requires 1 argument: (deviceId)");
        return NULL;
    }

    std::wstring deviceId = GetWideStringFromNapi(env, args[0]);
    if (deviceId.empty()) {
        napi_throw_type_error(env, NULL, "deviceId must be a non-empty string");
        return NULL;
    }

    IAudioEndpointVolume* pEndpointVolume = NULL;
    HRESULT hr = FindEndpointVolume(deviceId, &pEndpointVolume);
    if (FAILED(hr) || !pEndpointVolume) {
        std::string err = FormatHresultError("FindEndpointVolume", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    float scalar = 1.0f;
    hr = pEndpointVolume->GetMasterVolumeLevelScalar(&scalar);
    pEndpointVolume->Release();

    if (FAILED(hr)) {
        std::string err = FormatHresultError("IAudioEndpointVolume::GetMasterVolumeLevelScalar", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    int volPercent = (int)std::round(scalar * 100.0f);
    napi_value result;
    napi_create_int32(env, volPercent, &result);
    return result;
}

static napi_value Method_GetEndpointMute(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 1) {
        napi_throw_type_error(env, NULL, "getEndpointMute requires 1 argument: (deviceId)");
        return NULL;
    }

    std::wstring deviceId = GetWideStringFromNapi(env, args[0]);
    if (deviceId.empty()) {
        napi_throw_type_error(env, NULL, "deviceId must be a non-empty string");
        return NULL;
    }

    IAudioEndpointVolume* pEndpointVolume = NULL;
    HRESULT hr = FindEndpointVolume(deviceId, &pEndpointVolume);
    if (FAILED(hr) || !pEndpointVolume) {
        std::string err = FormatHresultError("FindEndpointVolume", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    BOOL muted = FALSE;
    hr = pEndpointVolume->GetMute(&muted);
    pEndpointVolume->Release();

    if (FAILED(hr)) {
        std::string err = FormatHresultError("IAudioEndpointVolume::GetMute", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    napi_value result;
    napi_get_boolean(env, (muted != FALSE), &result);
    return result;
}

static napi_value Method_SetApplicationOutput(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 2) {
        napi_throw_type_error(env, NULL, "setApplicationOutput requires 2 arguments: (processIdOrSessionId, endpointId)");
        return NULL;
    }

    DWORD processId = 0;
    napi_valuetype arg0Type;
    napi_typeof(env, args[0], &arg0Type);
    if (arg0Type == napi_number) {
        uint32_t pidVal = 0;
        napi_get_value_uint32(env, args[0], &pidVal);
        processId = pidVal;
    } else if (arg0Type == napi_string) {
        std::wstring strArg = GetWideStringFromNapi(env, args[0]);
        processId = FindProcessIdFromSession(strArg);
    }

    if (processId == 0) {
        napi_throw_error(env, NULL, "Target application session not found or has invalid process ID.");
        return NULL;
    }

    std::wstring targetDeviceId = GetWideStringFromNapi(env, args[1]);

    ScopedCoInitialize scopedCo;

    auto& api = WinRtApi::Instance();
    if (!api.loaded) {
        napi_throw_error(env, NULL, "Failed to load WinRT functions from combase.dll.");
        return NULL;
    }

    void* pPolicyConfig = NULL;
    HRESULT hr = GetAudioPolicyConfig(&pPolicyConfig);
    if (FAILED(hr) || !pPolicyConfig) {
        std::string err = FormatHresultError("AudioPolicyConfig activation", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    void** vtable = *(void***)pPolicyConfig;
    auto SetPersistedDefaultAudioEndpoint = (SetPersistedDefaultAudioEndpointFunc)vtable[25];

    // If targetDeviceId is empty or "default", clear the override
    if (targetDeviceId.empty() || targetDeviceId == L"default" || targetDeviceId == L"Default") {
        hr = SetPersistedDefaultAudioEndpoint(pPolicyConfig, processId, eRender, eConsole, nullptr);
        if (SUCCEEDED(hr)) {
            SetPersistedDefaultAudioEndpoint(pPolicyConfig, processId, eRender, eMultimedia, nullptr);
        }
        ((IUnknown*)pPolicyConfig)->Release();
        if (FAILED(hr)) {
            std::string err = FormatHresultError("SetPersistedDefaultAudioEndpoint(clear)", hr);
            napi_throw_error(env, NULL, err.c_str());
            return NULL;
        }
        napi_value result;
        napi_get_boolean(env, true, &result);
        return result;
    }

    // Validate target device endpoint
    IMMDeviceEnumerator* pEnumerator = NULL;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) {
        ((IUnknown*)pPolicyConfig)->Release();
        napi_throw_error(env, NULL, "Failed to create MMDeviceEnumerator.");
        return NULL;
    }

    IMMDevice* pTargetDevice = NULL;
    hr = pEnumerator->GetDevice(targetDeviceId.c_str(), &pTargetDevice);
    pEnumerator->Release();
    if (FAILED(hr) || !pTargetDevice) {
        ((IUnknown*)pPolicyConfig)->Release();
        std::string err = "Target audio endpoint not found: " + WideToUtf8(targetDeviceId.c_str());
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    DWORD devState = 0;
    hr = pTargetDevice->GetState(&devState);
    if (FAILED(hr) || !(devState & DEVICE_STATE_ACTIVE)) {
        pTargetDevice->Release();
        ((IUnknown*)pPolicyConfig)->Release();
        std::string err = "Target audio endpoint is not active: " + WideToUtf8(targetDeviceId.c_str());
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    std::wstring fullPath = GetFullDevicePath(pTargetDevice, targetDeviceId);
    pTargetDevice->Release();

    void* hTargetString = NULL;
    hr = api.WindowsCreateString(fullPath.c_str(), (UINT32)fullPath.length(), &hTargetString);
    if (FAILED(hr) || !hTargetString) {
        ((IUnknown*)pPolicyConfig)->Release();
        napi_throw_error(env, NULL, "Failed to create WinRT HSTRING for target endpoint.");
        return NULL;
    }

    hr = SetPersistedDefaultAudioEndpoint(pPolicyConfig, processId, eRender, eConsole, hTargetString);
    if (SUCCEEDED(hr)) {
        SetPersistedDefaultAudioEndpoint(pPolicyConfig, processId, eRender, eMultimedia, hTargetString);
    }

    api.WindowsDeleteString(hTargetString);
    ((IUnknown*)pPolicyConfig)->Release();

    if (FAILED(hr)) {
        std::string err = FormatHresultError("SetPersistedDefaultAudioEndpoint", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value Method_GetApplicationOutput(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 1) {
        napi_throw_type_error(env, NULL, "getApplicationOutput requires 1 argument: (processIdOrSessionId)");
        return NULL;
    }

    DWORD processId = 0;
    napi_valuetype arg0Type;
    napi_typeof(env, args[0], &arg0Type);
    if (arg0Type == napi_number) {
        uint32_t pidVal = 0;
        napi_get_value_uint32(env, args[0], &pidVal);
        processId = pidVal;
    } else if (arg0Type == napi_string) {
        std::wstring strArg = GetWideStringFromNapi(env, args[0]);
        processId = FindProcessIdFromSession(strArg);
    }

    if (processId == 0) {
        napi_value nullVal;
        napi_get_null(env, &nullVal);
        return nullVal;
    }

    ScopedCoInitialize scopedCo;

    auto& api = WinRtApi::Instance();
    if (!api.loaded) {
        napi_value nullVal;
        napi_get_null(env, &nullVal);
        return nullVal;
    }

    void* pPolicyConfig = NULL;
    HRESULT hr = GetAudioPolicyConfig(&pPolicyConfig);
    if (FAILED(hr) || !pPolicyConfig) {
        napi_value nullVal;
        napi_get_null(env, &nullVal);
        return nullVal;
    }

    void** vtable = *(void***)pPolicyConfig;
    auto GetPersistedDefaultAudioEndpoint = (GetPersistedDefaultAudioEndpointFunc)vtable[26];

    void* hPersisted = NULL;
    hr = GetPersistedDefaultAudioEndpoint(pPolicyConfig, processId, eRender, eConsole, &hPersisted);
    ((IUnknown*)pPolicyConfig)->Release();

    if (SUCCEEDED(hr) && hPersisted) {
        UINT32 len = 0;
        PCWSTR rawBuffer = api.WindowsGetStringRawBuffer(hPersisted, &len);
        std::wstring rawStr = rawBuffer ? rawBuffer : L"";
        api.WindowsDeleteString(hPersisted);

        std::wstring endpointId = ExtractEndpointIdFromPath(rawStr);
        if (!endpointId.empty()) {
            return CreateNapiStringFromWide(env, endpointId.c_str());
        }
    }

    napi_value nullVal;
    napi_get_null(env, &nullVal);
    return nullVal;
}

static napi_value Method_ClearApplicationOutput(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    if (napi_get_cb_info(env, info, &argc, args, NULL, NULL) != napi_ok || argc < 1) {
        napi_throw_type_error(env, NULL, "clearApplicationOutput requires 1 argument: (processIdOrSessionId)");
        return NULL;
    }

    DWORD processId = 0;
    napi_valuetype arg0Type;
    napi_typeof(env, args[0], &arg0Type);
    if (arg0Type == napi_number) {
        uint32_t pidVal = 0;
        napi_get_value_uint32(env, args[0], &pidVal);
        processId = pidVal;
    } else if (arg0Type == napi_string) {
        std::wstring strArg = GetWideStringFromNapi(env, args[0]);
        processId = FindProcessIdFromSession(strArg);
    }

    if (processId == 0) {
        napi_throw_error(env, NULL, "Target application session not found or has invalid process ID.");
        return NULL;
    }

    ScopedCoInitialize scopedCo;

    void* pPolicyConfig = NULL;
    HRESULT hr = GetAudioPolicyConfig(&pPolicyConfig);
    if (FAILED(hr) || !pPolicyConfig) {
        std::string err = FormatHresultError("AudioPolicyConfig activation", hr);
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    void** vtable = *(void***)pPolicyConfig;
    auto SetPersistedDefaultAudioEndpoint = (SetPersistedDefaultAudioEndpointFunc)vtable[25];

    hr = SetPersistedDefaultAudioEndpoint(pPolicyConfig, processId, eRender, eConsole, nullptr);
    if (SUCCEEDED(hr)) {
        SetPersistedDefaultAudioEndpoint(pPolicyConfig, processId, eRender, eMultimedia, nullptr);
    }
    ((IUnknown*)pPolicyConfig)->Release();

    if (FAILED(hr)) {
        std::string err = FormatHresultError("SetPersistedDefaultAudioEndpoint(clear)", hr);
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

// -----------------------------------------------------------------------------
// WASAPI Loopback Capture Node-API Methods
// -----------------------------------------------------------------------------

static napi_value Method_CaptureStart(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    std::wstring deviceId;
    if (argc > 0 && args[0] != nullptr) {
        napi_valuetype argType;
        napi_typeof(env, args[0], &argType);
        if (argType == napi_string) {
            deviceId = GetWideStringFromNapi(env, args[0]);
        } else if (argType == napi_object) {
            napi_value devIdVal;
            if (napi_get_named_property(env, args[0], "deviceId", &devIdVal) == napi_ok) {
                napi_valuetype propType;
                napi_typeof(env, devIdVal, &propType);
                if (propType == napi_string) {
                    deviceId = GetWideStringFromNapi(env, devIdVal);
                } else if (propType != napi_undefined && propType != napi_null) {
                    napi_throw_type_error(env, NULL, "Invalid argument: options.deviceId must be a string.");
                    return NULL;
                }
            }
        } else if (argType != napi_undefined && argType != napi_null) {
            napi_throw_type_error(env, NULL, "Invalid argument: endpointId must be a string or options object.");
            return NULL;
        }
    }

    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (!g_captureClient) {
        g_captureClient = std::make_unique<speakerflow::WasapiCaptureClient>();
    }

    std::string error;
    bool success = g_captureClient->StartCapture(deviceId, error);
    if (!success) {
        napi_throw_error(env, NULL, error.c_str());
        return NULL;
    }

    speakerflow::WasapiCaptureStats stats = g_captureClient->GetStats();

    napi_value resObj;
    napi_create_object(env, &resObj);

    napi_value boolVal;
    napi_get_boolean(env, true, &boolVal);
    napi_set_named_property(env, resObj, "success", boolVal);

    napi_value srVal, chVal, bpsVal;
    napi_create_uint32(env, stats.sampleRate, &srVal);
    napi_create_uint32(env, stats.channels, &chVal);
    napi_create_uint32(env, stats.bitsPerSample, &bpsVal);
    napi_set_named_property(env, resObj, "sampleRate", srVal);
    napi_set_named_property(env, resObj, "channels", chVal);
    napi_set_named_property(env, resObj, "bitsPerSample", bpsVal);

    napi_value fmtVal, nameVal, devIdVal;
    napi_create_string_utf8(env, stats.formatTag.c_str(), NAPI_AUTO_LENGTH, &fmtVal);
    napi_create_string_utf8(env, stats.deviceFriendlyName.c_str(), NAPI_AUTO_LENGTH, &nameVal);
    napi_create_string_utf8(env, stats.deviceId.c_str(), NAPI_AUTO_LENGTH, &devIdVal);
    napi_set_named_property(env, resObj, "format", fmtVal);
    napi_set_named_property(env, resObj, "deviceFriendlyName", nameVal);
    napi_set_named_property(env, resObj, "deviceId", devIdVal);

    napi_value evVal;
    napi_get_boolean(env, stats.isEventDriven, &evVal);
    napi_set_named_property(env, resObj, "isEventDriven", evVal);

    return resObj;
}

static napi_value Method_CaptureStop(napi_env env, napi_callback_info info) {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (g_captureClient) {
        g_captureClient->StopCapture();
    }
    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value Method_CaptureIsActive(napi_env env, napi_callback_info info) {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    bool active = g_captureClient ? g_captureClient->IsCapturing() : false;
    napi_value result;
    napi_get_boolean(env, active, &result);
    return result;
}

static napi_value Method_CaptureGetStats(napi_env env, napi_callback_info info) {
    speakerflow::WasapiCaptureStats stats;
    {
        std::lock_guard<std::mutex> lock(g_captureMutex);
        if (g_captureClient) {
            stats = g_captureClient->GetStats();
        }
    }

    napi_value obj;
    napi_create_object(env, &obj);

    napi_value isCapVal, isEvVal;
    napi_get_boolean(env, stats.isCapturing, &isCapVal);
    napi_get_boolean(env, stats.isEventDriven, &isEvVal);
    napi_set_named_property(env, obj, "isCapturing", isCapVal);
    napi_set_named_property(env, obj, "isEventDriven", isEvVal);

    napi_value srVal, chVal, bpsVal;
    napi_create_uint32(env, stats.sampleRate, &srVal);
    napi_create_uint32(env, stats.channels, &chVal);
    napi_create_uint32(env, stats.bitsPerSample, &bpsVal);
    napi_set_named_property(env, obj, "sampleRate", srVal);
    napi_set_named_property(env, obj, "channels", chVal);
    napi_set_named_property(env, obj, "bitsPerSample", bpsVal);

    napi_value fmtVal, devIdVal, devNameVal, lastErrVal;
    napi_create_string_utf8(env, stats.formatTag.c_str(), NAPI_AUTO_LENGTH, &fmtVal);
    napi_create_string_utf8(env, stats.deviceId.c_str(), NAPI_AUTO_LENGTH, &devIdVal);
    napi_create_string_utf8(env, stats.deviceFriendlyName.c_str(), NAPI_AUTO_LENGTH, &devNameVal);
    napi_create_string_utf8(env, stats.lastError.c_str(), NAPI_AUTO_LENGTH, &lastErrVal);
    napi_set_named_property(env, obj, "formatTag", fmtVal);
    napi_set_named_property(env, obj, "deviceId", devIdVal);
    napi_set_named_property(env, obj, "deviceFriendlyName", devNameVal);
    napi_set_named_property(env, obj, "lastError", lastErrVal);

    napi_value framesVal, silentVal, packetsVal;
    napi_create_int64(env, stats.framesCaptured, &framesVal);
    napi_create_int64(env, stats.silentFramesCaptured, &silentVal);
    napi_create_int64(env, stats.packetsCaptured, &packetsVal);
    napi_set_named_property(env, obj, "framesCaptured", framesVal);
    napi_set_named_property(env, obj, "silentFramesCaptured", silentVal);
    napi_set_named_property(env, obj, "packetsCaptured", packetsVal);

    napi_value peakVal, rmsVal;
    napi_create_double(env, static_cast<double>(stats.peakLevel), &peakVal);
    napi_create_double(env, static_cast<double>(stats.rmsLevel), &rmsVal);
    napi_set_named_property(env, obj, "peakLevel", peakVal);
    napi_set_named_property(env, obj, "rmsLevel", rmsVal);

    napi_value underrunVal, overrunVal, ringFramesVal, ringCapVal;
    napi_create_int64(env, stats.ringBufferUnderruns, &underrunVal);
    napi_create_int64(env, stats.ringBufferOverruns, &overrunVal);
    napi_create_int64(env, stats.ringBufferFrames, &ringFramesVal);
    napi_create_int64(env, stats.ringBufferCapacity, &ringCapVal);
    napi_set_named_property(env, obj, "ringBufferUnderruns", underrunVal);
    napi_set_named_property(env, obj, "ringBufferOverruns", overrunVal);
    napi_set_named_property(env, obj, "ringBufferFrames", ringFramesVal);
    napi_set_named_property(env, obj, "ringBufferCapacity", ringCapVal);

    return obj;
}

// -----------------------------------------------------------------------------
// WASAPI Render Node-API Methods (Phase 2B)
// -----------------------------------------------------------------------------

static napi_value Method_RenderStart(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    std::wstring deviceId;
    if (argc > 0 && args[0] != nullptr) {
        napi_valuetype argType;
        napi_typeof(env, args[0], &argType);
        if (argType == napi_string) {
            deviceId = GetWideStringFromNapi(env, args[0]);
        } else if (argType == napi_object) {
            napi_value devIdVal;
            if (napi_get_named_property(env, args[0], "deviceId", &devIdVal) == napi_ok) {
                napi_valuetype propType;
                napi_typeof(env, devIdVal, &propType);
                if (propType == napi_string) {
                    deviceId = GetWideStringFromNapi(env, devIdVal);
                }
            }
        }
    }

    // Must have active capture client to supply audio frames to the ring buffer
    speakerflow::WasapiCaptureStats capStats;
    speakerflow::AudioRingBuffer* pRingBuffer = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_captureMutex);
        if (!g_captureClient || !g_captureClient->IsCapturing()) {
            napi_throw_error(env, NULL, "Cannot start render: WASAPI loopback capture is not active.");
            return NULL;
        }
        capStats = g_captureClient->GetStats();
        pRingBuffer = g_captureClient->GetRingBuffer();
    }

    if (!pRingBuffer) {
        napi_throw_error(env, NULL, "Capture ring buffer is not initialized.");
        return NULL;
    }

    // Feedback-loop safety check:
    // Rendering captured audio back to the same endpoint creates an acoustic/digital feedback loop.
    std::string targetDeviceIdUtf8 = WideToUtf8(deviceId.c_str());
    bool isSameEndpoint = false;
    if (deviceId.empty() && capStats.deviceId.empty()) {
        isSameEndpoint = true;
    } else if (!deviceId.empty() && targetDeviceIdUtf8 == capStats.deviceId) {
        isSameEndpoint = true;
    }

    if (isSameEndpoint) {
        std::string err = "Feedback loop prevention: Cannot render to the same endpoint (" +
                          (capStats.deviceFriendlyName.empty() ? "Default Endpoint" : capStats.deviceFriendlyName) +
                          ") that is being captured via WASAPI loopback. Select a secondary physical or virtual output endpoint.";
        napi_throw_error(env, NULL, err.c_str());
        return NULL;
    }

    std::lock_guard<std::mutex> lock(g_renderMutex);
    if (!g_renderClient) {
        g_renderClient = std::make_unique<speakerflow::WasapiRenderClient>();
    }

    std::string error;
    bool success = g_renderClient->StartRender(deviceId,
                                               pRingBuffer,
                                               capStats.sampleRate,
                                               capStats.channels,
                                               error);
    if (!success) {
        napi_throw_error(env, NULL, error.c_str());
        return NULL;
    }

    speakerflow::WasapiRenderStats renderStats = g_renderClient->GetStats();

    napi_value resObj;
    napi_create_object(env, &resObj);

    napi_value boolVal;
    napi_get_boolean(env, true, &boolVal);
    napi_set_named_property(env, resObj, "success", boolVal);

    napi_value srVal, chVal, bpsVal;
    napi_create_uint32(env, renderStats.sampleRate, &srVal);
    napi_create_uint32(env, renderStats.channels, &chVal);
    napi_create_uint32(env, renderStats.bitsPerSample, &bpsVal);
    napi_set_named_property(env, resObj, "sampleRate", srVal);
    napi_set_named_property(env, resObj, "channels", chVal);
    napi_set_named_property(env, resObj, "bitsPerSample", bpsVal);

    napi_value fmtVal, nameVal, devIdVal;
    napi_create_string_utf8(env, renderStats.formatTag.c_str(), NAPI_AUTO_LENGTH, &fmtVal);
    napi_create_string_utf8(env, renderStats.deviceFriendlyName.c_str(), NAPI_AUTO_LENGTH, &nameVal);
    napi_create_string_utf8(env, renderStats.deviceId.c_str(), NAPI_AUTO_LENGTH, &devIdVal);
    napi_set_named_property(env, resObj, "format", fmtVal);
    napi_set_named_property(env, resObj, "deviceFriendlyName", nameVal);
    napi_set_named_property(env, resObj, "deviceId", devIdVal);

    napi_value evVal;
    napi_get_boolean(env, renderStats.isEventDriven, &evVal);
    napi_set_named_property(env, resObj, "isEventDriven", evVal);

    napi_value bufDurVal;
    napi_create_double(env, renderStats.bufferDurationMs, &bufDurVal);
    napi_set_named_property(env, resObj, "bufferDurationMs", bufDurVal);

    return resObj;
}

static napi_value Method_RenderStop(napi_env env, napi_callback_info info) {
    std::lock_guard<std::mutex> lock(g_renderMutex);
    if (g_renderClient) {
        g_renderClient->StopRender();
    }
    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value Method_RenderIsActive(napi_env env, napi_callback_info info) {
    std::lock_guard<std::mutex> lock(g_renderMutex);
    bool active = g_renderClient ? g_renderClient->IsRendering() : false;
    napi_value result;
    napi_get_boolean(env, active, &result);
    return result;
}

static napi_value Method_RenderGetStats(napi_env env, napi_callback_info info) {
    speakerflow::WasapiRenderStats stats;
    {
        std::lock_guard<std::mutex> lock(g_renderMutex);
        if (g_renderClient) {
            stats = g_renderClient->GetStats();
        }
    }

    napi_value obj;
    napi_create_object(env, &obj);

    napi_value isRenVal, isEvVal;
    napi_get_boolean(env, stats.isRendering, &isRenVal);
    napi_get_boolean(env, stats.isEventDriven, &isEvVal);
    napi_set_named_property(env, obj, "isRendering", isRenVal);
    napi_set_named_property(env, obj, "isEventDriven", isEvVal);

    napi_value srVal, chVal, bpsVal;
    napi_create_uint32(env, stats.sampleRate, &srVal);
    napi_create_uint32(env, stats.channels, &chVal);
    napi_create_uint32(env, stats.bitsPerSample, &bpsVal);
    napi_set_named_property(env, obj, "sampleRate", srVal);
    napi_set_named_property(env, obj, "channels", chVal);
    napi_set_named_property(env, obj, "bitsPerSample", bpsVal);

    napi_value fmtVal, devIdVal, devNameVal, lastErrVal;
    napi_create_string_utf8(env, stats.formatTag.c_str(), NAPI_AUTO_LENGTH, &fmtVal);
    napi_create_string_utf8(env, stats.deviceId.c_str(), NAPI_AUTO_LENGTH, &devIdVal);
    napi_create_string_utf8(env, stats.deviceFriendlyName.c_str(), NAPI_AUTO_LENGTH, &devNameVal);
    napi_create_string_utf8(env, stats.lastError.c_str(), NAPI_AUTO_LENGTH, &lastErrVal);
    napi_set_named_property(env, obj, "formatTag", fmtVal);
    napi_set_named_property(env, obj, "deviceId", devIdVal);
    napi_set_named_property(env, obj, "deviceFriendlyName", devNameVal);
    napi_set_named_property(env, obj, "lastError", lastErrVal);

    napi_value framesVal, silentVal;
    napi_create_int64(env, stats.framesRendered, &framesVal);
    napi_create_int64(env, stats.silentFramesRendered, &silentVal);
    napi_set_named_property(env, obj, "framesRendered", framesVal);
    napi_set_named_property(env, obj, "silentFramesRendered", silentVal);

    napi_value underrunVal, overrunVal, ringFramesVal, ringCapVal;
    napi_create_int64(env, stats.bufferUnderruns, &underrunVal);
    napi_create_int64(env, stats.bufferOverruns, &overrunVal);
    napi_create_int64(env, stats.ringBufferFrames, &ringFramesVal);
    napi_create_int64(env, stats.ringBufferCapacity, &ringCapVal);
    napi_set_named_property(env, obj, "bufferUnderruns", underrunVal);
    napi_set_named_property(env, obj, "bufferOverruns", overrunVal);
    napi_set_named_property(env, obj, "ringBufferFrames", ringFramesVal);
    napi_set_named_property(env, obj, "ringBufferCapacity", ringCapVal);

    napi_value bufDurVal, ringOccVal;
    napi_create_double(env, stats.bufferDurationMs, &bufDurVal);
    napi_create_double(env, stats.ringBufferOccupancyMs, &ringOccVal);
    napi_set_named_property(env, obj, "bufferDurationMs", bufDurVal);
    napi_set_named_property(env, obj, "ringBufferOccupancyMs", ringOccVal);

    return obj;
}

// -----------------------------------------------------------------------------
// WASAPI Fan-Out Engine Node-API Methods (Phase 2C)
// -----------------------------------------------------------------------------

static napi_value Method_EngineStartCapture(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    std::wstring deviceId;
    if (argc > 0 && args[0] != nullptr) {
        napi_valuetype argType;
        napi_typeof(env, args[0], &argType);
        if (argType == napi_string) {
            deviceId = GetWideStringFromNapi(env, args[0]);
        } else if (argType == napi_object) {
            napi_value devIdVal;
            if (napi_get_named_property(env, args[0], "deviceId", &devIdVal) == napi_ok) {
                napi_valuetype propType;
                napi_typeof(env, devIdVal, &propType);
                if (propType == napi_string) {
                    deviceId = GetWideStringFromNapi(env, devIdVal);
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_fanoutMutex);
    if (!g_fanoutEngine) {
        g_fanoutEngine = std::make_unique<speakerflow::WasapiFanOutEngine>();
    }

    std::string error;
    bool success = g_fanoutEngine->StartCapture(deviceId, error);
    if (!success) {
        napi_throw_error(env, NULL, error.c_str());
        return NULL;
    }

    speakerflow::WasapiCaptureStats stats = g_fanoutEngine->GetCaptureStats();

    napi_value resObj;
    napi_create_object(env, &resObj);

    napi_value boolVal;
    napi_get_boolean(env, true, &boolVal);
    napi_set_named_property(env, resObj, "success", boolVal);

    napi_value srVal, chVal, bpsVal;
    napi_create_uint32(env, stats.sampleRate, &srVal);
    napi_create_uint32(env, stats.channels, &chVal);
    napi_create_uint32(env, stats.bitsPerSample, &bpsVal);
    napi_set_named_property(env, resObj, "sampleRate", srVal);
    napi_set_named_property(env, resObj, "channels", chVal);
    napi_set_named_property(env, resObj, "bitsPerSample", bpsVal);

    napi_value fmtVal, nameVal, devIdVal;
    napi_create_string_utf8(env, stats.formatTag.c_str(), NAPI_AUTO_LENGTH, &fmtVal);
    napi_create_string_utf8(env, stats.deviceFriendlyName.c_str(), NAPI_AUTO_LENGTH, &nameVal);
    napi_create_string_utf8(env, stats.deviceId.c_str(), NAPI_AUTO_LENGTH, &devIdVal);
    napi_set_named_property(env, resObj, "format", fmtVal);
    napi_set_named_property(env, resObj, "deviceFriendlyName", nameVal);
    napi_set_named_property(env, resObj, "deviceId", devIdVal);

    napi_value evVal;
    napi_get_boolean(env, stats.isEventDriven, &evVal);
    napi_set_named_property(env, resObj, "isEventDriven", evVal);

    return resObj;
}

static napi_value Method_EngineStopCapture(napi_env env, napi_callback_info info) {
    std::lock_guard<std::mutex> lock(g_fanoutMutex);
    if (g_fanoutEngine) {
        g_fanoutEngine->StopCapture();
    }
    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value Method_EngineIsCaptureActive(napi_env env, napi_callback_info info) {
    std::lock_guard<std::mutex> lock(g_fanoutMutex);
    bool active = g_fanoutEngine ? g_fanoutEngine->IsCaptureActive() : false;
    napi_value result;
    napi_get_boolean(env, active, &result);
    return result;
}

static napi_value Method_EngineGetCaptureStats(napi_env env, napi_callback_info info) {
    speakerflow::WasapiCaptureStats stats;
    {
        std::lock_guard<std::mutex> lock(g_fanoutMutex);
        if (g_fanoutEngine) {
            stats = g_fanoutEngine->GetCaptureStats();
        }
    }

    napi_value obj;
    napi_create_object(env, &obj);

    napi_value isCapVal, isEvVal;
    napi_get_boolean(env, stats.isCapturing, &isCapVal);
    napi_get_boolean(env, stats.isEventDriven, &isEvVal);
    napi_set_named_property(env, obj, "isCapturing", isCapVal);
    napi_set_named_property(env, obj, "isEventDriven", isEvVal);

    napi_value srVal, chVal, bpsVal;
    napi_create_uint32(env, stats.sampleRate, &srVal);
    napi_create_uint32(env, stats.channels, &chVal);
    napi_create_uint32(env, stats.bitsPerSample, &bpsVal);
    napi_set_named_property(env, obj, "sampleRate", srVal);
    napi_set_named_property(env, obj, "channels", chVal);
    napi_set_named_property(env, obj, "bitsPerSample", bpsVal);

    napi_value fmtVal, devIdVal, devNameVal, lastErrVal;
    napi_create_string_utf8(env, stats.formatTag.c_str(), NAPI_AUTO_LENGTH, &fmtVal);
    napi_create_string_utf8(env, stats.deviceId.c_str(), NAPI_AUTO_LENGTH, &devIdVal);
    napi_create_string_utf8(env, stats.deviceFriendlyName.c_str(), NAPI_AUTO_LENGTH, &devNameVal);
    napi_create_string_utf8(env, stats.lastError.c_str(), NAPI_AUTO_LENGTH, &lastErrVal);
    napi_set_named_property(env, obj, "formatTag", fmtVal);
    napi_set_named_property(env, obj, "deviceId", devIdVal);
    napi_set_named_property(env, obj, "deviceFriendlyName", devNameVal);
    napi_set_named_property(env, obj, "lastError", lastErrVal);

    napi_value framesVal, silentVal, packetsVal;
    napi_create_int64(env, stats.framesCaptured, &framesVal);
    napi_create_int64(env, stats.silentFramesCaptured, &silentVal);
    napi_create_int64(env, stats.packetsCaptured, &packetsVal);
    napi_set_named_property(env, obj, "framesCaptured", framesVal);
    napi_set_named_property(env, obj, "silentFramesCaptured", silentVal);
    napi_set_named_property(env, obj, "packetsCaptured", packetsVal);

    napi_value peakVal, rmsVal;
    napi_create_double(env, static_cast<double>(stats.peakLevel), &peakVal);
    napi_create_double(env, static_cast<double>(stats.rmsLevel), &rmsVal);
    napi_set_named_property(env, obj, "peakLevel", peakVal);
    napi_set_named_property(env, obj, "rmsLevel", rmsVal);

    napi_value underrunVal, overrunVal, ringFramesVal, ringCapVal;
    napi_create_int64(env, stats.ringBufferUnderruns, &underrunVal);
    napi_create_int64(env, stats.ringBufferOverruns, &overrunVal);
    napi_create_int64(env, stats.ringBufferFrames, &ringFramesVal);
    napi_create_int64(env, stats.ringBufferCapacity, &ringCapVal);
    napi_set_named_property(env, obj, "ringBufferUnderruns", underrunVal);
    napi_set_named_property(env, obj, "ringBufferOverruns", overrunVal);
    napi_set_named_property(env, obj, "ringBufferFrames", ringFramesVal);
    napi_set_named_property(env, obj, "ringBufferCapacity", ringCapVal);

    return obj;
}

static napi_value Method_EngineAddOutput(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = { nullptr, nullptr, nullptr, nullptr };
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    std::string branchId;
    std::wstring endpointId;
    bool driftCorrectionEnabled = true;
    double delayMs = 0.0;

    if (argc >= 1 && args[0] != nullptr) {
        napi_valuetype t0;
        napi_typeof(env, args[0], &t0);
        if (t0 == napi_object) {
            napi_value bIdVal, devIdVal, dcVal, delayVal;
            if (napi_get_named_property(env, args[0], "branchId", &bIdVal) == napi_ok) {
                size_t len = 0;
                napi_get_value_string_utf8(env, bIdVal, NULL, 0, &len);
                if (len > 0) {
                    branchId.resize(len);
                    napi_get_value_string_utf8(env, bIdVal, &branchId[0], len + 1, &len);
                }
            }
            if (napi_get_named_property(env, args[0], "deviceId", &devIdVal) == napi_ok) {
                endpointId = GetWideStringFromNapi(env, devIdVal);
            }
            if (napi_get_named_property(env, args[0], "driftCorrectionEnabled", &dcVal) == napi_ok) {
                napi_valuetype dcType;
                napi_typeof(env, dcVal, &dcType);
                if (dcType == napi_boolean) {
                    napi_get_value_bool(env, dcVal, &driftCorrectionEnabled);
                }
            }
            if (napi_get_named_property(env, args[0], "delayMs", &delayVal) == napi_ok) {
                napi_valuetype delayType;
                napi_typeof(env, delayVal, &delayType);
                if (delayType == napi_number) {
                    napi_get_value_double(env, delayVal, &delayMs);
                } else if (delayType != napi_undefined && delayType != napi_null) {
                    napi_throw_error(env, NULL, "delayMs must be a number");
                    return NULL;
                }
            }
        } else if (t0 == napi_string) {
            size_t len = 0;
            napi_get_value_string_utf8(env, args[0], NULL, 0, &len);
            if (len > 0) {
                branchId.resize(len);
                napi_get_value_string_utf8(env, args[0], &branchId[0], len + 1, &len);
            }
            if (argc >= 2 && args[1] != nullptr) {
                endpointId = GetWideStringFromNapi(env, args[1]);
            }
            if (argc >= 3 && args[2] != nullptr) {
                napi_valuetype dcType;
                napi_typeof(env, args[2], &dcType);
                if (dcType == napi_boolean) {
                    napi_get_value_bool(env, args[2], &driftCorrectionEnabled);
                }
            }
            if (argc >= 4 && args[3] != nullptr) {
                napi_valuetype delayType;
                napi_typeof(env, args[3], &delayType);
                if (delayType == napi_number) {
                    napi_get_value_double(env, args[3], &delayMs);
                } else if (delayType != napi_undefined && delayType != napi_null) {
                    napi_throw_error(env, NULL, "delayMs must be a number");
                    return NULL;
                }
            }
        }
    }

    if (branchId.empty()) {
        napi_throw_error(env, NULL, "branchId is required for engineAddOutput");
        return NULL;
    }

    if (std::isnan(delayMs) || std::isinf(delayMs) || delayMs < 0.0 || delayMs > 500.0) {
        napi_throw_error(env, NULL, "delayMs must be a finite number between 0.0 and 500.0 milliseconds.");
        return NULL;
    }

    std::lock_guard<std::mutex> lock(g_fanoutMutex);
    if (!g_fanoutEngine || !g_fanoutEngine->IsCaptureActive()) {
        napi_throw_error(env, NULL, "Cannot add output: WASAPI capture is not active.");
        return NULL;
    }

    std::string error;
    bool success = g_fanoutEngine->AddOutput(branchId, endpointId, error, driftCorrectionEnabled, delayMs);
    if (!success) {
        napi_throw_error(env, NULL, error.c_str());
        return NULL;
    }

    speakerflow::WasapiBranchStats bStats;
    g_fanoutEngine->GetOutputStats(branchId, bStats);

    napi_value resObj;
    napi_create_object(env, &resObj);

    napi_value boolVal;
    napi_get_boolean(env, true, &boolVal);
    napi_set_named_property(env, resObj, "success", boolVal);

    napi_value bIdOut, devIdOut, nameOut, fmtOut;
    napi_create_string_utf8(env, bStats.branchId.c_str(), NAPI_AUTO_LENGTH, &bIdOut);
    napi_create_string_utf8(env, bStats.endpointId.c_str(), NAPI_AUTO_LENGTH, &devIdOut);
    napi_create_string_utf8(env, bStats.deviceFriendlyName.c_str(), NAPI_AUTO_LENGTH, &nameOut);
    napi_create_string_utf8(env, bStats.renderStats.formatTag.c_str(), NAPI_AUTO_LENGTH, &fmtOut);
    napi_set_named_property(env, resObj, "branchId", bIdOut);
    napi_set_named_property(env, resObj, "deviceId", devIdOut);
    napi_set_named_property(env, resObj, "deviceFriendlyName", nameOut);
    napi_set_named_property(env, resObj, "format", fmtOut);

    napi_value srVal, chVal, bpsVal;
    napi_create_uint32(env, bStats.renderStats.sampleRate, &srVal);
    napi_create_uint32(env, bStats.renderStats.channels, &chVal);
    napi_create_uint32(env, bStats.renderStats.bitsPerSample, &bpsVal);
    napi_set_named_property(env, resObj, "sampleRate", srVal);
    napi_set_named_property(env, resObj, "channels", chVal);
    napi_set_named_property(env, resObj, "bitsPerSample", bpsVal);

    napi_value evVal;
    napi_get_boolean(env, bStats.renderStats.isEventDriven, &evVal);
    napi_set_named_property(env, resObj, "isEventDriven", evVal);

    napi_value bufDurVal;
    napi_create_double(env, bStats.renderStats.bufferDurationMs, &bufDurVal);
    napi_set_named_property(env, resObj, "bufferDurationMs", bufDurVal);

    napi_value confDelayVal, effFramesVal;
    napi_create_double(env, bStats.configuredDelayMs, &confDelayVal);
    napi_create_uint32(env, bStats.effectiveDelayFrames, &effFramesVal);
    napi_set_named_property(env, resObj, "configuredDelayMs", confDelayVal);
    napi_set_named_property(env, resObj, "effectiveDelayFrames", effFramesVal);

    return resObj;
}

static napi_value Method_EngineSetBranchDelay(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr, nullptr };
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    std::string branchId;
    double delayMs = 0.0;

    if (argc >= 1 && args[0] != nullptr) {
        napi_valuetype t0;
        napi_typeof(env, args[0], &t0);
        if (t0 == napi_object) {
            napi_value bIdVal, delayVal;
            if (napi_get_named_property(env, args[0], "branchId", &bIdVal) == napi_ok) {
                size_t len = 0;
                napi_get_value_string_utf8(env, bIdVal, NULL, 0, &len);
                if (len > 0) {
                    branchId.resize(len);
                    napi_get_value_string_utf8(env, bIdVal, &branchId[0], len + 1, &len);
                }
            }
            if (napi_get_named_property(env, args[0], "delayMs", &delayVal) == napi_ok) {
                napi_valuetype delayType;
                napi_typeof(env, delayVal, &delayType);
                if (delayType == napi_number) {
                    napi_get_value_double(env, delayVal, &delayMs);
                } else {
                    napi_throw_error(env, NULL, "delayMs must be a number");
                    return NULL;
                }
            }
        } else if (t0 == napi_string) {
            size_t len = 0;
            napi_get_value_string_utf8(env, args[0], NULL, 0, &len);
            if (len > 0) {
                branchId.resize(len);
                napi_get_value_string_utf8(env, args[0], &branchId[0], len + 1, &len);
            }
            if (argc >= 2 && args[1] != nullptr) {
                napi_valuetype t1;
                napi_typeof(env, args[1], &t1);
                if (t1 == napi_number) {
                    napi_get_value_double(env, args[1], &delayMs);
                } else {
                    napi_throw_error(env, NULL, "delayMs must be a number");
                    return NULL;
                }
            } else {
                napi_throw_error(env, NULL, "delayMs argument is required");
                return NULL;
            }
        }
    }

    if (branchId.empty()) {
        napi_throw_error(env, NULL, "branchId is required for engineSetBranchDelay");
        return NULL;
    }

    if (std::isnan(delayMs) || std::isinf(delayMs) || delayMs < 0.0 || delayMs > 500.0) {
        napi_throw_error(env, NULL, "delayMs must be a finite number between 0.0 and 500.0 milliseconds.");
        return NULL;
    }

    std::string error;
    bool success = false;
    {
        std::lock_guard<std::mutex> lock(g_fanoutMutex);
        if (g_fanoutEngine) {
            success = g_fanoutEngine->SetBranchDelay(branchId, delayMs, error);
        } else {
            error = "Fan-out engine is not initialized.";
        }
    }

    if (!success) {
        napi_throw_error(env, NULL, error.c_str());
        return NULL;
    }

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

static napi_value Method_EngineGetBranchDelay(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    std::string branchId;
    if (argc > 0 && args[0] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], NULL, 0, &len);
        if (len > 0) {
            branchId.resize(len);
            napi_get_value_string_utf8(env, args[0], &branchId[0], len + 1, &len);
        }
    }

    if (branchId.empty()) {
        napi_throw_error(env, NULL, "branchId is required for engineGetBranchDelay");
        return NULL;
    }

    double delayMs = 0.0;
    size_t delayFrames = 0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_fanoutMutex);
        if (g_fanoutEngine) {
            found = g_fanoutEngine->GetBranchDelay(branchId, delayMs, delayFrames);
        }
    }

    if (!found) {
        napi_throw_error(env, NULL, ("Branch not found: " + branchId).c_str());
        return NULL;
    }

    napi_value obj;
    napi_create_object(env, &obj);
    napi_value msVal, framesVal;
    napi_create_double(env, delayMs, &msVal);
    napi_create_uint32(env, static_cast<uint32_t>(delayFrames), &framesVal);
    napi_set_named_property(env, obj, "configuredDelayMs", msVal);
    napi_set_named_property(env, obj, "effectiveDelayFrames", framesVal);
    return obj;
}

static napi_value Method_EngineRemoveOutput(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    std::string branchId;
    if (argc > 0 && args[0] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], NULL, 0, &len);
        if (len > 0) {
            branchId.resize(len);
            napi_get_value_string_utf8(env, args[0], &branchId[0], len + 1, &len);
        }
    }

    if (branchId.empty()) {
        napi_throw_error(env, NULL, "branchId is required for engineRemoveOutput");
        return NULL;
    }

    std::lock_guard<std::mutex> lock(g_fanoutMutex);
    bool removed = g_fanoutEngine ? g_fanoutEngine->RemoveOutput(branchId) : false;
    napi_value result;
    napi_get_boolean(env, removed, &result);
    return result;
}

static napi_value Method_EngineSetDriftCorrectionEnabled(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr, nullptr };
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    std::string branchId;
    if (argc > 0 && args[0] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], NULL, 0, &len);
        if (len > 0) {
            branchId.resize(len);
            napi_get_value_string_utf8(env, args[0], &branchId[0], len + 1, &len);
        }
    }

    if (branchId.empty()) {
        napi_throw_error(env, NULL, "branchId is required for engineSetDriftCorrectionEnabled");
        return NULL;
    }

    bool enabled = true;
    if (argc > 1 && args[1] != nullptr) {
        napi_get_value_bool(env, args[1], &enabled);
    }

    std::lock_guard<std::mutex> lock(g_fanoutMutex);
    bool success = g_fanoutEngine ? g_fanoutEngine->SetBranchDriftCorrectionEnabled(branchId, enabled) : false;
    napi_value result;
    napi_get_boolean(env, success, &result);
    return result;
}

static napi_value Method_EngineIsDriftCorrectionEnabled(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    std::string branchId;
    if (argc > 0 && args[0] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], NULL, 0, &len);
        if (len > 0) {
            branchId.resize(len);
            napi_get_value_string_utf8(env, args[0], &branchId[0], len + 1, &len);
        }
    }

    if (branchId.empty()) {
        napi_throw_error(env, NULL, "branchId is required for engineIsDriftCorrectionEnabled");
        return NULL;
    }

    std::lock_guard<std::mutex> lock(g_fanoutMutex);
    bool enabled = g_fanoutEngine ? g_fanoutEngine->IsBranchDriftCorrectionEnabled(branchId) : false;
    napi_value result;
    napi_get_boolean(env, enabled, &result);
    return result;
}

static napi_value Method_EngineGetOutputStats(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, NULL, NULL);

    std::string branchId;
    if (argc > 0 && args[0] != nullptr) {
        size_t len = 0;
        napi_get_value_string_utf8(env, args[0], NULL, 0, &len);
        if (len > 0) {
            branchId.resize(len);
            napi_get_value_string_utf8(env, args[0], &branchId[0], len + 1, &len);
        }
    }

    if (branchId.empty()) {
        napi_throw_error(env, NULL, "branchId is required for engineGetOutputStats");
        return NULL;
    }

    speakerflow::WasapiBranchStats bStats;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_fanoutMutex);
        if (g_fanoutEngine) {
            found = g_fanoutEngine->GetOutputStats(branchId, bStats);
        }
    }

    if (!found) {
        napi_throw_error(env, NULL, ("Branch not found: " + branchId).c_str());
        return NULL;
    }

    napi_value obj;
    napi_create_object(env, &obj);

    napi_value bIdVal, devIdVal, nameVal;
    napi_create_string_utf8(env, bStats.branchId.c_str(), NAPI_AUTO_LENGTH, &bIdVal);
    napi_create_string_utf8(env, bStats.endpointId.c_str(), NAPI_AUTO_LENGTH, &devIdVal);
    napi_create_string_utf8(env, bStats.deviceFriendlyName.c_str(), NAPI_AUTO_LENGTH, &nameVal);
    napi_set_named_property(env, obj, "branchId", bIdVal);
    napi_set_named_property(env, obj, "deviceId", devIdVal);
    napi_set_named_property(env, obj, "deviceFriendlyName", nameVal);

    napi_value actVal, isRenVal, isEvVal, lifeVal, lifeNameVal;
    napi_get_boolean(env, bStats.active, &actVal);
    napi_get_boolean(env, bStats.renderStats.isRendering, &isRenVal);
    napi_get_boolean(env, bStats.renderStats.isEventDriven, &isEvVal);
    napi_create_uint32(env, static_cast<uint32_t>(bStats.lifecycleState), &lifeVal);
    napi_create_string_utf8(env, bStats.lifecycleStateName.c_str(), NAPI_AUTO_LENGTH, &lifeNameVal);
    napi_set_named_property(env, obj, "active", actVal);
    napi_set_named_property(env, obj, "isRendering", isRenVal);
    napi_set_named_property(env, obj, "isEventDriven", isEvVal);
    napi_set_named_property(env, obj, "lifecycleState", lifeVal);
    napi_set_named_property(env, obj, "lifecycleStateName", lifeNameVal);

    napi_value lossErrVal, recAttVal, recHrVal, recPendVal;
    napi_create_uint32(env, bStats.lastDeviceLossError, &lossErrVal);
    napi_create_uint32(env, bStats.recoveryAttemptCount, &recAttVal);
    napi_create_uint32(env, bStats.lastRecoveryHresult, &recHrVal);
    napi_get_boolean(env, bStats.recoveryPending, &recPendVal);
    napi_set_named_property(env, obj, "lastDeviceLossHresult", lossErrVal);
    napi_set_named_property(env, obj, "lastDeviceLossError", lossErrVal);
    napi_set_named_property(env, obj, "recoveryAttemptCount", recAttVal);
    napi_set_named_property(env, obj, "lastRecoveryHresult", recHrVal);
    napi_set_named_property(env, obj, "recoveryPending", recPendVal);

    napi_value srVal, chVal, bpsVal;
    napi_create_uint32(env, bStats.renderStats.sampleRate, &srVal);
    napi_create_uint32(env, bStats.renderStats.channels, &chVal);
    napi_create_uint32(env, bStats.renderStats.bitsPerSample, &bpsVal);
    napi_set_named_property(env, obj, "sampleRate", srVal);
    napi_set_named_property(env, obj, "channels", chVal);
    napi_set_named_property(env, obj, "bitsPerSample", bpsVal);

    napi_value fmtVal, lastErrVal;
    napi_create_string_utf8(env, bStats.renderStats.formatTag.c_str(), NAPI_AUTO_LENGTH, &fmtVal);
    napi_create_string_utf8(env, bStats.renderStats.lastError.c_str(), NAPI_AUTO_LENGTH, &lastErrVal);
    napi_set_named_property(env, obj, "formatTag", fmtVal);
    napi_set_named_property(env, obj, "lastError", lastErrVal);

    napi_value framesVal, silentVal;
    napi_create_int64(env, bStats.renderStats.framesRendered, &framesVal);
    napi_create_int64(env, bStats.renderStats.silentFramesRendered, &silentVal);
    napi_set_named_property(env, obj, "framesRendered", framesVal);
    napi_set_named_property(env, obj, "silentFramesRendered", silentVal);

    napi_value underrunVal, overrunVal, ringFramesVal, ringCapVal;
    napi_create_int64(env, bStats.renderStats.bufferUnderruns, &underrunVal);
    napi_create_int64(env, bStats.renderStats.bufferOverruns, &overrunVal);
    napi_create_int64(env, bStats.renderStats.ringBufferFrames, &ringFramesVal);
    napi_create_int64(env, bStats.renderStats.ringBufferCapacity, &ringCapVal);
    napi_set_named_property(env, obj, "bufferUnderruns", underrunVal);
    napi_set_named_property(env, obj, "bufferOverruns", overrunVal);
    napi_set_named_property(env, obj, "ringBufferFrames", ringFramesVal);
    napi_set_named_property(env, obj, "ringBufferCapacity", ringCapVal);

    napi_value bufDurVal, ringOccVal;
    napi_create_double(env, bStats.renderStats.bufferDurationMs, &bufDurVal);
    napi_create_double(env, bStats.renderStats.ringBufferOccupancyMs, &ringOccVal);
    napi_set_named_property(env, obj, "bufferDurationMs", bufDurVal);
    napi_set_named_property(env, obj, "ringBufferOccupancyMs", ringOccVal);

    napi_value occFramesVal, occMsVal, driftVal, occErrVal, stableVal, slopeVal, stateVal, ratioVal;
    napi_create_int64(env, static_cast<int64_t>(bStats.occupancyFrames), &occFramesVal);
    napi_create_double(env, bStats.occupancyMs, &occMsVal);
    napi_create_double(env, bStats.driftPpm, &driftVal);
    napi_create_int64(env, bStats.occupancyErrorFrames, &occErrVal);
    napi_get_boolean(env, bStats.estimatorStable, &stableVal);
    napi_create_double(env, bStats.occupancySlope, &slopeVal);
    napi_create_string_utf8(env, bStats.renderState.c_str(), NAPI_AUTO_LENGTH, &stateVal);
    napi_create_double(env, bStats.renderStats.resampleRatioMultiplier, &ratioVal);
    napi_set_named_property(env, obj, "occupancyFrames", occFramesVal);
    napi_set_named_property(env, obj, "occupancyMs", occMsVal);
    napi_set_named_property(env, obj, "driftPpm", driftVal);
    napi_set_named_property(env, obj, "occupancyErrorFrames", occErrVal);
    napi_set_named_property(env, obj, "estimatorStable", stableVal);
    napi_set_named_property(env, obj, "occupancySlope", slopeVal);
    napi_set_named_property(env, obj, "renderState", stateVal);
    napi_set_named_property(env, obj, "resampleRatioMultiplier", ratioVal);

    napi_value trueDriftVal, filtDriftVal, ffVal, fbVal, targetRatioVal, clampedVal, driftCorrVal;
    napi_create_double(env, bStats.trueDriftPpm, &trueDriftVal);
    napi_create_double(env, bStats.filteredDriftPpm, &filtDriftVal);
    napi_create_double(env, bStats.feedforwardCorrection, &ffVal);
    napi_create_double(env, bStats.feedbackCorrection, &fbVal);
    napi_create_double(env, bStats.targetMultiplier, &targetRatioVal);
    napi_get_boolean(env, bStats.isClamped, &clampedVal);
    napi_get_boolean(env, bStats.driftCorrectionEnabled, &driftCorrVal);
    napi_set_named_property(env, obj, "trueDriftPpm", trueDriftVal);
    napi_set_named_property(env, obj, "filteredDriftPpm", filtDriftVal);
    napi_set_named_property(env, obj, "feedforwardCorrection", ffVal);
    napi_set_named_property(env, obj, "feedbackCorrection", fbVal);
    napi_set_named_property(env, obj, "targetMultiplier", targetRatioVal);
    napi_set_named_property(env, obj, "isClamped", clampedVal);
    napi_set_named_property(env, obj, "driftCorrectionEnabled", driftCorrVal);

    napi_value confDelayVal, effFramesVal;
    napi_create_double(env, bStats.configuredDelayMs, &confDelayVal);
    napi_create_uint32(env, bStats.effectiveDelayFrames, &effFramesVal);
    napi_set_named_property(env, obj, "configuredDelayMs", confDelayVal);
    napi_set_named_property(env, obj, "effectiveDelayFrames", effFramesVal);

    return obj;
}

static napi_value Method_EngineGetStatus(napi_env env, napi_callback_info info) {
    speakerflow::WasapiFanOutEngineStatus status;
    {
        std::lock_guard<std::mutex> lock(g_fanoutMutex);
        if (g_fanoutEngine) {
            status = g_fanoutEngine->GetStatus();
        }
    }

    napi_value obj;
    napi_create_object(env, &obj);

    napi_value isCapVal, lastErrVal;
    napi_get_boolean(env, status.isCapturing, &isCapVal);
    napi_create_string_utf8(env, status.lastError.c_str(), NAPI_AUTO_LENGTH, &lastErrVal);
    napi_set_named_property(env, obj, "isCapturing", isCapVal);
    napi_set_named_property(env, obj, "lastError", lastErrVal);

    napi_value branchArr;
    napi_create_array_with_length(env, status.branches.size(), &branchArr);
    for (size_t i = 0; i < status.branches.size(); ++i) {
        const auto& b = status.branches[i];
        napi_value bObj;
        napi_create_object(env, &bObj);

        napi_value bIdVal, devIdVal, nameVal, actVal;
        napi_create_string_utf8(env, b.branchId.c_str(), NAPI_AUTO_LENGTH, &bIdVal);
        napi_create_string_utf8(env, b.endpointId.c_str(), NAPI_AUTO_LENGTH, &devIdVal);
        napi_create_string_utf8(env, b.deviceFriendlyName.c_str(), NAPI_AUTO_LENGTH, &nameVal);
        napi_get_boolean(env, b.active, &actVal);
        napi_set_named_property(env, bObj, "branchId", bIdVal);
        napi_set_named_property(env, bObj, "deviceId", devIdVal);
        napi_set_named_property(env, bObj, "deviceFriendlyName", nameVal);
        napi_set_named_property(env, bObj, "active", actVal);

        napi_value bLifeVal, bLifeNameVal, bLossErrVal, bRecAttVal, bRecHrVal, bRecPendVal;
        napi_create_uint32(env, static_cast<uint32_t>(b.lifecycleState), &bLifeVal);
        napi_create_string_utf8(env, b.lifecycleStateName.c_str(), NAPI_AUTO_LENGTH, &bLifeNameVal);
        napi_create_uint32(env, b.lastDeviceLossError, &bLossErrVal);
        napi_create_uint32(env, b.recoveryAttemptCount, &bRecAttVal);
        napi_create_uint32(env, b.lastRecoveryHresult, &bRecHrVal);
        napi_get_boolean(env, b.recoveryPending, &bRecPendVal);
        napi_set_named_property(env, bObj, "lifecycleState", bLifeVal);
        napi_set_named_property(env, bObj, "lifecycleStateName", bLifeNameVal);
        napi_set_named_property(env, bObj, "lastDeviceLossHresult", bLossErrVal);
        napi_set_named_property(env, bObj, "lastDeviceLossError", bLossErrVal);
        napi_set_named_property(env, bObj, "recoveryAttemptCount", bRecAttVal);
        napi_set_named_property(env, bObj, "lastRecoveryHresult", bRecHrVal);
        napi_set_named_property(env, bObj, "recoveryPending", bRecPendVal);

        napi_value framesVal, underVal, overVal;
        napi_create_int64(env, b.renderStats.framesRendered, &framesVal);
        napi_create_int64(env, b.renderStats.bufferUnderruns, &underVal);
        napi_create_int64(env, b.renderStats.bufferOverruns, &overVal);
        napi_set_named_property(env, bObj, "framesRendered", framesVal);
        napi_set_named_property(env, bObj, "bufferUnderruns", underVal);
        napi_set_named_property(env, bObj, "bufferOverruns", overVal);

        napi_value occFramesVal, occMsVal, driftVal, occErrVal, stableVal, slopeVal, stateVal, bRatioVal;
        napi_create_int64(env, static_cast<int64_t>(b.occupancyFrames), &occFramesVal);
        napi_create_double(env, b.occupancyMs, &occMsVal);
        napi_create_double(env, b.driftPpm, &driftVal);
        napi_create_int64(env, b.occupancyErrorFrames, &occErrVal);
        napi_get_boolean(env, b.estimatorStable, &stableVal);
        napi_create_double(env, b.occupancySlope, &slopeVal);
        napi_create_string_utf8(env, b.renderState.c_str(), NAPI_AUTO_LENGTH, &stateVal);
        napi_create_double(env, b.renderStats.resampleRatioMultiplier, &bRatioVal);
        napi_set_named_property(env, bObj, "occupancyFrames", occFramesVal);
        napi_set_named_property(env, bObj, "occupancyMs", occMsVal);
        napi_set_named_property(env, bObj, "driftPpm", driftVal);
        napi_set_named_property(env, bObj, "occupancyErrorFrames", occErrVal);
        napi_set_named_property(env, bObj, "estimatorStable", stableVal);
        napi_set_named_property(env, bObj, "occupancySlope", slopeVal);
        napi_set_named_property(env, bObj, "renderState", stateVal);
        napi_set_named_property(env, bObj, "resampleRatioMultiplier", bRatioVal);

        napi_value bTrueDriftVal, bFiltDriftVal, bFfVal, bFbVal, bTargetRatioVal, bClampedVal, bDriftCorrVal;
        napi_create_double(env, b.trueDriftPpm, &bTrueDriftVal);
        napi_create_double(env, b.filteredDriftPpm, &bFiltDriftVal);
        napi_create_double(env, b.feedforwardCorrection, &bFfVal);
        napi_create_double(env, b.feedbackCorrection, &bFbVal);
        napi_create_double(env, b.targetMultiplier, &bTargetRatioVal);
        napi_get_boolean(env, b.isClamped, &bClampedVal);
        napi_get_boolean(env, b.driftCorrectionEnabled, &bDriftCorrVal);
        napi_set_named_property(env, bObj, "trueDriftPpm", bTrueDriftVal);
        napi_set_named_property(env, bObj, "filteredDriftPpm", bFiltDriftVal);
        napi_set_named_property(env, bObj, "feedforwardCorrection", bFfVal);
        napi_set_named_property(env, bObj, "feedbackCorrection", bFbVal);
        napi_set_named_property(env, bObj, "targetMultiplier", bTargetRatioVal);
        napi_set_named_property(env, bObj, "isClamped", bClampedVal);
        napi_set_named_property(env, bObj, "driftCorrectionEnabled", bDriftCorrVal);

        napi_value bConfDelayVal, bEffFramesVal;
        napi_create_double(env, b.configuredDelayMs, &bConfDelayVal);
        napi_create_uint32(env, b.effectiveDelayFrames, &bEffFramesVal);
        napi_set_named_property(env, bObj, "configuredDelayMs", bConfDelayVal);
        napi_set_named_property(env, bObj, "effectiveDelayFrames", bEffFramesVal);

        napi_set_element(env, branchArr, (uint32_t)i, bObj);
    }
    napi_set_named_property(env, obj, "branches", branchArr);

    return obj;
}

static napi_value Method_EngineShutdown(napi_env env, napi_callback_info info) {
    std::lock_guard<std::mutex> lock(g_fanoutMutex);
    if (g_fanoutEngine) {
        g_fanoutEngine->Shutdown();
        g_fanoutEngine.reset();
    }
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

    napi_create_function(env, NULL, 0, Method_GetEndpointVolume, NULL, &fn);
    napi_set_named_property(env, exports, "getEndpointVolume", fn);

    napi_create_function(env, NULL, 0, Method_GetEndpointMute, NULL, &fn);
    napi_set_named_property(env, exports, "getEndpointMute", fn);

    napi_create_function(env, NULL, 0, Method_SetApplicationOutput, NULL, &fn);
    napi_set_named_property(env, exports, "setApplicationOutput", fn);

    napi_create_function(env, NULL, 0, Method_GetApplicationOutput, NULL, &fn);
    napi_set_named_property(env, exports, "getApplicationOutput", fn);

    napi_create_function(env, NULL, 0, Method_ClearApplicationOutput, NULL, &fn);
    napi_set_named_property(env, exports, "clearApplicationOutput", fn);

    napi_create_function(env, NULL, 0, Method_StartMonitoring, NULL, &fn);
    napi_set_named_property(env, exports, "startMonitoring", fn);

    napi_create_function(env, NULL, 0, Method_StopMonitoring, NULL, &fn);
    napi_set_named_property(env, exports, "stopMonitoring", fn);

    napi_create_function(env, NULL, 0, Method_Destroy, NULL, &fn);
    napi_set_named_property(env, exports, "destroy", fn);

    napi_create_function(env, NULL, 0, Method_CaptureStart, NULL, &fn);
    napi_set_named_property(env, exports, "captureStart", fn);

    napi_create_function(env, NULL, 0, Method_CaptureStop, NULL, &fn);
    napi_set_named_property(env, exports, "captureStop", fn);

    napi_create_function(env, NULL, 0, Method_CaptureIsActive, NULL, &fn);
    napi_set_named_property(env, exports, "captureIsActive", fn);

    napi_create_function(env, NULL, 0, Method_CaptureGetStats, NULL, &fn);
    napi_set_named_property(env, exports, "captureGetStats", fn);

    napi_create_function(env, NULL, 0, Method_RenderStart, NULL, &fn);
    napi_set_named_property(env, exports, "renderStart", fn);

    napi_create_function(env, NULL, 0, Method_RenderStop, NULL, &fn);
    napi_set_named_property(env, exports, "renderStop", fn);

    napi_create_function(env, NULL, 0, Method_RenderIsActive, NULL, &fn);
    napi_set_named_property(env, exports, "renderIsActive", fn);

    napi_create_function(env, NULL, 0, Method_RenderGetStats, NULL, &fn);
    napi_set_named_property(env, exports, "renderGetStats", fn);

    // Multi-output Fan-Out Engine exports (Phase 2C)
    napi_create_function(env, NULL, 0, Method_EngineStartCapture, NULL, &fn);
    napi_set_named_property(env, exports, "engineStartCapture", fn);

    napi_create_function(env, NULL, 0, Method_EngineStopCapture, NULL, &fn);
    napi_set_named_property(env, exports, "engineStopCapture", fn);

    napi_create_function(env, NULL, 0, Method_EngineIsCaptureActive, NULL, &fn);
    napi_set_named_property(env, exports, "engineIsCaptureActive", fn);

    napi_create_function(env, NULL, 0, Method_EngineGetCaptureStats, NULL, &fn);
    napi_set_named_property(env, exports, "engineGetCaptureStats", fn);

    napi_create_function(env, NULL, 0, Method_EngineAddOutput, NULL, &fn);
    napi_set_named_property(env, exports, "engineAddOutput", fn);

    napi_create_function(env, NULL, 0, Method_EngineRemoveOutput, NULL, &fn);
    napi_set_named_property(env, exports, "engineRemoveOutput", fn);

    napi_create_function(env, NULL, 0, Method_EngineSetDriftCorrectionEnabled, NULL, &fn);
    napi_set_named_property(env, exports, "engineSetDriftCorrectionEnabled", fn);

    napi_create_function(env, NULL, 0, Method_EngineIsDriftCorrectionEnabled, NULL, &fn);
    napi_set_named_property(env, exports, "engineIsDriftCorrectionEnabled", fn);

    napi_create_function(env, NULL, 0, Method_EngineGetOutputStats, NULL, &fn);
    napi_set_named_property(env, exports, "engineGetOutputStats", fn);

    napi_create_function(env, NULL, 0, Method_EngineGetStatus, NULL, &fn);
    napi_set_named_property(env, exports, "engineGetStatus", fn);

    napi_create_function(env, NULL, 0, Method_EngineSetBranchDelay, NULL, &fn);
    napi_set_named_property(env, exports, "engineSetBranchDelay", fn);
    napi_set_named_property(env, exports, "engineSetOutputDelay", fn);

    napi_create_function(env, NULL, 0, Method_EngineGetBranchDelay, NULL, &fn);
    napi_set_named_property(env, exports, "engineGetBranchDelay", fn);
    napi_set_named_property(env, exports, "engineGetOutputDelay", fn);

    napi_create_function(env, NULL, 0, Method_EngineShutdown, NULL, &fn);
    napi_set_named_property(env, exports, "engineShutdown", fn);

    return exports;
}
