#include "encoder_rate_compensation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ce::media {
namespace {

// A change is worth a reconfigure only past this ratio (on top of the
// one-frame window rounding handled in UpdateDesired).
constexpr double kHysteresis = 1.03;
// Encoder frames in the short window, relative to what the current factor
// expects, that mean the source sped up rather than jittered.
constexpr double kSpeedUpRatio = 1.25;
// NVENC takes rates as uint32_t bits per second.
constexpr double kEncoderRateLimit = static_cast<double>(std::numeric_limits<uint32_t>::max());

}  // namespace

void SubmittedRateCompensation::Window::Reset(size_t slots, double fill) {
    values_.assign(slots, fill);
    next_ = 0;
    sum_ = fill * static_cast<double>(slots);
}

double SubmittedRateCompensation::Window::Push(double value) {
    const double dropped = values_[next_];
    sum_ += value - dropped;
    values_[next_] = value;
    next_ = (next_ + 1) % values_.size();
    return dropped;
}

void SubmittedRateCompensation::Reset(int fps, int64_t baseBitRate, int64_t baseMaxRate, bool liveCap) {
    *this = SubmittedRateCompensation{};
    liveCap_ = liveCap;
    baseBitRate_ = std::max<int64_t>(0, baseBitRate);
    baseMaxRate_ = std::max<int64_t>(0, baseMaxRate);
    active_ = fps > 0 && (baseBitRate_ > 0 || baseMaxRate_ > 0);
    const int64_t largest = std::max(baseBitRate_, baseMaxRate_);
    factorCap_ = largest > 0 ? std::max(1.0, kEncoderRateLimit / static_cast<double>(largest)) : 1.0;
    // Half a second: shorter windows misjudge irregular cadences, longer ones
    // follow cadence changes too late (both measured against re-encoding,
    // llm-wiki/nvenc-encoding.md); rounding guards keep it stable.
    longWindow_.Reset(static_cast<size_t>(std::max(8, fps / 2)), 1.0);
    shortWindow_.Reset(static_cast<size_t>(std::max(4, fps / 8)), 1.0);
    spend_.Reset(static_cast<size_t>(std::max(8, fps)), 1.0);
    current_ = RatesFor(1.0);
}

SubmittedRateCompensation::Rates SubmittedRateCompensation::RatesFor(double factor) const {
    Rates rates;
    rates.factor = factor;
    rates.bitRate = baseBitRate_ > 0 ? std::llround(static_cast<double>(baseBitRate_) * factor) : 0;
    rates.maxRate = baseMaxRate_ > 0 ? std::llround(static_cast<double>(baseMaxRate_) * factor) : 0;
    return rates;
}

void SubmittedRateCompensation::UpdateDesired() {
    const double slots = static_cast<double>(longWindow_.size());
    const double frames = longWindow_.Sum();
    // A window holds whole frames, so a steady cadence reads one frame more or
    // less depending on its phase. Raise to the measured value only if even
    // one more frame would not ask for less than now, lower only if even one
    // less frame would still ask for less: no cadence can toggle the rate,
    // and none settles a frame short of its value.
    const double measured = std::min(factorCap_, slots / std::max(frames, 1.0));
    const double raiseFloor = slots / (frames + 1.0);
    const double lowerTarget = frames > 1.0 ? slots / (frames - 1.0) : factorCap_;
    // The short window counts once it clearly saw more encoder frames than a
    // factor expects (the source sped up): it lowers at once and blocks a
    // raise while the long window still remembers the slower source.
    const double shortFraction = shortWindow_.Fraction();
    const double quantum = 1.0 / static_cast<double>(shortWindow_.size());
    const auto spedUpFor = [&](double factor) {
        return factor > 1.0 && shortFraction >= std::min(1.0, kSpeedUpRatio / factor + quantum);
    };
    const bool spedUp = spedUpFor(desired_);
    const bool raise = measured > desired_ * kHysteresis && raiseFloor >= desired_ &&
                       slotsSinceChange_ >= shortWindow_.size() && !spedUpFor(measured);
    const bool lower = spedUp || lowerTarget < desired_ / kHysteresis;
    if (!raise && !lower)
        return;
    const double fraction = std::max({frames / slots, spedUp ? shortFraction : 0.0, 1.0 / factorCap_});
    const double target = std::min(factorCap_, 1.0 / fraction);
    if (target == desired_)
        return;
    desired_ = target;
    slotsSinceChange_ = 0;
}

void SubmittedRateCompensation::Apply(double factor) {
    if (factor == current_.factor)
        return;
    current_ = RatesFor(factor);
    maxAppliedFactor_ = std::max(maxAppliedFactor_, factor);
    pending_ = true;
    ++changes_;
}

void SubmittedRateCompensation::ObserveSlot(bool submitted) {
    if (!active_)
        return;
    longWindow_.Push(submitted ? 1.0 : 0.0);
    shortWindow_.Push(submitted ? 1.0 : 0.0);
    ++slotsSinceChange_;
    UpdateDesired();
    if (!submitted) {
        spend_.Push(0.0);
        return;
    }
    // Live cap: what this frame may take so that the last second's budgets
    // stay within kPeakAllowance of the target. It lowers at once and gives
    // room back only past the hysteresis (or straight to the desired factor),
    // so a bound cap cannot reconfigure the encoder on every frame.
    double factor = desired_;
    if (liveCap_) {
        const double room = kPeakAllowance * static_cast<double>(spend_.size()) - (spend_.Sum() - spend_.Oldest());
        if (factor > room) {
            factor = std::max(kLiveMinFactor, room);
            ++peakCapped_;
        }
    }
    if (factor > current_.factor && factor != desired_ && factor < current_.factor * kHysteresis)
        factor = current_.factor;
    Apply(factor);
    spend_.Push(current_.factor);
}

bool SubmittedRateCompensation::TakeChange() {
    if (!pending_)
        return false;
    pending_ = false;
    return true;
}

void SubmittedRateCompensation::OnRejected() {
    ++rejections_;
    factorCap_ = std::max(1.0, std::min(factorCap_, current_.factor / kHysteresis));
    desired_ = std::min(desired_, factorCap_);
    slotsSinceChange_ = 0;
    Apply(std::min(current_.factor, factorCap_));
}

}  // namespace ce::media
