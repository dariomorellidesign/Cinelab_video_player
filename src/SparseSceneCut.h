#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

// Samples the BGRA buffer already produced by the decoder. No GPU readback,
// no extra frame copy and no full-resolution CPU image analysis.
class SparseSceneCut {
    static constexpr int W=32,H=18,N=W*H;
    std::array<float,N> previous{};
    bool have=false;
public:
    bool Update(const uint8_t* bgra,uint32_t width,uint32_t height,bool reset){
        std::array<float,N> current{};
        for(int y=0;y<H;++y)for(int x=0;x<W;++x){
            const uint32_t px=uint32_t((x+.5)*width/W),py=uint32_t((y+.5)*height/H);
            float sum=0;
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
                const auto xx=std::clamp(int(px)+dx,0,int(width)-1),yy=std::clamp(int(py)+dy,0,int(height)-1);
                const uint8_t* p=bgra+(size_t(yy)*width+xx)*4;
                sum+=(.2126f*p[2]+.7152f*p[1]+.0722f*p[0])/255;
            }
            current[y*W+x]=sum/9;
        }
        bool cut=false;
        if(have&&!reset){
            float sum=0;int changed=0;
            std::array<float,16> a{},b{};
            for(int i=0;i<N;++i){float d=std::abs(current[i]-previous[i]);sum+=d;changed+=d>.14f;a[std::min(15,int(current[i]*16))]++;b[std::min(15,int(previous[i]*16))]++;}
            float histogram=0;for(int i=0;i<16;++i)histogram+=std::abs(a[i]-b[i])/(2*N);
            // Both broad change and a distribution change are required. Fast pans
            // with unchanged histogram do not trigger this conservative detector.
            cut=changed>N*.60f && sum/N>.18f && histogram>.25f;
        }
        previous=current;have=true;return cut;
    }
};
