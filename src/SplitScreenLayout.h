#pragma once
#include <cstdint>

struct SplitScreenLayout {
    uint32_t splitX = 0;
};

// Computes the pixel boundary used by the presentation scissor rectangles.
// The viewport intentionally remains full-output-size so both halves preserve
// identical spatial coordinates/UV mapping rather than squeezing two images.
SplitScreenLayout ComputeSplitScreenLayout(uint32_t outputWidth, float fraction = 0.5f);
