#include "config_load_internal.h"

namespace {

constexpr const char* kSection = "FaceCamera";

std::string NormalizedValue(ConfigReader& reader, const char* key, const char* defaultValue) {
    return NormalizeConfigToken(reader.GetStr(kSection, key, defaultValue));
}

uint32_t ParseRgbColor(ConfigReader& reader, const char* key, uint32_t defaultColor) {
    std::string value = reader.GetStr(kSection, key, "#FFFFFF");
    if (!value.empty() && value.front() == '#')
        value.erase(value.begin());
    uint32_t parsed = 0;
    if (value.size() == 6 &&
        std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }) &&
        TryParseUInt32(value, parsed, 16)) {
        return parsed;
    }
    LogInvalidConfigBoundary(kSection, key, value, "#FFFFFF");
    return defaultColor;
}

}  // namespace

void LoadFaceCamera(ConfigReader& reader, AppConfig& config) {
    auto& faceCamera = config.video.faceCamera;
    faceCamera.enabled = reader.GetBool(kSection, "enabled", false);
    faceCamera.device = Trim(StripOuterQuotes(reader.GetStr(kSection, "device", "default")));
    if (faceCamera.device.empty())
        faceCamera.device = "default";

    const std::string resolution = NormalizedValue(reader, "resolution", "1280x720");
    if (!ce::face_camera::TryParseResolution(resolution, &faceCamera.requestedWidth,
                                             &faceCamera.requestedHeight)) {
        LogInvalidConfigBoundary(kSection, "resolution", resolution, "1280x720");
        faceCamera.requestedWidth = 1280;
        faceCamera.requestedHeight = 720;
    }
    faceCamera.requestedFps =
        static_cast<uint32_t>(reader.GetBoundedInt(kSection, "fps", 30, 1, 240));

    const std::string position = NormalizedValue(reader, "position", "bottom_right");
    if (!ce::face_camera::TryParsePosition(position, &faceCamera.position)) {
        LogInvalidConfigBoundary(kSection, "position", position, "bottom_right");
        faceCamera.position = ce::face_camera::Position::kBottomRight;
    }
    const std::string shape = NormalizedValue(reader, "shape", "rounded");
    if (!ce::face_camera::TryParseShape(shape, &faceCamera.shape)) {
        LogInvalidConfigBoundary(kSection, "shape", shape, "rounded");
        faceCamera.shape = ce::face_camera::Shape::kRoundedRectangle;
    }
    const std::string crop = NormalizedValue(reader, "crop", "fill");
    if (!ce::face_camera::TryParseCrop(crop, &faceCamera.crop)) {
        LogInvalidConfigBoundary(kSection, "crop", crop, "fill");
        faceCamera.crop = ce::face_camera::Crop::kFill;
    }

    faceCamera.widthPercent = reader.GetBoundedFloat(kSection, "width_percent", 24.0f, 5.0f, 100.0f);
    faceCamera.marginPercent = reader.GetBoundedFloat(kSection, "margin_percent", 2.0f, 0.0f, 25.0f);
    faceCamera.customXPercent = reader.GetBoundedFloat(kSection, "x_percent", 100.0f, 0.0f, 100.0f);
    faceCamera.customYPercent = reader.GetBoundedFloat(kSection, "y_percent", 100.0f, 0.0f, 100.0f);
    faceCamera.cornerRadiusPercent =
        reader.GetBoundedFloat(kSection, "corner_radius_percent", 12.0f, 0.0f, 50.0f);
    faceCamera.opacity = reader.GetBoundedFloat(kSection, "opacity_percent", 100.0f, 0.0f, 100.0f) / 100.0f;
    faceCamera.borderWidthPercent =
        reader.GetBoundedFloat(kSection, "border_width_percent", 0.75f, 0.0f, 10.0f);
    faceCamera.borderColorRgb = ParseRgbColor(reader, "border_color", 0xFFFFFFu);
    faceCamera.mirror = reader.GetBool(kSection, "mirror", true);
    faceCamera.staleTimeoutMs =
        static_cast<uint32_t>(reader.GetBoundedInt(kSection, "stale_timeout_ms", 2000, 0, 10000));
}
