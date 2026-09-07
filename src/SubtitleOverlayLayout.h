#pragma once
#include <algorithm>
#include <cstdint>

struct SubtitleOverlayLayout {
    uint32_t textureW = 0;
    uint32_t textureH = 0;
    int32_t dstX = 0;
    int32_t dstY = 0;
    int32_t dstW = 0;
    int32_t dstH = 0;
    int32_t textMarginX = 0;
    int32_t textBottomMargin = 0;
    int32_t fontPixels = 0;
    int32_t outlinePixels = 0;
};

inline SubtitleOverlayLayout ComputeSubtitleOverlayLayout(uint32_t outputW, uint32_t outputH) {
    SubtitleOverlayLayout o{};
    outputW = std::max(1u, outputW);
    outputH = std::max(1u, outputH);

    uint32_t h = (outputH * 7u + 12u) / 25u; // ~28% of image height
    h = std::max(96u, h);
    h = std::min(h, 720u);
    h = std::min(h, outputH);

    o.textureW = outputW;
    o.textureH = h;
    o.dstX = 0;
    o.dstY = static_cast<int32_t>(outputH - h);
    o.dstW = static_cast<int32_t>(outputW);
    o.dstH = static_cast<int32_t>(h);
    o.textMarginX = std::max(20, static_cast<int>(outputW / 18u));
    o.textBottomMargin = std::max(16, static_cast<int>(h / 10u));
    o.fontPixels = std::clamp(static_cast<int>(h / 5u), 24, 96);
    o.outlinePixels = std::clamp(o.fontPixels / 18, 2, 6);
    return o;
}
