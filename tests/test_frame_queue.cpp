#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "../common/frame_queue.h"

namespace {

class FakeTexture2D : public ID3D11Texture2D {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        *ppvObject = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(ID3D11Resource) ||
            riid == __uuidof(ID3D11Texture2D)) {
            *ppvObject = static_cast<ID3D11Texture2D*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        releaseCalls_.fetch_add(1, std::memory_order_relaxed);
        return refCount_.fetch_sub(1, std::memory_order_relaxed) - 1;
    }

    void STDMETHODCALLTYPE GetDevice(ID3D11Device** ppDevice) override {
        if (ppDevice) {
            *ppDevice = nullptr;
        }
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override {
        return E_NOTIMPL;
    }

    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* pResourceDimension) override {
        if (pResourceDimension) {
            *pResourceDimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
        }
    }

    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}

    UINT STDMETHODCALLTYPE GetEvictionPriority() override {
        return 0;
    }

    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* pDesc) override {
        if (pDesc) {
            *pDesc = {};
            pDesc->Width = 1;
            pDesc->Height = 1;
            pDesc->MipLevels = 1;
            pDesc->ArraySize = 1;
            pDesc->Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            pDesc->SampleDesc.Count = 1;
            pDesc->Usage = D3D11_USAGE_DEFAULT;
            pDesc->BindFlags = D3D11_BIND_SHADER_RESOURCE;
        }
    }

    uint32_t ReleaseCallCount() const {
        return releaseCalls_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<ULONG> refCount_{1};
    std::atomic<uint32_t> releaseCalls_{0};
};

}  // namespace

TEST(FrameQueueTest, PushPop) {
    FrameQueue queue(5);
    QueuedFrame frameIn;
    frameIn.timestamp = 12345;

    EXPECT_TRUE(queue.Push(std::move(frameIn)));
    EXPECT_EQ(queue.Size(), 1);

    QueuedFrame frameOut;
    EXPECT_TRUE(queue.Pop(frameOut, 100));  // 100ms timeout
    EXPECT_EQ(frameOut.timestamp, 12345);
    EXPECT_EQ(queue.Size(), 0);
}

TEST(FrameQueueTest, PeekTimestampReturnsFrontWithoutRemovingIt) {
    FrameQueue queue(3);
    QueuedFrame frame;
    frame.timestamp = 98765;

    EXPECT_TRUE(queue.Push(std::move(frame)));

    int64_t timestamp = 0;
    EXPECT_TRUE(queue.PeekTimestamp(timestamp));
    EXPECT_EQ(timestamp, 98765);
    EXPECT_EQ(queue.Size(), 1u);
}

TEST(FrameQueueTest, CapacityAndDrop) {
    FrameQueue queue(2);

    QueuedFrame f1;
    f1.timestamp = 1;
    QueuedFrame f2;
    f2.timestamp = 2;
    QueuedFrame f3;
    f3.timestamp = 3;

    EXPECT_TRUE(queue.Push(std::move(f1)));
    EXPECT_TRUE(queue.Push(std::move(f2)));
    EXPECT_TRUE(queue.IsFull());

    // Third push should drop the oldest (timestamp=1)
    EXPECT_TRUE(queue.Push(std::move(f3)));
    EXPECT_EQ(queue.Size(), 2);
    EXPECT_EQ(queue.GetDroppedCount(), 1);

    QueuedFrame out;
    EXPECT_TRUE(queue.Pop(out, 0));
    EXPECT_EQ(out.timestamp, 2);  // timestamp=1 dropped, expect 2

    EXPECT_TRUE(queue.Pop(out, 0));
    EXPECT_EQ(out.timestamp, 3);
}

TEST(FrameQueueTest, StartRecordingGracePeriodSuppresssImmediateOverflowDrops) {
    FrameQueue queue(1);
    queue.StartRecording();

    QueuedFrame first;
    first.timestamp = 1;
    QueuedFrame second;
    second.timestamp = 2;

    EXPECT_TRUE(queue.Push(std::move(first)));
    EXPECT_TRUE(queue.Push(std::move(second)));
    EXPECT_EQ(queue.GetDroppedCount(), 0u);

    QueuedFrame out;
    ASSERT_TRUE(queue.Pop(out, 0));
    EXPECT_EQ(out.timestamp, 2);
}

TEST(FrameQueueTest, PopTimeout) {
    FrameQueue queue(5);
    QueuedFrame out;

    auto start = std::chrono::steady_clock::now();
    bool result = queue.Pop(out, 10);  // 10ms wait
    auto end = std::chrono::steady_clock::now();

    EXPECT_FALSE(result);

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_GE(duration, 5);
}

TEST(FrameQueueTest, Shutdown) {
    FrameQueue queue(5);

    std::thread consumer([&queue]() {
        QueuedFrame out;
        bool result = queue.Pop(out, 1000);
        EXPECT_FALSE(result);  // Should return false on shutdown even if timeout not reached
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    queue.Shutdown();

    if (consumer.joinable())
        consumer.join();
}

TEST(FrameQueueHardeningTest, DropReleasesOldestTexture) {
    FrameQueue queue(1);

    FakeTexture2D droppedTexture;
    {
        QueuedFrame firstFrame;
        firstFrame.texture = &droppedTexture;
        queue.Push(std::move(firstFrame), false);
    }
    {
        QueuedFrame secondFrame;
        secondFrame.isInjectMode = true;
        secondFrame.timestamp = 2;
        queue.Push(std::move(secondFrame), false);
    }
    EXPECT_EQ(droppedTexture.ReleaseCallCount(), 1u);
}

TEST(FrameQueueHardeningTest, ClearReleasesAllQueuedTextures) {
    FrameQueue queue(4);

    FakeTexture2D textureA;
    FakeTexture2D textureB;

    {
        QueuedFrame frameA;
        frameA.texture = &textureA;
        queue.Push(std::move(frameA), false);
    }
    {
        QueuedFrame frameB;
        frameB.texture = &textureB;
        queue.Push(std::move(frameB), false);
    }

    queue.Clear();

    EXPECT_EQ(textureA.ReleaseCallCount(), 1u);
    EXPECT_EQ(textureB.ReleaseCallCount(), 1u);
    EXPECT_TRUE(queue.IsEmpty());
}

TEST(FrameQueueHardeningTest, DestructorReleasesRemainingTextures) {
    FakeTexture2D texture;
    {
        FrameQueue queue(2);
        QueuedFrame frame;
        frame.texture = &texture;

        queue.Push(std::move(frame), false);
        EXPECT_EQ(texture.ReleaseCallCount(), 0u);
    }

    EXPECT_EQ(texture.ReleaseCallCount(), 1u);
}
