#include "../common/frame_queue.h"
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

TEST(FrameQueueTest, PushPop) {
  FrameQueue queue(5);
  QueuedFrame frameIn;
  frameIn.timestamp = 12345;

  EXPECT_TRUE(queue.Push(frameIn));
  EXPECT_EQ(queue.Size(), 1);

  QueuedFrame frameOut;
  EXPECT_TRUE(queue.Pop(frameOut, 100)); // 100ms timeout
  EXPECT_EQ(frameOut.timestamp, 12345);
  EXPECT_EQ(queue.Size(), 0);
}

TEST(FrameQueueTest, CapacityAndDrop) {
  FrameQueue queue(2);
  QueuedFrame f1{.timestamp = 1};
  QueuedFrame f2{.timestamp = 2};
  QueuedFrame f3{.timestamp = 3};

  EXPECT_TRUE(queue.Push(f1));
  EXPECT_TRUE(queue.Push(f2));
  EXPECT_TRUE(queue.IsFull());

  // Third push should drop the oldest (f1)
  EXPECT_TRUE(queue.Push(f3));
  EXPECT_EQ(queue.Size(), 2);
  EXPECT_EQ(queue.GetDroppedCount(), 1);

  QueuedFrame out;
  EXPECT_TRUE(queue.Pop(out, 0));
  EXPECT_EQ(out.timestamp, 2); // f1 dropped, expect f2

  EXPECT_TRUE(queue.Pop(out, 0));
  EXPECT_EQ(out.timestamp, 3);
}

TEST(FrameQueueTest, PopTimeout) {
  FrameQueue queue(5);
  QueuedFrame out;

  auto start = std::chrono::steady_clock::now();
  bool result = queue.Pop(out, 50); // 50ms wait
  auto end = std::chrono::steady_clock::now();

  EXPECT_FALSE(result);

  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
          .count();
  // Ensure we waited at least closely to 50ms (allow some slack for OS
  // scheduling)
  EXPECT_GE(duration, 30);
}

TEST(FrameQueueTest, Shutdown) {
  FrameQueue queue(5);

  std::thread consumer([&queue]() {
    QueuedFrame out;
    bool result = queue.Pop(out, 1000);
    EXPECT_FALSE(
        result); // Should return false on shutdown even if timeout not reached
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  queue.Shutdown();

  if (consumer.joinable())
    consumer.join();
}
