#include "AIDepthIpc.h"
#include "TrtRtxDepthEngine.h"
#include "Log.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

using namespace DmpAIDepthIpc;

namespace {
std::wstring ArgValue(int argc, wchar_t** argv, const wchar_t* key) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring(argv[i]) == key) return argv[i + 1];
    }
    return {};
}

struct ScopedMutex {
    HANDLE h = nullptr;
    bool owned = false;
    explicit ScopedMutex(HANDLE handle, DWORD timeoutMs = INFINITE) : h(handle) {
        if (!h) return;
        const DWORD r = WaitForSingleObject(h, timeoutMs);
        owned = (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED);
    }
    ~ScopedMutex() { if (owned && h) ReleaseMutex(h); }
};

void CopyText(char* dst, size_t cap, const std::string& text) {
    if (!dst || !cap) return;
    const size_t n = std::min(cap - 1u, text.size());
    if (n) std::memcpy(dst, text.data(), n);
    dst[n] = '\0';
}

void PublishFailure(void* view, HANDLE mutex, HANDLE resultEvent, const std::string& error) {
    ScopedMutex lock(mutex);
    if (lock.owned && view) {
        auto* h = static_cast<SharedHeader*>(view);
        h->childState = Failed;
        CopyText(h->errorText, ErrorTextBytes, error);
    }
    if (resultEvent) SetEvent(resultEvent);
    LOG("[AI Sidecar] FAILED: " << error);
}
}

int wmain(int argc, wchar_t** argv) {
    const std::wstring mapName = ArgValue(argc, argv, L"--map");
    const std::wstring frameEventName = ArgValue(argc, argv, L"--frame-event");
    const std::wstring resultEventName = ArgValue(argc, argv, L"--result-event");
    const std::wstring stopEventName = ArgValue(argc, argv, L"--stop-event");
    const std::wstring mutexName = ArgValue(argc, argv, L"--mutex");
    const std::wstring enginePath = ArgValue(argc, argv, L"--engine");
    const std::wstring cachePath = ArgValue(argc, argv, L"--cache");
    if (mapName.empty() || frameEventName.empty() || resultEventName.empty() ||
        stopEventName.empty() || mutexName.empty() || enginePath.empty()) return 2;

    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapName.c_str());
    HANDLE frameEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, frameEventName.c_str());
    HANDLE resultEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, resultEventName.c_str());
    HANDLE stopEvent = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, stopEventName.c_str());
    HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName.c_str());
    if (!mapping || !frameEvent || !resultEvent || !stopEvent || !mutex) return 3;
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!view) return 4;

    auto cleanup = [&]() {
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
        if (frameEvent) CloseHandle(frameEvent);
        if (resultEvent) CloseHandle(resultEvent);
        if (stopEvent) CloseHandle(stopEvent);
        if (mutex) CloseHandle(mutex);
    };

    bool ipcHeaderValid = false;
    {
        ScopedMutex lock(mutex);
        if (!lock.owned) { cleanup(); return 5; }
        const auto* h = static_cast<const SharedHeader*>(view);
        ipcHeaderValid = h->magic == Magic && h->version == Version && h->inputCapacityBytes != 0 &&
            h->outputCapacityFloats >= uint64_t(OutputWidth) * OutputHeight;
    }
    if (!ipcHeaderValid) { cleanup(); return 6; }

    LOG("[AI Sidecar] starting TensorRT-RTX in isolated process.");
    TrtRtxDepthEngine engine;
    if (!engine.Initialize(enginePath, cachePath)) {
        PublishFailure(view, mutex, resultEvent, engine.LastError());
        cleanup();
        return 7;
    }
    {
        ScopedMutex lock(mutex);
        if (!lock.owned) { cleanup(); return 8; }
        auto* h = static_cast<SharedHeader*>(view);
        h->childState = Ready;
        CopyText(h->summaryText, SummaryTextBytes, engine.ModelSummary());
        h->errorText[0] = '\0';
    }
    LOG("[AI Sidecar] ready: " << engine.ModelSummary());

    uint64_t consumedSequence = 0;
    HANDLE waits[2] = { stopEvent, frameEvent };
    for (;;) {
        const DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0) break;
        if (wr != WAIT_OBJECT_0 + 1) {
            PublishFailure(view, mutex, resultEvent, "WaitForMultipleObjects failed in AI sidecar.");
            cleanup();
            return 9;
        }

        std::vector<uint8_t> bgra;
        uint32_t width = 0, height = 0;
        size_t stride = 0;
        int64_t timestamp100ns = 0;
        uint64_t sequence = 0, generation = 0;
        {
            ScopedMutex lock(mutex);
            if (!lock.owned) continue;
            const auto* h = static_cast<const SharedHeader*>(view);
            if (h->frameSequence == 0 || h->frameSequence <= consumedSequence ||
                h->frameBytes == 0 || h->frameBytes > h->inputCapacityBytes) continue;
            sequence = h->frameSequence;
            generation = h->frameGeneration;
            width = h->frameWidth;
            height = h->frameHeight;
            stride = static_cast<size_t>(h->frameStrideBytes);
            timestamp100ns = h->frameTimestamp100ns;
            bgra.assign(InputBytes(view), InputBytes(view) + static_cast<size_t>(h->frameBytes));
        }
        consumedSequence = sequence;

        TrtDepthInferenceResult result;
        if (!engine.InferBGRA(bgra.data(), width, height, stride, result)) {
            PublishFailure(view, mutex, resultEvent, engine.LastError());
            cleanup();
            return 10;
        }
        if (result.width != OutputWidth || result.height != OutputHeight ||
            result.depth.size() != size_t(OutputWidth) * OutputHeight) {
            PublishFailure(view, mutex, resultEvent, "Unexpected TensorRT-RTX depth output shape in sidecar.");
            cleanup();
            return 11;
        }

        {
            ScopedMutex lock(mutex);
            if (!lock.owned) continue;
            auto* h = static_cast<SharedHeader*>(view);
            std::memcpy(OutputDepth(view, *h), result.depth.data(), result.depth.size() * sizeof(float));
            h->resultSequence = sequence;
            h->resultGeneration = generation;
            h->resultTimestamp100ns = timestamp100ns;
            h->resultWidth = result.width;
            h->resultHeight = result.height;
            h->percentile02 = result.percentile02;
            h->percentile98 = result.percentile98;
            h->inferenceMs = result.inferenceMs;
            h->totalMs = result.totalMs;
            h->childState = Ready;
        }
        SetEvent(resultEvent);
        if ((sequence % 120u) == 0u) {
            LOG("[AI Sidecar] seq=" << sequence << " infer=" << result.inferenceMs
                << " ms total=" << result.totalMs << " ms");
        }
    }

    {
        ScopedMutex lock(mutex, 250);
        if (lock.owned) static_cast<SharedHeader*>(view)->childState = Stopped;
    }
    LOG("[AI Sidecar] clean shutdown.");
    cleanup();
    return 0;
}
