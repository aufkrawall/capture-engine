#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
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

TEST(FrameQueueHardeningTest, RetargetDropsOnlyRetiredWgcEpochs) {
    FrameQueue queue(4);
    FakeTexture2D retiredTexture;
    FakeTexture2D activeTexture;

    QueuedFrame retired;
    retired.texture = &retiredTexture;
    retired.wgcSourceEpoch = 4;
    ASSERT_TRUE(queue.Push(std::move(retired), false));

    QueuedFrame active;
    active.texture = &activeTexture;
    active.wgcSourceEpoch = 5;
    ASSERT_TRUE(queue.Push(std::move(active), false));

    QueuedFrame inject;
    inject.isInjectMode = true;
    inject.wgcSourceEpoch = 0;
    ASSERT_TRUE(queue.Push(std::move(inject), false));

    EXPECT_EQ(queue.DiscardWgcEpochNotEqual(5), 1u);
    EXPECT_EQ(retiredTexture.ReleaseCallCount(), 1u);
    EXPECT_EQ(activeTexture.ReleaseCallCount(), 0u);
    EXPECT_EQ(queue.Size(), 2u);

    queue.Clear();
    EXPECT_EQ(activeTexture.ReleaseCallCount(), 1u);
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

TEST(FrameQueueHardeningTest, WgcPoolLeaseBlocksReuseUntilReleased) {
    auto leaseState = std::make_shared<WgcPoolLeaseState>();
    leaseState->Init(/*count=*/1, /*gen=*/77);

    ASSERT_TRUE(leaseState->TryAcquire(0));
    {
        WgcPoolSlotLease lease(leaseState, 0, 77);
        EXPECT_FALSE(leaseState->TryAcquire(0));
        EXPECT_EQ(leaseState->leasedCurrent.load(std::memory_order_relaxed), 1u);
        EXPECT_EQ(leaseState->leasedMax.load(std::memory_order_relaxed), 1u);
        EXPECT_EQ(leaseState->freeMin.load(std::memory_order_relaxed), 0u);
    }

    EXPECT_EQ(leaseState->leasedCurrent.load(std::memory_order_relaxed), 0u);
    EXPECT_TRUE(leaseState->TryAcquire(0));
    WgcPoolSlotLease reacquired(leaseState, 0, 77);
}

TEST(FrameQueueHardeningTest, DroppingQueuedWgcFrameReleasesPoolLease) {
    auto leaseState = std::make_shared<WgcPoolLeaseState>();
    leaseState->Init(/*count=*/2, /*gen=*/11);
    FrameQueue queue(1);

    QueuedFrame first;
    ASSERT_TRUE(leaseState->TryAcquire(0));
    first.wgcPoolSlot = 0;
    first.wgcPoolGeneration = 11;
    first.wgcPoolLease = WgcPoolSlotLease(leaseState, 0, 11);
    EXPECT_TRUE(queue.Push(std::move(first), false));
    EXPECT_EQ(leaseState->leasedCurrent.load(std::memory_order_relaxed), 1u);

    QueuedFrame second;
    ASSERT_TRUE(leaseState->TryAcquire(1));
    second.wgcPoolSlot = 1;
    second.wgcPoolGeneration = 11;
    second.wgcPoolLease = WgcPoolSlotLease(leaseState, 1, 11);
    EXPECT_TRUE(queue.Push(std::move(second), false));

    EXPECT_EQ(leaseState->slotLeases[0].load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(leaseState->slotLeases[1].load(std::memory_order_relaxed), 1u);
    EXPECT_EQ(leaseState->leasedCurrent.load(std::memory_order_relaxed), 1u);

    QueuedFrame out;
    ASSERT_TRUE(queue.Pop(out, 0));
    EXPECT_EQ(out.wgcPoolSlot, 1u);
    out.wgcPoolLease.Reset();
    EXPECT_EQ(leaseState->leasedCurrent.load(std::memory_order_relaxed), 0u);
}

TEST(FrameQueueHardeningTest, ClearingQueuedWgcFramesReleasesPoolLeases) {
    auto leaseState = std::make_shared<WgcPoolLeaseState>();
    leaseState->Init(/*count=*/2, /*gen=*/13);
    FrameQueue queue(2);

    for (uint32_t slot = 0; slot < 2; ++slot) {
        QueuedFrame frame;
        ASSERT_TRUE(leaseState->TryAcquire(slot));
        frame.wgcPoolSlot = slot;
        frame.wgcPoolGeneration = 13;
        frame.wgcPoolLease = WgcPoolSlotLease(leaseState, slot, 13);
        EXPECT_TRUE(queue.Push(std::move(frame), false));
    }
    EXPECT_EQ(leaseState->leasedCurrent.load(std::memory_order_relaxed), 2u);

    queue.Clear();

    EXPECT_EQ(leaseState->leasedCurrent.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(leaseState->slotLeases[0].load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(leaseState->slotLeases[1].load(std::memory_order_relaxed), 0u);
}

TEST(FrameQueueHardeningTest, InjectLineageSurvivesQueuedFrameMove) {
    QueuedFrame source;
    source.isInjectMode = true;
    source.sharedHandle = reinterpret_cast<HANDLE>(0x1234);
    source.fenceHandle = reinterpret_cast<HANDLE>(0x5678);
    source.fenceValue = 99;
    source.ringIndex = 3;
    source.frameIndex = 44;
    source.textureIndex = 2;
    source.deferCount = 2;

    QueuedFrame moved;
    moved = std::move(source);

    EXPECT_TRUE(moved.isInjectMode);
    EXPECT_EQ(moved.sharedHandle, reinterpret_cast<HANDLE>(0x1234));
    EXPECT_EQ(moved.fenceHandle, reinterpret_cast<HANDLE>(0x5678));
    EXPECT_EQ(moved.fenceValue, 99u);
    EXPECT_EQ(moved.ringIndex, 3u);
    EXPECT_EQ(moved.frameIndex, 44u);
    EXPECT_EQ(moved.textureIndex, 2);
    EXPECT_EQ(moved.deferCount, 2u);
    EXPECT_FALSE(source.isInjectMode);
    EXPECT_EQ(source.sharedHandle, nullptr);
    EXPECT_EQ(source.fenceHandle, nullptr);
    EXPECT_EQ(source.textureIndex, -1);
}
