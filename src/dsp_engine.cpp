#include "../include/dsp_engine.h"
#include <cmath>
#include <algorithm>
#include <cassert>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

DSPEngine::DSPEngine()  = default;
DSPEngine::~DSPEngine() { Stop(); }

bool DSPEngine::Init(int sampleRate,
                     int fftSize,
                     float suppressionStrength,
                     ThreadSafeQueue<AudioBuffer>* inQueue,
                     ThreadSafeQueue<AudioBuffer>* outQueue)
{
    m_sampleRate = sampleRate;
    m_fftSize    = fftSize;
    m_inQueue    = inQueue;
    m_outQueue   = outQueue;
    m_suppressionStrength.store(suppressionStrength);

    // Create FFT processor (1024 FFT, 512 hop = 50% overlap)
    m_fftProc = std::make_unique<FFTProcessor>();
    if (!m_fftProc->Init(sampleRate, fftSize, fftSize / 2))
        return false;

    // Create noise profiler
    m_noiseProf = std::make_unique<NoiseProfile>();
    if (!m_noiseProf->Init(fftSize, sampleRate))
        return false;

    // Design fixed notch filters
    for (int i = 0; i < kNumFixedNotches; ++i)
    {
        m_notchFilters[i] = DesignNotch(
            m_notchConfigs[i].centerFreqHz,
            m_notchConfigs[i].bandwidthHz,
            sampleRate);
    }

    // Zero out adaptive band-stop filter states
    for (auto& f : m_bandStopFilters) f.Reset();
    m_numActiveBandStops = 0;

    return true;
}

// ─── Biquad design ──────────────────────────────────────────────────────────

BiquadFilter DSPEngine::DesignNotch(float centerFreqHz, float bandwidthHz, int sampleRate)
{
    // Direct Form II Transposed notch biquad
    // Reference: Audio EQ Cookbook
    double omega  = 2.0 * M_PI * centerFreqHz / sampleRate;
    double bw     = 2.0 * M_PI * bandwidthHz  / sampleRate;
    double alpha  = std::sin(omega) * std::sinh(std::log(2.0) / 2.0 * bw * omega / std::sin(omega));
    double cosw   = std::cos(omega);
    double a0     = 1.0 + alpha;

    BiquadFilter f;
    f.b0 =  1.0       / a0;
    f.b1 = -2.0 * cosw / a0;
    f.b2 =  1.0       / a0;
    f.a1 = -2.0 * cosw / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

BiquadFilter DSPEngine::DesignBandStop(float centerFreqHz, float bandwidthHz, int sampleRate)
{
    // Band-stop = notch (same formula but wider bandwidth)
    return DesignNotch(centerFreqHz, bandwidthHz, sampleRate);
}

// ─── Strength setter ────────────────────────────────────────────────────────

void DSPEngine::SetSuppressionStrength(float s)
{
    s = std::max(0.0f, std::min(1.0f, s));
    m_suppressionStrength.store(s);
    // Map strength [0,1] → over-subtraction α [1.0, 2.5]
    m_overSubtractionFactor = 1.0f + 1.5f * s;
}

// ─── Noise learning ─────────────────────────────────────────────────────────

void DSPEngine::StartNoiseLearn(float durationSeconds)
{
    if (m_noiseProf)
        m_noiseProf->StartLearning(durationSeconds);
}

bool DSPEngine::IsLearning() const
{
    return m_noiseProf && m_noiseProf->IsLearning();
}

float DSPEngine::GetLearningProgress() const
{
    return m_noiseProf ? m_noiseProf->GetLearningProgress() : 0.0f;
}

std::vector<float> DSPEngine::GetMagnitudeSpectrum() const
{
    return m_fftProc ? m_fftProc->GetMagnitudeSpectrum() : std::vector<float>{};
}

std::vector<NoiseBand> DSPEngine::GetActiveNoiseBands() const
{
    return m_noiseProf ? m_noiseProf->GetActiveNoiseBands() : std::vector<NoiseBand>{};
}

// ─── Thread management ──────────────────────────────────────────────────────

void DSPEngine::Start()
{
    if (m_running.load()) return;
    m_running.store(true);
    m_thread = std::thread(&DSPEngine::DSPLoop, this);
}

void DSPEngine::Stop()
{
    m_running.store(false);
    if (m_thread.joinable()) m_thread.join();
}

// ─── DSP loop (runs on dedicated thread) ────────────────────────────────────

void DSPEngine::DSPLoop()
{
    while (m_running.load())
    {
        auto optBuf = m_inQueue->PopTimeout(std::chrono::milliseconds(50));
        if (!optBuf.has_value()) continue;
        if (!m_running.load()) break;

        AudioBuffer buf = std::move(*optBuf);
        ProcessBuffer(buf);

        if (m_outQueue)
            m_outQueue->TryPush(std::move(buf));
    }
}

void DSPEngine::ProcessBuffer(AudioBuffer& buf)
{
    if (buf.samples.empty()) return;
    float* samples = buf.samples.data();
    int    count   = static_cast<int>(buf.samples.size());

    if (!m_suppressionEnabled.load())
        return; // Pass through unmodified

    // ── Stage 1: Fixed notch filters (50 Hz, 60 Hz) ─────────────────────────
    ApplyNotchFilters(samples, count);

    // ── Stage 2: FFT + spectral subtraction + adaptive band-stops ───────────
    ApplySpectralSubtraction(samples, count);

    // ── Stage 3: Adaptive band-stop filters on reconstructed signal ──────────
    UpdateAdaptiveBandStops();
    ApplyBandStopFilters(samples, count);
}

void DSPEngine::ApplyNotchFilters(float* samples, int count)
{
    for (int i = 0; i < kNumFixedNotches; ++i)
    {
        if (!m_notchConfigs[i].enabled) continue;
        // Scale filter depth by suppression strength
        float strength = m_suppressionStrength.load();
        for (int n = 0; n < count; ++n)
        {
            float filtered = m_notchFilters[i].Process(samples[n]);
            samples[n] = samples[n] + strength * (filtered - samples[n]);
        }
    }
}

void DSPEngine::ApplyBandStopFilters(float* samples, int count)
{
    float strength = m_suppressionStrength.load();
    for (int i = 0; i < m_numActiveBandStops; ++i)
    {
        for (int n = 0; n < count; ++n)
        {
            float filtered = m_bandStopFilters[i].Process(samples[n]);
            samples[n] = samples[n] + strength * (filtered - samples[n]);
        }
    }
}

void DSPEngine::ApplySpectralSubtraction(float* samples, int count)
{
    if (!m_fftProc || !m_noiseProf) return;

    float strength = m_suppressionStrength.load();
    float alpha    = m_overSubtractionFactor;   // Over-subtraction
    float beta     = m_spectralFloor;            // Floor to avoid musical noise

    // Feed samples into the FFT processor
    m_fftProc->ProcessBlock(samples, count);

    // Get noise floor profile from noise profiler (linear magnitudes)
    std::vector<float> noiseProfile = m_noiseProf->GetNoiseMagnitudeProfile();

    // Get current spectrum
    auto& spec = m_fftProc->GetComplexSpectrum();
    int numBins = static_cast<int>(spec.size());

    if (!noiseProfile.empty() && m_noiseProf->HasProfile())
    {
        // Apply spectral subtraction: |X_clean(k)| = max(|X(k)| - α|N(k)|, β|X(k)|)
        for (int k = 0; k < numBins && k < static_cast<int>(noiseProfile.size()); ++k)
        {
            float re  = spec[k].real();
            float im  = spec[k].imag();
            float mag = std::sqrt(re * re + im * im);
            float phase = std::atan2(im, re);

            float noiseMag  = noiseProfile[k] * strength;
            float cleanMag  = mag - alpha * noiseMag;
            float floorMag  = beta * mag;

            cleanMag = std::max(cleanMag, floorMag);

            // Reconstruct with original phase
            spec[k] = std::complex<float>(
                cleanMag * std::cos(phase),
                cleanMag * std::sin(phase));
        }
        m_fftProc->SetComplexSpectrum(spec);
    }

    // Update noise profile with current frame magnitudes (linear)
    std::vector<float> linearMags(numBins);
    for (int k = 0; k < numBins; ++k)
    {
        float re = spec[k].real(), im = spec[k].imag();
        linearMags[k] = std::sqrt(re * re + im * im) / (m_fftSize * 0.5f);
    }
    m_noiseProf->UpdateFrame(linearMags);

    // Reconstruct time-domain samples via IFFT (overlap-add)
    m_fftProc->GetProcessedSamples(samples, count);
}

void DSPEngine::UpdateAdaptiveBandStops()
{
    auto bands = m_noiseProf->GetActiveNoiseBands();
    int n = std::min(static_cast<int>(bands.size()), kMaxAdaptiveBandStops);

    for (int i = 0; i < n; ++i)
    {
        float center = bands[i].centerFreq;
        float bw     = std::max(10.0f, bands[i].freqHigh - bands[i].freqLow);

        // Only create new filter if this centre frequency changes significantly
        bool needReinit = (i >= m_numActiveBandStops);
        if (!needReinit)
        {
            // Simple hysteresis: reinit if centre changes by more than half a semitone
            // (This avoids rapid filter coefficient changes that cause artifacts)
            // For now, reinit every time bands update (called infrequently enough)
            needReinit = true;
        }

        if (needReinit)
        {
            m_bandStopFilters[i] = DesignBandStop(center, bw, m_sampleRate);
        }
    }
    m_numActiveBandStops = n;
}
