#include "audio_ring_buffer.h"
#include <cstdio>
#include <cstring>

size_t AudioRingBuffer::Write(const float *data, size_t count) {
  std::lock_guard<std::mutex> lock(mutex);

  size_t avail = available.load();
  size_t freeSpace = capacity - avail;

  if (count > freeSpace) {
    // Track how many samples we're dropping for downstream gap compensation
    size_t dropped = count - freeSpace;
    droppedSamples.fetch_add(dropped);
    overflowFlag = true;
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
  available += count;

  return count;
}

size_t AudioRingBuffer::Read(float *outData, size_t count) {
  std::lock_guard<std::mutex> lock(mutex);

  size_t toRead = std::min(count, (size_t)available);

  size_t firstChunk = std::min(toRead, capacity - readPos);
  memcpy(outData, &buffer[readPos], firstChunk * sizeof(float));

  size_t secondChunk = toRead - firstChunk;
  if (secondChunk > 0) {
    memcpy(outData + firstChunk, &buffer[0], secondChunk * sizeof(float));
  }

  readPos = (readPos + toRead) % capacity;
  available -= toRead;

  return toRead;
}

size_t AudioRingBuffer::Peek(float *outData, size_t count) {
  std::lock_guard<std::mutex> lock(mutex);
  // Same as Read but don't advance pointer
  size_t toRead = std::min(count, (size_t)available);
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
  size_t toSkip = std::min(count, (size_t)available);

  readPos = (readPos + toSkip) % capacity;
  available -= toSkip;
  return toSkip;
}

void AudioRingBuffer::Clear() {
  std::lock_guard<std::mutex> lock(mutex);
  readPos = 0;
  writePos = 0;
  available = 0;
  overflowFlag = false;
  droppedSamples = 0; // Reset dropped counter on clear
}
