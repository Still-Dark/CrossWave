#pragma once
#include <vector>
#include <atomic>
#include <memory>
#include <array>
#include "fft_processor.h"
#include "noise_profile.h"
#include "thread_safe_queue.h"
#include "audio_capture.h"   // For AudioBuffer

/**
 * @brief IIR Biquad filter state — Direct Form II Transposed.
 *
 * Can be configured as: low-pass, high-pass, notch, band-stop.
 */
struct BiquadFilter
{
    // Coefficients
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0; // a0 is normalized to 1

    // State variables (Direct Form II transposed)
    double w1 = 0.0, w2 = 0.0;

    /** @brief Process a single sample through the biquad. */
    inline float Process(float x)
    {
        double y = b0 * x + w1;
        w1 = b1 * x - a1 * y + w2;
        w2 = b2 * x - a2 * y;
        return static_cast<float>(y);
    }

    /** @brief Reset filter state (zero the delay registers). */
    void Reset() { w1 = 0.0; w2 = 0.0; }
};

/**
 * @brief Design parameters for a notch filter.
 */
struct NotchFilterConfig
{
    float centerFreqHz;  // e.g., 50.0f or 60.0f
    float bandwidthHz;   // e.g., 5.0f — narrower = deeper notch, slower roll-off
    bool  enabled;
};

/**
 * @brief Core DSP processing engine.
 *
 * Runs on the DSP thread. Reads from CaptureQueue, applies:
 *   1. IIR Notch Filters (50/60 Hz)
 *   2. Adaptive Band-Stop Filters
 *   3. Spectral Subtraction (via FFTProcessor + NoiseProfile)
 *
 * Writes processed AudioBuffers to OutputQueue.
 */
class DSPEngine
{
public:
    DSPEngine();
    ~DSPEngine();

    /**
     * @brief Initialize the DSP engine.
     * @param sampleRate         Audio sample rate.
     * @param fftSize            FFT window size (power of 2, e.g., 1024).
     * @param suppressionStrength Initial suppression strength [0.0, 1.0].
     * @param inQueue            Source queue (from audio capture).
     * @param outQueue           Destination queue (to audio output).
     */
    bool Init(int sampleRate,
              int fftSize,
              float suppressionStrength,
              ThreadSafeQueue<AudioBuffer>* inQueue,
              ThreadSafeQueue<AudioBuffer>* outQueue);

    /** @brief Start the DSP processing thread. */
    void Start();

    /** @brief Stop the DSP thread gracefully. */
    void Stop();

    // --- Runtime control (thread-safe) ---

    /** @brief Enable or disable all noise suppression. */
    void EnableSuppression(bool on) { m_suppressionEnabled.store(on); }

    /** @brief Set suppression strength [0.0 = off, 1.0 = maximum]. */
    void SetSuppressionStrength(float s);

    /** @brief Returns current suppression strength. */
    float GetSuppressionStrength() const { return m_suppressionStrength.load(); }

    /** @brief Trigger noise learning for the specified duration in seconds. */
    void StartNoiseLearn(float durationSeconds = 5.0f);

    /** @brief Returns true while noise learning is in progress. */
    bool IsLearning() const;

    /** @brief Returns the noise learning progress [0.0, 1.0]. */
    float GetLearningProgress() const;

    /** @brief Returns a snapshot of the current FFT magnitude spectrum. UI thread safe. */
    std::vector<float> GetMagnitudeSpectrum() const;

    /** @brief Returns detected active noise bands. UI thread safe. */
    std::vector<NoiseBand> GetActiveNoiseBands() const;

    /** @brief Returns true if DSP thread is running. */
    bool IsRunning() const { return m_running.load(); }

    // Static utility: design a biquad notch filter.
    static BiquadFilter DesignNotch(float centerFreqHz, float bandwidthHz, int sampleRate);

    // Static utility: design a biquad band-stop filter.
    static BiquadFilter DesignBandStop(float centerFreqHz, float bandwidthHz, int sampleRate);

private:
    void DSPLoop();
    void ProcessBuffer(AudioBuffer& buf);
    void ApplyNotchFilters(float* samples, int count);
    void ApplyBandStopFilters(float* samples, int count);
    void ApplySpectralSubtraction(float* samples, int count);
    void UpdateAdaptiveBandStops();

    int m_sampleRate = 48000;
    int m_fftSize    = 1024;

    // Processing blocks
    std::unique_ptr<FFTProcessor> m_fftProc;
    std::unique_ptr<NoiseProfile> m_noiseProf;

    // Fixed notch filters: 50 Hz and 60 Hz
    static constexpr int kNumFixedNotches = 2;
    BiquadFilter m_notchFilters[kNumFixedNotches];  // [0]=50 Hz, [1]=60 Hz
    NotchFilterConfig m_notchConfigs[kNumFixedNotches] = {
        { 50.0f,  5.0f, true },
        { 60.0f,  5.0f, true }
    };

    // Adaptive band-stop filters (dynamically updated from noise profile)
    static constexpr int kMaxAdaptiveBandStops = 8;
    std::array<BiquadFilter, kMaxAdaptiveBandStops> m_bandStopFilters;
    int m_numActiveBandStops = 0;

    // Spectral subtraction parameters
    float m_overSubtractionFactor = 1.5f;  // α: controlled by strength slider
    float m_spectralFloor         = 0.1f;  // β: minimum residual energy

    // Queues
    ThreadSafeQueue<AudioBuffer>* m_inQueue  = nullptr;
    ThreadSafeQueue<AudioBuffer>* m_outQueue = nullptr;

    // Control
    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_suppressionEnabled{true};
    std::atomic<float> m_suppressionStrength{0.75f};

    std::thread m_thread;
};
