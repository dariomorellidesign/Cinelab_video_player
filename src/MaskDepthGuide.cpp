#include "MaskDepthGuide.h"
#include <algorithm>
#include <cmath>

namespace {
float Clean01(float v, bool& finite) {
    finite = std::isfinite(v);
    return finite ? std::clamp(v, 0.0f, 1.0f) : 0.5f;
}
}

bool ResampleMaskDepthGuide01(const float* src,
                              uint32_t srcW, uint32_t srcH,
                              uint32_t dstW, uint32_t dstH,
                              std::vector<float>& dst) {
    if (!src || !srcW || !srcH || !dstW || !dstH) {
        dst.clear();
        return false;
    }

    dst.assign(size_t(dstW) * dstH, 0.5f);
    uint64_t finiteSamples = 0;
    uint64_t totalSamples = 0;
    for (uint32_t y = 0; y < dstH; ++y) {
        float sy = (float(y) + 0.5f) * float(srcH) / float(dstH) - 0.5f;
        sy = std::clamp(sy, 0.0f, float(srcH - 1));
        const uint32_t y0 = uint32_t(std::floor(sy));
        const uint32_t y1 = std::min(srcH - 1, y0 + 1);
        const float ty = sy - float(y0);
        for (uint32_t x = 0; x < dstW; ++x) {
            float sx = (float(x) + 0.5f) * float(srcW) / float(dstW) - 0.5f;
            sx = std::clamp(sx, 0.0f, float(srcW - 1));
            const uint32_t x0 = uint32_t(std::floor(sx));
            const uint32_t x1 = std::min(srcW - 1, x0 + 1);
            const float tx = sx - float(x0);

            bool fa=false, fb=false, fc=false, fd=false;
            const float a = Clean01(src[size_t(y0) * srcW + x0], fa);
            const float b = Clean01(src[size_t(y0) * srcW + x1], fb);
            const float c = Clean01(src[size_t(y1) * srcW + x0], fc);
            const float d = Clean01(src[size_t(y1) * srcW + x1], fd);
            finiteSamples += uint64_t(fa) + uint64_t(fb) + uint64_t(fc) + uint64_t(fd);
            totalSamples += 4;

            const float top = a + (b - a) * tx;
            const float bottom = c + (d - c) * tx;
            dst[size_t(y) * dstW + x] = std::clamp(top + (bottom - top) * ty, 0.0f, 1.0f);
        }
    }

    // A mostly-invalid AI map should never steer the mask. Normal Depth Anything
    // output is fully finite, so this is only a defensive fallback.
    if (!totalSamples || double(finiteSamples) / double(totalSamples) < 0.90) {
        dst.clear();
        return false;
    }
    return true;
}
