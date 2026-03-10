# CrossWave

> **Real-Time Background Noise Suppression for Windows**  
> C++ · WASAPI · KissFFT · Dear ImGui · DirectX 11

---

## Overview

CrossWave is a Windows desktop application that captures microphone audio and suppresses constant background noise in real time with **< 20 ms latency**.

It targets:
- 🌀 Fan noise (120–400 Hz)
- ❄️ Air conditioner hum (50/60 Hz harmonics)
- 🚗 Engine rumble (80–250 Hz)
- ⚡ Electrical hum (50 Hz / 60 Hz)

---

## Features

| Feature | Details |
|---|---|
| **WASAPI Exclusive Mode** | Lowest-latency direct hardware access |
| **IIR Notch Filters** | Fixed 50 Hz & 60 Hz electrical hum removal |
| **Adaptive Band-Stop Filters** | Dynamically computed from detected noise bands |
| **Spectral Subtraction** | FFT-based noise floor subtraction with over-subtraction |
| **Noise Learning** | 5-second environmental noise profile capture |
| **Real-Time Spectrum View** | Live FFT display with noise band markers |
| **Device Selection** | Dropdown menus for all WASAPI endpoints |
| **Dark UI** | Dear ImGui + D3D11, responsive at 60 fps |

---

## Requirements

### Build Requirements
- **Windows 10** or **Windows 11** (64-bit)
- **CMake 3.20+** — [cmake.org/download](https://cmake.org/download/)
- One of:
  - **Visual Studio 2019 or 2022** with "Desktop development with C++" workload *(recommended)*
  - **MinGW-w64** with GCC 12+ and Ninja

### Runtime Requirements
- **DirectX 11** (included in all Windows 10/11 installs)
- **Wired headset** with microphone
- Internet connection (first build only, to download dependencies)

### Dependencies (auto-downloaded by `build.bat`)

| Library | Version | Purpose |
|---|---|---|
| [KissFFT](https://github.com/mborgerding/kissfft) | latest | Fast Fourier Transform |
| [Dear ImGui](https://github.com/ocornut/imgui) | latest master | GUI framework |

---

## Building

### Quick Build (Recommended)

```bat
cd c:\path\to\CrossWave
build.bat
```

This script will:
1. Download **KissFFT** into `third_party/kissfft/`
2. Download **Dear ImGui** into `third_party/imgui/` and `third_party/imgui_backends/`
3. Run CMake configure
4. Build in Release mode

The output executable will be at:
```
build\bin\Release\CrossWave.exe
```

### Manual Build (MSVC)

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### Manual Build (MinGW / Ninja)

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

---

## Usage

1. Launch `CrossWave.exe`
2. Select your **microphone** from the Input Device dropdown
3. Select your **headphones** from the Output Device dropdown
4. Click **Start Audio**
5. Optionally: click **Learn Noise (5s)** in a quiet moment to capture your specific noise environment
6. Toggle **Suppression ON**
7. Adjust the **Noise Reduction Strength** slider (75% is a good starting point)

---

## Project Structure

```
CrossWave/
├── CMakeLists.txt          — CMake build configuration
├── build.bat               — One-click Windows build script
├── README.md               — This file
│
├── include/
│   ├── thread_safe_queue.h — Lock-free inter-thread queue
│   ├── audio_device_enum.h — WASAPI device enumeration
│   ├── audio_capture.h     — WASAPI microphone capture
│   ├── audio_output.h      — WASAPI headphone output
│   ├── fft_processor.h     — KissFFT overlap-add processor
│   ├── noise_profile.h     — Noise floor estimator
│   └── dsp_engine.h        — Main DSP pipeline orchestrator
│
├── src/
│   ├── main.cpp            — Entry point stub
│   ├── audio_device_enum.cpp
│   ├── audio_capture.cpp   — WASAPI capture thread
│   ├── audio_output.cpp    — WASAPI render thread
│   ├── fft_processor.cpp   — FFT + overlap-add + peak detection
│   ├── noise_profile.cpp   — Adaptive noise profiling
│   ├── dsp_engine.cpp      — Biquad filters + spectral subtraction
│   └── ui_interface.cpp    — Dear ImGui UI + WinMain
│
└── third_party/            — Auto-populated by build.bat
    ├── kissfft/
    └── imgui/
```

---

## Architecture

```
[Mic Hardware]
     │
     ▼ (WASAPI Exclusive/Shared Capture)
[Thread 1: AudioCapture]
     │ AudioBuffer (float32 mono, 48 kHz)
     ▼ CaptureQueue (ThreadSafeQueue<AudioBuffer>)
[Thread 2: DSPEngine]
     │  Stage 1: IIR Notch Filters (50/60 Hz)
     │  Stage 2: FFT Spectral Subtraction + NoiseProfile
     │  Stage 3: Adaptive Band-Stop Filters
     ▼ OutputQueue
[Thread 3: AudioOutput]
     │  Mono → Stereo
     ▼ (WASAPI Exclusive/Shared Render)
[Headphone Hardware]

[Main Thread: ImGui UI]  ←→  DSPEngine (read-only status queries)
```

---

## DSP Pipeline Details

### Notch Filters
Fixed IIR biquad notch at 50 Hz and 60 Hz (electrical hum). Coefficients use the Audio EQ Cookbook formula:

```
H(z) = (1 - 2cos(ω₀)z⁻¹ + z⁻²) / (1 - 2r·cos(ω₀)z⁻¹ + r²·z⁻²)
```

### Spectral Subtraction
For each FFT bin:
```
|X_clean(k)| = max( |X(k)| − α·|N(k)|,  β·|X(k)| )
```
- `α` = over-subtraction factor (1.0–2.5, controlled by strength slider)
- `β` = spectral floor (0.1, prevents musical noise artifacts)
- `|N(k)|` = stored noise floor magnitude per bin

### Overlap-Add Synthesis
- FFT size: 1024 samples
- Hop size: 512 samples (50% overlap)
- Window: Hann

---

## Performance Targets

| Metric | Target | Typical |
|---|---|---|
| End-to-end latency | < 20 ms | ~5–12 ms (exclusive) |
| CPU usage | < 10% | ~2–5% |
| Memory | < 200 MB | ~30–60 MB |

---

## Troubleshooting

**"WASAPI exclusive mode not supported"**  
> The device driver doesn't allow exclusive mode. The app will automatically fall back to shared mode (slightly higher latency).

**No audio output / silence**  
> Ensure both Input Device and Output Device are selected and "Start Audio" was clicked.

**High latency**  
> Try exclusive mode (happens automatically if supported). Reduce buffer size from the default 256 by modifying `kBufferFrames` in `ui_interface.cpp`.

**Build fails with missing headers**  
> Ensure `build.bat` ran successfully and `third_party/kissfft/kiss_fft.h` and `third_party/imgui/imgui.h` exist.

---

## License

This project is provided for educational and personal use.
- **KissFFT**: BSD-3-Clause
- **Dear ImGui**: MIT
- **Application code**: MIT
