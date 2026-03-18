# Contributing to CrossWave

Thanks for your interest in contributing. CrossWave is a personal DSP project — contributions that improve real-world noise cancellation performance or fix bugs are very welcome.

## Getting started

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/your-feature-name`
3. Make your changes
4. Build and test (see README)
5. Submit a pull request

## Development setup

Follow the build instructions in the README. You will need:
- Visual Studio 2022 Build Tools
- CMake 3.20+
- vcpkg with `portaudio:x64-windows`

## Areas where contributions are especially welcome

- **Improved harmonic detection** — better f0 estimation, more robust drift tracking
- **Feedback ANC** — inner-mic based error correction on top of the existing feedforward path
- **Linux / macOS port** — PortAudio and the DSP core are cross-platform; only `MainWindow.cpp` is Win32
- **Real-time parameter UI** — expose `variance_threshold`, `harmonic_tol_hz` etc. in the UI without a recompile
- **PESQ / SNR benchmarking** — automated test suite with reference audio files
- **Documentation** — diagrams, explanations, tuning guides

## Code style

- C++17, MSVC-compatible
- No external DSP libraries — keep the FFT self-contained
- Real-time callback code (`processBuffer`) must be allocation-free
- All new modules go in `src/` as a `.h` + `.cpp` pair

## Reporting bugs

Open a GitHub Issue and include:
- Your headphone model
- Whether WASAPI Exclusive mode activated
- The noise type (fan, AC, etc.)
- What the spectrum display showed (colours, frequencies)

## Pull request checklist

- [ ] Builds cleanly with `cmake --build build --config Release` (no new warnings)
- [ ] Tested with at least one real noise source
- [ ] Speech passthrough verified (voice not distorted)
- [ ] README updated if new settings or behaviour added
