#include "daemon/audio/devices.h"

#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include "common/strings.h"

using Microsoft::WRL::ComPtr;

namespace wa {

std::vector<AudioDevice> list_capture_devices() {
    std::vector<AudioDevice> out;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        if (com && hr != RPC_E_CHANGED_MODE) CoUninitialize();
        return out;
    }

    std::wstring default_id;
    {
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &def))) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(def->GetId(&id)) && id) {
                default_id = id;
                CoTaskMemFree(id);
            }
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection))) {
        if (com && hr != RPC_E_CHANGED_MODE) CoUninitialize();
        return out;
    }
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;
        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id)) || !id) continue;
        AudioDevice d;
        d.id = to_utf8(id);
        d.is_default = default_id == id;
        CoTaskMemFree(id);

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR && var.pwszVal) {
                d.name = to_utf8(var.pwszVal);
            }
            PropVariantClear(&var);
        }
        if (d.name.empty()) d.name = d.id;
        out.push_back(std::move(d));
    }
    if (com && hr != RPC_E_CHANGED_MODE) CoUninitialize();
    return out;
}

}  // namespace wa
