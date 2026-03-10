#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <functional>
#include "thread_safe_queue.h"

/**
 * @brief Audio buffer passed between pipeline stages.
 *
 * Contains a block of float32 PCM samples (mono, 48 kHz).
 * Timestamp is in 100-nanosecond units (REFERENCE_TIME / QPC).
 */
struct AudioBuffer
{
    std::vector<float> samples;   // PCM float32 samples
    UINT64             timestamp; // Capture timestamp (for latency measurement)
    int                sampleRate;
    int                channels;
};

/**
 * @brief WASAPI-based microphone capture module.
 *
 * Runs on a dedicated thread. Captures audio from the selected endpoint
 * and pushes AudioBuffer objects into the provided output queue.
 *
 * Attempts exclusive mode first; falls back to shared mode if the device
 * does not support exclusive mode.
 */
class AudioCapture
{
public:
    AudioCapture();
    ~AudioCapture();

    /**
     * @brief Initialize the capture endpoint.
     * @param deviceId   WASAPI device ID (from AudioDeviceEnumerator). Empty = default.
     * @param sampleRate Desired sample rate (48000).
     * @param bufferFrames Desired buffer size in frames (64–256).
     * @param outQueue   Queue where captured buffers will be pushed.
     * @return true on success.
     */
    bool Init(const std::wstring& deviceId,
              int sampleRate,
              int bufferFrames,
              ThreadSafeQueue<AudioBuffer>* outQueue);

    /** @brief Start the capture thread. */
    void Start();

    /** @brief Stop the capture thread and release WASAPI resources. */
    void Stop();

    /** @brief Returns whether capture is currently running. */
    bool IsRunning() const { return m_running.load(); }

    /** @brief Returns the actual sample rate negotiated with WASAPI. */
    int GetActualSampleRate() const { return m_actualSampleRate; }

    /** @brief Returns true if exclusive mode was successfully acquired. */
    bool IsExclusive() const { return m_exclusiveMode; }

    /** @brief Returns current capture latency in milliseconds. */
    double GetLatencyMs() const { return m_latencyMs; }

    /** @brief Returns last error description. */
    const std::string& GetLastError() const { return m_lastError; }

private:
    void CaptureLoop();
    bool InitExclusive(IMMDevice* pDevice, int sampleRate, int bufferFrames);
    bool InitShared(IMMDevice* pDevice);
    void Cleanup();

    // WASAPI COM objects
    IMMDevice*          m_pDevice        = nullptr;
    IAudioClient*       m_pAudioClient   = nullptr;
    IAudioCaptureClient* m_pCaptureClient = nullptr;
    HANDLE              m_hEvent         = nullptr;

    // Configuration
    int  m_sampleRate    = 48000;
    int  m_bufferFrames  = 256;
    bool m_exclusiveMode = false;

    // Runtime
    std::atomic<bool>           m_running{false};
    std::thread                 m_thread;
    ThreadSafeQueue<AudioBuffer>* m_outQueue = nullptr;

    // Stats
    int    m_actualSampleRate = 0;
    double m_latencyMs        = 0.0;
    std::string m_lastError;
};
