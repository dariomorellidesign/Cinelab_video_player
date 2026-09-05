#pragma once
#include <cstdint>
#include <vector>
#include "SoftTemporalMask.h"
#include <utility>

struct ExternalMotionField {
    // Interleaved X,Y in SOURCE PIXELS, current frame -> previous frame.
    const float* motionXY = nullptr;
    uint32_t gridW = 0;
    uint32_t gridH = 0;
    uint32_t sourceW = 0;
    uint32_t sourceH = 0;
    bool valid = false;
};

struct ExternalDepthField {
    // Step 04E-1: normalized AI relative-nearness map used ONLY to guide Temporal Mask
    // structure/softening. Its polarity is irrelevant because the mask consumes spatial
    // discontinuities. The legacy Guide B / NGX depth path is not changed by this struct.
    const float* depth01 = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    float ageMs = -1.0f;
    float ageFrames = -1.0f;
    bool valid = false;
};

struct GuideFrame {
    // Compact/high-quality guide grid consumed by the GPU expansion pass:
    // R = motion X, G = motion Y (current -> previous, in DLSS input pixels)
    // B = depth proxy [0,1], A = BiasCurrentColor/disocclusion mask [0,1].
    std::vector<float> guideGridRGBA32F;
    uint32_t gridW = 0;
    uint32_t gridH = 0;
    bool hasHistory = false;
    float globalMotionX = 0.0f;
    float globalMotionY = 0.0f;
    float globalMatchCost = 0.0f;
    bool usedExternalMotion = false;
    bool hardCut = false;
    bool usedHardwareFastPath = false;
    bool legacyFlowEvaluated = false;
    bool sceneCutDetected = false;
    float sceneCutResidualMean = 0.0f;
    float sceneCutResidualMedian = 0.0f;
    float sceneCutStrongFraction = 0.0f;
    float sceneCutDirectStrongFraction = 0.0f;
    bool maskUsedAIDepth = false;
    bool maskAIDepthAvailable = false;
    float maskDepthAgeMs = -1.0f;
    float maskDepthAgeFrames = -1.0f;
};

class TemporalGuideGenerator {
public:
    enum class DepthMode { Flat, Estimated };

    static std::pair<uint32_t,uint32_t> AnalysisGrid(uint32_t sourceW, uint32_t sourceH, double targetFps = 30.0);

    void Reset();
    void SetDepthMode(DepthMode mode) { m_depthMode = mode; }
    DepthMode GetDepthMode() const { return m_depthMode; }
    void SetOutputGrid(uint32_t w, uint32_t h) { m_outputGridW = w; m_outputGridH = h; }

    bool Generate(const uint8_t* bgra, uint32_t sourceW, uint32_t sourceH,
                  uint32_t renderW, uint32_t renderH, double targetFps, bool reset,
                  GuideFrame& out, const ExternalMotionField* externalMotion = nullptr,
                  const ExternalDepthField* externalMaskDepth = nullptr);

private:
    static float Luma(const uint8_t* p);
    void DownsampleLuma(const uint8_t* bgra, uint32_t w, uint32_t h,
                        uint32_t gw, uint32_t gh, std::vector<float>& out) const;
    void EstimateFlow(const std::vector<float>& cur, const std::vector<float>& prev,
                      uint32_t gw, uint32_t gh,
                      std::vector<float>& flowX, std::vector<float>& flowY,
                      std::vector<float>& mismatch,
                      float& globalX, float& globalY, float& globalCost) const;
    void MedianFlow(std::vector<float>& x, std::vector<float>& y,
                    uint32_t gw, uint32_t gh) const;
    void BuildDepthProxy(const std::vector<float>& luma,
                         const std::vector<float>& flowX, const std::vector<float>& flowY,
                         uint32_t gw, uint32_t gh,
                         std::vector<float>& depth);

    std::vector<float> m_prevLuma;
    std::vector<float> m_prevDepth;
    SoftTemporalMaskProcessor m_softMask;
    uint32_t m_gridW = 0, m_gridH = 0;
    uint32_t m_outputGridW = 0, m_outputGridH = 0;
    bool m_havePrev = false;
    DepthMode m_depthMode = DepthMode::Estimated;
};
