#pragma once
// Frozen Step06A4C reference copied before Step06A4D. Do not optimize this oracle.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include <ppl.h>
namespace reference_a4c {
using OfClock = std::chrono::steady_clock;
inline double MsBetween(OfClock::time_point a, OfClock::time_point b) { return std::chrono::duration<double, std::milli>(b-a).count(); }
struct Repair {
    uint32_t width=0,height=0,gridSize=0,gridW=0,gridH=0;
    std::vector<uint32_t> hostCostStorage;
    std::vector<uint8_t> confidenceTexture,confidenceLuma,confidenceReliable;
    std::vector<float> confidenceScratchX,confidenceScratchY;
    // STEP 06A3 NVOF confidence-aware MV rejection/infill.
    // STEP 06A4C parallel confidence repair: exact Step06A3 decision policy, parallelized independent rows.
    // Reliable seed classification and global robust median remain serial/deterministic. Texture scoring and
    // low-confidence local repair are row-independent and run on the MSVC PPL scheduler so the render thread
    // no longer spends tens of milliseconds walking millions of compact-grid cells serially.
    struct ConfidenceRepairStats {
        uint8_t costQ25 = 0, costQ50 = 0, costQ75 = 0, costThreshold = 0;
        float globalX = 0.0f, globalY = 0.0f;
        bool globalTrusted = false;
        double texturedPct = 0.0, reliablePct = 0.0, globalCoveragePct = 0.0, localFillPct = 0.0, globalFillPct = 0.0, zeroPct = 0.0;
        double textureMs = 0.0, classifyMs = 0.0, infillMs = 0.0;
    };
    static float MedianInPlace(float* values, size_t count) {
        if (!count) return 0.0f;
        const size_t mid = count / 2u;
        std::nth_element(values, values + mid, values + count);
        return values[mid];
    }
    uint8_t SampleLuma(const uint8_t* bgra, uint32_t x, uint32_t y) const {
        x = std::min(x, width - 1u); y = std::min(y, height - 1u);
        const size_t i = (size_t(y) * width + x) * 4u;
        return static_cast<uint8_t>((19u * bgra[i + 0u] + 183u * bgra[i + 1u] + 54u * bgra[i + 2u] + 128u) >> 8u);
    }
    void BuildTextureScores(const uint8_t* bgra) {
        const uint32_t radius = std::max(2u, gridSize * 2u);
        Concurrency::parallel_for(uint32_t(0), gridH, [&](uint32_t gy) {
            const uint32_t cy = std::min(height - 1u, gy * gridSize + gridSize / 2u);
            for (uint32_t gx = 0; gx < gridW; ++gx) {
                const uint32_t cx = std::min(width - 1u, gx * gridSize + gridSize / 2u);
                const uint32_t xl = cx > radius ? cx - radius : 0u, xr = std::min(width - 1u, cx + radius);
                const uint32_t yu = cy > radius ? cy - radius : 0u, yd = std::min(height - 1u, cy + radius);
                const uint8_t cc=SampleLuma(bgra,cx,cy), l=SampleLuma(bgra,xl,cy), r=SampleLuma(bgra,xr,cy), u=SampleLuma(bgra,cx,yu), dd=SampleLuma(bgra,cx,yd);
                const uint8_t lo=std::min({cc,l,r,u,dd}), hi=std::max({cc,l,r,u,dd});
                const size_t cell=size_t(gy)*gridW+gx;
                confidenceLuma[cell]=cc;
                confidenceTexture[cell]=static_cast<uint8_t>(hi-lo);
            }
        });
    }
    ConfidenceRepairStats RepairLowConfidenceMotion(std::vector<float>& motionXY, const uint8_t* bgra) {
        ConfidenceRepairStats rs{}; const size_t cells=size_t(gridW)*gridH;
        if(!bgra || hostCostStorage.size()*sizeof(uint32_t)<cells || motionXY.size()!=cells*2u || !cells)return rs;
        const uint8_t* costBytes=reinterpret_cast<const uint8_t*>(hostCostStorage.data());
        const auto textureStart=OfClock::now();
        BuildTextureScores(bgra);
        const auto textureEnd=OfClock::now();rs.textureMs=MsBetween(textureStart,textureEnd);
        const auto classifyStart=textureEnd;
        constexpr uint8_t kTextureReliable=12u; constexpr float kMaxSeedMagnitude=96.0f;
        uint32_t hist[256]{}; size_t textured=0;
        for(size_t i=0;i<cells;++i)if(confidenceTexture[i]>=kTextureReliable){++hist[costBytes[i]];++textured;}
        auto percentile=[&](uint32_t numer,uint32_t denom)->uint8_t{if(!textured)return 0u;const size_t target=(textured*numer+denom-1u)/denom;size_t acc=0;for(uint32_t v=0;v<256u;++v){acc+=hist[v];if(acc>=target)return static_cast<uint8_t>(v);}return 255u;};
        rs.costQ25=percentile(1,4);rs.costQ50=percentile(1,2);rs.costQ75=percentile(3,4);
        rs.costThreshold=static_cast<uint8_t>(std::clamp(int(rs.costQ75)+6,24,160));
        rs.texturedPct=100.0*double(textured)/double(cells);
        confidenceScratchX.clear();confidenceScratchY.clear();confidenceScratchX.reserve(cells/2u+1u);confidenceScratchY.reserve(cells/2u+1u);
        size_t reliable=0;
        constexpr uint32_t kCoverageTilesX=8u,kCoverageTilesY=4u; bool coverageTiles[kCoverageTilesX*kCoverageTilesY]{};
        for(size_t i=0;i<cells;++i){
            const float x=motionXY[i*2u],y=motionXY[i*2u+1u],mag=std::sqrt(x*x+y*y);
            const bool good=confidenceTexture[i]>=kTextureReliable && costBytes[i]<=rs.costThreshold && mag<=kMaxSeedMagnitude;
            confidenceReliable[i]=good?1u:0u;
            if(good){++reliable;confidenceScratchX.push_back(x);confidenceScratchY.push_back(y);const uint32_t gx=uint32_t(i%gridW),gy=uint32_t(i/gridW);const uint32_t tx=std::min(kCoverageTilesX-1u,gx*kCoverageTilesX/std::max(1u,gridW));const uint32_t ty=std::min(kCoverageTilesY-1u,gy*kCoverageTilesY/std::max(1u,gridH));coverageTiles[ty*kCoverageTilesX+tx]=true;}
        }
        rs.reliablePct=100.0*double(reliable)/double(cells);
        uint32_t coveredTiles=0;for(bool v:coverageTiles)if(v)++coveredTiles;rs.globalCoveragePct=100.0*double(coveredTiles)/double(kCoverageTilesX*kCoverageTilesY);
        rs.globalTrusted=reliable>=8u && coveredTiles>=8u; // >=25% spatial coverage: avoid treating one moving object as camera motion.
        if(rs.globalTrusted){rs.globalX=MedianInPlace(confidenceScratchX.data(),confidenceScratchX.size());rs.globalY=MedianInPlace(confidenceScratchY.data(),confidenceScratchY.size());}
        const float globalMag=std::sqrt(rs.globalX*rs.globalX+rs.globalY*rs.globalY);
        const auto classifyEnd=OfClock::now();rs.classifyMs=MsBetween(classifyStart,classifyEnd);
        const auto infillStart=classifyEnd;
        std::vector<uint32_t> rowLocal(gridH,0u),rowGlobal(gridH,0u),rowZero(gridH,0u);
        Concurrency::parallel_for(uint32_t(0), gridH, [&](uint32_t gy) {
            uint32_t localFillRow=0u,globalFillRow=0u,zeroedRow=0u;
            for(uint32_t gx=0;gx<gridW;++gx){
                const size_t i=size_t(gy)*gridW+gx;if(confidenceReliable[i])continue;
                float xs[25]{},ys[25]{};size_t count=0;const int y0=std::max(0,int(gy)-2),y1=std::min(int(gridH)-1,int(gy)+2),x0=std::max(0,int(gx)-2),x1=std::min(int(gridW)-1,int(gx)+2);
                for(int yy=y0;yy<=y1;++yy)for(int xx=x0;xx<=x1;++xx){const size_t j=size_t(yy)*gridW+size_t(xx);if(!confidenceReliable[j])continue;if(std::abs(int(confidenceLuma[j])-int(confidenceLuma[i]))>18)continue;xs[count]=motionXY[j*2u];ys[count]=motionXY[j*2u+1u];++count;}
                float fillX=0.0f,fillY=0.0f;bool usedLocal=false,usedGlobal=false;
                if(count>=3u){const float localX=MedianInPlace(xs,count),localY=MedianInPlace(ys,count);float dev[25]{};size_t dk=0;for(int yy=y0;yy<=y1;++yy)for(int xx=x0;xx<=x1;++xx){const size_t j=size_t(yy)*gridW+size_t(xx);if(!confidenceReliable[j])continue;if(std::abs(int(confidenceLuma[j])-int(confidenceLuma[i]))>18)continue;const float dx=motionXY[j*2u]-localX,dy=motionXY[j*2u+1u]-localY;dev[dk++]=std::sqrt(dx*dx+dy*dy);}const float mad=MedianInPlace(dev,dk);if(mad<=1.5f){fillX=localX;fillY=localY;usedLocal=true;}}
                if(!usedLocal && rs.globalTrusted){fillX=rs.globalX;fillY=rs.globalY;usedGlobal=true;}
                const float fillMag=std::sqrt(fillX*fillX+fillY*fillY);(void)fillMag;
                // Preserve coherent LOCAL sub-pixel motion: this step targets false vectors in ambiguous regions,
                // not genuine very-slow object motion. Only unresolved cells or textureless cells in an effectively
                // static globally-supported field are snapped to exact zero.
                const bool staticGlobal = rs.globalTrusted && globalMag < 0.08f;
                if((!usedLocal&&!usedGlobal) || (!usedLocal && confidenceTexture[i]<6u && staticGlobal)){fillX=0.0f;fillY=0.0f;++zeroedRow;}else if(usedLocal)++localFillRow;else if(usedGlobal)++globalFillRow;
                motionXY[i*2u]=fillX;motionXY[i*2u+1u]=fillY;
            }
            rowLocal[gy]=localFillRow;rowGlobal[gy]=globalFillRow;rowZero[gy]=zeroedRow;
        });
        size_t localFill=0,globalFill=0,zeroed=0;for(uint32_t gy=0;gy<gridH;++gy){localFill+=rowLocal[gy];globalFill+=rowGlobal[gy];zeroed+=rowZero[gy];}
        const auto infillEnd=OfClock::now();rs.infillMs=MsBetween(infillStart,infillEnd);
        rs.localFillPct=100.0*double(localFill)/double(cells);rs.globalFillPct=100.0*double(globalFill)/double(cells);rs.zeroPct=100.0*double(zeroed)/double(cells);return rs;
    }
    ConfidenceRepairStats Run(std::vector<float>& motion, const uint8_t* bgra, const uint8_t* cost, uint32_t w, uint32_t h, uint32_t grid) {
        width=w; height=h; gridSize=grid; gridW=(w+grid-1)/grid; gridH=(h+grid-1)/grid;
        const size_t cells=size_t(gridW)*gridH;
        hostCostStorage.resize((cells+3)/4); std::memcpy(hostCostStorage.data(),cost,cells);
        confidenceTexture.resize(cells); confidenceLuma.resize(cells); confidenceReliable.resize(cells);
        return RepairLowConfidenceMotion(motion,bgra);
    }
};
}