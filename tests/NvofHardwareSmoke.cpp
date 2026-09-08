#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>
#include "OpticalFlowEngine.h"

using Microsoft::WRL::ComPtr;

static uint32_t Hash2(uint32_t x,uint32_t y){
    uint32_t v=x*0x9E3779B9u ^ y*0x85EBCA6Bu ^ 0xC2B2AE35u;
    v^=v>>16;v*=0x7FEB352Du;v^=v>>15;v*=0x846CA68Bu;v^=v>>16;return v;
}
static void MakeTexture(uint32_t w,uint32_t h,std::vector<uint8_t>& out){
    out.resize(size_t(w)*h*4u);
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
        const uint32_t h0=Hash2(x/3u,y/3u),h1=Hash2(x/17u,y/17u);
        const uint8_t fine=uint8_t(h0&255u),coarse=uint8_t((h1>>8)&255u);
        const uint8_t grad=uint8_t((x*5u+y*3u)&255u);
        const size_t o=(size_t(y)*w+x)*4u;
        out[o+0]=uint8_t((uint32_t(fine)*2u+coarse+grad)/4u);
        out[o+1]=uint8_t((uint32_t(coarse)*2u+fine+(255u-grad))/4u);
        out[o+2]=uint8_t((uint32_t(fine)+coarse+uint8_t((x+y)&255u))/3u);
        out[o+3]=255u;
    }
}
static void TranslateWrap(const std::vector<uint8_t>& src,uint32_t w,uint32_t h,int dx,int dy,std::vector<uint8_t>& dst){
    dst.resize(src.size());
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){
        int sx=int(x)-dx,sy=int(y)-dy;
        sx%=int(w);if(sx<0)sx+=int(w);sy%=int(h);if(sy<0)sy+=int(h);
        const size_t so=(size_t(sy)*w+uint32_t(sx))*4u,doff=(size_t(y)*w+x)*4u;
        dst[doff+0]=src[so+0];dst[doff+1]=src[so+1];dst[doff+2]=src[so+2];dst[doff+3]=255u;
    }
}
static float Median(std::vector<float> v){
    if(v.empty())return 0.0f;const size_t n=v.size()/2u;std::nth_element(v.begin(),v.begin()+n,v.end());return v[n];
}
int wmain(){
    constexpr uint32_t W=1918,H=1080;constexpr int DX=8,DY=0;
    ComPtr<IDXGIFactory6> factory;if(FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)))){std::cerr<<"CreateDXGIFactory2 failed\n";return 10;}
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&a))==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if((d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)==0 && d.VendorId==0x10DE){adapter=a;break;}}
    if(!adapter){std::cerr<<"No NVIDIA hardware adapter found\n";return 11;}
    DXGI_ADAPTER_DESC1 ad{};adapter->GetDesc1(&ad);std::wcout<<L"GPU="<<ad.Description<<L"\n";
    ComPtr<ID3D12Device> device;if(FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)))){std::cerr<<"D3D12CreateDevice failed\n";return 12;}
    OpticalFlowEngine of;if(!of.Initialize(device.Get(),W,H,2)){std::cerr<<"OpticalFlowEngine Initialize failed\n";return 13;}
    std::vector<uint8_t> prev,cur;MakeTexture(W,H,prev);TranslateWrap(prev,W,H,DX,DY,cur);
    OpticalFlowFrame first,second;if(!of.Generate(prev.data(),prev.size(),true,first)){std::cerr<<"First Generate failed\n";return 14;}if(first.valid){std::cerr<<"First frame unexpectedly has history\n";return 15;}
    if(!of.Generate(cur.data(),cur.size(),false,second)||!second.valid){std::cerr<<"Second Generate did not produce flow\n";return 16;}
    std::vector<float> xs,ys;xs.reserve(size_t(second.gridW)*second.gridH);ys.reserve(xs.capacity());
    const uint32_t borderX=std::max(8u,80u/std::max(1u,second.gridSize)),borderY=std::max(8u,48u/std::max(1u,second.gridSize));
    size_t active=0,total=0;double meanMag=0.0;
    for(uint32_t y=borderY;y+borderY<second.gridH;++y)for(uint32_t x=borderX;x+borderX<second.gridW;++x){const size_t i=(size_t(y)*second.gridW+x)*2u;const float vx=second.motionXY[i],vy=second.motionXY[i+1u];if(!std::isfinite(vx)||!std::isfinite(vy))continue;xs.push_back(vx);ys.push_back(vy);const float mag=std::sqrt(vx*vx+vy*vy);meanMag+=mag;++total;if(mag>0.25f)++active;}
    if(xs.empty()){std::cerr<<"No interior flow samples\n";return 17;}
    const float mx=Median(xs),my=Median(ys);meanMag/=double(std::max<size_t>(1,total));const double activePct=100.0*double(active)/double(std::max<size_t>(1,total));
    const float expectedX=-float(DX),expectedY=-float(DY);const float err=std::sqrt((mx-expectedX)*(mx-expectedX)+(my-expectedY)*(my-expectedY));const float reverseErr=std::sqrt((mx+expectedX)*(mx+expectedX)+(my+expectedY)*(my+expectedY));
    std::cout<<"NVOF grid="<<second.gridW<<"x"<<second.gridH<<" hwGrid="<<second.gridSize<<"\n";
    std::cout<<"Synthetic content translation prev->current=(+"<<DX<<","<<DY<<") px\n";
    std::cout<<"Expected current->previous=("<<expectedX<<","<<expectedY<<") px\n";
    std::cout<<"Measured median=("<<mx<<","<<my<<") px meanMagnitude="<<meanMag<<" active>0.25px="<<activePct<<"%\n";
    if(err<=2.0f){const auto stats=of.GetStats(); std::cout<<"NVOF_HW_SMOKE=PASS input="<<W<<"x"<<H<<" totalMs="<<stats.lastTotalMs<<" stabilizeMs="<<stats.lastStabilizeMs<<" error="<<err<<" px\n"; OpticalFlowFrame resetFrame; if(!of.Generate(prev.data(),prev.size(),true,resetFrame)||resetFrame.valid){std::cerr<<"Reset did not clear history\n";return 22;} return 0;}
    if(reverseErr<=2.0f){std::cerr<<"[FAIL] NVOFA direction appears reversed relative to current->previous contract.\n";return 20;}
    std::cerr<<"[FAIL] NVOFA synthetic translation mismatch. expected error="<<err<<" px reverse error="<<reverseErr<<" px\n";return 21;
}
