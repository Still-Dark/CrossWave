#pragma once
#include <vector>
#include <atomic>
#include <mutex>
#include <deque>

/**
 * @brief Represents a band of frequencies identified as persistent noise.
 */
struct NoiseBand
{
    float freqLow;      // Low edge of noise band (Hz)
    float freqHigh;     // High edge of noise band (Hz)
    float centerFreq;   // Center frequency (Hz)
    float avgMagnitude; // Average magnitude dBFS in this band
};

/**
 * @brief Dynamic noise profile and noise floor estimator.
 *
 * Maintains a rolling average of FFT magnitude spectra to distinguish
 * persistent noise (fan, AC, engine hum) from transient signals (speech).
 *
 * A frequency bin is classified as "noise" if:
 *   1. It has been above kPresenceThresholdDb for > kPresenceRatio of frames.
 *   2. Its short-term energy is below the speech detection threshold.
 *
 * The noise profile magnitude array is used by the DSP engine for spectral
 * subtraction. Detected noise bands are used for adaptive band-stop filters.
 */
class NoiseProfile
{
public:
    NoiseProfile();

    /**
     * @brief Initialize the noise profiler.
     * @param sampleRate  Audio sample rate.
     * @param fftSize     FFT size (must match FFTProcessor).
     * @param sampleRate  Sample rate, used to calculate frequencies.
     */
    bool Init(int fftSize, int sampleRate);

    /**
     * @brief Feed a new FFT magnitude spectrum frame (linear scale).
     *
     * Called every hop from the DSP thread with the current magnitude spectrum.
     * Internally updates the rolling average and detects noise.
     *
     * @param magnitudes Per-bin linear magnitudes (size = fftSize/2 + 1).
     */
    void UpdateFrame(const std::vector<float>& magnitudes);

    /**
     * @brief Start the noise learning phase.
     *
     * Clears current history and accumulates frames for the specified duration.
     * The resulting profile is stored as the noise floor.
     *
     * @param durationSeconds  How long to record environmental noise.
     */
    void StartLearning(float durationSeconds);

    /** @brief Returns true while in the learning phase. */
    bool IsLearning() const { return m_learning.load(); }

    /** @brief Returns progress of learning phase [0.0, 1.0]. */
    float GetLearningProgress() const;

    /**
     * @brief Get the stored noise floor magnitude profile (linear scale).
     * Thread-safe.
     * @return Per-bin noise floor magnitudes (size = fftSize/2 + 1).
     */
    std::vector<float> GetNoiseMagnitudeProfile() const;

    /**
     * @brief Get the list of currently active noise bands.
     * Thread-safe.
     */
    std::vector<NoiseBand> GetActiveNoiseBands() const;

    /**
     * @brief Returns whether a noise profile has been learned.
     */
    bool HasProfile() const { return m_hasProfile.load(); }

    /**
     * @brief Reset the noise profile and rolling history.
     */
    void Reset();

private:
    void AnalyzeNoiseBands();
    float BinToFreq(int bin) const;

    int m_fftSize    = 1024;
    int m_numBins    = 513;   // fftSize/2 + 1
    int m_sampleRate = 48000;

    // Rolling magnitude history across frames
    static constexpr int kRollingFrames   = 200;  // ~2 seconds at 100 fps hop rate
    static constexpr float kPresenceRatio = 0.80f; // Bin must be active 80% of frames
    static constexpr float kPresenceThresholdLinear = 0.001f; // ~-60 dBFS

    std::deque<std::vector<float>> m_frameHistory;  // Rolling window of magnitudes

    // Smoothed long-term average (noise floor estimate during normal run)
    std::vector<float> m_longTermAvg;
    static constexpr float kLongTermAlpha = 0.05f; // EMA smoothing factor

    // Stored noise profile (from learning phase or adaptive estimate)
    std::vector<float> m_noiseProfile;

    // Active noise bands (result of analysis)
    std::vector<NoiseBand> m_activeBands;

    // Learning state
    std::atomic<bool>  m_learning{false};
    std::atomic<bool>  m_hasProfile{false};
    int                m_learningTargetFrames  = 0;
    int                m_learningCurrentFrames = 0;
    std::vector<float> m_learnAccumulator;

    mutable std::mutex m_mutex;
};
