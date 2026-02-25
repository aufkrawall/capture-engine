#include "audio_ring_buffer.h"
#include <cstdio>
#include <cstring>

size_t AudioRingBuffer::Write(const float *data, size_t count) {
  std::lock_guard<std::mutex> lock(mutex);

  // CRITICAL FIX: Use acquire ordering to synchronize with reads
  size_t avail = available.load(std::memory_order_acquire);
  size_t freeSpace = capacity - avail;

  if (count > freeSpace) {
    // Track how many samples we're dropping for downstream gap compensation
    size_t dropped = count - freeSpace;
    droppedSamples.fetch_add(dropped, std::memory_order_relaxed);
    overflowFlag.store(true, std::memory_order_relaxed);
    count = freeSpace;
  }

  if (count == 0) {
    return 0;
  }

  size_t firstChunk = std::min(count, capacity - writePos);
  memcpy(&buffer[writePos], data, firstChunk * sizeof(float));

  size_t secondChunk = count - firstChunk;
  if (secondChunk > 0) {
    memcpy(&buffer[0], data + firstChunk, secondChunk * sizeof(float));
  }

  writePos = (writePos + count) % capacity;
  // CRITICAL FIX: Use release ordering to publish writes to readers
  available.store(avail + count, std::memory_order_release);

  return count;
}

size_t AudioRingBuffer::Read(float *outData, size_t count) {
  std::lock_guard<std::mutex> lock(mutex);

  // CRITICAL FIX: Use acquire ordering to synchronize with writes
  size_t avail = available.load(std::memory_order_acquire);
  size_t toRead = std::min(count, avail);

  size_t firstChunk = std::min(toRead, capacity - readPos);
  memcpy(outData, &buffer[readPos], firstChunk * sizeof(float));

  size_t secondChunk = toRead - firstChunk;
  if (secondChunk > 0) {
    memcpy(outData + firstChunk, &buffer[0], secondChunk * sizeof(float));
  }

  readPos = (readPos + toRead) % capacity;
  // CRITICAL FIX: Use release ordering to publish the updated available count
  available.store(avail - toRead, std::memory_order_release);

  return toRead;
}

size_t AudioRingBuffer::Peek(float *outData, size_t count) {
  std::lock_guard<std::mutex> lock(mutex);
  // Same as Read but don't advance pointer
  // CRITICAL FIX: Use acquire ordering to synchronize with writes
  size_t avail = available.load(std::memory_order_acquire);
  size_t toRead = std::min(count, avail);
  size_t tempReadPos = readPos;

  size_t firstChunk = std::min(toRead, capacity - tempReadPos);
  memcpy(outData, &buffer[tempReadPos], firstChunk * sizeof(float));

  size_t secondChunk = toRead - firstChunk;
  if (secondChunk > 0) {
    memcpy(outData + firstChunk, &buffer[0], secondChunk * sizeof(float));
  }
  return toRead;
}

size_t AudioRingBuffer::Skip(size_t count) {
  std::lock_guard<std::mutex> lock(mutex);
  // CRITICAL FIX: Use acquire ordering to synchronize with writes
  size_t avail = available.load(std::memory_order_acquire);
  size_t toSkip = std::min(count, avail);

  readPos = (readPos + toSkip) % capacity;
  // CRITICAL FIX: Use release ordering to publish the updated available count
  available.store(avail - toSkip, std::memory_order_release);
  return toSkip;
}

void AudioRingBuffer::Clear() {
  std::lock_guard<std::mutex> lock(mutex);
  readPos = 0;
  writePos = 0;
  // CRITICAL FIX: Use release ordering to publish the cleared state
  available.store(0, std::memory_order_release);
  overflowFlag.store(false, std::memory_order_relaxed);
  droppedSamples.store(0, std::memory_order_relaxed); // Reset dropped counter on clear
}
