#include "AIDepthTemporalWorker.h"
#include "Log.h"

#include <algorithm>
#include <chrono>
#include <utility>

using TemporalClock = std::chrono::steady_clock;

AIDepthTemporalWorker::AIDepthTemporalWorker()
    : m_thread(&AIDepthTemporalWorker::ThreadMain, this) {}

AIDepthTemporalWorker::~AIDepthTemporalWorker() {
    m_stop.store(true, std::memory_order_release);
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void AIDepthTemporalWorker::Reset() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_generation.fetch_add(1, std::memory_order_acq_rel);
        m_queue.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_latestMutex);
        m_latest.reset();
        m_latestTimestamp100ns = -1;
    }
    m_cv.notify_all();
}

bool AIDepthTemporalWorker::Submit(int64_t currentTimestamp100ns,
                                   int64_t previousRenderedTimestamp100ns,
                                   uint32_t sourceW,
                                   uint32_t sourceH,
                                   const AIDepthMotionView* currentToPrevious,
                                   const AIDepthMeasurementView* newMeasurement) {
    if (m_stop.load(std::memory_order_acquire) || !sourceW || !sourceH || currentTimestamp100ns < 0)
        return false;

    Job job;
    job.currentTimestamp100ns = currentTimestamp100ns;
    job.previousRenderedTimestamp100ns = previousRenderedTimestamp100ns;
    job.sourceW = sourceW;
    job.sourceH = sourceH;

    if (currentToPrevious && currentToPrevious->valid && currentToPrevious->motionXY &&
        currentToPrevious->gridW && currentToPrevious->gridH &&
        currentToPrevious->countFloats >= size_t(currentToPrevious->gridW) * currentToPrevious->gridH * 2u) {
        const size_t n = size_t(currentToPrevious->gridW) * currentToPrevious->gridH * 2u;
        job.motionXY.assign(currentToPrevious->motionXY, currentToPrevious->motionXY + n);
        job.motionGridW = currentToPrevious->gridW;
        job.motionGridH = currentToPrevious->gridH;
        job.motionSourceW = currentToPrevious->sourceW;
        job.motionSourceH = currentToPrevious->sourceH;
        job.hasMotion = true;
    }

    if (newMeasurement && newMeasurement->rawDepth && newMeasurement->width && newMeasurement->height &&
        newMeasurement->count >= size_t(newMeasurement->width) * newMeasurement->height) {
        const size_t n = size_t(newMeasurement->width) * newMeasurement->height;
        job.measurementDepth.assign(newMeasurement->rawDepth, newMeasurement->rawDepth + n);
        job.measurementW = newMeasurement->width;
        job.measurementH = newMeasurement->height;
        job.measurementTimestamp100ns = newMeasurement->timestamp100ns;
        job.measurementSequence = newMeasurement->sequence;
        job.measurementP02 = newMeasurement->percentile02;
        job.measurementP98 = newMeasurement->percentile98;
        job.hasMeasurement = true;
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_stop.load(std::memory_order_relaxed)) return false;

        if (m_queue.size() >= MaxQueuedJobs) {
            m_queue.clear();
            job.generation = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1u;
            job.forceReset = true;
            ++m_queueResets;
        } else {
            job.generation = m_generation.load(std::memory_order_acquire);
        }

        m_queue.push_back(std::move(job));
        ++m_submitted;
        m_maxQueueDepth = std::max(m_maxQueueDepth, m_queue.size());
    }
    m_cv.notify_one();
    return true;
}

std::shared_ptr<const AIDepthTemporalFrame> AIDepthTemporalWorker::Latest() const {
    std::lock_guard<std::mutex> lock(m_latestMutex);
    return m_latest;
}

AIDepthTemporalWorkerStats AIDepthTemporalWorker::GetStats() const {
    AIDepthTemporalWorkerStats s;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        s.submitted = m_submitted;
        s.queueResets = m_queueResets;
        s.queueDepth = m_queue.size();
        s.maxQueueDepth = m_maxQueueDepth;
    }
    {
        std::lock_guard<std::mutex> lock(m_latestMutex);
        s.processed = m_processed;
        s.staleRecoveries = m_staleRecoveries;
        s.lastProcessMs = m_lastProcessMs;
        s.emaProcessMs = m_emaProcessMs;
        s.latestTimestamp100ns = m_latestTimestamp100ns;
    }
    return s;
}

void AIDepthTemporalWorker::ThreadMain() {
    AIDepthTemporalStabilizer stabilizer;
    uint64_t localGeneration = 0;
    auto nextLog = TemporalClock::now() + std::chrono::seconds(1);

    while (!m_stop.load(std::memory_order_acquire)) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait(lock, [&] {
                return m_stop.load(std::memory_order_acquire) || !m_queue.empty();
            });
            if (m_stop.load(std::memory_order_acquire)) break;
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }

        if (job.forceReset || job.generation != localGeneration) {
            stabilizer.Reset();
            localGeneration = job.generation;
        }

        AIDepthMotionView motion{};
        const AIDepthMotionView* motionPtr = nullptr;
        if (job.hasMotion && !job.motionXY.empty()) {
            motion.motionXY = job.motionXY.data();
            motion.countFloats = job.motionXY.size();
            motion.gridW = job.motionGridW;
            motion.gridH = job.motionGridH;
            motion.sourceW = job.motionSourceW;
            motion.sourceH = job.motionSourceH;
            motion.valid = true;
            motionPtr = &motion;
        }

        AIDepthMeasurementView measurement{};
        const AIDepthMeasurementView* measurementPtr = nullptr;
        if (job.hasMeasurement && !job.measurementDepth.empty()) {
            measurement.rawDepth = job.measurementDepth.data();
            measurement.count = job.measurementDepth.size();
            measurement.width = job.measurementW;
            measurement.height = job.measurementH;
            measurement.timestamp100ns = job.measurementTimestamp100ns;
            measurement.sequence = job.measurementSequence;
            measurement.percentile02 = job.measurementP02;
            measurement.percentile98 = job.measurementP98;
            measurementPtr = &measurement;
        }

        const auto t0 = TemporalClock::now();
        AIDepthTemporalFrame result;
        bool valid = stabilizer.ProcessFrame(job.currentTimestamp100ns,
                                             job.previousRenderedTimestamp100ns,
                                             job.sourceW,
                                             job.sourceH,
                                             motionPtr,
                                             measurementPtr,
                                             result);
        bool staleRecovered = false;

        // Step 04B-3 fail-soft recovery. If the normal reprojection chain cannot
        // publish for more than 250 ms but a fresh AI measurement exists, snap that
        // measurement to the current frame and restart temporal state. This is debug
        // depth, so a short one-frame spatial snap is preferable to displaying depth
        // that is several seconds old.
        if (!valid && measurementPtr) {
            int64_t lastPublished = -1;
            {
                std::lock_guard<std::mutex> lock(m_latestMutex);
                lastPublished = m_latestTimestamp100ns;
            }
            const bool stale = lastPublished < 0 ||
                job.currentTimestamp100ns - lastPublished > MaxPublishedAge100ns;
            if (stale) {
                stabilizer.Reset();
                AIDepthMeasurementView currentMeasurement = measurement;
                currentMeasurement.timestamp100ns = job.currentTimestamp100ns;
                AIDepthTemporalFrame recovered;
                valid = stabilizer.ProcessFrame(job.currentTimestamp100ns,
                                                -1,
                                                job.sourceW,
                                                job.sourceH,
                                                nullptr,
                                                &currentMeasurement,
                                                recovered);
                if (valid) {
                    result = std::move(recovered);
                    staleRecovered = true;
                }
            }
        }

        const double processMs = std::chrono::duration<double, std::milli>(TemporalClock::now() - t0).count();

        const bool generationStillCurrent =
            m_generation.load(std::memory_order_acquire) == job.generation;
        {
            std::lock_guard<std::mutex> lock(m_latestMutex);
            ++m_processed;
            if (staleRecovered) ++m_staleRecoveries;
            m_lastProcessMs = processMs;
            m_emaProcessMs = (m_processed <= 1u) ? processMs : (m_emaProcessMs * 0.90 + processMs * 0.10);
            if (valid && generationStillCurrent) {
                auto published = std::make_shared<AIDepthTemporalFrame>(std::move(result));
                m_latestTimestamp100ns = published->currentTimestamp100ns;
                m_latest = std::move(published);
            }
        }

        if (staleRecovered) {
            LOG("[AI Temporal Async] stale-depth recovery: currentTs=" << job.currentTimestamp100ns
                << " measurementSeq=" << job.measurementSequence
                << " maxAgeMs=" << (double(MaxPublishedAge100ns) * 1.0e-4));
        }

        const auto now = TemporalClock::now();
        if (now >= nextLog) {
            const auto stats = GetStats();
            LOG("[AI Temporal Async] processMs=" << stats.lastProcessMs
                << " emaMs=" << stats.emaProcessMs
                << " queue=" << stats.queueDepth << "/" << MaxQueuedJobs
                << " maxQueue=" << stats.maxQueueDepth
                << " queueResets=" << stats.queueResets
                << " staleRecoveries=" << stats.staleRecoveries
                << " submitted=" << stats.submitted
                << " processed=" << stats.processed);
            nextLog = now + std::chrono::seconds(1);
        }
    }
}
