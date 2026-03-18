cmake_minimum_required(VERSION 3.20)
project(CrossWave VERSION 4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /WX- /MP /O2)
endif()

# ── PortAudio detection (no toolchain flag needed) ───────────────────────────
find_package(portaudio CONFIG QUIET)

if(NOT portaudio_FOUND)
    set(_VCPKG_ROOTS
        "C:/vcpkg/installed/x64-windows"
        "$ENV{VCPKG_ROOT}/installed/x64-windows"
        "$ENV{LOCALAPPDATA}/vcpkg/installed/x64-windows"
        "C:/dev/vcpkg/installed/x64-windows"
        "C:/tools/vcpkg/installed/x64-windows"
    )
    foreach(ROOT ${_VCPKG_ROOTS})
        if(EXISTS "${ROOT}/include/portaudio.h")
            message(STATUS "PortAudio found at: ${ROOT}")
            add_library(portaudio STATIC IMPORTED GLOBAL)
            set_target_properties(portaudio PROPERTIES
                IMPORTED_LOCATION         "${ROOT}/lib/portaudio.lib"
                IMPORTED_LOCATION_RELEASE "${ROOT}/lib/portaudio.lib"
                IMPORTED_LOCATION_DEBUG   "${ROOT}/debug/lib/portaudio.lib"
                INTERFACE_INCLUDE_DIRECTORIES "${ROOT}/include"
            )
            set(portaudio_FOUND TRUE)
            break()
        endif()
    endforeach()
endif()

if(NOT portaudio_FOUND)
    message(FATAL_ERROR
        "\nPortAudio not found. Run:\n"
        "  C:/vcpkg/vcpkg.exe install portaudio:x64-windows\n")
endif()

# ── Sources ──────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/AudioPipeline.cpp
    src/FFTAnalyzer.cpp
    src/NoiseDetector.cpp
    src/FreqDomainALE.cpp
    src/SpeechProtector.cpp
    src/ANCEngine.cpp
    src/MainWindow.cpp
)

add_executable(CrossWave WIN32 ${SOURCES})
target_include_directories(CrossWave PRIVATE src)
target_link_libraries(CrossWave PRIVATE portaudio winmm comctl32 gdi32 shlwapi)
target_compile_definitions(CrossWave PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

set_target_properties(CrossWave PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "CrossWave")

install(TARGETS CrossWave DESTINATION .)
message(STATUS "CrossWave v4 configured — run: cmake --build build --config Release")
