#pragma once
#include <cstdint>
#include <vector>

// Resample an arbitrary normalized depth/nearness field into the small
// TemporalGuideGenerator analysis grid. Polarity is intentionally irrelevant:
// the mask consumes spatial depth discontinuities, not absolute near/far meaning.
bool ResampleMaskDepthGuide01(const float* src,
                              uint32_t srcW, uint32_t srcH,
                              uint32_t dstW, uint32_t dstH,
                              std::vector<float>& dst);
