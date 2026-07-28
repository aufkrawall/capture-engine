                                         Colors::LabelYellow, shadowColor);
        } else {
            const char* recLabel = frameLayout.recordingAudioOnly ? "AUDIO" : "REC";
            int hours = (int)(frameLayout.recordingSeconds / 3600);
            int minutes = (int)((frameLayout.recordingSeconds % 3600) / 60);
            int seconds = (int)(frameLayout.recordingSeconds % 60);

            if (frameLayout.showOverloadWarning) {
                const std::string overloadLabel =
                    FormatEncoderOverloadLabel(frameLayout.recordingSustainFpsX100, frameLayout.recordingTargetFps);
                std::snprintf(buf, sizeof(buf), "%s %02d:%02d:%02d %s", recLabel, hours, minutes, seconds,
                              overloadLabel.c_str());
                renderer->DrawTextWithShadow(labelCol, cursorY, buf, Colors::Red, shadowColor);
            } else {
                // Normal recording display
                snprintf(buf, 64, "%s %02d:%02d:%02d", recLabel, hours, minutes, seconds);
                renderer->DrawTextWithShadow(labelCol, cursorY, buf, Colors::Red, shadowColor);
            }
        }
        cursorY += lineHeight;
    }

    // Frame time graph labels and markers
    if (showGraph) {
        uint32_t graphLabelColor = Colors::Green;
        uint32_t grayColor = 0xFFB0B0B0;  // Light gray for scale marker
        float smallFontScale = 0.75f;     // Smaller font for graph labels

        // Scale marker: small gray line at top left with ceiling value (with ms
        // unit)
        float scaleLineLength = 15.0f * dpiScale;
        float scaleLineY = graphY + 1.0f * dpiScale;
        renderer->DrawLine(graphX + 4 * dpiScale, scaleLineY, graphX + 4 * dpiScale + scaleLineLength, scaleLineY,
                           grayColor, 1.0f * dpiScale);

        // Scale marker text (ceiling value with ms unit in small gray font)
        snprintf(buf, 64, "%.0f ms", graphMaxVal);
        float scaleTextWidth = 0, scaleTextHeight = 0;
        renderer->CalcTextSizeScaled(buf, &scaleTextWidth, &scaleTextHeight, smallFontScale);
        renderer->DrawTextScaledWithShadow(graphX + 6 * dpiScale + scaleLineLength, scaleLineY - scaleTextHeight * 0.5f,
                                           buf, grayColor, shadowColor, smallFontScale);

        // Current frame time display at top right of graph
        // Color based on comparison with average: green (close), yellow (spike),
        // red (stutter)
        uint32_t frameTimeColor = Colors::Green;
        if (recentAvgFrameTime > 0.001f) {
            float ratio = recentMaxFrameTime / recentAvgFrameTime;
            if (ratio > 2.0f) {
                frameTimeColor = Colors::Red;  // Bad stutter (2x average)
            } else if (ratio > 1.5f) {
                frameTimeColor = Colors::Yellow;  // Moderate spike (1.5x average)
            }
        }

        snprintf(buf, 64, "%.1f ms", recentMaxFrameTime);
        float ftTextWidth = 0, ftTextHeight = 0;
        renderer->CalcTextSizeScaled(buf, &ftTextWidth, &ftTextHeight, smallFontScale);
        renderer->DrawTextScaledWithShadow(graphX + graphWidth - ftTextWidth - 4 * dpiScale,
                                           scaleLineY - ftTextHeight * 0.5f, buf, frameTimeColor, shadowColor,
                                           smallFontScale);
    }

    (void)viewportWidth;
    (void)viewportHeight;
}
