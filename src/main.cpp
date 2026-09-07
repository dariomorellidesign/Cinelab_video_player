#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <wrl/client.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <sstream>
#include <algorithm>
#include <memory>
#include <cmath>
#include <utility>
#include <iterator>
#include <cstdint>
#include <vector>
#include <cwctype>
#include <cstdlib>
#include "VideoDecoder.h"
#include "D3D12Renderer.h"
#include "TemporalGuides.h"
#include "OpticalFlowEngine.h"
#include "AIDepthWorker.h"
#include "AIDepthTemporal.h"
#include "AIDepthTemporalWorker.h"
#include "AudioPlayer.h"
#include "MediaTracks.h"
#include "SubtitlePlayer.h"
#include "Localization.h"
#include "Log.h"

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
static constexpr int CONTROL_H = 112;

static const wchar_t* kVideoPatterns =
    L"*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.wmv;*.asf;*.flv;*.f4v;"
    L"*.ts;*.m2ts;*.mts;*.mpg;*.mpeg;*.mpe;*.vob;*.ogv;*.ogg;*.3gp;*.3g2;"
    L"*.mxf;*.nut;*.rm;*.rmvb;*.divx;*.dv;*.y4m;*.ivf;*.hevc;*.h265;*.h264;*.264;*.av1;*.vp9";

enum : UINT {
    IDM_OPEN=100, IDM_EXIT,
    IDM_PLAY=200, IDM_STOP, IDM_BACK10, IDM_FWD10, IDM_MUTE,
    IDM_DLSS=300, IDM_REHOOK, IDM_VIEW_FINAL, IDM_VIEW_INPUT, IDM_VIEW_MV, IDM_VIEW_DEPTH, IDM_VIEW_MASK, IDM_VIEW_AI_DEPTH, IDM_VIEW_AI_HW_DEPTH, IDM_DEPTH_MODE,
    IDM_QUALITY_AUTO=330, IDM_QUALITY_QUALITY, IDM_QUALITY_BALANCED, IDM_QUALITY_PERFORMANCE, IDM_QUALITY_ULTRAPERF, IDM_QUALITY_DLAA,
    IDM_NVOF_PERF_SLOW=350, IDM_NVOF_PERF_MEDIUM, IDM_NVOF_PERF_FAST,
    IDM_NVOF_GRID_AUTO=360, IDM_NVOF_GRID_1, IDM_NVOF_GRID_2, IDM_NVOF_GRID_4,
    IDM_DEPTH_SOURCE_LEGACY=370, IDM_DEPTH_SOURCE_FLAT, IDM_DEPTH_SOURCE_AI,
    IDM_ASPECT_FIT=400, IDM_ASPECT_FILL, IDM_FULLSCREEN, IDM_VIDEO_ADJUSTMENTS,
    IDM_LANG_BASE=500
};

static constexpr UINT IDM_SPLIT_SCREEN = 380;
static constexpr UINT IDM_SPLIT_RESET = 381;
static constexpr UINT IDM_VSYNC = 382;
static constexpr UINT IDM_FRAMEGEN_OFF = 383;
static constexpr UINT IDM_FRAMEGEN_2X = 384;
static constexpr UINT IDM_FRAMEGEN_3X = 385;
static constexpr UINT IDM_FRAMEGEN_4X = 386;
static constexpr UINT IDM_FRAMEGEN_5X = 387;
static constexpr UINT IDM_FRAMEGEN_6X = 388;
static constexpr UINT IDM_TEMPORAL_MASK_BYPASS = 389; // STEP 05C-1 temporal-mask bypass + VSync FG safety
// STEP 05C FG UX + telemetry
static constexpr UINT IDM_AUDIO_TRACK_BASE = 600;
static constexpr UINT IDM_SUBTITLE_OFF = 700;
static constexpr UINT IDM_SUBTITLE_TRACK_BASE = 710;

static constexpr int HK_PLAY_PAUSE = 9001;
static constexpr int HK_BACK_10 = 9002;
static constexpr int HK_FORWARD_10 = 9003;
static constexpr int HK_MUTE = 9004;
static constexpr int HK_DLSS = 9005;
static constexpr int HK_MEDIA_PLAY_PAUSE = 9006;
static constexpr int HK_ADJUSTMENTS = 9007;
static constexpr int HK_SPLIT_SCREEN = 9008;
static constexpr int HK_SPLIT_RESET = 9009;
static constexpr int HK_SPLIT_LEFT = 9010;
static constexpr int HK_SPLIT_RIGHT = 9011;
static constexpr int HK_VSYNC = 9012;
static constexpr int HK_FRAMEGEN = 9013;

static constexpr int IDC_ADJ_BRIGHTNESS = 7101;
static constexpr int IDC_ADJ_CONTRAST = 7102;
static constexpr int IDC_ADJ_SATURATION = 7103;
static constexpr int IDC_ADJ_GAMMA = 7104;
static constexpr int IDC_ADJ_TEMPERATURE = 7105;
static constexpr int IDC_ADJ_TINT = 7106;
static constexpr int IDC_ADJ_RESET = 7110;
static constexpr int IDC_ADJ_CLOSE = 7111;

struct AppOptions {
    // Step 04B-1: output is monitor-native by default. maxW/maxH are only
    // populated when --output WIDTHxHEIGHT is explicitly requested.
    uint32_t maxW=0, maxH=0;
    bool outputExplicit=false;
    NVSDK_NGX_PerfQuality_Value quality=NVSDK_NGX_PerfQuality_Value_MaxQuality;
    bool qualityExplicit=false;
    D3D12Renderer::DepthSource depthSource=D3D12Renderer::DepthSource::Legacy;
    std::wstring file;
};

static AppOptions ParseArgs() {
    AppOptions o; int argc=0; LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (!argv) return o;
    for(int i=1;i<argc;++i) {
        std::wstring a=argv[i];
        if(a==L"--output" && i+1<argc) {
            std::wstring v=argv[++i];std::wstring lower=v;std::transform(lower.begin(),lower.end(),lower.begin(),::towlower);
            if(lower==L"auto") { o.outputExplicit=false;o.maxW=0;o.maxH=0; }
            else {
                auto x=v.find(L'x');if(x==std::wstring::npos)x=v.find(L'X');
                if(x!=std::wstring::npos){
                    const int w=_wtoi(v.substr(0,x).c_str()),h=_wtoi(v.substr(x+1).c_str());
                    if(w>=64&&h>=64){o.maxW=uint32_t(w);o.maxH=uint32_t(h);o.outputExplicit=true;}
                }
            }
        } else if(a==L"--quality" && i+1<argc) {
            std::wstring q=argv[++i]; std::transform(q.begin(),q.end(),q.begin(),::towlower);
            if(q==L"auto") { o.qualityExplicit=false; }
            else {
                o.qualityExplicit=true;
                if(q==L"performance"||q==L"perf") o.quality=NVSDK_NGX_PerfQuality_Value_MaxPerf;
                else if(q==L"balanced") o.quality=NVSDK_NGX_PerfQuality_Value_Balanced;
                else if(q==L"ultra-performance"||q==L"ultraperf") o.quality=NVSDK_NGX_PerfQuality_Value_UltraPerformance;
                else if(q==L"dlaa") o.quality=NVSDK_NGX_PerfQuality_Value_DLAA;
                else o.quality=NVSDK_NGX_PerfQuality_Value_MaxQuality;
            }
        } else if(a==L"--depth-source" && i+1<argc) {
            std::wstring d=argv[++i];std::transform(d.begin(),d.end(),d.begin(),::towlower);
            if(d==L"flat")o.depthSource=D3D12Renderer::DepthSource::Flat;
            else if(d==L"ai"||d==L"synthetic"||d==L"ai-synthetic")o.depthSource=D3D12Renderer::DepthSource::AISynthetic;
            else o.depthSource=D3D12Renderer::DepthSource::Legacy;
        } else if(!a.empty() && a[0]!=L'-') o.file=a;
    }
    LocalFree(argv); return o;
}

static std::wstring PickVideoFileFallback(HWND owner, const Localizer& loc) {
    wchar_t path[32768]{};
    std::wstring filter;
    filter += loc.Get(L"dialog.all_ffmpeg"); filter.push_back(L'\0');
    filter += L"*.*"; filter.push_back(L'\0');
    filter += loc.Get(L"dialog.supported"); filter.push_back(L'\0');
    filter += kVideoPatterns; filter.push_back(L'\0');
    filter += loc.Get(L"dialog.all"); filter.push_back(L'\0');
    filter += L"*.*"; filter.push_back(L'\0'); filter.push_back(L'\0');
    const std::wstring title = loc.Get(L"dialog.title");
    OPENFILENAMEW o{}; o.lStructSize=sizeof(o); o.hwndOwner=owner; o.lpstrFile=path; o.nMaxFile=static_cast<DWORD>(std::size(path));
    o.lpstrFilter=filter.c_str(); o.nFilterIndex=1; o.lpstrTitle=title.c_str();
    o.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_EXPLORER|OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&o)?path:L"";
}

static std::wstring PickVideoFile(HWND owner, const Localizer& loc) {
    ComPtr<IFileOpenDialog> dlg;
    HRESULT hr=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dlg));
    if(SUCCEEDED(hr) && dlg) {
        const std::wstring allFfmpeg=loc.Get(L"dialog.all_ffmpeg"), supported=loc.Get(L"dialog.supported"), all=loc.Get(L"dialog.all"), title=loc.Get(L"dialog.title");
        COMDLG_FILTERSPEC specs[3]={{allFfmpeg.c_str(),L"*.*"},{supported.c_str(),kVideoPatterns},{all.c_str(),L"*.*"}};
        dlg->SetFileTypes(3,specs); dlg->SetFileTypeIndex(1); dlg->SetTitle(title.c_str());
        FILEOPENDIALOGOPTIONS opts{}; if(SUCCEEDED(dlg->GetOptions(&opts))) dlg->SetOptions(opts|FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_PATHMUSTEXIST);
        hr=dlg->Show(owner);
        if(hr==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return L"";
        if(SUCCEEDED(hr)) {
            ComPtr<IShellItem> item; if(SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR p=nullptr; if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&p)) && p) {
                    std::wstring result(p); CoTaskMemFree(p); return result;
                }
            }
        }
    }
    return PickVideoFileFallback(owner,loc);
}

static std::wstring TimeText(double sec) {
    if(!std::isfinite(sec)||sec<0) sec=0; int s=int(sec+0.5),h=s/3600; s%=3600; int m=s/60; s%=60; wchar_t b[64];
    if(h) swprintf_s(b,L"%d:%02d:%02d",h,m,s); else swprintf_s(b,L"%02d:%02d",m,s); return b;
}

static const wchar_t* DepthSourceNameW(D3D12Renderer::DepthSource s){
    switch(s){case D3D12Renderer::DepthSource::Flat:return L"Flat";case D3D12Renderer::DepthSource::AISynthetic:return L"AI Synthetic";default:return L"Legacy";}
}
class PlayerApp {
    bool m_splitScreen=false;
    float m_splitFraction=0.5f;
    bool m_splitDragging=false;
    MediaTrackCatalog m_mediaTracks;
    SubtitlePlayer m_subtitles;
    int m_selectedAudioStream=-1;
    int m_selectedSubtitleStream=-1;
    std::wstring m_subtitleText;
    bool m_subtitleClockAnnounced=false;
    bool m_vsyncEnabled=true;
    bool m_frameGenerationEnabled=false;
    uint32_t m_frameGenerationMultiplier=2;
    bool m_temporalMaskBypass=false;
    // STEP 05B DLSS-G 2x bring-up
    // STEP 04H V-Sync toggle
    // STEP 04G-4 playback-clock subtitle sync
    HWND m_subtitleWnd=nullptr;
    // STEP 04G media track selection
    // STEP 04F-1 movable split divider
public:
    explicit PlayerApp(AppOptions o):m_opt(std::move(o)){}
    ~PlayerApp(){SaveVideoSettings();if(m_adjustWnd)DestroyWindow(m_adjustWnd);UnregisterOverlayHotkeys();Unload(); if(m_font)DeleteObject(m_font); if(m_fontSmall)DeleteObject(m_fontSmall);}

    bool Create(HINSTANCE hi) {
        m_loc.Initialize();
        m_loc.SetLanguage(L"en-US", true);
        LoadVideoSettings();
        INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_BAR_CLASSES|ICC_WIN95_CLASSES};InitCommonControlsEx(&icc);
        WNDCLASSW r{}; r.style=CS_DBLCLKS|CS_OWNDC; r.lpfnWndProc=RenderWndProcStatic; r.hInstance=hi; r.lpszClassName=L"DLSSVideoRenderClassV11"; r.hCursor=LoadCursor(nullptr,IDC_ARROW); r.hbrBackground=nullptr; RegisterClassW(&r);
        WNDCLASSW v{}; v.lpfnWndProc=ViewportWndProcStatic; v.hInstance=hi; v.lpszClassName=L"DLSSVideoViewportClassV11"; v.hCursor=LoadCursor(nullptr,IDC_ARROW); v.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassW(&v);
        WNDCLASSW u{}; u.style=CS_DBLCLKS; u.lpfnWndProc=ControlsWndProcStatic; u.hInstance=hi; u.lpszClassName=L"DLSSMediaControlsClassV12"; u.hCursor=LoadCursor(nullptr,IDC_ARROW); u.hbrBackground=nullptr; RegisterClassW(&u);
        WNDCLASSW a{}; a.lpfnWndProc=AdjustWndProcStatic; a.hInstance=hi; a.lpszClassName=L"DLSSVideoAdjustmentsClassV11"; a.hCursor=LoadCursor(nullptr,IDC_ARROW); a.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1); RegisterClassW(&a);
        WNDCLASSW sub{}; sub.lpfnWndProc=SubtitleWndProcStatic; sub.hInstance=hi; sub.lpszClassName=L"DLSSSubtitleOverlayClass"; sub.hCursor=LoadCursor(nullptr,IDC_ARROW); sub.hbrBackground=nullptr; RegisterClassW(&sub);
        WNDCLASSW w{}; w.lpfnWndProc=WndProcStatic; w.hInstance=hi; w.lpszClassName=L"DLSSVideoPlayerV11Class"; w.hCursor=LoadCursor(nullptr,IDC_ARROW); w.hbrBackground=CreateSolidBrush(RGB(18,19,21)); RegisterClassW(&w);
        RECT rc{0,0,1440,880}; AdjustWindowRect(&rc,WS_OVERLAPPEDWINDOW,TRUE);
        const std::wstring appTitle=m_loc.Get(L"app.title");
        m_hwnd=CreateWindowExW(WS_EX_ACCEPTFILES,w.lpszClassName,appTitle.c_str(),WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,rc.right-rc.left,rc.bottom-rc.top,nullptr,CreateMenuBar(),hi,this);
        if(!m_hwnd) return false;
        m_menuBar=GetMenu(m_hwnd);
        RegisterOverlayHotkeys();
        BOOL dark=TRUE; DwmSetWindowAttribute(m_hwnd,20,&dark,sizeof(dark)); DWORD corner=2; DwmSetWindowAttribute(m_hwnd,33,&corner,sizeof(corner));
        m_viewport=CreateWindowExW(0,v.lpszClassName,nullptr,WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,100,100,m_hwnd,nullptr,hi,this);
        m_renderWnd=CreateWindowExW(WS_EX_ACCEPTFILES,L"DLSSVideoRenderClassV11",nullptr,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,0,0,100,100,m_viewport,nullptr,hi,this);
        m_subtitleWnd=nullptr; // Step 04G-3: D3D12 backbuffer compositor replaces the invisible layered child overlay.
        m_controlsWnd=CreateWindowExW(0,L"DLSSMediaControlsClassV12",nullptr,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,0,0,100,CONTROL_H,m_hwnd,nullptr,hi,this);
        if(!m_controlsWnd)return false;
        CreateDebugTooltips();
        m_font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        m_fontSmall=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
        DragAcceptFiles(m_hwnd,TRUE); DragAcceptFiles(m_renderWnd,TRUE); ShowWindow(m_viewport,SW_HIDE); Layout(); UpdateTitle();
        if(!m_opt.file.empty()) Load(m_opt.file); // No startup file picker: the player opens idle by default.
        return true;
    }

    void Tick() {
        UpdateFullscreenUiVisibility();
        if(m_seekPending) {
            const double target=m_pendingSeekSec; const bool resume=m_seekResumePlaying;
            m_seekPending=false; PerformSeek(target,resume); return;
        }
        if(m_loaded&&!m_playing&&!m_seeking&&m_renderer){
            const auto nowClock=Clock::now();
            if(std::chrono::duration<double>(nowClock-m_lastStaticPresent).count()>=1.0/60.0){
                UpdateSubtitleForTime(Position());m_renderer->PresentCurrent();
                m_lastStaticPresent=nowClock;
            }
        }
        if(!m_loaded||!m_playing||!m_haveNext||m_seeking) return;
        double now=Position(); UpdateSubtitleForTime(now); const double frameDur=1.0/std::max(1.0,m_decoder.FrameRate());
        bool dropped=false,droppedDiscontinuity=false;
        while(m_haveNext) {
            double due=double(m_next.timestamp100ns)*1e-7;
            if(now-due <= std::max(0.085,frameDur*2.25)) break;
            VideoFrame skip=std::move(m_next); droppedDiscontinuity=droppedDiscontinuity||skip.discontinuity; ++m_droppedFrames; dropped=true;
            if(!m_decoder.ReadNext(m_next)){m_haveNext=false;break;}
        }
        if(droppedDiscontinuity){
            // A discontinuity marker may itself be skipped while catching up. Propagate
            // that semantic reset to the next rendered frame.
            m_guides.Reset();m_guideReset=true;m_dlssReset=true;
            LOG("[Temporal] skipped decoder discontinuity -> hard reset");
        }else if(dropped){
            // ordinary playback catch-up: preserving NVOF/guide/DLSS history
            // The next NVOF pair spans from the last PRESENTED frame to the new one.
            // frameTimeMs already uses the real timestamp gap, so a normal drop is not
            // a scene cut and must not zero motion or restart DLSS history.
            if(m_droppedFrames<=8 || (m_droppedFrames%60u)==0u)
                LOG("[Temporal] playback catch-up drop; preserving history. droppedTotal="<<m_droppedFrames);
        }
        if(!m_haveNext){m_playing=false;m_audio.Pause(true);InvalidateRect(m_hwnd,nullptr,FALSE);return;}
        double due=double(m_next.timestamp100ns)*1e-7;
        if(now+0.001<due) return;
        const auto frameProcessStart=Clock::now();
        const bool frameProcessOk=RenderVideoFrame(m_next,m_next.discontinuity||m_guideReset);
        m_lastFrameProcessMs=std::chrono::duration<double,std::milli>(Clock::now()-frameProcessStart).count();
        if(frameProcessOk) {
            ++m_fpsWindowFrames;
            const auto fpsNow=Clock::now();
            const double fpsElapsed=std::chrono::duration<double>(fpsNow-m_fpsWindowStart).count();
            if(fpsElapsed>=0.75){
                m_submitFps=double(m_fpsWindowFrames)/fpsElapsed;m_fpsWindowFrames=0;m_fpsWindowStart=fpsNow;
                if(m_renderer){
                    const uint64_t totalDisplayed=m_renderer->FrameGenerationDisplayedFramesTotal();
                    const uint64_t deltaDisplayed=totalDisplayed>=m_lastDisplayedFramesTotal?totalDisplayed-m_lastDisplayedFramesTotal:0u;
                    m_displayFps=deltaDisplayed?double(deltaDisplayed)/fpsElapsed:m_submitFps;
                    m_lastDisplayedFramesTotal=totalDisplayed;
                    m_actualDisplayRatio=m_submitFps>0.01?m_displayFps/m_submitFps:1.0;
                }else{m_displayFps=m_submitFps;m_actualDisplayRatio=1.0;}
                if(m_renderer){
                    const auto aiStats=m_aiDepthTemporalWorker.GetStats();
                    const double aiAgeMs=(aiStats.latestTimestamp100ns>=0&&m_lastRenderedTs>=aiStats.latestTimestamp100ns)?double(m_lastRenderedTs-aiStats.latestTimestamp100ns)*1.0e-4:-1.0;
                    LOG("[Perf] submitFps="<<m_submitFps<<" sourceFps="<<m_decoder.FrameRate()<<" dropped="<<m_droppedFrames<<" frameMs="<<m_lastFrameProcessMs<<" input="<<m_renderer->DLSSInputW()<<"x"<<m_renderer->DLSSInputH()<<" output="<<m_renderer->OutputW()<<"x"<<m_renderer->OutputH()<<" aiTempMs="<<aiStats.emaProcessMs<<" aiQ="<<aiStats.queueDepth<<" aiQReset="<<aiStats.queueResets<<" aiAgeMs="<<aiAgeMs);
                    const OpticalFlowStats ofStats=m_opticalFlow?m_opticalFlow->GetStats():OpticalFlowStats{};
                    const double pipelineKnownMs=ofStats.lastTotalMs+m_lastGuidesMs+m_lastRendererMs;
                    const double pipelineOtherMs=std::max(0.0,m_lastFrameProcessMs-pipelineKnownMs);
                    LOG("[Pipeline] quality="<<QualityNameA(m_activeQuality)<<" nvofMode="<<(m_opticalFlow?m_opticalFlow->PerfName():"OFF")<<" grid="<<(m_opticalFlow?m_opticalFlow->GridSize():0)<<" nvofMs="<<ofStats.lastTotalMs<<" nvofUploadMs="<<ofStats.lastUploadMs<<" nvofExecuteMs="<<ofStats.lastExecuteMs<<" nvofDownloadMs="<<ofStats.lastDownloadMs<<" nvofConvertMs="<<ofStats.lastConvertMs<<" guidesMs="<<m_lastGuidesMs<<" guideFast="<<(m_lastGuideHardwareFastPath?1:0)<<" legacyFlow="<<(m_lastGuideLegacyFlow?1:0)<<" rendererMs="<<m_lastRendererMs<<" otherMs="<<pipelineOtherMs<<" frameMs="<<m_lastFrameProcessMs<<" aiStaleRecoveries="<<aiStats.staleRecoveries);
                }
            }
        }
        m_currentSec=due; m_guideReset=false; m_dlssReset=false;
        if(!m_decoder.ReadNext(m_next)){m_haveNext=false;m_playing=false;m_audio.Pause(true);}
        if((++m_uiTick%15)==0) UpdateTitle();
        InvalidateControls();
    }

    bool Running()const{return m_running;}
    bool NeedsRealtimeTick()const{return m_loaded;}
    DWORD TickSleepMs()const{return (m_loaded&&!m_playing&&!m_seekPending&&!m_seeking)?8u:0u;}


private:
    std::wstring T(const wchar_t* key)const{return m_loc.Get(key);}

    std::filesystem::path SettingsPath()const{
        wchar_t p[32768]{};DWORD n=GetModuleFileNameW(nullptr,p,static_cast<DWORD>(std::size(p)));
        if(!n||n>=std::size(p))return std::filesystem::current_path()/L"DLSSVideoPlayer.ini";
        return std::filesystem::path(p).parent_path()/L"DLSSVideoPlayer.ini";
    }

    float ReadIniFloat(const wchar_t* section,const wchar_t* key,float fallback)const{
        wchar_t def[64]{},buf[128]{};swprintf_s(def,L"%.6f",fallback);
        const auto path=SettingsPath();GetPrivateProfileStringW(section,key,def,buf,static_cast<DWORD>(std::size(buf)),path.c_str());
        wchar_t* end=nullptr;double v=wcstod(buf,&end);return (end&&end!=buf&&std::isfinite(v))?float(v):fallback;
    }

    void WriteIniFloat(const wchar_t* section,const wchar_t* key,float value)const{
        wchar_t buf[64]{};swprintf_s(buf,L"%.6f",value);const auto path=SettingsPath();WritePrivateProfileStringW(section,key,buf,path.c_str());
    }

    void LoadVideoSettings(){
        m_colorSettings.brightness=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Brightness",0.0f),-2.0f,2.0f);
        m_colorSettings.contrast=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Contrast",1.0f),0.0f,3.0f);
        m_colorSettings.saturation=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Saturation",1.0f),0.0f,3.0f);
        m_colorSettings.gamma=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Gamma",1.0f),0.25f,3.0f);
        m_colorSettings.temperature=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Temperature",0.0f),-1.0f,1.0f);
        m_colorSettings.tint=std::clamp(ReadIniFloat(L"VideoAdjustments",L"Tint",0.0f),-1.0f,1.0f);
        m_splitFraction=std::clamp(ReadIniFloat(L"SplitScreen",L"Fraction",0.5f),0.05f,0.95f);
        m_vsyncEnabled=ReadIniFloat(L"Playback",L"VSync",1.0f)>=0.5f;
        m_frameGenerationEnabled=ReadIniFloat(L"Playback",L"FrameGeneration2x",0.0f)>=0.5f;
        m_frameGenerationMultiplier=uint32_t(std::clamp(int(std::lround(ReadIniFloat(L"Playback",L"FrameGenerationMultiplier",2.0f))),2,6));
        m_temporalMaskBypass=ReadIniFloat(L"Temporal",L"MaskBypass",0.0f)>=0.5f;
        const auto path=SettingsPath();
        m_fullscreenAutoHide=GetPrivateProfileIntW(L"UI",L"FullscreenAutoHide",1,path.c_str())!=0;
    }

    void SaveVideoSettings()const{
        WriteIniFloat(L"VideoAdjustments",L"Brightness",m_colorSettings.brightness);
        WriteIniFloat(L"VideoAdjustments",L"Contrast",m_colorSettings.contrast);
        WriteIniFloat(L"VideoAdjustments",L"Saturation",m_colorSettings.saturation);
        WriteIniFloat(L"VideoAdjustments",L"Gamma",m_colorSettings.gamma);
        WriteIniFloat(L"VideoAdjustments",L"Temperature",m_colorSettings.temperature);
        WriteIniFloat(L"VideoAdjustments",L"Tint",m_colorSettings.tint);
        WriteIniFloat(L"SplitScreen",L"Fraction",m_splitFraction);
        WriteIniFloat(L"Playback",L"VSync",m_vsyncEnabled?1.0f:0.0f);
        WriteIniFloat(L"Playback",L"FrameGeneration2x",m_frameGenerationEnabled?1.0f:0.0f);
        WriteIniFloat(L"Playback",L"FrameGenerationMultiplier",float(m_frameGenerationMultiplier));
        WriteIniFloat(L"Temporal",L"MaskBypass",m_temporalMaskBypass?1.0f:0.0f);
        const auto path=SettingsPath();
        WritePrivateProfileStringW(L"UI",L"FullscreenAutoHide",m_fullscreenAutoHide?L"1":L"0",path.c_str());
    }

    void ApplyVideoAdjustments(bool refreshPaused=true){
        if(m_renderer){
            m_renderer->SetColorSettings(m_colorSettings);m_renderer->SetSplitScreen(m_splitScreen);m_renderer->SetSplitFraction(m_splitFraction);m_renderer->SetVSync(m_vsyncEnabled);m_renderer->SetFrameGeneration(m_frameGenerationEnabled,m_frameGenerationMultiplier);
            if(refreshPaused&&!m_playing&&!m_seeking)m_renderer->PresentCurrent();
        }
    }

    void InvalidateControls(){
        if(!m_loaded){if(m_hwnd)InvalidateRect(m_hwnd,nullptr,FALSE);return;}
        if(m_controlsWnd&&IsWindowVisible(m_controlsWnd))InvalidateRect(m_controlsWnd,nullptr,FALSE);
    }

    void SetTrack(HWND h,int id,int lo,int hi,int pos){
        HWND t=GetDlgItem(h,id);if(!t)return;SendMessageW(t,TBM_SETRANGE,TRUE,MAKELPARAM(lo,hi));SendMessageW(t,TBM_SETPOS,TRUE,pos);
    }

    void SetAdjustmentValue(HWND h,int id,const std::wstring& value){
        HWND v=GetDlgItem(h,id+100);if(v)SetWindowTextW(v,value.c_str());
    }

    static std::wstring SignedValue(float v,const wchar_t* suffix=L""){
        wchar_t b[64]{};swprintf_s(b,L"%+.2f%ls",v,suffix);return b;
    }

    static std::wstring PlainValue(float v,const wchar_t* suffix=L""){
        wchar_t b[64]{};swprintf_s(b,L"%.2f%ls",v,suffix);return b;
    }

    void UpdateAdjustmentValueLabels(HWND h){
        SetAdjustmentValue(h,IDC_ADJ_BRIGHTNESS,SignedValue(m_colorSettings.brightness,L" EV"));
        SetAdjustmentValue(h,IDC_ADJ_CONTRAST,PlainValue(m_colorSettings.contrast));
        SetAdjustmentValue(h,IDC_ADJ_SATURATION,PlainValue(m_colorSettings.saturation));
        SetAdjustmentValue(h,IDC_ADJ_GAMMA,PlainValue(m_colorSettings.gamma));
        SetAdjustmentValue(h,IDC_ADJ_TEMPERATURE,SignedValue(m_colorSettings.temperature));
        SetAdjustmentValue(h,IDC_ADJ_TINT,SignedValue(m_colorSettings.tint));
    }

    void SyncAdjustmentControls(HWND h){
        SetTrack(h,IDC_ADJ_BRIGHTNESS,0,400,int(std::lround((m_colorSettings.brightness+2.0f)*100.0f)));
        SetTrack(h,IDC_ADJ_CONTRAST,0,300,int(std::lround(m_colorSettings.contrast*100.0f)));
        SetTrack(h,IDC_ADJ_SATURATION,0,300,int(std::lround(m_colorSettings.saturation*100.0f)));
        SetTrack(h,IDC_ADJ_GAMMA,25,300,int(std::lround(m_colorSettings.gamma*100.0f)));
        SetTrack(h,IDC_ADJ_TEMPERATURE,0,200,int(std::lround((m_colorSettings.temperature+1.0f)*100.0f)));
        SetTrack(h,IDC_ADJ_TINT,0,200,int(std::lround((m_colorSettings.tint+1.0f)*100.0f)));
        UpdateAdjustmentValueLabels(h);
    }

    void ReadAdjustmentControls(HWND h){
        auto pos=[&](int id)->int{HWND t=GetDlgItem(h,id);return t?int(SendMessageW(t,TBM_GETPOS,0,0)):0;};
        m_colorSettings.brightness=float(pos(IDC_ADJ_BRIGHTNESS))/100.0f-2.0f;
        m_colorSettings.contrast=float(pos(IDC_ADJ_CONTRAST))/100.0f;
        m_colorSettings.saturation=float(pos(IDC_ADJ_SATURATION))/100.0f;
        m_colorSettings.gamma=std::max(0.25f,float(pos(IDC_ADJ_GAMMA))/100.0f);
        m_colorSettings.temperature=float(pos(IDC_ADJ_TEMPERATURE))/100.0f-1.0f;
        m_colorSettings.tint=float(pos(IDC_ADJ_TINT))/100.0f-1.0f;
        UpdateAdjustmentValueLabels(h);ApplyVideoAdjustments(true);
    }

    void CreateAdjustmentRow(HWND h,int id,const wchar_t* labelKey,int y){
        HFONT f=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND label=CreateWindowExW(0,L"STATIC",T(labelKey).c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,16,y,116,20,h,nullptr,nullptr,nullptr);
        HWND track=CreateWindowExW(0,TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|TBS_HORZ|TBS_NOTICKS,132,y-6,236,30,h,(HMENU)(INT_PTR)id,nullptr,nullptr);
        HWND value=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE|SS_RIGHT,370,y,64,20,h,(HMENU)(INT_PTR)(id+100),nullptr,nullptr);
        SendMessageW(label,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(track,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(value,WM_SETFONT,(WPARAM)f,TRUE);
    }

    void BuildAdjustmentControls(HWND h){
        CreateAdjustmentRow(h,IDC_ADJ_BRIGHTNESS,L"adjustments.brightness",28);
        CreateAdjustmentRow(h,IDC_ADJ_CONTRAST,L"adjustments.contrast",78);
        CreateAdjustmentRow(h,IDC_ADJ_SATURATION,L"adjustments.saturation",128);
        CreateAdjustmentRow(h,IDC_ADJ_GAMMA,L"adjustments.gamma",178);
        CreateAdjustmentRow(h,IDC_ADJ_TEMPERATURE,L"adjustments.temperature",228);
        CreateAdjustmentRow(h,IDC_ADJ_TINT,L"adjustments.tint",278);
        HFONT f=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND note=CreateWindowExW(0,L"STATIC",T(L"adjustments.note").c_str(),WS_CHILD|WS_VISIBLE|SS_LEFT,16,322,418,38,h,nullptr,nullptr,nullptr);SendMessageW(note,WM_SETFONT,(WPARAM)f,TRUE);
        HWND reset=CreateWindowExW(0,L"BUTTON",T(L"adjustments.reset").c_str(),WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,252,368,86,30,h,(HMENU)(INT_PTR)IDC_ADJ_RESET,nullptr,nullptr);
        HWND close=CreateWindowExW(0,L"BUTTON",T(L"adjustments.close").c_str(),WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON,348,368,86,30,h,(HMENU)(INT_PTR)IDC_ADJ_CLOSE,nullptr,nullptr);
        SendMessageW(reset,WM_SETFONT,(WPARAM)f,TRUE);SendMessageW(close,WM_SETFONT,(WPARAM)f,TRUE);
        SyncAdjustmentControls(h);
    }

    void ShowAdjustments(){
        if(m_adjustWnd&&IsWindow(m_adjustWnd)){ShowWindow(m_adjustWnd,SW_SHOWNORMAL);SetForegroundWindow(m_adjustWnd);return;}
        RECT pr{};GetWindowRect(m_hwnd,&pr);const int w=466,h=452,pw=int(pr.right-pr.left),ph=int(pr.bottom-pr.top);int x=int(pr.left)+std::max(0,(pw-w)/2),y=int(pr.top)+std::max(0,(ph-h)/2);
        m_adjustWnd=CreateWindowExW(WS_EX_TOOLWINDOW,L"DLSSVideoAdjustmentsClassV11",T(L"adjustments.title").c_str(),
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_VISIBLE,x,y,w,h,m_hwnd,nullptr,GetModuleHandleW(nullptr),this);
    }

    LRESULT AdjustWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        switch(m){
        case WM_CREATE:BuildAdjustmentControls(h);return 0;
        case WM_HSCROLL:ReadAdjustmentControls(h);return 0;
        case WM_COMMAND:
            if(LOWORD(w)==IDC_ADJ_RESET){m_colorSettings={};SyncAdjustmentControls(h);ApplyVideoAdjustments(true);SaveVideoSettings();return 0;}
            if(LOWORD(w)==IDC_ADJ_CLOSE){DestroyWindow(h);return 0;}
            break;
        case WM_CLOSE:DestroyWindow(h);return 0;
        case WM_DESTROY:SaveVideoSettings();if(h==m_adjustWnd)m_adjustWnd=nullptr;return 0;
        }
        return DefWindowProcW(h,m,w,l);
    }

    void PaintSubtitle(HWND h){
        PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);RECT r{};GetClientRect(h,&r);HBRUSH key=CreateSolidBrush(RGB(1,2,3));FillRect(dc,&r,key);DeleteObject(key);
        if(!m_subtitleText.empty()&&r.right>40&&r.bottom>40){const int H=std::max(1,int(r.bottom-r.top)),W=std::max(1,int(r.right-r.left));const int px=std::clamp(H/22,26,58);HFONT font=CreateFontW(-px,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");auto old=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);RECT tr{std::max(20,W/18),int(H*0.64),W-std::max(20,W/18),H-std::max(20,H/18)};const UINT fmt=DT_CENTER|DT_WORDBREAK|DT_NOPREFIX;RECT calc=tr;DrawTextW(dc,m_subtitleText.c_str(),-1,&calc,fmt|DT_CALCRECT);const int textH=std::max(1,int(calc.bottom-calc.top));tr.top=std::max(tr.top,tr.bottom-textH);SetTextColor(dc,RGB(0,0,0));for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx)if(dx||dy){RECT q=tr;OffsetRect(&q,dx,dy);DrawTextW(dc,m_subtitleText.c_str(),-1,&q,fmt);}SetTextColor(dc,RGB(246,246,246));DrawTextW(dc,m_subtitleText.c_str(),-1,&tr,fmt);SelectObject(dc,old);DeleteObject(font);}
        EndPaint(h,&ps);
    }
    LRESULT SubtitleWndProc(HWND h,UINT m,WPARAM w,LPARAM l){switch(m){case WM_NCHITTEST:return HTTRANSPARENT;case WM_ERASEBKGND:return 1;case WM_PAINT:PaintSubtitle(h);return 0;}return DefWindowProcW(h,m,w,l);}
    static LRESULT CALLBACK SubtitleWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l){PlayerApp*a=nullptr;if(m==WM_NCCREATE){auto*cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));return a?a->SubtitleWndProc(h,m,w,l):DefWindowProcW(h,m,w,l);}
    static LRESULT CALLBACK WndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        PlayerApp* a=nullptr;
        if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
        else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        return a?a->WndProc(h,m,w,l):DefWindowProcW(h,m,w,l);
    }
    static LRESULT CALLBACK ViewportWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        PlayerApp* a=nullptr;
        if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
        else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        switch(m){
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);RECT r{};GetClientRect(h,&r);FillRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));EndPaint(h,&ps);return 0;}
        case WM_MOUSEMOVE:
            if(a){POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};MapWindowPoints(h,a->m_hwnd,&p,1);a->HandleFullscreenPointer(p.x,p.y);}return 0;
        case WM_MOUSEWHEEL:case WM_KEYDOWN:case WM_SYSKEYDOWN:
            if(a)return SendMessageW(a->m_hwnd,m,w,l);break;
        }
        return DefWindowProcW(h,m,w,l);
    }

    static LRESULT CALLBACK AdjustWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        PlayerApp* a=nullptr;
        if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
        else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        return a?a->AdjustWndProc(h,m,w,l):DefWindowProcW(h,m,w,l);
    }

    static LRESULT CALLBACK RenderWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        PlayerApp* a=nullptr;
        if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
        else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(a){
            if(m==WM_ERASEBKGND)return 1;
            if(m==WM_PAINT){PAINTSTRUCT ps{};BeginPaint(h,&ps);EndPaint(h,&ps);return 0;}
            if(m==WM_MOUSEMOVE){POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};MapWindowPoints(h,a->m_hwnd,&p,1);a->HandleFullscreenPointer(p.x,p.y);return 0;}
            if(m==WM_LBUTTONDOWN){SetFocus(a->m_hwnd);if(a->BeginSplitDrag(GET_X_LPARAM(l)))return 0;return 0;}
            if(m==WM_MOUSEMOVE){if(a->m_splitDragging){a->UpdateSplitDrag(GET_X_LPARAM(l));return 0;}if(a->SplitDividerHitTest(GET_X_LPARAM(l),10))SetCursor(LoadCursor(nullptr,IDC_SIZEWE));}
            if(m==WM_LBUTTONUP){if(a->m_splitDragging){a->UpdateSplitDrag(GET_X_LPARAM(l));a->EndSplitDrag();return 0;}}
            if(m==WM_CAPTURECHANGED){if(a->m_splitDragging)a->EndSplitDrag();return 0;}
            if(m==WM_LBUTTONDBLCLK){a->ToggleFullscreen();return 0;}
            if(m==WM_MOUSEWHEEL||m==WM_KEYDOWN||m==WM_SYSKEYDOWN)return SendMessageW(a->m_hwnd,m,w,l);
            if(m==WM_DROPFILES)return SendMessageW(a->m_hwnd,m,w,l);
        }
        return DefWindowProcW(h,m,w,l);
    }

    static std::wstring NvofEnvLower(const wchar_t* name,const wchar_t* fallback){
        wchar_t b[64]{};DWORD n=GetEnvironmentVariableW(name,b,static_cast<DWORD>(std::size(b)));
        std::wstring v=(n&&n<std::size(b))?std::wstring(b,n):std::wstring(fallback);
        std::transform(v.begin(),v.end(),v.begin(),::towlower);return v;
    }
    UINT CurrentNvofPerfMenuId()const{
        const std::wstring v=NvofEnvLower(L"DMP_NVOF_PERF",L"medium");
        if(v==L"slow")return IDM_NVOF_PERF_SLOW;if(v==L"fast")return IDM_NVOF_PERF_FAST;return IDM_NVOF_PERF_MEDIUM;
    }
    UINT CurrentNvofGridMenuId()const{
        const std::wstring v=NvofEnvLower(L"DMP_NVOF_GRID",L"auto");
        if(v==L"1"||v==L"1x1")return IDM_NVOF_GRID_1;if(v==L"2"||v==L"2x2")return IDM_NVOF_GRID_2;if(v==L"4"||v==L"4x4")return IDM_NVOF_GRID_4;return IDM_NVOF_GRID_AUTO;
    }
    void UpdateNvofMenuChecks(){
        if(m_nvofPerfMenu)CheckMenuRadioItem(m_nvofPerfMenu,IDM_NVOF_PERF_SLOW,IDM_NVOF_PERF_FAST,CurrentNvofPerfMenuId(),MF_BYCOMMAND);
        if(m_nvofGridMenu)CheckMenuRadioItem(m_nvofGridMenu,IDM_NVOF_GRID_AUTO,IDM_NVOF_GRID_4,CurrentNvofGridMenuId(),MF_BYCOMMAND);
        if(m_hwnd)DrawMenuBar(m_hwnd);
    }
    void ReloadForNvofChange(){
        if(m_loaded&&!m_path.empty()){std::wstring p=m_path;double keep=Position();bool wasPlaying=m_playing;if(Load(p))RequestSeek(keep,wasPlaying);}
    }
    void SetNvofPerf(const wchar_t* value){
        const std::wstring next=value?value:L"medium";if(NvofEnvLower(L"DMP_NVOF_PERF",L"medium")==next)return;
        _wputenv_s(L"DMP_NVOF_PERF",next.c_str());LOG("[NVOF UI] preset="<<std::string(next.begin(),next.end()));UpdateNvofMenuChecks();ReloadForNvofChange();
    }
    void SetNvofGrid(const wchar_t* value){
        const std::wstring next=value?value:L"auto";if(NvofEnvLower(L"DMP_NVOF_GRID",L"auto")==next)return;
        _wputenv_s(L"DMP_NVOF_GRID",next.c_str());LOG("[NVOF UI] gridPolicy="<<std::string(next.begin(),next.end()));UpdateNvofMenuChecks();ReloadForNvofChange();
    }
    UINT CurrentDepthSourceMenuId()const{
        switch(m_opt.depthSource){case D3D12Renderer::DepthSource::Flat:return IDM_DEPTH_SOURCE_FLAT;case D3D12Renderer::DepthSource::AISynthetic:return IDM_DEPTH_SOURCE_AI;default:return IDM_DEPTH_SOURCE_LEGACY;}
    }
    void UpdateDepthSourceMenuChecks(){
        if(m_depthSourceMenu)CheckMenuRadioItem(m_depthSourceMenu,IDM_DEPTH_SOURCE_LEGACY,IDM_DEPTH_SOURCE_AI,CurrentDepthSourceMenuId(),MF_BYCOMMAND);
        if(m_hwnd)DrawMenuBar(m_hwnd);
    }
    void SetDepthSource(D3D12Renderer::DepthSource source){
        if(m_opt.depthSource==source)return;
        m_opt.depthSource=source;
        if(source==D3D12Renderer::DepthSource::Legacy && m_guides.GetDepthMode()!=TemporalGuideGenerator::DepthMode::Estimated){m_guides.SetDepthMode(TemporalGuideGenerator::DepthMode::Estimated);m_guideReset=true;}
        if(m_renderer)m_renderer->SetDepthSource(source);
        m_dlssReset=true;
        const char* sourceName=source==D3D12Renderer::DepthSource::Flat?"FLAT":(source==D3D12Renderer::DepthSource::AISynthetic?"AI_SYNTHETIC":"LEGACY");
        LOG("[NGX Depth UI] requested="<<sourceName);
        UpdateDepthSourceMenuChecks();UpdateTitle();
    }
    void CycleDepthSource(){
        if(m_opt.depthSource==D3D12Renderer::DepthSource::Legacy)SetDepthSource(D3D12Renderer::DepthSource::Flat);
        else if(m_opt.depthSource==D3D12Renderer::DepthSource::Flat)SetDepthSource(D3D12Renderer::DepthSource::AISynthetic);
        else SetDepthSource(D3D12Renderer::DepthSource::Legacy);
    }
    HMENU CreateMenuBar() {
        HMENU audioTracks=CreatePopupMenu(),subtitleTracks=CreatePopupMenu();
        HMENU bar=CreateMenu(),file=CreatePopupMenu(),play=CreatePopupMenu(),video=CreatePopupMenu(),dlss=CreatePopupMenu(),quality=CreatePopupMenu(),nvof=CreatePopupMenu(),nvofPerf=CreatePopupMenu(),nvofGrid=CreatePopupMenu(),depthSource=CreatePopupMenu(),frameGen=CreatePopupMenu();m_nvofPerfMenu=nvofPerf;m_nvofGridMenu=nvofGrid;m_depthSourceMenu=depthSource;
        auto add=[&](HMENU m,UINT id,const wchar_t* key){std::wstring s=T(key);AppendMenuW(m,MF_STRING,id,s.c_str());};
        add(file,IDM_OPEN,L"menu.open"); AppendMenuW(file,MF_SEPARATOR,0,nullptr); add(file,IDM_EXIT,L"menu.exit");
        add(play,IDM_PLAY,L"menu.playpause"); add(play,IDM_STOP,L"menu.stop"); add(play,IDM_BACK10,L"menu.back10"); add(play,IDM_FWD10,L"menu.forward10"); add(play,IDM_MUTE,L"menu.mute");
        if(m_mediaTracks.AudioTracks().empty())AppendMenuW(audioTracks,MF_STRING|MF_GRAYED,0,L"(No audio tracks)");
        else for(size_t i=0;i<m_mediaTracks.AudioTracks().size()&&i<90;++i){const auto&t=m_mediaTracks.AudioTracks()[i];const auto label=MediaTrackCatalog::MenuLabel(t);AppendMenuW(audioTracks,MF_STRING|(t.streamIndex==m_selectedAudioStream?MF_CHECKED:MF_UNCHECKED),IDM_AUDIO_TRACK_BASE+UINT(i),label.c_str());}
        AppendMenuW(subtitleTracks,MF_STRING|(m_selectedSubtitleStream<0?MF_CHECKED:MF_UNCHECKED),IDM_SUBTITLE_OFF,L"Off");
        for(size_t i=0;i<m_mediaTracks.SubtitleTracks().size()&&i<90;++i){const auto&t=m_mediaTracks.SubtitleTracks()[i];const auto label=MediaTrackCatalog::MenuLabel(t);UINT flags=MF_STRING|(t.streamIndex==m_selectedSubtitleStream?MF_CHECKED:MF_UNCHECKED);if(!t.textSubtitleSupported)flags|=MF_GRAYED;AppendMenuW(subtitleTracks,flags,IDM_SUBTITLE_TRACK_BASE+UINT(i),label.c_str());}
        AppendMenuW(play,MF_POPUP,reinterpret_cast<UINT_PTR>(audioTracks),L"Audio Track");
        AppendMenuW(play,MF_POPUP,reinterpret_cast<UINT_PTR>(subtitleTracks),L"Subtitles");
        AppendMenuW(play,MF_STRING|(m_vsyncEnabled?MF_CHECKED:MF_UNCHECKED),IDM_VSYNC,L"V-Sync");
        AppendMenuW(frameGen,MF_STRING|(!m_frameGenerationEnabled?MF_CHECKED:MF_UNCHECKED),IDM_FRAMEGEN_OFF,L"Off");
        const bool fgMenuAvailable=!m_renderer||m_renderer->FrameGenerationAvailable();
        const uint32_t fgRuntimeMax=(m_renderer&&m_renderer->FrameGenerationAvailable())?m_renderer->FrameGenerationMaxMultiplier():6u;
        const uint32_t fgMenuMax=m_vsyncEnabled?std::min(fgRuntimeMax,VSyncFrameGenerationCap()):fgRuntimeMax;
        auto addFg=[&](UINT id,uint32_t mult,const wchar_t* label){UINT flags=MF_STRING;if(m_frameGenerationEnabled&&m_frameGenerationMultiplier==mult)flags|=MF_CHECKED;if(!fgMenuAvailable||(m_renderer&&mult>fgMenuMax))flags|=MF_GRAYED;AppendMenuW(frameGen,flags,id,label);};
        addFg(IDM_FRAMEGEN_2X,2,L"DLSS-G 2x");addFg(IDM_FRAMEGEN_3X,3,L"DLSS-G 3x");addFg(IDM_FRAMEGEN_4X,4,L"DLSS-G 4x");addFg(IDM_FRAMEGEN_5X,5,L"DLSS-G 5x");addFg(IDM_FRAMEGEN_6X,6,L"DLSS-G 6x");
        AppendMenuW(play,MF_POPUP,reinterpret_cast<UINT_PTR>(frameGen),L"Frame Generation");        add(video,IDM_ASPECT_FIT,L"menu.aspectfit"); add(video,IDM_ASPECT_FILL,L"menu.aspectfill"); add(video,IDM_VIDEO_ADJUSTMENTS,L"menu.adjustments"); AppendMenuW(video,MF_SEPARATOR,0,nullptr);
        add(video,IDM_VIEW_FINAL,L"menu.final"); add(video,IDM_VIEW_INPUT,L"menu.input"); add(video,IDM_VIEW_MV,L"menu.mv"); add(video,IDM_VIEW_DEPTH,L"menu.depth"); AppendMenuW(video,MF_STRING,IDM_VIEW_AI_DEPTH,L"AI Depth"); AppendMenuW(video,MF_STRING,IDM_VIEW_AI_HW_DEPTH,L"AI HW Depth"); AppendMenuW(video,MF_STRING,IDM_VIEW_MASK,L"Temporal Mask"); AppendMenuW(video,MF_STRING|(m_temporalMaskBypass?MF_CHECKED:MF_UNCHECKED),IDM_TEMPORAL_MASK_BYPASS,L"Temporal Mask: Bypass (Full Frame)"); AppendMenuW(video,MF_SEPARATOR,0,nullptr); add(video,IDM_FULLSCREEN,L"menu.fullscreen");
        add(quality,IDM_QUALITY_AUTO,L"menu.quality_auto"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_QUALITY,L"Quality"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_BALANCED,L"Balanced"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_PERFORMANCE,L"Performance"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_ULTRAPERF,L"Ultra Performance"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_DLAA,L"DLAA");
        AppendMenuW(nvofPerf,MF_STRING,IDM_NVOF_PERF_SLOW,L"Slow");AppendMenuW(nvofPerf,MF_STRING,IDM_NVOF_PERF_MEDIUM,L"Medium");AppendMenuW(nvofPerf,MF_STRING,IDM_NVOF_PERF_FAST,L"Fast");
        AppendMenuW(nvofGrid,MF_STRING,IDM_NVOF_GRID_AUTO,L"Auto");AppendMenuW(nvofGrid,MF_STRING,IDM_NVOF_GRID_1,L"1x1");AppendMenuW(nvofGrid,MF_STRING,IDM_NVOF_GRID_2,L"2x2");AppendMenuW(nvofGrid,MF_STRING,IDM_NVOF_GRID_4,L"4x4");
        AppendMenuW(nvof,MF_POPUP,reinterpret_cast<UINT_PTR>(nvofPerf),L"Preset");AppendMenuW(nvof,MF_POPUP,reinterpret_cast<UINT_PTR>(nvofGrid),L"Grid");
        add(dlss,IDM_DLSS,L"menu.dlss_toggle"); add(dlss,IDM_REHOOK,L"menu.rehook"); AppendMenuW(depthSource,MF_STRING,IDM_DEPTH_SOURCE_LEGACY,L"Legacy Estimated");AppendMenuW(depthSource,MF_STRING,IDM_DEPTH_SOURCE_FLAT,L"Flat 0.75");AppendMenuW(depthSource,MF_STRING,IDM_DEPTH_SOURCE_AI,L"AI Synthetic");
        AppendMenuW(dlss,MF_POPUP,reinterpret_cast<UINT_PTR>(depthSource),L"Depth Source (NGX)"); std::wstring qualityName=T(L"menu.quality"); AppendMenuW(dlss,MF_POPUP,reinterpret_cast<UINT_PTR>(quality),qualityName.c_str()); AppendMenuW(dlss,MF_POPUP,reinterpret_cast<UINT_PTR>(nvof),L"Optical Flow (NVOF)");
        AppendMenuW(dlss,MF_STRING|(m_splitScreen?MF_CHECKED:MF_UNCHECKED),IDM_SPLIT_SCREEN,L"Split Screen: DLSS OFF | ON");
        AppendMenuW(dlss,MF_STRING,IDM_SPLIT_RESET,L"Split Divider: Reset 50/50");
        m_languageCodes.clear();
        std::wstring sFile=T(L"menu.file"),sPlay=T(L"menu.playback"),sVideo=T(L"menu.video"),sDlss=T(L"menu.dlss");
        AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(file),sFile.c_str());
        AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(play),sPlay.c_str());
        AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(video),sVideo.c_str());
        AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(dlss),sDlss.c_str());
        UpdateNvofMenuChecks();
        UpdateDepthSourceMenuChecks();
        return bar;
    }

    void ApplyLanguage(const std::wstring& code) {
        const bool reopenAdjust=(m_adjustWnd!=nullptr);if(m_adjustWnd)DestroyWindow(m_adjustWnd);
        m_loc.SetLanguage(code,true); HMENU old=GetMenu(m_hwnd),fresh=CreateMenuBar(); SetMenu(m_hwnd,fresh); DrawMenuBar(m_hwnd); if(old)DestroyMenu(old); UpdateTitle(); InvalidateRect(m_hwnd,nullptr,TRUE);
        if(reopenAdjust)ShowAdjustments();
    }

    void StartAIDepth(){
        m_aiDepthLatest={};m_aiDepthSequence=0;m_aiDepthTemporalWorker.Reset();
        const auto base=SettingsPath().parent_path();
        const auto engine=base/L"models"/L"depth_anything_v2_small_fp16_dynamic_518.trt";
        const auto cache=base/L"cache"/L"depth_anything_v2_small_fp16_dynamic_518.cache";
        std::error_code ec;std::filesystem::create_directories(cache.parent_path(),ec);
        auto worker=std::make_unique<AIDepthWorker>();
        if(!worker->Start(engine.wstring(),cache.wstring(),m_decoder.Width(),m_decoder.Height())){LOG("[AI Depth] disabled: "<<worker->LastError());return;}
        m_aiDepthWorker=std::move(worker);LOG("[AI Depth] isolated TensorRT-RTX sidecar armed; debug-only, not connected to NGX depth.");LOG("[AI Temporal] Step 04B active: AI Depth debug is reprojected to the current frame and temporally stabilized; still NOT connected to NGX depth.");LOG("[AI Temporal Async] Step 04B-2 active: temporal depth runs on a dedicated CPU worker; render thread never waits for it.");LOG("[AI HWDepth] Step 04C active: robust stabilized relative nearness is expanded to full-resolution conventional D3D depth (0 near, 1 far); DEBUG ONLY, NOT connected to NGX.");LOG("[NGX Depth] Step 04D active: live depth A/B = Legacy / Flat 0.75 / AI Synthetic; default Legacy; AI falls back to Legacy until valid.");
    }
    bool Load(const std::wstring& path) {
        const bool sameMedia=!m_path.empty()&&_wcsicmp(m_path.c_str(),path.c_str())==0;
        const int keepAudio=sameMedia?m_selectedAudioStream:-1,keepSubtitle=sameMedia?m_selectedSubtitleStream:-1;
        if(path.empty())return false;
        Unload();
        if(!m_decoder.Open(path)){std::wstring e=T(L"error.decode"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);return false;}
        m_mediaTracks.Probe(path);
        m_selectedAudioStream=(sameMedia&&m_mediaTracks.FindAudio(keepAudio))?keepAudio:m_mediaTracks.DefaultAudioStream();
        m_selectedSubtitleStream=(sameMedia&&m_mediaTracks.FindSubtitle(keepSubtitle)&&m_mediaTracks.FindSubtitle(keepSubtitle)->textSubtitleSupported)?keepSubtitle:-1;
        if(m_selectedSubtitleStream>=0)m_subtitles.LoadAsync(path,m_selectedSubtitleStream);        m_dar=m_decoder.DisplayAspectRatio(); if(!std::isfinite(m_dar)||m_dar<0.2)m_dar=double(m_decoder.Width())/std::max(1u,m_decoder.Height());
        const auto outputBox=m_opt.outputExplicit?std::make_pair(m_opt.maxW,m_opt.maxH):MonitorNativeOutputBox(m_hwnd);
        const uint32_t outputBoxW=outputBox.first,outputBoxH=outputBox.second;
        auto [ow,oh]=OutputForAspect(m_dar,outputBoxW,outputBoxH);
        LOG("[Output] "<<(m_opt.outputExplicit?"explicit":"monitor-native auto")<<" box="<<outputBoxW<<"x"<<outputBoxH<<" dar="<<m_dar<<" target="<<ow<<"x"<<oh);
        m_activeQuality = m_opt.qualityExplicit ? m_opt.quality : AutoQuality(m_decoder.NativeWidth(),m_decoder.NativeHeight(),ow,oh,m_decoder.FrameRate());
        LOG("DLSS quality policy: " << (m_opt.qualityExplicit?"explicit":"auto-realtime") << " -> " << QualityNameA(m_activeQuality));
        const auto [decodeW,decodeH]=RecommendedDecodeSize(m_decoder.NativeWidth(),m_decoder.NativeHeight(),ow,oh,m_activeQuality);
        if((decodeW!=m_decoder.Width()||decodeH!=m_decoder.Height()) && !m_decoder.SetDecodeSize(decodeW,decodeH))
            LOG("Realtime decode scaling unavailable; continuing at native decoder resolution.");
        const auto [analysisGuideW,analysisGuideH]=TemporalGuideGenerator::AnalysisGrid(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate());
        uint32_t guideW=analysisGuideW,guideH=analysisGuideH;
        const bool nvofRuntime=OpticalFlowEngine::RuntimeAvailable();
        if(nvofRuntime){
            // Step 02A target: a 2x2 guide grid (960x540 for 1080p) instead of ~160x90.
            guideW=std::max(analysisGuideW,(m_decoder.Width()+1u)/2u);
            guideH=std::max(analysisGuideH,(m_decoder.Height()+1u)/2u);
            LOG("[NVOF] Driver runtime detected; guide upload grid="<<guideW<<"x"<<guideH);
        }
        ShowWindow(m_viewport,SW_SHOW); Layout();
        m_renderer=std::make_unique<D3D12Renderer>();
        if(!m_renderer->Initialize(m_renderWnd,m_decoder.Width(),m_decoder.Height(),ow,oh,guideW,guideH,m_activeQuality)){std::wstring e=T(L"error.renderer"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);m_renderer.reset();m_decoder.Close();ShowWindow(m_viewport,SW_HIDE);return false;}
        m_renderer->SetColorSettings(m_colorSettings);m_renderer->SetSplitScreen(m_splitScreen);m_renderer->SetSplitFraction(m_splitFraction);m_renderer->SetVSync(m_vsyncEnabled);m_renderer->SetFrameGeneration(m_frameGenerationEnabled,m_frameGenerationMultiplier);
        m_renderer->SetDepthSource(m_opt.depthSource);if(m_frameGenerationEnabled&&m_renderer->FrameGenerationAvailable()){uint32_t cap=m_renderer->FrameGenerationMaxMultiplier();if(m_vsyncEnabled)cap=std::min(cap,VSyncFrameGenerationCap());if(cap<2u){m_frameGenerationEnabled=false;m_renderer->SetFrameGeneration(false,m_frameGenerationMultiplier);}else{m_frameGenerationMultiplier=std::min(m_frameGenerationMultiplier,cap);m_renderer->SetFrameGeneration(true,m_frameGenerationMultiplier);}}m_lastDisplayedFramesTotal=m_renderer->FrameGenerationDisplayedFramesTotal();m_displayFps=0.0;m_actualDisplayRatio=1.0;
        m_guides.SetOutputGrid(guideW,guideH);
        m_opticalFlow.reset();
        if(nvofRuntime){
            auto of=std::make_unique<OpticalFlowEngine>();
            if(of->Initialize(m_renderer->Device(),m_decoder.Width(),m_decoder.Height(),2u)){
                LOG("[NVOF] Active: hwGrid="<<of->GridSize()<<" output="<<of->GridW()<<"x"<<of->GridH());
                m_opticalFlow=std::move(of);
            }else{
                LOG("[NVOF] Initialization failed; legacy motion fallback remains active.");
            }
        }
        VideoFrame first; if(!m_decoder.ReadNext(first)){std::wstring e=T(L"error.frame"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);Unload();return false;}
        m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;RenderVideoFrame(first,true);m_currentSec=double(first.timestamp100ns)*1e-7;
        m_haveNext=m_decoder.ReadNext(m_next);m_audio.Start(path,m_currentSec,m_selectedAudioStream);m_audio.SetVolume(m_muted?0.0f:m_volume);m_playing=true;m_playStartSec=m_currentSec;m_playStart=Clock::now();m_loaded=true;m_path=path;m_droppedFrames=0;m_uiTick=0;m_seekPending=false;m_seeking=false;m_fpsWindowStart=Clock::now();m_fpsWindowFrames=0;m_submitFps=0.0;m_lastFrameProcessMs=0.0;
        RebuildMenuBar();
        UpdateTitle();Layout();InvalidateRect(m_hwnd,nullptr,TRUE);return true;
    }

    void Unload() {
        m_subtitles.Clear();m_mediaTracks.Clear();m_selectedAudioStream=-1;m_selectedSubtitleStream=-1;SetSubtitleText(L"");
        m_seekPending=false;m_seeking=false;m_audio.Stop(); m_aiDepthWorker.reset();m_aiDepthLatest={};m_aiDepthSequence=0;m_aiDepthTemporalWorker.Reset(); m_opticalFlow.reset(); if(m_renderer){m_renderer->WaitGPU();m_renderer.reset();} m_decoder.Close();m_guides.Reset();m_haveNext=false;m_next=VideoFrame{};m_loaded=false;m_playing=false;m_currentSec=0;m_lastRenderedTs=-1;m_path.clear();
        if(m_viewport)ShowWindow(m_viewport,SW_HIDE); UpdateTitle(); if(m_hwnd)InvalidateRect(m_hwnd,nullptr,TRUE);
    }

    bool RenderVideoFrame(const VideoFrame& f,bool resetGuide) {
        // Step 04G-4: subtitle lookup is driven by Position() in Tick()/paused PresentCurrent, not synthetic decoder CFR timestamps.
        if(!m_renderer)return false; GuideFrame g;
        // Let NGX/ReShade/RenoDX finish their initial feature capture and delayed
        // recreate before starting the independent CUDA/TensorRT sidecar.
        if(!m_aiDepthWorker && m_renderer->FramesPresented()>=90)StartAIDepth();

        // AI depth is completely asynchronous. Submit() only maintains one waiting
        // frame; if playback outruns inference, the newest frame replaces it.
        bool newAIDepth=false;
        if(m_aiDepthWorker){
            // resetGuide is also asserted after ordinary playback frame drops. Do not
            // advance the AI IPC generation for those resets: TensorRT is asynchronous,
            // so doing that can invalidate every in-flight result and leave AI Depth black.
            // A real decoder discontinuity still resets the AI history/debug texture.
            if(f.discontinuity){m_aiDepthWorker->Reset();m_aiDepthLatest={};m_aiDepthSequence=0;m_aiDepthTemporalWorker.Reset();m_renderer->ResetAIDepthDebug();}
            (void)m_aiDepthWorker->Submit(f.bgra.data(),f.bgra.size(),m_decoder.Width(),m_decoder.Height(),size_t(m_decoder.Width())*4u,f.timestamp100ns);
            AIDepthFrame latest;if(m_aiDepthWorker->GetLatest(m_aiDepthSequence,latest)){m_aiDepthLatest=std::move(latest);m_aiDepthSequence=m_aiDepthLatest.sequence;newAIDepth=true;}
        }

        OpticalFlowFrame ofFrame; ExternalMotionField external{}; const ExternalMotionField* externalPtr=nullptr;
        if(m_opticalFlow){
            if(m_opticalFlow->Generate(f.bgra.data(),f.bgra.size(),resetGuide,ofFrame) && ofFrame.valid){
                external.motionXY=ofFrame.motionXY.data();external.gridW=ofFrame.gridW;external.gridH=ofFrame.gridH;
                external.sourceW=ofFrame.sourceW;external.sourceH=ofFrame.sourceH;external.valid=true;externalPtr=&external;
            }
        }
        // Step 04E-1: snapshot the latest completed temporal AI depth before guide
        // generation. This is non-blocking. It steers only the mask depth edges/blur;
        // Guide B and the selected NGX Depth Source remain independent.
        ExternalDepthField aiMaskDepth{};const ExternalDepthField* aiMaskDepthPtr=nullptr;
        const auto aiMaskSnapshot=m_aiDepthTemporalWorker.Latest();
        if(aiMaskSnapshot&&aiMaskSnapshot->valid&&aiMaskSnapshot->preview01.size()==size_t(AIDepthTemporalStabilizer::DepthW)*AIDepthTemporalStabilizer::DepthH){
            const double ageMs=(f.timestamp100ns>=aiMaskSnapshot->currentTimestamp100ns)?double(f.timestamp100ns-aiMaskSnapshot->currentTimestamp100ns)*1.0e-4:0.0;
            const double frameMs=1000.0/std::max(1.0,m_decoder.FrameRate());
            const double ageFrames=frameMs>0.0?ageMs/frameMs:0.0;
            aiMaskDepth.depth01=aiMaskSnapshot->preview01.data();aiMaskDepth.width=AIDepthTemporalStabilizer::DepthW;aiMaskDepth.height=AIDepthTemporalStabilizer::DepthH;
            aiMaskDepth.ageMs=float(ageMs);aiMaskDepth.ageFrames=float(ageFrames);aiMaskDepth.valid=ageFrames<=3.0;
            aiMaskDepthPtr=&aiMaskDepth;
        }
        const auto guidesStageStart=Clock::now();
        const bool guidesOk=m_guides.Generate(f.bgra.data(),m_decoder.Width(),m_decoder.Height(),m_renderer->DLSSInputW(),m_renderer->DLSSInputH(),m_decoder.FrameRate(),resetGuide,g,externalPtr,aiMaskDepthPtr);
        m_lastGuidesMs=std::chrono::duration<double,std::milli>(Clock::now()-guidesStageStart).count();
        m_lastGuideHardwareFastPath=g.usedHardwareFastPath;
        m_lastGuideLegacyFlow=g.legacyFlowEvaluated;
        static uint64_t maskDepthDiag=0;if((++maskDepthDiag%120u)==0u)LOG("[Mask Depth] effective="<<(g.maskUsedAIDepth?"AI":"LEGACY")<<" aiAvailable="<<(g.maskAIDepthAvailable?1:0)<<" ageMs="<<g.maskDepthAgeMs<<" ageFrames="<<g.maskDepthAgeFrames);
        if(!guidesOk)return false;
        // Step 04E-2: a detected shot cut is a true temporal discontinuity even when
        // the decoder did not flag one. Purge asynchronous AI-depth history too, so an
        // old-shot depth map cannot steer the first masks of the new shot.
        const bool sceneCutReset=g.hardCut;
        if(sceneCutReset){
            m_aiDepthTemporalWorker.Reset();
            if(m_renderer)m_renderer->ResetAIDepthDebug();
            LOG("[Scene Cut] hard reset: residualMean="<<g.sceneCutResidualMean<<" residualMedian="<<g.sceneCutResidualMedian<<" strong="<<g.sceneCutStrongFraction<<" directStrong="<<g.sceneCutDirectStrongFraction<<" externalNVOF="<<(externalPtr?1:0)<<" ts="<<f.timestamp100ns);
        }

        static uint64_t temporalDiagSeq=0,nvofNotConsumed=0,nvofMissing=0;++temporalDiagSeq;
        if(g.hardCut)LOG("[Temporal] hard cut: cpuCost="<<g.globalMatchCost<<" externalNVOF="<<(externalPtr?1:0));
        if(m_opticalFlow && !resetGuide && !externalPtr){
            ++nvofMissing;
            if(nvofMissing<=8u || (nvofMissing%60u)==0u)
                LOG("[Temporal] NVOFA frame missing unexpectedly: count="<<nvofMissing<<" ts="<<f.timestamp100ns);
        }
        if(externalPtr && !g.usedExternalMotion && !resetGuide){
            ++nvofNotConsumed;
            if(nvofNotConsumed<=8u || (nvofNotConsumed%60u)==0u)
                LOG("[Temporal] NVOFA supplied but not consumed: count="<<nvofNotConsumed<<" history="<<(g.hasHistory?1:0)<<" cpuCost="<<g.globalMatchCost<<" ts="<<f.timestamp100ns);
        }
        if((temporalDiagSeq%120u)==0u)LOG("[Temporal] guide health: externalNVOF="<<(g.usedExternalMotion?1:0)<<" history="<<(g.hasHistory?1:0)<<" cpuCost="<<g.globalMatchCost<<" dropped="<<m_droppedFrames<<" nvofMissing="<<nvofMissing<<" nvofNotConsumed="<<nvofNotConsumed);
        float ms=float(1000.0/std::max(1.0,m_decoder.FrameRate()));
        if(m_lastRenderedTs>=0 && f.timestamp100ns>m_lastRenderedTs){double d=double(f.timestamp100ns-m_lastRenderedTs)*1e-4;if(d>0.1&&d<500.0)ms=float(d);}
        bool r=m_dlssReset||resetGuide||!g.hasHistory;
        // Step 04B-2: temporal AI depth is CPU-heavy, so the render thread only
        // enqueues immutable NVOFA/measurement snapshots and consumes the latest
        // completed result. It never waits for reprojection/affine/blend/percentiles.
        AIDepthMotionView aiMotion{};const AIDepthMotionView* aiMotionPtr=nullptr;
        if(ofFrame.valid&&!ofFrame.motionXY.empty()){
            aiMotion.motionXY=ofFrame.motionXY.data();aiMotion.countFloats=ofFrame.motionXY.size();
            aiMotion.gridW=ofFrame.gridW;aiMotion.gridH=ofFrame.gridH;aiMotion.sourceW=ofFrame.sourceW;aiMotion.sourceH=ofFrame.sourceH;aiMotion.valid=true;aiMotionPtr=&aiMotion;
        }
        AIDepthMeasurementView aiMeasurement{};const AIDepthMeasurementView* aiMeasurementPtr=nullptr;
        if(newAIDepth&&!m_aiDepthLatest.rawDepth.empty()){
            aiMeasurement.rawDepth=m_aiDepthLatest.rawDepth.data();aiMeasurement.count=m_aiDepthLatest.rawDepth.size();
            aiMeasurement.width=m_aiDepthLatest.width;aiMeasurement.height=m_aiDepthLatest.height;aiMeasurement.timestamp100ns=m_aiDepthLatest.timestamp100ns;
            aiMeasurement.sequence=m_aiDepthLatest.sequence;aiMeasurement.percentile02=m_aiDepthLatest.percentile02;aiMeasurement.percentile98=m_aiDepthLatest.percentile98;aiMeasurementPtr=&aiMeasurement;
        }
        (void)m_aiDepthTemporalWorker.Submit(f.timestamp100ns,m_lastRenderedTs,m_decoder.Width(),m_decoder.Height(),aiMotionPtr,aiMeasurementPtr);
        const auto aiTemporal=m_aiDepthTemporalWorker.Latest();
        const bool aiTemporalValid=aiTemporal&&aiTemporal->valid;
        static uint64_t aiTemporalDiag=0;++aiTemporalDiag;
        if(aiTemporalValid&&(aiTemporalDiag%120u)==0u){
            const auto stats=m_aiDepthTemporalWorker.GetStats();
            const double ageMs=(f.timestamp100ns>=aiTemporal->currentTimestamp100ns)?double(f.timestamp100ns-aiTemporal->currentTimestamp100ns)*1.0e-4:0.0;
            LOG("[AI Temporal Async] health: currentTs="<<f.timestamp100ns<<" stableTs="<<aiTemporal->currentTimestamp100ns<<" ageMs="<<ageMs<<" norm="<<aiTemporal->normalizedLo<<".."<<aiTemporal->normalizedHi<<" history="<<aiTemporal->historyCoverage<<" workerMs="<<stats.emaProcessMs<<" queue="<<stats.queueDepth<<" queueResets="<<stats.queueResets);
        }
        if(m_temporalMaskBypass&&!g.guideGridRGBA32F.empty()){
            for(size_t i=3;i<g.guideGridRGBA32F.size();i+=4u)g.guideGridRGBA32F[i]=0.0f;
        }
        const float* aiPreview=(aiTemporalValid&&!aiTemporal->preview01.empty())?aiTemporal->preview01.data():nullptr;
        const size_t aiBytes=aiPreview?aiTemporal->preview01.size()*sizeof(float):0;
        const auto rendererStageStart=Clock::now();
        bool ok=m_renderer->RenderFrame(f.bgra.data(),f.bgra.size(),g.guideGridRGBA32F.data(),g.guideGridRGBA32F.size()*sizeof(float),g.gridW,g.gridH,r,ms,aiPreview,aiBytes,m_aiDepthLatest.width,m_aiDepthLatest.height);
        m_lastRendererMs=std::chrono::duration<double,std::milli>(Clock::now()-rendererStageStart).count();
        m_lastRenderedTs=f.timestamp100ns;m_lastGlobalX=g.globalMotionX;m_lastGlobalY=g.globalMotionY;return ok;
    }

    static std::pair<uint32_t,uint32_t> RecommendedDecodeSize(uint32_t nw,uint32_t nh,uint32_t ow,uint32_t oh,NVSDK_NGX_PerfQuality_Value q) {
        if(!nw||!nh||!ow||!oh)return{nw,nh};
        if(q==NVSDK_NGX_PerfQuality_Value_DLAA){
            // Step 04B-5: DLAA decode is bounded by the output target. DLAA renders at
            // output resolution, so decoding a larger 4K source only to downsize it before
            // NGX wastes decoder/NVOFA/readback/guide bandwidth. Never upscale decode here.
            const double fit=std::min(1.0,std::min(double(ow)/double(nw),double(oh)/double(nh)));
            if(fit>=0.999999)return{nw,nh};
            const uint32_t dw=std::max(2u,uint32_t(std::floor(double(nw)*fit))&~1u);
            const uint32_t dh=std::max(2u,uint32_t(std::floor(double(nh)*fit))&~1u);
            LOG("[DLAA Decode] native="<<nw<<"x"<<nh<<" output="<<ow<<"x"<<oh<<" selected="<<dw<<"x"<<dh);
            return{dw,dh};
        }
        double scale=2.0/3.0;
        if(q==NVSDK_NGX_PerfQuality_Value_Balanced)scale=0.58;
        else if(q==NVSDK_NGX_PerfQuality_Value_MaxPerf)scale=0.50;
        else if(q==NVSDK_NGX_PerfQuality_Value_UltraPerformance)scale=1.0/3.0;
        uint32_t tw=std::max(2u,uint32_t(std::lround(double(ow)*scale))&~1u);
        uint32_t th=std::max(2u,uint32_t(std::lround(double(oh)*scale))&~1u);
        // Never decode-upscale a smaller movie just to feed DLSS. The renderer/NGX
        // policy will preserve the genuine reconstruction distance for low-res sources.
        if(uint64_t(nw)*nh<=uint64_t(tw)*th)return{nw,nh};
        return{tw,th};
    }

    static NVSDK_NGX_PerfQuality_Value AutoQuality(uint32_t sw,uint32_t sh,uint32_t ow,uint32_t oh,double fps) {
        if(!sw||!sh||!ow||!oh) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        const double scale=std::sqrt((double(sw)*double(sh))/(double(ow)*double(oh)));
        // Realtime policy: when the movie already matches the output resolution, DLAA
        // needlessly evaluates DLSS at full output resolution.  Auto instead performs a
        // genuine DLSS upscale.  4K high-frame-rate video starts at Balanced; otherwise
        // Quality. Users can still explicitly select DLAA from the DLSS menu.
        if(scale>=0.90) {
            const uint64_t outPixels=uint64_t(ow)*uint64_t(oh);
            if(outPixels>=uint64_t(3840)*2160 && fps>=45.0) return NVSDK_NGX_PerfQuality_Value_Balanced;
            return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        }
        struct C{double s;NVSDK_NGX_PerfQuality_Value q;};
        const C cands[]={{2.0/3.0,NVSDK_NGX_PerfQuality_Value_MaxQuality},{0.58,NVSDK_NGX_PerfQuality_Value_Balanced},{0.50,NVSDK_NGX_PerfQuality_Value_MaxPerf},{1.0/3.0,NVSDK_NGX_PerfQuality_Value_UltraPerformance}};
        double best=1e9;NVSDK_NGX_PerfQuality_Value q=NVSDK_NGX_PerfQuality_Value_MaxQuality;
        for(const auto& c:cands){double e=std::abs(std::log(std::max(scale,0.05)/c.s));if(e<best){best=e;q=c.q;}}
        return q;
    }
    static const wchar_t* QualityNameW(NVSDK_NGX_PerfQuality_Value q){switch(q){case NVSDK_NGX_PerfQuality_Value_MaxPerf:return L"Performance";case NVSDK_NGX_PerfQuality_Value_Balanced:return L"Balanced";case NVSDK_NGX_PerfQuality_Value_UltraPerformance:return L"UltraPerf";case NVSDK_NGX_PerfQuality_Value_DLAA:return L"DLAA";default:return L"Quality";}}
    static const char* QualityNameA(NVSDK_NGX_PerfQuality_Value q){switch(q){case NVSDK_NGX_PerfQuality_Value_MaxPerf:return "Performance";case NVSDK_NGX_PerfQuality_Value_Balanced:return "Balanced";case NVSDK_NGX_PerfQuality_Value_UltraPerformance:return "UltraPerf";case NVSDK_NGX_PerfQuality_Value_DLAA:return "DLAA";default:return "Quality";}}

    static std::pair<uint32_t,uint32_t> MonitorNativeOutputBox(HWND hwnd) {
        // Query the physical display mode, not DPI-scaled logical monitor coordinates.
        // The target is chosen once when a video is loaded, so resizing the player does
        // not continuously recreate NGX/resources.
        HMONITOR mon=MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi{};mi.cbSize=sizeof(mi);
        if(mon&&GetMonitorInfoW(mon,reinterpret_cast<MONITORINFO*>(&mi))){
            DEVMODEW dm{};dm.dmSize=sizeof(dm);
            if(EnumDisplaySettingsW(mi.szDevice,ENUM_CURRENT_SETTINGS,&dm)&&dm.dmPelsWidth>=64&&dm.dmPelsHeight>=64)
                return{uint32_t(dm.dmPelsWidth),uint32_t(dm.dmPelsHeight)};
            const LONG rw=mi.rcMonitor.right-mi.rcMonitor.left,rh=mi.rcMonitor.bottom-mi.rcMonitor.top;
            if(rw>=64&&rh>=64)return{uint32_t(rw),uint32_t(rh)};
        }
        RECT r{};
        if(hwnd&&GetClientRect(hwnd,&r)){
            const LONG rw=r.right-r.left,rh=r.bottom-r.top;
            if(rw>=64&&rh>=64)return{uint32_t(rw),uint32_t(rh)};
        }
        return{1920u,1080u};
    }

    static std::pair<uint32_t,uint32_t> OutputForAspect(double dar,uint32_t maxW,uint32_t maxH) {
        maxW=std::max(64u,maxW);maxH=std::max(64u,maxH);
        double box=double(maxW)/maxH;uint32_t w,h;if(dar>=box){w=maxW;h=uint32_t(std::lround(double(w)/dar));}else{h=maxH;w=uint32_t(std::lround(double(h)*dar));}
        w=std::max(64u,w&~1u);h=std::max(64u,h&~1u);return{w,h};
    }

    double Position() const {
        if(!m_loaded)return 0;if(!m_playing)return m_currentSec;
        double audio=m_audio.PositionSeconds();
        if(audio>=0.0){double d=m_decoder.DurationSeconds();return d>0?std::clamp(audio,0.0,d):audio;}
        double s=m_playStartSec+std::chrono::duration<double>(Clock::now()-m_playStart).count();double d=m_decoder.DurationSeconds();return d>0?std::clamp(s,0.0,d):std::max(0.0,s);
    }

    double ClampSeek(double sec)const{double dur=m_decoder.DurationSeconds();if(dur>0)return std::clamp(sec,0.0,dur);return std::max(0.0,sec);}

    void RequestSeek(double sec) {
        const bool resume=m_seekPending?m_seekResumePlaying:m_playing; RequestSeek(sec,resume);
    }

    void RequestSeek(double sec,bool resumeAfter) {
        if(!m_loaded)return; sec=ClampSeek(sec);
        if(!m_seekPending) m_currentSec=Position();
        m_pendingSeekSec=sec;m_seekResumePlaying=resumeAfter;m_seekPending=true;m_playing=false;m_audio.Pause(true);m_seekPreview=sec;InvalidateRect(m_hwnd,nullptr,FALSE);
    }

    bool PerformSeek(double sec,bool resumeAfter) {
        if(!m_loaded||m_seeking)return false;m_seeking=true;sec=ClampSeek(sec);LOG("Seek begin target="<<sec<<" resume="<<resumeAfter);
        // Seek is deliberately transactional and performed from Tick(), never from a mouse message.
        // Shut down the audio producer first, wait for GPU work, then restart the video decoder.
        m_audio.Stop(); if(m_renderer)m_renderer->WaitGPU(); m_haveNext=false;m_next=VideoFrame{};
        auto readAt=[&](double target,VideoFrame& frame)->bool{
            if(!m_decoder.SeekSeconds(target))return false;
            if(m_decoder.ReadNext(frame))return true;
            const double dur=m_decoder.DurationSeconds(),fd=1.0/std::max(1.0,m_decoder.FrameRate());
            if(dur>0.0&&target>0.0){const double safe=std::max(0.0,std::min(target,dur-fd*1.5));if(safe<target&&m_decoder.SeekSeconds(safe)&&m_decoder.ReadNext(frame))return true;}
            return false;
        };
        VideoFrame f; bool got=readAt(sec,f);
        if(!got){
            LOG("Seek decoder restart failed; reopening the same file for recovery.");
            m_decoder.Close(); if(m_decoder.Open(m_path))got=readAt(sec,f);
        }
        if(!got){
            LOG("Seek failed without crashing; playback remains paused.");m_playing=false;m_seeking=false;m_currentSec=sec;UpdateTitle();InvalidateRect(m_hwnd,nullptr,FALSE);return false;
        }
        m_guides.Reset();m_guideReset=true;m_dlssReset=true;m_lastRenderedTs=-1;
        if(!RenderVideoFrame(f,true)){LOG("Seek frame render failed.");m_playing=false;m_seeking=false;return false;}
        m_currentSec=double(f.timestamp100ns)*1e-7;m_haveNext=m_decoder.ReadNext(m_next);
        const bool audioOk=m_audio.Start(m_path,m_currentSec,m_selectedAudioStream);if(audioOk){m_audio.SetVolume(m_muted?0.0f:m_volume);m_audio.Pause(!resumeAfter);}else LOG("Seek: no audio stream/output; using steady-clock video pacing.");
        m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=resumeAfter&&m_haveNext;m_guideReset=false;m_dlssReset=false;m_seeking=false;UpdateTitle();InvalidateRect(m_hwnd,nullptr,FALSE);LOG("Seek complete actual="<<m_currentSec);return true;
    }

    void SetPaused(bool pause){if(!m_loaded||m_seeking)return;if(pause==!m_playing)return;if(pause){m_currentSec=Position();m_playing=false;m_audio.Pause(true);}else{if(!m_haveNext&&m_decoder.DurationSeconds()>0){RequestSeek(0,true);return;}m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=true;m_audio.Pause(false);}InvalidateRect(m_hwnd,nullptr,FALSE);}
    void TogglePause(){SetPaused(m_playing);}
    void StopPlayback(){RequestSeek(0,false);}

    void UpdateTitle(){
        if(!m_hwnd)return; if(!m_loaded||!m_renderer){SetWindowTextW(m_hwnd,T(L"app.title").c_str());return;}
        std::wstringstream s;s<<L"DLSS Video Player V11 | source "<<m_decoder.NativeWidth()<<L"x"<<m_decoder.NativeHeight();if(m_decoder.Width()!=m_decoder.NativeWidth()||m_decoder.Height()!=m_decoder.NativeHeight())s<<L" decode "<<m_decoder.Width()<<L"x"<<m_decoder.Height();s<<L" | "<<QualityNameW(m_activeQuality)<<L" | DLSS "<<m_renderer->DLSSInputW()<<L"x"<<m_renderer->DLSSInputH()<<L" -> "<<m_renderer->OutputW()<<L"x"<<m_renderer->OutputH()<<L" | "<<m_decoder.BackendName()<<L" | NGX "<<(m_renderer->DLSSFeatureCreated()?L"CREATE OK":(m_renderer->DLSSAvailable()?L"READY":L"FALLBACK"))<<L" | "<<(m_renderer->DLSSLastEvaluationUsedC()?L"evalC ":L"eval ")<<m_renderer->DLSSEvaluations()<<L" | result 0x"<<std::hex<<uint32_t(m_renderer->DLSSLastResult())<<std::dec;if(m_opticalFlow)s<<L" | NVOF "<<m_opticalFlow->GridSize()<<L"x "<<m_opticalFlow->GridW()<<L"x"<<m_opticalFlow->GridH();if(const auto*at=m_mediaTracks.FindAudio(m_selectedAudioStream))s<<L" | Audio "<<MediaTrackCatalog::ShortLabel(*at);else s<<L" | Audio auto";if(const auto*st=m_mediaTracks.FindSubtitle(m_selectedSubtitleStream)){s<<L" | Subs "<<MediaTrackCatalog::ShortLabel(*st);if(m_subtitles.Loading())s<<L" (loading)";}else s<<L" | Subs Off";SetWindowTextW(m_hwnd,s.str().c_str());
    }

    void Layout(){
        if(!m_hwnd||!m_viewport||!m_renderWnd||!m_controlsWnd)return;
        RECT c{};GetClientRect(m_hwnd,&c);int W=static_cast<int>(std::max<LONG>(1,c.right-c.left)),H=static_cast<int>(std::max<LONG>(1,c.bottom-c.top));
        if(!m_loaded){ShowWindow(m_controlsWnd,SW_HIDE);MoveWindow(m_viewport,0,0,W,H,TRUE);return;}
        ShowWindow(m_viewport,SW_SHOW);
        if(m_fullscreen){
            MoveWindow(m_viewport,0,0,W,H,TRUE);
            if(m_fullscreenAutoHide&&m_fullscreenControlsHidden){ShowWindow(m_controlsWnd,SW_HIDE);if(m_tooltipWnd)SendMessageW(m_tooltipWnd,TTM_POP,0,0);}
            else{MoveWindow(m_controlsWnd,0,std::max(0,H-CONTROL_H),W,CONTROL_H,TRUE);ShowWindow(m_controlsWnd,SW_SHOW);SetWindowPos(m_controlsWnd,HWND_TOP,0,std::max(0,H-CONTROL_H),W,CONTROL_H,SWP_SHOWWINDOW|SWP_NOACTIVATE);}
        }else{
            m_fullscreenControlsHidden=false;
            int areaH=std::max(1,H-CONTROL_H);MoveWindow(m_viewport,0,0,W,areaH,TRUE);MoveWindow(m_controlsWnd,0,areaH,W,CONTROL_H,TRUE);ShowWindow(m_controlsWnd,SW_SHOW);SetWindowPos(m_controlsWnd,HWND_TOP,0,areaH,W,CONTROL_H,SWP_SHOWWINDOW|SWP_NOACTIVATE);
        }
        RECT vc{};GetClientRect(m_viewport,&vc);int areaW=std::max<LONG>(1,vc.right-vc.left),areaH=std::max<LONG>(1,vc.bottom-vc.top);
        double ar=m_dar>0?m_dar:16.0/9.0;double areaAr=double(areaW)/areaH;int rw=0,rh=0;
        if(m_fill){if(areaAr>ar){rw=areaW;rh=int(std::lround(areaW/ar));}else{rh=areaH;rw=int(std::lround(areaH*ar));}}
        else{if(areaAr>ar){rh=areaH;rw=int(std::lround(areaH*ar));}else{rw=areaW;rh=int(std::lround(areaW/ar));}}
        SetWindowPos(m_renderWnd,nullptr,(areaW-rw)/2,(areaH-rh)/2,std::max(1,rw),std::max(1,rh),SWP_NOZORDER|SWP_NOACTIVATE);
        if(m_subtitleWnd&&IsWindow(m_subtitleWnd))SetWindowPos(m_subtitleWnd,HWND_TOP,(W-rw)/2,(areaH-rh)/2,std::max(1,rw),std::max(1,rh),SWP_NOACTIVATE);
        InvalidateRect(m_viewport,nullptr,FALSE);UpdateTooltipRects();InvalidateControls();
    }

    RECT ControlClientRect()const{RECT c{};if(m_controlsWnd)GetClientRect(m_controlsWnd,&c);return c;}
    RECT TimelineRect()const{RECT c=ControlClientRect();return RECT{18,c.bottom-18,std::max<LONG>(19,c.right-18),c.bottom-10};}
    RECT VolumeRect()const{RECT c=ControlClientRect();return RECT{std::max<LONG>(18,c.right-260),c.bottom-66,std::max<LONG>(19,c.right-145),c.bottom-58};}
    RECT MuteRect()const{RECT c=ControlClientRect();return RECT{std::max<LONG>(18,c.right-132),c.bottom-82,std::max<LONG>(19,c.right-70),c.bottom-44};}
    RECT FpsRect()const{RECT c=ControlClientRect();return RECT{std::max<LONG>(18,c.right-430),c.bottom-82,std::max<LONG>(19,c.right-278),c.bottom-44};}
    RECT EmptyOpenRect()const{RECT c{};GetClientRect(m_hwnd,&c);int cx=(c.left+c.right)/2,cy=(c.top+c.bottom)/2;return RECT{cx-95,cy+46,cx+95,cy+88};}
    int ButtonWidth(int idx)const{static const int widths[]={56,42,46,42,42,84,66,64,74,48,58,52,54,70,72,68};return (idx>=0&&idx<16)?widths[idx]:0;}
    RECT ButtonRect(int idx)const{RECT c=ControlClientRect();int x=12;for(int i=0;i<idx&&i<16;++i)x+=ButtonWidth(i)+5;return RECT{x,9,x+ButtonWidth(idx),45};}
    bool PtIn(const RECT&r,int x,int y)const{return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}

    void DrawButton(HDC dc,const RECT&r,const std::wstring&text,bool active=false,bool hover=false){
        const COLORREF fill=active?RGB(32,102,170):(hover?RGB(58,61,67):RGB(39,41,46));
        const COLORREF edge=active?RGB(80,164,240):(hover?RGB(92,96,104):RGB(62,65,72));
        HGDIOBJ ob=SelectObject(dc,GetStockObject(DC_BRUSH)),op=SelectObject(dc,GetStockObject(DC_PEN));SetDCBrushColor(dc,fill);SetDCPenColor(dc,edge);RoundRect(dc,r.left,r.top,r.right,r.bottom,10,10);SelectObject(dc,ob);SelectObject(dc,op);
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,active?RGB(248,250,252):RGB(232,234,238));auto of=SelectObject(dc,m_font);RECT t=r;DrawTextW(dc,text.c_str(),-1,&t,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);SelectObject(dc,of);
    }

    int HitTestButton(int x,int y)const{for(int i=0;i<16;++i)if(PtIn(ButtonRect(i),x,y))return i;if(PtIn(MuteRect(),x,y))return 100;return -1;}
    RECT HoverRect(int id)const{if(id>=0&&id<16)return ButtonRect(id);if(id==100)return MuteRect();return RECT{};}
    void UpdateHover(int x,int y){
        const int next=HitTestButton(x,y);if(next==m_hoverButton)return;const int old=m_hoverButton;m_hoverButton=next;
        if(m_controlsWnd){if(old!=-1){RECT r=HoverRect(old);InvalidateRect(m_controlsWnd,&r,FALSE);}if(next!=-1){RECT r=HoverRect(next);InvalidateRect(m_controlsWnd,&r,FALSE);}}
    }

    void CreateDebugTooltips(){
        if(!m_controlsWnd)return;
        m_tooltipWnd=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_BALLOON|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,m_controlsWnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!m_tooltipWnd)return;
        SetWindowPos(m_tooltipWnd,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);SendMessageW(m_tooltipWnd,TTM_SETMAXTIPWIDTH,0,360);SendMessageW(m_tooltipWnd,TTM_SETDELAYTIME,TTDT_INITIAL,2000);SendMessageW(m_tooltipWnd,TTM_SETDELAYTIME,TTDT_AUTOPOP,12000);
        auto add=[&](UINT_PTR id,int button,const wchar_t* text){TTTOOLINFOW ti{sizeof(ti)};ti.uFlags=TTF_SUBCLASS;ti.hwnd=m_controlsWnd;ti.uId=id;ti.rect=ButtonRect(button);ti.lpszText=const_cast<LPWSTR>(text);SendMessageW(m_tooltipWnd,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&ti));};
        add(1001,9,L"Motion Vectors\nCurrent-to-previous motion supplied to DLSS/NR. Coherent regions indicate similar direction and magnitude. This is reconstructed optical flow, not the original game motion buffer.");
        add(1002,10,L"Depth Guide\nEstimated depth supplied to DLSS/NR. It is reconstructed from the video and is not the original game Z-buffer.");
        add(1003,11,L"Temporal Mask\nBias/current-color and disocclusion guide. Bright regions tell the temporal renderer to trust the current frame more strongly.");
    }
    void UpdateTooltipRects(){
        if(!m_tooltipWnd||!m_controlsWnd)return;auto upd=[&](UINT_PTR id,int button){TTTOOLINFOW ti{sizeof(ti)};ti.uFlags=TTF_SUBCLASS;ti.hwnd=m_controlsWnd;ti.uId=id;ti.rect=ButtonRect(button);SendMessageW(m_tooltipWnd,TTM_NEWTOOLRECTW,0,reinterpret_cast<LPARAM>(&ti));};upd(1001,9);upd(1002,10);upd(1003,11);
    }

    void Paint(){
        PAINTSTRUCT ps{};HDC dc=BeginPaint(m_hwnd,&ps);if(!dc)return;RECT c{};GetClientRect(m_hwnd,&c);
        if(m_loaded){EndPaint(m_hwnd,&ps);return;}
        const int W=std::max<LONG>(1,c.right),H=std::max<LONG>(1,c.bottom);HDC mem=CreateCompatibleDC(dc);HBITMAP bmp=CreateCompatibleBitmap(dc,W,H);HGDIOBJ old=SelectObject(mem,bmp);
        HBRUSH bg=CreateSolidBrush(RGB(18,19,21));FillRect(mem,&c,bg);DeleteObject(bg);SetBkMode(mem,TRANSPARENT);
        RECT title{40,(c.bottom/2)-62,c.right-40,(c.bottom/2)-18};SetTextColor(mem,RGB(242,243,245));auto of=SelectObject(mem,m_font);std::wstring tt=T(L"idle.title");DrawTextW(mem,tt.c_str(),-1,&title,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        RECT sub{40,(c.bottom/2)-17,c.right-40,(c.bottom/2)+24};SetTextColor(mem,RGB(160,164,172));SelectObject(mem,m_fontSmall);std::wstring ss=T(L"idle.subtitle");DrawTextW(mem,ss.c_str(),-1,&sub,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(mem,of);
        DrawButton(mem,EmptyOpenRect(),T(L"idle.open"),false,PtIn(EmptyOpenRect(),m_mouseX,m_mouseY));BitBlt(dc,0,0,W,H,mem,0,0,SRCCOPY);SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);EndPaint(m_hwnd,&ps);
    }

    void ControlsPaint(){
        if(!m_controlsWnd)return;PAINTSTRUCT ps{};HDC dc=BeginPaint(m_controlsWnd,&ps);if(!dc)return;RECT c{};GetClientRect(m_controlsWnd,&c);const int W=std::max<LONG>(1,c.right),H=std::max<LONG>(1,c.bottom);
        HDC mem=CreateCompatibleDC(dc);HBITMAP bmp=CreateCompatibleBitmap(dc,W,H);HGDIOBJ old=SelectObject(mem,bmp);HBRUSH bg=CreateSolidBrush(RGB(24,25,29));FillRect(mem,&c,bg);DeleteObject(bg);
        HGDIOBJ op=SelectObject(mem,GetStockObject(DC_PEN));SetDCPenColor(mem,RGB(57,60,66));MoveToEx(mem,0,0,nullptr);LineTo(mem,c.right,0);SelectObject(mem,op);
        DrawButton(mem,ButtonRect(0),L"Open",false,m_hoverButton==0);DrawButton(mem,ButtonRect(1),L"\u23EA",false,m_hoverButton==1);DrawButton(mem,ButtonRect(2),m_playing?L"\u23F8":L"\u25B6",m_playing,m_hoverButton==2);DrawButton(mem,ButtonRect(3),L"\u23F9",false,m_hoverButton==3);DrawButton(mem,ButtonRect(4),L"\u23E9",false,m_hoverButton==4);
        // STEP 05C v1.1 UI clarity hotfix: toolbar buttons express user-selected modes.
        // Actual raw-DLSS execution is diagnosed independently from FeatureCreated/EvaluationCount/logs;
        // do not turn a transient backend availability detail into a permanent "WAIT" user state.
        const bool dlssRequested=m_renderer&&m_renderer->DLSSRequested();
        const std::wstring dlssButton=dlssRequested?L"DLSS ON":L"DLSS OFF";
        const bool fgAvailable=m_renderer&&m_renderer->FrameGenerationAvailable();const std::wstring fgButton=fgAvailable?(m_frameGenerationEnabled?(L"FG "+std::to_wstring(m_frameGenerationMultiplier)+L"x"):L"FG OFF"):L"FG N/A";
        DrawButton(mem,ButtonRect(5),dlssButton,dlssRequested,m_hoverButton==5);DrawButton(mem,ButtonRect(6),m_fill?L"Crop":L"Fit",m_fill,m_hoverButton==6);DrawButton(mem,ButtonRect(7),L"Color",m_adjustWnd!=nullptr,m_hoverButton==7);DrawButton(mem,ButtonRect(8),fgButton,m_frameGenerationEnabled&&fgAvailable,m_hoverButton==8);
        DrawButton(mem,ButtonRect(9),L"MV",m_renderer&&m_renderer->GetDebugView()==D3D12Renderer::DebugView::MotionVectors,m_hoverButton==9);DrawButton(mem,ButtonRect(10),L"Depth",m_renderer&&m_renderer->GetDebugView()==D3D12Renderer::DebugView::Depth,m_hoverButton==10);DrawButton(mem,ButtonRect(11),L"T-Mask",m_renderer&&m_renderer->GetDebugView()==D3D12Renderer::DebugView::BiasMask,m_hoverButton==11);DrawButton(mem,ButtonRect(12),L"Full",m_fullscreen,m_hoverButton==12);DrawButton(mem,ButtonRect(13),L"Auto UI",m_fullscreenAutoHide,m_hoverButton==13);DrawButton(mem,ButtonRect(14),L"AI Depth",m_renderer&&m_renderer->GetDebugView()==D3D12Renderer::DebugView::AIDepth,m_hoverButton==14);DrawButton(mem,ButtonRect(15),L"HW Z",m_renderer&&m_renderer->GetDebugView()==D3D12Renderer::DebugView::AIHardwareDepth,m_hoverButton==15);

        RECT vr=VolumeRect();op=SelectObject(mem,GetStockObject(DC_PEN));SetDCPenColor(mem,RGB(94,98,105));MoveToEx(mem,vr.left,(vr.top+vr.bottom)/2,nullptr);LineTo(mem,vr.right,(vr.top+vr.bottom)/2);SelectObject(mem,op);int vx=vr.left+int((vr.right-vr.left)*(m_muted?0.0f:m_volume));HGDIOBJ ob=SelectObject(mem,GetStockObject(DC_BRUSH));SelectObject(mem,GetStockObject(DC_PEN));SetDCBrushColor(mem,RGB(230,232,235));SetDCPenColor(mem,RGB(230,232,235));Ellipse(mem,vx-5,(vr.top+vr.bottom)/2-5,vx+5,(vr.top+vr.bottom)/2+5);SelectObject(mem,ob);
        DrawButton(mem,MuteRect(),m_muted?L"Unmute":L"Mute",m_muted,m_hoverButton==100);

        double shown=m_dragSeek?m_seekPreview:(m_seekPending?m_pendingSeekSec:Position());RECT tr=TimelineRect();HBRUSH tb=CreateSolidBrush(RGB(68,71,77));FillRect(mem,&tr,tb);DeleteObject(tb);double d=m_decoder.DurationSeconds(),f=d>0?std::clamp(shown/d,0.0,1.0):0;RECT done=tr;done.right=done.left+int((done.right-done.left)*f);HBRUSH db=CreateSolidBrush(RGB(55,139,226));FillRect(mem,&done,db);DeleteObject(db);int kx=done.right;ob=SelectObject(mem,GetStockObject(DC_BRUSH));SetDCBrushColor(mem,RGB(246,246,248));Ellipse(mem,kx-5,tr.top-3,kx+5,tr.bottom+3);SelectObject(mem,ob);

        SetBkMode(mem,TRANSPARENT);SetTextColor(mem,RGB(202,205,211));auto of=SelectObject(mem,m_fontSmall);std::wstring time=TimeText(shown)+L" / "+TimeText(d);TextOutW(mem,18,c.bottom-43,time.c_str(),int(time.size()));
        std::wstringstream st;if(m_seeking||m_seekPending)st<<T(L"status.seeking")<<L"  |  ";st<<L"source "<<m_decoder.NativeWidth()<<L"x"<<m_decoder.NativeHeight();if(m_decoder.Width()!=m_decoder.NativeWidth()||m_decoder.Height()!=m_decoder.NativeHeight())st<<L" -> decode "<<m_decoder.Width()<<L"x"<<m_decoder.Height();st<<L"  |  "<<QualityNameW(m_activeQuality)<<L"  |  input "<<m_renderer->DLSSInputW()<<L"x"<<m_renderer->DLSSInputH()<<L"  |  output "<<m_renderer->OutputW()<<L"x"<<m_renderer->OutputH()<<L"  |  drop "<<m_droppedFrames;if(m_opticalFlow)st<<L"  |  NVOF Grid "<<m_opticalFlow->GridSize()<<L"x"<<m_opticalFlow->GridSize();st<<L"  |  Depth "<<DepthSourceNameW(m_opt.depthSource);std::wstring status=st.str();RECT sr{145,c.bottom-48,std::max<LONG>(146,c.right-445),c.bottom-28};DrawTextW(mem,status.c_str(),-1,&sr,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        const double sourceFps=m_decoder.FrameRate();const bool fpsLow=m_submitFps>0.0&&sourceFps>0.0&&(m_submitFps+0.5<sourceFps);SetTextColor(mem,fpsLow?RGB(238,76,76):RGB(216,219,224));
        const LONG fpsLeft=std::max<LONG>(160,c.right-560);RECT fr{fpsLeft,c.bottom-48,c.right-18,c.bottom-28};
        wchar_t fpsBuf[224]{};
        if(m_frameGenerationEnabled)swprintf_s(fpsBuf,L"Video %.2f FPS | Display %.1f FPS | DLSS-G %ux",sourceFps,m_displayFps,m_frameGenerationMultiplier);
        else swprintf_s(fpsBuf,L"Video %.2f FPS | Display %.1f FPS | Frame Gen OFF",sourceFps,m_displayFps);
        std::wstring fps=fpsBuf;DrawTextW(mem,fps.c_str(),-1,&fr,DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        SetTextColor(mem,RGB(202,205,211));std::wstring vol=m_muted?T(L"status.muted"):(T(L"status.volume")+L" "+std::to_wstring(int(m_volume*100))+L"%");RECT vl{vr.left,vr.top-23,vr.right,vr.top-5};DrawTextW(mem,vol.c_str(),-1,&vl,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(mem,of);
        BitBlt(dc,0,0,W,H,mem,0,0,SRCCOPY);SelectObject(mem,old);DeleteObject(bmp);DeleteDC(mem);EndPaint(m_controlsWnd,&ps);
    }

    void HandleFullscreenPointer(int x,int y){
        if(!m_fullscreen||!m_loaded)return;m_lastFullscreenMouse=Clock::now();if(!m_fullscreenAutoHide)return;RECT c{};GetClientRect(m_hwnd,&c);if(m_fullscreenControlsHidden&&y>=c.bottom-96){m_fullscreenControlsHidden=false;Layout();InvalidateControls();}
    }
    void UpdateFullscreenUiVisibility(){
        if(!m_fullscreen||!m_loaded||!m_controlsWnd)return;if(!m_fullscreenAutoHide){if(m_fullscreenControlsHidden){m_fullscreenControlsHidden=false;Layout();}return;}if(m_fullscreenControlsHidden||m_dragSeek||m_dragVolume)return;const double idle=std::chrono::duration<double>(Clock::now()-m_lastFullscreenMouse).count();if(idle>=2.5){m_fullscreenControlsHidden=true;Layout();}
    }
    void ToggleFullscreenAutoHide(){m_fullscreenAutoHide=!m_fullscreenAutoHide;m_lastFullscreenMouse=Clock::now();if(m_fullscreen&&!m_fullscreenAutoHide)m_fullscreenControlsHidden=false;SaveVideoSettings();Layout();InvalidateControls();}

    static LRESULT CALLBACK ControlsWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l){
        PlayerApp* a=nullptr;if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));return a?a->ControlsWndProc(h,m,w,l):DefWindowProcW(h,m,w,l);
    }
    LRESULT ControlsWndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        switch(m){
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:ControlsPaint();return 0;
        case WM_MOUSEMOVE:{m_mouseX=GET_X_LPARAM(l);m_mouseY=GET_Y_LPARAM(l);m_lastFullscreenMouse=Clock::now();UpdateHover(m_mouseX,m_mouseY);if(m_dragSeek&&GetCapture()==h){m_seekPreview=SecondsFromX(m_mouseX);RECT dirty=TimelineRect();InvalidateRect(h,&dirty,FALSE);}if(m_dragVolume&&GetCapture()==h)SetVolumeFromX(m_mouseX);if(!m_trackingMouseLeave){TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,h,0};TrackMouseEvent(&t);m_trackingMouseLeave=true;}return 0;}
        case WM_MOUSELEAVE:{m_trackingMouseLeave=false;int old=m_hoverButton;m_hoverButton=-1;m_mouseX=m_mouseY=-999;if(old!=-1){RECT r=HoverRect(old);InvalidateRect(h,&r,FALSE);}return 0;}
        case WM_LBUTTONDOWN:ControlsMouseDown(GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
        case WM_LBUTTONUP:if(m_dragSeek){double target=m_seekPreview;m_dragSeek=false;if(GetCapture()==h)ReleaseCapture();RequestSeek(target);}else if(m_dragVolume){m_dragVolume=false;if(GetCapture()==h)ReleaseCapture();}return 0;
        case WM_CAPTURECHANGED:if(m_dragSeek){m_dragSeek=false;InvalidateRect(h,nullptr,FALSE);}if(m_dragVolume)m_dragVolume=false;return 0;
        case WM_MOUSEWHEEL:return SendMessageW(m_hwnd,m,w,l);
        }
        return DefWindowProcW(h,m,w,l);
    }

    void RegisterOverlayHotkeys(){
        // WM_HOTKEY is posted by Windows independently of the swapchain WndProc.
        // This remains usable while ReShade owns/captures normal mouse/keyboard input.
        auto reg=[&](int id,UINT mods,UINT vk,const char* name){if(!RegisterHotKey(m_hwnd,id,mods|MOD_NOREPEAT,vk))LOG("Overlay hotkey unavailable: "<<name<<" winerr="<<GetLastError());};
        reg(HK_PLAY_PAUSE,MOD_CONTROL|MOD_ALT,VK_SPACE,"Ctrl+Alt+Space");
        reg(HK_BACK_10,MOD_CONTROL|MOD_ALT,VK_LEFT,"Ctrl+Alt+Left");
        reg(HK_FORWARD_10,MOD_CONTROL|MOD_ALT,VK_RIGHT,"Ctrl+Alt+Right");
        reg(HK_MUTE,MOD_CONTROL|MOD_ALT,'M',"Ctrl+Alt+M");
        reg(HK_DLSS,MOD_CONTROL|MOD_ALT,'D',"Ctrl+Alt+D");
        reg(HK_ADJUSTMENTS,MOD_CONTROL|MOD_ALT,'C',"Ctrl+Alt+C");
        reg(HK_SPLIT_SCREEN,MOD_CONTROL|MOD_ALT,'S',"Ctrl+Alt+S");
        reg(HK_SPLIT_RESET,MOD_CONTROL|MOD_ALT,'0',"Ctrl+Alt+0");
        reg(HK_SPLIT_LEFT,MOD_CONTROL|MOD_ALT,VK_OEM_COMMA,"Ctrl+Alt+,");
        reg(HK_SPLIT_RIGHT,MOD_CONTROL|MOD_ALT,VK_OEM_PERIOD,"Ctrl+Alt+.");
        reg(HK_VSYNC,MOD_CONTROL|MOD_ALT,'V',"Ctrl+Alt+V");
        reg(HK_FRAMEGEN,MOD_CONTROL|MOD_ALT,'G',"Ctrl+Alt+G");
        if(!RegisterHotKey(m_hwnd,HK_MEDIA_PLAY_PAUSE,MOD_NOREPEAT,VK_MEDIA_PLAY_PAUSE))LOG("Media Play/Pause hotkey unavailable winerr="<<GetLastError());
    }
    void UnregisterOverlayHotkeys(){if(!m_hwnd)return;for(int id:{HK_PLAY_PAUSE,HK_BACK_10,HK_FORWARD_10,HK_MUTE,HK_DLSS,HK_ADJUSTMENTS,HK_SPLIT_SCREEN,HK_SPLIT_RESET,HK_SPLIT_LEFT,HK_SPLIT_RIGHT,HK_VSYNC,HK_FRAMEGEN,HK_MEDIA_PLAY_PAUSE})UnregisterHotKey(m_hwnd,id);}
    void HandleHotkey(int id){
        switch(id){case HK_PLAY_PAUSE:case HK_MEDIA_PLAY_PAUSE:TogglePause();break;case HK_BACK_10:RequestSeek(Position()-10);break;case HK_FORWARD_10:RequestSeek(Position()+10);break;case HK_MUTE:ToggleMute();break;case HK_DLSS:ToggleDLSS();break;case HK_ADJUSTMENTS:ShowAdjustments();break;case HK_SPLIT_SCREEN:ToggleSplitScreen();break;case HK_SPLIT_RESET:ResetSplitDivider();break;case HK_SPLIT_LEFT:NudgeSplitDivider(-0.05f);break;case HK_SPLIT_RIGHT:NudgeSplitDivider(0.05f);break;case HK_VSYNC:ToggleVSync();break;case HK_FRAMEGEN:ToggleFrameGeneration();break;}
    }

    void OpenFromDialog(){auto p=PickVideoFile(m_hwnd,m_loc);if(!p.empty())Load(p);}
    void MouseDown(int x,int y){SetFocus(m_hwnd);if(!m_loaded&&PtIn(EmptyOpenRect(),x,y))OpenFromDialog();}
    void ControlsMouseDown(int x,int y){
        SetFocus(m_hwnd);if(!m_loaded||m_seeking)return;m_lastFullscreenMouse=Clock::now();RECT tr=TimelineRect();if(PtIn(tr,x,y)){m_dragSeek=true;m_seekPreview=SecondsFromX(x);SetCapture(m_controlsWnd);InvalidateRect(m_controlsWnd,&tr,FALSE);return;}RECT vr=VolumeRect();if(PtIn(vr,x,y)){m_muted=false;m_dragVolume=true;SetCapture(m_controlsWnd);SetVolumeFromX(x);return;}if(PtIn(MuteRect(),x,y)){ToggleMute();return;}
        const int b=HitTestButton(x,y);switch(b){case 0:OpenFromDialog();break;case 1:RequestSeek(Position()-10);break;case 2:TogglePause();break;case 3:StopPlayback();break;case 4:RequestSeek(Position()+10);break;case 5:ToggleDLSS();break;case 6:m_fill=!m_fill;Layout();break;case 7:ShowAdjustments();break;case 8:CycleFrameGeneration();break;case 9:ToggleDebug(D3D12Renderer::DebugView::MotionVectors);break;case 10:ToggleDebug(D3D12Renderer::DebugView::Depth);break;case 11:ToggleDebug(D3D12Renderer::DebugView::BiasMask);break;case 12:ToggleFullscreen();break;case 13:ToggleFullscreenAutoHide();break;case 14:ToggleDebug(D3D12Renderer::DebugView::AIDepth);break;case 15:ToggleDebug(D3D12Renderer::DebugView::AIHardwareDepth);break;}
    }

    double SecondsFromX(int x)const{RECT r=TimelineRect();const LONG span=(r.right>r.left)?(r.right-r.left):LONG(1);double t=double(LONG(x)-r.left)/double(span);return std::clamp(t,0.0,1.0)*m_decoder.DurationSeconds();}
    void SetVolumeFromX(int x){RECT r=VolumeRect();const LONG span=(r.right>r.left)?(r.right-r.left):LONG(1);m_volume=float(std::clamp(double(LONG(x)-r.left)/double(span),0.0,1.0));m_audio.SetVolume(m_volume);InvalidateControls();}
    void ToggleMute(){m_muted=!m_muted;m_audio.SetVolume(m_muted?0.0f:m_volume);InvalidateControls();}
    void ToggleDLSS(){
        if(!m_renderer)return;
        const bool next=!m_renderer->DLSSRequested();
        const bool fgWasRequested=m_frameGenerationEnabled;const uint32_t fgMultiplier=m_frameGenerationMultiplier;
        if(fgWasRequested)m_renderer->SetFrameGeneration(false,fgMultiplier);
        m_renderer->SetDLSS(next);m_dlssReset=true;
        if(next&&!m_renderer->DLSSAvailable())m_renderer->RequestDLSSRecreate();
        if(fgWasRequested)m_renderer->SetFrameGeneration(true,fgMultiplier);
        if(!m_playing)m_renderer->PresentCurrent();
        InvalidateControls();UpdateTitle();
        LOG("[DLSS] UI request="<<(next?"ON":"OFF")<<" available="<<(m_renderer->DLSSAvailable()?1:0)<<" active="<<(m_renderer->DLSSEnabled()?1:0)<<" FGRestored="<<(fgWasRequested?1:0));
    }
    bool SplitDividerHitTest(int x,int tolerance=12)const{
        if(!m_splitScreen||!m_renderer||m_renderer->GetDebugView()!=D3D12Renderer::DebugView::Final||!m_renderWnd)return false;
        RECT r{};GetClientRect(m_renderWnd,&r);const int w=std::max(1,int(r.right-r.left));
        const int sx=int(std::lround(double(w)*double(std::clamp(m_splitFraction,0.05f,0.95f))));
        return std::abs(x-sx)<=tolerance;
    }
    void SetSplitFractionValue(float fraction,bool persist){
        m_splitFraction=std::clamp(std::isfinite(fraction)?fraction:0.5f,0.05f,0.95f);
        if(m_renderer){m_renderer->SetSplitFraction(m_splitFraction);if(!m_playing)m_renderer->PresentCurrent();}
        if(persist)SaveVideoSettings();
        InvalidateControls();
    }
    bool BeginSplitDrag(int x){
        if(!SplitDividerHitTest(x,14))return false;
        m_splitDragging=true;SetCapture(m_renderWnd);SetCursor(LoadCursor(nullptr,IDC_SIZEWE));return true;
    }
    void UpdateSplitDrag(int x){
        if(!m_splitDragging||!m_renderWnd)return;RECT r{};GetClientRect(m_renderWnd,&r);const int w=std::max(1,int(r.right-r.left));
        SetSplitFractionValue(float(x)/float(w),false);SetCursor(LoadCursor(nullptr,IDC_SIZEWE));
    }
    void EndSplitDrag(){
        if(!m_splitDragging)return;m_splitDragging=false;if(GetCapture()==m_renderWnd)ReleaseCapture();SaveVideoSettings();
        LOG("[Split Divider] drag fraction="<<m_splitFraction);
    }
    void ResetSplitDivider(){SetSplitFractionValue(0.5f,true);LOG("[Split Divider] reset 50/50");}
    void NudgeSplitDivider(float delta){SetSplitFractionValue(m_splitFraction+delta,true);LOG("[Split Divider] fraction="<<m_splitFraction);}
    void RebuildMenuBar(){
        if(!m_hwnd)return;HMENU old=GetMenu(m_hwnd),fresh=CreateMenuBar();SetMenu(m_hwnd,fresh);DrawMenuBar(m_hwnd);if(old)DestroyMenu(old);
    }
    void SetSubtitleText(const std::wstring& text){
        if(text!=m_subtitleText)m_subtitleText=text;
        if(m_renderer)m_renderer->SetSubtitleText(m_subtitleText);
    }
    void UpdateSubtitleForTime(double seconds){
        if(m_selectedSubtitleStream<0){m_subtitleClockAnnounced=false;SetSubtitleText(L"");return;}
        const size_t cueCount=m_subtitles.CueCount();
        if(cueCount>0&&!m_subtitleClockAnnounced){LOG("[Subtitles Sync] playback clock active t="<<seconds<<"s stream="<<m_selectedSubtitleStream<<" cues="<<cueCount);m_subtitleClockAnnounced=true;}
        const std::wstring text=m_subtitles.TextAt(seconds);
        if(text!=m_subtitleText){LOG("[Subtitles Sync] cue "<<(text.empty()?"clear":"hit")<<" t="<<seconds<<"s stream="<<m_selectedSubtitleStream<<" chars="<<text.size()<<" cues="<<cueCount);}
        SetSubtitleText(text);
    }
    void SelectAudioTrack(size_t ordinal){
        if(ordinal>=m_mediaTracks.AudioTracks().size()||m_path.empty())return;const int next=m_mediaTracks.AudioTracks()[ordinal].streamIndex;if(next==m_selectedAudioStream)return;
        const int old=m_selectedAudioStream;const double keep=Position();m_selectedAudioStream=next;bool ok=m_audio.Start(m_path,keep,next);if(!ok){LOG("[Tracks] audio switch failed stream="<<next<<"; attempting previous stream="<<old);m_selectedAudioStream=old;ok=m_audio.Start(m_path,keep,old);}if(ok){m_audio.SetVolume(m_muted?0.0f:m_volume);m_audio.Pause(!m_playing);}RebuildMenuBar();UpdateTitle();
    }
    void SelectSubtitleOff(){m_selectedSubtitleStream=-1;m_subtitleClockAnnounced=false;m_subtitles.Clear();SetSubtitleText(L"");RebuildMenuBar();UpdateTitle();LOG("[Subtitles] Off");}
    void SelectSubtitleTrack(size_t ordinal){
        if(ordinal>=m_mediaTracks.SubtitleTracks().size()||m_path.empty())return;
        const auto&t=m_mediaTracks.SubtitleTracks()[ordinal];if(!t.textSubtitleSupported)return;
        m_selectedSubtitleStream=t.streamIndex;m_subtitleClockAnnounced=false;
        m_subtitles.LoadAsync(m_path,t.streamIndex);
        SetSubtitleText(L"");
        RebuildMenuBar();UpdateTitle();
        LOG("[Subtitles Async] selection requested stream="<<t.streamIndex<<"; video/render thread continues immediately");
    }
    void ToggleTemporalMaskBypass(){
        m_temporalMaskBypass=!m_temporalMaskBypass;
        m_guides.Reset();m_guideReset=true;m_dlssReset=true;
        SaveVideoSettings();RebuildMenuBar();UpdateTitle();InvalidateControls();
        LOG("[Temporal Mask] mode="<<(m_temporalMaskBypass?"BYPASS_FULL_FRAME":"ADAPTIVE")
            <<" biasCurrentColorW="<<(m_temporalMaskBypass?0:1));
    }
    double CurrentDisplayRefreshHz()const{
        HWND ref=m_renderWnd?m_renderWnd:m_hwnd;
        HMONITOR monitor=MonitorFromWindow(ref,MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW info{};info.cbSize=sizeof(info);
        if(!monitor||!GetMonitorInfoW(monitor,reinterpret_cast<MONITORINFO*>(&info)))return 60.0;
        DEVMODEW mode{};mode.dmSize=sizeof(mode);
        if(!EnumDisplaySettingsW(info.szDevice,ENUM_CURRENT_SETTINGS,&mode))return 60.0;
        const double hz=double(mode.dmDisplayFrequency);
        return hz>=20.0&&hz<=1000.0?hz:60.0;
    }
    uint32_t VSyncFrameGenerationCap()const{
        if(!m_vsyncEnabled)return 6u;
        const double source=m_decoder.FrameRate(),refresh=CurrentDisplayRefreshHz();
        if(!std::isfinite(source)||source<=1.0||!std::isfinite(refresh)||refresh<20.0)return 6u;
        const int cap=int(std::floor((refresh+0.25)/source));
        return uint32_t(std::clamp(cap,1,6));
    }
    void SetFrameGenerationMode(bool enabled,uint32_t multiplier){
        multiplier=std::clamp(multiplier,2u,6u);
        if(enabled&&m_renderer&&!m_renderer->FrameGenerationAvailable()){LOG("[DLSS-G] UI request ignored: runtime unavailable");return;}
        if(enabled&&m_renderer)multiplier=std::min(multiplier,m_renderer->FrameGenerationMaxMultiplier());
        if(enabled&&m_vsyncEnabled){
            const uint32_t cap=VSyncFrameGenerationCap();
            if(cap<2u){
                LOG("[DLSS-G] VSync safety disabled FG: source="<<m_decoder.FrameRate()<<" refresh="<<CurrentDisplayRefreshHz()<<"Hz");
                enabled=false;
            }else if(multiplier>cap){
                LOG("[DLSS-G] VSync safety cap requested="<<multiplier<<"x effective="<<cap<<"x source="<<m_decoder.FrameRate()<<" refresh="<<CurrentDisplayRefreshHz()<<"Hz");
                multiplier=cap;
            }
        }
        m_frameGenerationEnabled=enabled;m_frameGenerationMultiplier=multiplier;
        if(m_renderer)m_renderer->SetFrameGeneration(enabled,multiplier);
        SaveVideoSettings();RebuildMenuBar();UpdateTitle();InvalidateControls();
        LOG("[DLSS-G] UI mode="<<(enabled?"ON":"OFF")<<" requestedMultiplier="<<multiplier
            <<" maxMultiplier="<<(m_renderer?m_renderer->FrameGenerationMaxMultiplier():1u)
            <<" available="<<(m_renderer&&m_renderer->FrameGenerationAvailable()?1:0));
    }
    void CycleFrameGeneration(){
        if(m_renderer&&!m_renderer->FrameGenerationAvailable()){LOG("[DLSS-G] toolbar cycle ignored: runtime unavailable");return;}
        uint32_t maxMult=m_renderer?std::max(2u,m_renderer->FrameGenerationMaxMultiplier()):6u;
        if(m_vsyncEnabled)maxMult=std::min(maxMult,VSyncFrameGenerationCap());
        if(maxMult<2u){SetFrameGenerationMode(false,m_frameGenerationMultiplier);return;}
        if(!m_frameGenerationEnabled){SetFrameGenerationMode(true,2u);return;}
        if(m_frameGenerationMultiplier<maxMult){SetFrameGenerationMode(true,m_frameGenerationMultiplier+1u);return;}
        SetFrameGenerationMode(false,m_frameGenerationMultiplier);
    }
    void ToggleFrameGeneration(){CycleFrameGeneration();}
    void ToggleVSync(){
        m_vsyncEnabled=!m_vsyncEnabled;
        if(m_renderer){
            m_renderer->SetVSync(m_vsyncEnabled);
            if(m_frameGenerationEnabled&&m_vsyncEnabled){
                const uint32_t cap=VSyncFrameGenerationCap();
                if(cap<2u){m_frameGenerationEnabled=false;m_renderer->SetFrameGeneration(false,m_frameGenerationMultiplier);LOG("[DLSS-G] VSync safety disabled FG after VSync ON");}
                else if(m_frameGenerationMultiplier>cap){LOG("[DLSS-G] VSync safety cap after VSync ON requested="<<m_frameGenerationMultiplier<<"x effective="<<cap<<"x");m_frameGenerationMultiplier=cap;m_renderer->SetFrameGeneration(true,cap);}
                else m_renderer->SetFrameGeneration(true,m_frameGenerationMultiplier);
            }else m_renderer->SetFrameGeneration(m_frameGenerationEnabled,m_frameGenerationMultiplier);
            if(!m_playing)m_renderer->PresentCurrent();
        }
        SaveVideoSettings();RebuildMenuBar();UpdateTitle();InvalidateControls();
        LOG("[VSync] "<<(m_vsyncEnabled?"ON":"OFF")<<" presentSyncInterval="<<(m_vsyncEnabled?1:0));
    }
    void ToggleSplitScreen(){
        m_splitScreen=!m_splitScreen;
        if(m_renderer){m_renderer->SetSplitScreen(m_splitScreen);m_renderer->SetSplitFraction(m_splitFraction);if(!m_playing)m_renderer->PresentCurrent();}if(!m_splitScreen&&m_splitDragging)EndSplitDrag();
        if(m_hwnd){HMENU bar=GetMenu(m_hwnd);if(bar)CheckMenuItem(bar,IDM_SPLIT_SCREEN,MF_BYCOMMAND|(m_splitScreen?MF_CHECKED:MF_UNCHECKED));}
        LOG("[Split Screen] "<<(m_splitScreen?"ON left=DLSS_OFF right=DLSS_NR_ON":"OFF")<<" spatial=same-frame/full-viewport-scissor");
        InvalidateControls();UpdateTitle();
    }
    void Rehook(){if(m_renderer){m_renderer->RequestDLSSRecreate();m_dlssReset=true;}}
    void SetQualityMode(bool automatic,NVSDK_NGX_PerfQuality_Value q){
        m_opt.qualityExplicit=!automatic;m_opt.quality=q;
        if(m_loaded&&!m_path.empty()){
            if(m_renderer&&m_frameGenerationEnabled)m_renderer->SetFrameGeneration(false,m_frameGenerationMultiplier);
            std::wstring p=m_path;double keep=Position();bool wasPlaying=m_playing;
            if(Load(p))RequestSeek(keep,wasPlaying);
        }
    }
    void ToggleDepthMode(){auto n=m_guides.GetDepthMode()==TemporalGuideGenerator::DepthMode::Estimated?TemporalGuideGenerator::DepthMode::Flat:TemporalGuideGenerator::DepthMode::Estimated;m_guides.SetDepthMode(n);m_guideReset=true;m_dlssReset=true;UpdateTitle();}
    void SetDebug(D3D12Renderer::DebugView v){if(m_renderer){m_renderer->SetDebugView(v);if(!m_playing)m_renderer->PresentCurrent();InvalidateControls();}}
    void ToggleDebug(D3D12Renderer::DebugView v){if(!m_renderer)return;m_renderer->SetDebugView(m_renderer->GetDebugView()==v?D3D12Renderer::DebugView::Final:v);if(!m_playing)m_renderer->PresentCurrent();InvalidateControls();}
    void ToggleFullscreen(){
        if(!m_fullscreen){
            m_savedStyle=GetWindowLongW(m_hwnd,GWL_STYLE);GetWindowRect(m_hwnd,&m_savedRect);m_menuBar=GetMenu(m_hwnd);if(m_menuBar){SetMenu(m_hwnd,nullptr);DrawMenuBar(m_hwnd);}MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(m_hwnd,MONITOR_DEFAULTTONEAREST),&mi);SetWindowLongW(m_hwnd,GWL_STYLE,m_savedStyle&~(WS_CAPTION|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_SYSMENU));m_fullscreen=true;m_fullscreenControlsHidden=m_fullscreenAutoHide;m_lastFullscreenMouse=Clock::now();SetWindowPos(m_hwnd,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED);
        }else{
            m_fullscreen=false;m_fullscreenControlsHidden=false;SetWindowLongW(m_hwnd,GWL_STYLE,m_savedStyle);if(m_menuBar){SetMenu(m_hwnd,m_menuBar);DrawMenuBar(m_hwnd);}SetWindowPos(m_hwnd,nullptr,m_savedRect.left,m_savedRect.top,m_savedRect.right-m_savedRect.left,m_savedRect.bottom-m_savedRect.top,SWP_NOZORDER|SWP_FRAMECHANGED);
        }
        Layout();InvalidateControls();
    }

    LRESULT WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        switch(m){
        case WM_ERASEBKGND:return 1;
        case WM_DESTROY:m_running=false;PostQuitMessage(0);return 0;
        case WM_CLOSE:DestroyWindow(h);return 0;
        case WM_SIZE:Layout();return 0;
        case WM_PAINT:Paint();return 0;
        case WM_MOUSEMOVE:{m_mouseX=GET_X_LPARAM(l);m_mouseY=GET_Y_LPARAM(l);if(m_loaded)HandleFullscreenPointer(m_mouseX,m_mouseY);else{RECT r=EmptyOpenRect();InvalidateRect(m_hwnd,&r,FALSE);}return 0;}
        case WM_LBUTTONDOWN:MouseDown(GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
        case WM_DROPFILES:{HDROP d=reinterpret_cast<HDROP>(w);wchar_t p[32768]{};UINT count=DragQueryFileW(d,0xFFFFFFFF,nullptr,0);if(count>0&&DragQueryFileW(d,0,p,static_cast<UINT>(std::size(p))))Load(p);DragFinish(d);return 0;}
        case WM_MOUSEWHEEL:{if(m_loaded){m_muted=false;float step=(GET_WHEEL_DELTA_WPARAM(w)>0)?0.05f:-0.05f;m_volume=std::clamp(m_volume+step,0.0f,1.0f);m_audio.SetVolume(m_volume);InvalidateControls();}return 0;}
        case WM_COMMAND:HandleCommand(LOWORD(w));return 0;
        case WM_HOTKEY:HandleHotkey(int(w));return 0;
        case WM_KEYDOWN:
            if((GetKeyState(VK_CONTROL)&0x8000)&&w=='O'){OpenFromDialog();return 0;}if((GetKeyState(VK_CONTROL)&0x8000)&&w=='E'){ShowAdjustments();return 0;}if(w==VK_SPACE){TogglePause();return 0;}if(w==VK_LEFT){RequestSeek(Position()-10);return 0;}if(w==VK_RIGHT){RequestSeek(Position()+10);return 0;}if(w==VK_F11){ToggleFullscreen();return 0;}if(w==VK_F6){Rehook();return 0;}if(w=='D'){ToggleDLSS();return 0;}if(w=='G'){CycleDepthSource();return 0;}if(w=='M'){ToggleMute();return 0;}if(w=='1'){SetDebug(D3D12Renderer::DebugView::Final);return 0;}if(w=='2'){SetDebug(D3D12Renderer::DebugView::Input);return 0;}if(w=='3'){SetDebug(D3D12Renderer::DebugView::MotionVectors);return 0;}if(w=='4'){SetDebug(D3D12Renderer::DebugView::Depth);return 0;}if(w=='5'){SetDebug(D3D12Renderer::DebugView::BiasMask);return 0;}if(w=='6'){SetDebug(D3D12Renderer::DebugView::AIDepth);return 0;}if(w=='7'){SetDebug(D3D12Renderer::DebugView::AIHardwareDepth);return 0;}if(w==VK_ESCAPE&&m_fullscreen){ToggleFullscreen();return 0;}break;
        }
        return DefWindowProcW(h,m,w,l);
    }

    void HandleCommand(UINT id){
        if(id==IDM_TEMPORAL_MASK_BYPASS){ToggleTemporalMaskBypass();return;}
        if(id==IDM_FRAMEGEN_OFF){SetFrameGenerationMode(false,m_frameGenerationMultiplier);return;}
        if(id>=IDM_FRAMEGEN_2X&&id<=IDM_FRAMEGEN_6X){SetFrameGenerationMode(true,2u+uint32_t(id-IDM_FRAMEGEN_2X));return;}
        if(id==IDM_VSYNC){ToggleVSync();return;}
        if(id>=IDM_AUDIO_TRACK_BASE&&id<IDM_AUDIO_TRACK_BASE+90){SelectAudioTrack(size_t(id-IDM_AUDIO_TRACK_BASE));return;}
        if(id==IDM_SUBTITLE_OFF){SelectSubtitleOff();return;}
        if(id>=IDM_SUBTITLE_TRACK_BASE&&id<IDM_SUBTITLE_TRACK_BASE+90){SelectSubtitleTrack(size_t(id-IDM_SUBTITLE_TRACK_BASE));return;}
        const UINT langEnd=IDM_LANG_BASE+static_cast<UINT>(m_languageCodes.size());if(id>=IDM_LANG_BASE && id<langEnd){ApplyLanguage(m_languageCodes[id-IDM_LANG_BASE]);return;}
        switch(id){
        case IDM_SPLIT_SCREEN:ToggleSplitScreen();break;case IDM_SPLIT_RESET:ResetSplitDivider();break;
        case IDM_OPEN:OpenFromDialog();break;case IDM_EXIT:DestroyWindow(m_hwnd);break;case IDM_PLAY:TogglePause();break;case IDM_STOP:StopPlayback();break;case IDM_BACK10:RequestSeek(Position()-10);break;case IDM_FWD10:RequestSeek(Position()+10);break;case IDM_MUTE:ToggleMute();break;case IDM_DLSS:ToggleDLSS();break;case IDM_REHOOK:Rehook();break;
        case IDM_DEPTH_SOURCE_LEGACY:SetDepthSource(D3D12Renderer::DepthSource::Legacy);break;case IDM_DEPTH_SOURCE_FLAT:SetDepthSource(D3D12Renderer::DepthSource::Flat);break;case IDM_DEPTH_SOURCE_AI:SetDepthSource(D3D12Renderer::DepthSource::AISynthetic);break;        case IDM_NVOF_PERF_SLOW:SetNvofPerf(L"slow");break;case IDM_NVOF_PERF_MEDIUM:SetNvofPerf(L"medium");break;case IDM_NVOF_PERF_FAST:SetNvofPerf(L"fast");break;
        case IDM_NVOF_GRID_AUTO:SetNvofGrid(L"auto");break;case IDM_NVOF_GRID_1:SetNvofGrid(L"1");break;case IDM_NVOF_GRID_2:SetNvofGrid(L"2");break;case IDM_NVOF_GRID_4:SetNvofGrid(L"4");break;        case IDM_QUALITY_AUTO:SetQualityMode(true,NVSDK_NGX_PerfQuality_Value_MaxQuality);break;case IDM_QUALITY_QUALITY:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_MaxQuality);break;case IDM_QUALITY_BALANCED:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_Balanced);break;case IDM_QUALITY_PERFORMANCE:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_MaxPerf);break;case IDM_QUALITY_ULTRAPERF:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_UltraPerformance);break;case IDM_QUALITY_DLAA:SetQualityMode(false,NVSDK_NGX_PerfQuality_Value_DLAA);break;
        case IDM_VIEW_FINAL:SetDebug(D3D12Renderer::DebugView::Final);break;case IDM_VIEW_INPUT:SetDebug(D3D12Renderer::DebugView::Input);break;case IDM_VIEW_MV:SetDebug(D3D12Renderer::DebugView::MotionVectors);break;case IDM_VIEW_DEPTH:SetDebug(D3D12Renderer::DebugView::Depth);break;case IDM_VIEW_AI_DEPTH:SetDebug(D3D12Renderer::DebugView::AIDepth);break;case IDM_VIEW_AI_HW_DEPTH:SetDebug(D3D12Renderer::DebugView::AIHardwareDepth);break;case IDM_VIEW_MASK:SetDebug(D3D12Renderer::DebugView::BiasMask);break;case IDM_DEPTH_MODE:ToggleDepthMode();break;case IDM_VIDEO_ADJUSTMENTS:ShowAdjustments();break;case IDM_ASPECT_FIT:m_fill=false;Layout();break;case IDM_ASPECT_FILL:m_fill=true;Layout();break;case IDM_FULLSCREEN:ToggleFullscreen();break;
        }
    }

    AppOptions m_opt;Localizer m_loc;std::vector<std::wstring> m_languageCodes;D3D12Renderer::ColorSettings m_colorSettings{};NVSDK_NGX_PerfQuality_Value m_activeQuality=NVSDK_NGX_PerfQuality_Value_MaxQuality;HWND m_hwnd=nullptr,m_viewport=nullptr,m_renderWnd=nullptr,m_controlsWnd=nullptr,m_tooltipWnd=nullptr,m_adjustWnd=nullptr;HMENU m_menuBar=nullptr,m_nvofPerfMenu=nullptr,m_nvofGridMenu=nullptr,m_depthSourceMenu=nullptr;HFONT m_font=nullptr,m_fontSmall=nullptr;
    bool m_running=true,m_loaded=false,m_playing=false,m_haveNext=false,m_fill=false,m_fullscreen=false,m_dragSeek=false,m_dragVolume=false,m_muted=false,m_seekPending=false,m_seekResumePlaying=false,m_seeking=false,m_fullscreenAutoHide=true,m_fullscreenControlsHidden=false,m_trackingMouseLeave=false;
    LONG m_savedStyle=0;RECT m_savedRect{};double m_dar=16.0/9.0,m_currentSec=0,m_playStartSec=0,m_seekPreview=0,m_pendingSeekSec=0;float m_volume=1.0f,m_lastGlobalX=0,m_lastGlobalY=0;int m_mouseX=-999,m_mouseY=-999,m_hoverButton=-1;
    Clock::time_point m_playStart=Clock::now(),m_fpsWindowStart=Clock::now(),m_lastStaticPresent=Clock::now(),m_lastFullscreenMouse=Clock::now();double m_submitFps=0.0,m_displayFps=0.0,m_actualDisplayRatio=1.0;uint64_t m_lastDisplayedFramesTotal=0;bool m_lastGuideHardwareFastPath=false,m_lastGuideLegacyFlow=false;double m_lastGuidesMs=0.0,m_lastRendererMs=0.0;double m_lastFrameProcessMs=0.0;uint64_t m_fpsWindowFrames=0;std::wstring m_path;VideoDecoder m_decoder;VideoFrame m_next;std::unique_ptr<D3D12Renderer>m_renderer;std::unique_ptr<OpticalFlowEngine>m_opticalFlow;TemporalGuideGenerator m_guides;AudioPlayer m_audio;
    std::unique_ptr<AIDepthWorker>m_aiDepthWorker;AIDepthFrame m_aiDepthLatest{};uint64_t m_aiDepthSequence=0;AIDepthTemporalWorker m_aiDepthTemporalWorker;
    bool m_guideReset=true,m_dlssReset=true;int64_t m_lastRenderedTs=-1;uint64_t m_droppedFrames=0,m_uiTick=0;
};

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int){if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE)))return 1;if(FAILED(MFStartup(MF_VERSION,MFSTARTUP_FULL))){CoUninitialize();return 1;}PlayerApp app(ParseArgs());if(!app.Create(hi)){MFShutdown();CoUninitialize();return 1;}MSG msg{};bool quit=false;while(app.Running()&&!quit){while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT){quit=true;break;}TranslateMessage(&msg);DispatchMessageW(&msg);}if(quit)break;app.Tick();if(app.NeedsRealtimeTick())Sleep(app.TickSleepMs());else WaitMessage();}MFShutdown();CoUninitialize();return 0;}
