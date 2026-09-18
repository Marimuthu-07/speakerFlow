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
#include <cmath>

static bool g_com_initialized_by_us = false;

static std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr || !*wstr) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (sizeNeeded <= 0) return "";
    std::string str(sizeNeeded - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], sizeNeeded, NULL, NULL);
    return str;
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

    napi_create_function(env, NULL, 0, Method_Destroy, NULL, &fn);
    napi_set_named_property(env, exports, "destroy", fn);

    return exports;
}
