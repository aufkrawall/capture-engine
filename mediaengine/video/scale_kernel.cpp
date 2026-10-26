#include "scale_kernel.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace ce::video_scale {

namespace {

constexpr double kPi = 3.14159265358979323846;
// Weights this small are exact zeros of the kernel (integer offsets at unit
// stretch) or below float precision; trimming them saves taps.
constexpr double kNegligibleWeight = 1e-9;

bool EqualsIgnoreCase(const std::string& value, const char* expected) {
    size_t i = 0;
    for (; i < value.size() && expected[i]; ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) != expected[i])
            return false;
    }
    return i == value.size() && !expected[i];
}

struct RawTaps {
    int32_t first = 0;
    std::vector<double> weights;
    uint32_t lobeBegin = 0;
    uint32_t lobeEnd = 0;
};

RawTaps OutputTaps(uint32_t index, double ratio, double stretch, const KernelSpec& spec) {
    const double centre = (static_cast<double>(index) + 0.5) * ratio - 0.5;
    const double support = static_cast<double>(spec.lobes) * stretch;
    // Source samples strictly inside the support; its ends weigh zero.
    int32_t lo = static_cast<int32_t>(std::floor(centre - support)) + 1;
    int32_t hi = static_cast<int32_t>(std::ceil(centre + support)) - 1;
    std::vector<double> weights;
    for (int32_t j = lo; j <= hi; ++j)
        weights.push_back(KernelWeight((static_cast<double>(j) - centre) / stretch, spec));
    size_t begin = 0;
    size_t end = weights.size();
    while (begin < end && std::fabs(weights[begin]) <= kNegligibleWeight)
        ++begin;
    while (end > begin && std::fabs(weights[end - 1]) <= kNegligibleWeight)
        --end;

    RawTaps taps;
    taps.first = lo + static_cast<int32_t>(begin);
    taps.weights.assign(weights.begin() + static_cast<std::ptrdiff_t>(begin),
                        weights.begin() + static_cast<std::ptrdiff_t>(end));
    double sum = 0.0;
    for (double w : taps.weights)
        sum += w;
    for (double& w : taps.weights)
        w /= sum;  // the main lobe is positive, so sum > 0

    // Main lobe: |j - centre| < stretch. At least one sample lies in an open
    // interval of length 2 * stretch >= 2.
    bool lobeFound = false;
    for (uint32_t k = 0; k < taps.weights.size(); ++k) {
        const double offset = std::fabs(static_cast<double>(taps.first + static_cast<int32_t>(k)) - centre);
        if (offset < stretch) {
            if (!lobeFound)
                taps.lobeBegin = k;
            taps.lobeEnd = k + 1;
            lobeFound = true;
        }
    }
    return taps;
}

uint64_t PassTaps(uint64_t outputs, const AxisTable& table) {
    return outputs * table.taps;
}

}  // namespace

KernelSpec KernelSpecFromConfig(const std::string& quality, int sharpness) {
    KernelSpec spec;
    spec.lobes = EqualsIgnoreCase(quality, "best") ? 3u : 2u;
    spec.sideLobeGain = static_cast<float>(std::clamp(sharpness, 0, 100)) / 100.0f;
    return spec;
}

const char* DescribeKernel(const KernelSpec& spec) {
    return spec.lobes >= 3 ? "lanczos3" : "lanczos2";
}

double KernelWeight(double x, const KernelSpec& spec) {
    const double a = static_cast<double>(spec.lobes);
    const double distance = std::fabs(x);
    if (distance >= a)
        return 0.0;
    if (distance < 1e-12)
        return 1.0;
    const double px = kPi * distance;
    const double lanczos = a * std::sin(px) * std::sin(px / a) / (px * px);
    return distance < 1.0 ? lanczos : lanczos * static_cast<double>(std::clamp(spec.sideLobeGain, 0.0f, 1.0f));
}

AxisTable BuildAxisTable(uint32_t inSize, uint32_t outSize, const KernelSpec& spec) {
    AxisTable table;
    table.inSize = inSize;
    table.outSize = outSize;
    if (inSize == 0 || outSize == 0 || spec.lobes == 0)
        return table;
    const double ratio = static_cast<double>(inSize) / static_cast<double>(outSize);
    table.stretch = std::max(1.0, ratio);

    std::vector<RawTaps> outputs;
    outputs.reserve(outSize);
    size_t widest = 1;
    for (uint32_t i = 0; i < outSize; ++i) {
        outputs.push_back(OutputTaps(i, ratio, table.stretch, spec));
        widest = std::max(widest, outputs.back().weights.size());
    }
    table.taps = static_cast<uint32_t>((widest + 3) / 4 * 4);
    table.first.resize(outSize);
    table.lobeBegin.resize(outSize);
    table.lobeEnd.resize(outSize);
    table.weights.assign(static_cast<size_t>(outSize) * table.taps, 0.0f);
    for (uint32_t i = 0; i < outSize; ++i) {
        const RawTaps& taps = outputs[i];
        table.first[i] = taps.first;
        table.lobeBegin[i] = taps.lobeBegin;
        table.lobeEnd[i] = taps.lobeEnd;
        for (size_t k = 0; k < taps.weights.size(); ++k)
            table.weights[static_cast<size_t>(i) * table.taps + k] = static_cast<float>(taps.weights[k]);
    }
    return table;
}

std::vector<float> PackAxisTable(const AxisTable& table) {
    std::vector<float> packed;
    if (table.taps == 0)
        return packed;
    const uint32_t stride = PackedAxisStride(table);
    packed.assign(static_cast<size_t>(table.outSize) * stride * 4u, 0.0f);
    for (uint32_t i = 0; i < table.outSize; ++i) {
        float* header = &packed[static_cast<size_t>(i) * stride * 4u];
        header[0] = static_cast<float>(table.first[i]);
        header[1] = static_cast<float>(table.lobeBegin[i]);
        header[2] = static_cast<float>(table.lobeEnd[i]);
        std::copy_n(&table.weights[static_cast<size_t>(i) * table.taps], table.taps, header + 4);
    }
    return packed;
}

ScalePlan PlanScale(uint32_t inWidth, uint32_t inHeight, uint32_t outWidth, uint32_t outHeight,
                    const KernelSpec& spec, uint32_t sourceBits, uint32_t outputBits) {
    ScalePlan plan;
    plan.scaleX = inWidth != outWidth;
    plan.scaleY = inHeight != outHeight;
    plan.precision = ScaleIntermediatePrecision(sourceBits, outputBits);
    if (plan.scaleX)
        plan.horizontal = BuildAxisTable(inWidth, outWidth, spec);
    if (plan.scaleY)
        plan.vertical = BuildAxisTable(inHeight, outHeight, spec);

    const uint64_t outputs = static_cast<uint64_t>(outWidth) * outHeight;
    if (plan.scaleX && plan.scaleY) {
        const uint64_t horizontalFirst =
            PassTaps(static_cast<uint64_t>(outWidth) * inHeight, plan.horizontal) + PassTaps(outputs, plan.vertical);
        const uint64_t verticalFirst =
            PassTaps(static_cast<uint64_t>(inWidth) * outHeight, plan.vertical) + PassTaps(outputs, plan.horizontal);
        plan.order = verticalFirst < horizontalFirst ? PassOrder::kVerticalFirst : PassOrder::kHorizontalFirst;
        plan.intermediateWidth = plan.order == PassOrder::kHorizontalFirst ? outWidth : inWidth;
        plan.intermediateHeight = plan.order == PassOrder::kHorizontalFirst ? inHeight : outHeight;
        plan.estimatedTaps = std::min(horizontalFirst, verticalFirst);
    } else if (plan.scaleX) {
        plan.estimatedTaps = PassTaps(outputs, plan.horizontal);
    } else if (plan.scaleY) {
        plan.estimatedTaps = PassTaps(outputs, plan.vertical);
    }
    return plan;
}

}  // namespace ce::video_scale
