#include "OpticalFlowEngine.h"

#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <sstream>
#include <utility>

#include "NvOFD3D12.h"
#include "NvOFD3DCommon.h"
#include "Log.h"

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<ID3D12Resource> AllocateTexture(ID3D12Device* device,
                                       const NV_OF_BUFFER_DESCRIPTOR& desc) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = desc.width;
    resource.Height = desc.height;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    resource.Format = NvOFBufferFormatToDxgiFormat(desc.bufferFormat);
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> result;
    D3D_API_CALL(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &resource,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&result)));
    return result;
}

void CreateFencePoint(ID3D12Device* device, NV_OF_FENCE_POINT& point) {
    point = {};
    D3D_API_CALL(device->CreateFence(
        0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&point.fence)));
    point.value = 0;
}

void ReleaseFencePoint(NV_OF_FENCE_POINT& point) {
    if (point.fence) {
        point.fence->Release();
        point.fence = nullptr;
    }
    point.value = 0;
}

} // namespace

struct OpticalFlowEngine::Impl {
    ID3D12Device* device = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t gridSize = 0;
    uint32_t gridW = 0;
    uint32_t gridH = 0;

    NvOFObj flow;
    std::vector<ComPtr<ID3D12Resource>> inputResources;
    ComPtr<ID3D12Resource> outputResource;
    std::vector<NvOFBufferObj> inputBuffers;
    NvOFBufferObj outputBuffer;

    NV_OF_FENCE_POINT appFence{};
    NV_OF_FENCE_POINT ofaFence{};

    std::vector<NV_OF_FLOW_VECTOR> hostFlow;
    uint32_t previousIndex = 0;
    bool havePrevious = false;
    bool disableHintsOnNextPair = true;

    ~Impl() {
        Shutdown();
    }

    void Shutdown() {
        // Buffers must unregister while the NvOF session/fences are still alive.
        outputBuffer.reset();
        inputBuffers.clear();
        outputResource.Reset();
        inputResources.clear();
        flow.reset();
        ReleaseFencePoint(appFence);
        ReleaseFencePoint(ofaFence);
        hostFlow.clear();
        havePrevious = false;
        device = nullptr;
        width = height = gridSize = gridW = gridH = 0;
    }

    void ResetHistory() {
        havePrevious = false;
        previousIndex = 0;
        disableHintsOnNextPair = true;
    }

    bool Initialize(ID3D12Device* newDevice, uint32_t w, uint32_t h,
                    uint32_t preferredGrid) {
        Shutdown();
        if (!newDevice || !w || !h) return false;

        device = newDevice;
        width = w;
        height = h;

        flow = NvOFD3D12::Create(device, width, height,
                                 NV_OF_BUFFER_FORMAT_ABGR8,
                                 NV_OF_MODE_OPTICALFLOW,
                                 NV_OF_PERF_LEVEL_SLOW);
        if (!flow) return false;

        uint32_t selectedGrid = std::max(1u, preferredGrid);
        if (!flow->CheckGridSize(selectedGrid)) {
            uint32_t nextGrid = 0;
            if (!flow->GetNextMinGridSize(selectedGrid, nextGrid)) {
                LOG("[NVOF] No supported output grid >= " << selectedGrid);
                Shutdown();
                return false;
            }
            selectedGrid = nextGrid;
        }

        gridSize = selectedGrid;
        gridW = (width + gridSize - 1u) / gridSize;
        gridH = (height + gridSize - 1u) / gridSize;
        flow->Init(gridSize);

        CreateFencePoint(device, appFence);
        CreateFencePoint(device, ofaFence);

        NV_OF_BUFFER_DESCRIPTOR inputDesc{};
        inputDesc.width = width;
        inputDesc.height = height;
        inputDesc.bufferUsage = NV_OF_BUFFER_USAGE_INPUT;
        inputDesc.bufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;

        inputResources.resize(2);
        inputBuffers.reserve(2);
        for (uint32_t i = 0; i < 2; ++i) {
            inputResources[i] = AllocateTexture(device, inputDesc);
            ++ofaFence.value;
            inputBuffers.emplace_back(flow->RegisterPreAllocBuffers(
                inputDesc, inputResources[i].Get(), &appFence, &ofaFence));
        }

        NV_OF_BUFFER_DESCRIPTOR outputDesc{};
        outputDesc.width = gridW;
        outputDesc.height = gridH;
        outputDesc.bufferUsage = NV_OF_BUFFER_USAGE_OUTPUT;
        outputDesc.bufferFormat = NV_OF_BUFFER_FORMAT_SHORT2;

        outputResource = AllocateTexture(device, outputDesc);
        ++ofaFence.value;
        outputBuffer = flow->RegisterPreAllocBuffers(
            outputDesc, outputResource.Get(), &appFence, &ofaFence);

        hostFlow.resize(size_t(gridW) * gridH);
        ResetHistory();

        LOG("[NVOF] Initialized D3D12: input=" << width << "x" << height
            << " requestedGrid=" << preferredGrid
            << " hwGrid=" << gridSize
            << " output=" << gridW << "x" << gridH
            << " perf=SLOW");
        return true;
    }

    bool Generate(const uint8_t* bgra, size_t bytes, bool reset,
                  OpticalFlowFrame& out) {
        out = {};
        if (!flow || !bgra || bytes < size_t(width) * height * 4u) return false;
        if (reset) ResetHistory();

        if (!havePrevious) {
            previousIndex = 0;
            ++appFence.value;
            inputBuffers[previousIndex]->UploadData(bgra, &ofaFence, &appFence);
            havePrevious = true;
            return true;
        }

        const uint32_t currentIndex = 1u - previousIndex;
        ++appFence.value;
        inputBuffers[currentIndex]->UploadData(bgra, &ofaFence, &appFence);

        ++ofaFence.value;
        flow->Execute(inputBuffers[currentIndex].get(),
                      inputBuffers[previousIndex].get(),
                      outputBuffer.get(),
                      nullptr, nullptr, 0, nullptr,
                      &appFence, 1, &ofaFence,
                      disableHintsOnNextPair ? NV_OF_TRUE : NV_OF_FALSE);

        outputBuffer->DownloadData(hostFlow.data(), &ofaFence);
        disableHintsOnNextPair = false;
        previousIndex = currentIndex;

        out.motionXY.resize(size_t(gridW) * gridH * 2u);
        for (size_t i = 0; i < hostFlow.size(); ++i) {
            // NVIDIA Optical Flow uses signed S10.5: divide by 32 for pixels.
            out.motionXY[i * 2u + 0u] = float(hostFlow[i].flowx) / 32.0f;
            out.motionXY[i * 2u + 1u] = float(hostFlow[i].flowy) / 32.0f;
        }
        out.gridW = gridW;
        out.gridH = gridH;
        out.gridSize = gridSize;
        out.sourceW = width;
        out.sourceH = height;
        out.valid = true;
        return true;
    }
};

OpticalFlowEngine::OpticalFlowEngine() = default;
OpticalFlowEngine::~OpticalFlowEngine() = default;

bool OpticalFlowEngine::RuntimeAvailable() {
#if defined(_WIN64)
    HMODULE module = LoadLibraryW(L"nvofapi64.dll");
#else
    HMODULE module = LoadLibraryW(L"nvofapi.dll");
#endif
    if (!module) return false;
    FreeLibrary(module);
    return true;
}

bool OpticalFlowEngine::Initialize(ID3D12Device* device, uint32_t width,
                                   uint32_t height, uint32_t preferredGridSize) {
    try {
        auto impl = std::make_unique<Impl>();
        if (!impl->Initialize(device, width, height, preferredGridSize)) return false;
        m_impl = std::move(impl);
        return true;
    } catch (const NvOFException& e) {
        LOG("[NVOF] Initialize failed: code=" << e.getErrorCode() << " " << e.what());
    } catch (const std::exception& e) {
        LOG("[NVOF] Initialize failed: " << e.what());
    } catch (...) {
        LOG("[NVOF] Initialize failed: unknown exception");
    }
    m_impl.reset();
    return false;
}

void OpticalFlowEngine::Shutdown() {
    m_impl.reset();
}

void OpticalFlowEngine::Reset() {
    if (m_impl) m_impl->ResetHistory();
}

bool OpticalFlowEngine::Generate(const uint8_t* bgra, size_t bytes, bool reset,
                                 OpticalFlowFrame& out) {
    if (!m_impl) {
        out = {};
        return false;
    }
    try {
        return m_impl->Generate(bgra, bytes, reset, out);
    } catch (const NvOFException& e) {
        LOG("[NVOF] Generate failed: code=" << e.getErrorCode() << " " << e.what());
    } catch (const std::exception& e) {
        LOG("[NVOF] Generate failed: " << e.what());
    } catch (...) {
        LOG("[NVOF] Generate failed: unknown exception");
    }
    out = {};
    // Disable NVOF after a runtime failure to avoid repeating the same exception every frame.
    m_impl.reset();
    return false;
}

bool OpticalFlowEngine::Available() const { return m_impl != nullptr; }
uint32_t OpticalFlowEngine::GridSize() const { return m_impl ? m_impl->gridSize : 0; }
uint32_t OpticalFlowEngine::GridW() const { return m_impl ? m_impl->gridW : 0; }
uint32_t OpticalFlowEngine::GridH() const { return m_impl ? m_impl->gridH : 0; }
