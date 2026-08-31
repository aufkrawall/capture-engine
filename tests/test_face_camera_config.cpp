#include "test_config_shared.h"

#include "../common/face_camera_config.h"

TEST(FaceCameraPolicyTest, ParsesResolutionShapePositionAndCropTokens) {
    uint32_t width = 99;
    uint32_t height = 77;
    EXPECT_TRUE(ce::face_camera::TryParseResolution("1920x1080", &width, &height));
    EXPECT_EQ(width, 1920u);
    EXPECT_EQ(height, 1080u);
    EXPECT_TRUE(ce::face_camera::TryParseResolution("auto", &width, &height));
    EXPECT_EQ(width, 0u);
    EXPECT_EQ(height, 0u);
    EXPECT_FALSE(ce::face_camera::TryParseResolution("159x120", &width, &height));
    EXPECT_FALSE(ce::face_camera::TryParseResolution("1920x1080junk", &width, &height));

    ce::face_camera::Shape shape = ce::face_camera::Shape::kRectangle;
    ce::face_camera::Position position = ce::face_camera::Position::kTopLeft;
    ce::face_camera::Crop crop = ce::face_camera::Crop::kStretch;
    EXPECT_TRUE(ce::face_camera::TryParseShape("circle", &shape));
    EXPECT_EQ(shape, ce::face_camera::Shape::kCircle);
    EXPECT_TRUE(ce::face_camera::TryParsePosition("bottom_center", &position));
    EXPECT_EQ(position, ce::face_camera::Position::kBottomCenter);
    EXPECT_TRUE(ce::face_camera::TryParseCrop("fill", &crop));
    EXPECT_EQ(crop, ce::face_camera::Crop::kFill);
    EXPECT_FALSE(ce::face_camera::TryParseShape("ellipse", &shape));
}

TEST(FaceCameraPolicyTest, EqualityTracksEveryRuntimeSetting) {
    ce::face_camera::Config first;
    ce::face_camera::Config second;
    EXPECT_EQ(first, second);
    second.staleTimeoutMs = 0;
    EXPECT_NE(first, second);
    second = first;
    second.position = ce::face_camera::Position::kTopRight;
    EXPECT_NE(first, second);
    second = first;
    second.borderColorRgb = 0x123456u;
    EXPECT_NE(first, second);
}

TEST(FaceCameraPolicyTest, StaleTimeoutIsDisabledOrExpiresOnlyAfterTheDeadline) {
    EXPECT_FALSE(ce::face_camera::IsFrameStale(1000, 9000, 0));
    EXPECT_FALSE(ce::face_camera::IsFrameStale(0, 9000, 2000));
    EXPECT_FALSE(ce::face_camera::IsFrameStale(1000, 999, 2000));
    EXPECT_FALSE(ce::face_camera::IsFrameStale(1000, 3000, 2000));
    EXPECT_TRUE(ce::face_camera::IsFrameStale(1000, 3001, 2000));
}

TEST(FaceCameraPolicyTest, InitialRepeatSourceGraceIsBoundedEvenWhenStaleFramesAreRetained) {
    EXPECT_FALSE(ce::face_camera::IsWithinInitialRepeatSourceGrace(0, 1000, 0));
    EXPECT_TRUE(ce::face_camera::IsWithinInitialRepeatSourceGrace(1000, 6000, 0));
    EXPECT_FALSE(ce::face_camera::IsWithinInitialRepeatSourceGrace(1000, 6001, 0));
    EXPECT_TRUE(ce::face_camera::IsWithinInitialRepeatSourceGrace(1000, 9000, 8000));
    EXPECT_FALSE(ce::face_camera::IsWithinInitialRepeatSourceGrace(1000, 9001, 8000));
}

TEST(FaceCameraPolicyTest, MapsOutputLayoutBackToSourceWithoutMovingAnchors) {
    ce::face_camera::Config config;
    config.widthPercent = 25.0f;
    config.marginPercent = 2.0f;
    config.position = ce::face_camera::Position::kBottomRight;

    // Layout is specified in the 1920x1080 output, then mapped into a source
    // whose aspect ratio differs. The resulting output-space geometry remains
    // a 480x270 camera with a 21.6-pixel margin.
    const auto layout = ce::face_camera::ResolveLayout(config, 2560, 1440, 1920, 1080, 1280, 720);
    ASSERT_TRUE(layout.valid);
    EXPECT_EQ(layout.left, 1891);
    EXPECT_EQ(layout.top, 1051);
    EXPECT_EQ(layout.right, 2532);
    EXPECT_EQ(layout.bottom, 1412);
    EXPECT_NEAR(layout.displayedWidthPixels, 480.0f, 0.01f);
    EXPECT_NEAR(layout.displayedHeightPixels, 270.0f, 0.01f);
    EXPECT_FLOAT_EQ(layout.sourceU0, 0.0f);
    EXPECT_FLOAT_EQ(layout.sourceU1, 1.0f);
}

TEST(FaceCameraPolicyTest, CircleUsesSquareDestinationAndCenteredCameraCrop) {
    ce::face_camera::Config config;
    config.shape = ce::face_camera::Shape::kCircle;
    config.position = ce::face_camera::Position::kCenter;
    config.widthPercent = 20.0f;
    config.marginPercent = 0.0f;
    config.crop = ce::face_camera::Crop::kFill;

    const auto layout = ce::face_camera::ResolveLayout(config, 1920, 1080, 1920, 1080, 1920, 1080);
    ASSERT_TRUE(layout.valid);
    EXPECT_EQ(layout.right - layout.left, layout.bottom - layout.top);
    EXPECT_EQ(layout.left, 768);
    EXPECT_EQ(layout.top, 348);
    EXPECT_NEAR(layout.sourceU0, 0.21875f, 0.0001f);
    EXPECT_NEAR(layout.sourceU1, 0.78125f, 0.0001f);
    EXPECT_FLOAT_EQ(layout.sourceV0, 0.0f);
    EXPECT_FLOAT_EQ(layout.sourceV1, 1.0f);
}

TEST(FaceCameraPolicyTest, CustomPositionClampsInsideConfiguredMargin) {
    ce::face_camera::Config config;
    config.position = ce::face_camera::Position::kCustom;
    config.widthPercent = 40.0f;
    config.marginPercent = 5.0f;
    config.customXPercent = 500.0f;
    config.customYPercent = -50.0f;

    const auto layout = ce::face_camera::ResolveLayout(config, 1000, 600, 1000, 600, 1600, 900);
    ASSERT_TRUE(layout.valid);
    EXPECT_EQ(layout.top, 30);
    EXPECT_LE(layout.right, 970);
    EXPECT_GE(layout.left, 30);
}

TEST_F(ConfigTest, LoadsFaceCameraConfiguration) {
    WriteConfig(R"ini(
[FaceCamera]
enabled=true
device="USB Camera 4K"
resolution=1920X1080
fps=60
position=custom
x_percent=12.5
y_percent=87.5
width_percent=31.25
margin_percent=3.5
shape=circle
corner_radius_percent=22
crop=stretch
mirror=false
opacity_percent=75
border_width_percent=2.5
border_color=#12AbEF
stale_timeout_ms=3500
)ini");

    AppConfig config;
    LoadConfig(tempConfigFile, config);
    const auto& camera = config.video.faceCamera;
    EXPECT_TRUE(camera.enabled);
    EXPECT_EQ(camera.device, "USB Camera 4K");
    EXPECT_EQ(camera.requestedWidth, 1920u);
    EXPECT_EQ(camera.requestedHeight, 1080u);
    EXPECT_EQ(camera.requestedFps, 60u);
    EXPECT_EQ(camera.position, ce::face_camera::Position::kCustom);
    EXPECT_EQ(camera.shape, ce::face_camera::Shape::kCircle);
    EXPECT_EQ(camera.crop, ce::face_camera::Crop::kStretch);
    EXPECT_FLOAT_EQ(camera.customXPercent, 12.5f);
    EXPECT_FLOAT_EQ(camera.customYPercent, 87.5f);
    EXPECT_FLOAT_EQ(camera.widthPercent, 31.25f);
    EXPECT_FLOAT_EQ(camera.marginPercent, 3.5f);
    EXPECT_FLOAT_EQ(camera.cornerRadiusPercent, 22.0f);
    EXPECT_FLOAT_EQ(camera.opacity, 0.75f);
    EXPECT_FLOAT_EQ(camera.borderWidthPercent, 2.5f);
    EXPECT_EQ(camera.borderColorRgb, 0x12ABEFu);
    EXPECT_FALSE(camera.mirror);
    EXPECT_EQ(camera.staleTimeoutMs, 3500u);
}

TEST_F(ConfigTest, FaceCameraSupportsPerProcessOverrides) {
    WriteConfig(R"ini(
[FaceCamera]
enabled=false
position=bottom_right
width_percent=24

[Profile.Game]
Process=example-game.exe
FaceCamera.enabled=true
FaceCamera.position=top_left
FaceCamera.width_percent=18
)ini");

    AppConfig config;
    LoadConfig(tempConfigFile, config, "example-game.exe");
    EXPECT_TRUE(config.video.faceCamera.enabled);
    EXPECT_EQ(config.video.faceCamera.position, ce::face_camera::Position::kTopLeft);
    EXPECT_FLOAT_EQ(config.video.faceCamera.widthPercent, 18.0f);
}

TEST_F(ConfigTest, InvalidFaceCameraValuesFallBackToSafeDefaults) {
    WriteConfig(R"ini(
[FaceCamera]
enabled=true
resolution=99999x1
fps=0
position=somewhere
shape=ellipse
crop=distort
width_percent=1
margin_percent=99
x_percent=-1
y_percent=101
opacity_percent=120
border_width_percent=50
border_color=not-a-color
stale_timeout_ms=50000
)ini");

    AppConfig config;
    LoadConfig(tempConfigFile, config);
    const auto& camera = config.video.faceCamera;
    EXPECT_TRUE(camera.enabled);
    EXPECT_EQ(camera.requestedWidth, 1280u);
    EXPECT_EQ(camera.requestedHeight, 720u);
    EXPECT_EQ(camera.requestedFps, 30u);
    EXPECT_EQ(camera.position, ce::face_camera::Position::kBottomRight);
    EXPECT_EQ(camera.shape, ce::face_camera::Shape::kRoundedRectangle);
    EXPECT_EQ(camera.crop, ce::face_camera::Crop::kFill);
    EXPECT_FLOAT_EQ(camera.widthPercent, 24.0f);
    EXPECT_FLOAT_EQ(camera.marginPercent, 2.0f);
    EXPECT_FLOAT_EQ(camera.customXPercent, 100.0f);
    EXPECT_FLOAT_EQ(camera.customYPercent, 100.0f);
    EXPECT_FLOAT_EQ(camera.opacity, 1.0f);
    EXPECT_FLOAT_EQ(camera.borderWidthPercent, 0.75f);
    EXPECT_EQ(camera.borderColorRgb, 0xFFFFFFu);
    EXPECT_EQ(camera.staleTimeoutMs, 2000u);
}

TEST_F(ConfigTest, DefaultTemplateDocumentsFaceCameraGpuPath) {
    const std::string contents = ReadTextFile(DefaultTemplatePath());
    ASSERT_FALSE(contents.empty());
    EXPECT_NE(contents.find("[FaceCamera]"), std::string::npos);
    EXPECT_NE(contents.find("enabled=false"), std::string::npos);
    EXPECT_NE(contents.find("analytic GPU"), std::string::npos);
    EXPECT_NE(contents.find("stale_timeout_ms=2000"), std::string::npos);
}
