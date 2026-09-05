#include "TrtRtxDepthEngine.h"
#include "Log.h"

#include <NvInfer.h>
#include <NvInferRuntime.h>
#include <cuda_runtime_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

template <typename T>
using TrtPtr = std::unique_ptr<T>;

class Logger final : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* msg) noexcept override {
        if (severity <= Severity::kWARNING && msg) {
            LOG("[TRT-RTX] " << msg);
        }
    }
};

static std::vector<uint8_t> ReadBinary(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto end = f.tellg();
    if (end <= 0) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(end));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) return {};
    return bytes;
}

static bool WriteBinary(const std::filesystem::path& path, const void* data, size_t bytes) {
    if (!data || bytes == 0) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    return static_cast<bool>(f);
}

static bool PositiveDims(const nvinfer1::Dims& d) {
    if (d.nbDims <= 0) return false;
    for (int32_t i = 0; i < d.nbDims; ++i) if (d.d[i] <= 0) return false;
    return true;
}

static bool HasDynamicDims(const nvinfer1::Dims& d) {
    for (int32_t i = 0; i < d.nbDims; ++i) if (d.d[i] < 0) return true;
    return false;
}

static size_t Volume(const nvinfer1::Dims& d) {
    if (!PositiveDims(d)) return 0;
    size_t v = 1;
    for (int32_t i = 0; i < d.nbDims; ++i) v *= static_cast<size_t>(d.d[i]);
    return v;
}

static std::string DimsText(const nvinfer1::Dims& d) {
    std::ostringstream s;
    s << '[';
    for (int32_t i = 0; i < d.nbDims; ++i) {
        if (i) s << ',';
        s << d.d[i];
    }
    s << ']';
    return s.str();
}

static const char* DataTypeName(nvinfer1::DataType t) {
    switch (t) {
    case nvinfer1::DataType::kFLOAT: return "float32";
    case nvinfer1::DataType::kHALF: return "float16";
    case nvinfer1::DataType::kINT8: return "int8";
    case nvinfer1::DataType::kINT32: return "int32";
    case nvinfer1::DataType::kBOOL: return "bool";
    default: return "other";
    }
}

static void FillNormalizedRGB(const uint8_t* bgra,
                              uint32_t sourceW,
                              uint32_t sourceH,
                              size_t sourceStride,
                              uint32_t targetW,
                              uint32_t targetH,
                              std::vector<float>& nchw) {
    constexpr float mean[3] = {0.485f, 0.456f, 0.406f};
    constexpr float stddev[3] = {0.229f, 0.224f, 0.225f};
    const size_t plane = static_cast<size_t>(targetW) * targetH;
    nchw.assign(plane * 3u, 0.0f);
    const float scaleX = static_cast<float>(sourceW) / static_cast<float>(targetW);
    const float scaleY = static_cast<float>(sourceH) / static_cast<float>(targetH);

    for (uint32_t y = 0; y < targetH; ++y) {
        const float sy = std::clamp((static_cast<float>(y) + 0.5f) * scaleY - 0.5f,
                                    0.0f, static_cast<float>(sourceH - 1));
        const uint32_t y0 = static_cast<uint32_t>(std::floor(sy));
        const uint32_t y1 = std::min(y0 + 1, sourceH - 1);
        const float fy = sy - static_cast<float>(y0);
        const uint8_t* row0 = bgra + static_cast<size_t>(y0) * sourceStride;
        const uint8_t* row1 = bgra + static_cast<size_t>(y1) * sourceStride;
        for (uint32_t x = 0; x < targetW; ++x) {
            const float sx = std::clamp((static_cast<float>(x) + 0.5f) * scaleX - 0.5f,
                                        0.0f, static_cast<float>(sourceW - 1));
            const uint32_t x0 = static_cast<uint32_t>(std::floor(sx));
            const uint32_t x1 = std::min(x0 + 1, sourceW - 1);
            const float fx = sx - static_cast<float>(x0);
            const uint8_t* p00 = row0 + static_cast<size_t>(x0) * 4u;
            const uint8_t* p10 = row0 + static_cast<size_t>(x1) * 4u;
            const uint8_t* p01 = row1 + static_cast<size_t>(x0) * 4u;
            const uint8_t* p11 = row1 + static_cast<size_t>(x1) * 4u;
            const size_t outIndex = static_cast<size_t>(y) * targetW + x;
            for (uint32_t c = 0; c < 3; ++c) {
                const uint32_t bgraChannel = 2u - c;
                const float a = static_cast<float>(p00[bgraChannel]) * (1.0f - fx) + static_cast<float>(p10[bgraChannel]) * fx;
                const float b = static_cast<float>(p01[bgraChannel]) * (1.0f - fx) + static_cast<float>(p11[bgraChannel]) * fx;
                const float value01 = (a * (1.0f - fy) + b * fy) / 255.0f;
                nchw[static_cast<size_t>(c) * plane + outIndex] = (value01 - mean[c]) / stddev[c];
            }
        }
    }
}

static float Percentile(std::vector<float> values, double fraction) {
    if (values.empty()) return 0.0f;
    fraction = std::clamp(fraction, 0.0, 1.0);
    const size_t index = static_cast<size_t>(std::llround(fraction * static_cast<double>(values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return values[index];
}
}

struct TrtRtxDepthEngine::Impl {
    Logger logger;
    TrtPtr<nvinfer1::IRuntime> runtime;
    TrtPtr<nvinfer1::ICudaEngine> engine;
    TrtPtr<nvinfer1::IRuntimeConfig> runtimeConfig;
    TrtPtr<nvinfer1::IRuntimeCache> runtimeCache;
    TrtPtr<nvinfer1::IExecutionContext> context;

    cudaStream_t stream = nullptr;
    void* inputDevice = nullptr;
    void* outputDevice = nullptr;
    size_t inputBytes = 0;
    size_t outputBytes = 0;

    std::string inputName;
    std::string outputName;
    nvinfer1::Dims inputDims{};
    nvinfer1::Dims outputDims{};
    uint32_t inputW = 0;
    uint32_t inputH = 0;
    uint32_t outputW = 0;
    uint32_t outputH = 0;
    std::filesystem::path cachePath;
    bool cacheHadData = false;
    bool cacheSaved = false;
    double contextJitMs = 0.0;
    std::string lastError;
    std::string summary;

    ~Impl() {
        context.reset();
        if (inputDevice) cudaFree(inputDevice);
        if (outputDevice) cudaFree(outputDevice);
        if (stream) cudaStreamDestroy(stream);
        runtimeCache.reset();
        runtimeConfig.reset();
        engine.reset();
        runtime.reset();
    }

    void Fail(const std::string& m) { lastError = m; }

    bool CudaOk(cudaError_t e, const char* what) {
        if (e == cudaSuccess) return true;
        std::ostringstream s;
        s << what << ": " << cudaGetErrorString(e) << " (" << static_cast<int>(e) << ')';
        Fail(s.str());
        return false;
    }

    void SaveCacheIfNeeded() {
        if (cacheSaved || cachePath.empty() || !runtimeCache) return;
        TrtPtr<nvinfer1::IHostMemory> blob(runtimeCache->serialize());
        if (!blob || !blob->data() || blob->size() == 0) return;
        if (WriteBinary(cachePath, blob->data(), blob->size())) cacheSaved = true;
    }
};

TrtRtxDepthEngine::TrtRtxDepthEngine() : m_impl(std::make_unique<Impl>()) {}
TrtRtxDepthEngine::~TrtRtxDepthEngine() = default;

bool TrtRtxDepthEngine::Initialize(const std::wstring& enginePath, const std::wstring& runtimeCachePath) {
    m_impl = std::make_unique<Impl>();
    try {
        const std::filesystem::path plan(enginePath);
        if (!std::filesystem::exists(plan)) {
            m_impl->Fail("TensorRT-RTX engine file does not exist.");
            return false;
        }
        m_impl->cachePath = std::filesystem::path(runtimeCachePath);
        const auto bytes = ReadBinary(plan);
        if (bytes.empty()) {
            m_impl->Fail("Could not read TensorRT-RTX engine bytes.");
            return false;
        }

        m_impl->runtime.reset(nvinfer1::createInferRuntime(m_impl->logger));
        if (!m_impl->runtime) { m_impl->Fail("createInferRuntime failed."); return false; }
        m_impl->engine.reset(m_impl->runtime->deserializeCudaEngine(bytes.data(), bytes.size()));
        if (!m_impl->engine) { m_impl->Fail("deserializeCudaEngine failed."); return false; }

        int inputCount = 0;
        int outputCount = 0;
        for (int32_t i = 0; i < m_impl->engine->getNbIOTensors(); ++i) {
            const char* name = m_impl->engine->getIOTensorName(i);
            if (!name) continue;
            const auto mode = m_impl->engine->getTensorIOMode(name);
            if (mode == nvinfer1::TensorIOMode::kINPUT) {
                ++inputCount;
                if (m_impl->inputName.empty()) m_impl->inputName = name;
            } else if (mode == nvinfer1::TensorIOMode::kOUTPUT) {
                ++outputCount;
                if (m_impl->outputName.empty()) m_impl->outputName = name;
            }
        }
        if (inputCount != 1 || outputCount < 1 || m_impl->inputName.empty() || m_impl->outputName.empty()) {
            m_impl->Fail("Expected one image input and at least one depth output.");
            return false;
        }
        if (m_impl->engine->getTensorDataType(m_impl->inputName.c_str()) != nvinfer1::DataType::kFLOAT ||
            m_impl->engine->getTensorDataType(m_impl->outputName.c_str()) != nvinfer1::DataType::kFLOAT) {
            std::ostringstream s;
            s << "Player AI-depth backend expects float32 I/O; input="
              << DataTypeName(m_impl->engine->getTensorDataType(m_impl->inputName.c_str()))
              << " output=" << DataTypeName(m_impl->engine->getTensorDataType(m_impl->outputName.c_str()));
            m_impl->Fail(s.str());
            return false;
        }

        m_impl->runtimeConfig.reset(m_impl->engine->createRuntimeConfig());
        if (!m_impl->runtimeConfig) { m_impl->Fail("createRuntimeConfig failed."); return false; }
        m_impl->runtimeConfig->setExecutionContextAllocationStrategy(nvinfer1::ExecutionContextAllocationStrategy::kSTATIC);
        m_impl->runtimeCache.reset(m_impl->runtimeConfig->createRuntimeCache());
        if (!m_impl->runtimeCache) { m_impl->Fail("createRuntimeCache failed."); return false; }
        if (!m_impl->cachePath.empty() && std::filesystem::exists(m_impl->cachePath)) {
            const auto cacheBytes = ReadBinary(m_impl->cachePath);
            if (!cacheBytes.empty()) {
                m_impl->runtimeCache->deserialize(cacheBytes.data(), cacheBytes.size());
                m_impl->cacheHadData = true;
            }
        }
        m_impl->runtimeConfig->setRuntimeCache(*m_impl->runtimeCache);

        const auto jitStart = Clock::now();
        m_impl->context.reset(m_impl->engine->createExecutionContext(m_impl->runtimeConfig.get()));
        const auto jitEnd = Clock::now();
        m_impl->contextJitMs = std::chrono::duration<double, std::milli>(jitEnd - jitStart).count();
        if (!m_impl->context) { m_impl->Fail("createExecutionContext failed during TensorRT-RTX JIT."); return false; }

        m_impl->inputDims = m_impl->engine->getTensorShape(m_impl->inputName.c_str());
        if (m_impl->inputDims.nbDims != 4) {
            m_impl->Fail("Depth engine input is not NCHW rank 4: " + DimsText(m_impl->inputDims));
            return false;
        }
        if (HasDynamicDims(m_impl->inputDims)) {
            nvinfer1::Dims4 concrete{1, 3, 518, 518};
            if (!m_impl->context->setInputShape(m_impl->inputName.c_str(), concrete)) {
                m_impl->Fail("setInputShape(1x3x518x518) failed for dynamic depth engine.");
                return false;
            }
            m_impl->inputDims = concrete;
        }
        if (!PositiveDims(m_impl->inputDims) || m_impl->inputDims.d[0] != 1 || m_impl->inputDims.d[1] != 3) {
            m_impl->Fail("Depth engine input shape is incompatible: " + DimsText(m_impl->inputDims));
            return false;
        }
        m_impl->inputH = static_cast<uint32_t>(m_impl->inputDims.d[2]);
        m_impl->inputW = static_cast<uint32_t>(m_impl->inputDims.d[3]);

        m_impl->outputDims = m_impl->context->getTensorShape(m_impl->outputName.c_str());
        if (!PositiveDims(m_impl->outputDims)) {
            m_impl->Fail("Depth engine output shape unresolved: " + DimsText(m_impl->outputDims));
            return false;
        }
        if (m_impl->outputDims.nbDims == 3) {
            m_impl->outputH = static_cast<uint32_t>(m_impl->outputDims.d[1]);
            m_impl->outputW = static_cast<uint32_t>(m_impl->outputDims.d[2]);
        } else if (m_impl->outputDims.nbDims == 4 && m_impl->outputDims.d[1] == 1) {
            m_impl->outputH = static_cast<uint32_t>(m_impl->outputDims.d[2]);
            m_impl->outputW = static_cast<uint32_t>(m_impl->outputDims.d[3]);
        } else {
            m_impl->Fail("Depth engine output is not [1,H,W] or [1,1,H,W]: " + DimsText(m_impl->outputDims));
            return false;
        }

        m_impl->inputBytes = Volume(m_impl->inputDims) * sizeof(float);
        m_impl->outputBytes = Volume(m_impl->outputDims) * sizeof(float);
        if (!m_impl->inputBytes || !m_impl->outputBytes) { m_impl->Fail("Invalid TensorRT-RTX tensor byte size."); return false; }
        if (!m_impl->CudaOk(cudaStreamCreate(&m_impl->stream), "cudaStreamCreate")) return false;
        if (!m_impl->CudaOk(cudaMalloc(&m_impl->inputDevice, m_impl->inputBytes), "cudaMalloc(input)")) return false;
        if (!m_impl->CudaOk(cudaMalloc(&m_impl->outputDevice, m_impl->outputBytes), "cudaMalloc(output)")) return false;
        if (!m_impl->context->setTensorAddress(m_impl->inputName.c_str(), m_impl->inputDevice) ||
            !m_impl->context->setTensorAddress(m_impl->outputName.c_str(), m_impl->outputDevice)) {
            m_impl->Fail("setTensorAddress failed.");
            return false;
        }

        std::ostringstream s;
        s << "TensorRT-RTX input=" << m_impl->inputName << ' ' << DimsText(m_impl->inputDims)
          << " float32 output=" << m_impl->outputName << ' ' << DimsText(m_impl->outputDims)
          << " float32 contextJIT=" << std::fixed << std::setprecision(2) << m_impl->contextJitMs << " ms"
          << " cache=" << (m_impl->cacheHadData ? "loaded" : "new");
        m_impl->summary = s.str();
        m_impl->lastError.clear();
        return true;
    } catch (const std::exception& e) {
        m_impl->Fail(e.what());
        return false;
    }
}

bool TrtRtxDepthEngine::InferBGRA(const uint8_t* bgra,
                                  uint32_t width,
                                  uint32_t height,
                                  size_t strideBytes,
                                  TrtDepthInferenceResult& result) {
    result = {};
    if (!IsReady()) { m_impl->Fail("TensorRT-RTX depth engine is not initialized."); return false; }
    if (!bgra || width == 0 || height == 0 || strideBytes < static_cast<size_t>(width) * 4u) {
        m_impl->Fail("Invalid BGRA frame."); return false;
    }

    const auto totalStart = Clock::now();
    const auto preStart = Clock::now();
    std::vector<float> input;
    FillNormalizedRGB(bgra, width, height, strideBytes, m_impl->inputW, m_impl->inputH, input);
    const auto preEnd = Clock::now();
    if (input.size() * sizeof(float) != m_impl->inputBytes) { m_impl->Fail("Preprocessed input size mismatch."); return false; }

    std::vector<float> output(m_impl->outputBytes / sizeof(float));
    cudaEvent_t up0=nullptr, up1=nullptr, in0=nullptr, in1=nullptr, down0=nullptr, down1=nullptr;
    auto makeEvent = [&](cudaEvent_t* e, const char* what) { return m_impl->CudaOk(cudaEventCreate(e), what); };
    if (!makeEvent(&up0,"cudaEventCreate") || !makeEvent(&up1,"cudaEventCreate") ||
        !makeEvent(&in0,"cudaEventCreate") || !makeEvent(&in1,"cudaEventCreate") ||
        !makeEvent(&down0,"cudaEventCreate") || !makeEvent(&down1,"cudaEventCreate")) {
        if (up0) cudaEventDestroy(up0); if (up1) cudaEventDestroy(up1); if (in0) cudaEventDestroy(in0);
        if (in1) cudaEventDestroy(in1); if (down0) cudaEventDestroy(down0); if (down1) cudaEventDestroy(down1);
        return false;
    }
    auto destroyEvents = [&]() {
        cudaEventDestroy(up0); cudaEventDestroy(up1); cudaEventDestroy(in0);
        cudaEventDestroy(in1); cudaEventDestroy(down0); cudaEventDestroy(down1);
    };

    if (!m_impl->CudaOk(cudaEventRecord(up0, m_impl->stream), "cudaEventRecord(upload start)") ||
        !m_impl->CudaOk(cudaMemcpyAsync(m_impl->inputDevice, input.data(), m_impl->inputBytes, cudaMemcpyHostToDevice, m_impl->stream), "cudaMemcpyAsync H2D") ||
        !m_impl->CudaOk(cudaEventRecord(up1, m_impl->stream), "cudaEventRecord(upload end)") ||
        !m_impl->CudaOk(cudaEventRecord(in0, m_impl->stream), "cudaEventRecord(inference start)")) {
        destroyEvents(); return false;
    }
    if (!m_impl->context->enqueueV3(m_impl->stream)) {
        const cudaError_t e = cudaGetLastError();
        std::ostringstream s;
        s << "enqueueV3 failed";
        if (e != cudaSuccess) s << ": " << cudaGetErrorString(e);
        m_impl->Fail(s.str());
        destroyEvents(); return false;
    }
    if (!m_impl->CudaOk(cudaEventRecord(in1, m_impl->stream), "cudaEventRecord(inference end)") ||
        !m_impl->CudaOk(cudaEventRecord(down0, m_impl->stream), "cudaEventRecord(download start)") ||
        !m_impl->CudaOk(cudaMemcpyAsync(output.data(), m_impl->outputDevice, m_impl->outputBytes, cudaMemcpyDeviceToHost, m_impl->stream), "cudaMemcpyAsync D2H") ||
        !m_impl->CudaOk(cudaEventRecord(down1, m_impl->stream), "cudaEventRecord(download end)") ||
        !m_impl->CudaOk(cudaStreamSynchronize(m_impl->stream), "cudaStreamSynchronize")) {
        destroyEvents(); return false;
    }
    float upload=0, infer=0, download=0;
    cudaEventElapsedTime(&upload, up0, up1);
    cudaEventElapsedTime(&infer, in0, in1);
    cudaEventElapsedTime(&download, down0, down1);
    destroyEvents();

    const auto postStart = Clock::now();
    result.width = m_impl->outputW;
    result.height = m_impl->outputH;
    result.depth = std::move(output);
    std::vector<float> finite;
    finite.reserve(result.depth.size());
    result.minValue = std::numeric_limits<float>::infinity();
    result.maxValue = -std::numeric_limits<float>::infinity();
    for (float v : result.depth) {
        if (!std::isfinite(v)) continue;
        finite.push_back(v);
        result.minValue = std::min(result.minValue, v);
        result.maxValue = std::max(result.maxValue, v);
    }
    if (finite.empty()) { m_impl->Fail("Depth output contains no finite values."); return false; }
    result.percentile02 = Percentile(finite, 0.02);
    result.percentile98 = Percentile(std::move(finite), 0.98);
    const auto postEnd = Clock::now();

    result.preprocessMs = std::chrono::duration<double, std::milli>(preEnd - preStart).count();
    result.uploadMs = upload;
    result.inferenceMs = infer;
    result.downloadMs = download;
    result.postprocessMs = std::chrono::duration<double, std::milli>(postEnd - postStart).count();
    result.totalMs = std::chrono::duration<double, std::milli>(postEnd - totalStart).count();
    m_impl->SaveCacheIfNeeded();
    m_impl->lastError.clear();
    return true;
}

bool TrtRtxDepthEngine::IsReady() const { return m_impl && m_impl->context != nullptr; }
const std::string& TrtRtxDepthEngine::LastError() const { static const std::string e; return m_impl ? m_impl->lastError : e; }
const std::string& TrtRtxDepthEngine::ModelSummary() const { static const std::string e; return m_impl ? m_impl->summary : e; }
