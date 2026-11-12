// Graph history and animation are shared by all overlay adapters attached to this metrics stream.
// Capture the active series under its writer lock, so the sample index, timestamp and period refer to one update.
PerformanceMetrics::GraphHistoryFrame PerformanceMetrics::PrepareGraphHistory(int64_t drawUs, GraphOutputContext output,
                                                                              float* data, int count) {
    std::lock_guard<std::mutex> graphLock(m_graphMutex);
    std::lock_guard<std::mutex> displayLock(m_displayConsumeMutex);
    const bool useDisplay = m_effectiveSource.load(std::memory_order_acquire) == FrameTimeSource::DisplayChange;
    std::unique_lock<std::mutex> presentationLock(m_presentationUpdateMutex, std::defer_lock);
    if (!useDisplay)
        presentationLock.lock();
    const MetricSeries& series = useDisplay ? m_display : m_presentation;
    GraphHistoryFrame result;
    result.state = GetFrameTimingState();
    result.identity = useDisplay ? m_displayHistoryEpoch.load(std::memory_order_acquire) * 2 + 1 : 0;
    const uint64_t samples = series.sampleCount.load(std::memory_order_acquire);
    const int historyIndex = series.historyIdx.load(std::memory_order_acquire);
    const int64_t lastSampleUs = series.lastFrameTimeUs.load(std::memory_order_relaxed);
    double sumUs = 0.0;
    int measured = 0;
    for (int i = 0; i < 60 && static_cast<uint64_t>(i) < samples; ++i) {
        const int index = (historyIndex - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
        const float milliseconds = series.history[index].load(std::memory_order_relaxed);
        if (milliseconds > 0.0f) {
            sumUs += milliseconds * 1000.0;
            ++measured;
        }
    }
    const double samplePeriodUs = measured ? sumUs / measured : 0.0;
    double displayPeriodUs = static_cast<double>(m_graphRefreshPeriodUs);
    if (displayPeriodUs <= 0.0 || (useDisplay && samplePeriodUs > displayPeriodUs))
        displayPeriodUs = samplePeriodUs;
    const int64_t lastDisplayUs = m_graphLastDisplayUs > 0 ? m_graphLastDisplayUs : lastSampleUs;
    if (result.identity != m_graphCursorIdentity) {
        m_graphScroll.Reset();
        m_graphSteps.Restart();
        m_graphCursorIdentity = result.identity;
    }
    if (result.state != FrameTimingState::DisplayStarting) {
        // Presentation samples are the presents themselves, so one slot per draw is already the sample cadence.
        // Display samples are scanouts, which extra application draws do not produce: map those draws to the
        // display slot they can reach instead.
        result.cursor = useDisplay ? m_graphScroll.Advance({result.identity, samples, lastSampleUs, samplePeriodUs,
                                                            lastDisplayUs, displayPeriodUs, drawUs, output.kind,
                                                            output.outputId})
                                   : m_graphScroll.Advance(samples);
        m_graphSteps.Observe(output.kind, output.outputId, result.cursor, drawUs, samplePeriodUs, displayPeriodUs,
                             !useDisplay);
        const uint64_t end = static_cast<uint64_t>(result.cursor) + 1;
        const uint64_t oldest = samples > HISTORY_SIZE ? samples - HISTORY_SIZE : 0;
        count = (std::min)((std::max)(count, 0), HISTORY_SIZE);
        for (int i = 0; data && i < count; ++i) {
            const uint64_t age = static_cast<uint64_t>(count - 1 - i);
            const uint64_t index = end >= age ? end - age : 0;
            data[i] = end >= age && index >= oldest && index < samples
                          ? series.history[index % HISTORY_SIZE].load(std::memory_order_relaxed) : 0.0f;
        }
    }
    ++m_graphDraws;
    if (output.kind == GraphOutputKind::kDistinctOutput)
        ++m_graphOutputs;
    if (result.cursor == m_graphLastPosition)
        ++m_graphDuplicatePositions;
    else if (m_graphLastPosition >= 0.0 && result.cursor - m_graphLastPosition >= 1.5)
        ++m_graphJumps;
    m_graphLastPosition = result.cursor;
    if (result.identity != m_graphLastIdentity) {
        ++m_graphHistoryResets;
        m_graphLastIdentity = result.identity;
        m_graphLastSampleCount = samples;
    }
    if (m_graphLogStartUs == 0)
        m_graphLogStartUs = drawUs;
    if (drawUs > m_graphLogStartUs && drawUs - m_graphLogStartUs >= 10'000'000) {
        const uint64_t published = samples >= m_graphLastSampleCount ? samples - m_graphLastSampleCount : 0;
        // Per route: draws/holds/jumps/mean |step - elapsed slots|/largest step.
        char steps[192] = {};
        m_graphSteps.Format(steps, sizeof(steps));
        HookLogImportant("[OverlayGraph] draws=%llu outputs=%llu samples=%llu samePosition=%llu jumps=%llu "
                         "resets=%llu samplePeriodUs=%.1f displayPeriodUs=%.1f %s",
                         static_cast<unsigned long long>(m_graphDraws), static_cast<unsigned long long>(m_graphOutputs),
                         static_cast<unsigned long long>(published),
                         static_cast<unsigned long long>(m_graphDuplicatePositions),
                         static_cast<unsigned long long>(m_graphJumps),
                         static_cast<unsigned long long>(m_graphHistoryResets), samplePeriodUs, displayPeriodUs, steps);
        m_graphDraws = m_graphOutputs = m_graphDuplicatePositions = m_graphJumps = m_graphHistoryResets = 0;
        m_graphSteps.ResetCounters();
        m_graphLastSampleCount = samples;
        m_graphLogStartUs = drawUs;
    }
    return result;
}
