#include "SceneCutDetector.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace {
float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

float HistogramQuantile(const std::array<unsigned,64>& hist, std::size_t count, float q) {
    if (!count) return 0.0f;
    const std::size_t target = std::min(count - 1, std::size_t(std::floor(q * double(count - 1))));
    std::size_t acc = 0;
    for (std::size_t i = 0; i < hist.size(); ++i) {
        acc += hist[i];
        if (acc > target) return (float(i) + 0.5f) / float(hist.size());
    }
    return 1.0f;
}
}

SceneCutMetrics DetectHardSceneCut(const std::vector<float>& currentLuma,
                                   const std::vector<float>& previousLuma,
                                   const std::vector<float>& motionCompensatedMismatch) {
    SceneCutMetrics out{};
    const std::size_t n = currentLuma.size();
    if (!n || previousLuma.size() != n || motionCompensatedMismatch.size() != n) return out;

    std::array<unsigned,64> warpedHist{};
    double warpedSum = 0.0;
    double directSum = 0.0;
    std::size_t warpedStrong = 0;
    std::size_t directStrong = 0;

    // Conservative thresholds: film grain and compression noise can raise residuals,
    // but a hard cut normally breaks correspondence over most of the picture at once.
    constexpr float kWarpedStrong = 0.20f;
    constexpr float kDirectStrong = 0.20f;

    for (std::size_t i = 0; i < n; ++i) {
        const float warped = Clamp01(std::isfinite(motionCompensatedMismatch[i]) ? motionCompensatedMismatch[i] : 1.0f);
        const float direct = Clamp01(std::abs(currentLuma[i] - previousLuma[i]));
        warpedSum += warped;
        directSum += direct;
        if (warped >= kWarpedStrong) ++warpedStrong;
        if (direct >= kDirectStrong) ++directStrong;
        const std::size_t bin = std::min<std::size_t>(warpedHist.size() - 1, std::size_t(warped * float(warpedHist.size())));
        ++warpedHist[bin];
    }

    out.warpedResidualMean = float(warpedSum / double(n));
    out.directDifferenceMean = float(directSum / double(n));
    out.warpedStrongFraction = float(double(warpedStrong) / double(n));
    out.directStrongFraction = float(double(directStrong) / double(n));
    out.warpedResidualMedian = HistogramQuantile(warpedHist, n, 0.50f);

    // Broad failure requires both failed motion-compensated correspondence and a
    // large direct visual change. This rejects camera pans when NVOFA tracks them.
    const bool broadFailure =
        out.warpedResidualMean >= 0.14f &&
        out.warpedStrongFraction >= 0.60f &&
        out.directStrongFraction >= 0.55f;

    // Severe correspondence failure can stand on its own. This catches cuts where
    // direct luminance distributions happen to be similar between the two shots.
    const bool severeFailure =
        out.warpedResidualMean >= 0.19f &&
        out.warpedResidualMedian >= 0.18f &&
        out.warpedStrongFraction >= 0.70f;

    out.hardCut = broadFailure || severeFailure;
    return out;
}
