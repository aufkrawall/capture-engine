#pragma once

#include <dxgiformat.h>
#include <mfapi.h>
#include <mfobjects.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "common/config/face_camera_config.h"

// Format, colour-space and plane-layout policy for the face camera
// (FaceCameraCapture, FaceCameraRenderer). Media Foundation's SourceReader runs
// with advanced video processing off so it never inserts the D3D11
// VideoProcessor MFT (llm-wiki/gpu-api-plan.md): NV12, YUY2 and RGB32 cameras
// pass through unchanged, MJPEG and H.264 cameras decode to NV12 or YUY2, and
// the renderer converts YUV to RGB in its single composite draw.

namespace ce::face_camera {

enum class PixelFormat : uint32_t {
    kBgra8 = 0,  // MFVideoFormat_RGB32 / ARGB32 (4 B/px)
    kNv12 = 1,   // MFVideoFormat_NV12, 4:2:0 Y + interleaved CbCr (1.5 B/px)
    kYuy2 = 2,   // MFVideoFormat_YUY2, packed 4:2:2 [Y0, Cb, Y1, Cr] (2 B/px)
};

enum class YuvMatrix : uint32_t {
    kBt709 = 0,
    kBt601 = 1,
    kBt2020 = 2,
};

enum class YuvRange : uint32_t {
    kLimited = 0,  // Y 16..235, Cb/Cr 16..240
    kFull = 1,     // Y 0..255, Cb/Cr 0..255
};

inline const char* DescribePixelFormat(PixelFormat format) {
    switch (format) {
        case PixelFormat::kNv12:
            return "NV12";
        case PixelFormat::kYuy2:
            return "YUY2";
        case PixelFormat::kBgra8:
        default:
            return "RGB32";
    }
}

inline const char* DescribeYuvMatrix(YuvMatrix matrix, PixelFormat format) {
    if (format == PixelFormat::kBgra8)
        return "srgb";
    switch (matrix) {
        case YuvMatrix::kBt601:
            return "bt601";
        case YuvMatrix::kBt2020:
            return "bt2020";
        case YuvMatrix::kBt709:
        default:
            return "bt709";
    }
}

inline const char* DescribeYuvRange(YuvRange range, PixelFormat format) {
    if (format == PixelFormat::kBgra8)
        return "full";
    return range == YuvRange::kFull ? "full" : "limited";
}

inline const char* ShapeName(Shape shape) {
    switch (shape) {
        case Shape::kRectangle: return "rectangle";
        case Shape::kRoundedRectangle: return "rounded";
        case Shape::kCircle: return "circle";
    }
    return "unknown";
}

inline const char* PositionName(Position position) {
    switch (position) {
        case Position::kTopLeft: return "top_left";
        case Position::kTopCenter: return "top_center";
        case Position::kTopRight: return "top_right";
        case Position::kCenterLeft: return "center_left";
        case Position::kCenter: return "center";
        case Position::kCenterRight: return "center_right";
        case Position::kBottomLeft: return "bottom_left";
        case Position::kBottomCenter: return "bottom_center";
        case Position::kBottomRight: return "bottom_right";
        case Position::kCustom: return "custom";
    }
    return "unknown";
}

inline const char* CropName(Crop crop) {
    return crop == Crop::kStretch ? "stretch" : "fill";
}

// Media Foundation's default when a camera type omits MF_MT_YUV_MATRIX:
// BT.709 for HD frames (width >= 1280 or height > 576), BT.601 for SD.
inline YuvMatrix ResolveYuvMatrix(uint32_t mfVideoTransferMatrix, uint32_t width, uint32_t height) {
    switch (mfVideoTransferMatrix) {
        case MFVideoTransferMatrix_BT709:
            return YuvMatrix::kBt709;
        case MFVideoTransferMatrix_BT601:
        case MFVideoTransferMatrix_SMPTE240M:
            return YuvMatrix::kBt601;
        case 4:  // MFVideoTransferMatrix_BT2020_10
        case 5:  // MFVideoTransferMatrix_BT2020_12
            return YuvMatrix::kBt2020;
        default:
            return (width >= 1280u || height > 576u) ? YuvMatrix::kBt709 : YuvMatrix::kBt601;
    }
}

// Media Foundation's default when a camera type omits MF_MT_VIDEO_NOMINAL_RANGE:
// studio range (16..235) unless full range (MFNominalRange_0_255) is declared.
inline YuvRange ResolveYuvRange(uint32_t mfNominalRange) {
    return mfNominalRange == MFNominalRange_0_255 ? YuvRange::kFull : YuvRange::kLimited;
}

inline bool SubtypeToPixelFormat(REFGUID subtype, PixelFormat* out) {
    if (!out)
        return false;
    if (subtype == MFVideoFormat_NV12) {
        *out = PixelFormat::kNv12;
        return true;
    }
    if (subtype == MFVideoFormat_YUY2) {
        *out = PixelFormat::kYuy2;
        return true;
    }
    if (subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32) {
        *out = PixelFormat::kBgra8;
        return true;
    }
    return false;
}

inline bool DxgiToPixelFormat(DXGI_FORMAT format, PixelFormat* out) {
    if (!out)
        return false;
    switch (format) {
        case DXGI_FORMAT_NV12:
            *out = PixelFormat::kNv12;
            return true;
        case DXGI_FORMAT_YUY2:
            *out = PixelFormat::kYuy2;
            return true;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            *out = PixelFormat::kBgra8;
            return true;
        default:
            return false;
    }
}

// Uncompressed subtypes to try on IMFSourceReader::SetCurrentMediaType. When
// the camera's chosen native mode is already uncompressed (NV12, YUY2, RGB32,
// ARGB32), that exact subtype is tried first so the reader passes samples
// through without any MFT. For compressed modes (MJPG, H264), NV12 and YUY2
// are tried before RGB32 so the decoder outputs native YUV without the Video
// Processor MFT.
inline std::vector<GUID> CandidateOutputSubtypes(REFGUID nativeSubtype) {
    const GUID fallbackOrder[] = {MFVideoFormat_NV12, MFVideoFormat_YUY2, MFVideoFormat_RGB32, MFVideoFormat_ARGB32};
    std::vector<GUID> candidates;
    PixelFormat nativePixelFormat = PixelFormat::kBgra8;
    if (SubtypeToPixelFormat(nativeSubtype, &nativePixelFormat))
        candidates.push_back(nativeSubtype);
    for (const GUID& candidate : fallbackOrder) {
        bool present = false;
        for (const GUID& existing : candidates) {
            if (existing == candidate) {
                present = true;
                break;
            }
        }
        if (!present)
            candidates.push_back(candidate);
    }
    return candidates;
}

inline bool DimensionsValidForFormat(PixelFormat format, uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        return false;
    if (format == PixelFormat::kNv12)
        return width >= 2u && height >= 2u && (width & 1u) == 0u && (height & 1u) == 0u;
    if (format == PixelFormat::kYuy2)
        return width >= 2u && (width & 1u) == 0u;
    return true;
}

inline uint32_t DefaultRowBytes(PixelFormat format, uint32_t width) {
    switch (format) {
        case PixelFormat::kNv12:
            return width;
        case PixelFormat::kYuy2:
            return width * 2u;
        case PixelFormat::kBgra8:
        default:
            return width * 4u;
    }
}

enum class CpuBufferOrigin { kAllocationStart, kTopScanline };

// Extracts tightly packed planes from a contiguous or 2D-locked CPU camera
// buffer. For NV12, `primary` receives `width * height` Y bytes and `chroma`
// receives `(width / 2) * (height / 2) * 2` interleaved Cb,Cr bytes. For YUY2
// and BGRA8, `primary` receives the packed rows and `chroma` is cleared.
// Lock2D already returns the displayed top scanline, even for negative strides;
// IMFMediaBuffer::Lock returns the allocation start instead.
inline bool ExtractCpuPlanes(PixelFormat format, const uint8_t* data, size_t dataLength, uint32_t width,
                             uint32_t height, int64_t stride, std::vector<uint8_t>* primary,
                             std::vector<uint8_t>* chroma,
                             CpuBufferOrigin origin = CpuBufferOrigin::kAllocationStart) {
    if (!data || !primary || !chroma || !DimensionsValidForFormat(format, width, height))
        return false;
    const size_t rowBytes = DefaultRowBytes(format, width);
    if (static_cast<uint64_t>(rowBytes) * height > (std::numeric_limits<size_t>::max)() / 2u)
        return false;
    const int64_t effectiveStride = stride == 0 ? static_cast<int64_t>(rowBytes) : stride;
    if (format != PixelFormat::kBgra8 && effectiveStride < 0)
        return false;  // YUV formats are always top-down
    const uint64_t absStride = static_cast<uint64_t>(std::llabs(effectiveStride));
    if (absStride < rowBytes)
        return false;
    const uint32_t totalRows = format == PixelFormat::kNv12 ? (height + height / 2u) : height;
    const uint64_t requiredBytes = absStride * (totalRows - 1u) + rowBytes;
    if (requiredBytes > dataLength)
        return false;

    primary->resize(rowBytes * height);
    const uint8_t* firstRow =
        effectiveStride < 0 && origin == CpuBufferOrigin::kAllocationStart
            ? data + static_cast<size_t>(height - 1u) * static_cast<size_t>(absStride)
            : data;
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(primary->data() + static_cast<size_t>(row) * rowBytes,
                    firstRow + static_cast<ptrdiff_t>(row) * effectiveStride, rowBytes);
    }
    if (format == PixelFormat::kNv12) {
        const uint32_t chromaRows = height / 2u;
        chroma->resize(rowBytes * chromaRows);
        const uint8_t* chromaStart = data + static_cast<size_t>(height) * static_cast<size_t>(absStride);
        for (uint32_t row = 0; row < chromaRows; ++row) {
            std::memcpy(chroma->data() + static_cast<size_t>(row) * rowBytes,
                        chromaStart + static_cast<size_t>(row) * static_cast<size_t>(absStride), rowBytes);
        }
    } else {
        chroma->clear();
    }
    return true;
}

}  // namespace ce::face_camera
