#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include "thread_safe_queue.h"
#include "audio_capture.h"  // For AudioBuffer

/**
 * @brief WASAPI-based headphone output module.
 *
 * Runs on a dedicated thread. Reads processed AudioBuffer objects from
 * the input queue and renders them to the selected output endpoint.
 *
 * Attempts exclusive mode first; falls back to shared mode.
 * Converts mono input to stereo output (L=R copy).
 */
class AudioOutput
{
public:
    AudioOutput();
    ~AudioOutput();

    /**
     * @brief Initialize the render endpoint.
     * @param deviceId    WASAPI device ID. Empty = default.
     * @param sampleRate  Desired sample rate (48000).
     * @param bufferFrames Desired buffer size in frames.
     * @param inQueue     Queue to read processed buffers from.
     * @return true on success.
     */
    bool Init(const std::wstring& deviceId,
              int sampleRate,
              int bufferFrames,
              ThreadSafeQueue<AudioBuffer>* inQueue);

    /** @brief Start the output render thread. */
    void Start();

    /** @brief Stop the render thread and release WASAPI resources. */
    void Stop();

    /** @brief Returns whether output is currently running. */
    bool IsRunning() const { return m_running.load(); }

    /** @brief Returns true if exclusive mode was successfully acquired. */
    bool IsExclusive() const { return m_exclusiveMode; }

    /** @brief Returns the current output latency in milliseconds. */
    double GetLatencyMs() const { return m_latencyMs; }

    /** @brief Returns last error description. */
    const std::string& GetLastError() const { return m_lastError; }

private:
    void RenderLoop();
    bool InitExclusive(IMMDevice* pDevice, int sampleRate, int bufferFrames);
    bool InitShared(IMMDevice* pDevice);
    void Cleanup();

    // WASAPI COM objects
    IMMDevice*         m_pDevice       = nullptr;
    IAudioClient*      m_pAudioClient  = nullptr;
    IAudioRenderClient* m_pRenderClient = nullptr;
    HANDLE             m_hEvent        = nullptr;

    // Configuration
    int  m_sampleRate    = 48000;
    int  m_bufferFrames  = 256;
    bool m_exclusiveMode = false;

    // Runtime
    std::atomic<bool>          m_running{false};
    std::thread                m_thread;
    ThreadSafeQueue<AudioBuffer>* m_inQueue = nullptr;

    // Silence buffer for underrun fill
    std::vector<float>         m_silenceBuffer;

    // Stats
    double      m_latencyMs = 0.0;
    std::string m_lastError;
};
