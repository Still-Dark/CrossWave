#include "../include/fft_processor.h"
// KissFFT headers — vendored in third_party/kissfft/
#include "../third_party/kissfft/kiss_fft.h"
#include "../third_party/kissfft/kiss_fftr.h"

#include <cmath>
#include <algorithm>
#include <cassert>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

FFTProcessor::FFTProcessor()  = default;

FFTProcessor::~FFTProcessor()
{
    if (m_fftCfg)  { kiss_fftr_free(reinterpret_cast<kiss_fftr_cfg>(m_fftCfg));  m_fftCfg  = nullptr; }
    if (m_ifftCfg) { kiss_fftr_free(reinterpret_cast<kiss_fftr_cfg>(m_ifftCfg)); m_ifftCfg = nullptr; }
}

bool FFTProcessor::Init(int sampleRate, int fftSize, int hopSize)
{
    assert((fftSize & (fftSize - 1)) == 0 && "fftSize must be a power of 2");
    m_sampleRate = sampleRate;
    m_fftSize    = fftSize;
    m_hopSize    = hopSize;

    // Allocate KissFFT real-to-complex configs
    m_fftCfg  = kiss_fftr_alloc(fftSize, 0, nullptr, nullptr); // Forward
    m_ifftCfg = kiss_fftr_alloc(fftSize, 1, nullptr, nullptr); // Inverse
    if (!m_fftCfg || !m_ifftCfg) return false;

    // Build Hann window
    m_hannWindow.resize(fftSize);
    for (int i = 0; i < fftSize; ++i)
        m_hannWindow[i] = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * i / (fftSize - 1)));

    // Input overlap buffer: last (fftSize - hopSize) samples are carried over
    m_inputBuffer.assign(fftSize, 0.0f);
    m_fftInput.resize(fftSize);

    // FFT output: N/2+1 complex bins
    int numBins = fftSize / 2 + 1;
    m_spectrum.resize(numBins);
    m_magnitudeDb.resize(numBins, -120.0f);
    m_peaks.clear();

    // IFFT output + overlap-add buffers
    m_ifftOutput.resize(fftSize);
    m_outputOverlap.assign(fftSize, 0.0f);
    m_outFifo.clear();

    return true;
}

void FFTProcessor::ProcessBlock(const float* samples, int count)
{
    // Append new samples to input buffer
    // We maintain a sliding window: shift old data left, append new at right
    int overlap = m_fftSize - m_hopSize;

    // Shift existing overlap samples to front
    for (int i = 0; i < overlap; ++i)
        m_inputBuffer[i] = m_inputBuffer[m_hopSize + i];

    // Fill hop portion with new samples (circular if count < hopSize)
    int toCopy = std::min(count, m_hopSize);
    for (int i = 0; i < toCopy; ++i)
        m_inputBuffer[overlap + i] = samples[i];

    // Zero-pad if count < hopSize
    for (int i = toCopy; i < m_hopSize; ++i)
        m_inputBuffer[overlap + i] = 0.0f;

    RunForwardFFT();
    DetectPeaks();
    RunInverseFFT();
}

void FFTProcessor::RunForwardFFT()
{
    // Apply Hann window to current frame
    for (int i = 0; i < m_fftSize; ++i)
        m_fftInput[i] = m_inputBuffer[i] * m_hannWindow[i];

    // Forward real FFT: float → complex
    kiss_fftr(
        reinterpret_cast<kiss_fftr_cfg>(m_fftCfg),
        m_fftInput.data(),
        reinterpret_cast<kiss_fft_cpx*>(m_spectrum.data())
    );

    // Compute magnitude spectrum (dBFS) — protected for UI reads
    {
        std::lock_guard<std::mutex> lock(m_spectrumMutex);
        for (size_t k = 0; k < m_spectrum.size(); ++k)
        {
            float re  = m_spectrum[k].real();
            float im  = m_spectrum[k].imag();
            float mag = std::sqrt(re * re + im * im) / (m_fftSize * 0.5f);
            m_magnitudeDb[k] = (mag > 1e-10f)
                ? 20.0f * std::log10(mag)
                : -120.0f;
        }
    }
}

void FFTProcessor::RunInverseFFT()
{
    // Inverse real FFT: complex → float
    kiss_fftri(
        reinterpret_cast<kiss_fftr_cfg>(m_ifftCfg),
        reinterpret_cast<const kiss_fft_cpx*>(m_spectrum.data()),
        m_ifftOutput.data()
    );

    // Normalize KissFFT inverse output (multiplied by N)
    float norm = 1.0f / m_fftSize;
    for (int i = 0; i < m_fftSize; ++i)
        m_ifftOutput[i] *= norm;

    // Overlap-add synthesis with Hann window
    for (int i = 0; i < m_fftSize; ++i)
        m_outputOverlap[i] += m_ifftOutput[i] * m_hannWindow[i];

    // First hopSize samples are complete — push into output FIFO
    for (int i = 0; i < m_hopSize; ++i)
        m_outFifo.push_back(m_outputOverlap[i]);

    // Shift overlap accumulator by hopSize
    for (int i = 0; i < m_fftSize - m_hopSize; ++i)
        m_outputOverlap[i] = m_outputOverlap[m_hopSize + i];
    for (int i = m_fftSize - m_hopSize; i < m_fftSize; ++i)
        m_outputOverlap[i] = 0.0f;
}

int FFTProcessor::GetProcessedSamples(float* outSamples, int count)
{
    int available = static_cast<int>(m_outFifo.size());
    int toRead    = std::min(count, available);
    for (int i = 0; i < toRead; ++i)
        outSamples[i] = m_outFifo[i];
    m_outFifo.erase(m_outFifo.begin(), m_outFifo.begin() + toRead);
    return toRead;
}

void FFTProcessor::DetectPeaks()
{
    std::lock_guard<std::mutex> lock(m_spectrumMutex);
    m_peaks.clear();

    int numBins = static_cast<int>(m_magnitudeDb.size());
    int minSepBins = std::max(1, FreqToBin(kPeakMinSeparation));

    for (int k = 1; k < numBins - 1; ++k)
    {
        float mag = m_magnitudeDb[k];
        if (mag <= kPeakThresholdDb) continue;

        // Local maximum check
        if (mag > m_magnitudeDb[k - 1] && mag > m_magnitudeDb[k + 1])
        {
            // Check it's not too close to an existing peak
            bool tooClose = false;
            for (const auto& p : m_peaks)
            {
                if (std::abs(FreqToBin(p.frequencyHz) - k) < minSepBins)
                {
                    tooClose = true;
                    break;
                }
            }
            if (tooClose) continue;

            // Estimate bandwidth at -3 dB down
            float bwHz = 10.0f; // default narrow
            for (int bk = k + 1; bk < numBins; ++bk)
            {
                if (m_magnitudeDb[bk] <= mag - 3.0f)
                {
                    bwHz = 2.0f * (BinToFreq(bk) - BinToFreq(k));
                    break;
                }
            }

            FrequencyPeak peak;
            peak.frequencyHz = BinToFreq(k);
            peak.magnitudeDb = mag;
            peak.bandwidth   = bwHz;
            m_peaks.push_back(peak);
        }
    }

    // Sort by magnitude descending
    std::sort(m_peaks.begin(), m_peaks.end(),
        [](const FrequencyPeak& a, const FrequencyPeak& b) {
            return a.magnitudeDb > b.magnitudeDb;
        });

    // Keep top-N peaks
    if (m_peaks.size() > 16) m_peaks.resize(16);
}

void FFTProcessor::SetComplexSpectrum(const std::vector<std::complex<float>>& spec)
{
    assert(spec.size() == m_spectrum.size());
    m_spectrum = spec;
}

std::vector<float> FFTProcessor::GetMagnitudeSpectrum() const
{
    std::lock_guard<std::mutex> lock(m_spectrumMutex);
    return m_magnitudeDb;
}

std::vector<FrequencyPeak> FFTProcessor::GetPeakFrequencies() const
{
    std::lock_guard<std::mutex> lock(m_spectrumMutex);
    return m_peaks;
}

int FFTProcessor::FreqToBin(float freqHz) const
{
    return static_cast<int>(freqHz * m_fftSize / m_sampleRate + 0.5f);
}

float FFTProcessor::BinToFreq(int bin) const
{
    return static_cast<float>(bin) * m_sampleRate / m_fftSize;
}
