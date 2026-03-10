#pragma once
#include <vector>
#include <complex>
#include <cstddef>
#include <string>
#include <mutex>

/**
 * @brief Represents a detected dominant frequency peak.
 */
struct FrequencyPeak
{
    float frequencyHz;  // Center frequency of peak
    float magnitudeDb;  // Magnitude in dB relative to full scale
    float bandwidth;    // Estimated bandwidth in Hz
};

/**
 * @brief FFT-based spectral analysis module.
 *
 * Uses KissFFT to perform real-to-complex FFT on incoming audio blocks.
 * Maintains an internal overlap-add input buffer and Hann window.
 *
 * Thread-safety: FFTProcessor is NOT thread-safe. It should only be called
 * from the DSP thread. The GetMagnitudeSpectrum() call acquires a lock
 * to allow safe reads from the UI thread.
 */
class FFTProcessor
{
public:
    FFTProcessor();
    ~FFTProcessor();

    /**
     * @brief Initialize the FFT processor.
     * @param sampleRate  Audio sample rate (e.g., 48000).
     * @param fftSize     FFT window size — must be power of 2 (e.g., 1024 or 2048).
     * @param hopSize     Number of NEW samples per processing step (e.g., fftSize/2 for 50% overlap).
     * @return true on success.
     */
    bool Init(int sampleRate, int fftSize, int hopSize);

    /**
     * @brief Feed new samples into the processor.
     *
     * Call this with each new audio block. ProcessBlock() will internally
     * buffer samples and run FFT analysis when enough data accumulates.
     *
     * @param samples Pointer to float PCM samples (mono).
     * @param count   Number of samples.
     */
    void ProcessBlock(const float* samples, int count);

    /**
     * @brief Apply spectral modification and reconstruct output samples.
     *
     * After ProcessBlock(), call this to get the time-domain output via
     * inverse FFT and overlap-add synthesis.
     *
     * @param[out] outSamples Buffer to write processed samples into.
     * @param[in]  count      Number of samples to read.
     * @return Actual number of samples written (may be less than count if
     *         not enough output is available yet).
     */
    int GetProcessedSamples(float* outSamples, int count);

    /**
     * @brief Returns the current FFT magnitude spectrum (dBFS per bin).
     * Thread-safe: can be called from the UI thread.
     */
    std::vector<float> GetMagnitudeSpectrum() const;

    /**
     * @brief Returns detected dominant frequency peaks.
     * Thread-safe.
     */
    std::vector<FrequencyPeak> GetPeakFrequencies() const;

    /**
     * @brief Get the raw complex spectrum (for spectral subtraction).
     * NOT thread-safe — call from DSP thread only.
     */
    std::vector<std::complex<float>>& GetComplexSpectrum() { return m_spectrum; }

    /**
     * @brief Copy modified complex spectrum back before IFFT.
     * NOT thread-safe — call from DSP thread only.
     */
    void SetComplexSpectrum(const std::vector<std::complex<float>>& spec);

    int GetFFTSize()    const { return m_fftSize; }
    int GetHopSize()    const { return m_hopSize; }
    int GetSampleRate() const { return m_sampleRate; }

    /** @brief Converts a frequency in Hz to the nearest FFT bin index. */
    int FreqToBin(float freqHz) const;

    /** @brief Converts a bin index to frequency in Hz. */
    float BinToFreq(int bin) const;

private:
    void RunForwardFFT();
    void RunInverseFFT();
    void DetectPeaks();
    void ApplyHannWindow(float* buffer, int size);

    // KissFFT handles (void* to avoid including kiss_fft.h in the header)
    void* m_fftCfg  = nullptr;
    void* m_ifftCfg = nullptr;

    int m_sampleRate  = 48000;
    int m_fftSize     = 1024;
    int m_hopSize     = 512;

    // Input overlap buffer
    std::vector<float> m_inputBuffer;
    std::vector<float> m_hannWindow;
    std::vector<float> m_fftInput;    // Windowed frame

    // FFT output
    std::vector<std::complex<float>> m_spectrum;

    // IFFT output & overlap-add accumulator
    std::vector<float> m_ifftOutput;
    std::vector<float> m_outputOverlap;  // Synthesis overlap-add buffer
    std::vector<float> m_outFifo;        // Output ring buffer

    // Results (protected by mutex for UI reads)
    mutable std::mutex             m_spectrumMutex;
    std::vector<float>             m_magnitudeDb;     // Per-bin dBFS magnitudes
    std::vector<FrequencyPeak>     m_peaks;

    // Peak detection parameters
    static constexpr float kPeakThresholdDb   = -60.0f;  // Minimum dB for a peak
    static constexpr float kPeakMinSeparation = 30.0f;   // Minimum Hz between peaks
};
