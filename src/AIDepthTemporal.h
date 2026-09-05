#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

struct AIDepthMeasurementView {
    const float* rawDepth = nullptr;
    size_t count = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    int64_t timestamp100ns = 0;
    uint64_t sequence = 0;
    float percentile02 = 0.0f;
    float percentile98 = 0.0f;
};

struct AIDepthMotionView {
    // Interleaved X,Y current -> previous in SOURCE PIXELS.
    const float* motionXY = nullptr;
    size_t countFloats = 0;
    uint32_t gridW = 0;
    uint32_t gridH = 0;
    uint32_t sourceW = 0;
    uint32_t sourceH = 0;
    bool valid = false;
};

struct AIDepthTemporalFrame {
    bool valid = false;
    bool usedMeasurement = false;
    bool measurementReprojected = false;
    bool predictionReprojected = false;
    bool measurementRejected = false;
    uint32_t measurementReprojectionSteps = 0;
    uint32_t predictionReprojectionSteps = 0;
    int64_t currentTimestamp100ns = 0;
    int64_t measurementTimestamp100ns = 0;
    uint64_t measurementSequence = 0;
    double measurementAgeMs = 0.0;
    float affineScale = 1.0f;
    float affineShift = 0.0f;
    float historyCoverage = 0.0f;
    float measurementCoverage = 0.0f;
    float normalizedLo = 0.0f;
    float normalizedHi = 1.0f;
    // Robust stabilized relative nearness used by the AI Depth debug texture:
    // 0 = robust far anchor, 1 = robust near anchor. Depth Anything V2 relative
    // output is disparity/inverse-depth-like, so larger stabilized raw values are near.
    std::vector<float> preview01;
};

class AIDepthTemporalStabilizer {
public:
    static constexpr uint32_t DepthW = 518;
    static constexpr uint32_t DepthH = 518;

    // Step 04C synthetic hardware depth convention. This is intentionally a
    // relative mapping, not metric camera distance: conventional D3D depth is
    // 0 = near and 1 = far, the opposite polarity of preview01 nearness.
    static float SyntheticHardwareDepthFromRelative01(float relativeNearness01);

    void Reset();
    const std::vector<float>& StabilizedRaw() const { return m_state.values; }
    const std::vector<uint8_t>& StabilizedValidMask() const { return m_state.valid; }
    int64_t StabilizedTimestamp100ns() const { return m_state.timestamp100ns; }

    // Processes one PRESENTED video frame. currentToPrevious describes the current
    // frame against previousRenderedTimestamp100ns. New AI measurements may arrive
    // late; the stabilizer walks retained NVOFA steps forward until they are aligned
    // to currentTimestamp100ns before blending them into the temporal state.
    bool ProcessFrame(int64_t currentTimestamp100ns,
                      int64_t previousRenderedTimestamp100ns,
                      uint32_t sourceW,
                      uint32_t sourceH,
                      const AIDepthMotionView* currentToPrevious,
                      const AIDepthMeasurementView* newMeasurement,
                      AIDepthTemporalFrame& out);

private:
    struct FlowStep {
        int64_t previousTimestamp100ns = 0;
        int64_t currentTimestamp100ns = 0;
        std::vector<float> motionDepthXY;
        std::vector<float> confidence;
    };

    struct Map {
        std::vector<float> values;
        std::vector<uint8_t> valid;
        std::vector<float> confidence;
        int64_t timestamp100ns = 0;
    };

    static constexpr size_t PixelCount = size_t(DepthW) * DepthH;
    static constexpr size_t MaxFlowHistory = 16;

    bool AppendFlowStep(int64_t previousTimestamp100ns,
                        int64_t currentTimestamp100ns,
                        uint32_t sourceW,
                        uint32_t sourceH,
                        const AIDepthMotionView& motion);
    bool WarpOne(const Map& src, const FlowStep& step, Map& dst) const;
    bool WarpAcrossHistory(const Map& src, int64_t targetTimestamp100ns,
                           Map& dst, uint32_t& steps) const;
    bool MeasurementToMap(const AIDepthMeasurementView& measurement, Map& out) const;
    bool RobustAffine(const Map& measurement, const Map& prediction,
                      float& scale, float& shift) const;
    void Blend(const Map& prediction, const Map& measurement,
               float scale, float shift, Map& out) const;
    void BuildPreview(const Map& state, AIDepthTemporalFrame& out);
    static bool ComputePercentiles(const Map& map, float lowFraction, float highFraction,
                                   float& low, float& high);
    static float Coverage(const Map& map);

    std::deque<FlowStep> m_flowHistory;
    Map m_state;
    uint32_t m_sourceW = 0;
    uint32_t m_sourceH = 0;
    bool m_normInitialized = false;
    float m_normLo = 0.0f;
    float m_normHi = 1.0f;
};
