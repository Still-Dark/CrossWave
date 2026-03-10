#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <string>
#include <vector>

/**
 * @brief Describes a WASAPI audio endpoint device.
 */
struct AudioDeviceInfo
{
    std::wstring id;        // IMMDevice ID (used with IMMDeviceEnumerator)
    std::wstring name;      // Human-readable friendly name
    bool         isDefault; // Whether this is the system default device
};

/**
 * @brief Enumerates WASAPI capture (microphone) and render (speaker/headphone) endpoints.
 *
 * Wraps IMMDeviceEnumerator. Must be called after CoInitializeEx().
 */
class AudioDeviceEnumerator
{
public:
    AudioDeviceEnumerator();
    ~AudioDeviceEnumerator();

    // Prevent copying
    AudioDeviceEnumerator(const AudioDeviceEnumerator&) = delete;
    AudioDeviceEnumerator& operator=(const AudioDeviceEnumerator&) = delete;

    /**
     * @brief Returns all active capture (microphone) endpoints.
     */
    std::vector<AudioDeviceInfo> GetCaptureDevices() const;

    /**
     * @brief Returns all active render (speaker/headphone) endpoints.
     */
    std::vector<AudioDeviceInfo> GetRenderDevices() const;

    /**
     * @brief Returns the default capture device ID.
     */
    std::wstring GetDefaultCaptureDeviceId() const;

    /**
     * @brief Returns the default render device ID.
     */
    std::wstring GetDefaultRenderDeviceId() const;

private:
    std::vector<AudioDeviceInfo> EnumerateDevices(EDataFlow flow) const;
    std::wstring GetDefaultDeviceId(EDataFlow flow) const;

    IMMDeviceEnumerator* m_pEnumerator = nullptr;
};
