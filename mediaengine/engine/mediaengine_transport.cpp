#include "mediaengine_internal.h"

#include "common/capture/media_ended_recording.h"

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
                ++audioOnlyWriteErrorCount;
                if (audioOnlyWriteErrorCount <= 4 || (audioOnlyWriteErrorCount & (audioOnlyWriteErrorCount - 1)) == 0) {
                    DLL_Log("MediaEngine: ERROR audio-only packet write failed: %d (failure #%u, written=%llu)",
                            writeResult, static_cast<unsigned>(audioOnlyWriteErrorCount),
                            static_cast<unsigned long long>(audioOnlyWrittenPackets));
                }
                // Sticky AVIO error: every later packet would be dropped as
                // well, so stop at the first loss and keep the committed ones.
                if (auto publication = sharedMemLayout.Publish(); audioOnlyWriteErrorCount == 1 && publication) {
                    ce::media_end::RequestStop(publication->runtimeState, ce::media_end::Reason::OutputFailed,
                                               GetTickCount64());
                }
            }
        } else if (videoEnc) {
            videoEnc->WriteFrame(pkt);
        }

}
