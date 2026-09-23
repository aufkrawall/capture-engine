#include "mediaengine_internal.h"


void MediaEngine::ReleaseEncoderTextures() {


        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (videoEnc) {
            videoEnc->ReleasePreservedEncoderTextures();
        }

}


void MediaEngine::UpdateVideoEncoderSharedMem(void* sharedMem,  void* shmemBuffer) {


        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        sharedMemLayout = (SharedMemoryLayout*)sharedMem;
        if (videoEnc) {
            videoEnc->SetSharedMem((SharedMemoryLayout*)sharedMem, (ShmemBuffer*)shmemBuffer);
        }

}


bool MediaEngine::CreateSharedCaptureTextures(uint32_t width,  uint32_t height,  uint32_t format,  SharedMemoryLayout* sharedMem) {


        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (!videoEnc) {
            DLL_Log("MediaEngine: CreateSharedCaptureTextures - no encoder");
            return false;
        }
        if (!sharedMem) {
            DLL_Log("MediaEngine: CreateSharedCaptureTextures - sharedMem is null");
            return false;
        }

        // IMPORTANT: Set encoder dimensions and LUID from the parameters before
        // EnsureDevice Otherwise EnsureDevice fails because width/height are still
        // 0 or uses wrong GPU
        videoEnc->SetDimensions(width, height);
        videoEnc->SetAdapterLUID(sharedMem->GetLuidLowPart(), sharedMem->GetLuidHighPart());

        if (!videoEnc->EnsureDevice()) {
            DLL_Log(
                "MediaEngine: CreateSharedCaptureTextures - device init failed "
                "for LUID %08x:%08x",
                sharedMem->GetLuidLowPart(), sharedMem->GetLuidHighPart());
            return false;
        }
        return videoEnc->CreateSharedCaptureTextures(width, height, format, sharedMem);

}


void MediaEngine::WritePacket(AVPacket* pkt) {


        std::lock_guard<std::recursive_mutex> lock(muxMutex);
        if (audioOnly && audioOnlyFmtCtx) {
            if (pkt->stream_index >= 0 && (unsigned int)pkt->stream_index < audioOnlyFmtCtx->nb_streams) {
                AVStream* st = audioOnlyFmtCtx->streams[pkt->stream_index];
                AVRational codec_tb = {1, st->codecpar->sample_rate};
                if (codec_tb.den > 0)
                    av_packet_rescale_ts(pkt, codec_tb, st->time_base);
            }
            const int writeResult = av_interleaved_write_frame(audioOnlyFmtCtx, pkt);
            if (writeResult >= 0) {
                ++audioOnlyWrittenPackets;
            } else {
                static std::atomic<uint32_t> s_audioOnlyWriteFailures{0};
                const uint32_t failures = s_audioOnlyWriteFailures.fetch_add(1, std::memory_order_relaxed) + 1;
                if (failures <= 4 || (failures & (failures - 1)) == 0) {
                    DLL_Log("MediaEngine: ERROR audio-only packet write failed: %d (failure #%u, written=%llu)",
                            writeResult, failures, static_cast<unsigned long long>(audioOnlyWrittenPackets));
                }
            }
        } else if (videoEnc) {
            videoEnc->WriteFrame(pkt);
        }

}
