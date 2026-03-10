#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>
#include <cstring>
#include <stdexcept>
#include <cassert>
#include "../include/audio_capture.h"

// Helper macro for checking WASAPI HRESULTs
#define RETURN_ON_FAIL(hr, msg) \
    if (FAILED(hr)) { m_lastError = (msg); return false; }

AudioCapture::AudioCapture()  = default;
AudioCapture::~AudioCapture() { Stop(); }

bool AudioCapture::Init(const std::wstring& deviceId,
                        int sampleRate,
                        int bufferFrames,
                        ThreadSafeQueue<AudioBuffer>* outQueue)
{
    m_sampleRate   = sampleRate;
    m_bufferFrames = bufferFrames;
    m_outQueue     = outQueue;

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum));
    RETURN_ON_FAIL(hr, "CoCreateInstance(IMMDeviceEnumerator) failed");

    if (deviceId.empty())
    {
        hr = pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &m_pDevice);
    }
    else
    {
        hr = pEnum->GetDevice(deviceId.c_str(), &m_pDevice);
    }
    pEnum->Release();
    RETURN_ON_FAIL(hr, "GetAudioEndpoint (capture) failed");

    // Try exclusive mode first
    if (InitExclusive(m_pDevice, sampleRate, bufferFrames))
    {
        m_exclusiveMode = true;
        m_actualSampleRate = sampleRate;
        return true;
    }

    // Fallback to shared mode
    m_lastError.clear();
    if (InitShared(m_pDevice))
    {
        m_exclusiveMode = false;
        m_actualSampleRate = sampleRate;
        return true;
    }

    return false;
}

bool AudioCapture::InitExclusive(IMMDevice* pDevice, int sampleRate, int bufferFrames)
{
    HRESULT hr = pDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&m_pAudioClient));
    RETURN_ON_FAIL(hr, "Activate IAudioClient (exclusive capture) failed");

    // Define desired format: float32, mono, 48 kHz
    WAVEFORMATEX wfx         = {};
    wfx.wFormatTag           = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels            = 1;
    wfx.nSamplesPerSec       = static_cast<DWORD>(sampleRate);
    wfx.wBitsPerSample       = 32;
    wfx.nBlockAlign          = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec      = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize               = 0;

    // Buffer duration request: bufferFrames / sampleRate in 100-ns units
    REFERENCE_TIME bufDur = static_cast<REFERENCE_TIME>(
        (double)bufferFrames / sampleRate * 10000000.0);

    m_hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_hEvent) RETURN_ON_FAIL(E_FAIL, "CreateEvent failed");

    hr = m_pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        bufDur, bufDur, &wfx, nullptr);

    if (FAILED(hr))
    {
        // Clean up so fallback can try
        if (m_hEvent) { CloseHandle(m_hEvent); m_hEvent = nullptr; }
        m_pAudioClient->Release(); m_pAudioClient = nullptr;
        m_lastError = "WASAPI exclusive mode not supported, falling back to shared";
        return false;
    }

    hr = m_pAudioClient->SetEventHandle(m_hEvent);
    RETURN_ON_FAIL(hr, "SetEventHandle failed");

    hr = m_pAudioClient->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(&m_pCaptureClient));
    RETURN_ON_FAIL(hr, "GetService(IAudioCaptureClient) failed");

    // Calculate latency
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    m_pAudioClient->GetDevicePeriod(&defPeriod, &minPeriod);
    m_latencyMs = static_cast<double>(minPeriod) / 10000.0;
    return true;
}

bool AudioCapture::InitShared(IMMDevice* pDevice)
{
    HRESULT hr = pDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&m_pAudioClient));
    RETURN_ON_FAIL(hr, "Activate IAudioClient (shared capture) failed");

    // Get the mix format that the shared-mode engine is already running at
    WAVEFORMATEX* pMixFmt = nullptr;
    hr = m_pAudioClient->GetMixFormat(&pMixFmt);
    RETURN_ON_FAIL(hr, "GetMixFormat failed");

    m_actualSampleRate = pMixFmt->nSamplesPerSec;

    REFERENCE_TIME bufDur = 0;
    m_pAudioClient->GetDevicePeriod(&bufDur, nullptr);

    m_hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_hEvent)
    {
        CoTaskMemFree(pMixFmt);
        RETURN_ON_FAIL(E_FAIL, "CreateEvent (shared) failed");
    }

    hr = m_pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        bufDur, 0, pMixFmt, nullptr);
    CoTaskMemFree(pMixFmt);
    RETURN_ON_FAIL(hr, "Initialize shared capture failed");

    hr = m_pAudioClient->SetEventHandle(m_hEvent);
    RETURN_ON_FAIL(hr, "SetEventHandle (shared) failed");

    hr = m_pAudioClient->GetService(
        __uuidof(IAudioCaptureClient),
        reinterpret_cast<void**>(&m_pCaptureClient));
    RETURN_ON_FAIL(hr, "GetService(IAudioCaptureClient) shared failed");

    REFERENCE_TIME period = 0;
    m_pAudioClient->GetDevicePeriod(&period, nullptr);
    m_latencyMs = static_cast<double>(period) / 10000.0;
    return true;
}

void AudioCapture::Start()
{
    if (m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread(&AudioCapture::CaptureLoop, this);

    // Boost thread priority for audio
    SetThreadPriority(m_thread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
}

void AudioCapture::Stop()
{
    m_running.store(false);
    if (m_hEvent) SetEvent(m_hEvent); // Wake blocking WaitForSingleObject
    if (m_thread.joinable()) m_thread.join();
    Cleanup();
}

void AudioCapture::CaptureLoop()
{
    // Promote to Pro Audio MMCSS task
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    if (FAILED(m_pAudioClient->Start()))
    {
        m_running.store(false);
        if (hTask) AvRevertMmThreadCharacteristics(hTask);
        return;
    }

    while (m_running.load())
    {
        DWORD waitResult = WaitForSingleObject(m_hEvent, 200);
        if (!m_running.load()) break;
        if (waitResult != WAIT_OBJECT_0) continue;

        UINT32 packetSize = 0;
        while (SUCCEEDED(m_pCaptureClient->GetNextPacketSize(&packetSize)) && packetSize > 0)
        {
            BYTE*  pData      = nullptr;
            UINT32 numFrames  = 0;
            DWORD  flags      = 0;
            UINT64 devicePos  = 0;
            UINT64 qpcPos     = 0;

            HRESULT hr = m_pCaptureClient->GetBuffer(
                &pData, &numFrames, &flags, &devicePos, &qpcPos);
            if (FAILED(hr)) break;

            AudioBuffer buf;
            buf.sampleRate = m_actualSampleRate;
            buf.channels   = 1;
            buf.timestamp  = qpcPos;
            buf.samples.resize(numFrames);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
            {
                // Device is generating silence
                std::fill(buf.samples.begin(), buf.samples.end(), 0.0f);
            }
            else
            {
                // Convert incoming data to float32 mono
                // The mix format could be float or PCM 16/24/32
                // In exclusive float32 mode this is a direct copy
                std::memcpy(buf.samples.data(), pData,
                            numFrames * sizeof(float));
            }

            m_pCaptureClient->ReleaseBuffer(numFrames);

            // Push to DSP queue (drop if queue is full to avoid latency buildup)
            if (m_outQueue)
                m_outQueue->TryPush(std::move(buf));

            m_pCaptureClient->GetNextPacketSize(&packetSize);
        }
    }

    m_pAudioClient->Stop();
    if (hTask) AvRevertMmThreadCharacteristics(hTask);
}

void AudioCapture::Cleanup()
{
    if (m_pCaptureClient) { m_pCaptureClient->Release(); m_pCaptureClient = nullptr; }
    if (m_pAudioClient)   { m_pAudioClient->Release();   m_pAudioClient   = nullptr; }
    if (m_pDevice)        { m_pDevice->Release();         m_pDevice        = nullptr; }
    if (m_hEvent)         { CloseHandle(m_hEvent);        m_hEvent         = nullptr; }
}
