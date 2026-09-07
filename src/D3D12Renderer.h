#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>
#include <string>
#include <vector>
#include "DLSSBackend.h"

class D3D12Renderer {
public:
    enum class DebugView { Final, Input, MotionVectors, Depth, BiasMask, AIDepth, AIHardwareDepth };
    enum class DepthSource { Legacy, Flat, AISynthetic };

    void SetDepthSource(DepthSource source) { m_depthSource = source; }
    DepthSource GetDepthSource() const { return m_depthSource; }
    DepthSource GetEffectiveDepthSource() const { return m_effectiveDepthSource; }

    struct ColorSettings {
        float brightness = 0.0f;   // exposure-like brightness, in stops (-2..+2)
        float contrast = 1.0f;     // 0..3
        float saturation = 1.0f;   // 0..3
        float gamma = 1.0f;        // 0.25..3
        float temperature = 0.0f;  // -1..+1 (cool..warm)
        float tint = 0.0f;         // -1..+1 (green..magenta)
    };

    ~D3D12Renderer();
    bool Initialize(HWND hwnd, uint32_t sourceW, uint32_t sourceH,
                    uint32_t outputW, uint32_t outputH,
                    uint32_t gridW, uint32_t gridH,
                    NVSDK_NGX_PerfQuality_Value quality);
    bool RenderFrame(const uint8_t* bgra, size_t bytes,
                     const float* guideGridRGBA32F, size_t guideBytes,
                     uint32_t gridW, uint32_t gridH,
                     bool temporalReset, float frameTimeMs,
                     const float* aiDepthPreview01, size_t aiDepthBytes,
                     uint32_t aiDepthW, uint32_t aiDepthH);

    void SetDLSS(bool enabled) { m_dlssEnabled = enabled; }
    bool DLSSAvailable() const { return m_dlss.Available(); }
    bool DLSSEnabled() const { return m_dlssEnabled && m_dlss.Available(); }
    bool DLSSRequested() const { return m_dlssEnabled; }
    uint32_t DLSSInputW() const { return m_renderW; }
    uint32_t DLSSInputH() const { return m_renderH; }
    uint32_t OutputW() const { return m_outputW; }
    uint32_t OutputH() const { return m_outputH; }
    ID3D12Device* Device() const { return m_device.Get(); }
    void SetDebugView(DebugView v) { m_debugView = v; }
    DebugView GetDebugView() const { return m_debugView; }
    void SetSplitScreen(bool enabled) { m_splitScreen = enabled; }
    void SetVSync(bool enabled) { m_vsyncEnabled = enabled; }
    bool VSyncEnabled() const { return m_vsyncEnabled; }
    void SetFrameGeneration(bool enabled, uint32_t multiplier=2);
    bool FrameGenerationRequested() const { return m_frameGenerationEnabled; }
    uint32_t FrameGenerationMultiplier() const { return m_frameGenerationMultiplier; }
    bool FrameGenerationAvailable() const;
    uint32_t FrameGenerationMaxMultiplier() const;
    uint32_t FrameGenerationFramesActuallyPresented() const;
    uint64_t FrameGenerationDisplayedFramesTotal() const { return m_frameGenerationDisplayedFramesTotal; }
    void SetSplitFraction(float fraction) { m_splitFraction = fraction; }
    float SplitFraction() const { return m_splitFraction; }
    void SetSubtitleText(const std::wstring& text);
    bool SplitScreenEnabled() const { return m_splitScreen; }
    void ResetAIDepthDebug() { m_aiDepthClearPending = true; m_aiDepthValid = false; m_aiHardwareDepthClearPending = true; m_aiHardwareDepthValid = false; }
    void RequestDLSSRecreate() { m_recreateRequested = true; }
    uint64_t FramesPresented() const { return m_framesPresented; }
    bool DLSSFeatureCreated() const { return m_dlss.FeatureCreated(); }
    uint64_t DLSSEvaluations() const { return m_dlss.EvaluationCount(); }
    bool DLSSLastEvaluationUsedC() const { return m_dlss.LastEvaluationUsedC(); }
    NVSDK_NGX_Result DLSSLastResult() const { return m_dlss.LastResult(); }
    void WaitGPU();
    bool PresentCurrent();
    void SetColorSettings(const ColorSettings& settings) { m_colorSettings = settings; }
    const ColorSettings& GetColorSettings() const { return m_colorSettings; }

private:
    static constexpr uint32_t FrameCount = 3;
    static constexpr uint32_t SubtitleSrvIndex = 10;
    // STEP 04G-3 D3D12 subtitle compositor
    static constexpr uint32_t AIDepthW = 518;
    static constexpr uint32_t AIDepthH = 518;
    // NVIDIA's D3D12 DLSS contract expects input resources in NON_PIXEL_SHADER_RESOURCE
    // at EvaluateFeature time. Debug/presentation passes temporarily transition selected
    // resources to PIXEL_SHADER_RESOURCE and restore them before the frame ends.
    static constexpr D3D12_RESOURCE_STATES GuideReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    static constexpr D3D12_RESOURCE_STATES DepthGuideReadState =
        D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    void DrawSplitComparison(ID3D12GraphicsCommandList* cmd, bool dlssUsed);
    bool CreateSubtitleResources();
    bool CreateFrameGenerationResources();
    void CaptureFrameGenerationHudless(ID3D12GraphicsCommandList* cmd, uint32_t slot, uint32_t backbufferIndex);
    bool BuildSubtitleBitmap(const std::wstring& text);
    void DrawSubtitleOverlay(ID3D12GraphicsCommandList* cmd, uint32_t slot);
    bool CreateDeviceAndSwapchain(HWND hwnd);
    bool CreateHeapsAndBackbuffers();
    bool CreatePipelines();
    bool CreateVideoResources();
    bool InitializeDLSS();
    bool CreateUploadForTexture(const D3D12_RESOURCE_DESC& desc,
                                Microsoft::WRL::ComPtr<ID3D12Resource>& upload,
                                uint8_t*& mapped,
                                D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                uint32_t& rows,
                                uint64_t& rowBytes,
                                uint64_t& totalBytes,
                                const char* name);
    void CopyMappedRows(uint8_t* mapped, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
                        const void* src, size_t tightRowBytes, uint32_t rows);
    bool WaitForFrameSlot(uint32_t slot);
    void SignalFrameSlot(uint32_t slot);
    void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                 D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    D3D12_CPU_DESCRIPTOR_HANDLE RTV(uint32_t index) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DSV(uint32_t index=0) const;
    D3D12_CPU_DESCRIPTOR_HANDLE SRVCPU(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE SRVGPU(uint32_t index) const;
    static float Halton(uint32_t index, uint32_t base);

    HWND m_hwnd = nullptr;
    uint32_t m_sourceW=0,m_sourceH=0,m_outputW=0,m_outputH=0,m_renderW=0,m_renderH=0,m_gridW=0,m_gridH=0;
    NVSDK_NGX_PerfQuality_Value m_quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapchain;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocators[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmds[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    uint64_t m_frameFence[FrameCount]{};
    uint32_t m_frameSlot = 0;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
    uint32_t m_rtvInc=0,m_srvInc=0,m_dsvInc=0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backbuffers[FrameCount];

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoConvert;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoPresent;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoSubtitle;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoMotionDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoAIHardwareDepthDebug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoDepthWrite;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoAIHardwareDepthWrite;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoExpandGuides;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_decodedTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_upload[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dlssColor;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depth;      // R32_TYPELESS: D32 DSV + R32 SRV, same resource passed to NGX
    Microsoft::WRL::ComPtr<ID3D12Resource> m_motion;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_biasCurrent;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_dlssOutput;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_subtitleTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_subtitleUpload[FrameCount];
    Microsoft::WRL::ComPtr<ID3D12Resource> m_guideGrid;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_guideUpload[FrameCount];
    // Debug-only AI relative-depth texture. It remains separate from the NGX depth input.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_aiDepth;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_aiDepthUpload[FrameCount];
    // Step 04C: full DLSS-input-resolution synthetic hardware Z. Same typeless/D32/R32
    // resource pattern as NGX depth. Step 04D may select it as the live NGX depth input.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_aiHardwareDepth;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_flatDepth;

    uint8_t* m_uploadMapped[FrameCount]{};
    uint8_t* m_guideMapped[FrameCount]{};
    uint8_t* m_aiDepthMapped[FrameCount]{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_uploadFootprint{};
    // STEP 05B: one immutable-at-present HUD-less copy per frame slot.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fgHudless[FrameCount];
    D3D12_RESOURCE_STATES m_fgHudlessState[FrameCount]{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fgUiAlpha;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fgUiAlphaUpload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_fgUiAlphaFootprint{};
    D3D12_RESOURCE_STATES m_fgUiAlphaState = D3D12_RESOURCE_STATE_COPY_DEST;
    bool m_fgUiAlphaInitialized = false;
    bool m_frameGenerationEnabled = false;
    bool m_frameGenerationActiveThisFrame = false;
    uint32_t m_frameGenerationFrameIndex = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_guideFootprint{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_subtitleFootprint{};
    uint8_t* m_subtitleUploadMapped[FrameCount]{};
    uint32_t m_subtitleRows=0;
    uint64_t m_subtitleRowSize=0,m_subtitleUploadBytes=0;
    uint32_t m_subtitleTexW=0,m_subtitleTexH=0;
    std::wstring m_subtitleText;
    std::vector<uint8_t> m_subtitlePixels;
    bool m_subtitleDirty=false;
    bool m_subtitleInCopyDest=true;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_aiDepthFootprint{};
    uint32_t m_numRows=0,m_guideRows=0,m_aiDepthRows=0;
    uint64_t m_rowSize=0,m_uploadBytes=0,m_guideRowSize=0,m_guideUploadBytes=0,m_aiDepthRowSize=0,m_aiDepthUploadBytes=0;

    bool m_sourceInCopyDest = true;
    bool m_gridInCopyDest = true;
    bool m_aiDepthInCopyDest = true;
    bool m_aiDepthClearPending = true;
    bool m_aiDepthValid = false;
    bool m_aiHardwareDepthInWrite = true;
    bool m_aiHardwareDepthClearPending = true;
    bool m_aiHardwareDepthValid = false;
    bool m_flatDepthInWrite = true;
    DepthSource m_depthSource = DepthSource::Legacy;
    DepthSource m_effectiveDepthSource = DepthSource::Legacy;
    bool m_colorInRT = true;
    bool m_guidesInRT = true;
    bool m_depthInWrite = true;
    bool m_outputInUAV = true;
    bool m_dlssEnabled = true;
    bool m_allowTearing = false;
    bool m_vsyncEnabled = true; // STEP 04H: policy; m_allowTearing remains only a DXGI capability.
    bool m_recreateRequested = false;
    bool m_delayedRecreateDone = false;
    uint64_t m_framesPresented = 0;
    uint64_t m_frameGenerationDisplayedFramesTotal = 0;
    uint32_t m_frameGenerationMultiplier = 2;
    DebugView m_debugView = DebugView::Final;
    ColorSettings m_colorSettings{};
    bool m_lastDLSSUsed = false;
    bool m_splitScreen = false;
    float m_splitFraction = 0.5f;
    DLSSBackend m_dlss;
};
