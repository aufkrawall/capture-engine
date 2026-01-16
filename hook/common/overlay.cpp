#include "overlay.h"
#include "fg_detection.h"
#include <cstdio>
#include <string>

// Global overlay instance
Overlay g_SharedOverlay;

// Helper to format bytes to GiB string
static void FormatBytes(char* buf, size_t size, uint64_t bytes) {
    float gib = (float)bytes / (1024.0f * 1024.0f * 1024.0f);
    snprintf(buf, size, "%.1f GiB", gib);
    snprintf(buf, size, "%.1f GiB", gib);
}

static const char* GetQualityString(int mode) {
    switch (mode) {
        case 0: return "Perf";
        case 1: return "Bal";
        case 2: return "Qual";
        case 3: return "UltPerf";
        case 4: return "UltQual";
        case 5: return "DLAA";
        default: return "";
    }
}

void Overlay::InitImGui(void* hwnd) {
    if (initialized) return;
    this->hwnd = hwnd;
    IMGUI_CHECKVERSION();
    context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGui::GetIO().IniFilename = nullptr; 
    ImGui_ImplWin32_Init(hwnd);
    
    // Get DPI scale for proper font sizing
    float dpiScale = GetDpiScale();
    
    // Load Segoe UI Bold for thicker, prettier text
    // Scale font size by DPI to prevent blurriness
    float baseFontSize = 18.0f; // Increased from 16.0f per user request
    float scaledFontSize = baseFontSize * dpiScale;
    
    ImGuiIO& io = ImGui::GetIO();
    
    // Configure font for sharper rendering
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3;  // Horizontal oversampling for sharper text
    fontConfig.OversampleV = 3;  // Vertical oversampling
    fontConfig.PixelSnapH = true; // Snap to pixel grid
    fontConfig.RasterizerMultiply = 1.0f; // No additional boldness
    
    mainFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", scaledFontSize, &fontConfig);
    if (!mainFont) {
        // Fallback to standard Segoe UI if Bold missing
        mainFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", scaledFontSize, &fontConfig);
    }
    
    // Build font atlas with proper DPI awareness
    io.Fonts->Build();
    
    initialized = true;
    headless = false;
}

void Overlay::InitImGuiHeadless() {
    if (initialized) return;
    this->hwnd = nullptr;
    IMGUI_CHECKVERSION();
    context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGui::GetIO().IniFilename = nullptr; 
    
    // No ImGui_ImplWin32_Init
    
    // Default DPI scale for headless (layer can update font scale later if needed)
    // Default to 1.5 (High DPI) so it's readable on 4K.
    // Ideally we should detect system DPI or read from config.
    float dpiScale = 1.5f;
    
    // Load Segoe UI Bold for thicker, prettier text
    float baseFontSize = 18.0f; 
    float scaledFontSize = baseFontSize * dpiScale;
    
    ImGuiIO& io = ImGui::GetIO();
    
    // Configure font for sharper rendering
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3; 
    fontConfig.OversampleV = 3;
    fontConfig.PixelSnapH = true; 
    fontConfig.RasterizerMultiply = 1.0f;
    
    mainFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", scaledFontSize, &fontConfig);
    if (!mainFont) {
        mainFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", scaledFontSize, &fontConfig);
    }
    
    io.Fonts->Build();
    
    initialized = true;
    headless = true;
}

void Overlay::ShutdownImGui() {
    if (!initialized) return;
    if (context) ImGui::SetCurrentContext(context);
    if (!headless) ImGui_ImplWin32_Shutdown();
    if (context) {
        ImGui::DestroyContext(context);
        context = nullptr;
    }
    initialized = false;
}

void Overlay::BeginFrame() {
    if (!initialized) return;
    if (context) ImGui::SetCurrentContext(context);
    if (!headless) ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Overlay::EndFrame() {
    if (!initialized) return;
    if (context) ImGui::SetCurrentContext(context);
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
    lastDrawResult = DrawResult::Unknown;
    if (!initialized) {
        lastDrawResult = DrawResult::SkippedNotInitialized;
        return;
    }
    if (!context) {
        lastDrawResult = DrawResult::SkippedNoContext;
        return;
    }
    ImGui::SetCurrentContext(context);
    if (hwnd && IsWindow((HWND)hwnd) && !IsWindowVisible((HWND)hwnd)) {
        lastDrawResult = DrawResult::SkippedWindowHidden;
        return;
    }
    if (!ipc || !ipc->GetSharedMem()) {
        // Fallback: Draw "Waiting for connection" if initialized but no IPC
        if (initialized && context) {
            ImGui::SetCurrentContext(context);
            if (!hwnd || (IsWindow((HWND)hwnd) && IsWindowVisible((HWND)hwnd))) {
                 // Setup basic window for the message
                ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.5f);
                if (ImGui::Begin("OverlayFallback", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
                    ImGui::TextColored(ImVec4(1, 0, 0, 1), "CaptureEngine: Waiting for connection...");
                    ImGui::End();
                    lastDrawResult = DrawResult::Drawn;
                }
            }
        } else {
             lastDrawResult = DrawResult::SkippedNoIPC;
        }
        return;
    }

    auto& mem = *ipc->GetSharedMem();
    const auto& cfg = mem.overlayConfig;

    if (!cfg.showOverlay) {
        lastDrawResult = DrawResult::SkippedShowDisabled;
        return;
    }

    // Update Throttling
    DWORD now = GetTickCount();
    // Use configurable interval from shared memory
    bool shouldUpdateText = (now - lastTextUpdateTime) >= cfg.textUpdateInterval;
    if (shouldUpdateText) {
        lastTextUpdateTime = now;
        if (metrics) {
            cachedFPS = metrics->GetCurrentFPS();
            cachedAvgFPS = metrics->GetAverageFPS();
            cached1PercentLow = metrics->Get1PercentLowFPS();
            cached01PercentLow = metrics->Get01PercentLowFPS();
        }
        
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

        // Trigger System Metrics Update and Cache
        if (cfg.showCPU || cfg.showRAM || cfg.showGPU || cfg.showVRAM) {
             SystemMetricsCollector::Get().Update();
             cachedMetrics = SystemMetricsCollector::Get().GetMetrics();
        }
    }
    // Setup Styling
    float dpiScale = GetDpiScale();
    // Don't use FontGlobalScale - fonts are already DPI-scaled at load time
    
    float bgAlpha = 1.0f; // Fully opaque by default
    ImU32 textColor = IM_COL32(255, 255, 255, 255);
    
    if (ipc && ipc->GetSharedMem()) {
        float alpha = ipc->GetSharedMem()->overlayConfig.bgAlpha;
        // Only use config value if it's explicitly set (0.1-1.0 range)
        if (alpha >= 0.1f && alpha <= 1.0f) {
            bgAlpha = alpha;
        }
        
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
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(12, 2)); // Increased padding for column separation
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
    }

    if (mainFont) ImGui::PushFont(mainFont);

    if (ImGui::Begin("Overlay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        
        // Use a table for layout: Label | Value
        float hdrScale = 1.0f;
        if (isHDR && ipc && ipc->GetSharedMem()) {
            float paperWhite = ipc->GetSharedMem()->overlayConfig.hdrPaperWhite;
            if (paperWhite > 10.0f) {
                hdrScale = paperWhite / 80.0f;
            }
        }

        auto ScaleColor = [&](ImU32 col, float scale) -> ImU32 {
            if (scale <= 1.0f) return col;
            // Scale R, G, B components but keep Alpha
            ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
            c.x *= scale;
            c.y *= scale;
            c.z *= scale;
            return ImGui::ColorConvertFloat4ToU32(c);
        };
        
        bool beginTable = ImGui::BeginTable("HudTable", 2, ImGuiTableFlags_SizingFixedFit);
        if (beginTable) {
            
            auto RenderRow = [&](const char* label, const char* value, ImU32 labelColor, ImU32 valColor) {
               ImGui::TableNextRow();
               ImGui::TableSetColumnIndex(0);
               RenderTextWithOutline(label, ScaleColor(labelColor, hdrScale), cfg.textOutline, cfg.textOutlineColor, cfg.textOutlineThickness);
               ImGui::TableSetColumnIndex(1);
               RenderTextWithOutline(value, ScaleColor(valColor, hdrScale), cfg.textOutline, cfg.textOutlineColor, cfg.textOutlineThickness);
            };


            auto RenderRowBytes = [&](const char* label, uint64_t used, uint64_t total, ImU32 labelColor, ImU32 valColor) {
               ImGui::TableNextRow();
               ImGui::TableSetColumnIndex(0);
               RenderTextWithOutline(label, ScaleColor(labelColor, hdrScale), cfg.textOutline, cfg.textOutlineColor, cfg.textOutlineThickness);
               ImGui::TableSetColumnIndex(1);
               
               // Render "4.50 GB"
               char buf[32];
               float gbUsed = (float)used / (1024.0f * 1024.0f * 1024.0f);
               snprintf(buf, 32, "%.2f GB", gbUsed);
               RenderTextWithOutline(buf, ScaleColor(valColor, hdrScale), cfg.textOutline, cfg.textOutlineColor, cfg.textOutlineThickness);
               
               // Render " of 16.00 GB" smaller
               if (total == 0 && strstr(label, "RAM")) {
                   // Fallback: Fetch RAM total directly if missing
                   MEMORYSTATUSEX memLoc;
                   memLoc.dwLength = sizeof(MEMORYSTATUSEX);
                   if (GlobalMemoryStatusEx(&memLoc)) {
                       total = memLoc.ullTotalPhys;
                   }
               }

               if (total > 0) {
                   ImGui::SameLine();
                   ImGui::SetWindowFontScale(0.75f); // 75% size for total
                   float gbTotal = (float)total / (1024.0f * 1024.0f * 1024.0f);
                   snprintf(buf, 32, " of %.2f GB", gbTotal);
                   // Use generic text color for the suffix
                   RenderTextWithOutline(buf, ScaleColor(cfg.textColor, hdrScale), cfg.textOutline, cfg.textOutlineColor, cfg.textOutlineThickness);
                   ImGui::SetWindowFontScale(1.0f);
               }
            };

             char buf[64];

             // GPU
             if (cfg.showGPU) {
                 snprintf(buf, 64, "%.0f%%", cachedMetrics.gpuUsage);
                 ImU32 valCol = GetLoadColor(cachedMetrics.gpuUsage, cfg);
                 RenderRow(SystemMetricsCollector::Get().GetGPUName(), buf, cfg.gpuColor, valCol);
             }
             
             // CPU
             if (cfg.showCPU) {
                 snprintf(buf, 64, "%.0f%% (%.0f%%)", cachedMetrics.cpuUsage, cachedMetrics.cpuMaxCoreUsage);
                 ImU32 valCol = GetLoadColor(cachedMetrics.cpuUsage, cfg);
                 RenderRow(SystemMetricsCollector::Get().GetCPUName(), buf, cfg.cpuColor, valCol);
             }
             
             // VRAM
             if (cfg.showVRAM) {
                 RenderRowBytes("VRAM", cachedMetrics.vramUsed, cachedMetrics.vramTotal, cfg.vramColor, cfg.textColor);
             }
             
             // RAM
             if (cfg.showRAM) {
                 RenderRowBytes("RAM", cachedMetrics.ramUsed, cachedMetrics.ramTotal, cfg.ramColor, cfg.textColor);
             }

            // FPS
            if (cfg.showFPS) {
                snprintf(buf, 64, "%.0f FPS", cachedFPS);
                ImU32 fpsCol = cfg.fpsColor;
                RenderRow(graphicsAPI[0] ? graphicsAPI : "FPS", buf, cfg.textColor, fpsCol);
                
                // FPS Statistics in one line: Avg / 1% Low / 0.1% Low (using cached values)
                if (cachedAvgFPS > 0.0f && cached1PercentLow > 0.0f && cached01PercentLow > 0.0f) {
                    snprintf(buf, 64, "%.0f / %.0f / %.0f FPS", cachedAvgFPS, cached1PercentLow, cached01PercentLow);
                    RenderRow("Avg/1%/0.1%", buf, cfg.textColor, cfg.fpsColor);
                }
                
                // DLSS / FG Status Line
                // Shows: DLSS SR [Preset] [Scale]x | DLSS RR [Preset] | [FG Status] | v[Version]
                
                // 1. DLSS SR Status
                auto& dlss = mem.dlssState;
                bool showDlssLine = false;
                std::string dlssText = "";
                
                if (dlss.srActive) {
                    char srBuf[64];
                    char preset = dlss.srPreset.load();
                    float scale = dlss.renderScale.load();
                    int qMode = dlss.qualityMode.load();
                    const char* qStr = GetQualityString(qMode);
                    
                    char qBuf[16] = "";
                    if (qStr[0]) snprintf(qBuf, 16, " %s", qStr);
                    
                    if (preset != '?') {
                         if (qStr[0]) snprintf(srBuf, 64, "DLSS SR %c %s", preset, qStr);
                         else snprintf(srBuf, 64, "DLSS SR %c %.1fx", preset, scale); // Fallback to scale if Q unknown
                    } else {
                        // If preset unknown (common without NvApi), show Quality/Scale if possible or just "DLSS SR"
                        if (qStr[0]) snprintf(srBuf, 64, "DLSS SR %s", qStr);
                        else snprintf(srBuf, 64, "DLSS SR %.1fx", scale);
                    }
                    dlssText += srBuf;
                    showDlssLine = true;
                }
                
                // 2. DLSS RR Status
                if (dlss.rrActive) {
                    if (!dlssText.empty()) dlssText += " | ";
                    char rrBuf[32];
                    char preset = dlss.rrPreset.load();
                     if (preset != '?') {
                         snprintf(rrBuf, 32, "RR %c", preset);
                    } else {
                        snprintf(rrBuf, 32, "RR On");
                    }
                    dlssText += rrBuf;
                    showDlssLine = true;
                }

                // 3. FG Status (Integrated)
                // Reuse existing FG logic but ensure string is separated
                extern FGCompatibility g_FGCompat;
                auto fgType = g_FGCompat.DetectLoadedFGRuntime();
                bool fgActive = g_FGCompat.IsFGActive();
                
                // Also check our captured state from NVNGX
                if (dlss.fgActive.load()) fgActive = true; 

                if (cfg.showFG && (fgType != FGCompatibility::FGType::None || fgActive)) {
                     int mult = g_FGCompat.GetFGMultiplier();
                     if (mult < 2) mult = 2; // FG implies at least 2x
                     
                     char fgBuf[32];
                     switch (fgType) {
                        case FGCompatibility::FGType::DLSS_FG: 
                            snprintf(fgBuf, 32, "FG %dx", mult);
                            break;
                        case FGCompatibility::FGType::FSR_FG: 
                            snprintf(fgBuf, 32, "FSR FG %dx", mult);
                            break;
                        case FGCompatibility::FGType::DLSS_MSFG: 
                            snprintf(fgBuf, 32, "MSFG %dx", mult);
                            break;
                        default: 
                            snprintf(fgBuf, 32, "FG %dx", mult);
                            break;
                    }
                    
                    if (!dlssText.empty()) dlssText += " | ";
                    dlssText += fgBuf;
                    showDlssLine = true;
                }
                
                // 4. Version display REMOVED as per user request

                if (showDlssLine) {
                     // Render the unified line
                     // Use a distinct color (Cyan-ish)
                     ImU32 dlssColor = IM_COL32(100, 220, 255, 255);
                     
                     // Reset to first column to span or use custom logic
                     // The HudTable has 2 columns. We can use TableHeader or span.
                     // Or just put it in the "Label" column and let it overflow? No.
                     // Better: "DLSS" as label, Rest as value.
                     
                     // If text is long, might need spanning.
                     // Let's try: Label="DLSS", Value="SR 1.5x | FG 2x | v3.7.0"
                     // If SR not active but FG is: Label="FG", Value="DLSS 2xFG ..."
                     
                     const char* label = "DLSS";
                     if (!dlss.srActive && fgActive) label = "FG";
                     
                     static int reportLog = 0;
                     if (reportLog++ % 120 == 0) {
                         HookLog("OVERLAY REPORT: Label='%s' Text='%s'", label, dlssText.c_str());
                     }

                     RenderRow(label, dlssText.c_str(), dlssColor, dlssColor);
                }
            }
            
            // Recording
            if (cfg.showRecording) {
                 if (cachedIsRecording) {
                     snprintf(buf, 64, "REC %02d:%02d:%02d", cachedRecordingSeconds / 3600, (cachedRecordingSeconds % 3600) / 60, cachedRecordingSeconds % 60);
                     ImGui::TableNextRow();
                     ImGui::TableSetColumnIndex(0);
                     // Span 2 columns
                     RenderTextWithOutline(buf, IM_COL32(255, 50, 50, 255), cfg.textOutline, cfg.textOutlineColor, cfg.textOutlineThickness);

                     uint32_t overloadFlags = mem.runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
                     if (overloadFlags != 0) {
                         const bool encOver = (overloadFlags & 1u) != 0;
                         const bool muxOver = (overloadFlags & 2u) != 0;
                         if (encOver && muxOver) {
                             snprintf(buf, 64, "ENC+I/O OVERLOAD");
                         } else if (encOver) {
                             snprintf(buf, 64, "ENC OVERLOAD");
                         } else {
                             snprintf(buf, 64, "I/O OVERLOAD");
                         }
                         ImGui::TableNextRow();
                         ImGui::TableSetColumnIndex(0);
                         RenderTextWithOutline(buf, IM_COL32(255, 140, 0, 255), cfg.textOutline, cfg.textOutlineColor, cfg.textOutlineThickness);

                         if (muxOver) {
                             uint32_t qBytes = mem.runtimeState.muxQueueBytes.load(std::memory_order_relaxed);
                             double qMB = (double)qBytes / (1024.0 * 1024.0);
                             snprintf(buf, 64, "MuxQ %.0f MB", qMB);
                             ImGui::TableNextRow();
                             ImGui::TableSetColumnIndex(0);
                             RenderTextWithOutline(buf, IM_COL32(255, 140, 0, 255), cfg.textOutline, cfg.textOutlineColor, cfg.textOutlineThickness);
                         }
                     }
                 }
            }

            // FrameTime Graph
            if (cfg.showFrameTime && metrics && cfg.showFPS) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
            }
            
            ImGui::EndTable();
        }
        
             // FrameTime Graph
            if (cfg.showFrameTime && metrics && cfg.showFPS) {
                 float minScale, maxScale;
                 metrics->GetSmartScale(minScale, maxScale);
                 
                 // Get Latency stats (Max in last 2 seconds)
                 float maxLatency = metrics->GetMaxFrameTime(2.0f);
                 float avgFrameTime = metrics->GetAverageFPS() > 0 ? (1000.0f / metrics->GetAverageFPS()) : 16.6f;
                 
                 // Scale graph height by DPI to match font scaling
                 float graphHeight = 40.0f * dpiScale;
                 
                 // MangoHud-style graph with transparent background (no blue)
                 ImGui::PushStyleColor(ImGuiCol_PlotLines, cfg.frametimeColor);
                 ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0)); // Transparent background
                 
                 float graphBuf[PerformanceMetrics::GRAPH_HISTORY_SIZE];
                 metrics->GetLastHistory(graphBuf, PerformanceMetrics::GRAPH_HISTORY_SIZE);

                 ImVec2 graphPos = ImGui::GetCursorScreenPos();
                 ImVec2 graphSize = ImVec2(ImGui::GetContentRegionAvail().x, graphHeight);
                 
                 ImGui::PlotLines("##FrameTime", graphBuf,
                             PerformanceMetrics::GRAPH_HISTORY_SIZE,
                             0, nullptr, minScale,
                             maxScale, graphSize);

                 // Draw Indicators on top of the graph
                 auto drawList = ImGui::GetWindowDrawList();
                 
                 // 1. Scale Marker (Upper Left) - Max Scale
                 char scaleBuf[32];
                 snprintf(scaleBuf, 32, "%.1f ms", maxScale);
                 ImU32 scaleColor = IM_COL32(255, 255, 255, 180);

                 // Draw little bar/hyphen (6px wide) at the max level
                 drawList->AddLine(ImVec2(graphPos.x, graphPos.y), ImVec2(graphPos.x + 6.0f * dpiScale, graphPos.y), scaleColor, 1.5f);

                 // Draw text smaller (0.6x scale), offset to the right of the bar
                 // Adjust Y slightly to align center of text with the line looks odd, usually just top alignment is fine for max marker
                 // But since it's "Max", text below the line makes sense? Or just next to it.
                 // Let's put it next to it.
                 metrics->GetSmartScale(minScale, maxScale); // Call again? No, we have it.
                 
                 drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.6f, 
                                   ImVec2(graphPos.x + 8.0f * dpiScale, graphPos.y), 
                                   scaleColor, scaleBuf);

                 // 2. Latency Value (Upper Right) - Max in last 2s
                 char latBuf[32];
                 snprintf(latBuf, 32, "%.1f ms", maxLatency);
                 
                 // Color Logic
                 ImU32 latColor = IM_COL32(0, 255, 0, 255); // Green
                 if (maxLatency > avgFrameTime * 2.0f && maxLatency > 20.0f) {
                     latColor = IM_COL32(255, 0, 0, 255); // Red (Hard Spike)
                 } else if (maxLatency > avgFrameTime * 1.5f && maxLatency > 10.0f) {
                     latColor = IM_COL32(255, 140, 0, 255); // Orange (Medium Spike)
                 }

                 ImVec2 txtSize = ImGui::CalcTextSize(latBuf);
                 drawList->AddText(ImVec2(graphPos.x + graphSize.x - txtSize.x - 2, graphPos.y), latColor, latBuf);

                 ImGui::PopStyleColor(2); // PlotLines + FrameBg
            }

    }
    ImGui::End();

    if (mainFont) ImGui::PopFont();

    ImGui::PopStyleVar(3); // Rounding + CellPadding + ItemSpacing
    ImGui::PopStyleColor(2); // WinBg, Border

    lastDrawResult = DrawResult::Drawn;
}
