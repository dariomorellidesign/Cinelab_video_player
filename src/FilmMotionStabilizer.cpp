#include "FilmMotionStabilizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace {

struct AffineMotionModel {
    // vx = x[0] + x[1]*nx + x[2]*ny
    // vy = y[0] + y[1]*nx + y[2]*ny
    std::array<double, 3> x{};
    std::array<double, 3> y{};
    bool valid = false;
};

inline float Clamp01(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

inline float Len(float x, float y) {
    return std::sqrt(x * x + y * y);
}

bool Solve3x3(double a[3][3], double b[3], std::array<double, 3>& out) {
    double m[3][4] = {
        {a[0][0], a[0][1], a[0][2], b[0]},
        {a[1][0], a[1][1], a[1][2], b[1]},
        {a[2][0], a[2][1], a[2][2], b[2]},
    };
    for (int c = 0; c < 3; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 3; ++r)
            if (std::abs(m[r][c]) > std::abs(m[pivot][c])) pivot = r;
        if (std::abs(m[pivot][c]) < 1e-9) return false;
        if (pivot != c) for (int j = c; j < 4; ++j) std::swap(m[c][j], m[pivot][j]);
        const double inv = 1.0 / m[c][c];
        for (int j = c; j < 4; ++j) m[c][j] *= inv;
        for (int r = 0; r < 3; ++r) {
            if (r == c) continue;
            const double f = m[r][c];
            for (int j = c; j < 4; ++j) m[r][j] -= f * m[c][j];
        }
    }
    out = {m[0][3], m[1][3], m[2][3]};
    return true;
}

float Median(std::vector<float> values) {
    if (values.empty()) return 0.0f;
    const size_t mid = values.size() / 2u;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    float med = values[mid];
    if ((values.size() & 1u) == 0u && mid > 0u) {
        const float lo = *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid));
        med = 0.5f * (lo + med);
    }
    return med;
}

struct Sample {
    float nx = 0.0f;
    float ny = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
};

bool FitAffine(const std::vector<Sample>& samples,
               const std::vector<uint8_t>* keep,
               AffineMotionModel& model) {
    double normal[3][3]{};
    double bx[3]{};
    double by[3]{};
    size_t n = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (keep && !(*keep)[i]) continue;
        const Sample& s = samples[i];
        const double q[3] = {1.0, s.nx, s.ny};
        for (int r = 0; r < 3; ++r) {
            bx[r] += q[r] * s.vx;
            by[r] += q[r] * s.vy;
            for (int c = 0; c < 3; ++c) normal[r][c] += q[r] * q[c];
        }
        ++n;
    }
    if (n < 12u) return false;

    double nx[3][3], ny[3][3];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) nx[r][c] = ny[r][c] = normal[r][c];
    model.valid = Solve3x3(nx, bx, model.x) && Solve3x3(ny, by, model.y);
    return model.valid;
}

inline void Eval(const AffineMotionModel& m, float nx, float ny, float& vx, float& vy) {
    vx = float(m.x[0] + m.x[1] * nx + m.x[2] * ny);
    vy = float(m.y[0] + m.y[1] * nx + m.y[2] * ny);
}

AffineMotionModel RobustAffineModel(const std::vector<float>& motionXY,
                                    uint32_t w,
                                    uint32_t h,
                                    float& robustNoisePx) {
    robustNoisePx = 0.0f;
    AffineMotionModel model{};
    if (!w || !h || motionXY.size() < size_t(w) * h * 2u) return model;

    const size_t total = size_t(w) * h;
    const uint32_t step = std::max(1u, uint32_t(std::sqrt(double(total) / 12000.0)));
    std::vector<Sample> samples;
    samples.reserve((w / step + 1u) * (h / step + 1u));
    for (uint32_t y = step / 2u; y < h; y += step) {
        const float ny = h > 1u ? (2.0f * float(y) / float(h - 1u) - 1.0f) : 0.0f;
        for (uint32_t x = step / 2u; x < w; x += step) {
            const size_t i = (size_t(y) * w + x) * 2u;
            const float vx = motionXY[i + 0u];
            const float vy = motionXY[i + 1u];
            if (!std::isfinite(vx) || !std::isfinite(vy) || std::abs(vx) > 256.0f || std::abs(vy) > 256.0f) continue;
            const float nx = w > 1u ? (2.0f * float(x) / float(w - 1u) - 1.0f) : 0.0f;
            samples.push_back({nx, ny, vx, vy});
        }
    }
    if (samples.size() < 24u || !FitAffine(samples, nullptr, model)) return model;

    std::vector<float> residual;
    residual.reserve(samples.size());
    for (const Sample& s : samples) {
        float px = 0.0f, py = 0.0f;
        Eval(model, s.nx, s.ny, px, py);
        residual.push_back(Len(s.vx - px, s.vy - py));
    }
    const float med = Median(residual);
    std::vector<float> dev;
    dev.reserve(residual.size());
    for (float r : residual) dev.push_back(std::abs(r - med));
    const float mad = Median(dev);
    robustNoisePx = std::clamp(1.4826f * mad, 0.0f, 8.0f);

    const float inlierThreshold = std::clamp(med + std::max(0.35f, robustNoisePx * 2.5f), 0.55f, 5.0f);
    std::vector<uint8_t> keep(samples.size(), 0u);
    size_t inliers = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (residual[i] <= inlierThreshold) { keep[i] = 1u; ++inliers; }
    }
    if (inliers >= std::max<size_t>(24u, samples.size() / 2u)) {
        AffineMotionModel refined{};
        if (FitAffine(samples, &keep, refined)) model = refined;
    }

    // Re-estimate the noise floor after robust fitting so foreground objects do not
    // dominate the threshold used to suppress random micro-motion.
    residual.clear();
    for (const Sample& s : samples) {
        float px = 0.0f, py = 0.0f;
        Eval(model, s.nx, s.ny, px, py);
        const float r = Len(s.vx - px, s.vy - py);
        if (r <= inlierThreshold) residual.push_back(r);
    }
    if (!residual.empty()) {
        const float med2 = Median(residual);
        dev.clear(); dev.reserve(residual.size());
        for (float r : residual) dev.push_back(std::abs(r - med2));
        robustNoisePx = std::clamp(1.4826f * Median(dev), 0.0f, 8.0f);
    }
    return model;
}

inline void CellNorm(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float& nx, float& ny) {
    nx = w > 1u ? (2.0f * float(x) / float(w - 1u) - 1.0f) : 0.0f;
    ny = h > 1u ? (2.0f * float(y) / float(h - 1u) - 1.0f) : 0.0f;
}

inline void ResidualAt(const std::vector<float>& motionXY,
                       const AffineMotionModel& model,
                       uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       float& rx, float& ry) {
    float nx = 0.0f, ny = 0.0f, mx = 0.0f, my = 0.0f;
    CellNorm(x, y, w, h, nx, ny);
    Eval(model, nx, ny, mx, my);
    const size_t i = (size_t(y) * w + x) * 2u;
    rx = motionXY[i + 0u] - mx;
    ry = motionXY[i + 1u] - my;
}

} // namespace

void FilmMotionStabilizer::Reset() {
    m_prevResidualXY.clear();
    m_prevW = m_prevH = 0;
}

bool FilmMotionStabilizer::Process(std::vector<float>& motionXY,
                                   uint32_t gridW,
                                   uint32_t gridH,
                                   bool enabled,
                                   FilmMotionStabilizationStats* outStats) {
    FilmMotionStabilizationStats stats{};
    stats.enabled = enabled;
    if (!enabled) {
        Reset();
        if (outStats) *outStats = stats;
        return true;
    }
    const size_t cells = size_t(gridW) * gridH;
    if (!gridW || !gridH || motionXY.size() != cells * 2u) {
        if (outStats) *outStats = stats;
        return false;
    }
    // Manual 1x1/2x2 true-4K experiments can create multi-million-vector fields.
    // Do not turn a diagnostic grid override into a CPU stall; AUTO already chooses 4x4.
    if (cells > 1200000u) {
        Reset();
        stats.performanceBypass = true;
        if (outStats) *outStats = stats;
        return true;
    }

    float robustNoise = 0.0f;
    const AffineMotionModel model = RobustAffineModel(motionXY, gridW, gridH, robustNoise);
    stats.modelValid = model.valid;
    stats.robustNoisePx = robustNoise;
    if (!model.valid) {
        Reset();
        if (outStats) *outStats = stats;
        return true;
    }

    float cx = 0.0f, cy = 0.0f;
    Eval(model, 0.0f, 0.0f, cx, cy);
    stats.centerMotionX = cx;
    stats.centerMotionY = cy;

    // Work in small NVOF-grid tiles. Grain is high-frequency, so a 4x4 flow tile
    // (8x8 source pixels with the normal NVOF 2x2 grid) is fine enough to preserve
    // real object boundaries while making the stabilizer cheap enough for realtime.
    constexpr uint32_t blockSize = 4u;
    const uint32_t blocksW = (gridW + blockSize - 1u) / blockSize;
    const uint32_t blocksH = (gridH + blockSize - 1u) / blockSize;
    const size_t blocks = size_t(blocksW) * blocksH;
    const bool haveHistory = (m_prevW == blocksW && m_prevH == blocksH && m_prevResidualXY.size() == blocks * 2u);
    stats.usedHistory = haveHistory;
    std::vector<float> nextBlockResidual(blocks * 2u, 0.0f);

    const float micro = std::clamp(0.35f + robustNoise * 3.0f, 0.40f, 1.35f);
    const float hardDeadZone = std::clamp(0.10f + robustNoise * 0.45f, 0.12f, 0.32f);
    const float microL1 = micro * 1.4142f;
    const float deadL1 = hardDeadZone * 1.4142f;

    size_t corrected = 0u, snapped = 0u;
    double correctionSum = 0.0;
    float maxCorrection = 0.0f;

    const float nxScale = gridW > 1u ? 2.0f / float(gridW - 1u) : 0.0f;
    const float nyScale = gridH > 1u ? 2.0f / float(gridH - 1u) : 0.0f;
    const float ax1 = float(model.x[1]) * nxScale;
    const float ay1 = float(model.y[1]) * nxScale;
    const float ax2 = float(model.x[2]) * nyScale;
    const float ay2 = float(model.y[2]) * nyScale;
    const float baseX = float(model.x[0] - model.x[1] - model.x[2]);
    const float baseY = float(model.y[0] - model.y[1] - model.y[2]);

    for (uint32_t by = 0; by < blocksH; ++by) {
        const uint32_t y0 = by * blockSize;
        const uint32_t y1 = std::min(gridH, y0 + blockSize);
        for (uint32_t bx = 0; bx < blocksW; ++bx) {
            const uint32_t x0 = bx * blockSize;
            const uint32_t x1 = std::min(gridW, x0 + blockSize);
            float meanRx = 0.0f, meanRy = 0.0f;
            uint32_t count = 0u;

            for (uint32_t y = y0; y < y1; ++y) {
                float mx = baseX + ax2 * float(y) + ax1 * float(x0);
                float my = baseY + ay2 * float(y) + ay1 * float(x0);
                for (uint32_t x = x0; x < x1; ++x, mx += ax1, my += ay1) {
                    const size_t i = (size_t(y) * gridW + x) * 2u;
                    meanRx += motionXY[i + 0u] - mx;
                    meanRy += motionXY[i + 1u] - my;
                    ++count;
                }
            }
            const float invCount = 1.0f / float(std::max(1u, count));
            meanRx *= invCount; meanRy *= invCount;

            float scatter = 0.0f;
            for (uint32_t y = y0; y < y1; ++y) {
                float mx = baseX + ax2 * float(y) + ax1 * float(x0);
                float my = baseY + ay2 * float(y) + ay1 * float(x0);
                for (uint32_t x = x0; x < x1; ++x, mx += ax1, my += ay1) {
                    const size_t i = (size_t(y) * gridW + x) * 2u;
                    const float rx = motionXY[i + 0u] - mx;
                    const float ry = motionXY[i + 1u] - my;
                    scatter += std::abs(rx - meanRx) + std::abs(ry - meanRy);
                }
            }
            scatter *= invCount;

            const float meanL1 = std::abs(meanRx) + std::abs(meanRy);
            const float incoherence = Clamp01((scatter - 0.07f) / std::max(0.16f, microL1 * 0.55f));
            const float neighborhoodIsModel = 1.0f - Clamp01(meanL1 / std::max(0.18f, microL1 * 0.75f));
            const size_t bi = (size_t(by) * blocksW + bx) * 2u;
            float temporalMeanX = meanRx, temporalMeanY = meanRy;
            if (haveHistory && meanL1 < microL1 * 0.85f) {
                const float prx = m_prevResidualXY[bi + 0u];
                const float pry = m_prevResidualXY[bi + 1u];
                const float diffL1 = std::abs(meanRx - prx) + std::abs(meanRy - pry);
                if (diffL1 < microL1) {
                    const float t = std::clamp(0.18f + robustNoise * 0.22f, 0.18f, 0.40f);
                    temporalMeanX = meanRx * (1.0f - t) + prx * t;
                    temporalMeanY = meanRy * (1.0f - t) + pry * t;
                }
            }
            nextBlockResidual[bi + 0u] = temporalMeanX;
            nextBlockResidual[bi + 1u] = temporalMeanY;

            for (uint32_t y = y0; y < y1; ++y) {
                float mx = baseX + ax2 * float(y) + ax1 * float(x0);
                float my = baseY + ay2 * float(y) + ay1 * float(x0);
                for (uint32_t x = x0; x < x1; ++x, mx += ax1, my += ay1) {
                    const size_t i = (size_t(y) * gridW + x) * 2u;
                    const float rawRx = motionXY[i + 0u] - mx;
                    const float rawRy = motionXY[i + 1u] - my;
                    const float rawL1 = std::abs(rawRx) + std::abs(rawRy);
                    float rx = rawRx, ry = rawRy;
                    if (rawL1 <= deadL1) {
                        rx = 0.0f; ry = 0.0f;
                    } else if (rawL1 < microL1) {
                        const float smallness = Clamp01((microL1 - rawL1) / std::max(0.12f, microL1 - deadL1));
                        float strength = Clamp01(0.15f * smallness + 0.82f * incoherence * neighborhoodIsModel);
                        // Use the temporally stabilized tile residual as the coherent target.
                        rx = rawRx * (1.0f - strength) + temporalMeanX * strength;
                        ry = rawRy * (1.0f - strength) + temporalMeanY * strength;
                        if (meanL1 < microL1 * 0.25f && incoherence > 0.32f) {
                            const float modelStrength = Clamp01((incoherence - 0.32f) / 0.68f) * 0.58f;
                            rx *= (1.0f - modelStrength);
                            ry *= (1.0f - modelStrength);
                        }
                    }

                    const float correction = std::abs(rx - rawRx) + std::abs(ry - rawRy);
                    if (correction > 0.055f) ++corrected;
                    if (rawL1 > deadL1 && (std::abs(rx) + std::abs(ry)) <= deadL1 * 0.80f) ++snapped;
                    correctionSum += correction * 0.7071;
                    maxCorrection = std::max(maxCorrection, correction * 0.7071f);
                    motionXY[i + 0u] = mx + rx;
                    motionXY[i + 1u] = my + ry;
                }
            }
        }
    }

    m_prevResidualXY.swap(nextBlockResidual);
    m_prevW = blocksW;
    m_prevH = blocksH;
    stats.correctedPct = cells ? float(100.0 * double(corrected) / double(cells)) : 0.0f;
    stats.snappedPct = cells ? float(100.0 * double(snapped) / double(cells)) : 0.0f;
    stats.meanCorrectionPx = cells ? float(correctionSum / double(cells)) : 0.0f;
    stats.maxCorrectionPx = maxCorrection;
    if (outStats) *outStats = stats;
    return true;
}
