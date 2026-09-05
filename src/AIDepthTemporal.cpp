#include "AIDepthTemporal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr float kEps = 1.0e-6f;

static float Clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

static float BilinearComponent(const float* xy, uint32_t gw, uint32_t gh,
                               float gx, float gy, uint32_t component) {
    gx = std::clamp(gx, 0.0f, static_cast<float>(gw - 1));
    gy = std::clamp(gy, 0.0f, static_cast<float>(gh - 1));
    const uint32_t x0 = static_cast<uint32_t>(std::floor(gx));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(gy));
    const uint32_t x1 = std::min(gw - 1, x0 + 1);
    const uint32_t y1 = std::min(gh - 1, y0 + 1);
    const float tx = gx - static_cast<float>(x0);
    const float ty = gy - static_cast<float>(y0);
    auto at = [&](uint32_t x, uint32_t y) {
        return xy[(size_t(y) * gw + x) * 2u + component];
    };
    const float a = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
    const float b = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
    return a + (b - a) * ty;
}

static float Percentile(std::vector<float>& values, float fraction) {
    if (values.empty()) return 0.0f;
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    const size_t i = static_cast<size_t>(std::llround(
        static_cast<double>(fraction) * static_cast<double>(values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(i), values.end());
    return values[i];
}
}

void AIDepthTemporalStabilizer::Reset() {
    m_flowHistory.clear();
    m_state = {};
    m_sourceW = m_sourceH = 0;
    m_normInitialized = false;
    m_normLo = 0.0f;
    m_normHi = 1.0f;
}

float AIDepthTemporalStabilizer::SyntheticHardwareDepthFromRelative01(float relativeNearness01) {
    return 1.0f - Clamp01(relativeNearness01);
}
bool AIDepthTemporalStabilizer::AppendFlowStep(int64_t previousTimestamp100ns,
                                               int64_t currentTimestamp100ns,
                                               uint32_t sourceW,
                                               uint32_t sourceH,
                                               const AIDepthMotionView& motion) {
    if (!motion.valid || !motion.motionXY || motion.gridW == 0 || motion.gridH == 0 ||
        motion.sourceW != sourceW || motion.sourceH != sourceH ||
        motion.countFloats < size_t(motion.gridW) * motion.gridH * 2u ||
        previousTimestamp100ns < 0 || currentTimestamp100ns <= previousTimestamp100ns) {
        return false;
    }

    FlowStep step;
    step.previousTimestamp100ns = previousTimestamp100ns;
    step.currentTimestamp100ns = currentTimestamp100ns;
    step.motionDepthXY.resize(PixelCount * 2u);
    step.confidence.assign(PixelCount, 1.0f);

    const float sourceToDepthX = static_cast<float>(DepthW) / static_cast<float>(sourceW);
    const float sourceToDepthY = static_cast<float>(DepthH) / static_cast<float>(sourceH);
    for (uint32_t y = 0; y < DepthH; ++y) {
        const float gy = (static_cast<float>(y) + 0.5f) *
            static_cast<float>(motion.gridH) / static_cast<float>(DepthH) - 0.5f;
        for (uint32_t x = 0; x < DepthW; ++x) {
            const float gx = (static_cast<float>(x) + 0.5f) *
                static_cast<float>(motion.gridW) / static_cast<float>(DepthW) - 0.5f;
            const size_t i = size_t(y) * DepthW + x;
            const float mx = BilinearComponent(motion.motionXY, motion.gridW, motion.gridH, gx, gy, 0u);
            const float my = BilinearComponent(motion.motionXY, motion.gridW, motion.gridH, gx, gy, 1u);
            step.motionDepthXY[i * 2u + 0u] = mx * sourceToDepthX;
            step.motionDepthXY[i * 2u + 1u] = my * sourceToDepthY;
        }
    }

    // Low confidence near motion discontinuities. Until forward/backward OF cost is
    // available, this is a conservative proxy for boundaries/disocclusion regions.
    for (uint32_t y = 0; y < DepthH; ++y) {
        for (uint32_t x = 0; x < DepthW; ++x) {
            const size_t i = size_t(y) * DepthW + x;
            const float mx = step.motionDepthXY[i * 2u + 0u];
            const float my = step.motionDepthXY[i * 2u + 1u];
            float grad = 0.0f;
            if (x + 1u < DepthW) {
                const size_t j = i + 1u;
                const float dx = step.motionDepthXY[j * 2u + 0u] - mx;
                const float dy = step.motionDepthXY[j * 2u + 1u] - my;
                grad = std::max(grad, std::sqrt(dx * dx + dy * dy));
            }
            if (y + 1u < DepthH) {
                const size_t j = i + DepthW;
                const float dx = step.motionDepthXY[j * 2u + 0u] - mx;
                const float dy = step.motionDepthXY[j * 2u + 1u] - my;
                grad = std::max(grad, std::sqrt(dx * dx + dy * dy));
            }
            step.confidence[i] = 1.0f / (1.0f + 1.35f * grad);
        }
    }

    if (!m_flowHistory.empty() &&
        m_flowHistory.back().currentTimestamp100ns == currentTimestamp100ns) {
        m_flowHistory.back() = std::move(step);
    } else {
        m_flowHistory.push_back(std::move(step));
    }
    while (m_flowHistory.size() > MaxFlowHistory) m_flowHistory.pop_front();
    return true;
}

bool AIDepthTemporalStabilizer::WarpOne(const Map& src, const FlowStep& step, Map& dst) const {
    if (src.values.size() != PixelCount || src.valid.size() != PixelCount ||
        src.confidence.size() != PixelCount || step.motionDepthXY.size() != PixelCount * 2u ||
        step.confidence.size() != PixelCount || src.timestamp100ns != step.previousTimestamp100ns) {
        return false;
    }

    dst.values.assign(PixelCount, 0.0f);
    dst.valid.assign(PixelCount, 0u);
    dst.confidence.assign(PixelCount, 0.0f);
    dst.timestamp100ns = step.currentTimestamp100ns;

    auto sample = [&](float sx, float sy, float& value, float& confidence) -> bool {
        if (sx < 0.0f || sy < 0.0f || sx > static_cast<float>(DepthW - 1) ||
            sy > static_cast<float>(DepthH - 1)) return false;
        const uint32_t x0 = static_cast<uint32_t>(std::floor(sx));
        const uint32_t y0 = static_cast<uint32_t>(std::floor(sy));
        const uint32_t x1 = std::min(DepthW - 1, x0 + 1u);
        const uint32_t y1 = std::min(DepthH - 1, y0 + 1u);
        const float tx = sx - static_cast<float>(x0);
        const float ty = sy - static_cast<float>(y0);
        const uint32_t xs[4] = {x0, x1, x0, x1};
        const uint32_t ys[4] = {y0, y0, y1, y1};
        const float ws[4] = {(1.0f - tx) * (1.0f - ty), tx * (1.0f - ty),
                             (1.0f - tx) * ty, tx * ty};
        float sumW = 0.0f, sumV = 0.0f, sumC = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const size_t j = size_t(ys[k]) * DepthW + xs[k];
            if (!src.valid[j] || !std::isfinite(src.values[j])) continue;
            sumW += ws[k];
            sumV += ws[k] * src.values[j];
            sumC += ws[k] * src.confidence[j];
        }
        if (sumW < 0.50f) return false;
        value = sumV / sumW;
        confidence = sumC / sumW;
        return std::isfinite(value);
    };

    for (uint32_t y = 0; y < DepthH; ++y) {
        for (uint32_t x = 0; x < DepthW; ++x) {
            const size_t i = size_t(y) * DepthW + x;
            const float sx = static_cast<float>(x) + step.motionDepthXY[i * 2u + 0u];
            const float sy = static_cast<float>(y) + step.motionDepthXY[i * 2u + 1u];
            float v = 0.0f, c = 0.0f;
            if (!sample(sx, sy, v, c)) continue;
            dst.values[i] = v;
            dst.valid[i] = 1u;
            dst.confidence[i] = Clamp01(c * step.confidence[i] * 0.985f);
        }
    }
    return true;
}

bool AIDepthTemporalStabilizer::WarpAcrossHistory(const Map& src,
                                                  int64_t targetTimestamp100ns,
                                                  Map& dst,
                                                  uint32_t& steps) const {
    steps = 0;
    if (src.values.size() != PixelCount || src.timestamp100ns > targetTimestamp100ns) return false;
    if (src.timestamp100ns == targetTimestamp100ns) { dst = src; return true; }

    Map cur = src;
    while (cur.timestamp100ns < targetTimestamp100ns) {
        const FlowStep* found = nullptr;
        for (const auto& step : m_flowHistory) {
            if (step.previousTimestamp100ns == cur.timestamp100ns) {
                found = &step;
                break;
            }
        }
        if (!found || found->currentTimestamp100ns > targetTimestamp100ns) return false;
        Map next;
        if (!WarpOne(cur, *found, next)) return false;
        cur = std::move(next);
        ++steps;
        if (steps > MaxFlowHistory) return false;
    }
    if (cur.timestamp100ns != targetTimestamp100ns) return false;
    dst = std::move(cur);
    return true;
}

bool AIDepthTemporalStabilizer::MeasurementToMap(const AIDepthMeasurementView& measurement,
                                                 Map& out) const {
    if (!measurement.rawDepth || measurement.width != DepthW || measurement.height != DepthH ||
        measurement.count < PixelCount) return false;
    out.values.assign(measurement.rawDepth, measurement.rawDepth + PixelCount);
    out.valid.assign(PixelCount, 0u);
    out.confidence.assign(PixelCount, 1.0f);
    out.timestamp100ns = measurement.timestamp100ns;
    size_t valid = 0;
    for (size_t i = 0; i < PixelCount; ++i) {
        if (std::isfinite(out.values[i])) { out.valid[i] = 1u; ++valid; }
        else { out.values[i] = 0.0f; out.confidence[i] = 0.0f; }
    }
    return valid > PixelCount / 2u;
}

bool AIDepthTemporalStabilizer::ComputePercentiles(const Map& map,
                                                   float lowFraction,
                                                   float highFraction,
                                                   float& low,
                                                   float& high) {
    if (map.values.size() != PixelCount || map.valid.size() != PixelCount) return false;
    std::vector<float> sample;
    sample.reserve(PixelCount / 4u + 1u);
    for (uint32_t y = 0; y < DepthH; y += 2u) {
        for (uint32_t x = 0; x < DepthW; x += 2u) {
            const size_t i = size_t(y) * DepthW + x;
            if (map.valid[i] && std::isfinite(map.values[i])) sample.push_back(map.values[i]);
        }
    }
    if (sample.size() < 128u) return false;
    std::vector<float> a = sample;
    std::vector<float> b = sample;
    low = Percentile(a, lowFraction);
    high = Percentile(b, highFraction);
    return std::isfinite(low) && std::isfinite(high) && high > low + kEps;
}

bool AIDepthTemporalStabilizer::RobustAffine(const Map& measurement,
                                             const Map& prediction,
                                             float& scale,
                                             float& shift) const {
    if (measurement.values.size() != PixelCount || prediction.values.size() != PixelCount) return false;

    auto solve = [&](float residualLimit, float seedScale, float seedShift,
                     float& outScale, float& outShift, size_t& outCount) -> bool {
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        size_t n = 0;
        for (uint32_t y = 2u; y + 2u < DepthH; y += 4u) {
            for (uint32_t x = 2u; x + 2u < DepthW; x += 4u) {
                const size_t i = size_t(y) * DepthW + x;
                if (!measurement.valid[i] || !prediction.valid[i] ||
                    prediction.confidence[i] < 0.20f) continue;
                const float xv = measurement.values[i];
                const float yv = prediction.values[i];
                if (!std::isfinite(xv) || !std::isfinite(yv)) continue;
                if (std::isfinite(residualLimit)) {
                    const float r = std::fabs((seedScale * xv + seedShift) - yv);
                    if (r > residualLimit) continue;
                }
                sx += xv; sy += yv; sxx += double(xv) * xv; sxy += double(xv) * yv; ++n;
            }
        }
        outCount = n;
        if (n < 256u) return false;
        const double den = double(n) * sxx - sx * sx;
        if (std::fabs(den) < 1.0e-10) return false;
        const double a = (double(n) * sxy - sx * sy) / den;
        const double b = (sy - a * sx) / double(n);
        if (!std::isfinite(a) || !std::isfinite(b)) return false;
        outScale = std::clamp(static_cast<float>(a), 0.20f, 5.0f);
        outShift = static_cast<float>(b);
        return true;
    };

    size_t n0 = 0;
    float a0 = 1.0f, b0 = 0.0f;
    if (!solve(std::numeric_limits<float>::infinity(), 1.0f, 0.0f, a0, b0, n0)) return false;

    float predLo = 0.0f, predHi = 1.0f;
    if (!ComputePercentiles(prediction, 0.05f, 0.95f, predLo, predHi)) return false;
    const float span = std::max(predHi - predLo, 1.0e-3f);

    std::vector<float> residuals;
    residuals.reserve(n0);
    for (uint32_t y = 2u; y + 2u < DepthH; y += 4u) {
        for (uint32_t x = 2u; x + 2u < DepthW; x += 4u) {
            const size_t i = size_t(y) * DepthW + x;
            if (!measurement.valid[i] || !prediction.valid[i] || prediction.confidence[i] < 0.20f) continue;
            const float r = std::fabs((a0 * measurement.values[i] + b0) - prediction.values[i]);
            if (std::isfinite(r)) residuals.push_back(r);
        }
    }
    if (residuals.size() < 256u) return false;
    const float medianResidual = Percentile(residuals, 0.50f);
    const float limit = std::max(3.0f * medianResidual, 0.035f * span);

    size_t n1 = 0;
    float a1 = a0, b1 = b0;
    if (solve(limit, a0, b0, a1, b1, n1)) {
        scale = a1;
        shift = b1;
    } else {
        scale = a0;
        shift = b0;
    }
    return true;
}

void AIDepthTemporalStabilizer::Blend(const Map& prediction,
                                      const Map& measurement,
                                      float scale,
                                      float shift,
                                      Map& out) const {
    out.values.assign(PixelCount, 0.0f);
    out.valid.assign(PixelCount, 0u);
    out.confidence.assign(PixelCount, 0.0f);
    out.timestamp100ns = prediction.timestamp100ns;

    float lo = 0.0f, hi = 1.0f;
    if (!ComputePercentiles(prediction, 0.05f, 0.95f, lo, hi)) {
        lo = 0.0f; hi = 1.0f;
    }
    const float span = std::max(hi - lo, 1.0e-3f);

    for (size_t i = 0; i < PixelCount; ++i) {
        const bool hp = prediction.valid[i] != 0u;
        const bool hm = measurement.valid[i] != 0u;
        if (!hp && !hm) continue;
        if (!hp) {
            out.values[i] = scale * measurement.values[i] + shift;
            out.valid[i] = 1u;
            out.confidence[i] = Clamp01(0.90f * measurement.confidence[i]);
            continue;
        }
        if (!hm) {
            out.values[i] = prediction.values[i];
            out.valid[i] = 1u;
            out.confidence[i] = Clamp01(prediction.confidence[i] * 0.985f);
            continue;
        }

        const float p = prediction.values[i];
        const float m = scale * measurement.values[i] + shift;
        const float diff = std::fabs(m - p) / span;
        const float predConf = Clamp01(prediction.confidence[i]);
        const float measConf = Clamp01(measurement.confidence[i]);

        // Stable regions retain history; disagreement and low motion confidence
        // rapidly prefer the fresh AI measurement to avoid temporal ghost trails.
        float newWeight = 0.22f;
        if (diff > 0.14f) newWeight = 0.90f;
        else if (diff > 0.075f) newWeight = 0.62f;
        else if (diff > 0.035f) newWeight = 0.38f;
        newWeight = std::max(newWeight, 0.20f + 0.70f * (1.0f - predConf));
        newWeight = std::clamp(newWeight * (0.80f + 0.20f * measConf), 0.18f, 0.95f);

        out.values[i] = p + (m - p) * newWeight;
        out.valid[i] = 1u;
        out.confidence[i] = Clamp01(std::max(predConf * (1.0f - 0.35f * newWeight),
                                                 0.80f * measConf));
    }
}

float AIDepthTemporalStabilizer::Coverage(const Map& map) {
    if (map.valid.size() != PixelCount) return 0.0f;
    size_t n = 0;
    for (uint8_t v : map.valid) n += v ? 1u : 0u;
    return static_cast<float>(n) / static_cast<float>(PixelCount);
}

void AIDepthTemporalStabilizer::BuildPreview(const Map& state, AIDepthTemporalFrame& out) {
    float lo = 0.0f, hi = 1.0f;
    if (!ComputePercentiles(state, 0.02f, 0.98f, lo, hi)) {
        out.preview01.clear();
        out.valid = false;
        return;
    }

    if (!m_normInitialized) {
        m_normLo = lo;
        m_normHi = hi;
        m_normInitialized = true;
    } else {
        const float oldSpan = std::max(m_normHi - m_normLo, 1.0e-3f);
        const float newSpan = std::max(hi - lo, 1.0e-3f);
        const bool majorChange = newSpan > oldSpan * 1.8f || newSpan < oldSpan * 0.55f ||
            std::fabs((lo + hi) - (m_normLo + m_normHi)) > oldSpan * 1.25f;
        const float alpha = majorChange ? 0.25f : 0.055f;
        m_normLo += (lo - m_normLo) * alpha;
        m_normHi += (hi - m_normHi) * alpha;
    }
    if (m_normHi <= m_normLo + kEps) m_normHi = m_normLo + 1.0f;

    const float inv = 1.0f / (m_normHi - m_normLo);
    out.preview01.assign(PixelCount, 0.0f);
    for (size_t i = 0; i < PixelCount; ++i) {
        if (!state.valid[i] || !std::isfinite(state.values[i])) continue;
        out.preview01[i] = Clamp01((state.values[i] - m_normLo) * inv);
    }
    out.normalizedLo = m_normLo;
    out.normalizedHi = m_normHi;
    out.valid = true;
}

bool AIDepthTemporalStabilizer::ProcessFrame(int64_t currentTimestamp100ns,
                                             int64_t previousRenderedTimestamp100ns,
                                             uint32_t sourceW,
                                             uint32_t sourceH,
                                             const AIDepthMotionView* currentToPrevious,
                                             const AIDepthMeasurementView* newMeasurement,
                                             AIDepthTemporalFrame& out) {
    out = {};
    out.currentTimestamp100ns = currentTimestamp100ns;
    if (!sourceW || !sourceH || currentTimestamp100ns < 0) return false;

    if ((m_sourceW && m_sourceW != sourceW) || (m_sourceH && m_sourceH != sourceH)) Reset();
    m_sourceW = sourceW;
    m_sourceH = sourceH;

    if (currentToPrevious && currentToPrevious->valid) {
        (void)AppendFlowStep(previousRenderedTimestamp100ns, currentTimestamp100ns,
                             sourceW, sourceH, *currentToPrevious);
    }

    Map prediction;
    bool havePrediction = false;
    uint32_t predictionSteps = 0;
    if (m_state.values.size() == PixelCount) {
        if (m_state.timestamp100ns == currentTimestamp100ns) {
            prediction = m_state;
            havePrediction = true;
        } else if (m_state.timestamp100ns < currentTimestamp100ns) {
            havePrediction = WarpAcrossHistory(m_state, currentTimestamp100ns, prediction, predictionSteps);
        }
    }
    out.predictionReprojected = havePrediction && predictionSteps > 0u;
    out.predictionReprojectionSteps = predictionSteps;
    if (havePrediction) out.historyCoverage = Coverage(prediction);

    Map measurementCurrent;
    bool haveMeasurement = false;
    uint32_t measurementSteps = 0;
    if (newMeasurement && newMeasurement->rawDepth) {
        out.measurementTimestamp100ns = newMeasurement->timestamp100ns;
        out.measurementSequence = newMeasurement->sequence;
        out.measurementAgeMs = static_cast<double>(currentTimestamp100ns - newMeasurement->timestamp100ns) * 1.0e-4;
        Map raw;
        if (MeasurementToMap(*newMeasurement, raw) && raw.timestamp100ns <= currentTimestamp100ns) {
            if (raw.timestamp100ns == currentTimestamp100ns) {
                measurementCurrent = std::move(raw);
                haveMeasurement = true;
            } else {
                haveMeasurement = WarpAcrossHistory(raw, currentTimestamp100ns, measurementCurrent, measurementSteps);
                out.measurementReprojected = haveMeasurement && measurementSteps > 0u;
            }
        }
        out.measurementReprojectionSteps = measurementSteps;
        out.measurementRejected = !haveMeasurement;
        if (haveMeasurement) out.measurementCoverage = Coverage(measurementCurrent);
    }

    if (!havePrediction && !haveMeasurement) {
        if (m_state.values.size() == PixelCount) {
            // Keep the last valid visualization rather than flashing black if one OF
            // step is temporarily unavailable. It is marked stale by its timestamp.
            BuildPreview(m_state, out);
            out.historyCoverage = Coverage(m_state);
            return out.valid;
        }
        return false;
    }

    Map next;
    float scale = 1.0f, shift = 0.0f;
    if (havePrediction && haveMeasurement) {
        if (RobustAffine(measurementCurrent, prediction, scale, shift)) {
            Blend(prediction, measurementCurrent, scale, shift, next);
            out.usedMeasurement = true;
        } else {
            next = prediction;
            out.measurementRejected = true;
        }
    } else if (haveMeasurement) {
        next = measurementCurrent;
        out.usedMeasurement = true;
    } else {
        next = prediction;
    }

    next.timestamp100ns = currentTimestamp100ns;
    m_state = std::move(next);
    out.affineScale = scale;
    out.affineShift = shift;
    BuildPreview(m_state, out);
    return out.valid;
}
