#pragma once

#include "AIDepthTemporal.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct AIDepthTemporalWorkerStats {
    uint64_t submitted = 0;
    uint64_t processed = 0;
    uint64_t queueResets = 0;
    uint64_t staleRecoveries = 0;
    size_t queueDepth = 0;
    size_t maxQueueDepth = 0;
    double lastProcessMs = 0.0;
    double emaProcessMs = 0.0;
    int64_t latestTimestamp100ns = -1;
};

// Step 04B-2/04B-3: owns AIDepthTemporalStabilizer on a dedicated CPU thread.
// Submit() only copies the compact NVOFA field / occasional AI measurement and
// returns immediately. The render thread never waits for reprojection, affine fit,
// temporal blending, percentiles, or preview normalization.
class AIDepthTemporalWorker {
public:
    AIDepthTemporalWorker();
    ~AIDepthTemporalWorker();

    AIDepthTemporalWorker(const AIDepthTemporalWorker&) = delete;
    AIDepthTemporalWorker& operator=(const AIDepthTemporalWorker&) = delete;

    void Reset();

    bool Submit(int64_t currentTimestamp100ns,
                int64_t previousRenderedTimestamp100ns,
                uint32_t sourceW,
                uint32_t sourceH,
                const AIDepthMotionView* currentToPrevious,
                const AIDepthMeasurementView* newMeasurement);

    std::shared_ptr<const AIDepthTemporalFrame> Latest() const;
    AIDepthTemporalWorkerStats GetStats() const;

private:
    struct Job {
        uint64_t generation = 0;
        bool forceReset = false;
        int64_t currentTimestamp100ns = 0;
        int64_t previousRenderedTimestamp100ns = -1;
        uint32_t sourceW = 0;
        uint32_t sourceH = 0;

        bool hasMotion = false;
        std::vector<float> motionXY;
        uint32_t motionGridW = 0;
        uint32_t motionGridH = 0;
        uint32_t motionSourceW = 0;
        uint32_t motionSourceH = 0;

        bool hasMeasurement = false;
        std::vector<float> measurementDepth;
        uint32_t measurementW = 0;
        uint32_t measurementH = 0;
        int64_t measurementTimestamp100ns = 0;
        uint64_t measurementSequence = 0;
        float measurementP02 = 0.0f;
        float measurementP98 = 0.0f;
    };

    void ThreadMain();

    static constexpr size_t MaxQueuedJobs = 12;
    static constexpr int64_t MaxPublishedAge100ns = 2500000; // 250 ms

    mutable std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::deque<Job> m_queue;
    std::atomic<bool> m_stop{false};
    std::atomic<uint64_t> m_generation{1};
    std::thread m_thread;

    uint64_t m_submitted = 0;
    uint64_t m_queueResets = 0;
    size_t m_maxQueueDepth = 0;

    mutable std::mutex m_latestMutex;
    std::shared_ptr<const AIDepthTemporalFrame> m_latest;
    uint64_t m_processed = 0;
    uint64_t m_staleRecoveries = 0;
    double m_lastProcessMs = 0.0;
    double m_emaProcessMs = 0.0;
    int64_t m_latestTimestamp100ns = -1;
};
