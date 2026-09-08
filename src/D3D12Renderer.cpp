#include "OpticalFlowEngine.h"
#include "D3D12Renderer.h"
#include "MotionResolveShader.h"
#include "SplitScreenLayout.h"
#include "DLSSFrameGeneration.h"
#include "SubtitleOverlayLayout.h"
#include "Log.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <cmath>

using Microsoft::WRL::ComPtr;

static bool HR(HRESULT hr, const char* what) {
    if (FAILED(hr)) { LOG(what << " failed hr=0x" << std::hex << hr); return false; }
    return true;
}
static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{}; p.Type=type; p.CreationNodeMask=1; p.VisibleNodeMask=1; return p;
}
static D3D12_RESOURCE_DESC Tex2D(DXGI_FORMAT fmt,uint32_t w,uint32_t h,D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=w; d.Height=h;
    d.DepthOrArraySize=1; d.MipLevels=1; d.Format=fmt; d.SampleDesc={1,0}; d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags=flags; return d;
}
static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource=r;
    x.Transition.StateBefore=a; x.Transition.StateAfter=b; x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; return x;
}

D3D12Renderer::~D3D12Renderer() {
    WaitGPU();
    for (uint32_t i=0;i<FrameCount;++i) {
        if (m_upload[i] && m_uploadMapped[i]) m_upload[i]->Unmap(0,nullptr);
        if (m_guideUpload[i] && m_guideMapped[i]) m_guideUpload[i]->Unmap(0,nullptr);
        if (m_aiDepthUpload[i] && m_aiDepthMapped[i]) m_aiDepthUpload[i]->Unmap(0,nullptr);
        m_uploadMapped[i]=nullptr;
        m_guideMapped[i]=nullptr;
        m_aiDepthMapped[i]=nullptr;
    }
    for (uint32_t i=0;i<FrameCount;++i) {
        if (m_subtitleUpload[i] && m_subtitleUploadMapped[i]) m_subtitleUpload[i]->Unmap(0,nullptr);
        m_subtitleUploadMapped[i]=nullptr;
    }
    m_dlss.Shutdown();
    DLSSFrameGenerationRuntime::Instance().ShutdownForDevice(m_device.Get()); // device-safe: retired renderer cannot shut down a newer Streamline session
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
}

bool D3D12Renderer::Initialize(HWND hwnd,uint32_t sourceW,uint32_t sourceH,uint32_t outputW,uint32_t outputH,uint32_t gridW,uint32_t gridH,NVSDK_NGX_PerfQuality_Value quality) {
    m_hwnd=hwnd; m_sourceW=sourceW; m_sourceH=sourceH; m_outputW=outputW; m_outputH=outputH; m_gridW=gridW; m_gridH=gridH; m_quality=quality;
    if(!m_gridW||!m_gridH)return false;
    if(!CreateDeviceAndSwapchain(hwnd) || !CreateHeapsAndBackbuffers() || !CreatePipelines()) return false;
    if(!InitializeDLSS()) {
        LOG("DLSS unavailable; using D3D12 scaler fallback.");
        m_renderW=std::max(1u,outputW*2u/3u); m_renderH=std::max(1u,outputH*2u/3u);
    }
    if(!CreateVideoResources()) return false;
    if(!CreateSubtitleResources()) return false;
    if(!CreateFrameGenerationResources()) return false;
    LOG("V11 guide contract: compact CPU optical-flow grid expanded on GPU into full R16G16_FLOAT MVs + R8 bias; depth is written directly into the same R32_TYPELESS/D32_FLOAT resource passed to NGX; temporal reset only on discontinuities.");
    return true;
}

bool D3D12Renderer::CreateDeviceAndSwapchain(HWND hwnd) {
    auto& frameGen=DLSSFrameGenerationRuntime::Instance();frameGen.InitializeProcess();
    UINT ff=0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> dbg; if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); ff|=DXGI_CREATE_FACTORY_DEBUG; }
#endif
    if(!HR(CreateDXGIFactory2(ff,IID_PPV_ARGS(&m_factory)),"CreateDXGIFactory2")) return false;
    ComPtr<IDXGIAdapter1> fallback;
    for(UINT i=0;;++i){
        ComPtr<IDXGIAdapter1>a; if(m_factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&a))==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d); if(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if(FAILED(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_12_0,_uuidof(ID3D12Device),nullptr))) continue;
        if(!fallback) fallback=a; if(d.VendorId==0x10DE){m_adapter=a;break;}
    }
    if(!m_adapter)m_adapter=fallback; if(!m_adapter){LOG("No D3D12 hardware adapter.");return false;}
    DXGI_ADAPTER_DESC1 ad{};m_adapter->GetDesc1(&ad);LOG("D3D12 adapter vendor=0x"<<std::hex<<ad.VendorId<<" device=0x"<<ad.DeviceId);
    if(!HR(D3D12CreateDevice(m_adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&m_device)),"D3D12CreateDevice"))return false;
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12Device* queueDevice=frameGen.UpgradeDeviceForQueue(m_device.Get());
    HRESULT queueHr=queueDevice->CreateCommandQueue(&q,IID_PPV_ARGS(&m_queue));
    // STEP 05B v1.6 retain Streamline manual-hook proxies.
    // Streamline's D3D12CommandQueue proxy stores the upgraded D3D12Device proxy as its parent.
    // Releasing that device proxy here leaves the queue with a dangling parent before DXGI asks
    // the queue for its device while creating the swapchain. Keep it process-lifetime for bring-up,
    // matching the validated Step05A-1 manual-hook probe. Production ownership comes later.
    if(queueDevice!=m_device.Get())LOG("[DLSS-G] retaining D3D12 device proxy for command-queue lifetime");
    if(!HR(queueHr,"CreateCommandQueue"))return false;
    for(uint32_t i=0;i<FrameCount;++i) {
        if(!HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&m_allocators[i])),"CreateCommandAllocator"))return false;
    }
    for(uint32_t i=0;i<FrameCount;++i) {
        if(!HR(m_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,m_allocators[i].Get(),nullptr,IID_PPV_ARGS(&m_cmds[i])),"CreateCommandList"))return false;
        m_cmds[i]->Close();
    }
    BOOL tearing=FALSE;if(SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing))))m_allowTearing=tearing==TRUE;
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=m_outputW;sd.Height=m_outputH;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc={1,0};sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount=FrameCount;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sd.Scaling=DXGI_SCALING_STRETCH;sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;sd.Flags=m_allowTearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
    IDXGIFactory6* swapFactory=frameGen.UpgradeFactoryForSwapchain(m_factory.Get());
    ComPtr<IDXGISwapChain1>sc1;if(!HR(swapFactory->CreateSwapChainForHwnd(m_queue.Get(),hwnd,&sd,nullptr,nullptr,&sc1),"CreateSwapChainForHwnd"))return false;
    m_factory->MakeWindowAssociation(hwnd,DXGI_MWA_NO_ALT_ENTER);sc1.As(&m_swapchain);
    // Keep the manual-hook DXGI factory proxy alive for the same conservative bring-up lifetime.
    // The short-lived Step05A-1 probe intentionally did not release upgraded proxies before shutdown.
    if(swapFactory!=m_factory.Get())LOG("[DLSS-G] retaining DXGI factory proxy after swapchain creation");
    frameGen.OnSwapchainCreated(m_swapchain.Get());
    if(m_swapchain) m_swapchain->SetMaximumFrameLatency(2);
    if(!HR(m_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&m_fence)),"CreateFence"))return false;
    m_fenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);return m_fenceEvent!=nullptr;
}

bool D3D12Renderer::CreateHeapsAndBackbuffers(){
    D3D12_DESCRIPTOR_HEAP_DESC rh{};rh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rh.NumDescriptors=FrameCount+5;
    if(!HR(m_device->CreateDescriptorHeap(&rh,IID_PPV_ARGS(&m_rtvHeap)),"Create RTV heap"))return false;m_rtvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for(uint32_t i=0;i<FrameCount;++i){if(!HR(m_swapchain->GetBuffer(i,IID_PPV_ARGS(&m_backbuffers[i])),"Get backbuffer"))return false;m_device->CreateRenderTargetView(m_backbuffers[i].Get(),nullptr,RTV(i));}
    // Slots 11..22 are three four-SRV tables: NVOF motion, cost, current color,
    // previous color. Keeping each table per frame slot avoids overwriting a
    // descriptor still used by the GPU.
    D3D12_DESCRIPTOR_HEAP_DESC sh{};sh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;sh.NumDescriptors=32;sh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(!HR(m_device->CreateDescriptorHeap(&sh,IID_PPV_ARGS(&m_srvHeap)),"Create SRV heap"))return false;
    m_srvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC dh{};dh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;dh.NumDescriptors=3;
    if(!HR(m_device->CreateDescriptorHeap(&dh,IID_PPV_ARGS(&m_dsvHeap)),"Create DSV heap"))return false;
    m_dsvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return true;
}

bool D3D12Renderer::CreatePipelines(){
    const char* hlsl=R"(
Texture2D T:register(t0); SamplerState S:register(s0);
cbuffer Params:register(b0){
    float2 JitterUV;
    float2 Misc;
    float4 ColorA; // brightness, contrast, saturation, gamma
    float4 ColorB; // temperature, tint, reserved, reserved
}
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);V o;o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}
float3 SRGBToLinear(float3 c){float3 lo=c/12.92;float3 hi=pow(max((c+0.055)/1.055,0),2.4);return lerp(hi,lo,step(c,0.04045));}
float3 LinearToSRGB(float3 c){c=max(c,0);float3 lo=c*12.92;float3 hi=1.055*pow(c,1.0/2.4)-0.055;return saturate(lerp(hi,lo,step(c,0.0031308)));}
float4 PSConvert(V i):SV_Target{float3 c=T.SampleLevel(S,i.uv+JitterUV,0).rgb;return float4(SRGBToLinear(c),1);}
float3 ApplyVideoAdjustments(float3 c){
    float brightness=ColorA.x;
    float contrast=max(ColorA.y,0.0);
    float saturation=max(ColorA.z,0.0);
    float gamma=max(ColorA.w,0.05);
    float temperature=clamp(ColorB.x,-1.0,1.0);
    float tint=clamp(ColorB.y,-1.0,1.0);

    c=max(c,0.0);
    c*=exp2(brightness);
    c=(c-0.18)*contrast+0.18;
    float l=dot(c,float3(0.2126,0.7152,0.0722));
    c=lerp(l.xxx,c,saturation);
    c*=float3(1.0+0.12*temperature,1.0,1.0-0.12*temperature);
    c*=float3(1.0+0.05*tint,1.0-0.10*tint,1.0+0.05*tint);
    c=pow(max(c,0.0),1.0/gamma);
    return c;
}
float4 PSPresent(V i):SV_Target{float3 c=T.SampleLevel(S,i.uv,0).rgb;c=ApplyVideoAdjustments(c);return float4(LinearToSRGB(c),1);}
float4 PSSubtitle(V i):SV_Target{return T.SampleLevel(S,i.uv,0);}
float3 hsv2rgb(float3 c){float4 K=float4(1,2.0/3.0,1.0/3.0,3);float3 p=abs(frac(c.xxx+K.xyz)*6-K.www);return c.z*lerp(K.xxx,saturate(p-K.xxx),c.y);}
float4 PSMotion(V i):SV_Target{
    float2 m=T.SampleLevel(S,i.uv,0).rg;float mag=length(m);
    // MV debug dead-zone: hardware optical flow naturally contains tiny sub-pixel
    // estimates on nominally static regions. Do not turn those into colored snow.
    const float dead=ColorB.z;if(mag<=dead)return float4(0.035,0.035,0.035,1);
    float h=frac(atan2(-m.y,m.x)/6.2831853+1.0);
    float sat=saturate((mag-dead)/1.25);float v=saturate(0.18+(mag-dead)/14.0);
    return float4(hsv2rgb(float3(h,sat,v)),1);
}
float4 PSDepth(V i):SV_Target{float d=saturate(T.SampleLevel(S,i.uv,0).r);d=pow(d,0.7);return float4(d,d,d,1);}
float4 PSAIHardwareDepthDebug(V i):SV_Target{float d=saturate(T.SampleLevel(S,i.uv,0).r);return float4(d,d,d,1);}
float PSAIHardwareDepthWrite(V i):SV_Depth{float relativeNearness=saturate(T.SampleLevel(S,i.uv,0).r);return 1.0-relativeNearness;}
    // Depth comes directly from compact-guide B and is written through SV_Depth into
    // the exact typeless/D32 resource that NGX receives later in the frame.
    float PSWriteDepth(V i):SV_Depth{return saturate(T.SampleLevel(S,i.uv+JitterUV,0).b);}
    struct GuideOut{float2 mv:SV_Target0;float bias:SV_Target1;};
    GuideOut PSExpandGuides(V i){float4 g=T.SampleLevel(S,i.uv+JitterUV,0);GuideOut o;o.mv=g.xy;o.bias=saturate(g.w);return o;}
)";
    UINT flags=D3DCOMPILE_OPTIMIZATION_LEVEL3;ComPtr<ID3DBlob>vs,convert,present,motion,depth,aiHwDepthDebug,aiHwDepthWrite,depthWrite,expand,err;
    auto C=[&](const char*entry,const char*target,ComPtr<ID3DBlob>&out)->bool{err.Reset();HRESULT hr=D3DCompile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,entry,target,flags,0,&out,&err);if(FAILED(hr)){if(err)LOG((char*)err->GetBufferPointer());return false;}return true;};
    ComPtr<ID3DBlob> subtitle;if(!C("PSSubtitle","ps_5_1",subtitle))return false;
    if(!C("VS","vs_5_1",vs)||!C("PSConvert","ps_5_1",convert)||!C("PSPresent","ps_5_1",present)||!C("PSMotion","ps_5_1",motion)||!C("PSDepth","ps_5_1",depth)||!C("PSAIHardwareDepthDebug","ps_5_1",aiHwDepthDebug)||!C("PSAIHardwareDepthWrite","ps_5_1",aiHwDepthWrite)||!C("PSWriteDepth","ps_5_1",depthWrite)||!C("PSExpandGuides","ps_5_1",expand))return false;
    D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;range.BaseShaderRegister=0;
    D3D12_ROOT_PARAMETER rp[2]{};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[0].DescriptorTable.NumDescriptorRanges=1;rp[0].DescriptorTable.pDescriptorRanges=&range;
    rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;rp[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[1].Constants.Num32BitValues=12;rp[1].Constants.ShaderRegister=0;
    D3D12_STATIC_SAMPLER_DESC smp{};smp.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;smp.AddressU=smp.AddressV=smp.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;smp.ShaderRegister=0;smp.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;smp.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=2;rs.pParameters=rp;rs.NumStaticSamplers=1;rs.pStaticSamplers=&smp;rs.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob>sig;if(!HR(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err),"SerializeRootSignature"))return false;
    if(!HR(m_device->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&m_rootSig)),"CreateRootSignature"))return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=m_rootSig.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={convert->GetBufferPointer(),convert->GetBufferSize()};
    p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.BlendState.RenderTarget[1].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.BlendState.RenderTarget[2].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
    p.DepthStencilState.DepthEnable=FALSE;p.DepthStencilState.StencilEnable=FALSE;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.SampleDesc={1,0};
    p.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoConvert)),"Create convert PSO"))return false;
    p.PS={present->GetBufferPointer(),present->GetBufferSize()};
    p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoPresent)),"Create present PSO"))return false;
    p.PS={subtitle->GetBufferPointer(),subtitle->GetBufferSize()};
    p.BlendState.RenderTarget[0].BlendEnable=TRUE;
    p.BlendState.RenderTarget[0].SrcBlend=D3D12_BLEND_SRC_ALPHA;
    p.BlendState.RenderTarget[0].DestBlend=D3D12_BLEND_INV_SRC_ALPHA;
    p.BlendState.RenderTarget[0].BlendOp=D3D12_BLEND_OP_ADD;
    p.BlendState.RenderTarget[0].SrcBlendAlpha=D3D12_BLEND_ONE;
    p.BlendState.RenderTarget[0].DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;
    p.BlendState.RenderTarget[0].BlendOpAlpha=D3D12_BLEND_OP_ADD;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoSubtitle)),"Create subtitle overlay PSO"))return false;
    p.BlendState.RenderTarget[0].BlendEnable=FALSE;    p.PS={motion->GetBufferPointer(),motion->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoMotionDebug)),"Create MV debug PSO"))return false;
    p.PS={depth->GetBufferPointer(),depth->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoDepthDebug)),"Create depth debug PSO"))return false;
    p.PS={aiHwDepthDebug->GetBufferPointer(),aiHwDepthDebug->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoAIHardwareDepthDebug)),"Create AI hardware depth debug PSO"))return false;
    p.PS={expand->GetBufferPointer(),expand->GetBufferSize()};p.NumRenderTargets=2;p.RTVFormats[0]=DXGI_FORMAT_R16G16_FLOAT;p.RTVFormats[1]=DXGI_FORMAT_R8_UNORM;p.RTVFormats[2]=DXGI_FORMAT_UNKNOWN;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoExpandGuides)),"Create GPU guide expansion PSO"))return false;
    // Integer NVOF S10.5 texture -> dense input-pixel RG16F, entirely on GPU.
    // A vector is rejected only when the current/previous pixels show that zero
    // motion explains the image at least as well. Low contrast by itself is never
    // enough: coherent low-cost motion stays alive, protecting object interiors.
    const char* rawHlsl=MotionResolveHlsl;
    ComPtr<ID3DBlob> raw;err.Reset();
    if(FAILED(D3DCompile(rawHlsl,strlen(rawHlsl),nullptr,nullptr,nullptr,"Raw","ps_5_1",flags,0,&raw,&err))){if(err)LOG((char*)err->GetBufferPointer());return false;}
    p.PS={raw->GetBufferPointer(),raw->GetBufferSize()};
    // The ordinary shaders keep a one-SRV root table (including the last color
    // descriptor). Only the MV shader binds four inputs plus a root model SRV.
    D3D12_ROOT_PARAMETER rawParams[3]={rp[0],rp[1],{}};
    D3D12_DESCRIPTOR_RANGE rawRange=range;rawRange.NumDescriptors=4;
    rawParams[0].DescriptorTable.pDescriptorRanges=&rawRange;
    rawParams[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;rawParams[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rawParams[2].Descriptor.ShaderRegister=4;
    D3D12_ROOT_SIGNATURE_DESC rawRs=rs;rawRs.NumParameters=3;rawRs.pParameters=rawParams;
    sig.Reset();err.Reset();
    if(!HR(D3D12SerializeRootSignature(&rawRs,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err),"Serialize raw motion root"))return false;
    if(!HR(m_device->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&m_rawRootSig)),"Create raw motion root"))return false;
    if(!m_cameraMotion.Initialize(m_device.Get()))return false;
    D3D12_QUERY_HEAP_DESC queryDesc{};queryDesc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;queryDesc.Count=FrameCount*4;
    if(!HR(m_device->CreateQueryHeap(&queryDesc,IID_PPV_ARGS(&m_motionQueries)),"Create motion timestamp heap"))return false;
    auto readbackHeap=HeapProps(D3D12_HEAP_TYPE_READBACK);
    D3D12_RESOURCE_DESC timesDesc{};timesDesc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;timesDesc.Width=FrameCount*32;timesDesc.Height=1;timesDesc.DepthOrArraySize=1;timesDesc.MipLevels=1;timesDesc.SampleDesc.Count=1;timesDesc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(!HR(m_device->CreateCommittedResource(&readbackHeap,D3D12_HEAP_FLAG_NONE,&timesDesc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_motionTimes)),"Create timestamp readback"))return false;
    if(!HR(m_queue->GetTimestampFrequency(&m_motionFrequency),"Timestamp frequency"))return false;
    p.pRootSignature=m_rawRootSig.Get();
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoRawMotion)),"Create raw GPU MV PSO"))return false;
    p.pRootSignature=m_rootSig.Get();
    p.PS={depthWrite->GetBufferPointer(),depthWrite->GetBufferSize()};
    p.NumRenderTargets=0;p.RTVFormats[0]=DXGI_FORMAT_UNKNOWN;p.RTVFormats[1]=DXGI_FORMAT_UNKNOWN;p.RTVFormats[2]=DXGI_FORMAT_UNKNOWN;p.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    p.DepthStencilState.DepthEnable=TRUE;p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;p.DepthStencilState.StencilEnable=FALSE;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoDepthWrite)),"Create real depth-buffer PSO"))return false;
    p.PS={aiHwDepthWrite->GetBufferPointer(),aiHwDepthWrite->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoAIHardwareDepthWrite)),"Create AI hardware depth write PSO"))return false;
    return true;
}

bool D3D12Renderer::InitializeDLSS(){
    auto* cmd=m_cmds[0].Get();
    m_allocators[0]->Reset();cmd->Reset(m_allocators[0].Get(),nullptr);bool ok=m_dlss.Initialize(m_device.Get(),cmd,m_sourceW,m_sourceH,m_outputW,m_outputH,m_quality);
    if(ok){m_renderW=m_dlss.RenderWidth();m_renderH=m_dlss.RenderHeight();}
    cmd->Close();ID3D12CommandList*l[]={cmd};m_queue->ExecuteCommandLists(1,l);WaitGPU();return ok;
}

bool D3D12Renderer::CreateUploadForTexture(const D3D12_RESOURCE_DESC&desc,ComPtr<ID3D12Resource>&upload,uint8_t*&mapped,D3D12_PLACED_SUBRESOURCE_FOOTPRINT&fp,uint32_t&rows,uint64_t&rowBytes,uint64_t&total,const char*name){
    m_device->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&rowBytes,&total);D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=total;b.Height=1;b.DepthOrArraySize=1;b.MipLevels=1;b.SampleDesc={1,0};b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto hp=HeapProps(D3D12_HEAP_TYPE_UPLOAD);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),name))return false;D3D12_RANGE r{0,0};return HR(upload->Map(0,&r,reinterpret_cast<void**>(&mapped)),"Map upload resource");
}

bool D3D12Renderer::CreateVideoResources(){
    auto hp=HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto src=Tex2D(DXGI_FORMAT_B8G8R8A8_UNORM,m_sourceW,m_sourceH,D3D12_RESOURCE_FLAG_NONE);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&src,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_decodedTexture)),"Create decoded texture"))return false;
    m_decodedTexture->SetName(L"Video_Decoded_BGRA_sRGB");
    for(uint32_t i=0;i<FrameCount;++i) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; uint32_t rows=0; uint64_t rowBytes=0,total=0;
        if(!CreateUploadForTexture(src,m_upload[i],m_uploadMapped[i],fp,rows,rowBytes,total,"Create video upload"))return false;
        if(i==0){m_uploadFootprint=fp;m_numRows=rows;m_rowSize=rowBytes;m_uploadBytes=total;}
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
    srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;m_device->CreateShaderResourceView(m_decodedTexture.Get(),&srv,SRVCPU(0));

    D3D12_CLEAR_VALUE cv{};cv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;auto col=Tex2D(cv.Format,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&col,D3D12_RESOURCE_STATE_RENDER_TARGET,&cv,IID_PPV_ARGS(&m_dlssColor)),"Create DLSS color"))return false;m_dlssColor->SetName(L"DLSS_Color_Input_Linear_FP16");m_device->CreateRenderTargetView(m_dlssColor.Get(),nullptr,RTV(FrameCount));
    srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;m_device->CreateShaderResourceView(m_dlssColor.Get(),&srv,SRVCPU(4));
    auto mot=Tex2D(DXGI_FORMAT_R16G16_FLOAT,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&mot,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_motion)),"Create motion guide"))return false;
    m_motion->SetName(L"DLSS_MotionVectors_CurrentToPrevious_RG16F");srv.Format=DXGI_FORMAT_R16G16_FLOAT;m_device->CreateShaderResourceView(m_motion.Get(),&srv,SRVCPU(2));m_device->CreateRenderTargetView(m_motion.Get(),nullptr,RTV(FrameCount+1));

    // One depth resource, two views: D32_FLOAT DSV for real depth writes / ReShade
    // discovery and R32_FLOAT SRV for debug/NGX sampling. Passing this exact resource
    // to NGX avoids the old "R32 proxy + unrelated mirrored D32" ambiguity.
    D3D12_CLEAR_VALUE dcv{};dcv.Format=DXGI_FORMAT_D32_FLOAT;dcv.DepthStencil.Depth=1.0f;dcv.DepthStencil.Stencil=0;
    auto dep=Tex2D(DXGI_FORMAT_R32_TYPELESS,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&dep,D3D12_RESOURCE_STATE_DEPTH_WRITE,&dcv,IID_PPV_ARGS(&m_depth)),"Create unified DLSS depth"))return false;
    m_depth->SetName(L"DLSS_Depth_R32_TYPELESS_D32_DSV_R32_SRV");
    srv.Format=DXGI_FORMAT_R32_FLOAT;m_device->CreateShaderResourceView(m_depth.Get(),&srv,SRVCPU(3));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT;dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;m_device->CreateDepthStencilView(m_depth.Get(),&dsv,DSV());
    // Step 04C synthetic hardware depth. It mirrors the real NGX depth resource shape
    // and state contract, but m_dlss.Evaluate still receives m_depth in this step.
    auto aiHwDep=Tex2D(DXGI_FORMAT_R32_TYPELESS,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&aiHwDep,D3D12_RESOURCE_STATE_DEPTH_WRITE,&dcv,IID_PPV_ARGS(&m_aiHardwareDepth)),"Create synthetic AI hardware depth"))return false;
    m_aiHardwareDepth->SetName(L"AI_Synthetic_HW_Depth_DebugOnly_R32_TYPELESS_D32_DSV_R32_SRV");
    srv.Format=DXGI_FORMAT_R32_FLOAT;m_device->CreateShaderResourceView(m_aiHardwareDepth.Get(),&srv,SRVCPU(8));
    m_device->CreateDepthStencilView(m_aiHardwareDepth.Get(),&dsv,DSV(1));
    LOG("[AI HWDepth] resource created: "<<m_renderW<<"x"<<m_renderH<<" R32_TYPELESS/D32_FLOAT/R32_FLOAT; mapping=1-relativeNearness; Step 04D A/B candidate when Depth Source=AI Synthetic.");
    // Step 04D A/B baseline: independent constant conventional hardware depth.
    // It deliberately does not modify the legacy guide/depth generator, so switching
    // depth source changes only the resource supplied to NGX.
    auto flatDep=Tex2D(DXGI_FORMAT_R32_TYPELESS,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE flatCv=dcv;flatCv.DepthStencil.Depth=0.75f;
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&flatDep,D3D12_RESOURCE_STATE_DEPTH_WRITE,&flatCv,IID_PPV_ARGS(&m_flatDepth)),"Create flat A/B depth"))return false;
    m_flatDepth->SetName(L"DLSS_Flat_Depth_AB_R32_TYPELESS_D32_DSV_R32_SRV");
    srv.Format=DXGI_FORMAT_R32_FLOAT;m_device->CreateShaderResourceView(m_flatDepth.Get(),&srv,SRVCPU(9));
    m_device->CreateDepthStencilView(m_flatDepth.Get(),&dsv,DSV(2));
    LOG("[NGX Depth] flat A/B resource created: "<<m_renderW<<"x"<<m_renderH<<" Z=0.75 conventional (0 near, 1 far).");

    auto bias=Tex2D(DXGI_FORMAT_R8_UNORM,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bias,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_biasCurrent)),"Create BiasCurrentColor mask"))return false;
    m_biasCurrent->SetName(L"DLSS_BiasCurrentColor_Disocclusion_R8");srv.Format=DXGI_FORMAT_R8_UNORM;m_device->CreateShaderResourceView(m_biasCurrent.Get(),&srv,SRVCPU(5));m_device->CreateRenderTargetView(m_biasCurrent.Get(),nullptr,RTV(FrameCount+2));
    auto grid=Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT,m_gridW,m_gridH,D3D12_RESOURCE_FLAG_NONE);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&grid,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_guideGrid)),"Create compact temporal guide grid"))return false;
    m_guideGrid->SetName(L"DLSS_CompactGuideGrid_RGBA32F");
    for(uint32_t i=0;i<FrameCount;++i) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; uint32_t rows=0; uint64_t rowBytes=0,total=0;
        if(!CreateUploadForTexture(grid,m_guideUpload[i],m_guideMapped[i],fp,rows,rowBytes,total,"Create compact guide upload"))return false;
        if(i==0){m_guideFootprint=fp;m_guideRows=rows;m_guideRowSize=rowBytes;m_guideUploadBytes=total;}
    }
    srv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;m_device->CreateShaderResourceView(m_guideGrid.Get(),&srv,SRVCPU(6));

    // Step 04A-2: a separate normalized AI-depth texture exists only for debug
    // presentation. It is never passed to NGX and therefore cannot change NR behavior.
    auto ai=Tex2D(DXGI_FORMAT_R32_FLOAT,AIDepthW,AIDepthH,D3D12_RESOURCE_FLAG_NONE);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&ai,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_aiDepth)),"Create AI depth debug texture"))return false;
    m_aiDepth->SetName(L"AI_Depth_DebugOnly_R32_FLOAT_518x518");
    for(uint32_t i=0;i<FrameCount;++i){
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};uint32_t rows=0;uint64_t rowBytes=0,total=0;
        if(!CreateUploadForTexture(ai,m_aiDepthUpload[i],m_aiDepthMapped[i],fp,rows,rowBytes,total,"Create AI depth upload"))return false;
        if(i==0){m_aiDepthFootprint=fp;m_aiDepthRows=rows;m_aiDepthRowSize=rowBytes;m_aiDepthUploadBytes=total;}
        if(m_aiDepthMapped[i])memset(m_aiDepthMapped[i],0,static_cast<size_t>(total));
    }
    srv.Format=DXGI_FORMAT_R32_FLOAT;m_device->CreateShaderResourceView(m_aiDepth.Get(),&srv,SRVCPU(7));

    auto out=Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,m_outputW,m_outputH,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&m_dlssOutput)),"Create DLSS output"))return false;
    m_dlssOutput->SetName(L"DLSS_Output_Linear_FP16_UAV");
    srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;m_device->CreateShaderResourceView(m_dlssOutput.Get(),&srv,SRVCPU(1));
    LOG("DLSS resource contract ready: Color=R16G16B16A16_FLOAT " << m_renderW << "x" << m_renderH
        << ", MV=R16G16_FLOAT " << m_renderW << "x" << m_renderH
        << ", Depth=R32_TYPELESS resource / D32_FLOAT DSV / R32_FLOAT SRV " << m_renderW << "x" << m_renderH
        << ", BiasCurrentColor=R8_UNORM " << m_renderW << "x" << m_renderH
        << ", Output=R16G16B16A16_FLOAT UAV " << m_outputW << "x" << m_outputH
        << ", CompactGrid=R32G32B32A32_FLOAT " << m_gridW << "x" << m_gridH << " -> GPU MV/bias expansion + direct SV_Depth write");
    return true;
}

void D3D12Renderer::CopyMappedRows(uint8_t*mapped,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT&fp,const void*src,size_t tight,uint32_t rows){const uint8_t*s=static_cast<const uint8_t*>(src);for(uint32_t y=0;y<rows;++y)memcpy(mapped+fp.Offset+size_t(fp.Footprint.RowPitch)*y,s+tight*y,tight);}

float D3D12Renderer::Halton(uint32_t index,uint32_t base){float f=1.0f,r=0.0f;while(index){f/=float(base);r+=f*float(index%base);index/=base;}return r;}

bool D3D12Renderer::RenderFrame(const uint8_t*bgra,size_t bytes,const float*guideGridRGBA32F,size_t guideBytes,uint32_t gridW,uint32_t gridH,bool temporalReset,float frameTimeMs,const float*aiDepthPreview01,size_t aiDepthBytes,uint32_t aiDepthW,uint32_t aiDepthH,const GpuOpticalFlowFrame* gpuFlow){
    const size_t videoRow=size_t(m_sourceW)*4u,guideRow=size_t(m_gridW)*sizeof(float)*4u;
    if(!gpuFlow && (!bgra||bytes<videoRow*m_sourceH||!guideGridRGBA32F||gridW!=m_gridW||gridH!=m_gridH||guideBytes<guideRow*m_gridH))return false;
    if(gpuFlow){if(!gpuFlow->color||!gpuFlow->previousColor||!gpuFlow->motion||!gpuFlow->cost||!gpuFlow->readyFence)return false;m_depthSource=DepthSource::Flat;}
    m_motionDebugDeadZone=gpuFlow?0.0f:0.20f;
    const bool haveAIDepth=aiDepthPreview01&&aiDepthW==AIDepthW&&aiDepthH==AIDepthH&&aiDepthBytes>=size_t(AIDepthW)*AIDepthH*sizeof(float);
    const DepthSource effectiveDepth = m_depthSource==DepthSource::AISynthetic && !haveAIDepth ? DepthSource::Flat : m_depthSource;
    const bool depthSourceChanged=effectiveDepth!=m_effectiveDepthSource;
    temporalReset=temporalReset||depthSourceChanged;
    m_effectiveDepthSource=effectiveDepth;
    ID3D12Resource* selectedDepth=effectiveDepth==DepthSource::AISynthetic?m_aiHardwareDepth.Get():(effectiveDepth==DepthSource::Flat?m_flatDepth.Get():m_depth.Get());
    if(depthSourceChanged || (m_framesPresented%120u)==0u){
        auto name=[](DepthSource x){return x==DepthSource::AISynthetic?"AI":(x==DepthSource::Flat?"FLAT":"LEGACY");};
        float zMin=1.0f,zMax=0.0f; double zSum=0.0;
        const size_t n=effectiveDepth==DepthSource::AISynthetic?size_t(AIDepthW)*AIDepthH:(effectiveDepth==DepthSource::Legacy?size_t(gridW)*gridH:1u);
        for(size_t i=0;i<n;++i){const float z=effectiveDepth==DepthSource::AISynthetic?1.0f-aiDepthPreview01[i]:(effectiveDepth==DepthSource::Legacy?guideGridRGBA32F[i*4u+2u]:.75f);zMin=std::min(zMin,z);zMax=std::max(zMax,z);zSum+=z;}
        LOG("[Depth Signal] min="<<zMin<<" max="<<zMax<<" mean="<<zSum/double(n)<<" samples="<<n<<" aiProducerRequested="<<(m_depthSource==DepthSource::AISynthetic));
        LOG("[Depth Contract] requested="<<name(m_depthSource)<<" effective="<<name(effectiveDepth)<<" DLSS="<<name(effectiveDepth)<<" Mask="<<name(effectiveDepth)<<" FG="<<name(effectiveDepth)<<" resource="<<selectedDepth<<" format=D32_FLOAT aiReady="<<haveAIDepth<<" reset="<<temporalReset);
    }
    const uint32_t slot=m_frameSlot%FrameCount;
    if(!WaitForFrameSlot(slot)) return false;
    // Only read completed diagnostic timestamps; this never introduces a wait
    // beyond the existing frame-slot fence, and never reads image/model data.
    if(gpuFlow&&m_motionTimesReady[slot]&&(m_framesPresented%120u)==0u){
        uint64_t* stamps=nullptr;D3D12_RANGE read{slot*32,(slot+1)*32};
        if(SUCCEEDED(m_motionTimes->Map(0,&read,reinterpret_cast<void**>(&stamps)))){
            const auto* t=stamps+slot*4;
            LOG("[GPU Motion Time] cameraMs="<<double(t[1]-t[0])*1000/m_motionFrequency<<" filterExpandMs="<<double(t[3]-t[2])*1000/m_motionFrequency<<" diagnosticBytes=32 imageReadbackBytes=0");
            D3D12_RANGE none{0,0};m_motionTimes->Unmap(0,&none);
        }
    }
    ++m_frameGenerationFrameIndex;
    m_frameGenerationActiveThisFrame=m_frameGenerationEnabled&&m_fgHudless[slot]&&
        DLSSFrameGenerationRuntime::Instance().BeginFrame(m_debugView==DebugView::Final,
            m_frameGenerationFrameIndex,temporalReset);
    if(!gpuFlow){CopyMappedRows(m_uploadMapped[slot],m_uploadFootprint,bgra,videoRow,m_sourceH);
    CopyMappedRows(m_guideMapped[slot],m_guideFootprint,guideGridRGBA32F,guideRow,m_gridH);}
    else {
        if(!HR(m_queue->Wait(gpuFlow->readyFence,gpuFlow->readyValue),"GPU wait for NVOF"))return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        const uint32_t gpuSrv=11+slot*4;
        srv.Format=DXGI_FORMAT_R16G16_SINT;m_device->CreateShaderResourceView(gpuFlow->motion,&srv,SRVCPU(gpuSrv));
        srv.Format=DXGI_FORMAT_R8_UINT;m_device->CreateShaderResourceView(gpuFlow->cost,&srv,SRVCPU(gpuSrv+1));
        srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;m_device->CreateShaderResourceView(gpuFlow->color,&srv,SRVCPU(gpuSrv+2));
        m_device->CreateShaderResourceView(gpuFlow->previousColor,&srv,SRVCPU(gpuSrv+3));
    }
    if(haveAIDepth)CopyMappedRows(m_aiDepthMapped[slot],m_aiDepthFootprint,aiDepthPreview01,size_t(AIDepthW)*sizeof(float),AIDepthH);
    else if(m_aiDepthClearPending)memset(m_aiDepthMapped[slot],0,static_cast<size_t>(m_aiDepthUploadBytes));
    if(!HR(m_allocators[slot]->Reset(),"Reset frame allocator")) return false;
    auto* cmd=m_cmds[slot].Get();
    if(!HR(cmd->Reset(m_allocators[slot].Get(),nullptr),"Reset frame command list")) return false;
    ID3D12DescriptorHeap*heaps[]={m_srvHeap.Get()};cmd->SetDescriptorHeaps(1,heaps);

    D3D12_TEXTURE_COPY_LOCATION d{},s{};
    if(!gpuFlow){
    if(!m_sourceInCopyDest)Barrier(cmd,m_decodedTexture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    d.pResource=m_decodedTexture.Get();d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;s.pResource=m_upload[slot].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;s.PlacedFootprint=m_uploadFootprint;cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);Barrier(cmd,m_decodedTexture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_sourceInCopyDest=false;

    if(!m_gridInCopyDest)Barrier(cmd,m_guideGrid.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    d.pResource=m_guideGrid.Get();s.pResource=m_guideUpload[slot].Get();s.PlacedFootprint=m_guideFootprint;cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);
    Barrier(cmd,m_guideGrid.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_gridInCopyDest=false;

    }else{
        const auto computeRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        Barrier(cmd,gpuFlow->color,D3D12_RESOURCE_STATE_COMMON,computeRead);
        if(gpuFlow->previousColor!=gpuFlow->color)Barrier(cmd,gpuFlow->previousColor,D3D12_RESOURCE_STATE_COMMON,computeRead);
        Barrier(cmd,gpuFlow->motion,D3D12_RESOURCE_STATE_COMMON,computeRead);
        Barrier(cmd,gpuFlow->cost,D3D12_RESOURCE_STATE_COMMON,computeRead);
        cmd->EndQuery(m_motionQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*4);
        m_cameraMotion.Record(cmd,SRVGPU(11+slot*4),gpuFlow->valid);
        cmd->EndQuery(m_motionQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*4+1);
        Barrier(cmd,gpuFlow->color,computeRead,D3D12_RESOURCE_STATE_COMMON);
        if(gpuFlow->previousColor!=gpuFlow->color)Barrier(cmd,gpuFlow->previousColor,computeRead,D3D12_RESOURCE_STATE_COMMON);
        Barrier(cmd,gpuFlow->motion,computeRead,D3D12_RESOURCE_STATE_COMMON);
        Barrier(cmd,gpuFlow->cost,computeRead,D3D12_RESOURCE_STATE_COMMON);
        Barrier(cmd,gpuFlow->color,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if(gpuFlow->previousColor!=gpuFlow->color)Barrier(cmd,gpuFlow->previousColor,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(cmd,gpuFlow->motion,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(cmd,gpuFlow->cost,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    // Upload only a newly completed AI result (or a requested clear). The texture is
    // separate from m_depth, which remains the exact resource supplied to NGX.
    if(haveAIDepth||m_aiDepthClearPending||m_aiDepthInCopyDest){
        if(!m_aiDepthInCopyDest)Barrier(cmd,m_aiDepth.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;s.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;d.pResource=m_aiDepth.Get();s.pResource=m_aiDepthUpload[slot].Get();s.PlacedFootprint=m_aiDepthFootprint;cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);
        Barrier(cmd,m_aiDepth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_aiDepthInCopyDest=false;
        m_aiDepthValid=haveAIDepth;m_aiDepthClearPending=false;
    }

    // One temporal jitter sample drives BOTH the color reconstruction input and the
    // spatial lookup of all guide buffers.  The motion-vector VALUES themselves remain
    // unjittered (hence no MVJittered create flag), matching the standard DLSS contract.
    // Encoded video is already a raster sample; do not synthesize camera jitter by
    // shifting that finished image. We have no hidden sub-pixel raster samples to
    // reveal, and alternating offsets can visibly shake the player/NR output.
    // NGX receives zero jitter because the color/depth/guide inputs are unjittered.
    const float jitterX=0.0f,jitterY=0.0f;
    const float jitterUVX=0.0f,jitterUVY=0.0f;

    // GPU-expand the compact CPU optical-flow/mask analysis to exact DLSS input
    // resolution. Depth is deliberately NOT mirrored through a color RT anymore:
    // it is written directly into the same typeless depth resource that NGX receives.
    if(!m_guidesInRT){Barrier(cmd,m_motion.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);Barrier(cmd,m_biasCurrent.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);}m_guidesInRT=true;
    D3D12_VIEWPORT gvp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT gsc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&gvp);cmd->RSSetScissorRects(1,&gsc);
    D3D12_CPU_DESCRIPTOR_HANDLE grt[2]={RTV(FrameCount+1),RTV(FrameCount+2)};cmd->OMSetRenderTargets(2,grt,FALSE,nullptr);
    if(gpuFlow)cmd->EndQuery(m_motionQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*4+2);
    cmd->SetGraphicsRootSignature(gpuFlow?m_rawRootSig.Get():m_rootSig.Get());cmd->SetPipelineState(gpuFlow?m_psoRawMotion.Get():m_psoExpandGuides.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(gpuFlow?11+slot*4:6));float guideParams[4]={jitterUVX,jitterUVY,0,0};if(gpuFlow){guideParams[0]=float(m_renderW)/gpuFlow->sourceW;guideParams[1]=float(m_renderH)/gpuFlow->sourceH;guideParams[2]=gpuFlow->valid?1.0f:0.0f;guideParams[3]=m_biasMaskMode==BiasMaskMode::Off?-1.0f:(m_biasMaskMode==BiasMaskMode::ForceCurrent?1.0f:0.0f);cmd->SetGraphicsRootShaderResourceView(2,m_cameraMotion.Resource()->GetGPUVirtualAddress());}cmd->SetGraphicsRoot32BitConstants(1,4,guideParams,0);cmd->DrawInstanced(3,1,0,0);
    if(gpuFlow){cmd->EndQuery(m_motionQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*4+3);cmd->ResolveQueryData(m_motionQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,slot*4,4,m_motionTimes.Get(),slot*32);m_motionTimesReady[slot]=true;}
    Barrier(cmd,m_motion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);Barrier(cmd,m_biasCurrent.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);m_guidesInRT=false;

    // Populate the exact depth resource passed to NGX. The resource is R32_TYPELESS,
    // viewed as D32_FLOAT while writing and R32_FLOAT while sampling/debugging.
    if(effectiveDepth==DepthSource::Legacy){
    if(!m_depthInWrite)Barrier(cmd,m_depth.Get(),DepthGuideReadState,D3D12_RESOURCE_STATE_DEPTH_WRITE);m_depthInWrite=true;
    D3D12_VIEWPORT dvp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT dsc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&dvp);cmd->RSSetScissorRects(1,&dsc);
    auto dsvh=DSV();cmd->OMSetRenderTargets(0,nullptr,FALSE,&dsvh);cmd->ClearDepthStencilView(dsvh,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoDepthWrite.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(6));cmd->SetGraphicsRoot32BitConstants(1,4,guideParams,0);cmd->DrawInstanced(3,1,0,0);
    Barrier(cmd,m_depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,DepthGuideReadState);m_depthInWrite=false;
    }
    // Step 04C: expand stabilized relative AI depth to a true full-resolution D32
    // hardware-depth resource. AI relative nearness is white=near; conventional D3D
    // hardware depth is the inverse polarity: 0=near, 1=far. Shared by NGX and FG when selected.
    if(m_aiDepthValid||m_aiHardwareDepthClearPending||m_aiHardwareDepthInWrite){
        if(!m_aiHardwareDepthInWrite)Barrier(cmd,m_aiHardwareDepth.Get(),DepthGuideReadState,D3D12_RESOURCE_STATE_DEPTH_WRITE);
        auto aiDsvh=DSV(1);cmd->OMSetRenderTargets(0,nullptr,FALSE,&aiDsvh);cmd->ClearDepthStencilView(aiDsvh,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);
        if(m_aiDepthValid){
            cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoAIHardwareDepthWrite.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(7));cmd->DrawInstanced(3,1,0,0);
        }
        Barrier(cmd,m_aiHardwareDepth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,DepthGuideReadState);m_aiHardwareDepthInWrite=false;
        m_aiHardwareDepthValid=m_aiDepthValid;m_aiHardwareDepthClearPending=false;
    }
    // Step 04D: initialize the independent flat depth baseline once.
    if(m_flatDepthInWrite){
        auto flatDsv=DSV(2);cmd->OMSetRenderTargets(0,nullptr,FALSE,&flatDsv);cmd->ClearDepthStencilView(flatDsv,D3D12_CLEAR_FLAG_DEPTH,0.75f,0,0,nullptr);
        Barrier(cmd,m_flatDepth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,DepthGuideReadState);m_flatDepthInWrite=false;
    }

    if(!m_colorInRT)Barrier(cmd,m_dlssColor.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);m_colorInRT=true;
    D3D12_VIEWPORT vp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT sc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&vp);cmd->RSSetScissorRects(1,&sc);
    auto crt=RTV(FrameCount);cmd->OMSetRenderTargets(1,&crt,FALSE,nullptr);const float black[4]={0,0,0,1};cmd->ClearRenderTargetView(crt,black,0,nullptr);cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoConvert.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(gpuFlow?13+slot*4:0));
    float params[4]={jitterUVX,jitterUVY,0,0};cmd->SetGraphicsRoot32BitConstants(1,4,params,0);cmd->DrawInstanced(3,1,0,0);Barrier(cmd,m_dlssColor.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);m_colorInRT=false;

    ID3D12Resource* dlssColorForEval=m_dlssColor.Get();

    if(gpuFlow){
        Barrier(cmd,gpuFlow->color,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        if(gpuFlow->previousColor!=gpuFlow->color)Barrier(cmd,gpuFlow->previousColor,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        Barrier(cmd,gpuFlow->motion,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        Barrier(cmd,gpuFlow->cost,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    }

    ++m_framesPresented;

    // Create/recreate the NGX feature on an open command list, submit that list,
    // and only then evaluate on a fresh list. This mirrors the robust game-style
    // NGX lifetime instead of relying on CreateFeature and EvaluateFeature being
    // accepted back-to-back before the creation commands have reached the GPU.
    bool needFeatureFlush = false;
    if (DLSSEnabled() && !m_dlss.FeatureCreated() && m_framesPresented >= 2) {
        // Intentionally allow one complete Present before the first NGX CreateFeature.
        // ReShade add-ons finish their swapchain/runtime initialization on that first frame;
        // creating on frame 2 makes the raw CreateFeature much harder for RenoDX to miss.
        needFeatureFlush = m_dlss.EnsureFeature(cmd);
        temporalReset = true;
        m_recreateRequested = false;
    } else if (DLSSEnabled() && ((!m_delayedRecreateDone && m_framesPresented >= 60) || m_recreateRequested)) {
        needFeatureFlush = m_dlss.RecreateFeature(cmd);
        temporalReset = true;
        m_delayedRecreateDone = true;
        m_recreateRequested = false;
    }
    if (needFeatureFlush) {
        if (!HR(cmd->Close(), "Close command list after NGX CreateFeature")) return false;
        ID3D12CommandList* initLists[] = { cmd };
        m_queue->ExecuteCommandLists(1, initLists);
        WaitGPU();
        if (!HR(m_allocators[slot]->Reset(), "Reset allocator after NGX CreateFeature")) return false;
        if (!HR(cmd->Reset(m_allocators[slot].Get(), nullptr), "Reset command list after NGX CreateFeature")) return false;
        ID3D12DescriptorHeap* postCreateHeaps[] = { m_srvHeap.Get() };
        cmd->SetDescriptorHeaps(1, postCreateHeaps);
        LOG("NGX feature creation flushed before EvaluateFeature; temporal history reset.");
    }

    bool used=false;if(DLSSEnabled() && m_dlss.FeatureCreated()){
        if(!m_outputInUAV)Barrier(cmd,m_dlssOutput.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);m_outputInUAV=true;
        used=m_dlss.Evaluate(cmd,dlssColorForEval,m_dlssOutput.Get(),selectedDepth,m_motion.Get(),m_biasCurrent.Get(),temporalReset,frameTimeMs,jitterX,jitterY);
        if(used){Barrier(cmd,m_dlssOutput.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_outputInUAV=false;}
    }

    m_lastDLSSUsed=used;
    uint32_t bi=m_swapchain->GetCurrentBackBufferIndex();Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);D3D12_VIEWPORT ovp{0,0,float(m_outputW),float(m_outputH),0,1};D3D12_RECT osc{0,0,LONG(m_outputW),LONG(m_outputH)};cmd->RSSetViewports(1,&ovp);cmd->RSSetScissorRects(1,&osc);auto brt=RTV(bi);cmd->OMSetRenderTargets(1,&brt,FALSE,nullptr);cmd->ClearRenderTargetView(brt,black,0,nullptr);cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const bool applyColor=(m_debugView==DebugView::Final);
    const ColorSettings cs=applyColor?m_colorSettings:ColorSettings{};
    float presentParams[12]={0,0,0,0,cs.brightness,cs.contrast,cs.saturation,cs.gamma,cs.temperature,cs.tint,m_motionDebugDeadZone,0};
    cmd->SetGraphicsRoot32BitConstants(1,12,presentParams,0);
    // DLSS inputs stay shader-readable for NGX. Only the texture selected for the
    // debug/fallback presentation pass is temporarily made pixel-shader readable.
    if(m_splitScreen && m_debugView==DebugView::Final){
        DrawSplitComparison(cmd,used);
    }else{
    ID3D12Resource* debugPixelResource=nullptr;
    D3D12_RESOURCE_STATES debugBefore=GuideReadState;
    switch(m_debugView){
        case DebugView::MotionVectors:debugPixelResource=m_motion.Get();cmd->SetPipelineState(m_psoMotionDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(2));break;
        case DebugView::Depth:debugPixelResource=m_effectiveDepthSource==DepthSource::AISynthetic?m_aiHardwareDepth.Get():(m_effectiveDepthSource==DepthSource::Flat?m_flatDepth.Get():m_depth.Get());debugBefore=DepthGuideReadState;cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(m_effectiveDepthSource==DepthSource::AISynthetic?8:(m_effectiveDepthSource==DepthSource::Flat?9:3)));break;
        case DebugView::BiasMask:debugPixelResource=m_biasCurrent.Get();cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(5));break;
        case DebugView::AIDepth:cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(7));break;
        case DebugView::AIHardwareDepth:debugPixelResource=m_aiHardwareDepth.Get();debugBefore=DepthGuideReadState;cmd->SetPipelineState(m_psoAIHardwareDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(8));break;
        case DebugView::Input:debugPixelResource=m_dlssColor.Get();cmd->SetPipelineState(m_psoPresent.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));break;
        default:cmd->SetPipelineState(m_psoPresent.Get());if(used)cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(1));else{debugPixelResource=m_dlssColor.Get();cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));}break;
    }
    if(debugPixelResource)Barrier(cmd,debugPixelResource,debugBefore,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->DrawInstanced(3,1,0,0);
    if(debugPixelResource)Barrier(cmd,debugPixelResource,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,debugBefore);
    }
    CaptureFrameGenerationHudless(cmd,slot,bi);
    DrawSubtitleOverlay(cmd,slot);
    Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    if(!HR(cmd->Close(),"Close frame command list")) return false;
    ID3D12CommandList*ls[]={cmd};
    auto& fgRuntime=DLSSFrameGenerationRuntime::Instance();
    if(m_frameGenerationActiveThisFrame)fgRuntime.MarkRenderSubmitStart();
    m_queue->ExecuteCommandLists(1,ls);
    if(m_frameGenerationActiveThisFrame){
        fgRuntime.MarkRenderSubmitEnd();
        if(!fgRuntime.PreparePresent(selectedDepth,static_cast<uint32_t>(DepthGuideReadState),
            m_motion.Get(),static_cast<uint32_t>(GuideReadState),m_fgHudless[slot].Get(),
            static_cast<uint32_t>(m_fgHudlessState[slot]),m_fgUiAlpha.Get(),
            static_cast<uint32_t>(m_fgUiAlphaState),temporalReset))m_frameGenerationActiveThisFrame=false;
    }
    const bool fgPresentedThisFrame=m_frameGenerationActiveThisFrame;
    if(fgPresentedThisFrame)fgRuntime.MarkPresentStart();
    HRESULT phr=m_swapchain->Present(m_vsyncEnabled?1u:0u,(!m_vsyncEnabled&&m_allowTearing)?DXGI_PRESENT_ALLOW_TEARING:0u);
    if(fgPresentedThisFrame){fgRuntime.MarkPresentEnd();fgRuntime.AfterPresent();}
    m_frameGenerationActiveThisFrame=false;
    if(FAILED(phr)){LOG("Present failed hr=0x"<<std::hex<<phr);return false;}
    const uint32_t displayedThisPresent=fgPresentedThisFrame?std::max(1u,fgRuntime.LastFramesActuallyPresented()):1u;
    m_frameGenerationDisplayedFramesTotal+=displayedThisPresent;
    SignalFrameSlot(slot);
    m_frameSlot=(slot+1u)%FrameCount;
    return true;
}

void D3D12Renderer::DrawSplitComparison(ID3D12GraphicsCommandList* cmd,bool dlssUsed){
    // Step 04F: keep the FULL output viewport for both draws and clip only with
    // scissors. This preserves exact spatial correspondence: left x is the same
    // source/output coordinate as right x; neither half is horizontally squeezed.
    const auto layout=ComputeSplitScreenLayout(m_outputW,m_splitFraction);
    const LONG splitX=LONG(layout.splitX);
    D3D12_RECT left{0,0,splitX,LONG(m_outputH)};
    D3D12_RECT right{splitX,0,LONG(m_outputW),LONG(m_outputH)};
    D3D12_RECT full{0,0,LONG(m_outputW),LONG(m_outputH)};
    Barrier(cmd,m_dlssColor.Get(),GuideReadState,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->SetPipelineState(m_psoPresent.Get());
    cmd->RSSetScissorRects(1,&left);
    cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4)); // LEFT = pre-NGX input
    cmd->DrawInstanced(3,1,0,0);
    cmd->RSSetScissorRects(1,&right);
    cmd->SetGraphicsRootDescriptorTable(0,dlssUsed?SRVGPU(1):SRVGPU(4)); // RIGHT = NGX/RenoDX output
    cmd->DrawInstanced(3,1,0,0);
    cmd->RSSetScissorRects(1,&full);
    Barrier(cmd,m_dlssColor.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,GuideReadState);
}
bool D3D12Renderer::CreateSubtitleResources(){
    const auto layout=ComputeSubtitleOverlayLayout(m_outputW,m_outputH);
    m_subtitleTexW=layout.textureW;m_subtitleTexH=layout.textureH;
    auto hp=HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto desc=Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM,m_subtitleTexW,m_subtitleTexH,D3D12_RESOURCE_FLAG_NONE);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_subtitleTexture)),"Create subtitle texture"))return false;
    m_subtitleTexture->SetName(L"Post_DLSS_Subtitle_Overlay_RGBA8");
    for(uint32_t i=0;i<FrameCount;++i){
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};uint32_t rows=0;uint64_t rowBytes=0,total=0;
        if(!CreateUploadForTexture(desc,m_subtitleUpload[i],m_subtitleUploadMapped[i],fp,rows,rowBytes,total,"Create subtitle upload"))return false;
        if(i==0){m_subtitleFootprint=fp;m_subtitleRows=rows;m_subtitleRowSize=rowBytes;m_subtitleUploadBytes=total;}
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    m_device->CreateShaderResourceView(m_subtitleTexture.Get(),&srv,SRVCPU(SubtitleSrvIndex));
    m_subtitlePixels.assign(size_t(m_subtitleTexW)*size_t(m_subtitleTexH)*4u,0u);
    m_subtitleDirty=false;m_subtitleInCopyDest=true;
    LOG("[Subtitles GPU] compositor ready texture="<<m_subtitleTexW<<"x"<<m_subtitleTexH<<" srv="<<SubtitleSrvIndex<<" stage=post-DLSS/post-split");
    return true;
}

bool D3D12Renderer::BuildSubtitleBitmap(const std::wstring& text){
    if(!m_subtitleTexW||!m_subtitleTexH)return false;
    const size_t pixelCount=size_t(m_subtitleTexW)*size_t(m_subtitleTexH);
    if(pixelCount>size_t(64)*1024u*1024u)return false;
    m_subtitlePixels.assign(pixelCount*4u,0u);
    if(text.empty())return true;

    const auto layout=ComputeSubtitleOverlayLayout(m_outputW,m_outputH);
    std::vector<uint8_t> outline(pixelCount,0u),fill(pixelCount,0u);
    auto renderMask=[&](std::vector<uint8_t>&mask,bool drawOutline)->bool{
        HDC dc=CreateCompatibleDC(nullptr);if(!dc)return false;
        BITMAPINFO bmi{};bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bmi.bmiHeader.biWidth=LONG(m_subtitleTexW);bmi.bmiHeader.biHeight=-LONG(m_subtitleTexH);bmi.bmiHeader.biPlanes=1;bmi.bmiHeader.biBitCount=32;bmi.bmiHeader.biCompression=BI_RGB;
        void*bits=nullptr;HBITMAP bmp=CreateDIBSection(dc,&bmi,DIB_RGB_COLORS,&bits,nullptr,0);if(!bmp||!bits){if(bmp)DeleteObject(bmp);DeleteDC(dc);return false;}
        HGDIOBJ oldBmp=SelectObject(dc,bmp);memset(bits,0,pixelCount*4u);
        HFONT font=CreateFontW(-layout.fontPixels,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        if(!font){SelectObject(dc,oldBmp);DeleteObject(bmp);DeleteDC(dc);return false;}
        HGDIOBJ oldFont=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(255,255,255));
        const UINT fmt=DT_CENTER|DT_WORDBREAK|DT_NOPREFIX;
        RECT calc{layout.textMarginX,0,LONG(m_subtitleTexW)-layout.textMarginX,LONG(m_subtitleTexH)};
        DrawTextW(dc,text.c_str(),-1,&calc,fmt|DT_CALCRECT);
        const int textH=std::max(1,int(calc.bottom-calc.top));
        RECT tr{layout.textMarginX,std::max(0,int(m_subtitleTexH)-layout.textBottomMargin-textH),LONG(m_subtitleTexW)-layout.textMarginX,int(m_subtitleTexH)-layout.textBottomMargin};
        if(drawOutline){
            const int o=layout.outlinePixels;
            for(int dy=-o;dy<=o;++dy)for(int dx=-o;dx<=o;++dx){if(dx==0&&dy==0)continue;if(dx*dx+dy*dy>o*o+1)continue;RECT q=tr;OffsetRect(&q,dx,dy);DrawTextW(dc,text.c_str(),-1,&q,fmt);}
        }else DrawTextW(dc,text.c_str(),-1,&tr,fmt);
        const uint8_t*src=static_cast<const uint8_t*>(bits);
        for(size_t i=0;i<pixelCount;++i){const uint8_t*b=src+i*4u;mask[i]=std::max(b[0],std::max(b[1],b[2]));}
        SelectObject(dc,oldFont);DeleteObject(font);SelectObject(dc,oldBmp);DeleteObject(bmp);DeleteDC(dc);return true;
    };
    if(!renderMask(outline,true)||!renderMask(fill,false))return false;
    for(size_t i=0;i<pixelCount;++i){
        const uint8_t fa=fill[i],oa=outline[i];const uint8_t a=std::max(fa,oa);if(!a)continue;
        const uint8_t c=a?uint8_t((uint32_t(246u)*uint32_t(fa)+uint32_t(a)/2u)/uint32_t(a)):0u;uint8_t*d=m_subtitlePixels.data()+i*4u;d[0]=c;d[1]=c;d[2]=c;d[3]=a;
    }
    return true;
}

void D3D12Renderer::SetSubtitleText(const std::wstring& text){
    if(text==m_subtitleText)return;
    m_subtitleText=text;
    if(!BuildSubtitleBitmap(text)){LOG("[Subtitles GPU] bitmap rasterization failed chars="<<text.size());m_subtitleText.clear();m_subtitlePixels.clear();m_subtitleDirty=false;return;}
    m_subtitleDirty=!text.empty();
    if(text.empty())LOG("[Subtitles GPU] cue cleared");
    else LOG("[Subtitles GPU] cue rasterized chars="<<text.size()<<" band="<<m_subtitleTexW<<"x"<<m_subtitleTexH);
}

void D3D12Renderer::DrawSubtitleOverlay(ID3D12GraphicsCommandList*cmd,uint32_t slot){
    if(m_frameGenerationActiveThisFrame||!cmd||m_debugView!=DebugView::Final||m_subtitleText.empty()||!m_subtitleTexture||slot>=FrameCount)return;
    if(m_subtitleDirty){
        const size_t tight=size_t(m_subtitleTexW)*4u;
        if(m_subtitlePixels.size()<tight*size_t(m_subtitleTexH))return;
        CopyMappedRows(m_subtitleUploadMapped[slot],m_subtitleFootprint,m_subtitlePixels.data(),tight,m_subtitleTexH);
        if(!m_subtitleInCopyDest)Barrier(cmd,m_subtitleTexture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=m_subtitleTexture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=m_subtitleUpload[slot].Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=m_subtitleFootprint;
        cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Barrier(cmd,m_subtitleTexture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_subtitleInCopyDest=false;m_subtitleDirty=false;
    }
    if(m_subtitleInCopyDest)return;
    const auto layout=ComputeSubtitleOverlayLayout(m_outputW,m_outputH);
    D3D12_VIEWPORT vp{float(layout.dstX),float(layout.dstY),float(layout.dstW),float(layout.dstH),0,1};
    D3D12_RECT sc{layout.dstX,layout.dstY,layout.dstX+layout.dstW,layout.dstY+layout.dstH};
    cmd->RSSetViewports(1,&vp);cmd->RSSetScissorRects(1,&sc);cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoSubtitle.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(SubtitleSrvIndex));cmd->DrawInstanced(3,1,0,0);
}
bool D3D12Renderer::CreateFrameGenerationResources(){
    auto& fg=DLSSFrameGenerationRuntime::Instance();
    if(!fg.FeatureReady())return true;
    fg.Configure(m_renderW,m_renderH,m_outputW,m_outputH,FrameCount);
    auto hp=HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto desc=Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM,m_outputW,m_outputH,D3D12_RESOURCE_FLAG_NONE);
    for(uint32_t i=0;i<FrameCount;++i){
        if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_fgHudless[i])),
            "Create DLSS-G HUD-less frame"))return false;
        m_fgHudlessState[i]=D3D12_RESOURCE_STATE_COPY_DEST;
        std::wstring name=L"DLSSG_HUDLess_RGBA8_"+std::to_wstring(i);m_fgHudless[i]->SetName(name.c_str());
    }

    // No UI is composited into the D3D12 backbuffer while FG is active in Step05B.
    // Tag an immutable all-zero R8 UI-alpha texture so Final == HUD-less exactly and
    // newer OTA plugins that request a UI tag still receive a valid full-size resource.
    auto uiDesc=Tex2D(DXGI_FORMAT_R8_UNORM,m_outputW,m_outputH,D3D12_RESOURCE_FLAG_NONE);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&uiDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_fgUiAlpha)),
        "Create DLSS-G zero UI alpha"))return false;
    m_fgUiAlpha->SetName(L"DLSSG_UIAlpha_Zero_R8");
    UINT64 uploadBytes=0;
    m_device->GetCopyableFootprints(&uiDesc,0,1,0,&m_fgUiAlphaFootprint,nullptr,nullptr,&uploadBytes);
    D3D12_RESOURCE_DESC uploadDesc{};uploadDesc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;uploadDesc.Alignment=0;
    uploadDesc.Width=uploadBytes;uploadDesc.Height=1;uploadDesc.DepthOrArraySize=1;uploadDesc.MipLevels=1;
    uploadDesc.Format=DXGI_FORMAT_UNKNOWN;uploadDesc.SampleDesc.Count=1;uploadDesc.SampleDesc.Quality=0;
    uploadDesc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;uploadDesc.Flags=D3D12_RESOURCE_FLAG_NONE;
    auto up=HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    if(!HR(m_device->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&m_fgUiAlphaUpload)),
        "Create DLSS-G zero UI alpha upload"))return false;
    void* mapped=nullptr;if(!HR(m_fgUiAlphaUpload->Map(0,nullptr,&mapped),"Map DLSS-G zero UI alpha"))return false;
    std::memset(mapped,0,static_cast<size_t>(uploadBytes));m_fgUiAlphaUpload->Unmap(0,nullptr);
    m_fgUiAlphaState=D3D12_RESOURCE_STATE_COPY_DEST;m_fgUiAlphaInitialized=false;
    LOG("[DLSS-G] HUD-less ring ready "<<m_outputW<<"x"<<m_outputH<<" slots="<<FrameCount
        <<" zeroUIAlpha=R8 depth=m_depth(D32 hardware-Z) motion=m_motion(RG16F current-to-previous)");
    return true;
}

void D3D12Renderer::SetFrameGeneration(bool enabled,uint32_t multiplier){
    multiplier=std::clamp(multiplier,2u,6u);m_frameGenerationEnabled=enabled;m_frameGenerationMultiplier=multiplier;
    DLSSFrameGenerationRuntime::Instance().SetRequested(enabled,multiplier);
    LOG("[DLSS-G] application request="<<(enabled?"ON":"OFF")<<" multiplier="<<multiplier
        <<" runtimeReady="<<(DLSSFrameGenerationRuntime::Instance().FeatureReady()?1:0)
        <<" maxMultiplier="<<DLSSFrameGenerationRuntime::Instance().MaxMultiplier());
}

bool D3D12Renderer::FrameGenerationAvailable()const{
    return DLSSFrameGenerationRuntime::Instance().FeatureReady();
}

uint32_t D3D12Renderer::FrameGenerationMaxMultiplier()const{
    return DLSSFrameGenerationRuntime::Instance().MaxMultiplier();
}

uint32_t D3D12Renderer::FrameGenerationFramesActuallyPresented()const{
    return DLSSFrameGenerationRuntime::Instance().LastFramesActuallyPresented();
}

void D3D12Renderer::CaptureFrameGenerationHudless(ID3D12GraphicsCommandList*cmd,uint32_t slot,uint32_t backbufferIndex){
    if(!m_frameGenerationActiveThisFrame||!cmd||slot>=FrameCount||backbufferIndex>=FrameCount||!m_fgHudless[slot]||!m_fgUiAlpha)return;
    if(!m_fgUiAlphaInitialized){
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=m_fgUiAlpha.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=0;
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=m_fgUiAlphaUpload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=m_fgUiAlphaFootprint;
        cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Barrier(cmd,m_fgUiAlpha.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_fgUiAlphaState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;m_fgUiAlphaInitialized=true;
    }
    Barrier(cmd,m_backbuffers[backbufferIndex].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    if(m_fgHudlessState[slot]!=D3D12_RESOURCE_STATE_COPY_DEST)
        Barrier(cmd,m_fgHudless[slot].Get(),m_fgHudlessState[slot],D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(m_fgHudless[slot].Get(),m_backbuffers[backbufferIndex].Get());
    Barrier(cmd,m_fgHudless[slot].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_fgHudlessState[slot]=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Barrier(cmd,m_backbuffers[backbufferIndex].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
}
bool D3D12Renderer::PresentCurrent(){
    m_frameGenerationActiveThisFrame=false;DLSSFrameGenerationRuntime::Instance().SuspendForStaticPresent();
    if(!m_swapchain||!m_queue||!m_rootSig)return false;
    const uint32_t slot=m_frameSlot%FrameCount;
    if(!WaitForFrameSlot(slot))return false;
    if(!HR(m_allocators[slot]->Reset(),"Reset static-present allocator"))return false;
    auto* cmd=m_cmds[slot].Get();
    if(!HR(cmd->Reset(m_allocators[slot].Get(),nullptr),"Reset static-present command list"))return false;
    ID3D12DescriptorHeap*heaps[]={m_srvHeap.Get()};cmd->SetDescriptorHeaps(1,heaps);

    const float black[4]={0,0,0,1};
    uint32_t bi=m_swapchain->GetCurrentBackBufferIndex();
    Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_VIEWPORT ovp{0,0,float(m_outputW),float(m_outputH),0,1};
    D3D12_RECT osc{0,0,LONG(m_outputW),LONG(m_outputH)};
    cmd->RSSetViewports(1,&ovp);cmd->RSSetScissorRects(1,&osc);
    auto brt=RTV(bi);cmd->OMSetRenderTargets(1,&brt,FALSE,nullptr);cmd->ClearRenderTargetView(brt,black,0,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const bool applyColor=(m_debugView==DebugView::Final);
    const ColorSettings cs=applyColor?m_colorSettings:ColorSettings{};
    float presentParams[12]={0,0,0,0,cs.brightness,cs.contrast,cs.saturation,cs.gamma,cs.temperature,cs.tint,m_motionDebugDeadZone,0};
    cmd->SetGraphicsRoot32BitConstants(1,12,presentParams,0);

    if(m_splitScreen && m_debugView==DebugView::Final){
        DrawSplitComparison(cmd,m_lastDLSSUsed&&DLSSEnabled());
    }else{
    ID3D12Resource* debugPixelResource=nullptr;
    D3D12_RESOURCE_STATES debugBefore=GuideReadState;
    switch(m_debugView){
        case DebugView::MotionVectors:debugPixelResource=m_motion.Get();cmd->SetPipelineState(m_psoMotionDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(2));break;
        case DebugView::Depth:debugPixelResource=m_effectiveDepthSource==DepthSource::AISynthetic?m_aiHardwareDepth.Get():(m_effectiveDepthSource==DepthSource::Flat?m_flatDepth.Get():m_depth.Get());debugBefore=DepthGuideReadState;cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(m_effectiveDepthSource==DepthSource::AISynthetic?8:(m_effectiveDepthSource==DepthSource::Flat?9:3)));break;
        case DebugView::BiasMask:debugPixelResource=m_biasCurrent.Get();cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(5));break;
        case DebugView::AIDepth:cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(7));break;
        case DebugView::AIHardwareDepth:debugPixelResource=m_aiHardwareDepth.Get();debugBefore=DepthGuideReadState;cmd->SetPipelineState(m_psoAIHardwareDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(8));break;
        case DebugView::Input:debugPixelResource=m_dlssColor.Get();cmd->SetPipelineState(m_psoPresent.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));break;
        default:
            cmd->SetPipelineState(m_psoPresent.Get());
            if(m_lastDLSSUsed&&DLSSEnabled())cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(1));
            else{debugPixelResource=m_dlssColor.Get();cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));}
            break;
    }
    if(debugPixelResource)Barrier(cmd,debugPixelResource,debugBefore,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->DrawInstanced(3,1,0,0);
    if(debugPixelResource)Barrier(cmd,debugPixelResource,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,debugBefore);
    }
    DrawSubtitleOverlay(cmd,slot);
    Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    if(!HR(cmd->Close(),"Close static-present command list"))return false;
    ID3D12CommandList*ls[]={cmd};m_queue->ExecuteCommandLists(1,ls);
    HRESULT phr=m_swapchain->Present(m_vsyncEnabled?1u:0u,(!m_vsyncEnabled&&m_allowTearing)?DXGI_PRESENT_ALLOW_TEARING:0u);
    if(FAILED(phr)){LOG("Static Present failed hr=0x"<<std::hex<<phr);return false;}
    SignalFrameSlot(slot);m_frameSlot=(slot+1u)%FrameCount;
    return true;
}

void D3D12Renderer::Barrier(ID3D12GraphicsCommandList*cmd,ID3D12Resource*res,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){if(a==b)return;auto x=Transition(res,a,b);cmd->ResourceBarrier(1,&x);}
bool D3D12Renderer::WaitForFrameSlot(uint32_t slot){
    if(slot>=FrameCount||!m_fence||!m_fenceEvent)return false;
    const uint64_t v=m_frameFence[slot];
    if(v && m_fence->GetCompletedValue()<v){
        if(FAILED(m_fence->SetEventOnCompletion(v,m_fenceEvent)))return false;
        WaitForSingleObject(m_fenceEvent,INFINITE);
    }
    return true;
}
void D3D12Renderer::SignalFrameSlot(uint32_t slot){
    if(slot>=FrameCount||!m_queue||!m_fence)return;
    const uint64_t v=++m_fenceValue;
    if(SUCCEEDED(m_queue->Signal(m_fence.Get(),v)))m_frameFence[slot]=v;
}
void D3D12Renderer::WaitGPU(){
    if(!m_queue||!m_fence||!m_fenceEvent)return;
    uint64_t v=++m_fenceValue;m_queue->Signal(m_fence.Get(),v);
    if(m_fence->GetCompletedValue()<v){m_fence->SetEventOnCompletion(v,m_fenceEvent);WaitForSingleObject(m_fenceEvent,INFINITE);}
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::RTV(uint32_t i)const{auto h=m_rtvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*m_rtvInc;return h;}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::DSV(uint32_t index)const{auto h=m_dsvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(index)*m_dsvInc;return h;}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::SRVCPU(uint32_t i)const{auto h=m_srvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*m_srvInc;return h;}
D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::SRVGPU(uint32_t i)const{auto h=m_srvHeap->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(i)*m_srvInc;return h;}
