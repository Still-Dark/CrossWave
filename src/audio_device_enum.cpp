#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <combaseapi.h>
#include <stdexcept>
#include "../include/audio_device_enum.h"

AudioDeviceEnumerator::AudioDeviceEnumerator()
{
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&m_pEnumerator)
    );
    if (FAILED(hr))
        throw std::runtime_error("Failed to create IMMDeviceEnumerator");
}

AudioDeviceEnumerator::~AudioDeviceEnumerator()
{
    if (m_pEnumerator)
    {
        m_pEnumerator->Release();
        m_pEnumerator = nullptr;
    }
}

std::vector<AudioDeviceInfo> AudioDeviceEnumerator::GetCaptureDevices() const
{
    return EnumerateDevices(eCapture);
}

std::vector<AudioDeviceInfo> AudioDeviceEnumerator::GetRenderDevices() const
{
    return EnumerateDevices(eRender);
}

std::wstring AudioDeviceEnumerator::GetDefaultCaptureDeviceId() const
{
    return GetDefaultDeviceId(eCapture);
}

std::wstring AudioDeviceEnumerator::GetDefaultRenderDeviceId() const
{
    return GetDefaultDeviceId(eRender);
}

std::vector<AudioDeviceInfo> AudioDeviceEnumerator::EnumerateDevices(EDataFlow flow) const
{
    std::vector<AudioDeviceInfo> devices;
    if (!m_pEnumerator) return devices;

    IMMDeviceCollection* pCollection = nullptr;
    HRESULT hr = m_pEnumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) return devices;

    // Get default device ID for comparison
    std::wstring defaultId = GetDefaultDeviceId(flow);

    UINT count = 0;
    pCollection->GetCount(&count);

    for (UINT i = 0; i < count; ++i)
    {
        IMMDevice* pDevice = nullptr;
        if (FAILED(pCollection->Item(i, &pDevice))) continue;

        LPWSTR pwszId = nullptr;
        pDevice->GetId(&pwszId);

        AudioDeviceInfo info;
        if (pwszId)
        {
            info.id = pwszId;
            CoTaskMemFree(pwszId);
        }
        info.isDefault = (info.id == defaultId);

        // Get friendly name via property store
        IPropertyStore* pProps = nullptr;
        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProps)))
        {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(pProps->GetValue(PKEY_Device_FriendlyName, &varName)))
            {
                if (varName.vt == VT_LPWSTR)
                    info.name = varName.pwszVal;
            }
            PropVariantClear(&varName);
            pProps->Release();
        }

        devices.push_back(info);
        pDevice->Release();
    }

    pCollection->Release();
    return devices;
}

std::wstring AudioDeviceEnumerator::GetDefaultDeviceId(EDataFlow flow) const
{
    if (!m_pEnumerator) return {};

    IMMDevice* pDevice = nullptr;
    HRESULT hr = m_pEnumerator->GetDefaultAudioEndpoint(flow, eConsole, &pDevice);
    if (FAILED(hr)) return {};

    LPWSTR pwszId = nullptr;
    pDevice->GetId(&pwszId);

    std::wstring id;
    if (pwszId)
    {
        id = pwszId;
        CoTaskMemFree(pwszId);
    }
    pDevice->Release();
    return id;
}
