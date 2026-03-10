#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>

// Dear ImGui
#include "../third_party/imgui/imgui.h"
#include "../third_party/imgui_backends/imgui_impl_win32.h"
#include "../third_party/imgui_backends/imgui_impl_dx11.h"

#include "../include/audio_device_enum.h"
#include "../include/audio_capture.h"
#include "../include/audio_output.h"
#include "../include/dsp_engine.h"
#include "../include/thread_safe_queue.h"

// Forward declare ImGui Win32 message handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ─── Application State ───────────────────────────────────────────────────────

struct AppState
{
    // Audio devices
    std::vector<AudioDeviceInfo> captureDevices;
    std::vector<AudioDeviceInfo> renderDevices;
    int selectedCapture = 0;
    int selectedRender  = 0;
    bool devicesDirty   = false; // Restart audio when device changes

    // Suppression control
    bool  suppressionOn     = false;
    float suppressionStrength = 0.75f; // 0.0 – 1.0

    // Noise learning
    bool  learningActive   = false;
    float learningProgress = 0.0f;

    // Status
    bool   audioRunning    = false;
    double captureLatencyMs = 0.0;
    double outputLatencyMs  = 0.0;
    bool   exclusiveCapture = false;
    bool   exclusiveRender  = false;
    std::string lastError;

    // Spectrum display
    std::vector<float> spectrumDb;
    std::vector<NoiseBand> activeBands;

    // FPS / timing
    float frameTimeMs = 0.0f;
};

// ─── Globals ─────────────────────────────────────────────────────────────────

static ID3D11Device*           g_pD3DDevice        = nullptr;
static ID3D11DeviceContext*    g_pD3DDeviceContext  = nullptr;
static IDXGISwapChain*         g_pSwapChain         = nullptr;
static ID3D11RenderTargetView* g_pMainRTV           = nullptr;
static HWND                    g_hWnd               = nullptr;
static bool                    g_swapChainOccluded  = false;

// Audio pipeline globals
static std::unique_ptr<AudioDeviceEnumerator> g_devEnum;
static std::unique_ptr<AudioCapture>          g_capture;
static std::unique_ptr<AudioOutput>           g_output;
static std::unique_ptr<DSPEngine>             g_dsp;

static ThreadSafeQueue<AudioBuffer> g_captureQueue(64);
static ThreadSafeQueue<AudioBuffer> g_outputQueue(64);

static AppState g_state;

// ─── DX11 helpers ────────────────────────────────────────────────────────────

static bool CreateDX11Device()
{
    DXGI_SWAP_CHAIN_DESC sd             = {};
    sd.BufferCount                       = 2;
    sd.BufferDesc.Width                  = 0;
    sd.BufferDesc.Height                 = 0;
    sd.BufferDesc.Format                 = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator  = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                             = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                      = g_hWnd;
    sd.SampleDesc.Count                  = 1;
    sd.Windowed                          = TRUE;
    sd.SwapEffect                        = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevelArray[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain,
        &g_pD3DDevice, &featureLevel, &g_pD3DDeviceContext);

    return SUCCEEDED(hr);
}

static void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                            reinterpret_cast<void**>(&pBackBuffer));
    if (pBackBuffer)
    {
        g_pD3DDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_pMainRTV);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_pMainRTV) { g_pMainRTV->Release(); g_pMainRTV = nullptr; }
}

static void CleanupDX11Device()
{
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain        = nullptr; }
    if (g_pD3DDeviceContext) { g_pD3DDeviceContext->Release(); g_pD3DDeviceContext = nullptr; }
    if (g_pD3DDevice)        { g_pD3DDevice->Release();        g_pD3DDevice        = nullptr; }
}

// ─── Audio pipeline management ───────────────────────────────────────────────

static void StartAudioPipeline()
{
    // Stop existing pipeline
    if (g_dsp)     { g_dsp->Stop();     g_dsp.reset();     }
    if (g_output)  { g_output->Stop();  g_output.reset();  }
    if (g_capture) { g_capture->Stop(); g_capture.reset(); }
    g_captureQueue.Reset();
    g_outputQueue.Reset();

    g_state.audioRunning = false;
    g_state.lastError.clear();

    // Determine device IDs
    std::wstring captureId, renderId;
    if (!g_state.captureDevices.empty() && g_state.selectedCapture < (int)g_state.captureDevices.size())
        captureId = g_state.captureDevices[g_state.selectedCapture].id;
    if (!g_state.renderDevices.empty() && g_state.selectedRender < (int)g_state.renderDevices.size())
        renderId = g_state.renderDevices[g_state.selectedRender].id;

    constexpr int kSampleRate    = 48000;
    constexpr int kBufferFrames  = 256;
    constexpr int kFFTSize       = 1024;

    // Create and init capture
    g_capture = std::make_unique<AudioCapture>();
    if (!g_capture->Init(captureId, kSampleRate, kBufferFrames, &g_captureQueue))
    {
        g_state.lastError = "Capture init failed: " + g_capture->GetLastError();
        g_capture.reset();
        return;
    }
    g_state.exclusiveCapture = g_capture->IsExclusive();
    g_state.captureLatencyMs = g_capture->GetLatencyMs();

    // Create and init output
    g_output = std::make_unique<AudioOutput>();
    if (!g_output->Init(renderId, kSampleRate, kBufferFrames, &g_outputQueue))
    {
        g_state.lastError = "Output init failed: " + g_output->GetLastError();
        g_output.reset(); g_capture.reset();
        return;
    }
    g_state.exclusiveRender  = g_output->IsExclusive();
    g_state.outputLatencyMs  = g_output->GetLatencyMs();

    // Create and init DSP engine
    g_dsp = std::make_unique<DSPEngine>();
    if (!g_dsp->Init(kSampleRate, kFFTSize, g_state.suppressionStrength,
                     &g_captureQueue, &g_outputQueue))
    {
        g_state.lastError = "DSP engine init failed";
        g_dsp.reset(); g_output.reset(); g_capture.reset();
        return;
    }

    g_dsp->EnableSuppression(g_state.suppressionOn);
    g_dsp->SetSuppressionStrength(g_state.suppressionStrength);

    // Start all threads
    g_capture->Start();
    g_output->Start();
    g_dsp->Start();

    g_state.audioRunning = true;
}

static void StopAudioPipeline()
{
    if (g_dsp)     { g_dsp->Stop();     g_dsp.reset();     }
    if (g_output)  { g_output->Stop();  g_output.reset();  }
    if (g_capture) { g_capture->Stop(); g_capture.reset(); }
    g_captureQueue.Shutdown();
    g_outputQueue.Shutdown();
    g_state.audioRunning = false;
}

// ─── Win32 Window Procedure ──────────────────────────────────────────────────

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) break;
        CleanupRenderTarget();
        g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
                                    DXGI_FORMAT_UNKNOWN, 0);
        CreateRenderTarget();
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ─── Modern dark theme ────────────────────────────────────────────────────────

static void ApplyDarkTheme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 8.0f;
    style.FrameRounding     = 5.0f;
    style.GrabRounding      = 4.0f;
    style.PopupRounding     = 5.0f;
    style.ScrollbarRounding = 6.0f;
    style.ItemSpacing       = ImVec2(8, 5);
    style.FramePadding      = ImVec2(6, 4);
    style.WindowPadding     = ImVec2(10, 10);
    style.ScrollbarSize     = 12.0f;
    style.TabRounding       = 5.0f;

    ImVec4* colors = style.Colors;
    // Dark background palette
    colors[ImGuiCol_WindowBg]         = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_ChildBg]          = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
    colors[ImGuiCol_PopupBg]          = ImVec4(0.09f, 0.10f, 0.12f, 0.97f);
    colors[ImGuiCol_Border]           = ImVec4(0.25f, 0.27f, 0.32f, 1.00f);
    colors[ImGuiCol_FrameBg]          = ImVec4(0.15f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive]    = ImVec4(0.23f, 0.25f, 0.32f, 1.00f);
    colors[ImGuiCol_TitleBg]          = ImVec4(0.07f, 0.08f, 0.10f, 1.00f);
    colors[ImGuiCol_TitleBgActive]    = ImVec4(0.10f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_MenuBarBg]        = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    // Accent: electric blue / teal
    colors[ImGuiCol_CheckMark]        = ImVec4(0.25f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]       = ImVec4(0.20f, 0.65f, 0.90f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.30f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]           = ImVec4(0.18f, 0.42f, 0.65f, 1.00f);
    colors[ImGuiCol_ButtonHovered]    = ImVec4(0.22f, 0.55f, 0.80f, 1.00f);
    colors[ImGuiCol_ButtonActive]     = ImVec4(0.28f, 0.68f, 1.00f, 1.00f);
    colors[ImGuiCol_Header]           = ImVec4(0.18f, 0.40f, 0.62f, 0.80f);
    colors[ImGuiCol_HeaderHovered]    = ImVec4(0.22f, 0.50f, 0.76f, 0.80f);
    colors[ImGuiCol_HeaderActive]     = ImVec4(0.28f, 0.60f, 0.90f, 0.80f);
    colors[ImGuiCol_Tab]              = ImVec4(0.12f, 0.16f, 0.22f, 1.00f);
    colors[ImGuiCol_TabHovered]       = ImVec4(0.22f, 0.50f, 0.78f, 1.00f);
    colors[ImGuiCol_TabActive]        = ImVec4(0.18f, 0.42f, 0.68f, 1.00f);
    colors[ImGuiCol_PlotLines]        = ImVec4(0.30f, 0.78f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotHistogram]    = ImVec4(0.20f, 0.60f, 0.90f, 1.00f);
    colors[ImGuiCol_Text]             = ImVec4(0.88f, 0.90f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]     = ImVec4(0.40f, 0.42f, 0.48f, 1.00f);
    colors[ImGuiCol_Separator]        = ImVec4(0.25f, 0.27f, 0.32f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]      = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]    = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);
}

// ─── Helper: convert C++ wide string → UTF-8 for ImGui ───────────────────────
static std::string WstrToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), size, nullptr, nullptr);
    return s;
}

// ─── UI Rendering ────────────────────────────────────────────────────────────

static void RenderUI()
{
    // Full-screen invisible host window
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("CrossWave_Main", nullptr,
                 ImGuiWindowFlags_NoTitleBar  |
                 ImGuiWindowFlags_NoResize    |
                 ImGuiWindowFlags_NoMove      |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_MenuBar);

    // ── Menu bar ────────────────────────────────────────────────────────────
    if (ImGui::BeginMenuBar())
    {
        ImGui::TextColored(ImVec4(0.25f, 0.75f, 1.0f, 1.0f), "🎧 CrossWave");
        ImGui::SameLine(io.DisplaySize.x - 120.0f);
        ImGui::TextDisabled("v1.0  |  %.1f fps", io.Framerate);
        ImGui::EndMenuBar();
    }

    // ── Left panel (controls) ────────────────────────────────────────────────
    ImGui::BeginChild("LeftPanel", ImVec2(280, 0), true);

    // --- Power toggle ---
    ImGui::Spacing();
    ImGui::PushFont(nullptr); // Use default font
    bool wasOn = g_state.suppressionOn;
    if (g_state.suppressionOn)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.60f, 0.30f, 1.0f));
    else
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.50f, 0.15f, 0.15f, 1.0f));

    float btnW = ImGui::GetContentRegionAvail().x;
    const char* powerLabel = g_state.suppressionOn ? "[ ON ]  Suppression Active" : "[ OFF ] Suppression Disabled";
    if (ImGui::Button(powerLabel, ImVec2(btnW, 40)))
    {
        g_state.suppressionOn = !g_state.suppressionOn;
        if (g_dsp) g_dsp->EnableSuppression(g_state.suppressionOn);
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Noise Reduction Strength ---
    ImGui::TextColored(ImVec4(0.60f, 0.75f, 1.0f, 1.0f), "Noise Reduction Strength");
    int strengthPct = static_cast<int>(g_state.suppressionStrength * 100.0f);
    if (ImGui::SliderInt("##Strength", &strengthPct, 0, 100, "%d%%"))
    {
        g_state.suppressionStrength = strengthPct / 100.0f;
        if (g_dsp) g_dsp->SetSuppressionStrength(g_state.suppressionStrength);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Device selection ---
    ImGui::TextColored(ImVec4(0.60f, 0.75f, 1.0f, 1.0f), "Input Device (Microphone)");

    // Build capture device name list
    std::vector<const char*> captureNames;
    std::vector<std::string> captureNamesUtf8;
    for (const auto& d : g_state.captureDevices)
    {
        captureNamesUtf8.push_back(WstrToUtf8(d.name));
    }
    for (const auto& s : captureNamesUtf8) captureNames.push_back(s.c_str());

    if (!captureNames.empty())
    {
        if (ImGui::Combo("##InputDev", &g_state.selectedCapture,
                         captureNames.data(), (int)captureNames.size()))
            g_state.devicesDirty = true;
    }
    else
        ImGui::TextDisabled("No capture devices found");

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.60f, 0.75f, 1.0f, 1.0f), "Output Device (Headphones)");

    std::vector<const char*> renderNames;
    std::vector<std::string> renderNamesUtf8;
    for (const auto& d : g_state.renderDevices)
        renderNamesUtf8.push_back(WstrToUtf8(d.name));
    for (const auto& s : renderNamesUtf8) renderNames.push_back(s.c_str());

    if (!renderNames.empty())
    {
        if (ImGui::Combo("##OutputDev", &g_state.selectedRender,
                         renderNames.data(), (int)renderNames.size()))
            g_state.devicesDirty = true;
    }
    else
        ImGui::TextDisabled("No render devices found");

    ImGui::Spacing();

    // --- Start / Stop buttons ---
    bool isRunning = g_state.audioRunning;
    if (!isRunning)
    {
        if (ImGui::Button("  Start Audio  ", ImVec2(btnW, 30)))
            StartAudioPipeline();
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.25f, 0.10f, 1.0f));
        if (ImGui::Button("  Stop Audio  ", ImVec2(btnW, 30)))
            StopAudioPipeline();
        ImGui::PopStyleColor();
    }

    // Restart if device changed while running
    if (g_state.devicesDirty && isRunning)
    {
        g_state.devicesDirty = false;
        StartAudioPipeline();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Noise Learn button ---
    ImGui::TextColored(ImVec4(0.60f, 0.75f, 1.0f, 1.0f), "Noise Profile Learning");
    ImGui::TextWrapped("Record 5 seconds of environmental noise to build a noise floor profile.");
    ImGui::Spacing();

    if (g_state.learningActive)
    {
        ImGui::ProgressBar(g_state.learningProgress, ImVec2(btnW, 20), "Learning...");
        if (g_dsp && !g_dsp->IsLearning())
        {
            g_state.learningActive   = false;
            g_state.learningProgress = 1.0f;
        }
        else if (g_dsp)
        {
            g_state.learningProgress = g_dsp->GetLearningProgress();
        }
    }
    else
    {
        bool canLearn = isRunning;
        if (!canLearn) ImGui::BeginDisabled();

        if (ImGui::Button("  Learn Noise (5s)  ", ImVec2(btnW, 30)))
        {
            if (g_dsp)
            {
                g_dsp->StartNoiseLearn(5.0f);
                g_state.learningActive   = true;
                g_state.learningProgress = 0.0f;
            }
        }
        if (!canLearn) { ImGui::EndDisabled(); ImGui::TextDisabled("(Start audio first)"); }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // --- Status ---
    ImGui::TextColored(ImVec4(0.60f, 0.75f, 1.0f, 1.0f), "Status");
    auto statusColor = isRunning
        ? ImVec4(0.25f, 0.85f, 0.40f, 1.0f)
        : ImVec4(0.70f, 0.30f, 0.30f, 1.0f);
    ImGui::TextColored(statusColor, isRunning ? "● Running" : "● Stopped");

    if (isRunning)
    {
        ImGui::TextDisabled("Capture: %.1f ms (%s)", g_state.captureLatencyMs,
                             g_state.exclusiveCapture ? "Exclusive" : "Shared");
        ImGui::TextDisabled("Output:  %.1f ms (%s)", g_state.outputLatencyMs,
                             g_state.exclusiveRender ? "Exclusive" : "Shared");
        double totalLatency = g_state.captureLatencyMs + g_state.outputLatencyMs;
        ImGui::TextDisabled("Total:   ~%.1f ms end-to-end", totalLatency);
    }

    if (!g_state.lastError.empty())
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "Error:");
        ImGui::TextWrapped("%s", g_state.lastError.c_str());
    }

    // --- Active noise bands ---
    if (!g_state.activeBands.empty())
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.60f, 0.75f, 1.0f, 1.0f), "Detected Noise Bands");
        for (const auto& b : g_state.activeBands)
        {
            ImGui::TextDisabled("  %.0f – %.0f Hz  (%.1f dBFS)",
                b.freqLow, b.freqHigh, b.avgMagnitude);
        }
    }

    ImGui::EndChild();

    // ── Right panel (spectrum) ───────────────────────────────────────────────
    ImGui::SameLine();
    ImGui::BeginChild("RightPanel", ImVec2(0, 0), true);

    ImGui::TextColored(ImVec4(0.60f, 0.75f, 1.0f, 1.0f), "Real-Time FFT Spectrum");
    ImGui::TextDisabled("Frequency domain view (0 Hz – 24 kHz), magnitudes in dBFS");
    ImGui::Spacing();

    // Pull spectrum from DSP engine every frame
    if (g_dsp && g_state.audioRunning)
    {
        g_state.spectrumDb   = g_dsp->GetMagnitudeSpectrum();
        g_state.activeBands  = g_dsp->GetActiveNoiseBands();
    }

    if (!g_state.spectrumDb.empty())
    {
        // Normalise dBFS [-120, 0] → [0, 1] for plotting
        std::vector<float> plotData(g_state.spectrumDb.size());
        for (size_t i = 0; i < plotData.size(); ++i)
        {
            float db    = std::max(-120.0f, g_state.spectrumDb[i]);
            plotData[i] = (db + 120.0f) / 120.0f;
        }

        ImVec2 plotSize = ImGui::GetContentRegionAvail();
        plotSize.y -= 60;  // Leave room for labels

        // Draw the spectrum histogram
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
                              g_state.suppressionOn
                                  ? ImVec4(0.20f, 0.65f, 1.00f, 0.85f)
                                  : ImVec4(0.50f, 0.50f, 0.55f, 0.70f));
        ImGui::PlotHistogram("##Spectrum",
                             plotData.data(),
                             static_cast<int>(plotData.size()),
                             0, nullptr, 0.0f, 1.0f, plotSize);
        ImGui::PopStyleColor();

        // Frequency axis labels
        ImGui::TextDisabled("0 Hz");
        ImGui::SameLine(plotSize.x * 0.25f);   ImGui::TextDisabled("6 kHz");
        ImGui::SameLine(plotSize.x * 0.50f);   ImGui::TextDisabled("12 kHz");
        ImGui::SameLine(plotSize.x * 0.75f);   ImGui::TextDisabled("18 kHz");
        ImGui::SameLine(plotSize.x - 50);      ImGui::TextDisabled("24 kHz");

        ImGui::Spacing();

        // Noise band markers as colored text
        for (const auto& b : g_state.activeBands)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f),
                               " [%.0f Hz]", b.centerFreq);
        }
    }
    else
    {
        ImVec2 sz = ImGui::GetContentRegionAvail();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + sz.y * 0.4f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + sz.x * 0.3f);
        ImGui::TextDisabled("Start audio to see real-time spectrum...");
    }

    ImGui::EndChild();

    ImGui::End();
}

// ─── Win32 Entry Point ───────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    // Initialize COM for WASAPI
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Register Win32 window class
    WNDCLASSEXW wc  = {};
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"CrossWave";
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(
        0, wc.lpszClassName,
        L"CrossWave — Real-Time Noise Suppression",
        WS_OVERLAPPEDWINDOW,
        100, 100, 1100, 680,
        nullptr, nullptr, hInstance, nullptr);

    if (!CreateDX11Device())
    {
        DestroyWindow(g_hWnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        CoUninitialize();
        return 1;
    }

    CreateRenderTarget();
    ShowWindow(g_hWnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hWnd);

    // Setup Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyDarkTheme();

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pD3DDevice, g_pD3DDeviceContext);

    // Enumerate audio devices
    try
    {
        g_devEnum = std::make_unique<AudioDeviceEnumerator>();
        g_state.captureDevices = g_devEnum->GetCaptureDevices();
        g_state.renderDevices  = g_devEnum->GetRenderDevices();

        // Pre-select default devices
        auto setDefault = [](std::vector<AudioDeviceInfo>& devs, int& sel, const std::wstring& defaultId)
        {
            for (int i = 0; i < (int)devs.size(); ++i)
                if (devs[i].id == defaultId) { sel = i; return; }
        };
        setDefault(g_state.captureDevices, g_state.selectedCapture,
                   g_devEnum->GetDefaultCaptureDeviceId());
        setDefault(g_state.renderDevices,  g_state.selectedRender,
                   g_devEnum->GetDefaultRenderDeviceId());
    }
    catch (...) { g_state.lastError = "Failed to enumerate audio devices"; }

    // Main loop
    bool done = false;
    while (!done)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // Handle swap chain occlusion
        if (g_swapChainOccluded)
        {
            HRESULT hr = g_pSwapChain->Present(0, DXGI_PRESENT_TEST);
            if (hr == DXGI_STATUS_OCCLUDED) { Sleep(10); continue; }
            g_swapChainOccluded = false;
        }

        // New ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderUI();

        // Render
        ImGui::Render();
        const float clearColor[] = { 0.06f, 0.07f, 0.09f, 1.0f };
        g_pD3DDeviceContext->OMSetRenderTargets(1, &g_pMainRTV, nullptr);
        g_pD3DDeviceContext->ClearRenderTargetView(g_pMainRTV, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0); // VSync
        if (hr == DXGI_STATUS_OCCLUDED) g_swapChainOccluded = true;
    }

    // Shutdown
    StopAudioPipeline();
    g_devEnum.reset();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDX11Device();
    DestroyWindow(g_hWnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    CoUninitialize();
    return 0;
}
