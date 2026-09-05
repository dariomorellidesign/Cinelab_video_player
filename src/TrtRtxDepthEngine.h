#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct TrtDepthInferenceResult {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> depth;

    double preprocessMs = 0.0;
    double uploadMs = 0.0;
    double inferenceMs = 0.0;
    double downloadMs = 0.0;
    double postprocessMs = 0.0;
    double totalMs = 0.0;

    float minValue = 0.0f;
    float maxValue = 0.0f;
    float percentile02 = 0.0f;
    float percentile98 = 0.0f;
};

class TrtRtxDepthEngine {
public:
    TrtRtxDepthEngine();
    ~TrtRtxDepthEngine();

    TrtRtxDepthEngine(const TrtRtxDepthEngine&) = delete;
    TrtRtxDepthEngine& operator=(const TrtRtxDepthEngine&) = delete;

    bool Initialize(const std::wstring& enginePath, const std::wstring& runtimeCachePath);
    bool InferBGRA(const uint8_t* bgra,
                   uint32_t width,
                   uint32_t height,
                   size_t strideBytes,
                   TrtDepthInferenceResult& result);

    bool IsReady() const;
    const std::string& LastError() const;
    const std::string& ModelSummary() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
