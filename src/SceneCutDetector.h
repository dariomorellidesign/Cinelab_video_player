#pragma once
#include <vector>

struct SceneCutMetrics {
    float warpedResidualMean = 0.0f;
    float warpedResidualMedian = 0.0f;
    float warpedStrongFraction = 0.0f;
    float directDifferenceMean = 0.0f;
    float directStrongFraction = 0.0f;
    bool hardCut = false;
};

// Detect an abrupt scene cut using the already motion-compensated residual produced
// by the current->previous flow plus a direct frame-difference sanity check.
// This intentionally targets hard/drastic cuts, not fades or dissolves.
SceneCutMetrics DetectHardSceneCut(const std::vector<float>& currentLuma,
                                   const std::vector<float>& previousLuma,
                                   const std::vector<float>& motionCompensatedMismatch);
