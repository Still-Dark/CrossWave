@echo off
setlocal EnableDelayedExpansion

echo ==========================================================
echo  CrossWave Build Script
echo  Real-Time Noise Suppression Software
echo ==========================================================
echo.

:: ── Check for CMake ──────────────────────────────────────────────────────────
where cmake >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] CMake not found. Please install CMake 3.20+ and add it to PATH.
    echo         Download: https://cmake.org/download/
    pause
    exit /b 1
)

:: ── Detect Visual Studio generator ───────────────────────────────────────────
set GENERATOR="Visual Studio 17 2022"
where msbuild >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo [INFO] Detected MSVC toolchain - using Visual Studio 17 2022 generator
) else (
    :: Try Ninja / MinGW
    where ninja >nul 2>&1
    if %ERRORLEVEL% EQU 0 (
        set GENERATOR="Ninja"
        echo [INFO] Using Ninja generator with default C++ compiler
    ) else (
        echo [WARNING] Neither MSVC nor Ninja found.
        echo           Attempting default CMake generator...
        set GENERATOR=""
    )
)

:: ── Download third-party dependencies ────────────────────────────────────────
echo.
echo [STEP 1/4] Checking third-party dependencies...

:: Check if KissFFT is already present
if not exist "third_party\kissfft\kiss_fft.h" (
    echo [INFO] Downloading KissFFT...
    powershell -Command "& { Invoke-WebRequest -Uri 'https://github.com/mborgerding/kissfft/archive/refs/heads/master.zip' -OutFile 'kissfft.zip' -UseBasicParsing; Expand-Archive -Path 'kissfft.zip' -DestinationPath 'kissfft_tmp' -Force; Copy-Item -Path 'kissfft_tmp\kissfft-master\*' -Destination 'third_party\kissfft\' -Recurse -Force; Remove-Item 'kissfft.zip','kissfft_tmp' -Recurse -Force }"
    if %ERRORLEVEL% NEQ 0 (
        echo [ERROR] Failed to download KissFFT. Check internet connection.
        pause
        exit /b 1
    )
    echo [OK] KissFFT downloaded.
) else (
    echo [OK] KissFFT already present.
)

:: Check if Dear ImGui is already present
if not exist "third_party\imgui\imgui.h" (
    echo [INFO] Downloading Dear ImGui v1.91.x...
    powershell -Command "& { Invoke-WebRequest -Uri 'https://github.com/ocornut/imgui/archive/refs/heads/master.zip' -OutFile 'imgui.zip' -UseBasicParsing; Expand-Archive -Path 'imgui.zip' -DestinationPath 'imgui_tmp' -Force; $src = Get-ChildItem 'imgui_tmp' -Directory | Select-Object -First 1; Copy-Item -Path ($src.FullName + '\*.h') -Destination 'third_party\imgui\' -Force; Copy-Item -Path ($src.FullName + '\*.cpp') -Destination 'third_party\imgui\' -Force; Copy-Item -Path ($src.FullName + '\backends\imgui_impl_win32.*') -Destination 'third_party\imgui_backends\' -Force; Copy-Item -Path ($src.FullName + '\backends\imgui_impl_dx11.*') -Destination 'third_party\imgui_backends\' -Force; Remove-Item 'imgui.zip','imgui_tmp' -Recurse -Force }"
    if %ERRORLEVEL% NEQ 0 (
        echo [ERROR] Failed to download Dear ImGui. Check internet connection.
        pause
        exit /b 1
    )
    echo [OK] Dear ImGui downloaded.
) else (
    echo [OK] Dear ImGui already present.
)

:: ── Configure ────────────────────────────────────────────────────────────────
echo.
echo [STEP 2/4] Configuring with CMake...

if "%GENERATOR%"=="" (
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
) else if "%GENERATOR%"=="Ninja" (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
) else (
    cmake -S . -B build -G %GENERATOR% -A x64
)

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] CMake configuration failed. See above for details.
    pause
    exit /b 1
)

:: ── Build ────────────────────────────────────────────────────────────────────
echo.
echo [STEP 3/4] Building in Release mode...

cmake --build build --config Release --parallel

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Build failed. See above for details.
    pause
    exit /b 1
)

:: ── Done ─────────────────────────────────────────────────────────────────────
echo.
echo [STEP 4/4] Build complete!
echo.
echo  Output executable:
if exist "build\bin\Release\CrossWave.exe" (
    echo    build\bin\Release\CrossWave.exe
) else if exist "build\bin\CrossWave.exe" (
    echo    build\bin\CrossWave.exe
) else (
    echo    Check build\ directory for CrossWave.exe
)
echo.
echo  To run: build\bin\Release\CrossWave.exe
echo ==========================================================

pause
endlocal
