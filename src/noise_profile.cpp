#include "../include/noise_profile.h"
#include <cmath>
#include <algorithm>
#include <numeric>

NoiseProfile::NoiseProfile() = default;

bool NoiseProfile::Init(int fftSize, int sampleRate)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    m_fftSize    = fftSize;
    m_numBins    = fftSize / 2 + 1;
    m_sampleRate = sampleRate;

    m_longTermAvg.assign(m_numBins, 0.0f);
    m_noiseProfile.assign(m_numBins, 0.0f);
    m_learnAccumulator.assign(m_numBins, 0.0f);
    m_frameHistory.clear();
    m_activeBands.clear();

    m_learning.store(false);
    m_hasProfile.store(false);
    m_learningCurrentFrames = 0;
    m_learningTargetFrames  = 0;

    return true;
}

void NoiseProfile::UpdateFrame(const std::vector<float>& magnitudes)
{
    if (static_cast<int>(magnitudes.size()) != m_numBins) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    // ─── Learning phase ─────────────────────────────────────────────────────
    if (m_learning.load())
    {
        for (int k = 0; k < m_numBins; ++k)
            m_learnAccumulator[k] += magnitudes[k];

        m_learningCurrentFrames++;

        if (m_learningCurrentFrames >= m_learningTargetFrames)
        {
            // Finalise the noise profile as the average over learning period
            float invFrames = 1.0f / static_cast<float>(m_learningCurrentFrames);
            for (int k = 0; k < m_numBins; ++k)
                m_noiseProfile[k] = m_learnAccumulator[k] * invFrames;

            m_hasProfile.store(true);
            m_learning.store(false);
            AnalyzeNoiseBands();
        }
        return; // Don't update rolling history during learning
    }

    // ─── Normal rolling operation ───────────────────────────────────────────

    // Update long-term EMA (exponential moving average) — noise floor estimate
    for (int k = 0; k < m_numBins; ++k)
        m_longTermAvg[k] = kLongTermAlpha * magnitudes[k]
                         + (1.0f - kLongTermAlpha) * m_longTermAvg[k];

    // Maintain rolling frame history
    m_frameHistory.push_back(magnitudes);
    if (static_cast<int>(m_frameHistory.size()) > kRollingFrames)
        m_frameHistory.pop_front();

    // If we don't have an explicit profile yet, use the long-term avg
    if (!m_hasProfile.load())
    {
        if (static_cast<int>(m_frameHistory.size()) >= kRollingFrames / 2)
        {
            // Build an adaptive noise floor from rolling history
            // Count how often each bin exceeds the presence threshold
            std::vector<int>   presenceCount(m_numBins, 0);
            std::vector<float> avgMag(m_numBins, 0.0f);

            for (const auto& frame : m_frameHistory)
            {
                for (int k = 0; k < m_numBins; ++k)
                {
                    avgMag[k] += frame[k];
                    if (frame[k] > kPresenceThresholdLinear)
                        presenceCount[k]++;
                }
            }

            int frames = static_cast<int>(m_frameHistory.size());
            for (int k = 0; k < m_numBins; ++k)
            {
                avgMag[k] /= frames;
                float presenceRatio = static_cast<float>(presenceCount[k]) / frames;
                // Only include as noise if persistently present (not transient speech)
                m_noiseProfile[k] = (presenceRatio > kPresenceRatio) ? avgMag[k] : 0.0f;
            }

            AnalyzeNoiseBands();
        }
    }
}

void NoiseProfile::StartLearning(float durationSeconds)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Estimate ~100 frames per second at 48 kHz / 512-hop
    // hopRate = sampleRate / hopSize
    float hopRate = static_cast<float>(m_sampleRate) / 512.0f;
    m_learningTargetFrames  = static_cast<int>(durationSeconds * hopRate);
    m_learningCurrentFrames = 0;

    std::fill(m_learnAccumulator.begin(), m_learnAccumulator.end(), 0.0f);
    m_hasProfile.store(false);
    m_learning.store(true);
}

float NoiseProfile::GetLearningProgress() const
{
    if (!m_learning.load()) return m_hasProfile.load() ? 1.0f : 0.0f;
    if (m_learningTargetFrames <= 0) return 0.0f;
    return std::min(1.0f, static_cast<float>(m_learningCurrentFrames)
                        / static_cast<float>(m_learningTargetFrames));
}

std::vector<float> NoiseProfile::GetNoiseMagnitudeProfile() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_noiseProfile;
}

std::vector<NoiseBand> NoiseProfile::GetActiveNoiseBands() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_activeBands;
}

void NoiseProfile::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::fill(m_longTermAvg.begin(),    m_longTermAvg.end(),    0.0f);
    std::fill(m_noiseProfile.begin(),   m_noiseProfile.end(),   0.0f);
    std::fill(m_learnAccumulator.begin(),m_learnAccumulator.end(), 0.0f);
    m_frameHistory.clear();
    m_activeBands.clear();
    m_learning.store(false);
    m_hasProfile.store(false);
    m_learningCurrentFrames = 0;
}

void NoiseProfile::AnalyzeNoiseBands()
{
    // Called with m_mutex already held
    m_activeBands.clear();

    // Find contiguous runs of bins with non-zero noise profile
    bool inBand = false;
    int  bandStart = 0;
    float bandEnergy = 0.0f;

    for (int k = 0; k < m_numBins; ++k)
    {
        bool isNoise = (m_noiseProfile[k] > kPresenceThresholdLinear);

        if (isNoise && !inBand)
        {
            inBand     = true;
            bandStart  = k;
            bandEnergy = m_noiseProfile[k];
        }
        else if (isNoise && inBand)
        {
            bandEnergy += m_noiseProfile[k];
        }
        else if (!isNoise && inBand)
        {
            // Close the band
            int bandEnd = k - 1;
            float freqLow  = BinToFreq(bandStart);
            float freqHigh = BinToFreq(bandEnd);
            float centerHz = 0.5f * (freqLow + freqHigh);
            float avgMag   = bandEnergy / static_cast<float>(bandEnd - bandStart + 1);

            // Convert linear magnitude to dBFS
            float magDb = (avgMag > 1e-10f) ? 20.0f * std::log10(avgMag) : -120.0f;

            // Only report bands that are significant (above -50 dBFS average)
            if (magDb > -50.0f)
            {
                NoiseBand band;
                band.freqLow     = freqLow;
                band.freqHigh    = freqHigh;
                band.centerFreq  = centerHz;
                band.avgMagnitude = magDb;
                m_activeBands.push_back(band);
            }

            inBand     = false;
            bandEnergy = 0.0f;
        }
    }

    // Close any band still open at Nyquist
    if (inBand)
    {
        int bandEnd = m_numBins - 1;
        NoiseBand band;
        band.freqLow    = BinToFreq(bandStart);
        band.freqHigh   = BinToFreq(bandEnd);
        band.centerFreq = 0.5f * (band.freqLow + band.freqHigh);
        band.avgMagnitude = (bandEnergy > 1e-10f)
            ? 20.0f * std::log10(bandEnergy / (bandEnd - bandStart + 1)) : -120.0f;
        if (band.avgMagnitude > -50.0f)
            m_activeBands.push_back(band);
    }

    // Cap at max adaptive band stops
    if (m_activeBands.size() > 8) m_activeBands.resize(8);
}

float NoiseProfile::BinToFreq(int bin) const
{
    return static_cast<float>(bin) * m_sampleRate / m_fftSize;
}
