#include "OpticalFlowEngine.h"
#include "FilmMotionStabilizer.h"
#include <cmath>
#include "NvofConfidenceRepair.h"

#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>

#include "NvOFD3D12.h"
#include "NvOFD3DCommon.h"
#include "Log.h"

using Microsoft::WRL::ComPtr;
using OfClock = std::chrono::steady_clock;

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

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string ReadEnvAscii(const char* name) {
    char buffer[128]{};
    const DWORD n = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (!n || n >= sizeof(buffer)) return {};
    return std::string(buffer, buffer + n);
}

struct PerfChoice {
    NV_OF_PERF_LEVEL level = NV_OF_PERF_LEVEL_SLOW;
    const char* name = "SLOW";
};

PerfChoice ResolvePerfChoice() {
    // Step 04B-5: MEDIUM is the normal player default. SLOW/FAST remain explicit A/B choices.
    const std::string requested = LowerAscii(ReadEnvAscii("DMP_NVOF_PERF"));
    if (requested == "slow" || requested == "0")
        return {NV_OF_PERF_LEVEL_SLOW, "SLOW"};
    if (requested == "fast" || requested == "20")
        return {NV_OF_PERF_LEVEL_FAST, "FAST"};
    return {NV_OF_PERF_LEVEL_MEDIUM, "MEDIUM"};
}

bool ResolveFilmStabilization(std::string& modeName) {
    const std::string requested = LowerAscii(ReadEnvAscii("DMP_NVOF_FILM_STABILIZE"));
    if (requested == "off" || requested == "0" || requested == "false" || requested == "raw") {
        modeName = "OFF_RAW";
        return false;
    }
    modeName = "AUTO_FILM_GRAIN";
    return true;
}

// STEP 06A1 NVOF analysis-only contrast prefilter.
// This transform exists only on the private CPU copy uploaded to NVOFA. The decoded frame,
// DLSS color input, RenoDX/NR input, subtitles, depth and temporal-mask color remain untouched.
struct AnalysisContrastChoice {
    const char* name = "OFF";
    double gamma = 1.0;
};

AnalysisContrastChoice ResolveAnalysisContrast() {
    const std::string requested = LowerAscii(ReadEnvAscii("DMP_NVOF_ANALYSIS_CONTRAST"));
    if (requested == "mild" || requested == "1") return {"MILD", 1.12};
    if (requested == "medium" || requested == "2") return {"MEDIUM", 1.50}; // STEP 06A2 previous Strong
    if (requested == "strong" || requested == "3") return {"STRONG", 2.40}; // STEP 06A2 extreme
    if (requested.empty() || requested == "off" || requested == "0" || requested == "raw") return {"OFF", 1.0};
    return {"OFF_INVALID_FALLBACK", 1.0};
}
// STEP 06A2 shadow-noise-aware NVOF preprocessing.
// The filter is 3x3, edge-aware and luminance-only. It runs only in deep shadows and only
// on the private NVOF analysis copy, before the Step06A1 contrast LUT.
struct ShadowFilterChoice {
    const char* name = "OFF";
    int lumaThreshold = 0;
    int edgeDelta = 0;
    double blend = 0.0;
};

ShadowFilterChoice ResolveShadowFilter() {
    const std::string requested = LowerAscii(ReadEnvAscii("DMP_NVOF_SHADOW_FILTER"));
    if (requested == "low" || requested == "1") return {"LOW", 42, 10, 0.35};
    if (requested == "medium" || requested == "2") return {"MEDIUM", 58, 14, 0.55};
    if (requested.empty() || requested == "off" || requested == "0" || requested == "raw") return {"OFF", 0, 0, 0.0};
    return {"OFF_INVALID_FALLBACK", 0, 0, 0.0};
}
uint32_t ResolveGridChoice(uint32_t preferredGrid, uint32_t width, uint32_t height,
                           std::string& policyName) {
    // Step 04B-5 AUTO grid policy: keep the validated 2x2 path for normal video,
    // but avoid a 2M-vector field on true 4K-class NVOFA inputs. Manual 1/2/4 override AUTO.
    const std::string requested = LowerAscii(ReadEnvAscii("DMP_NVOF_GRID"));
    if (requested.empty() || requested == "auto") {
        policyName = "AUTO";
        const uint64_t pixels = uint64_t(width) * uint64_t(height);
        return pixels >= 6000000ull ? 4u : std::max(1u, preferredGrid);
    }
    if (requested == "1" || requested == "1x1") { policyName = "MANUAL_1X1"; return 1u; }
    if (requested == "2" || requested == "2x2") { policyName = "MANUAL_2X2"; return 2u; }
    if (requested == "4" || requested == "4x4") { policyName = "MANUAL_4X4"; return 4u; }
    policyName = "AUTO_INVALID_FALLBACK";
    const uint64_t pixels = uint64_t(width) * uint64_t(height);
    return pixels >= 6000000ull ? 4u : std::max(1u, preferredGrid);
}

inline double MsBetween(OfClock::time_point a, OfClock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

} // namespace

struct OpticalFlowEngine::Impl {
    ID3D12Device* device = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t gridSize = 0;
    uint32_t gridW = 0;
    uint32_t gridH = 0;
    std::string perfName = "SLOW";
    std::string filmModeName = "AUTO_FILM_GRAIN";
    bool filmStabilizationEnabled = true;
    FilmMotionStabilizer filmStabilizer;
    std::string analysisContrastName = "OFF";
    double analysisContrastGamma = 1.0;
    std::vector<uint8_t> analysisContrastLut;
    std::vector<uint8_t> analysisScratch;
    std::string shadowFilterName = "OFF";
    int shadowFilterLumaThreshold = 0;
    int shadowFilterEdgeDelta = 0;
    double shadowFilterBlend = 0.0;
    std::vector<uint8_t> analysisLuma;

    NvOFObj flow;
    std::vector<ComPtr<ID3D12Resource>> inputResources;
    ComPtr<ID3D12Resource> outputResource;
    std::vector<NvOFBufferObj> inputBuffers;
    NvOFBufferObj outputBuffer;
    ComPtr<ID3D12Resource> costResource;
    NvOFBufferObj costBuffer;

    NV_OF_FENCE_POINT appFence{};
    NV_OF_FENCE_POINT ofaFence{};

    std::vector<NV_OF_FLOW_VECTOR> hostFlow;
    std::vector<uint32_t> hostCostStorage;
    NvofConfidenceRepair confidenceRepair;
    uint32_t previousIndex = 0;
    bool havePrevious = false;
    bool disableHintsOnNextPair = true;
    OpticalFlowStats stats{};

    ~Impl() {
        Shutdown();
    }

    void Shutdown() {
        // Buffers must unregister while the NvOF session/fences are still alive.
        costBuffer.reset();
        costResource.Reset();
        outputBuffer.reset();
        inputBuffers.clear();
        outputResource.Reset();
        inputResources.clear();
        flow.reset();
        ReleaseFencePoint(appFence);
        ReleaseFencePoint(ofaFence);
        hostFlow.clear();
        hostCostStorage.clear();
        confidenceRepair.Reset();
        analysisContrastLut.clear();
        analysisScratch.clear();
        analysisLuma.clear();
        filmStabilizer.Reset();
        havePrevious = false;
        device = nullptr;
        width = height = gridSize = gridW = gridH = 0;
        perfName = "SLOW";
        filmModeName = "AUTO_FILM_GRAIN";
        analysisContrastName = "OFF";
        analysisContrastGamma = 1.0;
        shadowFilterName = "OFF";
        shadowFilterLumaThreshold = 0;
        shadowFilterEdgeDelta = 0;
        shadowFilterBlend = 0.0;
        filmStabilizationEnabled = true;
        stats = {};
    }

    void ResetHistory() {
        havePrevious = false;
        previousIndex = 0;
        disableHintsOnNextPair = true;
        filmStabilizer.Reset();
    }

    const uint8_t* PrepareAnalysisInput(const uint8_t* bgra, size_t bytes, double& preprocessMs) {
        preprocessMs = 0.0;
        const size_t pixelCount = size_t(width) * height;
        const size_t expected = pixelCount * 4u;
        if (!bgra || bytes < expected) return bgra;
        const bool contrastActive = analysisContrastGamma > 1.000001 && analysisContrastLut.size() == 256u;
        const bool shadowActive = shadowFilterBlend > 0.000001 && shadowFilterLumaThreshold > 0 && shadowFilterEdgeDelta > 0;
        // Exact baseline bypass: no copy, no LUT, no shadow pass and the original pointer.
        if (!contrastActive && !shadowActive) return bgra;

        const auto start = OfClock::now();
        analysisScratch.resize(expected);
        if (shadowActive) {
            analysisLuma.resize(pixelCount);
            for (size_t p = 0; p < pixelCount; ++p) {
                const size_t i = p * 4u;
                // Integer BT.709-style luma from BGRA. We filter this scalar only.
                analysisLuma[p] = static_cast<uint8_t>((19u * bgra[i + 0u] + 183u * bgra[i + 1u] + 54u * bgra[i + 2u] + 128u) >> 8u);
            }
        }

        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const size_t p = size_t(y) * width + x;
                const size_t i = p * 4u;
                int workingB = bgra[i + 0u];
                int workingG = bgra[i + 1u];
                int workingR = bgra[i + 2u];

                if (shadowActive && x > 0u && y > 0u && x + 1u < width && y + 1u < height) {
                    const int centerLuma = analysisLuma[p];
                    if (centerLuma <= shadowFilterLumaThreshold) {
                        int lumaSum = centerLuma;
                        int accepted = 1;
                        for (int dy = -1; dy <= 1; ++dy) {
                            for (int dx = -1; dx <= 1; ++dx) {
                                if (dx == 0 && dy == 0) continue;
                                const size_t np = size_t(int(y) + dy) * width + size_t(int(x) + dx);
                                const int neighborLuma = analysisLuma[np];
                                if (neighborLuma > shadowFilterLumaThreshold + shadowFilterEdgeDelta) continue;
                                if (std::abs(neighborLuma - centerLuma) > shadowFilterEdgeDelta) continue;
                                lumaSum += neighborLuma;
                                ++accepted;
                            }
                        }
                        if (accepted >= 3) {
                            const double averageLuma = double(lumaSum) / double(accepted);
                            const int filteredLuma = std::clamp(int(std::lround(double(centerLuma) * (1.0 - shadowFilterBlend) + averageLuma * shadowFilterBlend)), 0, 255);
                            // Add the same delta to B/G/R: only luminance is smoothed; local chroma offsets are retained.
                            const int delta = filteredLuma - centerLuma;
                            workingB = std::clamp(workingB + delta, 0, 255);
                            workingG = std::clamp(workingG + delta, 0, 255);
                            workingR = std::clamp(workingR + delta, 0, 255);
                        }
                    }
                }

                analysisScratch[i + 0u] = contrastActive ? analysisContrastLut[size_t(workingB)] : static_cast<uint8_t>(workingB);
                analysisScratch[i + 1u] = contrastActive ? analysisContrastLut[size_t(workingG)] : static_cast<uint8_t>(workingG);
                analysisScratch[i + 2u] = contrastActive ? analysisContrastLut[size_t(workingR)] : static_cast<uint8_t>(workingR);
                analysisScratch[i + 3u] = bgra[i + 3u];
            }
        }
        preprocessMs = MsBetween(start, OfClock::now());
        return analysisScratch.data();
    }

    // STEP 06A4D: exact Step06A3 policy, parallel classification and S10.5 histogram medians.
    using ConfidenceRepairStats = NvofConfidenceRepair::Stats;
    ConfidenceRepairStats RepairLowConfidenceMotion(std::vector<float>& motionXY, const uint8_t* bgra) {
        const size_t cells=size_t(gridW)*gridH;
        if(hostCostStorage.size()*sizeof(uint32_t)<cells) return {};
        return confidenceRepair.Run(motionXY,bgra,
            reinterpret_cast<const uint8_t*>(hostCostStorage.data()),width,height,gridSize);
    }
    void UpdatePairStats(double preprocessMs, double uploadMs, double executeMs, double downloadMs,
                         double convertMs, double stabilizeMs, double totalMs,
                         const FilmMotionStabilizationStats& filmStats) {
        ++stats.calls;
        ++stats.pairs;
        stats.lastPreprocessMs = preprocessMs;
        stats.lastUploadMs = uploadMs;
        stats.lastExecuteMs = executeMs;
        stats.lastDownloadMs = downloadMs;
        stats.lastConvertMs = convertMs;
        stats.lastStabilizeMs = stabilizeMs;
        stats.lastTotalMs = totalMs;
        stats.filmStabilizationEnabled = filmStabilizationEnabled;
        stats.filmPerformanceBypass = filmStats.performanceBypass;
        stats.filmNoisePx = filmStats.robustNoisePx;
        stats.filmCorrectedPct = filmStats.correctedPct;
        stats.filmSnappedPct = filmStats.snappedPct;
        stats.filmMeanCorrectionPx = filmStats.meanCorrectionPx;
        stats.filmMaxCorrectionPx = filmStats.maxCorrectionPx;
        stats.filmCenterMotionX = filmStats.centerMotionX;
        stats.filmCenterMotionY = filmStats.centerMotionY;
        const double a = 0.10;
        if (stats.pairs == 1u) {
            stats.emaPreprocessMs = preprocessMs;
            stats.emaUploadMs = uploadMs;
            stats.emaExecuteMs = executeMs;
            stats.emaDownloadMs = downloadMs;
            stats.emaConvertMs = convertMs;
            stats.emaStabilizeMs = stabilizeMs;
            stats.emaTotalMs = totalMs;
        } else {
            stats.emaPreprocessMs = stats.emaPreprocessMs * (1.0 - a) + preprocessMs * a;
            stats.emaUploadMs = stats.emaUploadMs * (1.0 - a) + uploadMs * a;
            stats.emaExecuteMs = stats.emaExecuteMs * (1.0 - a) + executeMs * a;
            stats.emaDownloadMs = stats.emaDownloadMs * (1.0 - a) + downloadMs * a;
            stats.emaConvertMs = stats.emaConvertMs * (1.0 - a) + convertMs * a;
            stats.emaStabilizeMs = stats.emaStabilizeMs * (1.0 - a) + stabilizeMs * a;
            stats.emaTotalMs = stats.emaTotalMs * (1.0 - a) + totalMs * a;
        }

        if (stats.pairs <= 3u || (stats.pairs % 60u) == 0u) {
            LOG("[NVOF Perf] mode=" << perfName << " analysisContrast=" << analysisContrastName << " shadowFilter=" << shadowFilterName
                << " grid=" << gridSize
                << " pair=" << stats.pairs
                << " preprocessMs=" << preprocessMs << " emaPreprocessMs=" << stats.emaPreprocessMs
                << " uploadMs=" << uploadMs
                << " executeMs=" << executeMs
                << " downloadMs=" << downloadMs
                << " convertMs=" << convertMs
                << " stabilizeMs=" << stabilizeMs
                << " totalMs=" << totalMs
                << " emaTotalMs=" << stats.emaTotalMs);
            LOG("[NVOF Film] mode=" << filmModeName
                << " model=" << (filmStats.performanceBypass ? "PERF_BYPASS" : (filmStats.modelValid ? "AFFINE" : "BYPASS"))
                << " noisePx=" << filmStats.robustNoisePx
                << " correctedPct=" << filmStats.correctedPct
                << " snappedPct=" << filmStats.snappedPct
                << " meanCorrectionPx=" << filmStats.meanCorrectionPx
                << " maxCorrectionPx=" << filmStats.maxCorrectionPx
                << " centerMotion=(" << filmStats.centerMotionX << "," << filmStats.centerMotionY << ")"
                << " history=" << (filmStats.usedHistory ? 1 : 0));
        }
    }

    bool Initialize(ID3D12Device* newDevice, uint32_t w, uint32_t h,
                    uint32_t preferredGrid) {
        Shutdown();
        if (!newDevice || !w || !h) return false;

        device = newDevice;
        width = w;
        height = h;

        const PerfChoice perf = ResolvePerfChoice();
        perfName = perf.name;
        filmStabilizationEnabled = ResolveFilmStabilization(filmModeName);
        const AnalysisContrastChoice analysisContrast = ResolveAnalysisContrast();
        analysisContrastName = analysisContrast.name;
        analysisContrastGamma = analysisContrast.gamma;
        const ShadowFilterChoice shadowFilter = ResolveShadowFilter();
        shadowFilterName = shadowFilter.name;
        shadowFilterLumaThreshold = shadowFilter.lumaThreshold;
        shadowFilterEdgeDelta = shadowFilter.edgeDelta;
        shadowFilterBlend = shadowFilter.blend;
        analysisContrastLut.resize(256u);
        for (size_t i = 0; i < analysisContrastLut.size(); ++i) {
            const double x = double(i) / 255.0;
            double y = x;
            if (analysisContrastGamma > 1.000001) {
                y = x <= 0.5
                    ? 0.5 * std::pow(2.0 * x, analysisContrastGamma)
                    : 1.0 - 0.5 * std::pow(2.0 * (1.0 - x), analysisContrastGamma);
            }
            analysisContrastLut[i] = static_cast<uint8_t>(std::clamp(int(std::lround(y * 255.0)), 0, 255));
        }
        analysisScratch.clear();
        analysisLuma.clear();
        std::string gridPolicy;
        const uint32_t requestedGrid = ResolveGridChoice(std::max(1u, preferredGrid), width, height, gridPolicy);

        flow = NvOFD3D12::Create(device, width, height,
                                 NV_OF_BUFFER_FORMAT_ABGR8,
                                 NV_OF_MODE_OPTICALFLOW,
                                 perf.level);
        if (!flow) return false;

        uint32_t selectedGrid = requestedGrid;
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

        NV_OF_BUFFER_DESCRIPTOR costDesc{};
        costDesc.width = gridW;
        costDesc.height = gridH;
        costDesc.bufferUsage = NV_OF_BUFFER_USAGE_COST;
        costDesc.bufferFormat = NV_OF_BUFFER_FORMAT_UINT8;
        costResource = AllocateTexture(device, costDesc);
        ++ofaFence.value;
        costBuffer = flow->RegisterPreAllocBuffers(
            costDesc, costResource.Get(), &appFence, &ofaFence);

        hostFlow.resize(size_t(gridW) * gridH);
        hostCostStorage.resize((size_t(gridW) * gridH + sizeof(uint32_t) - 1u) / sizeof(uint32_t));
        ResetHistory();

        LOG("[NVOF] Initialized D3D12: input=" << width << "x" << height
            << " gridPolicy=" << gridPolicy
            << " requestedGrid=" << requestedGrid
            << " hwGrid=" << gridSize
            << " output=" << gridW << "x" << gridH
            << " perf=" << perfName
            << " filmStabilize=" << filmModeName
            << " analysisContrast=" << analysisContrastName << " contrastGamma=" << analysisContrastGamma
            << " shadowFilter=" << shadowFilterName << " shadowLumaThreshold=" << shadowFilterLumaThreshold << " shadowEdgeDelta=" << shadowFilterEdgeDelta << " shadowBlend=" << shadowFilterBlend
            << " envPerf=" << (ReadEnvAscii("DMP_NVOF_PERF").empty() ? "default" : ReadEnvAscii("DMP_NVOF_PERF")));
        return true;
    }

    bool GenerateGpu(const uint8_t* bgra,size_t bytes,bool reset,ID3D12Fence* consumedFence,uint64_t consumedValue,GpuOpticalFlowFrame& out){
        out={};if(!flow||!bgra||bytes<size_t(width)*height*4u)return false;
        if(reset)ResetHistory();
        const auto start=OfClock::now();
        const uint32_t current=havePrevious?1u-previousIndex:0u;
        // SDK staging reuse waits for the last renderer consumer. No readback is performed.
        NV_OF_FENCE_POINT consumed{};consumed.fence=consumedFence;consumed.value=consumedValue;
        auto* waitPoint=consumedFence&&consumedValue?&consumed:&ofaFence;
        ++appFence.value;inputBuffers[current]->UploadData(bgra,waitPoint,&appFence);
        const auto uploaded=OfClock::now();
        const bool pair=havePrevious;
        if(pair){
            ++ofaFence.value;
            flow->Execute(inputBuffers[current].get(),inputBuffers[previousIndex].get(),outputBuffer.get(),nullptr,costBuffer.get(),0,nullptr,&appFence,1,&ofaFence,disableHintsOnNextPair?NV_OF_TRUE:NV_OF_FALSE);
            disableHintsOnNextPair=false;
        }
        out.color=inputResources[current].Get();
        out.previousColor=pair?inputResources[previousIndex].Get():inputResources[current].Get();
        previousIndex=current;havePrevious=true;
        out.motion=outputResource.Get();out.cost=costResource.Get();
        out.readyFence=pair?ofaFence.fence:appFence.fence;out.readyValue=pair?ofaFence.value:appFence.value;
        out.gridW=gridW;out.gridH=gridH;out.sourceW=width;out.sourceH=height;out.valid=pair;
        filmStabilizationEnabled=false;filmModeName="OFF_GPU_RAW";analysisContrastName="OFF_GPU_RAW";shadowFilterName="OFF_GPU_RAW";
        FilmMotionStabilizationStats noFilters{};
        UpdatePairStats(0,MsBetween(start,uploaded),MsBetween(uploaded,OfClock::now()),0,0,0,MsBetween(start,OfClock::now()),noFilters);
        if(stats.calls<=3u||stats.calls%120u==0u)LOG("[GPU MV] raw=1 rendererResolve=LOCAL_PLUS_CAMERA sceneCut=SPARSE_DECODER confidenceCPU=OFF filmFilter=OFF depth=CONSTANT mask=GPU_DISOCCLUSION uploadBytes="<<size_t(width)*height*4u<<" readbackBytes=0 guideUploadBytes=0 pair="<<pair<<" grid="<<gridW<<"x"<<gridH);
        return true;
    }

    bool Generate(const uint8_t* bgra, size_t bytes, bool reset,
                  OpticalFlowFrame& out) {
        out = {};
        if (!flow || !bgra || bytes < size_t(width) * height * 4u) return false;
        if (reset) ResetHistory();

        const auto totalStart = OfClock::now();
        double preprocessMs = 0.0;
        const uint8_t* analysisInput = PrepareAnalysisInput(bgra, bytes, preprocessMs);

        if (!havePrevious) {
            previousIndex = 0;
            const auto uploadStart = OfClock::now();
            ++appFence.value;
            inputBuffers[previousIndex]->UploadData(analysisInput, &ofaFence, &appFence);
            const auto uploadEnd = OfClock::now();
            havePrevious = true;
            ++stats.calls;
            stats.lastPreprocessMs = preprocessMs;
            stats.lastUploadMs = MsBetween(uploadStart, uploadEnd);
            stats.emaPreprocessMs = preprocessMs;
            stats.lastExecuteMs = 0.0;
            stats.lastDownloadMs = 0.0;
            stats.lastConvertMs = 0.0;
            stats.lastStabilizeMs = 0.0;
            stats.lastTotalMs = MsBetween(totalStart, uploadEnd);
            stats.filmStabilizationEnabled = filmStabilizationEnabled;
            return true;
        }

        const uint32_t currentIndex = 1u - previousIndex;
        const auto uploadStart = OfClock::now();
        ++appFence.value;
        inputBuffers[currentIndex]->UploadData(analysisInput, &ofaFence, &appFence);
        const auto uploadEnd = OfClock::now();

        const auto executeStart = uploadEnd;
        ++ofaFence.value;
        flow->Execute(inputBuffers[currentIndex].get(),
                      inputBuffers[previousIndex].get(),
                      outputBuffer.get(),
                      nullptr, costBuffer.get(), 0, nullptr,
                      &appFence, 1, &ofaFence,
                      disableHintsOnNextPair ? NV_OF_TRUE : NV_OF_FALSE);
        const auto executeEnd = OfClock::now();

        const auto downloadStart = executeEnd;
        outputBuffer->DownloadData(hostFlow.data(), &ofaFence);
        costBuffer->DownloadData(hostCostStorage.data(), &ofaFence);
        const auto downloadEnd = OfClock::now();
        disableHintsOnNextPair = false;
        previousIndex = currentIndex;

        const auto convertStart = downloadEnd;
        out.motionXY.resize(size_t(gridW) * gridH * 2u);
        for (size_t i = 0; i < hostFlow.size(); ++i) {
            // NVIDIA Optical Flow uses signed S10.5: divide by 32 for pixels.
            out.motionXY[i * 2u + 0u] = float(hostFlow[i].flowx) / 32.0f;
            out.motionXY[i * 2u + 1u] = float(hostFlow[i].flowy) / 32.0f;
        }
        const auto convertEnd = OfClock::now();
        const auto confidenceStart = convertEnd;
        const ConfidenceRepairStats confidenceStats = RepairLowConfidenceMotion(out.motionXY, bgra);
        const auto confidenceEnd = OfClock::now();
        const double confidenceMs = MsBetween(confidenceStart, confidenceEnd);
        const uint64_t confidencePair = stats.pairs + 1u;
        if (confidencePair <= 3u || (confidencePair % 60u) == 0u) {
            LOG("[NVOF Confidence] pair=" << confidencePair
                << " q25=" << unsigned(confidenceStats.costQ25) << " q50=" << unsigned(confidenceStats.costQ50) << " q75=" << unsigned(confidenceStats.costQ75)
                << " threshold=" << unsigned(confidenceStats.costThreshold) << " texturedPct=" << confidenceStats.texturedPct << " reliablePct=" << confidenceStats.reliablePct
                << " globalCoveragePct=" << confidenceStats.globalCoveragePct << " globalTrusted=" << (confidenceStats.globalTrusted ? 1 : 0)
                << " localFillPct=" << confidenceStats.localFillPct << " globalFillPct=" << confidenceStats.globalFillPct << " zeroPct=" << confidenceStats.zeroPct
                << " globalMotion=(" << confidenceStats.globalX << "," << confidenceStats.globalY << ")" << " repairMs=" << confidenceMs << " textureMs=" << confidenceStats.textureMs << " classifyMs=" << confidenceStats.classifyMs << " infillMs=" << confidenceStats.infillMs << " parallel=1 repairVersion=4D histogramMedian=1");
        }

        FilmMotionStabilizationStats filmStats{};
        const auto stabilizeStart = convertEnd;
        if (filmStabilizationEnabled) {
            filmStabilizer.Process(out.motionXY, gridW, gridH, true, &filmStats);
        } else {
            filmStabilizer.Reset();
            filmStats.enabled = false;
        }
        const auto stabilizeEnd = OfClock::now();

        out.gridW = gridW;
        out.gridH = gridH;
        out.gridSize = gridSize;
        out.sourceW = width;
        out.sourceH = height;
        out.valid = true;

        UpdatePairStats(preprocessMs,
                        MsBetween(uploadStart, uploadEnd),
                        MsBetween(executeStart, executeEnd),
                        MsBetween(downloadStart, downloadEnd),
                        MsBetween(convertStart, convertEnd),
                        MsBetween(stabilizeStart, stabilizeEnd),
                        MsBetween(totalStart, stabilizeEnd),
                        filmStats);
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
const char* OpticalFlowEngine::PerfName() const { return m_impl ? m_impl->perfName.c_str() : "OFF"; }
bool OpticalFlowEngine::FilmStabilizationEnabled() const { return m_impl ? m_impl->filmStabilizationEnabled : false; }
OpticalFlowStats OpticalFlowEngine::GetStats() const { return m_impl ? m_impl->stats : OpticalFlowStats{}; }

bool OpticalFlowEngine::GenerateGpu(const uint8_t* bgra,size_t bytes,bool reset,ID3D12Fence* consumedFence,uint64_t consumedValue,GpuOpticalFlowFrame& out){
    if(!m_impl){out={};return false;}try{return m_impl->GenerateGpu(bgra,bytes,reset,consumedFence,consumedValue,out);}catch(const std::exception& e){LOG("[GPU MV] failure: "<<e.what());out={};return false;}
}
