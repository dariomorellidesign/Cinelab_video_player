#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <mfapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

#include "DepthEngine.h"
#include "VideoDecoder.h"

using Microsoft::WRL::ComPtr;

namespace {
std::filesystem::path ExeDirectory() {
    wchar_t path[32768]{};
    const DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (!n || n >= std::size(path)) return std::filesystem::current_path();
    return std::filesystem::path(path).parent_path();
}

float SampleDepth(const DepthInferenceResult& depth, float x, float y) {
    if (depth.width == 0 || depth.height == 0 || depth.depth.empty()) return 0.0f;
    x = std::clamp(x, 0.0f, static_cast<float>(depth.width - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(depth.height - 1));
    const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
    const uint32_t x1 = std::min(x0 + 1, depth.width - 1);
    const uint32_t y1 = std::min(y0 + 1, depth.height - 1);
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    const auto at = [&](uint32_t xx, uint32_t yy) {
        return depth.depth[static_cast<size_t>(yy) * depth.width + xx];
    };
    const float a = at(x0, y0) * (1.0f - fx) + at(x1, y0) * fx;
    const float b = at(x0, y1) * (1.0f - fx) + at(x1, y1) * fx;
    return a * (1.0f - fy) + b * fy;
}

std::vector<uint8_t> BuildDepthPreview(const DepthInferenceResult& depth,
                                       uint32_t outW,
                                       uint32_t outH) {
    std::vector<uint8_t> pixels(static_cast<size_t>(outW) * outH, 0);
    float lo = depth.percentile02;
    float hi = depth.percentile98;
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo + 1e-8f) {
        lo = depth.minValue;
        hi = depth.maxValue;
    }
    const float denom = std::max(hi - lo, 1e-8f);
    const float sx = static_cast<float>(depth.width) / static_cast<float>(outW);
    const float sy = static_cast<float>(depth.height) / static_cast<float>(outH);
    for (uint32_t y = 0; y < outH; ++y) {
        for (uint32_t x = 0; x < outW; ++x) {
            const float dx = (static_cast<float>(x) + 0.5f) * sx - 0.5f;
            const float dy = (static_cast<float>(y) + 0.5f) * sy - 0.5f;
            float value = SampleDepth(depth, dx, dy);
            if (!std::isfinite(value)) value = lo;
            const float normalized = std::clamp((value - lo) / denom, 0.0f, 1.0f);
            pixels[static_cast<size_t>(y) * outW + x] = static_cast<uint8_t>(std::lround(normalized * 255.0f));
        }
    }
    return pixels;
}

bool SaveGrayPng(const std::filesystem::path& path,
                 uint32_t width,
                 uint32_t height,
                 const std::vector<uint8_t>& pixels) {
    if (pixels.size() < static_cast<size_t>(width) * height) return false;
    DeleteFileW(path.c_str());

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if (FAILED(encoder->CreateNewFrame(&frame, &properties))) return false;
    if (FAILED(frame->Initialize(properties.Get()))) return false;
    if (FAILED(frame->SetSize(width, height))) return false;
    WICPixelFormatGUID format = GUID_WICPixelFormat8bppGray;
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat8bppGray) return false;
    if (FAILED(frame->WritePixels(height, width, static_cast<UINT>(pixels.size()),
                                  const_cast<BYTE*>(pixels.data())))) return false;
    if (FAILED(frame->Commit())) return false;
    return SUCCEEDED(encoder->Commit());
}

void PrintUsage() {
    std::wcout << L"Usage:\n"
               << L"  DMPDepthProbe.exe <video-path> [seconds] [output.png]\n\n"
               << L"Example:\n"
               << L"  DMPDepthProbe.exe C:\\video\\test.mp4 5 C:\\temp\\depth.png\n";
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    const std::filesystem::path videoPath = argv[1];
    const double seconds = argc >= 3 ? std::max(0.0, _wtof(argv[2])) : 1.0;
    const auto exeDir = ExeDirectory();
    const std::filesystem::path modelPath = exeDir / L"models" / L"depth_anything_v2_small_fp16.onnx";
    const std::filesystem::path outputPath = argc >= 4 ? std::filesystem::path(argv[3]) : (exeDir / L"depth_probe.png");

    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) {
        std::wcerr << L"[FAIL] CoInitializeEx hr=0x" << std::hex << static_cast<unsigned long>(co) << L"\n";
        return 3;
    }
    const bool shouldUninit = SUCCEEDED(co);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        std::wcerr << L"[FAIL] Media Foundation startup failed.\n";
        if (shouldUninit) CoUninitialize();
        return 4;
    }

    int exitCode = 0;
    {
        std::wcout << L"DMP Depth Probe - Step 04A-1\n";
        std::wcout << L"Video: " << videoPath.c_str() << L"\n";
        std::wcout << L"Sample time: " << std::fixed << std::setprecision(3) << seconds << L" s\n";
        std::wcout << L"Model: " << modelPath.c_str() << L"\n\n";

        VideoDecoder decoder;
        if (!decoder.Open(videoPath.wstring())) {
            std::wcerr << L"[FAIL] Could not open video. See CineLabVideoPlayer.log if FFmpeg/MF emitted details.\n";
            exitCode = 5;
        } else {
            if (seconds > 0.0 && !decoder.SeekSeconds(seconds)) {
                std::wcout << L"[WARN] Seek failed; using the first decodable frame instead.\n";
            }
            VideoFrame frame;
            if (!decoder.ReadNext(frame) || frame.bgra.size() < static_cast<size_t>(decoder.Width()) * decoder.Height() * 4u) {
                std::wcerr << L"[FAIL] No BGRA frame decoded.\n";
                exitCode = 6;
            } else {
                std::wcout << L"[PASS] Decoded " << decoder.Width() << L"x" << decoder.Height()
                           << L" @ " << decoder.FrameRate() << L" fps via " << decoder.BackendName() << L"\n";

                DepthEngine depthEngine;
                if (!depthEngine.Initialize(modelPath.wstring(), 0)) {
                    std::cerr << "[FAIL] DepthEngine init: " << depthEngine.LastError() << "\n";
                    exitCode = 7;
                } else {
                    std::cout << "[PASS] " << depthEngine.ModelSummary() << "\n";
                    DepthInferenceResult warmup;
                    if (!depthEngine.InferBGRA(frame.bgra.data(), decoder.Width(), decoder.Height(),
                                               static_cast<size_t>(decoder.Width()) * 4u, warmup)) {
                        std::cerr << "[FAIL] Warmup inference: " << depthEngine.LastError() << "\n";
                        exitCode = 8;
                    } else {
                        std::cout << std::fixed << std::setprecision(2)
                                  << "[INFO] Warmup: preprocess=" << warmup.preprocessMs
                                  << " ms infer=" << warmup.inferenceMs
                                  << " ms post=" << warmup.postprocessMs
                                  << " ms total=" << warmup.totalMs << " ms\n";

                        constexpr int measuredRuns = 3;
                        std::vector<double> inferenceTimes;
                        std::vector<double> totalTimes;
                        DepthInferenceResult result;
                        for (int i = 0; i < measuredRuns; ++i) {
                            if (!depthEngine.InferBGRA(frame.bgra.data(), decoder.Width(), decoder.Height(),
                                                       static_cast<size_t>(decoder.Width()) * 4u, result)) {
                                std::cerr << "[FAIL] Timed inference: " << depthEngine.LastError() << "\n";
                                exitCode = 9;
                                break;
                            }
                            inferenceTimes.push_back(result.inferenceMs);
                            totalTimes.push_back(result.totalMs);
                            std::cout << "[RUN " << (i + 1) << "] infer=" << result.inferenceMs
                                      << " ms total=" << result.totalMs << " ms\n";
                        }

                        if (exitCode == 0) {
                            const double avgInference = std::accumulate(inferenceTimes.begin(), inferenceTimes.end(), 0.0) / inferenceTimes.size();
                            const double avgTotal = std::accumulate(totalTimes.begin(), totalTimes.end(), 0.0) / totalTimes.size();
                            const double depthOnlyFps = avgTotal > 0.0 ? 1000.0 / avgTotal : 0.0;
                            std::cout << "[RESULT] output=" << result.width << 'x' << result.height
                                      << " rawMin=" << result.minValue << " rawMax=" << result.maxValue
                                      << " p02=" << result.percentile02 << " p98=" << result.percentile98 << "\n";
                            std::cout << "[RESULT] average inference=" << avgInference
                                      << " ms average total=" << avgTotal
                                      << " ms depth-only-equivalent=" << depthOnlyFps << " fps\n";

                            std::error_code ec;
                            if (outputPath.has_parent_path()) std::filesystem::create_directories(outputPath.parent_path(), ec);
                            auto preview = BuildDepthPreview(result, decoder.Width(), decoder.Height());
                            if (!SaveGrayPng(outputPath, decoder.Width(), decoder.Height(), preview)) {
                                std::wcerr << L"[FAIL] Could not save depth PNG: " << outputPath.c_str() << L"\n";
                                exitCode = 10;
                            } else {
                                std::wcout << L"[PASS] Depth preview saved: " << outputPath.c_str() << L"\n";
                                std::cout << "[NOTE] Preview uses robust per-frame normalization; brighter = larger relative inverse-depth response.\n";
                                std::cout << "STEP04A1_PROBE=PASS\n";
                            }
                        }
                    }
                }
            }
        }
    }

    MFShutdown();
    if (shouldUninit) CoUninitialize();
    return exitCode;
}
