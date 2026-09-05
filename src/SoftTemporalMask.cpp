#include "SoftTemporalMask.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace {
float Saturate(float v){ return std::clamp(v,0.0f,1.0f); }
float SmoothStep(float a,float b,float x){
    if(!(b>a)) return x>=b?1.0f:0.0f;
    const float t=Saturate((x-a)/(b-a));
    return t*t*(3.0f-2.0f*t);
}
}

void SoftTemporalMaskProcessor::Reset(){
    m_previous.clear();
    m_stats={};
}

bool SoftTemporalMaskProcessor::Build(const std::vector<float>& luma,
                                      const std::vector<float>& flowX,
                                      const std::vector<float>& flowY,
                                      const std::vector<float>& mismatch,
                                      const std::vector<float>& depth,
                                      uint32_t width,uint32_t height,
                                      bool history,
                                      std::vector<float>& outMask){
    const size_t n=size_t(width)*height;
    if(!width||!height||luma.size()!=n||flowX.size()!=n||flowY.size()!=n||mismatch.size()!=n||depth.size()!=n){
        outMask.assign(n,0.0f);Reset();return false;
    }
    if(!history){outMask.assign(n,0.0f);m_previous=outMask;m_stats={};return true;}

    std::vector<float> raw(n,0.0f);
    for(uint32_t y=0;y<height;++y){
        for(uint32_t x=0;x<width;++x){
            const size_t i=size_t(y)*width+x;
            const uint32_t xl=x?x-1:x, xr=std::min(width-1,x+1);
            const uint32_t yt=y?y-1:y, yb=std::min(height-1,y+1);
            const float div=std::abs(flowX[size_t(y)*width+xr]-flowX[size_t(y)*width+xl])+
                            std::abs(flowY[size_t(yb)*width+x]-flowY[size_t(yt)*width+x]);
            const float depthEdge=std::max({
                std::abs(depth[i]-depth[size_t(y)*width+xl]),
                std::abs(depth[i]-depth[size_t(y)*width+xr]),
                std::abs(depth[i]-depth[size_t(yt)*width+x]),
                std::abs(depth[i]-depth[size_t(yb)*width+x])});
            const float lumaEdge=std::max({
                std::abs(luma[i]-luma[size_t(y)*width+xl]),
                std::abs(luma[i]-luma[size_t(y)*width+xr]),
                std::abs(luma[i]-luma[size_t(yt)*width+x]),
                std::abs(luma[i]-luma[size_t(yb)*width+x])});

            const float mismatchScore=SmoothStep(0.035f,0.18f,mismatch[i]);
            const float motionScore=SmoothStep(0.30f,2.40f,div);
            const float depthScore=SmoothStep(0.018f,0.11f,depthEdge);
            const float structure=std::max(motionScore,depthScore);

            // Film grain usually raises photometric mismatch without producing a coherent
            // motion/depth boundary. Suppress that isolated mismatch while retaining strong
            // real disocclusion evidence and structural boundaries.
            const float grainLike=SmoothStep(0.025f,0.11f,lumaEdge)*(1.0f-structure);
            float photometric=mismatchScore*(0.18f+0.82f*structure);
            photometric*=1.0f-0.55f*grainLike;
            const float severeMismatch=SmoothStep(0.28f,0.55f,mismatch[i]);
            float v=std::max({0.82f*motionScore,0.58f*depthScore,photometric,0.75f*severeMismatch});
            raw[i]=Saturate(v);
        }
    }

    // Two-pass 5-tap depth-aware Gaussian. This removes block boundaries but prevents
    // the mask from freely bleeding across large depth discontinuities.
    static constexpr float k[5]={1.0f,4.0f,6.0f,4.0f,1.0f};
    std::vector<float> temp(n,0.0f),smooth(n,0.0f);
    auto depthWeight=[](float a,float b){ return std::exp(-std::abs(a-b)*22.0f); };
    for(uint32_t y=0;y<height;++y){
        for(uint32_t x=0;x<width;++x){
            const size_t i=size_t(y)*width+x;float s=0.0f,w=0.0f;
            for(int d=-2;d<=2;++d){
                const uint32_t xx=uint32_t(std::clamp<int>(int(x)+d,0,int(width)-1));
                const size_t j=size_t(y)*width+xx;const float ww=k[d+2]*depthWeight(depth[i],depth[j]);s+=raw[j]*ww;w+=ww;
            }
            temp[i]=w>0.0f?s/w:raw[i];
        }
    }
    for(uint32_t y=0;y<height;++y){
        for(uint32_t x=0;x<width;++x){
            const size_t i=size_t(y)*width+x;float s=0.0f,w=0.0f;
            for(int d=-2;d<=2;++d){
                const uint32_t yy=uint32_t(std::clamp<int>(int(y)+d,0,int(height)-1));
                const size_t j=size_t(yy)*width+x;const float ww=k[d+2]*depthWeight(depth[i],depth[j]);s+=temp[j]*ww;w+=ww;
            }
            smooth[i]=w>0.0f?s/w:temp[i];
        }
    }

    outMask.resize(n);
    if(m_previous.size()!=n)m_previous.assign(n,0.0f);
    double sum=0.0;float maxV=0.0f;size_t active=0;
    for(size_t i=0;i<n;++i){
        const float prev=m_previous[i],target=Saturate(smooth[i]);
        // Fast attack for a new disocclusion, slower release to avoid on/off flicker.
        const float alpha=target>prev?0.72f:0.20f;
        const float v=Saturate(prev+(target-prev)*alpha);
        outMask[i]=v;m_previous[i]=v;sum+=v;maxV=std::max(maxV,v);if(v>=0.10f)++active;
    }
    m_stats.mean=n?float(sum/double(n)):0.0f;
    m_stats.maxValue=maxV;
    m_stats.activeFraction=n?float(double(active)/double(n)):0.0f;
    return true;
}
