#include "overlay.h"
#include <cstdio>
#include <string>

// Global overlay instance
Overlay g_SharedOverlay;

// Helper to format bytes to GiB string
static void FormatBytes(char* buf, size_t size, uint64_t bytes) {
    float gib = (float)bytes / (1024.0f * 1024.0f * 1024.0f);
    snprintf(buf, size, "%.1f GiB", gib);
}

void Overlay::InitImGui(void* hwnd) {
    if (initialized) return;
    this->hwnd = hwnd;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; 
    ImGui_ImplWin32_Init(hwnd);
    
    // Load Segoe UI Bold for thicker, prettier text
    // Default size 18.0f (larger than default 13.0f)
    ImGuiIO& io = ImGui::GetIO();
    mainFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 18.0f);
    if (!mainFont) {
        // Fallback to standard Segoe UI if Bold missing
        mainFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    }
    // If still null, ImGui uses default font
    
    initialized = true;
}

void Overlay::ShutdownImGui() {
    if (!initialized) return;
    ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext()) {
        ImGui::DestroyContext();
    }
    initialized = false;
}

void Overlay::BeginFrame() {
    if (!initialized) return;
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Overlay::EndFrame() {
    if (!initialized) return;
    ImGui::Render();
}

ImU32 Overlay::GetLoadColor(float load, const OverlayConfig& cfg) {
    // Check if colors are set (non-zero alpha)
    // If not set, use hardcoded defaults if config parser didn't set them
    // But config parser sets defaults.
    
    if (load < 50.0f) return cfg.loadColorLow;
    if (load < 85.0f) return cfg.loadColorMed;
    return cfg.loadColorHigh;
}

void Overlay::RenderTextWithOutline(const char* text, ImU32 color, bool outline, ImU32 outlineColor, float thickness) {
    auto drawList = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    
    if (outline) {
        // Draw outline by drawing text at offsets
        // Simple 4-way offset
        ImU32 outlineCol = outlineColor; 
        if ((outlineCol & 0xFF000000) == 0) outlineCol |= 0xFF000000; // Force alpha if missing

        drawList->AddText(ImVec2(pos.x - thickness, pos.y), outlineCol, text);
        drawList->AddText(ImVec2(pos.x + thickness, pos.y), outlineCol, text);
        drawList->AddText(ImVec2(pos.x, pos.y - thickness), outlineCol, text);
        drawList->AddText(ImVec2(pos.x, pos.y + thickness), outlineCol, text);
    }
    
    drawList->AddText(pos, color, text);
    
    // Advance cursor manually since we used DrawList
    ImVec2 size = ImGui::CalcTextSize(text);
    ImGui::Dummy(size); 
}

void Overlay::RenderUI() {
    if (hwnd && IsWindow((HWND)hwnd) && !IsWindowVisible((HWND)hwnd)) return;
    if (!ipc || !ipc->GetSharedMem()) return;

    auto& mem = *ipc->GetSharedMem();
    const auto& cfg = mem.overlayConfig;

    if (!cfg.showOverlay) return;

    // Update Throttling
    DWORD now = GetTickCount();
    bool shouldUpdateText = (now - lastTextUpdateTime) >= TEXT_UPDATE_INTERVAL_MS;
    if (shouldUpdateText) {
        lastTextUpdateTime = now;
        if (metrics) cachedFPS = metrics->GetCurrentFPS();
        
        // Update Recording Timer
        if (ipc->IsRecording()) {
             int64_t duration = GetTickCount64() - mem.runtimeState.recordingStartTime;
             cachedRecordingSeconds = (int)(duration / 1000);
             cachedIsRecording = true;
             
             uint32_t ringDrops = mem.frameRing.droppedFrames.load(std::memory_order_relaxed);
             uint32_t hostDrops = mem.runtimeState.hostDroppedFrames;
             cachedTotalDroppedFrames = localDroppedFrames + ringDrops + hostDrops;
        } else {
            cachedIsRecording = false;
        }

        // Trigger System Metrics Update
        if (cfg.showCPU || cfg.showRAM || cfg.showGPU || cfg.showVRAM) {
             SystemMetricsCollector::Get().Update();
        }
    }

    // Setup Styling
    float dpiScale = GetDpiScale();
    ImGui::GetIO().FontGlobalScale = (cfg.fontSize > 0.0f) ? (cfg.fontSize / 13.0f) : dpiScale;
    
    float bgAlpha = 0.7f; // Default less transparent
    ImU32 textColor = IM_COL32(255, 255, 255, 255);
    
    if (ipc && ipc->GetSharedMem()) {
        float alpha = ipc->GetSharedMem()->overlayConfig.bgAlpha;
        if (alpha > 0.0f) bgAlpha = alpha;
        
        uint32_t color = ipc->GetSharedMem()->overlayConfig.textColor;
        if (color != 0) textColor = color;
    }
    
    float padding = (float)cfg.padding;
    padding *= dpiScale;
    
    ImVec2 windowPos(padding, padding);
    ImVec2 windowPivot(0.0f, 0.0f);
    ImVec2 viewportSize = ImGui::GetMainViewport()->Size;

    switch (cfg.position) {
    case OverlayPosition::TopLeft:
        windowPos = ImVec2(padding, padding);
        windowPivot = ImVec2(0.0f, 0.0f);
        break;
    case OverlayPosition::TopRight:
        windowPos = ImVec2(viewportSize.x - padding, padding);
        windowPivot = ImVec2(1.0f, 0.0f);
        break;
    case OverlayPosition::BottomLeft:
        windowPos = ImVec2(padding, viewportSize.y - padding);
        windowPivot = ImVec2(0.0f, 1.0f);
        break;
    case OverlayPosition::BottomRight:
        windowPos = ImVec2(viewportSize.x - padding, viewportSize.y - padding);
        windowPivot = ImVec2(1.0f, 1.0f);
        break;
    }

    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, windowPivot);
    ImGui::SetNextWindowBgAlpha(bgAlpha);
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, cfg.roundedCorners);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, cfg.bgColor);
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0,0,0,0)); // No border usually
    
    // Compact Mode Padding
    if (cfg.compactMode) {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(2, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 1));
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(5, 2)); // Slightly increased padding
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
    }

    if (mainFont) ImGui::PushFont(mainFont);

    if (ImGui::Begin("Overlay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        
        // Use a table for layout: Label | Value
        
        bool beginTable = ImGui::BeginTable("HudTable", 2, ImGuiTableFlags_SizingFixedFit);
        if (beginTable) {
            
            auto RenderRow = [&](const char* label, const char* value, ImU32 labelColor, ImU32 valColor) {
               ImGui::TableNextRow();
               ImGui::TableSetColumnIndex(0);
               RenderTextWithOutline(label, labelColor, cfg.textOutline, cfg.textOutlineColor, cfg.textOutlineThickness);
               ImGui::TableSetColumnIndex(1);
               RenderTextWithOutline(value, valColor, cfg.textOutline, cfg.textOutlineColor, cfg.textOutlineThickness);
            };

            SystemMetrics sys = SystemMetricsCollector::Get().GetMetrics();
            char buf[64];

            // GPU
            if (cfg.showGPU) {
                snprintf(buf, 64, "%.0f%%", sys.gpuUsage); // 0% if invalid for now
                ImU32 valCol = GetLoadColor(sys.gpuUsage, cfg);
                RenderRow("GPU", buf, cfg.gpuColor, valCol);
            }
            
            // CPU
            if (cfg.showCPU) {
                snprintf(buf, 64, "%.0f%%", sys.cpuUsage);
                ImU32 valCol = GetLoadColor(sys.cpuUsage, cfg);
                RenderRow("CPU", buf, cfg.cpuColor, valCol);
            }
            
            // VRAM
            if (cfg.showVRAM) {
                FormatBytes(buf, 64, sys.vramUsed);
                RenderRow("VRAM", buf, cfg.vramColor, cfg.textColor);
            }

            // RAM
            if (cfg.showRAM) {
                FormatBytes(buf, 64, sys.ramUsed);
                RenderRow("RAM", buf, cfg.ramColor, cfg.textColor);
            }

            // FPS
            if (cfg.showFPS) {
                snprintf(buf, 64, "%.0f", cachedFPS);
                ImU32 fpsCol = cfg.fpsColor; // Could use frametime thresholds?
                RenderRow(graphicsAPI[0] ? graphicsAPI : "FPS", buf, cfg.textColor, fpsCol);
            }
            
            // Recording
            if (cfg.showRecording) {
                 if (cachedIsRecording) {
                     snprintf(buf, 64, "REC %02d:%02d:%02d", cachedRecordingSeconds / 3600, (cachedRecordingSeconds % 3600) / 60, cachedRecordingSeconds % 60);
                     ImGui::TableNextRow();
                     ImGui::TableSetColumnIndex(0);
                     // Span 2 columns
                     RenderTextWithOutline(buf, IM_COL32(255, 50, 50, 255), cfg.textOutline, cfg.textOutlineColor, cfg.textOutlineThickness);
                 }
            }

            // FrameTime Graph
            if (cfg.showFrameTime && metrics && cfg.showFPS) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
            }
            
            ImGui::EndTable();
        }
        
        // Graph outside table to span full width
        if (cfg.showFrameTime && metrics && cfg.showFPS) {
             float minScale, maxScale;
             metrics->GetSmartScale(minScale, maxScale);
             
             // MangoHud-style graph: often filled or specific color
             // We can use same color as FPS or standard green
             ImGui::PushStyleColor(ImGuiCol_PlotLines, cfg.frametimeColor);
             ImGui::PlotLines("##FrameTime", metrics->GetHistoryArray(),
                         PerformanceMetrics::HISTORY_SIZE,
                         metrics->GetHistoryIndex(), nullptr, minScale,
                         maxScale, ImVec2(ImGui::GetContentRegionAvail().x, 40)); // Slightly taller
             ImGui::PopStyleColor();
        }

    }
    ImGui::End();

    if (mainFont) ImGui::PopFont();

    ImGui::PopStyleVar(3); // Rounding + CellPadding + ItemSpacing
    ImGui::PopStyleColor(2); // WinBg, Border
}
