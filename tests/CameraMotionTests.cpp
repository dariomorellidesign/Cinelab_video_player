#include "CameraMotionGpu.h"
#include "SparseSceneCut.h"
#include <dxgi1_6.h>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <cmath>
using Microsoft::WRL::ComPtr;
static void check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
static void hr(HRESULT r){check(SUCCEEDED(r),"D3D12 failure");}
static void barrier(ID3D12GraphicsCommandList* c,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};c->ResourceBarrier(1,&b);}
static ComPtr<ID3D12Resource> buffer(ID3D12Device* d,UINT64 bytes,D3D12_HEAP_TYPE type){
    D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;r.Width=bytes;r.Height=1;r.DepthOrArraySize=1;r.MipLevels=1;r.SampleDesc.Count=1;r.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> out;hr(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&r,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out)));return out;
}
#include "MotionPixelProbe.h"
int main(int argc,char**){try{
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&a))==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 desc{};a->GetDesc1(&desc);if(desc.VendorId==0x10de){adapter=a;break;}}
    check(bool(adapter),"NVIDIA GPU missing");ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
    CameraMotionGpu camera;check(camera.Initialize(device.Get()),"Camera shader initialization");
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};hr(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));UINT64 serial=0;
    ComPtr<ID3D12QueryHeap> queries;D3D12_QUERY_HEAP_DESC qh{};qh.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;qh.Count=4;hr(device->CreateQueryHeap(&qh,IID_PPV_ARGS(&queries)));auto times=buffer(device.Get(),32,D3D12_HEAP_TYPE_READBACK);
    const UINT W=argc>1?3840:1024,H=argc>1?2160:576,GW=W/4,GH=H/4;
    auto signal=[](float x,float y){return .45f+.10f*std::sin(x*.43f)+.09f*std::cos(y*.37f);};
    struct Case{const char* name;float tx,ty,s,r;int mode;bool expect;};
    const Case cases[]={{"slow pan",.125f,-.0625f,0,0,0,true},{"slow zoom",0,0,2,0,0,true},{"rotation",.125f,0,0,1,0,true},{"foreground minority",.125f,0,0,0,1,true},{"one quadrant only",1,0,0,0,2,false},{"uniform black",0,0,0,0,3,false},{"reset",1,0,0,0,4,false}};
    for(const auto& test:cases){
        ComPtr<ID3D12CommandAllocator> allocator;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));ComPtr<ID3D12GraphicsCommandList> cmd;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&cmd)));
        ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=4;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;hr(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));const UINT inc=device->GetDescriptorHandleIncrementSize(hd.Type);
        std::vector<uint8_t> current(W*H*4),previous(W*H*4),cost(GW*GH,20);std::vector<int16_t> flow(GW*GH*2);
        for(UINT y=0;y<H;++y)for(UINT x=0;x<W;++x){float px=(float(x)+.5f-W*.5f)/W,py=(float(y)+.5f-H*.5f)/W;float mx=test.tx+test.s*px-test.r*py,my=test.ty+test.s*py+test.r*px;
            float a=signal(float(x)+mx,float(y)+my),b=signal(float(x),float(y));if(test.mode==3)a=b=0;
            for(int k=0;k<3;++k){current[(y*W+x)*4+k]=uint8_t(a*255);previous[(y*W+x)*4+k]=uint8_t(b*255);}current[(y*W+x)*4+3]=previous[(y*W+x)*4+3]=255;
        }
        for(UINT y=0;y<GH;++y)for(UINT x=0;x<GW;++x){float px=((x+.5f)*4-W*.5f)/W,py=((y+.5f)*4-H*.5f)/W;float mx=test.tx+test.s*px-test.r*py,my=test.ty+test.s*py+test.r*px;
            if(test.mode==1&&x>GW/3&&x<GW/2){mx+=8;my+=3;}
            if(test.mode==2&&(x>=GW/2||y>=GH/2))cost[y*GW+x]=255;
            flow[(y*GW+x)*2]=int16_t(std::round(mx*32));flow[(y*GW+x)*2+1]=int16_t(std::round(my*32));
        }
        // A uniform patch with deliberately false local flow must inherit camera
        // motion, or remain zero when no camera model exists. This checks pixels
        // produced by the production resolver, not just the fitted coefficients.
        for(UINT y=H/4-12;y<H/4+12;++y)for(UINT x=W/4-12;x<W/4+12;++x)for(int k=0;k<3;++k)current[(y*W+x)*4+k]=previous[(y*W+x)*4+k]=0;
        for(UINT y=GH/4-3;y<GH/4+3;++y)for(UINT x=GW/4-3;x<GW/4+3;++x){flow[(y*GW+x)*2]=64;flow[(y*GW+x)*2+1]=-32;cost[y*GW+x]=200;}
        // A newly visible moving patch has no usable previous colour. It must
        // raise the second render target (BiasCurrentColor), even if the MV
        // resolver subsequently rejects that correspondence.
        if(test.mode==0){for(UINT y=H*3/4-12;y<H*3/4+12;++y)for(UINT x=W*3/4-12;x<W*3/4+12;++x)for(int k=0;k<3;++k){current[(y*W+x)*4+k]=220;previous[(y*W+x)*4+k]=20;}for(UINT y=GH*3/4-3;y<GH*3/4+3;++y)for(UINT x=GW*3/4-3;x<GW*3/4+3;++x){flow[(y*GW+x)*2]=64;flow[(y*GW+x)*2+1]=0;cost[y*GW+x]=20;}}
        ComPtr<ID3D12Resource> textures[4],uploads[4];const void* data[]={flow.data(),cost.data(),current.data(),previous.data()};const UINT stride[]={GW*4,GW,W*4,W*4};
        SparseSceneCut movingCut;check(!movingCut.Update(previous.data(),W,H,true),"Initial cut");check(!movingCut.Update(current.data(),W,H,false),"Camera motion misdetected as cut");
        for(int j=0;j<4;++j){D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=j<2?GW:W;d.Height=j<2?GH:H;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Format=j==0?DXGI_FORMAT_R16G16_SINT:(j==1?DXGI_FORMAT_R8_UINT:DXGI_FORMAT_B8G8R8A8_UNORM);hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&textures[j])));
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows;UINT64 rowBytes,total;device->GetCopyableFootprints(&d,0,1,0,&fp,&rows,&rowBytes,&total);uploads[j]=buffer(device.Get(),total,D3D12_HEAP_TYPE_UPLOAD);void* map;D3D12_RANGE none{0,0};hr(uploads[j]->Map(0,&none,&map));for(UINT y=0;y<rows;++y)memcpy(static_cast<uint8_t*>(map)+fp.Offset+y*fp.Footprint.RowPitch,static_cast<const uint8_t*>(data[j])+y*stride[j],stride[j]);uploads[j]->Unmap(0,nullptr);
            D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=textures[j].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.pResource=uploads[j].Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(cmd.Get(),textures[j].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=d.Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;auto handle=heap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=j*inc;device->CreateShaderResourceView(textures[j].Get(),&srv,handle);
        }
        ID3D12DescriptorHeap* heaps[]={heap.Get()};cmd->SetDescriptorHeaps(1,heaps);cmd->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);camera.Record(cmd.Get(),heap->GetGPUDescriptorHandleForHeapStart(),test.mode!=4);cmd->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);cmd->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,times.Get(),0);
        MotionPixelProbe pixels;for(auto& tex:textures)barrier(cmd.Get(),tex.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        pixels.Record(device.Get(),cmd.Get(),heap->GetGPUDescriptorHandleForHeapStart(),camera.Resource(),W,H,test.mode!=4,queries.Get());cmd->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2,2,times.Get(),16);
        auto result=buffer(device.Get(),32,D3D12_HEAP_TYPE_READBACK);barrier(cmd.Get(),camera.Resource(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyBufferRegion(result.Get(),0,camera.Resource(),0,32);barrier(cmd.Get(),camera.Resource(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);hr(cmd->Close());ID3D12CommandList* lists[]={cmd.Get()};queue->ExecuteCommandLists(1,lists);hr(queue->Signal(fence.Get(),++serial));HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);hr(fence->SetEventOnCompletion(serial,event));check(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"GPU timeout");CloseHandle(event);
        float px=(float(W/4)+.5f-W*.5f)/W,py=(float(H/4)+.5f-H*.5f)/W;
        pixels.Check(W/4,H/4,test.expect?test.tx+test.s*px-test.r*py:0,test.expect?test.ty+test.s*py+test.r*px:0);
        if(test.mode==0)pixels.CheckBias(W*3/4,H*3/4,.20f);
        float* r;D3D12_RANGE range{0,32};hr(result->Map(0,&range,reinterpret_cast<void**>(&r)));std::cout<<test.name<<" fit="<<r[0]<<","<<r[1]<<","<<r[2]<<","<<r[3]<<" trusted="<<r[4]<<" inliers="<<r[6];check((r[4]>.5f)==test.expect,"Wrong model trust");if(test.expect){check(std::abs(r[0]-test.tx)<.10f&&std::abs(r[1]-test.ty)<.10f,"Translation fit error");check(std::abs(r[2]-test.s)<.20f&&std::abs(r[3]-test.r)<.20f,"Zoom/rotation fit error");}result->Unmap(0,nullptr);
        UINT64* stamps,frequency;hr(queue->GetTimestampFrequency(&frequency));hr(times->Map(0,nullptr,reinterpret_cast<void**>(&stamps)));std::cout<<" GPU ms="<<double(stamps[1]-stamps[0])*1000/frequency<<" resolve GPU ms="<<double(stamps[3]-stamps[2])*1000/frequency<<"\n";times->Unmap(0,nullptr);
    }
    SparseSceneCut cuts;std::vector<uint8_t> black(W*H*4,0),white(W*H*4,230),mid(W*H*4,100);
    check(!cuts.Update(black.data(),W,H,true),"First frame cut");check(!cuts.Update(black.data(),W,H,false),"Static cut");check(cuts.Update(white.data(),W,H,false),"Black/white cut missing");check(!cuts.Update(white.data(),W,H,false),"Repeated cut");check(!cuts.Update(mid.data(),W,H,true),"Reset cut");
    ComPtr<ID3D12InfoQueue> messages;if(SUCCEEDED(device.As(&messages))){for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());hr(messages->GetMessage(i,m,&size));if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::cerr<<m->pDescription;throw std::runtime_error("D3D12 debug validation error");}}}
    std::cout<<"Camera motion and cut tests PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
