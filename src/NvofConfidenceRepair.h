#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>
#include <ppl.h>

// Step06A4D: the Step06A3 acceptance/infill policy with bounded histogram
// medians for NVOF S10.5 vectors. Only call this BEFORE FilmMotionStabilizer:
// each input component must be an exact multiple of 1/32 pixel.
class NvofConfidenceRepair {
public:
    struct Stats {
        uint8_t costQ25=0, costQ50=0, costQ75=0, costThreshold=0;
        float globalX=0, globalY=0;
        bool globalTrusted=false;
        double texturedPct=0, reliablePct=0, globalCoveragePct=0;
        double localFillPct=0, globalFillPct=0, zeroPct=0;
        double textureMs=0, classifyMs=0, infillMs=0;
    };

    void Reset() {
        m_texture.clear(); m_luma.clear(); m_reliable.clear();
        m_stripes.clear(); m_rows.clear();
    }

    Stats Run(std::vector<float>& motion, const uint8_t* bgra, const uint8_t* cost,
              uint32_t width, uint32_t height, uint32_t grid) {
        Stats result{};
        if (!width || !height || !grid || !bgra || !cost) return result;
        const uint32_t gridW=(width+grid-1)/grid, gridH=(height+grid-1)/grid;
        const size_t cells=size_t(gridW)*gridH;
        if (motion.size()!=cells*2) return result;
        m_texture.resize(cells); m_luma.resize(cells); m_reliable.resize(cells);
        const uint32_t stripeCount=std::min(16u,gridH);
        m_stripes.resize(stripeCount);
        m_rows.resize(gridH);
        const auto textureStart=Clock::now();
        const uint32_t radius=std::max(2u,grid*2u);

        // Fine-grained rows let PPL balance textured regions and letterbox bars
        // while the decoder and depth worker compete for CPU time.
        Concurrency::parallel_for(0u,gridH,[&](uint32_t gy) {
                const uint32_t cy=std::min(height-1,gy*grid+grid/2);
                const uint32_t yu=cy>radius?cy-radius:0, yd=std::min(height-1,cy+radius);
                const auto* row=bgra+size_t(cy)*width*4;
                const auto* upper=bgra+size_t(yu)*width*4;
                const auto* lower=bgra+size_t(yd)*width*4;
                for (uint32_t gx=0;gx<gridW;++gx) {
                    const uint32_t cx=std::min(width-1,gx*grid+grid/2);
                    const uint32_t xl=cx>radius?cx-radius:0, xr=std::min(width-1,cx+radius);
                    const uint8_t c=Luma(row+size_t(cx)*4), l=Luma(row+size_t(xl)*4);
                    const uint8_t r=Luma(row+size_t(xr)*4), u=Luma(upper+size_t(cx)*4), d=Luma(lower+size_t(cx)*4);
                    const auto lo=std::min({c,l,r,u,d}), hi=std::max({c,l,r,u,d});
                    const size_t cell=size_t(gy)*gridW+gx;
                    m_luma[cell]=c; m_texture[cell]=uint8_t(hi-lo);
                }
        });
        const auto textureEnd=Clock::now();
        std::array<uint32_t,256> costs{};
        size_t textured=0;
        for(size_t cell=0;cell<cells;++cell) if(m_texture[cell]>=12) ++costs[cost[cell]];
        for (auto count:costs) textured+=count;
        auto percentile=[&](uint32_t num,uint32_t den) {
            if (!textured) return uint8_t(0);
            const size_t target=(textured*num+den-1)/den;
            size_t count=0;
            for (size_t i=0;i<costs.size();++i) { count+=costs[i]; if(count>=target)return uint8_t(i); }
            return uint8_t(255);
        };
        result.costQ25=percentile(1,4); result.costQ50=percentile(1,2); result.costQ75=percentile(3,4);
        result.costThreshold=uint8_t(std::clamp(int(result.costQ75)+6,24,160));
        result.texturedPct=100.0*double(textured)/double(cells);

        Concurrency::parallel_for(0u,stripeCount,[&](uint32_t stripeIndex) {
            auto& stripe=m_stripes[stripeIndex];
            stripe.x.fill(0); stripe.y.fill(0); stripe.reliable=0; stripe.coverage=0;
            const uint32_t first=gridH*stripeIndex/stripeCount;
            const uint32_t last=gridH*(stripeIndex+1)/stripeCount;
            for(uint32_t gy=first;gy<last;++gy) {
                const uint32_t ty=std::min(3u,gy*4/gridH);
                for(uint32_t gx=0;gx<gridW;++gx) {
                    const size_t cell=size_t(gy)*gridW+gx;
                    bool good=false;
                    if(m_texture[cell]>=12 && cost[cell]<=result.costThreshold) {
                        const float x=motion[cell*2],y=motion[cell*2+1];
                        // Keep sqrt and comparison identical to the reference at the boundary.
                        good=std::sqrt(x*x+y*y)<=96.0f;
                        if(good) {
                            ++stripe.x[size_t(int(x*32)+MaxSeedUnits)];
                            ++stripe.y[size_t(int(y*32)+MaxSeedUnits)];
                            ++stripe.reliable;
                            const uint32_t tx=std::min(7u,gx*8/gridW);
                            stripe.coverage|=uint32_t(1)<<(ty*8+tx);
                        }
                    }
                    m_reliable[cell]=good?1:0;
                }
            }
        });
        size_t reliable=0;
        uint32_t coverage=0;
        for(const auto& stripe:m_stripes) { reliable+=stripe.reliable; coverage|=stripe.coverage; }
        uint32_t covered=0;
        for(uint32_t bits=coverage;bits;bits>>=1) covered+=bits&1;
        result.globalTrusted=reliable>=8 && covered>=8;
        result.reliablePct=100.0*double(reliable)/double(cells);
        result.globalCoveragePct=100.0*double(covered)/32.0;
        if(result.globalTrusted) {
            // nth_element's upper median, including even seed counts and ties.
            size_t xCount=0,yCount=0;
            bool xFound=false,yFound=false;
            for(size_t bin=0;bin<SeedBins && (!xFound||!yFound);++bin) {
                for(const auto& stripe:m_stripes) { xCount+=stripe.x[bin]; yCount+=stripe.y[bin]; }
                if(!xFound && xCount>reliable/2) { result.globalX=float(int(bin)-MaxSeedUnits)/32.0f; xFound=true; }
                if(!yFound && yCount>reliable/2) { result.globalY=float(int(bin)-MaxSeedUnits)/32.0f; yFound=true; }
            }
        }
        const float globalMagnitude=std::sqrt(result.globalX*result.globalX+result.globalY*result.globalY);
        const bool staticGlobal=result.globalTrusted && globalMagnitude<0.08f;
        const auto classifyEnd=Clock::now();

        Concurrency::parallel_for(0u,gridH,[&](uint32_t gy) {
            Row counts{};
            for(uint32_t gx=0;gx<gridW;++gx) {
                const size_t cell=size_t(gy)*gridW+gx;
                if(m_reliable[cell])continue;
                float xs[25],ys[25];
                struct Pair { float x,y; } neighbors[25];
                size_t count=0;
                const int y0=std::max(0,int(gy)-2),y1=std::min(int(gridH)-1,int(gy)+2);
                const int x0=std::max(0,int(gx)-2),x1=std::min(int(gridW)-1,int(gx)+2);
                for(int yy=y0;yy<=y1;++yy) for(int xx=x0;xx<=x1;++xx) {
                    const size_t n=size_t(yy)*gridW+size_t(xx);
                    // Reliable seed cells are immutable during infill, including across stripes.
                    if(!m_reliable[n] || std::abs(int(m_luma[n])-int(m_luma[cell]))>18)continue;
                    xs[count]=motion[n*2]; ys[count]=motion[n*2+1];
                    neighbors[count]={xs[count],ys[count]}; ++count;
                }
                float fillX=0,fillY=0;
                bool usedLocal=false,usedGlobal=false;
                if(count>=3) {
                    const float x=Median(xs,count),y=Median(ys,count);
                    float deviations[25];
                    for(size_t n=0;n<count;++n) {
                        const float dx=neighbors[n].x-x,dy=neighbors[n].y-y;
                        deviations[n]=std::sqrt(dx*dx+dy*dy);
                    }
                    if(Median(deviations,count)<=1.5f) { fillX=x; fillY=y; usedLocal=true; }
                }
                if(!usedLocal && result.globalTrusted) { fillX=result.globalX; fillY=result.globalY; usedGlobal=true; }
                if((!usedLocal&&!usedGlobal) || (!usedLocal && m_texture[cell]<6 && staticGlobal)) {
                    fillX=0;fillY=0;++counts.zero;
                } else if(usedLocal) ++counts.local;
                else if(usedGlobal) ++counts.global;
                motion[cell*2]=fillX; motion[cell*2+1]=fillY;
            }
            m_rows[gy]=counts;
        });
        size_t local=0,global=0,zero=0;
        for(const auto& row:m_rows) { local+=row.local; global+=row.global; zero+=row.zero; }
        result.localFillPct=100.0*double(local)/double(cells);
        result.globalFillPct=100.0*double(global)/double(cells);
        result.zeroPct=100.0*double(zero)/double(cells);
        const auto end=Clock::now();
        result.textureMs=Milliseconds(textureStart,textureEnd);
        result.classifyMs=Milliseconds(textureEnd,classifyEnd);
        result.infillMs=Milliseconds(classifyEnd,end);
        return result;
    }

private:
    using Clock=std::chrono::steady_clock;
    static constexpr int MaxSeedUnits=96*32;
    static constexpr size_t SeedBins=MaxSeedUnits*2+1;
    struct Stripe {
        std::array<uint32_t,SeedBins> x{},y{};
        uint32_t reliable=0,coverage=0;
    };
    struct Row { uint32_t local=0,global=0,zero=0; };
    std::vector<uint8_t> m_texture,m_luma,m_reliable;
    std::vector<Stripe> m_stripes;
    std::vector<Row> m_rows;

    static uint8_t Luma(const uint8_t* p) { return uint8_t((19u*p[0]+183u*p[1]+54u*p[2]+128u)>>8); }
    static float Median(float* values,size_t count) {
        std::nth_element(values,values+count/2,values+count);
        return values[count/2];
    }
    static double Milliseconds(Clock::time_point a,Clock::time_point b) {
        return std::chrono::duration<double,std::milli>(b-a).count();
    }
};
