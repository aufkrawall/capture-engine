#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadSource(const char* filename) {
    const std::filesystem::path source = std::filesystem::current_path() / "mediaengine" / filename;
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string ReadProjectSource(const std::filesystem::path& relativePath) {
    std::ifstream file(std::filesystem::current_path() / relativePath, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

void ExpectBufferEmptyHandledBeforeGenericFailure(const std::string& source) {
    const size_t emptyStatus = source.find("if (hr == AUDCLNT_S_BUFFER_EMPTY)");
    ASSERT_NE(emptyStatus, std::string::npos);
    const size_t genericFailure = source.find("if (FAILED(hr))", emptyStatus);
    ASSERT_NE(genericFailure, std::string::npos);
    EXPECT_LT(emptyStatus, genericFailure);
    EXPECT_EQ(source.substr(emptyStatus, genericFailure - emptyStatus).find("pCaptureClient->ReleaseBuffer("),
              std::string::npos);
}

}  // namespace

TEST(AudioCaptureSourceTest, BufferEmptyNeverReleasesAnUnacquiredPacket) {
    const std::string endpointSource = ReadSource("audio_capture.cpp");
    const std::string appSource = ReadSource("app_audio_capture.cpp");
    ASSERT_FALSE(endpointSource.empty());
    ASSERT_FALSE(appSource.empty());

    ExpectBufferEmptyHandledBeforeGenericFailure(endpointSource);
    ExpectBufferEmptyHandledBeforeGenericFailure(appSource);
}

TEST(AudioCaptureSourceTest, PacketAllocationFailureCannotEscapeCaptureThread) {
    const std::string endpointSource = ReadSource("audio_capture.cpp");
    const std::string appSource = ReadSource("app_audio_capture.cpp");
    ASSERT_FALSE(endpointSource.empty());
    ASSERT_FALSE(appSource.empty());

    EXPECT_NE(endpointSource.find("packet allocation failed"), std::string::npos);
    EXPECT_NE(endpointSource.find("after packet allocation failure"), std::string::npos);
    EXPECT_NE(appSource.find("packet allocation failed"), std::string::npos);
    EXPECT_NE(appSource.find("after packet allocation failure"), std::string::npos);
    EXPECT_NE(endpointSource.find("capture queue insertion failed"), std::string::npos);
    EXPECT_NE(appSource.find("capture queue insertion failed"), std::string::npos);
    EXPECT_NE(endpointSource.find("packetQueue.emplace_back(std::move(packet));"), std::string::npos);
    EXPECT_NE(appSource.find("packetQueue.emplace_back(std::move(packet));"), std::string::npos);
}

TEST(AudioCaptureSourceTest, UnsupportedStreamLatencyIsReportedAsExpectedTelemetry) {
    const std::string endpointSource = ReadSource("audio_capture.cpp");
    const std::string appSource = ReadSource("app_audio_capture.cpp");
    ASSERT_FALSE(endpointSource.empty());
    ASSERT_FALSE(appSource.empty());

    for (const std::string* source : {&endpointSource, &appSource}) {
        const size_t query = source->find("GetStreamLatency(&streamLatency)");
        const size_t unsupported = source->find("else if (hr == E_NOTIMPL)", query);
        const size_t zeroFallback = source->find("streamLatency100ns", unsupported);
        ASSERT_NE(query, std::string::npos);
        ASSERT_NE(unsupported, std::string::npos);
        ASSERT_NE(zeroFallback, std::string::npos);
        EXPECT_LT(query, unsupported);
    }

    EXPECT_NE(endpointSource.find("GetStreamLatency is not implemented for this capture client"),
              std::string::npos);
    EXPECT_NE(appSource.find("GetStreamLatency is not implemented for this process-loopback client"),
              std::string::npos);
    EXPECT_NE(appSource.find("automatic render-endpoint latency probing remains authoritative"), std::string::npos);
    EXPECT_EQ(endpointSource.find("GetStreamLatency failed"), std::string::npos);
    EXPECT_EQ(appSource.find("GetStreamLatency failed"), std::string::npos);
}

TEST(AudioCaptureSourceTest, EveryCaptureRouteUsesOwnerAcknowledgedEpochRejoin) {
    const std::string appSource = ReadSource("app_audio_capture.cpp");
    const std::string endpointSource = ReadSource("audio_capture.cpp");
    const std::string mediaSource = ReadSource("mediaengine.cpp");
    ASSERT_FALSE(appSource.empty());
    ASSERT_FALSE(endpointSource.empty());
    ASSERT_FALSE(mediaSource.empty());

    EXPECT_NE(appSource.find("captureEpoch.fetch_add(1"), std::string::npos);
    EXPECT_NE(appSource.find("packet.captureEpoch = captureEpoch.load("), std::string::npos);
    EXPECT_NE(endpointSource.find("captureEpoch_.fetch_add(1"), std::string::npos);
    EXPECT_NE(endpointSource.find("packet.captureEpoch = captureEpoch_.load("), std::string::npos);
    EXPECT_NE(mediaSource.find("IsAudioCaptureEpochTransition("), std::string::npos);
    EXPECT_NE(mediaSource.find("FlushCaptureResamplerForEpoch("), std::string::npos);
    EXPECT_NE(mediaSource.find("ServiceAudioEpochResetOnPull("), std::string::npos);
    EXPECT_NE(mediaSource.find("epochResetRequested->store(packet.captureEpoch"), std::string::npos);
    EXPECT_NE(mediaSource.find("const uint64_t deferredCaptureEpoch = packet.captureEpoch;"), std::string::npos);
    EXPECT_NE(mediaSource.find("static_cast<unsigned long long>(deferredCaptureEpoch)"), std::string::npos);
    EXPECT_NE(mediaSource.find("epochResetAcknowledged->store(requested"), std::string::npos);
    EXPECT_NE(mediaSource.find("sourceTimestamps[srcIdx] = 0"), std::string::npos);
}

TEST(AudioCaptureSourceTest, InitialProcessLoopbackEpochMarkerSurvivesUntilWorkerConsumption) {
    const std::string source = ReadSource("app_audio_capture.cpp");
    ASSERT_FALSE(source.empty());

    const size_t threadStart = source.find("bool AppAudioCapture::StartCaptureThreadForCurrentClient()");
    const size_t initialize = source.find("bool AppAudioCapture::InitializeCaptureForPID", threadStart);
    ASSERT_NE(threadStart, std::string::npos);
    ASSERT_NE(initialize, std::string::npos);
    const std::string threadStartBody = source.substr(threadStart, initialize - threadStart);
    EXPECT_EQ(threadStartBody.find("packetQueue.clear()"), std::string::npos);

    const size_t initializeEnd = source.find("bool AppAudioCapture::ReactivateClientForPID", initialize);
    ASSERT_NE(initializeEnd, std::string::npos);
    const std::string initializeBody = source.substr(initialize, initializeEnd - initialize);
    const size_t activationCall = initializeBody.find("ActivateClientForPID(pid, true)");
    const size_t threadStartCall = initializeBody.find("StartCaptureThreadForCurrentClient()");
    ASSERT_NE(activationCall, std::string::npos);
    ASSERT_NE(threadStartCall, std::string::npos);
    EXPECT_LT(activationCall, threadStartCall);

    const size_t activate = source.find("bool AppAudioCapture::ActivateClientForPID");
    const size_t captureLoop = source.find("void AppAudioCapture::CaptureLoop()", activate);
    ASSERT_NE(activate, std::string::npos);
    ASSERT_NE(captureLoop, std::string::npos);
    const std::string activateBody = source.substr(activate, captureLoop - activate);
    EXPECT_NE(activateBody.find("QueueCaptureEpochMarker(AudioPacketRecordType::EpochStart"), std::string::npos);
}

TEST(AudioCaptureSourceTest, ProcessLoopbackReactivationClosesPriorEpochBeforePublishingReplacement) {
    const std::string source = ReadSource("app_audio_capture.cpp");
    ASSERT_FALSE(source.empty());

    const size_t loopBegin = source.find("void AppAudioCapture::CaptureLoop()");
    const size_t reactivate = source.find("const bool ok = ReactivateClientForPID", loopBegin);
    const size_t loopEnd = source.find("void AppAudioCapture::ProcessMonitorLoop()", reactivate);
    ASSERT_NE(loopBegin, std::string::npos);
    ASSERT_NE(reactivate, std::string::npos);
    ASSERT_NE(loopEnd, std::string::npos);

    const std::string beforeReactivate = source.substr(loopBegin, reactivate - loopBegin);
    EXPECT_NE(beforeReactivate.find("QueueCaptureEpochMarker(AudioPacketRecordType::EndOfStream"), std::string::npos);
    const std::string afterReactivate = source.substr(reactivate, loopEnd - reactivate);
    EXPECT_NE(afterReactivate.find("captureEpochOpen = true"), std::string::npos);
    EXPECT_NE(afterReactivate.find("if (captureEpochOpen)"), std::string::npos);
    EXPECT_NE(afterReactivate.find("\"capture loop exit\""), std::string::npos);
}

TEST(AudioCaptureSourceTest, ProcessLoopbackComStateLivesInDisposableInheritedHandleWorker) {
    const std::string mediaSource = ReadSource("mediaengine.cpp");
    const std::string proxySource = ReadSource("process_loopback_capture.cpp");
    const std::string workerSource = ReadSource("process_loopback_worker.cpp");
    const std::string workerHostSource = ReadProjectSource("captureengine/process_loopback_worker_host.cpp");
    ASSERT_FALSE(mediaSource.empty());
    ASSERT_FALSE(proxySource.empty());
    ASSERT_FALSE(workerSource.empty());
    ASSERT_FALSE(workerHostSource.empty());

    EXPECT_NE(mediaSource.find("std::unique_ptr<ProcessLoopbackCapture> appCapture"), std::string::npos);
    EXPECT_NE(proxySource.find("GetModuleFileNameW(nullptr, executablePath"), std::string::npos);
    EXPECT_NE(proxySource.find("--process-loopback-worker"), std::string::npos);
    EXPECT_NE(proxySource.find("LaunchRestrictedChildProcess"), std::string::npos);
    EXPECT_NE(proxySource.find("{worker->mappingHandle, worker->packetEvent, worker->stopEvent}"), std::string::npos);
    EXPECT_EQ(proxySource.find("process_loopback_helper.exe"), std::string::npos);
    EXPECT_NE(proxySource.find("retiredWorkers_.push_back(std::move(activeWorker_))"), std::string::npos);
    EXPECT_NE(workerSource.find("AppAudioCapture capture"), std::string::npos);
    EXPECT_NE(workerSource.find("Recycling after process-loopback reactivation"), std::string::npos);
    EXPECT_NE(workerHostSource.find("MediaEngine_RunProcessLoopbackWorker"), std::string::npos);
    EXPECT_NE(workerHostSource.find("IsInheritedHandle"), std::string::npos);
    EXPECT_NE(workerHostSource.find("IsUnsignaledEvent"), std::string::npos);
    EXPECT_NE(workerHostSource.find("ValidateInheritedTransport"), std::string::npos);
    EXPECT_NE(workerHostSource.find("SetHandleInformation"), std::string::npos);
}

TEST(AudioCaptureSourceTest, AppAudioCaptureEndMarkerOrdersEveryFanoutRouteBeforeTimelineSilence) {
    const std::string appSource = ReadSource("app_audio_capture.cpp");
    const std::string mediaSource = ReadSource("mediaengine.cpp");
    ASSERT_FALSE(appSource.empty());
    ASSERT_FALSE(mediaSource.empty());

    const size_t endMarker = appSource.find("marker.endOfStream = recordType == AudioPacketRecordType::EndOfStream");
    const size_t endMarkerQueue = appSource.find("packetQueue.emplace_back(std::move(marker));", endMarker);
    const size_t captureLoop = appSource.find("void AppAudioCapture::CaptureLoop()");
    const size_t dataQueue = appSource.find("packetQueue.emplace_back(std::move(packet));", captureLoop);
    const size_t orderedEndCall =
        appSource.find("QueueCaptureEpochMarker(AudioPacketRecordType::EndOfStream", dataQueue);
    ASSERT_NE(dataQueue, std::string::npos);
    ASSERT_NE(endMarker, std::string::npos);
    ASSERT_NE(endMarkerQueue, std::string::npos);
    EXPECT_LT(endMarker, endMarkerQueue);
    ASSERT_NE(orderedEndCall, std::string::npos);
    EXPECT_LT(dataQueue, orderedEndCall);

    const size_t fanout = mediaSource.find("captureFanoutQueues[routeIdx].push_back(packet)");
    const size_t routeEnd = mediaSource.find("packet.recordType == AudioPacketRecordType::EndOfStream");
    const size_t routeResume = mediaSource.find("src.appCaptureRouteEnded->store(false", routeEnd);
    ASSERT_NE(fanout, std::string::npos);
    ASSERT_NE(routeEnd, std::string::npos);
    ASSERT_NE(routeResume, std::string::npos);
    EXPECT_LT(fanout, routeEnd);
    EXPECT_LT(routeEnd, routeResume);
    EXPECT_NE(mediaSource.find("src.appCaptureRouteEnded->store(true", routeEnd), std::string::npos);
    EXPECT_NE(mediaSource.find("src.timelineValid = true", routeEnd), std::string::npos);
    EXPECT_NE(mediaSource.find("src.sourceType != AudioConfig::AppAudio || appCaptureRouteEnded"), std::string::npos);
}

TEST(AudioCaptureSourceTest, AppAudioStopDrainsAlreadyCommittedPacketsWithoutRecovery) {
    const std::string source = ReadSource("app_audio_capture.cpp");
    ASSERT_FALSE(source.empty());

    const size_t loopBegin = source.find("void AppAudioCapture::CaptureLoop()");
    const size_t loopEnd = source.find("void AppAudioCapture::ProcessMonitorLoop()", loopBegin);
    ASSERT_NE(loopBegin, std::string::npos);
    ASSERT_NE(loopEnd, std::string::npos);
    const std::string loopBody = source.substr(loopBegin, loopEnd - loopBegin);

    EXPECT_NE(loopBody.find("while (true)"), std::string::npos);
    EXPECT_NE(loopBody.find("readNextPacketSize(\"final stop drain\")"), std::string::npos);
    EXPECT_NE(loopBody.find("if (drainingAfterStop)"), std::string::npos);
    EXPECT_NE(loopBody.find("Final stop drain queued"), std::string::npos);
}

TEST(AudioCaptureSourceTest, ActivationFailureDoesNotRepeatModeIndependentTimeout) {
    const std::string appSource = ReadSource("app_audio_capture.cpp");
    ASSERT_FALSE(appSource.empty());

    const size_t activationFailure = appSource.find("if (!ActivateAudioInterfaceForPID(pid, &activatedClient))");
    ASSERT_NE(activationFailure, std::string::npos);
    const size_t returnFailure = appSource.find("return false;", activationFailure);
    ASSERT_NE(returnFailure, std::string::npos);
    const size_t nextModeContinue = appSource.find("continue;", activationFailure);
    EXPECT_TRUE(nextModeContinue == std::string::npos || returnFailure < nextModeContinue);
    EXPECT_NE(appSource.find("Activation cancelled for PID"), std::string::npos);
    EXPECT_NE(appSource.find("WaitForMultipleObjects(waitHandleCount, waitHandles"), std::string::npos);
    EXPECT_NE(appSource.find("SetEvent(stopEvent_)"), std::string::npos);
}

TEST(AudioCaptureSourceTest, SilentStallRecoveryUsesCurrentActivationQualification) {
    const std::string appSource = ReadSource("app_audio_capture.cpp");
    ASSERT_FALSE(appSource.empty());

    const size_t loopBegin = appSource.find("void AppAudioCapture::CaptureLoop()");
    const size_t recoveryCall = appSource.find("ShouldReactivateForSilentStall(", loopBegin);
    ASSERT_NE(loopBegin, std::string::npos);
    ASSERT_NE(recoveryCall, std::string::npos);
    const std::string recoveryWindow = appSource.substr(recoveryCall, 512);
    EXPECT_NE(recoveryWindow.find("currentActivationQualified"), std::string::npos);
    EXPECT_EQ(recoveryWindow.find("sawAnyPacket"), std::string::npos);
    EXPECT_NE(appSource.find("currentActivationQualified = false;", loopBegin), std::string::npos);
    EXPECT_NE(appSource.find("currentActivationQualified = true;", loopBegin), std::string::npos);
}

TEST(AudioCaptureSourceTest, CaptureRecyclesWhenTargetRenderSessionRequiresFreshBinding) {
    const std::string appSource = ReadSource("app_audio_capture.cpp");
    const std::string selectionSource = ReadSource("process_tree_selection.h");
    const std::string sessionMonitorSource = ReadSource("process_audio_session_monitor.cpp");
    const std::string workerSource = ReadSource("process_loopback_worker.cpp");
    ASSERT_FALSE(appSource.empty());
    ASSERT_FALSE(selectionSource.empty());
    ASSERT_FALSE(sessionMonitorSource.empty());
    ASSERT_FALSE(workerSource.empty());

    const size_t monitorStart = appSource.find("audioSessionMonitor_.Start(stopEvent_)");
    const size_t clientActivation = appSource.find("ActivateClientForPID(pid, true)", monitorStart);
    ASSERT_NE(monitorStart, std::string::npos);
    ASSERT_NE(clientActivation, std::string::npos);
    EXPECT_LT(monitorStart, clientActivation);
    EXPECT_NE(sessionMonitorSource.find("COINIT_MULTITHREADED"), std::string::npos);
    EXPECT_NE(sessionMonitorSource.find("RegisterSessionNotification(notification)"), std::string::npos);
    EXPECT_NE(sessionMonitorSource.find("UnregisterSessionNotification(notification)"), std::string::npos);
    EXPECT_NE(sessionMonitorSource.find("GetSessionEnumerator(&sessionEnumerator)"), std::string::npos);
    EXPECT_NE(sessionMonitorSource.find("sessionEnumerator->GetCount(&sessionCount)"), std::string::npos);
    EXPECT_NE(sessionMonitorSource.find("sessionEnumerator->GetSession(sessionIndex, &session)"), std::string::npos);
    EXPECT_NE(sessionMonitorSource.find("std::shared_ptr<SharedState>"), std::string::npos);
    EXPECT_NE(sessionMonitorSource.find("acceptingNotifications"), std::string::npos);
    EXPECT_NE(sessionMonitorSource.find("std::array<AudioSessionCreation"), std::string::npos);
    EXPECT_EQ(sessionMonitorSource.find("std::deque<AudioSessionCreation>"), std::string::npos);
    const size_t activationFunction = appSource.find("bool AppAudioCapture::ActivateClientForPID");
    const size_t processTreeSnapshot =
        appSource.find("const auto processTree = SnapshotProcessTree();", activationFunction);
    const size_t generationBoundary = appSource.find("SnapshotGenerationAndObservedProcessIds(", activationFunction);
    const size_t activateInterface =
        appSource.find("ActivateAudioInterfaceForPID(pid, &activatedClient)", activationFunction);
    ASSERT_NE(processTreeSnapshot, std::string::npos);
    ASSERT_NE(generationBoundary, std::string::npos);
    ASSERT_NE(activateInterface, std::string::npos);
    EXPECT_LT(processTreeSnapshot, generationBoundary);
    EXPECT_LT(generationBoundary, activateInterface);
    EXPECT_NE(appSource.find("activationHadObservedTargetSession_.store("), std::string::npos);
    EXPECT_NE(selectionSource.find("ShouldRecycleCaptureForSessionCreation("), std::string::npos);
    const size_t recyclePolicy = appSource.find("ShouldRecycleCaptureForSessionCreation(");
    EXPECT_NE(recyclePolicy, std::string::npos);
    const size_t captureLoop = appSource.find("void AppAudioCapture::CaptureLoop()");
    const size_t notificationDrain = appSource.find("audioSessionMonitor_.TakeSessionCreations(", captureLoop);
    const size_t packetRead = appSource.find("readNextPacketSize(\"outer\")", captureLoop);
    ASSERT_NE(notificationDrain, std::string::npos);
    ASSERT_NE(packetRead, std::string::npos);
    EXPECT_LT(notificationDrain, packetRead);
    EXPECT_EQ(appSource.find("DiscardSessionCreations"), std::string::npos);
    const size_t recycleRequest = appSource.find("workerRecycleRequested.store(true");
    ASSERT_NE(recycleRequest, std::string::npos);
    EXPECT_NE(appSource.substr(recycleRequest, 256).find("SetEvent(packetReadyEvent_)"), std::string::npos);
    EXPECT_NE(workerSource.find("capture.IsWorkerRecycleRequested()"), std::string::npos);
    EXPECT_EQ(appSource.find("unqualified_process_tree_growth"), std::string::npos);
}

TEST(AudioCaptureSourceTest, EpochResetUsesTerminalResamplerFlushResultInsteadOfReportedDelay) {
    const std::string mediaSource = ReadSource("mediaengine.cpp");
    ASSERT_FALSE(mediaSource.empty());

    const size_t captureFlush = mediaSource.find("bool FlushCaptureResamplerForEpoch(");
    const size_t resetService = mediaSource.find("bool ServiceAudioEpochResetOnPull(", captureFlush);
    const size_t audioOnlyFlush = mediaSource.find("void FlushAudioOnlyResamplerTails(", resetService);
    ASSERT_NE(captureFlush, std::string::npos);
    ASSERT_NE(resetService, std::string::npos);
    ASSERT_NE(audioOnlyFlush, std::string::npos);

    const std::string captureBody = mediaSource.substr(captureFlush, resetService - captureFlush);
    const std::string resetBody = mediaSource.substr(resetService, audioOnlyFlush - resetService);
    EXPECT_NE(captureBody.find("AudioResampler::FlushResult::Complete"), std::string::npos);
    EXPECT_NE(resetBody.find("AudioResampler::FlushResult::Complete"), std::string::npos);
    EXPECT_EQ(captureBody.find("remainingDelay != 0"), std::string::npos);
    EXPECT_EQ(resetBody.find("remainingDelay != 0"), std::string::npos);
}

TEST(AudioCaptureSourceTest, EndpointWasapiInterfacesRemainWorkerOwned) {
    const std::string source = ReadSource("audio_capture.cpp");
    ASSERT_FALSE(source.empty());

    const size_t startBegin = source.find("bool AudioCapture::Start(");
    const size_t resolveBegin = source.find("bool AudioCapture::ResolveCaptureDevice(", startBegin);
    ASSERT_NE(startBegin, std::string::npos);
    ASSERT_NE(resolveBegin, std::string::npos);
    const std::string startBody = source.substr(startBegin, resolveBegin - startBegin);
    EXPECT_EQ(startBody.find("CoInitializeEx("), std::string::npos);
    EXPECT_EQ(startBody.find("CoCreateInstance("), std::string::npos);
    EXPECT_EQ(startBody.find("ActivateAndStartClientOnDevice()"), std::string::npos);
    EXPECT_NE(startBody.find("startupCv_.wait("), std::string::npos);

    const size_t stopBegin = source.find("void AudioCapture::Stop(");
    const size_t loopBegin = source.find("void AudioCapture::CaptureLoop(", stopBegin);
    ASSERT_NE(stopBegin, std::string::npos);
    ASSERT_NE(loopBegin, std::string::npos);
    const std::string stopBody = source.substr(stopBegin, loopBegin - stopBegin);
    EXPECT_EQ(stopBody.find("pCaptureClient->Release()"), std::string::npos);
    EXPECT_EQ(stopBody.find("pAudioClient->Release()"), std::string::npos);
    EXPECT_EQ(stopBody.find("CoUninitialize()"), std::string::npos);
    EXPECT_NE(stopBody.find("reactivationMutex_"), std::string::npos);

    const size_t packetState = source.find("UINT32 packetLength", loopBegin);
    ASSERT_NE(packetState, std::string::npos);
    const std::string workerStartup = source.substr(loopBegin, packetState - loopBegin);
    EXPECT_NE(workerStartup.find("CoInitializeEx("), std::string::npos);
    EXPECT_NE(workerStartup.find("CoCreateInstance("), std::string::npos);
    EXPECT_NE(workerStartup.find("ResolveCaptureDevice()"), std::string::npos);
    EXPECT_NE(workerStartup.find("ActivateAndStartClientOnDevice()"), std::string::npos);
    EXPECT_NE(workerStartup.find("CompleteStartup(true)"), std::string::npos);

    const size_t releaseAllAtExit = source.find("ReleaseAllInterfacesOnWorkerThread();", packetState);
    const size_t workerCoUninitialize = source.find("CoUninitialize();", releaseAllAtExit);
    EXPECT_NE(releaseAllAtExit, std::string::npos);
    EXPECT_NE(workerCoUninitialize, std::string::npos);
    EXPECT_LT(releaseAllAtExit, workerCoUninitialize);
}

TEST(AudioCaptureSourceTest, FailedEndpointStartClearsGetServiceInterfaceBeforeRetry) {
    const std::string source = ReadSource("audio_capture.cpp");
    ASSERT_FALSE(source.empty());

    const size_t getService = source.find("GetService(IID_IAudioCaptureClient");
    const size_t successfulReturn = source.find("return true;", getService);
    ASSERT_NE(getService, std::string::npos);
    ASSERT_NE(successfulReturn, std::string::npos);
    const std::string finalActivation = source.substr(getService, successfulReturn - getService);
    EXPECT_NE(finalActivation.find("ReleaseActiveClientOnWorkerThread(false)"), std::string::npos);
    EXPECT_NE(finalActivation.find("pAudioClient->Start()"), std::string::npos);

    const size_t reactivate = source.find("bool AudioCapture::ReactivateClient()");
    const size_t stop = source.find("void AudioCapture::Stop(", reactivate);
    ASSERT_NE(reactivate, std::string::npos);
    ASSERT_NE(stop, std::string::npos);
    const std::string reactivateBody = source.substr(reactivate, stop - reactivate);
    EXPECT_NE(reactivateBody.find("ReleaseActiveClientOnWorkerThread(true)"), std::string::npos);
    EXPECT_NE(reactivateBody.find("Leave pCaptureClient null"), std::string::npos);
}

TEST(AudioCaptureSourceTest, EndpointStopPerformsOneNoWaitNoRecoveryFinalDrain) {
    const std::string source = ReadSource("audio_capture.cpp");
    ASSERT_FALSE(source.empty());

    const size_t loopBegin = source.find("void AudioCapture::CaptureLoop(");
    const size_t getNextPacketBegin = source.find("bool AudioCapture::GetNextPacket(", loopBegin);
    ASSERT_NE(loopBegin, std::string::npos);
    ASSERT_NE(getNextPacketBegin, std::string::npos);
    const std::string loopBody = source.substr(loopBegin, getNextPacketBegin - loopBegin);

    EXPECT_EQ(loopBody.find("while (isCapturing)"), std::string::npos);
    EXPECT_NE(loopBody.find("bool finalDrainPassDone = false"), std::string::npos);
    EXPECT_NE(loopBody.find("const bool stopRequestedBeforeWait"), std::string::npos);
    EXPECT_NE(loopBody.find("Exactly one final pass starts without waiting"), std::string::npos);
    EXPECT_NE(loopBody.find("finalDrainFrameBudget = bufferFrameCount_"), std::string::npos);
    EXPECT_NE(loopBody.find("finalDrainFrames >= finalDrainFrameBudget"), std::string::npos);
    EXPECT_NE(loopBody.find("readNextPacketSize(drainingAfterStop ? \"final drain\" : \"outer\", !drainingAfterStop)"),
              std::string::npos);

    const size_t reactivationLambda = loopBody.find("auto attemptReactivate");
    const size_t reactivationClock = loopBody.find("const uint64_t now = GetTickCount64()", reactivationLambda);
    ASSERT_NE(reactivationLambda, std::string::npos);
    ASSERT_NE(reactivationClock, std::string::npos);
    const std::string reactivationGate = loopBody.substr(reactivationLambda, reactivationClock - reactivationLambda);
    EXPECT_NE(reactivationGate.find("reactivationMutex_"), std::string::npos);
    EXPECT_NE(reactivationGate.find("!isCapturing.load(std::memory_order_acquire)"), std::string::npos);

    const size_t releaseInterfaces = loopBody.rfind("ReleaseAllInterfacesOnWorkerThread();");
    ASSERT_NE(releaseInterfaces, std::string::npos);
    const size_t finalDrain = loopBody.rfind("if (drainingAfterStop) {", releaseInterfaces);
    ASSERT_NE(finalDrain, std::string::npos);
    EXPECT_NE(loopBody.substr(finalDrain, releaseInterfaces - finalDrain).find("break;"), std::string::npos);
    EXPECT_LT(finalDrain, releaseInterfaces);
}

TEST(AudioCaptureSourceTest, ProcessLoopbackFinalDrainIsNonBlockingAndEndpointBounded) {
    const std::string source = ReadSource("app_audio_capture.cpp");
    ASSERT_FALSE(source.empty());

    const size_t loopBegin = source.find("void AppAudioCapture::CaptureLoop(");
    const size_t monitorBegin = source.find("void AppAudioCapture::ProcessMonitorLoop(", loopBegin);
    ASSERT_NE(loopBegin, std::string::npos);
    ASSERT_NE(monitorBegin, std::string::npos);
    const std::string loopBody = source.substr(loopBegin, monitorBegin - loopBegin);

    EXPECT_NE(loopBody.find("finalDrainFrameBudget = bufferFrameCount"), std::string::npos);
    EXPECT_NE(loopBody.find("finalDrainFrames >= finalDrainFrameBudget"), std::string::npos);
    EXPECT_NE(loopBody.find("readNextPacketSize(\"final stop drain\")"), std::string::npos);
    EXPECT_NE(loopBody.find("Final drain reached the endpoint-buffer bound"), std::string::npos);
}

TEST(AudioCaptureSourceTest, AudioOnlyStopDrainsCommittedWasapiTailBeforeLoopShutdown) {
    const std::string source = ReadSource("mediaengine.cpp");
    ASSERT_FALSE(source.empty());

    const size_t stopRecording = source.find("void StopRecording()");
    ASSERT_NE(stopRecording, std::string::npos);
    const size_t audioOnlyStop = source.find("if (audioOnly) {", stopRecording);
    ASSERT_NE(audioOnlyStop, std::string::npos);
    const size_t preservedDrain = source.find("StopCaptureSourcesAndDrainAudioLoop()", audioOnlyStop);
    const size_t stopLoop = source.find("audioRunning = false", audioOnlyStop);
    const size_t joinLoop = source.find("audioThread.join()", audioOnlyStop);
    ASSERT_NE(preservedDrain, std::string::npos);
    ASSERT_NE(stopLoop, std::string::npos);
    ASSERT_NE(joinLoop, std::string::npos);
    EXPECT_LT(preservedDrain, stopLoop);
    EXPECT_LT(stopLoop, joinLoop);
    EXPECT_NE(source.find("Audio-only source tail preserved before loop shutdown"), std::string::npos);
}

TEST(AudioCaptureSourceTest, AudioOnlyUsesSharedTrackMixerInsteadOfOwnerOnlyDirectEncode) {
    const std::string source = ReadSource("mediaengine.cpp");
    ASSERT_FALSE(source.empty());

    const size_t loopBegin = source.find("void AudioLoop()");
    const size_t loopEnd = source.find("MediaEngine: Audio thread stopped", loopBegin);
    ASSERT_NE(loopBegin, std::string::npos);
    ASSERT_NE(loopEnd, std::string::npos);
    const std::string loopBody = source.substr(loopBegin, loopEnd - loopBegin);

    EXPECT_EQ(loopBody.find("if (audioOnly && writeSamples"), std::string::npos);
    EXPECT_NE(loopBody.find("Audio-only recording has no video thread to drive the pull model"), std::string::npos);
    EXPECT_NE(loopBody.find("PullAndEncodeAudio(elapsedUs)"), std::string::npos);
    EXPECT_NE(source.find("ResetAudioPullStateForRecording();"), std::string::npos);
    EXPECT_NE(source.find("cachedTrackToSources[src.track].push_back(i)"), std::string::npos);
}

TEST(AudioCaptureSourceTest, AudioOnlyFinalDrainFlushesBothResamplerLayersBeforeCompletingHandshake) {
    const std::string source = ReadSource("mediaengine.cpp");
    ASSERT_FALSE(source.empty());

    const size_t flushBegin = source.find("void FlushAudioOnlyResamplerTails()");
    const size_t pullBegin = source.find("void PullAndEncodeAudio(", flushBegin);
    ASSERT_NE(flushBegin, std::string::npos);
    ASSERT_NE(pullBegin, std::string::npos);
    const std::string flushBody = source.substr(flushBegin, pullBegin - flushBegin);
    EXPECT_NE(flushBody.find("src.resampler->Flush("), std::string::npos);
    EXPECT_NE(flushBody.find("PumpSourceRingThroughSyncResampler("), std::string::npos);
    EXPECT_NE(flushBody.find("src.syncResampler->Flush("), std::string::npos);
    EXPECT_NE(flushBody.find("AudioResampler::FreeOutputBuffer("), std::string::npos);

    const size_t loopBegin = source.find("void AudioLoop()");
    const size_t flushCall = source.find("FlushAudioOnlyResamplerTails();", loopBegin);
    const size_t finalPull = source.find("PullAndEncodeAudio(finalTargetUs, true);", flushCall);
    const size_t drainComplete = source.find("audioStopDrainComplete.store(true", finalPull);
    ASSERT_NE(flushCall, std::string::npos);
    ASSERT_NE(finalPull, std::string::npos);
    ASSERT_NE(drainComplete, std::string::npos);
    EXPECT_LT(flushCall, finalPull);
    EXPECT_LT(finalPull, drainComplete);
}

TEST(AudioCaptureSourceTest, AudioWorkerExitAlwaysReleasesDrainWaiters) {
    const std::string source = ReadSource("mediaengine.cpp");
    ASSERT_FALSE(source.empty());

    const size_t entry = source.find("void AudioThreadEntry() noexcept");
    const size_t start = source.find("bool StartAudioThread()", entry);
    ASSERT_NE(entry, std::string::npos);
    ASSERT_NE(start, std::string::npos);
    const std::string entryBody = source.substr(entry, start - entry);
    EXPECT_NE(entryBody.find("catch (const std::exception& e)"), std::string::npos);
    EXPECT_NE(entryBody.find("catch (...)"), std::string::npos);
    EXPECT_NE(entryBody.find("audioRunning.store(false"), std::string::npos);
    EXPECT_NE(entryBody.find("audioStopDrainComplete.store(true"), std::string::npos);
    EXPECT_NE(entryBody.find("audioDrainCv.notify_all()"), std::string::npos);

    const size_t afterStart = source.find("void DrainStoppedCaptureQueuesBeforeFinalPull", start);
    ASSERT_NE(afterStart, std::string::npos);
    const std::string startBody = source.substr(start, afterStart - start);
    EXPECT_NE(startBody.find("try {"), std::string::npos);
    EXPECT_NE(startBody.find("std::thread(&MediaEngine::AudioThreadEntry, this)"), std::string::npos);
    EXPECT_NE(startBody.find("StopAudioCaptureSources(true)"), std::string::npos);
    EXPECT_EQ(source.find("std::thread(&MediaEngine::AudioLoop, this)"), std::string::npos);
}

TEST(AudioCaptureSourceTest, AudioWorkerSchedulingGapsAreRateLimitedDiagnosticsNotContinuityRecovery) {
    const std::string source = ReadSource("mediaengine.cpp");
    ASSERT_FALSE(source.empty());

    const size_t loopBegin = source.find("void AudioLoop()");
    const size_t loopEnd = source.find("MediaEngine: Audio thread stopped", loopBegin);
    ASSERT_NE(loopBegin, std::string::npos);
    ASSERT_NE(loopEnd, std::string::npos);
    const std::string loopBody = source.substr(loopBegin, loopEnd - loopBegin);
    EXPECT_NE(loopBody.find("kAudioWorkerSchedulingGapThresholdUs = 25000"), std::string::npos);
    EXPECT_NE(loopBody.find("audioWorkerSchedulingDiagnosticsArmTime"), std::string::npos);
    EXPECT_NE(loopBody.find("[AudioLoop] Scheduling gap:"), std::string::npos);
    EXPECT_NE(loopBody.find("[AudioLoop] Scheduling summary:"), std::string::npos);
    EXPECT_NE(loopBody.find("packet continuity counters remain "), std::string::npos);
    EXPECT_NE(loopBody.find("authoritative for audible-loss classification"), std::string::npos);
    EXPECT_EQ(loopBody.find("Skip("), std::string::npos);
    EXPECT_EQ(loopBody.find("WriteSilence"), std::string::npos);
}

TEST(AudioCaptureSourceTest, MultiTrackRoutesShareOnePhysicalCaptureAndFanOutPackets) {
    const std::string source = ReadSource("mediaengine.cpp");
    ASSERT_FALSE(source.empty());

    const size_t coalesceBegin = source.find("void CoalesceCaptureRoutes()");
    const size_t bufferInit = source.find("void InitAudioSourceBuffers(", coalesceBegin);
    ASSERT_NE(coalesceBegin, std::string::npos);
    ASSERT_NE(bufferInit, std::string::npos);
    const std::string coalesceBody = source.substr(coalesceBegin, bufferInit - coalesceBegin);
    EXPECT_NE(coalesceBody.find("src.captureFanoutOwnerIndex = ownerIdx"), std::string::npos);
    EXPECT_NE(coalesceBody.find("src.capture.reset()"), std::string::npos);
    EXPECT_NE(coalesceBody.find("src.appCapture.reset()"), std::string::npos);
    EXPECT_NE(coalesceBody.find("AppAudioTrackIdentity("), std::string::npos);
    EXPECT_NE(coalesceBody.find("src.config.processName"), std::string::npos);
    EXPECT_NE(coalesceBody.find("src.config.device.empty() ? \"<default>\""), std::string::npos);
    EXPECT_NE(coalesceBody.find("appCaptureFormats[key]"), std::string::npos);
    EXPECT_NE(coalesceBody.find("src.appCapture->SetRequestedFormat(48000, format.first, format.second)"),
              std::string::npos);

    const size_t loopBegin = source.find("void AudioLoop()");
    const size_t loopEnd = source.find("MediaEngine: Audio thread stopped", loopBegin);
    ASSERT_NE(loopBegin, std::string::npos);
    ASSERT_NE(loopEnd, std::string::npos);
    const std::string loopBody = source.substr(loopBegin, loopEnd - loopBegin);
    EXPECT_NE(loopBody.find("captureFanoutQueues[routeIdx].push_back(packet)"), std::string::npos);
    EXPECT_NE(loopBody.find("captureFanoutQueues[srcIdx].pop_front()"), std::string::npos);
    EXPECT_NE(loopBody.find("Fanned packet owner="), std::string::npos);
}

TEST(AudioCaptureSourceTest, StartupQueueDropsPreAnchorPacketsInBoundedBatchesBeforeResampling) {
    const std::string source = ReadSource("mediaengine.cpp");
    ASSERT_FALSE(source.empty());

    const size_t loopBegin = source.find("void AudioLoop()");
    const size_t resample = source.find("src.resampler->Process(", loopBegin);
    const size_t batch = source.find("kMaxPreStartDiscardBatchPackets", loopBegin);
    ASSERT_NE(loopBegin, std::string::npos);
    ASSERT_NE(batch, std::string::npos);
    ASSERT_NE(resample, std::string::npos);
    EXPECT_LT(batch, resample);
    EXPECT_NE(source.find("Batched pre-start discard owner=", batch), std::string::npos);
    EXPECT_NE(source.find("batchedDiscards < kMaxPreStartDiscardBatchPackets", batch), std::string::npos);
}

TEST(AudioCaptureSourceTest, AppBacklogDrainCannotStarveRouteFromDelayedSharedCaptureSibling) {
    const std::string source = ReadSource("mediaengine.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(source.find("GetCaptureGroupMaxBufferedSamples"), std::string::npos);
    EXPECT_NE(source.find("const int64_t compensationBufferedSamples = rbLevel"), std::string::npos);
    EXPECT_NE(source.find("GetCaptureGroupBufferedSampleRange(srcIdx)"), std::string::npos);
    EXPECT_NE(source.find("Applying route-local compensation so a delayed sibling cannot starve this"),
              std::string::npos);
    EXPECT_NE(source.find("ShouldTreatInactiveStartedAppCaptureAsSilence"), std::string::npos);
}

TEST(AudioCaptureSourceTest, CfrSourceGapsAreRouteLocalSilenceWithoutDestructiveResync) {
    const std::string source = ReadSource("mediaengine.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("ce::audio::ComputeSettledCfrAudioPullLatencyMs("), std::string::npos);
    EXPECT_NE(source.find("ce::audio::IsSourceBootstrapTimelineReady("), std::string::npos);

    const size_t expectedSilence = source.find("const bool expectedTimelineSilence =");
    ASSERT_NE(expectedSilence, std::string::npos);
    const size_t sparseSilence = source.find("sparseStartedSourceMaySilence ||", expectedSilence);
    ASSERT_NE(sparseSilence, std::string::npos);
    EXPECT_LT(sparseSilence, expectedSilence + 300);
    EXPECT_NE(source.find("src.pendingUnderrunRecoveryFade = !startupPadding;"), std::string::npos);
    EXPECT_EQ(source.find("ComputeCatastrophicBacklogResyncTrim"), std::string::npos);
    EXPECT_EQ(source.find("App source catastrophic backlog resync"), std::string::npos);

    const size_t cursorGuard = source.find("if (srcIdx < encodedSamplesPerSource.size()) {");
    const size_t exportedCursor = source.find("ce::audio::ResolveSourceTimelineWriteCursor(", cursorGuard);
    ASSERT_NE(cursorGuard, std::string::npos);
    ASSERT_NE(exportedCursor, std::string::npos);
    EXPECT_LT(exportedCursor, cursorGuard + 500);
    EXPECT_EQ(source.substr(cursorGuard, exportedCursor - cursorGuard).find("AudioConfig::AppAudio"),
              std::string::npos);
}
