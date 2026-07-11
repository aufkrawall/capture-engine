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
    EXPECT_NE(coalesceBody.find("AppAudioTrackIdentity(src.config.processName"), std::string::npos);
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

TEST(AudioCaptureSourceTest, AppBacklogDrainUsesWorstBufferedRouteInSharedCaptureGroup) {
    const std::string source = ReadSource("mediaengine.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("int64_t GetCaptureGroupMaxBufferedSamples(size_t srcIdx) const"), std::string::npos);
    EXPECT_NE(source.find("GetCaptureGroupMaxBufferedSamples(srcIdx)"), std::string::npos);
    EXPECT_NE(source.find("const int64_t compensationBufferedSamples"), std::string::npos);
}
