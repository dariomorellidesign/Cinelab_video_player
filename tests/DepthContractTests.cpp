#include "TemporalGuides.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
void check(bool b){if(!b)throw std::runtime_error("depth contract violated");}
int main(){try{
std::vector<uint8_t> pixels(192*108*4,100);std::vector<float> ai(16*16,.2f);
ExternalDepthField field{ai.data(),16,16,0,0,true};
auto run=[&](TemporalGuideGenerator::DepthMode mode,const ExternalDepthField* d){TemporalGuideGenerator g;g.SetDepthMode(mode);g.SetOutputGrid(96,54);GuideFrame f;check(g.Generate(pixels.data(),192,108,192,108,60,true,f,nullptr,d));check(g.Generate(pixels.data(),192,108,192,108,60,false,f,nullptr,d));return f;};
auto z=[&](const GuideFrame& f,float value){for(size_t i=2;i<f.guideGridRGBA32F.size();i+=4)check(std::abs(f.guideGridRGBA32F[i]-value)<1e-5f);};
auto flat=run(TemporalGuideGenerator::DepthMode::Flat,&field);z(flat,.75f);check(!flat.maskUsedAIDepth);
auto legacy=run(TemporalGuideGenerator::DepthMode::Estimated,&field);check(legacy.guideGridRGBA32F==run(TemporalGuideGenerator::DepthMode::Estimated,nullptr).guideGridRGBA32F);check(!legacy.maskUsedAIDepth);
_putenv_s("DMP_MASK_DEPTH","legacy");auto selected=run(TemporalGuideGenerator::DepthMode::AI,&field);z(selected,.8f);check(selected.maskUsedAIDepth);
field.valid=false;auto stale=run(TemporalGuideGenerator::DepthMode::AI,&field);z(stale,.75f);check(!stale.maskUsedAIDepth);
TemporalGuideGenerator g;GuideFrame f;g.Generate(pixels.data(),192,108,192,108,60,true,f);g.Generate(pixels.data(),192,108,192,108,60,false,f);g.SetDepthMode(TemporalGuideGenerator::DepthMode::Flat);g.Generate(pixels.data(),192,108,192,108,60,false,f);check(!f.hasHistory);z(f,.75f);
std::vector<float> lum(64),fx(64),fy(64),mm(64),constant(64,.75f),a,b;
for(size_t i=0;i<64;++i){lum[i]=float(i%9)/9;fx[i]=float(i%3);fy[i]=float(i%5);mm[i]=float(i%7)/8;}
SoftTemporalMaskProcessor full,bypass;check(full.Build(lum,fx,fy,mm,constant,8,8,true,a));check(bypass.Build(lum,fx,fy,mm,constant,8,8,true,b,true));check(a==b);
std::cout<<"Flat, Legacy, AI, invalid fallback, env isolation, history reset, constant-mask equivalence PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what();return 1;}}

