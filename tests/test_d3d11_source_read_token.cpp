#include <gtest/gtest.h>
#include <wrl/client.h>

#include "captureengine/media/inject_source_read.h"

namespace {

using Microsoft::WRL::ComPtr;

// These tests own one context on one thread; production supplies the shared
// MediaEngine context lock to the same token implementation.
void LockTestContext() {}

class D3D11SourceReadTokenTest : public testing::Test {
protected:
    void SetUp() override {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ASSERT_EQ(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, nullptr, &context), S_OK);
        ASSERT_EQ(device.As(&device_), S_OK);
        ASSERT_EQ(context.As(&context_), S_OK);
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ASSERT_NE(event_, nullptr);
    }

    void TearDown() override {
        if (event_)
            CloseHandle(event_);
    }

    ComPtr<ID3D11Device5> device_;
    ComPtr<ID3D11DeviceContext4> context_;
    HANDLE event_ = nullptr;
};

TEST_F(D3D11SourceReadTokenTest, FenceWakesOnlyAfterGpuSignalAndSurvivesOwnerRelease) {
    ComPtr<ID3D11Fence> fence;
    ASSERT_EQ(device_->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), S_OK);
    fence->AddRef();
    D3D11SourceReadToken token(fence.Get(), 1, LockTestContext, LockTestContext);
    EXPECT_FALSE(token.Completed());
    ASSERT_TRUE(token.ArmEvent(event_));
    EXPECT_EQ(WaitForSingleObject(event_, 0), static_cast<DWORD>(WAIT_TIMEOUT));

    ASSERT_EQ(context_->Signal(fence.Get(), 1), S_OK);
    context_->Flush();
    fence.Reset();
    ASSERT_EQ(WaitForSingleObject(event_, 5000), static_cast<DWORD>(WAIT_OBJECT_0));
    EXPECT_TRUE(token.Completed());
}

TEST_F(D3D11SourceReadTokenTest, QueryFallbackFlushesAndWakesWithoutCpuPolling) {
    ComPtr<ID3D11Query> query;
    const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    ASSERT_EQ(device_->CreateQuery(&desc, &query), S_OK);
    context_->End(query.Get());
    D3D11SourceReadToken token(query.Detach(), 0, LockTestContext, LockTestContext);
    ASSERT_TRUE(token.ArmEvent(event_));
    ASSERT_EQ(WaitForSingleObject(event_, 5000), static_cast<DWORD>(WAIT_OBJECT_0));
    EXPECT_TRUE(token.Completed());
}

TEST_F(D3D11SourceReadTokenTest, MovedQueryTokenRetainsCompletionAndDeviceOwnership) {
    ComPtr<ID3D11Query> query;
    const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    ASSERT_EQ(device_->CreateQuery(&desc, &query), S_OK);
    context_->End(query.Get());
    D3D11SourceReadToken original(query.Detach(), 0, LockTestContext, LockTestContext);
    D3D11SourceReadToken moved;
    moved = std::move(original);
    device_.Reset();
    context_.Reset();
    ASSERT_TRUE(moved.ArmEvent(event_));
    ASSERT_EQ(WaitForSingleObject(event_, 5000), static_cast<DWORD>(WAIT_OBJECT_0));
    EXPECT_TRUE(moved.Completed());
}

}  // namespace
