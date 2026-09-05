#include "SplitScreenLayout.h"
#include <algorithm>
#include <cmath>

SplitScreenLayout ComputeSplitScreenLayout(uint32_t outputWidth, float fraction) {
    SplitScreenLayout out{};
    if (outputWidth < 2) return out;
    const float f = std::clamp(std::isfinite(fraction) ? fraction : 0.5f, 0.05f, 0.95f);
    uint32_t x = uint32_t(std::lround(double(outputWidth) * double(f)));
    x = std::clamp<uint32_t>(x, 1u, outputWidth - 1u);
    out.splitX = x;
    return out;
}
