#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/packet.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mem.h>
}

#include <cstring>
#include <vector>

#include "../mediaengine/video_metadata.h"

namespace {

void ExpectNominalMasteringMetadata(const AVMasteringDisplayMetadata* mastering, int peak) {
    ASSERT_NE(mastering, nullptr);
    EXPECT_EQ(mastering->display_primaries[0][0].num, 17);
    EXPECT_EQ(mastering->display_primaries[0][0].den, 25);
    EXPECT_EQ(mastering->display_primaries[0][1].num, 8);
    EXPECT_EQ(mastering->display_primaries[0][1].den, 25);
    EXPECT_EQ(mastering->display_primaries[1][0].num, 53);
    EXPECT_EQ(mastering->display_primaries[1][0].den, 200);
    EXPECT_EQ(mastering->display_primaries[1][1].num, 69);
    EXPECT_EQ(mastering->display_primaries[1][1].den, 100);
    EXPECT_EQ(mastering->display_primaries[2][0].num, 3);
    EXPECT_EQ(mastering->display_primaries[2][0].den, 20);
    EXPECT_EQ(mastering->display_primaries[2][1].num, 3);
    EXPECT_EQ(mastering->display_primaries[2][1].den, 50);
    EXPECT_EQ(mastering->white_point[0].num, 3127);
    EXPECT_EQ(mastering->white_point[0].den, 10000);
    EXPECT_EQ(mastering->white_point[1].num, 329);
    EXPECT_EQ(mastering->white_point[1].den, 1000);
    EXPECT_EQ(mastering->min_luminance.num, 0);
    EXPECT_EQ(mastering->max_luminance.num, peak);
    EXPECT_EQ(mastering->max_luminance.den, 1);
    EXPECT_EQ(mastering->has_primaries, 1);
    EXPECT_EQ(mastering->has_luminance, 1);
}

void ExpectNominalContentLightMetadata(const AVContentLightMetadata* light, int peak) {
    ASSERT_NE(light, nullptr);
    EXPECT_EQ(light->MaxCLL, peak);
    EXPECT_EQ(light->MaxFALL, peak);
}

AVCodecParameters* MakeCodecParameters(AVCodecID codecId, const uint8_t* extradata, size_t extradataSize) {
    AVCodecParameters* parameters = avcodec_parameters_alloc();
    if (!parameters) {
        return nullptr;
    }
    parameters->extradata =
        static_cast<uint8_t*>(av_mallocz(extradataSize + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!parameters->extradata) {
        avcodec_parameters_free(&parameters);
        return nullptr;
    }
    std::memcpy(parameters->extradata, extradata, extradataSize);
    parameters->extradata_size = static_cast<int>(extradataSize);
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = codecId;
    return parameters;
}

constexpr uint8_t kHevcExtradata[] = {
    0x01, 0x02, 0x20, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e, 0xf0, 0x00, 0xfc,
    0xfd, 0xfa, 0xfa, 0x00, 0x00, 0x0f, 0x03, 0x20, 0x00, 0x01, 0x00, 0x18, 0x40, 0x01, 0x0c, 0x01,
    0xff, 0xff, 0x02, 0x20, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00,
    0x1e, 0x95, 0x98, 0x09, 0x21, 0x00, 0x01, 0x00, 0x28, 0x42, 0x01, 0x01, 0x02, 0x20, 0x00, 0x00,
    0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x1e, 0xa0, 0x88, 0x44, 0xd9, 0x65,
    0x66, 0xab, 0xca, 0xf0, 0x16, 0x80, 0x80, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00, 0x03, 0x00,
    0x84, 0x22, 0x00, 0x01, 0x00, 0x06, 0x44, 0x01, 0xc1, 0x73, 0xd0, 0x89,
};

constexpr uint8_t kAv1Extradata[] = {
    0x81, 0x00, 0x4c, 0x00, 0x0a, 0x0a, 0x00, 0x00, 0x00, 0x01, 0x9f, 0xf9, 0xb5, 0xf2, 0x80, 0x80,
};

}  // namespace

TEST(VideoMetadataTest, NominalPeakIsBoundedToDocumentedRange) {
    EXPECT_EQ(ce::video_metadata::ClampHdrNominalPeakNits(1), 100);
    EXPECT_EQ(ce::video_metadata::ClampHdrNominalPeakNits(1600), 1600);
    EXPECT_EQ(ce::video_metadata::ClampHdrNominalPeakNits(20000), 10000);
}

TEST(VideoMetadataTest, EncoderAndFrameUseTheSameNominalP3Metadata) {
    AVCodecContext* codec = avcodec_alloc_context3(nullptr);
    ASSERT_NE(codec, nullptr);
    ASSERT_EQ(ce::video_metadata::AddNominalHdrMetadataToCodecContext(codec, 1600), 0);

    const AVFrameSideData* codecMastering = av_frame_side_data_get(
        codec->decoded_side_data, codec->nb_decoded_side_data, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    const AVFrameSideData* codecLight = av_frame_side_data_get(
        codec->decoded_side_data, codec->nb_decoded_side_data, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    ASSERT_NE(codecMastering, nullptr);
    ASSERT_NE(codecLight, nullptr);
    ExpectNominalMasteringMetadata(
        reinterpret_cast<const AVMasteringDisplayMetadata*>(codecMastering->data), 1600);
    ExpectNominalContentLightMetadata(reinterpret_cast<const AVContentLightMetadata*>(codecLight->data), 1600);

    AVFrame* frame = av_frame_alloc();
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(ce::video_metadata::AddNominalHdrMetadataToFrame(frame, 1600), 0);
    const AVFrameSideData* frameMastering =
        av_frame_get_side_data(frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    const AVFrameSideData* frameLight = av_frame_get_side_data(frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    ASSERT_NE(frameMastering, nullptr);
    ASSERT_NE(frameLight, nullptr);
    ExpectNominalMasteringMetadata(
        reinterpret_cast<const AVMasteringDisplayMetadata*>(frameMastering->data), 1600);
    ExpectNominalContentLightMetadata(reinterpret_cast<const AVContentLightMetadata*>(frameLight->data), 1600);

    av_frame_free(&frame);
    avcodec_free_context(&codec);
}

TEST(VideoMetadataTest, ContainerParametersCarryMasteringAndContentLightSideData) {
    AVCodecParameters* parameters = avcodec_parameters_alloc();
    ASSERT_NE(parameters, nullptr);
    ASSERT_EQ(ce::video_metadata::AddNominalHdrMetadataToCodecParameters(parameters, 1000), 0);

    const AVPacketSideData* masteringSideData = av_packet_side_data_get(
        parameters->coded_side_data, parameters->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
    const AVPacketSideData* lightSideData = av_packet_side_data_get(
        parameters->coded_side_data, parameters->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
    ASSERT_NE(masteringSideData, nullptr);
    ASSERT_NE(lightSideData, nullptr);
    ExpectNominalMasteringMetadata(
        reinterpret_cast<const AVMasteringDisplayMetadata*>(masteringSideData->data), 1000);
    ExpectNominalContentLightMetadata(reinterpret_cast<const AVContentLightMetadata*>(lightSideData->data), 1000);

    ASSERT_EQ(ce::video_metadata::AddNominalHdrMetadataToCodecParameters(parameters, 2000), 0);
    EXPECT_EQ(parameters->nb_coded_side_data, 2);
    masteringSideData = av_packet_side_data_get(parameters->coded_side_data, parameters->nb_coded_side_data,
                                                 AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
    lightSideData = av_packet_side_data_get(parameters->coded_side_data, parameters->nb_coded_side_data,
                                             AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
    ExpectNominalMasteringMetadata(
        reinterpret_cast<const AVMasteringDisplayMetadata*>(masteringSideData->data), 2000);
    ExpectNominalContentLightMetadata(reinterpret_cast<const AVContentLightMetadata*>(lightSideData->data), 2000);

    avcodec_parameters_free(&parameters);
}

TEST(VideoMetadataTest, HdrGlobalHeadersAreNormalizedWithoutAStreamingFilter) {
    struct EncoderCase {
        const char* name;
        AVCodecID codecId;
        const uint8_t* extradata;
        size_t extradataSize;
    };
    const EncoderCase cases[] = {{"HEVC", AV_CODEC_ID_HEVC, kHevcExtradata, sizeof(kHevcExtradata)},
                                 {"AV1", AV_CODEC_ID_AV1, kAv1Extradata, sizeof(kAv1Extradata)}};

    for (const EncoderCase& encoderCase : cases) {
        AVCodecParameters* parameters = MakeCodecParameters(
            encoderCase.codecId, encoderCase.extradata, encoderCase.extradataSize);
        ASSERT_NE(parameters, nullptr) << encoderCase.name;
        const std::vector<uint8_t> originalExtradata(parameters->extradata,
                                                      parameters->extradata + parameters->extradata_size);
        parameters->color_range = AVCOL_RANGE_MPEG;
        parameters->color_primaries = AVCOL_PRI_BT2020;
        parameters->color_trc = AVCOL_TRC_SMPTE2084;
        parameters->color_space = AVCOL_SPC_BT2020_NCL;
        parameters->chroma_location = AVCHROMA_LOC_TOPLEFT;

        ASSERT_EQ(ce::video_metadata::NormalizeHdrCodecExtradata(parameters, AVRational{1, 60}), 0)
            << encoderCase.name;
        EXPECT_EQ(parameters->chroma_location, AVCHROMA_LOC_TOPLEFT) << encoderCase.name;
        EXPECT_TRUE(parameters->extradata_size != static_cast<int>(originalExtradata.size()) ||
                    std::memcmp(parameters->extradata, originalExtradata.data(), originalExtradata.size()) != 0)
            << encoderCase.name;
        avcodec_parameters_free(&parameters);
    }
}

TEST(VideoMetadataTest, PacketCarriedAv1HeaderIsNormalizedOnlyWhenNewExtradataIsPresent) {
    AVCodecParameters* parameters = avcodec_parameters_alloc();
    ASSERT_NE(parameters, nullptr);
    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = AV_CODEC_ID_AV1;
    parameters->color_range = AVCOL_RANGE_MPEG;
    parameters->color_primaries = AVCOL_PRI_BT2020;
    parameters->color_trc = AVCOL_TRC_SMPTE2084;
    parameters->color_space = AVCOL_SPC_BT2020_NCL;
    parameters->chroma_location = AVCHROMA_LOC_TOPLEFT;

    // QSV AV1 has no open-time extradata. That is a valid deferred-header
    // contract, not a reason to reject the encoder before its first packet.
    EXPECT_EQ(ce::video_metadata::NormalizeHdrCodecExtradata(parameters, AVRational{1, 60}), 0);

    AVPacket* ordinaryPacket = av_packet_alloc();
    ASSERT_NE(ordinaryPacket, nullptr);
    ASSERT_EQ(av_new_packet(ordinaryPacket, 1), 0);
    ordinaryPacket->data[0] = 0x12;
    EXPECT_EQ(ce::video_metadata::NormalizeHdrPacketMetadata(ordinaryPacket, parameters, AVRational{1, 60}), 0);
    av_packet_free(&ordinaryPacket);

    constexpr size_t kAv1ConfigRecordSize = 4;
    const uint8_t* sequenceHeader = kAv1Extradata + kAv1ConfigRecordSize;
    const size_t sequenceHeaderSize = sizeof(kAv1Extradata) - kAv1ConfigRecordSize;
    const std::vector<uint8_t> originalHeader(sequenceHeader, sequenceHeader + sequenceHeaderSize);

    AVPacket* headerPacket = av_packet_alloc();
    ASSERT_NE(headerPacket, nullptr);
    ASSERT_EQ(av_new_packet(headerPacket, static_cast<int>(sequenceHeaderSize)), 0);
    std::memcpy(headerPacket->data, sequenceHeader, sequenceHeaderSize);
    uint8_t* newExtradata = static_cast<uint8_t*>(av_memdup(sequenceHeader, sequenceHeaderSize));
    ASSERT_NE(newExtradata, nullptr);
    ASSERT_EQ(av_packet_add_side_data(headerPacket, AV_PKT_DATA_NEW_EXTRADATA, newExtradata, sequenceHeaderSize), 0);

    ASSERT_EQ(ce::video_metadata::NormalizeHdrPacketMetadata(headerPacket, parameters, AVRational{1, 60}), 1);
    size_t normalizedSideDataSize = 0;
    const uint8_t* normalizedSideData =
        av_packet_get_side_data(headerPacket, AV_PKT_DATA_NEW_EXTRADATA, &normalizedSideDataSize);
    ASSERT_NE(normalizedSideData, nullptr);
    EXPECT_TRUE(headerPacket->size != static_cast<int>(originalHeader.size()) ||
                std::memcmp(headerPacket->data, originalHeader.data(), originalHeader.size()) != 0);
    EXPECT_TRUE(normalizedSideDataSize != originalHeader.size() ||
                std::memcmp(normalizedSideData, originalHeader.data(), originalHeader.size()) != 0);

    av_packet_free(&headerPacket);
    avcodec_parameters_free(&parameters);
}
