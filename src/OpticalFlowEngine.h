#pragma once

#include <d3d12.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct OpticalFlowFrame {
    // Interleaved X,Y motion in SOURCE PIXELS, current frame -> previous frame.
    std::vector<float> motionXY;
    uint32_t gridW = 0;
    uint32_t gridH = 0;
    uint32_t gridSize = 0;
    uint32_t sourceW = 0;
    uint32_t sourceH = 0;
    bool valid = false;
};

// Borrowed resources owned by OpticalFlowEngine. Renderer must signal completion before reuse.
struct GpuOpticalFlowFrame {
    // Both NVOF inputs and its compact output remain owned by OpticalFlowEngine.
    // The renderer only borrows them until it signals the supplied completion fence.
    ID3D12Resource* color = nullptr;          // current input frame
    ID3D12Resource* previousColor = nullptr;  // previous input frame, paired with motion
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* cost = nullptr;           // NVOF R8_UINT: larger value means less confidence
    ID3D12Fence* readyFence = nullptr;
    uint64_t readyValue = 0;
    uint32_t gridW=0,gridH=0,sourceW=0,sourceH=0;
    bool valid=false;
};

struct OpticalFlowStats {
    uint64_t calls = 0;
    uint64_t pairs = 0;
    double lastPreprocessMs = 0.0; // STEP 06A1 CPU-only private NVOF input transform
    double lastUploadMs = 0.0;
    double lastExecuteMs = 0.0;
    double lastDownloadMs = 0.0;
    double lastConvertMs = 0.0;
    double lastStabilizeMs = 0.0;
    double lastTotalMs = 0.0;
    double emaPreprocessMs = 0.0;
    double emaUploadMs = 0.0;
    double emaExecuteMs = 0.0;
    double emaDownloadMs = 0.0;
    double emaConvertMs = 0.0;
    double emaStabilizeMs = 0.0;
    double emaTotalMs = 0.0;
    bool filmStabilizationEnabled = false;
    bool filmPerformanceBypass = false;
    float filmNoisePx = 0.0f;
    float filmCorrectedPct = 0.0f;
    float filmSnappedPct = 0.0f;
    float filmMeanCorrectionPx = 0.0f;
    float filmMaxCorrectionPx = 0.0f;
    float filmCenterMotionX = 0.0f;
    float filmCenterMotionY = 0.0f;
};

class OpticalFlowEngine {
public:
    OpticalFlowEngine();
    ~OpticalFlowEngine();

    OpticalFlowEngine(const OpticalFlowEngine&) = delete;
    OpticalFlowEngine& operator=(const OpticalFlowEngine&) = delete;

    static bool RuntimeAvailable();

    bool Initialize(ID3D12Device* device, uint32_t width,
                    uint32_t height, uint32_t preferredGridSize = 2);
    void Shutdown();
    void Reset();

    bool Generate(const uint8_t* bgra, size_t bytes, bool reset,
                  OpticalFlowFrame& out);

    bool GenerateGpu(const uint8_t* bgra, size_t bytes, bool reset, ID3D12Fence* consumedFence, uint64_t consumedValue, GpuOpticalFlowFrame& out);

    bool Available() const;
    uint32_t GridSize() const;
    uint32_t GridW() const;
    uint32_t GridH() const;
    const char* PerfName() const;
    bool FilmStabilizationEnabled() const;
    OpticalFlowStats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
