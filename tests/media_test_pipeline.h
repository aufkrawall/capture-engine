#pragma once

// A media pipeline owned by one test: created through MediaEngine_Create and destroyed with the owner (DR-42).

#include <gtest/gtest.h>

#include "mediaengine/engine/mediaengine.h"

class MediaTestPipeline {
public:
    MediaTestPipeline() {
        MediaEngineCreateDesc desc{};
        desc.structSize = sizeof(desc);
        EXPECT_TRUE(MediaEngine_Create(&desc, &handle_));
    }
    ~MediaTestPipeline() {
        if (handle_)
            MediaEngine_Destroy(handle_);
    }
    MediaTestPipeline(const MediaTestPipeline&) = delete;
    MediaTestPipeline& operator=(const MediaTestPipeline&) = delete;

    MediaEngineHandle handle() const { return handle_; }

private:
    MediaEngineHandle handle_ = 0;
};
