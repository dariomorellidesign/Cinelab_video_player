#define wmain ReferenceMotionTestMain
#include "NvofHardwareSmoke.cpp"
#undef wmain
#include <stdexcept>
static void HRTest(HRESULT r){if(FAILED(r))throw std::runtime_error("D3D12 test operation failed");}
static void WaitTest(ID3D12Fence* fence,uint64_t value){if(fence->GetCompletedValue()>=value)return;HANDLE e=CreateEvent(nullptr,FALSE,FALSE,nullptr);HRTest(fence->SetEventOnCompletion(value,e));const auto r=WaitForSingleObject(e,10000);CloseHandle(e);if(r!=WAIT_OBJECT_0)throw std::runtime_error("GPU test timeout");}
int wmain(int argc,wchar_t**){try{
const uint32_t W=argc>1?3840:1918,H=argc>1?2160:1080;
ComPtr<IDXGIFactory6> factory;HRTest(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));ComPtr<IDXGIAdapter1> adapter;
for(UINT i=0;;++i){ComPtr<IDXGIAdapter1>a;if(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&a))==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d{};a->GetDesc1(&d);if(d.VendorId==0x10DE){adapter=a;break;}}
if(!adapter)throw std::runtime_error("No NVIDIA adapter");ComPtr<ID3D12Device> device;HRTest(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
OpticalFlowEngine engine;if(!engine.Initialize(device.Get(),W,H,2))return 1;
std::vector<uint8_t> original,current;MakeTexture(W,H,original);GpuOpticalFlowFrame f;
if(!engine.GenerateGpu(original.data(),original.size(),true,nullptr,0,f)||f.valid)return 2;
WaitTest(f.readyFence,f.readyValue);
ComPtr<ID3D12CommandQueue> q;D3D12_COMMAND_QUEUE_DESC qd{};HRTest(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
ComPtr<ID3D12Fence> done;HRTest(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)));
uint64_t value=0;
for(int step=1;step<=3;++step){
ID3D12Resource* expectedPrevious=f.color;
TranslateWrap(original,W,H,8*step,0,current);
if(!engine.GenerateGpu(current.data(),current.size(),false,value?done.Get():f.readyFence,value?value:f.readyValue,f)||!f.valid)return 3;
if(f.previousColor!=expectedPrevious||f.previousColor==f.color||!f.cost)throw std::runtime_error("GPU frame pair aliases current frame or loses previous frame");
D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows;UINT64 rowBytes,total;auto desc=f.motion->GetDesc();device->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&rowBytes,&total);
D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=total;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ComPtr<ID3D12Resource> readback;HRTest(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
ComPtr<ID3D12CommandAllocator> alloc;ComPtr<ID3D12GraphicsCommandList> cmd;HRTest(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));HRTest(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&cmd)));
D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.pResource=f.motion;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;cmd->ResourceBarrier(1,&barrier);
D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=f.motion;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);cmd->ResourceBarrier(1,&barrier);HRTest(cmd->Close());HRTest(q->Wait(f.readyFence,f.readyValue));ID3D12CommandList* lists[]={cmd.Get()};q->ExecuteCommandLists(1,lists);HRTest(q->Signal(done.Get(),++value));WaitTest(done.Get(),value);
void* mapped;D3D12_RANGE range{0,size_t(total)};HRTest(readback->Map(0,&range,&mapped));std::vector<float> xs,ys;for(uint32_t y=32;y+32<f.gridH;++y){auto row=reinterpret_cast<const int16_t*>(static_cast<const uint8_t*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch);for(uint32_t x=40;x+40<f.gridW;++x){xs.push_back(row[x*2]/32.0f);ys.push_back(row[x*2+1]/32.0f);}}D3D12_RANGE none{0,0};readback->Unmap(0,&none);
const float mx=Median(xs),my=Median(ys);std::cout<<W<<"x"<<H<<" pair "<<step<<" raw median="<<mx<<","<<my<<"\n";if(std::abs(mx+8)>1||std::abs(my)>1)return 4;
}
if(engine.GetStats().lastDownloadMs!=0||engine.GetStats().lastStabilizeMs!=0)return 5;
if(!engine.GenerateGpu(original.data(),original.size(),true,done.Get(),value,f)||f.valid)return 6;WaitTest(f.readyFence,f.readyValue);
std::cout<<"GPU raw flow: direction, scale, consecutive pairs, reset, no engine readback PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what();return 10;}}
