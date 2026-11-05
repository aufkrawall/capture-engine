#include "overlay_adapter_internal.h"
#include "hook/runtime/hook_clock.h"

void OverlayAdapter::RenderContent(int viewportWidth, int viewportHeight, const OverlayConfig& cfg,
                                   const FrameLayoutSnapshot& frameLayout, bool refreshLayout,
                                   const GraphFrame* graph) {
    using namespace CustomOverlay;

    if (!ipc || !ipc->GetSharedMem())
        return;
    if (!cfg.showOverlay)
        return;
    // Get DPI scale for consistent sizing
    float dpiScale = renderer->GetDpiScale();

    // DPI-aware distance from the screen edge
    const float padding = (float)cfg.padding * dpiScale;

    const bool rowGPU = (frameLayout.rowMask & kRowGPU) != 0;
    const bool rowCPU = (frameLayout.rowMask & kRowCPU) != 0;
    const bool rowGPUClocks = (frameLayout.rowMask & kRowGPUClocks) != 0;
    const bool rowCPUClocks = (frameLayout.rowMask & kRowCPUClocks) != 0;

    if (refreshLayout) {
        if (rowGPU) {
            FormatGpuMetricsValue(cachedGpuMetricsText, sizeof(cachedGpuMetricsText),
                                  cachedSystemMetrics.gpuUsageValid, cachedSystemMetrics.gpuUsage,
                                  cachedSystemMetrics.gpuTemperatureValid, cachedSystemMetrics.gpuTemperatureC,
                                  cachedSystemMetrics.gpuPackagePowerValid, cachedSystemMetrics.gpuPackagePowerW,
                                  cachedSystemMetrics.gpuFanValid, cachedSystemMetrics.gpuFanRpm);
        }
        if (rowCPU) {
            FormatCpuMetricsValue(cachedCpuMetricsText, sizeof(cachedCpuMetricsText), cachedSystemMetrics.cpuUsage,
                                  cachedSystemMetrics.cpuMaxCoreUsage, cachedSystemMetrics.cpuTemperatureValid,
                                  cachedSystemMetrics.cpuTemperatureC, cachedSystemMetrics.cpuPackagePowerValid,
                                  cachedSystemMetrics.cpuPackagePowerW);
        }
        if (rowGPUClocks) {
            FormatGpuClocksValue(cachedGpuClocksText, sizeof(cachedGpuClocksText),
                                 cachedSystemMetrics.gpuCoreClockValid, cachedSystemMetrics.gpuCoreClockMhz,
                                 cachedSystemMetrics.gpuMemoryClockValid, cachedSystemMetrics.gpuMemoryClockMhz,
                                 cachedSystemMetrics.gpuCoreVoltageValid, cachedSystemMetrics.gpuCoreVoltageV);
        }
        if (rowCPUClocks) {
            FormatCpuClocksValue(cachedCpuClocksText, sizeof(cachedCpuClocksText),
                                 cachedSystemMetrics.cpuCoreClockValid, cachedSystemMetrics.cpuCoreClockMhz,
                                 cachedSystemMetrics.cpuMaxCoreClockValid, cachedSystemMetrics.cpuMaxCoreClockMhz);
        }

        // What the rows actually say, and the validity behind it, whenever any
        // of it changes. A row that alternates between a reading and "--" is
        // indistinguishable in the log from one that never moves, and the
        // overlay is the only place it was visible.
        char rowDigest[320];
        std::snprintf(
            rowDigest, sizeof(rowDigest), "cpu='%s' gpu='%s' gpuClk='%s' cpuClk='%s' valid=%c%c%c%c%c%c%c%c%c%c",
            cachedCpuMetricsText, cachedGpuMetricsText, cachedGpuClocksText, cachedCpuClocksText,
            cachedSystemMetrics.gpuUsageValid ? 'G' : '-', cachedSystemMetrics.vramUsageValid ? 'V' : '-',
            cachedSystemMetrics.cpuTemperatureValid ? 'c' : '-', cachedSystemMetrics.gpuTemperatureValid ? 'g' : '-',
            cachedSystemMetrics.cpuPackagePowerValid ? 'p' : '-', cachedSystemMetrics.gpuPackagePowerValid ? 'P' : '-',
            cachedSystemMetrics.gpuFanValid ? 'f' : '-', cachedSystemMetrics.gpuCoreClockValid ? 'k' : '-',
            cachedSystemMetrics.cpuCoreClockValid ? 'K' : '-', cachedSystemMetrics.cpuMaxCoreClockValid ? 'M' : '-');
        // Readability (the valid= mask and any row reading "--") is logged the moment it changes; the
        // values themselves move with every sensor update and repeat the [Sensors] summary, so a change
        // of values alone is logged at most once a minute.
        char rowValidity[sizeof(lastLoggedRowValidity)];
        std::snprintf(
            rowValidity, sizeof(rowValidity), "%.10s%c%c%c%c", std::strrchr(rowDigest, '=') + 1,
            std::strstr(cachedCpuMetricsText, "--") ? 'x' : '.', std::strstr(cachedGpuMetricsText, "--") ? 'x' : '.',
            std::strstr(cachedGpuClocksText, "--") ? 'x' : '.', std::strstr(cachedCpuClocksText, "--") ? 'x' : '.');
        if (std::strcmp(rowDigest, lastLoggedRowDigest) != 0) {
            std::snprintf(lastLoggedRowDigest, sizeof(lastLoggedRowDigest), "%s", rowDigest);
            ++rowDigestChanges;
            const ULONGLONG rowNowMs = ce::hook_clock::TickCount64();
            const bool validityChanged = std::strcmp(rowValidity, lastLoggedRowValidity) != 0;
            if (validityChanged || rowNowMs - lastRowDigestLogMs >= 60000) {
                std::snprintf(lastLoggedRowValidity, sizeof(lastLoggedRowValidity), "%s", rowValidity);
                lastRowDigestLogMs = rowNowMs;
                HookLogImportant("[Overlay] Rows changed #%u: %s", rowDigestChanges, rowDigest);
            }
        }
    }

    FillPanelInputs(cfg, frameLayout, graph, static_cast<float>(viewportWidth) - padding * 2.0f);
    layoutDirty = false;
    graphTail.valid = false;
    if (panelScene.width <= 0.0f)
        return;

    float x = padding;
    float y = padding;
    if (cfg.position == OverlayPosition::TopRight || cfg.position == OverlayPosition::BottomRight)
        x = (std::max)(0.0f, static_cast<float>(viewportWidth) - padding - panelScene.width);
    if (cfg.position == OverlayPosition::BottomLeft || cfg.position == OverlayPosition::BottomRight)
        y = (std::max)(0.0f, static_cast<float>(viewportHeight) - padding - panelScene.height);

    namespace panel = ce::overlay_panel;
    panel::SceneStyle style;
    const uint32_t bgAlpha = static_cast<uint8_t>(cfg.bgAlpha * 255.0f);
    style.background = (bgAlpha << 24) | (cfg.bgColor & 0x00FFFFFF);
    style.value = cfg.textColor ? cfg.textColor : Colors::White;
    style.fps = cfg.fpsColor ? cfg.fpsColor : Colors::White;
    style.shadow = cfg.textOutlineColor ? cfg.textOutlineColor : Colors::Black;
    style.shadowPixels = cfg.textOutline ? panel::ShadowPixelsForScale(dpiScale) : 0;
    panel::DrawScene(*renderer, panelScene, x, y, style);

    // The size and the rows behind it on change, so a panel that keeps resizing shows in the log.
    const uint64_t panelKey = (static_cast<uint64_t>(cfg.layout) << 56) ^
                              (static_cast<uint64_t>(panelScene.width) << 40) ^
                              (static_cast<uint64_t>(panelScene.height) << 24) ^ panelRowMask ^
                              (static_cast<uint64_t>(panelSensorColumns) << 16);
    if (const auto verdict = panelLogGate.Observe(panelKey)) {
        HookLogImportant("[Overlay] Panel: layout=%d size=%.0fx%.0f at=%.0f,%.0f rows=0x%x columns=0x%x repeats=%llu",
                         static_cast<int>(cfg.layout), panelScene.width, panelScene.height, x, y, panelRowMask,
                         panelSensorColumns, static_cast<unsigned long long>(verdict.suppressed));
    }

    // The graph line last, after everything that stays between text updates: a frame whose only
    // change is the graph redraws this tail alone (RenderGraphOnlyFrame).
    if (graph && panelScene.graph.valid) {
        graphTail.x = x + panelScene.graph.x;
        graphTail.y = y + panelScene.graph.y;
        graphTail.width = panelScene.graph.width;
        graphTail.height = panelScene.graph.height;
        graphTail.color = cfg.frametimeColor ? cfg.frametimeColor : style.fps;
        std::snprintf(graphTail.scaleLabel, sizeof(graphTail.scaleLabel), "%s", graph->label);
        graphTail.valid = true;
        renderer->MarkTail();
        DrawGraphTail(*graph);
    }
}

void OverlayAdapter::FillPanelInputs(const OverlayConfig& cfg, const FrameLayoutSnapshot& frameLayout,
                                     const GraphFrame* graph, float maxWidth) {
    namespace panel = ce::overlay_panel;
    using namespace CustomOverlay;
    const SystemMetrics& sensors = cachedSystemMetrics;
    constexpr float kBytesPerGb = 1024.0f * 1024.0f * 1024.0f;
    constexpr float kLayoutBasePixels = 14.0f;

    panel::Inputs in;
    in.rowMask = frameLayout.rowMask;
    in.unit = static_cast<float>(renderer->GetTextMetrics(TextSize::Base).pixelSize) / kLayoutBasePixels;
    in.maxWidth = maxWidth;

    in.api = graphicsAPI;
    in.fps = cachedFPS;
    in.averageFps = cachedAvgFPS;
    in.low1Fps = cached1PercentLow;
    in.low01Fps = cached01PercentLow;

    in.latencyLabel = ce::system_latency::SnapshotOverlayLabel(frameLayout.systemLatency);
    static const char* const kLatencyLabels[] = {
        ce::system_latency::SourceOverlayLabel(ce::system_latency::Source::Unavailable),
        ce::system_latency::SourceOverlayLabel(ce::system_latency::Source::ReflexMarkers),
        ce::system_latency::SourceOverlayLabel(ce::system_latency::Source::Estimated),
    };
    in.latencyLabelChoices = kLatencyLabels;
    in.latencyLabelChoiceCount = std::size(kLatencyLabels);
    in.latencyValid = frameLayout.systemLatency.valid;
    in.latencyMs = frameLayout.systemLatency.milliseconds;

    in.fgLabel = frameLayout.fgLabel;
    in.fgMultiplier = frameLayout.fgMultiplier;
    in.fgBaseFps = frameLayout.fgBaseFPS;
    in.fgOutputFps = frameLayout.fgOutputFPS;

    in.gpu.name = SystemMetricsCollector::Get().GetGPUName();
    in.gpu.load = {sensors.gpuUsageValid, sensors.gpuUsage};
    in.gpu.temperatureC = {sensors.gpuTemperatureValid, sensors.gpuTemperatureC};
    in.gpu.powerW = {sensors.gpuPackagePowerValid, sensors.gpuPackagePowerW};
    in.gpu.fanRpm = {sensors.gpuFanValid, sensors.gpuFanRpm};
    in.gpu.coreMhz = {sensors.gpuCoreClockValid, sensors.gpuCoreClockMhz};
    in.gpu.memoryMhz = {sensors.gpuMemoryClockValid, sensors.gpuMemoryClockMhz};
    in.gpu.voltageV = {sensors.gpuCoreVoltageValid, sensors.gpuCoreVoltageV};
    in.cpu.name = SystemMetricsCollector::Get().GetCPUName();
    in.cpu.load = sensors.cpuUsage;
    in.cpu.peakCoreLoad = sensors.cpuMaxCoreUsage;
    in.cpu.temperatureC = {sensors.cpuTemperatureValid, sensors.cpuTemperatureC};
    in.cpu.powerW = {sensors.cpuPackagePowerValid, sensors.cpuPackagePowerW};
    in.cpu.coreMhz = {sensors.cpuCoreClockValid, sensors.cpuCoreClockMhz};
    in.cpu.peakCoreMhz = {sensors.cpuMaxCoreClockValid, sensors.cpuMaxCoreClockMhz};

    panelHardware.Apply(in);
    panelSensorColumns = in.sensorColumns;
    panelRowMask = in.rowMask;

    const auto memory = [&](bool usageValid, uint64_t used, uint64_t total) {
        const MemoryValueMode mode = SelectMemoryValueMode(usageValid, used, total);
        panel::MemoryValue value;
        value.available = mode != MemoryValueMode::Unavailable;
        value.hasTotal = mode == MemoryValueMode::UsedAndTotal;
        value.usedGb = static_cast<float>(used) / kBytesPerGb;
        value.totalGb = static_cast<float>(total) / kBytesPerGb;
        return value;
    };
    in.vram = memory(sensors.vramUsageValid, sensors.vramUsed, sensors.vramTotal);
    in.ram = memory(sensors.ramUsed != 0, sensors.ramUsed, sensors.ramTotal);

    // Text the scene copies during Build, so these only have to outlive that call.
    char recordingText[32] = "";
    std::string recordingWarning;
    if ((frameLayout.rowMask & kRowRecording) != 0) {
        if (ce::recording_indicator::IsStarting(frameLayout.recordingState) && !frameLayout.recordingStatusDark) {
            in.recording.kind = panel::RecordingKind::Starting;
            in.recording.text = ce::recording_indicator::GetStartingText(frameLayout.recordingState);
        } else if (frameLayout.recordingActive) {
            in.recording.kind = panel::RecordingKind::Live;
            const int hours = static_cast<int>(frameLayout.recordingSeconds / 3600);
            const int minutes = static_cast<int>((frameLayout.recordingSeconds % 3600) / 60);
            const int seconds = static_cast<int>(frameLayout.recordingSeconds % 60);
            std::snprintf(recordingText, sizeof(recordingText), "%s %02d:%02d:%02d",
                          frameLayout.recordingAudioOnly ? "AUDIO" : "REC", hours, minutes, seconds);
            in.recording.text = recordingText;
            if (frameLayout.showOverloadWarning) {
                recordingWarning =
                    FormatRecordingHealthLabel(frameLayout.recordingWarningKind, frameLayout.recordingSustainFpsX100,
                                               frameLayout.recordingTargetFps);
                in.recording.warning = recordingWarning.c_str();
            }
        } else if (frameLayout.warnNotRecording) {
            in.recording.kind = panel::RecordingKind::NotRecording;
            in.recording.text = "NOT RECORDING";
            in.recording.visible = frameLayout.warnBlinkVisible;
        }
    }

    const panel::RendererMeasure measure(*renderer);
    if ((frameLayout.rowMask & kRowNotification) != 0) {
        const auto type = frameLayout.notificationType;
        if (type == static_cast<uint32_t>(OverlayNotificationType::ScreenshotSaved)) {
            in.notification = "Screenshot saved!";
        } else if (type == static_cast<uint32_t>(OverlayNotificationType::ScreenshotFailed)) {
            in.notification = "Screenshot failed!";
            in.notificationTone = panel::Tone::Crit;
        } else if (type == static_cast<uint32_t>(OverlayNotificationType::RecordingFinalizing)) {
            in.notification = "Finalizing recording...";
            in.notificationTone = panel::Tone::Warn;
        } else {
            const auto completion =
                ce::output_completion::DescribeOutputCompletion(static_cast<OverlayNotificationType>(type));
            in.notification = completion.text;
            if (completion.warning)
                in.notificationTone = panel::Tone::Crit;
        }
        // Reserved for every text it can become, so the panel does not jump between them.
        for (const char* reserved : {"Screenshot saved!", "Screenshot failed!", "Finalizing recording..."})
            in.notificationMinWidth = (std::max)(in.notificationMinWidth, measure.Width(reserved, panel::Size::Base));
        for (const OverlayNotificationType completion : ce::output_completion::kOutputCompletionNotificationTypes) {
            const char* reserved = ce::output_completion::DescribeOutputCompletion(completion).text;
            in.notificationMinWidth = (std::max)(in.notificationMinWidth, measure.Width(reserved, panel::Size::Base));
        }
    }

    in.benchmarkTimer = frameLayout.benchmarkTimerText;
    in.benchmarkFps = frameLayout.benchmarkFpsText;
    in.benchmarkTone = frameLayout.benchmarkState == BenchmarkState::Delaying  ? panel::Tone::Warn
                       : frameLayout.benchmarkState == BenchmarkState::Results ? panel::Tone::FgStatus
                                                                               : panel::Tone::Crit;

    if (graph) {
        in.graph.shown = true;
        in.graph.scaleLabel = graph->label;
        in.graph.peakLabel = graph->peakLabel;
        in.graph.peakTone = panel::PeakFrameTimeTone(cachedMaxFrameTime, cachedAvgFrameTimeForColor);
    }

    panel::Layout layout = panel::Layout::Hero;
    switch (cfg.layout) {
        case OverlayLayout::List:
            layout = panel::Layout::List;
            break;
        case OverlayLayout::Meters:
            layout = panel::Layout::Meters;
            break;
        case OverlayLayout::Strip:
            layout = panel::Layout::Strip;
            break;
        case OverlayLayout::Hero:
            break;
    }
    panel::Build(layout, in, measure, panelScene);
}
