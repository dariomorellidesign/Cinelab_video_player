#include "OpticalFlowEngine.h"
#include "FilmMotionStabilizer.h"

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
    OpticalFlowStats stats{};

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
        filmStabilizer.Reset();
        havePrevious = false;
        device = nullptr;
        width = height = gridSize = gridW = gridH = 0;
        perfName = "SLOW";
        filmModeName = "AUTO_FILM_GRAIN";
        filmStabilizationEnabled = true;
        stats = {};
    }

    void ResetHistory() {
        havePrevious = false;
        previousIndex = 0;
        disableHintsOnNextPair = true;
        filmStabilizer.Reset();
    }

    void UpdatePairStats(double uploadMs, double executeMs, double downloadMs,
                         double convertMs, double stabilizeMs, double totalMs,
                         const FilmMotionStabilizationStats& filmStats) {
        ++stats.calls;
        ++stats.pairs;
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
            stats.emaUploadMs = uploadMs;
            stats.emaExecuteMs = executeMs;
            stats.emaDownloadMs = downloadMs;
            stats.emaConvertMs = convertMs;
            stats.emaStabilizeMs = stabilizeMs;
            stats.emaTotalMs = totalMs;
        } else {
            stats.emaUploadMs = stats.emaUploadMs * (1.0 - a) + uploadMs * a;
            stats.emaExecuteMs = stats.emaExecuteMs * (1.0 - a) + executeMs * a;
            stats.emaDownloadMs = stats.emaDownloadMs * (1.0 - a) + downloadMs * a;
            stats.emaConvertMs = stats.emaConvertMs * (1.0 - a) + convertMs * a;
            stats.emaStabilizeMs = stats.emaStabilizeMs * (1.0 - a) + stabilizeMs * a;
            stats.emaTotalMs = stats.emaTotalMs * (1.0 - a) + totalMs * a;
        }

        if (stats.pairs <= 3u || (stats.pairs % 60u) == 0u) {
            LOG("[NVOF Perf] mode=" << perfName
                << " grid=" << gridSize
                << " pair=" << stats.pairs
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

        hostFlow.resize(size_t(gridW) * gridH);
        ResetHistory();

        LOG("[NVOF] Initialized D3D12: input=" << width << "x" << height
            << " gridPolicy=" << gridPolicy
            << " requestedGrid=" << requestedGrid
            << " hwGrid=" << gridSize
            << " output=" << gridW << "x" << gridH
            << " perf=" << perfName
            << " filmStabilize=" << filmModeName
            << " envPerf=" << (ReadEnvAscii("DMP_NVOF_PERF").empty() ? "default" : ReadEnvAscii("DMP_NVOF_PERF")));
        return true;
    }

    bool Generate(const uint8_t* bgra, size_t bytes, bool reset,
                  OpticalFlowFrame& out) {
        out = {};
        if (!flow || !bgra || bytes < size_t(width) * height * 4u) return false;
        if (reset) ResetHistory();

        const auto totalStart = OfClock::now();

        if (!havePrevious) {
            previousIndex = 0;
            const auto uploadStart = OfClock::now();
            ++appFence.value;
            inputBuffers[previousIndex]->UploadData(bgra, &ofaFence, &appFence);
            const auto uploadEnd = OfClock::now();
            havePrevious = true;
            ++stats.calls;
            stats.lastUploadMs = MsBetween(uploadStart, uploadEnd);
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
        inputBuffers[currentIndex]->UploadData(bgra, &ofaFence, &appFence);
        const auto uploadEnd = OfClock::now();

        const auto executeStart = uploadEnd;
        ++ofaFence.value;
        flow->Execute(inputBuffers[currentIndex].get(),
                      inputBuffers[previousIndex].get(),
                      outputBuffer.get(),
                      nullptr, nullptr, 0, nullptr,
                      &appFence, 1, &ofaFence,
                      disableHintsOnNextPair ? NV_OF_TRUE : NV_OF_FALSE);
        const auto executeEnd = OfClock::now();

        const auto downloadStart = executeEnd;
        outputBuffer->DownloadData(hostFlow.data(), &ofaFence);
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

        UpdatePairStats(MsBetween(uploadStart, uploadEnd),
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
