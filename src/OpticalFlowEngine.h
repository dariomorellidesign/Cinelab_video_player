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

class OpticalFlowEngine {
public:
    OpticalFlowEngine();
    ~OpticalFlowEngine();

    OpticalFlowEngine(const OpticalFlowEngine&) = delete;
    OpticalFlowEngine& operator=(const OpticalFlowEngine&) = delete;

    static bool RuntimeAvailable();

    bool Initialize(ID3D12Device* device, uint32_t width, uint32_t height,
                    uint32_t preferredGridSize = 2);
    void Shutdown();
    void Reset();

    bool Generate(const uint8_t* bgra, size_t bytes, bool reset,
                  OpticalFlowFrame& out);

    bool Available() const;
    uint32_t GridSize() const;
    uint32_t GridW() const;
    uint32_t GridH() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
