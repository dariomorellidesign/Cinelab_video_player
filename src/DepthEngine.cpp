#include "DepthEngine.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

// ONNX Runtime exposes this factory function from the DirectML build.  Declaring
// it here keeps the probe independent from DirectML headers/import libraries;
// the actual DirectML runtime DLL is staged next to the executable.
extern "C" OrtStatus* ORT_API_CALL OrtSessionOptionsAppendExecutionProvider_DML(
    OrtSessionOptions* options, int device_id);

namespace {
using Clock = std::chrono::steady_clock;

static const char* TensorTypeName(ONNXTensorElementDataType type) {
    switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: return "float32";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: return "float16";
    default: return "unsupported";
    }
}

static std::string ShapeText(const std::vector<int64_t>& shape) {
    std::ostringstream s;
    s << '[';
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i) s << ',';
        s << shape[i];
    }
    s << ']';
    return s.str();
}

static uint32_t ConstrainToMultiple(float value, uint32_t multiple) {
    if (multiple == 0) return std::max(1u, static_cast<uint32_t>(std::lround(value)));
    const auto rounded = static_cast<int64_t>(std::llround(value / static_cast<float>(multiple))) * multiple;
    return static_cast<uint32_t>(std::max<int64_t>(multiple, rounded));
}

static std::pair<uint32_t, uint32_t> DynamicDepthInputSize(uint32_t sourceW, uint32_t sourceH) {
    // Mirrors the important part of DPTImageProcessor for this checkpoint:
    // target 518x518, keep_aspect_ratio=true, ensure_multiple_of=14.
    constexpr float target = 518.0f;
    constexpr uint32_t multiple = 14;
    const float scaleH = target / static_cast<float>(sourceH);
    const float scaleW = target / static_cast<float>(sourceW);
    float chosenH = scaleH;
    float chosenW = scaleW;
    if (std::fabs(1.0f - scaleW) < std::fabs(1.0f - scaleH)) {
        chosenH = scaleW;
    } else {
        chosenW = scaleH;
    }
    const uint32_t h = ConstrainToMultiple(chosenH * static_cast<float>(sourceH), multiple);
    const uint32_t w = ConstrainToMultiple(chosenW * static_cast<float>(sourceW), multiple);
    return {w, h};
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
                const uint32_t bgraChannel = 2u - c; // RGB from BGRA
                const float a = static_cast<float>(p00[bgraChannel]) * (1.0f - fx) +
                                static_cast<float>(p10[bgraChannel]) * fx;
                const float b = static_cast<float>(p01[bgraChannel]) * (1.0f - fx) +
                                static_cast<float>(p11[bgraChannel]) * fx;
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

struct DepthEngine::Impl {
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::string inputName;
    std::string outputName;
    std::vector<int64_t> declaredInputShape;
    ONNXTensorElementDataType inputType = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    ONNXTensorElementDataType outputType = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    std::string lastError;
    std::string summary;
    int deviceId = 0;

    void Fail(const std::string& message) {
        lastError = message;
    }
};

DepthEngine::DepthEngine() : m_impl(std::make_unique<Impl>()) {}
DepthEngine::~DepthEngine() = default;

bool DepthEngine::Initialize(const std::wstring& modelPath, int dmlDeviceId) {
    m_impl = std::make_unique<Impl>();
    m_impl->deviceId = dmlDeviceId;
    try {
        if (modelPath.empty() || !std::filesystem::exists(modelPath)) {
            m_impl->Fail("Depth model file does not exist.");
            return false;
        }

        m_impl->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "DMPDepth");
        Ort::SessionOptions options;
        options.DisableMemPattern();
        options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(options, dmlDeviceId));

        m_impl->session = std::make_unique<Ort::Session>(*m_impl->env, modelPath.c_str(), options);
        if (m_impl->session->GetInputCount() != 1 || m_impl->session->GetOutputCount() < 1) {
            m_impl->Fail("Unexpected model I/O count; expected one image input and at least one depth output.");
            return false;
        }

        Ort::AllocatorWithDefaultOptions allocator;
        auto inputName = m_impl->session->GetInputNameAllocated(0, allocator);
        auto outputName = m_impl->session->GetOutputNameAllocated(0, allocator);
        m_impl->inputName = inputName.get() ? inputName.get() : "";
        m_impl->outputName = outputName.get() ? outputName.get() : "";

        // Keep the owning TypeInfo objects alive while using the unowned
        // ConstTensorTypeAndShapeInfo views returned by GetTensorTypeAndShapeInfo().
        // Chaining GetInputTypeInfo(...).GetTensorTypeAndShapeInfo() creates a
        // dangling view as soon as the temporary TypeInfo is destroyed at the end
        // of the full expression, which can make rank/element-type metadata look
        // random even though the ONNX graph itself is valid.
        auto inputTypeInfo = m_impl->session->GetInputTypeInfo(0);
        auto outputTypeInfo = m_impl->session->GetOutputTypeInfo(0);
        auto inputInfo = inputTypeInfo.GetTensorTypeAndShapeInfo();
        auto outputInfo = outputTypeInfo.GetTensorTypeAndShapeInfo();
        m_impl->declaredInputShape = inputInfo.GetShape();
        m_impl->inputType = inputInfo.GetElementType();
        m_impl->outputType = outputInfo.GetElementType();
        const auto outputShape = outputInfo.GetShape();

        // Some ONNX exports intentionally leave the input rank unspecified even
        // though the graph accepts the standard Depth Anything NCHW image tensor.
        // Treat an empty shape as dynamic/unknown and validate the concrete tensor
        // we create at inference time.  A non-empty, non-rank-4 declaration is
        // still rejected because that would be a genuinely different model contract.
        if (!m_impl->declaredInputShape.empty() && m_impl->declaredInputShape.size() != 4) {
            m_impl->Fail(std::string("Depth model input rank is incompatible with NCHW: declared shape=") +
                         ShapeText(m_impl->declaredInputShape));
            return false;
        }
        if (m_impl->declaredInputShape.size() == 4) {
            if (m_impl->declaredInputShape[0] > 0 && m_impl->declaredInputShape[0] != 1) {
                m_impl->Fail("Depth model batch dimension must be 1 for this probe.");
                return false;
            }
            if (m_impl->declaredInputShape[1] > 0 && m_impl->declaredInputShape[1] != 3) {
                m_impl->Fail("Depth model channel dimension must be RGB=3.");
                return false;
            }
        }
        if (m_impl->inputType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
            m_impl->inputType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            m_impl->Fail(std::string("Depth model input tensor type is neither float32 nor float16; enum=") +
                         std::to_string(static_cast<int>(m_impl->inputType)) + ".");
            return false;
        }
        if (m_impl->outputType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT &&
            m_impl->outputType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            m_impl->Fail(std::string("Depth model output tensor type is neither float32 nor float16; enum=") +
                         std::to_string(static_cast<int>(m_impl->outputType)) + ".");
            return false;
        }

        std::ostringstream s;
        s << "DirectML device=" << dmlDeviceId
          << " input=" << m_impl->inputName << ' ' << ShapeText(m_impl->declaredInputShape)
          << ' ' << TensorTypeName(m_impl->inputType)
          << " output=" << m_impl->outputName << ' ' << ShapeText(outputShape)
          << ' ' << TensorTypeName(m_impl->outputType);
        m_impl->summary = s.str();
        return true;
    } catch (const Ort::Exception& e) {
        m_impl->Fail(std::string("ONNX Runtime: ") + e.what());
    } catch (const std::exception& e) {
        m_impl->Fail(e.what());
    }
    return false;
}

bool DepthEngine::InferBGRA(const uint8_t* bgra,
                            uint32_t width,
                            uint32_t height,
                            size_t strideBytes,
                            DepthInferenceResult& result) {
    result = {};
    if (!IsReady()) {
        m_impl->Fail("DepthEngine is not initialized.");
        return false;
    }
    if (!bgra || width == 0 || height == 0 || strideBytes < static_cast<size_t>(width) * 4u) {
        m_impl->Fail("Invalid BGRA frame passed to DepthEngine.");
        return false;
    }

    try {
        const auto totalStart = Clock::now();
        uint32_t inputW = 0;
        uint32_t inputH = 0;
        int64_t declaredH = -1;
        int64_t declaredW = -1;
        if (m_impl->declaredInputShape.size() == 4) {
            declaredH = m_impl->declaredInputShape[2];
            declaredW = m_impl->declaredInputShape[3];
        }
        if (declaredH > 0 && declaredW > 0) {
            inputH = static_cast<uint32_t>(declaredH);
            inputW = static_cast<uint32_t>(declaredW);
        } else {
            std::tie(inputW, inputH) = DynamicDepthInputSize(width, height);
        }

        const auto preprocessStart = Clock::now();
        std::vector<float> inputFloat;
        FillNormalizedRGB(bgra, width, height, strideBytes, inputW, inputH, inputFloat);
        const auto preprocessEnd = Clock::now();

        std::array<int64_t, 4> inputShape{1, 3, static_cast<int64_t>(inputH), static_cast<int64_t>(inputW)};
        Ort::MemoryInfo memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const char* inputNames[] = {m_impl->inputName.c_str()};
        const char* outputNames[] = {m_impl->outputName.c_str()};
        std::vector<Ort::Value> outputs;

        const auto inferenceStart = Clock::now();
        if (m_impl->inputType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            auto tensor = Ort::Value::CreateTensor<float>(memoryInfo,
                                                          inputFloat.data(), inputFloat.size(),
                                                          inputShape.data(), inputShape.size());
            outputs = m_impl->session->Run(Ort::RunOptions{nullptr}, inputNames, &tensor, 1, outputNames, 1);
        } else {
            std::vector<Ort::Float16_t> inputHalf;
            inputHalf.reserve(inputFloat.size());
            for (float v : inputFloat) inputHalf.emplace_back(v);
            auto tensor = Ort::Value::CreateTensor<Ort::Float16_t>(memoryInfo,
                                                                    inputHalf.data(), inputHalf.size(),
                                                                    inputShape.data(), inputShape.size());
            outputs = m_impl->session->Run(Ort::RunOptions{nullptr}, inputNames, &tensor, 1, outputNames, 1);
        }
        const auto inferenceEnd = Clock::now();

        if (outputs.empty() || !outputs[0].IsTensor()) {
            m_impl->Fail("Depth model returned no tensor output.");
            return false;
        }

        const auto postStart = Clock::now();
        auto outputInfo = outputs[0].GetTensorTypeAndShapeInfo();
        const auto outputShape = outputInfo.GetShape();
        const size_t count = outputInfo.GetElementCount();
        if (count == 0) {
            m_impl->Fail("Depth model returned an empty tensor.");
            return false;
        }

        uint32_t outputH = 0;
        uint32_t outputW = 0;
        if (outputShape.size() == 3 && outputShape[1] > 0 && outputShape[2] > 0) {
            outputH = static_cast<uint32_t>(outputShape[1]);
            outputW = static_cast<uint32_t>(outputShape[2]);
        } else if (outputShape.size() == 4 && outputShape[2] > 0 && outputShape[3] > 0) {
            outputH = static_cast<uint32_t>(outputShape[2]);
            outputW = static_cast<uint32_t>(outputShape[3]);
        } else {
            m_impl->Fail("Depth model output shape is not [1,H,W] or [1,1,H,W].");
            return false;
        }
        if (static_cast<size_t>(outputW) * outputH != count) {
            m_impl->Fail("Depth model output element count does not match its spatial shape.");
            return false;
        }

        result.width = outputW;
        result.height = outputH;
        result.depth.resize(count);
        const auto actualOutputType = outputInfo.GetElementType();
        if (actualOutputType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            const float* src = outputs[0].GetTensorData<float>();
            std::copy(src, src + count, result.depth.begin());
        } else if (actualOutputType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) {
            const Ort::Float16_t* src = outputs[0].GetTensorData<Ort::Float16_t>();
            for (size_t i = 0; i < count; ++i) result.depth[i] = src[i].ToFloat();
        } else {
            m_impl->Fail("Depth output type changed to an unsupported tensor type at runtime.");
            return false;
        }

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
        if (finite.empty()) {
            m_impl->Fail("Depth output contains no finite values.");
            return false;
        }
        result.percentile02 = Percentile(finite, 0.02);
        result.percentile98 = Percentile(std::move(finite), 0.98);
        const auto postEnd = Clock::now();

        result.preprocessMs = std::chrono::duration<double, std::milli>(preprocessEnd - preprocessStart).count();
        result.inferenceMs = std::chrono::duration<double, std::milli>(inferenceEnd - inferenceStart).count();
        result.postprocessMs = std::chrono::duration<double, std::milli>(postEnd - postStart).count();
        result.totalMs = std::chrono::duration<double, std::milli>(postEnd - totalStart).count();
        m_impl->lastError.clear();
        return true;
    } catch (const Ort::Exception& e) {
        m_impl->Fail(std::string("ONNX Runtime inference: ") + e.what());
    } catch (const std::exception& e) {
        m_impl->Fail(e.what());
    }
    return false;
}

bool DepthEngine::IsReady() const {
    return m_impl && m_impl->session != nullptr;
}

const std::string& DepthEngine::LastError() const {
    static const std::string empty;
    return m_impl ? m_impl->lastError : empty;
}

const std::string& DepthEngine::ModelSummary() const {
    static const std::string empty;
    return m_impl ? m_impl->summary : empty;
}
