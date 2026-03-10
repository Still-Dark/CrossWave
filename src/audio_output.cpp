#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <avrt.h>
#include <cstring>
#include "../include/audio_output.h"

#define RETURN_ON_FAIL(hr, msg) \
    if (FAILED(hr)) { m_lastError = (msg); return false; }

AudioOutput::AudioOutput()  = default;
AudioOutput::~AudioOutput() { Stop(); }

bool AudioOutput::Init(const std::wstring& deviceId,
                       int sampleRate,
                       int bufferFrames,
                       ThreadSafeQueue<AudioBuffer>* inQueue)
{
    m_sampleRate   = sampleRate;
    m_bufferFrames = bufferFrames;
    m_inQueue      = inQueue;

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum));
    RETURN_ON_FAIL(hr, "CoCreateInstance(IMMDeviceEnumerator) render failed");

    if (deviceId.empty())
        hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &m_pDevice);
    else
        hr = pEnum->GetDevice(deviceId.c_str(), &m_pDevice);
    pEnum->Release();
    RETURN_ON_FAIL(hr, "GetAudioEndpoint (render) failed");

    if (InitExclusive(m_pDevice, sampleRate, bufferFrames))
    {
        m_exclusiveMode = true;
        return true;
    }

    m_lastError.clear();
    if (InitShared(m_pDevice))
    {
        m_exclusiveMode = false;
        return true;
    }

    return false;
}

bool AudioOutput::InitExclusive(IMMDevice* pDevice, int sampleRate, int bufferFrames)
{
    HRESULT hr = pDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&m_pAudioClient));
    RETURN_ON_FAIL(hr, "Activate IAudioClient (exclusive render) failed");

    // Stereo float32 output
    WAVEFORMATEX wfx    = {};
    wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
    wfx.wBitsPerSample  = 32;
    wfx.nBlockAlign     = wfx.nChannels * (wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    REFERENCE_TIME bufDur = static_cast<REFERENCE_TIME>(
        (double)bufferFrames / sampleRate * 10000000.0);

    m_hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_hEvent) RETURN_ON_FAIL(E_FAIL, "CreateEvent (render) failed");

    hr = m_pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_EXCLUSIVE,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        bufDur, bufDur, &wfx, nullptr);

    if (FAILED(hr))
    {
        CloseHandle(m_hEvent); m_hEvent = nullptr;
        m_pAudioClient->Release(); m_pAudioClient = nullptr;
        m_lastError = "Exclusive render not supported, falling back to shared";
        return false;
    }

    hr = m_pAudioClient->SetEventHandle(m_hEvent);
    RETURN_ON_FAIL(hr, "SetEventHandle (render) failed");

    hr = m_pAudioClient->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(&m_pRenderClient));
    RETURN_ON_FAIL(hr, "GetService(IAudioRenderClient) failed");

    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    m_pAudioClient->GetDevicePeriod(&defPeriod, &minPeriod);
    m_latencyMs = static_cast<double>(minPeriod) / 10000.0;

    // Pre-fill silence buffer (stereo: 2 floats per frame)
    m_silenceBuffer.assign(bufferFrames * 2, 0.0f);
    return true;
}

bool AudioOutput::InitShared(IMMDevice* pDevice)
{
    HRESULT hr = pDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&m_pAudioClient));
    RETURN_ON_FAIL(hr, "Activate IAudioClient (shared render) failed");

    WAVEFORMATEX* pMixFmt = nullptr;
    hr = m_pAudioClient->GetMixFormat(&pMixFmt);
    RETURN_ON_FAIL(hr, "GetMixFormat (render) failed");

    REFERENCE_TIME bufDur = 0;
    m_pAudioClient->GetDevicePeriod(&bufDur, nullptr);

    m_hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_hEvent)
    {
        CoTaskMemFree(pMixFmt);
        RETURN_ON_FAIL(E_FAIL, "CreateEvent (shared render) failed");
    }

    hr = m_pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        bufDur, 0, pMixFmt, nullptr);
    CoTaskMemFree(pMixFmt);
    RETURN_ON_FAIL(hr, "Initialize shared render failed");

    hr = m_pAudioClient->SetEventHandle(m_hEvent);
    RETURN_ON_FAIL(hr, "SetEventHandle (shared render) failed");

    hr = m_pAudioClient->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(&m_pRenderClient));
    RETURN_ON_FAIL(hr, "GetService(IAudioRenderClient) shared failed");

    REFERENCE_TIME period = 0;
    m_pAudioClient->GetDevicePeriod(&period, nullptr);
    m_latencyMs = static_cast<double>(period) / 10000.0;

    m_silenceBuffer.assign(m_bufferFrames * 2, 0.0f);
    return true;
}

void AudioOutput::Start()
{
    if (m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread(&AudioOutput::RenderLoop, this);
    SetThreadPriority(m_thread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);
}

void AudioOutput::Stop()
{
    m_running.store(false);
    if (m_hEvent) SetEvent(m_hEvent);
    if (m_thread.joinable()) m_thread.join();
    Cleanup();
}

void AudioOutput::RenderLoop()
{
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    UINT32 bufferFrameCount = 0;
    m_pAudioClient->GetBufferSize(&bufferFrameCount);

    if (FAILED(m_pAudioClient->Start()))
    {
        m_running.store(false);
        if (hTask) AvRevertMmThreadCharacteristics(hTask);
        return;
    }

    // Pending samples from last received buffer
    std::vector<float> pending;
    size_t pendingOffset = 0;

    while (m_running.load())
    {
        DWORD waitResult = WaitForSingleObject(m_hEvent, 200);
        if (!m_running.load()) break;
        if (waitResult != WAIT_OBJECT_0) continue;

        // How much free space is in the render buffer?
        UINT32 numFramesPadding = 0;
        m_pAudioClient->GetCurrentPadding(&numFramesPadding);
        UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;
        if (numFramesAvailable == 0) continue;

        BYTE* pData = nullptr;
        HRESULT hr = m_pRenderClient->GetBuffer(numFramesAvailable, &pData);
        if (FAILED(hr)) continue;

        float* pOut = reinterpret_cast<float*>(pData);
        UINT32 framesWritten = 0;

        // Fill render buffer from pending + new queue items
        while (framesWritten < numFramesAvailable)
        {
            if (pending.empty() || pendingOffset >= pending.size() / 1)
            {
                // Fetch next buffer from queue (non-blocking, 2ms timeout)
                auto optBuf = m_inQueue->PopTimeout(std::chrono::milliseconds(2));
                if (!optBuf.has_value())
                {
                    // Underrun: fill remaining with silence
                    UINT32 remaining = numFramesAvailable - framesWritten;
                    for (UINT32 f = 0; f < remaining; ++f)
                    {
                        pOut[(framesWritten + f) * 2 + 0] = 0.0f;
                        pOut[(framesWritten + f) * 2 + 1] = 0.0f;
                    }
                    framesWritten = numFramesAvailable;
                    break;
                }
                pending      = std::move(optBuf->samples);
                pendingOffset = 0;
            }

            // Copy mono samples → stereo interleaved (L=R)
            while (pendingOffset < pending.size() && framesWritten < numFramesAvailable)
            {
                float mono = pending[pendingOffset++];
                pOut[framesWritten * 2 + 0] = mono; // Left
                pOut[framesWritten * 2 + 1] = mono; // Right
                framesWritten++;
            }
        }

        m_pRenderClient->ReleaseBuffer(numFramesAvailable, 0);
    }

    m_pAudioClient->Stop();
    if (hTask) AvRevertMmThreadCharacteristics(hTask);
}

void AudioOutput::Cleanup()
{
    if (m_pRenderClient) { m_pRenderClient->Release(); m_pRenderClient = nullptr; }
    if (m_pAudioClient)  { m_pAudioClient->Release();  m_pAudioClient  = nullptr; }
    if (m_pDevice)       { m_pDevice->Release();        m_pDevice       = nullptr; }
    if (m_hEvent)        { CloseHandle(m_hEvent);       m_hEvent        = nullptr; }
}
