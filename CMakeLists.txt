cmake_minimum_required(VERSION 3.20)
project(CrossWave VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Output directory ──────────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ─── KissFFT (vendored) ────────────────────────────────────────────────────────
# KissFFT is a single source + header library.
# We compile kiss_fft.c and kiss_fftr.c directly.
add_library(kissfft STATIC
    third_party/kissfft/kiss_fft.c
    third_party/kissfft/kiss_fftr.c
)
target_include_directories(kissfft PUBLIC third_party/kissfft)
target_compile_definitions(kissfft PUBLIC kiss_fft_scalar=float)

# ─── Dear ImGui (vendored) ─────────────────────────────────────────────────────
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui_backends/imgui_impl_win32.cpp
    third_party/imgui_backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    third_party/imgui
    third_party/imgui_backends
)
target_compile_definitions(imgui PUBLIC
    IMGUI_USER_CONFIG=""  # Use default ImGui config
    _UNICODE UNICODE
)
# ImGui Win32 backend needs d3d11 headers
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler)

# ─── CrossWave executable ─────────────────────────────────────────────────
add_executable(CrossWave WIN32
    src/audio_capture.cpp
    src/audio_output.cpp
    src/audio_device_enum.cpp
    src/dsp_engine.cpp
    src/fft_processor.cpp
    src/noise_profile.cpp
    src/ui_interface.cpp
)

target_include_directories(CrossWave PRIVATE include)

target_link_libraries(CrossWave PRIVATE
    kissfft
    imgui
    # WASAPI / Windows audio
    ole32
    oleaut32
    avrt
    # Win32 multimedia & UI
    winmm
    user32
    gdi32
    shell32
    # DirectX 11 (DX11 backend for ImGui)
    d3d11
    dxgi
    d3dcompiler
)

# ─── Compiler flags ──────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(CrossWave PRIVATE
        /W3          # Warning level 3
        /O2          # Optimize for speed
        /arch:AVX2   # Enable AVX2 SIMD (modern CPUs)
        /fp:fast     # Fast floating point
        /MP          # Multi-processor compilation
    )
    target_compile_options(kissfft PRIVATE /O2 /fp:fast /W0)
    target_compile_options(imgui    PRIVATE /O2 /W0)
    # Suppress specific MSVC warnings for cleaner output
    target_compile_definitions(CrossWave PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00   # Target Windows 10
    )
else()
    # MinGW / GCC / Clang fallback
    target_compile_options(CrossWave PRIVATE
        -O2 -march=native -ffast-math -Wall -Wextra -Wno-unused-parameter
    )
    target_compile_options(kissfft PRIVATE -O2 -ffast-math -w)
    target_compile_options(imgui    PRIVATE -O2 -w)
    target_compile_definitions(CrossWave PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    )
endif()

# ─── Install ─────────────────────────────────────────────────────────────────
install(TARGETS CrossWave RUNTIME DESTINATION .)
