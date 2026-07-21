#include "video_metadata.h"

extern "C" {
#include <libavcodec/bsf.h>
#include <libavcodec/packet.h>
#include <libavutil/error.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace ce::video_metadata {
namespace {

void PopulateNominalMasteringMetadata(AVMasteringDisplayMetadata* mastering, int nominalPeakNits) {
    // Display P3-D65 is a pragmatic mastering-display compatibility convention
    // for desktop HDR capture. The coded pixels remain BT.2020/PQ; mastering
    // display primaries are not the same field as coded color primaries.
    mastering->display_primaries[0][0] = av_make_q(17, 25);
    mastering->display_primaries[0][1] = av_make_q(8, 25);
    mastering->display_primaries[1][0] = av_make_q(53, 200);
    mastering->display_primaries[1][1] = av_make_q(69, 100);
    mastering->display_primaries[2][0] = av_make_q(3, 20);
    mastering->display_primaries[2][1] = av_make_q(3, 50);
    mastering->white_point[0] = av_make_q(3127, 10000);
    mastering->white_point[1] = av_make_q(329, 1000);
    mastering->min_luminance = av_make_q(0, 1);
    mastering->max_luminance = av_make_q(nominalPeakNits, 1);
    mastering->has_primaries = 1;
    mastering->has_luminance = 1;
}

void PopulateNominalContentLightMetadata(AVContentLightMetadata* light, int nominalPeakNits) {
    // This is an explicitly nominal ceiling, not a pixel analysis result. Using
    // the same value for both fields avoids claiming a fabricated measured FALL.
    light->MaxCLL = nominalPeakNits;
    light->MaxFALL = nominalPeakNits;
}

int SetBsfInt(AVBSFContext* bsf, const char* name, int64_t value) {
    return av_opt_set_int(bsf->priv_data, name, value, 0);
}

int AllocateConfiguredHdrBsf(const AVCodecParameters* parameters, AVRational timeBase, AVBSFContext** output) {
    if (!parameters || !output || parameters->chroma_location != AVCHROMA_LOC_TOPLEFT ||
        parameters->color_primaries != AVCOL_PRI_BT2020 || parameters->color_trc != AVCOL_TRC_SMPTE2084 ||
        parameters->color_space != AVCOL_SPC_BT2020_NCL) {
        return AVERROR(EINVAL);
    }

    const char* filterName = nullptr;
    if (parameters->codec_id == AV_CODEC_ID_HEVC) {
        filterName = "hevc_metadata";
    } else if (parameters->codec_id == AV_CODEC_ID_AV1) {
        filterName = "av1_metadata";
    } else {
        return AVERROR(ENOSYS);
    }

    const AVBitStreamFilter* filter = av_bsf_get_by_name(filterName);
    if (!filter) {
        return AVERROR(ENOSYS);
    }

    AVBSFContext* bsf = nullptr;
    int result = av_bsf_alloc(filter, &bsf);
    if (result < 0) {
        return result;
    }

    result = avcodec_parameters_copy(bsf->par_in, parameters);
    if (result < 0) {
        av_bsf_free(&bsf);
        return result;
    }
    bsf->time_base_in = timeBase;

    const int fullRange = parameters->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    if (parameters->codec_id == AV_CODEC_ID_HEVC) {
        if ((result = SetBsfInt(bsf, "video_full_range_flag", fullRange)) < 0 ||
            (result = SetBsfInt(bsf, "colour_primaries", parameters->color_primaries)) < 0 ||
            (result = SetBsfInt(bsf, "transfer_characteristics", parameters->color_trc)) < 0 ||
            (result = SetBsfInt(bsf, "matrix_coefficients", parameters->color_space)) < 0 ||
            (result = SetBsfInt(bsf, "chroma_sample_loc_type", 2)) < 0) {
            av_bsf_free(&bsf);
            return result;
        }
    } else {
        if ((result = SetBsfInt(bsf, "color_range", fullRange)) < 0 ||
            (result = SetBsfInt(bsf, "color_primaries", parameters->color_primaries)) < 0 ||
            (result = SetBsfInt(bsf, "transfer_characteristics", parameters->color_trc)) < 0 ||
            (result = SetBsfInt(bsf, "matrix_coefficients", parameters->color_space)) < 0 ||
            (result = av_opt_set(bsf->priv_data, "chroma_sample_position", "colocated", 0)) < 0) {
            av_bsf_free(&bsf);
            return result;
        }
    }

    result = av_bsf_init(bsf);
    if (result < 0) {
        av_bsf_free(&bsf);
        return result;
    }
    *output = bsf;
    return 0;
}

}  // namespace

int ClampHdrNominalPeakNits(int nominalPeakNits) {
    return std::clamp(nominalPeakNits, kMinHdrNominalPeakNits, kMaxHdrNominalPeakNits);
}

int AddNominalHdrMetadataToCodecContext(AVCodecContext* codec, int nominalPeakNits) {
    if (!codec) {
        return AVERROR(EINVAL);
    }

    const int peak = ClampHdrNominalPeakNits(nominalPeakNits);
    AVFrameSideData* masteringSideData = av_frame_side_data_new(
        &codec->decoded_side_data, &codec->nb_decoded_side_data, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
        sizeof(AVMasteringDisplayMetadata), AV_FRAME_SIDE_DATA_FLAG_REPLACE);
    if (!masteringSideData) {
        return AVERROR(ENOMEM);
    }
    PopulateNominalMasteringMetadata(reinterpret_cast<AVMasteringDisplayMetadata*>(masteringSideData->data), peak);

    AVFrameSideData* lightSideData =
        av_frame_side_data_new(&codec->decoded_side_data, &codec->nb_decoded_side_data,
                               AV_FRAME_DATA_CONTENT_LIGHT_LEVEL, sizeof(AVContentLightMetadata),
                               AV_FRAME_SIDE_DATA_FLAG_REPLACE);
    if (!lightSideData) {
        return AVERROR(ENOMEM);
    }
    PopulateNominalContentLightMetadata(reinterpret_cast<AVContentLightMetadata*>(lightSideData->data), peak);
    return 0;
}

int AddNominalHdrMetadataToFrame(AVFrame* frame, int nominalPeakNits) {
    if (!frame) {
        return AVERROR(EINVAL);
    }

    const int peak = ClampHdrNominalPeakNits(nominalPeakNits);
    AVFrameSideData* masteringSideData = av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    AVMasteringDisplayMetadata* mastering =
        masteringSideData ? reinterpret_cast<AVMasteringDisplayMetadata*>(masteringSideData->data)
                          : av_mastering_display_metadata_create_side_data(frame);
    if (!mastering) {
        return AVERROR(ENOMEM);
    }
    PopulateNominalMasteringMetadata(mastering, peak);

    AVFrameSideData* lightSideData = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    AVContentLightMetadata* light =
        lightSideData ? reinterpret_cast<AVContentLightMetadata*>(lightSideData->data)
                      : av_content_light_metadata_create_side_data(frame);
    if (!light) {
        return AVERROR(ENOMEM);
    }
    PopulateNominalContentLightMetadata(light, peak);
    return 0;
}

int AddNominalHdrMetadataToCodecParameters(AVCodecParameters* parameters, int nominalPeakNits) {
    if (!parameters) {
        return AVERROR(EINVAL);
    }

    av_packet_side_data_remove(parameters->coded_side_data, &parameters->nb_coded_side_data,
                               AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
    av_packet_side_data_remove(parameters->coded_side_data, &parameters->nb_coded_side_data,
                               AV_PKT_DATA_CONTENT_LIGHT_LEVEL);

    const int peak = ClampHdrNominalPeakNits(nominalPeakNits);
    size_t masteringSize = 0;
    AVMasteringDisplayMetadata* mastering = av_mastering_display_metadata_alloc_size(&masteringSize);
    if (!mastering) {
        return AVERROR(ENOMEM);
    }
    PopulateNominalMasteringMetadata(mastering, peak);
    if (!av_packet_side_data_add(&parameters->coded_side_data, &parameters->nb_coded_side_data,
                                 AV_PKT_DATA_MASTERING_DISPLAY_METADATA, mastering, masteringSize, 0)) {
        av_free(mastering);
        return AVERROR(ENOMEM);
    }

    size_t lightSize = 0;
    AVContentLightMetadata* light = av_content_light_metadata_alloc(&lightSize);
    if (!light) {
        return AVERROR(ENOMEM);
    }
    PopulateNominalContentLightMetadata(light, peak);
    if (!av_packet_side_data_add(&parameters->coded_side_data, &parameters->nb_coded_side_data,
                                 AV_PKT_DATA_CONTENT_LIGHT_LEVEL, light, lightSize, 0)) {
        av_free(light);
        av_packet_side_data_remove(parameters->coded_side_data, &parameters->nb_coded_side_data,
                                   AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
        return AVERROR(ENOMEM);
    }
    return 0;
}

int NormalizeHdrCodecExtradata(AVCodecParameters* parameters, AVRational timeBase) {
    if (!parameters) {
        return AVERROR(EINVAL);
    }
    if (!parameters->extradata || parameters->extradata_size <= 0) {
        // QSV AV1 supplies its sequence header as NEW_EXTRADATA on the first
        // packet. NormalizeHdrPacketMetadata handles that path.
        return 0;
    }

    AVBSFContext* bsf = nullptr;
    int result = AllocateConfiguredHdrBsf(parameters, timeBase, &bsf);
    if (result < 0) {
        return result;
    }
    auto finish = [&](int value) {
        av_bsf_free(&bsf);
        return value;
    };

    if (!bsf->par_out->extradata || bsf->par_out->extradata_size <= 0) {
        return finish(AVERROR_INVALIDDATA);
    }

    result = avcodec_parameters_copy(parameters, bsf->par_out);
    if (result >= 0) {
        parameters->chroma_location = AVCHROMA_LOC_TOPLEFT;
    }
    return finish(result);
}

int NormalizeHdrPacketMetadata(AVPacket* packet, const AVCodecParameters* parameters, AVRational timeBase) {
    if (!packet || !parameters) {
        return AVERROR(EINVAL);
    }
    if (!av_packet_get_side_data(packet, AV_PKT_DATA_NEW_EXTRADATA, nullptr)) {
        return 0;
    }

    AVBSFContext* bsf = nullptr;
    int result = AllocateConfiguredHdrBsf(parameters, timeBase, &bsf);
    if (result < 0) {
        return result;
    }

    AVPacket* filtered = av_packet_alloc();
    if (!filtered) {
        av_bsf_free(&bsf);
        return AVERROR(ENOMEM);
    }

    result = av_bsf_send_packet(bsf, packet);
    if (result >= 0) {
        result = av_bsf_receive_packet(bsf, filtered);
    }
    if (result >= 0) {
        av_packet_move_ref(packet, filtered);
        result = 1;
    }
    av_packet_free(&filtered);
    av_bsf_free(&bsf);
    return result;
}

}  // namespace ce::video_metadata
