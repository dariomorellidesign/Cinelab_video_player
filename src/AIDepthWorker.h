#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct AIDepthFrame {
    uint64_t sequence = 0;
    int64_t timestamp100ns = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> rawDepth;
    std::vector<float> preview01;
    float percentile02 = 0.0f;
    float percentile98 = 0.0f;
    double inferenceMs = 0.0;
    double totalMs = 0.0;
};

class AIDepthWorker {
public:
    AIDepthWorker();
    ~AIDepthWorker();

    AIDepthWorker(const AIDepthWorker&) = delete;
    AIDepthWorker& operator=(const AIDepthWorker&) = delete;

    // Step 04A-2 v1.1: TensorRT-RTX runs in a sidecar process. This keeps CUDA,
    // TensorRT and their JIT context out of the ReShade/RenoDX/NGX player process.
    bool Start(const std::wstring& enginePath, const std::wstring& runtimeCachePath,
               uint32_t maxFrameWidth, uint32_t maxFrameHeight);
    void Stop();
    void Reset();

    // Non-blocking/latest-frame-wins submission. AI depth is intentionally capped
    // near 30 Hz for this debug checkpoint so DLSS/NR retains GPU headroom.
    bool Submit(const uint8_t* bgra,
                size_t bytes,
                uint32_t width,
                uint32_t height,
                size_t strideBytes,
                int64_t timestamp100ns);

    bool GetLatest(uint64_t afterSequence, AIDepthFrame& out) const;

    bool IsReady() const;
    bool Failed() const;
    std::string LastError() const;
    std::string ModelSummary() const;
    uint64_t DroppedPendingFrames() const;

private:
    void CloseHandlesLocked();
    void RefreshStateLocked() const;
    static std::wstring QuoteArg(const std::wstring& value);

    mutable std::mutex m_mutex;
    HANDLE m_mapping = nullptr;
    void* m_view = nullptr;
    HANDLE m_frameEvent = nullptr;
    HANDLE m_resultEvent = nullptr;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_ipcMutex = nullptr;
    PROCESS_INFORMATION m_process{};

    uint64_t m_mappingBytes = 0;
    uint64_t m_generation = 1;
    uint64_t m_submitSequence = 0;
    mutable uint64_t m_lastCopiedSequence = 0;
    uint64_t m_droppedPending = 0;
    int64_t m_lastSubmitTimestamp100ns = -1;

    mutable bool m_started = false;
    mutable bool m_ready = false;
    mutable bool m_failed = false;
    mutable bool m_loggedReady = false;
    mutable bool m_loggedFailure = false;
    mutable std::string m_lastError;
    mutable std::string m_modelSummary;
};
