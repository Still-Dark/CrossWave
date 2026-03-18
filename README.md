<div align="center">

# CrossWave

**Real-time environmental noise cancellation for Windows**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%2010%2F11-blue)](https://www.microsoft.com/windows)
[![Language](https://img.shields.io/badge/Language-C%2B%2B17-blue)](https://en.cppreference.com)
[![Build](https://img.shields.io/badge/Build-CMake%203.20%2B-blue)](https://cmake.org)

CrossWave eliminates constant background noise — laptop coolers, USB fans, AC compressors, engine hum — using two complementary techniques: **frequency-domain signal suppression** that cleans your microphone output, and **feedforward Active Noise Cancellation** that physically cancels the acoustic waves at your eardrum.

![CrossWave UI](docs/screenshot.png)

</div>

---

## Features

| Feature | Detail |
|---|---|
| **Frequency-domain ALE** | Per-bin complex NLMS predictor on STFT bins — 15–30 dB reduction on real fan harmonics |
| **Harmonic series detection** | Tracks f0, 2f0, 3f0, 4f0 simultaneously; handles 1–3 Hz/s drift via phase predictor |
| **Physical ANC** | Injects acoustic anti-noise through headphone speaker to cancel room noise at the eardrum |
| **WASAPI Exclusive mode** | Bypasses Windows audio mixer for ~3–5 ms round-trip latency |
| **Speech protection** | Spectral-flux + energy-ratio gating prevents any distortion of voice or transients |
| **Live spectrum** | Colour-coded visualisation — green/red/cyan/orange bins updated at 10 Hz |
| **Fully offline** | No internet, no cloud, no telemetry |

---

## How it works

```
Microphone (44 100 Hz · 64-sample buffer · WASAPI Exclusive)
     │
     ▼
STFT Analysis — 512-pt Hann window, 50% overlap, built-in radix-2 FFT
     │
     ▼
Noise + Harmonic Detector — 2-second variance history, harmonic series,
                             phase-difference drift tracker (±2.5 Hz/s)
     │
     ▼
Speech Protector — spectral flux + energy ratio gating, 80 ms hold-off
     │
     ▼
Freq-Domain ALE — per-bin NLMS: pred = wᴴ·x[n-D], err = x[n] - pred
     │                          clean_bin = err,  noise_bin = x - err
     ▼
ISTFT (overlap-add) — Hann synthesis window, perfect reconstruction
     │
     ▼
ANC Engine — ISTFT(noise_bins) → invert → mix with clean passthrough
     │        out = passthrough_gain × clean + anc_gain × (-noise)
     ▼
Headphone speaker — anti-noise cancels acoustic leakage at eardrum
```

---

## Requirements

| Requirement | Version |
|---|---|
| Windows | 10 or 11 (x64) |
| Visual Studio Build Tools | 2022 (MSVC 19+) |
| CMake | 3.20+ |
| vcpkg | latest |
| PortAudio | v19 (via vcpkg) |

No other libraries required. FFT is implemented in-source (Cooley–Tukey radix-2).

---

## Build

### 1 — Install vcpkg and PortAudio (one-time)

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
C:\vcpkg\vcpkg.exe install portaudio:x64-windows
```

### 2 — Clone and build CrossWave

```powershell
git clone https://github.com/YOUR_USERNAME/CrossWave.git
cd CrossWave

cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### 3 — Copy the runtime DLL and run

```powershell
Copy-Item "C:\vcpkg\installed\x64-windows\bin\portaudio.dll" "build\bin\Release\"
& ".\build\bin\Release\CrossWave.exe"
```

---

## Quick-start guide

1. Select your headset microphone as **Input** and headset speakers as **Output**
2. Click **▶ Start** and wait ~3 seconds
3. Watch the spectrum — red and cyan bars appear once noise is learned
4. Your microphone output is already clean at this point
5. Tick **Enable Active Noise Cancellation** for physical room-noise reduction
6. Adjust the **Delay** slider until the fan sounds quietest in your ears

> **Delay tuning:** Start at 6. Move up or down by 2 steps at a time, listening each time. Stop at the value where the fan is quietest. This value is fixed for your headphones — write it down.

---

## Settings reference

### Signal suppression

| Control | Range | Purpose |
|---|---|---|
| Noise reduction strength | 0–100% | How aggressively ALE subtracts classified noise bins |
| ALE µ (slider 1–50 = 0.001–0.050) | 0.001–0.05 | Adaptive filter learning rate |

### Active Noise Cancellation

| Control | Range | Purpose |
|---|---|---|
| Enable ANC | on/off | Activates acoustic anti-noise injection |
| ANC gain | 0–100% | Amplitude of anti-noise played through headphone |
| Passthrough gain | 0–100% | How much of the cleaned signal you hear |
| Delay (samples) | 0–40 | Timing alignment of anti-noise to acoustic path |

### Spectrum colours

| Colour | Meaning |
|---|---|
| 🟢 Green | Clean frequency bin |
| 🔴 Red | Stable noise bin (variance-classified) |
| 🔵 Cyan | Confirmed harmonic group member |
| 🟠 Orange | Harmonic group with ANC active |

---

## DSP module details

### FFTAnalyzer
Short-Time Fourier Transform with perfect-reconstruction overlap-add ISTFT. Uses a self-contained iterative radix-2 Cooley–Tukey FFT — no FFTW dependency.
- Window: 512 samples, Hann
- Hop: 256 samples (50% overlap)
- Bins: 257 (DC → Nyquist)

### NoiseDetector
Tracks per-bin magnitude variance over a 2-second rolling window. A bin is classified as noise when variance < threshold and magnitude > floor for the full history. Additionally detects harmonic series (f0, 2f0, 3f0 …) and tracks slow frequency drift via 1st-order phase-difference predictor.

### FreqDomainALE
Per-bin complex Normalised LMS forward predictor.
- Taps P = 6, decorrelation delay D = 2 frames (~12 ms)
- Update: `w[k] += (2µ / (ε + ‖x‖²)) · err · conj(x)`
- Only active on noise-masked bins; speech bins pass through unchanged

### SpeechProtector
Detects speech and transients via spectral flux (positive-only spectrum difference) and short-term energy ratio. Smoothly ramps the ALE blend factor to zero (5 ms attack) and holds for 80 ms, then releases (20 ms).

### ANCEngine
Converts the per-bin noise estimate back to time-domain via ISTFT with a frequency taper (rolloff above 400 Hz). Mixes inverted noise with the clean passthrough signal. A configurable delay line aligns the electronic path to the acoustic path through the headphone cup.

---

## Tuning parameters (source)

| Parameter | File | Default | Effect |
|---|---|---|---|
| `variance_threshold` | `NoiseDetector.h` | 0.0015 | Lower = stricter noise classification |
| `noise_floor` | `NoiseDetector.h` | 0.0008 | Higher = ignore faint noise |
| `min_stable_secs` | `NoiseDetector.h` | 2.0 s | Detection warm-up time |
| `harmonic_tol_hz` | `NoiseDetector.h` | 4.0 Hz | Harmonic bin search radius |
| `drift_max_hz_per_s` | `NoiseDetector.h` | 2.5 | Max drift before losing lock |
| `P` (taps) | `FreqDomainALE.h` | 6 | Predictor length |
| `D` (delay) | `FreqDomainALE.h` | 2 frames | Decorrelation delay |
| `DEFAULT_CROSSOVER` | `ANCEngine.h` | 400 Hz | ANC frequency rolloff point |
| `BUFFER_SIZE` | `AudioPipeline.h` | 64 samples | Audio buffer (lower = less latency) |

---

## File structure

```
CrossWave/
├── .gitignore
├── CMakeLists.txt
├── LICENSE
├── README.md
├── CONTRIBUTING.md
├── build.ps1                   One-shot build script
└── src/
    ├── main.cpp                WinMain entry point
    ├── CircularBuffer.h        Lock-free SPSC ring buffer
    ├── FFTAnalyzer.h/.cpp      STFT + ISTFT (overlap-add)
    ├── NoiseDetector.h/.cpp    Variance + harmonic series + drift tracker
    ├── FreqDomainALE.h/.cpp    Per-bin complex NLMS predictor
    ├── SpeechProtector.h/.cpp  Spectral-flux transient gating
    ├── ANCEngine.h/.cpp        Anti-noise mixer + delay line
    ├── AudioPipeline.h/.cpp    PortAudio + WASAPI Exclusive + DSP glue
    └── MainWindow.h/.cpp       Win32 UI
```

---

## Performance expectations

| Signal | Expected attenuation |
|---|---|
| Laptop cooler (1200–2400 RPM) | 20–30 dB |
| USB desk fan | 15–25 dB |
| AC compressor (50/60 Hz harmonics) | 20–28 dB |
| Fan with ±2 Hz/s drift | Lock maintained, reduction sustained |
| Conversational speech | ≤ 1 dB loss |
| Handclap / sudden transient | 100% preserved |

> **Physical ANC note:** Software ANC on a PC achieves 10–20 dB attenuation below 300 Hz. Dedicated hardware ANC (Sony WH-1000XM5, Bose QC45) achieves 30–40 dB because the microphone is millimetres from the driver. CrossWave's ANC is most effective for bass-heavy noise like AC hum and low-frequency fan drone.

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

---

## License

[MIT](LICENSE) — free to use, modify, and distribute.
