#pragma once

// Shared includes and helpers for the test_video_encoder_source suite, which is split
// across several .cpp files to stay under the AGENTS.md size ceiling.

#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <mmsystem.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "../common/raii_helpers.h"
#include "../mediaengine/video_color_conversion_shader.h"
#include "source_fragment_reader.h"

namespace {

std::string ReadVideoEncoderSource() {
    const std::filesystem::path source = std::filesystem::current_path() / "mediaengine" / "video_encoder.cpp";
    return ce::test_source::ReadLogicalSource(source);
}

std::string ReadCursorRendererSource() {
    const std::filesystem::path source = std::filesystem::current_path() / "mediaengine" / "cursor_renderer.cpp";
    return ce::test_source::ReadLogicalSource(source);
}

std::string ReadVideoColorShaderSource() {
    const std::filesystem::path source =
        std::filesystem::current_path() / "mediaengine" / "video_color_conversion_shader.h";
    return ce::test_source::ReadLogicalSource(source);
}

std::string ReadVideoMetadataSource() {
    const std::filesystem::path source = std::filesystem::current_path() / "mediaengine" / "video_metadata.cpp";
    return ce::test_source::ReadLogicalSource(source);
}

size_t CountOccurrences(const std::string& source, const std::string& needle) {
    size_t count = 0;
    size_t position = 0;
    while ((position = source.find(needle, position)) != std::string::npos) {
        count++;
        position += needle.size();
    }
    return count;
}

}  // namespace
