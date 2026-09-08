#include "NvofConfidenceRepair.h"
#include "reference/ConfidenceRepairStep06A4C.h"
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <iomanip>

struct Input {
    uint32_t width,height,grid;
    std::vector<uint8_t> color,cost;
    std::vector<float> motion;
};

Input MakeInput(uint32_t w,uint32_t h,uint32_t grid,unsigned seed,int scenario) {
    Input in{w,h,grid,{},{},{}};
    const size_t cells=size_t((w+grid-1)/grid)*((h+grid-1)/grid);
    in.color.resize(size_t(w)*h*4);in.cost.resize(cells);in.motion.resize(cells*2);
    std::mt19937 rng(seed);
    for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x) {
        const size_t i=(size_t(y)*w+x)*4;
        int value=0;
        if(scenario==1)value=int(rng()%256);
        if(scenario==2)value=((x/19+y/23)%3==0)?int(rng()%256):32;
        if(scenario==3)value=(x*5+y*3)%256;
        if(scenario==4)value=(x<w/4 && y<h/4)?int(rng()%256):64;
        if(scenario==5)value=int((x/11+y/7)%2)*12+42;
        for(size_t c=0;c<3;++c)in.color[i+c]=uint8_t(value);
        in.color[i+3]=255;
    }
    for(size_t i=0;i<cells;++i) {
        in.cost[i]=uint8_t((scenario==2)?rng()%32:rng()%256);
        int x=0,y=0;
        if(scenario==0){x=int(rng()%6145)-3072;y=int(rng()%6145)-3072;}
        if(scenario==1){x=int(rng()%8193)-4096;y=int(rng()%8193)-4096;}
        if(scenario==2){x=int(rng()%5)-2;y=int(rng()%5)-2;}
        if(scenario==3){x=320+int(rng()%9)-4;y=-16+int(rng()%7)-3;}
        if(scenario==4){x=3072;y=(i%2)?0:1;in.cost[i]=uint8_t(i%25);}
        if(scenario==5){x=(i%2)?2:-2;y=1;in.cost[i]=24;}
        in.motion[i*2]=float(x)/32;in.motion[i*2+1]=float(y)/32;
    }
    return in;
}

template<class S> auto Policy(const S& s) {
    return std::make_tuple(s.costQ25,s.costQ50,s.costQ75,s.costThreshold,
        s.globalX,s.globalY,s.globalTrusted,s.texturedPct,s.reliablePct,s.globalCoveragePct,
        s.localFillPct,s.globalFillPct,s.zeroPct);
}

void Check(Input& in,NvofConfidenceRepair& optimized,reference_a4c::Repair& reference) {
    auto expected=in.motion,actual=in.motion;
    const auto a=reference.Run(expected,in.color.data(),in.cost.data(),in.width,in.height,in.grid);
    const auto b=optimized.Run(actual,in.color.data(),in.cost.data(),in.width,in.height,in.grid);
    if(expected.size()!=actual.size() || std::memcmp(expected.data(),actual.data(),actual.size()*sizeof(float))!=0)
        throw std::runtime_error("Motion differs from frozen Step06A4C");
    if(Policy(a)!=Policy(b))throw std::runtime_error("Confidence policy statistics differ from Step06A4C");
}

void Benchmark(uint32_t w,uint32_t h,int scenario) {
    auto in=MakeInput(w,h,2,713,scenario);
    NvofConfidenceRepair optimized;reference_a4c::Repair reference;
    Check(in,optimized,reference);
    std::vector<double> oldTimes,newTimes;
    auto run=[&](auto& repair,std::vector<double>& times) {
        auto motion=in.motion;
        const auto start=std::chrono::steady_clock::now();
        repair.Run(motion,in.color.data(),in.cost.data(),w,h,2);
        const auto end=std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double,std::milli>(end-start).count());
    };
    for(int i=0;i<12;++i) {
        if(i%2){run(optimized,newTimes);run(reference,oldTimes);}
        else{run(reference,oldTimes);run(optimized,newTimes);}
    }
    std::sort(oldTimes.begin(),oldTimes.end());std::sort(newTimes.begin(),newTimes.end());
    std::cout<<"BENCH input="<<w<<"x"<<h<<" scenario="<<scenario
        <<" referenceMedianMs="<<oldTimes[oldTimes.size()/2]
        <<" optimizedMedianMs="<<newTimes[newTimes.size()/2]
        <<" speedup="<<oldTimes[oldTimes.size()/2]/newTimes[newTimes.size()/2]<<"\n";
}

int main(int argc,char** argv) {
    try {
        NvofConfidenceRepair optimized;reference_a4c::Repair reference;
        unsigned cases=0;
        for(auto dims: {std::pair{1u,1u},{3u,5u},{65u,37u},{160u,96u},{321u,181u}})
            for(uint32_t grid:{1u,2u,4u}) for(int scenario=0;scenario<6;++scenario)
                for(unsigned seed=1;seed<=4;++seed) {
                    auto in=MakeInput(dims.first,dims.second,grid,seed,scenario);
                    Check(in,optimized,reference);++cases;
                }
        // Full-size cases exercise stripe boundaries, large seed counts and histogram medians.
        for(auto dims:{std::pair{1278u,720u},{1918u,1080u},{3840u,2160u}})
            for(int scenario:{1,2,3,4,5}) {
                auto in=MakeInput(dims.first,dims.second,2,923,scenario);
                Check(in,optimized,reference);++cases;
            }
        optimized.Reset();
        auto in=MakeInput(101,55,2,391,2);Check(in,optimized,reference);++cases;
        std::cout<<"CONFIDENCE_EQUIVALENCE=PASS cases="<<cases<<" bitExactMotion=1 exactPolicyStats=1\n";
        if(argc>1 && std::string(argv[1])=="--benchmark") {
            std::cout<<std::fixed<<std::setprecision(3);
            for(auto dims:{std::pair{1278u,720u},{1918u,1080u},{3840u,2160u}})
                for(int scenario:{1,2,3})Benchmark(dims.first,dims.second,scenario);
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
