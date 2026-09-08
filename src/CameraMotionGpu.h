#pragma once
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include "Log.h"

// One workgroup fits a similarity motion field from 256 spatially distributed
// observations. All model data stays in VRAM. Units: source pixels, centered
// position divided by max(source width, source height).
inline constexpr char CameraMotionHlsl[]=R"(
Texture2D<int2> Flow:register(t0);
Texture2D<uint> Cost:register(t1);
Texture2D<float4> Current:register(t2);
Texture2D<float4> Previous:register(t3);
SamplerState S:register(s0);
RWStructuredBuffer<float4> Model:register(u0);
cbuffer Config:register(b0){uint PairValid;};
groupshared float4 A[256];
groupshared float4 B[256];
groupshared float4 D[256];
groupshared float4 Fit;
float lum(float3 c){return dot(c,float3(.2126,.7152,.0722));}
float2 predict(float4 f,float2 p){return f.xy+f.z*p+f.w*float2(-p.y,p.x);}
[numthreads(256,1,1)]
void FitCamera(uint id:SV_GroupIndex){
    if(PairValid==0){if(id==0){Model[0]=0;Model[1]=0;}return;}
    uint w,h,gw,gh;Current.GetDimensions(w,h);Flow.GetDimensions(gw,gh);
    float2 uv=(float2(id%16,id/16)+.5)/16;
    float2 p=(uv-.5)*float2(w,h)/max(w,h);
    int2 g=min(int2(uv*float2(gw,gh)),int2(gw-1,gh-1));
    float2 v=float2(Flow.Load(int3(g,0)))/32;
    float2 d=1.0/float2(w,h);
    float c=lum(Current.SampleLevel(S,uv,0).rgb);
    float contrast=max(abs(c-lum(Current.SampleLevel(S,uv+float2(2*d.x,0),0).rgb)),abs(c-lum(Current.SampleLevel(S,uv+float2(0,2*d.y),0).rgb)));
    float error=abs(c-lum(Previous.SampleLevel(S,uv+v*d,0).rgb));
    float direct=abs(c-lum(Previous.SampleLevel(S,uv,0).rgb));
    float valid=(contrast>.012 && Cost.Load(int3(g,0))<100 && length(v)<48 && error<.06 && error<direct+.012)?1:0;
    // IRLS: ordinary fit followed by three robust reweightings. Large independent
    // motions lose influence. No temporal averaging, so there is no camera lag.
    for(uint iteration=0;iteration<4;++iteration){
        float weight=valid;
        if(iteration>0){float r=length(v-predict(Fit,p));weight*=min(1.0,.30/max(r,.001));}
        A[id]=float4(weight,weight*p,weight*dot(p,p));
        B[id]=float4(weight*v,weight*dot(p,v),weight*(p.x*v.y-p.y*v.x));
        GroupMemoryBarrierWithGroupSync();
        for(uint stride=128;stride>0;stride>>=1){if(id<stride){A[id]+=A[id+stride];B[id]+=B[id+stride];}GroupMemoryBarrierWithGroupSync();}
        if(id==0){
            float n=max(A[0].x,.001);float2 center=A[0].yz/n,mean=B[0].xy/n;
            float denom=max(A[0].w-n*dot(center,center),.0001);
            float scale=(B[0].z-n*dot(center,mean))/denom;
            float rot=(B[0].w-n*(center.x*mean.y-center.y*mean.x))/denom;
            Fit=float4(mean-scale*center-rot*float2(-center.y,center.x),scale,rot);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    float residual=length(v-predict(Fit,p));
    float inlier=valid*(residual<.45?1:0);
    A[id]=float4(valid,inlier,inlier*residual,0);
    uint quadrant=(uv.x>.5?1:0)+(uv.y>.5?2:0);
    D[id]=float4(quadrant==0,quadrant==1,quadrant==2,quadrant==3)*inlier;
    GroupMemoryBarrierWithGroupSync();
    for(uint stride=128;stride>0;stride>>=1){if(id<stride){A[id]+=A[id+stride];D[id]+=D[id+stride];}GroupMemoryBarrierWithGroupSync();}
    if(id==0){
        float ratio=A[0].y/max(A[0].x,1);
        float spread=min(min(D[0].x,D[0].y),min(D[0].z,D[0].w));
        // Require support across ALL quadrants, not just one moving foreground.
        float trusted=(A[0].y>=32 && ratio>=.75 && spread>=4 && length(Fit.xy)<12 && length(Fit.zw)<24)?1:0;
        Model[0]=Fit;Model[1]=float4(trusted,ratio,A[0].y,A[0].z/max(A[0].y,1));
    }
}
)";

class CameraMotionGpu {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    Microsoft::WRL::ComPtr<ID3D12Resource> model;
    bool readable=false;
public:
    bool Initialize(ID3D12Device* device){
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3DBlob> code,errors,signature;
        if(FAILED(D3DCompile(CameraMotionHlsl,strlen(CameraMotionHlsl),nullptr,nullptr,nullptr,"FitCamera","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors))){if(errors)LOG((char*)errors->GetBufferPointer());return false;}
        D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=4;
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={1,&range};
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;params[1].Descriptor.ShaderRegister=0;
        params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants={0,0,1};
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.MaxLOD=D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=3;rs.pParameters=params;rs.NumStaticSamplers=1;rs.pStaticSamplers=&sampler;
        if(FAILED(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&errors)))return false;
        if(FAILED(device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();ps.CS={code->GetBufferPointer(),code->GetBufferSize()};
        if(FAILED(device->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pso))))return false;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=32;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&model)));
    }
    void Record(ID3D12GraphicsCommandList* cmd,D3D12_GPU_DESCRIPTOR_HANDLE inputs,bool valid){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=model.Get();b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if(readable)cmd->ResourceBarrier(1,&b);
        cmd->SetComputeRootSignature(root.Get());cmd->SetPipelineState(pso.Get());cmd->SetComputeRootDescriptorTable(0,inputs);cmd->SetComputeRootUnorderedAccessView(1,model->GetGPUVirtualAddress());cmd->SetComputeRoot32BitConstant(2,valid?1:0,0);cmd->Dispatch(1,1,1);
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;b.Transition.StateAfter=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;cmd->ResourceBarrier(1,&b);readable=true;
    }
    ID3D12Resource* Resource()const{return model.Get();}
};
