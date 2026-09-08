#include "TemporalGuides.h"
#include "MaskDepthGuide.h"
#include "SceneCutDetector.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>

void TemporalGuideGenerator::Reset() {
    m_prevLuma.clear();
    m_prevDepth.clear();
    m_softMask.Reset();
    m_gridW = m_gridH = 0;
    m_havePrev = false;
}

std::pair<uint32_t,uint32_t> TemporalGuideGenerator::AnalysisGrid(uint32_t sourceW, uint32_t sourceH, double targetFps) {
    if (!sourceW || !sourceH) return {0,0};
    // High-frame-rate playback uses a slightly more compact analysis field so guide
    // generation cannot become the reason a 50/60-fps movie misses realtime. The GPU
    // still expands this field to the exact DLSS input resolution.
    const bool highFps = std::isfinite(targetFps) && targetFps >= 45.0;
    const uint32_t maxGridW = highFps ? 128u : 160u;
    const uint32_t divisor = highFps ? 14u : 10u;
    const uint32_t gw = std::clamp(sourceW / divisor, 96u, maxGridW);
    const uint32_t minH = highFps ? 48u : 54u;
    const uint32_t gh = std::max(minH, uint32_t((uint64_t(gw) * sourceH) / sourceW));
    return {gw, gh};
}

float TemporalGuideGenerator::Luma(const uint8_t* p) {
    // BGRA -> Rec.709-ish luma in [0,1].
    return (0.0722f * p[0] + 0.7152f * p[1] + 0.2126f * p[2]) * (1.0f / 255.0f);
}

void TemporalGuideGenerator::DownsampleLuma(const uint8_t* bgra, uint32_t w, uint32_t h,
                                             uint32_t gw, uint32_t gh, std::vector<float>& out) const {
    out.assign(size_t(gw) * gh, 0.0f);
    for (uint32_t gy = 0; gy < gh; ++gy) {
        const uint32_t y0 = uint32_t((uint64_t(gy) * h) / gh);
        const uint32_t y1 = std::max(y0 + 1, uint32_t((uint64_t(gy + 1) * h) / gh));
        for (uint32_t gx = 0; gx < gw; ++gx) {
            const uint32_t x0 = uint32_t((uint64_t(gx) * w) / gw);
            const uint32_t x1 = std::max(x0 + 1, uint32_t((uint64_t(gx + 1) * w) / gw));
            // Four stratified samples are much cheaper than averaging every source pixel.
            const uint32_t xs[2] = { x0, std::min(w - 1, (x0 + x1) / 2) };
            const uint32_t ys[2] = { y0, std::min(h - 1, (y0 + y1) / 2) };
            float s = 0.0f;
            for (uint32_t yy : ys) for (uint32_t xx : xs)
                s += Luma(bgra + (size_t(yy) * w + xx) * 4u);
            out[size_t(gy) * gw + gx] = s * 0.25f;
        }
    }
}

static float PatchSad(const std::vector<float>& cur, const std::vector<float>& prev,
                      int x, int y, int dx, int dy, int w, int h) {
    float sad = 0.0f;
    int count = 0;
    for (int py = -1; py <= 1; ++py) {
        int cy = y + py, oy = cy + dy;
        if (cy < 0 || cy >= h || oy < 0 || oy >= h) continue;
        for (int px = -1; px <= 1; ++px) {
            int cx = x + px, ox = cx + dx;
            if (cx < 0 || cx >= w || ox < 0 || ox >= w) continue;
            sad += std::abs(cur[size_t(cy) * w + cx] - prev[size_t(oy) * w + ox]);
            ++count;
        }
    }
    return count ? sad / float(count) : 10.0f;
}

static float SampleBilinear(const std::vector<float>& img, float x, float y, int w, int h) {
    if (x < 0.0f || y < 0.0f || x > float(w - 1) || y > float(h - 1))
        return std::numeric_limits<float>::quiet_NaN();
    const int x0 = std::clamp(int(std::floor(x)), 0, w - 1);
    const int y0 = std::clamp(int(std::floor(y)), 0, h - 1);
    const int x1 = std::min(x0 + 1, w - 1);
    const int y1 = std::min(y0 + 1, h - 1);
    const float tx = x - float(x0), ty = y - float(y0);
    const float a = img[size_t(y0) * w + x0] * (1.0f - tx) + img[size_t(y0) * w + x1] * tx;
    const float b = img[size_t(y1) * w + x0] * (1.0f - tx) + img[size_t(y1) * w + x1] * tx;
    return a * (1.0f - ty) + b * ty;
}

static float PatchSadSubpixel(const std::vector<float>& cur, const std::vector<float>& prev,
                              int x, int y, float dx, float dy, int w, int h) {
    float sad = 0.0f;
    int count = 0;
    for (int py = -1; py <= 1; ++py) {
        const int cy = y + py;
        if (cy < 0 || cy >= h) continue;
        for (int px = -1; px <= 1; ++px) {
            const int cx = x + px;
            if (cx < 0 || cx >= w) continue;
            const float pv = SampleBilinear(prev, float(cx) + dx, float(cy) + dy, w, h);
            if (!std::isfinite(pv)) continue;
            sad += std::abs(cur[size_t(cy) * w + cx] - pv);
            ++count;
        }
    }
    return count ? sad / float(count) : 10.0f;
}

void TemporalGuideGenerator::EstimateFlow(const std::vector<float>& cur, const std::vector<float>& prev,
                                           uint32_t gw, uint32_t gh,
                                           std::vector<float>& flowX, std::vector<float>& flowY,
                                           std::vector<float>& mismatch,
                                           float& globalX, float& globalY, float& globalCost) const {
    const int w = int(gw), h = int(gh);
    // First find a coarse whole-frame translation. This is especially valuable for camera pans.
    float bestGlobal = std::numeric_limits<float>::max();
    float zeroGlobal = std::numeric_limits<float>::max();
    int bestGX = 0, bestGY = 0;
    constexpr int globalRadius = 7;
    for (int dy = -globalRadius; dy <= globalRadius; ++dy) {
        for (int dx = -globalRadius; dx <= globalRadius; ++dx) {
            float sad = 0.0f; int n = 0;
            for (int y = 4; y < h - 4; y += 4) {
                const int oy = y + dy; if (oy < 0 || oy >= h) continue;
                for (int x = 4; x < w - 4; x += 4) {
                    const int ox = x + dx; if (ox < 0 || ox >= w) continue;
                    sad += std::abs(cur[size_t(y) * w + x] - prev[size_t(oy) * w + ox]);
                    ++n;
                }
            }
            if (n) sad /= float(n);
            // Mild penalty avoids jumping to large vectors in flat/noisy regions.
            sad += 0.0015f * float(dx * dx + dy * dy);
            if (dx == 0 && dy == 0) zeroGlobal = sad;
            if (sad < bestGlobal) { bestGlobal = sad; bestGX = dx; bestGY = dy; }
        }
    }
    // A small independently moving object on an otherwise flat/static frame can make a
    // whole-frame translation look marginally better than zero. Do not smear that motion
    // over every pixel unless the global shift wins by a meaningful margin. Local block
    // matching below will still recover object motion around the zero/global seed.
    if ((bestGX != 0 || bestGY != 0) && std::isfinite(zeroGlobal) &&
        (zeroGlobal - bestGlobal) < 0.012f) {
        bestGX = bestGY = 0;
        bestGlobal = zeroGlobal;
    }
    globalX = float(bestGX); globalY = float(bestGY); globalCost = bestGlobal;

    flowX.assign(size_t(gw) * gh, float(bestGX));
    flowY.assign(size_t(gw) * gh, float(bestGY));
    mismatch.assign(size_t(gw) * gh, bestGlobal);
    constexpr int localRadius = 3;
    // Solve local flow on a 2x2 lattice, then expand each result to the tiny block.
    // At a 160-wide analysis grid this retains useful object motion while making
    // 30/60 fps playback much less CPU-bound than matching every grid pixel.
    for (int y = 0; y < h; y += 2) {
        for (int x = 0; x < w; x += 2) {
            float best = std::numeric_limits<float>::max();
            int bx = bestGX, by = bestGY;
            for (int oy = -localRadius; oy <= localRadius; ++oy) {
                for (int ox = -localRadius; ox <= localRadius; ++ox) {
                    const int dx = bestGX + ox, dy = bestGY + oy;
                    float cost = PatchSad(cur, prev, x, y, dx, dy, w, h);
                    cost += 0.002f * float(ox * ox + oy * oy);
                    if (cost < best) { best = cost; bx = dx; by = dy; }
                }
            }
            float fbx = float(bx), fby = float(by);
            // Integer block matching on a compact grid is too quantized after scaling to
            // 1440p/4K. Refine the winning vector at quarter-grid precision using bilinear
            // samples of the previous frame. This keeps the CPU implementation self-contained
            // while giving DLSS materially smoother per-pixel motion.
            if (best <= 0.18f) {
                static constexpr float sub[] = {-0.50f, -0.25f, 0.0f, 0.25f, 0.50f};
                float refined = best;
                for (float sy : sub) {
                    for (float sx : sub) {
                        const float dx = float(bx) + sx, dy = float(by) + sy;
                        float cost = PatchSadSubpixel(cur, prev, x, y, dx, dy, w, h);
                        cost += 0.0015f * (sx * sx + sy * sy);
                        if (cost < refined) { refined = cost; fbx = dx; fby = dy; }
                    }
                }
                best = refined;
            }

            // High mismatch means a cut/disocclusion/no reliable correspondence.
            if (best > 0.18f) { fbx = 0.0f; fby = 0.0f; }
            for (int yy = y; yy < std::min(y + 2, h); ++yy) {
                for (int xx = x; xx < std::min(x + 2, w); ++xx) {
                    const size_t oi = size_t(yy) * gw + xx;
                    flowX[oi] = fbx;
                    flowY[oi] = fby;
                    mismatch[oi] = best;
                }
            }
        }
    }
}

void TemporalGuideGenerator::MedianFlow(std::vector<float>& x, std::vector<float>& y,
                                         uint32_t gw, uint32_t gh) const {
    std::vector<float> ox = x, oy = y;
    for (uint32_t py = 1; py + 1 < gh; ++py) {
        for (uint32_t px = 1; px + 1 < gw; ++px) {
            std::array<float, 9> xs{}, ys{}; size_t k = 0;
            for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) {
                const size_t idx = size_t(int(py) + j) * gw + size_t(int(px) + i);
                xs[k] = ox[idx]; ys[k] = oy[idx]; ++k;
            }
            std::nth_element(xs.begin(), xs.begin() + 4, xs.end());
            std::nth_element(ys.begin(), ys.begin() + 4, ys.end());
            const size_t idx = size_t(py) * gw + px;
            x[idx] = xs[4]; y[idx] = ys[4];
        }
    }
}

void TemporalGuideGenerator::BuildDepthProxy(const std::vector<float>& luma,
                                              const std::vector<float>& flowX, const std::vector<float>& flowY,
                                              uint32_t gw, uint32_t gh,
                                              std::vector<float>& depth) {
    depth.assign(size_t(gw) * gh, 0.75f);
    if (m_depthMode == DepthMode::Flat) return;

    float maxMotion = 1.0f;
    for (size_t i = 0; i < flowX.size(); ++i)
        maxMotion = std::max(maxMotion, std::sqrt(flowX[i] * flowX[i] + flowY[i] * flowY[i]));

    for (uint32_t y = 0; y < gh; ++y) {
        for (uint32_t x = 0; x < gw; ++x) {
            const size_t idx = size_t(y) * gw + x;
            const float yn = gh > 1 ? float(y) / float(gh - 1) : 0.5f;
            float grad = 0.0f;
            if (x > 0 && x + 1 < gw) grad += std::abs(luma[idx + 1] - luma[idx - 1]);
            if (y > 0 && y + 1 < gh) grad += std::abs(luma[idx + gw] - luma[idx - gw]);
            const float motion = std::sqrt(flowX[idx] * flowX[idx] + flowY[idx] * flowY[idx]) / maxMotion;
            // This is explicitly a VIDEO DEPTH PROXY, not geometric engine depth.
            // It provides stable segmentation/disocclusion hints when a movie has no Z buffer.
            float d = 0.92f - 0.42f * yn - 0.17f * std::clamp(motion, 0.0f, 1.0f)
                            - 0.10f * std::clamp(grad * 2.0f, 0.0f, 1.0f);
            d = std::clamp(d, 0.08f, 0.97f);
            if (m_prevDepth.size() == depth.size()) d = m_prevDepth[idx] * 0.80f + d * 0.20f;
            depth[idx] = d;
        }
    }
    m_prevDepth = depth;
}

bool TemporalGuideGenerator::Generate(const uint8_t* bgra, uint32_t sourceW, uint32_t sourceH,
                                       uint32_t renderW, uint32_t renderH, double targetFps, bool reset,
                                       GuideFrame& out, const ExternalMotionField* externalMotion,
                                       const ExternalDepthField* externalMaskDepth) {
    if (!bgra || !sourceW || !sourceH || !renderW || !renderH) return false;
    if (reset) Reset();

    // Keep the legacy CPU analysis grid small for depth/mask fallback and scene-cut logic.
    const auto [gw, gh] = AnalysisGrid(sourceW, sourceH, targetFps);
    if (!gw || !gh) return false;
    if (gw != m_gridW || gh != m_gridH) Reset();
    m_gridW = gw; m_gridH = gh;

    std::vector<float> cur;
    DownsampleLuma(bgra, sourceW, sourceH, gw, gh, cur);

    std::vector<float> fx(size_t(gw) * gh, 0.0f), fy(size_t(gw) * gh, 0.0f), mismatch(size_t(gw) * gh, 1.0f);
    float globalX = 0.0f, globalY = 0.0f;
    const bool externalCandidate = externalMotion && externalMotion->valid &&
        externalMotion->motionXY && externalMotion->gridW && externalMotion->gridH &&
        externalMotion->sourceW == sourceW && externalMotion->sourceH == sourceH;
    out.usedHardwareFastPath = false;
    out.legacyFlowEvaluated = false;
    bool history = m_havePrev && m_prevLuma.size() == cur.size();
    float globalCost = 0.0f;
    bool hardCut = false;
    if (history) {
        if (externalCandidate) {
            // Step 04B-4 hardware-flow fast path. NVOFA is already the authoritative
            // current->previous motion source, so do not spend CPU time solving a second
            // block-matching flow field that will be discarded. Resample NVOFA only onto
            // the tiny legacy analysis grid used by the depth/mask fallback.
            out.usedHardwareFastPath = true;
            const float sourceToLegacyX = float(gw) / float(sourceW);
            const float sourceToLegacyY = float(gh) / float(sourceH);
            auto sampleExternalLegacy = [&](float sx, float sy, float& mx, float& my) {
                sx = std::clamp(sx, 0.0f, float(externalMotion->gridW - 1));
                sy = std::clamp(sy, 0.0f, float(externalMotion->gridH - 1));
                const uint32_t x0 = uint32_t(std::floor(sx));
                const uint32_t y0 = uint32_t(std::floor(sy));
                const uint32_t x1 = std::min(externalMotion->gridW - 1, x0 + 1);
                const uint32_t y1 = std::min(externalMotion->gridH - 1, y0 + 1);
                const float tx = sx - float(x0), ty = sy - float(y0);
                auto c = [&](uint32_t x, uint32_t y, uint32_t component) {
                    return externalMotion->motionXY[(size_t(y) * externalMotion->gridW + x) * 2u + component];
                };
                const float ax = c(x0,y0,0), bx = c(x1,y0,0), cx = c(x0,y1,0), dx = c(x1,y1,0);
                const float ay = c(x0,y0,1), by = c(x1,y0,1), cy = c(x0,y1,1), dy = c(x1,y1,1);
                mx = (ax + (bx - ax) * tx) + ((cx + (dx - cx) * tx) - (ax + (bx - ax) * tx)) * ty;
                my = (ay + (by - ay) * tx) + ((cy + (dy - cy) * tx) - (ay + (by - ay) * tx)) * ty;
            };

            double sumX = 0.0, sumY = 0.0, sumCost = 0.0;
            uint64_t validSamples = 0;
            for (uint32_t y = 0; y < gh; ++y) {
                for (uint32_t x = 0; x < gw; ++x) {
                    const size_t i = size_t(y) * gw + x;
                    const float ofX = (float(x) + 0.5f) * float(externalMotion->gridW) / float(gw) - 0.5f;
                    const float ofY = (float(y) + 0.5f) * float(externalMotion->gridH) / float(gh) - 0.5f;
                    float mx = 0.0f, my = 0.0f;
                    sampleExternalLegacy(ofX, ofY, mx, my);
                    const float gxv = mx * sourceToLegacyX;
                    const float gyv = my * sourceToLegacyY;
                    fx[i] = gxv;
                    fy[i] = gyv;
                    const float previous = SampleBilinear(m_prevLuma, float(x) + gxv, float(y) + gyv, int(gw), int(gh));
                    const float residual = std::isfinite(previous) ? std::abs(cur[i] - previous) : 1.0f;
                    mismatch[i] = std::clamp(residual, 0.0f, 1.0f);
                    if (std::isfinite(previous)) {
                        sumX += gxv; sumY += gyv; sumCost += residual; ++validSamples;
                    }
                }
            }
            if (validSamples) {
                globalX = float(sumX / double(validSamples));
                globalY = float(sumY / double(validSamples));
                globalCost = float(sumCost / double(validSamples));
            } else {
                globalX = globalY = 0.0f;
                globalCost = 1.0f;
            }
            // No legacy scene-cut gate is applied here. Step 04A-3 established that a
            // valid hardware flow pair must not be invalidated by the old CPU heuristic.
        } else {
            // Fallback is intentionally unchanged for systems/frames without NVOFA.
            out.legacyFlowEvaluated = true;
            EstimateFlow(cur, m_prevLuma, gw, gh, fx, fy, mismatch, globalX, globalY, globalCost);
            hardCut = globalCost > 0.10f;
            if (hardCut) {
                history = false;
                std::fill(fx.begin(), fx.end(), 0.0f);
                std::fill(fy.begin(), fy.end(), 0.0f);
                std::fill(mismatch.begin(), mismatch.end(), 1.0f);
                globalX = globalY = 0.0f;
                m_prevDepth.clear();
    m_softMask.Reset();
            } else {
                MedianFlow(fx, fy, gw, gh);
            }
        }
    }
    // Step 04E-2 hard scene-cut mask reset. NVOFA remains authoritative for normal
    // motion, but a real cut has no meaningful current->previous correspondence. Use
    // the already motion-compensated residual distribution plus direct frame change to
    // distinguish a hard cut from grain, local motion, or a tracked camera pan.
    const char* sceneCutPolicy = std::getenv("DMP_SCENE_CUT_RESET");
    const bool sceneCutResetEnabled = !(sceneCutPolicy &&
        (std::strcmp(sceneCutPolicy,"off")==0 || std::strcmp(sceneCutPolicy,"OFF")==0 ||
         std::strcmp(sceneCutPolicy,"0")==0 || std::strcmp(sceneCutPolicy,"false")==0));
    if (history && !hardCut && sceneCutResetEnabled) {
        const SceneCutMetrics cut = DetectHardSceneCut(cur, m_prevLuma, mismatch);
        out.sceneCutResidualMean = cut.warpedResidualMean;
        out.sceneCutResidualMedian = cut.warpedResidualMedian;
        out.sceneCutStrongFraction = cut.warpedStrongFraction;
        out.sceneCutDirectStrongFraction = cut.directStrongFraction;
        if (cut.hardCut) {
            hardCut = true;
            history = false;
            out.sceneCutDetected = true;
            std::fill(fx.begin(), fx.end(), 0.0f);
            std::fill(fy.begin(), fy.end(), 0.0f);
            std::fill(mismatch.begin(), mismatch.end(), 0.0f);
            globalX = globalY = 0.0f;
            m_prevDepth.clear();
            // Critical fix: do not let the slow-release mask from the previous shot
            // leak into the new shot. The cut frame itself is rendered with zero mask
            // and !hasHistory forces a DLSS/NR temporal reset.
            m_softMask.Reset();
        }
    }
    if (hardCut) out.sceneCutDetected = true;
    std::vector<float> depthGrid;
    // Only the selected producer runs. Flat is the no-depth-computation baseline.
    if (m_depthMode == DepthMode::Estimated) BuildDepthProxy(cur, fx, fy, gw, gh, depthGrid);
    else depthGrid.assign(size_t(gw) * gh, 0.75f);

    // Mask and Guide B follow the selected source. AI input is one immutable snapshot.
    std::vector<float> maskDepthGuide = depthGrid;
    out.maskAIDepthAvailable = externalMaskDepth && externalMaskDepth->depth01 &&
        externalMaskDepth->width && externalMaskDepth->height;
    out.maskDepthAgeMs = out.maskAIDepthAvailable ? externalMaskDepth->ageMs : -1.0f;
    out.maskDepthAgeFrames = out.maskAIDepthAvailable ? externalMaskDepth->ageFrames : -1.0f;
    if (m_depthMode == DepthMode::AI && !hardCut && out.maskAIDepthAvailable && externalMaskDepth->valid) {
        std::vector<float> aiDepthOnAnalysisGrid;
        if (ResampleMaskDepthGuide01(externalMaskDepth->depth01, externalMaskDepth->width,
                                     externalMaskDepth->height, gw, gh, aiDepthOnAnalysisGrid)) {
            // The same conventional depth signal also occupies Guide B.
            for (auto& z : aiDepthOnAnalysisGrid) z = 1.0f - z;
            depthGrid = aiDepthOnAnalysisGrid;
            maskDepthGuide = std::move(aiDepthOnAnalysisGrid);
            out.maskUsedAIDepth = true;
        }
    }
    std::vector<float> maskGrid;
    m_softMask.Build(cur, fx, fy, mismatch, maskDepthGuide, gw, gh, history, maskGrid, m_depthMode!=DepthMode::Estimated && !out.maskUsedAIDepth);
    const uint32_t outGW = m_outputGridW ? m_outputGridW : gw;
    const uint32_t outGH = m_outputGridH ? m_outputGridH : gh;
    if (!outGW || !outGH) return false;

    auto sampleScalar = [](const std::vector<float>& field, uint32_t sw, uint32_t sh,
                           float sx, float sy) -> float {
        if (field.empty() || !sw || !sh) return 0.0f;
        sx = std::clamp(sx, 0.0f, float(sw - 1));
        sy = std::clamp(sy, 0.0f, float(sh - 1));
        const uint32_t x0 = uint32_t(std::floor(sx));
        const uint32_t y0 = uint32_t(std::floor(sy));
        const uint32_t x1 = std::min(sw - 1, x0 + 1);
        const uint32_t y1 = std::min(sh - 1, y0 + 1);
        const float tx = sx - float(x0);
        const float ty = sy - float(y0);
        const float a = field[size_t(y0) * sw + x0];
        const float b = field[size_t(y0) * sw + x1];
        const float c = field[size_t(y1) * sw + x0];
        const float d = field[size_t(y1) * sw + x1];
        return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
    };

    const bool useExternal = history && externalCandidate;


    auto sampleExternalMotion = [&](float sx, float sy, float& mx, float& my) {
        sx = std::clamp(sx, 0.0f, float(externalMotion->gridW - 1));
        sy = std::clamp(sy, 0.0f, float(externalMotion->gridH - 1));
        const uint32_t x0 = uint32_t(std::floor(sx));
        const uint32_t y0 = uint32_t(std::floor(sy));
        const uint32_t x1 = std::min(externalMotion->gridW - 1, x0 + 1);
        const uint32_t y1 = std::min(externalMotion->gridH - 1, y0 + 1);
        const float tx = sx - float(x0);
        const float ty = sy - float(y0);
        auto component = [&](uint32_t x, uint32_t y, uint32_t c) {
            return externalMotion->motionXY[(size_t(y) * externalMotion->gridW + x) * 2u + c];
        };
        const float ax = component(x0, y0, 0), bx = component(x1, y0, 0);
        const float cx = component(x0, y1, 0), dx = component(x1, y1, 0);
        const float ay = component(x0, y0, 1), by = component(x1, y0, 1);
        const float cy = component(x0, y1, 1), dy = component(x1, y1, 1);
        const float topX = ax + (bx - ax) * tx, bottomX = cx + (dx - cx) * tx;
        const float topY = ay + (by - ay) * tx, bottomY = cy + (dy - cy) * tx;
        mx = topX + (bottomX - topX) * ty;
        my = topY + (bottomY - topY) * ty;
    };

    out.gridW = outGW;
    out.gridH = outGH;
    out.guideGridRGBA32F.assign(size_t(outGW) * outGH * 4u, 0.0f);

    const float legacyToRenderX = float(renderW) / float(gw);
    const float legacyToRenderY = float(renderH) / float(gh);
    const float sourceToRenderX = float(renderW) / float(sourceW);
    const float sourceToRenderY = float(renderH) / float(sourceH);

    // Common NVOFA path: SetOutputGrid() is the hardware OF grid itself. Avoid doing
    // a bilinear NVOFA sample for every guide cell when the coordinates are identical.
    // Depth/mask preserve the previous bilinear result, but their X/Y indices and
    // interpolation weights are precomputed once per row/column instead of recomputed
    // for every scalar sample. Motion values remain bit-for-bit source samples apart
    // from the existing source->DLSS-input scale.
    const bool directExternalGrid = useExternal && outGW == externalMotion->gridW && outGH == externalMotion->gridH;
    if (directExternalGrid) {
        struct AxisLerp { uint32_t i0=0, i1=0; float t=0.0f; };
        std::vector<AxisLerp> xMap(outGW), yMap(outGH);
        for (uint32_t x = 0; x < outGW; ++x) {
            float s = (float(x) + 0.5f) * float(gw) / float(outGW) - 0.5f;
            s = std::clamp(s, 0.0f, float(gw - 1));
            const uint32_t i0 = uint32_t(std::floor(s));
            xMap[x] = AxisLerp{i0, std::min(gw - 1, i0 + 1), s - float(i0)};
        }
        for (uint32_t y = 0; y < outGH; ++y) {
            float s = (float(y) + 0.5f) * float(gh) / float(outGH) - 0.5f;
            s = std::clamp(s, 0.0f, float(gh - 1));
            const uint32_t i0 = uint32_t(std::floor(s));
            yMap[y] = AxisLerp{i0, std::min(gh - 1, i0 + 1), s - float(i0)};
        }
        auto mappedScalar = [&](const std::vector<float>& field, const AxisLerp& ax, const AxisLerp& ay) -> float {
            if (field.empty()) return 0.0f;
            const float a = field[size_t(ay.i0) * gw + ax.i0];
            const float b = field[size_t(ay.i0) * gw + ax.i1];
            const float c = field[size_t(ay.i1) * gw + ax.i0];
            const float d = field[size_t(ay.i1) * gw + ax.i1];
            const float top = a + (b - a) * ax.t;
            const float bottom = c + (d - c) * ax.t;
            return top + (bottom - top) * ay.t;
        };
        for (uint32_t y = 0; y < outGH; ++y) {
            const AxisLerp ay = yMap[y];
            for (uint32_t x = 0; x < outGW; ++x) {
                const size_t cell = size_t(y) * outGW + x;
                const size_t o = cell * 4u;
                const size_t m = cell * 2u;
                out.guideGridRGBA32F[o + 0] = externalMotion->motionXY[m + 0] * sourceToRenderX;
                out.guideGridRGBA32F[o + 1] = externalMotion->motionXY[m + 1] * sourceToRenderY;
                out.guideGridRGBA32F[o + 2] = mappedScalar(depthGrid, xMap[x], ay);
                out.guideGridRGBA32F[o + 3] = history ? mappedScalar(maskGrid, xMap[x], ay) : 0.0f;
            }
        }
    } else {
        // Generic/fallback path retains the existing bilinear behavior.
        for (uint32_t y = 0; y < outGH; ++y) {
            for (uint32_t x = 0; x < outGW; ++x) {
                const float oldX = (float(x) + 0.5f) * float(gw) / float(outGW) - 0.5f;
                const float oldY = (float(y) + 0.5f) * float(gh) / float(outGH) - 0.5f;
                const size_t o = (size_t(y) * outGW + x) * 4u;

                float motionX = 0.0f, motionY = 0.0f;
                if (history) {
                    if (useExternal) {
                        const float ofX = (float(x) + 0.5f) * float(externalMotion->gridW) / float(outGW) - 0.5f;
                        const float ofY = (float(y) + 0.5f) * float(externalMotion->gridH) / float(outGH) - 0.5f;
                        sampleExternalMotion(ofX, ofY, motionX, motionY);
                        motionX *= sourceToRenderX;
                        motionY *= sourceToRenderY;
                    } else {
                        motionX = sampleScalar(fx, gw, gh, oldX, oldY) * legacyToRenderX;
                        motionY = sampleScalar(fy, gw, gh, oldX, oldY) * legacyToRenderY;
                    }
                }

                out.guideGridRGBA32F[o + 0] = motionX;
                out.guideGridRGBA32F[o + 1] = motionY;
                out.guideGridRGBA32F[o + 2] = sampleScalar(depthGrid, gw, gh, oldX, oldY);
                out.guideGridRGBA32F[o + 3] = history ? sampleScalar(maskGrid, gw, gh, oldX, oldY) : 0.0f;
            }
        }
    }
    out.hasHistory = history;
    out.globalMotionX = globalX * legacyToRenderX;
    out.globalMotionY = globalY * legacyToRenderY;
    out.globalMatchCost = globalCost;
    out.usedExternalMotion = useExternal;
    out.hardCut = hardCut;
    m_prevLuma = std::move(cur);
    m_havePrev = true;
    return true;
}
