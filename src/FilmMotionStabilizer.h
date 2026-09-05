#pragma once

#include <cstdint>
#include <vector>

struct FilmMotionStabilizationStats {
    bool enabled = false;
    bool modelValid = false;
    bool usedHistory = false;
    bool performanceBypass = false;
    float robustNoisePx = 0.0f;
    float correctedPct = 0.0f;
    float snappedPct = 0.0f;
    float meanCorrectionPx = 0.0f;
    float maxCorrectionPx = 0.0f;
    float centerMotionX = 0.0f;
    float centerMotionY = 0.0f;
};

class FilmMotionStabilizer {
public:
    void Reset();

    // motionXY is interleaved current->previous motion in source pixels.
    // The filter models coherent background/camera motion as a robust affine field,
    // then suppresses only small, spatially incoherent residual motion. This targets
    // film grain / sensor noise without low-pass filtering the visible video itself.
    bool Process(std::vector<float>& motionXY,
                 uint32_t gridW,
                 uint32_t gridH,
                 bool enabled,
                 FilmMotionStabilizationStats* stats = nullptr);

private:
    std::vector<float> m_prevResidualXY;
    uint32_t m_prevW = 0;
    uint32_t m_prevH = 0;
};
