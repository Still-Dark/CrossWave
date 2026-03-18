# CrossWave — build script for PowerShell
# Run from the CrossWave folder: .\build.ps1
# Requires: Visual Studio 2022 Build Tools, vcpkg at C:\vcpkg

param(
    [switch]$Clean,
    [string]$VcpkgRoot = "C:\vcpkg"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path "CMakeLists.txt")) {
    Write-Error "Run this script from the CrossWave root folder (where CMakeLists.txt lives)."
    exit 1
}

# Install vcpkg if missing
if (-not (Test-Path "$VcpkgRoot\vcpkg.exe")) {
    Write-Host "[1/4] vcpkg not found — cloning..." -ForegroundColor Yellow
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Write-Error "Git is required. Install from https://git-scm.com"
    }
    git clone https://github.com/microsoft/vcpkg.git $VcpkgRoot
    & "$VcpkgRoot\bootstrap-vcpkg.bat" -disableMetrics
} else {
    Write-Host "[1/4] vcpkg found at $VcpkgRoot" -ForegroundColor Green
}

# Install PortAudio
Write-Host "[2/4] Installing portaudio:x64-windows..." -ForegroundColor Yellow
& "$VcpkgRoot\vcpkg.exe" install portaudio:x64-windows
Write-Host "[2/4] PortAudio ready." -ForegroundColor Green

# Clean build folder if requested
if ($Clean -and (Test-Path "build")) {
    Write-Host "Cleaning build folder..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force build
}

# CMake configure
Write-Host "[3/4] Configuring..." -ForegroundColor Yellow
cmake -B build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { Write-Error "CMake configure failed."; exit 1 }

# Build
Write-Host "[4/4] Building Release..." -ForegroundColor Yellow
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { Write-Error "Build failed."; exit 1 }

# Copy DLL
$dll = "$VcpkgRoot\installed\x64-windows\bin\portaudio.dll"
$dest = "build\bin\Release\"
if (Test-Path $dll) {
    Copy-Item $dll $dest -Force
    Write-Host "portaudio.dll copied." -ForegroundColor Green
}

$exe = "build\bin\Release\CrossWave.exe"
if (Test-Path $exe) {
    Write-Host "`n Build complete: $(Resolve-Path $exe)" -ForegroundColor Green
    Write-Host " Run with:  & '.\$exe'" -ForegroundColor Cyan
} else {
    Write-Warning "Build finished but CrossWave.exe not found at expected path."
}
