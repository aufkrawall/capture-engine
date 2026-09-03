#include "benchmark_overlay_render.h"

#include <cstdio>
#include <string>
#include <vector>

#include "benchmark_manager.h"
#include "custom_overlay.h"

void RenderBenchmarkOverlay(CustomOverlay::Renderer* renderer, float x, float y, float dpiScale,
                            int viewportWidth, int viewportHeight) {
    if (!renderer)
        return;

    const BenchmarkState state = BenchmarkManager::Get().GetState();
    if (state == BenchmarkState::Idle)
        return;

    const int lineHeight = renderer->GetLineHeight();
    const float pad = 10.0f * dpiScale;

    if (state == BenchmarkState::Delaying) {
        const float remaining = BenchmarkManager::Get().GetDelayRemainingSeconds();
        char buf[64];
        snprintf(buf, sizeof(buf), "BENCHMARK STARTING IN %.0f s...", remaining);

        float textW = 0.0f, textH = 0.0f;
        renderer->CalcTextSize(buf, &textW, &textH);

        const float boxW = textW + pad * 2.0f;
        const float boxH = textH + pad * 2.0f;
        const float vpW = static_cast<float>(viewportWidth);
        const float drawX = (vpW > boxW) ? (vpW - boxW) * 0.5f : x;
        const float drawY = y + 10.0f * dpiScale;

        renderer->DrawRectFilled(drawX, drawY, boxW, boxH, 0xD0101010);
        renderer->DrawRect(drawX, drawY, boxW, boxH, CustomOverlay::Colors::Orange);
        renderer->DrawTextWithShadow(drawX + pad, drawY + pad, buf, CustomOverlay::Colors::Yellow,
                                     CustomOverlay::Colors::Black);
        return;
    }

    if (state == BenchmarkState::Recording) {
        const float elapsed = BenchmarkManager::Get().GetRecordingElapsedSeconds();
        const uint64_t frames = BenchmarkManager::Get().GetRecordingFrameCount();
        const uint32_t dur = BenchmarkManager::Get().GetDurationSeconds();

        const int elapsedSec = static_cast<int>(elapsed);
        const int mm = elapsedSec / 60;
        const int ss = elapsedSec % 60;

        char buf[128];
        if (dur > 0) {
            const int durMm = static_cast<int>(dur / 60);
            const int durSs = static_cast<int>(dur % 60);
            snprintf(buf, sizeof(buf), "[REC] BENCHMARK  %02d:%02d / %02d:%02d  (%llu frames)",
                     mm, ss, durMm, durSs, static_cast<unsigned long long>(frames));
        } else {
            snprintf(buf, sizeof(buf), "[REC] BENCHMARK  %02d:%02d  (%llu frames)",
                     mm, ss, static_cast<unsigned long long>(frames));
        }

        float textW = 0.0f, textH = 0.0f;
        renderer->CalcTextSize(buf, &textW, &textH);

        const float boxW = textW + pad * 2.0f;
        const float boxH = textH + pad * 2.0f;
        const float drawX = x;
        const float drawY = y;

        renderer->DrawRectFilled(drawX, drawY, boxW, boxH, 0xD0101010);
        renderer->DrawRect(drawX, drawY, boxW, boxH, CustomOverlay::Colors::Red);

        // Blinking indicator dot
        const bool blink = (static_cast<int>(elapsed * 2.0f) % 2) == 0;
        const uint32_t dotColor = blink ? CustomOverlay::Colors::Red : CustomOverlay::Colors::DarkGray;
        renderer->DrawRectFilled(drawX + pad, drawY + pad + (textH * 0.25f), 8.0f * dpiScale, 8.0f * dpiScale, dotColor);

        renderer->DrawTextWithShadow(drawX + pad + 12.0f * dpiScale, drawY + pad, buf,
                                     CustomOverlay::Colors::White, CustomOverlay::Colors::Black);
        return;
    }

    if (state == BenchmarkState::Results) {
        const BenchmarkResults& r = BenchmarkManager::Get().GetResults();

        std::vector<std::pair<std::string, uint32_t>> lines;
        lines.reserve(10);

        char title[128];
        snprintf(title, sizeof(title), "BENCHMARK RESULTS - %.1fs (%llu frames)",
                 r.durationSeconds, static_cast<unsigned long long>(r.totalFrames));
        lines.emplace_back(title, CustomOverlay::Colors::LabelCyan);

        char fpsLine1[128];
        snprintf(fpsLine1, sizeof(fpsLine1), "Avg: %.1f FPS  |  Max: %.1f FPS  |  Min: %.1f FPS",
                 r.presentationStats.avgFps, r.presentationStats.maxFps, r.presentationStats.minFps);
        lines.emplace_back(fpsLine1, CustomOverlay::Colors::FPS);

        char fpsLine2[128];
        snprintf(fpsLine2, sizeof(fpsLine2), "1%% Low: %.1f FPS  |  0.1%% Low: %.1f FPS",
                 r.presentationStats.onePercentLowFps, r.presentationStats.zeroPointOnePercentLowFps);
        lines.emplace_back(fpsLine2, CustomOverlay::Colors::Orange);

        // CPU row
        std::string cpuStr = "CPU: ";
        char cpuBuf[64];
        snprintf(cpuBuf, sizeof(cpuBuf), "%.0f%% (Max: %.0f%%)", r.cpuUsage.avg, r.cpuMaxCoreUsage.avg);
        cpuStr += cpuBuf;
        if (r.cpuTemp.valid) {
            char tempBuf[32];
            snprintf(tempBuf, sizeof(tempBuf), " | %.0fC", r.cpuTemp.avg);
            cpuStr += tempBuf;
        }
        if (r.cpuPower.valid) {
            char pwrBuf[32];
            snprintf(pwrBuf, sizeof(pwrBuf), " | %.0fW", r.cpuPower.avg);
            cpuStr += pwrBuf;
        }
        lines.emplace_back(cpuStr, CustomOverlay::Colors::CPU);

        // GPU row
        std::string gpuStr = "GPU: ";
        char gpuBuf[64];
        snprintf(gpuBuf, sizeof(gpuBuf), "%.0f%% (Max: %.0f%%)", r.gpuUsage.avg, r.gpuUsage.max);
        gpuStr += gpuBuf;
        if (r.gpuTemp.valid) {
            char tempBuf[32];
            snprintf(tempBuf, sizeof(tempBuf), " | %.0fC", r.gpuTemp.avg);
            gpuStr += tempBuf;
        }
        if (r.gpuPower.valid) {
            char pwrBuf[32];
            snprintf(pwrBuf, sizeof(pwrBuf), " | %.0fW", r.gpuPower.avg);
            gpuStr += pwrBuf;
        }
        if (r.gpuFan.valid && r.gpuFan.avg > 0.0f) {
            char fanBuf[32];
            snprintf(fanBuf, sizeof(fanBuf), " | %.0f RPM", r.gpuFan.avg);
            gpuStr += fanBuf;
        }
        lines.emplace_back(gpuStr, CustomOverlay::Colors::GPU);

        // Memory row
        char memBuf[128];
        snprintf(memBuf, sizeof(memBuf), "VRAM: %.1f / %.1f GB  |  RAM: %.1f / %.1f GB",
                 r.vramUsedGb.avg, r.vramTotalGb, r.ramUsedGb.avg, r.ramTotalGb);
        lines.emplace_back(memBuf, CustomOverlay::Colors::RAM);

        lines.emplace_back("Report saved to benchmarks folder (HTML)", CustomOverlay::Colors::Gray);
        lines.emplace_back("[Press Ctrl+7 to dismiss]", CustomOverlay::Colors::LabelYellow);

        float maxW = 0.0f;
        for (const auto& line : lines) {
            float lw = 0.0f, lh = 0.0f;
            renderer->CalcTextSize(line.first.c_str(), &lw, &lh);
            if (lw > maxW)
                maxW = lw;
        }

        const float boxW = maxW + pad * 2.0f;
        const float lineH = static_cast<float>(lineHeight);
        const float boxH = (static_cast<float>(lines.size()) * lineH) + pad * 2.0f + 4.0f * dpiScale;
        const float drawX = x;
        const float drawY = y;

        renderer->DrawRectFilled(drawX, drawY, boxW, boxH, 0xE814181F);
        renderer->DrawRect(drawX, drawY, boxW, boxH, 0xFF388BFD);

        float curY = drawY + pad;
        for (const auto& line : lines) {
            renderer->DrawTextWithShadow(drawX + pad, curY, line.first.c_str(), line.second,
                                         CustomOverlay::Colors::Black);
            curY += lineH;
        }
    }
}
