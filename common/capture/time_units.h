#pragma once

#include <cstdint>

namespace ce::time {

// Internal units only; descriptors and shared/DLL layouts retain their fixed-width integers.
template <typename Unit>
class Value {
public:
    constexpr Value() = default;
    explicit constexpr Value(int64_t value) : value_(value) {}
    constexpr int64_t count() const { return value_; }
private:
    int64_t value_ = 0;
};

using QpcTicks = Value<struct QpcTicksUnit>;
using QpcFrequency = Value<struct QpcFrequencyUnit>;
using Microseconds = Value<struct MicrosecondsUnit>;
using Milliseconds = Value<struct MillisecondsUnit>;
using AudioHundredNanoseconds = Value<struct AudioHundredNanosecondsUnit>;
using FrameIndex = Value<struct FrameIndexUnit>;  // zero-based encoded frame index
using GridTick = Value<struct GridTickUnit>;      // legacy one-based selection tick

}  // namespace ce::time
