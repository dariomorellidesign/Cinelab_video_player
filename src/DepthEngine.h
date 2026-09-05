#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct DepthInferenceResult {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> depth;

    double preprocessMs = 0.0;
    double inferenceMs = 0.0;
    double postprocessMs = 0.0;
    double totalMs = 0.0;

    float minValue = 0.0f;
    float maxValue = 0.0f;
    float percentile02 = 0.0f;
    float percentile98 = 0.0f;
};

class DepthEngine {
public:
    DepthEngine();
    ~DepthEngine();

    DepthEngine(const DepthEngine&) = delete;
    DepthEngine& operator=(const DepthEngine&) = delete;

    bool Initialize(const std::wstring& modelPath, int dmlDeviceId = 0);
    bool InferBGRA(const uint8_t* bgra,
                   uint32_t width,
                   uint32_t height,
                   size_t strideBytes,
                   DepthInferenceResult& result);

    bool IsReady() const;
    const std::string& LastError() const;
    const std::string& ModelSummary() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
