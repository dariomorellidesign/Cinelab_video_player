#include "DLSSFrameGeneration.h"
#include <iostream>
#include <stdexcept>

namespace {
int shutdownCalls=0,upgradeCalls=0;
sl::Result FailDevice(void*) { return sl::Result::eErrorExceptionHandler; }
sl::Result Shutdown() { ++shutdownCalls;return sl::Result::eOk; }
sl::Result Upgrade(void**) { ++upgradeCalls;return sl::Result::eOk; }
void Require(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
}

struct DLSSFrameGenerationRuntimeTestAccess {
    static void Run() {
        DLSSFrameGenerationRuntime runtime;
        runtime.m_initialized=true;
        runtime.m_setD3DDevice=&FailDevice;
        runtime.m_shutdown=&Shutdown;
        runtime.m_upgradeInterface=&Upgrade;
        auto* device=reinterpret_cast<ID3D12Device*>(uintptr_t(1));
        auto* factory=reinterpret_cast<IDXGIFactory6*>(uintptr_t(2));
        Require(runtime.UpgradeDeviceForQueue(device)==device,"Failed device must return native device");
        Require(shutdownCalls==1,"Failed initialization must shut down its Streamline session");
        Require(!runtime.ProcessInitialized(),"Failed session must not remain initialized");
        Require(!runtime.FeatureReady(),"Failed device must never enable frame generation");
        Require(runtime.UpgradeFactoryForSwapchain(factory)==factory,"Failed setup must return native factory");
        Require(upgradeCalls==0,"DXGI must not be upgraded after failed device initialization");
        runtime.ShutdownProcess();
        Require(shutdownCalls==1,"Shutdown must be idempotent");

        runtime.m_initialized=true;
        runtime.m_deviceHooked=false;
        Require(runtime.UpgradeFactoryForSwapchain(factory)==factory,"Initialized process alone must not enable DXGI hooks");
        Require(upgradeCalls==0,"Factory requires a successfully upgraded device");
        runtime.m_deviceHooked=true;
        Require(runtime.UpgradeFactoryForSwapchain(factory)==factory,"Stub must preserve factory pointer");
        Require(upgradeCalls==1,"Successful device gate must permit factory upgrade");
        runtime.ShutdownProcess();
        Require(!runtime.m_deviceHooked,"Shutdown must reset the device gate");
    }
};

int main() {
    try {
        DLSSFrameGenerationRuntimeTestAccess::Run();
        std::cout<<"STREAMLINE_FAILURE_FALLBACK=PASS injectedResult=24 noFactoryHookAfterFailure=1\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
