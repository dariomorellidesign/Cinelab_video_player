#pragma once

// STEP 05B experimental Streamline/DLSS-G 2x bridge.
// STEP 05B v1.4 Streamline 2.12 API compatibility: use kBufferTypeMotionVectors.
// STEP 05B v1.5 isolated-runtime opt-in hardening: env OR executable-side marker.
// Header-only on purpose: D3D12Renderer.cpp is also compiled into helper/test targets.
// Streamline is loaded dynamically, so no sl.interposer.lib dependency is added.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iterator>
#include <string>
#include <cwchar>
#include "Log.h"

class DLSSFrameGenerationRuntime {
public:
    static DLSSFrameGenerationRuntime& Instance() {
        static DLSSFrameGenerationRuntime instance;
        return instance;
    }

    bool InitializeProcess() {
        if (m_initialized) return true;
        const bool envOptIn = EnvironmentOptIn();
        const bool markerOptIn = RuntimeMarkerOptIn();
        if (!RuntimeOptIn()) {
            LOG("[DLSS-G] runtime opt-in disabled env=" << (envOptIn ? 1 : 0) << " marker=" << (markerOptIn ? 1 : 0));
            return false;
        }
        LOG("[DLSS-G] runtime opt-in accepted env=" << (envOptIn ? 1 : 0) << " marker=" << (markerOptIn ? 1 : 0));

        if (!m_module) {
            m_module = GetModuleHandleW(L"sl.interposer.dll");
            if (m_module) {
                LOG("[DLSS-G] using already-loaded sl.interposer.dll module");
            } else {
                wchar_t exePath[MAX_PATH]{};
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                wchar_t* slash = std::wcsrchr(exePath, L'\\');
                if (slash) *(slash + 1) = L'\0';
                std::wstring interposerPath = slash ? std::wstring(exePath) + L"sl.interposer.dll" : L"sl.interposer.dll";
                m_module = LoadLibraryW(interposerPath.c_str());
                if (!m_module) {
                    LOG("[DLSS-G] sl.interposer.dll not found; experimental frame generation unavailable winerr=" << GetLastError());
                    return false;
                }
                LOG("[DLSS-G] interposer loaded from isolated executable directory");
            }
        }
        if (!ResolveCoreExports()) return false;

        const sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
        sl::Preferences pref{};
        pref.showConsole = false;
        pref.logLevel = VerboseOptIn() ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
        pref.logMessageCallback = &DLSSFrameGenerationRuntime::StreamlineLog;
        pref.featuresToLoad = features;
        pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
        pref.engine = sl::EngineType::eCustom;
        pref.engineVersion = "CineLabVideoPlayer-10.0";
        pref.projectId = "50f09991-2962-44db-bad7-4be06dbbd1d2";
        const auto baseFlags = static_cast<uint64_t>(pref.flags);
        pref.flags = static_cast<sl::PreferenceFlags>(
            baseFlags |
            static_cast<uint64_t>(sl::PreferenceFlags::eUseManualHooking) |
            static_cast<uint64_t>(sl::PreferenceFlags::eUseFrameBasedResourceTagging));

        const sl::Result result = m_init(pref, sl::kSDKVersion);
        if (result != sl::Result::eOk) {
            LOG("[DLSS-G] slInit failed result=" << static_cast<int>(result));
            return false;
        }
        m_initialized = true;
        m_deviceHooked = false;
        m_featureReady = false;
        m_configured = false;
        m_modeApplied = false;
        m_frameToken = nullptr;
        m_currentDeviceIdentity = nullptr;
        LOG("[DLSS-G] Streamline initialized manualHook=1 frameTagging=1 hostSDK=2.12.0 project=50f09991-2962-44db-bad7-4be06dbbd1d2");
        return true;
    }

    // Call before native D3D/DXGI members are released. We intentionally keep
    // sl.interposer.dll mapped until process exit so existing proxy vtables remain valid.
    void ShutdownProcess() {
        if (!m_initialized) return;
        SuspendForStaticPresent();
        if (m_shutdown) {
            const sl::Result result = m_shutdown();
            LOG("[DLSS-G] slShutdown result=" << static_cast<int>(result));
        }
        m_initialized = false;
        m_deviceHooked = false;
        m_featureReady = false;
        m_configured = false;
        m_modeApplied = false;
        m_frameToken = nullptr;
        m_currentDeviceIdentity = nullptr;
        m_dlssgSetOptions = nullptr;
        m_dlssgGetState = nullptr;
        m_reflexSetOptions = nullptr;
        m_reflexSleep = nullptr;
        m_pclSetMarker = nullptr;
    }

    bool ProcessInitialized() const { return m_initialized; }
    bool OwnsNgxSessionFor(ID3D12Device* nativeDevice) const {
        // Borrow only after DLSS-G finished its post-swapchain load/state gate; this
        // avoids suppressing the raw NGX fallback when Streamline core initialized but FG did not.
        if (!m_initialized || !m_featureReady || !nativeDevice || !m_currentDeviceIdentity) return false;
        IUnknown* identity = nullptr;
        if (FAILED(nativeDevice->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity))) || !identity) return false;
        const bool same = identity == m_currentDeviceIdentity;
        identity->Release();
        return same;
    }
    void ShutdownForDevice(ID3D12Device* nativeDevice) {
        if (!m_initialized) return;
        bool same = false;
        if (nativeDevice && m_currentDeviceIdentity) {
            IUnknown* identity = nullptr;
            if (SUCCEEDED(nativeDevice->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity))) && identity) {
                same = identity == m_currentDeviceIdentity;
                identity->Release();
            }
        }
        if (!same) {
            LOG("[DLSS-G] Streamline shutdown skipped for retired/non-current D3D12 device");
            return;
        }
        ShutdownProcess();
    }
    bool FeatureReady() const { return m_initialized && m_featureReady; }
    uint32_t MaxGeneratedFrames() const { return m_maxGeneratedFrames; }
    bool VSyncSupported() const { return m_vsyncSupported; }
    bool DynamicMFGSupported() const { return m_dynamicMfgSupported; }
    uint32_t LastFramesActuallyPresented() const { return m_lastFramesActuallyPresented; }
    uint32_t LastStatus() const { return m_lastStatus; }

    ID3D12Device* UpgradeDeviceForQueue(ID3D12Device* nativeDevice) {
        if (!nativeDevice || !InitializeProcess()) return nativeDevice;
        m_deviceHooked = false;
        const sl::Result setDevice = m_setD3DDevice(nativeDevice);
        if (setDevice != sl::Result::eOk) {
            LOG("[DLSS-G] slSetD3DDevice failed result=" << static_cast<int>(setDevice));
            // Step06A4D: never hook DXGI with a native queue after device setup failed.
            // No proxies have been made for this attempt, so tear down the failed
            // session before the renderer takes its normal native-D3D12 fallback.
            ShutdownProcess();
            LOG("[DLSS-G] device setup aborted; native D3D12 fallback for this renderer");
            return nativeDevice;
        }
        IUnknown* deviceIdentity = nullptr;
        m_currentDeviceIdentity = nullptr;
        if (SUCCEEDED(nativeDevice->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&deviceIdentity))) && deviceIdentity) {
            m_currentDeviceIdentity = deviceIdentity;
            deviceIdentity->Release();
        }
        void* base = nativeDevice;
        const sl::Result upgraded = m_upgradeInterface(&base);
        if (upgraded != sl::Result::eOk || !base) {
            LOG("[DLSS-G] device upgrade failed result=" << static_cast<int>(upgraded));
            return nativeDevice;
        }
        m_deviceHooked = true;
        LOG("[DLSS-G] D3D12 device hook ready for CreateCommandQueue");
        return static_cast<ID3D12Device*>(base);
    }

    IDXGIFactory6* UpgradeFactoryForSwapchain(IDXGIFactory6* nativeFactory) {
        if (!nativeFactory || !m_initialized || !m_deviceHooked || !m_upgradeInterface) return nativeFactory;
        void* base = nativeFactory;
        const sl::Result upgraded = m_upgradeInterface(&base);
        if (upgraded != sl::Result::eOk || !base) {
            LOG("[DLSS-G] factory upgrade failed result=" << static_cast<int>(upgraded));
            return nativeFactory;
        }
        LOG("[DLSS-G] DXGI factory hook ready for CreateSwapChainForHwnd");
        return static_cast<IDXGIFactory6*>(base);
    }

    void OnSwapchainCreated(IDXGISwapChain3* swapchain) {
        m_featureReady = false;
        if (!m_initialized || !m_deviceHooked || !swapchain || !m_isFeatureLoaded || !m_getFeatureFunction) return;

        // Required by DLSS-G and also proves GetCurrentBackBufferIndex is routed through the proxy.
        (void)swapchain->GetCurrentBackBufferIndex();

        bool fgLoaded = false;
        bool reflexLoaded = false;
        const sl::Result fgLoad = m_isFeatureLoaded(sl::kFeatureDLSS_G, fgLoaded);
        const sl::Result reflexLoad = m_isFeatureLoaded(sl::kFeatureReflex, reflexLoaded);
        if (fgLoad != sl::Result::eOk || !fgLoaded || reflexLoad != sl::Result::eOk || !reflexLoaded) {
            LOG("[DLSS-G] plugin load gate failed fgResult=" << static_cast<int>(fgLoad) << " fg=" << fgLoaded
                << " reflexResult=" << static_cast<int>(reflexLoad) << " reflex=" << reflexLoaded);
            return;
        }

        if (!ResolveFeature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", m_dlssgSetOptions) ||
            !ResolveFeature(sl::kFeatureDLSS_G, "slDLSSGGetState", m_dlssgGetState) ||
            !ResolveFeature(sl::kFeatureReflex, "slReflexSetOptions", m_reflexSetOptions) ||
            !ResolveFeature(sl::kFeatureReflex, "slReflexSleep", m_reflexSleep) ||
            !ResolveFeature(sl::kFeaturePCL, "slPCLSetMarker", m_pclSetMarker)) {
            LOG("[DLSS-G] required feature function resolution failed");
            return;
        }

        sl::ReflexOptions reflex{};
        reflex.mode = sl::eLowLatency;
        const sl::Result reflexOptions = m_reflexSetOptions(reflex);
        if (reflexOptions != sl::Result::eOk) {
            LOG("[DLSS-G] Reflex low-latency enable failed result=" << static_cast<int>(reflexOptions));
            return;
        }

        sl::DLSSGState state{};
        const sl::ViewportHandle viewport = {0};
        const sl::Result stateResult = m_dlssgGetState(viewport, state, nullptr);
        if (stateResult != sl::Result::eOk) {
            LOG("[DLSS-G] initial state query failed result=" << static_cast<int>(stateResult));
            return;
        }
        m_maxGeneratedFrames = state.numFramesToGenerateMax;
        m_vsyncSupported = state.bIsVsyncSupportAvailable == sl::Boolean::eTrue;
        m_dynamicMfgSupported = state.bIsDynamicMFGSupported == sl::Boolean::eTrue;
        m_lastStatus = static_cast<uint32_t>(state.status);
        m_featureReady = m_maxGeneratedFrames >= 1 && m_lastStatus == 0;
        LOG("[DLSS-G] plugin ready maxGenerated=" << m_maxGeneratedFrames
            << " vsync=" << (m_vsyncSupported ? 1 : 0)
            << " dynamic=" << (m_dynamicMfgSupported ? 1 : 0)
            << " status=" << m_lastStatus);
    }

    void Configure(uint32_t mvecDepthW, uint32_t mvecDepthH,
                   uint32_t colorW, uint32_t colorH, uint32_t backBuffers) {
        m_mvecDepthW = mvecDepthW;
        m_mvecDepthH = mvecDepthH;
        m_colorW = colorW;
        m_colorH = colorH;
        m_backBuffers = backBuffers;
        m_configured = m_featureReady && m_mvecDepthW && m_mvecDepthH && m_colorW && m_colorH && m_backBuffers;
        if (m_configured) {
            LOG("[DLSS-G] geometry configured mvDepth=" << m_mvecDepthW << "x" << m_mvecDepthH
                << " color=" << m_colorW << "x" << m_colorH << " backBuffers=" << m_backBuffers);
        }
        // Keep the swapchain in FG-off mode until an eligible realtime frame begins.
        // This prevents UI toggles, pause, or resource creation from enabling FG outside the render path.
        if (m_configured) (void)ApplyMode(false);
    }

    void SetRequested(bool enabled, uint32_t multiplier=2) {
        const uint32_t clamped=multiplier<2u?2u:(multiplier>6u?6u:multiplier);
        if(m_requestedMultiplier!=clamped){m_requestedMultiplier=clamped;m_modeKnown=false;}
        m_requested=enabled;
        if(!enabled)(void)ApplyMode(false);
    }
    uint32_t RequestedMultiplier() const { return m_requestedMultiplier; }
    uint32_t MaxMultiplier() const { return m_maxGeneratedFrames?std::min(6u,m_maxGeneratedFrames+1u):1u; }
    uint32_t RequestedGeneratedFrames() const {
        uint32_t generated=m_requestedMultiplier>1u?m_requestedMultiplier-1u:1u;
        if(m_maxGeneratedFrames)generated=std::min(generated,m_maxGeneratedFrames);
        return std::max(1u,generated);
    }

    bool BeginFrame(bool eligible, uint32_t frameIndex, bool reset) {
        m_frameToken = nullptr;
        m_currentFrameIndex = frameIndex;
        m_currentReset = reset;
        if (!eligible || !m_requested || !m_featureReady || !m_configured) {
            ApplyMode(false);
            return false;
        }
        if (!ApplyMode(true)) return false;

        sl::FrameToken* token = nullptr;
        const sl::Result tokenResult = m_getNewFrameToken(token, &m_currentFrameIndex);
        if (tokenResult != sl::Result::eOk || !token) {
            LOG("[DLSS-G] slGetNewFrameToken failed result=" << static_cast<int>(tokenResult));
            ApplyMode(false);
            return false;
        }
        m_frameToken = token;
        const sl::Result sleepResult = m_reflexSleep(*m_frameToken);
        if (sleepResult != sl::Result::eOk) {
            LOG("[DLSS-G] slReflexSleep failed result=" << static_cast<int>(sleepResult));
            m_frameToken = nullptr;
            ApplyMode(false);
            return false;
        }
        Mark(sl::PCLMarker::eSimulationStart);
        Mark(sl::PCLMarker::eSimulationEnd);
        return true;
    }

    void MarkRenderSubmitStart() { Mark(sl::PCLMarker::eRenderSubmitStart); }
    void MarkRenderSubmitEnd() { Mark(sl::PCLMarker::eRenderSubmitEnd); }
    void MarkPresentStart() { Mark(sl::PCLMarker::ePresentStart); }
    void MarkPresentEnd() { Mark(sl::PCLMarker::ePresentEnd); }

    bool PreparePresent(ID3D12Resource* depth, uint32_t depthState,
                        ID3D12Resource* motion, uint32_t motionState,
                        ID3D12Resource* hudless, uint32_t hudlessState,
                        ID3D12Resource* uiAlpha, uint32_t uiAlphaState, bool reset) {
        m_currentReset = reset;
        if (!m_frameToken || !depth || !motion || !hudless || !uiAlpha || !m_setConstants || !m_setTagForFrame) {
            CancelCurrentFrame("missing token/resource/core function");
            return false;
        }

        sl::Constants constants{};
        constants.cameraViewToClip = IdentityMatrix();
        constants.clipToCameraView = IdentityMatrix();
        constants.clipToLensClip = IdentityMatrix();
        constants.clipToPrevClip = IdentityMatrix();
        constants.prevClipToClip = IdentityMatrix();
        constants.jitterOffset = sl::float2(0.0f, 0.0f);
        constants.mvecScale = sl::float2(1.0f / static_cast<float>(m_mvecDepthW),
                                        1.0f / static_cast<float>(m_mvecDepthH));
        constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
        constants.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);
        constants.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
        constants.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
        constants.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
        constants.cameraNear = 0.1f;
        constants.cameraFar = 1000.0f;
        constants.cameraFOV = 1.0f;
        constants.cameraAspectRatio = static_cast<float>(m_mvecDepthW) / static_cast<float>(m_mvecDepthH);
        constants.motionVectorsInvalidValue = 65504.0f;
        constants.depthInverted = sl::Boolean::eFalse;
        constants.cameraMotionIncluded = sl::Boolean::eTrue;
        constants.motionVectors3D = sl::Boolean::eFalse;
        constants.reset = m_currentReset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.orthographicProjection = sl::Boolean::eTrue;
        constants.motionVectorsDilated = sl::Boolean::eTrue;
        constants.motionVectorsJittered = sl::Boolean::eFalse;

        const sl::ViewportHandle viewport = {0};
        const sl::Result constantsResult = m_setConstants(constants, *m_frameToken, viewport);
        if (constantsResult != sl::Result::eOk) {
            LOG("[DLSS-G] slSetConstants failed result=" << static_cast<int>(constantsResult));
            CancelCurrentFrame("common constants rejected");
            return false;
        }

        sl::Resource depthResource(sl::ResourceType::eTex2d, depth, depthState);
        sl::Resource motionResource(sl::ResourceType::eTex2d, motion, motionState);
        sl::Resource hudlessResource(sl::ResourceType::eTex2d, hudless, hudlessState);
        sl::Resource uiAlphaResource(sl::ResourceType::eTex2d, uiAlpha, uiAlphaState);
        const sl::Extent mvecExtent{0, 0, m_mvecDepthW, m_mvecDepthH};
        const sl::Extent colorExtent{0, 0, m_colorW, m_colorH};
        sl::ResourceTag tags[] = {
            sl::ResourceTag(&depthResource, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &mvecExtent),
            sl::ResourceTag(&motionResource, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &mvecExtent),
            sl::ResourceTag(&hudlessResource, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &colorExtent),
            sl::ResourceTag(&uiAlphaResource, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent, &colorExtent)
        };
        const sl::Result tagResult = m_setTagForFrame(*m_frameToken, viewport, tags,
                                                       static_cast<uint32_t>(std::size(tags)), nullptr);
        if (tagResult != sl::Result::eOk) {
            LOG("[DLSS-G] slSetTagForFrame failed result=" << static_cast<int>(tagResult));
            CancelCurrentFrame("resource tags rejected");
            return false;
        }
        return true;
    }

    void AfterPresent() {
        if (!m_frameToken || !m_dlssgGetState) return;
        sl::DLSSGState state{};
        const sl::ViewportHandle viewport = {0};
        const sl::Result result = m_dlssgGetState(viewport, state, nullptr);
        if (result == sl::Result::eOk) {
            m_lastFramesActuallyPresented = state.numFramesActuallyPresented;
            m_lastStatus = static_cast<uint32_t>(state.status);
            ++m_stateQueries;
            if (m_stateQueries <= 12 || (m_stateQueries % 120u) == 0 || m_lastStatus != 0 || m_lastFramesActuallyPresented < 2) {
                LOG("[DLSS-G] present frame=" << m_currentFrameIndex
                    << " actualPresented=" << m_lastFramesActuallyPresented
                    << " requestedGenerated=" << RequestedGeneratedFrames() << " requestedMultiplier=" << m_requestedMultiplier << " status=" << m_lastStatus);
            }
        } else {
            LOG("[DLSS-G] post-present state query failed result=" << static_cast<int>(result));
        }
        m_frameToken = nullptr;
    }

    void SuspendForStaticPresent() {
        m_frameToken = nullptr;
        ApplyMode(false);
    }

private:
#ifdef DMP_TESTING
    friend struct DLSSFrameGenerationRuntimeTestAccess;
#endif
    bool m_deviceHooked = false;
    DLSSFrameGenerationRuntime() = default;
    DLSSFrameGenerationRuntime(const DLSSFrameGenerationRuntime&) = delete;
    DLSSFrameGenerationRuntime& operator=(const DLSSFrameGenerationRuntime&) = delete;

    static bool EnvironmentOptIn() {
        const char* value = std::getenv("DMP_DLSSG_RUNTIME");
        if (!value) return false;
        return std::strcmp(value, "1") == 0 || _stricmp(value, "true") == 0 || _stricmp(value, "on") == 0;
    }
    static bool RuntimeMarkerOptIn() {
        wchar_t exePath[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (!length || length >= MAX_PATH) return false;
        wchar_t* slash = std::wcsrchr(exePath, L'\\');
        if (!slash) return false;
        *(slash + 1) = L'\0';
        const std::wstring markerPath = std::wstring(exePath) + L"step05b-runtime.enable";
        const DWORD attrs = GetFileAttributesW(markerPath.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }
    static bool RuntimeOptIn() {
        return EnvironmentOptIn() || RuntimeMarkerOptIn();
    }
    static bool VerboseOptIn() {
        const char* value = std::getenv("DMP_DLSSG_VERBOSE");
        return value && (std::strcmp(value, "1") == 0 || _stricmp(value, "true") == 0 || _stricmp(value, "on") == 0);
    }
    static void StreamlineLog(sl::LogType type, const char* msg) {
        if (static_cast<uint32_t>(type) >= 1u && msg) LOG("[Streamline] " << msg);
    }

    bool ResolveCoreExports() {
        if (!m_module) return false;
        m_init = reinterpret_cast<PFun_slInit*>(GetProcAddress(m_module, "slInit"));
        m_shutdown = reinterpret_cast<PFun_slShutdown*>(GetProcAddress(m_module, "slShutdown"));
        m_setD3DDevice = reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(m_module, "slSetD3DDevice"));
        m_upgradeInterface = reinterpret_cast<PFun_slUpgradeInterface*>(GetProcAddress(m_module, "slUpgradeInterface"));
        m_getFeatureFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(m_module, "slGetFeatureFunction"));
        m_isFeatureLoaded = reinterpret_cast<PFun_slIsFeatureLoaded*>(GetProcAddress(m_module, "slIsFeatureLoaded"));
        m_getNewFrameToken = reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(m_module, "slGetNewFrameToken"));
        m_setConstants = reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(m_module, "slSetConstants"));
        m_setTagForFrame = reinterpret_cast<PFun_slSetTagForFrame*>(GetProcAddress(m_module, "slSetTagForFrame"));
        const bool ok = m_init && m_shutdown && m_setD3DDevice && m_upgradeInterface && m_getFeatureFunction &&
                        m_isFeatureLoaded && m_getNewFrameToken && m_setConstants && m_setTagForFrame;
        if (!ok) LOG("[DLSS-G] missing required core Streamline exports");
        return ok;
    }

    template <typename T>
    bool ResolveFeature(sl::Feature feature, const char* name, T*& out) {
        void* raw = nullptr;
        const sl::Result result = m_getFeatureFunction(feature, name, raw);
        out = reinterpret_cast<T*>(raw);
        if (result != sl::Result::eOk || !out) {
            LOG("[DLSS-G] slGetFeatureFunction " << name << " failed result=" << static_cast<int>(result));
            return false;
        }
        return true;
    }

    static sl::float4x4 IdentityMatrix() {
        sl::float4x4 matrix{};
        matrix.setRow(0, sl::float4(1.0f, 0.0f, 0.0f, 0.0f));
        matrix.setRow(1, sl::float4(0.0f, 1.0f, 0.0f, 0.0f));
        matrix.setRow(2, sl::float4(0.0f, 0.0f, 1.0f, 0.0f));
        matrix.setRow(3, sl::float4(0.0f, 0.0f, 0.0f, 1.0f));
        return matrix;
    }

    sl::DLSSGOptions Options(bool enabled) const {
        sl::DLSSGOptions options{};
        options.mode = enabled ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        options.numFramesToGenerate = RequestedGeneratedFrames(); // STEP 05C: capability-clamped user multiplier.
        options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        options.numBackBuffers = m_backBuffers;
        options.mvecDepthWidth = m_mvecDepthW;
        options.mvecDepthHeight = m_mvecDepthH;
        options.colorWidth = m_colorW;
        options.colorHeight = m_colorH;
        options.colorBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        options.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT;
        options.depthBufferFormat = DXGI_FORMAT_D32_FLOAT;
        options.hudLessBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        options.uiBufferFormat = DXGI_FORMAT_R8_UNORM;
        options.enableUserInterfaceRecomposition = sl::Boolean::eFalse;
        return options;
    }

    bool ApplyMode(bool enabled) {
        if (!m_featureReady || !m_configured || !m_dlssgSetOptions) return !enabled;
        if (m_modeKnown && m_modeApplied == enabled) return true;
        const sl::DLSSGOptions options = Options(enabled);
        const sl::ViewportHandle viewport = {0};
        const sl::Result result = m_dlssgSetOptions(viewport, options);
        if (result != sl::Result::eOk) {
            LOG("[DLSS-G] slDLSSGSetOptions mode=" << (enabled ? "ON" : "OFF")
                << " failed result=" << static_cast<int>(result));
            return false;
        }
        m_modeApplied = enabled;
        m_modeKnown = true;
        LOG("[DLSS-G] mode=" << (enabled ? "ON 2x" : "OFF") << " generatedFrames=" << (enabled ? 1 : 0));
        return true;
    }

    void Mark(sl::PCLMarker marker) {
        if (!m_frameToken || !m_pclSetMarker) return;
        const sl::Result result = m_pclSetMarker(marker, *m_frameToken);
        if (result != sl::Result::eOk) {
            LOG("[DLSS-G] PCL marker " << static_cast<uint32_t>(marker) << " failed result=" << static_cast<int>(result));
        }
    }

    void CancelCurrentFrame(const char* reason) {
        LOG("[DLSS-G] frame generation bypass: " << (reason ? reason : "unknown"));
        m_frameToken = nullptr;
        (void)ApplyMode(false);
    }

    HMODULE m_module{};
    bool m_initialized{};
    bool m_featureReady{};
    bool m_configured{};
    bool m_requested{};
    bool m_modeKnown{};
    bool m_modeApplied{};
    bool m_vsyncSupported{};
    bool m_dynamicMfgSupported{};
    uint32_t m_maxGeneratedFrames{};
    uint32_t m_requestedMultiplier{2};
    uint32_t m_lastFramesActuallyPresented{};
    uint32_t m_lastStatus{};
    uint32_t m_stateQueries{};
    uint32_t m_mvecDepthW{};
    uint32_t m_mvecDepthH{};
    uint32_t m_colorW{};
    uint32_t m_colorH{};
    uint32_t m_backBuffers{};
    uint32_t m_currentFrameIndex{};
    bool m_currentReset{};
    sl::FrameToken* m_frameToken{};
    IUnknown* m_currentDeviceIdentity{}; // identity value only; no retained COM reference

    PFun_slInit* m_init{};
    PFun_slShutdown* m_shutdown{};
    PFun_slSetD3DDevice* m_setD3DDevice{};
    PFun_slUpgradeInterface* m_upgradeInterface{};
    PFun_slGetFeatureFunction* m_getFeatureFunction{};
    PFun_slIsFeatureLoaded* m_isFeatureLoaded{};
    PFun_slGetNewFrameToken* m_getNewFrameToken{};
    PFun_slSetConstants* m_setConstants{};
    PFun_slSetTagForFrame* m_setTagForFrame{};
    PFun_slDLSSGSetOptions* m_dlssgSetOptions{};
    PFun_slDLSSGGetState* m_dlssgGetState{};
    PFun_slReflexSetOptions* m_reflexSetOptions{};
    PFun_slReflexSleep* m_reflexSleep{};
    PFun_slPCLSetMarker* m_pclSetMarker{};
};
