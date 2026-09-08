#pragma once
#include <cstdint>
#include <vector>

struct SoftTemporalMaskStats {
    float mean = 0.0f;
    float maxValue = 0.0f;
    float activeFraction = 0.0f;
};

class SoftTemporalMaskProcessor {
public:
    void Reset();
    bool Build(const std::vector<float>& luma,
               const std::vector<float>& flowX,
               const std::vector<float>& flowY,
               const std::vector<float>& mismatch,
               const std::vector<float>& depth,
               uint32_t width, uint32_t height,
               bool history,
               std::vector<float>& outMask, bool constantDepth = false);
    const SoftTemporalMaskStats& Stats() const { return m_stats; }

private:
    std::vector<float> m_previous;
    SoftTemporalMaskStats m_stats{};
};
